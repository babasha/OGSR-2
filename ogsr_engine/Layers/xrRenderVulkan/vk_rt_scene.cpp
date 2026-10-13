// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — camera-local ray-tracing scene. See vk_rt_scene.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_rt_scene.h"
#include "vk_rt_world.h"        // the OTHER source of RT triangles: the render geometry
#include "vk_cluster_stream.h"  // ClusterStream::ResidencyGen - the world can page in AFTER we bake it
#include "vk_pass_gather.h"     // GatherPass::Wanted — the final gather traces this same bubble
#include "vk_wrc.h"             // WRC::Wanted — the cache's shade pass traces this bubble too
#include "vk_buffer.h"          // CVulkanBuffer (soup / AS storage / scratch / instances)
#include "vk_barriers.h"        // VK::MemoryBarrier
#include "vk_profiler.h"        // VK::Prof zones around the builds
#include "vk_clk.h"             // VK::ClkToMs — one calibrated rdtsc->ms for the collect timer
#include "vk_command_buffer.h"  // CVulkanCommandManager::FRAMES_IN_FLIGHT
#include "HW_Vulkan.h"

#include "../../xrCDB/xrCDB.h"          // CDB::COLLIDER / RESULT
#include "../../xr_3da/IGame_Level.h"   // g_pGameLevel->ObjectSpace
#include "../../xr_3da/device.h"        // RV().frameIndex — the once-per-frame guard
#include "../../xr_3da/GameMtlLib.h"    // GMLib — material index -> name -> albedo

#include <thread>
#include <atomic>

extern int   ps_r_rtao;            // master gate (also gates the RTAO pass itself)
extern float ps_r_rtao_range;      // half-extent of the camera bubble, metres
extern int   ps_r_rtao_maxtris;    // hard cap on the soup (thousands of tris)
extern int   ps_r_rtao_filter;     // drop `suppress_shadows` collision-only tris
extern int   ps_r_rtgi_debug;      // RT preview (vk_pass_rtgi) — also wants the bubble built
extern int   ps_r_ddgi;            // probe volume (vk_ddgi) — likewise
extern float ps_r_ddgi_light;      // the probe field IN THE LIGHTING — turning it up must build the bubble on its own
extern int   ps_r_rt_src;          // 0 = level.cform (collision), 1 = the render geometry (vk_rt_world)
extern float ps_r_rt_lod;          // RT LOD error scale (bigger = finer cut, more triangles)
extern float ps_r_rt_tree_range;   // foliage-only bubble half-extent (m) - a change forces a rebuild, like r_rt_lod

namespace VK { namespace RTScene {

namespace {

constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;

PFN_vkCreateAccelerationStructureKHR           pfnCreateAS   = nullptr;
PFN_vkDestroyAccelerationStructureKHR          pfnDestroyAS  = nullptr;
PFN_vkGetAccelerationStructureBuildSizesKHR    pfnGetSizes   = nullptr;
PFN_vkCmdBuildAccelerationStructuresKHR        pfnCmdBuild   = nullptr;
PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetASAddr  = nullptr;

bool s_inited = false;
bool s_failed = false;
u32  s_scratchAlign = 256;

// ---------------------------------------------------------------------------
//  Slots — a rebuild always lands in the one that is NOT being traced.
// ---------------------------------------------------------------------------
struct Slot {
    CVulkanBuffer soup;      // triangle soup, 3 verts per tri (no index buffer)
    CVulkanBuffer mat;       // one u32 game-material index per triangle (shading input)
    CVulkanBuffer blasBuf;   // opaque BLAS backing store
    CVulkanBuffer blasBufAT; // cutout BLAS backing store (leaf cards, fences, grates)
    CVulkanBuffer tlasBuf;   // TLAS backing store
    CVulkanBuffer instBuf;   // up to two VkAccelerationStructureInstanceKHR
    CVulkanBuffer scratch;   // build scratch (two regions — see the build)
    VkAccelerationStructureKHR blas   = VK_NULL_HANDLE;
    VkAccelerationStructureKHR blasAT = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas   = VK_NULL_HANDLE;
    u32     tris   = 0;
    u32     opaque = 0;      // triangles before the cutout run starts
    Fvector anchor = {};
    float   cover  = 0.f;
    u64     retire = 0;      // frame stamp until which this slot may still be in flight
    u8      src    = 0xFF;   // which world these buffers were sized/placed for (see kSrc*)
    float   lod    = 0.f;    // r_rt_lod this slot was cut at (render source only)
    float   treeRange = -1.f;// r_rt_tree_range this slot was cut at (render source only)
    u32     resGen = 0;      // ClusterStream residency generation this cut was taken at
};
// The two sources need buffers in different MEMORY: the collision soup is built
// on a worker and memcpy'd (host-visible), the render soup is written by a
// compute pass (device-local). Switching r_rt_src therefore has to recreate them.
constexpr u8 kSrcCform  = 0;
constexpr u8 kSrcRender = 1;
Slot s_slot[2];
u32  s_cur   = ~0u;          // slot currently traced (~0 = nothing built yet)
u32  s_gen   = 0;
u64  s_frame = ~0ull;        // "no frame seen yet" — RV().frameIndex starts at 0

// Streaming settle tracker for the residency trigger in Update(). The world can
// page in AFTER this pass bakes it into a BLAS, and standing still meant that cut
// was kept forever. Rebuild when residency has CHANGED and then gone QUIET - not
// while pages are still landing, which right after a load is every frame for a
// couple of seconds.
u32  s_resGenSeen  = 0;
u64  s_resGenFrame = 0;
// Frames of stillness that count as "streaming has settled here".
constexpr u64 kResQuietFrames = 60;

// ---------------------------------------------------------------------------
//  Collector job (worker thread) — box-query the collision model into a soup.
// ---------------------------------------------------------------------------
enum { ST_IDLE = 0, ST_COLLECTING, ST_COLLECTED, ST_UPLOADED };
std::atomic<int> s_state{ ST_IDLE };
std::thread      s_worker;

xr_vector<Fvector> s_soup;      // worker writes, render thread consumes (state-gated)
xr_vector<u32>     s_soupMat;   // parallel to s_soup / 3 — 4 attribute uints per triangle
Fvector s_jobCenter = {};
float   s_jobHalf   = 0.f;
u32     s_jobDropped = 0;
float   s_jobMs      = 0.f;
u32     s_jobDst     = 0;       // slot the job is destined for
u8      s_jobSrc     = kSrcCform;
u32     s_jobResGen  = 0;       // residency generation the job walked (render source)

// Render-geometry source: the worker produces a LIST OF ENTRIES, not triangles —
// the triangles themselves are copied out of the render pools by a compute pass
// on the render thread (vk_rt_world), because that is where they live. The list
// stays inside that module; this side only carries the triangle total.
u32 s_jobTris = 0;

void JoinWorker() { if (s_worker.joinable()) s_worker.join(); }

// The render-geometry twin of CollectJob. Also a pure host walk (cull metas +
// residency bits), so it belongs on the same worker.
void SelectJob(Fvector center, float half, u32 maxTris)
{
    set_current_thread_name("VK RTSelect");
    const u64 t0 = CPU::GetCLK();
    s_jobDropped = 0;
    s_jobTris    = RTWorld::Select(center, half, maxTris);
    s_jobMs      = float(CPU::GetCLK() - t0) * VK::ClkToMs();
    s_state.store(ST_COLLECTED, std::memory_order_release);
}

// The soup walk. Runs on the worker: box_query into the tiled/monolithic CDB,
// then unpack the surviving triangles into a flat vertex array. RESULT already
// carries the three world-space verts, so no second indirection into the global
// arrays is needed (and no lifetime dependency on them past this point).
void CollectJob(Fvector center, float half, u32 maxTris, bool filter)
{
    set_current_thread_name("VK RTCollect");
    const u64 t0 = CPU::GetCLK();
    s_soup.clear();
    s_soupMat.clear();
    s_jobDropped = 0;

    CDB::MODEL* model = g_pGameLevel ? g_pGameLevel->ObjectSpace.GetStaticModel() : nullptr;
    if (!model) { s_jobMs = 0.f; s_state.store(ST_COLLECTED, std::memory_order_release); return; }

    Fvector dim;
    dim.set(half, half, half);

    CDB::COLLIDER xrc;
    xrc.r_clear();
    xrc.box_query(CDB::OPT_FULL_TEST, model, center, dim);
    xrc.r_dedup_by_id();   // tiled queries duplicate boundary tris into every tile

    const size_t n = xrc.r_count();
    const size_t cap = size_t(maxTris) * 3;
    s_soup.reserve(_min(n * 3, cap));
    s_soupMat.reserve(_min(n, cap / 3) * 4);
    CDB::RESULT* it = xrc.r_begin();
    for (size_t i = 0; i < n && s_soup.size() < cap; ++i, ++it) {
        // Collision-only geometry (invisible walls, blockers) is flagged not to
        // cast shadows; it must not cast ambient occlusion either.
        if (filter && it->suppress_shadows) { ++s_jobDropped; continue; }
        s_soup.push_back(it->verts[0]);
        s_soup.push_back(it->verts[1]);
        s_soup.push_back(it->verts[2]);
        // The other half of the same bitfield: the game material this face is made
        // of. Free here (the triangle is already in a register), and it is the only
        // thing in the collision model that says anything about how a surface looks.
        // Four uints per triangle so the attribute block has ONE layout for both
        // sources (rt_shade.glsl); collision geometry has no texcoords to put in
        // the other three.
        s_soupMat.push_back(it->material);
        s_soupMat.push_back(0);
        s_soupMat.push_back(0);
        s_soupMat.push_back(0);
    }

    s_jobMs = float(CPU::GetCLK() - t0) * VK::ClkToMs();
    s_state.store(ST_COLLECTED, std::memory_order_release);
}

// ---------------------------------------------------------------------------
//  GPU side
// ---------------------------------------------------------------------------
void DestroySlot(Slot& s)
{
    if (s.blas   != VK_NULL_HANDLE && pfnDestroyAS) { pfnDestroyAS(VulkanHW.m_Device, s.blas, nullptr);   s.blas = VK_NULL_HANDLE; }
    if (s.blasAT != VK_NULL_HANDLE && pfnDestroyAS) { pfnDestroyAS(VulkanHW.m_Device, s.blasAT, nullptr); s.blasAT = VK_NULL_HANDLE; }
    if (s.tlas   != VK_NULL_HANDLE && pfnDestroyAS) { pfnDestroyAS(VulkanHW.m_Device, s.tlas, nullptr);   s.tlas = VK_NULL_HANDLE; }
    s.soup.Destroy(); s.mat.Destroy(); s.blasBuf.Destroy(); s.blasBufAT.Destroy();
    s.tlasBuf.Destroy(); s.instBuf.Destroy(); s.scratch.Destroy();
    s.tris = 0; s.opaque = 0; s.cover = 0.f; s.retire = 0;
}

// Grow-only buffer (re)create. Returns false on allocation failure.
//
// The two BUILD INPUTS (soup + instance) are deliberately HOST-VISIBLE and are
// written with a plain memcpy instead of the async staging path. Sync validation
// is what settled this: the staging ring copies on the TRANSFER queue, and the
// graphics submit's semaphore wait does not cover ACCELERATION_STRUCTURE_BUILD,
// so the build read the soup with no dependency on the copy
// (SYNC-HAZARD-READ-AFTER-WRITE, caught 20-08). A host write before the submit
// needs no barrier at all, and the sizes make it free — the bubble measures under
// a megabyte on the levels seen so far. The AS storage and scratch stay GPU-only.
bool EnsureBuffer(CVulkanBuffer& b, VkDeviceSize need, VkBufferUsageFlags usage, bool gpuOnly)
{
    if (b.IsValid() && b.GetSize() >= need) return true;
    b.Destroy();
    b.Create(need, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
             gpuOnly ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO_PREFER_HOST, gpuOnly);
    return b.IsValid();
}

// memcpy into a host-visible build input. False = the mapping failed (VMA put it
// somewhere unmappable), which must disable the rebuild rather than build garbage.
bool WriteHost(CVulkanBuffer& b, const void* src, size_t bytes)
{
    void* dst = b.Map();
    if (!dst) return false;
    memcpy(dst, src, bytes);
    b.Flush();
    b.Unmap();
    return true;
}

VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) & ~(a - 1); }

// ---------------------------------------------------------------------------
//  ALBEDO TABLE — game material -> linear diffuse reflectance
//
//  The collision model carries a material INDEX per face and nothing else about
//  appearance, so this is the whole bridge from "what did the ray hit" to "what
//  colour does it bounce". One entry per entry in the material library, matched
//  by NAME SUBSTRING: gamemtl names are stable, descriptive and few
//  ("materials\concrete", "materials\earth", "objects\metal"), and a substring
//  rule survives a mod adding its own variants of them.
//
//  ⚠ These are guesses with a physical range, not measurements: real reflectance
//  of dry concrete is ~0.20-0.35, soil ~0.10-0.20, foliage ~0.10-0.20 with the
//  green dominant. Bounce light reads as WRONG mostly through hue and relative
//  brightness, so the table is tuned for those and deliberately avoids extremes
//  (a 0.9 albedo anywhere would make the future probe grid ring).
//
//  Unmatched materials get neutral mid-grey rather than black: an unknown
//  surface that bounces nothing is a much more visible error than one that
//  bounces the wrong hue, and the log line below names every fallback so the
//  table can be corrected against a real level instead of guessed at twice.
// ---------------------------------------------------------------------------
CVulkanBuffer s_albedo;
u32           s_albedoCount = 0;
bool          s_albedoTried = false;

struct MtlAlbedo { const char* key; float r, g, b; };
const MtlAlbedo kAlbedoRules[] = {
    // ⚠ORDERED: the FIRST match wins, so the specific must precede the generic.
    // "car_wheel" has to be caught by "wheel" before "car" turns it into painted
    // steel, and "tin_can" by "can" before "tin" does the same.
    { "wheel",    0.06f, 0.06f, 0.06f },
    { "tyre",     0.06f, 0.06f, 0.06f },
    { "fuel_can", 0.24f, 0.23f, 0.21f },
    { "tin_can",  0.34f, 0.33f, 0.31f },
    { "car",      0.22f, 0.22f, 0.24f },
    { "setka",    0.13f, 0.13f, 0.13f },   // chain-link: mostly holes, reads dark
    { "asphalt",  0.09f, 0.09f, 0.10f },
    { "concrete", 0.28f, 0.27f, 0.25f },
    { "cement",   0.28f, 0.27f, 0.25f },
    { "brick",    0.30f, 0.15f, 0.11f },
    { "stone",    0.26f, 0.25f, 0.23f },
    { "gravel",   0.22f, 0.21f, 0.19f },
    { "rock",     0.24f, 0.23f, 0.21f },
    { "sand",     0.40f, 0.34f, 0.23f },
    { "earth",    0.19f, 0.14f, 0.10f },
    { "ground",   0.19f, 0.14f, 0.10f },
    { "dirt",     0.19f, 0.14f, 0.10f },
    { "mud",      0.14f, 0.11f, 0.08f },
    { "grass",    0.11f, 0.19f, 0.07f },
    { "foliage",  0.10f, 0.18f, 0.06f },
    { "bush",     0.10f, 0.18f, 0.06f },
    { "tree",     0.16f, 0.12f, 0.07f },
    { "wood",     0.23f, 0.15f, 0.08f },
    { "board",    0.23f, 0.15f, 0.08f },
    { "furniture",0.23f, 0.16f, 0.10f },
    { "box",      0.28f, 0.22f, 0.15f },
    { "stucco",   0.42f, 0.40f, 0.36f },
    { "plaster",  0.42f, 0.40f, 0.36f },
    { "shifer",   0.22f, 0.22f, 0.22f },   // slate roofing sheet
    { "flooring", 0.34f, 0.33f, 0.31f },
    { "tile",     0.34f, 0.33f, 0.31f },
    { "barrel",   0.26f, 0.17f, 0.11f },   // the zone's barrels are rusted, not painted
    { "tin",      0.30f, 0.30f, 0.31f },
    { "metal",    0.32f, 0.32f, 0.33f },
    { "iron",     0.26f, 0.25f, 0.24f },
    { "steel",    0.32f, 0.32f, 0.33f },
    { "glass",    0.16f, 0.18f, 0.19f },
    { "water",    0.04f, 0.07f, 0.09f },
    { "snow",     0.72f, 0.75f, 0.78f },
    { "ice",      0.50f, 0.56f, 0.62f },
    { "fabric",   0.24f, 0.21f, 0.18f },
    { "cloth",    0.24f, 0.21f, 0.18f },
    { "flesh",    0.34f, 0.19f, 0.15f },
    { "body",     0.34f, 0.19f, 0.15f },
    // The creature classes are here for completeness only: nothing dynamic is in
    // the BVH yet (that needs the multi-instance TLAS), so they can never be hit.
    { "creatures", 0.32f, 0.22f, 0.17f },
    { "plastic",  0.20f, 0.20f, 0.21f },
    { "rubber",   0.08f, 0.08f, 0.08f },
    { "tyre",     0.07f, 0.07f, 0.07f },
    { "paper",    0.55f, 0.53f, 0.48f },
};

// Build once per session. GMLib is loaded before any level, but this is called
// lazily anyway so a run that never enables RT never pays for it.
void EnsureAlbedoTable()
{
    if (s_albedoTried) return;
    const u32 count = GMLib.CountMaterial();
    if (count == 0) return;              // library not up yet — try again next frame
    s_albedoTried = true;

    xr_vector<Fvector4> tab;
    tab.resize(count);
    u32 matched = 0;
    string512 unmatched; unmatched[0] = 0;

    for (u32 i = 0; i < count; ++i) {
        SGameMtl* m = GMLib.GetMaterialByIdx(u16(i));
        const char* name = (m && m->m_Name.size()) ? m->m_Name.c_str() : "";

        const MtlAlbedo* hit = nullptr;
        for (const MtlAlbedo& r : kAlbedoRules)
            if (strstr(name, r.key)) { hit = &r; break; }

        // w < 0 = "no texture behind this material, use the rgb" (rt_shade.glsl).
        // The collision world has no textures at all, so every entry says so.
        if (hit) { tab[i].set(hit->r, hit->g, hit->b, -1.f); ++matched; }
        else {
            tab[i].set(0.30f, 0.29f, 0.28f, -1.f);
            if (xr_strlen(unmatched) < sizeof(unmatched) - 64) {
                if (unmatched[0]) strcat_s(unmatched, sizeof(unmatched), ", ");
                strcat_s(unmatched, sizeof(unmatched), name);
            }
        }
    }

    const size_t bytes = tab.size() * sizeof(Fvector4);
    if (!EnsureBuffer(s_albedo, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false) ||
        !WriteHost(s_albedo, tab.data(), bytes)) {
        Msg("![VK RT] albedo table upload failed (%u materials) — RT shading off", count);
        s_albedo.Destroy();
        return;
    }
    s_albedoCount = count;
    Msg("[VK RT] albedo table: %u materials, %u matched by name%s%s",
        count, matched, unmatched[0] ? " | UNMATCHED (neutral grey): " : "", unmatched);
}

}   // anon namespace

bool Supported() { return VulkanHW.m_bRayQuerySupported && !s_failed; }

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;
    if (!VulkanHW.m_bRayQuerySupported) { s_failed = true; return false; }

    auto load = [&](const char* name) { return vkGetDeviceProcAddr(VulkanHW.m_Device, name); };
    pfnCreateAS  = (PFN_vkCreateAccelerationStructureKHR)          load("vkCreateAccelerationStructureKHR");
    pfnDestroyAS = (PFN_vkDestroyAccelerationStructureKHR)         load("vkDestroyAccelerationStructureKHR");
    pfnGetSizes  = (PFN_vkGetAccelerationStructureBuildSizesKHR)   load("vkGetAccelerationStructureBuildSizesKHR");
    pfnCmdBuild  = (PFN_vkCmdBuildAccelerationStructuresKHR)       load("vkCmdBuildAccelerationStructuresKHR");
    pfnGetASAddr = (PFN_vkGetAccelerationStructureDeviceAddressKHR)load("vkGetAccelerationStructureDeviceAddressKHR");
    if (!pfnCreateAS || !pfnDestroyAS || !pfnGetSizes || !pfnCmdBuild || !pfnGetASAddr) {
        Msg("![VK RT] acceleration-structure entry points missing — RT disabled");
        s_failed = true; return false;
    }

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps = {};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 p2 = {};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(VulkanHW.m_PhysicalDevice, &p2);
    s_scratchAlign = _max(1u, asProps.minAccelerationStructureScratchOffsetAlignment);

    Msg("[VK RT] init OK (scratch align %u, max prims %llu)",
        s_scratchAlign, (unsigned long long)asProps.maxPrimitiveCount);
    return true;
}

void Destroy()
{
    JoinWorker();
    RTWorld::Destroy();
    for (Slot& s : s_slot) DestroySlot(s);
    s_soup.clear(); s_soup.shrink_to_fit();
    s_soupMat.clear(); s_soupMat.shrink_to_fit();
    s_jobTris = 0;
    s_albedo.Destroy(); s_albedoCount = 0; s_albedoTried = false;
    s_cur = ~0u;
    s_state.store(ST_IDLE);
}

void OnLevelUnload()
{
    // The cform this soup was cut from is about to be freed, and the next level
    // is a different world — forget the bubble and start cold. The GPU objects
    // stay allocated (they are level-independent scratch) and are refilled by
    // the first rebuild of the new level.
    JoinWorker();
    RTWorld::OnLevelUnload();   // its group table holds addresses into the level's pools
    for (Slot& s : s_slot) { s.tris = 0; s.cover = 0.f; s.retire = 0; s.src = 0xFF; }
    s_cur = ~0u;
    s_jobTris = 0;
    s_state.store(ST_IDLE);
    ++s_gen;
}

// Anyone who traces wants the bubble kept up to date, not RTAO alone — with the
// gate still reading ps_r_rtao only, `r_rtgi_debug 1` on its own would find an
// empty scene and look like a broken shader instead of a disabled feature.
// ⚠The gather belongs in this list and was MISSED here once already: the extern
// sat two screens up with a comment saying it must build one, and the expression
// never got it. `r_gather 1` then built no bubble, so DDGI::Enabled() stayed
// false, so GatherPass::Enabled() stayed false — the feature read as "no visible
// change" with every shader loaded and every init line printed.
static bool RTWanted() { return ps_r_rtao != 0 || ps_r_rtgi_debug != 0 || ps_r_ddgi != 0 || ps_r_ddgi_light > 0.f
                             || VK::GatherPass::Wanted() || VK::WRC::Wanted(); }

bool                       Ready()      { return s_cur != ~0u && s_slot[s_cur].tris > 0 && RTWanted(); }
VkAccelerationStructureKHR TLAS()       { return Ready() ? s_slot[s_cur].tlas : VK_NULL_HANDLE; }
u32                        Generation() { return s_gen; }
float                      Coverage()   { return s_cur != ~0u ? s_slot[s_cur].cover : 0.f; }

VkBuffer VertexBuffer()   { return Ready() ? s_slot[s_cur].soup.GetHandle() : VK_NULL_HANDLE; }
VkBuffer MaterialBuffer() { return Ready() ? s_slot[s_cur].mat.GetHandle()  : VK_NULL_HANDLE; }
u32      TriCount()       { return Ready() ? s_slot[s_cur].tris : 0; }
// The albedo table follows the SOURCE of the structure being traced: the game
// material library for a cform bubble, the world materials for a render-geometry
// one. Both have the same shape (rgb + texture slot in w), and the handle changing
// is what makes every consumer rebind (they all compare handles).
bool     UsingRenderGeometry() { return s_cur != ~0u && s_slot[s_cur].src == kSrcRender && RTWorld::Ready(); }
VkBuffer AlbedoBuffer()   { return UsingRenderGeometry() ? RTWorld::AlbedoBuffer()
                                                         : (s_albedoCount ? s_albedo.GetHandle() : VK_NULL_HANDLE); }
u32      AlbedoCount()    { return UsingRenderGeometry() ? RTWorld::AlbedoCount() : s_albedoCount; }

// Shared with vk_rt_dyn (see the header). The instance descriptors are rebuilt
// from the slot rather than read back from instBuf — that buffer is host-visible
// and the mapping was only ever a write path.
const ASFns& Fns()
{
    static ASFns f;
    f.create = pfnCreateAS; f.destroy = pfnDestroyAS; f.sizes = pfnGetSizes;
    f.cmdBuild = pfnCmdBuild; f.address = pfnGetASAddr;
    return f;
}
u32 ScratchAlign() { return s_scratchAlign; }
u32 StaticInstances(VkAccelerationStructureInstanceKHR out[2])
{
    if (!Ready() || !pfnGetASAddr) return 0;
    const Slot& s = s_slot[s_cur];
    const VkAccelerationStructureKHR as[2] = { s.blas, s.blasAT };
    const u32 firstTri[2] = { 0u, s.opaque };
    u32 n = 0;
    for (u32 k = 0; k < 2; ++k) {
        if (as[k] == VK_NULL_HANDLE) continue;
        VkAccelerationStructureDeviceAddressInfoKHR ai = {};
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = as[k];
        VkAccelerationStructureInstanceKHR& i = out[n];
        i = {};
        i.transform.matrix[0][0] = 1.f; i.transform.matrix[1][1] = 1.f; i.transform.matrix[2][2] = 1.f;
        i.instanceCustomIndex = firstTri[k] & 0xFFFFFF;
        i.mask  = (k == 0) ? 0x01u : 0x02u;
        i.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        i.accelerationStructureReference = pfnGetASAddr(VulkanHW.m_Device, &ai);
        ++n;
    }
    return n;
}

const Fvector& Anchor()
{
    static Fvector zero = {};
    return s_cur != ~0u ? s_slot[s_cur].anchor : zero;
}

void Update(VkCommandBuffer cmd, const Fvector& camPos)
{
    if (!Init()) return;

    // SSAOPass::Execute runs twice on some frames (the home pass and a Stage-D
    // clone), and both would otherwise step this state machine. The slot retire
    // stamps are counted in FRAMES, so drive them off the engine's frame counter
    // and let only the first call per frame do anything.
    if (RV().frameIndex == s_frame) return;
    s_frame = RV().frameIndex;

    // Track when page residency last moved. One compare, and it has to happen
    // before the early-outs below so the "has it been quiet?" answer stays true
    // even on frames where nothing else in here runs.
    {
        const u32 rgen = ClusterStream::ResidencyGen();
        if (rgen != s_resGenSeen) { s_resGenSeen = rgen; s_resGenFrame = s_frame; }
    }

    if (!RTWanted()) return;                          // hard off: keep what we have, build nothing
    if (!g_pGameLevel || !g_pGameLevel->ObjectSpace.GetStaticModel()) return;

    // Cheap and idempotent after the first success; kept here so the table is
    // ready before the first traced frame rather than on some consumer's path.
    EnsureAlbedoTable();

    // The render-geometry source needs the group/material tables resolved (once
    // per level) and its diffuse array republished when streaming moved a view.
    // Both are no-ops the moment the source is the collision model.
    bool useRender = ps_r_rt_src != 0;
    if (useRender && !RTWorld::Build()) useRender = false;   // level still loading, or unavailable
    // Tick even after r_rt_src goes back to 0: the structure being traced stays
    // render-sourced until the next rebuild lands, and its diffuse array has to
    // keep up with texture streaming for as long as anything reads it.
    if (RTWorld::Ready()) RTWorld::FrameTick();

    const float half = _max(16.f, ps_r_rtao_range);

    switch (s_state.load(std::memory_order_acquire))
    {
    case ST_IDLE: {
        // Rebuild when the camera has eaten into the bubble's margin (or nothing
        // is built yet, or the requested radius changed under us). 0.4 of the
        // half-extent keeps well over half the bubble ahead of the player.
        const u8 src = useRender ? kSrcRender : kSrcCform;
        const bool stale = (s_cur == ~0u) || s_slot[s_cur].cover <= 0.f
                        || camPos.distance_to(s_slot[s_cur].anchor) > half * 0.4f
                        || _abs(s_slot[s_cur].cover - half) > 0.5f
                        || s_slot[s_cur].src != src    // r_rt_src flipped: rebuild from the other world
                        // ...and r_rt_lod likewise, or a sweep of it would silently
                        // measure the cut the bubble happened to be built with.
                        || (src == kSrcRender && _abs(s_slot[s_cur].lod - ps_r_rt_lod) > 0.5f)
                        // ...and the foliage reach, for the same reason: sweeping it without a
                        // rebuild would measure the tree set the bubble happened to be built with.
                        || (src == kSrcRender && _abs(s_slot[s_cur].treeRange - ps_r_rt_tree_range) > 0.5f)
                        // ...and when STREAMING has moved the world since this cut was
                        // taken. Everything else in this test is about where the CAMERA is;
                        // none of it fires while the player stands still, and the first
                        // build lands seconds after a load with most of the level still
                        // paging in. Measured on Escape 24-08: that first cut gathered 1139
                        // entries / 45k opaque triangles, a forced rebuild 40 s later on the
                        // SAME cvars gathered 2715 / 109k - the house the player was standing
                        // in was simply not in the traced world, so every GI ray left through
                        // its walls and the room read as a flat 8x too much open sky.
                        // NOTE: QUIET, not merely CHANGED: pages land every frame for a couple of
                        // seconds after a load, and rebuilding on each of them would be a BLAS
                        // build per frame. One rebuild once it settles is what is wanted.
                        || (src == kSrcRender && s_slot[s_cur].resGen != s_resGenSeen
                            && s_frame - s_resGenFrame >= kResQuietFrames);
        if (!stale) break;
        // Target the OTHER slot, and only once the frames that could still be
        // reading it have retired.
        const u32 dst = (s_cur == ~0u) ? 0u : (s_cur ^ 1u);
        if (s_slot[dst].retire > s_frame) break;
        JoinWorker();
        s_jobCenter = camPos;
        s_jobHalf   = half;
        s_jobDst    = dst;
        s_jobSrc    = src;
        s_jobResGen = s_resGenSeen;
        s_state.store(ST_COLLECTING, std::memory_order_release);
        const u32 maxTris = u32(_max(64, ps_r_rtao_maxtris)) * 1000u;
        if (src == kSrcRender) s_worker = std::thread(SelectJob, camPos, half, maxTris);
        else                   s_worker = std::thread(CollectJob, camPos, half, maxTris, ps_r_rtao_filter != 0);
        break;
    }

    case ST_COLLECTING:
        break;   // worker still walking the collision model

    case ST_COLLECTED: {
        JoinWorker();
        Slot& s = s_slot[s_jobDst];
        const bool render = (s_jobSrc == kSrcRender);
        const u32  tris   = render ? s_jobTris : u32(s_soup.size() / 3);
        if (tris == 0) {
            // Nothing to trace here (open field far from any collision). Keep the
            // previous structure rather than blinding the pass, but re-anchor so
            // we do not re-run the query every frame.
            Msg("[VK RTAO] collect: 0 tris at (%.0f %.0f %.0f) — keeping previous BLAS",
                s_jobCenter.x, s_jobCenter.y, s_jobCenter.z);
            s_state.store(ST_IDLE, std::memory_order_release);
            break;
        }
        // The two sources want the soup in DIFFERENT memory (host-visible for the
        // memcpy, device-local for the compute gather), so a flipped r_rt_src must
        // drop the buffers instead of reusing the ones that merely happen to be
        // large enough.
        if (s.src != s_jobSrc) { s.soup.Destroy(); s.mat.Destroy(); s.src = s_jobSrc; }

        const size_t bytes    = size_t(tris) * 3 * sizeof(Fvector);
        const size_t matBytes = size_t(tris) * 4 * sizeof(u32);
        // STORAGE_BUFFER on top of the build-input usage: rt_shade.glsl reads the
        // very same vertices back to reconstruct a face normal at a hit, so the
        // soup is both what the BLAS is built FROM and what shading reads.
        const VkBufferUsageFlags soupUsage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (!EnsureBuffer(s.soup, bytes, soupUsage, render) ||
            (!render && !WriteHost(s.soup, s_soup.data(), bytes))) {
            Msg("![VK RTAO] soup buffer alloc/map failed (%u tris) — RT scene off", tris);
            s_state.store(ST_IDLE, std::memory_order_release);
            break;
        }
        // Per-triangle attributes. A failure here is NOT fatal: the AO path never
        // reads them, so leave the buffer empty and let the shading consumers see
        // a null handle rather than lose the whole bubble.
        if (!EnsureBuffer(s.mat, matBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, render) ||
            (!render && (s_soupMat.size() < size_t(tris) * 4 ||
                         !WriteHost(s.mat, s_soupMat.data(), matBytes)))) {
            Msg("![VK RT] attribute buffer alloc/map failed (%u tris) — RT shading off this bubble", tris);
            s.mat.Destroy();
            if (render) {
                // Not survivable on this path: the gather writes the positions and
                // the attributes in one dispatch, and without a destination for the
                // second half the BLAS would be built over uninitialised memory.
                s_state.store(ST_IDLE, std::memory_order_release);
                break;
            }
        }
        s.tris   = tris;
        s.anchor = s_jobCenter;
        s.cover  = s_jobHalf;
        s.lod    = ps_r_rt_lod;
        s.treeRange = ps_r_rt_tree_range;
        // The generation the SelectJob actually walked. Stamped from the value
        // captured when the job was launched, not from "now": pages can land while
        // the worker walks, and crediting this cut with them would lose exactly the
        // rebuild this field exists to trigger.
        s.resGen = s_jobResGen;
        s_state.store(ST_UPLOADED, std::memory_order_release);
        break;
    }

    case ST_UPLOADED: {
        // Collision source: the soup is a host write from the previous step, so the
        // submit's implicit host-write dependency already covers it.
        // Render source: the triangles are still in the render pools — copy them
        // into the soup FIRST, in this same command buffer, and let the gather's
        // own barrier order the build behind it.
        Slot& s = s_slot[s_jobDst];
        if (s_jobSrc == kSrcRender &&
            !RTWorld::RecordGather(cmd, s.tris, s.soup.GetHandle(), s.mat.GetHandle())) {
            Msg("![VK RTWorld] gather failed (%u entries, %u tris) — keeping the previous BLAS",
                RTWorld::LastSelect().picked, s.tris);
            s.tris = 0;
            s_state.store(ST_IDLE, std::memory_order_release);
            break;
        }
        const int zone = VK::Prof::ZoneBegin(cmd, "RT/BuildAS");

        // ---- BLAS: ONE SOUP, TWO STRUCTURES -------------------------------
        // Ray flags are per geometry, so opacity has to be a property of the
        // STRUCTURE, not of a triangle. The gather sorted the soup — opaque first,
        // cutouts after — and these two builds read that one buffer at two offsets:
        //   A  [0, opaque)      declared OPAQUE: its hits commit inside traversal.
        //   B  [opaque, tris)   declared not opaque: every hit asks the alpha test.
        // Without the split, one leaf card would put the whole world through the
        // shader callback; with it, the callback is paid only where leaves are.
        // The collision path has no cutouts at all, so B is simply empty there.
        const u32 nOpaque = (s_jobSrc == kSrcRender) ? _min(RTWorld::OpaqueTris(), s.tris) : s.tris;
        const u32 nCutout = s.tris - nOpaque;
        s.opaque = nOpaque;

        VkAccelerationStructureGeometryKHR geo[2] = {};
        VkAccelerationStructureBuildGeometryInfoKHR  bi[2]  = {};
        VkAccelerationStructureBuildSizesInfoKHR     sz2[2] = {};
        const u32 primCount[2] = { nOpaque, nCutout };
        for (u32 k = 0; k < 2; ++k) {
            geo[k].sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            geo[k].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geo[k].flags        = (k == 0) ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
            geo[k].geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            geo[k].geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            geo[k].geometry.triangles.vertexData.deviceAddress =
                s.soup.GetDeviceAddress() + VkDeviceSize(k == 0 ? 0u : nOpaque) * 3 * sizeof(Fvector);
            geo[k].geometry.triangles.vertexStride = sizeof(Fvector);
            geo[k].geometry.triangles.maxVertex    = _max(1u, primCount[k] * 3) - 1;
            geo[k].geometry.triangles.indexType    = VK_INDEX_TYPE_NONE_KHR;

            bi[k].sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            bi[k].type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            bi[k].flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            bi[k].mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            bi[k].geometryCount = 1;
            bi[k].pGeometries   = &geo[k];

            sz2[k].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            if (primCount[k])
                pfnGetSizes(VulkanHW.m_Device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                            &bi[k], &primCount[k], &sz2[k]);
        }
        VkAccelerationStructureBuildSizesInfoKHR& sz = sz2[0];

        // ---- TLAS sizes (one instance per structure) ----------------------
        VkAccelerationStructureGeometryKHR tgeo = {};
        tgeo.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tgeo.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
        tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

        VkAccelerationStructureBuildGeometryInfoKHR tbi = {};
        tbi.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        tbi.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tbi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tbi.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tbi.geometryCount = 1;
        tbi.pGeometries   = &tgeo;

        const u32 nInstances = (nOpaque ? 1u : 0u) + (nCutout ? 1u : 0u);
        VkAccelerationStructureBuildSizesInfoKHR tsz = {};
        tsz.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        pfnGetSizes(VulkanHW.m_Device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tbi, &nInstances, &tsz);

        // Two scratch REGIONS, so the two BLAS builds can go in one command with no
        // barrier between them; the TLAS reuses the first after one. Sized apart
        // rather than as twice the larger — on a forest bubble the cutout build
        // wants ~35 MB and the opaque one a fraction of that.
        const VkDeviceSize scratchOne =
            AlignUp(_max(sz2[0].buildScratchSize, tsz.buildScratchSize), s_scratchAlign);
        const VkDeviceSize scratchNeed =
            scratchOne + AlignUp(sz2[1].buildScratchSize, s_scratchAlign) + s_scratchAlign;
        const bool ok =
            EnsureBuffer(s.blasBuf, _max<VkDeviceSize>(sz2[0].accelerationStructureSize, 256),
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, true) &&
            (nCutout == 0 ||
             EnsureBuffer(s.blasBufAT, sz2[1].accelerationStructureSize,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, true)) &&
            EnsureBuffer(s.tlasBuf, tsz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, true) &&
            EnsureBuffer(s.scratch, scratchNeed,                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) &&
            EnsureBuffer(s.instBuf, 2 * sizeof(VkAccelerationStructureInstanceKHR),
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false);
        if (!ok) {
            Msg("![VK RTAO] AS buffer alloc failed — RT scene off");
            VK::Prof::ZoneEnd(cmd, zone);
            s_state.store(ST_IDLE, std::memory_order_release);
            break;
        }

        // The structures themselves are recreated on every rebuild: their size is
        // derived from the tri count, which changes with the bubble's contents.
        if (s.blas   != VK_NULL_HANDLE) { pfnDestroyAS(VulkanHW.m_Device, s.blas, nullptr);   s.blas = VK_NULL_HANDLE; }
        if (s.blasAT != VK_NULL_HANDLE) { pfnDestroyAS(VulkanHW.m_Device, s.blasAT, nullptr); s.blasAT = VK_NULL_HANDLE; }
        if (s.tlas   != VK_NULL_HANDLE) { pfnDestroyAS(VulkanHW.m_Device, s.tlas, nullptr);   s.tlas = VK_NULL_HANDLE; }

        VkAccelerationStructureCreateInfoKHR ci = {};
        ci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = s.blasBuf.GetHandle();
        ci.size   = sz.accelerationStructureSize;
        ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (nOpaque && pfnCreateAS(VulkanHW.m_Device, &ci, nullptr, &s.blas) != VK_SUCCESS) {
            Msg("![VK RTAO] vkCreateAccelerationStructureKHR (BLAS) failed");
            VK::Prof::ZoneEnd(cmd, zone);
            s_state.store(ST_IDLE, std::memory_order_release);
            break;
        }
        if (nCutout) {
            ci.buffer = s.blasBufAT.GetHandle();
            ci.size   = sz2[1].accelerationStructureSize;
            if (pfnCreateAS(VulkanHW.m_Device, &ci, nullptr, &s.blasAT) != VK_SUCCESS) {
                Msg("![VK RTAO] vkCreateAccelerationStructureKHR (cutout BLAS) failed");
                if (s.blas) { pfnDestroyAS(VulkanHW.m_Device, s.blas, nullptr); s.blas = VK_NULL_HANDLE; }
                VK::Prof::ZoneEnd(cmd, zone);
                s_state.store(ST_IDLE, std::memory_order_release);
                break;
            }
        }
        ci.buffer = s.tlasBuf.GetHandle();
        ci.size   = tsz.accelerationStructureSize;
        ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (pfnCreateAS(VulkanHW.m_Device, &ci, nullptr, &s.tlas) != VK_SUCCESS) {
            Msg("![VK RTAO] vkCreateAccelerationStructureKHR (TLAS) failed");
            if (s.blas)   { pfnDestroyAS(VulkanHW.m_Device, s.blas, nullptr);   s.blas = VK_NULL_HANDLE; }
            if (s.blasAT) { pfnDestroyAS(VulkanHW.m_Device, s.blasAT, nullptr); s.blasAT = VK_NULL_HANDLE; }
            VK::Prof::ZoneEnd(cmd, zone);
            s_state.store(ST_IDLE, std::memory_order_release);
            break;
        }

        const VkDeviceSize scratchAddr = AlignUp(s.scratch.GetDeviceAddress(), s_scratchAlign);

        // Both BLASes in one command, on two scratch regions: they write different
        // structures out of different halves of the same soup, so nothing orders
        // them against each other.
        VkAccelerationStructureBuildGeometryInfoKHR      biUse[2];
        VkAccelerationStructureBuildRangeInfoKHR         range[2] = { { nOpaque, 0, 0, 0 }, { nCutout, 0, 0, 0 } };
        const VkAccelerationStructureBuildRangeInfoKHR*  pRange[2] = { &range[0], &range[1] };
        u32 nBuild = 0;
        if (nOpaque) {
            bi[0].dstAccelerationStructure  = s.blas;
            bi[0].scratchData.deviceAddress = scratchAddr;
            biUse[nBuild] = bi[0]; pRange[nBuild] = &range[0]; ++nBuild;
        }
        if (nCutout) {
            bi[1].dstAccelerationStructure  = s.blasAT;
            bi[1].scratchData.deviceAddress = scratchAddr + scratchOne;
            biUse[nBuild] = bi[1]; pRange[nBuild] = &range[1]; ++nBuild;
        }
        if (nBuild) pfnCmdBuild(cmd, nBuild, biUse, pRange);

        // The TLAS build both READS the finished BLAS and REUSES the same scratch
        // buffer, so the dst access has to cover the write too — with READ alone
        // sync validation reports a WRITE_AFTER_WRITE on the scratch (caught 20-08).
        VK::MemoryBarrier(cmd,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

        // Instances: identity transforms — the soup is already in world space. The
        // ⭐custom index is the structure's FIRST TRIANGLE in that soup, which is
        // how rt_shade.glsl gets back to one flat attribute array from two
        // structures whose primitive indices both start at zero.
        {
            VkAccelerationStructureInstanceKHR inst[2] = {};
            const VkAccelerationStructureKHR as[2] = { s.blas, s.blasAT };
            const u32 firstTri[2] = { 0u, nOpaque };
            u32 n = 0;
            for (u32 k = 0; k < 2; ++k) {
                if (as[k] == VK_NULL_HANDLE) continue;
                VkAccelerationStructureDeviceAddressInfoKHR ai = {};
                ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
                ai.accelerationStructure = as[k];
                inst[n].transform.matrix[0][0] = 1.f;
                inst[n].transform.matrix[1][1] = 1.f;
                inst[n].transform.matrix[2][2] = 1.f;
                inst[n].instanceCustomIndex = firstTri[k] & 0xFFFFFF;
                // ⭐The two structures are separately MASKABLE: 0x01 the opaque
                // world, 0x02 the alpha-tested cutouts. With r_rt_src 1 the cutouts
                // are the forest — 1.27 M of the 1.28 M triangles on a Pripyat spawn
                // — and every one of them asks a texture on the way through, so a
                // consumer that cannot afford them (the final gather at one ray per
                // pixel) can drop them by mask instead of by building a second BVH.
                // Everything written before this traced with 0xFF and still does.
                inst[n].mask  = (k == 0) ? 0x01u : 0x02u;
                inst[n].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
                inst[n].accelerationStructureReference = pfnGetASAddr(VulkanHW.m_Device, &ai);
                ++n;
            }
            if (!WriteHost(s.instBuf, inst, n * sizeof(VkAccelerationStructureInstanceKHR))) {
                Msg("![VK RTAO] instance buffer map failed — RT scene off");
                VK::Prof::ZoneEnd(cmd, zone);
                s_state.store(ST_IDLE, std::memory_order_release);
                break;
            }
        }

        tgeo.geometry.instances.data.deviceAddress = s.instBuf.GetDeviceAddress();
        tbi.dstAccelerationStructure  = s.tlas;
        tbi.scratchData.deviceAddress = scratchAddr;
        VkAccelerationStructureBuildRangeInfoKHR trange = { nInstances, 0, 0, 0 };
        const VkAccelerationStructureBuildRangeInfoKHR* pTRange = &trange;
        pfnCmdBuild(cmd, 1, &tbi, &pTRange);

        VK::MemoryBarrier(cmd,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        VK::Prof::ZoneEnd(cmd, zone);

        // The slot we are leaving stays untouchable until every frame that could
        // still be tracing it has retired.
        if (s_cur != ~0u) s_slot[s_cur].retire = s_frame + kFramesInFlight + 1;
        s_cur = s_jobDst;
        ++s_gen;
        s_state.store(ST_IDLE, std::memory_order_release);

        Msg("[VK RTAO] BLAS rebuilt from %s: %u tris (%u %s) @ (%.0f %.0f %.0f) r=%.0fm | collect %.1f ms | "
            "blas %.1f + %.1f MB, tlas %u KB, scratch %.1f MB, soup %.1f MB",
            s_jobSrc == kSrcRender ? "RENDER geometry" : "cform",
            s.tris, s_jobSrc == kSrcRender ? RTWorld::LastSelect().picked : s_jobDropped,
            s_jobSrc == kSrcRender ? "entries" : "dropped",
            s.anchor.x, s.anchor.y, s.anchor.z, s.cover, s_jobMs,
            float(sz2[0].accelerationStructureSize) / (1024.f * 1024.f),
            float(sz2[1].accelerationStructureSize) / (1024.f * 1024.f),
            u32(tsz.accelerationStructureSize / 1024),
            float(scratchNeed) / (1024.f * 1024.f),
            float(s.soup.GetSize()) / (1024.f * 1024.f));
        if (s_jobSrc == kSrcRender) {
            // Which of the three reasons an entry can be absent actually fired.
            // "7892 triangles" alone cannot tell a too-coarse LOD cut from a level
            // whose pages are mostly out of residency.
            const RTWorld::SelectStats& st = RTWorld::LastSelect();
            Msg("[VK RTWorld] cut: %u in range -> %u gathered (%u not resident, %u other LOD level, %u over budget) | "
                "trees %u of %u in range (%u past r_rt_tree_range %.0f) | %u opaque + %u cutout tris | r_rt_lod %.0f | resGen %u",
                st.inRange, st.picked, st.noPage, st.lodCut, st.capped,
                st.trees, st.treesInRange, st.treesCut, ps_r_rt_tree_range,
                s.opaque, s.tris - s.opaque, ps_r_rt_lod, s.resGen);
        }
        break;
    }
    }
}

}}  // namespace VK::RTScene

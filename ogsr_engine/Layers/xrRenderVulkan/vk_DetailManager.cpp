// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - CDetailManager Session A: load + heightmap bake +
// SSBO upload. See vk_DetailManager.h header notes.

#include "stdafx.h"

extern int ps_r__tf_Anisotropic;   // r__tf_aniso (vk_console_min.cpp). At FILE scope: a block-scope extern inside namespace VK is VK::ps_r__tf_Anisotropic to GCC (Switch link error), while MSVC accepts it as the global.

#include "vk_DetailManager.h"
#include "HW_Vulkan.h"
#include "vk_image.h"      // VK::CreateImage / CreateImage2D / CreateImageView
#include "vk_compute_util.h" // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_swapchain.h"               // for color/depth formats in CreateGfxPipeline
#include "vk_scene_color.h"             // HDR scene target format
#include "vk_motionvec.h"               // VK::MotionVec::Format — RG16F MV target (grass MV overlay)
#include "vk_gfx_pipeline.h"            // VK::GfxPipelineBuilder
#include "vk_descriptors.h"             // VK::DescriptorWriter
#include "vk_env_light.h"               // VK::EnvLight — set 1 (sun_vp + sun shadow map)
#include "vk_shaders.h"                 // g_ShaderManager
#include "vk_texture.h"                 // CVulkanTexture (LoadDDS)
#include "vk_pass_context.h"            // FrameContext
#include "CRender_Vulkan.h"             // RImplementation (for sampler reuse if needed)

#include "../../xr_3da/IGame_Persistent.h"
#include "../../xr_3da/IGame_Level.h"
#include "../../xrCDB/xrCDB.h"           // CDB::TRI, CDB::MODEL
#include <thread>       // ParallelRows — slot heal/pack over the detail grid
#include <atomic>
#include <functional>

// Grass reductions the Switch preset turns on (vk_console_min.cpp; PC defaults 0 = stock). GLOBAL scope on purpose.
extern int ps_r_grass_gen;   // r_grass_gen: 0 full generator, 1 lean, 2 lean2, 3 tuft cache (pipelines built at level load)
extern int ps_r_grass_r1;    // r_grass_r1:  0 full grass FS, 1 R1 FS (detail_r1.frag), 2 R1 per vertex (detail_r1v.vert/frag)

namespace VK
{

// Warn loudly if a push range exceeds this GPU's maxPushConstantsSize. The grass
// push blocks are 208 B; Vulkan only *guarantees* 128 B, so vkCreatePipelineLayout
// would fail on a minimal-limit GPU. Desktop GPUs offer 256, so it works here.
static void DM_CheckPushSize(const char* tag, u32 size)
{
    if (size > VulkanHW.Caps.maxPushConstantsSize)
        Msg("![VK Grass] %s push %u B exceeds device maxPushConstantsSize %u B — pipeline layout creation will FAIL on this GPU",
            tag, size, VulkanHW.Caps.maxPushConstantsSize);
}

// ============================================================================
// SSwingValue::lerp
// ============================================================================
void CDetailManager::SSwingValue::lerp(const SSwingValue& v1, const SSwingValue& v2, float factor)
{
    const float inv = 1.0f - factor;
    rot1  = v1.rot1  * inv + v2.rot1  * factor;
    rot2  = v1.rot2  * inv + v2.rot2  * factor;
    amp1  = v1.amp1  * inv + v2.amp1  * factor;
    amp2  = v1.amp2  * inv + v2.amp2  * factor;
    speed = v1.speed * inv + v2.speed * factor;
}

// ============================================================================
// Construction / destruction
// ============================================================================
CDetailManager::CDetailManager()
{
    // Defaults match monolith so Session B picks up sensible values before
    // the env subsystem is wired.
    swing_desc[0] = { 0.0f,     0.0f,     0.1f, 0.2f, 1.0f };  // normal
    swing_desc[1] = { PI_MUL_2, PI_MUL_2, 0.3f, 0.5f, 3.0f };  // fast
    swing_current = swing_desc[0];
}

CDetailManager::~CDetailManager()
{
    Unload();
}

// ============================================================================
// QueryDB — world-slot lookup with empty-slot fallback.
// ============================================================================
DetailSlot& CDetailManager::QueryDB(int sx, int sz)
{
    const int db_x = sx + dtH.offs_x;
    const int db_z = sz + dtH.offs_z;
    if (db_x >= 0 && db_x < int(dtH.size_x) && db_z >= 0 && db_z < int(dtH.size_z) && dtSlots) {
        return dtSlots[u32(db_z) * dtH.size_x + u32(db_x)];
    }
    DS_empty.w_id(0, DetailSlot::ID_Empty);
    DS_empty.w_id(1, DetailSlot::ID_Empty);
    DS_empty.w_id(2, DetailSlot::ID_Empty);
    DS_empty.w_id(3, DetailSlot::ID_Empty);
    return DS_empty;
}

// ============================================================================
// Load — full Session A pipeline:
//   1. Open level.details, parse header + objects + slots
//   2. BakeHeightmap from collision tris
//   3. UploadSlotData (per-slot 32 B SSBO)
//   4. UploadObjInfo (per-type 16 B SSBO)
// ============================================================================
void CDetailManager::Load()
{
    VK::Vram::Scope _vram_scope("Grass");
    if (m_bCreated) return;

    if (!FS.exist("$level$", "level.details")) {
        Msg("[VK Grass] level.details not found — grass disabled");
        return;
    }

    string_path fn;
    FS.update_path(fn, "$level$", "level.details");
    dtFS = FS.r_open(fn);
    if (!dtFS) {
        Msg("![VK Grass] level.details exists but failed to open");
        return;
    }

    // Chunk 0: header
    dtFS->r_chunk_safe(0, &dtH, sizeof(dtH));
    Msg("[VK Grass] DetailHeader: ver=%u objects=%u offs=(%d,%d) size=(%u,%u)",
        dtH.version, dtH.object_count, dtH.offs_x, dtH.offs_z, dtH.size_x, dtH.size_z);

    if (dtH.version != DETAIL_VERSION) {
        Msg("![VK Grass] Unsupported level.details version %u (expected %u)", dtH.version, DETAIL_VERSION);
        FS.r_close(dtFS); dtFS = nullptr;
        return;
    }
    if (dtH.object_count == 0 || dtH.object_count > dm_max_objects) {
        Msg("![VK Grass] object_count=%u out of range (max %u)", dtH.object_count, dm_max_objects);
        FS.r_close(dtFS); dtFS = nullptr;
        return;
    }

    // Chunk 1: object models
    {
        IReader* objs_chunk = dtFS->open_chunk(1);
        if (!objs_chunk) {
            Msg("![VK Grass] objects chunk (1) not found");
            FS.r_close(dtFS); dtFS = nullptr;
            return;
        }
        // svector has fixed capacity (dm_max_objects=64), no reserve().
        for (u32 id = 0; id < dtH.object_count; ++id) {
            auto* dt = xr_new<VK::CDetail>();
            IReader* sub = objs_chunk->open_chunk(id);
            if (sub) {
                dt->Load(sub);
                sub->close();
            } else {
                Msg("![VK Grass] sub-chunk %u missing in objects", id);
            }
            objects.push_back(dt);
        }
        objs_chunk->close();
    }

    // Chunk 2: slot grid (zero-copy view into VFS-mapped data)
    {
        IReader* slots_chunk = dtFS->open_chunk(2);
        if (!slots_chunk) {
            Msg("![VK Grass] slots chunk (2) not found");
            FS.r_close(dtFS); dtFS = nullptr;
            return;
        }
        dtSlots = (DetailSlot*)slots_chunk->pointer();
        slots_chunk->close();
    }
    Memory.mem_fill(&DS_empty, 0, sizeof(DS_empty));
    DS_empty.w_id(0, DetailSlot::ID_Empty);
    DS_empty.w_id(1, DetailSlot::ID_Empty);
    DS_empty.w_id(2, DetailSlot::ID_Empty);
    DS_empty.w_id(3, DetailSlot::ID_Empty);

    // Sub-timers: this pass is 1.85 s of a 21 s pripyat_full load (18-08), and it
    // mixes four unrelated kinds of work — a heightmap bake off the collision tris,
    // SSBO uploads, texture loads and pipeline creation. Which one owns the time
    // decides whether the answer is a cook, a prefetch or a pipeline cache, so
    // measure before designing anything (twice today the obvious guess was wrong).
    CTimer _t;
    float msBake, msSlots, msObj, msBufs, msDummy, msHZB, msTex, msGenPipe, msGfxPipe;

    // GPU-side prep
    _t.Start(); BakeHeightmap();   msBake  = _t.GetElapsed_ms_total();
    _t.Start(); UploadSlotData();  msSlots = _t.GetElapsed_ms_total();
    _t.Start(); UploadObjInfo();   msObj   = _t.GetElapsed_ms_total();


    // Session B: render-side resources
    _t.Start(); CreateGpuBuffers();    msBufs  = _t.GetElapsed_ms_total();
    _t.Start(); CreateDummyTextures(); msDummy = _t.GetElapsed_ms_total();

    // before gen pipeline → binding 6 picks real HZB
    _t.Start(); CreateHZB(Swapchain.m_Extent.width, Swapchain.m_Extent.height); msHZB = _t.GetElapsed_ms_total();
    _t.Start(); LoadDetailTextures();  msTex     = _t.GetElapsed_ms_total();
    _t.Start(); CreateGpuGenPipeline(); msGenPipe = _t.GetElapsed_ms_total();
    _t.Start(); CreateGfxPipeline();    msGfxPipe = _t.GetElapsed_ms_total();


    Msg("[load step]   Details::Load: bake %.0f | slots %.0f | objinfo %.0f | bufs %.0f | dummy %.0f | hzb %.0f | textures %.0f | genPipe %.0f | gfxPipe %.0f ms",
        msBake, msSlots, msObj, msBufs, msDummy, msHZB, msTex, msGenPipe, msGfxPipe);

    m_bCreated = true;
    Msg("[VK Grass] Loaded: %u objects, %ux%u slots, heightmap %ux%u",
        (u32)objects.size(), dtH.size_x, dtH.size_z, m_HeightmapW, m_HeightmapH);
}

// ============================================================================
// Unload
// ============================================================================
void CDetailManager::Unload()
{
    for (auto* obj : objects) {
        if (!obj) continue;
        obj->Unload();
        xr_delete(obj);
    }
    objects.clear();

    if (dtFS) { FS.r_close(dtFS); dtFS = nullptr; }
    dtSlots = nullptr;

    DestroyGfxPipeline();
    DestroyGpuGenPipeline();
    DestroyHZB();
    DestroyDetailTextures();
    DestroyDummyTextures();
    DestroyGpuBuffers();

    if (m_SlotDataSSBO) { m_SlotDataSSBO->Destroy(); xr_delete(m_SlotDataSSBO); }
    if (m_ObjInfoSSBO)  { m_ObjInfoSSBO->Destroy();  xr_delete(m_ObjInfoSSBO);  }
    m_TotalSlots = 0;

    if (VulkanHW.m_Device != VK_NULL_HANDLE) {
        if (m_HeightmapSampler) { vkDestroySampler   (VulkanHW.m_Device, m_HeightmapSampler, nullptr); m_HeightmapSampler = VK_NULL_HANDLE; }
        if (m_HeightmapView)    { vkDestroyImageView (VulkanHW.m_Device, m_HeightmapView,    nullptr); m_HeightmapView    = VK_NULL_HANDLE; }
        if (m_HeightmapImage)   {
            VK::Vram::DestroyImage(VulkanHW.m_Allocator, m_HeightmapImage, m_HeightmapAlloc);
            m_HeightmapImage = VK_NULL_HANDLE;
            m_HeightmapAlloc = VK_NULL_HANDLE;
        }
    }
    m_HeightmapW = m_HeightmapH = 0;

    m_bCreated = false;
    Msg("[VK Grass] Unloaded");
}

// ============================================================================
// BakeHeightmap — rasterize static collision tris into an R32F image where
// each texel holds the MIN ground Y at that XZ. Sentinel 9000.0f marks
// texels no triangle covered (compute shader tests abs(h)<9000).
//
// Tris with normal.y < 0.3 (>73° slopes — walls, ceilings) are skipped:
// grass shouldn't grow on cliffs. Holes are filled with a 5×5 neighbour
// average pass to avoid black spots.
// ============================================================================
// ============================================================================
// Heightmap disk cache
// ============================================================================
// BakeHeightmap rasterizes the level's ENTIRE collision set (61.3M tris on
// pripyat_full) into a 2048x2048 R32F map on the loading thread: measured 18-08
// at 1523 ms of a 21 s load — 84% of Details::Load — and recomputed identically
// every single time. Its only inputs are level.cform and level.details, so it
// caches exactly like the CDB tile cache it sits next to: same directory
// (app_data_root), same crc32(levelPath) naming, same "content key in the
// header" validation (xr_area.cpp). A stale entry could only misplace grass
// height, never crash, but the key still covers every input the bake reads —
// both files' ages, the triangle count and the detail header.
namespace {

constexpr u32 kHMCacheMagic   = 0x314D4847u;   // "GHM1"
constexpr u32 kHMCacheVersion = 1;

struct HMCacheHeader
{
    u32 magic;
    u32 version;
    u64 key;
    u32 w, h;
};

bool HeightmapCachePath(string_path& out)
{
    string_path levelPath;
    FS.update_path(levelPath, "$level$", "");
    string64 name;
    xr_sprintf(name, "grasshm_%08x.ghm", crc32(levelPath, (u32)xr_strlen(levelPath)));
    FS.update_path(out, "$app_data_root$", name);
    return out[0] != 0;
}

u64 HeightmapCacheKey(u32 triCount, const DetailHeader& H)
{
    string_path fn;
    FS.update_path(fn, "$level$", "level.cform");
    const u32 ageCform = FS.get_file_age(fn);
    FS.update_path(fn, "$level$", "level.details");
    const u32 ageDetails = FS.get_file_age(fn);

    struct Key
    {
        u32 tri, ageC, ageD, ver, obj, sx, sz;
        int ox, oz;
    } k{triCount, ageCform, ageDetails, H.version, H.object_count, H.size_x, H.size_z, H.offs_x, H.offs_z};

    return ((u64)crc32(&k, sizeof(k)) << 32) | (u64)triCount;
}

bool HeightmapCacheLoad(const char* path, u64 key, u32 w, u32 h, xr_vector<float>& data)
{
    FILE* f = fopen(XR_OSPATH(path), "rb");
    if (!f)
        return false;

    HMCacheHeader hdr{};
    const bool ok = fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == kHMCacheMagic &&
                    hdr.version == kHMCacheVersion && hdr.key == key && hdr.w == w && hdr.h == h &&
                    fread(data.data(), sizeof(float), size_t(w) * h, f) == size_t(w) * h;
    fclose(f);
    return ok;
}

void HeightmapCacheSave(const char* path, u64 key, u32 w, u32 h, const xr_vector<float>& data)
{
    FILE* f = fopen(XR_OSPATH(path), "wb");
    if (!f) {
        Msg("![VK Grass] heightmap cache: cannot write '%s' — baking again next load", path);
        return;
    }
    const HMCacheHeader hdr{kHMCacheMagic, kHMCacheVersion, key, w, h};
    const bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 &&
                    fwrite(data.data(), sizeof(float), size_t(w) * h, f) == size_t(w) * h;
    fclose(f);
    if (!ok)
        Msg("![VK Grass] heightmap cache: write to '%s' failed (disk full?)", path);
}

}   // namespace

void CDetailManager::BakeHeightmap()
{
    if (!g_pGameLevel) {
        Msg("![VK Grass] g_pGameLevel null — heightmap baking skipped");
        return;
    }
    CDB::MODEL* model = g_pGameLevel->ObjectSpace.GetStaticModel();
    if (!model) {
        Msg("![VK Grass] static collision model unavailable — heightmap baking skipped");
        return;
    }

    Fvector*  verts = g_pGameLevel->ObjectSpace.GetStaticVerts();
    CDB::TRI* tris  = g_pGameLevel->ObjectSpace.GetStaticTris();
    const u32 triCount = model->get_tris_count();

    // World bounds derived from the detail header — same coordinate frame
    // the gen compute uses (no extra transform required at sample time).
    m_HMOriginX    = -float(dtH.offs_x) * dm_slot_size;
    m_HMOriginZ    = -float(dtH.offs_z) * dm_slot_size;
    m_HMWorldSizeX = float(dtH.size_x) * dm_slot_size;
    m_HMWorldSizeZ = float(dtH.size_z) * dm_slot_size;

    m_HeightmapW = _min(u32(ceilf(m_HMWorldSizeX)), 2048u);
    m_HeightmapH = _min(u32(ceilf(m_HMWorldSizeZ)), 2048u);
    if (m_HeightmapW == 0 || m_HeightmapH == 0) {
        Msg("![VK Grass] degenerate heightmap dims (%ux%u) — abort", m_HeightmapW, m_HeightmapH);
        return;
    }

    const float texelSizeX = m_HMWorldSizeX / float(m_HeightmapW);
    const float texelSizeZ = m_HMWorldSizeZ / float(m_HeightmapH);

    xr_vector<float> heightData(m_HeightmapW * m_HeightmapH, 10000.0f);

    // Cache first — see the note above the helpers. On a HIT `heightData` is the
    // finished map, so both passes below are skipped through their loop conditions
    // (`hmMiss &&`) rather than by wrapping 80 lines in an `if`: the guard costs one
    // predictable test per iteration on the miss path and keeps this diff readable.
    string_path hmPath;
    const bool  hmHavePath = HeightmapCachePath(hmPath);
    const u64   hmKey      = HeightmapCacheKey(triCount, dtH);
    CTimer      hmTimer;
    hmTimer.Start();
    const bool  hmMiss = !(hmHavePath && HeightmapCacheLoad(hmPath, hmKey, m_HeightmapW, m_HeightmapH, heightData));
    if (!hmMiss)
        Msg("[VK Grass] heightmap cache HIT '%s' — %u ms (bake skipped)", hmPath, hmTimer.GetElapsed_ms());
    else
        Msg("[VK Grass] Baking heightmap %ux%u from %u tris (%.0fx%.0fm)…",
            m_HeightmapW, m_HeightmapH, triCount, m_HMWorldSizeX, m_HMWorldSizeZ);

    u32 rasterized = 0;
    for (u32 t = 0; hmMiss && t < triCount; ++t) {
        CDB::TRI& T = tris[t];
        Fvector v0 = verts[T.verts[0]];
        Fvector v1 = verts[T.verts[1]];
        Fvector v2 = verts[T.verts[2]];

        Fvector n; n.mknormal(v0, v1, v2);
        if (n.y < 0.3f) continue;   // skip walls/ceilings

        const float px0 = (v0.x - m_HMOriginX) / texelSizeX, pz0 = (v0.z - m_HMOriginZ) / texelSizeZ;
        const float px1 = (v1.x - m_HMOriginX) / texelSizeX, pz1 = (v1.z - m_HMOriginZ) / texelSizeZ;
        const float px2 = (v2.x - m_HMOriginX) / texelSizeX, pz2 = (v2.z - m_HMOriginZ) / texelSizeZ;

        const int minPX = _max(0,                       (int)floorf(_min(_min(px0, px1), px2)));
        const int maxPX = _min((int)m_HeightmapW - 1,   (int)ceilf (_max(_max(px0, px1), px2)));
        const int minPZ = _max(0,                       (int)floorf(_min(_min(pz0, pz1), pz2)));
        const int maxPZ = _min((int)m_HeightmapH - 1,   (int)ceilf (_max(_max(pz0, pz1), pz2)));

        const float dx10 = px1 - px0, dz10 = pz1 - pz0;
        const float dx20 = px2 - px0, dz20 = pz2 - pz0;
        const float denom = dx10 * dz20 - dx20 * dz10;
        if (_abs(denom) < 1e-6f) continue;       // degenerate
        const float invDenom = 1.0f / denom;

        for (int pz = minPZ; pz <= maxPZ; ++pz) {
            for (int px = minPX; px <= maxPX; ++px) {
                const float qx = float(px) + 0.5f - px0;
                const float qz = float(pz) + 0.5f - pz0;
                const float u  = (qx * dz20 - qz * dx20) * invDenom;
                const float v  = (dx10 * qz - dz10 * qx) * invDenom;
                if (u < -0.01f || v < -0.01f || (u + v) > 1.01f) continue;
                const float y = v0.y + u * (v1.y - v0.y) + v * (v2.y - v0.y);
                const u32 idx = u32(pz) * m_HeightmapW + u32(px);

                // Authored-range filter: UNDERGROUND floors (tunnels, bunkers)
                // face up too, so the plain MIN picked the basement floor for
                // every surface texel above one — the gen's interior guard
                // (terrainY < y_base-1) then rejected ALL surface grass there
                // = the "field 300×300 without grass" holes. Accept a candidate
                // only inside the slot's authored detail Y range (±2 m). Slots
                // with no details keep the raw MIN (their texels only feed
                // border gathers; empty slots never spawn grass anyway).
                const float wx = m_HMOriginX + (float(px) + 0.5f) * texelSizeX;
                const float wz = m_HMOriginZ + (float(pz) + 0.5f) * texelSizeZ;
                DetailSlot& ds = QueryDB(int(floorf(wx / dm_slot_size)), int(floorf(wz / dm_slot_size)));
                const bool slotHasDetails =
                    ds.r_id(0) != DetailSlot::ID_Empty || ds.r_id(1) != DetailSlot::ID_Empty ||
                    ds.r_id(2) != DetailSlot::ID_Empty || ds.r_id(3) != DetailSlot::ID_Empty;
                if (slotHasDetails) {
                    const float yb = ds.r_ybase();
                    if (y < yb - 2.0f || y > yb + ds.r_yheight() + 2.0f) continue;
                }

                if (y < heightData[idx]) heightData[idx] = y;
            }
        }
        ++rasterized;
    }

    // Hole fill: any sentinel-marked texel takes a 5×5 neighbour mean.
    for (u32 z = 0; hmMiss && z < m_HeightmapH; ++z) {
        for (u32 x = 0; x < m_HeightmapW; ++x) {
            const u32 idx = z * m_HeightmapW + x;
            if (heightData[idx] < 9999.0f) continue;
            float sum = 0; int count = 0;
            for (int dz = -2; dz <= 2; ++dz) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int nx = int(x) + dx, nz = int(z) + dz;
                    if (nx < 0 || nx >= int(m_HeightmapW) || nz < 0 || nz >= int(m_HeightmapH)) continue;
                    const float h = heightData[u32(nz) * m_HeightmapW + u32(nx)];
                    if (h < 9999.0f) { sum += h; ++count; }
                }
            }
            // 9000.0f is the GPU sentinel — preserve when truly isolated.
            heightData[idx] = (count > 0) ? (sum / float(count)) : 9000.0f;
        }
    }

    if (hmMiss) {
        Msg("[VK Grass] Rasterized %u tris in %u ms, uploading R32F image…", rasterized, hmTimer.GetElapsed_ms());
        if (hmHavePath)
            HeightmapCacheSave(hmPath, hmKey, m_HeightmapW, m_HeightmapH, heightData);
    }

    // Allocate device-local R32F image via VMA.
    if (!VK::CreateImage2D(VK_FORMAT_R32_SFLOAT, { m_HeightmapW, m_HeightmapH },
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            m_HeightmapImage, m_HeightmapAlloc, "Grass.Heightmap")) {
        return;
    }
    m_HeightmapView = VK::CreateImageView(m_HeightmapImage, VK_FORMAT_R32_SFLOAT);

    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(VulkanHW.m_Device, &sci, nullptr, &m_HeightmapSampler);

    // Stage upload via host buffer + one-shot copy command.
    const u32 dataSize = m_HeightmapW * m_HeightmapH * sizeof(float);
    VK::CVulkanBuffer staging;
    staging.Create(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    if (void* mapped = staging.Map()) {
        memcpy(mapped, heightData.data(), dataSize);
        staging.Flush();
    }

    VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
    if (cmd != VK_NULL_HANDLE) {
        VkImageMemoryBarrier b{};
        b.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask        = 0;
        b.dstAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout            = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
        b.image                = m_HeightmapImage;
        b.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);

        VkBufferImageCopy r{};
        r.bufferOffset      = 0;
        r.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.imageExtent       = { m_HeightmapW, m_HeightmapH, 1 };
        vkCmdCopyBufferToImage(cmd, staging.GetHandle(), m_HeightmapImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);

        VulkanHW.EndSingleTimeCommands(cmd);
    }
    staging.Destroy();

    Msg("[VK Grass] Heightmap %ux%u baked (%.1fMB), origin=(%.1f, %.1f)",
        m_HeightmapW, m_HeightmapH, dataSize / (1024.0f * 1024.0f), m_HMOriginX, m_HMOriginZ);
}

// ============================================================================
// UploadSlotData — pack DetailSlot bitfield array into GpuSlotPacked SSBO.
// 32 B/slot. Lighting fields keep R4 quantisation (4-bit→16-bit fixed),
// palette alphas widen 4-bit→8-bit (a*255/15). Compute shader bilerps.
// ============================================================================
// Row-parallel for. UploadSlotData walks the whole slot grid twice (a 7x7 heal and
// the pack) and measured 232 ms of an 1.9 s post-visuals budget; rows are
// independent and each output index is written by exactly one row, so this is a
// plain split with no shared state.
static void ParallelRows(int rows, const std::function<void(int, int)>& body)
{
    u32 nThr = std::thread::hardware_concurrency();
    nThr = _min(_max(1u, nThr), 16u);
    if (rows <= 64 || nThr <= 1) { body(0, rows); return; }
    xr_vector<std::thread> pool;
    pool.reserve(nThr);
    const int chunk = (rows + int(nThr) - 1) / int(nThr);
    for (u32 w = 0; w < nThr; ++w) {
        const int lo = int(w) * chunk, hi = _min(lo + chunk, rows);
        if (lo >= hi) break;
        pool.emplace_back([&body, lo, hi] { body(lo, hi); });
    }
    for (auto& th : pool) th.join();
}

void CDetailManager::UploadSlotData()
{
    if (!dtSlots) return;

    m_TotalSlots = dtH.size_x * dtH.size_z;
    const u32 dataSize = m_TotalSlots * u32(sizeof(GpuSlotPacked));

    // Anomaly/GAMMA level.details bakes carry BUGGED full-black slots: c_hemi==0
    // blobs under tree crowns (SSFX's own deffer_grass.vs clamps with the comment
    // "Some spots are bugged (Full black)"). Heal instead of just clamping: rebuild
    // each zero slot from the healthy 7x7 neighbourhood mean. Small crown blobs
    // fill in with plausible shade; large legitimately dark areas (building
    // interiors) keep zero neighbours and stay dark. The SSFX 0.05 floor is
    // applied on top at pack time below.
    // dtSlots is a VIEW INTO THE MAPPED level.details chunk, so this first read
    // is where 3.7M slots get paged in — on one thread it hid ~10 ms of page
    // faults inside the copy. Same split as the heal below.
    xr_vector<u8> hemiHealed(m_TotalSlots);
    ParallelRows(int(m_TotalSlots), [&](int i0, int i1) {
        for (u32 i = u32(i0); i < u32(i1); ++i) hemiHealed[i] = u8(dtSlots[i].c_hemi);
    });
    u32 healedCnt = 0;
    {
        const int W = int(dtH.size_x), H = int(dtH.size_z);
        std::atomic<u32> healedAtomic{ 0 };
        ParallelRows(H, [&](int z0, int z1) {
            u32 local = 0;
            for (int z = z0; z < z1; ++z)
            for (int x = 0; x < W; ++x) {
                const u32 i = u32(z) * u32(W) + u32(x);
                if (dtSlots[i].c_hemi != 0) continue;
                u32 sum = 0, cnt = 0;
                for (int dz = -3; dz <= 3; ++dz)
                    for (int dx = -3; dx <= 3; ++dx) {
                        const int nx = x + dx, nz = z + dz;
                        if (nx < 0 || nx >= W || nz < 0 || nz >= H) continue;
                        const u32 v = dtSlots[u32(nz) * u32(W) + u32(nx)].c_hemi;
                        if (v > 0) { sum += v; ++cnt; }
                    }
                // Reads dtSlots (never written here) and writes only its own index,
                // so neighbour rows handled by other threads stay consistent.
                if (cnt >= 3) { hemiHealed[i] = u8((sum + cnt / 2) / cnt); ++local; }
            }
            healedAtomic += local;
        });
        healedCnt = healedAtomic.load();
    }
    if (healedCnt)
        Msg("[VK Grass] healed %u bugged hemi==0 slots (of %u) from neighbours", healedCnt, m_TotalSlots);

    xr_vector<GpuSlotPacked> packed(m_TotalSlots);
    ParallelRows(int(m_TotalSlots), [&](int i0, int i1) {
    for (u32 i = u32(i0); i < u32(i1); ++i) {
        DetailSlot& ds = dtSlots[i];
        GpuSlotPacked& p = packed[i];
        p.y_base   = ds.r_ybase();
        p.y_height = ds.r_yheight();
        p.ids      = u32(ds.id0)
                  | (u32(ds.id1) << 8)
                  | (u32(ds.id2) << 16)
                  | (u32(ds.id3) << 24);
        // hemi: healed value + the SSFX 0.05 floor (their deffer_grass.vs clamp).
        const float hemiF = std::max(float(hemiHealed[i]) / 15.0f, 0.05f);
        p.lighting = u32(ds.r_qclr(ds.c_dir,  15) * 65535.0f)
                  | (u32(hemiF * 65535.0f) << 16);

        auto packPal = [](const DetailPalette& pal) -> u32 {
            const u32 a0 = u32(float(pal.a0) / 15.0f * 255.0f);
            const u32 a1 = u32(float(pal.a1) / 15.0f * 255.0f);
            const u32 a2 = u32(float(pal.a2) / 15.0f * 255.0f);
            const u32 a3 = u32(float(pal.a3) / 15.0f * 255.0f);
            return a0 | (a1 << 8) | (a2 << 16) | (a3 << 24);
        };
        p.palette0 = packPal(ds.palette[0]);
        p.palette1 = packPal(ds.palette[1]);
        p.palette2 = packPal(ds.palette[2]);
        p.palette3 = packPal(ds.palette[3]);
    }
    });

    m_SlotDataSSBO = xr_new<VK::CVulkanBuffer>();
    m_SlotDataSSBO->Create(dataSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_SlotDataSSBO->Upload(packed.data(), dataSize);

    Msg("[VK Grass] Slot SSBO uploaded: %u slots (%.1f KB)", m_TotalSlots, dataSize / 1024.0f);
}

// ============================================================================
// UploadObjInfo — per-detail-type metadata for compute frustum cull / scale.
// ============================================================================
void CDetailManager::UploadObjInfo()
{
    if (objects.empty()) return;

    const u32 count = u32(objects.size());
    const u32 dataSize = count * u32(sizeof(GpuDetailObjInfo));

    xr_vector<GpuDetailObjInfo> infos(count);
    for (u32 i = 0; i < count; ++i) {
        const VK::CDetail* obj = objects[i];
        if (obj) {
            infos[i].minScale = obj->m_MinScale;
            infos[i].maxScale = obj->m_MaxScale;
            infos[i].bvRadius = obj->bv_sphere.R;
            infos[i].flags    = obj->m_Flags;
        } else {
            infos[i] = { 1.0f, 1.0f, 1.0f, 0u };
        }
    }

    m_ObjInfoSSBO = xr_new<VK::CVulkanBuffer>();
    m_ObjInfoSSBO->Create(dataSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_ObjInfoSSBO->Upload(infos.data(), dataSize);

    Msg("[VK Grass] ObjInfo SSBO uploaded: %u types (%.1f KB)", count, dataSize / 1024.0f);
}

// ============================================================================
// Session B: GPU buffers — VisibleSSBO (compute output → instance VB),
// IndirectCmdBuf, AtomicCounters, GenUBO.
// ============================================================================
void CDetailManager::CreateGpuBuffers()
{
    m_OutputCapacity  = GPU_OUTPUT_CAPACITY;   // per-level manager: start small, grow to demand
    m_PendingCapacity = 0;
    m_VisibleSSBO = xr_new<VK::CVulkanBuffer>();
    m_VisibleSSBO->Create(
        u64(m_OutputCapacity) * sizeof(DetailInstance),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT  |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    m_IndirectCmdBuf = xr_new<VK::CVulkanBuffer>();
    m_IndirectCmdBuf->Create(
        GPU_MAX_OBJ_TYPES * sizeof(VkDrawIndexedIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT  |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    m_AtomicCounters = xr_new<VK::CVulkanBuffer>();
    m_AtomicCounters->Create(
        (GPU_MAX_OBJ_TYPES * 2 + 4) * sizeof(u32),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT   |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    // Host mirror of counters[0..127] (used [0..63] + dropped [64..127] per
    // type) — read a few frames stale, logged throttled from Render, and
    // drives the auto-grow. Overflow used to be SILENT and showed up as whole
    // fields without grass (per-type section exhausted).
    m_OverflowRB = xr_new<VK::CVulkanBuffer>();
    m_OverflowRB->Create(2u * GPU_MAX_OBJ_TYPES * sizeof(u32),
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    m_OverflowPtr = (u32*)m_OverflowRB->Map();
    if (m_OverflowPtr) memset(m_OverflowPtr, 0, 2u * GPU_MAX_OBJ_TYPES * sizeof(u32));

    // GenUBO: host-visible so PrepareFrame can rewrite without a staging copy.
    m_GenUBO = xr_new<VK::CVulkanBuffer>();
    m_GenUBO->Create(
        sizeof(DetailGenUBO),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST);

    // Shadow-CASTER set: second (smaller) instance stream from the caster gen
    // dispatch — distance-only cull, so blades outside the camera frustum still
    // cast into the spot/cascade/cube/VSM shadow maps.
    m_CasterSSBO = xr_new<VK::CVulkanBuffer>();
    m_CasterSSBO->Create(
        u64(GPU_CASTER_CAPACITY) * sizeof(DetailInstance),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT  |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    m_CasterIndirectBuf = xr_new<VK::CVulkanBuffer>();
    m_CasterIndirectBuf->Create(
        GPU_MAX_OBJ_TYPES * sizeof(VkDrawIndexedIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT  |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    m_CasterAtomic = xr_new<VK::CVulkanBuffer>();
    m_CasterAtomic->Create(
        (GPU_MAX_OBJ_TYPES * 2 + 4) * sizeof(u32),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT   |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    Msg("[VK Grass] GPU buffers: visible=%.1fMB (auto-grow, cap %.0fMB) indirect=%.1fKB atomic=%.1fKB ubo=64B sectionSize=%u",
        u64(m_OutputCapacity) * sizeof(DetailInstance) / (1024.0f * 1024.0f),
        u64(GPU_OUTPUT_CAP_MAX) * sizeof(DetailInstance) / (1024.0f * 1024.0f),
        GPU_MAX_OBJ_TYPES * sizeof(VkDrawIndexedIndirectCommand) / 1024.0f,
        (GPU_MAX_OBJ_TYPES * 2 + 4) * sizeof(u32) / 1024.0f,
        m_OutputCapacity / _max(u32(objects.size()), 1u));

    // One-shot fill of IndirectCmdBuf: indexCount/firstIndex/vertexOffset/
    // firstInstance never change during the level. instanceCount is patched
    // each frame by the atomic→indirect copy (see Render's Phase 7). Doing
    // the static fill once at load avoids ~21 × 20 B vkCmdUpdateBuffer per
    // frame plus the implicit transfer barrier it triggers.
    {
        const u32 nObj = u32(objects.size());
        xr_vector<VkDrawIndexedIndirectCommand> cmds(GPU_MAX_OBJ_TYPES);
        for (u32 i = 0; i < GPU_MAX_OBJ_TYPES; ++i) {
            cmds[i].indexCount    = (i < nObj && objects[i]) ? objects[i]->m_IndexCount : 0;
            cmds[i].instanceCount = 0;
            cmds[i].firstIndex    = 0;
            cmds[i].vertexOffset  = 0;
            cmds[i].firstInstance = 0;
        }

        // Stage upload via host buffer + one-shot copy command (vkCmdUpdateBuffer
        // also works but is capped at 64 KB; this is safer for the full 1.3 KB).
        const VkDeviceSize sz = GPU_MAX_OBJ_TYPES * sizeof(VkDrawIndexedIndirectCommand);
        VK::CVulkanBuffer staging;
        staging.Create(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        if (void* m = staging.Map()) { memcpy(m, cmds.data(), sz); staging.Flush(); }
        VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
        if (cmd != VK_NULL_HANDLE) {
            VkBufferCopy r{ 0, 0, sz };
            vkCmdCopyBuffer(cmd, staging.GetHandle(), m_IndirectCmdBuf->GetHandle(), 1, &r);
            if (m_CasterIndirectBuf)   // same static fields; instanceCount patched per frame
                vkCmdCopyBuffer(cmd, staging.GetHandle(), m_CasterIndirectBuf->GetHandle(), 1, &r);
            VulkanHW.EndSingleTimeCommands(cmd);
        }
        staging.Destroy();
    }
}

void CDetailManager::DestroyGpuBuffers()
{
    if (m_VisibleSSBO)    { m_VisibleSSBO->Destroy();    xr_delete(m_VisibleSSBO); }
    if (m_IndirectCmdBuf) { m_IndirectCmdBuf->Destroy(); xr_delete(m_IndirectCmdBuf); }
    if (m_AtomicCounters) { m_AtomicCounters->Destroy(); xr_delete(m_AtomicCounters); }
    if (m_OverflowRB)     { m_OverflowRB->Destroy();     xr_delete(m_OverflowRB); m_OverflowPtr = nullptr; }
    if (m_GenUBO)         { m_GenUBO->Destroy();         xr_delete(m_GenUBO); }
    if (m_CasterSSBO)        { m_CasterSSBO->Destroy();        xr_delete(m_CasterSSBO); }
    if (m_CasterIndirectBuf) { m_CasterIndirectBuf->Destroy(); xr_delete(m_CasterIndirectBuf); }
    if (m_CasterAtomic)      { m_CasterAtomic->Destroy();      xr_delete(m_CasterAtomic); }
}

// ============================================================================
// Helper: 1×1 image with given pixel value in TRANSFER_DST → SHADER_READ_ONLY.
// ============================================================================
namespace {
bool Create1x1Image(VkFormat fmt, const void* pixel, u32 pixelSize,
                    VkImage& outImage, VmaAllocation& outAlloc, VkImageView& outView)
{
    if (!VK::CreateImage2D(fmt, { 1, 1 },
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            outImage, outAlloc))
        return false;

    outView = VK::CreateImageView(outImage, fmt);

    VK::CVulkanBuffer staging;
    staging.Create(pixelSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    if (void* m = staging.Map()) { memcpy(m, pixel, pixelSize); staging.Flush(); }

    VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
    if (cmd != VK_NULL_HANDLE) {
        VkImageMemoryBarrier b{};
        b.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout            = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
        b.image                = outImage;
        b.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.dstAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        VkBufferImageCopy r{};
        r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.imageExtent      = { 1, 1, 1 };
        vkCmdCopyBufferToImage(cmd, staging.GetHandle(), outImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        VulkanHW.EndSingleTimeCommands(cmd);
    }
    staging.Destroy();
    return true;
}
}  // namespace

void CDetailManager::CreateDummyTextures()
{
    // HZB: white = depth 1.0 → instanceDepth > 1.0 never → no occlusion cull.
    const u32 white = 0x3F800000u;   // 1.0f bits, but we read .r as float → fine for R32F too
    const float whiteF = 1.0f;
    Create1x1Image(VK_FORMAT_R32_SFLOAT, &whiteF, sizeof(float),
                   m_DummyHZBImage, m_DummyHZBAlloc, m_DummyHZBView);

    // Trail: black = no press-down.
    const float blackF = 0.0f;
    Create1x1Image(VK_FORMAT_R32_SFLOAT, &blackF, sizeof(float),
                   m_DummyTrailImage, m_DummyTrailAlloc, m_DummyTrailView);

    (void)white;  // silence unused
}

void CDetailManager::DestroyDummyTextures()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (m_DummyHZBView)   { vkDestroyImageView(VulkanHW.m_Device, m_DummyHZBView,   nullptr); m_DummyHZBView   = VK_NULL_HANDLE; }
    if (m_DummyHZBImage)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, m_DummyHZBImage,  m_DummyHZBAlloc); m_DummyHZBImage = VK_NULL_HANDLE; }
    if (m_DummyTrailView) { vkDestroyImageView(VulkanHW.m_Device, m_DummyTrailView, nullptr); m_DummyTrailView = VK_NULL_HANDLE; }
    if (m_DummyTrailImage){ VK::Vram::DestroyImage(VulkanHW.m_Allocator, m_DummyTrailImage,m_DummyTrailAlloc); m_DummyTrailImage = VK_NULL_HANDLE; }
}

// ============================================================================
// HZB — Hierarchical-Z occlusion pyramid.
//
// Built each frame inside Render() from the scene depth buffer (which already
// holds Pass_World statics + dynamics). A max-reduce compute pass produces a
// mip chain; the gen compute samples it (binding 6) and drops instances that
// are farther than the farthest surface in their screen region — i.e. hidden
// behind world geometry. mip0 is half the depth resolution.
//
// Layout: the HZB image stays in GENERAL for its whole lifetime (storage write
// each pass + sampled read by both later passes and the gen shader). The depth
// buffer is transitioned DEPTH_ATTACHMENT_OPTIMAL→SHADER_READ_ONLY and back
// around the build (see BuildHZB) — a deliberate, localized exception to the
// single-layout convention, owned entirely by the grass pass.
// ============================================================================
namespace {
inline u32 MipDim(u32 base, u32 level) { const u32 v = base >> level; return v ? v : 1u; }
}  // namespace

void CDetailManager::CreateHZB(u32 depthW, u32 depthH)
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (depthW == 0 || depthH == 0) return;
    if (Swapchain.m_DepthImage == VK_NULL_HANDLE) {
        Msg("![VK Grass] HZB: no depth image — occlusion culling disabled");
        return;
    }

    // mip0 = half depth res; full mip chain down to 1×1.
    m_HZBWidth    = _max(1u, depthW / 2u);
    m_HZBHeight   = _max(1u, depthH / 2u);
    m_HZBMipCount = 1u;
    for (u32 d = _max(m_HZBWidth, m_HZBHeight); d > 1u; d >>= 1) ++m_HZBMipCount;
    m_HZBDepthExtent = { depthW, depthH };

    // ----- HZB image (R32F, mip chain, STORAGE+SAMPLED) --------------------
    VK::ImageDesc hzb;
    hzb.format = VK_FORMAT_R32_SFLOAT;
    hzb.extent = { m_HZBWidth, m_HZBHeight, 1 };
    hzb.mips   = m_HZBMipCount;
    hzb.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    hzb.name   = "Grass.HZB";
    if (!VK::CreateImage(hzb, m_HZBImage, m_HZBAlloc)) return;

    // Full-mip sampled view (gen binding 6 + build source for HZB passes).
    m_HZBView = VK::CreateImageView(m_HZBImage, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D,
                                    VK_IMAGE_ASPECT_COLOR_BIT, 0, m_HZBMipCount);
    // Bumped on every (re)creation so consumers caching a descriptor can tell
    // "same pyramid" from "rebuilt pyramid". Comparing the raw VkImageView is not
    // enough: after Destroy+Create the driver may hand back the SAME numeric
    // handle, and a cache keyed on it would keep a descriptor pointing at the
    // destroyed object. See vk_TreeManager_Render.cpp's binding-4 write.
    ++m_HZBGeneration;

    // Per-mip single-level storage views (build destination).
    m_HZBMipViews.resize(m_HZBMipCount, VK_NULL_HANDLE);
    for (u32 m = 0; m < m_HZBMipCount; ++m)
        m_HZBMipViews[m] = VK::CreateImageView(m_HZBImage, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D,
                                               VK_IMAGE_ASPECT_COLOR_BIT, m, 1);

    // DEPTH-aspect view of the swapchain depth (first build pass source).
    // Explicit DEPTH-only aspect so combined D32_S8 formats stay samplable.
    m_DepthSampleView = VK::CreateImageView(Swapchain.m_DepthImage, Swapchain.m_DepthFormat,
                                            VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1);

    // Sampler — nearest + clamp; the build shader picks exact texels itself,
    // the gen shader reads explicit mips. CLAMP avoids wrap at screen edges.
    VkSamplerCreateInfo sci{};
    sci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter     = VK_FILTER_NEAREST;
    sci.minFilter     = VK_FILTER_NEAREST;
    sci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.minLod        = 0.0f;
    sci.maxLod        = float(m_HZBMipCount);
    vkCreateSampler(VulkanHW.m_Device, &sci, nullptr, &m_HZBSampler);

    // ----- One-time UNDEFINED → GENERAL for all mips -----------------------
    if (VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands()) {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_HZBImage;
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_HZBMipCount, 0, 1 };
        b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        VulkanHW.EndSingleTimeCommands(cmd);
        m_bHZBLayoutInit = true;
    }

    // ----- Build compute pipeline + per-mip descriptor sets ----------------
    if (!g_ShaderManager) { Msg("![VK Grass] HZB: g_ShaderManager null — build disabled"); return; }

    // 0 = src mip (sampled), 1 = dst mip (storage). One set per mip.
    m_HZBDescSets.resize(m_HZBMipCount, VK_NULL_HANDLE);
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },
                                m_HZBMipCount, m_HZBDescLayout, m_HZBDescPool,
                                m_HZBDescSets.data(), VK_SHADER_STAGE_COMPUTE_BIT, "Grass.HZBBuild"))
        return;

    m_HZBPipelineLayout = VK::MakePipelineLayout({ m_HZBDescLayout }, sizeof(HZBBuildPush));

    VkShaderModule cs = g_ShaderManager->Load("hzb_build.comp.spv");
    if (cs == VK_NULL_HANDLE) { Msg("![VK Grass] hzb_build.comp.spv load failed"); return; }

    m_HZBPipeline = VK::CreateComputePipeline(cs, m_HZBPipelineLayout, "Grass.HZBBuild");
    if (m_HZBPipeline == VK_NULL_HANDLE) return;

    // Write the descriptor sets once — all views are stable for the HZB's life.
    // set[0]: src = depth (SHADER_READ_ONLY at sample time), dst = mip0.
    // set[i]: src = HZB full view (GENERAL),               dst = mip i.
    for (u32 m = 0; m < m_HZBMipCount; ++m) {
        VK::DescriptorWriter(m_HZBDescSets[m])
            .ImageSampler(0, (m == 0) ? m_DepthSampleView : m_HZBView, m_HZBSampler,
                             (m == 0) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                      : VK_IMAGE_LAYOUT_GENERAL)
            .StorageImage(1, m_HZBMipViews[m])
            .Flush();
    }

    Msg("[VK Grass] HZB ready: %ux%u, %u mips (depth %ux%u)",
        m_HZBWidth, m_HZBHeight, m_HZBMipCount, depthW, depthH);
}

void CDetailManager::DestroyHZB()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    VK_DeviceWaitIdleLocked();   // HZB views/sets may be in flight

    if (m_HZBPipeline)       { vkDestroyPipeline(VulkanHW.m_Device, m_HZBPipeline, nullptr); m_HZBPipeline = VK_NULL_HANDLE; }
    if (m_HZBPipelineLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, m_HZBPipelineLayout, nullptr); m_HZBPipelineLayout = VK_NULL_HANDLE; }
    if (m_HZBDescPool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, m_HZBDescPool, nullptr); m_HZBDescPool = VK_NULL_HANDLE; }
    m_HZBDescSets.clear();
    if (m_HZBDescLayout)     { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, m_HZBDescLayout, nullptr); m_HZBDescLayout = VK_NULL_HANDLE; }

    if (m_HZBSampler)        { vkDestroySampler(VulkanHW.m_Device, m_HZBSampler, nullptr); m_HZBSampler = VK_NULL_HANDLE; }
    if (m_DepthSampleView)   { vkDestroyImageView(VulkanHW.m_Device, m_DepthSampleView, nullptr); m_DepthSampleView = VK_NULL_HANDLE; }
    for (VkImageView v : m_HZBMipViews) if (v) vkDestroyImageView(VulkanHW.m_Device, v, nullptr);
    m_HZBMipViews.clear();
    if (m_HZBView)           { vkDestroyImageView(VulkanHW.m_Device, m_HZBView, nullptr); m_HZBView = VK_NULL_HANDLE; }
    if (m_HZBImage)          { VK::Vram::DestroyImage(VulkanHW.m_Allocator, m_HZBImage, m_HZBAlloc); m_HZBImage = VK_NULL_HANDLE; }

    m_HZBWidth = m_HZBHeight = m_HZBMipCount = 0;
    m_HZBDepthExtent = { 0, 0 };
    m_bHZBLayoutInit = false;
}

// ============================================================================
// Per-detail-type DDS load + descriptor allocation. Each detail object's
// diffuse texture sits in $level$ or $game_textures$. Sampler is shared.
// ============================================================================
void CDetailManager::LoadDetailTextures()
{
    // Sampler — anisotropic linear, repeat (atlas-style detail textures often
    // tile within their own UV space; addressing mode REPEAT is monolith default).
    VkSamplerCreateInfo sci{};
    sci.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter        = VK_FILTER_LINEAR;
    sci.minFilter        = VK_FILTER_LINEAR;
    sci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.minLod           = 0.0f;
    sci.maxLod           = VK_LOD_CLAMP_NONE;
    {
        // r__tf_aniso (default 16 = the old fixed value); the Switch preset sets 1 -- 16x costs a lot of texel bandwidth on GM20B.
        const int aniso = ps_r__tf_Anisotropic < 1 ? 1 : (ps_r__tf_Anisotropic > 16 ? 16 : ps_r__tf_Anisotropic);
        sci.anisotropyEnable = aniso > 1 ? VK_TRUE : VK_FALSE;
        sci.maxAnisotropy    = float(aniso);
    }
    vkCreateSampler(VulkanHW.m_Device, &sci, nullptr, &m_DetailSampler);

    m_DetailTextures.resize(objects.size(), nullptr);
    std::unordered_map<std::string, VK::CVulkanTexture*> byPath;   // dedup: see m_DetailTexOwned
    for (size_t i = 0; i < objects.size(); ++i) {
        const VK::CDetail* obj = objects[i];
        if (!obj || obj->m_TextureName.size() == 0) continue;

        string_path leaf;
        xr_sprintf(leaf, "%s.dds", obj->m_TextureName.c_str());
        string_path full;
        FS.update_path(full, "$level$", leaf);
        if (!FS.exist(full)) {
            FS.update_path(full, "$game_textures$", leaf);
            if (!FS.exist(full)) {
                Msg("![VK Grass] detail texture not found: %s.dds — using fallback", obj->m_TextureName.c_str());
                continue;
            }
        }

        // Share by resolved path: detail objects almost always name the SAME level
        // atlas, and a texture object per object meant a full GPU copy per object.
        if (auto it = byPath.find(full); it != byPath.end()) {
            m_DetailTextures[i] = it->second;
            continue;
        }

        auto* tex = xr_new<VK::CVulkanTexture>();
        // Grass blade albedo — Colour. The stream class is passed explicitly only
        // because the colourspace argument follows it; it keeps the historical
        // TexStreamClass::UI default (preserved as-is, not endorsed — changing grass
        // residency is a separate question from colourspace).
        if (!tex->LoadDDS(full, /*applyBCSwizzle*/ false, VK::TexStreamClass::UI,
                          VK::TexColorSpace::Color)) {
            Msg("![VK Grass] LoadDDS failed for %s", full);
            xr_delete(tex);
            continue;
        }
        m_DetailTextures[i] = tex;
        m_DetailTexOwned.push_back(tex);
        byPath.emplace(full, tex);
    }

    // SSFX wind flow map (s_waves). Sampled in the grass vertex shader to drive
    // the flow-map wind. fx\wind_wave.dds ships with the SSFX "10 - Wind" module.
    {
        string_path full;
        FS.update_path(full, "$game_textures$", "fx\\wind_wave.dds");
        if (FS.exist(full)) {
            auto* tex = xr_new<VK::CVulkanTexture>();
            if (tex->LoadDDS(full, /*applyBCSwizzle*/ false)) {
                m_WaveTex = tex;
                Msg("[VK Grass] SSFX wind flow map loaded: fx\\wind_wave.dds");
            } else {
                Msg("![VK Grass] LoadDDS failed for fx\\wind_wave.dds — grass wind will be flat");
                xr_delete(tex);
            }
        } else {
            Msg("![VK Grass] fx\\wind_wave.dds not found — grass wind will be flat (deploy the SSFX wind texture)");
        }
    }

    Msg("[VK Grass] Detail textures: %u loaded", (u32)m_DetailTextures.size());
}

void CDetailManager::DestroyDetailTextures()
{
    // Free the OWNER list — m_DetailTextures holds repeated references into it.
    for (auto* t : m_DetailTexOwned) {
        if (t) { t->Destroy(); xr_delete(t); }
    }
    m_DetailTexOwned.clear();
    m_DetailTextures.clear();
    if (m_WaveTex) { m_WaveTex->Destroy(); xr_delete(m_WaveTex); m_WaveTex = nullptr; }

    if (m_DetailSampler && VulkanHW.m_Device != VK_NULL_HANDLE) {
        vkDestroySampler(VulkanHW.m_Device, m_DetailSampler, nullptr);
        m_DetailSampler = VK_NULL_HANDLE;
    }
}

// ============================================================================
// Compute (generator) pipeline. 9-binding descriptor set, 208 B push range.
// Loads detail_generate.comp.spv from the global shader manager.
// ============================================================================
void CDetailManager::CreateGpuGenPipeline()
{
    if (!g_ShaderManager) {
        Msg("![VK Grass] g_ShaderManager null — gen pipeline disabled");
        return;
    }

    // 9 bindings — stored in a compact array for layout creation + write.
    // 0: heightmap   sampler       (R32F + sampler)
    // 1: SlotData    storage RO
    // 2: ObjInfo     storage RO
    // 3: Visible     storage RW
    // 4: Atomic      storage RW
    // 5: Indirect    storage RW    (gen doesn't write, but layout matches)
    // 6: HZB         sampler       (1×1 white placeholder, R32F)
    // 7: GenUBO      uniform       DYNAMIC (vk_dyn_sets.h): a plain UBO never reaches the constant
    //                              bank in COMPUTE on pre-Turing NVK -- 20 memory loads per thread
    //                              in the lean generator. Zero dynamic offset via the bind wrapper.
    // 8: TrailMap    sampler       (1×1 black placeholder, R32F)
    // ×2: the VISIBLE set + the shadow-CASTER set (same layout, caster
    // SSBO/atomics/indirect swapped in at bindings 3/4/5).
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    constexpr auto kTex  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const std::initializer_list<VkDescriptorType> genTypes =
        { kTex, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kTex, VK::DynSets::UboType(), kTex };
    m_GenDescLayout = VK::MakeSetLayout(genTypes, VK_SHADER_STAGE_COMPUTE_BIT, "Grass.DetailGen");
    m_GenDescPool   = VK::MakeDescriptorPool(genTypes, 2, "Grass.DetailGen");
    if (!m_GenDescLayout || !m_GenDescPool) return;
    if (!VK::AllocSets(m_GenDescPool, m_GenDescLayout, 1, &m_GenDescSet,    "Grass.DetailGen")) return;
    if (VK::DynSets::Enabled()) VK::DynSets::Register(m_GenDescSet, 1);   // binding 7
    if (!VK::AllocSets(m_GenDescPool, m_GenDescLayout, 1, &m_CasterDescSet, "Grass.DetailGen.caster")) return;
    if (VK::DynSets::Enabled()) VK::DynSets::Register(m_CasterDescSet, 1);

    // Pipeline layout: set 0 = gen data, set 1 = the shared EnvLight set — the
    // generator samples the DDGI probe field ONCE PER TUFT into the instance
    // record (r_grass_gi_inst; the crown's tree_gi.comp already widened the
    // EnvLight stage flags to COMPUTE). Same layout the forward draw binds.
    DM_CheckPushSize("gen", sizeof(DetailGenPushConstants));
    if (VK::EnvLight::GetSetLayout() == VK_NULL_HANDLE) {
        Msg("![VK Grass] EnvLight layout not ready — gen pipeline disabled");
        return;
    }
    m_GenPipelineLayout = VK::MakePipelineLayout({ m_GenDescLayout, VK::EnvLight::GetSetLayout() },
                                                 sizeof(DetailGenPushConstants));

    // nfs25: + distance thinning (r_grass_thin, 0 = off), nfs34: + the conservative per-slot pre-cull.
    VkShaderModule cs = g_ShaderManager->Load("detail_generate.comp.spv");
    if (cs == VK_NULL_HANDLE) {
        Msg("![VK Grass] detail_generate.comp.spv load failed"); return;
    }

    m_GenPipeline = VK::CreateComputePipeline(cs, m_GenPipelineLayout, "Grass.DetailGen");
    if (m_GenPipeline == VK_NULL_HANDLE) return;
    // r_grass_gen > 0 (the Switch preset): the reduced generators and the counters-by-compute path. Built only when
    // asked for at level load -- the full generator (0, PC default) never pays for them. Missing twin = the step below.
    if (ps_r_grass_gen >= 1) {
        if (VkShaderModule csL = g_ShaderManager->Load("detail_generate_lean.comp.spv"))   // -DDETAIL_GEN_LEAN
            m_GenPipelineLean = VK::CreateComputePipeline(csL, m_GenPipelineLayout, "Grass.DetailGenLean");
        Msg("[VK Grass] lean generator: %s", m_GenPipelineLean ? "OK" : "missing (full generator + HZB)");
        if (VkShaderModule csC = g_ShaderManager->Load("detail_counters.comp.spv"))
            m_CountersPipeline = VK::CreateComputePipeline(csC, m_GenPipelineLayout, "Grass.Counters");
        Msg("[VK Grass] counters pipeline: %s", m_CountersPipeline ? "OK" : "missing (fill + copy)");
    }
    if (ps_r_grass_gen >= 2) {
        if (VkShaderModule csL2 = g_ShaderManager->Load("detail_generate_lean2.comp.spv"))   // -DDETAIL_GEN_LEAN -DDETAIL_GEN_LEAN2
            m_GenPipelineLean2 = VK::CreateComputePipeline(csL2, m_GenPipelineLayout, "Grass.DetailGenLean2");
        Msg("[VK Grass] lean2 generator: %s", m_GenPipelineLean2 ? "OK" : "missing (lean)");
    }
    if (ps_r_grass_gen >= 3) {   // nfs40: the tuft cache -- its own set 2 so the shared generator set layout stays as it is
        const std::initializer_list<VkDescriptorType> cacheTypes = { kSSBO, kSSBO, kSSBO };
        m_CacheDescLayout = VK::MakeSetLayout(cacheTypes, VK_SHADER_STAGE_COMPUTE_BIT, "Grass.Cache");
        m_CacheDescPool   = VK::MakeDescriptorPool(cacheTypes, 1, "Grass.Cache");
        if (m_CacheDescLayout && m_CacheDescPool && VK::AllocSets(m_CacheDescPool, m_CacheDescLayout, 1, &m_CacheDescSet, "Grass.Cache")) {
            m_CachePipelineLayout = VK::MakePipelineLayout({ m_GenDescLayout, VK::EnvLight::GetSetLayout(), m_CacheDescLayout },
                                                           sizeof(DetailGenPushConstants));
            VkShaderModule csB = g_ShaderManager->Load("detail_cache_bake.comp.spv");
            VkShaderModule csC = g_ShaderManager->Load("detail_cache_cull.comp.spv");
            if (m_CachePipelineLayout && csB && csC) {
                m_CacheBakePipeline = VK::CreateComputePipeline(csB, m_CachePipelineLayout, "Grass.CacheBake");
                m_CacheCullPipeline = VK::CreateComputePipeline(csC, m_CachePipelineLayout, "Grass.CacheCull");
            }
        }
        Msg("[VK Grass] tuft cache pipelines: %s", (m_CacheBakePipeline && m_CacheCullPipeline) ? "OK" : "missing (lean2)");
    }

    UpdateGenDescriptors();
    Msg("[VK Grass] Gen pipeline OK (1 set, 9 bindings, 208 B push)");
}

// nfs40: (re)size the tuft cache for a W x W window of posPerSlot positions; every cell becomes "not baked".
static constexpr u32 kCacheRing = 4;   // bake-list regions (> frames in flight): the CPU never rewrites one the GPU reads
bool CDetailManager::CacheEnsure(u32 W, u32 pps)
{
    if (W == m_CacheW && pps == m_CachePPS && m_CacheEntries && m_CacheHeaders && m_CacheBakeList && m_CacheBakePtr)
        return true;
    VK_DeviceWaitIdleLocked();   // a resize (level start, radius/density change), never per frame
    CacheDestroy();
    const u64 cells = u64(W) * W;
    m_CacheEntries = xr_new<VK::CVulkanBuffer>();
    m_CacheEntries->Create(cells * pps * 32u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_CacheHeaders = xr_new<VK::CVulkanBuffer>();
    m_CacheHeaders->Create(cells * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_CacheBakeList = xr_new<VK::CVulkanBuffer>();
    m_CacheBakeList->Create(kCacheRing * cells * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    m_CacheBakePtr = (u32*)m_CacheBakeList->Map();
    if (!m_CacheBakePtr || m_CacheEntries->GetHandle() == VK_NULL_HANDLE || m_CacheHeaders->GetHandle() == VK_NULL_HANDLE) {
        Msg("![nx grass] tuft cache allocation failed (W %u, %u per slot)", W, pps);
        CacheDestroy();
        return false;
    }
    VK::DescriptorWriter(m_CacheDescSet)
        .StorageBuffer(0, m_CacheEntries->GetHandle())
        .StorageBuffer(1, m_CacheHeaders->GetHandle())
        .StorageBuffer(2, m_CacheBakeList->GetHandle())
        .Flush();
    m_CacheW = W; m_CachePPS = pps; m_CacheRing = 0;
    m_CacheCellCoord.assign(size_t(cells) * 2u, s32(0x80000000u));   // no slot coordinate
    Msg("[nx grass] tuft cache: %u x %u cells x %u positions = %.1f MB", W, W, pps,
        double(cells * pps * 32u + cells * 16u + kCacheRing * cells * 16u) / (1024.0 * 1024.0));
    return true;
}

void CDetailManager::CacheDestroy()
{
    if (m_CacheBakeList) { if (m_CacheBakePtr) m_CacheBakeList->Unmap(); m_CacheBakeList->Destroy(); xr_delete(m_CacheBakeList); }
    if (m_CacheEntries)  { m_CacheEntries->Destroy();  xr_delete(m_CacheEntries); }
    if (m_CacheHeaders)  { m_CacheHeaders->Destroy();  xr_delete(m_CacheHeaders); }
    m_CacheBakePtr = nullptr; m_CacheW = 0; m_CachePPS = 0;
    m_CacheCellCoord.clear();
}

void CDetailManager::DestroyGpuGenPipeline()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (m_GenPipeline)       { vkDestroyPipeline(VulkanHW.m_Device, m_GenPipeline, nullptr); m_GenPipeline = VK_NULL_HANDLE; }
    if (m_GenPipelineLean)   { vkDestroyPipeline(VulkanHW.m_Device, m_GenPipelineLean, nullptr); m_GenPipelineLean = VK_NULL_HANDLE; }
    if (m_GenPipelineLean2)  { vkDestroyPipeline(VulkanHW.m_Device, m_GenPipelineLean2, nullptr); m_GenPipelineLean2 = VK_NULL_HANDLE; }
    CacheDestroy();
    if (m_CacheBakePipeline) { vkDestroyPipeline(VulkanHW.m_Device, m_CacheBakePipeline, nullptr); m_CacheBakePipeline = VK_NULL_HANDLE; }
    if (m_CacheCullPipeline) { vkDestroyPipeline(VulkanHW.m_Device, m_CacheCullPipeline, nullptr); m_CacheCullPipeline = VK_NULL_HANDLE; }
    if (m_CachePipelineLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, m_CachePipelineLayout, nullptr); m_CachePipelineLayout = VK_NULL_HANDLE; }
    if (m_CacheDescPool)     { vkDestroyDescriptorPool(VulkanHW.m_Device, m_CacheDescPool, nullptr); m_CacheDescPool = VK_NULL_HANDLE; m_CacheDescSet = VK_NULL_HANDLE; }
    if (m_CacheDescLayout)   { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, m_CacheDescLayout, nullptr); m_CacheDescLayout = VK_NULL_HANDLE; }
    if (m_CountersPipeline)  { vkDestroyPipeline(VulkanHW.m_Device, m_CountersPipeline, nullptr); m_CountersPipeline = VK_NULL_HANDLE; }
    if (m_GenPipelineLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, m_GenPipelineLayout, nullptr); m_GenPipelineLayout = VK_NULL_HANDLE; }
    VK::DynSets::Unregister(m_GenDescSet); VK::DynSets::Unregister(m_CasterDescSet);   // before the pool frees them
    if (m_GenDescPool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, m_GenDescPool, nullptr); m_GenDescPool = VK_NULL_HANDLE; m_GenDescSet = VK_NULL_HANDLE; m_CasterDescSet = VK_NULL_HANDLE; }
    if (m_GenDescLayout)     { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, m_GenDescLayout, nullptr); m_GenDescLayout = VK_NULL_HANDLE; }
}

void CDetailManager::UpdateGenDescriptors()
{
    if (m_GenDescSet == VK_NULL_HANDLE) return;
    if (!m_HeightmapView || !m_DetailSampler || !m_SlotDataSSBO || !m_ObjInfoSSBO ||
        !m_VisibleSSBO || !m_AtomicCounters || !m_IndirectCmdBuf || !m_GenUBO ||
        !m_DummyHZBView || !m_DummyTrailView) return;

    VkDescriptorImageInfo hm{};   hm.sampler = m_HeightmapSampler;  hm.imageView = m_HeightmapView;  hm.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Binding 6 = real Hi-Z pyramid (GENERAL, nearest) when built; else 1×1
    // white dummy (depth 1.0 → never culls). HZB stays GENERAL all frame.
    VkDescriptorImageInfo hzb{};
    if (m_HZBView != VK_NULL_HANDLE) {
        hzb.sampler = m_HZBSampler;   hzb.imageView = m_HZBView;       hzb.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    } else {
        hzb.sampler = m_DetailSampler; hzb.imageView = m_DummyHZBView; hzb.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDescriptorImageInfo trl{};  trl.sampler= m_DetailSampler;     trl.imageView= m_DummyTrailView; trl.imageLayout= VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // The caster set is the same layout with output/atomics/indirect swapped for
    // the caster buffers (bindings 3/4/5).
    const bool haveCaster = m_CasterDescSet != VK_NULL_HANDLE
        && m_CasterSSBO && m_CasterAtomic && m_CasterIndirectBuf;

    auto writeSet = [&](VkDescriptorSet ds, bool caster) {
        VK::DescriptorWriter(ds)
            .Image        (0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, hm)
            .StorageBuffer(1, m_SlotDataSSBO->GetHandle())
            .StorageBuffer(2, m_ObjInfoSSBO->GetHandle())
            .StorageBuffer(3, caster ? m_CasterSSBO->GetHandle()        : m_VisibleSSBO->GetHandle())
            .StorageBuffer(4, caster ? m_CasterAtomic->GetHandle()      : m_AtomicCounters->GetHandle())
            .StorageBuffer(5, caster ? m_CasterIndirectBuf->GetHandle() : m_IndirectCmdBuf->GetHandle())
            .Image        (6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, hzb)
            .Buffer       (7, VK::DynSets::UboType(), { m_GenUBO->GetHandle(), 0, sizeof(DetailGenUBO) })
            .Image        (8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, trl)
            .Flush();
    };
    writeSet(m_GenDescSet, false);
    if (haveCaster) writeSet(m_CasterDescSet, true);
}

// ============================================================================
// Graphics pipeline. Vertex layout: binding 0 = per-vertex (24 B = 3 attrs),
// binding 1 = per-instance (64 B = 4 vec4 attrs, INSTANCE rate). Push 208 B.
// 1 descriptor set with 1 sampler (diffuse).
// ============================================================================
void CDetailManager::CreateGfxPipeline()
{
    if (!g_ShaderManager) { Msg("![VK Grass] g_ShaderManager null — gfx pipeline disabled"); return; }

    // Descriptor layout: binding 0 = per-type diffuse (fragment), binding 1 =
    // SSFX wind flow map s_waves (vertex — sampled to drive the wind).
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 2; lci.pBindings = b;
    vkCreateDescriptorSetLayout(VulkanHW.m_Device, &lci, nullptr, &m_GfxDescLayout);

    // Pool: one set per object type, 2 image samplers each (diffuse + flow map).
    const u32 nSets = _max(u32(objects.size()), 1u);
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps.descriptorCount = nSets * 2;
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = nSets; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &m_GfxDescPool);

    m_GfxDescSets.resize(objects.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < objects.size(); ++i) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool     = m_GfxDescPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts        = &m_GfxDescLayout;
        vkAllocateDescriptorSets(VulkanHW.m_Device, &dai, &m_GfxDescSets[i]);

        // binding 0 = per-type diffuse (or dummy white); binding 1 = SSFX flow map
        // (shared; dummy white when absent → flat wind, never crashes).
        const VkImageView diffuse = (i < m_DetailTextures.size() && m_DetailTextures[i])
                                    ? m_DetailTextures[i]->GetView()
                                    : m_DummyHZBView;
        VK::DescriptorWriter(m_GfxDescSets[i])
            .ImageSampler(0, diffuse, m_DetailSampler)
            .ImageSampler(1, m_WaveTex ? m_WaveTex->GetView() : m_DummyHZBView, m_DetailSampler)
            .Flush();
    }

    // Pipeline layout: set0 = per-type diffuse, set1 = shared env lighting
    // (sun_vp + sun shadow map — vk_env_light), + push (VS + FS).
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(DetailGfxPushConstants);
    DM_CheckPushSize("gfx", pcr.size);
    VkDescriptorSetLayout gfxSetLayouts[2] = { m_GfxDescLayout, VK::EnvLight::GetSetLayout() };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 2;
    plci.pSetLayouts            = gfxSetLayouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (gfxSetLayouts[1] == VK_NULL_HANDLE) { Msg("![VK Grass] EnvLight layout not ready"); return; }
    vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &m_GfxPipelineLayout);

    // Shaders. Two FS twins from one source: detail.frag.spv walks the probe
    // field per pixel, detail_giinst.frag.spv reads the per-tuft sample the gen
    // compute packed into the instance record (r_grass_gi_inst, the default).
    VkShaderModule vs  = g_ShaderManager->Load("detail.vert.spv");
    VkShaderModule fs  = g_ShaderManager->Load("detail.frag.spv");
    VkShaderModule fsI = g_ShaderManager->Load("detail_giinst.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        Msg("![VK Grass] detail.{vert,frag}.spv load failed"); return;
    }

    // Vertex bindings: binding 0 per-vertex (24 B), binding 1 per-instance (64 B).
    VkVertexInputBindingDescription vibd[2]{};
    vibd[0] = { 0, sizeof(VK::CDetail::Vertex), VK_VERTEX_INPUT_RATE_VERTEX   };
    vibd[1] = { 1, sizeof(DetailInstance),       VK_VERTEX_INPUT_RATE_INSTANCE };

    // Attribute layout matches detail.vert.glsl locations 0..7. Location 7 is a
    // SECOND view of the instance color's .g bytes (offset 48+4) as raw uint —
    // the rgb9e5 field sample must not ride through a float attribute fetch
    // (NaN/denorm bit patterns have no preservation guarantee there).
    VkVertexInputAttributeDescription via[8]{};
    via[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  };                      // aPos
    via[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,    12 };                      // aUV
    via[2] = { 2, 0, VK_FORMAT_R32_SFLOAT,       20 };                      // aHeight
    via[3] = { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0  };                   // aInstRow0
    via[4] = { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 };                   // aInstRow1
    via[5] = { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 };                   // aInstRow2
    via[6] = { 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48 };                   // aInstColor
    via[7] = { 7, 1, VK_FORMAT_R32_UINT,            52 };                   // aGIField (color.g bits)

    // Grass is double-sided (cull NONE, the builder default) and sits on a baked
    // negative bias so blades don't z-fight the terrain they grow out of.
    m_GfxPipeline = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
        .Vert(vs).Frag(fs)
        .Bindings(vibd, 2).Attrs(via, 8)
        .DepthBias(-2.0f, -1.0f)
        .Depth(true, true)
        .Color(VK::SceneColor::Format())
        .DepthTarget(Swapchain.m_DepthFormat)
        .Build("Grass gfx");
    if (m_GfxPipeline == VK_NULL_HANDLE)
        return;

    // r_grass_gi_inst twin — same everything, FS swapped. Missing .spv is not
    // fatal: the draw falls back to the per-pixel pipeline and says so once.
    if (fsI != VK_NULL_HANDLE) {
        m_GfxPipelineInst = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
            .Vert(vs).Frag(fsI)
            .Bindings(vibd, 2).Attrs(via, 8)
            .DepthBias(-2.0f, -1.0f)
            .Depth(true, true)
            .Color(VK::SceneColor::Format())
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Grass gfx giinst");
    }
    // nfs22 R1 grass (r_grass_r1 >= 1, Switch preset): detail.frag -DDETAIL_R1 = detail_r1.frag.spv -- sun x baked
    // occlusion + hemi + ambient + fog; reads neither the probe field nor the per-tuft GI. Same state as the pair above.
    // Built only when asked for at level load (the PC never pays the extra pipelines).
    if (ps_r_grass_r1 >= 1) {
        if (VkShaderModule fsR1 = g_ShaderManager->Load("detail_r1.frag.spv"))
            m_GfxPipelineR1 = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
                .Vert(vs).Frag(fsR1)
                .Bindings(vibd, 2).Attrs(via, 8)
                .DepthBias(-2.0f, -1.0f)
                .Depth(true, true)
                .Color(VK::SceneColor::Format())
                .DepthTarget(Swapchain.m_DepthFormat)
                .Build("Grass gfx R1");
        Msg("[VK Grass] R1 pipeline: %s", m_GfxPipelineR1 ? "OK" : "missing (full grass FS stays)");
    }
    // nfs32 R1V (r_grass_r1 2): the R1 grass with its per-pixel distance work moved to the vertex (same state as the
    // pair above). detail.vert -DDETAIL_R1V = detail_r1v.vert.spv (+ nfs43 per-vertex live sun shadow, + nfs47 cascade
    // z cut), FS detail_r1v.frag.spv.
    if (ps_r_grass_r1 >= 2) {
        VkShaderModule rvs = g_ShaderManager->Load("detail_r1v.vert.spv");
        VkShaderModule rfs = rvs ? g_ShaderManager->Load("detail_r1v.frag.spv") : VK_NULL_HANDLE;
        if (rvs && rfs)
            m_GfxPipelineR1V = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
                .Vert(rvs).Frag(rfs)
                .Bindings(vibd, 2).Attrs(via, 8)
                .DepthBias(-2.0f, -1.0f)
                .Depth(true, true)
                .Color(VK::SceneColor::Format())
                .DepthTarget(Swapchain.m_DepthFormat)
                .Build("Grass gfx R1V");
        Msg("[VK Grass] R1V pipeline: %s", m_GfxPipelineR1V ? "OK" : "missing (R1 grass FS stays)");
    }
    if (m_GfxPipelineInst == VK_NULL_HANDLE)
        Msg("![VK Grass] detail_giinst.frag.spv unavailable — grass falls back to per-pixel GI");

    Msg("[VK Grass] Gfx pipeline OK (1 set, 8 attrs, 208 B push)");
}

void CDetailManager::DestroyGfxPipeline()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    DestroyMotionPipeline();   // built on top of m_GfxDescLayout — free it first
    if (m_GfxPipeline)       { vkDestroyPipeline(VulkanHW.m_Device, m_GfxPipeline, nullptr); m_GfxPipeline = VK_NULL_HANDLE; }
    if (m_GfxPipelineInst)   { vkDestroyPipeline(VulkanHW.m_Device, m_GfxPipelineInst, nullptr); m_GfxPipelineInst = VK_NULL_HANDLE; }
    if (m_GfxPipelineR1)     { vkDestroyPipeline(VulkanHW.m_Device, m_GfxPipelineR1, nullptr); m_GfxPipelineR1 = VK_NULL_HANDLE; }
    if (m_GfxPipelineR1V)    { vkDestroyPipeline(VulkanHW.m_Device, m_GfxPipelineR1V, nullptr); m_GfxPipelineR1V = VK_NULL_HANDLE; }
    if (m_GfxPipelineLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, m_GfxPipelineLayout, nullptr); m_GfxPipelineLayout = VK_NULL_HANDLE; }
    if (m_GfxDescPool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, m_GfxDescPool, nullptr); m_GfxDescPool = VK_NULL_HANDLE; m_GfxDescSets.clear(); }
    if (m_GfxDescLayout)     { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, m_GfxDescLayout, nullptr); m_GfxDescLayout = VK_NULL_HANDLE; }
}

// Grass wind-sway MV overlay pipeline. Reuses the gfx descriptor layout (set0 =
// diffuse @0 + s_waves @1) and vertex input, but outputs RG16F motion, tests depth
// LEQUAL WITHOUT writing, and carries the SAME depth bias as the forward grass draw
// so its clip depth bit-matches what that pass already stored (LEQUAL passes on
// equal). Push = cur/prev VP + cur/prev wind (detail_motion.vert reprojects both).
void CDetailManager::CreateMotionPipeline()
{
    if (m_MotionPipeline != VK_NULL_HANDLE) return;               // already built
    if (!g_ShaderManager || m_GfxDescLayout == VK_NULL_HANDLE) return;

    VkShaderModule vs = g_ShaderManager->Load("detail_motion.vert.spv");
    VkShaderModule fs = g_ShaderManager->Load("detail_motion.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        Msg("![VK Grass] detail_motion.{vert,frag}.spv load failed — grass MV disabled");
        return;
    }

    // Layout: set0 = gfx descriptor layout (diffuse + s_waves), push = MV (VS only).
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.size       = sizeof(DetailMotionPushConstants);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &m_GfxDescLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &m_MotionPipelineLayout) != VK_SUCCESS) {
        Msg("![VK Grass] MV pipeline layout create failed"); return;
    }

    // Same vertex input as the forward gfx pipeline.
    VkVertexInputBindingDescription vibd[2]{};
    vibd[0] = { 0, sizeof(VK::CDetail::Vertex), VK_VERTEX_INPUT_RATE_VERTEX   };
    vibd[1] = { 1, sizeof(DetailInstance),       VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription via[7]{};
    via[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  };
    via[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,       12 };
    via[2] = { 2, 0, VK_FORMAT_R32_SFLOAT,          20 };
    via[3] = { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0  };
    via[4] = { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
    via[5] = { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 };
    via[6] = { 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48 };
    // Cull NONE + the SAME depth bias as the forward grass draw so depth bit-matches.
    // Depth is tested but not written — the scene depth already owns the surface.
    m_MotionPipeline = VK::GfxPipelineBuilder(m_MotionPipelineLayout)
        .Vert(vs).Frag(fs)
        .Bindings(vibd, 2).Attrs(via, 7)
        .DepthBias(-2.0f, -1.0f)
        .Depth(true, false)
        .Color(VK::MotionVec::Format(),
               VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT)   // RG16F motion
        .DepthTarget(Swapchain.m_DepthFormat)
        .Build("Grass MV overlay");
    if (m_MotionPipeline == VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(VulkanHW.m_Device, m_MotionPipelineLayout, nullptr);
        m_MotionPipelineLayout = VK_NULL_HANDLE;
        return;
    }
    Msg("[VK Grass] MV overlay pipeline OK (wind-sway motion vectors)");
}

void CDetailManager::DestroyMotionPipeline()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (m_MotionPipeline)       { vkDestroyPipeline(VulkanHW.m_Device, m_MotionPipeline, nullptr); m_MotionPipeline = VK_NULL_HANDLE; }
    if (m_MotionPipelineLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, m_MotionPipelineLayout, nullptr); m_MotionPipelineLayout = VK_NULL_HANDLE; }
}

}  // namespace VK

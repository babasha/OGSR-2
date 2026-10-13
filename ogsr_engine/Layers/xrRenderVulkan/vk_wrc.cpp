// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — world radiance cache (cache #2 of the chain). See vk_wrc.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_wrc.h"
#include "vk_rt_world.h"      // set 1 = the RT diffuse array (per-texel albedo + cutout alpha)
#include "vk_buffer.h"
#include "vk_descriptors.h"
#include "vk_shaders.h"
#include "vk_compute_util.h"
#include "vk_barriers.h"
#include "vk_command_buffer.h"
#include "vk_profiler.h"
#include "HW_Vulkan.h"

#include "../../xr_3da/device.h"

extern float ps_r_wrc;            // master weight: 0 = off, 1 = the cache answers
extern float ps_r_wrc_cell;       // level-0 cell size (m)
extern float ps_r_wrc_lod;        // cell metres per metre of distance (the LOD slope)
extern int   ps_r_wrc_levels;     // highest LOD level
extern int   ps_r_wrc_cells;      // log2 of the table capacity  ⚠ reallocates
extern int   ps_r_wrc_chain;      // slots searched on a hash collision
extern int   ps_r_wrc_interval;   // frames between re-shades of one cell
extern int   ps_r_wrc_ttl;        // frames with no request before a cell is evicted
extern int   ps_r_wrc_shade_max;  // cells shaded per frame (the work cap)
extern float ps_r_wrc_blend;      // EMA weight of a re-shade against what the cell held
extern int   ps_r_wrc_debug;      // 1 = a colour per cell, 2 = the cache's contribution only
extern int   ps_r_wrc_stats;      // periodic hit/miss/overflow line
extern int   ps_r_wrc_skyvis;     // hemisphere-visibility rays per CELL shade (0 = the old unoccluded sky)
extern float ps_r_wrc_skyvis_len; // how far one of those rays reaches (m)
extern float ps_r_wrc_skyvis_bounce; // what a sky-vis ray that HITS contributes (0 = nothing, the A/B)
extern int   ps_r_wrc_sky_field;  // 1 = the cell reads the probe field instead of rolling its own rays (the idTech 8 cell); 2 = cache-to-cache (covered cells bounce off the cache, no field)
extern float ps_r_wrc_field_clamp; // ceiling on that field read: k x the cell's own measured injection (0 = off)
extern float ps_r_wrc_ema_up;      // EMA multiplier on BRIGHTENING re-shades (1 = symmetric) — the anti-shot-noise asymmetry
extern int   ps_r_gi_probe;        // the GI pixel debugger keeps the clamp audit line flowing

namespace VK { namespace WRC {

namespace {

constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
// ⚠ must match struct WrcCell in shaders/wrc_common.glsl. It was 48 and the width
// of this constant IS the cache's capacity: the first run lost 10-15% of a million
// queries a frame to full probe chains because 24 MB only bought 512k slots.
constexpr u32 kCellSize       = 32;
// With the cache off the shaders still declare — and therefore statically use —
// all four descriptors, so the buffers cannot be null. This is what they shrink
// to instead: 48 KB that nothing ever reads, because the constants block's weight
// is zero and wrcQuery returns on its first line.
constexpr u32 kIdleCells      = 1024;
constexpr u32 kDecaySlices    = 8;     // fraction of the table swept per frame
constexpr u32 kCtrlWords      = 15;    // ⚠ must match WrcCtrl in wrc_common.glsl (10..14 = the field-clamp audit)

// ⚠ Mirrors WrcConstsBlock in shaders/wrc_common.glsl EXACTLY (std140: four
// 16-byte rows, no padding needed because every member is already a vec4/uvec4).
struct Consts {
    float camPos[4];   // xyz = camera, w = level-0 cell size (m)
    float params[4];   // x = LOD slope, y = max level, z = master weight, w = re-shade EMA
    u32   ctl[4];      // x = capacity-1, y = frame index (never 0), z = shade cap, w = re-shade interval
    u32   ctl2[4];     // x = probe chain, y = stats on, z = debug mode, w = sky-vis rays per cell
    float params2[4];  // x = sky-vis ray reach (m); y = sky-vis 2nd-bounce weight; z = sky_field mode; w = field-clamp k (0 = off)
    // Tail append (same contract as the tails in vk_ddgi.cpp's Consts): only
    // recompiled consumers see it, every offset above stays put.
    float params3[4];  // x = r_wrc_ema_up (EMA multiplier on BRIGHTENING re-shades; 1 = symmetric), y/z/w reserved
};
static_assert(sizeof(Consts) == 96, "must match WrcConstsBlock");

struct DecayPush { u32 range[4]; };   // first slot, slots, TTL, unused

bool s_inited = false;
bool s_failed = false;

CVulkanBuffer s_cells;
CVulkanBuffer s_list;
CVulkanBuffer s_ctrl;
CVulkanBuffer s_consts[kFramesInFlight];
CVulkanBuffer s_stats[kFramesInFlight];   // host-visible readback of s_ctrl, one frame per slot

u32  s_capacity  = 0;      // cells, always a power of two
u32  s_listMax   = 0;      // entries in the shade queue
u32  s_gen       = 0;
bool s_needClear = true;
u32  s_frame     = ~0u;
u32  s_slot      = 0;
u32  s_decayCur  = 0;      // where the next eviction slice starts
u32  s_auditUntil = 0;     // frame the self-audit window closes (see StatsWanted)
bool s_wasLive    = false; // ...and what opens it: the cache going from off to on
u32  s_liveCells = 0;      // last known occupancy estimate (from the stats readback)

VkDescriptorSetLayout s_shadeSetLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_shadePool      = VK_NULL_HANDLE;
VkDescriptorSet       s_shadeSets[kFramesInFlight] = {};
VkPipelineLayout      s_shadePipeLayout = VK_NULL_HANDLE;
VkPipeline            s_shadePipe = VK_NULL_HANDLE;

VkDescriptorSetLayout s_decaySetLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_decayPool      = VK_NULL_HANDLE;
VkDescriptorSet       s_decaySets[kFramesInFlight] = {};
VkPipelineLayout      s_decayPipeLayout = VK_NULL_HANDLE;
VkPipeline            s_decayPipe = VK_NULL_HANDLE;

// What each set currently points at. A bubble rebuild swaps the TLAS and all
// three geometry buffers at once, and a set an in-flight command buffer still
// references must not be rewritten under it — so rewrite only on a real change.
// Same rule as every other RT consumer in this renderer.
struct BoundShade {
    VkAccelerationStructureKHR tlas;
    VkBuffer    verts, mats, alb, ddgi, sh, lights, consts, cells, list, ctrl;
    VkImageView cube, irr, dep;
};
BoundShade s_boundShade[kFramesInFlight] = {};

struct BoundDecay { VkBuffer consts, cells, list, ctrl; };
BoundDecay s_boundDecay[kFramesInFlight] = {};

u32 WantCapacity()
{
    if (!VK::WRC::Wanted()) return kIdleCells;
    const int log2n = _max(14, _min(21, ps_r_wrc_cells));
    return 1u << u32(log2n);
}

void DestroyBuffers()
{
    s_cells.Destroy();
    s_list.Destroy();
    s_ctrl.Destroy();
    s_capacity = 0;
    s_listMax  = 0;
    s_needClear = true;
    for (auto& b : s_boundShade) b = BoundShade{};
    for (auto& b : s_boundDecay) b = BoundDecay{};
    ++s_gen;
}

// (Re)allocate the table when the capacity or the shade cap moved. Both are
// console-driven, so this runs on a change and never per frame.
bool EnsureBuffers()
{
    const u32 wantCap  = WantCapacity();
    const u32 wantList = _max(64u, u32(_max(1024, _min(1 << 18, ps_r_wrc_shade_max))));
    if (s_cells.IsValid() && wantCap == s_capacity && wantList == s_listMax) return true;

    // The old buffers may still be referenced by frames in flight, and this only
    // runs on a console change or the first frame, so a full idle is the honest
    // way to be safe (the gather takes the same deal on a resize).
    if (s_cells.IsValid()) VK_DeviceWaitIdleLocked();
    DestroyBuffers();

    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    s_cells.Create(VkDeviceSize(wantCap) * kCellSize, storage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_list.Create(VkDeviceSize(wantList) * sizeof(u32), storage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_ctrl.Create(kCtrlWords * sizeof(u32), storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (!s_cells.IsValid() || !s_list.IsValid() || !s_ctrl.IsValid()) {
        Msg("![VK WRC] table allocation failed (%u cells, %.1f MB) — cache off",
            wantCap, double(wantCap) * kCellSize / (1024.0 * 1024.0));
        DestroyBuffers();
        return false;
    }

    s_capacity = wantCap;
    s_listMax  = wantList;
    s_decayCur = 0;
    s_needClear = true;
    ++s_gen;
    if (wantCap > kIdleCells) {
        Msg("[VK WRC] table %u cells (%.1f MB), queue %u, cell %.2f m, LOD slope %.4f, %u level(s)",
            wantCap, double(wantCap) * kCellSize / (1024.0 * 1024.0), wantList,
            ps_r_wrc_cell, ps_r_wrc_lod, u32(_max(0, _min(7, ps_r_wrc_levels)) + 1));
        // ⚠⚠ PRINT THE LADDER, NOT THE SLOPE. The first default put every rung
        // OUTSIDE the gather's 30 m cutoff — the LOD function ran, returned 0
        // everywhere that matters, and the only symptom was a table that filled up.
        // A slope is a number nobody can check by eye; the distance at which each
        // level starts is one anybody can compare against the cutoff in the very
        // next log line.
        const float base  = _max(0.05f, _min(4.f, ps_r_wrc_cell));
        const float slope = _max(0.f, _min(0.1f, ps_r_wrc_lod));
        string256 ladder{};
        int n = 0;
        n += sprintf_s(ladder + n, sizeof(ladder) - n, "[VK WRC] LOD ladder: %.2f m", base);
        if (slope > 1e-6f) {
            const int maxLvl = _max(0, _min(7, ps_r_wrc_levels));
            for (int l = 1; l <= maxLvl && n < 200; ++l)
                n += sprintf_s(ladder + n, sizeof(ladder) - n, " | %.2f m past %.0f m",
                               base * float(1 << l), base * float(1 << l) / slope);
        } else {
            n += sprintf_s(ladder + n, sizeof(ladder) - n, " everywhere (slope 0)");
        }
        Msg("%s", ladder);
    }
    return true;
}

// One line, throttled, naming what the cache is actually doing. Everything else
// in this arc has already proved that "init OK" in a log proves nothing: a pass
// can be fully initialised, fully bound, and never dispatched. These are the
// numbers that tell a live cache from a dead one.
// ⭐⭐⭐ THE CACHE AUDITS ITSELF WHEN IT COMES UP, WITHOUT BEING ASKED.
//
// Three runs in a row measured nothing because the diagnostic was behind a second
// console command that nobody typed — the feature was switched on, it ran, it hit,
// and the log stayed silent about whether any of it mattered. A number that needs
// a second command is a number that does not exist.
//
// So the first ~900 frames after the cache goes live report themselves, then the
// window closes and the atomics go away. r_wrc_stats 1 keeps them on for longer.
bool StatsWanted()
{
    return ps_r_wrc_stats != 0 || (s_auditUntil != 0 && RV().frameIndex < s_auditUntil);
}

void ReportStats(u32 slot)
{
    // The clamp audit prints on its own gate: whoever is reading [VK GIprobe]
    // blocks needs it flowing next to them without also turning r_wrc_stats on.
    const bool base  = StatsWanted();
    const bool clamp = ps_r_gi_probe != 0;
    if (!base && !clamp) return;
    void* src = s_stats[slot].Map();
    if (!src) return;
    s_stats[slot].Invalidate();
    u32 v[kCtrlWords];
    memcpy(v, src, sizeof(v));
    s_stats[slot].Unmap();

    static u32 s_last = 0;
    if (RV().frameIndex - s_last < 120) return;
    s_last = RV().frameIndex;

    // r_wrc_field_clamp self-audit, one frame's worth (words 10..14). The line
    // that separates "the gate never passes" (roof not in the opaque BVH — the
    // alpha-tested shingle suspicion) from "the budget is inflated" (sky through
    // the openings + bright neighbours) — the two failure modes that both look
    // like "the clamp changed nothing".
    if ((base || clamp) && v[10] > 0) {
        const double cov = double(v[11]);
        Msg("[VK WRC] clamp: %u sky_field cells this frame, %.1f%% covered overhead, %.1f%% bitten | covered mean budget %.3f vs field spend %.3f",
            v[10], 100.0 * cov / double(v[10]),
            (v[11] > 0) ? 100.0 * double(v[12]) / cov : 0.0,
            (v[11] > 0) ? double(v[13]) / cov / 1000.0 : 0.0,
            (v[11] > 0) ? double(v[14]) / cov / 1000.0 : 0.0);
    }
    if (!base) return;

    const double q = double(_max(1u, v[2]));
    // Reads per cell is THE number for a cache: shading a cell that two rays read
    // is worse than not caching at all, and no hit-rate percentage shows that.
    const double reads = double(v[3]) / double(_max(1u, v[9]));
    Msg("[VK WRC] %u queries: %.1f%% hit, %u new, %u lost (chain full), %u left to the screen cache",
        v[2], 100.0 * double(v[3]) / q, v[4], v[5], v[6]);
    Msg("[VK WRC] queued %u, overflow %u, shaded %u -> %.1f reads per shaded cell",
        v[0], v[1], v[9], reads);
    // ⭐⭐⭐ THE VERDICT LINE. Two sessions were spent asking an eye whether this
    // cache changes the frame; this asks the GPU. Under ~5% the cache agrees with
    // the lattice it displaced and is pure cost. Over ~30% it is doing real work
    // and any remaining invisibility is downstream, where four mechanisms share
    // the fill light — a different problem with a different fix.
    if (v[8] > 0)
        Msg("[VK WRC] cache vs lattice: mean |delta| = %.1f%% of the lattice answer (%u samples)",
            100.0 * (double(v[7]) / 1024.0) / double(v[8]), v[8]);
    if (v[1] > 0)
        Msg("![VK WRC] shade queue overflowed by %u — raise r_wrc_shade_max or r_wrc_interval", v[1]);
    if (v[5] > v[2] / 20 && v[2] > 1000)
        Msg("![VK WRC] %u of %u queries lost to full probe chains — raise r_wrc_cells or r_wrc_chain", v[5], v[2]);
}

}   // anon namespace

// ONE KNOB. r_wrc alone raises the probe volume (vk_ddgi) and the BVH bubble
// (vk_rt_scene), because both of those gates defer to THIS function — the
// producer/consumer trap that cost this arc a morning is closed at the source
// rather than diagnosed afterwards.
bool Wanted() { return ps_r_wrc > 0.f || ps_r_wrc_debug != 0; }

bool Enabled() { return Wanted() && s_inited && !s_failed && s_cells.IsValid() && s_capacity > kIdleCells; }

VkBuffer ConstsBuffer() { return s_consts[s_slot].GetHandle(); }
VkBuffer CellsBuffer()  { return s_cells.GetHandle(); }
VkBuffer ListBuffer()   { return s_list.GetHandle(); }
VkBuffer CtrlBuffer()   { return s_ctrl.GetHandle(); }
u32      Generation()   { return s_gen; }

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;

    if (!g_ShaderManager) { s_failed = true; return false; }

    const VkDescriptorSetLayout texLayout = RTWorld::TexSetLayout();
    if (texLayout == VK_NULL_HANDLE) {
        Msg("![VK WRC] no RT diffuse array — world radiance cache disabled");
        s_failed = true; return false;
    }

    // 0 TLAS, 1 verts, 2 per-tri attributes, 3 albedo table, 4 DDGI constants,
    // 5 sky SH9, 6 sky cube, 7/8 the probe atlases (the bounce term), 9 the RT
    // light list, 10 WRC constants, 11 cells, 12 queue, 13 control.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
                                kFramesInFlight, s_shadeSetLayout, s_shadePool, s_shadeSets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "WrcShade")) {
        s_failed = true; return false;
    }
    // The eviction sweep needs the table and the constants; it declares the queue
    // and the control block too because wrc_common.glsl does, and an unwritten
    // descriptor a shader declares is a validation error waiting for a driver
    // that decides the block was statically used after all.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
                                kFramesInFlight, s_decaySetLayout, s_decayPool, s_decaySets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "WrcDecay")) {
        s_failed = true; return false;
    }

    s_shadePipeLayout = VK::MakePipelineLayout({ s_shadeSetLayout, texLayout });
    s_decayPipeLayout = VK::MakePipelineLayout({ s_decaySetLayout }, sizeof(DecayPush));
    if (!s_shadePipeLayout || !s_decayPipeLayout) { s_failed = true; return false; }

    s_shadePipe = VK::CreateComputePipeline("wrc_shade.comp.spv", s_shadePipeLayout, "WrcShade");
    s_decayPipe = VK::CreateComputePipeline("wrc_decay.comp.spv", s_decayPipeLayout, "WrcDecay");
    if (!s_shadePipe || !s_decayPipe) {
        Msg("![VK WRC] compute pipeline create failed — world radiance cache off");
        s_failed = true; return false;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        s_consts[i].Create(sizeof(Consts), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST, false);
        // hostRead: this one is a readback target, and a write-combined mapping
        // reads at ~230 MB/s. 32 bytes would not care, but the flag is what says
        // out loud which direction this buffer travels.
        s_stats[i].Create(kCtrlWords * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VMA_MEMORY_USAGE_AUTO, false, false, true);
        if (!s_consts[i].IsValid() || !s_stats[i].IsValid()) {
            Msg("![VK WRC] per-frame buffer create failed — world radiance cache off");
            s_failed = true; return false;
        }
        // A readback slot is READ BEFORE IT IS FIRST WRITTEN — that is what makes
        // the ring free of stalls — so it has to start as zeros rather than as
        // whatever this mapping held before, or the first stats line reports noise.
        if (void* z = s_stats[i].Map()) { memset(z, 0, kCtrlWords * sizeof(u32)); s_stats[i].Flush(); s_stats[i].Unmap(); }
    }

    Msg("[VK WRC] init OK (world radiance cache: cell #2 of the chain, shaded once per cell per few frames)");
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    DestroyBuffers();
    for (u32 i = 0; i < kFramesInFlight; ++i) { s_consts[i].Destroy(); s_stats[i].Destroy(); }
    if (s_shadePipe) { vkDestroyPipeline(VulkanHW.m_Device, s_shadePipe, nullptr); s_shadePipe = VK_NULL_HANDLE; }
    if (s_decayPipe) { vkDestroyPipeline(VulkanHW.m_Device, s_decayPipe, nullptr); s_decayPipe = VK_NULL_HANDLE; }
    if (s_shadePipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_shadePipeLayout, nullptr); s_shadePipeLayout = VK_NULL_HANDLE; }
    if (s_decayPipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_decayPipeLayout, nullptr); s_decayPipeLayout = VK_NULL_HANDLE; }
    if (s_shadePool) { vkDestroyDescriptorPool(VulkanHW.m_Device, s_shadePool, nullptr); s_shadePool = VK_NULL_HANDLE; }
    if (s_decayPool) { vkDestroyDescriptorPool(VulkanHW.m_Device, s_decayPool, nullptr); s_decayPool = VK_NULL_HANDLE; }
    if (s_shadeSetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_shadeSetLayout, nullptr); s_shadeSetLayout = VK_NULL_HANDLE; }
    if (s_decaySetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_decaySetLayout, nullptr); s_decaySetLayout = VK_NULL_HANDLE; }
    for (auto& s : s_shadeSets) s = VK_NULL_HANDLE;
    for (auto& s : s_decaySets) s = VK_NULL_HANDLE;
    s_inited = false; s_failed = false;
}

void OnLevelUnload()
{
    // Every cell describes a surface of a world that is being torn down. Unlike a
    // probe atlas, which fades, a cell answers with FULL confidence — so a stale
    // one would be read as authoritative radiance for a wall that is gone.
    s_needClear = true;
    s_decayCur  = 0;
    s_liveCells = 0;
}

void Update(VkCommandBuffer cmd, const ShadeInputs& in)
{
    if (!Init()) return;
    if (!EnsureBuffers()) { s_failed = true; return; }

    // One step per frame. A second caller would sweep two slices and, worse,
    // shade the queue twice — the second pass would re-blend every cell against
    // the answer the first one had just written, silently doubling the EMA.
    if (RV().frameIndex == s_frame) return;
    s_frame = RV().frameIndex;
    s_slot  = CommandManager.GetCurrentFrame() % kFramesInFlight;

    const int zone = VK::Prof::ZoneBegin(cmd, "WRC");

    // Open the self-audit window the moment the cache comes up, so switching it on
    // is the whole recipe — no second command, no remembering a flag name.
    {
        const bool live = Enabled();
        if (live && !s_wasLive) {
            s_auditUntil = RV().frameIndex + 900;
            Msg("[VK WRC] self-audit for ~900 frames: hit rate, reads per cell and how far "
                "the cache's answer is from the lattice it replaces (r_wrc_stats 1 to keep it on)");
        }
        if (!live && s_wasLive) s_auditUntil = 0;
        s_wasLive = live;
    }

    // ---- constants --------------------------------------------------------
    // ⚠ THE FRAME INDEX IS NEVER 0. It is written into `touch` and compared
    // against by the eviction sweep, and a zero would make a freshly claimed cell
    // read as "already touched this frame", so it would never register for
    // shading and never become valid — a cache that initialises perfectly and
    // does nothing.
    Consts c{};
    c.camPos[0] = RV().vCameraPosition.x;
    c.camPos[1] = RV().vCameraPosition.y;
    c.camPos[2] = RV().vCameraPosition.z;
    c.camPos[3] = _max(0.05f, _min(4.f, ps_r_wrc_cell));
    c.params[0] = _max(0.f, _min(0.1f, ps_r_wrc_lod));
    c.params[1] = float(_max(0, _min(7, ps_r_wrc_levels)));
    // The debug views are worth nothing if the query they visualise is switched
    // off, so asking for one turns the cache on. Same rule as everywhere else in
    // this arc: the gate names its own dependency instead of failing quietly.
    c.params[2] = Enabled() ? (ps_r_wrc_debug ? _max(ps_r_wrc, 1.f) : _max(0.f, _min(1.f, ps_r_wrc))) : 0.f;
    c.params[3] = _max(0.02f, _min(1.f, ps_r_wrc_blend));
    c.ctl[0]    = s_capacity - 1u;
    c.ctl[1]    = RV().frameIndex + 1u;
    c.ctl[2]    = s_listMax;
    c.ctl[3]    = u32(_max(1, _min(64, ps_r_wrc_interval)));
    c.ctl2[0]   = u32(_max(1, _min(16, ps_r_wrc_chain)));
    c.ctl2[1]   = StatsWanted() ? 1u : 0u;
    c.ctl2[2]   = u32(_max(0, _min(3, ps_r_wrc_debug)));
    // Sky visibility: the term nothing else in this GI has. See wrc_shade.comp.
    // ⚠⚠ THE CEILING WAS 8 AND 8 WAS NOT ENOUGH - MEASURED 24.08, NOT GUESSED.
    // Walking the r_gi ladder inside a room showed that r_gi 1 (probes) and r_gi 2
    // (+ gather) are both indistinguishable from r_gi 0: indoors they contribute
    // NOTHING. Every photon in that room arrives at r_gi 3, through the loop below -
    // it is the only estimator in this renderer that integrates the window.
    // That makes its sample count the single number deciding both how bright the
    // room is AND how clean it is, and eight samples of an integral whose entire
    // energy sits in the small solid angle of an aperture is close to a coin flip:
    // one cell's ray finds the window, its neighbour's does not. The static
    // bright/dark cell pattern that reads as "dirt" is exactly that.
    // ⭐ Why nothing else moved it: more GATHER rays cannot help (they all read the
    // same cells), the per-cell EMA cannot help (r_wrc_blend swept 0.05..1.0 with no
    // visible change), and no spatial filter can help (the pattern is world-anchored
    // and identical every frame). The variance is born here.
    // ⚠ CAVEAT TO THE LADDER READING (25.08): at r_gi 1/2 the WRC is off, which
    // also kills the svLive gate in vk_env_light — so the black interior lightmap
    // multiplied ALL of the probes' answer away. "r_gi 1 = r_gi 0 indoors" proved
    // the gate, not that the probes cannot see the window. They can — which is
    // what r_wrc_sky_field builds on.
    c.ctl2[3]   = u32(_max(0, _min(64, ps_r_wrc_skyvis)));
    c.params2[0] = _max(0.5f, _min(64.f, ps_r_wrc_skyvis_len));
    // ...and what one that HITS brings back: the cache asking the cache (2nd bounce).
    c.params2[1] = _max(0.f, _min(1.f, ps_r_wrc_skyvis_bounce));
    // r_wrc_sky_field: retire the per-cell hemisphere entirely and read the probe
    // field instead — the idTech 8 cell (direct + previous frame's volumes). The
    // MC loop above stays reachable at 0 as the A/B and the fallback.
    // 2 = cache-to-cache: a COVERED cell's ambient is its own hemisphere — sky
    // through the openings + what the neighbour cells already radiate — and the
    // probe field never enters the chain; open-sky cells keep the field read.
    // The clamp's ceiling becomes moot for covered cells (there is nothing left
    // to clamp); the audit counters keep printing, budget = measured ambient.
    c.params2[2] = float(_max(0, _min(2, ps_r_wrc_sky_field)));
    // r_wrc_field_clamp: the ceiling on the sky_field cell's field read — k x the
    // cell's own measured injection (sky through the openings + shaded neighbour
    // cells). Bounds "the lights are on in that distant house" (an interior past
    // the fine cascades reading the coarse lattice) without re-admitting the
    // skyvis dirt: a CEILING only bites where the field is far above the measured
    // number, and there the result is dark-on-dark. See wrc_shade.comp.glsl.
    // Negative = the DIAGNOSTIC MODE (cells store their budget breakdown as
    // colour for [VK GIprobe] to print) — the ceiling itself still uses |k|.
    c.params2[3] = _max(-8.f, _min(8.f, ps_r_wrc_field_clamp));
    // r_wrc_ema_up: how fast a cell may BRIGHTEN relative to how fast it darkens.
    // The anti-shot-noise asymmetry — see the EMA store in wrc_shade.comp.glsl.
    c.params3[0] = _max(0.01f, _min(1.f, ps_r_wrc_ema_up));
    c.params3[1] = c.params3[2] = c.params3[3] = 0.f;
    if (void* dst = s_consts[s_slot].Map()) {
        memcpy(dst, &c, sizeof(c));
        s_consts[s_slot].Flush();
        s_consts[s_slot].Unmap();
    } else {
        VK::Prof::ZoneEnd(cmd, zone);
        return;
    }

    // ---- first use / level change -----------------------------------------
    // The table holds undefined memory, and "undefined" here means keys that look
    // occupied, `shaded` flags that look valid and radiance that is whatever the
    // last tenant of this VRAM left. All three would be read with confidence.
    if (s_needClear) {
        // ⚠ A BARRIER BEFORE THE FILL, not only after it. This block also runs
        // mid-session (a level change sets s_needClear), where the previous frame's
        // rays may still be reading and atomically writing this very table — and a
        // transfer clear against live shader access is a hazard sync validation
        // reports as WRITE_AFTER_READ, not a crash anyone would trace back here.
        VK::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, s_cells.GetHandle(), 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, s_list.GetHandle(),  0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, s_ctrl.GetHandle(),  0, VK_WHOLE_SIZE, 0);
        VK::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        s_needClear = false;
    }

    // Everything above is upkeep the consumers' descriptors need even with the
    // cache off (they bind these buffers unconditionally). From here on it is the
    // cache proper.
    if (!Enabled()) { VK::Prof::ZoneEnd(cmd, zone); return; }
    if (!in.tlas || !in.verts || !in.mats || !in.albedo || !in.ddgiConsts || !in.skySH ||
        !in.skyCube || !in.irr || !in.dep || !in.lights || !in.texSet) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Msg("![VK WRC] idle: tlas=%s geom=%s ddgi=%s sky=%s tex=%s",
                in.tlas ? "ok" : "MISSING", (in.verts && in.mats && in.albedo) ? "ok" : "MISSING",
                (in.ddgiConsts && in.irr && in.dep) ? "ok" : "MISSING",
                (in.skySH && in.skyCube) ? "ok" : "MISSING", in.texSet ? "ok" : "MISSING");
        }
        VK::Prof::ZoneEnd(cmd, zone);
        return;
    }

    // ---- the previous frame's counters, on their way out ------------------
    // Copied BEFORE anything this frame touches them, so the numbers belong to one
    // whole frame rather than to a mixture of two. Read back kFramesInFlight
    // frames later, which the frame fence has already made safe.
    ReportStats(s_slot);
    const VkBuffer ctrlBuf   = s_ctrl.GetHandle();
    const VkBuffer cellsBuf  = s_cells.GetHandle();
    const VkBuffer listBuf   = s_list.GetHandle();
    const VkBuffer constsBuf = s_consts[s_slot].GetHandle();

    VK::MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    {
        VkBufferCopy region{ 0, 0, kCtrlWords * sizeof(u32) };
        vkCmdCopyBuffer(cmd, ctrlBuf, s_stats[s_slot].GetHandle(), 1, &region);
        // The clamp-audit words (10..14) are written by the SHADE pass below —
        // inside this Update, unlike the query counters the rays fill later in
        // the frame — so they are cleared here, right after the copy that
        // preserved the previous shade's numbers. The end-of-Update fill only
        // touches 0..8 and would wipe them between the shade and the next copy.
        vkCmdFillBuffer(cmd, ctrlBuf, 10 * sizeof(u32), 5 * sizeof(u32), 0);
        VK::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    // ---- descriptors ------------------------------------------------------
    {
        BoundDecay want{ constsBuf, cellsBuf, listBuf, ctrlBuf };
        if (memcmp(&s_boundDecay[s_slot], &want, sizeof(want)) != 0) {
            VK::DescriptorWriter(s_decaySets[s_slot])
                .UniformBuffer(0, constsBuf, sizeof(Consts))
                .StorageBuffer(1, cellsBuf)
                .StorageBuffer(2, listBuf)
                .StorageBuffer(3, ctrlBuf)
                .Flush();
            s_boundDecay[s_slot] = want;
        }
    }
    {
        BoundShade want{ in.tlas, in.verts, in.mats, in.albedo, in.ddgiConsts, in.skySH, in.lights,
                         constsBuf, cellsBuf, listBuf, ctrlBuf, in.skyCube, in.irr, in.dep };
        if (memcmp(&s_boundShade[s_slot], &want, sizeof(want)) != 0) {
            VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
            asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            asInfo.accelerationStructureCount = 1;
            asInfo.pAccelerationStructures    = &in.tlas;
            VkWriteDescriptorSet asWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            asWrite.pNext = &asInfo;
            asWrite.dstSet = s_shadeSets[s_slot];
            asWrite.dstBinding = 0;
            asWrite.descriptorCount = 1;
            asWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(VulkanHW.m_Device, 1, &asWrite, 0, nullptr);

            // The probe atlases live in GENERAL permanently; the prefiltered sky
            // cube is left in SHADER_READ_ONLY by vk_ibl, like every consumer of it.
            VK::DescriptorWriter(s_shadeSets[s_slot])
                .StorageBuffer(1, in.verts)
                .StorageBuffer(2, in.mats)
                .StorageBuffer(3, in.albedo)
                .UniformBuffer(4, in.ddgiConsts)
                .StorageBuffer(5, in.skySH)
                .ImageSampler(6, in.skyCube, in.skyCubeSmp)
                .ImageSampler(7, in.irr, in.ddgiSmp, VK_IMAGE_LAYOUT_GENERAL)
                .ImageSampler(8, in.dep, in.ddgiSmp, VK_IMAGE_LAYOUT_GENERAL)
                .StorageBuffer(9, in.lights)
                .UniformBuffer(10, constsBuf, sizeof(Consts))
                .StorageBuffer(11, cellsBuf)
                .StorageBuffer(12, listBuf)
                .StorageBuffer(13, ctrlBuf)
                .Flush();
            s_boundShade[s_slot] = want;
        }
    }

    // ---- evict ------------------------------------------------------------
    // A slice, not the whole table: the sweep is pure bandwidth over tens of
    // megabytes, and one eighth per frame spreads it instead of spiking it.
    // It runs FIRST so a cell released here cannot be shaded a moment later.
    {
        const int z = VK::Prof::ZoneBegin(cmd, "WRC/Evict");
        const u32 slice = _max(64u, s_capacity / kDecaySlices);
        DecayPush dp{};
        dp.range[0] = s_decayCur;
        dp.range[1] = slice;
        dp.range[2] = u32(_max(4, _min(4096, ps_r_wrc_ttl)));
        s_decayCur = (s_decayCur + slice) & (s_capacity - 1u);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_decayPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_decayPipeLayout, 0, 1,
                                &s_decaySets[s_slot], 0, nullptr);
        vkCmdPushConstants(cmd, s_decayPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dp), &dp);
        vkCmdDispatch(cmd, (slice + 63) / 64, 1, 1);
        VK::Prof::ZoneEnd(cmd, z);
    }
    VK::ComputeBarrier(cmd);

    // ---- shade ------------------------------------------------------------
    // The queue built by LAST frame's rays. Dispatched at its cap rather than
    // indirectly off the counter: the threads past the real count read one uint
    // and return, which is cheaper than the extra dispatch and the barrier an
    // indirect argument buffer would cost.
    {
        const int z = VK::Prof::ZoneBegin(cmd, "WRC/Shade");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_shadePipe);
        const VkDescriptorSet sets[2] = { s_shadeSets[s_slot], in.texSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_shadePipeLayout, 0, 2, sets, 0, nullptr);
        vkCmdDispatch(cmd, (s_listMax + 63) / 64, 1, 1);
        VK::Prof::ZoneEnd(cmd, z);
    }

    // ---- reset the queue for THIS frame's rays -----------------------------
    // Words 0..8 only: `shadedLast` (9) was just written by the dispatch above and
    // is read back next frame.
    VK::MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, ctrlBuf, 0, 9 * sizeof(u32), 0);
    VK::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    VK::Prof::ZoneEnd(cmd, zone);

    // Say what changed, once, when it changes. Without it "the cache is on" and
    // "the cache is on and answering nothing" read identically in a log.
    static u32 s_lastSig = 0xFFFFFFFFu;
    const u32 sig = (c.ctl[3] & 0xFFu) | (c.ctl2[0] << 8) | (c.ctl2[2] << 16)
                  | (c.params2[2] > 0.f ? (1u << 18) : 0u)
                  | (c.params2[3] > 0.f ? (1u << 19) : 0u)
                  | (c.params2[2] > 1.5f ? (1u << 27) : 0u)
                  | (u32(c.params[2] * 100.f) << 20);
    if (s_lastSig != sig) {
        s_lastSig = sig;
        // ⚠ With the clamp up the "no rays" label would lie: the hemisphere comes
        // back as a METER (budget only), the ambient still reads the field.
        // Mode 2 with no skyvis rays degrades to the plain field read — the
        // label must say what actually runs, not what the cvar asked for.
        const bool bounce2   = c.params2[2] > 1.5f && c.ctl2[3] > 0u;
        const bool clampLive = c.params2[2] > 0.f && !bounce2 && c.params2[3] > 0.f && c.ctl2[3] > 0u;
        Msg("[VK WRC] weight %.2f, cell %.2f m, re-shade every %u frame(s), TTL %u, chain %u, cap %u cells, sky %s%s",
            c.params[2], c.camPos[3], c.ctl[3], u32(_max(4, _min(4096, ps_r_wrc_ttl))),
            c.ctl2[0], s_capacity,
            c.params2[2] > 0.f ? (bounce2   ? "CACHE-TO-CACHE (covered cells bounce off the cache; field only in the open)"
                                : clampLive ? "FIELD, clamped by measured injection (r_wrc_field_clamp)"
                                            : "FIELD (idTech8 cell, no rays)")
                               : "MC rays",
            ps_r_wrc_debug ? " [DEBUG VIEW]" : "");
    }
}

}}  // namespace VK::WRC

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — DDGI probe volume. See vk_ddgi.h.
#include "stdafx.h"
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_ddgi.h"
#include "vk_rt_scene.h"
#include "vk_rt_world.h"        // set 1 = the RT diffuse array (rt_shade.glsl samples it)
#include "vk_pass_gather.h"     // GatherPass::Wanted — the gather reads this field as its cache
#include "vk_wrc.h"             // the world radiance cache: a probe ray asks it BEFORE it shades anything
#include "vk_buffer.h"
#include "vk_image.h"
#include "vk_texture.h"      // CVulkanTexture — the 1x1x6 sky-probe fallback
#include "vk_barriers.h"
#include "vk_descriptors.h"
#include "vk_compute_util.h"
#include "vk_command_buffer.h"
#include "vk_color_space.h"
#include "vk_env_light.h"
#include "vk_light.h"       // Lights::CollectFrame — the world-space list the probes shade with
#include "vk_ibl.h"          // the sky the probes integrate: SH9 irradiance + the prefiltered radiance probe
#include "vk_profiler.h"
#include "vk_gfx_pipeline.h"    // GfxPipelineBuilder — the two debug views draw geometry
#include "vk_pass_context.h"    // VK::BeginOverlayRendering — they draw ON TOP of the world
#include "vk_scene_color.h"     // SceneColor::Format — the HDR target they draw into
#include "vk_rendering.h"       // RenderingBuilder (the x-ray variant skips depth entirely)
#include "vk_shaders.h"         // g_ShaderManager — the debug views load their own SPIR-V
#include "vk_swapchain.h"       // Swapchain.m_DepthFormat for the depth-tested twins
#include "HW_Vulkan.h"

#include "../../xr_3da/device.h"
#include "../../xr_3da/IGame_Persistent.h"
#include "../../xr_3da/Environment.h"

#include <cmath>

extern int   ps_r_ddgi;
extern int   ps_r_ddgi_dim_xz;
extern int   ps_r_ddgi_dim_y;
extern int   ps_r_ddgi_rays;
extern float ps_r_ddgi_spacing;
extern float ps_r_ddgi_hysteresis;
extern float ps_r_ddgi_fast;      // r_ddgi_fast - adaptive-hysteresis floor under a big change (0 = off)
extern float ps_r_ddgi_norm_bias;
extern float ps_r_ddgi_view_bias;
extern float ps_r_ddgi_depth_sharp;
extern float ps_r_ddgi_probe_dist;
extern float ps_r_ddgi_energy;
extern float ps_r_ddgi_clamp;
extern int   ps_r_ddgi_cascades;
extern int   ps_r_ddgi_lights;
extern float ps_r_ddgi_bounce;
extern float ps_r_rt_albedo;   // r_rt_albedo — GI-only reflectance scale, shipped in casc.w x256
extern float ps_r_gi_indoor;      // r_gi_indoor - reflectance boost under a ceiling, at FULL DAYLIGHT
extern float ps_r_gi_indoor_night;// r_gi_indoor_night - the same boost when the sun is gone
extern float ps_r_gi_indoor_dist; // r_gi_indoor_dist - how far up the ceiling test looks (m)
extern float ps_r_ddgi_conf;      // r_ddgi_conf - fade the lookup to black when NO probe can see the point (0 = off)
extern float ps_r_ddgi_light;
extern int   ps_r_rtgi_debug;
// PROBE RELOCATION — see shaders/ddgi_relocate.comp.glsl for what each one means.
extern int   ps_r_ddgi_reloc;
extern float ps_r_ddgi_reloc_near;
extern float ps_r_ddgi_reloc_frac;
extern float ps_r_ddgi_reloc_step;
extern float ps_r_ddgi_reloc_max;
extern float ps_r_ddgi_reloc_back;
extern float ps_r_ddgi_reloc_decay;
extern int   ps_r_gi_probe;       // the GI pixel debugger holds the relocation audit window open
extern float ps_r_ddgi_edge_clamp; // cap on the coarse cascade in the cross-fade (0 = off)
extern int   ps_r_ddgi_hit_sky;     // probe-ray hits take their sky from the field (RTXGI)
extern int   ps_r_ddgi_crush_tri;   // trilinear weight after the crush (RTXGI order)
extern int   ps_r_ddgi_fixed_rays;  // unrotated relocation/classification rays per probe
extern int   ps_r_ddgi_skip_buried; // lookup ignores probes still buried in geometry
extern int   ps_r_ddgi_classify;    // skip probes with no geometry in their cell box
extern int   ps_r_ddgi_classify_period;
extern int   ps_r_ddgi_sched;       // 0 = round-robin, 1 = cascade 0 every other frame
extern float ps_r_gather_clamp;    // the GATHER's own per-ray ceiling (0 = inherit ps_r_ddgi_clamp)
extern int   ps_r_gather_scache_blur; // 1 = prefilter the screen cache's hit (5 taps)
extern int   ps_r_gather_rays_edge;   // extra gather rays where depth is curved (0 = off)
extern int   ps_r_gather_lazy;        // skip the lattice walk where its weight is zero
extern int   ps_r_gather_wrc;         // world-cache query inside the gather (profiler switch)
extern int   ps_r_gather_indoor_ray;  // the second trace per pixel (profiler switch)
// DEBUG VIEWS — see DrawDebug below and shaders/ddgi_{probe,ray}_debug.*.
extern int   ps_r_ddgi_show_probes;
extern int   ps_r_ddgi_show_rays;
extern int   ps_r_ddgi_show_xray;
extern float ps_r_ddgi_show_dist;
extern float ps_r_ddgi_show_size;
extern float ps_r_ddgi_show_rays_scale;
extern float ps_r_ddgi_show_rays_dist;

namespace VK { namespace DDGI {

namespace {

constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
constexpr int kIrrN  = 6;
constexpr int kDepN  = 14;
constexpr int kIrrT  = kIrrN + 2;
constexpr int kDepT  = kDepN + 2;

// ⚠ Mirrors DDGIConstsBlock in shaders/ddgi_common.glsl EXACTLY (std140; every
// member is a 16-byte vector, so the offsets are unambiguous — that is why the
// block is built out of ivec4/vec4 and nothing else).
constexpr u32 kMaxCascades = 6;   // must equal DDGI_MAX_CASCADES in ddgi_common.glsl

struct Consts {
    s32   base[kMaxCascades][4];      // per cascade: xyz = window low corner, in THAT cascade's absolute grid coords
    s32   prevBase[kMaxCascades][4];  // per cascade: the window as of that cascade's PREVIOUS UPDATE
    s32   dims[4];        // xyz = grid dims (same for every cascade), w = rays per probe
    s32   layout_[4];     // x = probes per atlas row, y = SH9 sky live, z = sky cube live, w = cascade updated this frame
    s32   casc[4];        // x = cascade count, y = probes per cascade
    float grid[4];        // x = cascade 0 spacing, y = hysteresis, z = normal bias, w = depth sharpness
    float rayRot[4];      // per-frame rotation quaternion
    float sunDir[4];      // xyz = to-sun, w = max ray distance
    float sunColor[4];    // rgb; w = sky irradiance scale (the receivers' L.sky_params.y)
    float skyColor[4];    // rgb (flat fallback while the SH projection has not landed), w = energy scale
    float params[4];      // x = probe max distance, y = view bias, z = radiance clamp, w = bounce feedback
    s32   reloc[4];       // x = first atlas row of the offset strip, y = its width in texels, z = relocation live
    // INDOOR REFLECTANCE COMPENSATION (r_gi_indoor / r_gi_indoor_dist). Appended at
    // the TAIL for the reason the block already documents: every offset above it
    // stays put, so a shader that was NOT recompiled still reads the right words.
    float indoor[4];      // x = boost (1 = off), y = up-ray reach (m), z/w reserved
    // EDGE-OF-WINDOW ENERGY CEILING (r_ddgi_edge_clamp) — x = k, cap on the coarse
    // cascade's luminance in the cross-fade (see ddgi_common.glsl). Tail append,
    // same contract as `indoor`: only recompiled consumers see it.
    float edge[4];        // x = k (0 = off), y = the gather's own per-ray clamp
                          // (r_gather_clamp, 0 = inherit params.z), z = prefilter the
                          // screen cache (r_gather_scache_blur), w = extra rays on
                          // curved depth (r_gather_rays_edge). ⚠ THIS VEC4 IS FULL.
    // x = r_gather_lazy — skip the probe-lattice walk where its blend weight is
    // already zero (a pure cost knob; see gather.comp). y/z/w reserved.
    float edge2[4];
    // RTXGI conformance switches — x = r_ddgi_hit_sky, y = r_ddgi_crush_tri,
    // z = fixed rays per probe (the first z of every probe's dims.w), w =
    // r_ddgi_skip_buried. Tail append, same contract as `indoor`.
    s32   fix[4];
    // PROBE CLASSIFICATION — x = on, y = refresh period (updates), z = this
    // cascade's update counter (staggers the refreshes), w reserved.
    s32   classify[4];
};
static_assert(sizeof(Consts) == 32 * kMaxCascades + 240, "must match DDGIConstsBlock");

bool s_inited = false;
bool s_failed = false;

// ---- resources -------------------------------------------------------------
VkImage       s_irrImg = VK_NULL_HANDLE, s_depImg = VK_NULL_HANDLE;
VmaAllocation s_irrAll = nullptr,        s_depAll = nullptr;
VkImageView   s_irrView = VK_NULL_HANDLE, s_depView = VK_NULL_HANDLE;
VkSampler     s_sampler = VK_NULL_HANDLE;
CVulkanBuffer s_rays;
// One uint per ray of the cascade being updated: bit 0 = the ray stopped on SOLID
// geometry, bit 1 = it hit that geometry from behind. Its own channel rather than
// the sign of the distance (the DDGI convention) because this world has a THIRD
// kind of hit — a leaf card — and a probe inside a crown must not be relocated.
// See the write at the bottom of ddgi_trace.comp.glsl.
CVulkanBuffer s_relocHints;
CVulkanBuffer s_shDummy;   // zeroed stand-in for the sky SH9 SSBO until vk_ibl projects one
// The dynamic lights the probe rays shade with, world-space, refilled every frame.
// ⚠ Its own buffer rather than the env UBO: the probe trace is a COMPUTE pass with
// its own set, and pulling in light_ubo.glsl would drag the whole 5 KB Lighting
// block (and its ENV_SET indirection) into a shader that wants 48 bytes per light.
CVulkanBuffer s_lights[kFramesInFlight];
constexpr u32 kMaxRtLights = 64;
// Neutral 1x1x6 stand-in for the prefiltered sky cube. vk_ibl's probe does not
// exist until the first prefilter lands, and a statically-used descriptor that was
// never written is undefined behaviour even for a shader that never samples it.
CVulkanTexture* s_skyFallback = nullptr;
VkSampler       s_cubeSampler = VK_NULL_HANDLE;
CVulkanBuffer s_consts[kFramesInFlight];

u32 s_dimXZ = 0, s_dimY = 0, s_rayCount = 0, s_probeCount = 0, s_ppr = 0;
// FIXED RAYS (r_ddgi_fixed_rays): an unrotated set traced IN FRONT of the rotated
// one in every probe's run of the ray buffer — [0, F) fixed, [F, F + rays) rotated.
// s_rayCount stays the rotated (blended) count; s_rayStride is the run length and
// what the shaders see as dims.w.
u32 s_fixedRays = 0, s_rayStride = 0;
// The distance atlas format: RG32F wherever the device can storage-write AND
// linearly filter it (every desktop GPU in practice, but linear filtering of
// 32-bit float formats is optional in Vulkan), RG16F with its own blend .spv
// otherwise — the shader's storage format qualifier has to match the view.
VkFormat s_depFormat = VK_FORMAT_R32G32_SFLOAT;
// PROBE RELOCATION (r_ddgi_reloc). The per-probe offset lives in a strip of extra
// rows at the BOTTOM of the irradiance atlas — one RGBA16F texel per probe, xyz =
// metres off the lattice, w = "this probe just moved". Living there rather than in
// a buffer of its own is what keeps the feature out of six other descriptor sets:
// every consumer of the field already has this atlas bound. See ddgi_common.glsl.
u32 s_offsRow0 = 0;   // first atlas ROW of the strip
u32 s_offsW    = 0;   // its width in texels (= the atlas width)
u32 s_offsRows = 0;
u32 s_cascades = 0;      // nested windows, each 2x the spacing and reach of the last
u32 s_perCascade = 0;    // probes in ONE cascade = the per-frame work unit
u32 s_gen = 0;
bool s_needClear = true;

// Per cascade, because they are measured at different times: base[c] is where that
// cascade's atlas content lives, and it only moves on the frame that cascade updates.
s32  s_base[kMaxCascades][3]     = {};
s32  s_prevBase[kMaxCascades][3] = {};
bool s_haveBase[kMaxCascades]    = {};
u32  s_cascThisFrame = 0;
u64  s_updateTick    = 0;
// Updates each cascade has had since the field was (re)built. Staggers the
// refreshes of classified-inactive probes (Consts::classify.z).
u32  s_cascUpdates[kMaxCascades] = {};
u64  s_frame       = ~0ull;
u32  s_slot        = 0;

// ---- pipelines -------------------------------------------------------------
VkDescriptorSetLayout s_traceLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_tracePool   = VK_NULL_HANDLE;
VkDescriptorSet       s_traceSets[kFramesInFlight] = {};
VkPipelineLayout      s_tracePipeLayout = VK_NULL_HANDLE;
VkPipeline            s_tracePipe = VK_NULL_HANDLE;

// ---- probe relocation ------------------------------------------------------
VkDescriptorSetLayout s_relocLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_relocPool   = VK_NULL_HANDLE;
VkDescriptorSet       s_relocSets[kFramesInFlight] = {};
VkPipelineLayout      s_relocPipeLayout = VK_NULL_HANDLE;
VkPipeline            s_relocPipe = VK_NULL_HANDLE;
// Self-audit counters, one buffer per frame slot because the CPU reads a slot's
// numbers while later frames are still writing theirs. HOST-visible: six atomics
// per probe update is nothing, and a readback path would be more machinery than
// the numbers are worth.
CVulkanBuffer s_relocStats[kFramesInFlight];
struct RelocPush { float nearK, frac, stepK, maxK, backFrac, decay; s32 audit;
                   float camX, camY, camZ, nearR; };
// Accumulated across dispatches so the log gets one line per second or so rather
// than one per frame — and only for a while after the feature is switched on,
// the same window vk_wrc uses.
u64  s_relocSeen = 0, s_relocBuried = 0, s_relocMoved = 0, s_relocOffMM = 0;
u64  s_relocBackHits = 0, s_relocHits = 0;
// The same backface question asked of DOWNWARD rays only — see the shader. This is
// the one that says whether the winding is worth believing.
u64  s_relocDownHits = 0, s_relocDownBack = 0;
// ⭐AND THE PROBES AROUND THE PLAYER, IN ABSOLUTE NUMBERS. A percentage over a
// whole cascade printed 0.0% for a room whose entire question was two or three
// probes — the instrument was too coarse to answer the only thing being asked.
u64  s_relocLocal = 0, s_relocLocalBuried = 0, s_relocLocalMoved = 0, s_relocLocalOffMM = 0;
u64  s_relocLocalHits = 0;   // solid hits of THOSE probes — "are the walls in the BVH at all"
// PROBE CENSUS (milli-luminance sums): the probes near the camera against the whole
// cascade. Their ratio is "do the probes inside this house shine like the street".
u64  s_relocLocalIrrM = 0, s_relocAllIrrM = 0;
u64  s_relocInactive = 0;   // classification: probes flagged inactive (r_ddgi_classify)
u32  s_relocSamples = 0;   // dispatches folded into the sums above
u32  s_relocSamplesForLocal = 0;   // same count, kept separate so the local means survive a reset
u32  s_relocPrints  = 0;   // audit windows already printed
int  s_relocWas     = -1;  // last seen r_ddgi_reloc, to catch the toggle
float s_indoorBoostNow = 1.f;   // the indoor boost as fed to the shaders this frame (IndoorBoost())

// ---- debug views (r_ddgi_show_probes / r_ddgi_show_rays) --------------------
// Two draws that generate their own vertices: six per probe for the dots, two per
// ray for the segments. No vertex buffer, because everything they need already
// lives on the GPU — and the offsets in particular the CPU has never seen.
VkDescriptorSetLayout s_dbgLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_dbgPool   = VK_NULL_HANDLE;
VkDescriptorSet       s_dbgSets[kFramesInFlight] = {};
VkPipelineLayout      s_dbgPipeLayout = VK_NULL_HANDLE;
// Depth-tested and x-ray twins of each: a probe sealed inside a wall is exactly
// the one worth looking at, and depth testing is what hides it.
VkPipeline s_dbgProbePipe = VK_NULL_HANDLE, s_dbgProbePipeX = VK_NULL_HANDLE;
VkPipeline s_dbgRayPipe   = VK_NULL_HANDLE, s_dbgRayPipeX   = VK_NULL_HANDLE;
bool       s_dbgInited = false, s_dbgFailed = false;
struct DbgPush { Fmatrix viewProj; float cam[4]; float opts[4]; };
struct BoundDbg { VkBuffer consts, rays; VkImageView irr, dep; };
BoundDbg s_boundDbg[kFramesInFlight] = {};

VkDescriptorSetLayout s_blendLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_blendPool   = VK_NULL_HANDLE;
VkDescriptorSet       s_blendSets[kFramesInFlight * 2] = {};   // [frame*2 + 0] irr, [frame*2 + 1] dep
VkPipelineLayout      s_blendPipeLayout = VK_NULL_HANDLE;
VkPipeline            s_blendIrrPipe = VK_NULL_HANDLE;
VkPipeline            s_blendDepPipe = VK_NULL_HANDLE;

// What the trace set currently points at — the bubble rebuild swaps all three
// geometry handles at once and a set an in-flight command buffer still uses must
// not be rewritten under it (same rule as the RTAO/RTGI passes).
struct BoundRT { VkAccelerationStructureKHR tlas; VkBuffer verts, mats, alb, consts, rays, sh;
                 VkImageView skyCube, irr, dep;
                 VkBuffer wrcConsts, wrcCells, wrcList, wrcCtrl, hints; };
BoundRT s_boundTrace[kFramesInFlight] = {};
struct BoundBlend { VkBuffer consts, rays; VkImageView img, offs; };
BoundBlend s_boundBlend[kFramesInFlight * 2] = {};
struct BoundReloc { VkBuffer consts, rays, stats, hints; VkImageView img; };
BoundReloc s_boundReloc[kFramesInFlight] = {};

u32 CeilSqrt(u32 v)
{
    u32 r = u32(std::ceil(std::sqrt(double(v))));
    return _max(1u, r);
}

void DestroyAtlases()
{
    if (s_irrView) { vkDestroyImageView(VulkanHW.m_Device, s_irrView, nullptr); s_irrView = VK_NULL_HANDLE; }
    if (s_depView) { vkDestroyImageView(VulkanHW.m_Device, s_depView, nullptr); s_depView = VK_NULL_HANDLE; }
    if (s_irrImg)  { vmaDestroyImage(VulkanHW.m_Allocator, s_irrImg, s_irrAll); s_irrImg = VK_NULL_HANDLE; s_irrAll = nullptr; }
    if (s_depImg)  { vmaDestroyImage(VulkanHW.m_Allocator, s_depImg, s_depAll); s_depImg = VK_NULL_HANDLE; s_depAll = nullptr; }
    s_rays.Destroy();
    s_relocHints.Destroy();
    s_probeCount = 0;
}

// (Re)create everything whose size depends on the grid shape. Returns false if
// the volume cannot be built, which disables it rather than half-running it.
bool EnsureResources()
{
    const u32 dimXZ = u32(_max(4, _min(64, ps_r_ddgi_dim_xz)));
    const u32 dimY  = u32(_max(2, _min(32, ps_r_ddgi_dim_y)));
    const u32 rays  = u32(_max(8, _min(64, ps_r_ddgi_rays)));
    const u32 casc  = u32(_max(1, _min(int(kMaxCascades), ps_r_ddgi_cascades)));
    const u32 fixedR = u32(_max(0, _min(32, ps_r_ddgi_fixed_rays)));

    if (s_probeCount != 0 && dimXZ == s_dimXZ && dimY == s_dimY && rays == s_rayCount &&
        casc == s_cascades && fixedR == s_fixedRays) return true;

    // The old atlases may still be referenced by frames in flight. This only runs
    // on a console change, so a full idle is the honest way to make it safe.
    VK_DeviceWaitIdleLocked();
    DestroyAtlases();

    s_dimXZ = dimXZ; s_dimY = dimY; s_rayCount = rays; s_cascades = casc;
    s_fixedRays = fixedR; s_rayStride = rays + fixedR;
    s_perCascade = dimXZ * dimY * dimXZ;
    // All cascades share ONE atlas, laid out as consecutive runs. The atlas is sized
    // for every cascade, but only one cascade's worth is traced and blended per frame.
    s_probeCount = s_perCascade * casc;
    s_ppr = CeilSqrt(s_probeCount);
    const u32 rows = (s_probeCount + s_ppr - 1) / s_ppr;

    // The relocation strip: one texel per probe, packed across the atlas's full
    // width, in rows appended below the last tile row. It is part of the same image
    // so that consumers need no second binding — and it sits BELOW every tile, so
    // no bilinear tap inside a tile can reach it (the one-texel tile border already
    // keeps taps in their own tile).
    s_offsW    = s_ppr * kIrrT;
    s_offsRows = (s_probeCount + s_offsW - 1) / s_offsW;
    s_offsRow0 = rows * kIrrT;

    const VkExtent2D irrExt{ s_ppr * kIrrT, rows * kIrrT + s_offsRows };
    const VkExtent2D depExt{ s_ppr * kDepT, rows * kDepT };

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // ⚠ DISTANCE MOMENTS ARE RG32F. The Chebyshev variance is E[d^2] - E[d]^2, a
    // difference of two large, close numbers; in fp16 the step of d^2 at the
    // coarse cascades' 32 m+ ceilings (1.0 at 1024) is as large as the variance
    // being measured, so the test those cascades ran was mostly quantisation.
    // Twice the memory of the distance atlas (~16 MB at the shipping grid).
    // s_depFormat falls back to RG16F on a device that cannot filter RG32F (Init).
    if (!CreateImage2D(VK_FORMAT_R16G16B16A16_SFLOAT, irrExt, usage, s_irrImg, s_irrAll, "DDGI/irradiance") ||
        !CreateImage2D(s_depFormat,                   depExt, usage, s_depImg, s_depAll, "DDGI/distance")) {
        Msg("![VK DDGI] atlas create failed (%ux%u probes, %ux%u / %ux%u texels) — volume off",
            dimXZ, dimY, irrExt.width, irrExt.height, depExt.width, depExt.height);
        DestroyAtlases();
        return false;
    }
    s_irrView = CreateImageView(s_irrImg, VK_FORMAT_R16G16B16A16_SFLOAT);
    s_depView = CreateImageView(s_depImg, s_depFormat);

    // Sized for ONE cascade: that is all a frame ever traces.
    s_rays.Create(VkDeviceSize(s_perCascade) * s_rayStride * sizeof(float) * 4,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    // The relocation hints ride along with the rays: same count, same lifetime, and
    // written by the same dispatch. A uint each — one byte would do, but a byte
    // array costs an extension and this is 512 KB against the ray buffer's 2 MB.
    s_relocHints.Create(VkDeviceSize(s_perCascade) * s_rayStride * sizeof(u32),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    if (!s_irrView || !s_depView || !s_rays.IsValid() || !s_relocHints.IsValid()) {
        Msg("![VK DDGI] atlas view / ray buffer alloc failed — volume off");
        DestroyAtlases();
        return false;
    }

    s_needClear = true;
    for (auto& h : s_haveBase) h = false;
    s_updateTick = 0;
    for (auto& n : s_cascUpdates) n = 0;
    ++s_gen;
    for (auto& b : s_boundTrace) b = BoundRT{};
    for (auto& b : s_boundBlend) b = BoundBlend{};
    for (auto& b : s_boundReloc) b = BoundReloc{};
    s_relocPrints = 0; s_relocSamples = 0;

    const float reachOuter = ps_r_ddgi_spacing * float(1u << (casc - 1)) * float(dimXZ);
    Msg("[VK DDGI] %u cascades x %ux%ux%u = %u probes (%u per cascade, ONE updated per frame) | "
        "spacing %.2f..%.2f m, reach %.0f m, %u rays (+%u fixed) | irradiance %ux%u (+%u row%s of relocation "
        "offsets from y=%u), distance %ux%u, rays %.1f MB",
        casc, dimXZ, dimY, dimXZ, s_probeCount, s_perCascade,
        ps_r_ddgi_spacing, ps_r_ddgi_spacing * float(1u << (casc - 1)), reachOuter, rays, fixedR,
        irrExt.width, irrExt.height, s_offsRows, (s_offsRows == 1) ? "" : "s", s_offsRow0,
        depExt.width, depExt.height,
        float(s_rays.GetSize()) / (1024.f * 1024.f));
    return true;
}

// A uniformly distributed random rotation (Shoemake). Used to turn the fixed
// Fibonacci ray set into a different set every frame — without it the hysteresis
// would converge to whatever those exact directions happen to see.
void RandomQuat(float q[4])
{
    const float u1 = Random.randF(), u2 = Random.randF(), u3 = Random.randF();
    const float s1 = std::sqrt(1.f - u1), s2 = std::sqrt(u1);
    const float t1 = 2.f * PI * u2,       t2 = 2.f * PI * u3;
    q[0] = s1 * std::sin(t1);
    q[1] = s1 * std::cos(t1);
    q[2] = s2 * std::sin(t2);
    q[3] = s2 * std::cos(t2);
}

}   // anon namespace

void SunSky(Fvector& toSun, Fvector3& sunColor, Fvector3& skyColor)
{
    sunColor.set(1.f, 1.f, 1.f);
    skyColor.set(0.30f, 0.33f, 0.40f);
    if (g_pGamePersistent) {
        if (const auto* E = ES().CurrentEnv) {
            sunColor = E->sun_color;
            skyColor.set(E->hemi_color.x, E->hemi_color.y, E->hemi_color.z);
        }
    }
    ColorSpace::Linearize(sunColor);
    ColorSpace::Linearize(skyColor);

    toSun = EnvLight::SunDirVisual();     // env sun_dir travels DOWNWARD
    toSun.mul(-1.f);
    if (toSun.magnitude() > 1e-4f) toSun.normalize(); else toSun.set(0.f, 1.f, 0.f);
}

void SkyInputs(VkBuffer& shBuffer, VkImageView& skyCube, VkSampler& cubeSampler)
{
    // The same two liveness tests Update() writes into layout_.y / layout_.z, so a
    // consumer of ddgi_sky.glsl reads the coefficients through the flags that were
    // set for THIS frame's constants. Deriving them twice from IBL is safe because
    // both derivations happen inside one frame, before any submit.
    const bool shLive   = (IBL::GetSHBuffer() != VK_NULL_HANDLE) && IBL::SHReady();
    const bool cubeLive = (IBL::GetSpecView() != VK_NULL_HANDLE) && IBL::Ready();
    shBuffer    = shLive   ? IBL::GetSHBuffer() : s_shDummy.GetHandle();
    skyCube     = cubeLive ? IBL::GetSpecView() : (s_skyFallback ? s_skyFallback->GetView() : VK_NULL_HANDLE);
    cubeSampler = cubeLive ? IBL::GetSampler()  : s_cubeSampler;
}

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;

    if (!RTScene::Init()) { s_failed = true; return false; }

    // trace: 0 TLAS, 1 verts, 2 mats, 3 albedo, 4 consts, 5 ray output,
    //        6 sky SH9 (irradiance, for a HIT's sky term),
    //        7 prefiltered sky cube (radiance, for a MISS),
    //        8/9 the probe atlases THEMSELVES — the multi-bounce read of last
    //            frame's field at the hit point,
    //        10 the frame's dynamic lights, world-space. See ddgi_trace.comp.glsl.
    //        11..14 THE WORLD RADIANCE CACHE (vk_wrc): constants, cell table,
    //            shade queue, control block. A probe ray asks it before shading
    //            anything, and on a hit it shades NOTHING — which is the whole
    //            reason idTech 8's probe update costs 0.08 ms.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },   // 15: relocation hints
                                kFramesInFlight, s_traceLayout, s_tracePool, s_traceSets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "DDGItrace")) {
        s_failed = true; return false;
    }
    // blend: 0 consts, 1 rays, 2 target atlas, 3 the IRRADIANCE atlas again.
    // 3 exists for the relocation strip alone: both blends have to know whether the
    // probe moved since the history they hold was measured, and the distance blend
    // cannot read that out of its own (RG16F) atlas. The irradiance blend has the
    // same image at 2 and reads the strip through that one.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },
                                kFramesInFlight * 2, s_blendLayout, s_blendPool, s_blendSets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "DDGIblend")) {
        s_failed = true; return false;
    }
    // relocate: 0 consts, 1 rays (this update's), 2 the irradiance atlas (the offset
    // strip is written here), 3 the self-audit counters, 4 the per-ray hints.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
                                kFramesInFlight, s_relocLayout, s_relocPool, s_relocSets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "DDGIreloc")) {
        s_failed = true; return false;
    }

    // Set 1 = the world diffuse array, owned by vk_rt_world and shared with the
    // RT preview: one publication of the table, one place a slot can be wrong.
    const VkDescriptorSetLayout texLayout = RTWorld::TexSetLayout();
    if (texLayout == VK_NULL_HANDLE) {
        Msg("![VK DDGI] no RT diffuse array (descriptor indexing?) — probe volume disabled");
        s_failed = true; return false;
    }
    s_tracePipeLayout = VK::MakePipelineLayout({ s_traceLayout, texLayout });
    s_blendPipeLayout = VK::MakePipelineLayout({ s_blendLayout });
    s_relocPipeLayout = VK::MakePipelineLayout({ s_relocLayout }, sizeof(RelocPush));
    if (!s_tracePipeLayout || !s_blendPipeLayout || !s_relocPipeLayout) { s_failed = true; return false; }

    s_tracePipe    = VK::CreateComputePipeline("ddgi_trace.comp.spv",     s_tracePipeLayout, "DDGItrace");
    s_blendIrrPipe = VK::CreateComputePipeline("ddgi_blend_irr.comp.spv", s_blendPipeLayout, "DDGIblendIrr");
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(VulkanHW.m_PhysicalDevice, VK_FORMAT_R32G32_SFLOAT, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        const bool fp32 = (fp.optimalTilingFeatures & need) == need;
        s_depFormat = fp32 ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R16G16_SFLOAT;
        if (!fp32) Msg("~[VK DDGI] RG32F cannot be storage-written + linearly filtered here — distance moments fall back to RG16F");
        s_blendDepPipe = VK::CreateComputePipeline(fp32 ? "ddgi_blend_dep.comp.spv" : "ddgi_blend_dep_fp16.comp.spv",
                                                   s_blendPipeLayout, "DDGIblendDep");
    }
    s_relocPipe    = VK::CreateComputePipeline("ddgi_relocate.comp.spv",  s_relocPipeLayout, "DDGIreloc");
    if (!s_tracePipe || !s_blendIrrPipe || !s_blendDepPipe || !s_relocPipe) {
        Msg("![VK DDGI] compute pipeline create failed — volume off");
        s_failed = true; return false;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        s_consts[i].Create(sizeof(Consts), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST, false);
        if (!s_consts[i].IsValid()) { Msg("![VK DDGI] consts UBO alloc failed"); s_failed = true; return false; }
        // Per slot, because it is rewritten every frame while earlier frames are
        // still reading theirs.
        s_lights[i].Create(kMaxRtLights * sizeof(Lights::GpuLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST, false);
        if (!s_lights[i].IsValid()) { Msg("![VK DDGI] light SSBO alloc failed"); s_failed = true; return false; }
        // Six counters, zeroed by the CPU on the frame it reads them back.
        s_relocStats[i].Create(16 * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VMA_MEMORY_USAGE_AUTO_PREFER_HOST, false);
        if (!s_relocStats[i].IsValid()) { Msg("![VK DDGI] relocation stats alloc failed"); s_failed = true; return false; }
        if (void* p = s_relocStats[i].Map()) { memset(p, 0, 16 * sizeof(u32)); s_relocStats[i].Flush(); s_relocStats[i].Unmap(); }
    }

    // Placeholder for the sky SH9 SSBO (trace binding 6) until vk_ibl has projected
    // one. Its OWN buffer rather than an alias of the ray buffer: aliasing a buffer
    // the same dispatch writes would be a read-after-write hazard against itself,
    // and sync validation would (rightly) say so.
    s_shDummy.Create(9 * 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST, false);
    if (!s_shDummy.IsValid()) { Msg("![VK DDGI] SH placeholder alloc failed"); s_failed = true; return false; }
    if (void* p = s_shDummy.Map()) { memset(p, 0, 9 * 4 * sizeof(float)); s_shDummy.Flush(); s_shDummy.Unmap(); }

    // ...and the same for the prefiltered sky cube (trace binding 7): a neutral grey
    // 1x1x6 plus a trilinear clamp sampler, so the binding is defined from the first
    // dispatch. layout_.z gates the read, so this is never actually sampled.
    {
        VkSamplerCreateInfo sci{};
        sci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter     = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU  = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxAnisotropy = 1.0f; sci.minLod = 0.0f; sci.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(VulkanHW.m_Device, &sci, nullptr, &s_cubeSampler) != VK_SUCCESS) {
            Msg("![VK DDGI] cube sampler create failed"); s_failed = true; return false;
        }
    }
    s_skyFallback = xr_new<CVulkanTexture>();
    s_skyFallback->m_bCubemap = true; s_skyFallback->m_ArrayLayers = 6;
    s_skyFallback->Create(1, 1, VK_FORMAT_R8G8B8A8_UNORM, 1);
    {
        u8 face[24];
        for (u32 i = 0; i < 6; ++i) { face[i*4+0] = 128; face[i*4+1] = 128; face[i*4+2] = 128; face[i*4+3] = 255; }
        s_skyFallback->UploadData(face, sizeof(face));
    }
    if (s_skyFallback->GetView() == VK_NULL_HANDLE) { Msg("![VK DDGI] sky fallback cube failed"); s_failed = true; return false; }

    // Linear + clamp. The one-texel border each probe tile carries is what makes a
    // plain clamp sampler behave like the octahedral wrap, so no address trickery
    // is needed here — and must not be added, or the border stops being the seam.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.f;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) {
        s_failed = true; return false;
    }

    Msg("[VK DDGI] init OK");
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    DestroyAtlases();
    for (auto& c : s_consts) c.Destroy();
    for (auto& b : s_lights) b.Destroy();
    for (auto& b : s_relocStats) b.Destroy();
    s_shDummy.Destroy();
    if (s_skyFallback)  { s_skyFallback->Destroy(); xr_delete(s_skyFallback); }
    if (s_cubeSampler)  { vkDestroySampler(VulkanHW.m_Device, s_cubeSampler, nullptr); s_cubeSampler = VK_NULL_HANDLE; }
    if (s_sampler)         { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    if (s_tracePipe)       { vkDestroyPipeline(VulkanHW.m_Device, s_tracePipe, nullptr); s_tracePipe = VK_NULL_HANDLE; }
    if (s_blendIrrPipe)    { vkDestroyPipeline(VulkanHW.m_Device, s_blendIrrPipe, nullptr); s_blendIrrPipe = VK_NULL_HANDLE; }
    if (s_blendDepPipe)    { vkDestroyPipeline(VulkanHW.m_Device, s_blendDepPipe, nullptr); s_blendDepPipe = VK_NULL_HANDLE; }
    if (s_relocPipe)       { vkDestroyPipeline(VulkanHW.m_Device, s_relocPipe, nullptr); s_relocPipe = VK_NULL_HANDLE; }
    if (s_dbgProbePipe)    { vkDestroyPipeline(VulkanHW.m_Device, s_dbgProbePipe, nullptr); s_dbgProbePipe = VK_NULL_HANDLE; }
    if (s_dbgProbePipeX)   { vkDestroyPipeline(VulkanHW.m_Device, s_dbgProbePipeX, nullptr); s_dbgProbePipeX = VK_NULL_HANDLE; }
    if (s_dbgRayPipe)      { vkDestroyPipeline(VulkanHW.m_Device, s_dbgRayPipe, nullptr); s_dbgRayPipe = VK_NULL_HANDLE; }
    if (s_dbgRayPipeX)     { vkDestroyPipeline(VulkanHW.m_Device, s_dbgRayPipeX, nullptr); s_dbgRayPipeX = VK_NULL_HANDLE; }
    if (s_dbgPipeLayout)   { vkDestroyPipelineLayout(VulkanHW.m_Device, s_dbgPipeLayout, nullptr); s_dbgPipeLayout = VK_NULL_HANDLE; }
    if (s_dbgPool)         { vkDestroyDescriptorPool(VulkanHW.m_Device, s_dbgPool, nullptr); s_dbgPool = VK_NULL_HANDLE; }
    if (s_dbgLayout)       { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_dbgLayout, nullptr); s_dbgLayout = VK_NULL_HANDLE; }
    for (auto& s : s_dbgSets)   s = VK_NULL_HANDLE;
    for (auto& b : s_boundDbg)  b = BoundDbg{};
    s_dbgInited = s_dbgFailed = false;
    if (s_tracePipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_tracePipeLayout, nullptr); s_tracePipeLayout = VK_NULL_HANDLE; }
    if (s_blendPipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_blendPipeLayout, nullptr); s_blendPipeLayout = VK_NULL_HANDLE; }
    if (s_relocPipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_relocPipeLayout, nullptr); s_relocPipeLayout = VK_NULL_HANDLE; }
    if (s_tracePool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_tracePool, nullptr); s_tracePool = VK_NULL_HANDLE; }
    if (s_blendPool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_blendPool, nullptr); s_blendPool = VK_NULL_HANDLE; }
    if (s_relocPool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_relocPool, nullptr); s_relocPool = VK_NULL_HANDLE; }
    if (s_traceLayout)     { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_traceLayout, nullptr); s_traceLayout = VK_NULL_HANDLE; }
    if (s_blendLayout)     { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_blendLayout, nullptr); s_blendLayout = VK_NULL_HANDLE; }
    if (s_relocLayout)     { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_relocLayout, nullptr); s_relocLayout = VK_NULL_HANDLE; }
    for (auto& s : s_traceSets) s = VK_NULL_HANDLE;
    for (auto& s : s_blendSets) s = VK_NULL_HANDLE;
    for (auto& s : s_relocSets) s = VK_NULL_HANDLE;
    for (auto& b : s_boundTrace) b = BoundRT{};
    for (auto& b : s_boundBlend) b = BoundBlend{};
    for (auto& b : s_boundReloc) b = BoundReloc{};
    s_dimXZ = s_dimY = s_rayCount = 0;
    s_inited = false; s_failed = false;
}

void OnLevelUnload()
{
    s_needClear = true;
    for (auto& h : s_haveBase) h = false;
}

// ⭐ONE KNOB IS ENOUGH. r_ddgi_light is the CONSUMER and r_ddgi the PRODUCER, and
// leaving that split visible is a trap this renderer has already walked into once
// (the RTWanted note in vk_rt_scene.cpp): turning up the consumer alone found no
// field, changed nothing, and read as a broken feature rather than as a knob whose
// partner was off. So asking for the field in the lighting IS asking for the volume.
// r_ddgi stays as the way to run the volume WITHOUT it lighting anything, which is
// what the r_rtgi_debug 5/6 previews want.
// ⭐r_gather raises the volume too, and that is not a convenience: the final
// gather READS this field as its radiance cache — every hit its rays make resolves
// to ddgiSampleIrradiance. Asking for the gather with the volume off would trace a
// million rays into a black field, which looks exactly like a broken shader; the
// same producer/consumer trap the r_ddgi / r_ddgi_light split already cost a
// session to, so it is closed here on the way in rather than diagnosed later.
// ...and the world radiance cache for the same reason from the other side: its
// cells take their multi-bounce term out of THIS field, and its shade pass reads
// the sun and the sky out of THIS module's constants. r_wrc alone therefore has
// to raise the volume, or it would fill a cache from an empty one.
static bool Wanted() { return ps_r_ddgi != 0 || ps_r_ddgi_light > 0.f || VK::GatherPass::Wanted()
                          || VK::WRC::Wanted(); }

bool Enabled()
{
    return Wanted() && s_inited && !s_failed && s_probeCount != 0 && RTScene::Ready();
}

VkBuffer    ConstsBuffer()   { return s_consts[s_slot].GetHandle(); }
VkImageView IrradianceView() { return s_irrView; }
VkImageView DistanceView()   { return s_depView; }
VkSampler   GetSampler()     { return s_sampler; }
float       IndoorBoost()    { return s_indoorBoostNow; }
u32         Generation()     { return s_gen; }
// Probes across ALL cascades — the length of the atlas's flat run, and therefore
// the size of anything indexed by ddgiGlobalProbe(). 0 before the atlases exist.
u32         ProbeCount()     { return s_probeCount; }

namespace {

// Built on first use, like every other debug path here: a view nobody switched on
// should not cost two pipelines at startup.
bool EnsureDebugPipes()
{
    if (s_dbgInited) return !s_dbgFailed;
    s_dbgInited = true;

    // 0 consts, 1 irradiance atlas (the dots read their colour AND the relocation
    // strip out of it), 2 distance atlas (declared by DDGI_SAMPLE, unread here),
    // 3 the ray buffer. All in the VERTEX stage — these shaders compute geometry.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
                                kFramesInFlight, s_dbgLayout, s_dbgPool, s_dbgSets,
                                VK_SHADER_STAGE_VERTEX_BIT, "DDGIdbg")) {
        s_dbgFailed = true; return false;
    }
    s_dbgPipeLayout = VK::MakePipelineLayout({ s_dbgLayout }, sizeof(DbgPush),
                                             VK_SHADER_STAGE_VERTEX_BIT);
    if (!s_dbgPipeLayout) { s_dbgFailed = true; return false; }

    VkShaderModule pv = g_ShaderManager->Load("ddgi_probe_debug.vert.spv");
    VkShaderModule pf = g_ShaderManager->Load("ddgi_probe_debug.frag.spv");
    VkShaderModule rv = g_ShaderManager->Load("ddgi_ray_debug.vert.spv");
    VkShaderModule rf = g_ShaderManager->Load("ddgi_ray_debug.frag.spv");
    if (!pv || !pf || !rv || !rf) {
        Msg("![VK DDGI] debug view shaders failed to load — r_ddgi_show_* disabled");
        s_dbgFailed = true; return false;
    }

    // Depth TESTED but not written: the views sit on top of the world without
    // disturbing anything that reads depth later.
    s_dbgProbePipe = VK::GfxPipelineBuilder(s_dbgPipeLayout).Vert(pv).Frag(pf)
        .Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST).Depth(true, false)
        .Color(VK::SceneColor::Format()).DepthTarget(Swapchain.m_DepthFormat)
        .Build("DDGI probe dots");
    s_dbgRayPipe = VK::GfxPipelineBuilder(s_dbgPipeLayout).Vert(rv).Frag(rf)
        .Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST).Depth(true, false)
        .Color(VK::SceneColor::Format()).DepthTarget(Swapchain.m_DepthFormat)
        .Build("DDGI ray lines");
    // ⭐THE X-RAY TWINS ARE THE POINT, not a convenience. The probe this feature
    // exists for is the one sealed inside a wall, and a depth test hides exactly
    // that one. No depth attachment at all, so nothing can reject a fragment.
    s_dbgProbePipeX = VK::GfxPipelineBuilder(s_dbgPipeLayout).Vert(pv).Frag(pf)
        .Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST).Depth(false, false)
        .Color(VK::SceneColor::Format())
        .Build("DDGI probe dots (x-ray)");
    s_dbgRayPipeX = VK::GfxPipelineBuilder(s_dbgPipeLayout).Vert(rv).Frag(rf)
        .Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST).Depth(false, false)
        .Color(VK::SceneColor::Format())
        .Build("DDGI ray lines (x-ray)");

    if (!s_dbgProbePipe || !s_dbgRayPipe || !s_dbgProbePipeX || !s_dbgRayPipeX) {
        Msg("![VK DDGI] debug view pipeline create failed — r_ddgi_show_* disabled");
        s_dbgFailed = true; return false;
    }
    Msg("[VK DDGI] debug views ready: r_ddgi_show_probes 1/2, r_ddgi_show_rays 1/2 "
        "(r_ddgi_show_xray 1 to see the ones inside walls)");
    return true;
}

}   // anon namespace

void DrawDebug(VK::FrameContext& ctx)
{
    if (ps_r_ddgi_show_probes <= 0 && ps_r_ddgi_show_rays <= 0) return;
    if (!Enabled() || s_probeCount == 0) return;
    if (!EnsureDebugPipes()) return;

    const VkBuffer constsBuf = s_consts[s_slot].GetHandle();
    const VkBuffer raysBuf   = s_rays.GetHandle();
    if (!constsBuf || !raysBuf || !s_irrView || !s_depView) return;

    BoundDbg want{ constsBuf, raysBuf, s_irrView, s_depView };
    if (memcmp(&s_boundDbg[s_slot], &want, sizeof(want)) != 0) {
        const VkDescriptorBufferInfo cb{ constsBuf, 0, sizeof(Consts) };
        const VkDescriptorBufferInfo rb{ raysBuf, 0, VK_WHOLE_SIZE };
        // The atlases live permanently in GENERAL (storage write + sampled read).
        const VkDescriptorImageInfo ii{ s_sampler, s_irrView, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo di{ s_sampler, s_depView, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w[4]{};
        for (u32 i = 0; i < 4; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s_dbgSets[s_slot]; w[i].dstBinding = i; w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         w[0].pBufferInfo = &cb;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo  = &ii;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo  = &di;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         w[3].pBufferInfo = &rb;
        vkUpdateDescriptorSets(VulkanHW.m_Device, 4, w, 0, nullptr);
        s_boundDbg[s_slot] = want;
    }

    const int zone = VK::Prof::ZoneBegin(ctx.cmd, "DDGIdbg");
    const bool xray = ps_r_ddgi_show_xray != 0;

    DbgPush push{};
    push.viewProj = ctx.viewProj ? *ctx.viewProj : Fidentity;
    const Fvector& cam = RV().vCameraPosition;
    push.cam[0] = cam.x; push.cam[1] = cam.y; push.cam[2] = cam.z;
    push.cam[3] = _max(1.f, ps_r_ddgi_show_dist);
    push.opts[1] = _max(0.001f, ps_r_ddgi_show_size);
    push.opts[2] = _max(0.f, ps_r_ddgi_show_rays_scale);
    push.opts[3] = _max(0.5f, ps_r_ddgi_show_rays_dist);

    // X-ray draws with no depth attachment, so it needs its own rendering scope.
    if (xray) RenderingBuilder(ctx.extent).Color(ctx.colorView).BeginFlipped(ctx.cmd);
    else      VK::BeginOverlayRendering(ctx.cmd, ctx, VK_ATTACHMENT_STORE_OP_DONT_CARE);

    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_dbgPipeLayout, 0, 1,
                            &s_dbgSets[s_slot], 0, nullptr);

    // Rays first, dots on top: the dot marks the origin of its own bundle, and a
    // bundle drawn over its probe hides the state colour that explains it.
    if (ps_r_ddgi_show_rays > 0) {
        push.opts[0] = float(ps_r_ddgi_show_rays);
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, xray ? s_dbgRayPipeX : s_dbgRayPipe);
        vkCmdPushConstants(ctx.cmd, s_dbgPipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        // One cascade's worth — that is all the ray buffer ever holds.
        vkCmdDraw(ctx.cmd, s_perCascade * s_rayStride * 2u, 1, 0, 0);
    }
    if (ps_r_ddgi_show_probes > 0) {
        push.opts[0] = float(ps_r_ddgi_show_probes);
        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, xray ? s_dbgProbePipeX : s_dbgProbePipe);
        vkCmdPushConstants(ctx.cmd, s_dbgPipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(ctx.cmd, s_probeCount * 6u, 1, 0, 0);   // every cascade
    }

    vkCmdEndRendering(ctx.cmd);
    VK::Prof::ZoneEnd(ctx.cmd, zone);
}

void Update(VkCommandBuffer cmd)
{
    // The probes trace the bubble, so it has to exist first. RTScene guards itself
    // against being stepped twice in a frame, so whoever calls first wins and the
    // SSAO/RTAO call later this frame becomes a no-op.
    RTScene::Update(cmd, RV().vCameraPosition);

    // The debug preview declares the atlases and the constants UBO in its shader,
    // so they must EXIST and hold sane values even with the volume itself off —
    // a statically-used descriptor that was never written is a validation error,
    // and a zeroed spacing would divide by zero inside the lookup. So resources
    // and constants are maintained for either gate; only the dispatches below are
    // the volume's own.
    if (!Wanted() && ps_r_rtgi_debug == 0) return;
    if (!Init()) return;
    if (!EnsureResources()) { s_failed = true; return; }

    // One step per frame: this is called from its own pass, but a second caller
    // would otherwise double-blend and halve the effective hysteresis.
    if (RV().frameIndex == s_frame) return;
    s_frame = RV().frameIndex;
    s_slot  = CommandManager.GetCurrentFrame() % kFramesInFlight;

    const int zone = VK::Prof::ZoneBegin(cmd, "DDGI");

    // ---- which cascade updates this frame ---------------------------------
    // ONE per frame, round-robin. Tracing and blending every probe every frame is
    // what made this volume cost 0.65 ms where idTech 8 spends 0.08 for six
    // cascades; the fix there and here is the same — amortise.
    //
    // r_ddgi_sched 1: cascade 0 on every other frame, the rest round-robin over the
    // frames between. Cascade 0 is where the player stands and most of what they
    // see; it also scrolls most often. With 4 cascades: 0 every 2 frames (was 4),
    // 1..3 every 6 (was 4). Two cascades or fewer: plain round-robin either way.
    {
        const u64 t = s_updateTick++;
        if (ps_r_ddgi_sched != 0 && s_cascades > 2)
            s_cascThisFrame = (t & 1u) ? u32(1u + (t >> 1) % (s_cascades - 1)) : 0u;
        else
            s_cascThisFrame = u32(t % s_cascades);
    }

    // ---- window placement -------------------------------------------------
    // Snapped to whole cells in ABSOLUTE world grid coordinates: the volume must
    // move in lattice steps, or every probe would slide continuously and its
    // history would describe a place it is no longer at.
    //
    // ⚠ ONLY the cascade being updated moves. A cascade's base describes where its
    // ATLAS CONTENT is, not where the camera is; moving it on a frame that does not
    // re-measure it would re-point every one of its slots at a neighbour's position
    // — precisely the smear the toroidal addressing exists to prevent, just spread
    // over several frames instead of happening all at once.
    const float spacing0 = _max(0.25f, ps_r_ddgi_spacing);
    const Fvector& cam = RV().vCameraPosition;
    {
        const u32   k  = s_cascThisFrame;
        const float sp = spacing0 * float(1u << k);
        const s32 base[3] = {
            s32(std::floor(cam.x / sp)) - s32(s_dimXZ / 2),
            s32(std::floor(cam.y / sp)) - s32(s_dimY  / 2),
            s32(std::floor(cam.z / sp)) - s32(s_dimXZ / 2),
        };
        if (!s_haveBase[k]) {
            // First update (or after a wipe): declare the whole window new so every
            // probe takes its measurement whole instead of blending into garbage.
            s_prevBase[k][0] = base[0] + s32(s_dimXZ) * 4;
            s_prevBase[k][1] = base[1] + s32(s_dimY)  * 4;
            s_prevBase[k][2] = base[2] + s32(s_dimXZ) * 4;
            s_haveBase[k] = true;
        } else {
            for (u32 i = 0; i < 3; ++i) s_prevBase[k][i] = s_base[k][i];
        }
        for (u32 i = 0; i < 3; ++i) s_base[k][i] = base[i];
    }
    // A cascade that has not been measured yet holds nothing but the clear value.
    // Park its window absurdly far away so the sampler's in-window test fails for
    // every real world position and it contributes nothing — a plausible-looking
    // base (0,0,0 say) is a REAL place near the world origin, and would let those
    // probes be read as if they meant something.
    constexpr s32 kNoWindow = 1 << 24;
    for (u32 k = 0; k < s_cascades; ++k)
        if (!s_haveBase[k])
            for (u32 i = 0; i < 3; ++i) { s_base[k][i] = kNoWindow; s_prevBase[k][i] = kNoWindow; }

    // ---- probe relocation: the gate, and why toggling it wipes the field ----
    // ⚠⚠ THE ATLAS AND THE OFFSETS ARE ONE MEASUREMENT. Every tile was integrated
    // from wherever its probe stood, so switching relocation off does not restore
    // the old picture — it leaves the atlas describing displaced probes while every
    // reader places them back on the lattice, which is a state the renderer was
    // never in. The honest answer to a toggle is to wipe and re-measure: it costs
    // one cascade round (r_ddgi_cascades frames) and it makes an A/B mean something.
    // Same class of trap as "A/B by the gate" in the gather notes — closed on the
    // way in rather than diagnosed from a screenshot later.
    const bool relocOn = (ps_r_ddgi_reloc != 0) && s_offsRows > 0;
    if (s_relocWas != (relocOn ? 1 : 0)) {
        if (s_relocWas >= 0) {
            s_needClear = true;
            for (auto& h : s_haveBase) h = false;
            // Under r_ddgi_sched 1 the outer cascades come round every 2(n-1) frames.
            const u32 roundFrames = (ps_r_ddgi_sched != 0 && s_cascades > 2) ? 2u * (s_cascades - 1u) : s_cascades;
            Msg("[VK DDGI] probe relocation %s — field wiped, %u frames to re-measure every cascade",
                relocOn ? "ON" : "off", roundFrames);
        }
        s_relocWas = relocOn ? 1 : 0;
        s_relocPrints = 0; s_relocSamples = 0;
        s_relocSeen = s_relocBuried = s_relocMoved = s_relocOffMM = 0;
        s_relocBackHits = s_relocHits = 0;
    }

    // ---- constants --------------------------------------------------------
    Fvector  toSun; Fvector3 sunC, skyC;
    SunSky(toSun, sunC, skyC);

    Consts c{};
    for (u32 k = 0; k < kMaxCascades; ++k) {
        const u32 kk = _min(k, s_cascades - 1);   // unused slots mirror the last live one
        for (u32 i = 0; i < 3; ++i) {
            c.base[k][i]     = s_base[kk][i];
            c.prevBase[k][i] = s_prevBase[kk][i];
        }
    }
    c.dims[0] = s32(s_dimXZ); c.dims[1] = s32(s_dimY); c.dims[2] = s32(s_dimXZ);
    c.dims[3] = s32(s_rayStride);   // the run length per probe: [fixed | rotated]
    c.layout_[0] = s32(s_ppr);
    c.layout_[3] = s32(s_cascThisFrame);
    c.casc[0]    = s32(s_cascades);
    c.casc[1]    = s32(s_perCascade);
    // w was the reserved slot; it now carries the GI albedo boost x256 (r_rt_albedo).
    // Integer because widening the block would move every field after it.
    c.casc[3]    = s32(_max(0.25f, _min(3.f, ps_r_rt_albedo)) * 256.f + 0.5f);

    // ---- dynamic lights for the RT world ----------------------------------
    // Until this existed the probes knew the sun and the sky and nothing else, so a
    // torch, a campfire or an anomaly lit nothing that could bounce.
    //
    // ⚠ NOT the cluster grid: that binning is tied to the view frustum and a probe
    // ray leaves it immediately. This is the same nearest-first world-space list the
    // raster's UBO fallback uses, taken whole. idTech 8 needs a cascaded world grid
    // because it bins 4000 lights; the engine collects a few dozen, and a flat loop
    // beats any structure built to accelerate one.
    //
    // ⭐CollectFrame is idempotent per RV().frameIndex. This pass runs before
    // Pass_SunShadow, so DDGI now happens to be the first caller — same result, and
    // it means the probes see THIS frame's lights, not last frame's.
    {
        const u32 want = u32(_max(0, _min(int(kMaxRtLights), ps_r_ddgi_lights)));
        u32 n = 0, collected = 0;
        if (want > 0) {
            const auto& FL = Lights::CollectFrame(cam);
            collected = FL.count;
            n = _min(FL.count, want);
            if (n > 0)
                if (void* dst = s_lights[s_slot].Map()) {
                    memcpy(dst, FL.gpu, size_t(n) * sizeof(Lights::GpuLight));
                    s_lights[s_slot].Flush();
                    s_lights[s_slot].Unmap();
                }
        }
        c.casc[2] = s32(n);
        // Say it out loud when the number changes. Without this, "dynamic lights in
        // the GI" is unfalsifiable from a log: an outdoor daytime spawn collects
        // ZERO lights, so the feature working and the feature broken look identical.
        static u32 s_lastLit = ~0u;
        if (n != s_lastLit) {
            s_lastLit = n;
            Msg("[VK DDGI] dynamic lights in the RT world: %u shaded (of %u collected, cap %u)",
                n, collected, want);
        }
    }
    // ⭐THE PROBES INTEGRATE THE SAME SKY THE SURFACES DO. When vk_ibl has projected
    // the SH9 coefficients, a missed ray comes home with shIrradianceC(dir) — the
    // exact function skyAmbient() calls in every forward receiver — scaled by the
    // same kAmbientScale those receivers apply as L.sky_params.y. That is what lets
    // the lighting REPLACE its sky fill with the field (r_ddgi_light) instead of
    // adding to it: two estimates of the same quantity, agreeing on magnitude, so
    // the blend measures GI and not exposure. Until the projection lands, the flat
    // env hemi colour below stands in.
    const bool shLive   = (IBL::GetSHBuffer() != VK_NULL_HANDLE) && IBL::SHReady();
    const bool cubeLive = (IBL::GetSpecView() != VK_NULL_HANDLE) && IBL::Ready();
    c.layout_[1] = shLive   ? 1 : 0;
    c.layout_[2] = cubeLive ? 1 : 0;
    c.grid[0] = spacing0;   // cascade 0; cascade c is spacing0 << c

    // RTXGI-conformance switches and classification (see DDGIConstsBlock).
    c.fix[0] = ps_r_ddgi_hit_sky     != 0 ? 1 : 0;
    c.fix[1] = ps_r_ddgi_crush_tri   != 0 ? 1 : 0;
    c.fix[2] = s32(s_fixedRays);
    c.fix[3] = ps_r_ddgi_skip_buried != 0 ? 1 : 0;
    // Classification rides on the relocation pass (it writes the flag) and on the
    // fixed set (a flag decided on the rotated one would flicker); without both it
    // stays off rather than half-working.
    c.classify[0] = (ps_r_ddgi_classify != 0 && relocOn && s_fixedRays > 0) ? 1 : 0;
    c.classify[1] = _max(1, _min(64, ps_r_ddgi_classify_period));
    c.classify[2] = s32(s_cascUpdates[s_cascThisFrame]++ & 0x3FFFFFFFu);
    c.grid[1] = _max(0.f, _min(0.995f, ps_r_ddgi_hysteresis));
    c.grid[2] = ps_r_ddgi_norm_bias;
    c.grid[3] = _max(1.f, ps_r_ddgi_depth_sharp);
    RandomQuat(c.rayRot);
    c.sunDir[0] = toSun.x; c.sunDir[1] = toSun.y; c.sunDir[2] = toSun.z;

    // A probe ray must not outlive the bubble, for the same reason the preview's
    // primary ray must not: past the boundary a miss means "nothing built", and
    // the probe would record open sky where a wall stands.
    const float drift = cam.distance_to(RTScene::Anchor());
    const float reach = _max(1.f, RTScene::Coverage() - drift);
    c.sunDir[3] = reach;

    // ---- THE LADDER AGAINST THE WORLD IT TRACES ----------------------------
    // ⚠⚠ A CASCADE PAST THE BLAS BUBBLE CANNOT MEASURE ANYTHING. Its probes trace an
    // EMPTY structure, every ray misses, and the whole shell comes home as unoccluded
    // sky — a full fifth of the DDGI budget spent computing a constant, and every
    // other cascade refreshed a round slower for it. This is not a hypothetical: the
    // shipping ladder (5 cascades, 1 m, 16 probes across) reaches 128 m from the
    // camera while r_rtao_range builds 64 m of world, so cascade 4 is consulted only
    // where nothing is built.
    //
    // ⭐ SAY IT, do not leave it to be deduced. "Cover a bigger area" is the obvious
    // next move on a probe volume and the probe cvars are the obvious lever, but the
    // reach that decides the answer belongs to the RT scene, and nothing in the log
    // connected the two. The shell each cascade OWNS starts at the previous cascade's
    // half-extent (ddgiSampleIrradiance always takes the finest cascade that covers
    // the point), so cascade c is useful exactly while that inner boundary is still
    // inside the traced world.
    {
        const float half0 = 0.5f * float(s_dimXZ) * spacing0;   // cascade 0's half-extent
        const float world = RTScene::Coverage();                // bubble half-extent, drift aside
        // Where cascade k's OWN shell begins: the previous cascade's half-extent,
        // because ddgiSampleIrradiance always answers from the finest cascade that
        // covers the point. Cascade 0 starts at the camera.
        auto shellStart = [half0](u32 k) { return (k == 0) ? 0.f : half0 * float(1u << (k - 1)); };
        u32  useful = s_cascades;
        for (u32 k = 1; k < s_cascades; ++k)
            if (shellStart(k) >= world) { useful = k; break; }
        // ⚠ Silent until there IS a bubble. RTScene::Coverage() is 0 before the first
        // BLAS build lands, and a verdict computed against a 0 m world says every
        // cascade is wasted — printed once at init, where it is pure noise and
        // contradicts the line that follows seconds later.
        // ASCII only in the string itself: the log is written through a single-byte
        // codepage, and a glyph without a mapping comes out as noise.
        static u32   s_lastUseful = ~0u;
        static float s_lastWorld  = -1.f;
        if (world > 1.f && (useful != s_lastUseful || _abs(world - s_lastWorld) > 0.5f)) {
            s_lastUseful = useful; s_lastWorld = world;
            if (useful < s_cascades) {
                Msg("[VK DDGI] WASTED REACH: %u of %u cascades (index %u and up) are consulted only "
                    "past %.0f m, and the BLAS bubble ends at %.0f m (r_rtao_range) - out there "
                    "nothing is built, so they can only report unoccluded sky. Raise r_rtao_range, "
                    "or r_ddgi_cascades %u for a %.0f%% faster round-robin.",
                    s_cascades - useful, s_cascades, useful,
                    shellStart(useful), world,
                    // ...faster because every REMAINING cascade is re-measured every
                    // `useful` frames instead of every s_cascades.
                    useful, 100.f * (float(s_cascades) / float(useful) - 1.f));
            } else {
                // Every shell STARTS inside the world, but the outermost one can still
                // stick out past it — so report the covered fraction rather than a
                // flat "all good", which would read as a clean bill of health while a
                // third of the outer cascade traced nothing.
                const float outer = half0 * float(1u << (s_cascades - 1));
                Msg("[VK DDGI] ladder vs the traced world: %.0f m of the ladder's %.0f m reach has "
                    "geometry (BLAS bubble %.0f m, r_rtao_range; the outermost of %u shells spans "
                    "%.0f-%.0f m)",
                    _min(outer, world), outer, world, s_cascades,
                    shellStart(s_cascades - 1), outer);
            }
        }
    }

    c.sunColor[0] = sunC.x; c.sunColor[1] = sunC.y; c.sunColor[2] = sunC.z;
    c.sunColor[3] = EnvLight::AmbientScale();
    c.skyColor[0] = skyC.x; c.skyColor[1] = skyC.y; c.skyColor[2] = skyC.z;
    c.skyColor[3] = ps_r_ddgi_energy;
    // CASCADE 0's distance ceiling. The shaders scale it per cascade through
    // ddgiProbeDist() — a ceiling shorter than a cascade's own cell diagonal makes
    // the Chebyshev test reject the far corner of every cell that cascade owns, and
    // that is what capped how sparse the ladder could be made. 24 m is 24x cascade
    // 0's cell and unchanged for every cascade whose cell is under 6 m.
    c.params[0] = _min(ps_r_ddgi_probe_dist, reach);
    c.params[1] = ps_r_ddgi_view_bias;
    c.params[2] = ps_r_ddgi_clamp;
    c.params[3] = _max(0.f, _min(1.f, ps_r_ddgi_bounce));   // multi-bounce feedback (0 = single bounce)
    c.reloc[0]  = s32(s_offsRow0);
    c.reloc[1]  = s32(s_offsW);
    c.reloc[2]  = relocOn ? 1 : 0;
    // ⭐WHERE THE FOREST STARTS. Both structures are built over ONE soup with the
    // opaque triangles first, so a triangle index past this boundary IS a leaf card
    // — which is how the trace tells relocation that a hit is not a wall. Asked of
    // the BUBBLE (UsingRenderGeometry), not of r_rt_src: the cvar can be flipped
    // several frames before the structure is rebuilt, and a boundary that does not
    // match the soup in the BVH would mislabel every hit past it. 0 on the collision
    // path, where there are no cutouts at all.
    c.reloc[3]  = RTScene::UsingRenderGeometry()
                ? s32(_min(RTWorld::OpaqueTris(), RTScene::TriCount())) : 0;

    // ⭐WHERE the reflectance correction applies, which is the half r_rt_albedo was
    // missing. The diffuse maps of this game were authored for an engine with NO GI,
    // where a constant ambient was added on top, so they are deliberately dark. An
    // enclosure settles at inflow x 1/(1-albedo), so at ~0.25 the light that comes
    // through a window survives barely one bounce: the windowsill is lit and a metre
    // in there is nothing. r_rt_albedo corrects the number but GLOBALLY - raise it
    // enough for a room and the street blows out, which is why it has sat at 1.0.
    // The trace now measures whether a point has a CEILING within reach and applies
    // the boost only there, so open ground and forest (canopy far above the reach)
    // come out bit-identical.
    // ⭐⭐⭐HOW MUCH depends on the SUN, and that was the surprise. Measured on the
    // user two saves 24-08 with r_sky_sh_debug: the flat hemisphere ambient barely
    // moves between a clear afternoon and a dark night (0.0698 -> 0.0550, a factor
    // 1.27), while the sun goes 1.1519 -> 0.0000. So what a room loses after dark is
    // not "the sky got dimmer" - it is the ENTIRE sun contribution: the patch through
    // the window and everything the lit yard bounced back in. A boost calibrated by
    // eye at noon is therefore far too weak at night, which is exactly what the user
    // found: 1.6 reads right by day and 4 is wanted in the dark.
    // Proxy for outdoor illumination, linear: sun x sin(elevation) + sky.
    // ⚠CALIBRATED AGAINST THIS LINE OWN NUMBERS, NOT AGAINST r_sky_sh_debug. The two
    // are on different scales - that debug view prints the SH-projected ambient after
    // the sky scale, SunSky here hands over the raw linearised weather colours - and
    // the first cut of these constants was taken from the debug view, which put the
    // night mix at 0.60 instead of 1.00. Measured from the log this code prints:
    //   day   save (sun 58.8 deg): sun 0.843 x 0.86 + sky 0.378 = 1.10
    //   night save (sun  5.5 deg): sun 0.000 x 0.09 + sky 0.195 = 0.20   => 5.6x apart
    // ⚠⚠AND THESE ARE THE SETTLED READINGS. The first line this Msg prints after a
    // load is always "sun 1.000 x 1.00 + sky 1.000 = 2.00" - identical in BOTH saves,
    // because the weather has not been applied yet. Calibrating on it (which I did
    // once) puts the night mix at 0.25 instead of 1.00. Read the LAST line, not the
    // first. The transient itself is harmless: it lasts until the level environment
    // lands, which is before the player sees a frame.
    // Interpolated in LOG luminance because that range spans an order of magnitude
    // and a linear mix would sit at the night anchor for most of the evening.
    {
        const float sunLum = sunC.x * 0.2126f + sunC.y * 0.7152f + sunC.z * 0.0722f;
        const float skyLum = skyC.x * 0.2126f + skyC.y * 0.7152f + skyC.z * 0.0722f;
        const float lOut   = sunLum * _max(0.f, toSun.y) + skyLum;
        // The two calibration points above. Constants rather than cvars: they are a
        // property of this weather set, not a taste setting, and the two BOOSTS are
        // the knobs someone actually wants to turn.
        const float kDay = 1.10f, kNight = 0.195f;
        const float t = _max(0.f, _min(1.f, logf(kDay / _max(lOut, 1e-4f)) / logf(kDay / kNight)));
        const float bDay   = _max(1.f, _min(6.f, ps_r_gi_indoor));
        const float bNight = _max(1.f, _min(6.f, ps_r_gi_indoor_night));
        c.indoor[0] = bDay + (bNight - bDay) * t;
        s_indoorBoostNow = c.indoor[0];   // the pixel debugger reads it per dump

        // ⭐SAY IT OUT LOUD WHEN IT MOVES. The boost is now driven by the weather, so
        // "why is this room brighter than it was an hour ago" has to be answerable
        // without a rebuild - the same rule the GI weights line already follows.
        static float s_lastB = -1.f;
        if (_abs(c.indoor[0] - s_lastB) > 0.05f) {
            s_lastB = c.indoor[0];
            Msg("[VK GI] indoor boost %.2f (outdoor illum %.3f = sun %.3f x %.2f + sky %.3f, night mix %.2f)",
                c.indoor[0], lOut, sunLum, _max(0.f, toSun.y), skyLum, t);
        }
    }
    c.indoor[1] = _max(0.f, _min(16.f, ps_r_gi_indoor_dist));
    // The confidence gate (r_ddgi_conf) rides in the z of the same reserved tail
    // slot — no layout move, only the DDGI_SAMPLE consumers need their .spv fresh.
    c.indoor[2] = _max(0.f, _min(1.f, ps_r_ddgi_conf));
    // Adaptive hysteresis floor (r_ddgi_fast) — the last reserved slot. Clamped
    // under the running hysteresis so a "fast" floor can never SLOW a blend.
    c.indoor[3] = _max(0.f, _min(ps_r_ddgi_fast, c.grid[1]));
    // Edge-of-window energy ceiling (r_ddgi_edge_clamp) — see the struct comment.
    c.edge[0] = _max(0.f, ps_r_ddgi_edge_clamp);
    // The gather's own per-ray firefly ceiling (r_gather_clamp) rides in the next
    // reserved word of the same tail vec4 — no layout move, and a .spv that has not
    // been recompiled simply reads 0 and keeps inheriting params.z. ONE ray per
    // pixel cannot afford the ceiling a 64-ray probe is trimmed with; see the cvar.
    c.edge[1] = _max(0.f, ps_r_gather_clamp);
    // Screen-cache prefilter (r_gather_scache_blur) — the next reserved word. Same
    // contract again: an un-recompiled .spv reads 0 and keeps the single texel.
    c.edge[2] = (ps_r_gather_scache_blur != 0) ? 1.f : 0.f;
    // Extra rays on curved depth (r_gather_rays_edge) — the LAST word of the tail
    // vec4. When this one is spent the next feature appends a new vec4, same rule.
    c.edge[3] = float(_max(0, _min(7, ps_r_gather_rays_edge)));
    // Lazy lattice (r_gather_lazy) — a NEW tail vec4, so exe and .spv go in pairs.
    c.edge2[0] = (ps_r_gather_lazy != 0) ? 1.f : 0.f;
    c.edge2[1] = (ps_r_gather_wrc != 0) ? 1.f : 0.f;
    c.edge2[2] = (ps_r_gather_indoor_ray != 0) ? 1.f : 0.f;
    c.edge2[3] = 0.f;

    if (void* dst = s_consts[s_slot].Map()) {
        memcpy(dst, &c, sizeof(c));
        s_consts[s_slot].Flush();
        s_consts[s_slot].Unmap();
    } else {
        VK::Prof::ZoneEnd(cmd, zone);
        return;
    }

    // ---- first use: the atlases hold undefined memory ----------------------
    if (s_needClear) {
        // ⚠EXPLICIT stages, not the layout-derived overload. A layout transition IS
        // a write, and the derived form answers "compute storage read|write" for
        // GENERAL — which does not cover the transfer clear that follows, so the
        // clear raced the transition (SYNC-HAZARD-WRITE-AFTER-WRITE, caught 21-08).
        VK::ImageBarrier(cmd, s_irrImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VK::ImageBarrier(cmd, s_depImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero{};
        VkImageSubresourceRange rng{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, s_irrImg, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);
        vkCmdClearColorImage(cmd, s_depImg, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);
        VK::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        s_needClear = false;
    }

    // Everything above is upkeep the preview needs too. From here on it is the
    // volume proper, which only runs when the volume is on and has a bubble.
    if (!Wanted() || !RTScene::Ready()) { VK::Prof::ZoneEnd(cmd, zone); return; }

    const VkAccelerationStructureKHR tlas = RTScene::TLAS();
    const VkBuffer vertsBuf = RTScene::VertexBuffer();
    const VkBuffer matsBuf  = RTScene::MaterialBuffer();
    const VkBuffer albBuf   = RTScene::AlbedoBuffer();
    // The trace shader samples the diffuse array unconditionally (the choice
    // between texture and constant is per material, in the table), so an
    // unpublished set means no trace this frame rather than a duller one.
    const VkDescriptorSet texSet = RTWorld::TexSet();
    if (!tlas || !vertsBuf || !matsBuf || !albBuf || !texSet) { VK::Prof::ZoneEnd(cmd, zone); return; }

    // ---- descriptors ------------------------------------------------------
    const VkBuffer constsBuf = s_consts[s_slot].GetHandle();
    const VkBuffer raysBuf   = s_rays.GetHandle();
    const VkBuffer    shBuf   = shLive ? IBL::GetSHBuffer() : s_shDummy.GetHandle();
    const VkImageView cubeView = cubeLive ? IBL::GetSpecView() : s_skyFallback->GetView();
    const VkSampler   cubeSamp = cubeLive ? IBL::GetSampler()  : s_cubeSampler;

    // ---- world radiance cache ---------------------------------------------
    // HERE, and not in a pass of its own, for two reasons that both matter:
    //  * its shade dispatch reads the sun, the sky and the bounce feedback out of
    //    the constants uploaded a few lines above, so it cannot run before them;
    //  * it must run BEFORE the probe trace below, because the trace is one of the
    //    two things that fill its queue — and the queue is reset in there, at the
    //    end. Shading after the producers would answer this frame's requests with
    //    next frame's dispatch and add a frame of lag for nothing.
    // It also ensures the four buffers the trace set binds below exist at all.
    {
        WRC::ShadeInputs wi;
        wi.tlas       = tlas;
        wi.verts      = vertsBuf;
        wi.mats       = matsBuf;
        wi.albedo     = albBuf;
        wi.ddgiConsts = constsBuf;
        wi.skySH      = shBuf;
        wi.skyCube    = cubeView;
        wi.skyCubeSmp = cubeSamp;
        wi.irr        = s_irrView;
        wi.dep        = s_depView;
        wi.ddgiSmp    = s_sampler;
        wi.lights     = s_lights[s_slot].GetHandle();
        wi.texSet     = texSet;
        WRC::Update(cmd, wi);
    }
    const VkBuffer wrcConsts = WRC::ConstsBuffer();
    const VkBuffer wrcCells  = WRC::CellsBuffer();
    const VkBuffer wrcList   = WRC::ListBuffer();
    const VkBuffer wrcCtrl   = WRC::CtrlBuffer();
    // The trace shader declares all four unconditionally, so a null here is not a
    // duller frame, it is an invalid descriptor set. Skip the trace instead.
    if (!wrcConsts || !wrcCells || !wrcList || !wrcCtrl) { VK::Prof::ZoneEnd(cmd, zone); return; }
    {
        const VkBuffer hintsBuf = s_relocHints.GetHandle();
        BoundRT want{ tlas, vertsBuf, matsBuf, albBuf, constsBuf, raysBuf, shBuf,
                      cubeView, s_irrView, s_depView,
                      wrcConsts, wrcCells, wrcList, wrcCtrl, hintsBuf };
        if (memcmp(&s_boundTrace[s_slot], &want, sizeof(want)) != 0) {
            VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
            asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            asInfo.accelerationStructureCount = 1;
            asInfo.pAccelerationStructures    = &tlas;

            const VkDescriptorBufferInfo bufs[6] = {
                { vertsBuf, 0, VK_WHOLE_SIZE }, { matsBuf, 0, VK_WHOLE_SIZE }, { albBuf, 0, VK_WHOLE_SIZE },
                { constsBuf, 0, sizeof(Consts) }, { raysBuf, 0, VK_WHOLE_SIZE }, { shBuf, 0, VK_WHOLE_SIZE } };
            const VkDescriptorType types[6] = {
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };

            // The prefiltered probe is left in SHADER_READ_ONLY by vk_ibl, like every
            // other consumer of it (iblSpecular in the forward receivers).
            const VkDescriptorImageInfo cubeInfo{ cubeSamp, cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            // The atlases stay in GENERAL forever (storage write + sampled read), so
            // that is what the multi-bounce read has to declare too.
            const VkDescriptorImageInfo atlas[2] = {
                { s_sampler, s_irrView, VK_IMAGE_LAYOUT_GENERAL },
                { s_sampler, s_depView, VK_IMAGE_LAYOUT_GENERAL } };

            // The light SSBO handle is per-slot and stable for the module's life, so
            // it rides along with the geometry rebind rather than needing its own.
            const VkDescriptorBufferInfo lightInfo{ s_lights[s_slot].GetHandle(), 0, VK_WHOLE_SIZE };

            // 11..14: the world radiance cache. Owned by vk_wrc; bound here because
            // the probe ray is one of its two readers.
            const VkDescriptorBufferInfo wrcInfo[4] = {
                { wrcConsts, 0, VK_WHOLE_SIZE }, { wrcCells, 0, VK_WHOLE_SIZE },
                { wrcList,   0, VK_WHOLE_SIZE }, { wrcCtrl,  0, VK_WHOLE_SIZE } };
            const VkDescriptorType wrcTypes[4] = {
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };

            // 15: the per-ray relocation hints this dispatch also writes.
            const VkDescriptorBufferInfo hintInfo{ hintsBuf, 0, VK_WHOLE_SIZE };

            VkWriteDescriptorSet w[16]{};
            for (u32 i = 0; i < 16; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = s_traceSets[s_slot];
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
            }
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w[0].pNext = &asInfo;
            for (u32 i = 0; i < 6; ++i) { w[i + 1].descriptorType = types[i]; w[i + 1].pBufferInfo = &bufs[i]; }
            w[7].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[7].pImageInfo  = &cubeInfo;
            w[8].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[8].pImageInfo  = &atlas[0];
            w[9].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[9].pImageInfo  = &atlas[1];
            w[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         w[10].pBufferInfo = &lightInfo;
            for (u32 i = 0; i < 4; ++i) { w[i + 11].descriptorType = wrcTypes[i]; w[i + 11].pBufferInfo = &wrcInfo[i]; }
            w[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         w[15].pBufferInfo = &hintInfo;
            vkUpdateDescriptorSets(VulkanHW.m_Device, 16, w, 0, nullptr);
            s_boundTrace[s_slot] = want;
        }
    }
    for (u32 k = 0; k < 2; ++k) {
        const u32 idx = s_slot * 2 + k;
        const VkImageView img = k ? s_depView : s_irrView;
        BoundBlend want{ constsBuf, raysBuf, img, s_irrView };
        if (memcmp(&s_boundBlend[idx], &want, sizeof(want)) != 0) {
            const VkDescriptorBufferInfo cb{ constsBuf, 0, sizeof(Consts) };
            const VkDescriptorBufferInfo rb{ raysBuf, 0, VK_WHOLE_SIZE };
            const VkDescriptorImageInfo  ii{ VK_NULL_HANDLE, img, VK_IMAGE_LAYOUT_GENERAL };
            // Binding 3 is always the IRRADIANCE atlas: the relocation strip lives
            // there, and both blends need it to see the "probe moved" flag. For the
            // irradiance blend that is the same image as binding 2 — declared twice
            // rather than special-cased, so one descriptor writer covers both.
            const VkDescriptorImageInfo  oi{ VK_NULL_HANDLE, s_irrView, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[4]{};
            for (u32 i = 0; i < 4; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = s_blendSets[idx];
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
            }
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &cb;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &rb;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  w[2].pImageInfo  = &ii;
            w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  w[3].pImageInfo  = &oi;
            vkUpdateDescriptorSets(VulkanHW.m_Device, 4, w, 0, nullptr);
            s_boundBlend[idx] = want;
        }
    }
    {
        const VkBuffer statsBuf = s_relocStats[s_slot].GetHandle();
        const VkBuffer hintsBuf = s_relocHints.GetHandle();
        BoundReloc want{ constsBuf, raysBuf, statsBuf, hintsBuf, s_irrView };
        if (memcmp(&s_boundReloc[s_slot], &want, sizeof(want)) != 0) {
            const VkDescriptorBufferInfo cb{ constsBuf, 0, sizeof(Consts) };
            const VkDescriptorBufferInfo rb{ raysBuf, 0, VK_WHOLE_SIZE };
            const VkDescriptorImageInfo  ii{ VK_NULL_HANDLE, s_irrView, VK_IMAGE_LAYOUT_GENERAL };
            const VkDescriptorBufferInfo sb{ statsBuf, 0, VK_WHOLE_SIZE };
            const VkDescriptorBufferInfo hb{ hintsBuf, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet w[5]{};
            for (u32 i = 0; i < 5; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = s_relocSets[s_slot];
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
            }
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &cb;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &rb;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  w[2].pImageInfo  = &ii;
            w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &sb;
            w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &hb;
            vkUpdateDescriptorSets(VulkanHW.m_Device, 5, w, 0, nullptr);
            s_boundReloc[s_slot] = want;
        }
    }

    // ---- trace ------------------------------------------------------------
    {
        const int z = VK::Prof::ZoneBegin(cmd, "DDGI/Trace");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_tracePipe);
        const VkDescriptorSet sets[2] = { s_traceSets[s_slot], texSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_tracePipeLayout, 0, 2,
                                sets, 0, nullptr);
        const u32 total  = s_perCascade * s_rayStride;   // one cascade per frame, fixed rays included
        vkCmdDispatch(cmd, (total + 63) / 64, 1, 1);
        VK::Prof::ZoneEnd(cmd, z);
    }
    VK::ComputeBarrier(cmd);

    // ---- blend ------------------------------------------------------------
    // The two blends are independent (different atlases, same ray buffer read-only),
    // so they need no barrier between them — only one after, for the consumers.
    {
        const int z = VK::Prof::ZoneBegin(cmd, "DDGI/Blend");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_blendIrrPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_blendPipeLayout, 0, 1,
                                &s_blendSets[s_slot * 2 + 0], 0, nullptr);
        vkCmdDispatch(cmd, s_perCascade, 1, 1);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_blendDepPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_blendPipeLayout, 0, 1,
                                &s_blendSets[s_slot * 2 + 1], 0, nullptr);
        vkCmdDispatch(cmd, s_perCascade, 1, 1);
        VK::Prof::ZoneEnd(cmd, z);
    }

    // ---- probe relocation --------------------------------------------------
    // AFTER the blends, and the ordering IS the contract: the tiles they just wrote
    // were integrated from where each probe stands now, so a move decided here
    // belongs to the NEXT update of this cascade. What this pass leaves behind is
    // the offset the next trace departs from plus the flag telling the next blend
    // that its history describes the old place.
    if (relocOn) {
        // The blends READ the strip this pass WRITES, and the irradiance blend also
        // writes the same image (different texels — but sync validation tracks the
        // subresource, not the rectangle).
        VK::ComputeBarrier(cmd);
        const int z = VK::Prof::ZoneBegin(cmd, "DDGI/Reloc");

        // ⭐THE PASS AUDITS ITSELF, and the window closes on its own. A feature whose
        // entire subject is geometry nobody can see is unfalsifiable from a log
        // otherwise: "relocation is on" and "relocation moves nothing" look the
        // same. The numbers ride a per-slot buffer whose frame has already retired,
        // which is why they can be mapped straight from the CPU here.
        // The window closes on its own after 15 prints — EXCEPT while the GI pixel
        // debugger is up: whoever is reading [VK GIprobe] blocks wants the probe
        // census flowing next to them, not a window that expired ten minutes ago.
        const bool auditing = (s_relocPrints < 15) || (ps_r_gi_probe != 0);
        if (auditing) {
            if (void* p = s_relocStats[s_slot].Map()) {
                const u32* st = static_cast<const u32*>(p);
                s_relocSeen     += st[0]; s_relocBuried   += st[1];
                s_relocMoved    += st[2]; s_relocOffMM    += st[3];
                s_relocBackHits += st[4]; s_relocHits     += st[5];
                s_relocDownHits += st[6]; s_relocDownBack += st[7];
                s_relocLocal    += st[8];  s_relocLocalBuried += st[9];
                s_relocLocalMoved += st[10]; s_relocLocalOffMM += st[11];
                s_relocLocalHits  += st[12];
                s_relocLocalIrrM  += st[13]; s_relocAllIrrM += st[14];
                s_relocInactive   += st[15];
                memset(p, 0, 16 * sizeof(u32));
                s_relocStats[s_slot].Flush();
                s_relocStats[s_slot].Unmap();
                ++s_relocSamples; ++s_relocSamplesForLocal;
            }
            if (s_relocSamples >= 60 && s_relocSeen > 0) {
                const double seen = double(s_relocSeen);
                Msg("[VK DDGI] relocation: %.1f%% of probes buried, %.2f%% moved | mean offset %.2f m "
                    "(cap %.2f at cascade 0) | backface %.1f%% of solid hits, %.1f%% of DOWNWARD ones "
                    "(down should be ~0%% with sane winding; ~50%% = the sign carries nothing)",
                    100.0 * double(s_relocBuried) / seen,
                    100.0 * double(s_relocMoved) / seen,
                    double(s_relocOffMM) / seen / 1000.0,
                    _max(0.f, _min(0.49f, ps_r_ddgi_reloc_max)) * spacing0,
                    (s_relocHits > 0) ? 100.0 * double(s_relocBackHits) / double(s_relocHits) : 0.0,
                    (s_relocDownHits > 0) ? 100.0 * double(s_relocDownBack) / double(s_relocDownHits) : 0.0);
                if (ps_r_ddgi_classify != 0)
                    Msg("[VK DDGI]   classification: %.1f%% of probes inactive (no geometry in their cell box), "
                        "refreshed every %d updates",
                        100.0 * double(s_relocInactive) / seen, _max(1, _min(64, ps_r_ddgi_classify_period)));
                // The local line is the one to read while standing somewhere: counts,
                // not shares, and averaged per dispatch so "3 probes" means three
                // probes near you rather than three summed over 60 frames.
                if (s_relocLocal > 0) {
                    const double n = double(_max(1u, s_relocSamplesForLocal));
                    // ⭐The census pair: local mean vs cascade mean. Near 1.0 while
                    // standing in a building = the interior probes shine like the
                    // street (leak or over-boost); a healthy interior sits well
                    // below its cascade.
                    const double localLum = double(s_relocLocalIrrM) / double(s_relocLocal) / 1000.0;
                    const double cascLum  = (s_relocSeen > 0) ? double(s_relocAllIrrM) / double(s_relocSeen) / 1000.0 : 0.0;
                    Msg("[VK DDGI]   within 12 m of the camera: %.0f probes, %.1f buried, %.2f moved "
                        "| mean offset %.2f m | mean probe lum %.3f vs cascade %.3f (ratio %.2f)",
                        double(s_relocLocal) / n, double(s_relocLocalBuried) / n,
                        double(s_relocLocalMoved) / n,
                        double(s_relocLocalOffMM) / double(s_relocLocal) / 1000.0,
                        localLum, cascLum, (cascLum > 1e-6) ? localLum / cascLum : 0.0);
                } else {
                    Msg("[VK DDGI]   within 12 m of the camera: NO probes of this cascade at all "
                        "(the spacing puts its lattice elsewhere)");
                }
                // ⭐⭐⭐ THE SHARE OF RAYS THAT HIT ANYTHING SOLID, AROUND THE PLAYER,
                // and it is the first number to read when a room looks evenly lit. A
                // probe standing in a wooden room must stop most of its rays within a
                // couple of metres; a few percent means the ROOM IS NOT IN THE BVH and
                // every probe is reading open sky through the walls — which looks
                // exactly like "GI does nothing indoors" while being no GI bug at all,
                // and which would also explain a 0% buried count (no geometry, nothing
                // to be buried in).
                // ⚠ LOCAL, not the cascade average: the cascade spans tens of metres
                // of outdoors around this room, and its average answers a question
                // nobody asked.
                if (s_relocLocal > 0) {
                    Msg("[VK DDGI]   ...and %.1f%% of THEIR rays stopped on solid geometry "
                        "(a room should be high; a few %% = the walls are missing from the RT world)",
                        100.0 * double(s_relocLocalHits)
                              / (double(s_relocLocal) * double(_max(1u, s_fixedRays ? s_fixedRays : s_rayCount))));
                }
                ++s_relocPrints;
                s_relocSamples = 0;
                s_relocSeen = s_relocBuried = s_relocMoved = s_relocOffMM = 0;
                s_relocBackHits = s_relocHits = 0;
                s_relocDownHits = s_relocDownBack = 0;
                s_relocLocal = s_relocLocalBuried = s_relocLocalMoved = s_relocLocalOffMM = 0;
                s_relocLocalHits = 0;
                s_relocLocalIrrM = s_relocAllIrrM = 0;
                s_relocInactive = 0;
                s_relocSamplesForLocal = 0;
            }
        }

        RelocPush push{};
        push.nearK    = _max(0.02f, _min(0.5f,  ps_r_ddgi_reloc_near));
        push.frac     = _max(0.05f, _min(1.f,   ps_r_ddgi_reloc_frac));
        push.stepK    = _max(0.01f, _min(0.45f, ps_r_ddgi_reloc_step));
        push.maxK     = _max(0.f,   _min(0.49f, ps_r_ddgi_reloc_max));
        push.backFrac = _max(0.f,   _min(1.f,   ps_r_ddgi_reloc_back));
        push.decay    = _max(0.f,   _min(1.f,   ps_r_ddgi_reloc_decay));
        push.audit    = auditing ? 1 : 0;
        // The probes AROUND THE PLAYER get counted separately. A share over 2048
        // probes cannot see one room, and a room is exactly where this feature is
        // supposed to matter — 0.0% was printed for a cascade in which two or three
        // probes were the entire question.
        push.camX     = cam.x; push.camY = cam.y; push.camZ = cam.z;
        push.nearR    = 12.f;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_relocPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_relocPipeLayout, 0, 1,
                                &s_relocSets[s_slot], 0, nullptr);
        vkCmdPushConstants(cmd, s_relocPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (s_perCascade + 63) / 64, 1, 1);
        VK::Prof::ZoneEnd(cmd, z);
    }

    // Storage writes -> sampled reads, both in GENERAL. The layout does not change,
    // so the derivation-from-layout barrier would be wrong here; state it.
    //
    // ⚠ STORAGE_WRITE is in the DESTINATION mask too, and that is about the world
    // radiance cache rather than the atlases: the probe trace above and the final
    // gather later this frame BOTH write the same cell table. Without it the two
    // dispatches are a write-after-write on one buffer with nothing between them —
    // exactly the class of hazard sync validation was turned on to find.
    VK::MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
        | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    VK::Prof::ZoneEnd(cmd, zone);
}

}}  // namespace VK::DDGI

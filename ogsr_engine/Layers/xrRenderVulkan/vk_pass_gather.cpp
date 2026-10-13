// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — final gather (step 4 of the RT arc). See vk_pass_gather.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_pass_gather.h"
#include "vk_dlss.h"              // VK::Dlss::RayReconEnabled — RR owns the denoise when it runs
#include "vk_rt_scene.h"        // the camera-local BVH the rays walk
#include "vk_rt_world.h"        // set 1 = the RT diffuse array (per-texel albedo + cutout alpha)
#include "vk_ddgi.h"            // the probe field = the LAST fallback in the chain
#include "vk_wrc.h"             // ...and the world radiance cache, the link in front of it
#include "vk_descriptors.h"
#include "vk_shaders.h"
#include "vk_compute_util.h"
#include "vk_image.h"
#include "vk_barriers.h"
#include "vk_fullscreen.h"      // debug view: shared fullscreen pipeline skeleton
#include "vk_rendering.h"       // RenderingBuilder (the debug draw LOADs the scene colour)
#include "vk_scene_color.h"     // SceneColor::Format
#include "vk_motionvec.h"       // temporal reprojection of the gather history
#include "vk_texture.h"          // the blue-noise tile is a tiny CVulkanTexture
#include "vk_gather_bluenoise.h" // GENERATED (_tools/gen_bluenoise.js): 64x64 RG tile
#include "vk_command_buffer.h"
#include "vk_profiler.h"
#include "vk_pass_context.h"    // FrameContext (the debug view draws into the scene colour)
#include "HW_Vulkan.h"

#include "../../xr_3da/device.h"

extern int   ps_r_gather;
extern int   ps_r_gather_rays;
extern float ps_r_gather_len;
extern float ps_r_gather_dist;
extern float ps_r_gather_bias;
extern int   ps_r_gather_scale;
extern int   ps_r_gather_bn;     // r_gather_bn - blue-noise tile vs the old IGN + white hash
extern float ps_r_gather_temporal;
extern int   ps_r_gather_radius;
extern float ps_r_gather_hitgate;   // scales the hit-distance gate (1 = as shipped, 0 = off)
extern float ps_r_gather_plane;     // plane-aware depth weight (0 = as shipped)
extern float ps_r_gather_adapt;     // strength of the progressive alpha ramp (1 = full, 0 = the fixed weight)
extern float ps_r_gather_ffreject;  // k of the resolve's spatial sigma clip (0 = off)
extern float ps_r_gather_nrmw;      // exponent of the resolve's normal weight (0 = off)
extern int   ps_r_gather_lazy;        // skip the lattice walk where its blend weight is zero
extern int   ps_r_gather_wrc;         // the world-cache query INSIDE the gather (profiler switch)
extern int   ps_r_gather_indoor_ray;  // the second trace per pixel, the ceiling test (profiler switch)
extern int   ps_r_gather_profile;     // ray-cost profiler: 1 = one run, 2 = loop
extern float ps_r_gather_disocc;    // reprojection depth tolerance, RELATIVE (0 = test off)
extern int   ps_r_dlss_rr_raw;   // r_dlss_rr_raw — hand RR the raw rays instead of our filtered ones
extern int   ps_r_gather_foliage;
extern int   ps_r_gather_atest;       // how the cutout alpha test is answered — 0 exact, 1 solid, 2 probability, 3 coarse-mip coverage
extern float ps_r_gather_atest_cov;   // mode 2's accept probability
extern float ps_r_gather_atest_lod;   // mip floor for the exact test / mode 3's coverage read
extern float ps_r_gather_atest_dist;  // hits nearer than this keep the exact test
extern int   ps_r_gather_sh;     // r_gather_sh — publish the 2-band SH alongside the radiance
extern float ps_r_gather_sh_temporal;   // its own, longer history: direction moves slower than brightness
extern float ps_r_gather_light;
extern int   ps_r_gather_debug;
extern float ps_r_gather_scache;      // screen-space radiance cache: how much of a hit comes from last frame's screen
extern float ps_r_gather_scache_tol;  // its depth test, RELATIVE to distance (0.04 = 4%)
extern float ps_r_gather_scache_indoor; // enclosedness weights the cache OUT (it bypasses the reflectance compensation)
extern float ps_r_gather_field_clamp;   // world-cache ceiling on the probe fallback: k x the cell's own shaded radiance (0 = off)

namespace VK { namespace GatherPass {

namespace {

constexpr u32      kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
constexpr VkFormat kFormat         = VK_FORMAT_R16G16B16A16_SFLOAT;   // rgb = irradiance, a = mean hit distance
// Linear view Z of the gather's own pixels, kept for ONE frame so the next frame's
// hits can be tested for visibility against it. R32F and not R16F on purpose: the
// test is a RELATIVE comparison against distances that run to 60 m, and half-float
// steps of 0.06 m out there would start rejecting hits that are genuinely visible.
constexpr VkFormat kDepthFormat    = VK_FORMAT_R32_SFLOAT;
// The 2-band SH's L1: one deviation vector per colour channel, and the three of
// them stacked vertically inside ONE RGBA16F image. Three separate images would
// have cost nine extra bindings across the trace, resolve and receiver sets (and
// a placeholder for each while the feature is off); one tiled image costs three.
constexpr u32      kSHTiles        = 3;

bool s_inited = false;
bool s_failed = false;

// ---- targets ---------------------------------------------------------------
// All three live permanently in GENERAL: written as storage images by the two
// dispatches, sampled by the resolve, the debug view and the forward receivers.
// A per-frame layout round trip would buy nothing (the DDGI atlases take the
// same deal for the same reason).
VkImage       s_rawImg = VK_NULL_HANDLE;   VmaAllocation s_rawAlloc = nullptr;   VkImageView s_rawView = VK_NULL_HANDLE;
VkImage       s_resImg[2] = {};            VmaAllocation s_resAlloc[2] = {};     VkImageView s_resView[2] = {};
// The cache's own history pair: [parity] is written this frame, [parity^1] holds
// the previous frame's Z and is what the lookup tests against.
VkImage       s_dpImg[2] = {};             VmaAllocation s_dpAlloc[2] = {};      VkImageView s_dpView[2] = {};
// The SH pair, in lockstep with the radiance above (raw + the same ping-pong).
// ⚠ 1x1 while r_gather_sh is off: a descriptor that is statically used by the
// shader has to point at a real image either way, and sizing it down is cheaper
// in code than a second binding path — at the price of a rebuild when the cvar
// flips, which is the same deal r_gather_scale already takes.
VkImage       s_shRawImg = VK_NULL_HANDLE; VmaAllocation s_shRawAlloc = nullptr;  VkImageView s_shRawView = VK_NULL_HANDLE;
VkImage       s_shResImg[2] = {};          VmaAllocation s_shResAlloc[2] = {};    VkImageView s_shResView[2] = {};
// ACCUMULATED FRAME COUNT per gather texel, on the same ping-pong as the radiance
// it describes. This is what turns the resolve's fixed EMA alpha into a progressive
// mean for young pixels — see the header of gather_resolve.comp.glsl. R32F rather
// than a byte format because STORAGE_IMAGE support for R8 is optional in Vulkan
// while R32_SFLOAT is mandatory, and the pair costs 4 MB at 1080p half-res.
VkImage       s_ageImg[2] = {};            VmaAllocation s_ageAlloc[2] = {};      VkImageView s_ageView[2] = {};
bool          s_shOn     = false;     // what the targets were BUILT for, not what the cvar says now
VkSampler     s_sampler  = VK_NULL_HANDLE;   // linear/clamp — the resolve and the receivers upsample through it
VkSampler     s_sampNear = VK_NULL_HANDLE;   // nearest/clamp — depth and the normal G-buffer are POINT data
VkExtent2D    s_extent  = {};
VkExtent2D    s_srcExtent = {};   // the scene extent the targets were sized from
u32           s_scale   = 0;      // the r_gather_scale the targets were sized with
u32           s_gen     = 0;
bool          s_needClear  = true;
bool          s_hasResult  = false;   // a frame has actually written s_resImg[s_cur]
bool          s_histValid  = false;   // ...and the OTHER one holds a usable history
u32           s_cur        = 0;       // which resolved image THIS frame writes
u32           s_frame      = ~0u;
u32           s_mvWarm     = 0;       // motion vectors are one frame stale here; warm up like SSAO does

// ---- screen-space radiance cache ------------------------------------------
// The matrix that rendered the frame s_dpImg[parity^1] and the prev-colour buffer
// belong to, plus WHICH frame that was.
//
// ⚠ THE FRAME NUMBER IS NOT BOOKKEEPING, IT IS THE GUARD. The cache is only valid
// if the history is EXACTLY one frame old: after a pause, a level load, a console
// toggle or any frame this pass sat out, the buffers still hold a plausible-looking
// image belonging to a different camera — and reprojecting into it would read
// confident, wrong radiance instead of falling back to the probes.
Fmatrix       s_prevVP;
u32           s_prevVPframe = 0;
bool          s_prevVPvalid = false;
// The camera that rendered the RESOLVED history, kept for the resolve's
// reprojection test: forward advance tells it where a static surface stood last
// frame, and the turn angle is what the depth tolerance has to absorb so a flick
// of the mouse does not read as "every pixel is a stranger".
Fvector       s_prevCamPos = {};
Fvector       s_prevCamDir = {};

// ---- pipelines -------------------------------------------------------------
VkDescriptorSetLayout s_gatherSetLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_gatherPool      = VK_NULL_HANDLE;
VkDescriptorSet       s_gatherSets[kFramesInFlight] = {};
VkPipelineLayout      s_gatherPipeLayout = VK_NULL_HANDLE;
VkPipeline            s_gatherPipe = VK_NULL_HANDLE;

VkDescriptorSetLayout s_resolveSetLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_resolvePool      = VK_NULL_HANDLE;
VkDescriptorSet       s_resolveSets[kFramesInFlight * 2] = {};   // [slot*2 + parity]
VkPipelineLayout      s_resolvePipeLayout = VK_NULL_HANDLE;
VkPipeline            s_resolvePipe = VK_NULL_HANDLE;

VkDescriptorSetLayout s_dbgSetLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_dbgPool      = VK_NULL_HANDLE;
VkDescriptorSet       s_dbgSets[2]   = {};                       // [parity]
VkPipelineLayout      s_dbgPipeLayout = VK_NULL_HANDLE;
VkPipeline            s_dbgPipe = VK_NULL_HANDLE;

// What each set currently points at. A bubble rebuild swaps the TLAS AND both
// geometry buffers at once, and a set an in-flight command buffer still
// references must not be rewritten under it — so rewrite only on a real change
// (the retire stamps in vk_rt_scene are what keep the OLD buffers alive long
// enough for that to be safe). Same rule as the RTAO / RTGI / DDGI passes.
struct BoundGather {
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkBuffer    verts = VK_NULL_HANDLE, mats = VK_NULL_HANDLE, alb = VK_NULL_HANDLE;
    VkBuffer    consts = VK_NULL_HANDLE, sh = VK_NULL_HANDLE;
    VkImageView irr = VK_NULL_HANDLE, dep = VK_NULL_HANDLE, cube = VK_NULL_HANDLE;
    VkImageView depth = VK_NULL_HANDLE, normal = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
    VkImageView outSH = VK_NULL_HANDLE;
    VkImageView prevColor = VK_NULL_HANDLE, prevDepth = VK_NULL_HANDLE, outDepth = VK_NULL_HANDLE;
    VkBuffer    wrcConsts = VK_NULL_HANDLE, wrcCells = VK_NULL_HANDLE,
                wrcList = VK_NULL_HANDLE, wrcCtrl = VK_NULL_HANDLE;
};
BoundGather s_boundGather[kFramesInFlight] = {};

// ---- BLUE NOISE (r_gather_bn) ----------------------------------------------
// 64x64 two-channel void-and-cluster tile, uploaded once and never touched
// again. It is NOT tracked in BoundGather on purpose: the view is fixed for the
// life of the pass, so it can never be the thing that makes a set stale.
//
// WHY A TILE AND NOT ANOTHER HASH. This pass draws ONE cosine ray per pixel, and
// its two sampling dimensions decide where that ray goes. Azimuth was on
// interleaved-gradient noise (good) and ELEVATION on a plain hash (white) - and
// elevation is the axis that decides "sky or geometry", which is nearly all of
// the variance. White noise there leaves LOW-FREQUENCY clumps, and a clump is
// exactly what neither a 5x5 bilateral nor a temporal filter nor DLSS can
// remove: they all attack high frequencies. Blue noise does not make one ray
// more accurate - it moves the SAME error into the band the rest of the chain
// is built to delete.
CVulkanTexture* s_blueNoise = nullptr;

struct BoundResolve {
    VkImageView depth = VK_NULL_HANDLE, raw = VK_NULL_HANDLE, hist = VK_NULL_HANDLE;
    VkImageView mv = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
    VkImageView rawSH = VK_NULL_HANDLE, histSH = VK_NULL_HANDLE, outSH = VK_NULL_HANDLE;
    VkImageView histAge = VK_NULL_HANDLE, outAge = VK_NULL_HANDLE;
    VkImageView normal = VK_NULL_HANDLE;   // r_gather_nrmw — the denoiser's normal weight
};
BoundResolve s_boundResolve[kFramesInFlight * 2] = {};
VkImageView  s_boundDbg[2] = {};

// ⚠ Mirrors the PC block in gather.comp.glsl EXACTLY.
struct GatherPush {
    float camDir[4];      // xyz = forward (unit), w = gather ray length (m)
    float camRightT[4];
    float camTopT[4];
    float zp[4];          // x = proj _33, y = proj _43, z = rays, w = normal bias (m)
    float res[4];         // xy = target size, zw = 1 / size
    float camPos[4];      // xyz = camera world position, w = view-distance cutoff (m)
    float params[4];      // x = jitter phase, y = TLAS cull mask, z = cache weight, w = cache depth tolerance
    float prevVP[16];     // previous frame's view-proj, row-major Fmatrix copied verbatim (same deal as motion_vec.frag)
    float sh[4];          // x = write the 2-band SH (r_gather_sh); y = blue noise; z = scache-by-enclosedness; w = the field's world-cache ceiling
    float atest[4];       // the cutout alpha-test mode (r_gather_atest) — goes straight into rtAtest, see rt_shade.glsl
};
static_assert(sizeof(GatherPush) == 208, "must match gather.comp's PC block");

struct ResolvePush {
    float zp[4];          // x = proj _33, y = proj _43, z = the SH channel is live, w reserved
    float res[4];
    float temporal[4];    // x = EMA alpha, y = MV valid, z = history valid, w = spatial radius
    float weights[4];     // x = hit-distance gate strength, y = plane-aware depth weight (see the shader),
                          // z = reprojection depth tolerance (0 = off), w = camera forward advance (m)
    float hist[4];        // x = progressive alpha on, y = the count it saturates at
};
static_assert(sizeof(ResolvePush) == 80, "must match gather_resolve.comp's PC block");

struct DebugPush {
    float params[4];      // x = mode, y = 1/screenW, z = 1/screenH, w = ray length (m)
    float age[4];         // x = the count mode 6 normalises against, 1/(1-alpha)
};

void DestroyTargets()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_rawView) { vkDestroyImageView(VulkanHW.m_Device, s_rawView, nullptr); s_rawView = VK_NULL_HANDLE; }
    if (s_rawImg)  { vmaDestroyImage(VulkanHW.m_Allocator, s_rawImg, s_rawAlloc); s_rawImg = VK_NULL_HANDLE; s_rawAlloc = nullptr; }
    if (s_shRawView) { vkDestroyImageView(VulkanHW.m_Device, s_shRawView, nullptr); s_shRawView = VK_NULL_HANDLE; }
    if (s_shRawImg)  { vmaDestroyImage(VulkanHW.m_Allocator, s_shRawImg, s_shRawAlloc); s_shRawImg = VK_NULL_HANDLE; s_shRawAlloc = nullptr; }
    for (u32 i = 0; i < 2; ++i) {
        if (s_shResView[i]) { vkDestroyImageView(VulkanHW.m_Device, s_shResView[i], nullptr); s_shResView[i] = VK_NULL_HANDLE; }
        if (s_shResImg[i])  { vmaDestroyImage(VulkanHW.m_Allocator, s_shResImg[i], s_shResAlloc[i]); s_shResImg[i] = VK_NULL_HANDLE; s_shResAlloc[i] = nullptr; }
        if (s_resView[i]) { vkDestroyImageView(VulkanHW.m_Device, s_resView[i], nullptr); s_resView[i] = VK_NULL_HANDLE; }
        if (s_resImg[i])  { vmaDestroyImage(VulkanHW.m_Allocator, s_resImg[i], s_resAlloc[i]); s_resImg[i] = VK_NULL_HANDLE; s_resAlloc[i] = nullptr; }
        if (s_dpView[i])  { vkDestroyImageView(VulkanHW.m_Device, s_dpView[i], nullptr); s_dpView[i] = VK_NULL_HANDLE; }
        if (s_dpImg[i])   { vmaDestroyImage(VulkanHW.m_Allocator, s_dpImg[i], s_dpAlloc[i]); s_dpImg[i] = VK_NULL_HANDLE; s_dpAlloc[i] = nullptr; }
        if (s_ageView[i]) { vkDestroyImageView(VulkanHW.m_Device, s_ageView[i], nullptr); s_ageView[i] = VK_NULL_HANDLE; }
        if (s_ageImg[i])  { vmaDestroyImage(VulkanHW.m_Allocator, s_ageImg[i], s_ageAlloc[i]); s_ageImg[i] = VK_NULL_HANDLE; s_ageAlloc[i] = nullptr; }
    }
    s_prevVPvalid = false;   // the Z history just went away with the targets
    s_extent = {}; s_srcExtent = {}; s_scale = 0; s_shOn = false;
    s_needClear = true; s_hasResult = false; s_histValid = false;
    for (auto& b : s_boundGather)  b = BoundGather{};
    for (auto& b : s_boundResolve) b = BoundResolve{};
    for (auto& v : s_boundDbg)     v = VK_NULL_HANDLE;
    ++s_gen;
}

}   // anon namespace

// ONE KNOB, and this is the definition the OTHER gates defer to (vk_ddgi,
// vk_rt_scene): asking for the gathered answer IS asking for the pass that
// produces it, so r_gather_light alone means the same thing as r_gather 1.
bool Wanted() { return ps_r_gather != 0 || ps_r_gather_light > 0.f; }

// vk_pass_ssao asks this before deciding whether to keep filling the prev-colour
// history. Deliberately does NOT require Enabled(): the capture happens at the end
// of the frame, and demanding a live bubble here would mean the first frame the
// gather comes up finds an empty history and the cache stays dark for one frame
// longer than it has to.
bool WantsPrevColor() { return Wanted() && ps_r_gather_scache > 0.f; }

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;

    if (!RTScene::Init()) { s_failed = true; return false; }   // no ray query on this device
    if (!g_ShaderManager) { s_failed = true; return false; }

    // Set 1 is the world's diffuse array, owned by vk_rt_world and shared with the
    // probe trace and the RT preview: one publication of the table, one place a
    // material slot can be wrong.
    const VkDescriptorSetLayout texLayout = RTWorld::TexSetLayout();
    if (texLayout == VK_NULL_HANDLE) {
        Msg("![VK Gather] no RT diffuse array (descriptor indexing?) — final gather disabled");
        s_failed = true; return false;
    }

    // 0 TLAS, 1 verts, 2 per-tri attributes, 3 albedo table, 4 DDGI constants,
    // 5/6 the probe atlases (the radiance cache a hit reads), 7 sky SH9, 8 sky
    // cube, 9 scene depth, 10 normal G-buffer, 11 the raw gather target,
    // 12 last frame's colour + 13 last frame's Z (the screen-space cache),
    // 14 this frame's Z (which becomes 13 next frame),
    // 15..18 the WORLD radiance cache (vk_wrc) — cache #2, between the two,
    // 19 the 2-band SH's L1 (r_gather_sh; 1x1 and unwritten while that is off),
    // 20 the blue-noise tile (r_gather_bn) - constant for the life of the pass.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
                                kFramesInFlight, s_gatherSetLayout, s_gatherPool, s_gatherSets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "Gather")) {
        s_failed = true; return false;
    }
    // 0 depth, 1 raw gather, 2 history, 3 motion vectors, 4 resolved target,
    // 5/6/7 the same three for the SH's L1 (raw, history, target),
    // 8/9 the accumulated frame count (previous / this frame) that drives the
    // progressive alpha — see the header of gather_resolve.comp.glsl.
    // 10 the NORMAL G-buffer (r_gather_nrmw): the spatial weight idTech 8's denoiser
    // has and ours did not. Same image and the same NEAREST sampler the trace reads
    // it through, so the resolve weights by the very normal each ray started from.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
                                kFramesInFlight * 2, s_resolveSetLayout, s_resolvePool, s_resolveSets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "GatherResolve")) {
        s_failed = true; return false;
    }
    // 0 resolved, 1 raw, 2 the SH's L1, 3 the accumulated frame count (mode 6).
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
                                2, s_dbgSetLayout, s_dbgPool, s_dbgSets,
                                VK_SHADER_STAGE_FRAGMENT_BIT, "GatherDbg")) {
        s_failed = true; return false;
    }

    // ⚠ 192 B is past the 128 B every Vulkan device is REQUIRED to offer (the
    // motion-vector pass already spends 144 for the same reason: a mat4 costs 64).
    // Desktop drivers give 256, but say so out loud rather than letting
    // vkCreatePipelineLayout fail with a validation message nobody reads.
    if (sizeof(GatherPush) > VulkanHW.Caps.maxPushConstantsSize) {
        Msg("![VK Gather] GatherPush %u B exceeds device maxPushConstantsSize %u B — final gather disabled",
            u32(sizeof(GatherPush)), VulkanHW.Caps.maxPushConstantsSize);
        s_failed = true; return false;
    }
    s_gatherPipeLayout  = VK::MakePipelineLayout({ s_gatherSetLayout, texLayout }, sizeof(GatherPush));
    s_resolvePipeLayout = VK::MakePipelineLayout({ s_resolveSetLayout }, sizeof(ResolvePush));
    s_dbgPipeLayout     = VK::MakePipelineLayout({ s_dbgSetLayout }, sizeof(DebugPush),
                                                 VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!s_gatherPipeLayout || !s_resolvePipeLayout || !s_dbgPipeLayout) { s_failed = true; return false; }

    s_gatherPipe  = VK::CreateComputePipeline("gather.comp.spv",         s_gatherPipeLayout,  "Gather");
    s_resolvePipe = VK::CreateComputePipeline("gather_resolve.comp.spv", s_resolvePipeLayout, "GatherResolve");
    if (!s_gatherPipe || !s_resolvePipe) {
        Msg("![VK Gather] compute pipeline create failed — final gather off");
        s_failed = true; return false;
    }
    {
        VkShaderModule vs = g_ShaderManager->Load("tonemap.vert.spv");     // shared fullscreen triangle
        VkShaderModule fs = g_ShaderManager->Load("gather_debug.frag.spv");
        if (vs != VK_NULL_HANDLE && fs != VK_NULL_HANDLE)
            s_dbgPipe = Fullscreen::CreatePipeline(vs, fs, VK::SceneColor::Format(), s_dbgPipeLayout,
                                                   Fullscreen::OpaqueAttachment(), "GatherDbg");
        // A missing debug pipeline is not fatal — it only costs the preview.
        if (s_dbgPipe == VK_NULL_HANDLE) Msg("![VK Gather] debug view unavailable (gather_debug.frag.spv)");
    }

    // Linear + clamp: the receivers bilinearly upsample this to full resolution,
    // and the resolve reads its own neighbourhood through it.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.f;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) {
        Msg("![VK Gather] sampler create failed"); s_failed = true; return false;
    }
    // ⚠ NEAREST for depth and the normal G-buffer. gbuffer_recon.glsl reconstructs
    // at the texel CENTRE on purpose (the prepass rasterized the depth there), and
    // a filtered tap would average two surfaces across every silhouette — which
    // puts a ray origin in mid-air at exactly the edges the gather exists to
    // darken. The normal tag (a = 1 / 0.5 / 0) would be interpolated into nonsense
    // for the same reason.
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampNear) != VK_SUCCESS) {
        Msg("![VK Gather] point sampler create failed"); s_failed = true; return false;
    }

    // The blue-noise tile. A hard failure rather than a silent fallback: the
    // shader uses binding 20 statically, so a null view here would be a validation
    // error on every dispatch rather than a feature quietly switching off.
    s_blueNoise = xr_new<CVulkanTexture>();
    s_blueNoise->CreateFromData(kGatherBlueNoiseRG, kGatherBlueNoiseDim, kGatherBlueNoiseDim,
                                VK_FORMAT_R8G8_UNORM, sizeof(kGatherBlueNoiseRG));
    if (!s_blueNoise->IsValid() || s_blueNoise->GetView() == VK_NULL_HANDLE) {
        Msg("![VK Gather] blue-noise tile upload failed"); s_failed = true; return false;
    }

    Msg("[VK Gather] init OK (final gather: 1 ray/pixel into the probe field, denoise + temporal)");
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    DestroyTargets();
    if (s_sampler)  { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    if (s_sampNear) { vkDestroySampler(VulkanHW.m_Device, s_sampNear, nullptr); s_sampNear = VK_NULL_HANDLE; }
    if (s_gatherPipe)  { vkDestroyPipeline(VulkanHW.m_Device, s_gatherPipe, nullptr);  s_gatherPipe = VK_NULL_HANDLE; }
    if (s_resolvePipe) { vkDestroyPipeline(VulkanHW.m_Device, s_resolvePipe, nullptr); s_resolvePipe = VK_NULL_HANDLE; }
    if (s_dbgPipe)     { vkDestroyPipeline(VulkanHW.m_Device, s_dbgPipe, nullptr);     s_dbgPipe = VK_NULL_HANDLE; }
    if (s_gatherPipeLayout)  { vkDestroyPipelineLayout(VulkanHW.m_Device, s_gatherPipeLayout, nullptr);  s_gatherPipeLayout = VK_NULL_HANDLE; }
    if (s_resolvePipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_resolvePipeLayout, nullptr); s_resolvePipeLayout = VK_NULL_HANDLE; }
    if (s_dbgPipeLayout)     { vkDestroyPipelineLayout(VulkanHW.m_Device, s_dbgPipeLayout, nullptr);     s_dbgPipeLayout = VK_NULL_HANDLE; }
    if (s_gatherPool)  { vkDestroyDescriptorPool(VulkanHW.m_Device, s_gatherPool, nullptr);  s_gatherPool = VK_NULL_HANDLE; }
    if (s_resolvePool) { vkDestroyDescriptorPool(VulkanHW.m_Device, s_resolvePool, nullptr); s_resolvePool = VK_NULL_HANDLE; }
    if (s_dbgPool)     { vkDestroyDescriptorPool(VulkanHW.m_Device, s_dbgPool, nullptr);     s_dbgPool = VK_NULL_HANDLE; }
    if (s_gatherSetLayout)  { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_gatherSetLayout, nullptr);  s_gatherSetLayout = VK_NULL_HANDLE; }
    if (s_resolveSetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_resolveSetLayout, nullptr); s_resolveSetLayout = VK_NULL_HANDLE; }
    if (s_dbgSetLayout)     { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_dbgSetLayout, nullptr);     s_dbgSetLayout = VK_NULL_HANDLE; }
    if (s_blueNoise) { s_blueNoise->Destroy(); xr_delete(s_blueNoise); }
    for (auto& s : s_gatherSets)  s = VK_NULL_HANDLE;
    for (auto& s : s_resolveSets) s = VK_NULL_HANDLE;
    for (auto& s : s_dbgSets)     s = VK_NULL_HANDLE;
    s_inited = false; s_failed = false;
}

void OnLevelUnload()
{
    // The BVH and the probe field both describe a world that is going away; the
    // history describes it too, so it must not survive into the next level (a
    // reprojected stranger is worse than a cold start).
    s_hasResult = false;
    s_histValid = false;
    s_needClear = true;
    s_prevVPvalid = false;   // the screen cache's history belongs to the old world too
}

bool Enabled()
{
    // The volume is not optional: it IS this pass's radiance cache. Asking for the
    // gather without it would trace a million rays into a black field, which looks
    // exactly like a broken shader — so the gate names the dependency instead.
    return Wanted() && s_inited && !s_failed && s_rawImg != VK_NULL_HANDLE
        && RTScene::Ready() && DDGI::Enabled();
}

bool EnsureTargets(VkExtent2D sceneExtent)
{
    if (!Wanted()) { if (s_rawImg) DestroyTargets(); return false; }
    if (!Init()) return false;

    // Advance the ping-pong ONCE per frame, and do it HERE rather than in Execute:
    // EnvLight::Update binds ResultView() for the colour pass before the prepass
    // even runs, so the view a receiver will sample has to be settled before then.
    if (s_frame != RV().frameIndex) {
        s_frame = RV().frameIndex;
        if (s_hasResult) { s_cur ^= 1; s_histValid = true; }
    }

    const u32 div = 2u * u32(_max(1, _min(4, ps_r_gather_scale)));
    VkExtent2D want{ _max(1u, sceneExtent.width / div), _max(1u, sceneExtent.height / div) };
    const bool shOn = ps_r_gather_sh != 0;
    if (s_rawImg && want.width == s_extent.width && want.height == s_extent.height
        && shOn == s_shOn) return true;

    // The old targets may still be referenced by frames in flight. This only runs
    // on a resize or a console change, so a full idle is the honest way to be safe.
    if (s_rawImg) VK_DeviceWaitIdleLocked();
    DestroyTargets();

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                  | VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // cleared on first use
    // 1x1 when the SH is off — see the note on s_shRawImg. Three tiles tall when on.
    const VkExtent2D shWant = shOn ? VkExtent2D{ want.width, want.height * kSHTiles }
                                   : VkExtent2D{ 1u, 1u };
    if (!VK::CreateImage2D(kFormat, want, usage, s_rawImg, s_rawAlloc, "Gather/raw") ||
        !VK::CreateImage2D(kFormat, want, usage, s_resImg[0], s_resAlloc[0], "Gather/res0") ||
        !VK::CreateImage2D(kFormat, want, usage, s_resImg[1], s_resAlloc[1], "Gather/res1") ||
        !VK::CreateImage2D(kDepthFormat, want, usage, s_dpImg[0], s_dpAlloc[0], "Gather/z0") ||
        !VK::CreateImage2D(kDepthFormat, want, usage, s_dpImg[1], s_dpAlloc[1], "Gather/z1") ||
        !VK::CreateImage2D(kDepthFormat, want, usage, s_ageImg[0], s_ageAlloc[0], "Gather/age0") ||
        !VK::CreateImage2D(kDepthFormat, want, usage, s_ageImg[1], s_ageAlloc[1], "Gather/age1") ||
        !VK::CreateImage2D(kFormat, shWant, usage, s_shRawImg, s_shRawAlloc, "Gather/shRaw") ||
        !VK::CreateImage2D(kFormat, shWant, usage, s_shResImg[0], s_shResAlloc[0], "Gather/sh0") ||
        !VK::CreateImage2D(kFormat, shWant, usage, s_shResImg[1], s_shResAlloc[1], "Gather/sh1")) {
        Msg("![VK Gather] target create failed (%ux%u) — final gather off", want.width, want.height);
        DestroyTargets();
        return false;
    }
    s_rawView    = VK::CreateImageView(s_rawImg, kFormat);
    s_resView[0] = VK::CreateImageView(s_resImg[0], kFormat);
    s_resView[1] = VK::CreateImageView(s_resImg[1], kFormat);
    s_dpView[0]  = VK::CreateImageView(s_dpImg[0], kDepthFormat);
    s_dpView[1]  = VK::CreateImageView(s_dpImg[1], kDepthFormat);
    s_ageView[0] = VK::CreateImageView(s_ageImg[0], kDepthFormat);
    s_ageView[1] = VK::CreateImageView(s_ageImg[1], kDepthFormat);
    s_shRawView    = VK::CreateImageView(s_shRawImg, kFormat);
    s_shResView[0] = VK::CreateImageView(s_shResImg[0], kFormat);
    s_shResView[1] = VK::CreateImageView(s_shResImg[1], kFormat);
    if (!s_rawView || !s_resView[0] || !s_resView[1] || !s_dpView[0] || !s_dpView[1]
        || !s_ageView[0] || !s_ageView[1]
        || !s_shRawView || !s_shResView[0] || !s_shResView[1]) { DestroyTargets(); return false; }

    s_extent    = want;
    s_srcExtent = sceneExtent;
    s_scale     = div / 2u;
    s_shOn      = shOn;
    s_cur = 0; s_hasResult = false; s_histValid = false; s_needClear = true;
    s_prevVPvalid = false;
    // The SH triples the RGBA16F footprint when it is on (three tiles x three
    // images), so it is named separately rather than folded into one number.
    Msg("[VK Gather] targets %ux%u (1/%u of %ux%u), %.1f MB%s",
        want.width, want.height, div, sceneExtent.width, sceneExtent.height,
        double(want.width) * want.height
            * (8.0 * 3.0 + 4.0 * 4.0 + (shOn ? 8.0 * 3.0 * double(kSHTiles) : 0.0)) / (1024.0 * 1024.0),
        shOn ? " (2-band SH on)" : "");
    return true;
}

VkImageView ResultView() { return (s_hasResult && s_resView[s_cur]) ? s_resView[s_cur] : VK_NULL_HANDLE; }
// ⚠ Gated on s_shOn, which is what the targets were BUILT with — not on the cvar.
// Between the frame the cvar changes and the frame EnsureTargets rebuilds, the
// image is still 1x1, and a receiver told to read directions out of it would be
// reading one texel stretched over the frame.
VkImageView SHResultView()
{
    return (s_hasResult && s_shOn && s_shResView[s_cur]) ? s_shResView[s_cur] : VK_NULL_HANDLE;
}
VkSampler   GetSampler() { return s_sampler; }
u32         Generation() { return s_gen; }

float Weight()
{
    if (!s_hasResult || !Enabled()) return 0.f;
    return _max(0.f, _min(1.f, ps_r_gather_light));
}

// ---- WHY THE PASS IS NOT RUNNING, SAID OUT LOUD ---------------------------
// ⚠⚠ EIGHT GATES CAN CLOSE THIS PASS AND ALL BUT ONE USED TO DO IT IN SILENCE.
// That is worse here than in most passes, because the probe lattice keeps
// answering when the gather stops: the room still lights up and still reacts to
// where the player stands, it just goes FLAT — no light through a window, no
// bounce, no contact darkening. A dead gather and a merely subtle one are the
// SAME SCREENSHOT, so the failure reaches a person as "the GI got worse" rather
// than as a log line. It cost a session; hence this.
//
// ⭐⭐ IT IS A SEPARATE ENTRY POINT ON PURPOSE. Pass_World cannot simply call
// Execute every frame — it has to put the depth into SHADER_READ first, and
// paying two layout transitions a frame for a pass the player switched off is not
// free. So the CHEAP half of the gate (`Wanted`, pure user intent) stays at the
// call site and the EXPENSIVE, IMPLICIT half lives here, where it can name itself.
// ⚠ A gate duplicated at a call site is a gate that can skip the pass without
// entering it, which is exactly how the normal-G-buffer failure stayed invisible.
//
// Returns true when the pass must not run. Prints on CHANGE, not per frame —
// including the recovery, because "it started working again" is half a diagnosis.
bool ReportGate(VkImageView depthView, VkImageView normalView)
{
    const char* why = nullptr;
    if      (!Wanted())                     why = "r_gather 0 and r_gather_light 0 (see r_gi)";
    else if (!s_inited || s_failed)         why = "init failed";
    else if (s_rawImg == VK_NULL_HANDLE)    why = "no targets";
    else if (!RTScene::Ready())             why = "the BVH bubble is not built (RTScene not ready)";
    else if (!DDGI::Enabled())              why = "the probe volume is off (r_ddgi) — it IS this pass's radiance cache";
    else if (depthView == VK_NULL_HANDLE)   why = "no prepass depth";
    else if (normalView == VK_NULL_HANDLE)  why = "no normal G-buffer — it belongs to the SSAO pass (r_ssao)";
    else if (s_extent.width == 0)           why = "the targets have no extent";

    static const char* s_lastWhy = "";      // a literal no branch above can produce
    if (why != s_lastWhy) {
        s_lastWhy = why;
        if (why) Msg("![VK Gather] NOT RUNNING: %s", why);
        else     Msg("[VK Gather] running again");
    }
    return why != nullptr;
}

// ---------------------------------------------------------------------------
// THE RAY-COST PROFILER (r_gather_profile)
//
// Why it exists. Every attempt so far to price one link of the ray by hand — flip
// a cvar, read the zone, flip back — has been fought by the fact that the SCENE
// MOVES between flips: the same wall at a slightly different angle changes
// Gather/Trace by more than the term being measured, and three such A/Bs in a row
// produced numbers that could not be added up. So the pass drives the A/B itself:
// it holds each configuration for a fixed number of frames, discards the first few
// while the GPU timestamps catch up (Prof::gpuLast lags the submit by a frame or
// two), averages the rest, and prints one table where every row differs from the
// baseline by EXACTLY ONE term.
//
// The reference number this is aimed at: idTech 8 ships Final Gather at 0.5 ms for
// ~2M rays (4K, half-res collect) = ~4M rays/ms. Ours is 0.85 ms for 256k rays =
// ~0.3M rays/ms. The table below is how we find out where the other 13x lives.
//
// ⚠ The switches are applied to the ps_* globals and RESTORED when the run ends —
// including on an early exit, hence the explicit restore in Stop(). Two of them
// (indoor ray, wrc) visibly change the image while they are held; that is expected
// for a measurement and lasts a few seconds.
namespace RayProf {

struct Phase { const char* name; };
// Row 0 must stay the baseline: every delta is printed against it.
const Phase kPhases[] = {
    { "baseline"      },
    { "-lazy(lattice)" },   // lazy OFF = the old unconditional probe walk
    { "-foliage"      },    // no any-hit through alpha-tested cutouts
    { "-scache"       },    // no screen cache (its own cost AND it forces the lattice)
    { "-wrcQuery"     },    // no world-cache hash walk
    { "-indoorRay"    },    // no second trace per pixel
    { "rays 2"        },    // calibration: what one extra ray actually costs
    // ⭐ THE FOUR ROWS THAT SPLIT `-foliage` IN TWO. That row removes the cutout
    // structure whole, so it prices TRAVERSAL + ALPHA TEST together; these keep the
    // forest in the BVH and change only how a candidate is answered, so the gap
    // between `-foliage` and `atest:solid` is the traversal — the part no micromap
    // and no software stand-in can give back. Everything below `atest:solid` is what
    // is actually for sale.
    { "atest:solid"   },    // accept every candidate: the optimistic floor of alpha-test work
    { "atest:hash"    },    // no texture, no UV fetch — the honest "cost of the fetch"
    { "atest:cov"     },    // coverage read from a coarse (cache-resident) mip, sampled not thresholded
    { "atest:lod+2"   },    // the exact test, one knob: a mip floor instead of the finest level
    // ⚠ THE ONLY REASON THESE TWO ARE ROWS AND NOT A CONSOLE A/B. Tried by hand at
    // 23:36 the answer was unreadable: with the configuration UNCHANGED, Gather/Trace
    // walked 0.50 -> 0.67 in five seconds because the camera turned towards the
    // forest — 0.17 ms of drift against a 0.18 ms effect. Anything measured across
    // that is a reading of where the player was looking.
    { "atest:solid d8"  },  // exact within 8 m, solid beyond — a canopy IS opaque in aggregate at distance
    { "atest:solid d20" },  // the same, held off to 20 m: how fast the saving falls away
};
const int kPhaseCount = int(sizeof(kPhases) / sizeof(kPhases[0]));
const int kRowFoliage = 2, kRowRays = 6, kRowSolid = 7;
const int kRowHash = 8, kRowCov = 9, kRowLod = 10, kRowD8 = 11, kRowD20 = 12;
const int kFramesPerPhase = 48;
const int kWarmupFrames   = 10;   // GPU timestamps lag; discard while they do

bool  s_active = false;
int   s_phase = 0, s_frame = 0;
float s_accTrace = 0.f, s_accTotal = 0.f;
int   s_accN = 0;
float s_trace[kPhaseCount] = {}, s_total[kPhaseCount] = {};

// Saved user values.
int   s_svLazy = 1, s_svFoliage = 1, s_svWrc = 1, s_svIndoorRay = 1, s_svRays = 1;
float s_svScache = 1.f;
int   s_svAtest = 0;
float s_svAtestLod = 0.f, s_svAtestDist = 0.f;

void Save()
{
    s_svLazy = ps_r_gather_lazy;   s_svFoliage = ps_r_gather_foliage;
    s_svWrc  = ps_r_gather_wrc;    s_svIndoorRay = ps_r_gather_indoor_ray;
    s_svRays = ps_r_gather_rays;   s_svScache = ps_r_gather_scache;
    s_svAtest = ps_r_gather_atest; s_svAtestLod = ps_r_gather_atest_lod;
    s_svAtestDist = ps_r_gather_atest_dist;
}
void Restore()
{
    ps_r_gather_lazy = s_svLazy;   ps_r_gather_foliage = s_svFoliage;
    ps_r_gather_wrc  = s_svWrc;    ps_r_gather_indoor_ray = s_svIndoorRay;
    ps_r_gather_rays = s_svRays;   ps_r_gather_scache = s_svScache;
    ps_r_gather_atest = s_svAtest; ps_r_gather_atest_lod = s_svAtestLod;
    ps_r_gather_atest_dist = s_svAtestDist;
}
void Apply(int phase)
{
    Restore();                       // every row is baseline + exactly one change
    switch (phase) {
        case 1: ps_r_gather_lazy = 0; break;
        case 2: ps_r_gather_foliage = 0; break;
        case 3: ps_r_gather_scache = 0.f; break;
        case 4: ps_r_gather_wrc = 0; break;
        case 5: ps_r_gather_indoor_ray = 0; break;
        case 6: ps_r_gather_rays = _max(2, s_svRays + 1); break;
        // ⚠ The alpha-test rows deliberately do NOT touch r_gather_atest_dist: a row
        // that priced the cheap mode on only part of the ray would not be comparable
        // with `-foliage`, which drops all of it.
        case 7: ps_r_gather_atest = 1; break;
        case 8: ps_r_gather_atest = 2; break;
        case 9: ps_r_gather_atest = 3; break;
        case 10: ps_r_gather_atest_lod = 2.f; break;
        // The two rows that DO set the distance, and the reason the note above says
        // the others must not: here the distance IS the term being priced.
        case 11: ps_r_gather_atest = 1; ps_r_gather_atest_dist = 8.f;  break;
        case 12: ps_r_gather_atest = 1; ps_r_gather_atest_dist = 20.f; break;
        default: break;              // 0 = baseline
    }
}

// Last completed GPU time for a zone, by name. Prof keeps a flat array.
float ZoneMs(const char* name)
{
    const VK::Prof::ZoneStat* z = nullptr;
    const u32 n = VK::Prof::GetZones(&z);
    for (u32 i = 0; i < n; ++i)
        if (0 == xr_strcmp(z[i].name, name)) return z[i].gpuLast;
    return 0.f;
}

void Report()
{
    Msg("~ [VK GatherProf] %d rays/px, scale %d, %dx%d — %d frames per row",
        s_svRays, ps_r_gather_scale, s_extent.width, s_extent.height, kFramesPerPhase - kWarmupFrames);
    const float base = s_trace[0];
    for (int i = 0; i < kPhaseCount; ++i) {
        // ⚠ The ray row's name is COMPUTED, not written down. It said "rays 2" while
        // the default was 1; the day the default became 3 the row was applying 4 and
        // still printing 2, so the one number the whole table is denominated in was
        // labelled with someone else's experiment.
        char nameBuf[24];
        const char* rowName = kPhases[i].name;
        if (i == kRowRays) {
            xr_sprintf(nameBuf, "rays %d", _max(2, s_svRays + 1));
            rowName = nameBuf;
        }
        const float d = s_trace[i] - base;
        Msg("~ [VK GatherProf]   %-16s Trace %.3f ms  (%+.3f)   gpu_total %.2f",
            rowName, s_trace[i], d, s_total[i]);
    }
    // The deltas of rows 1..5 are COSTS OF TERMS (each row removes one thing, so a
    // negative delta is what that term costs); row 6 is the price of one more ray,
    // which is what any saving would be spent on.
    const float ray = s_trace[kRowRays] - base;
    Msg("~ [VK GatherProf] one extra ray costs %+.3f ms; removing all measured terms would free %.3f ms",
        ray, (base - s_trace[kRowFoliage]) + (base - s_trace[4]) + (base - s_trace[5]));

    // ⭐ THE ONE QUESTION THIS RUN EXISTS TO ANSWER. `-foliage` is traversal AND
    // alpha test; `atest:solid` keeps the traversal and drops only the test. What is
    // left between them is geometry, and no micromap gives that back — so the "for
    // sale" column below is the honest ceiling on any work in this direction, and it
    // is quoted in RAYS because a ray is what the saving would be spent on.
    const float foliage = base - s_trace[kRowFoliage];   // traversal + test
    const float test    = base - s_trace[kRowSolid];     // the test alone (optimistic: rays also stop sooner)
    const float fetch   = base - s_trace[kRowHash];       // the texture + UV fetch, statistics kept
    Msg("~ [VK GatherProf] foliage %.3f ms = traversal %.3f + alpha test %.3f  |  the fetch alone %.3f ms",
        foliage, foliage - test, test, fetch);
    if (ray > 1e-4f) {
        Msg("~ [VK GatherProf] for sale: solid %.2f rays, hash %.2f, coarse-mip %.2f, mip floor %.2f",
            test / ray, fetch / ray, (base - s_trace[kRowCov]) / ray, (base - s_trace[kRowLod]) / ray);
        // The distance sweep, and the only column that costs PICTURE rather than
        // code: what a canopy held exact nearby and solid beyond is worth. If d8 is
        // most of `solid` the near metres were never where the cost was.
        Msg("~ [VK GatherProf] solid beyond: 8 m %.3f ms (%.2f rays), 20 m %.3f ms (%.2f rays) — against solid-everywhere %.3f",
            base - s_trace[kRowD8],  (base - s_trace[kRowD8])  / ray,
            base - s_trace[kRowD20], (base - s_trace[kRowD20]) / ray, test);
    }
}

void Stop()
{
    if (!s_active) return;
    s_active = false;
    Restore();
}

// Called once per frame at the top of the pass.
void Tick()
{
    if (ps_r_gather_profile == 0) { Stop(); return; }
    if (!s_active) {
        s_active = true; s_phase = 0; s_frame = 0; s_accN = 0;
        s_accTrace = s_accTotal = 0.f;
        Save();
        Msg("~ [VK GatherProf] started — hold still, %d rows x %d frames",
            kPhaseCount, kFramesPerPhase);
    }
    if (s_frame >= kWarmupFrames) {
        s_accTrace += ZoneMs("Gather/Trace");
        s_accTotal += VK::Prof::GetFrame().gpuMs;
        ++s_accN;
    }
    if (++s_frame >= kFramesPerPhase) {
        s_trace[s_phase] = s_accN ? s_accTrace / float(s_accN) : 0.f;
        s_total[s_phase] = s_accN ? s_accTotal / float(s_accN) : 0.f;
        s_frame = 0; s_accN = 0; s_accTrace = s_accTotal = 0.f;
        if (++s_phase >= kPhaseCount) {
            Report();
            Restore();
            s_active = false;
            // 1 = one pass then off; 2 = keep looping (walk around and re-read).
            if (ps_r_gather_profile == 1) ps_r_gather_profile = 0;
            return;
        }
    }
    Apply(s_phase);
}

}  // namespace RayProf

void Execute(VkCommandBuffer cmd, VkExtent2D sceneExtent, VkImageView depthView,
             VkImageView normalView, const ProjTerms& pt, const Fmatrix& viewProj)
{
    RayProf::Tick();
    if (ReportGate(depthView, normalView)) return;

    const VkAccelerationStructureKHR tlas = RTScene::TLAS();
    const VkBuffer verts = RTScene::VertexBuffer();
    const VkBuffer mats  = RTScene::MaterialBuffer();
    const VkBuffer alb   = RTScene::AlbedoBuffer();
    const VkDescriptorSet texSet = RTWorld::TexSet();
    const VkBuffer    ddgiC = DDGI::ConstsBuffer();
    const VkImageView irrV  = DDGI::IrradianceView();
    const VkImageView depV  = DDGI::DistanceView();
    const VkSampler   ddgiS = DDGI::GetSampler();
    VkBuffer    shBuf = VK_NULL_HANDLE;
    VkImageView cubeV = VK_NULL_HANDLE;
    VkSampler   cubeS = VK_NULL_HANDLE;
    DDGI::SkyInputs(shBuf, cubeV, cubeS);   // the SAME sky the probe rays integrate
    // The world radiance cache. Its buffers are created and maintained by vk_wrc
    // inside DDGI::Update, which runs earlier in this frame; this pass only reads
    // them (and marks the cells its rays want). The shader declares all four
    // unconditionally, so a null is not a duller frame — it is an invalid set.
    const VkBuffer wrcConsts = WRC::ConstsBuffer();
    const VkBuffer wrcCells  = WRC::CellsBuffer();
    const VkBuffer wrcList   = WRC::ListBuffer();
    const VkBuffer wrcCtrl   = WRC::CtrlBuffer();
    if (!tlas || !verts || !mats || !alb || !texSet || !ddgiC || !irrV || !depV || !ddgiS ||
        !shBuf || !cubeV || !cubeS || !wrcConsts || !wrcCells || !wrcList || !wrcCtrl) {
        // Silence would read as "the shader is broken". Each missing piece has its
        // own cause, so name it once rather than leaving the pass quietly idle.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Msg("![VK Gather] idle: tlas=%s geom=%s tex=%s ddgi=%s sky=%s wrc=%s",
                tlas ? "ok" : "MISSING", (verts && mats && alb) ? "ok" : "MISSING",
                texSet ? "ok" : "MISSING", (ddgiC && irrV && depV) ? "ok" : "MISSING",
                (shBuf && cubeV) ? "ok" : "MISSING",
                (wrcConsts && wrcCells && wrcList && wrcCtrl) ? "ok" : "MISSING");
        }
        return;
    }

    // A gather ray must not outlive the bubble: past its boundary a miss means
    // "nothing built", not "open sky", and the pixel would come out lit by a sky
    // that a wall is standing in front of. Keep the cutoff inside the built radius
    // minus the camera's drift since the anchor, minus the ray length itself.
    const float rayLen = _max(0.5f, ps_r_gather_len);
    const float drift  = RV().vCameraPosition.distance_to(RTScene::Anchor());
    const float safe   = _max(0.f, RTScene::Coverage() * 0.85f - drift - rayLen);
    const float cutoff = _min(ps_r_gather_dist, safe);
    // The sixth gate, and the quietest of the lot: it depends on the BUBBLE and on
    // how far the player has walked from its anchor, so it can close in the middle
    // of a run with nothing on screen to say so — walk far enough from the anchor
    // and the gather simply stops, leaving the flat lattice behind.
    // ⚠ It is also the one that a large r_gather_len can close by itself, which is
    // why all four numbers are printed and not just the verdict.
    static int s_lastCut = -1;
    if (cutoff <= 1.f) {
        const int k = int(cutoff * 10.f);
        if (s_lastCut != k) {
            s_lastCut = k;
            Msg("![VK Gather] NOT RUNNING: no room for a ray — bubble %.1f m, drift from anchor %.1f m, "
                "ray %.1f m => cutoff %.1f m (needs > 1)", RTScene::Coverage(), drift, rayLen, cutoff);
        }
        return;
    }
    if (s_lastCut != -1) {
        s_lastCut = -1;
        Msg("[VK Gather] room for a ray again — bubble %.1f m, drift %.1f m => cutoff %.1f m",
            RTScene::Coverage(), drift, cutoff);
    }

    const u32 slot   = CommandManager.GetCurrentFrame() % kFramesInFlight;
    const u32 parity = s_cur & 1u;
    const u32 rIdx   = slot * 2u + parity;

    const int zone = VK::Prof::ZoneBegin(cmd, "World/Gather");

    // ---- first use: the targets hold undefined memory ----------------------
    if (s_needClear) {
        // ⚠ EXPLICIT stages, not the layout-derived overload: a layout transition IS
        // a write, and the derived form answers "compute storage read|write" for
        // GENERAL, which does not cover the transfer clear that follows (the exact
        // WRITE_AFTER_WRITE sync validation caught in the DDGI atlases).
        const VkImage imgs[10] = { s_rawImg, s_resImg[0], s_resImg[1], s_dpImg[0], s_dpImg[1],
                                   s_ageImg[0], s_ageImg[1],
                                   s_shRawImg, s_shResImg[0], s_shResImg[1] };
        for (VkImage img : imgs)
            VK::ImageBarrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                             VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero{};
        VkImageSubresourceRange rng{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (VkImage img : imgs)
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);
        VK::MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        s_needClear = false;
    }

    // ---- screen-space radiance cache ---------------------------------------
    // Three things must all hold, and each fails differently:
    //  * the history buffer exists at all (r_ssao off ⇒ no capture, no cache);
    //  * it is EXACTLY one frame old — see the note on s_prevVPframe;
    //  * the cvar is up.
    // Anything short of that leaves the weight at 0, and the shader then reads the
    // probe field exactly as it did before this cache existed.
    const VkImageView prevColorView = SSAOPass::GetPrevColorView();
    const bool  cacheLive = prevColorView != VK_NULL_HANDLE && s_prevVPvalid
                         && RV().frameIndex == s_prevVPframe + 1 && ps_r_gather_scache > 0.f;
    const float cacheW    = cacheLive ? _max(0.f, _min(1.f, ps_r_gather_scache)) : 0.f;
    // A statically used descriptor must point at a real image even when it is never
    // sampled — the resolved target is the same trick the motion-vector bind uses.
    const VkImageView   pcView   = prevColorView ? prevColorView : s_resView[0];
    const VkImageLayout pcLayout = prevColorView ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_GENERAL;

    // ---- descriptors -------------------------------------------------------
    {
        BoundGather want{ tlas, verts, mats, alb, ddgiC, shBuf, irrV, depV, cubeV,
                          depthView, normalView, s_rawView, s_shRawView,
                          pcView, s_dpView[parity ^ 1u], s_dpView[parity],
                          wrcConsts, wrcCells, wrcList, wrcCtrl };
        if (memcmp(&s_boundGather[slot], &want, sizeof(want)) != 0) {
            VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
            asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            asInfo.accelerationStructureCount = 1;
            asInfo.pAccelerationStructures    = &tlas;
            VkWriteDescriptorSet asWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            asWrite.pNext = &asInfo;
            asWrite.dstSet = s_gatherSets[slot];
            asWrite.dstBinding = 0;
            asWrite.descriptorCount = 1;
            asWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(VulkanHW.m_Device, 1, &asWrite, 0, nullptr);

            // The probe atlases live in GENERAL permanently; the prefiltered sky cube
            // is left in SHADER_READ_ONLY by vk_ibl, like every other consumer of it.
            VK::DescriptorWriter(s_gatherSets[slot])
                .StorageBuffer(1, verts)
                .StorageBuffer(2, mats)
                .StorageBuffer(3, alb)
                .UniformBuffer(4, ddgiC)
                .ImageSampler(5, irrV, ddgiS, VK_IMAGE_LAYOUT_GENERAL)
                .ImageSampler(6, depV, ddgiS, VK_IMAGE_LAYOUT_GENERAL)
                .StorageBuffer(7, shBuf)
                .ImageSampler(8, cubeV, cubeS)
                .ImageSampler(9, depthView, s_sampNear)
                .ImageSampler(10, normalView, s_sampNear)
                .StorageImage(11, s_rawView)
                // ⚠ NEAREST for the Z history, LINEAR for the colour. Filtering a
                // depth across a silhouette invents a surface halfway between two,
                // and the cache would then accept a hit that neither of them shows.
                .ImageSampler(12, pcView, s_sampler, pcLayout)
                .ImageSampler(13, s_dpView[parity ^ 1u], s_sampNear, VK_IMAGE_LAYOUT_GENERAL)
                .StorageImage(14, s_dpView[parity])
                .UniformBuffer(15, wrcConsts)
                .StorageBuffer(16, wrcCells)
                .StorageBuffer(17, wrcList)
                .StorageBuffer(18, wrcCtrl)
                .StorageImage(19, s_shRawView)
                .ImageSampler(20, s_blueNoise->GetView(), s_blueNoise->GetSampler())
                .Flush();
            s_boundGather[slot] = want;
        }
    }

    const bool  mvLive = MotionVec::Enabled() && MotionVec::GetResultView() != VK_NULL_HANDLE;
    if (mvLive) { if (s_mvWarm < 4) ++s_mvWarm; } else s_mvWarm = 0;
    // ⚠ ONE FRAME STALE, ON PURPOSE. MotionVec::Execute runs after the world pass,
    // so what is bound here is the PREVIOUS frame's motion — the same deal
    // vk_pass_ssao takes for its own temporal, and the same warm-up counter.
    const bool  mvReady = s_histValid && (s_mvWarm >= 2);
    // The placeholder is never sampled when !mvReady, but a statically used
    // descriptor still has to point at a real, readable image.
    const VkImageView mvView = mvReady ? MotionVec::GetResultView() : s_resView[parity ^ 1u];
    const VkSampler   mvSamp = mvReady ? MotionVec::GetSampler() : s_sampler;
    const VkImageLayout mvLayout = mvReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    {
        BoundResolve want{ depthView, s_rawView, s_resView[parity ^ 1u], mvView, s_resView[parity],
                           s_shRawView, s_shResView[parity ^ 1u], s_shResView[parity],
                           s_ageView[parity ^ 1u], s_ageView[parity], normalView };
        if (memcmp(&s_boundResolve[rIdx], &want, sizeof(want)) != 0) {
            VK::DescriptorWriter(s_resolveSets[rIdx])
                .ImageSampler(0, depthView, s_sampNear)
                .ImageSampler(1, s_rawView, s_sampler, VK_IMAGE_LAYOUT_GENERAL)
                .ImageSampler(2, s_resView[parity ^ 1u], s_sampler, VK_IMAGE_LAYOUT_GENERAL)
                .ImageSampler(3, mvView, mvSamp, mvLayout)
                .StorageImage(4, s_resView[parity])
                // ⚠ LINEAR on the SH history for the same reason as the radiance's:
                // the temporal tap is reprojected and lands between texels. The
                // shader keeps that filter half a texel inside its tile so it can
                // never blend one colour channel's direction into the next.
                .ImageSampler(5, s_shRawView, s_sampler, VK_IMAGE_LAYOUT_GENERAL)
                .ImageSampler(6, s_shResView[parity ^ 1u], s_sampler, VK_IMAGE_LAYOUT_GENERAL)
                .StorageImage(7, s_shResView[parity])
                // ⚠ NEAREST on the count, LINEAR on the radiance it travels with.
                // Filtering a sample count across a disocclusion edge would average
                // a settled 12 with a fresh 1 and hand the young side of the edge a
                // confidence it never earned — the exact thing the counter exists
                // to withhold.
                .ImageSampler(8, s_ageView[parity ^ 1u], s_sampNear, VK_IMAGE_LAYOUT_GENERAL)
                .StorageImage(9, s_ageView[parity])
                // ⚠ NEAREST, the same sampler the trace reads this image through: a
                // filtered normal on a silhouette is a direction no surface has, and
                // the weight would then reject BOTH sides of the edge.
                .ImageSampler(10, normalView, s_sampNear)
                .Flush();
            s_boundResolve[rIdx] = want;
        }
    }

    // ---- gather ------------------------------------------------------------
    // The golden-ratio phase makes each frame a different realisation of the same
    // integral, which is the whole reason the temporal filter converges instead of
    // re-averaging one fixed, biased ray set.
    float jitter = float(RV().frameIndex) * 0.61803399f; jitter -= std::floor(jitter);

    GatherPush gp{};
    gp.camDir[0] = pt.dir.x; gp.camDir[1] = pt.dir.y; gp.camDir[2] = pt.dir.z; gp.camDir[3] = rayLen;
    gp.camRightT[0] = pt.right.x * pt.tanX; gp.camRightT[1] = pt.right.y * pt.tanX;
    gp.camRightT[2] = pt.right.z * pt.tanX; gp.camRightT[3] = pt.tanX;
    gp.camTopT[0]   = pt.top.x * pt.tanY;   gp.camTopT[1]   = pt.top.y * pt.tanY;
    gp.camTopT[2]   = pt.top.z * pt.tanY;   gp.camTopT[3]   = pt.tanY;
    gp.zp[0] = pt.p33;
    gp.zp[1] = pt.p43;
    gp.zp[2] = float(_max(1, _min(8, ps_r_gather_rays)));
    gp.zp[3] = ps_r_gather_bias;
    gp.res[0] = float(s_extent.width);   gp.res[1] = float(s_extent.height);
    gp.res[2] = 1.f / float(s_extent.width); gp.res[3] = 1.f / float(s_extent.height);
    gp.camPos[0] = RV().vCameraPosition.x;
    gp.camPos[1] = RV().vCameraPosition.y;
    gp.camPos[2] = RV().vCameraPosition.z;
    gp.camPos[3] = cutoff;
    gp.params[0] = jitter;
    // 0x01 = the opaque world, 0x02 = the alpha-tested cutouts (vk_rt_scene tags the
    // two structures). The forest is ~99% of the cutout triangles and every one of
    // them asks a texture on the way through, so dropping it is the one lever that
    // changes the cost of this pass by an order of magnitude.
    gp.params[1] = ps_r_gather_foliage ? 3.f : 1.f;
    gp.params[2] = cacheW;
    gp.params[3] = _max(0.005f, _min(0.25f, ps_r_gather_scache_tol));
    // Row-major Fmatrix copied verbatim; the shader's mat4 reads it as the
    // transpose, which is what makes `prevVP * vec4(world,1)` the right order.
    // Same convention as motion_vec.frag — deliberately, so the two reprojections
    // cannot drift apart.
    std::memcpy(gp.prevVP, &s_prevVP, sizeof(gp.prevVP));
    // s_shOn, not the cvar: the shader writes into whatever this frame's targets
    // are, and those follow the rebuild rather than the console.
    gp.sh[0] = s_shOn ? 1.f : 0.f;
    // r_gather_bn. A pure shader branch over two already-bound sources, so it is a
    // legal in-run A/B: nothing is reallocated, nothing warms up, and the change
    // lands on the very next frame (unlike r_gather_sh, whose flip rebuilds the
    // targets and therefore cannot be A/B'd by its own gate).
    gp.sh[1] = ps_r_gather_bn ? 1.f : 0.f;
    // r_gather_scache_indoor: how much a point's enclosedness weights the screen
    // cache OUT of its hit chain. The cache returns raw-albedo raster pixels, the
    // rest of the chain carries the reflectance compensation — indoors the two
    // disagree by the whole boost, and the cache side is a darkness feedback loop
    // (measured 27-08, see the note at the cache blend in gather.comp).
    gp.sh[2] = _max(0.f, _min(1.f, ps_r_gather_scache_indoor));
    // r_gather_field_clamp: where the field prints straight to the pixel (past the
    // trace cutoff — which the bubble keeps at 5-30 m, not r_gather_dist — and at
    // hits with no shaded cell), cap it at k x the world cache's own answer for
    // that surface. The cells are dark inside rooms no lattice resolves; outdoors
    // they carry direct sun on top and the cap never fires. 0 = off.
    gp.sh[3] = _max(0.f, ps_r_gather_field_clamp);
    // r_gather_atest — see the modes in vk_console_min.cpp and rt_shade.glsl. Modes
    // 1..3 CHANGE THE PICTURE (a solid or a stochastic canopy), which is exactly
    // what the profiler holds them for; only 0 is the shipped answer.
    gp.atest[0] = float(_max(0, _min(3, ps_r_gather_atest)));
    gp.atest[1] = _max(0.f, _min(1.f, ps_r_gather_atest_cov));
    gp.atest[2] = _max(0.f, _min(6.f, ps_r_gather_atest_lod));
    gp.atest[3] = _max(0.f, ps_r_gather_atest_dist);

    {
        const int z = VK::Prof::ZoneBegin(cmd, "Gather/Trace");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_gatherPipe);
        const VkDescriptorSet sets[2] = { s_gatherSets[slot], texSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_gatherPipeLayout, 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(cmd, s_gatherPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gp), &gp);
        vkCmdDispatch(cmd, (s_extent.width + 7) / 8, (s_extent.height + 7) / 8, 1);
        VK::Prof::ZoneEnd(cmd, z);
    }
    VK::ComputeBarrier(cmd);

    // ---- resolve -----------------------------------------------------------
    ResolvePush rp{};
    rp.zp[0] = pt.p33; rp.zp[1] = pt.p43;
    rp.zp[2] = s_shOn ? 1.f : 0.f;
    rp.res[0] = gp.res[0]; rp.res[1] = gp.res[1]; rp.res[2] = gp.res[2]; rp.res[3] = gp.res[3];
    // r_dlss_rr_raw: when Ray Reconstruction is actually running it OWNS the denoise
    // — its input is spelled "Noisy Ray Traced Input Color". Our accumulator and
    // spatial filter would hand it a signal whose low-frequency structure it cannot
    // remove (a denoiser reads smooth error as lighting), so zero them both and let
    // the network do the job it exists for. Gated on RayReconEnabled(), not on the
    // cvar alone: with RR off this would simply un-denoise the frame.
    const bool rrOwnsDenoise = (ps_r_dlss_rr_raw != 0) && VK::Dlss::RayReconEnabled();
    rp.temporal[0] = rrOwnsDenoise ? 0.f : _max(0.f, _min(0.98f, ps_r_gather_temporal));
    rp.temporal[1] = mvReady ? 1.f : 0.f;
    rp.temporal[2] = s_histValid ? 1.f : 0.f;
    rp.temporal[3] = rrOwnsDenoise ? 0.f : float(_max(0, _min(3, ps_r_gather_radius)));
    // The SH's own history weight. Never BELOW the radiance's (the shader takes the
    // max), and zero on the RR path for the same reason the radiance's is: there the
    // network owns the denoise and wants the rays raw.
    rp.zp[3] = rrOwnsDenoise ? 0.f : _max(0.f, _min(0.99f, ps_r_gather_sh_temporal));
    // The two weight knobs. Both default to the shipped behaviour, so the binary is
    // unchanged until somebody types one of them - which is the only way the A/B
    // means anything.
    rp.weights[0] = _max(0.f, _min(1.f, ps_r_gather_hitgate));
    rp.weights[1] = _max(0.f, _min(1.f, ps_r_gather_plane));
    // ---- the reprojection test, and why the rotation is a CPU scalar ----------
    // A static surface z0 away this frame stood z0 + dot(curEye - prevEye, fwd)
    // away last frame — one number for the whole screen, because the first-order
    // term of a translation along the view axis does not depend on the pixel.
    //
    // ⚠ ROTATION DOES depend on the pixel, and ignoring it entirely would make a
    // fast turn reject the whole frame: at the screen edge a turn of theta shifts
    // the view depth of a static point by up to z * tan(fovX/2) * theta. That is a
    // RELATIVE error with no pixel term left in it, so it belongs in the tolerance
    // rather than in a per-pixel reconstruction — one dot product here instead of
    // a prev-view matrix and a world-position rebuild in every invocation.
    {
        const bool prevCamValid = s_prevVPvalid && RV().frameIndex == s_prevVPframe + 1;
        const float tolBase = _max(0.f, _min(1.f, ps_r_gather_disocc));
        if (tolBase <= 0.f || !prevCamValid) {
            // No tolerance, or no camera to measure against: leave the test off and
            // behave exactly as the pass did before it existed.
            rp.weights[2] = 0.f;
            rp.weights[3] = 0.f;
        } else {
            Fvector d; d.sub(RV().vCameraPosition, s_prevCamPos);
            rp.weights[3] = d.dotproduct(pt.dir);
            const float cosT  = _max(-1.f, _min(1.f, s_prevCamDir.dotproduct(pt.dir)));
            const float theta = acosf(cosT);
            rp.weights[2] = tolBase + pt.tanX * theta;
        }
    }
    // The progressive alpha. The saturation point is 1/(1-alpha) rather than a
    // cvar: past it n/(n+1) is already above alpha and the min() in the shader
    // picks alpha anyway, so any larger number would only cost R32F precision.
    rp.hist[0] = rrOwnsDenoise ? 0.f : _max(0.f, _min(1.f, ps_r_gather_adapt));
    rp.hist[1] = (rp.temporal[0] < 0.999f) ? (1.f / (1.f - rp.temporal[0])) : 1024.f;
    // Spatial firefly rejection (r_gather_ffreject). Off when Ray Reconstruction
    // owns the denoise, for the same reason the temporal weight and the filter
    // radius above are: that path deliberately hands the raw estimate over intact,
    // and a clip applied on the way out would be one this pass cannot then measure.
    // The per-ray ceiling (r_gather_clamp) still applies in either case — it lives
    // in the trace, where the tail is actually born.
    rp.hist[2] = rrOwnsDenoise ? 0.f : _max(0.f, _min(8.f, ps_r_gather_ffreject));
    // The normal weight's exponent (r_gather_nrmw). Zeroed on the RR path with the
    // rest of the spatial pass — there the filter does not run at all.
    rp.hist[3] = rrOwnsDenoise ? 0.f : _max(0.f, _min(128.f, ps_r_gather_nrmw));
    {
        const int z = VK::Prof::ZoneBegin(cmd, "Gather/Resolve");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_resolvePipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_resolvePipeLayout, 0, 1,
                                &s_resolveSets[rIdx], 0, nullptr);
        vkCmdPushConstants(cmd, s_resolvePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
        vkCmdDispatch(cmd, (s_extent.width + 7) / 8, (s_extent.height + 7) / 8, 1);
        VK::Prof::ZoneEnd(cmd, z);
    }

    // Storage writes -> sampled reads by the forward receivers, both in GENERAL.
    // The layout does not change, so the layout-derived barrier would be wrong here.
    VK::MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    s_hasResult = true;
    // This frame's matrix + Z become next frame's cache. Stamped with the frame
    // number so the lookup can tell "one frame old" from "old".
    s_prevVP      = viewProj;
    s_prevCamPos  = RV().vCameraPosition;
    s_prevCamDir  = pt.dir;
    s_prevVPframe = RV().frameIndex;
    s_prevVPvalid = true;
    VK::Prof::ZoneEnd(cmd, zone);

    // One line, once, naming what the pass is actually doing. Without it "the
    // gather is on" and "the gather is on and tracing nothing" look identical.
    static u32 s_lastRays = 0xFFFFFFFFu;
    const u32 raysNow = u32(gp.zp[2]) | (ps_r_gather_foliage ? 0x100u : 0u) | (cacheW > 0.f ? 0x200u : 0u)
                      | (s_shOn ? 0x400u : 0u);
    if (s_lastRays != raysNow) {
        s_lastRays = raysNow;
        Msg("[VK Gather] %u ray(s)/pixel at %ux%u, reach %.1f m, cutoff %.0f m, foliage %s, screen cache %s, SH %s",
            u32(gp.zp[2]), s_extent.width, s_extent.height, rayLen, cutoff,
            ps_r_gather_foliage ? "traced" : "skipped",
            cacheW > 0.f ? "on" : (ps_r_gather_scache > 0.f ? "WANTED BUT NO HISTORY" : "off"),
            s_shOn ? "2-band (per-pixel probe)" : "off (one colour per pixel)");
    }
}

void DrawDebug(FrameContext& ctx)
{
    if (ps_r_gather_debug <= 0 || s_dbgPipe == VK_NULL_HANDLE) return;
    if (!s_hasResult || !s_resView[s_cur] || !s_rawView) return;

    const u32 parity = s_cur & 1u;
    if (s_boundDbg[parity] != s_resView[s_cur]) {
        VK::DescriptorWriter(s_dbgSets[parity])
            .ImageSampler(0, s_resView[s_cur],   s_sampler, VK_IMAGE_LAYOUT_GENERAL)
            .ImageSampler(1, s_rawView,          s_sampler, VK_IMAGE_LAYOUT_GENERAL)
            // Modes 4/5 read this; with the SH off it is the 1x1 image and those
            // two views are meaningless, which is what the log line above says.
            .ImageSampler(2, s_shResView[s_cur], s_sampler, VK_IMAGE_LAYOUT_GENERAL)
            // ⚠ s_cur, not s_cur^1: the resolve has already run this frame, so the
            // count that describes the image bound at 0 is the one it just wrote.
            .ImageSampler(3, s_ageView[s_cur], s_sampNear, VK_IMAGE_LAYOUT_GENERAL)
            .Flush();
        s_boundDbg[parity] = s_resView[s_cur];
    }

    DebugPush dp{};
    dp.params[0] = float(ps_r_gather_debug);
    dp.params[1] = 1.f / float(ctx.extent.width);
    dp.params[2] = 1.f / float(ctx.extent.height);
    dp.params[3] = _max(0.5f, ps_r_gather_len);
    // The same saturation point the resolve uses, so white in mode 6 means
    // "handed over to the exponential window" and not some display constant.
    {
        const float a = _max(0.f, _min(0.98f, ps_r_gather_temporal));
        dp.age[0] = (a < 0.999f) ? (1.f / (1.f - a)) : 1024.f;
    }

    const int zone = VK::Prof::ZoneBegin(ctx.cmd, "World/GatherDbg");
    RenderingBuilder rb(ctx.extent);
    rb.Color(ctx.colorView, VK_ATTACHMENT_LOAD_OP_LOAD);
    rb.BeginPlain(ctx.cmd);
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_dbgPipe);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_dbgPipeLayout, 0, 1,
                            &s_dbgSets[parity], 0, nullptr);
    vkCmdPushConstants(ctx.cmd, s_dbgPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dp), &dp);
    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(ctx.cmd);
    VK::Prof::ZoneEnd(ctx.cmd, zone);
}

}}  // namespace VK::GatherPass

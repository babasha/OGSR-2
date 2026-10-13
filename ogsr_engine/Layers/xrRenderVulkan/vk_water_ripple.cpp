// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — INTERACTIVE WATER RIPPLES. See vk_water_ripple.h.

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"       // VK::DescriptorWriter
#include "vk_water_ripple.h"
#include "vk_image.h"
#include "vk_buffer.h"
#include "vk_shaders.h"
#include "vk_barriers.h"
#include "vk_compute_util.h"
#include "vk_command_buffer.h"
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"
#include "../../xr_3da/IGame_Level.h"   // g_pGameLevel->ObjectSpace — the lid asks the collision model
#include <algorithm>
#include <cmath>
#include <cstring>

extern int   ps_r_wtr_sim;        // master enable
extern float ps_r_wtr_sim_size;   // tile edge in metres
extern float ps_r_wtr_sim_damp;   // velocity retention per step
extern float ps_r_wtr_sim_speed;  // stiffness (wave speed); > 0.5 goes unstable
extern int   ps_r_wtr_sim_pools;  // bound the field by the pool mask (0 = one sheet)
extern float ps_r_wtr_sim_shore;  // shore absorption per dry side (1 = reflecting)
extern float ps_r_wtr_sim_reach;  // metres of full-strength ripple around the player
extern int   ps_r_wtr_sim_lid;    // 1 = build the lid from the collision model (0 = old rasterizer)
extern int   ps_r_wtr_sim_lid_rays;   // ray budget per frame while the grid fills
extern int   ps_r_wtr_sim_depth;      // 1 = wave speed follows the depth map
extern float ps_r_wtr_sim_depth_ref;  // depth at which the wave runs at full speed
extern float ps_r_wtr_sim_bed;        // bottom friction as the depth goes to zero
extern float ps_r_wtr_wet_dry;        // seconds for a wetted surface to dry out
extern float ps_r_wtr_wet_lift;       // capillary rise above the line the water touched
extern float ps_r_wtr_sim_height;     // the field's DRAWN displacement gain
extern float ps_r_wtr_disp;           // metres of geometry displaced per unit wave height
extern float ps_r_wtr_fetch;          // 0 = no fetch gating at all (open water everywhere)
extern int   ps_r_wtr_fetch_local;    // 1 = measure each pool's own span from the mask

namespace VK { namespace WaterRipple {

namespace {

constexpr u32 kTexels    = 512;                 // 512² — 12.5 cm/texel over a 64 m tile
constexpr u32 kMaxSplats = 64;                  // per frame; excess is dropped
constexpr VkFormat kFmt  = VK_FORMAT_R16G16_SFLOAT;
// Pool mask: the water SURFACE HEIGHT per texel, R32F. Half floats would quantize
// to ~12 cm at the world heights this game uses, and the mask is compared against
// a 35 cm same-pool threshold — that is far too close to the quantum.
constexpr VkFormat kMaskFmt = VK_FORMAT_R32_SFLOAT;
// The wetness map and the live-surface map both store WORLD HEIGHTS, and for the
// same reason as the mask they cannot be half floats: fp16 steps by 6 cm at y=64
// and 12 cm at y=128, which is the entire error budget of a waterline pinned to
// the wave. It shows up as a wet edge that climbs a beach in stairs.
constexpr VkFormat kWetFmt  = VK_FORMAT_R32G32_SFLOAT;
// The live-surface map carries a THIRD height: the still sheet level. The shore
// break is a function of the still water depth, and the pool mask cannot supply
// it — its clearance rule discards exactly the strip where the bank stands a few
// centimetres out of the water, which is the swash zone. This map is written
// there (see water_mask.frag), so it is the only place the depth is knowable
// across the whole shore.
constexpr VkFormat kSurfFmt = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr float    kDry     = -10000.f;         // "no water in this texel"
constexpr float    kNoLid   =  10000.f;         // "nothing above the water here"
constexpr float    kLidGap  =  0.35f;           // less headroom than this = water under a floor

// Two vec4s to match the shader's std430 struct: the second carries the source
// height (see Splat() in the header for why that is not optional).
struct Splat4 { float x, z, r, s;  float y, _pad0, _pad1, _pad2; };
static_assert(sizeof(Splat4) == 32, "must match `struct Splat` in water_ripple.comp.glsl");

bool             s_inited = false, s_failed = false;
VkImage          s_img[2]{};
VmaAllocation    s_alloc[2]{};
VkImageView      s_view[2]{};
VkImage          s_mask      = VK_NULL_HANDLE;   // pool mask (surface height / dry)
VmaAllocation    s_maskAlloc = nullptr;
VkImageView      s_maskView  = VK_NULL_HANDLE;
bool             s_maskValid = false;            // rasterized at least once this frame
// LID map: height of the lowest solid surface ABOVE the water in each texel, or
// kNoLid where the water is open. This is what separates a puddle from the same
// sheet running under a floor — see water_lid.frag.glsl.
VkImage          s_lid       = VK_NULL_HANDLE;
VmaAllocation    s_lidAlloc  = nullptr;
VkImageView      s_lidView   = VK_NULL_HANDLE;
// DEPTH map: metres of water between the surface and the bottom, from the same
// CDB sweep as the lid (one ray down instead of up). Wave speed follows it.
VkImage          s_depth      = VK_NULL_HANDLE;
VmaAllocation    s_depthAlloc = nullptr;
VkImageView      s_depthView  = VK_NULL_HANDLE;
// SHORE WETNESS: what the water has TOUCHED, and how long ago.
//   .r = the highest world Y this column has been wetted up to (metres)
//   .g = 1 right after contact, decaying with time = drying out
// The world shading reads it so a bank, a wall or a heap of rubble standing in
// water is dark and glossy up to the waterline and dries once the water leaves.
// Same tile as the ripple field, so it scrolls and snaps with everything else.
// ⚠ PING-PONG, like the field — and for exactly the same reason. The scroll reads
// this texel's PREVIOUS position (p = c + shift) and writes its current one, and
// in a single dispatch a neighbouring workgroup can overwrite p before we read it.
// Standing still shift is zero, p == c, and the race cannot happen; the moment you
// walk it smears the map ALONG THE DIRECTION OF TRAVEL — which is what "the
// wetness creeps sideways" and "it follows me a couple of metres" both were.
VkImage          s_wet[2]{};
VmaAllocation    s_wetAlloc[2]{};
VkImageView      s_wetView[2]{};
// LIVE SURFACE + GROUND, written by the pool-mask pass (water_mask.frag, the
// WATER_MASK_SURF variant): .r = the water surface as it is DRAWN this frame,
// .g = the ground in this column. The two inputs the wetness needs to answer
// "did the water actually get here" instead of "is this within 35 cm of the
// waterline in plan view".
VkImage          s_surf      = VK_NULL_HANDLE;
VmaAllocation    s_surfAlloc = nullptr;
VkImageView      s_surfView  = VK_NULL_HANDLE;
bool             s_surfValid = false;            // filled by the mask pass this frame
// LOCAL FETCH: how many metres of open water surround each column, out of the
// pool mask. The wave machinery has always gated its octaves on a fetch; what it
// was handed was the bounding box of the water VISUAL, and a level's water is one
// visual — so the 2 m circle inside a well in Cordon's village was told it sat in
// a 108 m basin and drew the river's swell, 37 cm of crest in 22 cm of water.
// Coarse on purpose: this is "how big is this pool", not a shoreline. 0.5 m per
// texel is finer than the smallest body the wave can tell apart anyway.
constexpr u32    kFetchTexels = 128;
constexpr float  kFetchReach  = 32.f;            // look this far; reaching it = "open"
constexpr float  kFetchStep   = 0.25f;           // walk step in metres
VkImage          s_fetch      = VK_NULL_HANDLE;
VmaAllocation    s_fetchAlloc = nullptr;
VkImageView      s_fetchView  = VK_NULL_HANDLE;
VkDescriptorSetLayout s_fetchSetLayout = VK_NULL_HANDLE;
VkDescriptorPool s_fetchPool   = VK_NULL_HANDLE;
VkDescriptorSet  s_fetchSet    = VK_NULL_HANDLE;
VkPipelineLayout s_fetchLayout = VK_NULL_HANDLE;
VkPipeline       s_fetchPipe   = VK_NULL_HANDLE;
// ⚠ Must match `PC` in water_fetch.comp.glsl. Scalars only — a vec2 in a push
// block is std430-aligned to 8 bytes and silently shifts every field after it,
// which has cost this renderer a whole debugging session once already.
struct FetchPush {
    float dry, mpt, reach, stepM;
    s32   maskTexels, fetchTexels, enable, pad;
};
static_assert(sizeof(FetchPush) == 32, "must match the push block in water_fetch.comp.glsl");
u32              s_cur = 0;                     // index written by the LAST dispatch
VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
VkDescriptorPool s_pool  = VK_NULL_HANDLE;
VkDescriptorSet  s_set[2]{};                    // one per ping-pong direction
VkPipelineLayout s_layout = VK_NULL_HANDLE;
VkPipeline       s_pipe   = VK_NULL_HANDLE;
VkSampler        s_sampler = VK_NULL_HANDLE;
VkSampler        s_pointSampler = VK_NULL_HANDLE;   // heights must not be filtered
CVulkanBuffer    s_splatBuf;
bool             s_first  = true;               // images still UNDEFINED
// ⚠s_fetch is the ONE image of this module a GRAPHICS pass samples directly, and
// the only thing that ever moves it out of UNDEFINED is the s_first clear inside
// Dispatch() — which returns early when r_wtr_sim is 0. With the sim off (or on
// the frames before its first dispatch) Pass_Water still bound the view and
// declared it GENERAL, so the draw sampled an UNDEFINED image: caught 16-08 as
// VUID-vkCmdDraw-None-09600 [WaterLocalFetch]. FetchView() now stays NULL until
// the clear has actually run, which routes the consumer down the fallback it
// already has ("no local answer, keep the visual's own fetch").
bool             s_fetchReady = false;
// Wave speed and decay are PER STEP, so a step-per-frame sim runs almost twice
// as fast at 106 fps as at 60 and settles in half the time. Accumulate real
// time and step at a fixed rate instead: the pond then behaves the same on
// every machine and at every frame rate.
constexpr float  kStepHz  = 60.0f;
float            s_accum  = 0.f;
// Steps this frame, earned in PrepareTile — where the anchor is decided, because
// the anchor may only move on a frame whose step can carry the images with it.
u32              s_steps  = 0;
u32              s_splatsSeen = 0;              // diagnostics
float            s_lastLog    = 0.f;

// Tile anchor: world XZ of texel (0,0), snapped to a whole texel so the scroll
// is an exact integer shift.
float s_originX = 0.f, s_originZ = 0.f;
bool  s_haveOrigin = false;
// Scroll for THIS frame, computed by PrepareTile and consumed by Dispatch. It
// used to be derived inside Dispatch, but the mask has to be rasterized in tile
// space BEFORE the step runs, so the anchor must be settled first.
s32   s_shiftX = 0, s_shiftZ = 0;
u32   s_tileFrame = 0xFFFFFFFFu;   // frame the anchor was last advanced on

xr_vector<Splat4> s_pending;

// ---- audit (r_wtr_audit) --------------------------------------------------
// Staging copies of the mask and the field, read back a few frames later so the
// GPU is certainly done with them. Debug-only path: allocated on first use.
CVulkanBuffer s_auMask, s_auField, s_auLid, s_auWet, s_auSurf;
int  s_auState = 0;     // 0 = idle, >0 = frames until the copy is safe to read
bool s_auWant  = false;

// ---- CDB LID: not "where is the water" but "where can it be SEEN" ----------
// The mask answers the first question and cannot answer the second, and the
// second is the one that separates puddles. Level water is a couple of huge flat
// sheets that dive under the buildings; most of a cellar's floor is that same
// sheet running centimetres BELOW the concrete — invisible, and continuous with
// the visible patches. So "I jump in one puddle and the others ripple" was never
// a leak: the audit measured one 19.9 x 13.1 m slab surfacing in several places,
// and the wave was crossing under the floor between them, entirely legally.
//
// The separating fact is the FLOOR, and the water pass cannot see floors. After
// pool compaction the level's statics are drawn only by the GPU-driven paths, so
// the CPU queue the lid rasterizer walks holds a handful of leftovers — the log
// said "lid map: 5 static(s)" and that number IS the bug. Asking the render queue
// where the world is became the wrong question the day the world moved paths.
//
// Ask the authority that cannot move instead: the COLLISION MODEL. One short ray
// straight up from the water surface; anything solid within kLidGap means this
// texel is roofed, so no wave may live there. Independent of which path draws the
// world — which is precisely the property the rasterized version lacked.
//
// Cost is bounded by REUSE, not by budget alone: the tile anchor is snapped to
// whole texels, so a texel resolved once stays resolved while the window scrolls
// over it. Only the newly uncovered band needs rays — walking at 3 m/s uncovers
// ~24 texel rows a second, a couple of hundred rays a frame in the steady state.
constexpr u32   kLidRings = 3;        // staging ring: the GPU may still be reading last frame's
constexpr float kDepthMax = 8.0f;     // how far down we bother looking for a bottom
xr_vector<float> s_lidCpu;            // lid height per texel, in CURRENT tile space
xr_vector<float> s_depthCpu;          // water column depth (m); kDepthMax = "deep / unknown"
xr_vector<u8>    s_lidDone;           // 0 = never resolved; the shader then sees "open and deep"
CVulkanBuffer    s_lidUp[kLidRings];  // host-visible upload staging
u32              s_lidRing = 0;
CVulkanBuffer    s_lidRb;             // mask readback — a ray needs the water height to start at
int   s_lidRbState = 0;               // >0 = frames until the snapshot is safe to read
// The snapshot has landed and waits for its rays. They are a MAIN-thread job (ObjectSpace reads the live level), so
// LidTick (record side) only raises this and LidExtract (PREPARE phase) spends it. The two never run at once: the
// extract of a frame starts after the render thread has finished the previous one.
// TWO-SLOT PIPELINE: LidTick runs on the render thread (frame N) while LidExtract runs on the main thread (extract of N+1),
// so the old "they never run at once" no longer holds. s_lidMx guards the CPU grid (s_lidCpu/s_depthCpu/s_lidDone) and its
// bookkeeping; the pending flag is an atomic hand-off that STAYS raised until the rays have finished reading s_lidRb, so the
// record side cannot start the next snapshot copy into it meanwhile.
std::mutex s_lidMx;
std::atomic<bool> s_lidConsumePending{false};
float s_lidOriginX = 0.f, s_lidOriginZ = 0.f;   // the origin the CPU grid is anchored to (set by LidTick, under s_lidMx)
float s_lidRbOX = 0.f, s_lidRbOZ = 0.f, s_lidRbMpt = 0.f;   // the tile the snapshot belongs to
float s_lidMpt  = 0.f;                // metres/texel the grid was built at
const void* s_lidLevel = nullptr;     // grid belongs to THIS level; a new one invalidates it
bool  s_lidDirty = false, s_lidUpFirst = true;
u32   s_lidRaysDone = 0, s_lidBuried = 0, s_lidOpenN = 0;   // diagnostics
double s_lidDepthSum = 0.0; u32 s_lidDepthN = 0, s_lidShallowN = 0;  // ...and the bottom, OPEN water only
float s_lidLogT = 0.f, s_lidWhereT = 0.f;
bool  s_lidReported = false;

// ⚠⚠ MUST match water_ripple.comp.glsl byte for byte, and std430 does NOT lay a
// struct out the way C++ does: a vec2 takes 8-byte alignment there and 4 here.
// `camXZ` was a vec2 and cost exactly that — the shader read it from @48 while
// this struct wrote it at @44, and took `reach` from @56, past the end of what
// was pushed. Everything below the first two vectors is a SCALAR for that reason.
// Verify with `spirv-dis water_ripple.comp.spv | grep Offset`, never by eye.
struct RipplePush {
    s32   shift[2];    // @0
    float origin[2];   // @8
    float mPerTexel;   // @16
    float damping;     // @20
    float stiffness;   // @24
    s32   splatCount;  // @28
    s32   usePools;    // @32  1 = the pool mask bounds the field
    float dryValue;    // @36  mask value meaning "no water in this texel"
    float shoreAbsorb; // @40  per dry side per step (1 = reflecting rim)
    float camX;        // @44  player position — ripples fade out beyond `reach`
    float camZ;        // @48
    float reach;       // @52  metres of full-strength ripple (0 = unlimited)
    s32   depthOn;     // @56  1 = wave speed follows the depth map
    float depthRef;    // @60  depth at which the wave runs at full speed (m)
    float bedFric;     // @64  extra per-step damping as the depth goes to zero
    float wetDry;      // @68  shore wetness retained per step (drying out)
    float wetLift;     // @72  capillary rise above the line the water touched (m)
    float depthMax;    // @76  ray range; a depth AT this value means "no bottom found"
    float wetSimH;     // @80  r_wtr_sim_height — the field's DRAWN displacement
};
static_assert(sizeof(RipplePush) == 84, "push block must stay in step with water_ripple.comp.glsl");

float MetresPerTexel() { return std::clamp(ps_r_wtr_sim_size, 8.f, 512.f) / float(kTexels); }

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;
    if (!g_ShaderManager) { s_failed = true; return false; }

    VkShaderModule cs = g_ShaderManager->Load("water_ripple.comp.spv");
    if (cs == VK_NULL_HANDLE) {
        Msg("![VK Ripple] water_ripple.comp.spv missing — interactive ripples off");
        s_failed = true; return false;
    }

    // TRANSFER_DST: the field must be CLEARED on first use, not just transitioned
    // — see Dispatch. Uninitialised noise here survives for the better part of a
    // minute at this damping and reads as "every puddle is already rippling".
    // TRANSFER_SRC as well: the audit copies the field back to the host, and an
    // image without the usage bit is a validation error even when the copy
    // happens to work on this driver.
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                  | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (u32 i = 0; i < 2; ++i) {
        if (!CreateImage2D(kFmt, { kTexels, kTexels }, usage, s_img[i], s_alloc[i], "WaterRipple")) {
            s_failed = true; return false;
        }
        s_view[i] = CreateImageView(s_img[i], kFmt);
        if (s_view[i] == VK_NULL_HANDLE) { s_failed = true; return false; }
    }

    // Pool mask. COLOR_ATTACHMENT because Pass_Water rasterizes the water
    // visuals into it from straight above; STORAGE because the step reads it.
    // SAMPLED as well: r_wtr_debug 7 shows the mask on the water surface, which
    // is the only way to tell "the mask is wrong" from "the mask is right and
    // the wave leaks anyway" without guessing.
    if (!CreateImage2D(kMaskFmt, { kTexels, kTexels },
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       s_mask, s_maskAlloc, "WaterRippleMask")) {
        s_failed = true; return false;
    }
    s_maskView = CreateImageView(s_mask, kMaskFmt);
    if (s_maskView == VK_NULL_HANDLE) { s_failed = true; return false; }

    // TRANSFER_DST too: with r_wtr_sim_lid the lid is not rasterized at all, it is
    // built on the CPU by ray-testing the collision model and uploaded.
    if (!CreateImage2D(kMaskFmt, { kTexels, kTexels },
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       s_lid, s_lidAlloc, "WaterRippleLid")) {
        s_failed = true; return false;
    }
    s_lidView = CreateImageView(s_lid, kMaskFmt);
    if (s_lidView == VK_NULL_HANDLE) { s_failed = true; return false; }

    // Depth map: CPU-built only, so no COLOR_ATTACHMENT — just storage + upload.
    if (!CreateImage2D(kMaskFmt, { kTexels, kTexels },
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       s_depth, s_depthAlloc, "WaterRippleDepth")) {
        s_failed = true; return false;
    }
    s_depthView = CreateImageView(s_depth, kMaskFmt);
    if (s_depthView == VK_NULL_HANDLE) { s_failed = true; return false; }

    // Shore wetness. SAMPLED as well as STORAGE: the world shaders read it
    // through the shared env set (with a POINT sampler — .r is a height with a
    // -10000 dry sentinel, and filtering that invents waterlines).
    for (u32 i = 0; i < 2; ++i) {
        if (!CreateImage2D(kWetFmt, { kTexels, kTexels },
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                           | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           s_wet[i], s_wetAlloc[i], "WaterShoreWet")) {
            s_failed = true; return false;
        }
        s_wetView[i] = CreateImageView(s_wet[i], kWetFmt);
        if (s_wetView[i] == VK_NULL_HANDLE) { s_failed = true; return false; }
    }

    // Live surface + ground. COLOR_ATTACHMENT because the pool-mask pass writes
    // it (it is the only pass that walks the water visuals and therefore the only
    // one that knows each body's fetch), STORAGE because the step reads it.
    if (!CreateImage2D(kSurfFmt, { kTexels, kTexels },
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       s_surf, s_surfAlloc, "WaterLiveSurface")) {
        s_failed = true; return false;
    }
    s_surfView = CreateImageView(s_surf, kSurfFmt);
    if (s_surfView == VK_NULL_HANDLE) { s_failed = true; return false; }

    // Local fetch. STORAGE (the measuring pass writes it) + SAMPLED (every stage
    // that evaluates the wave reads it). Its own resolution, not the tile's: the
    // question is how big a body of water is, and answering that at 12.5 cm would
    // cost 16x the taps for an answer that feeds a smoothstep.
    if (!CreateImage2D(kMaskFmt, { kFetchTexels, kFetchTexels },
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       s_fetch, s_fetchAlloc, "WaterLocalFetch")) {
        s_failed = true; return false;
    }
    s_fetchView = CreateImageView(s_fetch, kMaskFmt);
    if (s_fetchView == VK_NULL_HANDLE) { s_failed = true; return false; }

    // Sampled by the TESE and the FS; linear so the displacement is smooth
    // between texels, clamped so the border band reads as flat water.
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) { s_failed = true; return false; }

    // A GENUINELY point-filtered sampler for the HEIGHT maps. There was none in
    // this layer: ShadowMap::GetSampler is documented as "POINT" and is in fact
    // LINEAR (magFilter = minFilter = VK_FILTER_LINEAR), which is how a shoreline
    // ended up with a several-metre dead band — interpolating a -10000 "dry"
    // sentinel against a real water height produces nonsense, and no amount of
    // reading the comment would have shown it. Heights must never be filtered.
    VkSamplerCreateInfo pi{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    pi.magFilter = pi.minFilter = VK_FILTER_NEAREST;
    pi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    pi.addressModeU = pi.addressModeV = pi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(VulkanHW.m_Device, &pi, nullptr, &s_pointSampler) != VK_SUCCESS) { s_failed = true; return false; }

    s_splatBuf.Create(sizeof(Splat4) * kMaxSplats, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VMA_MEMORY_USAGE_AUTO_PREFER_HOST);

    // ⚠ SIZE THIS WITH THE BINDINGS. Every count below (the array, bindingCount,
    // the pool, the writes) has to move together; a stale one is a silent stack
    // smash, and this project has three of those on record.
    // 0/1 = ripple ping-pong, 2 = splats, 3 pool mask, 4 lid, 5 depth,
    // 6/7 = shore wetness SRC/DST, 8 = live surface + ground. Two sets (one per direction).
    constexpr auto kImg = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (!VK::MakeDescriptorSets({ kImg, kImg, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  kImg, kImg, kImg, kImg, kImg, kImg },
                                2, s_setLayout, s_pool, s_set,
                                VK_SHADER_STAGE_COMPUTE_BIT, "Water.Ripple")) {
        s_failed = true; return false;
    }

    // Set d reads image[d^1] and writes image[d].
    for (u32 d = 0; d < 2; ++d) {
        VK::DescriptorWriter(s_set[d])
            .StorageImage (0, s_view[d ^ 1])          // read the other half
            .StorageImage (1, s_view[d])              // write this one
            .StorageBuffer(2, s_splatBuf.GetHandle(), sizeof(Splat4) * kMaxSplats)
            .StorageImage (3, s_maskView)
            .StorageImage (4, s_lidView)
            .StorageImage (5, s_depthView)
            .StorageImage (6, s_wetView[d ^ 1])
            .StorageImage (7, s_wetView[d])
            .StorageImage (8, s_surfView)
            .Flush();
    }

    s_layout = MakePipelineLayout({ s_setLayout }, sizeof(RipplePush));
    if (s_layout == VK_NULL_HANDLE) { s_failed = true; return false; }
    s_pipe = CreateComputePipeline(cs, s_layout, "Water.Ripple");
    if (s_pipe == VK_NULL_HANDLE) { s_failed = true; return false; }

    // ---- local fetch --------------------------------------------------------
    // OPTIONAL, and it fails soft on purpose: without it the image is simply left
    // cleared to zero, zero means "no local answer", and every consumer keeps the
    // per-visual fetch it used before. A missing .spv costs the well its fix, not
    // the water its wave.
    if (VkShaderModule fs = g_ShaderManager->Load("water_fetch.comp.spv")) {
        constexpr auto kImg2 = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (VK::MakeDescriptorSets({ kImg2, kImg2 }, 1, s_fetchSetLayout, s_fetchPool, &s_fetchSet,
                                   VK_SHADER_STAGE_COMPUTE_BIT, "Water.Fetch")) {
            VK::DescriptorWriter(s_fetchSet)
                .StorageImage(0, s_maskView)
                .StorageImage(1, s_fetchView)
                .Flush();
            s_fetchLayout = MakePipelineLayout({ s_fetchSetLayout }, sizeof(FetchPush));
            if (s_fetchLayout != VK_NULL_HANDLE)
                s_fetchPipe = CreateComputePipeline(fs, s_fetchLayout, "Water.Fetch");
        }
    }
    Msg("[VK Water] local fetch: %s", s_fetchPipe != VK_NULL_HANDLE
        ? "on (each pool's own span, measured from the mask)"
        : "OFF (water_fetch.comp.spv missing) — every pool keeps its visual's bbox fetch");

    Msg("[VK Ripple] init OK — %u² tile, %.2f m/texel", kTexels, MetresPerTexel());
    return true;
}

// Minimal uncompressed 24-bpp TGA (BGR, bottom-up) — same shape the engine's own
// screenshots use, so the session's tga2png converter reads it as-is.
void WriteTGA(const char* path, const u8* bgr, u32 w, u32 h)
{
    // Alias and name are SEPARATE arguments — pasting them together writes a file
    // literally called "$app_data_root$wtr_pools.tga" next to the exe.
    IWriter* W = FS.w_open("$app_data_root$", path);
    if (!W) { Msg("![VK Ripple] audit: cannot open %s", path); return; }
    u8 hdr[18] = {};
    hdr[2] = 2;                                  // uncompressed true-colour
    hdr[12] = u8(w & 0xFF); hdr[13] = u8(w >> 8);
    hdr[14] = u8(h & 0xFF); hdr[15] = u8(h >> 8);
    hdr[16] = 24;
    W->w(hdr, sizeof(hdr));
    for (u32 y = 0; y < h; ++y)                  // bottom-up
        W->w(bgr + size_t(h - 1 - y) * w * 3, w * 3);
    FS.w_close(W);
}

// Flood-fill the mask into connected pools and report them. THE question this
// whole arc kept guessing at: puddles that look separate on screen may be one
// sheet joined behind a wall, and then a wave crossing between them is correct
// behaviour that no mask can forbid.
void RunAudit()
{
    const u32 N = kTexels;
    const float* mask = (const float*)s_auMask.Map();
    if (!mask) { Msg("![VK Ripple] audit: mask map failed"); return; }
    // The audit has to apply the SAME wetness test the sim does, lid included —
    // otherwise it reports the mask's view of the world (one big sheet) while the
    // sim is already working with a different one.
    const float* lid = (const float*)s_auLid.Map();
    auto wet = [&](u32 i) {
        if (mask[i] <= kDry + 1.f) return false;
        return !lid || (lid[i] - mask[i]) >= kLidGap;
    };
    u32 wetTexels = 0, buried = 0;
    for (u32 i = 0; i < N * N; ++i) {
        if (mask[i] <= kDry + 1.f) continue;
        if (wet(i)) ++wetTexels; else ++buried;
    }
    Msg("[VK Ripple] AUDIT: %u wet texel(s), %u buried under a floor (%.0f%% of the sheet hidden)",
        wetTexels, buried, (wetTexels + buried) ? 100.0 * buried / double(wetTexels + buried) : 0.0);

    xr_vector<s32> comp(size_t(N) * N, -1);
    xr_vector<u32> stack;
    struct Pool { u32 texels; float y; u32 x0, x1, z0, z1; };
    xr_vector<Pool> pools;
    const float mpt = MetresPerTexel();

    for (u32 i = 0; i < N * N; ++i) {
        if (comp[i] >= 0 || !wet(i)) continue;
        const s32 id = (s32)pools.size();
        Pool p{ 0, mask[i], N, 0, N, 0 };
        stack.clear(); stack.push_back(i); comp[i] = id;
        while (!stack.empty()) {
            const u32 cur = stack.back(); stack.pop_back();
            const u32 cx = cur % N, cz = cur / N;
            ++p.texels;
            p.x0 = _min(p.x0, cx); p.x1 = _max(p.x1, cx);
            p.z0 = _min(p.z0, cz); p.z1 = _max(p.z1, cz);
            const int dx[4] = { -1, 1, 0, 0 }, dz[4] = { 0, 0, -1, 1 };
            for (int k = 0; k < 4; ++k) {
                const int nx = int(cx) + dx[k], nz = int(cz) + dz[k];
                if (nx < 0 || nz < 0 || nx >= int(N) || nz >= int(N)) continue;
                const u32 ni = u32(nz) * N + u32(nx);
                if (comp[ni] >= 0 || !wet(ni)) continue;
                if (_abs(mask[ni] - p.y) > 0.35f) continue;      // same test the sim uses
                comp[ni] = id; stack.push_back(ni);
            }
        }
        pools.push_back(p);
    }

    // ---- SHORE WETNESS: is the map being WRITTEN at all? -------------------
    // Three fixes in a row failed to make reeds and a log look wet, all of them
    // reasoned rather than measured. This splits the chain in half: a populated
    // map means the fault is downstream (binding / UBO / shader), an empty one
    // means it is upstream (mask / lid / touch) — and no amount of staring at
    // the code was going to say which.
    if (const float* wetRaw = (const float*)s_auWet.Map()) {
        u32   nWet = 0;
        float yLo = 1e9f, yHi = -1e9f, gMax = 0.f;
        for (u32 i = 0; i < N * N; ++i) {
            const float wy = wetRaw[i * 2 + 0];
            const float g  = wetRaw[i * 2 + 1];
            if (g > 0.002f) { ++nWet; yLo = _min(yLo, wy); yHi = _max(yHi, wy); gMax = _max(gMax, g); }
        }
        Msg("[VK Ripple] AUDIT: shore wetness -> %u wet texel(s) of %u, wetY [%.2f .. %.2f], peak g=%.3f",
            nWet, N * N, nWet ? yLo : 0.f, nWet ? yHi : 0.f, gMax);
        s_auWet.Unmap();
    }

    // ---- CONTACT INPUTS: is the sim being TOLD where the water and the ground
    // are? The wetness is now a comparison between two heights, and a comparison
    // that always comes out one way is indistinguishable from a broken one. This
    // says how many columns have a live surface at all, how many have a ground to
    // test it against, and how many of those are in contact right now.
    if (const float* surfRaw = (const float*)s_auSurf.Map()) {
        u32 nLive = 0, nGround = 0, nContact = 0;
        double gapSum = 0.0;
        for (u32 i = 0; i < N * N; ++i) {
            const float live = surfRaw[i * 4 + 0], gy = surfRaw[i * 4 + 1];
            if (live <= kDry + 1.f) continue;
            ++nLive;
            if (gy <= kDry + 1.f) continue;
            ++nGround;
            gapSum += double(live - gy);
            if (live >= gy - 0.02f) ++nContact;
        }
        Msg("[VK Ripple] AUDIT: live surface -> %u texel(s) with water drawn, %u with a known ground, "
            "%u in CONTACT, mean (surface - ground) %.2f m",
            nLive, nGround, nContact, nGround ? (gapSum / double(nGround)) : 0.0);
        s_auSurf.Unmap();
    }

    // Report the pools big enough to matter, largest first.
    xr_vector<u32> order;
    for (u32 i = 0; i < pools.size(); ++i) if (pools[i].texels >= 16) order.push_back(i);
    std::sort(order.begin(), order.end(),
              [&](u32 a, u32 b) { return pools[a].texels > pools[b].texels; });
    Msg("[VK Ripple] AUDIT: %u connected pool(s) >=0.25 m2, %u total, tile (%.0f,%.0f) %.0f m",
        (u32)order.size(), (u32)pools.size(), s_originX, s_originZ, SizeMetres());
    for (u32 n = 0; n < order.size() && n < 8; ++n) {
        const Pool& p = pools[order[n]];
        Msg("[VK Ripple]   pool %u: %.1f m2, y=%.2f, span %.1f x %.1f m, at world (%.1f, %.1f)",
            n, p.texels * mpt * mpt, p.y,
            (p.x1 - p.x0 + 1) * mpt, (p.z1 - p.z0 + 1) * mpt,
            s_originX + (p.x0 + p.x1) * 0.5f * mpt, s_originZ + (p.z0 + p.z1) * 0.5f * mpt);
    }

    // Field energy: decaying or being pumped?
    const u16* fld = (const u16*)s_auField.Map();
    if (fld) {
        // RG16F: decode the half-float height (.r) cheaply — sign+exp+mantissa.
        double sum = 0.0; float peak = 0.f; u32 live = 0;
        for (u32 i = 0; i < N * N; ++i) {
            const u16 hbits = fld[i * 2];
            const u32 sign = (hbits >> 15) & 1, exp = (hbits >> 10) & 0x1F, man = hbits & 0x3FF;
            float v = 0.f;
            if (exp == 0)            v = std::ldexp(float(man), -24);
            else if (exp != 31)      v = std::ldexp(float(man | 0x400), int(exp) - 25);
            if (sign) v = -v;
            const float a = _abs(v);
            if (a > 0.0005f) { ++live; sum += a; }
            peak = _max(peak, a);
        }
        Msg("[VK Ripple] AUDIT: field |h| sum=%.3f m, peak=%.4f m, %u texel(s) above 0.5 mm",
            sum, peak, live);
        s_auField.Unmap();
    }

    // Pictures: pool map (a colour per body) and the field.
    xr_vector<u8> img(size_t(N) * N * 3);
    static const u8 pal[8][3] = { {60,220,60},{60,160,255},{255,200,60},{255,90,200},
                                  {120,255,230},{200,120,255},{255,140,60},{160,255,120} };
    for (u32 i = 0; i < N * N; ++i) {
        u8* px = &img[size_t(i) * 3];
        if (comp[i] < 0) { px[0] = px[1] = px[2] = 25; continue; }   // dry
        const u8* col = pal[comp[i] % 8];
        px[0] = col[2]; px[1] = col[1]; px[2] = col[0];              // BGR
    }
    WriteTGA("wtr_pools.tga", img.data(), N, N);
    if (lid) s_auLid.Unmap();
    s_auMask.Unmap();
    Msg("[VK Ripple] AUDIT: wrote wtr_pools.tga (%u^2, %.2f m/texel)", N, mpt);
}

// ---- CDB lid ---------------------------------------------------------------

void LidReset()
{
    s_lidCpu.assign(size_t(kTexels) * kTexels, kNoLid);
    s_depthCpu.assign(size_t(kTexels) * kTexels, kDepthMax);   // unknown = deep = full speed
    s_lidDone.assign(size_t(kTexels) * kTexels, 0);
    s_lidDirty    = true;
    s_lidRbState  = 0;
    s_lidConsumePending = false;
    s_lidRaysDone = s_lidBuried = s_lidOpenN = 0;
    s_lidDepthSum = 0.0; s_lidDepthN = s_lidShallowN = 0;
    s_lidReported = false;
}

// The window moved by an exact texel count, so everything it still covers is
// still known — carry that across and leave only the uncovered band unresolved.
// This is what makes the ray cost proportional to WALKING SPEED rather than to
// tile area: without it every frame would re-ask the collision model about a
// quarter of a million texels it had already answered.
void LidScroll(s32 dx, s32 dz)
{
    if (!dx && !dz) return;
    if (_abs(dx) >= s32(kTexels) || _abs(dz) >= s32(kTexels)) { LidReset(); return; }

    static xr_vector<float> tmpL, tmpDep;
    static xr_vector<u8>    tmpD;
    tmpL.assign(size_t(kTexels) * kTexels, kNoLid);
    tmpDep.assign(size_t(kTexels) * kTexels, kDepthMax);
    tmpD.assign(size_t(kTexels) * kTexels, 0);

    // new[c] = old[c + shift] — the very convention the field uses, so the grid
    // and the heightfield stay in step through the scroll.
    const s32 x0 = _max(0, -dx);
    const s32 x1 = _min(s32(kTexels), s32(kTexels) - dx);
    if (x0 < x1) {
        for (s32 z = 0; z < s32(kTexels); ++z) {
            const s32 oz = z + dz;
            if (oz < 0 || oz >= s32(kTexels)) continue;
            const size_t di = size_t(z)  * kTexels + size_t(x0);
            const size_t si = size_t(oz) * kTexels + size_t(x0 + dx);
            const size_t n  = size_t(x1 - x0);
            std::memcpy(&tmpL[di],   &s_lidCpu[si],   sizeof(float) * n);
            std::memcpy(&tmpDep[di], &s_depthCpu[si], sizeof(float) * n);
            std::memcpy(&tmpD[di],   &s_lidDone[si],  sizeof(u8)    * n);
        }
    }
    s_lidCpu.swap(tmpL);
    s_depthCpu.swap(tmpDep);
    s_lidDone.swap(tmpD);
    s_lidDirty = true;
}

// Spend the snapshot: every texel we have not resolved yet is either dry (free)
// or costs one ray. Budgeted, because the first fill covers the whole tile.
void LidConsume()
{
    const float mpt = MetresPerTexel();
    // The grid is keyed to a texel size: change r_wtr_sim_size and the world
    // underneath it rescales, so nothing already stored means anything.
    {
        std::lock_guard<std::mutex> g(s_lidMx);
        if (mpt != s_lidMpt || mpt != s_lidRbMpt) { s_lidMpt = mpt; LidReset(); return; }
    }

    const float* mask = (const float*)s_lidRb.Map();
    if (!mask) return;

    // Both anchors are snapped to the SAME texel grid, so the snapshot maps onto
    // this frame's tile by a whole-texel offset. Integer, not projected: it costs
    // nothing per texel, and it removes any chance of the two ends rounding to
    // different texels — the drift that would smear a pool rim by a texel a frame.
    s32 budget = _max(ps_r_wtr_sim_lid_rays, 0);
    const bool haveLevel = g_pGameLevel != nullptr;   // main thread (LidExtract): the collision model is safe to ray

    for (s32 sz = 0; sz < s32(kTexels); ++sz) {
        // ROW granularity: the render thread (LidTick: scroll, upload) waits for at most one row of rays, not the whole budget.
        // The mapping snapshot-texel -> grid-texel is re-read per row under the lock, so a scroll between two rows is harmless.
        std::lock_guard<std::mutex> rowLock(s_lidMx);
        const s32 offX = (s32)lroundf((s_lidRbOX - s_lidOriginX) / mpt);
        const s32 offZ = (s32)lroundf((s_lidRbOZ - s_lidOriginZ) / mpt);
        const s32 cz = sz + offZ;
        if (cz < 0 || cz >= s32(kTexels)) continue;
        for (s32 sx = 0; sx < s32(kTexels); ++sx) {
            const s32 cx = sx + offX;
            if (cx < 0 || cx >= s32(kTexels)) continue;
            const size_t ci = size_t(cz) * kTexels + size_t(cx);
            if (s_lidDone[ci]) continue;

            const float wy = mask[size_t(sz) * kTexels + size_t(sx)];
            if (wy <= kDry + 1.f) {                    // no water at all: nothing to roof
                s_lidDone[ci] = 1; s_lidCpu[ci] = kNoLid; s_depthCpu[ci] = kDepthMax;
                s_lidDirty = true;
                continue;
            }
            if (!haveLevel || budget <= 0) continue;   // retried on the next snapshot

            // Start just CLEAR of the surface so the sheet cannot be its own hit.
            // Erring this way on purpose: a ray started below the water would
            // report every texel roofed the moment level water turns out to be
            // collidable, and a lid that says "everything is buried" kills the
            // ripple entirely — the exact way an earlier mask attempt failed.
            // Missing a floor that sits within 3 cm of the surface only leaves a
            // rim texel live, which is the survivable direction to be wrong in.
            Fvector from; from.set(s_lidRbOX + (float(sx) + 0.5f) * mpt, wy + 0.03f,
                                   s_lidRbOZ + (float(sz) + 0.5f) * mpt);
            Fvector up;   up.set(0.f, 1.f, 0.f);
            const bool roofed = g_pGameLevel->ObjectSpace.RayTest(from, up, kLidGap, collide::rqtStatic, nullptr, nullptr);
            // Store a HEIGHT, not a flag: water_ripple.comp keeps its single test
            // (lid - surface < kLidGap) and nothing downstream learns a new rule.
            s_lidCpu[ci]  = roofed ? wy : kNoLid;

            // FLOOR, or something STANDING IN THE WATER? The up-ray cannot tell
            // them apart — a cellar floor over a buried sheet and a rock poking
            // through a river both put solid matter just above the surface. But
            // the surroundings can: a floor roofs its whole neighbourhood, while
            // a rock has open water around it. Four lateral probes at 60 cm; one
            // clear sky among them means this is an obstacle, not a lid.
            //
            // It matters because the two want OPPOSITE treatment. A rock must
            // still block waves (it is an obstacle) but its submerged side must
            // be WET — and being classed as a floor took both away.
            bool obstacle = false;
            if (roofed) {
                const float probe[4][2] = { { 0.6f, 0.f }, { -0.6f, 0.f }, { 0.f, 0.6f }, { 0.f, -0.6f } };
                for (int q = 0; q < 4 && !obstacle; ++q) {
                    Fvector pf; pf.set(from.x + probe[q][0], wy + 0.03f, from.z + probe[q][1]);
                    if (!g_pGameLevel->ObjectSpace.RayTest(pf, up, kLidGap, collide::rqtStatic, nullptr, nullptr))
                        obstacle = true;
                }
                budget -= 4;
                s_lidRaysDone += 4;
            }

            // DEPTH, from the same texel and the opposite direction. Start just
            // BELOW the surface so a collidable water sheet cannot be its own
            // bottom, and add the offset back so the number means what it says.
            float depth = kDepthMax;
            if (!roofed) {                       // roofed water carries no waves anyway
                collide::rq_result rr;
                Fvector fromD; fromD.set(from.x, wy - 0.02f, from.z);
                Fvector down;  down.set(0.f, -1.f, 0.f);
                if (g_pGameLevel->ObjectSpace.RayPick(fromD, down, kDepthMax, collide::rqtStatic, rr, nullptr))
                    depth = rr.range + 0.02f;
                ++s_lidRaysDone;                 // this one costs a trace too
                --budget;
                // Average OPEN water only. Roofed texels get the kDepthMax
                // sentinel, and folding those into the mean is not a rounding
                // error — it reported 2.63 m for water that is actually 0.36 m
                // deep, which is precisely the number this line exists to tell
                // you, and the one r_wtr_sim_depth_ref has to be set from.
                s_lidDepthSum += depth; ++s_lidDepthN;
                if (depth < 0.2f) ++s_lidShallowN;
            }
            // The obstacle flag rides in the SIGN of the depth. Depth is a length
            // and can never be negative, so the bit is free — no third image, no
            // format change, and the step just takes abs() before using it.
            s_depthCpu[ci] = obstacle ? -_max(depth, 0.01f) : depth;
            s_lidDone[ci] = 1;
            s_lidDirty    = true;
            if (roofed) ++s_lidBuried; else ++s_lidOpenN;
            ++s_lidRaysDone;
            --budget;
        }
    }
    s_lidRb.Unmap();
}

void LidUpload(VkCommandBuffer cmd)
{
    if (!s_lidDirty || s_lid == VK_NULL_HANDLE) return;
    const VkDeviceSize plane = sizeof(float) * VkDeviceSize(kTexels) * kTexels;
    CVulkanBuffer& buf = s_lidUp[s_lidRing];
    if (!buf.GetHandle()) {
        buf.Create(plane * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,   // lid, then depth
                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        if (!buf.GetHandle()) return;
    }
    u8* dst = (u8*)buf.Map();
    if (!dst) return;
    std::memcpy(dst,         s_lidCpu.data(),   size_t(plane));
    std::memcpy(dst + plane, s_depthCpu.data(), size_t(plane));
    buf.Unmap();

    const VkImage imgs[2] = { s_lid, s_depth };
    const VkImageLayout old = s_lidUpFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    s_lidUpFirst = false;
    for (u32 i = 0; i < 2; ++i) {
        if (imgs[i] == VK_NULL_HANDLE) continue;
        ImageBarrier(cmd, imgs[i], old, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy cp{};
        cp.bufferOffset     = plane * i;
        cp.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.imageExtent      = { kTexels, kTexels, 1 };
        vkCmdCopyBufferToImage(cmd, buf.GetHandle(), imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        ImageBarrier(cmd, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }

    // Ring, not one buffer: the copy of two frames ago may still be in flight,
    // and a torn lid is a wrong pool boundary rather than a dropped splat.
    s_lidRing  = (s_lidRing + 1) % kLidRings;
    s_lidDirty = false;
}

// One frame of the lid: carry what we know, spend the last snapshot, ask for a
// fresh one, upload. Called from Dispatch, which runs after Pass_Water has put
// this frame's mask into the image and left it in GENERAL.
void LidTick(VkCommandBuffer cmd, s32 shiftX, s32 shiftZ)
{
    std::lock_guard<std::mutex> lidLock(s_lidMx);   // the grid is shared with LidExtract (main thread)
    const void* lvl = (const void*)g_pGameLevel;
    if (lvl != s_lidLevel || s_lidCpu.size() != size_t(kTexels) * kTexels) {
        s_lidLevel = lvl;
        s_lidMpt   = MetresPerTexel();
        LidReset();          // a new level's floors have nothing to do with the old grid
    }

    LidScroll(shiftX, shiftZ);
    s_lidOriginX = s_originX; s_lidOriginZ = s_originZ;   // the grid now matches this frame's anchor

    if (!s_lidRb.GetHandle())
        s_lidRb.Create(sizeof(float) * kTexels * kTexels, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST);

    // The copy has certainly landed: hand the snapshot to the next extract's rays (LidExtract). It used to be spent
    // right here -- but this is the record side, on the render thread, where ObjectSpace may not be rayed, and the
    // guard that said so turned every lid ray off the day the render thread took every frame (03-10).
    if (s_lidRbState > 0 && --s_lidRbState == 0)
        s_lidConsumePending.store(true, std::memory_order_release);

    // The mask is the only place the water's HEIGHT per texel exists, and a ray
    // has to start there. Snapshot it whenever the last one has been spent.
    if (s_lidRbState == 0 && !s_lidConsumePending.load(std::memory_order_acquire) && s_maskValid && s_mask != VK_NULL_HANDLE && s_lidRb.GetHandle()) {
        VkBufferImageCopy cp{};
        cp.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.imageExtent      = { kTexels, kTexels, 1 };
        ImageBarrier(cmd, s_mask, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd, s_mask, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               s_lidRb.GetHandle(), 1, &cp);
        ImageBarrier(cmd, s_mask, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        s_lidRbOX  = s_originX; s_lidRbOZ = s_originZ; s_lidRbMpt = MetresPerTexel();
        s_lidRbState = 4;                     // frames before the copy is certainly done
    }

    LidUpload(cmd);

    // Say what the lid actually found. The rasterized version failed SILENTLY —
    // it drew 5 statics, buried nothing, and looked exactly like a lid that was
    // simply not needed. A count of roofed texels is the difference between
    // "the floors are accounted for" and "we are guessing again".
    // What the sim thinks is under the PLAYER. "My splashes do nothing" has one
    // more silent cause than the splat counter can see: the splat is queued on the
    // CPU and then rejected inside the step, because the texel it landed on reads
    // roofed or dry. The counter says 20 splats a second either way. This says
    // whether they had anywhere to land.
    if (RV().fTimeGlobal - s_lidWhereT > 3.f) {
        s_lidWhereT = RV().fTimeGlobal;
        const float mpt = MetresPerTexel();
        const s32 px = s32(std::floor((RV().vCameraPosition.x - s_originX) / mpt));
        const s32 pz = s32(std::floor((RV().vCameraPosition.z - s_originZ) / mpt));
        if (px >= 0 && pz >= 0 && px < s32(kTexels) && pz < s32(kTexels)) {
            const size_t pi = size_t(pz) * kTexels + size_t(px);
            Msg("[VK Ripple] under the player: %s, depth=%.2f m, resolved=%d",
                (s_lidCpu[pi] > kNoLid * 0.5f) ? "open water" : "ROOFED - splats are rejected here",
                s_depthCpu[pi], (int)s_lidDone[pi]);
        }
    }

    // Counting 262k flags every frame just to log would cost more than the rays,
    // so sample it about once a second.
    if (RV().fTimeGlobal - s_lidLogT > 1.f) {
        s_lidLogT = RV().fTimeGlobal;
        u32 unresolved = 0;
        for (u8 d : s_lidDone) if (!d) ++unresolved;
        if (!unresolved) {
            if (!s_lidReported) {
                s_lidReported = true;
                // Mean depth is the check that the DOWN ray found a real bottom.
                // If it comes back as kDepthMax the rays are missing the floor and
                // every texel is "deep" — the map would be built and meaningless,
                // which looks identical to working from the outside.
                Msg("[VK Ripple] lid(cdb): tile resolved - %u roofed texel(s), %u open, %u ray(s) cast; OPEN water mean depth %.2f m, %.0f%% under 20 cm  <-- set r_wtr_sim_depth_ref near this",
                    s_lidBuried, s_lidOpenN, s_lidRaysDone,
                    s_lidDepthN ? (s_lidDepthSum / double(s_lidDepthN)) : 0.0,
                    s_lidDepthN ? (100.0 * double(s_lidShallowN) / double(s_lidDepthN)) : 0.0);
            }
        } else {
            s_lidReported = false;
            static float s_pendT = 0.f;
            if (RV().fTimeGlobal - s_pendT > 5.f) {
                s_pendT = RV().fTimeGlobal;
                Msg("[VK Ripple] lid(cdb): %u texel(s) pending, %u roofed / %u open so far",
                    unresolved, s_lidBuried, s_lidOpenN);
            }
        }
    }
}

// ---- LOCAL FETCH ----------------------------------------------------------
// Measure every column's own body of water out of the pool mask, so the wave
// machinery stops being told that a well is a river.
//
// ⚠ ORDER. This reads the mask, which Pass_Water rasterizes BEFORE calling
// Dispatch, and it is read this same frame by the surfaces drawn AFTER it — so
// the only stage that gets a one-frame-old map is the mask pass itself, which is
// recorded earlier. That is fine and deliberate: the map answers "how big is this
// pool", and a pool does not change size in 16 ms.
//
// ⚠ And it runs BEFORE the step-count early-out below. The sim owes no step on a
// frame that has not accumulated one; the SURFACE still gets drawn on that frame,
// so its fetch cannot be allowed to go stale by a frame at a time.
void FetchTick(VkCommandBuffer cmd)
{
    if (s_fetchPipe == VK_NULL_HANDLE || s_fetchView == VK_NULL_HANDLE) return;

    FetchPush fp{};
    fp.dry         = kDry;
    fp.mpt         = MetresPerTexel();
    fp.reach       = kFetchReach;
    fp.stepM       = kFetchStep;
    fp.maskTexels  = (s32)kTexels;
    fp.fetchTexels = (s32)kFetchTexels;
    // Three ways this measurement must stand down, and all three mean the same
    // thing — "nobody is gating waves by basin size right now":
    //   the mask is not being drawn  -> there is nothing to measure and the image
    //                                   would hold whatever the last valid frame left;
    //   r_wtr_fetch 0                -> the user turned fetch gating off outright;
    //   r_wtr_fetch_local 0          -> the A/B switch for this map alone.
    // The shader then writes 0 everywhere, 0 means "no local answer", and every
    // consumer falls back to the per-visual fetch exactly as before.
    const bool live = s_maskValid && ps_r_wtr_sim_pools && ps_r_wtr_fetch > 0.f
                   && ps_r_wtr_fetch_local != 0;
    fp.enable = live ? 1 : 0;

    const u32 groups = (kFetchTexels + 7) / 8;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fetchPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fetchLayout, 0, 1, &s_fetchSet, 0, nullptr);
    vkCmdPushConstants(cmd, s_fetchLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fp), &fp);
    vkCmdDispatch(cmd, groups, groups, 1);

    // Read by the vertex/tessellation/fragment stages of the water pass later in
    // this same command buffer.
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

}  // anon namespace

void LidExtract()
{
    RT_ASSERT_MAIN();
    if (!s_lidConsumePending.load(std::memory_order_acquire)) return;
    LidConsume();   // takes s_lidMx itself, one row at a time
    s_lidConsumePending.store(false, std::memory_order_release);   // only now may the record side snapshot again
}

void Splat(float worldX, float worldZ, float radius, float strength, float srcY)
{
    if (s_pending.size() >= kMaxSplats) return;
    s_pending.push_back({ worldX, worldZ, radius, strength, srcY, 0.f, 0.f, 0.f });
    ++s_splatsSeen;
}

void RequestAudit() { s_auWant = true; }

void PrepareTile()
{
    if (!ps_r_wtr_sim) return;
    if (!Init())       return;
    // Idempotent per frame: Pass_Water calls this before drawing the mask, and
    // Dispatch would otherwise advance the anchor a second time and scroll the
    // field twice.
    if (s_tileFrame == RV().frameIndex) return;
    s_tileFrame = RV().frameIndex;

    // ⚠⚠⚠ THE STEPS ARE EARNED HERE, BEFORE THE ANCHOR MOVES, AND THAT ORDER IS
    // THE WHOLE POINT. The sim runs at a fixed 60 Hz off an accumulator, so a
    // frame above 60 fps earns NO step — and the scroll lives inside the step.
    // The anchor, however, used to advance every frame regardless: it moved, the
    // images did not, and the next frame measured its shift from the NEW anchor,
    // so the skipped scroll was never made up. The error ACCUMULATES for as long
    // as you keep walking.
    //
    // Everything that maps a texel to a world position goes through the anchor —
    // the wetness memory, the ripple field, the CPU lid/depth grid — so the whole
    // record slides along with the camera while the fresh marks land in the right
    // place. That is "I walk and the wet patch creeps after me": measured at this
    // machine's ~60 fps it is only centimetres a frame, but the wetness remembers
    // for r_wtr_wet_dry seconds (25), and centimetres a frame for twenty seconds
    // is a metre of smeared, travelling damp.
    //
    // ⭐ The invariant, stated once: THE TILE MAY ONLY MOVE ON A FRAME THAT CAN
    // MOVE ITS CONTENTS WITH IT. On a frame that earns no step the tile simply
    // stays where it is — one frame of lag, a couple of centimetres, and no debt
    // carried forward. Nothing else has to change: the mask is rasterized in tile
    // space AFTER this call, so it lands on the frozen anchor too.
    s_accum += std::clamp(RV().fTimeDelta, 0.f, 0.1f);
    s_steps = (u32)(s_accum * kStepHz);
    if (s_steps > 4) s_steps = 4;          // a hitch must not buy a hundred steps
    s_accum -= float(s_steps) / kStepHz;

    s_shiftX = s_shiftZ = 0;
    if (s_steps == 0) return;              // tile frozen: there is no scroll to lose

    const float mpt  = MetresPerTexel();
    const float half = 0.5f * mpt * float(kTexels);
    // Snap the anchor to a whole texel: the scroll then costs an integer
    // texel offset in the compute read, with no resampling and no drift.
    const Fvector& eye = RV().vCameraPosition;
    const float nx = std::floor((eye.x - half) / mpt) * mpt;
    const float nz = std::floor((eye.z - half) / mpt) * mpt;

    if (s_haveOrigin) {
        s_shiftX = (s32)lroundf((nx - s_originX) / mpt);
        s_shiftZ = (s32)lroundf((nz - s_originZ) / mpt);
    }
    s_originX = nx; s_originZ = nz; s_haveOrigin = true;
}

VkImage     MaskImage()    { return s_mask; }
VkImageView MaskView()     { return s_maskView; }
u32         MaskTexels()   { return kTexels; }
float       MaskDryValue() { return kDry; }
VkImage     LidImage()     { return s_lid; }
VkImageView LidView()      { return s_lidView; }
float       LidNoneValue() { return kNoLid; }
VkImage     SurfImage()    { return s_surf; }
VkImageView SurfView()     { return s_surfView; }
void        SetSurfValid(bool v) { s_surfValid = v; }
VkImageView FetchView()    { return s_fetchReady ? s_fetchView : VK_NULL_HANDLE; }

void EnsureFetchInitialized(VkCommandBuffer cmd)
{
    if (s_fetchReady || cmd == VK_NULL_HANDLE) return;
    if (s_fetch == VK_NULL_HANDLE) return;   // module not initialised — caller keeps its fallback
    VkClearColorValue zeroF{};
    VkImageSubresourceRange rng{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    ImageBarrier(cmd, s_fetch, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(cmd, s_fetch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroF, 1, &rng);
    ImageBarrier(cmd, s_fetch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    s_fetchReady = true;
}
u32         FetchTexels()  { return kFetchTexels; }
// Pass_Water reports whether it actually rasterized the mask this frame. If it
// did not (pipeline missing, pools disabled), the step runs the old unbounded
// way rather than reading an image with nothing in it and declaring the world dry.
void SetMaskValid(bool v)  { s_maskValid = v; }

void Dispatch(VkCommandBuffer cmd)
{
    if (!ps_r_wtr_sim || cmd == VK_NULL_HANDLE) { s_pending.clear(); return; }
    if (!Init())                                { s_pending.clear(); return; }

    PrepareTile();                       // no-op if Pass_Water already did it
    const float mpt = MetresPerTexel();
    const s32 shiftX = s_shiftX, shiftZ = s_shiftZ;

    // First use: both images are UNDEFINED, so they must be CLEARED, not merely
    // transitioned. The old comment here claimed the garbage "flushes out
    // immediately" — it does not. Damping is 0.9985 PER STEP at 60 Hz, so noise
    // decays by ~8% a second and stays visible for the better part of a minute,
    // and the debug field view showed exactly that: every puddle in the level
    // rippling from the moment the level loaded, with nothing having touched the
    // water. That is what "waves appear in the far puddles" really was.
    if (s_first) {
        s_first = false;
        VkClearColorValue zero{};
        VkImageSubresourceRange rng{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (u32 i = 0; i < 2; ++i) {
            ImageBarrier(cmd, s_img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, s_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rng);
            ImageBarrier(cmd, s_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        // The depth map has only one author (the CDB sweep), so with r_wtr_sim_lid
        // off nothing would ever write it and the shader would sample an UNDEFINED
        // image. Seed it "deep" — which is also the right fallback: unknown depth
        // means full wave speed, i.e. exactly how the sim behaved before.
        if (s_depth != VK_NULL_HANDLE) {
            VkClearColorValue deep{}; deep.float32[0] = kDepthMax;
            ImageBarrier(cmd, s_depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, s_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &deep, 1, &rng);
            ImageBarrier(cmd, s_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        for (u32 i = 0; i < 2; ++i) {   // dry everywhere, wetted up to nothing
            if (s_wet[i] == VK_NULL_HANDLE) continue;
            VkClearColorValue dry{}; dry.float32[0] = kDry; dry.float32[1] = 0.f;
            ImageBarrier(cmd, s_wet[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, s_wet[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dry, 1, &rng);
            ImageBarrier(cmd, s_wet[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        // The live-surface map has ONE author (the mask pass), which does not run
        // with pools off — so without this the step would hold a descriptor on an
        // UNDEFINED image. Cleared from UNDEFINED, which is legal whether or not
        // the mask pass has already written it this frame.
        if (s_surf != VK_NULL_HANDLE) {
            VkClearColorValue none{};
            none.float32[0] = kDry; none.float32[1] = kDry; none.float32[2] = kDry;
            ImageBarrier(cmd, s_surf, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, s_surf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &none, 1, &rng);
            ImageBarrier(cmd, s_surf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        // Local fetch. ZERO is the meaningful clear here — it reads as "no local
        // answer, keep the visual's own fetch", which is the pre-existing
        // behaviour. So a frame before the measuring pass has ever run, or a
        // build where its .spv is missing, draws exactly what it drew before
        // instead of sampling an undefined image.
        if (s_fetch != VK_NULL_HANDLE) {
            VkClearColorValue zeroF{};
            ImageBarrier(cmd, s_fetch, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, s_fetch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroF, 1, &rng);
            ImageBarrier(cmd, s_fetch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            s_fetchReady = true;   // only NOW may a draw declare this image GENERAL
        }
    }

    // The lid: which of this water is under a floor and therefore carries no
    // waves. Built here, before the step reads it, and BEFORE the early-out
    // below — a frame that owes no sim step still owes the grid its rays.
    if (ps_r_wtr_sim_lid) LidTick(cmd, shiftX, shiftZ);

    // How big is each pool, in metres — before the early-out below, because the
    // water is drawn on frames that owe no sim step.
    FetchTick(cmd);

    // Upload this frame's splats.
    const u32 n = std::min<u32>((u32)s_pending.size(), kMaxSplats);
    if (void* dst = s_splatBuf.Map()) {
        if (n) std::memcpy(dst, s_pending.data(), sizeof(Splat4) * n);
        s_splatBuf.Unmap();
    }
    s_pending.clear();

    RipplePush pc{};
    pc.shift[0] = shiftX; pc.shift[1] = shiftZ;
    pc.origin[0] = s_originX; pc.origin[1] = s_originZ;
    pc.mPerTexel = mpt;
    pc.damping   = std::clamp(ps_r_wtr_sim_damp, 0.9f, 0.9999f);
    pc.stiffness = std::clamp(ps_r_wtr_sim_speed, 0.01f, 0.5f);  // > 0.5 = explicit-scheme blow-up
    pc.splatCount = (s32)n;
    pc.usePools   = (s_maskValid && ps_r_wtr_sim_pools) ? 1 : 0;
    pc.dryValue   = kDry;
    pc.shoreAbsorb = std::clamp(ps_r_wtr_sim_shore, 0.5f, 1.f);
    pc.camX  = RV().vCameraPosition.x;
    pc.camZ  = RV().vCameraPosition.z;
    pc.reach = std::max(ps_r_wtr_sim_reach, 0.f);
    // Depth only means anything while the CDB sweep is maintaining it; with the
    // lid off the map is a flat "deep" and scaling by it would be a lie.
    pc.depthOn  = (ps_r_wtr_sim_depth && ps_r_wtr_sim_lid) ? 1 : 0;
    pc.depthRef = std::max(ps_r_wtr_sim_depth_ref, 0.01f);
    pc.bedFric  = std::clamp(ps_r_wtr_sim_bed, 0.5f, 1.f);
    // Drying is given in SECONDS to full dry; the step needs a per-step factor,
    // and the step rate is fixed at kStepHz precisely so this stays frame-rate
    // independent. 0.01 = "1% left", i.e. effectively dry after that many seconds.
    //
    // ⚠ Gated on the live-surface map as well. Wetness is now a COMPARISON of two
    // heights, and without that map neither of them exists; running anyway would
    // hold every texel at its last value for ever, which reads from the outside
    // exactly like a knob that does nothing. Off, and said out loud, instead.
    pc.wetDry  = (ps_r_wtr_wet_dry > 0.05f && s_surfValid)
               ? std::pow(0.01f, 1.0f / (ps_r_wtr_wet_dry * kStepHz)) : 0.f;
    if (!s_surfValid) {
        static float s_noSurfT = 0.f;
        if (RV().fTimeGlobal - s_noSurfT > 10.f) {
            s_noSurfT = RV().fTimeGlobal;
            Msg("![VK Ripple] shore wetness: no live-surface map this frame — the contact test is OFF "
                "(only the level map's submerged half is left). Check the [VK Water] 'live surface' line at init.");
        }
    }
    pc.wetLift  = std::max(ps_r_wtr_wet_lift, 0.f);
    pc.depthMax = kDepthMax;
    // The field is DISPLACED by BOTH of these in the tessellation stage
    // (water.tese: wp.y += f.x * p5.w * fade², and f.x carries r_wtr_sim_height),
    // so the wetted line has to use the same product — otherwise the wave you can
    // see wash over the sand and the wave the wetness reacts to are two different
    // waves. r_wtr_disp at 0 means the surface is not displaced at all.
    pc.wetSimH  = std::max(ps_r_wtr_sim_height, 0.f) * std::max(ps_r_wtr_disp, 0.f);

    // How many fixed steps this frame earned — decided in PrepareTile, together
    // with the anchor, because the two cannot be allowed to disagree: a step is
    // what carries the scroll, so a frame with no step must not have moved the
    // tile. Reading it here instead of recomputing it is the point.
    const u32 steps = s_steps;
    if (steps == 0) { s_pending.clear(); return; }   // nothing to do this frame

    // Ping-pong: descriptor set `d` reads image[d^1] and writes image[d], so
    // each step just flips which set is bound. Only the FIRST step carries the
    // scroll and the splats — the rest are pure propagation.
    const u32 groups = (kTexels + 7) / 8;
    u32 src = s_cur;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    for (u32 step = 0; step < steps; ++step) {
        const u32 dst = src ^ 1;
        if (step > 0) { pc.shift[0] = pc.shift[1] = 0; pc.splatCount = 0; }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_layout, 0, 1, &s_set[dst], 0, nullptr);
        vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, groups, groups, 1);
        if (step + 1 < steps)
            MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        src = dst;
    }
    s_cur = src;

    // The graphics stages that sample this must wait for the store.
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // ---- audit readback (r_wtr_audit) --------------------------------------
    if (s_auWant && s_auState == 0) {
        s_auWant = false;
        if (!s_auMask.GetHandle())
            s_auMask.Create(sizeof(float) * kTexels * kTexels, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        if (!s_auField.GetHandle())
            s_auField.Create(sizeof(u16) * 2 * kTexels * kTexels, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        VkBufferImageCopy cp{};
        cp.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.imageExtent      = { kTexels, kTexels, 1 };
        if (!s_auLid.GetHandle())
            s_auLid.Create(sizeof(float) * kTexels * kTexels, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        if (s_mask != VK_NULL_HANDLE) {
            ImageBarrier(cmd, s_mask, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdCopyImageToBuffer(cmd, s_mask, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   s_auMask.GetHandle(), 1, &cp);
            ImageBarrier(cmd, s_mask, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        if (s_lid != VK_NULL_HANDLE) {
            ImageBarrier(cmd, s_lid, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdCopyImageToBuffer(cmd, s_lid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   s_auLid.GetHandle(), 1, &cp);
            ImageBarrier(cmd, s_lid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        if (!s_auWet.GetHandle())
            s_auWet.Create(sizeof(float) * 2 * kTexels * kTexels, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        if (s_wet[s_cur] != VK_NULL_HANDLE) {
            ImageBarrier(cmd, s_wet[s_cur], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdCopyImageToBuffer(cmd, s_wet[s_cur], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   s_auWet.GetHandle(), 1, &cp);
            ImageBarrier(cmd, s_wet[s_cur], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        if (!s_auSurf.GetHandle())
            s_auSurf.Create(sizeof(float) * 4 * kTexels * kTexels, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        if (s_surf != VK_NULL_HANDLE) {
            ImageBarrier(cmd, s_surf, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdCopyImageToBuffer(cmd, s_surf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   s_auSurf.GetHandle(), 1, &cp);
            ImageBarrier(cmd, s_surf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        ImageBarrier(cmd, s_img[s_cur], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd, s_img[s_cur], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               s_auField.GetHandle(), 1, &cp);
        ImageBarrier(cmd, s_img[s_cur], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        s_auState = 4;                      // read it a few frames from now
    } else if (s_auState > 0 && --s_auState == 0) {
        RunAudit();
    }

    // Is anything actually writing into the field? "I walked in the water and
    // nothing happened" has two very different causes — no splats reaching the
    // sim, or splats too weak to see — and only a counter tells them apart.
    if (RV().fTimeGlobal - s_lastLog > 3.f) {
        if (s_splatsSeen) {
            Msg("[VK Ripple] %u splat(s) in the last 3 s, %u step(s) this frame, tile at (%.0f, %.0f), pools=%d (maskValid=%d cvar=%d)",
                s_splatsSeen, steps, s_originX, s_originZ, pc.usePools, (int)s_maskValid, ps_r_wtr_sim_pools);
        }
        s_splatsSeen = 0;
        s_lastLog = RV().fTimeGlobal;
    }
}

VkImageView WetView()    { return s_wetView[s_cur]; }
bool        Ready()      { return s_inited && !s_failed && s_haveOrigin; }
VkImageView GetView()    { return Ready() ? s_view[s_cur] : VK_NULL_HANDLE; }
VkSampler   GetSampler() { return s_sampler; }
VkSampler   GetPointSampler() { return s_pointSampler; }
float       OriginX()    { return s_originX; }
float       OriginZ()    { return s_originZ; }
float       SizeMetres() { return MetresPerTexel() * float(kTexels); }
u32         Texels()     { return kTexels; }

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipe)      { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_maskView)  { vkDestroyImageView(VulkanHW.m_Device, s_maskView, nullptr); s_maskView = VK_NULL_HANDLE; }
    if (s_mask)      { vmaDestroyImage(VulkanHW.m_Allocator, s_mask, s_maskAlloc); s_mask = VK_NULL_HANDLE; }
    // The lid was never released here — it is a full 512² R32F image, and it was
    // outliving the device alongside the audit staging.
    if (s_lidView)   { vkDestroyImageView(VulkanHW.m_Device, s_lidView, nullptr); s_lidView = VK_NULL_HANDLE; }
    if (s_lid)       { vmaDestroyImage(VulkanHW.m_Allocator, s_lid, s_lidAlloc); s_lid = VK_NULL_HANDLE; s_lidAlloc = nullptr; }
    if (s_depthView) { vkDestroyImageView(VulkanHW.m_Device, s_depthView, nullptr); s_depthView = VK_NULL_HANDLE; }
    if (s_depth)     { vmaDestroyImage(VulkanHW.m_Allocator, s_depth, s_depthAlloc); s_depth = VK_NULL_HANDLE; s_depthAlloc = nullptr; }
    for (u32 i = 0; i < 2; ++i) {
        if (s_wetView[i]) { vkDestroyImageView(VulkanHW.m_Device, s_wetView[i], nullptr); s_wetView[i] = VK_NULL_HANDLE; }
        if (s_wet[i])     { vmaDestroyImage(VulkanHW.m_Allocator, s_wet[i], s_wetAlloc[i]); s_wet[i] = VK_NULL_HANDLE; s_wetAlloc[i] = nullptr; }
    }
    if (s_surfView) { vkDestroyImageView(VulkanHW.m_Device, s_surfView, nullptr); s_surfView = VK_NULL_HANDLE; }
    if (s_surf)     { vmaDestroyImage(VulkanHW.m_Allocator, s_surf, s_surfAlloc); s_surf = VK_NULL_HANDLE; s_surfAlloc = nullptr; }
    if (s_fetchPipe)      { vkDestroyPipeline(VulkanHW.m_Device, s_fetchPipe, nullptr); s_fetchPipe = VK_NULL_HANDLE; }
    if (s_fetchLayout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_fetchLayout, nullptr); s_fetchLayout = VK_NULL_HANDLE; }
    if (s_fetchPool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_fetchPool, nullptr); s_fetchPool = VK_NULL_HANDLE; }
    if (s_fetchSetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_fetchSetLayout, nullptr); s_fetchSetLayout = VK_NULL_HANDLE; }
    if (s_fetchView) { vkDestroyImageView(VulkanHW.m_Device, s_fetchView, nullptr); s_fetchView = VK_NULL_HANDLE; }
    if (s_fetch)     { vmaDestroyImage(VulkanHW.m_Allocator, s_fetch, s_fetchAlloc); s_fetch = VK_NULL_HANDLE; s_fetchAlloc = nullptr; }
    s_fetchSet = VK_NULL_HANDLE;
    s_maskValid = false;
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    if (s_sampler)   { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    if (s_pointSampler) { vkDestroySampler(VulkanHW.m_Device, s_pointSampler, nullptr); s_pointSampler = VK_NULL_HANDLE; }
    s_splatBuf.Destroy();
    for (u32 i = 0; i < 2; ++i) {
        if (s_view[i]) { vkDestroyImageView(VulkanHW.m_Device, s_view[i], nullptr); s_view[i] = VK_NULL_HANDLE; }
        if (s_img[i])  { vmaDestroyImage(VulkanHW.m_Allocator, s_img[i], s_alloc[i]); s_img[i] = VK_NULL_HANDLE; s_alloc[i] = VK_NULL_HANDLE; }
    }
    s_auMask.Destroy(); s_auField.Destroy(); s_auLid.Destroy(); s_auWet.Destroy(); s_auSurf.Destroy();
    s_lidRb.Destroy();
    for (u32 i = 0; i < kLidRings; ++i) s_lidUp[i].Destroy();
    s_lidCpu.clear(); s_depthCpu.clear(); s_lidDone.clear();
    s_lidLevel = nullptr; s_lidUpFirst = true; s_lidDirty = false; s_lidRbState = 0; s_lidConsumePending = false;
    s_pending.clear();
    s_inited = s_failed = false; s_first = true; s_fetchReady = false; s_haveOrigin = false; s_cur = 0;
    s_accum = 0.f; s_steps = 0; s_tileFrame = 0xFFFFFFFFu; s_shiftX = s_shiftZ = 0;
}

}}  // namespace VK::WaterRipple

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  FROXEL VOLUMETRIC LIGHTING (vk_volumetrics) — P1: sun in-scatter + fog
// ============================================================================
//  A 3D froxel grid over the view frustum (160x90x64, exponential Z so it's
//  denser near the camera). Two compute passes per frame, both OUTSIDE any
//  render pass:
//    1. INJECT  (vol_inject.comp)   — per froxel: reconstruct its world pos from
//       the camera basis (DeriveProjTerms, the SSAO/shafts scheme), evaluate
//       height/base fog density, then the sun in-scatter = HG phase x cascade
//       sun-shadow x sun colour (+ a flat sky-ambient fill) x density.
//       Output s_scatter: rgb = in-scatter, a = extinction (= density).
//    2. INTEGRATE (vol_integrate.comp) — per (x,y) column, march front-to-back
//       over Z accumulating in-scatter and transmittance (Beer-Lambert).
//       Output s_integrated: rgb = accumulated in-scatter, a = transmittance.
//
//  The COMPOSITE is folded into the tonemap pass (vk_pass_tonemap): each scene
//  pixel maps its view-Z back to a froxel slice (inverse exp-Z) and applies
//  `scene = scene*transmittance + inscatter` in HDR, before tonemapping —
//  god rays through geometry + depth fog. Gated on r_vol (default OFF).
//
//  P1 occlusion source = the sun CASCADE maps (the r_vsm-OFF path + permanent
//  fallback). VSM-atlas sampling is the P4 upgrade for the VSM-default look.
//  No local lights (P2), no animated noise (P3), no temporal accum (P4) yet.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"
#include "vk_pass_ssao.h"   // VK::ProjTerms (camera basis + tan/near/far)

namespace VK { namespace Vol {

// Froxel grid resolution. 256x144x128 (16:9) ~= 4.7M froxels, 2x RGBA16F ~= 75 MB.
// Raised from 160x90x64 — the coarse grid read as "low-res/noisy" fog; the higher
// XY+Z resolution shrinks the visible froxels (smoother haze + crisper shafts), and
// the perf headroom is huge (~0.4 ms inject+integrate). Grid res = the quality
// slider for weak HW (drop back toward 160x90x64). Dims are 4-divisible (inject
// local 4x4x4) and 8-divisible in XY (integrate local 8x8).
constexpr u32 kGridX = 256;
constexpr u32 kGridY = 144;
constexpr u32 kGridZ = 128;

// Stage-1 VMS: a smoke particle injected as participating media. Filled by the
// particle pass (ParticlePass::CollectSmokeParticles) and uploaded to the splat
// SSBO. xyz = world centre, w = radius (world units); rgb = albedo, a = density
// (the particle's opacity). Layout matches the SmokeParticle in vol_splat.comp.
struct SmokeParticle { float pos[3]; float radius; float color[4]; };

bool Init();        // eager: creates the 3D volumes + compute pipelines, transitions
                    // the volumes to SHADER_READ so the tonemap binding is always valid.
void Destroy();
bool Ready();
bool Wanted();      // r_vol != 0

// Per-frame inject + integrate. Records into `cmd` (compute, OUTSIDE a render
// pass). pt = DeriveProjTerms(viewProj); slot = in-flight slot. No-op when
// r_vol is off (the volume keeps its SHADER_READ layout, the composite is gated).
// `smoke`/`smokeCount` = this frame's smoke particles to inject as media (Stage 1);
// pass nullptr/0 for none. Internally clamped to the splat capacity.
// `sceneDepthPrev` (optional): the scene depth ATTACHMENT view — Execute runs before
// this frame's prepass, so it still holds LAST frame's depth in SHADER_READ layout.
// Enables froxel depth rejection (r_vol_depth_reject): fog behind the geometry of its
// own view column gets no in-scatter (kills the through-wall glow the trilinear volume
// fetch smears onto walls). Pass VK_NULL_HANDLE to disable (editor path).
// `onComputeQueue`: `cmd` belongs to the dedicated COMPUTE queue (async compute).
// Graphics pipeline stages are ILLEGAL there, so the pass's boundary barriers — the
// ones handing the volumes to the tonemap / particle pass — drop their FRAGMENT
// stage and let the timeline semaphore carry that dependency instead, which is what
// a cross-queue handoff is expressed with. Everything in between is compute/transfer
// already. Pass VK_NULL_HANDLE for sceneDepthPrev in that mode: the scene depth is
// being rewritten by this frame's prepass on the other queue.
void Execute(VkCommandBuffer cmd, const ProjTerms& pt, u32 slot,
             const SmokeParticle* smoke, u32 smokeCount,
             VkImageView sceneDepthPrev = VK_NULL_HANDLE,
             bool onComputeQueue = false);

// GRAPHICS-queue call, recorded right after Pass_SunShadow. Under async compute
// (r_async) the inject records onto the COMPUTE queue, where sampling the live sun
// cascades would race against the graphics queue rewriting them for the same frame —
// and nothing stores last frame's cascade to read instead. This blits a downscaled
// (1024²) snapshot of the cascades + rain map into one of two ping-ponged sets; the
// NEXT frame's inject samples the set written here. No-op when r_async is off or the
// device has no dedicated compute family — then the inject samples the live maps.
void SnapshotShadows(VkCommandBuffer cmd);

// May the GRAPHICS queue sample the LOCAL scatter volume this frame (the Stage-0
// smoke light probe)? False under async compute: the inject writes that volume on
// the compute queue while the particle pass — which lives in the scene segment and
// deliberately does NOT wait on compute — would be reading it. Returning false costs
// the smoke its volumetric probe under r_async and nothing else; the fix that gives
// it back is ping-ponging the scatter volume so the graphics queue reads the
// previous frame's copy. The integrated volume needs no such gate: its only consumer
// is the tonemap, which sits in the post segment and already waits on the timeline.
bool ProbeAvailableToGraphics();

// May the inject record onto the COMPUTE queue this frame? Only once a COMPLETED
// shadow snapshot exists for it to sample (see SnapshotShadows). False on the first
// frames after a level load or after r_async is switched on — there the pass stays on
// the graphics queue, where reading the live cascades is safe.
bool AsyncInjectReady();

// GRAPHICS-queue prerequisites of Execute: the one-shot terrain-height bake (a real
// render pass) and the grass-canopy upload. Call with the frame's GRAPHICS command
// buffer BEFORE Execute — under async Execute records onto the compute queue, where
// a render pass is fatal. Both early-out once baked.
void RecordGraphicsBakes(VkCommandBuffer gfxCmd);

// Composite inputs for the tonemap fold: the integrated volume (rgb = in-scatter,
// a = transmittance) + a linear/clamp sampler. The view exists from Init on (the
// volumes are created eagerly), so the tonemap descriptor is always valid.
VkImageView GetIntegratedView();
// The LOCAL per-froxel inject result (rgb = in-scatter, a = extinction) — a light
// probe for translucents: dividing rgb by a gives the density-independent radiance
// (sun shaft / flashlight cone / campfire glow + HG phase + shadows) at that point,
// which the particle pass uses to volumetrically light smoke billboards (Stage 0).
// Execute leaves it in SHADER_READ when r_vol is on; eager-created so always valid.
VkImageView GetScatterView();
VkSampler   GetSampler();
u32         Generation();   // bumps when the volumes are (re)created (init only)

// The exp-Z grid params the LAST Execute used — the composite needs the EXACT
// same near/far/log2(far/near) to invert the froxel Z mapping (forward in inject
// must equal inverse in the tonemap, else the fog samples the wrong slice).
struct GridZParams { float nearZ, farZ, logFarNear; };
GridZParams GetGridZ();

}}  // namespace VK::Vol

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  DDGI PROBE VOLUME (vk_ddgi) — dynamic diffuse global illumination
// ============================================================================
//  A grid of probes around the camera. Every frame each probe shoots a small set
//  of rays into the camera-local BVH (vk_rt_scene), shades what they hit with
//  shaders/rt_shade.glsl, and folds the result into two small octahedral maps:
//  IRRADIANCE (what light arrives, per direction) and DISTANCE MOMENTS (how far
//  the geometry is, per direction, mean and mean-of-squares).
//
//  WHY TWO MAPS AND NOT JUST IRRADIANCE
//  ------------------------------------
//  An irradiance-only probe grid is an old idea with one famous failure: light
//  leaks through walls, because trilinear interpolation between eight probes has
//  no way of knowing that a wall runs between them and the surface being shaded.
//  The distance moments are the fix — a Chebyshev (variance-shadow) test asks
//  "how likely is this surface even visible from that probe" and weights the
//  probe on the wrong side of the wall out. That test is the difference between
//  DDGI and a light-leaking toy, so it is built in from the start rather than
//  bolted on after the leaks show up.
//
//  WHAT IS AND IS NOT SOLVED HERE
//  ------------------------------
//  Solved: one bounce of sun and sky light off static collision geometry, updated
//  continuously, with the volume scrolling toroidally around the player so probe
//  history stays attached to WORLD positions rather than to slots.
//
//  Not yet: this pass only PRODUCES the field. Nothing samples it in the actual
//  lighting yet (that is the next step) — r_rtgi_debug 5/6 is how it is looked at.
//  Since then: multi-bounce (r_ddgi_bounce), cascades, dynamic lights in the RT
//  world, and PROBE RELOCATION (r_ddgi_reloc) — a probe that lands inside a wall
//  or under the terrain slides out of it, up to r_ddgi_reloc_max of the spacing,
//  with the offset stored in a strip at the bottom of the irradiance atlas so no
//  consumer needs a new binding (shaders/ddgi_relocate.comp.glsl).
//  Since 23-09 (RTXGI conformance review): a FIXED unrotated ray set for
//  relocation (r_ddgi_fixed_rays), probe CLASSIFICATION (r_ddgi_classify — a
//  probe with no geometry in its cell box skips its rays between refreshes),
//  buried probes dropped from the lookup (r_ddgi_skip_buried), the RTXGI weight
//  order (r_ddgi_crush_tri), probe-ray hits that take their sky from the field
//  (r_ddgi_hit_sky), RG32F distance moments and a cascade-0-first schedule
//  (r_ddgi_sched).
//  Still absent: any dynamic geometry (the BVH is static — see the multi-instance
//  TLAS note in vk_rt_scene.h).
//
//  COST SHAPE
//  ----------
//  probeCount * raysPerProbe rays per frame — 16k probes x 32 rays is ~0.5 M
//  primary plus as many shadow rays, which measured ~13 M rays/ms on this
//  hardware. Tracing is NOT the expense here; the two blend dispatches, which
//  touch every atlas texel of every probe every frame, are.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"

namespace VK { struct FrameContext; }

namespace VK { namespace DDGI {

// Confidence gate: the lookup fades to black when NO probe can see the point
// (best Chebyshev below this). Shipped in the consts tail, read by the GI debugger.
constexpr float kDDGIConf = 0.25f;

// Idempotent; safe to call repeatedly. A failure leaves the volume off and is
// not fatal to the frame.
bool Init();
void Destroy();

// r_ddgi is on, init succeeded, and the RT scene has a bubble to trace against.
bool Enabled();

// Trace + blend for this frame. Records compute dispatches, so it MUST be called
// outside any rendering scope. Steps RTScene itself (the acceleration structure
// has to exist before the probes trace it).
void Update(VkCommandBuffer cmd);

// The cform the BVH was cut from is going away; the field it produced describes a
// world that no longer exists, so wipe it rather than fade it out.
void OnLevelUnload();

// ---- debug views (r_ddgi_show_probes / r_ddgi_show_rays) -------------------
// Draws the probes as dots and their rays as segments, ON TOP of the world.
// Everything this arc argues about — a probe inside a wall, a room with no valid
// probe, a field that is fine while the receiver drops it — has no pixels of its
// own, and a percentage averaged over 2048 probes cannot answer a question about
// three of them. r_ddgi_show_xray draws with no depth test, which is the only way
// to see the probes sealed inside geometry — i.e. the ones relocation is for.
// Records draws, so it must be called from inside a frame (see RegisterPass).
void DrawDebug(FrameContext& ctx);

// ---- consumers -------------------------------------------------------------
// The constants UBO for THIS frame's slot (matches DDGIConstsBlock in
// shaders/ddgi_common.glsl), and the two atlases. Both atlases live permanently
// in VK_IMAGE_LAYOUT_GENERAL: they are written as storage images by the blend
// dispatches and read as sampled images by everything else, and a per-frame
// round trip between layouts would buy nothing.
VkBuffer    ConstsBuffer();     // current frame slot; call after Update
VkImageView IrradianceView();
VkImageView DistanceView();
VkSampler   GetSampler();       // linear/clamp — the border texels do the wrapping
u32         Generation();       // bumps when the atlases are (re)created → rebind
// Probes across ALL cascades = the length of the atlas's flat run, i.e. the range
// ddgiGlobalProbe() indexes into. Anything that keeps a PER-PROBE array of its own
// (vk_gi_froxel does) must size it from this and resize when it changes — the grid
// shape is three console cvars away from being different.
u32         ProbeCount();

// Sun and sky exactly as the probe rays integrate them: environment values,
// linearised, thunderbolt-safe. Shared so the debug preview cannot drift from
// what the probes actually see — a preview lit differently from the field it is
// previewing would be worse than no preview.
void SunSky(Fvector& toSun, Fvector3& sunColor, Fvector3& skyColor);

// The indoor reflectance boost as fed to the shaders THIS frame (r_gi_indoor
// mixed by outdoor illumination — see the fill in Update). For instruments:
// the [VK GI] log line only prints it on change, and the pixel debugger needs
// the current value every dump.
float IndoorBoost();

// The two SKY DESCRIPTORS the probe trace binds — the SH9 coefficient buffer and
// the prefiltered sky cube, each already resolved to vk_ibl's live resource or to
// this module's placeholder. Handed out (rather than re-derived) because the final
// gather shades a miss with shaders/ddgi_sky.glsl, the same include the trace uses:
// sharing the CODE but binding a different sky would put the two back out of step
// exactly where it is hardest to see. Any output may come back VK_NULL_HANDLE
// before Init; the caller must treat that as "not this frame".
void SkyInputs(VkBuffer& shBuffer, VkImageView& skyCube, VkSampler& cubeSampler);

}}  // namespace VK::DDGI

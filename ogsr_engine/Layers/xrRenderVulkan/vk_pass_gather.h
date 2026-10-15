// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  FINAL GATHER (vk_pass_gather) — step 4 of the RT arc, after idTech 8.
// ============================================================================
//  The probe volume (vk_ddgi) answers "what light arrives around here" on a 3 m
//  lattice. This pass gives EVERY PIXEL its own gather ray and keeps the volume
//  as the cache that ray reads at its hit point, which is the structure Tiago
//  Sousa described for DOOM The Dark Ages: `Final Gather -> Denoise -> Upscale`,
//  with the irradiance volumes explicitly the LAST fallback in the lookup chain
//  rather than the answer.
//
//  WHY IT IS AFFORDABLE
//  --------------------
//  The ray shades NOTHING. No sun ray, no light loop — the hit multiplies its
//  albedo by the probe field's irradiance at that point and returns. The probe
//  pass already folded the sun, the sky, the dynamic lights and the previous
//  bounce into that field, so none of it is recomputed per pixel. What the ray
//  buys is not a better radiance value but the fact that THIS pixel's hemisphere
//  was tested from THIS point: occlusion, contact darkening and colour bleeding
//  come out sharp even though every value summed is a smooth probe answer.
//
//  WHAT IT REPLACES
//  ----------------
//  Nothing, until r_gather_light is turned up. The pass produces a screen-space
//  ambient texture; ddgiAmbient() blends it against the plain probe lookup with
//  that cvar, which is the only honest A/B this renderer accepts (weather and
//  actor position drift between runs, so two runs cannot be compared).
//
//  THE UNITS ARE THE PROBE FIELD'S UNITS — cosine-weighted mean radiance, the
//  convention ddgi_blend_irr stores. If they were not, r_gather_light would be
//  an exposure knob wearing a GI costume.
//
//  ⚠ EVERY TEXEL IS A USABLE ANSWER. Beyond the BLAS bubble, and on sky texels,
//  the shader writes the plain probe lookup instead of signalling "invalid" — so
//  the receivers blend ONE value with one weight rather than carrying a validity
//  mask through the denoiser, the upsample and three shaders.
//
//  THE 2-BAND SH (r_gather_sh) — ⛔PARKED 2026-08-23, DEFAULT OFF, DO NOT DELETE
//  ----------------------------------------------------------------------------
//  Parked the day it landed: at one ray per pixel the L1 carries more noise than
//  direction (|d| / L0 measured at a median of 0.65), and noise modulated by a
//  normal map reads as plastic rather than as light. The mechanism below is
//  proven correct and cheap to keep; the fix is variance in L1, not strength.
//  The full verdict, the numbers and what would unpark it are in the PARKED
//  banner in vk_console_min.cpp — read that before touching any of this.
//  One RGB value per pixel is the answer for the FLAT surface the depth buffer
//  holds, so every bump a normal map carves into it receives the same ambient.
//  idTech 8 stores its gather as a 2-band SH per pixel (3 x RGBA16F) instead, and
//  with r_gather_sh this pass publishes the same thing: L0 stays in the existing
//  target's rgb, and a second target carries L1 as one deviation vector per colour
//  channel — three tiles stacked vertically in ONE image, so the whole feature
//  costs one extra binding in each of the three sets rather than nine.
//
//  ⭐ THE DEVIATION IS PERPENDICULAR TO THE GATHER'S OWN NORMAL, which makes the
//  reconstruction return L0 exactly when the shading normal equals the geometric
//  one. Turning the SH on therefore cannot move the mean of the frame — only
//  redistribute light across normal-mapped detail — and that is a claim to
//  MEASURE (see r_gather_debug 5, the strength view).
//
//  ⚠ The targets are allocated at 1x1 while the cvar is off, so the descriptors
//  stay valid without a second code path and the VRAM cost is zero until asked
//  for. Flipping the cvar therefore REBUILDS the targets, like r_gather_scale.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"
#include "vk_pass_ssao.h"   // VK::ProjTerms — the same camera basis GTAO/RTAO decode depth with

namespace VK { struct FrameContext; }

namespace VK { namespace GatherPass {

// Pipelines + sets. Not fatal on failure: the gather simply stays off and the
// probe lookup keeps feeding the ambient exactly as before.
bool Init();
void Destroy();

// r_gather is on (the cvar alone — used to decide whether to build resources).
bool Wanted();

// ...and the screen-space radiance cache is on with it. vk_pass_ssao asks this to
// decide whether to keep capturing the prev-frame colour history: with SSIL off
// and the gather's cache on, the gather is the buffer's only reader.
bool WantsPrevColor();

// ...and init succeeded, the targets exist, the RT scene has a bubble and the
// probe volume is live. The gather has no meaning without the volume: the field
// IS its radiance cache.
bool Enabled();

// (Re)create the gather-resolution targets. Pass_World calls this BEFORE
// EnvLight::Update, so the view bound at the receivers' binding is the one this
// frame will write — never one a resize is about to destroy.
bool EnsureTargets(VkExtent2D sceneExtent);

// Trace + resolve. Records COMPUTE dispatches, so it must be called outside any
// rendering scope, with the scene depth in SHADER_READ_ONLY. `pt` must be the
// terms derived from `viewProj`, and `viewProj` must be the matrix that rendered
// that depth — the pass keeps it to reproject NEXT frame's hits into THIS frame's
// screen (the radiance cache), so a mismatched matrix would silently offset every
// cache lookup by whatever the two matrices disagree about, jitter included.
void Execute(VkCommandBuffer cmd, VkExtent2D sceneExtent, VkImageView depthView,
             VkImageView normalView, const ProjTerms& pt, const Fmatrix& viewProj);

// True when the pass CANNOT run, having said why (once, on change). Execute calls
// it first; the caller calls it on the frames it skips Execute, so an implicit
// dependency going away can never do so in silence.
// ⚠ CALL THIS INSTEAD OF DUPLICATING Enabled() AT A CALL SITE. A gate repeated
// outside the pass skips it without entering it, and then the pass cannot report
// its own failure — which is how a dead final gather once looked exactly like a
// weak one for a whole session. See the note on the definition.
bool ReportGate(VkImageView depthView, VkImageView normalView);

// r_gather_debug: overwrite the scene colour with the gathered term (1 resolved,
// 2 raw, 3 hit distance). Registered as its own pass, after the RT preview.
void DrawDebug(FrameContext& ctx);

// ---- consumers -------------------------------------------------------------
// The RESOLVED gather, permanently in VK_IMAGE_LAYOUT_GENERAL (written as a
// storage image, sampled by the forward receivers — a per-frame layout round
// trip would buy nothing, same rule as the DDGI atlases).
VkImageView ResultView();
// The resolved 2-BAND SH that goes with it (binding 38 in the env set): three
// tiles stacked vertically, R/G/B channel, each texel xyz = that channel's L1
// deviation vector. VK_NULL_HANDLE unless r_gather_sh produced one this frame —
// which is also what gates L.gather_params.y, so a receiver can never evaluate
// a direction from an image nothing has written.
VkImageView SHResultView();
VkSampler   GetSampler();     // linear/clamp — the receivers bilinearly upsample it
u32         Generation();     // bumps when the targets are (re)created → rebind trigger
// ddgi_params.z for the Lighting UBO: how much of the ambient comes from the
// gather instead of the probe lookup. 0 whenever the pass has not produced a
// result this level, so a receiver can never sample an image with no content.
float       Weight();

void OnLevelUnload();

}}  // namespace VK::GatherPass

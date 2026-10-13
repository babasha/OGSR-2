// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  THE GI FIELD IN FROXEL FORM (vk_gi_froxel) — step 6 of the RT arc
// ============================================================================
//  Everything the indirect-light chain built so far answers a SURFACE's question.
//  The probe volume, the final gather, the world radiance cache — all three are
//  read by G-buffer receivers (world / terrain / trees / grass), and NOTHING else
//  in the frame has ever asked them anything. The volumetric fog lights its air
//  with a flat per-frame constant. Smoke sprites take whatever the fog's local
//  in-scatter happens to hold. A pane of glass is lit as a world static and its
//  refraction carries no indirect light of its own.
//
//  idTech 8 closes exactly this gap with one structure (SIGGRAPH 2025, sec. 8):
//  a FROXEL-RESOLUTION copy of the irradiance volumes, in two-band SH, "reused by
//  all transparencies". This module is that copy.
//
//  WHY A COPY AND NOT JUST A LOOKUP
//  --------------------------------
//  ddgiSampleIrradiance costs 16 texture taps and needs a normal. A froxel has no
//  normal, and there are ~590k of them: sampling the lattice directly would be
//  ~28M taps a frame to resample a field whose finest detail is a metre wide. So
//  the field is resampled ONCE, into the view frustum, into a form that answers
//  every direction at once — after which a consumer pays ONE trilinear tap.
//
//  TWO DISPATCHES:
//    1. gi_probe_sh.comp — projects every probe's octahedral irradiance map into
//       two-band SH (10240 probes x 36 texels; the cheap end of the problem).
//    2. gi_froxel.comp   — per froxel, the eight-probe gather (trilinear x the
//       Chebyshev wall test) of those SH coefficients into two RGBA16F volumes.
//
//  WHAT READS IT
//    * vol_inject          — replaces the flat sky-ambient fill of the fog's
//                            ambient in-scatter term (the visible one indoors);
//    * particle.frag       — smoke sprites, when the fog probe is not already
//                            feeding them (it would otherwise be counted twice).
//  Surfaces do NOT read it and should not: they have a normal and a per-pixel
//  gather, both of which resolve detail this grid cannot.
//
//  ⚠ NOT UNDER ASYNC COMPUTE. When r_async moves the fog inject onto the compute
//  queue, this pass would have to read the DDGI atlases while the graphics queue
//  is still writing them — the same cross-queue race the shadow snapshot exists to
//  avoid, and there is no snapshot of a probe atlas. Under r_async the field is
//  simply not built and every consumer keeps its analytic fill (see Weight()).
// ============================================================================
#pragma once
#include "HW_Vulkan.h"
#include "vk_pass_ssao.h"   // VK::ProjTerms — the camera basis, shared with the fog grid

namespace VK { namespace GiFroxel {

// Idempotent. Creates the volumes EAGERLY (like vk_volumetrics does) so a consumer
// descriptor pointing at them is valid from init on, whatever the console says
// later. A failure disables the field and is not fatal to the frame.
bool Init();
void Destroy();
bool Ready();

// r_gi_froxel > 0. The producers this depends on (the probe volume) have their own
// gates; Enabled() is the conjunction.
bool Wanted();
bool Enabled();

// Fill the field for THIS frame's frustum. Records two compute dispatches, so it
// MUST be called outside any rendering scope. `nearZ`/`logFN` are the fog grid's
// own exp-Z parameters — passed in rather than re-derived, so the two grids cannot
// disagree about which slice a metre lands in.
void Execute(VkCommandBuffer cmd, const ProjTerms& pt, float nearZ, float logFN, u32 slot);

// The BVH and the probe field describe a world that is going away.
void OnLevelUnload();

// ---- consumers -------------------------------------------------------------
// How much of the ambient the field owns this frame: r_gi_froxel, or 0 when the
// field was not built (off, no probe volume, async, or no frame has filled it
// yet). Consumers multiply this by the per-froxel weight in L0's alpha, which is
// the volume fade — outside the probe volume the field says nothing and the
// consumer must keep its own analytic fill.
float Weight();
// The medium-directionality dial (r_gi_froxel_dir), as a multiplier on the phase
// function's own mean cosine. 0 = isotropic ambient, 1 = the band-1 term at full.
float DirWeight();
// r_gi_froxel_gain — the field's scale against the analytic fill it displaces. The
// two are not the same quantity (the probes integrate the SH9 sky at the surface
// receivers' scale; a medium's flat fill is the weather's ambient colour), so the
// calibration is a measurement, not a constant. See NoteFlatFill.
float Gain();
// What the consumer's OWN analytic fill is worth, as a luminance — handed in so the
// self-audit can print the field's answer and the constant it replaced on ONE line.
// A verdict that needs two log lines from two modules is a verdict nobody reads.
void  NoteFlatFill(float luminance, float effectiveGain);

VkImageView L0View();       // rgb = mean radiance, a = field weight
VkImageView L1View();       // rgb = the band-1 (directional) vector, luminance
VkSampler   GetSampler();   // linear / clamp — trilinear, like every froxel volume here
u32         Generation();   // bumps when the volumes are (re)created -> rebind

}}  // namespace VK::GiFroxel

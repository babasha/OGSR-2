// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once

// Auto-exposure formula inputs (R4 bloom_luminance_3.ps):
//   exposure = clamp(middleGray / (avgLum + lowLum), expMin, expMax)
// These are shared by the tonemap composite (vk_pass_tonemap.cpp) AND the bloom
// pre-exposure (vk_pass_bloom.cpp). They MUST be identical or bloom thresholds
// against a different exposure than the composite applies (halo/banding). This
// header is the single source of truth — previously the four values were
// copy-pasted in both files with a "keep in sync" comment.
#include "vk_color_space.h"      // ColorSpace::Active() — latched linear-pipeline selector

extern float ps_r_expo;          // exposure compensation multiplier (EV-style user knob, 1 = neutral)
extern float ps_r_expo_gray;     // metering target override (0 = auto per pipeline)
extern float ps_r_expo_min;      // exposure clamp lo
extern float ps_r_expo_max;      // exposure clamp hi

namespace VK { namespace Exposure {

// Exposure target. The meter feeds linear BT.709 luminance, but what "correctly
// exposed" means depends on which pipeline lit the scene:
//  - gamma pipeline (r_linear_color 0): 0.58 — tuned by eye against the
//    display-space-ish HDR values this renderer historically produced;
//  - linear pipeline: the photographic 18% gray card. Reusing 0.58 there pegs
//    the meter at the max clamp (linear day luminance is ~0.15-0.25) → the whole
//    image rides the clamp = uniformly overbright (L-3 retune, 23-07-2026).
// Keys off the same live cvar as the tonemap's OETF encode so they flip together.
// r_expo_gray > 0 overrides both (user tuning knob).
constexpr float kMiddleGrayGamma  = 0.58f;
constexpr float kMiddleGrayLinear = 0.18f;
inline    float MiddleGray()
{
    if (ps_r_expo_gray > 0.f) return ps_r_expo_gray;
    return ColorSpace::Active() ? kMiddleGrayLinear : kMiddleGrayGamma;
}

constexpr float kLowLum = 0.0001f;  // R4 ps_r2_tonemap_low_lum floor

// The linear pipeline's absolute HDR scale runs ~2x hotter through the Reinhard
// section than the gamma pipeline's, so it carries a fixed exposure scale on top
// of the metering. 0.5 is the user's eyes-on-Кордон verdict (23-07-2026: tuned
// r_expo to 0.5 and called it right) baked in, so r_expo stays a NEUTRAL knob.
constexpr float kLinearExposureScale = 0.5f;

// Clamp range + compensation — live user knobs (r_expo_min / r_expo_max / r_expo).
// Shared by the tonemap composite AND the bloom pre-exposure — both MUST read
// these, never local copies, or bloom thresholds against a different exposure.
inline float ExpMin()  { return ps_r_expo_min; }
inline float ExpMax()  { return ps_r_expo_max; }
inline float ExpComp() { return ps_r_expo * (ColorSpace::Active() ? kLinearExposureScale : 1.0f); }

// The smoothed exposure the tonemap composite APPLIES this frame (metered by
// r_expo_meter, eased over r_exp_adapt, ExpComp folded in). Defined in
// vk_pass_tonemap.cpp. Returns 0 until the first readback is valid — a caller
// must then fall back to its own instantaneous estimate, exactly like the
// composite shader does when its p5.x is 0.
float Smoothed();

}}  // namespace VK::Exposure

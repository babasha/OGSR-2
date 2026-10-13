// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  RAY-TRACED INDIRECT LIGHT (vk_pass_rtgi) — step 0: the shadeable hit
// ============================================================================
//  This file will hold the DDGI probe update. Right now it holds only the
//  instrument that has to come first: a full-screen preview that traces one
//  primary ray per pixel into the camera-local BVH and shades what it finds with
//  shaders/rt_shade.glsl — the exact code a probe ray will use.
//
//  WHY A DEBUG VIEW IS THE FIRST DELIVERABLE
//  -----------------------------------------
//  A probe ray's result is never looked at directly: it is averaged into an
//  octahedral irradiance texel, blended with history, filtered by a depth test
//  and finally read back through a trilinear lookup. If the albedo table, the
//  face normal, the shadow ray or the ray origin bias is wrong, every one of
//  those failures arrives at the eye as the same symptom — "the GI looks a bit
//  off" — and there is no way to tell them apart. Traced straight to the screen
//  they are all individually obvious: wrong albedo is the wrong colour, a
//  flipped normal is a black facet, a bad bias is shadow acne.
//
//  So this pass is not a toy. It is the thing that makes the probe pass
//  debuggable, and it stays in the tree afterwards for exactly that reason.
//
//  WHAT IT DELIBERATELY DOES NOT SHOW
//  ----------------------------------
//  The BVH is a bubble of STATIC COLLISION geometry around the camera, so the
//  preview has no foliage or grass (no collision), no dynamic objects (no
//  instances yet — see the multi-instance TLAS note in vk_rt_scene.h), and
//  nothing past r_rtao_range. Those are the BVH's limits, not this pass's bugs;
//  every one of them is a known input to the DDGI design.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"
#include "vk_pass_context.h"

namespace VK { namespace RTGIPass {

// Pipelines + sets. Lazy: called by Execute on first use, so a session that
// never turns the preview on pays nothing. Failure is not fatal.
bool Init();
void Destroy();

// r_rtgi_debug > 0: overwrite the scene colour with the traced view. Also steps
// the RT scene (RTScene::Update), which is why it must be called BETWEEN passes
// and never inside a rendering scope — an acceleration-structure build cannot be
// recorded inside one. The per-frame guard in RTScene makes the extra call free
// when SSAO has already stepped it this frame.
void Execute(FrameContext& ctx);

}}  // namespace VK::RTGIPass

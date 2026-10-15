// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — weather effects: rain (drops + ground splashes) and
// thunderbolt (lightning model + sky gradients). Port of the shared
// dxRainRender / dxThunderboltRender over the particle pass's pipelines
// (FVF::LIT vertices, PBM_BLEND / PBM_ADD, shared texture cache).
//
// The engine owns the simulation (CEffect_Rain: drop physics, collision
// ray-picks, splash particle pool, ambient sound; CEffect_Thunderbolt: bolt
// state machine + env color modulation). The renderer-side classes here only
// build vertices and record draws. Both are reached through the render
// factory (FactoryPtr<IRainRender> / <IThunderboltRender>), and the actual
// draw happens inside the "Rain" pass registered after Particles.
#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"

class IRainRender;
class IThunderboltRender;
class IThunderboltDescRender;
class IFlareRender;

namespace VK {

// Registered pass entry: runs the rain simulation tick (Calculate), then lets
// CEffect_Rain / CEffect_Thunderbolt route into the vk render classes below,
// which fill a per-frame vertex ring; finally draws the collected chunks over
// the scene (color LOAD + depth LOAD, no depth write).
void Pass_Rain(FrameContext& ctx);
// RENDER_THREAD_PLAN step 8: simulation tick + vertex build, PREPARE phase (extract side).
void Rain_Extract();
void RainPass_Destroy();

}  // namespace VK

// Factory creators (vk_RenderFactory.cpp) — replace the inert stubs.
IRainRender*            VK_CreateRainRender();
IThunderboltRender*     VK_CreateThunderboltRender();
IThunderboltDescRender* VK_CreateThunderboltDescRender();
IFlareRender*           VK_CreateFlareRender();

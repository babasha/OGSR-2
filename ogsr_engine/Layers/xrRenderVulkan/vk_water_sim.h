// xrRenderVulkan - Water flow / accumulation simulation (compute).
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// A small GPU shallow-water sim on the rain-occlusion ortho box (top-down, the
// same VP/resolution as the rain map). State = water DEPTH per cell. Rain feeds
// it, a surface-Laplacian relaxation makes water flow downhill and pool in
// basins, evaporation drains it. The result (a water-depth texture, aligned to
// the rain map so receivers sample it with the rain_vp) drives geometric puddles
// and the volumetric water render. See water_sim.comp.glsl.
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace WaterSim {

// Idempotent; lazily created on the first Dispatch. Resolution + ortho box come
// from VK::ShadowMap (the rain map), so the buffer lines up with the ground.
bool Init();
void Destroy();
bool Ready();

// Advance the sim one frame (runs `iters` relaxation steps). `rainDensity01` is
// the current weather rain density (0..1) — drives the rain input. Call once per
// frame after the rain occlusion map is rendered (Pass_SunShadow), before World.
// No-op when the sim is disabled (r_water_sim 0) or rainDensity is 0 and the
// field has already drained.
void Dispatch(VkCommandBuffer cmd, float rainDensity01);

// Water-depth texture (R16F, metres) + its sampler, for the EnvLight set
// (binding 11). Valid after the first Dispatch; VK_NULL_HANDLE before.
VkImageView GetStateView();   // water depth (R16F)
VkImageView GetVelView();     // water velocity (RG16F, uv/sec) — for flow rendering
VkSampler   GetSampler();

}}  // namespace VK::WaterSim

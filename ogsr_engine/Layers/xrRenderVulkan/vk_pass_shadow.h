// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — sun shadow caster pass (STEP 2). Renders static world depth
// from the sun's POV into the shadow map, then leaves it SHADER_READ for the
// world pass to sample. Registered BEFORE "World" in CRender::Render.
#pragma once
#include "vk_pass_context.h"

namespace VK {
void Pass_SunShadow(FrameContext& ctx);
void SunShadow_Destroy();   // grass-spot caster pipeline teardown (device shutdown)
void SunShadow_WaitWorker(); // level unload: the far map's caster collect (r_shadow_far_async) reads the level's visuals
// Spot shadow POOL: atlas tile owned by this light this frame (-1 = none).
// Valid after Pass_SunShadow; pass the vkLight* from FrameLights::src.
int  SpotShadow_TileOfLight(const void* light);
// Point shadow POOL: cube-array index owned by this light this frame (-1 = none).
int  PointShadow_CubeOfLight(const void* light);
// nfs46 (r_sun_mask): half-res screen-space sun mask for the R1 receivers (EnvLight binding 14, puddle_geo.w = -1).
namespace SunMask {
bool        Active();                       // this frame: r_sun_mask + r_world_flat, sun maps on, VSM off, pipeline ok
bool        Ensure(VkExtent2D screen);      // (re)create the half-res RGBA16F target for this render size
VkImageView View();
VkSampler   Sampler();
void        Resolve(VkCommandBuffer cmd, VkImageView sceneDepth, VkExtent2D screen, const Fmatrix& viewProj);
}
}

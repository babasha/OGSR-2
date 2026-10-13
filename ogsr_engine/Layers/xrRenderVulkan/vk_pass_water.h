// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — WATER bodies (level ponds / rivers / flooded basements).
//
// Level water is static geometry carrying the level shader `effects\water`.
// It used to be classified as nothing at all, so it rode the opaque vert-lit
// path — where a water polygon's baked vertex light and baked sky access are
// both ~0, the albedo got multiplied by zero, and every water surface in the
// game rendered BLACK. This pass is the missing surface class:
//
//   CVulkanShader::Create   sets m_bWater on `effects\water*`
//   vk_Visual::LoadTexture  marks the material aref = -5 (+ wmark, so water
//                           stays out of the depth prepass and shadow casters)
//   RenderQueue::Push       diverts those items into the WATER list
//   Pass_Water (here)       draws them after the opaque world + sky, blended
//
// Look: Fresnel-weighted sky reflection + analytic wave normals (no texture) +
// GGX sun glitter over a Beer-Lambert body, composited with premultiplied
// alpha so the already-drawn bottom shows through the shallows. No scene-colour
// copy and no depth attachment — see water.frag.glsl for why neither is needed.
//
// cvars: r_wtr (master), r_wtr_debug, r_wtr_wave, r_wtr_scale, r_wtr_speed,
//        r_wtr_murk, r_wtr_refl, r_wtr_glint, r_wtr_detail, r_wtr_rough,
//        r_wtr_color.
#pragma once
#include "vk_pass_context.h"

namespace VK {

// Snapshot the opaque statics the LID map needs (see vk_pass_water.cpp). Must be
// called while the render queue still HOLDS them: Pass_World clears the queue
// long before the water pass runs, so reading it there finds nothing — which is
// exactly what "lid map: 0 statics overlap the tile" meant.
void Water_CaptureLidStatics();

// LEVEL-WIDE water height map: every sheet on the level rasterized top-down ONCE,
// storing the surface height (dry = WaterRipple::MaskDryValue). The wetness reads
// it so a shore is wet because water TOUCHES it, not because the player walked
// close enough for the ripple tile to reach — which is what it used to mean.
VkImageView Water_LevelMapView();
float       Water_LevelMapOriginX();
float       Water_LevelMapOriginZ();
float       Water_LevelMapSize();
bool        Water_LevelMapReady();

    void Pass_Water(FrameContext& ctx);
    void Water_Destroy();
}

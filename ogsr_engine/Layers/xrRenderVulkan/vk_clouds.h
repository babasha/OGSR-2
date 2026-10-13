// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — volumetric cloud noise bake.
//
// Owns the three static fields the cloud raymarch (clouds.glsl) reads:
//   shape   128^3 RGBA8 — base cloud form (Perlin-Worley) + Worley FBM octaves
//   detail   32^3 RGBA8 — high-frequency edge erosion
//   weather 512^2 RGBA8 — coverage / type / large-scale band, per square km of sky
//
// All three are TILEABLE and STATIC: only the sampling position animates (wind), so
// this bakes exactly once at init and then costs nothing per frame. Total ~8.4 MB.
//
// The split between a big low-frequency shape volume and a small high-frequency
// detail volume is Schneider's (Horizon Zero Dawn / Nubis): one decides where cloud
// is, the other what its edge looks like. A single volume carrying both would need
// far more memory for the same silhouette.
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace Clouds {

bool        Init();        // idempotent; bakes on first call
void        Destroy();
bool        Ready();

VkImageView ShapeView();
VkImageView DetailView();
VkImageView WeatherView();
VkSampler   Sampler();     // trilinear, REPEAT on all axes (the fields tile)

}}  // namespace VK::Clouds

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — bloom (R4 bloom_build + bloom_filter port).
//
// Quarter-res bright-pass from the HDR scene (exposure-scaled, soft threshold)
// followed by a separable gaussian blur. The result is added back to the linear
// HDR in the tonemap composite — mathematically what R4's blend_soft does
// (inverse-tonemap → add bloom → re-tonemap). Runs inside Pass_TonemapComposite
// after SceneColor::GenerateMips (it samples the scene's quarter-res mip).
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace BloomPass {

bool        Init();
void        Destroy();

// Record the bloom chain: build (scene mips → A), blur H (A → B), blur V
// (B → A). Leaves the result image in SHADER_READ. sceneGen = the SceneColor
// generation (descriptor rebind trigger).
void        Execute(VkCommandBuffer cmd, u32 imageIndex, VkExtent2D sceneExtent, u32 sceneGen);

VkImageView GetResultView();   // blurred bloom (quarter res) — bound by the tonemap
u32         Generation();      // bumped on RT recreate — tonemap rebinds its set

}}  // namespace VK::BloomPass

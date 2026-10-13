// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — tonemap / exposure composite pass.
//
// Runs after every scene pass: reads the HDR scene target (vk_scene_color),
// applies exposure + a Reinhard-with-white operator (R4 combine_tonemap.ps),
// and writes the result to the swapchain image. The UI pass then draws on top
// of the swapchain (UI is NOT tonemapped).
#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"

namespace VK {

namespace TonemapPass {
    bool Init();
    void Destroy();
}

// Composite the HDR scene (SceneColor[imageIndex]) → swapchain image[imageIndex].
void Pass_TonemapComposite(FrameContext& ctx);
// RENDER_THREAD_PLAN step 8: the exposure enclosure rays, PREPARE phase (extract side).
void Tonemap_ExtractEnclosure();

}  // namespace VK

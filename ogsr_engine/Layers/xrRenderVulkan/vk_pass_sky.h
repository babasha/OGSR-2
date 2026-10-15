// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Sky pass.
//
// Phase 6: fills cleared (z = 1.0) pixels with a sky colour. Procedural
// gradient for now; cubemap path lands once CEnvironment is hooked. Runs
// after Pass_World so it can depth-test against the world's z buffer.

#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"

namespace VK {

namespace SkyPass {
    bool Init();
    void Destroy();

    // Load (if needed) the current weather sky cubes and hand back their views +
    // the shared trilinear sampler + the cross-fade weight, for the hemisphere
    // sky ambient (vk_env_light samples these as diffuse irradiance). Returns
    // false if the sky system isn't up yet — the caller keeps its fallback.
    bool AcquireAmbientCubes(VkImageView& v0, VkImageView& v1, VkSampler& sampler, float& weight);
}

void Pass_Sky(FrameContext& ctx);

}  // namespace VK

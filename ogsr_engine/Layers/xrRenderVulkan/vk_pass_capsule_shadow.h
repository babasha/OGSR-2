// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — capsule sun shadows for characters (the "R1-hybrid" shadow option, r_shadow_lite 1).
//
// R1 never put characters into a shadow map: it drew a cheap projected silhouette under them. This is the same idea
// done analytically: every nearby skeleton is a capsule (bottom/top centre of its bounding box + a radius), and one
// depth-sampled fullscreen multiply darkens every pixel whose ray to the sun passes through a capsule, with a penumbra
// that widens with distance from the caster, plus a soft contact patch on the ground at the feet. It works on slopes
// and walls (no receiver geometry), costs a few ALU per pixel per capsule, and does not depend on the shadow-map
// resolution. Not a replacement of the cascades: they stay for static-on-static; with r_shadow_lite 1 only the
// characters are taken OUT of the cascades and drawn here instead.
//
// cvars: r_shadow_lite (master, see vk_console_min.cpp), r_capsule_shadow (strength 0..1).
#pragma once
#include "vk_pass_context.h"

namespace VK {
    void Pass_CapsuleShadows(FrameContext& ctx);
// RENDER_THREAD_PLAN step 8: sun probes + character gather, PREPARE phase (extract side).
void CapsuleShadows_Extract();
    void CapsuleShadows_Destroy();
}

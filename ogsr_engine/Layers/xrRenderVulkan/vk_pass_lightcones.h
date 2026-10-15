// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — per-light volumetric cones (real directional light beams).
//
// Replaces the R4 `models\lightplanes` texture-sheet fakes: every spot light
// the game flags volumetric (headlights, searchlights, pole lamps) gets an
// analytic raymarched cone of in-scatter added over the HDR scene, built from
// the light's real position/direction/cone/range/colour. Depth-sampled additive
// fullscreen pass, right after Sky.
//
// cvars: r_light_cones (master, 2 = marched-cone debug), r_light_cone_density,
// r_light_cone_len (beam length = range × this), r_light_cone_soft (penumbra),
// + the r_light_cone_synth family for lightplanes-derived beams.
#pragma once
#include "vk_pass_context.h"

class vkRender_Visual;

namespace VK {
    void Pass_LightCones(FrameContext& ctx);
    void LightCones_Destroy();

    // Frame registry of lightplanes carriers (car headlights, searchlights…)
    // whose beam cone was synthesized from the fan geometry (vkSynthBeam).
    // RenderQueue::Push submits every lit-blend item here; Pass_LightCones
    // raymarches these next to the real volumetric spots — the fake sheets
    // themselves are never drawn, the synthesized beam replaces them.
    namespace SynthCones {
        void Submit(const vkRender_Visual* vis, const Fmatrix& xform);
        // Explicit OFF: the carrier is drawn but the beam's bone is hidden
        // (weapon torch switched off). View-culled carriers stay lit.
        void Revoke(const vkRender_Visual* vis);
    }
}

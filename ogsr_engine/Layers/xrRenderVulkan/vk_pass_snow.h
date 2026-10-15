// xrRenderVulkan - Snow MESH pass (VHM-style dense snow surface).
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// A dense, player-centred grid mesh rendered as the snow surface (r_snow_mesh).
// Decoupled from the coarse terrain so it has enough vertices for SMOOTH footprint
// dents (which terrain tessellation could not resolve). Samples the clean ground
// map for its base height, raises by the snow blanket, and presses dents from the
// persistent deform texture (vk_deform). Drawn after Pass_World into the same HDR +
// depth, lit via the shared EnvLight set. See shaders/snow_mesh.{vert,frag}.glsl.
#pragma once
#include "vk_pass_context.h"

namespace VK {

bool SnowMesh_Init();
void SnowMesh_Destroy();
void Pass_SnowMesh(FrameContext& ctx);

}  // namespace VK

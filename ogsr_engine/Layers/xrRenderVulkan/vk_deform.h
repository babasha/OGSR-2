// xrRenderVulkan - Snow (and mud) DEFORMATION buffer.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// A PERSISTENT top-down compression field aligned to the rain ortho box (same
// VP/size as the rain map). Feet (and later wheels) STAMP depressions into it; it
// is NOT cleared each frame (trampled snow stays trampled) and recovers only very
// slowly. The snow displacement (snow_displace.glsl) reads it to carve footprints
// into the geometric snow volume. Compute: see deform_stamp.comp.glsl. Modeled on
// VK::WaterSim (persistent ping-pong buffer + per-frame reprojection on camera move).
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace Deform {

// One foot/contact stamp this frame (world space). pressDepth 0..1 = how hard.
// dirX/dirZ = facing (world XZ, normalized) -> BOOT-shaped oriented print with a
// tread pattern; (0,0) = round contact (dropped items).
struct Stamp { Fvector pos; float radius; float pressDepth; float dirX, dirZ; };

bool Init();        // idempotent; lazily created on first Dispatch
void Destroy();
bool Ready();

// Advance one frame: reproject the persistent field into the current rain VP,
// apply slow recovery, then stamp `stamps`. No-op when disabled (r_snow_deform 0).
// Call once per frame after the rain/ground maps are rendered (Pass_SunShadow).
void Dispatch(VkCommandBuffer cmd, const Stamp* stamps, u32 count);

// Compression field (R16F, 0 = untouched .. 1 = fully pressed) + sampler, for the
// EnvLight set (binding 20). Valid after the first Dispatch; VK_NULL_HANDLE before.
VkImageView GetView();
VkSampler   GetSampler();

// Field geometry, for the EnvLight UBO (deform_vp + deform_tex): the straight-down
// ortho VP (world -> deform NDC), the square texel dimension, and the box half-extent
// (metres). Valid after the first Dispatch.
const Fmatrix& GetVP();
u32            Size();
float          Half();

}}  // namespace VK::Deform

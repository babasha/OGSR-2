// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — Sky specular IBL (image-based lighting) prefilter.
//
// The two weather sky cubes (env_s0/env_s1) ship BC-compressed and single-mip, so
// the forward shaders could only tap them at LOD 0 — no roughness blur, so glass /
// wet / "reflect the sky" looked like a sharp mirror at every roughness. This module
// resamples the BLENDED sky into an RGBA16F cube with a roughness-mip chain (mip N =
// rougher reflection) that the receivers sample with the split-sum specular IBL term
// (env_common.glsl iblSpecular). The sun's own GGX highlight rides the same F0/roughness.
//
// The prefilter is recorded into the FRAME's command buffer, on demand (only when the
// weather cube view changes or the cross-fade moves). v1 = straight resample + box-blit
// mips; the clean upgrade is per-mip GGX importance-sample convolution.
//
// ⚠⚠It used to run on the fence-waited IMMEDIATE queue, and "the sky changes rarely so
// there is no per-frame cost" was true of the GPU work and false of the cost. The submit
// went to the GRAPHICS queue and then blocked the main thread on its fence, so the CPU
// waited for the whole queue to drain — the entire frame's rendering — for a ~1 MB cube.
// Measured 01-08: 22-40 ms of CPU inside Pass_World, every few seconds, while the frame's
// own GPU zones read completely normal (the work was on a SEPARATE submit, so no zone
// could see it). That was the stutter users reported, and it fired while standing still
// because the trigger is the sky cross-fade, which advances on the game clock.
// See vk_env_light.cpp EnvLight::Update for the call site and the W/pro/env probe.
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace IBL {

bool        Init();                 // idempotent; safe to call from EnvLight::Init
void        Destroy();
bool        Ready();                // true once a prefiltered cube exists (has content)

// Refresh the prefiltered cube from the current weather cubes if they changed.
// Called from EnvLight::Update with the same views/weight it binds for skyAmbient.
// No-op (cheap early-out) when nothing changed.
//
// `cmd` must be the frame's command buffer, OUTSIDE any dynamic-rendering scope
// (compute + blits are illegal inside one) and BEFORE anything samples binding 26/31
// this frame — i.e. exactly where Pass_World calls EnvLight::Update. Pass VK_NULL_HANDLE
// from call sites that cannot promise that: the refresh is simply skipped and the probe
// keeps last frame's content, which for a <2%-per-refresh sky is invisible.
//
// skyRotation (rad) is the weather's sky_rotation — the SAME spin the dome draw
// applies. It is part of the probe's identity, not a cosmetic extra: without it
// the probe's azimuth drifts away from the visible sky, so it participates in the
// change detection like the cubes and the cross-fade do.
// groundBounce scales the below-horizon hemisphere during the SH projection (the
// sky cube's skirt is not sky radiance — see sky_sh_project.comp.glsl).
// Everything that defines WHAT SKY the probe represents. Every field gates the
// refresh: if any of them moves, the probe is a different sky and must be rebuilt.
// This arc's recurring bug was exactly a field missing from here — first the
// half-cube remap, then sky_rotation, then sky_color — each time leaving the probe
// describing a sky the player was not looking at.
struct SkyDesc {
    float weight       = 0.f;                  // weather cube cross-fade
    float rotation     = 0.f;                  // sky_rotation (rad), as the dome draw uses
    float groundBounce = 0.3f;                 // below-horizon weight in the SH projection
    float tint[3]      = { 1.f, 1.f, 1.f };    // sky_color — the dome's ×1.7 tint
    float sunDir[3]    = { 0.f, 1.f, 0.f };    // direction TO the sun (unit)
    // Procedural Rayleigh+Mie sky (r_sky_proc). When on, the cubes/tint are bypassed
    // and radiance comes from atmosphere.glsl — the same function the dome draws.
    bool  proc         = false;
    float intensity    = 22.f;
    float turbidity    = 1.f;
    float mieG         = 0.76f;
};

void        Update(VkCommandBuffer cmd, VkImageView sky0, VkImageView sky1, VkSampler skySampler, const SkyDesc& sky);

VkImageView GetSpecView();          // prefiltered specular cube (roughness mips); null until Ready
VkSampler   GetSampler();           // trilinear clamp sampler (maxLod = mip count)
u32         GetMaxMip();            // highest roughness mip index (= mip count - 1)

// Diffuse sky irradiance as 9 SH coefficients (std430 vec4[9]), projected from the
// prefiltered world-space probe by sky_sh_project.comp. GPU-only; read by the
// forward receivers via skyAmbient() in env_common.glsl.
VkBuffer    GetSHBuffer();          // null until the first projection has run
bool        SHReady();
bool        ReadSHMirror(float out[36]);   // the 9 SH9 coefficients (vec4 each) once a mirrored copy has completed

}}  // namespace VK::IBL

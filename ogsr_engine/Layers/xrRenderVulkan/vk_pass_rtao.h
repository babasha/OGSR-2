// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — hardware ray-traced AO (RTAO), layered over GTAO.
//
// This is NOT a separate AO pipeline: it draws into the SAME half-res raw-AO
// attachment GTAO just wrote, with a MIN blend. Everything downstream (the
// depth-aware blur, the temporal EMA, the EnvLight binding the world shaders
// sample) therefore needs no changes at all, and the two AO sources compose the
// only way that is always safe — whichever one sees an occluder wins:
//
//   GTAO  — screen space: sees NPCs, weapons, doors, anything dynamic, but only
//           what is ON SCREEN and only as far as its horizon march reaches.
//   RTAO  — world space: sees off-screen and behind-the-camera geometry with a
//           true cosine-weighted hemisphere, but only the STATIC collision world
//           inside the camera-local BLAS (vk_rt_scene).
//
// r_rtao_debug 1 swaps the min blend for a plain overwrite, i.e. shows the ray
// traced term alone — the A/B that tells the two apart.
#pragma once
#include "HW_Vulkan.h"
#include "vk_pass_ssao.h"   // VK::ProjTerms (shared depth-reconstruction terms)

namespace VK { namespace RTAOPass {

// Pipelines + sets. Call after the shader manager exists; a failure here is not
// fatal (RTAO simply stays off and GTAO keeps running alone).
bool Init();
void Destroy();

// Device supports ray query, init succeeded, r_rtao is on and the RT scene has a
// TLAS covering the camera. SSAOPass tests this before calling Overlay.
bool Enabled();

// Min-blend the traced AO over the raw GTAO result. `aoView`/`ilView` are
// SSAOPass's RAW (index 1) targets, both already in COLOR_ATTACHMENT_OPTIMAL;
// the IL attachment is bound only to satisfy the shared attachment layout and is
// write-masked off. `pt` must be the terms GTAO itself used this frame (derived
// from the matrix that rendered the prepass depth), `jitterPhase` the same
// per-frame phase, so the temporal EMA averages a different ray set each frame.
void Overlay(VkCommandBuffer cmd, VkImageView aoView, VkImageView ilView, VkExtent2D extent,
             VkImageView depthView, VkImageView normalView, VkSampler sampNear,
             const ProjTerms& pt, float jitterPhase);

}}  // namespace VK::RTAOPass

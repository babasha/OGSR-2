// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — GTAO screen-space ambient occlusion (R4 gtao.h port).
//
// Runs INSIDE Pass_World, between the depth prepass and the color pass: the
// prepass depth (statics + alpha-tested statics) is the only input, so AO is
// ready before any forward shading samples it. Half-res R8 target: GTAO
// (ssao.frag) → depth-aware 3×3 blur (ssao_blur.frag).
//
// Consumers reach the result through the shared EnvLight set (binding 8) — the
// world/skinned fragments multiply their hemisphere+ambient terms by it, which
// is exactly what R4's combine_1.ps does (hdiffuse *= occ; sun & dynamic
// lights are NOT occluded).
#pragma once
#include "HW_Vulkan.h"

namespace VK {

// Perspective terms + camera basis derived from a row-major view·proj matrix.
// Device.mProject is NOT maintained on the Vulkan path (it reads as identity),
// so any pass that decodes the scene depth buffer (SSAO, sun shafts) must pull
// the terms out of the matrix that actually RENDERED that depth
// (Device.mFullTransform == FrameContext.viewProj). Extraction for M = V·P
// (row-vector, V affine orthonormal): _33 = M_i3/M_i4, _43 = M_43 − M_44·_33,
// 1/tan = |M column|; camera dir/right/top are the w/x/y columns normalized.
struct ProjTerms {
    float   p33, p43;     // depth decode: zview = p43 / (zndc − p33)
    float   tanX, tanY;   // tan(fov/2) horizontal / vertical
    Fvector dir, right, top;   // camera basis (unit)
};
ProjTerms DeriveProjTerms(const Fmatrix& viewProj);

namespace SSAOPass {

bool Init();      // pipelines/sets; call after the SPIRV loader exists (vk_RenderDeviceRender)
void Destroy();

// True when the pass will actually run (init OK + quality > 0). Pass_World
// uses this to decide whether to flip depth to SHADER_READ around Execute.
bool Enabled();

// True when the SHARED buffers this file owns have a reader — the AO itself, or
// the final gather, which takes its ray origins from the normal G-buffer and its
// screen-space radiance cache from the colour history.
// ⚠ USE THIS, NOT Enabled(), FOR ANYTHING THAT PRODUCES THOSE TWO IMAGES. They are
// deferred-renderer resources that happen to live in the AO pass, and gating their
// existence on r_ssao is what made switching AO off switch GI off with it — a
// failure that only shows up as the lighting going FLAT, never as an error.
bool ResourcesWanted();

// (Re)create the half-res RTs for this scene extent. Pass_World calls this
// BEFORE EnvLight::Update so the descriptor it binds at binding 8 is the view
// Execute will render into this frame (not one a resize is about to destroy).
bool EnsureTargets(VkExtent2D sceneExtent);

// Record GTAO + blur. Depth must already be SHADER_READ_ONLY; leaves the AO
// result in SHADER_READ_ONLY for the color pass that follows.
void Execute(VkCommandBuffer cmd, VkExtent2D sceneExtent);

VkImageView GetResultView();   // final blurred AO; VK_NULL_HANDLE until first Execute sized the RTs
VkSampler   GetSampler();      // linear/clamp — receivers bilinearly upsample the half-res AO
u32         Generation();      // bumps when the RTs are (re)created — rebind triggers
float       Strength();        // ao_params.z for the Lighting UBO (0 disables in-shader)

// SSIL — one-bounce indirect light, gathered in the SAME GTAO horizon march (MRT
// target 1) from the previous frame's lit colour. The tonemap composite adds it
// (binding 5). VK_NULL_HANDLE until the first Execute with r_ssil on.
VkImageView GetILResultView();

// Capture THIS frame's lit scene (half-res, linear HDR) into the history buffer
// the NEXT frame's GTAO samples for IL. Blits `srcImage` (the composited HDR
// scene, SHADER_READ) down to the half-res prev-colour target. Called from the
// tonemap pass after the scene is complete. No-op when nothing wants the history.
void CapturePrevColor(VkCommandBuffer cmd, VkImage srcImage, VkExtent2D srcExtent);

// The history buffer itself — SSIL is no longer its only reader. The final
// gather uses it as a SCREEN-SPACE RADIANCE CACHE: a hit that was on screen last
// frame reads the lit pixel instead of interpolating the probe lattice. Null
// until EnsureTargets; SHADER_READ_ONLY (cleared black before the first capture,
// so it is always safe to sample even if nothing has been captured yet).
VkImageView GetPrevColorView();

// NPC normal G-buffer (full-res RGBA8). The skinned normal pass renders into it
// between the depth prepass and Execute; GTAO samples it (real normals where an
// NPC is visible, depth-derived fallback elsewhere). Null until EnsureTargets.
VkImage     GetNormalImage();
VkImageView GetNormalView();
VkFormat    GetNormalFormat();
VkExtent2D  GetNormalExtent();

}  // namespace SSAOPass
}  // namespace VK

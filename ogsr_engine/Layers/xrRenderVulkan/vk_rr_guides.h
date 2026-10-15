// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// ============================================================================
//  DLSS RAY RECONSTRUCTION — GUIDE G-BUFFER (vk_rr_guides)
// ============================================================================
//  RR is not a post filter you switch on. It is a denoiser that reconstructs the
//  lighting from the noisy colour, and to do that it has to be told what part of
//  each pixel is MATERIAL and what part is LIGHT. That separation is what these
//  three targets carry — NVIDIA calls them guide buffers, and the whole quality
//  of the result rides on them being honest.
//
//  WHY THEY COME OUT OF THE COLOUR PASS
//  ------------------------------------
//  The obvious cheap route is a second rasterization (the AO normal prepass
//  already does exactly that for GTAO). It is the wrong one here:
//
//    * the normal RR wants is the SHADING normal — after POM, after the material
//      bump, after the snow blend. A re-raster gives the interpolated vertex
//      normal, which is a different vector on every bumped surface in the game.
//    * the albedo RR wants is the one that actually multiplied the lighting —
//      after the detail texture, after the snow tint. A re-raster would have to
//      re-bind every material and re-run half the fragment shader to get it.
//
//  Inside the forward colour pass all three values ALREADY EXIST as locals a few
//  lines before the light loop. Writing them costs three MRT stores and no new
//  fetches, and they are by construction the exact values the shading used.
//
//  ⚠ TRANSPARENCY MUST NOT WRITE HERE. Water, particles, light cones, decals and
//  rain composite on top of an already-shaded pixel; their "albedo" is not the
//  material behind them, and RR has a separate (optional) transparency path for
//  exactly this. Those passes keep their single-attachment scopes untouched.
//
//  THE THREE TARGETS (all at RENDER resolution — RR consumes render-res inputs)
//  ---------------------------------------------------------------------------
//    RR.NormalRough  RGBA16F   world-space shading normal .xyz + LINEAR roughness .w
//                              (feature is created with Roughness_Mode_Packed)
//    RR.DiffAlbedo   RGBA8     diffuse reflectance. ⚠NOT sRGB — the SDK states
//                              sRGB formats are not supported for this input.
//    RR.SpecAlbedo   RGBA8     EnvBRDFApprox2(F0, roughness², NdotV) — the average
//                              specular reflectivity for THIS view direction. The
//                              exact function is in the SDK's appendix; ours lives
//                              in shaders/rr_guides.glsl.
//
//  ⚠ CLEARED EVERY FRAME, AND NOT TO BLACK. Leaving sky pixels unwritten is the
//  integration error the SDK guide calls out by name; its recommended default is
//  albedo (0.5, 0.5, 0.5). So the frame starts with both albedos at 0.5 and the
//  normal at zero, and opaque geometry overwrites what it covers. A geometry
//  class that has not been converted yet therefore reads as "flat grey material",
//  which is wrong but bounded — never as "black hole in the guide".
// ============================================================================
#pragma once
#include "vk_core.h"

namespace VK { class RenderingBuilder; class GfxPipelineBuilder; }

namespace VK { namespace RRGuide {

// r_dlss_rr != 0 — the cvar alone. Decides whether the targets get built at all;
// with RR off they do not exist and no pass pays for them.
bool Wanted();

// ...and the three targets exist at the current render extent. Every writer and
// every scope site gates on THIS, so a failed allocation degrades to "RR off"
// instead of to a half-bound framebuffer.
bool Ready();

void EnsureSize(VkExtent2D extent);   // (re)create on render-extent change
void Destroy();                       // targets only (the r_dlss_rr toggle path)
void Shutdown();                      // targets + the lazily built debug viewer (renderer teardown)

// Clear the three targets to their integration-safe defaults and leave them in
// COLOR_ATTACHMENT for the rest of the frame — the same single-layout discipline
// ctx.colorView/depthView follow (see vk_pass_context.h). Called once from
// CRender::Begin, NOT from a pass: Pass_World returns early when there is no
// level, and the overlay scopes (sky/trees/grass) run anyway.
void BeginFrame(VkCommandBuffer cmd);

VkExtent2D  Extent();
u32         Generation();   // bumped on every (re)create — RR feature rebuild trigger

VkImage     NormalImage();   VkImageView NormalView();   VkFormat NormalFormat();
VkImage     DiffAlbImage();  VkImageView DiffAlbView();  VkFormat DiffAlbFormat();
VkImage     SpecAlbImage();  VkImageView SpecAlbView();  VkFormat SpecAlbFormat();

// ---- scope + pipeline plumbing ---------------------------------------------
// Append the three guide targets (LOAD/STORE) to an OPAQUE scene rendering scope,
// right after the colour attachment. No-op unless Ready(). The two callers are
// Pass_World's colour scope and BeginOverlayRendering — every opaque draw in the
// frame goes through one of them.
void Attach(RenderingBuilder& rb);

// Declare the same three attachments on a pipeline built for such a scope.
// `write` false → the formats are still declared (dynamic rendering requires the
// pipeline's attachment list to match the scope) but with colorWriteMask 0, which
// is how a geometry class that has not been converted yet stays legal inside a
// guide-carrying scope. That is what makes the rollout incremental instead of one
// 20-shader commit. No-op unless Ready().
void PipelineTargets(GfxPipelineBuilder& pb, bool write);

// ---- NGX hand-off -----------------------------------------------------------
// COLOR_ATTACHMENT -> GENERAL before the RR evaluate and back after, mirroring the
// layout contract the SR path already uses for colour/depth/MV (vk_dlss.h): the
// caller owns the input transitions because it is the one that knows what layout
// the rest of the frame needs them in.
void ToGeneral(VkCommandBuffer cmd);
void ToAttachment(VkCommandBuffer cmd);

// ---- r_dlss_rr_debug --------------------------------------------------------
// Overwrite the (already tonemapped) swapchain with one guide channel:
//   1 diffuse albedo · 2 specular albedo · 3 normal · 4 roughness.
// Drawn POST-TONEMAP on purpose — the question a guide view answers is "is this
// number right", and the tone curve would move the number. No-op when the cvar
// is 0 or the targets do not exist. This is the instrument every later step is
// verified with: a bad guide makes RR look merely soft, which reads as a DLSS
// quality opinion rather than as the bug it is.
void DrawDebugOverlay(VkCommandBuffer cmd, VkImageView dstView, VkExtent2D extent);

}}  // namespace VK::RRGuide

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — screen-space motion vectors (foundation for DLSS/FSR
// upscaling, frame-generation and the path-tracer denoiser).
//
// Phase 1: a single fullscreen pass reconstructs camera/static motion from the
// prepass depth + the previous frame's view-projection — no per-geometry shader
// changes. Self-moving geometry (skinned/trees/grass) is a later phase that
// overwrites this field where dynamics draw.
//
// Owns one full-res RG16F target (prevUV − curUV, in [0,1] UV space). Modelled
// on VK::SSAOPass (depth reconstruction) + VK::SceneColor (target lifecycle).
#pragma once
#include "vk_core.h"

namespace VK { struct FrameContext; }

namespace VK { namespace MotionVec {

bool        Init();                                   // load shaders, pipelines, sets (after swapchain ready)
void        Destroy();
const Fmatrix& CurVP();                               // this frame's UNJITTERED view-proj (SL DLSS constants)
const Fmatrix& PrevVP();                              // previous frame's UNJITTERED view-proj
void        EnsureSize(VkExtent2D extent);            // (re)create the RG16F target on size change
bool        Enabled();                                // r_motion_vectors != 0 && inited && target ready

// Option A (jitter-free MV): CRender::Begin calls this ONCE per frame with the
// UNJITTERED world/HUD view-proj (BEFORE it applies the DLSS sub-pixel jitter to
// Device.mFullTransform) plus this frame's jitter offset in D3D-NDC (0 when DLSS
// off). The MV passes reproject with these unjittered matrices so motion vectors
// carry NO jitter wobble, and re-apply `jitterNdc` only to gl_Position so raster/
// depth still bit-match the jittered forward geometry.
void        SetFrameVP(const Fmatrix& worldVP, const Fmatrix& hudVP, float jitterNdcX, float jitterNdcY);

// Roll the cur/prev view-proj history for THIS Device.dwFrame (idempotent). The
// MV passes do it themselves, but they run AFTER the World pass — a consumer that
// needs PrevVP() earlier in the frame (the SSAO temporal reprojection) has to ask,
// or it reads the matrix pair as it was left by the PREVIOUS frame.
void        EnsureHistory();
void        FrameJitter(float& ndcX, float& ndcY);   // this frame's sub-pixel jitter in D3D-NDC (0 when no upscaler)

// Reconstruct camera/static screen-space motion from the prepass depth. Caller
// must have the scene depth in SHADER_READ_ONLY. Leaves the MV target in
// SHADER_READ_ONLY (so the debug overlay / future DLSS can sample it).
void        Execute(VkCommandBuffer cmd, VkExtent2D extent);

// ---------------------------------------------------------------------------
// PREPASS-WRITTEN MOTION (r_mv_prepass). Instead of re-rastering the dynamics a
// second time after the world pass, the DEPTH PREPASS carries an RG16F colour
// attachment cleared to a sentinel: geometry that already runs a fragment shader
// there (tree crowns, for the leaf alpha-test) writes its true screen motion in
// the same raster that lays its depth, and statics bind the same pipelines with
// colorWriteMask 0 so the sentinel survives. ResolveEarly then fills every
// sentinel texel by reconstruction from depth — which makes the motion field
// COMPLETE BEFORE SSAO, the whole point: the AO temporal history's first
// consumer runs immediately after the prepass and used to read the field of the
// previous frame (see r_ssao_reproj).
//
// The prepass scope declares the attachment UNCONDITIONALLY (with a null view
// when off) so every prepass pipeline keeps ONE attachment list and r_mv_prepass
// stays a live toggle instead of a pipeline rebuild.
// ---------------------------------------------------------------------------
bool        PrepassWanted();                          // r_mv_prepass != 0 && Enabled()
// Prepare the prepass field for THIS frame: (re)create it, transition it to
// COLOR_ATTACHMENT, and return the view to hang on the prepass scope. Returns
// VK_NULL_HANDLE when the path is off — hang THAT on the scope, the attachment
// count must not change.
VkImageView BeginPrepassTarget(VkCommandBuffer cmd, VkExtent2D extent);
void        EndPrepassTarget(VkCommandBuffer cmd);    // → SHADER_READ_ONLY, ready for ResolveEarly
VkClearColorValue PrepassClearValue();                // the sentinel every prepass scope clears to
// Fill the sentinel texels from the prepass depth (caller must have the scene
// depth in SHADER_READ_ONLY) and leave the MV target in SHADER_READ_ONLY. After
// this the field is final for statics + whatever wrote itself in the prepass;
// the leftover overlays (grass, HUD weapon — neither is in the prepass) still
// draw over it in ExecuteDynamic.
void        ResolveEarly(VkCommandBuffer cmd, VkExtent2D extent);
// Did ResolveEarly run for THIS Device.dwFrame? ExecuteDynamic asks: if it did,
// the fullscreen reconstruction and the tree overlay must NOT run again.
bool        ResolvedEarly();

// Dynamic overlay (Phase 2: self-moving geometry — skinned NPCs now, trees/grass
// later). Re-draws dynamics into the MV target with depth-test against the
// COMPLETE scene depth, so call it AFTER all opaque passes. Reads ctx.depthView +
// ctx.viewProj. No-op unless Enabled() and the static Execute ran this frame.
void        ExecuteDynamic(VkCommandBuffer cmd, const FrameContext& ctx);

// r_mv_debug: overwrite dstView (the swapchain, COLOR_ATTACHMENT) with a
// false-colour view of the motion target. No-op when r_mv_debug == 0.
void        DrawDebugOverlay(VkCommandBuffer cmd, VkImageView dstView, VkExtent2D extent);

VkImageView GetResultView();   // RG16F motion, SHADER_READ after Execute (null until ready)
VkImage     GetResultImage();  // the RG16F motion image (for DLSS resource tagging / barriers)
VkSampler   GetSampler();
VkFormat    Format();
u32         Generation();      // bumped on every (re)create

}}  // namespace VK::MotionVec

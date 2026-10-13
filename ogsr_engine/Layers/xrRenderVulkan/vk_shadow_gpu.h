// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  GPU-DRIVEN SHADOW CASTERS (vk_shadow_gpu)
// ============================================================================
//  Static world geometry is in SHARED VB/IB pools and baked in WORLD space
//  (identity model xform), so shadow casters need NO per-instance transform —
//  only metadata (bounds + draw params). This mirrors the GPU-driven tree path
//  (vk_TreeManager): extract caster meta at level load → per-target compute cull
//  (sphere vs the light frustum) → vkCmdDrawIndexedIndirectCount into the depth
//  target, reusing the existing depth pipelines.
//
//  Replaces the CPU per-object FlushDepth of statics in the sun shadow pass.
//  Per-frame near cascades benefit directly; the cached far map redraws cheaper
//  and smoother. Foundation for caster-LOD, grass shadows and (later) RT shadows.
//
//  v1: OPAQUE statics only (plain depth pipeline). Alpha-tested casters stay on
//  the CPU AT path until a textured group path is added.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace ShadowGPU {

// Cull/draw targets — each has its own light VP + frustum + indirect region.
enum Target : u32 { TGT_FAR = 0, TGT_CASCADE0, TGT_CASCADE1, TGT_COUNT };

void Build();    // extract opaque static casters at level load (idempotent)
bool Built();
void Destroy();

// Batch compute-cull `n` targets in ONE compute phase: `tgts[i]` is culled
// against `planes + i*6` (6 world-space frustum planes per target). MUST run
// OUTSIDE a dynamic-rendering scope (compute). Records into each target's
// indirect/count region with a single shared WAR / transfer / compute→indirect
// barrier set, and one dispatch per target (the caster carries its group id, so
// no per-group dispatch). Batching all of a frame's targets here keeps the depth
// raster from interleaving with compute (avoids per-target compute↔graphics
// stalls).
// camPos + lodDist drive caster-LOD: casters farther than lodDist from camPos
// emit their coarse LOD slice (lodDist <= 0 disables it → always full detail).
void Cull(VkCommandBuffer cmd, const Target* tgts, const Fvector4* planes, u32 n,
          const Fvector& camPos, float lodDist);

// Draw the culled casters into the currently-bound depth target with `lightVP`.
// MUST run INSIDE the target's vkCmdBeginRendering (viewport/scissor/bias set by
// the caller). Reuses the shared depth pipelines.
void Draw(VkCommandBuffer cmd, Target tgt, const Fmatrix& lightVP);

// Caster metadata access for OTHER systems that render the same opaque static
// casters (VSM binning/render reuse this instead of re-walking the visuals).
// GpuCasterMeta is std430, 48 B: vec3 sphere_P, float sphere_R, then draw ranges.
VkBuffer GetMetaBuffer();   // VK_NULL_HANDLE until Built()
u32      CasterCount();     // number of meta entries (deduped opaque casters)
u32      MetaStride();      // sizeof(GpuCasterMeta) — for non-array SSBO binding sanity
u32      GroupCount();      // number of (vb,ib,stride) groups
u32      MaxGroupMesh();    // largest group's mesh count (cull/indirect region size)
// Per-group vb/ib/stride for a foreign render loop (VSM draws the same casters into
// its atlas using its own indirect buffer). Returns false if g is out of range.
bool     GetGroupBind(u32 g, VkBuffer& vb, VkBuffer& ib, u32& stride, VkIndexType& iType);

}} // namespace VK::ShadowGPU

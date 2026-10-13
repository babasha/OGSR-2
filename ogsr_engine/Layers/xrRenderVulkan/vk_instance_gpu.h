// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  GPU-DRIVEN INSTANCED RIGID CASTERS (vk_instance_gpu)
// ============================================================================
//  Rigid visuals pushed by a HOST (the SDK level editor) arrive in
//  g_DynamicVisuals as {visual, xform} pairs: ONE shared mesh per distinct .ogf,
//  instanced many times over. Rigid_RenderShadow draws them one at a time on the
//  CPU — 6777 draw calls per shadow target on Cordon, re-issued for the far map
//  and again for every cascade. That is the editor's dominant frame cost.
//
//  WorldGPU/ShadowGPU cannot take them. Those bake geometry into WORLD SPACE at
//  level load and carry no transform at all (see vk_world_gpu.h:18,
//  world_cull.comp.glsl:8), so feeding them would explode 434 shared meshes into
//  7000 world-space copies and turn every edit into a re-bake. An editor moves
//  objects — that is the whole point of one.
//
//  This is the OTHER shape, the one CTreeManager already uses: geometry stays
//  LOCAL and shared, each instance carries a matrix in an SSBO, a compute pass
//  frustum-culls the instances into per-group indirect lists, and the draw is one
//  vkCmdDrawIndexedIndirectCount per group. The cull writes
//  firstInstance = instance index, so the vertex shader recovers its own matrix
//  from gl_InstanceIndex (needs drawIndirectFirstInstance — enabled device-wide
//  in HW_Vulkan.cpp:188, for the tree path).
//
//  Because of that, MOVING AN OBJECT COSTS 64 BYTES. The meta keeps its cull
//  sphere in MODEL space and the cull shader transforms it per instance, so an
//  edit dirties only the transform SSBO — never the meta, the groups, or the
//  geometry. That is the property the cluster path structurally cannot have.
//
//  v1: OPAQUE rigid leaves, sun shadows only. Alpha-tested leaves stay on the CPU
//  path via RenderQueue::FlushDepth(alphaTestedOnly=true) — exactly the split the
//  static GPU-driven path already uses (see the comment at vk_render_queue.cpp:477
//  "opaque handled by GPU-driven shadow path").
// ============================================================================
#pragma once
#include "HW_Vulkan.h"

class vkRender_Visual;

namespace VK { namespace InstanceGPU {

// Cull/draw targets. 0..2 mirror ShadowGPU::Target so the two paths can be swapped
// at the same call sites (0 = combined far map, 1/2 = cascades).
// The camera view needs TWO regions, not one, and the difference is load-bearing:
//   TGT_COLOR        — everything, alpha-tested included. The world FS applies
//                      alphaRef, so the colour pass needs no split.
//   TGT_COLOR_OPAQUE — opaque only, for the DEPTH PREPASS. The depth-only pipeline
//                      cannot discard, so feeding it alpha-tested leaves would
//                      punch SOLID depth for every leaf card and occlude whatever
//                      is behind the foliage.
enum Target : u32 { TGT_FAR = 0, TGT_CASCADE0, TGT_CASCADE1, TGT_COLOR, TGT_COLOR_OPAQUE, TGT_COUNT };

// One cullable INSTANCE: a rigid leaf mesh plus the slot of its transform.
// Meta index == transform index == firstInstance, so no extra indirection.
// std430, 32 B — layout mirrors `Meta` in instance_cull.comp.glsl.
struct GpuInstMeta {
    Fvector sphere_P; float sphere_R;   // cull sphere in MODEL space (see header note)
    u32 index_count; u32 ib_first; u32 first_vertex; u32 group;
};

// Structural change (objects added/removed, scene cleared/pushed): the meta,
// the groups and the buffer sizes must be rebuilt. Cheap to call redundantly.
void Invalidate();

// Transforms changed but the object set did not (gizmo drag, property edit,
// undo). Only the transform SSBO is re-uploaded — no realloc, no device wait.
void TouchTransforms();

// True once a non-empty instance set is live and the pipelines built OK. Until
// then the caller must keep using the CPU Rigid_RenderShadow path.
bool Ready();

// True if this TOP-LEVEL visual is drawn by this path. Ownership is whole-visual:
// a visual carrying any late-translucent leaf (wmark/glass/emissive/lit-blend) is
// left entirely to the CPU queue, because the CPU submit works per visual and a
// split would double-draw its ordinary leaves. Every CPU submit — colour and
// shadow alike — must skip exactly the visuals this returns true for; that is what
// keeps the two halves from overlapping or leaving a gap.
bool OwnsVisual(vkRender_Visual* v);

// Diagnostics.
u32  InstanceCount();
u32  GroupCount();

void Destroy();

// Compute-cull `n` targets against `planes + i*6` (6 world-space frustum planes
// per target, same source as ShadowGPU::Cull — VK::ExtractFrustumPlanes) into
// each target's indirect/count region. Rebuilds the set first if it was
// invalidated. MUST run OUTSIDE a dynamic-rendering scope.
void CullShadow(VkCommandBuffer cmd, const Target* tgts, const Fvector4* planes, u32 n);

// Draw one target's culled instances into the currently-bound depth attachment.
// MUST run INSIDE the target's vkCmdBeginRendering (viewport/scissor/bias are the
// caller's, exactly as for ShadowGPU::Draw).
void DrawShadow(VkCommandBuffer cmd, Target tgt, const Fmatrix& lightVP);

// ============================================================================
//  CAMERA VIEW — depth prepass + colour pass
// ============================================================================
// True when the instanced COLOUR path is usable (Ready() plus the instanced world
// VS modules). The host's CPU RenderQueue submit must be skipped when this is on,
// or every object draws twice.
bool ColorReady();

// Compute-cull the whole set against the camera frustum into the TGT_COLOR region.
// Unlike the shadow cull this keeps ALPHA-TESTED entries too: the world FS handles
// alphaRef from the material tail, so colour needs no opaque/AT split (only the
// depth-only shadow pipelines do). MUST run OUTSIDE a dynamic-rendering scope.
void CullColor(VkCommandBuffer cmd, const Fmatrix& viewProj);

// Depth-only prepass draw of the camera-culled set, reusing the shadow depth
// pipelines. INSIDE the prepass BeginRendering.
void DrawColorDepth(VkCommandBuffer cmd, const Fmatrix& viewProj);

// Forward colour draw of the camera-culled set: per group the world pipeline
// (derived exactly like RenderQueue::Flush), the material set and the push tail.
// INSIDE the colour BeginRendering. `envSet` = the EnvLight descriptor set.
void DrawColor(VkCommandBuffer cmd, const Fmatrix& viewProj, VkDescriptorSet envSet);

}} // namespace VK::InstanceGPU

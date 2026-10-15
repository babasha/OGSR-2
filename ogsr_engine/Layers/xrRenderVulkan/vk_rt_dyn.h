// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  RT DYNAMICS (vk_rt_dyn) — NPCs, weapons, doors and props in the BVH
// ============================================================================
//  The camera-local structure (vk_rt_scene) holds the STATIC world: a ray never
//  saw a stalker, a door or a crate, so RTAO left those to screen-space GTAO.
//  This module adds them, once per frame:
//
//    * SOURCE = the compute pre-skinning pool (vk_pass_skinned, r_preskin). The
//      PreSkin pass already writes every visible skeleton's WORLD-space vertices
//      there for the raster; with r_rt_dyn on it writes the 1-weight props too
//      (doors, crates, dropped guns — normally skipped as a raster economy), and
//      lists each leaf with its index buffer. The BLAS is built straight over
//      that pool and those indices — the triangles a ray hits are byte-for-byte
//      the ones the raster draws, with no second copy and no re-skinning.
//    * ONE BLAS, one geometry per leaf, rebuilt every frame (PREFER_FAST_BUILD —
//      a refit would need a stable topology and the visible set changes).
//    * ITS OWN TLAS: the static bubble's two instances (opaque 0x01 / cutout
//      0x02, exactly as vk_rt_scene wrote them) plus the dynamic one under mask
//      0x04. Consumers opt in by mask — RTAO traces 0x05; the GI rays keep
//      tracing the static TLAS and never pay for this.
//
//  Per frame-in-flight slot resources: the pool region a BLAS reads belongs to
//  that in-flight frame, and a structure must not be overwritten while a frame
//  that may still trace it is in flight. First-person HUD geometry is never
//  listed (it would black out the whole near field), nor is the shadow-only
//  actor body.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace RTDyn {

// r_rt_dyn on, RTAO on, pre-skinning on, ray query supported. Read by the
// PreSkin pass to decide whether every leaf has to go through the pool.
bool Wanted();

// Per frame, OUTSIDE a render pass, after the PreSkin pass and before the first
// consumer (RTAO). Records the BLAS + TLAS builds on `cmd`; a frame with no
// dynamic leaves records nothing and Ready() stays false, so consumers fall
// back to the static TLAS.
void Update(VkCommandBuffer cmd);

bool                       Ready();        // this frame's combined TLAS exists
VkAccelerationStructureKHR TLAS();         // static instances + dynamic, or VK_NULL_HANDLE
u32                        TriCount();     // dynamic triangles this frame
u32                        Generation();   // bumps on every build (descriptor rebind trigger)

void Destroy();
void OnLevelUnload();

}}  // namespace VK::RTDyn

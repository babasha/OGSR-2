// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  RAY-TRACING SCENE (vk_rt_scene) — camera-local acceleration structure
// ============================================================================
//  Everything hardware-traced in this renderer (RTAO first) needs ONE thing the
//  engine never had: a BVH of the static world. Building it over the whole level
//  is not an option — pripyat_full's level.cform is 61.3 MILLION triangles
//  (1.39 GB on disk), and a BLAS of that is multiple GB of VRAM.
//
//  But the rays we care about are SHORT (ambient occlusion: 1-3 m) and only
//  matter near the camera, so the structure only has to cover a bubble around
//  the player. This keeps a camera-local BLAS (a box query into the collision
//  model) + a one-instance TLAS, rebuilt on a worker whenever the camera leaves
//  the bubble's inner margin.
//
//  WHY THE COLLISION MODEL AND NOT THE RENDER GEOMETRY
//  ---------------------------------------------------
//  level.cform is already resident (ObjectSpace), is a plain world-space
//  triangle soup with no LOD DAG / no instancing / no per-cluster streaming,
//  and carries the `suppress_shadows` bit that marks collision-only geometry
//  (invisible walls) — exactly the tris that must NOT occlude. The render pools
//  would match the visuals better (foliage, decoration without collision), but
//  they are cluster-streamed and repacked, which is a much larger project. AO
//  from a hair-accurate mesh is not what the eye grades; contact darkening is.
//
//  Slots ping-pong: a rebuild always targets the slot that is NOT being traced,
//  and a slot is only reused once FRAMES_IN_FLIGHT frames have retired since it
//  was last bound — an in-flight frame must never see its BLAS overwritten.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace RTScene {

// Device can ray-query AND the extension entry points resolved. False makes
// every other call here a no-op (and RTAO stays off).
bool Supported();

// Resolve the KHR entry points + acceleration-structure limits. Safe to call
// repeatedly; called lazily from Update.
bool Init();
void Destroy();

// Per-frame. Records BLAS+TLAS builds on `cmd` when a rebuild is due, so it
// MUST be called outside a render pass and before anything that traces this
// frame (the build barrier makes the result visible in the same submit).
void Update(VkCommandBuffer cmd, const Fvector& camPos);

// True when a TLAS is built and the camera is still inside its coverage.
bool                       Ready();
VkAccelerationStructureKHR TLAS();
u32                        Generation();   // bumps on every swap → rebind trigger

// ---------------------------------------------------------------------------
//  SHADING INPUTS — the attributes a hit needs to become a light sample.
//
//  A visibility query (RTAO, shadows) needs none of this. Anything that shades
//  what it hit — indirect light, coloured bounce — needs to know WHAT it hit,
//  and the BLAS itself stores nothing but geometry. These three buffers are that
//  missing half, in the layout shaders/rt_shade.glsl expects:
//
//    VertexBuffer   3 floats per vertex, 3 vertices per triangle, no index
//                   buffer — so primitiveIndex*3 addresses the first vertex.
//    MaterialBuffer one u32 game-material index per TRIANGLE (CDB::TRI::material).
//    AlbedoBuffer   material index -> linear diffuse albedo (vec4, w unused).
//
//  The first two belong to the CURRENT slot and are swapped by a rebuild exactly
//  like the TLAS, so a consumer that caches descriptors must rebind on
//  Generation() change. The albedo table is level-independent and built once.
// ---------------------------------------------------------------------------
VkBuffer VertexBuffer();
VkBuffer MaterialBuffer();
u32      TriCount();
VkBuffer AlbedoBuffer();
u32      AlbedoCount();

// True when the structure currently traced was built from the RENDER geometry
// (r_rt_src 1, vk_rt_world) rather than from the collision model. Consumers that
// shade a hit bind RTWorld's diffuse array either way — the albedo table says per
// material whether there is a texture behind it — but the flag is what the logs
// and the debug view report, and it is how a pass knows the UVs are real.
bool     UsingRenderGeometry();

const Fvector& Anchor();        // centre of the current bubble
float          Coverage();      // half-extent of the current bubble (m)

void OnLevelUnload();           // drop everything (the cform is about to die)

// ---------------------------------------------------------------------------
//  SHARED WITH vk_rt_dyn — the per-frame dynamic structure builds its own TLAS
//  that carries the static bubble's instances PLUS the skeletons, so the KHR
//  entry points and the static instance descriptors are exposed rather than
//  loaded/derived twice.
// ---------------------------------------------------------------------------
struct ASFns {
    PFN_vkCreateAccelerationStructureKHR           create   = nullptr;
    PFN_vkDestroyAccelerationStructureKHR          destroy  = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR    sizes    = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR        cmdBuild = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR address  = nullptr;
};
const ASFns& Fns();             // valid after Init()
u32          ScratchAlign();    // minAccelerationStructureScratchOffsetAlignment
// The current slot's instances exactly as its own TLAS holds them (identity
// transform, masks 0x01 opaque / 0x02 cutout, custom index = first triangle).
// Returns the count written (0..2); 0 when nothing is built.
u32 StaticInstances(VkAccelerationStructureInstanceKHR out[2]);

}}  // namespace VK::RTScene

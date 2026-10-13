// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  RT WORLD FROM RENDER GEOMETRY (vk_rt_world) — the ray-traced world is the
//  world you SEE, not the world you bump into.
// ============================================================================
//  Everything traced so far (RTAO, the RTGI preview, the DDGI probes) hit a BVH
//  cut out of level.cform — the COLLISION model. That was the right first move:
//  cform is resident, world-space, flat, and carries a material index. But it is
//  a different mesh from the one on screen, and the differences are exactly the
//  things indirect light is made of:
//
//    * no foliage, no grass, no decoration — anything without collision is
//      INVISIBLE to a probe ray, so it neither shadows nor bounces;
//    * one albedo per material CLASS ("concrete", "earth"), matched by name,
//      instead of the texture the surface is actually painted with;
//    * invisible collision volumes (blockers, clip brushes) that only a filter
//      bit keeps from casting light they have no business casting;
//    * no emission, no alpha coverage, no LOD control — 61 M triangles that can
//      only be sampled through a bubble.
//
//  This module builds the RT triangles out of the SAME pools the forward world
//  pass draws from (vk_world_gpu + vk_cluster_stream), which means:
//
//    GEOMETRY  the cluster-LOD DAG, cut at an RT-specific error budget. The
//              raster's cut is view-Z projected (it only has to look right on
//              screen); a probe ray leaves the frustum immediately, so the RT
//              cut is EUCLIDEAN around the bubble centre instead.
//    ATTRIBUTES the real UVs, so a hit resolves to a TEXEL of the real diffuse.
//    RESIDENCY  the same 2-bit drawable/leaf state the cull shader reads, from
//              the same GPU buffers, in the same command buffer — so the RT
//              snapshot can never disagree with what the raster considers
//              installed (a page evicted between selection and gather would
//              otherwise put another page's triangles into the BVH).
//
//  WHY A GATHER PASS AND NOT A BLAS OVER THE POOLS DIRECTLY
//  --------------------------------------------------------
//  A BLAS can be built straight out of a vertex/index buffer, and one geometry
//  per entry would need no copy at all. But the pools are STREAMED: a page that
//  gets evicted and refilled changes the bytes under a BLAS that still points at
//  them, and nothing in the API says the structure has gone stale. Copying the
//  selected triangles into an owned buffer makes the BVH a SNAPSHOT — immune to
//  eviction, compaction and repack — for the price of ~16 bytes per triangle.
//  Since it is a compute pass anyway, the copy costs one dispatch per rebuild.
//
//  The gather reads the pools through BUFFER DEVICE ADDRESSES carried in the
//  per-group table, not through descriptors: a level has dozens of VB/IB pools
//  plus the cluster pools, and binding them all would need a descriptor-indexed
//  array of storage buffers per pool set. An address is 8 bytes in an SSBO.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace RTWorld {

// True once the group/material tables are resolved against a built WorldGPU set.
// Cheap after the first success; builds the tables on the first call of a level
// (render thread only — it touches descriptors).
bool Build();
bool Ready();
void Destroy();
void OnLevelUnload();          // the pools die with the level; forget their addresses

// WORKER THREAD. Picks what this bubble will contain — the static entries whose
// LOD cut lands in it, plus every tree instance that reaches it — and returns the
// total triangle count. The job lists stay inside the module; RecordGather uses
// them on the render thread, and the two are serialised by the caller's state
// machine (vk_rt_scene). Reads nothing but host arrays.
u32 Select(const Fvector& center, float half, u32 maxTris);

// How many of those triangles are OPAQUE. The soup is sorted so they come first,
// which lets the caller build two acceleration structures over one buffer: an
// opaque one whose hits commit inside the traversal, and a cutout one whose hits
// have to ask the shader (leaf cards, fences, grates). Ray flags are per geometry
// — without the split, one leaf card would make the whole world non-opaque.
u32 OpaqueTris();

// Why the last Select picked what it picked. Without this the RT world is one
// triangle count with no way to tell "the LOD cut is too coarse" from "half the
// pages are not resident" from "the budget ran out" — three different fixes.
struct SelectStats {
    u32 inRange;       // static entries whose cull sphere reaches the bubble
    u32 noPage;        // ...of those, dropped as not resident (an ancestor covers them)
    u32 lodCut;        // ...dropped by the LOD cut (another level of the DAG draws)
    u32 capped;        // ...dropped for want of soup budget (r_rtao_maxtris)
    u32 picked;        // ...gathered
    u32 treesInRange;  // tree instances reaching the bubble
    u32 treesCut;      // ...of those, dropped past r_rt_tree_range (foliage keeps its own, shorter reach)
    u32 trees;         // ...gathered
};
const SelectStats& LastSelect();

// RENDER THREAD. Uploads the job lists and records the gather dispatches (one
// for the static world, one for the trees): the triangles land in `outPos` (3
// floats x 3 vertices per triangle, the layout the BLAS and rt_shade.glsl both
// expect) and `outAttr` (4 uints per triangle: material slot + 3 half2 UVs).
// Records its own barriers for the pool reads. `tris` must be what Select
// returned — it bounds every write.
bool RecordGather(VkCommandBuffer cmd, u32 tris, VkBuffer outPos, VkBuffer outAttr);

// ---------------------------------------------------------------------------
//  Hit shading inputs (bound by every consumer of rt_shade.glsl as RT_TEX_SET)
// ---------------------------------------------------------------------------
// The diffuse of every material in the static world set, as one descriptor
// array indexed by the material slot in the per-triangle attributes. Slots the
// level does not use point at the cache's 1x1 white, so nothing is ever
// unwritten (PARTIALLY_BOUND would be legal but silently rewards a bug).
VkDescriptorSetLayout TexSetLayout();
VkDescriptorSet       TexSet();
// Per-frame: re-publish the array when texture streaming has swapped a view
// under us. Ring-buffered, so the set written now is never one an in-flight
// frame still reads.
void FrameTick();

// Material slot -> vec4(fallback rgb, texture slot as float). Shared shape with
// the cform table (RTScene): w < 0 means "no texture, use the rgb".
VkBuffer AlbedoBuffer();
u32      AlbedoCount();

}}  // namespace VK::RTWorld

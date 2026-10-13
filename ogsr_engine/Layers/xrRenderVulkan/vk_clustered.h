// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  CLUSTERED FORWARD / FORWARD+ (vk_clustered)
// ============================================================================
//  The forward shaders test all 16 dynamic lights against EVERY fragment with
//  ZERO culling — the structural cost gap vs R4's deferred path (which shades
//  each screen pixel once). This bins the active lights into a screen-space 3D
//  froxel grid (16x9x24) with a single compute dispatch: each cluster's thread
//  builds its view-space AABB and tests every light's bounding sphere, writing
//  the overlapping light indices into a fixed per-cluster slot region (no
//  atomics). The forward fragments then find their cluster from gl_FragCoord +
//  view-space depth and iterate only the ~2-3 lights touching it.
//
//  Result: the per-fragment light cap rises 16 -> 256 AND each shaded pixel is
//  cheaper (it skips the lights that can't reach it). Mirrors the compute-cull /
//  single-shared-buffer + WAR-barrier discipline of vk_world_gpu.
//
//  Behind r_clustered (default OFF): r_clustered 0 keeps the exact old 16-light
//  per-fragment loop. v1 covers world (lmap/vlit/terrain) + skinned; foliage
//  (tree/grass) stays on the 16-light UBO path.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"
#include "vk_light.h"       // Lights::FrameLights / GpuLight / kMaxClusterLights
#include "vk_pass_ssao.h"   // VK::ProjTerms (camera basis + tan/near/far)

namespace VK { namespace Clustered {

// Froxel grid — resolution-independent (tiles scale with the screen).
constexpr u32 kGridX        = 16;
constexpr u32 kGridY        = 9;
constexpr u32 kGridZ        = 24;
constexpr u32 kClusters     = kGridX * kGridY * kGridZ;   // 3456
constexpr u32 kMaxPerCluster = 64;                        // fixed slots, no atomics

// Depth-slice distribution (Olsson/Doom exponential) derived from the camera's
// near/far. The fragment shader needs sliceScale/sliceBias to find its cluster;
// the compute needs near/far for the AABB z bounds. One source of truth.
struct GridZ { float nearZ, farZ, sliceScale, sliceBias, logFarNear; };
GridZ DeriveGridZ(const ProjTerms& pt);

bool Init();        // idempotent; lazy on first UploadLights/Cull
void Destroy();
bool Ready();

// Per-frame: copy the collected lights into this in-flight slot's SSBO region
// (host-visible, double-buffered like the env UBO — CPU writes off the GPU
// timeline). Call before Cull.
void UploadLights(const Lights::FrameLights& fl, u32 slot);

// Per-frame compute cull: bin the uploaded lights into the froxel grid for this
// slot. MUST run OUTSIDE a render pass (mirrors WorldGPU::Cull). `numLights` =
// FrameLights.count (clamped to kMaxClusterLights).
void Cull(VkCommandBuffer cmd, const ProjTerms& pt, const Fvector& eye,
          VkExtent2D extent, u32 slot, u32 numLights);

// Buffer handles for the EnvLight receiver set (bindings 17/18/19).
VkBuffer     GetLightsHandle(u32 slot);
VkDeviceSize GetLightsOffset(u32 slot);
VkDeviceSize GetLightsRange();
VkBuffer     GetGridHandle();
VkBuffer     GetIndicesHandle();

}} // namespace VK::Clustered

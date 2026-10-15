// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  Variable Rate Shading (vk_vrs) — VK_KHR_fragment_shading_rate, attachment-based
// ============================================================================
//  Coarse-shade pixels via a shading-rate image (SRI): a small R8_UINT image,
//  one texel per ~16×16 screen tile, whose value encodes the fragment size
//  (1×1 / 2×2 / 4×4). The World color pass binds it as a shading-rate attachment
//  and sets combinerOps so the attachment REPLACES the rate → distant/peripheral
//  pixels run the (heavy forward) fragment shader once per 4 (2×2) or 16 (4×4).
//
//  v2 = DEPTH-DRIVEN: a compute pass reads the prepass depth and writes the rate
//  per tile by DISTANCE (near → 1×1, far/sky → 2×2 / 4×4). The SRI is written
//  every frame, so it's ring-buffered (FRAMES_IN_FLIGHT) to avoid in-flight
//  overwrite. Reuses the v1 pipeline/attachment/combiner wiring — only the FILL
//  changed from screen-position to depth. (v1 foveated is gone.)
//
//  Gated by `r_vrs` (0 off / 1 mild / 2 aggressive) AND VulkanHW.m_bVRSSupported
//  AND R8_UINT storage-image support.
// ============================================================================
#pragma once
#include "stdafx.h"

namespace VK { namespace VRS {

void        Init();                          // lazy: proc addr + compute pipeline + storage check
bool        Wanted();                        // VulkanHW.m_bVRSSupported && r_vrs>0 (cheap build gate)
// Build THIS frame's SRI from the scene depth (call with depth in SHADER_READ).
// A = ProjTerms.p43, B = ProjTerms.p33 (viewZ = A/(zndc-B)). Picks the ring slot
// from frameIndex; GetView() then returns that slot.
void        BuildFromDepth(VkCommandBuffer cmd, u32 frameIndex, VkImageView depthView,
                           VkExtent2D screen, float A, float B);
VkImageView GetView();                        // current frame's SRI view (null if not built)
VkExtent2D  TexelSize();                      // SRI tile size (device-queried)
// Per-draw coarse pipeline rate (combiners KEEP/KEEP = use this rate, ignore SRI/primitive).
// For pipelines built with a dynamic FRAGMENT_SHADING_RATE state. No-op unless pipeline-rate VRS.
void        CmdSetPipelineRate(VkCommandBuffer cmd, u32 w, u32 h);
// Diag: FS-invocation pipeline-stats query around the world color pass. Begin
// outside the rendering scope (resets + begins the slot's query, harvests the
// N-frames-old result into the log), End after vkCmdEndRendering.
void        StatsBegin(VkCommandBuffer cmd, u32 frameIndex);
void        StatsEnd(VkCommandBuffer cmd, u32 frameIndex);
// Diag (r_fsinv_split): 4-way FS-invocation attribution INSIDE the world color
// pass — 0=CPU statics flush, 1=GPU statics, 2=dynamics, 3=skinned. Sequential
// (never nested — Vulkan allows one active pipeline-stats query at a time), so
// it REPLACES the StatsBegin/End bracket while active. SubStatsReset must run
// OUTSIDE the rendering scope (vkCmdResetQueryPool restriction) — it also
// harvests+logs the N-frames-old results. Begin/End wrap each region inside.
// Regions: 0=CPU statics flush, 1=GPU terrain, 2=GPU meshes, 3=dynamics,
// 4=skinned (marks the frame's results ready). SubOcc* wraps the GPU statics
// draw with a precise OCCLUSION query (samples passed) — may run concurrently
// with the pipeline-stats queries (different query type).
void        SubStatsReset(VkCommandBuffer cmd, u32 frameIndex);
void        SubStatsBegin(VkCommandBuffer cmd, u32 frameIndex, u32 idx);
void        SubStatsEnd(VkCommandBuffer cmd, u32 frameIndex, u32 idx);
void        SubOccBegin(VkCommandBuffer cmd, u32 frameIndex);
void        SubOccEnd(VkCommandBuffer cmd, u32 frameIndex);
// ---- r_world_geom_stats: is a world pass geometry-bound or fragment-bound? ---
// The resolution sweep (26-08) showed 57% of the frame does not move when the
// render extent quarters, and that the two biggest survivors are World/Depth
// (85% of its cost survives) and World/Statics (68%). Zone time alone cannot say
// whether that is triangle setup, vertex fetch, tessellation or per-draw overhead
// — so ask the hardware. One pipeline-statistics query per zone, seven counters:
// IA vertices/primitives, VS invocations, clipped primitives, FS invocations and
// the two TESSELLATION counters (terrain draws through a tessellated pipeline in
// BOTH the prepass and the colour pass — a fixed cost by construction if it is
// large). Slots: 0 = World/Depth, 1 = World/Statics. Cost 0 while r_world_geom_stats
// is 0. Reset runs OUTSIDE any rendering scope and harvests the N-frames-old slot.
void        GeomStatsReset(VkCommandBuffer cmd, u32 frameIndex);
void        GeomStatsBegin(VkCommandBuffer cmd, u32 frameIndex, u32 idx);
void        GeomStatsEnd(VkCommandBuffer cmd, u32 frameIndex, u32 idx);

void        Destroy();

}} // namespace VK::VRS

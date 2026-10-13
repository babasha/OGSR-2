// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  GI PIXEL DEBUGGER (vk_gi_probe, r_gi_probe) — the GI answer under the
//  crosshair, decomposed into NUMBERS and printed to the log.
// ============================================================================
//  Every other DDGI instrument is visual (probe dots, debug views) and needs a
//  person at the screen; every question asked from a log had to be answered by
//  describing pixels in words. This pass dumps, for the surface at screen
//  centre: world position, the serving cascade, all eight probes with their
//  trilinear/wrap/Chebyshev weights and irradiance, the authoritative field
//  lookup before and after the confidence gate, the resolved gather, the final
//  HDR scene colour, plus the CPU-side context (applied exposure, indoor
//  boost, the relevant cvars). Aim, `r_gi_probe 1`, paste the [VK GIprobe]
//  block — the "why is this wall bright" question becomes ten numbers.
//
//  Cost: one 1-thread dispatch + a ~500 B readback per frame while the cvar is
//  on; nothing at all while it is 0.
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace GIProbe {

// Record the dispatch + read back / print the slot that has retired. Call once
// per frame with the scene depth ALREADY in SHADER_READ_ONLY, the gather
// resolved and the scene colour complete — i.e. from the tonemap, right after
// its depth barrier. Lazy-inits itself; a failure disables it for the session.
void Execute(VkCommandBuffer cmd, u32 imageIndex, const Fmatrix& viewProj);

void Destroy();

}}  // namespace VK::GIProbe

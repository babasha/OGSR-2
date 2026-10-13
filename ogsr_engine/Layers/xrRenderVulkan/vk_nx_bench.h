// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — synthetic device benchmark, run once at startup with `-nx_bench` (Switch).
//
// Question it answers: is the console slow because of the GAME (passes, shaders, content) or because of the device and
// driver underneath (memory type, tiling, compression, queue behaviour)? It times, in its own command buffer and with
// wall-clock around a submit+wait, the simplest GPU operations: buffer fill/copy (memory bandwidth), image clears and
// fullscreen draws into RGBA8 / RGBA16F targets (ROP rate), a textured fullscreen draw (texture rate) and a discard-all
// textured draw (alpha-test rate). Each result is logged as ms per operation and GB/s or Gpx/s, to be compared with the
// theoretical numbers of the GM20B printed on the last line.
#pragma once

namespace VK { namespace NxBench { void Run(); } }

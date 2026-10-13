// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Async compute foundation (vk_async).
//
// The renderer records the WHOLE frame into one graphics command buffer and
// submits it once to the graphics queue. To overlap compute with graphics we need
// a SECOND command buffer recorded + submitted to the dedicated compute queue, with
// a timeline semaphore ordering the cross-queue handoff. This module owns that
// machinery: a compute-family command pool, one command buffer per frame-in-flight,
// and a monotonic timeline the graphics submit waits on.
//
// INCREMENT 1 (this file's current scope) = the machinery + an INERT probe: each
// frame it records an EMPTY compute command buffer, submits it to the compute queue
// signaling the timeline, and the graphics submit waits on it. Purpose: prove the
// cross-queue submit + timeline are STABLE on the target GPU (no hang / device-lost)
// before any real pass moves onto the compute queue. r_async 0 = the machinery is
// dormant (exact old single-queue path).
//
// INCREMENT 2 (next, once the probe is verified stable): record a real pass into
// GetCmd() instead of leaving it empty — starting with one whose barriers are
// rewritten for the compute queue (the existing culls/SSAO carry graphics-stage
// barriers that are invalid on a compute queue) and whose shared buffers are made
// CONCURRENT (graphics+compute). Then split the graphics frame into segments so the
// compute actually OVERLAPS graphics raster (e.g. SSAO behind the VSM atlas render).

#pragma once
#include "vk_core.h"

namespace VK { namespace Async {

// True when async compute is enabled (r_async) AND the device exposes a compute
// queue family distinct from graphics (else there is nothing to overlap). Lazily
// initializes the pool/timeline on first true.
bool Available();

// Start of frame (CRender::Begin): remembers the frame-in-flight slot and clears
// last frame's pending state. Cheap and unconditional — call it even when no async
// work ends up being recorded, so a stale "wait me" never leaks into this frame.
void FrameBegin(u32 slot);

// Open this frame's compute command buffer for recording, or VK_NULL_HANDLE when
// async is unavailable (r_async off / no dedicated compute family) — callers then
// record onto the graphics buffer as before. The slot's buffer is safe to reset:
// the graphics frame that used it last has retired behind its fence.
VkCommandBuffer Begin();

// Close + submit what Begin() opened, signaling the timeline the graphics submit
// waits on. Waits on the graphics timeline value of the PREVIOUS frame first, so
// compute never overwrites a volume the last frame's passes are still reading —
// that ordering costs nothing against the CURRENT frame, which is what we overlap.
void Submit();

// For the graphics submit (CommandManager::Submit): if async work was submitted
// this frame, yields the timeline semaphore + value the graphics queue must wait
// on before consuming any compute output. Mirrors the upload-timeline wait.
bool GetGraphicsWait(VkSemaphore& sem, u64& value);

// Device teardown.
void Destroy();

}} // namespace VK::Async

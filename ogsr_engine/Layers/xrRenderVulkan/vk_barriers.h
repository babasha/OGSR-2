// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "vk_core.h"

namespace VK
{

// Image barrier with auto-derived stage/access from layouts.
// Uses VkImageMemoryBarrier2 + vkCmdPipelineBarrier2 (Vulkan 1.3).
void ImageBarrier(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    u32 layerCount = 1, u32 mipLevels = 1);

// Image barrier with EXPLICIT stage/access. Needed wherever the layout does not
// determine the synchronization: GENERAL->GENERAL between compute dispatches, a
// storage-image write consumed by the vertex stage, a compute result sampled by
// fragment — the derivation above answers "compute storage read|write" for every
// GENERAL, which is right for none of those. Passes that hand-rolled their own
// barrier helper did so for exactly this reason; this is that helper, once.
// Same argument order as BufferBarrier/MemoryBarrier: src pair, then dst pair.
void ImageBarrier(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    u32 mipLevels = 1, u32 layerCount = 1);

// Compute->compute "everything written is visible" fence between dispatches that
// share storage images/buffers and stay in GENERAL. The single most repeated
// barrier in the renderer.
void ComputeBarrier(VkCommandBuffer cmd);

// Batch image barriers - same transition for multiple images.
void ImageBarriers(VkCommandBuffer cmd, u32 count, const VkImage* images,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, u32 layerCount = 1);

// Buffer barrier - explicit stage/access (no auto-derivation).
void BufferBarrier(VkCommandBuffer cmd, VkBuffer buffer,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
    VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

// Memory barrier - global sync point.
void MemoryBarrier(VkCommandBuffer cmd,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

// Inter-pass ordering for the shared color+depth swapchain attachments WITHOUT a
// layout change. With the single-layout convention (image stays COLOR_ATTACHMENT
// the whole frame) two consecutive dynamic-rendering passes that LOAD the same
// color/depth target have no implicit dependency, so this orders pass N's
// attachment writes before pass N+1's attachment access. Replaces the old
// COLOR<->TRANSFER_DST round-trip each pass used to do (the layout thrash).
// A real framegraph will later swap this conservative global barrier for precise
// per-resource ones.
void SceneAttachmentBarrier(VkCommandBuffer cmd);

// ---------------------------------------------------------------------------
// ImageState — per-image layout/stage/access tracker (the framegraph execute seam).
//
// The first brick of the render graph. Instead of every consumer hand-placing a
// flip-to-READ / flip-back-to-ATTACHMENT round-trip, it calls Require(the state
// it needs) and a barrier is emitted ONLY when the tracked state actually has to
// change. A RUN of consumers that all need the SAME state (SSAO, VRS, VSM-mark,
// VSM-resolve all sampling the prepass depth) coalesces to a single transition —
// killing the depth-thrash (4 round-trips -> 1). A later write correctly waits on
// the union of every intervening reader (stage/access are OR-accumulated on the
// read-after-read no-op). Generalizes to every scene resource and, once it carries
// a queue family, to cross-queue ownership for async compute.
// ---------------------------------------------------------------------------
struct ImageState
{
    VkImage               image  = VK_NULL_HANDLE;
    VkImageAspectFlags    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2        access = 0;
    // Queue family that currently OWNS the contents. VK_QUEUE_FAMILY_IGNORED =
    // "never left the graphics queue", which is every resource today. It is a
    // recorded property, NOT an automatic transfer: moving a resource between
    // families needs a release barrier on the owning queue AND an acquire barrier
    // on the destination queue, ordered by a semaphore between the two submits.
    // Require() asserts rather than silently emitting a same-family barrier for a
    // resource owned elsewhere — that is the bug class async compute introduces,
    // and it must fail loudly instead of corrupting contents.
    u32                   queueFamily = VK_QUEUE_FAMILY_IGNORED;

    // Record the state the image is already known to be in — no barrier emitted.
    void Seed(VkImage img, VkImageAspectFlags asp, VkImageLayout lay,
              VkPipelineStageFlags2 stg, VkAccessFlags2 acc,
              u32 family = VK_QUEUE_FAMILY_IGNORED)
    { image = img; aspect = asp; layout = lay; stage = stg; access = acc; queueFamily = family; }

    // Make the image usable as (lay, stg, acc). Emits vkCmdPipelineBarrier2 only
    // when the layout differs or a write hazard exists; a read-after-read at the
    // same layout no-ops (the coalescing win) but is remembered for the next write.
    // Returns TRUE when a barrier was actually emitted — so a caller deriving
    // barriers can report what it really cost instead of re-deriving the hazard
    // rule to guess (see the inter-pass derivation in vk_pass_registry.cpp).
    // Every other caller ignores it, as before.
    bool Require(VkCommandBuffer cmd, VkImageLayout lay,
                 VkPipelineStageFlags2 stg, VkAccessFlags2 acc,
                 u32 mipLevels = 1, u32 layerCount = 1);

    // Require() split in two, so SEVERAL resources can share ONE
    // vkCmdPipelineBarrier2 instead of paying a call each. Plan() answers the same
    // hazard question and fills `out` with the barrier that Require would have
    // submitted; the caller batches the barriers it collected and submits them
    // together. Returns false when no barrier is needed.
    // ⚠ Plan() ADVANCES THE TRACKED STATE as if the barrier had been submitted —
    // it has to, or a second Plan() on the same resource in the same batch would
    // re-derive from a state the first one already superseded. So a caller that
    // gets true MUST submit what it was handed. Use Require() unless you are
    // actually batching.
    bool Plan(VkImageMemoryBarrier2& out, VkImageLayout lay,
              VkPipelineStageFlags2 stg, VkAccessFlags2 acc,
              u32 mipLevels = 1, u32 layerCount = 1);

    bool Valid() const { return image != VK_NULL_HANDLE; }
};

// ---------------------------------------------------------------------------
// BufferState — the ImageState discipline for buffers.
//
// Images were tracked first because their LAYOUT makes a missing transition
// visible (garbage on screen, a validation error). Buffers have no layout, so a
// missing barrier is invisible until it is a race: the compute cull writes an
// indirect/count buffer, the draw reads it, and on a busy GPU the draw wins.
// That class of bug does not reproduce on demand, which is exactly why it wants
// a tracker rather than 50 hand-placed calls.
//
// Same rule as ImageState: read-after-read accumulates, anything else emits.
// This is what the VSM bin / world-cull / tree-cull SSBO hand-offs actually
// need, and the async-compute split needs it BEFORE the images (the indirect
// buffers are what crosses the queue boundary first).
// ---------------------------------------------------------------------------
struct BufferState
{
    VkBuffer              buffer = VK_NULL_HANDLE;
    VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2        access = 0;
    u32                   queueFamily = VK_QUEUE_FAMILY_IGNORED;   // see ImageState

    void Seed(VkBuffer buf, VkPipelineStageFlags2 stg, VkAccessFlags2 acc,
              u32 family = VK_QUEUE_FAMILY_IGNORED)
    { buffer = buf; stage = stg; access = acc; queueFamily = family; }

    // Make the buffer usable as (stg, acc). No-ops on read-after-read (the union
    // of readers is remembered so the next write waits on all of them).
    void Require(VkCommandBuffer cmd, VkPipelineStageFlags2 stg, VkAccessFlags2 acc,
                 VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

    bool Valid() const { return buffer != VK_NULL_HANDLE; }
};

} // namespace VK

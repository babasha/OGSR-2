// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_barriers.h"

namespace VK
{

// ---------------------------------------------------------------------------
// Layout -> stage/access derivation (matches CRT::TransitionLayout logic)
// ---------------------------------------------------------------------------
static void DeriveStageAccess(VkImageLayout layout, bool isSrc,
    VkPipelineStageFlags2& stage, VkAccessFlags2& access)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        stage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        access = 0;
        break;

    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        break;

    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        stage  = isSrc ? VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                       : VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
               | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        break;

    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        break;

    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;

    case VK_IMAGE_LAYOUT_GENERAL:
        stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        stage  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        access = 0;
        break;

    default:
        stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        break;
    }
}

// ---------------------------------------------------------------------------
// ImageBarrier
// ---------------------------------------------------------------------------
void ImageBarrier(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkImageAspectFlags aspect, u32 layerCount, u32 mipLevels)
{
    VkPipelineStageFlags2 srcStage, dstStage;
    VkAccessFlags2 srcAccess, dstAccess;
    DeriveStageAccess(oldLayout, true,  srcStage, srcAccess);
    DeriveStageAccess(newLayout, false, dstStage, dstAccess);

    VkImageMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask  = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask     = aspect;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = layerCount;

    VkDependencyInfo dep = {};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);
}

// ---------------------------------------------------------------------------
// ImageBarrier — explicit stage/access (no derivation)
// ---------------------------------------------------------------------------
void ImageBarrier(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
    VkImageAspectFlags aspect, u32 mipLevels, u32 layerCount)
{
    VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask  = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask  = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspect, 0, mipLevels, 0, layerCount };

    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);
}

// ---------------------------------------------------------------------------
// ComputeBarrier — compute->compute visibility, no layout change
// ---------------------------------------------------------------------------
void ComputeBarrier(VkCommandBuffer cmd)
{
    MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

// ---------------------------------------------------------------------------
// ImageBarriers (batch)
// ---------------------------------------------------------------------------
void ImageBarriers(VkCommandBuffer cmd, u32 count, const VkImage* images,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkImageAspectFlags aspect, u32 layerCount)
{
    if (count == 0) return;

    VkPipelineStageFlags2 srcStage, dstStage;
    VkAccessFlags2 srcAccess, dstAccess;
    DeriveStageAccess(oldLayout, true,  srcStage, srcAccess);
    DeriveStageAccess(newLayout, false, dstStage, dstAccess);

    constexpr u32 MAX_BATCH = 8;
    VkImageMemoryBarrier2 barriers[MAX_BATCH] = {};
    u32 n = (count < MAX_BATCH) ? count : MAX_BATCH;

    for (u32 i = 0; i < n; i++)
    {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[i].srcStageMask  = srcStage;
        barriers[i].srcAccessMask = srcAccess;
        barriers[i].dstStageMask  = dstStage;
        barriers[i].dstAccessMask = dstAccess;
        barriers[i].oldLayout = oldLayout;
        barriers[i].newLayout = newLayout;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = images[i];
        barriers[i].subresourceRange.aspectMask     = aspect;
        barriers[i].subresourceRange.baseMipLevel   = 0;
        barriers[i].subresourceRange.levelCount     = 1;
        barriers[i].subresourceRange.baseArrayLayer = 0;
        barriers[i].subresourceRange.layerCount     = layerCount;
    }

    VkDependencyInfo dep = {};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = n;
    dep.pImageMemoryBarriers = barriers;

    vkCmdPipelineBarrier2(cmd, &dep);
}

// ---------------------------------------------------------------------------
// BufferBarrier
// ---------------------------------------------------------------------------
void BufferBarrier(VkCommandBuffer cmd, VkBuffer buffer,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
    VkDeviceSize offset, VkDeviceSize size)
{
    VkBufferMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask  = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask  = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = offset;
    barrier.size   = size;

    VkDependencyInfo dep = {};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);
}

// ---------------------------------------------------------------------------
// MemoryBarrier
// ---------------------------------------------------------------------------
void MemoryBarrier(VkCommandBuffer cmd,
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask  = dstStage;
    barrier.dstAccessMask = dstAccess;

    VkDependencyInfo dep = {};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);
}

// ---------------------------------------------------------------------------
// SceneAttachmentBarrier — inter-pass color+depth ordering, no layout change.
// ---------------------------------------------------------------------------
void SceneAttachmentBarrier(VkCommandBuffer cmd)
{
    MemoryBarrier(cmd,
        // src: prior pass finished writing color (and late depth)
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        // dst: next pass loads/tests color + depth (early depth test, blend reads color)
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
        | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
}

// ---------------------------------------------------------------------------
// ImageState::Plan / Require — emit a transition only when the state actually
// changes. Plan answers the hazard question and hands the barrier back so several
// resources can share one vkCmdPipelineBarrier2 (the derived inter-pass barrier in
// vk_pass_registry.cpp batches the two scene attachments that way); Require is
// Plan plus an immediate submit, which is what every other caller wants.
// ---------------------------------------------------------------------------
static constexpr VkAccessFlags2 kWriteAccessMask =
      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
    | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
    | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
    | VK_ACCESS_2_TRANSFER_WRITE_BIT
    | VK_ACCESS_2_MEMORY_WRITE_BIT;

bool ImageState::Plan(VkImageMemoryBarrier2& out, VkImageLayout lay,
    VkPipelineStageFlags2 stg, VkAccessFlags2 acc, u32 mipLevels, u32 layerCount)
{
    const bool isWrite  = (acc    & kWriteAccessMask) != 0;
    const bool wasWrite = (access & kWriteAccessMask) != 0;

    // Same layout AND pure read-after-read → nothing to synchronize. Remember this
    // reader's stage/access so a later write's barrier waits on the union of all
    // readers that ran since the last transition.
    if (lay == layout && !isWrite && !wasWrite)
    {
        stage  |= stg;
        access |= acc;
        return false;
    }

    out = VkImageMemoryBarrier2{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    out.srcStageMask  = stage;
    out.srcAccessMask = access;
    out.dstStageMask  = stg;
    out.dstAccessMask = acc;
    out.oldLayout = layout;
    out.newLayout = lay;
    out.srcQueueFamilyIndex = out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out.image = image;
    out.subresourceRange = { aspect, 0, mipLevels, 0, layerCount };

    // Advance now — see the warning on the declaration.
    layout = lay; stage = stg; access = acc;
    return true;
}

bool ImageState::Require(VkCommandBuffer cmd, VkImageLayout lay,
    VkPipelineStageFlags2 stg, VkAccessFlags2 acc, u32 mipLevels, u32 layerCount)
{
    VkImageMemoryBarrier2 b{};
    if (!Plan(b, lay, stg, acc, mipLevels, layerCount)) return false;

    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

// ---------------------------------------------------------------------------
// BufferState::Require — the same rule without a layout.
// ---------------------------------------------------------------------------
// A buffer has no layout, so "did the state change" reduces entirely to the
// write question: read-after-read needs nothing, everything else needs a
// barrier. HOST_WRITE is included because the ring allocator's persistently
// mapped writes are a real producer here, not a theoretical one.
static constexpr VkAccessFlags2 kBufWriteMask =
      VK_ACCESS_2_SHADER_WRITE_BIT
    | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
    | VK_ACCESS_2_TRANSFER_WRITE_BIT
    | VK_ACCESS_2_HOST_WRITE_BIT
    | VK_ACCESS_2_MEMORY_WRITE_BIT;

void BufferState::Require(VkCommandBuffer cmd, VkPipelineStageFlags2 stg, VkAccessFlags2 acc,
                          VkDeviceSize offset, VkDeviceSize size)
{
    if (buffer == VK_NULL_HANDLE) return;

    const bool isWrite  = (acc    & kBufWriteMask) != 0;
    const bool wasWrite = (access & kBufWriteMask) != 0;

    // Pure read-after-read: nothing to order. Remember this reader so a later
    // write waits on the union of everyone who read since the last barrier.
    if (!isWrite && !wasWrite)
    {
        stage  |= stg;
        access |= acc;
        return;
    }

    BufferBarrier(cmd, buffer, stage, access, stg, acc, offset, size);
    stage = stg; access = acc;
}

} // namespace VK

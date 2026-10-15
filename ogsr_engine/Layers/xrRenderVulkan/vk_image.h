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

// ============================================================================
// Image creation helpers
//
// Collapses the VkImageCreateInfo -> vmaCreateImage -> VkImageViewCreateInfo ->
// vkCreateImageView boilerplate that was copy-pasted ~30x across render targets
// (SSAO, motion-vec, VRS, shadow, detail/tree, IBL, volumetrics, ...). All use
// the global VulkanHW device/allocator; the create path always uses OPTIMAL
// tiling + UNDEFINED initial layout + the given VMA memory usage.
// ============================================================================

// Full-control image description (cube/3D/mip-chain/array cases go through this).
// Defaults describe the common case: a single-mip, single-layer, 2D device image.
struct ImageDesc
{
    VkFormat              format   = VK_FORMAT_UNDEFINED;
    VkExtent3D            extent   = { 0, 0, 1 };
    VkImageUsageFlags     usage    = 0;
    u32                   mips     = 1;
    u32                   layers   = 1;
    VkImageType           type     = VK_IMAGE_TYPE_2D;
    VkImageCreateFlags    flags    = 0;   // e.g. VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
    VkSampleCountFlagBits samples  = VK_SAMPLE_COUNT_1_BIT;
    VmaMemoryUsage        memUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    const char*           name     = nullptr;   // debug label (VK::Prof::NameImage), optional

    // Touched by BOTH the graphics and the dedicated compute queue (async compute,
    // r_async): an image written on one family and read on the other keeps its
    // contents only under CONCURRENT sharing or an explicit queue-family ownership
    // transfer. We take CONCURRENT — the transfer path would need a release/acquire
    // pair every frame in both directions. Applied ONLY when the families actually
    // differ (on a shared-compute GPU the flag is a no-op), mirroring the
    // graphics+transfer rule in vk_texture.cpp / vk_buffer.cpp.
    // ⚠️Do NOT set this on graphics render targets that rely on framebuffer
    // compression (depth, colour attachments): CONCURRENT can cost that compression
    // on some drivers. It is meant for compute-owned images the other queue samples.
    bool                  computeShared = false;
};

// Create a VkImage + VMA allocation. On failure logs "![VK] image '<name>' ..."
// and leaves outImage/outAlloc as VK_NULL_HANDLE, returning false.
bool CreateImage(const ImageDesc& desc, VkImage& outImage, VmaAllocation& outAlloc);

// Convenience for the overwhelmingly common case: a 2D, single-mip, single-layer,
// device-local image. Equivalent to CreateImage with the matching ImageDesc.
bool CreateImage2D(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                   VkImage& outImage, VmaAllocation& outAlloc, const char* name = nullptr);

// Create a VkImageView over an existing image. Returns VK_NULL_HANDLE on failure
// (logs "![VK] image view create failed"). Aspect defaults to COLOR; pass
// VK_IMAGE_ASPECT_DEPTH_BIT for depth targets.
VkImageView CreateImageView(VkImage image, VkFormat format,
                            VkImageViewType viewType  = VK_IMAGE_VIEW_TYPE_2D,
                            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                            u32 baseMip   = 0, u32 mipCount   = 1,
                            u32 baseLayer = 0, u32 layerCount = 1);

} // namespace VK

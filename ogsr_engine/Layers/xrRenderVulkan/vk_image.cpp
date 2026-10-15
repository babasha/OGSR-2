// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_image.h"
#include "vk_profiler.h"   // VK::Prof::NameImage
#include "HW_Vulkan.h"     // VulkanHW (device + allocator)

namespace VK
{

bool CreateImage(const ImageDesc& desc, VkImage& outImage, VmaAllocation& outAlloc)
{
    outImage = VK_NULL_HANDLE;
    outAlloc = VK_NULL_HANDLE;

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.flags         = desc.flags;
    ici.imageType     = desc.type;
    ici.format        = desc.format;
    ici.extent        = desc.extent;
    ici.mipLevels     = desc.mips;
    ici.arrayLayers   = desc.layers;
    ici.samples       = desc.samples;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = desc.usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Cross-queue (graphics + dedicated compute) image — see ImageDesc::computeShared.
    // Kept EXCLUSIVE when the compute family aliases graphics: there is no second
    // family to share with, and CONCURRENT with one index is not legal.
    const u32 csFamilies[2] = { VulkanHW.m_GraphicsFamily, VulkanHW.m_ComputeFamily };
    if (desc.computeShared && VulkanHW.m_ComputeFamily != VulkanHW.m_GraphicsFamily)
    {
        ici.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = 2;
        ici.pQueueFamilyIndices   = csFamilies;
    }

    VmaAllocationCreateInfo aci{};
    aci.usage = desc.memUsage;
    // VK_EXT_memory_priority: images built through this shared helper are render
    // targets / attachments / compute images / VSM atlases — all read+written every
    // frame. Tag them HIGH so under VRAM pressure the driver spills streamable world
    // textures (priority 0.25, see vk_texture.cpp) to system RAM FIRST and never
    // migrates a hot attachment across PCIe. Ignored when the extension is absent.
    aci.priority = 1.0f;

    // VRAM attribution: images built through this helper with no subsystem scope
    // active are render targets — bucket them as "RT" instead of "untagged".
    VK::Vram::Scope _vram_scope(VK::Vram::CurrentTag() ? VK::Vram::CurrentTag() : "RT");

    const VkResult res = VK::Vram::CreateImage(VulkanHW.m_Allocator, &ici, &aci, &outImage, &outAlloc, nullptr);
    if (res != VK_SUCCESS)
    {
        Msg("![VK] image '%s' create failed (vk=%d)", desc.name ? desc.name : "?", (int)res);
        outImage = VK_NULL_HANDLE;
        outAlloc = VK_NULL_HANDLE;
        return false;
    }

    if (desc.name)
        VK::Prof::NameImage(outImage, desc.name);

    return true;
}

bool CreateImage2D(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                   VkImage& outImage, VmaAllocation& outAlloc, const char* name)
{
    ImageDesc d;
    d.format = format;
    d.extent = { extent.width, extent.height, 1 };
    d.usage  = usage;
    d.name   = name;
    return CreateImage(d, outImage, outAlloc);
}

VkImageView CreateImageView(VkImage image, VkFormat format,
                            VkImageViewType viewType, VkImageAspectFlags aspect,
                            u32 baseMip, u32 mipCount, u32 baseLayer, u32 layerCount)
{
    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = image;
    vci.viewType = viewType;
    vci.format   = format;
    vci.subresourceRange.aspectMask     = aspect;
    vci.subresourceRange.baseMipLevel   = baseMip;
    vci.subresourceRange.levelCount     = mipCount;
    vci.subresourceRange.baseArrayLayer = baseLayer;
    vci.subresourceRange.layerCount     = layerCount;

    VkImageView view = VK_NULL_HANDLE;
    const VkResult res = vkCreateImageView(VulkanHW.m_Device, &vci, nullptr, &view);
    if (res != VK_SUCCESS)
    {
        Msg("![VK] image view create failed (vk=%d)", (int)res);
        return VK_NULL_HANDLE;
    }
    return view;
}

} // namespace VK

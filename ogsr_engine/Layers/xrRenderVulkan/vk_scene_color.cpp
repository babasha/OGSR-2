// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — HDR scene colour target. See vk_scene_color.h.
#include "stdafx.h"
#include "vk_scene_color.h"
#include "vk_image.h"      // VK::CreateImage / CreateImageView
#include "vk_profiler.h"   // VK::Prof::NameImage (debug-utils names)

extern int ps_r_scene_color_32;   // vk_console_min.cpp — r_scene_color_32

namespace VK { namespace SceneColor {

namespace {
    constexpr u32 kMaxImages = 8;   // swapchain rarely exceeds 3-4; cap for the arrays

    // ---------------------------------------------------------------------
    // THE FORMAT IS RESOLVED ONCE PER PROCESS, AND IT HAS TO BE.
    // ---------------------------------------------------------------------
    // Every scene pipeline bakes this into VkPipelineRenderingCreateInfo at
    // BUILD time, and those pipelines live in ~25 module-static caches with no
    // global rebuild hook. So r_scene_color_32 is read exactly once, on the
    // first Format() call, and a change needs a RESTART — flipping it mid-run
    // would leave every already-built pipeline disagreeing with the attachment
    // it renders into, which is undefined behaviour, not a visual glitch.
    // First Format() lands safely after user.ltx: x_ray.cpp executes it before
    // the render device exists (the ordering HW_Vulkan.cpp Step 0 relies on).
    VkFormat      s_format = VK_FORMAT_UNDEFINED;   // resolved by ResolveFormat()
    VkImage       s_image[kMaxImages] = {};
    VmaAllocation s_alloc[kMaxImages] = {};
    VkImageView   s_view[kMaxImages]  = {};       // mip-0 (render attachment)
    VkImageView   s_sampleView[kMaxImages] = {};  // full chain (tonemap sampling)
    u32           s_count  = 0;
    u32           s_mips   = 1;
    VkExtent2D    s_extent = {};
    u32           s_generation = 0;

    u32 CalcMips(u32 w, u32 h)
    {
        u32 m = 1, d = (w > h ? w : h);
        while (d > 1) { d >>= 1; ++m; }
        return m;
    }

    // Everything this target is actually used for, as format features. Miss any
    // one of them and the packed format is not a drop-in: COLOR_ATTACHMENT +
    // BLEND for the scene passes (water/particles/decals blend into it),
    // SAMPLED + FILTER_LINEAR for the tonemap and for the LINEAR blit that
    // builds the avg-luminance mip chain, BLIT_SRC/DST for GenerateMips and for
    // the water-refraction + SSIL-history snapshots taken off this image.
    bool Supported(VkFormat f)
    {
        if (VulkanHW.m_PhysicalDevice == VK_NULL_HANDLE) return false;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(VulkanHW.m_PhysicalDevice, f, &fp);
        const VkFormatFeatureFlags need =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT;
        return (fp.optimalTilingFeatures & need) == need;
    }

    void ResolveFormat()
    {
        if (s_format != VK_FORMAT_UNDEFINED) return;
        // No device yet — the caller gets the safe format WITHOUT latching, so
        // the real decision still happens once the GPU can be asked.
        if (VulkanHW.m_PhysicalDevice == VK_NULL_HANDLE) return;

        if (ps_r_scene_color_32 == 0) {
            s_format = VK_FORMAT_R16G16B16A16_SFLOAT;
            Msg("[VK SceneColor] format R16G16B16A16_SFLOAT (64 bpp) - r_scene_color_32 0");
            return;
        }
        if (Supported(VK_FORMAT_B10G11R11_UFLOAT_PACK32)) {
            s_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            Msg("[VK SceneColor] format B10G11R11_UFLOAT_PACK32 (32 bpp) - half the colour bandwidth of every scene pass");
            return;
        }
        s_format = VK_FORMAT_R16G16B16A16_SFLOAT;
        Msg("![VK SceneColor] B10G11R11_UFLOAT_PACK32 is missing a required format feature on this GPU - falling back to R16G16B16A16_SFLOAT");
    }

    u32         BytesPerTexel(VkFormat f) { return (f == VK_FORMAT_R16G16B16A16_SFLOAT) ? 8u : 4u; }
    const char* FormatName(VkFormat f)    { return (f == VK_FORMAT_R16G16B16A16_SFLOAT) ? "R16G16B16A16_SFLOAT" : "B10G11R11_UFLOAT_PACK32"; }

    void DestroyAll()
    {
        if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
        for (u32 i = 0; i < s_count; ++i) {
            if (s_sampleView[i]) { vkDestroyImageView(VulkanHW.m_Device, s_sampleView[i], nullptr); s_sampleView[i] = VK_NULL_HANDLE; }
            if (s_view[i])  { vkDestroyImageView(VulkanHW.m_Device, s_view[i], nullptr); s_view[i] = VK_NULL_HANDLE; }
            if (s_image[i]) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_image[i], s_alloc[i]); s_image[i] = VK_NULL_HANDLE; s_alloc[i] = VK_NULL_HANDLE; }
        }
        s_count = 0;
    }
}

VkFormat    Format()            { ResolveFormat(); return (s_format != VK_FORMAT_UNDEFINED) ? s_format : VK_FORMAT_R16G16B16A16_SFLOAT; }
VkImage     GetImage(u32 i)     { return (i < s_count) ? s_image[i] : VK_NULL_HANDLE; }
VkImageView GetView(u32 i)      { return (i < s_count) ? s_view[i]  : VK_NULL_HANDLE; }
VkImageView GetSampleView(u32 i){ return (i < s_count) ? s_sampleView[i] : VK_NULL_HANDLE; }
u32         MipLevels()         { return s_mips; }
VkExtent2D  Extent()            { return s_extent; }
u32         Count()             { return s_count; }
u32         Generation()        { return s_generation; }

void EnsureSize(VkExtent2D extent, u32 count)
{
    if (count > kMaxImages) count = kMaxImages;
    if (count == s_count && extent.width == s_extent.width && extent.height == s_extent.height && s_count > 0)
        return;   // already current

    // Resize happens at swapchain recreation, which idles the device; safe to
    // free + recreate here.
    DestroyAll();
    const VkFormat fmt = Format();   // resolves r_scene_color_32 on the first call
    s_extent = extent;
    s_mips   = CalcMips(extent.width, extent.height);   // full chain → top mip = whole-frame average

    for (u32 i = 0; i < count; ++i) {
        VK::ImageDesc d;
        d.format = fmt;
        d.extent = { extent.width, extent.height, 1 };
        d.mips   = s_mips;
        d.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT |   // per-frame clear + blit dst (mips)
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;    // blit src (mip-gen for avg luminance)
        d.name   = "SceneColor.HDR";
        if (!VK::CreateImage(d, s_image[i], s_alloc[i])) { s_count = i; return; }

        // mip-0 view (render attachment).
        s_view[i] = VK::CreateImageView(s_image[i], fmt, VK_IMAGE_VIEW_TYPE_2D,
                                        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        if (s_view[i] == VK_NULL_HANDLE) { s_count = i; return; }
        // full-chain view (tonemap sampling: mip0 = scene, top mip = avg luminance).
        s_sampleView[i] = VK::CreateImageView(s_image[i], fmt, VK_IMAGE_VIEW_TYPE_2D,
                                              VK_IMAGE_ASPECT_COLOR_BIT, 0, s_mips);
        if (s_sampleView[i] == VK_NULL_HANDLE) { s_count = i; return; }
    }
    s_count = count;
    ++s_generation;
    // Bytes named, not just the format: this target is written by EVERY scene
    // pass, so mip0 x count is the floor of the colour traffic the frame pays.
    const u32 bpt = BytesPerTexel(fmt);
    Msg("[VK SceneColor] HDR target ready (%ux%u %s x%u, %u mips, mip0 %.1f MB each, gen %u)",
        extent.width, extent.height, FormatName(fmt), count, s_mips,
        double(extent.width) * extent.height * bpt / (1024.0 * 1024.0), s_generation);
    // Handles printed to settle who the frame-start clear actually hits: validation
    // names DLSS.Output for a clear that this module's images are the argument of.
    for (u32 i = 0; i < s_count; ++i)
        Msg("[VK SceneColor]   img[%u]=%p", i, (void*)s_image[i]);
}

void GenerateMips(VkCommandBuffer cmd, u32 index)
{
    if (index >= s_count || s_mips <= 1) return;
    VkImage img = s_image[index];

    auto barrier = [&](u32 baseMip, u32 mipCount, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags2 srcA, VkAccessFlags2 dstA,
                       VkPipelineStageFlags2 srcS, VkPipelineStageFlags2 dstS) {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = srcS; b.srcAccessMask = srcA;
        b.dstStageMask = dstS; b.dstAccessMask = dstA;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1 };
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    };

    // mip0: COLOR_ATTACHMENT (scene just rendered) → TRANSFER_SRC.
    barrier(0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT);

    s32 mw = (s32)s_extent.width, mh = (s32)s_extent.height;
    for (u32 i = 1; i < s_mips; ++i) {
        const s32 nw = mw > 1 ? mw / 2 : 1;
        const s32 nh = mh > 1 ? mh / 2 : 1;
        barrier(i, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 };
        blit.srcOffsets[1]  = { mw, mh, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
        blit.dstOffsets[1]  = { nw, nh, 1 };
        vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier(i, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT);
        mw = nw; mh = nh;
    }

    // All mips TRANSFER_SRC → SHADER_READ for the tonemap.
    barrier(0, s_mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
}

void Destroy()
{
    DestroyAll();
    s_extent = {};
}

}}  // namespace VK::SceneColor

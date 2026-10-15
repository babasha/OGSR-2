// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — synthetic device benchmark (-nx_bench). See vk_nx_bench.h.
#include "stdafx.h"
#include "vk_descriptors.h"     // MakeDescriptorSets / DescriptorWriter
#include "vk_rendering.h"       // VK::RenderingBuilder
#include "vk_nx_bench.h"
#include "vk_image.h"           // CreateImage2D / CreateImageView
#include "vk_buffer.h"          // CVulkanBuffer
#include "vk_vram_stats.h"      // VK::Vram::DestroyImage
#include "vk_shaders.h"         // g_ShaderManager
#include "vk_fullscreen.h"      // VK::Fullscreen::CreatePipeline
#include "HW_Vulkan.h"
#include <chrono>
#include <cstring>

namespace VK {
namespace NxBench {
namespace {

using Clock = std::chrono::steady_clock;

VkCommandPool   s_pool   = VK_NULL_HANDLE;
VkCommandBuffer s_cmd    = VK_NULL_HANDLE;
VkFence         s_fence  = VK_NULL_HANDLE;

bool BeginCmd()
{
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(s_cmd, &bi) == VK_SUCCESS;
}

// Submit the recorded buffer, wait for it, return wall-clock milliseconds.
double EndAndRun()
{
    vkEndCommandBuffer(s_cmd);
    vkResetFences(VulkanHW.m_Device, 1, &s_fence);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd;
    const auto t0 = Clock::now();
    vkQueueSubmit(VulkanHW.m_GraphicsQueue, 1, &si, s_fence);
    vkWaitForFences(VulkanHW.m_Device, 1, &s_fence, VK_TRUE, UINT64_MAX);
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void ToLayout(VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask  = dstStage;                              b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(s_cmd, &di);
}

void MemBarrier()
{
    VkMemoryBarrier2 m{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    m.srcStageMask = m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1; di.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(s_cmd, &di);
}

struct Target { VkImage img = VK_NULL_HANDLE; VmaAllocation alloc = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkExtent2D ext{}; VkFormat fmt{}; u32 bpp = 4; };

bool MakeTarget(Target& t, VkFormat fmt, u32 w, u32 h, u32 bpp)
{
    t.fmt = fmt; t.ext = { w, h }; t.bpp = bpp;
    if (!VK::CreateImage2D(fmt, t.ext, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, t.img, t.alloc, "NxBench.rt"))
        return false;
    t.view = VK::CreateImageView(t.img, fmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    return t.view != VK_NULL_HANDLE;
}
void FreeTarget(Target& t)
{
    if (t.view) vkDestroyImageView(VulkanHW.m_Device, t.view, nullptr);
    if (t.img)  VK::Vram::DestroyImage(VulkanHW.m_Allocator, t.img, t.alloc);
    t = Target{};
}

VkPipelineColorBlendAttachmentState NoBlend()
{
    VkPipelineColorBlendAttachmentState b{};
    b.blendEnable = VK_FALSE;
    b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    return b;
}

void Report(const char* name, double ms, int reps, double bytesPerRep, double pxPerRep, double baseMs)
{
    const double per = (ms - baseMs) / double(reps);
    if (per <= 0.0) { Msg("[nx bench] %-34s too fast to measure (%.2f ms total)", name, ms); return; }
    char extra[96] = "";
    if (bytesPerRep > 0.0) snprintf(extra, sizeof(extra), " | %.1f GB/s", bytesPerRep / (per * 1e-3) / 1e9);
    else if (pxPerRep > 0.0) snprintf(extra, sizeof(extra), " | %.2f Gpx/s", pxPerRep / (per * 1e-3) / 1e9);
    Msg("[nx bench] %-34s %7.3f ms/op%s", name, per, extra);
}

} // namespace

void Run()
{
    Msg("[nx bench] ===== synthetic device benchmark (own command buffer, no engine passes) =====");
    VkDevice dev = VulkanHW.m_Device;
    vkDeviceWaitIdle(dev);

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = VulkanHW.m_GraphicsFamily;
    if (vkCreateCommandPool(dev, &pci, nullptr, &s_pool) != VK_SUCCESS) { Msg("![nx bench] no command pool"); return; }
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &ai, &s_cmd);
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(dev, &fci, nullptr, &s_fence);

    // ---- T0: an empty submit (latency floor, subtracted from everything below) ----
    double base = 1e9;
    for (int i = 0; i < 5; ++i) { BeginCmd(); const double t = EndAndRun(); if (t < base) base = t; }
    Msg("[nx bench] empty submit + wait latency: %.3f ms (subtracted from every test)", base);

    // ---- buffers: fill / copy -------------------------------------------------------
    constexpr VkDeviceSize kBuf = 64ull * 1024 * 1024;
    CVulkanBuffer a, b;
    a.Create(kBuf, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    b.Create(kBuf, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (a.GetHandle() && b.GetHandle()) {
        constexpr int R = 10;
        BeginCmd(); for (int i = 0; i < R; ++i) { vkCmdFillBuffer(s_cmd, a.GetHandle(), 0, kBuf, 0x80808080u); MemBarrier(); }
        Report("vkCmdFillBuffer 64 MB", EndAndRun(), R, double(kBuf), 0, base);
        BeginCmd(); for (int i = 0; i < R; ++i) { VkBufferCopy c{ 0, 0, kBuf }; vkCmdCopyBuffer(s_cmd, a.GetHandle(), b.GetHandle(), 1, &c); MemBarrier(); }
        Report("vkCmdCopyBuffer 64 MB (read+write)", EndAndRun(), R, double(kBuf) * 2.0, 0, base);
    } else Msg("![nx bench] buffer create failed");

    // ---- render targets --------------------------------------------------------------
    Target rt8, rt16;
    const bool ok8  = MakeTarget(rt8,  VK_FORMAT_R8G8B8A8_UNORM,       1280, 720, 4);
    const bool ok16 = MakeTarget(rt16, VK_FORMAT_R16G16B16A16_SFLOAT,   960, 540, 8);

    auto clearTest = [&](Target& t, const char* name) {
        constexpr int R = 50;
        BeginCmd();
        ToLayout(t.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue cv{}; cv.float32[0] = 0.2f; cv.float32[3] = 1.f;
        VkImageSubresourceRange rr{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (int i = 0; i < R; ++i) { vkCmdClearColorImage(s_cmd, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rr); MemBarrier(); }
        Report(name, EndAndRun(), R, double(t.ext.width) * t.ext.height * t.bpp, 0, base);
    };
    if (ok8)  clearTest(rt8,  "vkCmdClearColorImage 1280x720 RGBA8");
    if (ok16) clearTest(rt16, "vkCmdClearColorImage 960x540 RGBA16F");

    // ---- fullscreen draws -------------------------------------------------------------
    VkShaderModule vs = g_ShaderManager ? g_ShaderManager->Load("sunshafts.vert.spv") : VK_NULL_HANDLE;
    VkShaderModule fsFlat = g_ShaderManager ? g_ShaderManager->Load("bench_flat.frag.spv") : VK_NULL_HANDLE;
    VkShaderModule fsTex  = g_ShaderManager ? g_ShaderManager->Load("bench_tex.frag.spv") : VK_NULL_HANDLE;
    VkShaderModule fsDrop = g_ShaderManager ? g_ShaderManager->Load("bench_tex_drop.frag.spv") : VK_NULL_HANDLE;
    if (!vs || !fsFlat || !fsTex || !fsDrop) {
        Msg("![nx bench] bench_*.frag.spv missing -- draw tests skipped");
    } else if (ok8 && ok16) {
        // empty layout for the flat shader, one-sampler layout for the textured ones
        VkPipelineLayoutCreateInfo plE{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkPipelineLayout layE = VK_NULL_HANDLE; vkCreatePipelineLayout(dev, &plE, nullptr, &layE);

        VkDescriptorSetLayout sl = VK_NULL_HANDLE; VkDescriptorPool dp = VK_NULL_HANDLE; VkDescriptorSet set[1] = {};
        VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, 1, sl, dp, set, VK_SHADER_STAGE_FRAGMENT_BIT, "NxBench");
        VkPipelineLayoutCreateInfo plT{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plT.setLayoutCount = 1; plT.pSetLayouts = &sl;
        VkPipelineLayout layT = VK_NULL_HANDLE; vkCreatePipelineLayout(dev, &plT, nullptr, &layT);

        // a 1024x1024 RGBA8 texture, constant 0xCC(alpha 0.8) so the "drop" shader discards everything
        VkImage tex = VK_NULL_HANDLE; VmaAllocation texA = VK_NULL_HANDLE;
        VK::CreateImage2D(VK_FORMAT_R8G8B8A8_UNORM, { 1024, 1024 }, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, tex, texA, "NxBench.tex");
        VkImageView texV = tex ? VK::CreateImageView(tex, VK_FORMAT_R8G8B8A8_UNORM) : VK_NULL_HANDLE;
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSampler smp = VK_NULL_HANDLE; vkCreateSampler(dev, &sci, nullptr, &smp);

        if (tex && texV && smp && layE && layT && set[0]) {
            BeginCmd();
            ToLayout(tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            // fill the source buffer with the texel pattern and copy it in
            vkCmdFillBuffer(s_cmd, a.GetHandle(), 0, 4ull * 1024 * 1024, 0xCC806040u);
            MemBarrier();
            VkBufferImageCopy bic{}; bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; bic.imageExtent = { 1024, 1024, 1 };
            vkCmdCopyBufferToImage(s_cmd, a.GetHandle(), tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
            ToLayout(tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            EndAndRun();
            VK::DescriptorWriter(set[0]).ImageSampler(0, texV, smp).Flush();

            VkPipeline pFlat8  = Fullscreen::CreatePipeline(vs, fsFlat, rt8.fmt,  layE, NoBlend(), "bench flat8");
            VkPipeline pFlat16 = Fullscreen::CreatePipeline(vs, fsFlat, rt16.fmt, layE, NoBlend(), "bench flat16");
            VkPipeline pTex8   = Fullscreen::CreatePipeline(vs, fsTex,  rt8.fmt,  layT, NoBlend(), "bench tex8");
            VkPipeline pDrop8  = Fullscreen::CreatePipeline(vs, fsDrop, rt8.fmt,  layT, NoBlend(), "bench drop8");

            auto drawTest = [&](Target& t, VkPipeline p, VkPipelineLayout lay, bool useSet, const char* name, int R) {
                if (!p) { Msg("![nx bench] %s: pipeline failed", name); return; }
                BeginCmd();
                ToLayout(t.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                for (int i = 0; i < R; ++i) {
                    VK::RenderingBuilder(t.ext).Color(t.view, VK_ATTACHMENT_LOAD_OP_DONT_CARE).BeginPlain(s_cmd);
                    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                    if (useSet) vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &set[0], 0, nullptr);
                    vkCmdDraw(s_cmd, 3, 1, 0, 0);
                    vkCmdEndRendering(s_cmd);
                }
                Report(name, EndAndRun(), R, 0, double(t.ext.width) * t.ext.height, base);
            };
            drawTest(rt8,  pFlat8,  layE, false, "fullscreen flat colour 1280x720 RGBA8", 100);
            drawTest(rt16, pFlat16, layE, false, "fullscreen flat colour 960x540 RGBA16F", 100);
            drawTest(rt8,  pTex8,   layT, true,  "fullscreen textured 1280x720 RGBA8", 100);
            drawTest(rt8,  pDrop8,  layT, true,  "fullscreen textured+discard-all 1280x720", 100);

            for (VkPipeline p : { pFlat8, pFlat16, pTex8, pDrop8 }) if (p) vkDestroyPipeline(dev, p, nullptr);
        } else Msg("![nx bench] texture/sampler/layout setup failed -- draw tests skipped");

        if (smp)  vkDestroySampler(dev, smp, nullptr);
        if (texV) vkDestroyImageView(dev, texV, nullptr);
        if (tex)  VK::Vram::DestroyImage(VulkanHW.m_Allocator, tex, texA);
        if (layT) vkDestroyPipelineLayout(dev, layT, nullptr);
        if (layE) vkDestroyPipelineLayout(dev, layE, nullptr);
        if (dp)   vkDestroyDescriptorPool(dev, dp, nullptr);
        if (sl)   vkDestroyDescriptorSetLayout(dev, sl, nullptr);
    }

    Msg("[nx bench] theory (handheld 460 MHz, GM20B 2 SM / 16 ROP / 16 TMU, 25.6 GB/s shared): ~25 GB/s buffers, ~7 Gpx/s fill, ~7 Gtex/s");
    Msg("[nx bench] ===== done =====");
    FreeTarget(rt8); FreeTarget(rt16);
    a.Destroy(); b.Destroy();
    vkDestroyFence(dev, s_fence, nullptr); s_fence = VK_NULL_HANDLE;
    vkDestroyCommandPool(dev, s_pool, nullptr); s_pool = VK_NULL_HANDLE; s_cmd = VK_NULL_HANDLE;
}

} // namespace NxBench
} // namespace VK

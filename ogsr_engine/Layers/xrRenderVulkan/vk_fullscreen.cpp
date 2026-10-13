// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_fullscreen.h"
#include "vk_rendering.h"     // VK::RenderingBuilder
#include "vk_gfx_pipeline.h"     // VK::GfxPipelineBuilder
#include "HW_Vulkan.h"

namespace VK { namespace Fullscreen {

VkPipelineColorBlendAttachmentState OpaqueAttachment(VkColorComponentFlags writeMask)
{
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = writeMask;
    ba.blendEnable    = VK_FALSE;
    return ba;
}

VkPipelineColorBlendAttachmentState AdditiveAttachment()
{
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    ba.blendEnable         = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.colorBlendOp        = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.alphaBlendOp        = VK_BLEND_OP_ADD;
    return ba;
}

VkPipeline CreatePipeline(VkShaderModule vs, VkShaderModule fs, VkFormat colorFmt,
                          VkPipelineLayout layout,
                          const VkPipelineColorBlendAttachmentState& blend,
                          const char* tag,
                          VkExtent2D shadingRate)
{
    // Optional STATIC coarse shading rate (pipelineFragmentShadingRate). Combiners KEEP =
    // ignore primitive/attachment rate, use this pipeline rate. Only when the device
    // supports it AND the caller asked for > 1x1 (else per-pixel as before).
    VkPipelineFragmentShadingRateStateCreateInfoKHR fsr{};
    const void* rateNext = nullptr;
    if (VulkanHW.m_bVRSPipelineSupported && (shadingRate.width > 1 || shadingRate.height > 1)) {
        fsr.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
        fsr.fragmentSize = shadingRate;
        fsr.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        fsr.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        rateNext = &fsr;
    }

    // No vertex input, no depth — a fullscreen triangle drawn from gl_VertexIndex.
    return GfxPipelineBuilder(layout)
        .Vert(vs).Frag(fs)
        .ColorBlend(colorFmt, blend)
        .RenderingNext(rateNext)
        .Build("%s fullscreen", tag ? tag : "Fullscreen");
}

VkPipeline CreatePipelineMRT(VkShaderModule vs, VkShaderModule fs,
                             const VkFormat* colorFmts, u32 count,
                             VkPipelineLayout layout,
                             const VkPipelineColorBlendAttachmentState* blends,
                             const char* tag)
{
    GfxPipelineBuilder b(layout);
    b.Vert(vs).Frag(fs);
    for (u32 i = 0; i < count; ++i)
        b.ColorBlend(colorFmts[i], blends[i]);
    return b.Build("%s fullscreen MRT", tag ? tag : "Fullscreen");
}

void DrawSimple(VkCommandBuffer cmd, VkImageView dstView, VkExtent2D extent,
                VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set,
                const void* push, u32 pushSize)
{
    RenderingBuilder(extent).Color(dstView, VK_ATTACHMENT_LOAD_OP_DONT_CARE).BeginPlain(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    if (push && pushSize)
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void DrawMRT(VkCommandBuffer cmd, const VkImageView* dstViews, u32 count, VkExtent2D extent,
             VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set,
             const void* push, u32 pushSize)
{
    RenderingBuilder rb(extent);
    for (u32 i = 0; i < count && i < RenderingBuilder::kMaxColor; ++i)
        rb.Color(dstViews[i], VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    rb.BeginPlain(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    if (push && pushSize)
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

}}  // namespace VK::Fullscreen

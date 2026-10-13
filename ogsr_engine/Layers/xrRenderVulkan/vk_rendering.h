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
// Dynamic-rendering scope builder
//
// Opening a dynamic-rendering scope was ~20 hand-written lines per site (one
// VkRenderingAttachmentInfo per attachment + a VkRenderingInfo + the viewport /
// scissor pair), repeated 40+ times across the renderer — vk_rain.cpp and
// vk_wallmarks.cpp held 31 byte-identical lines in a row. This states only what
// a site actually decides: which views, which load/store ops, which extent.
//
// Defaults reproduce what the sites were already writing:
//   renderArea.offset {0,0}, layerCount 1,
//   colour imageLayout COLOR_ATTACHMENT_OPTIMAL, depth DEPTH_ATTACHMENT_OPTIMAL
//   (the renderer's single-layout convention — see vk_pass_context.h),
//   loadOp LOAD / storeOp STORE,
//   pDepthAttachment = nullptr until Depth() is called with a live view.
//
// Three ways to open the scope, matching the three conventions in the codebase:
//   Begin()        - scope only; the caller owns viewport/scissor.
//   BeginFlipped() - + the X-Ray D3D-style NEGATIVE-height viewport and a full
//                    scissor. Every scene / shadow raster uses this; the Y flip
//                    is what makes the depth match the sampling convention.
//   BeginPlain()   - + an unflipped viewport and a full scissor, for offscreen
//                    maps indexed directly by compute (water mask, terrain mask).
//
//   VK::RenderingBuilder(ctx.extent)
//       .Color(ctx.colorView)
//       .Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE)
//       .BeginFlipped(cmd);
//
// A builder is a plain value: keep one alive and Begin() it again after
// SetDepthLoad()/SetColorLoad() when a site re-enters the SAME attachments with
// a different load op (the spot-tile and Stage-D clone loops do exactly that).
// ============================================================================

// X-Ray renders with a D3D-style flipped Y: origin at the bottom, negative
// viewport height. Every raster that writes into a target sampled with the
// engine's UV convention must use this, or the image comes out upside down.
inline void SetFlippedViewport(VkCommandBuffer cmd, VkExtent2D extent)
{
    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = float(extent.height);
    vp.width    = float(extent.width);
    vp.height   = -float(extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {}, extent };
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

inline void SetPlainViewport(VkCommandBuffer cmd, VkExtent2D extent)
{
    VkViewport vp{ 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {}, extent };
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

class RenderingBuilder
{
public:
    enum : u32 { kMaxColor = 8 };

    explicit RenderingBuilder(VkExtent2D extent)
    {
        m_info.sType             = VK_STRUCTURE_TYPE_RENDERING_INFO;
        m_info.renderArea.extent = extent;
        m_info.layerCount        = 1;
    }
    RenderingBuilder(u32 width, u32 height) : RenderingBuilder(VkExtent2D{ width, height }) {}

    // ---- attachments -------------------------------------------------------
    RenderingBuilder& Color(VkImageView view,
                            VkAttachmentLoadOp  load  = VK_ATTACHMENT_LOAD_OP_LOAD,
                            VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE)
    {
        VERIFY2(m_colorN < kMaxColor, "RenderingBuilder: too many colour attachments");
        VkRenderingAttachmentInfo& a = m_color[m_colorN++];
        a             = VkRenderingAttachmentInfo{};
        a.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a.imageView   = view;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp      = load;
        a.storeOp     = store;
        return *this;
    }

    RenderingBuilder& ColorClear(VkImageView view, const VkClearColorValue& clear,
                                 VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE)
    {
        Color(view, VK_ATTACHMENT_LOAD_OP_CLEAR, store);
        m_color[m_colorN - 1].clearValue.color = clear;
        return *this;
    }

    // A null view leaves the scope depth-less (some sites conditionally have no
    // depth target at all), so callers can pass ctx.depthView unconditionally.
    RenderingBuilder& Depth(VkImageView view,
                            VkAttachmentLoadOp  load  = VK_ATTACHMENT_LOAD_OP_LOAD,
                            VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE,
                            float clearDepth = 1.0f)
    {
        m_depth             = VkRenderingAttachmentInfo{};
        m_depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        m_depth.imageView   = view;
        m_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        m_depth.loadOp      = load;
        m_depth.storeOp     = store;
        m_depth.clearValue.depthStencil = { clearDepth, 0 };
        m_hasDepth = (view != VK_NULL_HANDLE);
        return *this;
    }

    // ---- render area / layers / extensions --------------------------------
    RenderingBuilder& Offset(int32_t x, int32_t y) { m_info.renderArea.offset = { x, y }; return *this; }
    RenderingBuilder& Layers(u32 n)                { m_info.layerCount = n;               return *this; }
    // Extension chain on VkRenderingInfo — e.g. the VRS shading-rate attachment.
    // The pointee must outlive every Begin() on this builder.
    RenderingBuilder& Next(const void* pNext)      { m_info.pNext = pNext;                return *this; }

    // ---- re-arm between two Begin()s on the same attachments ---------------
    RenderingBuilder& SetDepthLoad(VkAttachmentLoadOp load) { m_depth.loadOp = load; return *this; }
    RenderingBuilder& SetColorLoad(VkAttachmentLoadOp load, u32 index = 0)
    {
        VERIFY(index < m_colorN);
        m_color[index].loadOp = load;
        return *this;
    }

    VkExtent2D Extent() const { return m_info.renderArea.extent; }

    // ---- open the scope ----------------------------------------------------
    void Begin(VkCommandBuffer cmd)
    {
        m_info.colorAttachmentCount = m_colorN;
        m_info.pColorAttachments    = m_colorN ? m_color : nullptr;
        m_info.pDepthAttachment     = m_hasDepth ? &m_depth : nullptr;
        vkCmdBeginRendering(cmd, &m_info);
    }
    void BeginFlipped(VkCommandBuffer cmd) { Begin(cmd); SetFlippedViewport(cmd, m_info.renderArea.extent); }
    void BeginPlain(VkCommandBuffer cmd)   { Begin(cmd); SetPlainViewport(cmd, m_info.renderArea.extent); }

private:
    VkRenderingAttachmentInfo m_color[kMaxColor]{};
    VkRenderingAttachmentInfo m_depth{};
    VkRenderingInfo           m_info{};
    u32                       m_colorN   = 0;
    bool                      m_hasDepth = false;
};

} // namespace VK

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Per-frame context shared between render passes.
//
// Plain-data carrier populated once in CRender::Begin() and forwarded to each
// Pass_* function. Pass code reads from this instead of poking globals
// directly, so when the framegraph lands the only thing that has to change is
// who fills the struct.

#pragma once
#include "vk_core.h"
#include "vk_rendering.h"
// Fmatrix lives in xrCore/_matrix.h, pulled in transitively via stdafx in
// every TU that includes this header.

namespace VK
{
    struct FrameContext
    {
        VkCommandBuffer cmd          = VK_NULL_HANDLE;
        u32             imageIndex   = 0;
        VkExtent2D      extent       = { 0, 0 };   // SCENE render resolution (< display when DLSS upscaling)
        VkExtent2D      displayExtent= { 0, 0 };   // swapchain / present resolution (== extent unless upscaling)
        VkExtent2D      outExtent    = { 0, 0 };   // nfs28: top-left part of the swapchain image the composite + UI draw
                                                    // (== displayExtent unless the Switch display crop stretches a smaller one)

        // Render targets. Passes render to THESE, never to Swapchain.m_Images[...]
        // directly — so when the framegraph repoints them at an offscreen HDR /
        // G-buffer target, pass code needs no change. Filled in CRender::Begin();
        // for now colorImage/View are this frame's swapchain image + view.
        // Layout invariant: colorImage is in COLOR_ATTACHMENT_OPTIMAL for the
        // entire pass sequence (Begin transitions in once, End transitions to
        // PRESENT once); passes must NOT transition it. depthView stays in
        // DEPTH_ATTACHMENT_OPTIMAL the whole frame likewise.
        VkImage         colorImage   = VK_NULL_HANDLE;
        VkImageView     colorView    = VK_NULL_HANDLE;
        VkImageView     depthView    = VK_NULL_HANDLE;

        const Fmatrix*  viewProj     = nullptr;
        const Fmatrix*  viewProjPrev = nullptr;  // history matrix for motion vectors / TAA — unused in phase 1
        u32             frameIdx     = 0;
        float           dt           = 0.0f;
    };

    // ---- The height screen-space LOD thresholds must be measured in ----------
    // Every "how many pixels does this cover" decision — the cluster DAG cut
    // (r_cluster_lod), the small-mesh SSA cull (r_ssa_px), the tree screen-size
    // tests — used Device.dwHeight, which is the WINDOW height. Under FSR / DLSS /
    // r_render_scale the scene is rasterised into a SMALLER buffer, so those tests
    // were choosing native-resolution geometry detail for a quarter-resolution
    // raster: the draw count did not fall when the resolution did.
    //
    // That is measurable, not theoretical. On a 780M at 2560x1600 the depth prepass
    // fits 4.8 ms + 95 ns x draws and the colour pass 10.0 ms + 82 ns x draws, so
    // ~50% of the prepass is per-draw cost pinned to a resolution it is not drawing
    // at. It is also the correctness-preserving direction: a cut whose error budget
    // is N pixels at 1600 rows is N/2 pixels at 800 rows — finer than anything the
    // raster can show.
    //
    // r_lod_render_res 0 restores the old display-height behaviour for A/B.
    u32 LodScreenHeight();

    // Begin a dynamic-rendering pass that draws ON TOP of the world pass under the
    // single-layout convention: colorView is already COLOR_ATTACHMENT and depthView
    // DEPTH_ATTACHMENT for the whole frame, so both are LOADed and no layout
    // transition happens here. Sets the X-Ray D3D-style negative-height viewport +
    // full scissor. Was copy-pasted in the grass/tree/LOD overlay draws.
    // depthStore = DONT_CARE for passes that test but don't write depth.
    inline void BeginOverlayRendering(VkCommandBuffer cmd, const FrameContext& ctx,
                                      VkAttachmentStoreOp depthStore = VK_ATTACHMENT_STORE_OP_STORE)
    {
        RenderingBuilder(ctx.extent)
            .Color(ctx.colorView)
            .Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_LOAD, depthStore)
            .BeginFlipped(cmd);
    }
}

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_framegraph.h"

// r_fg_barriers — see vk_console_min.cpp. Read here for exactly one decision:
// whether a sample window can be left open for the next pass to close.
extern int ps_r_fg_barriers;

namespace VK
{

FrameGraph g_FrameGraph;

void FrameGraph::BeginFrame(VkImage colorImg, VkImage depthImg)
{
    // CRender::Begin leaves the HDR color in COLOR_ATTACHMENT (after the clear's
    // TRANSFER_DST -> COLOR_ATTACHMENT barrier) and the scene depth in
    // DEPTH_ATTACHMENT (the once-per-frame UNDEFINED -> depth transition). Seed
    // both to those states — Seed records only, it emits no barrier. The tracked
    // stage/access mirror the last write Begin performed, so the first consumer's
    // Require() derives a correct src scope.
    m_color.Seed(colorImg, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    m_depth.Seed(depthImg, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    // NOTE: the v2 registry is deliberately NOT cleared here — see the header.
    // A persistent target's layout survives the frame boundary; wiping it every
    // frame would make the first consumer re-transition from a state the image is
    // not actually in.
}

// --- v2 registry -----------------------------------------------------------

ImageState& FrameGraph::Track(VkImage img, VkImageAspectFlags aspect, VkImageLayout curLayout,
                              VkPipelineStageFlags2 curStage, VkAccessFlags2 curAccess)
{
    // emplace returns (iterator, inserted). Seed ONLY on a real insert: on every
    // later call the caller's "current" arguments are its create-time belief,
    // which is stale — the tracked state is the truth.
    auto res = m_images.emplace(img, ImageState{});
    if (res.second)
        res.first->second.Seed(img, aspect, curLayout, curStage, curAccess);
    return res.first->second;
}

void FrameGraph::Forget(VkImage img)  { m_images.erase(img); }

// --- the scene-depth invariant (see the header) -----------------------------

void SceneDepthToSampled(VkCommandBuffer cmd, VkPipelineStageFlags2 readers)
{
    ImageState& st = g_FrameGraph.Depth();
    if (!st.Valid()) return;   // no frame seeded (level load, a Begin that bailed)
    st.Require(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, readers,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void SceneDepthToAttachment(VkCommandBuffer cmd)
{
    ImageState& st = g_FrameGraph.Depth();
    if (!st.Valid()) return;
    st.Require(cmd, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
}

void SceneDepthEndSample(VkCommandBuffer cmd)
{
    // Deferred only when the derivation is the one placing inter-pass barriers.
    if (ps_r_fg_barriers == 0) SceneDepthToAttachment(cmd);
}

void FrameGraph::Reset()
{
    m_images.clear();
    m_color = ImageState{};
    m_depth = ImageState{};
}

} // namespace VK

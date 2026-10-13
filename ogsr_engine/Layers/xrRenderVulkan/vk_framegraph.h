// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - FrameGraph: the render graph's frame-wide execute core (v1).
//
// A single place that tracks the current (layout / stage / access) of the scene's
// persistent attachments — the HDR color target and the scene depth — via the
// ImageState primitive (vk_barriers). Any pass Requires the state it needs and a
// barrier is emitted ONLY on a real change; a run of consumers that all need the
// same state coalesces to one transition. Seeded each frame in CRender::Begin at
// the layouts Begin leaves color/depth in.
//
// WHY it exists: the renderer's barriers were hand-placed per pass, and the depth
// target thrashed ATTACHMENT<->SHADER_READ four times per frame (SSAO, VRS, VSM
// mark, VSM resolve each round-tripping it) — serializing the GPU. Routing that
// window through Depth() coalesces it to a single round-trip (r_fg_coalesce).
//
// v1 SCOPE — deliberately minimal, NOT over-built:
//   * Tracks color + depth. The depth-read window (Pass_World) is routed through
//     Depth(); the coalescing is the measured win.
//   * It is NOT a scheduler: no topological sort, no transient-memory aliasing, no
//     async-queue assignment. The inter-pass SceneAttachmentBarrier stays the
//     conservative default until passes declare per-resource read/write sets.
// EXTENSION SURFACE (documented, not built): async compute adds a queue-family to
//   ImageState + cross-queue release/acquire barriers here; aliasing adds a
//   transient-resource table; precise inter-pass barriers replace SceneAttachment-
//   Barrier once passes declare their resource usage. See the framegraph memory.

//
// v2 (2026-08-16) — THE REGISTRY. v1 tracked exactly two images because they were
// the two that thrashed. Everything else (VSM atlas, cascades, SSAO/HZB targets,
// and every cull/indirect BUFFER) stayed hand-barriered across ~50 sites, and
// that is the single root the remaining Vulkan levers all sit behind:
//   * async compute needs a queue-family per resource to emit release/acquire;
//   * parallel command recording needs barriers DERIVED between passes, which is
//     impossible while each pass hand-places its own inside its body;
//   * transient-memory aliasing needs a resource table to alias against.
// So Track()/TrackBuffer() generalize the same Require() discipline to any
// resource, keyed by handle, with STABLE references (node-based map) so an owner
// can resolve its state once and keep the reference.
//
// Tracked state deliberately PERSISTS ACROSS FRAMES: a persistent render target's
// layout genuinely survives the frame boundary, and re-seeding it every frame is
// how a tracker starts lying. Only color/depth are re-seeded per frame, because
// CRender::Begin really does re-acquire/UNDEFINED them.
// ⚠ Forget() on destroy/resize is therefore MANDATORY — the driver reuses handles,
// so a stale entry would hand a new image the dead one's layout.

#pragma once
#include "vk_barriers.h"   // VK::ImageState, VK::BufferState

namespace VK
{
    class FrameGraph
    {
    public:
        // Seed color + depth at the layouts CRender::Begin leaves them in. Emits no
        // barrier — it only records the known frame-start state. Called every frame.
        void BeginFrame(VkImage colorImg, VkImage depthImg);

        // The tracked scene attachments. Passes route their transitions through
        // these so redundant barriers coalesce.
        ImageState& Color() { return m_color; }
        ImageState& Depth() { return m_depth; }

        // --- v2 registry --------------------------------------------------------
        // Register `img` on first call, recording the state it is ALREADY in (no
        // barrier emitted, exactly like Seed); later calls ignore the seed
        // arguments and return the live tracked state. The reference is stable for
        // the resource's lifetime, so a subsystem can resolve it once at create
        // time and keep it.
        ImageState& Track(VkImage img, VkImageAspectFlags aspect,
                          VkImageLayout curLayout,
                          VkPipelineStageFlags2 curStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                          VkAccessFlags2 curAccess = 0);

        // Same for a buffer. The seed is normally (TOP_OF_PIPE, 0) — a freshly
        // created buffer has no prior access to wait on.
        BufferState& TrackBuffer(VkBuffer buf,
                                 VkPipelineStageFlags2 curStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 VkAccessFlags2 curAccess = 0);

        // Drop a resource. MUST be called when the image/buffer is destroyed or
        // recreated (resize, quality flip) — see the handle-reuse warning above.
        void Forget(VkImage img);
        void Forget(VkBuffer buf);

        // Device teardown: drop every entry.
        void Reset();

        // Diagnostics: how many resources the graph is tracking.
        u32 TrackedImages() const  { return (u32)m_images.size(); }
        u32 TrackedBuffers() const { return (u32)m_buffers.size(); }

    private:
        ImageState m_color;
        ImageState m_depth;
        // Node-based on purpose: Track() hands out references that outlive
        // subsequent insertions. A vector here would dangle on the first regrow.
        xr_unordered_map<VkImage,  ImageState>  m_images;
        xr_unordered_map<VkBuffer, BufferState> m_buffers;
    };

    extern FrameGraph g_FrameGraph;

    // ---- The frame-wide scene-depth invariant, in one place ----------------
    //
    // The scene depth sits in DEPTH_ATTACHMENT_OPTIMAL for the whole frame (the
    // single-layout convention, vk_pass_context.h) and several passes need to
    // SAMPLE it instead: the SSAO/VRS/VSM chain inside Pass_World, and — outside
    // it — MotionVecDyn's reconstruction, the water surface's depth fade and the
    // light cones' occlusion march. Each of those used to hand-place its own
    // ATTACHMENT<->SHADER_READ pair, which had two costs beyond the duplication:
    //
    //  * the raw ImageBarrier DERIVES its src scope from the OLD LAYOUT, so it
    //    waits on that layout's canonical access rather than on what actually
    //    touched the image — under-approximating whenever the real previous access
    //    was something else (the same latent hole [#3] the Pass_World window was
    //    routed through the tracker to close);
    //  * the tracked state went stale, because a transition the graph never saw is
    //    a transition it cannot know about. That is what made the derived
    //    inter-pass barrier (r_fg_barriers) rest on an invariant kept by hand —
    //    "every one of those passes restores DEPTH_ATTACHMENT before it returns" —
    //    instead of on the tracker being the only writer of the layout.
    //
    // These two are that pair, once, over g_FrameGraph.Depth(): the src scope comes
    // from the accumulated real state, a redundant flip costs nothing, and the
    // tracker cannot be bypassed. CALL THEM IN PAIRS — ToAttachment restores the
    // frame-wide invariant the next pass's rendering scope depends on.
    //
    // `readers` is the stages that will sample it. FRAGMENT alone is right for the
    // three fullscreen/forward consumers; Pass_World's window passes COMPUTE too.
    //
    void SceneDepthToSampled(VkCommandBuffer cmd,
                             VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    void SceneDepthToAttachment(VkCommandBuffer cmd);

    // Close a sample window in a pass that NEVER attaches the depth itself — the
    // water surface and the light cones sample it and are done. Which is the whole
    // point: there is nothing in those passes that needs the attachment layout back,
    // only the passes AFTER them, and the derived barrier already asks for what each
    // of those needs. So under r_fg_barriers this DEFERS: the depth stays sampled,
    // the next declaration decides, and two consecutive samplers share one flip
    // instead of round-tripping twice each. ExecutePasses closes whatever is still
    // open after the last pass, so nothing downstream inherits a sampled depth.
    //
    // It still restores on the conservative path, which changes no layouts and so
    // has nobody to defer to — a pass that left the depth sampled there would hand
    // the next one a rendering scope in the wrong layout. That is the only reason
    // this reads the cvar: the policy belongs to whoever sources the barriers, and
    // pass code should just say what it did.
    //
    // A pass that samples the depth and then ATTACHES it (MotionVecDyn's overlay,
    // Pass_World's colour pass) calls SceneDepthToAttachment instead — it needs the
    // layout back for itself, not for anyone else.
    void SceneDepthEndSample(VkCommandBuffer cmd);
}

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - VulkanUI namespace: pipeline + vertex buffer + descriptor
// pool for the menu/UI subsystem.
//
// Adapted from the monolith UI path with two notable changes: RCache.m_Cmd
// is replaced by the global g_VkUI_FrameCmd (set by CRender::Begin each
// frame), and RCache.stat.calls/verts are dropped — vkUIRender already does
// its own throttled draw-count logging.

#pragma once
#include "vk_core.h"
#include "../../xrCore/render_thread.h"
#include <vulkan/vulkan.h>

// Current frame's command buffer, set by CRender::Begin and reset by End.
// VulkanUI / vkUIRender use this in immediate mode; if VK_NULL_HANDLE, draw
// commands are buffered into the deferred queue and replayed from CRender::End.
extern VkCommandBuffer g_VkUI_FrameCmd;

namespace VulkanUI
{
    void Create();
    void Destroy();
    void EndUIPass();
    void BeginUIPassInternal();
    void ReplayDeferredUI();
    // Prints the next <lines> UI-pass transitions (`ui_pass_trace 200`). Every
    // early-out in BeginUIPassInternal looks the same from outside -- "no UI this
    // frame" -- and one of them used to end in a driver fault, so the state
    // machine needs to be able to say which one it took.
    void TraceUIPass(u32 lines);

    // Render thread, at the head of the frame (Begin_Render): a fresh command buffer has no rendering instance open, so the
    // UI pass state is reset. The PRODUCER side (vertex ring region, write offset, draw queue) is per frame slot and is reset on
    // the main thread by BeginExtract, after the ring slot's fence was waited (that wait is the proof the GPU finished reading
    // the region -- frames-in-flight discipline, same as the bone SSBO).
    void OnFrameBegin();

    // TWO-SLOT PIPELINE: the PRODUCER side (the vertex ring region, the write offset, the draw queue) belongs to a frame
    // slot. The main thread emits UI into the slot of the frame it extracts while the render thread replays the previous
    // frame's from the other one. BeginExtract resets the extract slot and picks its ring region; it runs on the main thread
    // for EVERY frame (a frame that never reaches End used to leave its count and offset behind).
    void BeginExtract(u32 ringSlot, bool ringReady = true);   // ringReady false: the region may still be in the GPU's hands -- emit nothing

    // Exposed for vkUIRender — write target for PushPoint, drain for FlushPrimitive.
    extern void*           s_pMappedVB;
    extern VkDescriptorSet s_WhiteTextureSet;   // olive: a texture that should be there and is not
    extern VkDescriptorSet s_PlainTextureSet;   // pure white: a shader with no texture by design
    extern bool            s_bUIPassActive;
    extern VkPipeline      s_Pipeline;          // TRIANGLE_LIST
    extern VkPipeline      s_PipelineLineList;   // LINE_LIST  (crosshair)
    extern VkPipeline      s_PipelineLineStrip;  // LINE_STRIP (UIWindow borders)
    extern VkPipelineLayout s_PipelineLayout;
    extern VkDescriptorSetLayout s_DescriptorSetLayout;
    extern VkDescriptorPool s_DescriptorPool;

    // PER-SLOT region size (the actual buffer is this × FRAMES_IN_FLIGHT — a
    // frame-fenced ring; see OnFrameBegin). Settings menu draws hundreds of
    // widgets per frame (ui_common-backed panels + every option label). 256KB
    // wraps mid-frame and the wraparound path resets s_UIVertexOffset = 0,
    // overwriting still-pending draws — that produced the "only right side of
    // settings renders" bug. 8MB per slot is wasteful but cheap (host VMA).
    static const VkDeviceSize VERTEX_BUFFER_SIZE = 8 * 1024 * 1024;

    struct DeferredUICmd
    {
        enum Type { Draw, Scissor, ResetScissor };
        Type type;
        u32 vertexBufferOffset;
        u32 vertexCount;
        VkDescriptorSet textureSet;
        VkRect2D scissorRect;
        VkPipeline pipeline;   // topology-specific pipeline (line vs triangle); null = default triangle list

    };
    static const u32 MAX_DEFERRED_CMDS = 4096;
    struct UISlot
    {
        DeferredUICmd cmds[MAX_DEFERRED_CMDS];
        u32 count        = 0;
        u32 vertexOffset = 0;   // slot-relative write offset
        u32 vbBase       = 0;   // byte base of this frame region of the vertex ring
        bool dead        = false;   // the region is not proven free (failed fence wait): UI emission is dropped for this frame
    };
    extern RT::PerSlot<UISlot> g_UISlots;
    inline UISlot& UI() { return g_UISlots.cur(); }   // by role: extract writes its slot, End_Render replays its own

    // Stats — minimal counters; vkUIRender already does its own throttled logging.
    struct FrameStats { u32 droppedCmds = 0; u32 bufferWraps = 0; u32 deferredCmds = 0; u32 deferredDraws = 0; u32 totalVerts = 0; };
    extern FrameStats s_FrameStats;
}

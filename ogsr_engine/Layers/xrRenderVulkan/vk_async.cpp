// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_async.h"
#include "HW_Vulkan.h"
#include "vk_command_buffer.h"   // CommandManager — graphics frame timeline to order against

// Global scope (not namespaced), like the other ps_r_* render cvars.
extern int ps_r_async;

namespace VK { namespace Async {

static bool            s_inited  = false;
static bool            s_failed  = false;   // init failed once → never retry (avoid per-frame spam)
static VkCommandPool   s_pool    = VK_NULL_HANDLE;
static VkCommandBuffer s_cmd[VK_FRAMES_IN_FLIGHT] = {};
static VkSemaphore     s_timeline = VK_NULL_HANDLE;
static u64             s_value    = 0;       // monotonic timeline value

// Per-frame handoff state (set in Submit, read by GetGraphicsWait same frame).
static bool s_pendingThisFrame = false;
static u64  s_lastSignaled     = 0;
static bool s_open             = false;   // Begin() opened the buffer, Submit() owes it a close
static u32  s_slot             = 0;       // frame-in-flight slot, latched in FrameBegin
static u64  s_gfxWaitValue     = 0;       // graphics timeline point this frame's compute waits on

// --- Compute-batch timing -----------------------------------------------------
// The shared profiler pool is RESET by the graphics segment, so timestamps written
// from this queue would race that reset — which is why the Vol zones are skipped
// under async. Without them the compute work is INVISIBLE: the frame's gpu_total
// simply loses ~2 ms and that looks like a win whether the work overlapped or not.
// So this queue gets its OWN two-timestamp pool per in-flight slot, reset on the
// compute queue itself. It answers "how much work are we hiding", which combined
// with the graphics frame time tells whether it actually hid.
static VkQueryPool s_qpool     = VK_NULL_HANDLE;
static float       s_periodNs  = 0.f;
static bool        s_qWritten[VK_FRAMES_IN_FLIGHT] = {};   // slot holds a completed pair
static double      s_msAcc     = 0.0;                       // accumulated ms for the average
static u32         s_msN       = 0;
static float       s_msLast    = 0.f;                       // last measured batch (ms)

static bool EnsureInit()
{
    if (s_inited) return true;
    if (s_failed) return false;

    // No async without a dedicated compute family (m_ComputeFamily aliases graphics
    // otherwise — see HW_Vulkan queue selection). Nothing to overlap; stay dormant.
    if (VulkanHW.m_ComputeFamily == VulkanHW.m_GraphicsFamily) { s_failed = true; return false; }

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = VulkanHW.m_ComputeFamily;
    if (vkCreateCommandPool(VulkanHW.m_Device, &pci, nullptr, &s_pool) != VK_SUCCESS) {
        Msg("![VK Async] compute command pool create failed — disabled"); s_failed = true; return false;
    }

    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = s_pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = VK_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(VulkanHW.m_Device, &ai, s_cmd) != VK_SUCCESS) {
        Msg("![VK Async] compute command buffer alloc failed — disabled"); s_failed = true; return false;
    }

    VkSemaphoreTypeCreateInfo tci{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tci.initialValue  = 0;
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &tci;
    if (vkCreateSemaphore(VulkanHW.m_Device, &sci, nullptr, &s_timeline) != VK_SUCCESS) {
        Msg("![VK Async] timeline semaphore create failed — disabled"); s_failed = true; return false;
    }

    // Own timestamp pool (2 per slot: batch begin/end). Optional — a device without
    // timestamps just loses the measurement, not the feature.
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(VulkanHW.m_PhysicalDevice, &props);
        s_periodNs = props.limits.timestampPeriod;
        if (s_periodNs > 0.f) {
            VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = VK_FRAMES_IN_FLIGHT * 2;
            if (vkCreateQueryPool(VulkanHW.m_Device, &qci, nullptr, &s_qpool) != VK_SUCCESS)
                s_qpool = VK_NULL_HANDLE;
        }
        for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) s_qWritten[i] = false;
    }

    s_inited = true;
    Msg("[VK Async] init OK — compute family %u, timeline ready, batch timing %s (r_async)",
        VulkanHW.m_ComputeFamily, s_qpool ? "on" : "off");
    return true;
}

bool Available()
{
    if (!ps_r_async) return false;
    return EnsureInit();
}

void FrameBegin(u32 slot)
{
    s_pendingThisFrame = false;
    s_open             = false;
    s_slot             = slot;
    // Snapshot the graphics timeline BEFORE this frame's submits bump it: that value
    // means "every previously submitted graphics batch has completed". Submit() makes
    // the compute batch wait on it, so compute never overwrites a volume the previous
    // frame is still reading. Ordering against PAST frames only — the current frame,
    // the one we overlap, is unaffected.
    s_gfxWaitValue = CommandManager.GetFrameTimelineValue();
}

VkCommandBuffer Begin()
{
    if (!Available())                  return VK_NULL_HANDLE;
    if (s_slot >= VK_FRAMES_IN_FLIGHT) return VK_NULL_HANDLE;

    // This slot's command buffer last ran in the frame that used the same in-flight
    // slot; that graphics frame waited on this timeline and has since retired (its
    // fence was waited in CRender::Begin), so the reset is safe.
    VkCommandBuffer cmd = s_cmd[s_slot];
    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;

    if (s_qpool != VK_NULL_HANDLE) {
        const u32 base = s_slot * 2;
        // Harvest THIS slot's previous pair before overwriting it. The frame that
        // wrote it retired behind its fence (CRender::Begin waited), so the results
        // are ready and WAIT is not a stall — but ask without WAIT anyway and just
        // skip a sample if the driver says not-ready.
        if (s_qWritten[s_slot]) {
            u64 ts[2] = {};
            if (vkGetQueryPoolResults(VulkanHW.m_Device, s_qpool, base, 2, sizeof(ts), ts,
                                      sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
                s_msLast = float(double(ts[1] - ts[0]) * double(s_periodNs) * 1e-6);
                s_msAcc += s_msLast; ++s_msN;
            }
            s_qWritten[s_slot] = false;
        }
        // Reset + open the pair ON THE COMPUTE QUEUE — that is the whole point: no
        // cross-queue ordering needed between the reset and the writes.
        vkCmdResetQueryPool(cmd, s_qpool, base, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_qpool, base);
    }

    s_open = true;
    return cmd;
}

void Submit()
{
    if (!s_open) return;               // nothing was recorded this frame
    s_open = false;

    VkCommandBuffer cmd = s_cmd[s_slot];
    if (s_qpool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_qpool, s_slot * 2 + 1);
        s_qWritten[s_slot] = true;
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return;

    // Throttled report: how much GPU work this queue is carrying. Paired with the
    // graphics frame time it answers whether the work overlapped or merely moved.
    if (s_msN >= 120) {
        Msg("[VK Async] compute batch: %.2f ms avg over %u frames (last %.2f)", float(s_msAcc / s_msN), s_msN, s_msLast);
        s_msAcc = 0.0; s_msN = 0;
    }

    const u64 signalVal = ++s_value;

    // Wait: the graphics timeline point captured at frame start (see FrameBegin).
    // COMPUTE_SHADER|TRANSFER as the wait stage — this batch samples and writes
    // images and copies buffers, nothing else.
    VkSemaphore          waitSem   = CommandManager.GetFrameTimeline();
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    const bool           doWait   = (waitSem != VK_NULL_HANDLE && s_gfxWaitValue > 0);

    VkTimelineSemaphoreSubmitInfo tl{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tl.waitSemaphoreValueCount   = doWait ? 1 : 0;
    tl.pWaitSemaphoreValues      = &s_gfxWaitValue;
    tl.signalSemaphoreValueCount = 1;
    tl.pSignalSemaphoreValues    = &signalVal;

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext                = &tl;
    si.waitSemaphoreCount   = doWait ? 1 : 0;
    si.pWaitSemaphores      = doWait ? &waitSem : nullptr;
    si.pWaitDstStageMask    = doWait ? &waitStage : nullptr;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &s_timeline;

    VkResult r;
    {
        std::lock_guard<std::mutex> queueLock(g_VkQueueMx);   // RENDER_THREAD_PLAN step 10: one lock for every queue submit/present/wait
        r = vkQueueSubmit(VulkanHW.m_ComputeQueue, 1, &si, VK_NULL_HANDLE);
    }
    if (r != VK_SUCCESS) { Msg("![VK Async] compute submit failed: %d", r); return; }

    s_lastSignaled     = signalVal;
    s_pendingThisFrame = true;
}

bool GetGraphicsWait(VkSemaphore& sem, u64& value)
{
    if (!s_pendingThisFrame) return false;
    sem   = s_timeline;
    value = s_lastSignaled;
    return true;
}

void Destroy()
{
    // Drain any in-flight compute submit before tearing down the timeline/pool it
    // references (graphics idle at shutdown covers the dependency, but this is the
    // explicit guarantee against a validation error on the compute queue).
    if (s_inited && VulkanHW.m_ComputeQueue != VK_NULL_HANDLE)
        vkQueueWaitIdle(VulkanHW.m_ComputeQueue);
    if (s_qpool    != VK_NULL_HANDLE) { vkDestroyQueryPool(VulkanHW.m_Device, s_qpool, nullptr); s_qpool = VK_NULL_HANDLE; }
    if (s_timeline != VK_NULL_HANDLE) { vkDestroySemaphore(VulkanHW.m_Device, s_timeline, nullptr); s_timeline = VK_NULL_HANDLE; }
    if (s_pool     != VK_NULL_HANDLE) { vkDestroyCommandPool(VulkanHW.m_Device, s_pool, nullptr);   s_pool = VK_NULL_HANDLE; }
    for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) s_cmd[i] = VK_NULL_HANDLE;
    s_inited = false; s_failed = false; s_value = 0; s_pendingThisFrame = false; s_lastSignaled = 0;
    s_open = false; s_slot = 0; s_gfxWaitValue = 0;
    s_msAcc = 0.0; s_msN = 0; s_msLast = 0.f;
    for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) s_qWritten[i] = false;
}

}} // namespace VK::Async

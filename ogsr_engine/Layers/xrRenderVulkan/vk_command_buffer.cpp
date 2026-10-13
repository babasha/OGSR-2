// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "../../xrCore/load_prof.h" // [rt prof]: named parts of a slow render-thread job
#include "vk_command_buffer.h"
#include "HW_Vulkan.h"
#include "vk_async.h"          // VK::Async::GetGraphicsWait — async-compute timeline wait
#include "vk_profiler.h"       // VK::Prof::Checkpoint / DumpCheckpoints — GPU-hang post-mortem
#include "vk_texture.h"         // VK::TexPrefetch::t_TexWorker — who is filling the ring
#ifdef _MSC_VER
#include <emmintrin.h>         // SSE2 streaming stores — RingCopy into write-combined staging
#endif
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>          // VK::FarmRun body — the pool takes work, not just copies

// Console cvars used below. Global scope on purpose: a namespace-scope extern
// mangles differently and silently fails to bind (see the SSAO lesson).
extern int ps_r_upload_wc_copy;
extern int ps_r_upload_cached;
extern int ps_r_upload_copy_threads;

// Defined next to the copy helpers further down; Destroy joins them.
namespace VK { void UploadCopyFarmShutdown(); }

// Upload accounting, defined further down (next to the ring code it measures) but
// written to from EnsureUploadCmdOpen, which sits above it.
namespace VK { namespace UploadProf {
extern float s_wrapWaitMs, s_copyMs, s_recordMs, s_reopenWaitMs, s_flushMs;
extern u32   s_wraps, s_stagings, s_reopens, s_flushes;
extern u64   s_copyBytes;
void Dump();
}}



// Глобальный экземпляр
CVulkanCommandManager CommandManager;

// Создание command pools и buffers
namespace { void DestroyImmSlots(); }   // RENDER_THREAD_PLAN step 10 (defined with the per-thread immediate slots)

void CVulkanCommandManager::Create()
{
    VK::Vram::Scope _vram_scope("Staging");
    Msg("[Vulkan] Creating command pools and buffers...");

    // Создаём command pool для каждого frame in flight
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = VulkanHW.m_GraphicsFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VK_CHECK_CRITICAL(vkCreateCommandPool(VulkanHW.m_Device, &poolInfo, nullptr, &m_CommandPools[i]));
    }

    // Создаём command buffers
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_CommandPools[i];
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VK_CHECK_CRITICAL(vkAllocateCommandBuffers(VulkanHW.m_Device, &allocInfo, &m_CommandBuffers[i]));
    }

    // Second graphics segment per slot (async frame-split) — from the same per-slot
    // pool (RESET_COMMAND_BUFFER_BIT lets each buffer re-begin independently).
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_CommandPools[i];
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK_CRITICAL(vkAllocateCommandBuffers(VulkanHW.m_Device, &allocInfo, &m_PostBuffers[i]));
    }

    // Create dedicated immediate command pool+buffer for one-shot operations
    // (texture uploads, layout transitions) — separate from per-frame render buffers
    VK_CHECK_CRITICAL(vkCreateCommandPool(VulkanHW.m_Device, &poolInfo, nullptr, &m_ImmediatePool));

    {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_ImmediatePool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK_CRITICAL(vkAllocateCommandBuffers(VulkanHW.m_Device, &allocInfo, &m_ImmediateCmd));
    }

    {
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK_CRITICAL(vkCreateFence(VulkanHW.m_Device, &fenceInfo, nullptr, &m_ImmediateFence));
    }

    // --- Async uploader: transfer-family pool + cmd, timeline semaphore, staging ring ---
    {
        VkCommandPoolCreateInfo upi = {};
        upi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        upi.queueFamilyIndex = VulkanHW.m_TransferFamily;   // dedicated DMA family when present
        upi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK_CRITICAL(vkCreateCommandPool(VulkanHW.m_Device, &upi, nullptr, &m_UploadPool));

        VkCommandBufferAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = m_UploadPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = kUploadCmds;
        VK_CHECK_CRITICAL(vkAllocateCommandBuffers(VulkanHW.m_Device, &ai, m_UploadCmds));
        m_UploadCmd = m_UploadCmds[0];

        VkSemaphoreTypeCreateInfo tci = {};
        tci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        tci.initialValue = 0;
        VkSemaphoreCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &tci;
        VK_CHECK_CRITICAL(vkCreateSemaphore(VulkanHW.m_Device, &sci, nullptr, &m_UploadTimeline));
        m_UploadValue = 0;

        // Graphics frame timeline (async compute ordering) — same type, own counter.
        VK_CHECK_CRITICAL(vkCreateSemaphore(VulkanHW.m_Device, &sci, nullptr, &m_FrameTimeline));
        m_FrameValue = 0;

        m_StagingSize = (VkDeviceSize)64 * 1024 * 1024;   // 64 MB host-visible ring
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = m_StagingSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;       // CPU-written, transfer-read only
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        // SEQUENTIAL_WRITE asks VMA for the classic upload-staging memory: host
        // visible, not host cached, i.e. mapped WRITE-COMBINING. That is the textbook
        // choice — and measured here it fills at only ~2.1 GB/s even with streaming
        // stores, which turns 4.2 GB of level-load staging into two seconds of pure
        // memcpy. RANDOM asks for HOST_CACHED instead: the CPU writes at cache speed
        // and the transfer queue's DMA snoops. Which one actually wins is a property
        // of the machine, so it is a switch, decided by measurement (r_upload_cached).
        aci.flags = (ps_r_upload_cached ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                        : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo si = {};

        VK_CHECK_CRITICAL(VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &m_StagingBuf, &m_StagingAlloc, &si));
        m_StagingPtr  = (u8*)si.pMappedData;
        m_StagingHead = 0;
        if (m_StagingAlloc) vmaSetAllocationName(VulkanHW.m_Allocator, m_StagingAlloc, "buf:upload-ring");

        // WHERE the ring landed decides what a staging memcpy costs. VMA's
        // AUTO_PREFER_HOST is a preference, not a guarantee: on a ReBAR system it can
        // still pick DEVICE_LOCAL|HOST_VISIBLE, and then every byte the loader stages
        // crosses PCIe as an uncached write — measured at ~2 GB/s against 10+ for
        // plain system RAM, which is most of what a level load calls "upload".
        if (m_StagingAlloc) {
            VkMemoryPropertyFlags mp = 0;
            vmaGetAllocationMemoryProperties(VulkanHW.m_Allocator, m_StagingAlloc, &mp);
            Msg("[Vulkan] upload ring: %u MB, memory flags 0x%X (%s%s%s%s)", (u32)(m_StagingSize >> 20), mp,
                (mp & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)  ? "DEVICE_LOCAL " : "",
                (mp & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)  ? "HOST_VISIBLE " : "",
                (mp & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "",
                (mp & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)   ? "HOST_CACHED" : "");
        }

    }

    Msg("[Vulkan] Command pools and buffers created (3 frames in flight + 1 immediate + async uploader, transfer family %u)",
        VulkanHW.m_TransferFamily);
}

// Уничтожение
void CVulkanCommandManager::Destroy()
{
    VK::UploadCopyFarmShutdown();   // before the early-out: the helpers outlive a null device

    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;

    // Async uploader teardown (device already idle when Destroy runs).
    if (m_StagingBuf != VK_NULL_HANDLE) {
        VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, m_StagingBuf, m_StagingAlloc);
        m_StagingBuf = VK_NULL_HANDLE; m_StagingAlloc = VK_NULL_HANDLE; m_StagingPtr = nullptr;
    }
    if (m_FrameTimeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(VulkanHW.m_Device, m_FrameTimeline, nullptr);
        m_FrameTimeline = VK_NULL_HANDLE;
        m_FrameValue = 0;
    }
    if (m_UploadTimeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(VulkanHW.m_Device, m_UploadTimeline, nullptr);
        m_UploadTimeline = VK_NULL_HANDLE;
    }
    if (m_UploadPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(VulkanHW.m_Device, m_UploadPool, nullptr);
        m_UploadPool = VK_NULL_HANDLE; m_UploadCmd = VK_NULL_HANDLE;
        for (u32 i = 0; i < kUploadCmds; ++i) { m_UploadCmds[i] = VK_NULL_HANDLE; m_UploadCmdValue[i] = 0; }
    }

    DestroyImmSlots();
    if (m_ImmediateFence != VK_NULL_HANDLE) {
        vkDestroyFence(VulkanHW.m_Device, m_ImmediateFence, nullptr);
        m_ImmediateFence = VK_NULL_HANDLE;
    }

    if (m_ImmediatePool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(VulkanHW.m_Device, m_ImmediatePool, nullptr);
        m_ImmediatePool = VK_NULL_HANDLE;
    }

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (m_CommandPools[i] != VK_NULL_HANDLE) {
            vkDestroyCommandPool(VulkanHW.m_Device, m_CommandPools[i], nullptr);
            m_CommandPools[i] = VK_NULL_HANDLE;
        }
    }

    Msg("[Vulkan] Command pools destroyed");
}

// Начало записи команд — returns VK_NULL_HANDLE on error
VkCommandBuffer CVulkanCommandManager::Begin()
{
    if (g_bDeviceLost) return VK_NULL_HANDLE;

    VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult res = vkBeginCommandBuffer(cmd, &beginInfo);
    if (res != VK_SUCCESS) {
        Msg("!Vulkan vkBeginCommandBuffer error: %d", res);
        if (res == VK_ERROR_DEVICE_LOST && !g_bDeviceLost) {
            Msg("!Vulkan DEVICE LOST in Begin()");
            g_bDeviceLost = true;
        }
        return VK_NULL_HANDLE;
    }

    return cmd;
}

// Завершение записи команд — returns false on error
bool CVulkanCommandManager::End(VkCommandBuffer cmd)
{
    if (g_bDeviceLost || cmd == VK_NULL_HANDLE) return false;

    VkResult res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS) {
        Msg("!Vulkan vkEndCommandBuffer error: %d", res);
        if (res == VK_ERROR_DEVICE_LOST && !g_bDeviceLost) {
            Msg("!Vulkan DEVICE LOST in End()");
            g_bDeviceLost = true;
        }
        return false;
    }
    return true;
}

// Begin the frame's second graphics segment (async frame-split).
VkCommandBuffer CVulkanCommandManager::BeginSecondSegment()
{
    if (g_bDeviceLost) return VK_NULL_HANDLE;
    VkCommandBuffer cmd = m_PostBuffers[m_CurrentFrame];

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult res = vkBeginCommandBuffer(cmd, &beginInfo);
    if (res != VK_SUCCESS) {
        Msg("!Vulkan BeginSecondSegment error: %d", res);
        if (res == VK_ERROR_DEVICE_LOST && !g_bDeviceLost) { Msg("!Vulkan DEVICE LOST in BeginSecondSegment()"); g_bDeviceLost = true; }
        return VK_NULL_HANDLE;
    }
    return cmd;
}

// Submit команд в очередь — returns false on error
bool CVulkanCommandManager::Submit(VkCommandBuffer cmd, VkSemaphore waitSemaphore,
                                   VkSemaphore signalSemaphore, VkFence fence,
                                   bool waitAsyncCompute)
{
    if (g_bDeviceLost || cmd == VK_NULL_HANDLE) return false;

    // Build the wait list: imageAvailable (binary) plus — when async uploads are
    // pending — the upload timeline, so the GPU defers draws that consume just-
    // uploaded vertex/index/texture data until the transfer-queue copy completes.
    VkSemaphore          waitSems[3];
    VkPipelineStageFlags waitStages[3];
    u64                  waitVals[3];
    u32 nWait = 0;
    if (waitSemaphore != VK_NULL_HANDLE) {
        waitSems[nWait]   = waitSemaphore;
        waitStages[nWait] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        waitVals[nWait]   = 0;   // binary — value ignored
        nWait++;
    }
    // Snapshot the upload timeline under the upload lock: a worker thread may be
    // mid-FlushUploads (++m_UploadValue then transfer submit). Capturing both under
    // the lock yields a value whose copy has already been submitted, so this
    // graphics frame waits on a consistent, in-flight upload point.
    VkSemaphore uploadSem;
    u64         uploadVal;
    {
        std::lock_guard<std::recursive_mutex> lock(m_UploadMutex);
        uploadSem = m_UploadTimeline;
        uploadVal = m_UploadValue;
    }
    const bool waitUpload = (uploadSem != VK_NULL_HANDLE && uploadVal > 0);
    if (waitUpload) {
        waitSems[nWait]   = uploadSem;
        waitStages[nWait] = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                          | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        waitVals[nWait]   = uploadVal;
        nWait++;
    }

    // Async compute (vk_async): when compute work was submitted to the compute queue
    // this frame, the graphics work must wait on its timeline before consuming any
    // compute output. Broad dst stages (indirect/vertex/compute/fragment) so any
    // future consumer is covered. Mirrors the upload-timeline wait above.
    VkSemaphore asyncSem = VK_NULL_HANDLE;
    u64         asyncVal = 0;
    const bool  waitAsync = waitAsyncCompute && VK::Async::GetGraphicsWait(asyncSem, asyncVal);
    if (waitAsync) {
        waitSems[nWait]   = asyncSem;
        waitStages[nWait] = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
                          | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        waitVals[nWait]   = asyncVal;
        nWait++;
    }

    // Signal list: the caller's binary renderFinished (when given) PLUS the frame
    // timeline, bumped by every graphics submit. The async compute submit waits on
    // the value read at frame start, which is how it stays behind the previous
    // frame's reads of the volumes it overwrites.
    VkSemaphore signalSems[2];
    u64         signalVals[2];
    u32 nSignal = 0;
    if (signalSemaphore != VK_NULL_HANDLE) {
        signalSems[nSignal] = signalSemaphore;
        signalVals[nSignal] = 0;              // binary — value ignored
        nSignal++;
    }
    if (m_FrameTimeline != VK_NULL_HANDLE) {
        signalSems[nSignal] = m_FrameTimeline;
        signalVals[nSignal] = ++m_FrameValue;
        nSignal++;
    }

    const bool useTimeline = waitUpload || waitAsync || m_FrameTimeline != VK_NULL_HANDLE;
    VkTimelineSemaphoreSubmitInfo tl = {};
    tl.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tl.waitSemaphoreValueCount   = nWait;
    tl.pWaitSemaphoreValues      = waitVals;
    tl.signalSemaphoreValueCount = nSignal;
    tl.pSignalSemaphoreValues    = signalVals;

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = useTimeline ? &tl : nullptr;   // only need timeline values when a timeline is in play
    submitInfo.waitSemaphoreCount = nWait;
    submitInfo.pWaitSemaphores = waitSems;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = nSignal;
    submitInfo.pSignalSemaphores = signalSems;

    VkResult res;
    {
        LoadProf::Scope _lp("rt.submit"); // with the queue lock's wait
        std::lock_guard<std::mutex> queueLock(g_VkQueueMx);   // RENDER_THREAD_PLAN step 10: one lock for every queue submit/present/wait
        res = vkQueueSubmit(VulkanHW.m_GraphicsQueue, 1, &submitInfo, fence);
    }
    if (res != VK_SUCCESS) {
        Msg("!Vulkan vkQueueSubmit error: %d", res);
        if (res == VK_ERROR_DEVICE_LOST) {
            VK::Prof::DumpCheckpoints("Submit");
            if (!g_bDeviceLost) {
                Msg("!Vulkan DEVICE LOST in Submit()");
                g_bDeviceLost = true;
            }
        }
        return false;
    }
    return true;
}

// ============================================================================
// Immediate (one-shot) command buffer — safe to call during rendering.
// Uses a dedicated pool+buffer that never conflicts with per-frame render buffers.
// ============================================================================
// RENDER_THREAD_PLAN step 10: every THREAD gets its own immediate pool + command buffer + fence. The single shared
// m_ImmediateCmd/m_ImmediateFence could not be used from the loader/spawn worker and the render thread at once (two
// threads recording one VkCommandBuffer). The queue submit itself is serialised by g_VkQueueMx.
namespace {
struct ImmSlot { VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; };
std::mutex            g_immMx;
std::vector<ImmSlot*> g_immSlots;          // never freed individually (small); handles are destroyed in DestroyImmSlots
thread_local ImmSlot* tl_imm = nullptr;

ImmSlot* GetImmSlot()
{
    ImmSlot* s = tl_imm;
    if (!s) {
        s = new ImmSlot();
        { std::lock_guard<std::mutex> g(g_immMx); g_immSlots.push_back(s); }
        tl_imm = s;
    }
    if (s->pool == VK_NULL_HANDLE) {   // first use, or the device was recreated
        VkCommandPoolCreateInfo pi = {};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = VulkanHW.m_GraphicsFamily;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(VulkanHW.m_Device, &pi, nullptr, &s->pool) != VK_SUCCESS) { s->pool = VK_NULL_HANDLE; return nullptr; }
        VkCommandBufferAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = s->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkFenceCreateInfo fi = {}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkAllocateCommandBuffers(VulkanHW.m_Device, &ai, &s->cmd) != VK_SUCCESS ||
            vkCreateFence(VulkanHW.m_Device, &fi, nullptr, &s->fence) != VK_SUCCESS) {
            vkDestroyCommandPool(VulkanHW.m_Device, s->pool, nullptr);
            s->pool = VK_NULL_HANDLE; s->cmd = VK_NULL_HANDLE; s->fence = VK_NULL_HANDLE; return nullptr;
        }
    }
    return s;
}

void DestroyImmSlots()
{
    std::lock_guard<std::mutex> g(g_immMx);
    for (ImmSlot* s : g_immSlots) {
        if (s->fence != VK_NULL_HANDLE) vkDestroyFence(VulkanHW.m_Device, s->fence, nullptr);
        if (s->pool  != VK_NULL_HANDLE) vkDestroyCommandPool(VulkanHW.m_Device, s->pool, nullptr);   // frees its buffer too
        s->fence = VK_NULL_HANDLE; s->pool = VK_NULL_HANDLE; s->cmd = VK_NULL_HANDLE;
    }
}
}  // namespace

VkCommandBuffer CVulkanCommandManager::BeginImmediate()
{
    if (g_bDeviceLost) return VK_NULL_HANDLE;
    ImmSlot* slot = GetImmSlot();
    if (!slot || slot->cmd == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    // Reset this thread's immediate command buffer before reuse
    vkResetCommandBuffer(slot->cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult res = vkBeginCommandBuffer(slot->cmd, &beginInfo);
    if (res != VK_SUCCESS) {
        Msg("!Vulkan BeginImmediate error: %d", res);
        return VK_NULL_HANDLE;
    }

    VK::Prof::Checkpoint(slot->cmd, "Immediate");
    return slot->cmd;
}

void CVulkanCommandManager::EndAndSubmitImmediate(VkCommandBuffer cmd)
{
    if (g_bDeviceLost || cmd == VK_NULL_HANDLE) return;
    LoadProf::Scope _lp("rt.immediate"); // a one-off submit + a wait for the GPU to finish it
    ImmSlot* slot = tl_imm;   // Begin and End run on the same thread
    if (!slot || slot->fence == VK_NULL_HANDLE) return;

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkResetFences(VulkanHW.m_Device, 1, &slot->fence);
    VkResult res;
    {
        std::lock_guard<std::mutex> queueLock(g_VkQueueMx);   // RENDER_THREAD_PLAN step 10: one lock for every queue submit/present/wait
        res = vkQueueSubmit(VulkanHW.m_GraphicsQueue, 1, &submitInfo, slot->fence);
    }
    if (res != VK_SUCCESS) {
        Msg("!Vulkan EndAndSubmitImmediate: submit error %d", res);
        if (res == VK_ERROR_DEVICE_LOST) VK::Prof::DumpCheckpoints("EndAndSubmitImmediate");
        return;
    }

    // A timeout here means the graphics queue is wedged -- the next BeginImmediate
    // would reset this buffer while the GPU may still own it. Log + post-mortem
    // (this is the classic pre-TDR window of the level-transition hang).
    res = vkWaitForFences(VulkanHW.m_Device, 1, &slot->fence, VK_TRUE, 2000000000ULL);
    if (res != VK_SUCCESS) {
        Msg("!Vulkan EndAndSubmitImmediate: fence wait %s (%d)", res == VK_TIMEOUT ? "TIMEOUT" : "error", res);
        VK::Prof::DumpCheckpoints("ImmediateFenceWait");
    }
}

// ============================================================================
// Async buffer uploader — transfer queue + staging ring + timeline semaphore.
// ============================================================================
void CVulkanCommandManager::EnsureUploadCmdOpen()
{
    if (m_UploadOpen) return;

    // "Effectively free" (below) holds per frame, but a level load stages 4 GB
    // through here and every reopen after a flush blocks on the transfer queue —
    // time that lands in the caller's "upload" bucket without being visible in it.
    // Measured, not assumed: see the upload split line.
    CTimer _tReopen; _tReopen.Start();
    struct _reopenScope {
        CTimer& t;
        ~_reopenScope() { VK::UploadProf::s_reopenWaitMs += t.GetElapsed_ms_total(); ++VK::UploadProf::s_reopens; }
    } _rs{ _tReopen };


    // Rotate to the next buffer -- the one that has had the most time to finish --
    // and wait only for ITS submission. Waiting for the newest value instead (what
    // a single reused buffer forced) is a full drain: during a level load it cost
    // 0.75 ms per reopen and ate exactly what finer ring segments had just saved.
    m_UploadCmdCur = (m_UploadCmdCur + 1) % kUploadCmds;
    m_UploadCmd    = m_UploadCmds[m_UploadCmdCur];
    const u64 mine = m_UploadCmdValue[m_UploadCmdCur];

    if (m_UploadTimeline != VK_NULL_HANDLE && mine > 0) {
        VkSemaphoreWaitInfo wi = {};
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1;
        wi.pSemaphores    = &m_UploadTimeline;
        wi.pValues        = &mine;
        VkResult wr = vkWaitSemaphores(VulkanHW.m_Device, &wi, 2000000000ULL);
        if (wr != VK_SUCCESS) {
            // Resetting m_UploadCmd below while the transfer queue still owns it is
            // UB that cascades into a device loss — make the real culprit visible.
            Msg("!Vulkan EnsureUploadCmdOpen: upload timeline wait %s (%d, value %llu)",
                wr == VK_TIMEOUT ? "TIMEOUT" : "error", wr, (unsigned long long)mine);
            VK::Prof::DumpCheckpoints("UploadTimelineWait");
        }
    }

    vkResetCommandBuffer(m_UploadCmd, 0);
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_UploadCmd, &bi);
    m_UploadOpen = true;
}

// ---------------------------------------------------------------------------
// Upload accounting. With the level-texture reads moved off the loader thread
// (VK::TexPrefetch), `upload` became the largest single item left in the texture
// phase — and it is three unrelated costs in one number: the ring-wrap drain
// (CPU blocked on the GPU), the host memcpy into the ring, and command recording.
// Only the first is a structural problem, so split them before touching anything.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Copy into the upload ring.
//
// The ring is HOST_VISIBLE|HOST_COHERENT and NOT HOST_CACHED — i.e. mapped
// write-combining. The CRT memcpy (ERMSB `rep movsb`) is tuned for cached
// destinations and measured 1.97 GB/s into this buffer, which made the staging
// copy 2150 of the load's ~2500 ms of "upload". Streaming stores in whole
// 64-byte groups keep every write-combine buffer full, which is the entire
// difference. SSE2 (not AVX) on purpose: it is baseline on x64, so no CPU
// feature test and no /arch: change.
//
// The ring hands out 64-byte-aligned offsets (StageBytes) so `dst` is aligned for
// MOVNTDQ and each group covers exactly one WC buffer.
// ---------------------------------------------------------------------------


namespace {

// Портируемые обёртки над _mm_pause (спин-хинт, PAUSE) и _mm_sfence (барьер
// публикации non-temporal записей StreamBlock). На MSVC -- буквально те же
// интринсики; вне x86 PAUSE не существует, ARM-аналог -- инструкция YIELD
// (тот же хинт: не занимать конвейер, ничего не гарантирует и не обязателен
// для корректности). store_fence на портируемой ветке -- честный
// последовательно согласованный барьер: раз StreamBlock там всегда обычный
// memcpy (см. ниже), non-temporal-специфичный sfence не нужен по смыслу, но
// барьер оставлен, чтобы не менять модель публикации между потоками.
inline void cpu_relax()
{
#ifdef _MSC_VER
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

inline void store_fence()
{
#ifdef _MSC_VER
    _mm_sfence();
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

// One thread's share of the copy. No fence here -- the fence belongs to whoever
// finishes a whole piece (see CWorkFarm::Drain / RingCopy).
void StreamBlock(u8* dst, const u8* src, size_t n)
{
    if (!ps_r_upload_wc_copy) { memcpy(dst, src, n); return; }

#ifdef _MSC_VER
    size_t i = 0;
    if ((((uintptr_t)dst) & 15) == 0) {
        for (; i + 64 <= n; i += 64) {
            const __m128i a = _mm_loadu_si128((const __m128i*)(src + i +  0));
            const __m128i b = _mm_loadu_si128((const __m128i*)(src + i + 16));
            const __m128i c = _mm_loadu_si128((const __m128i*)(src + i + 32));
            const __m128i d = _mm_loadu_si128((const __m128i*)(src + i + 48));
            _mm_stream_si128((__m128i*)(dst + i +  0), a);
            _mm_stream_si128((__m128i*)(dst + i + 16), b);
            _mm_stream_si128((__m128i*)(dst + i + 32), c);
            _mm_stream_si128((__m128i*)(dst + i + 48), d);
        }
    }
    if (i < n) memcpy(dst + i, src + i, n - i);
#else
    // Портируемая ветка: тот же путь, что и `ps_r_upload_wc_copy 0` на MSVC --
    // это уже проверенный, рабочий код в ЭТОЙ ЖЕ функции, а не догадка. Сама
    // не-временная (WC-дружелюбная) запись -- чистая оптимизация записи в
    // незакэшированное кольцо, не наблюдаемое поведение; у ARM есть свои
    // аналоги (DC ZVA/non-temporal store), но заводить их ради перфоманса
    // сюда не входит -- это шов портируемости, не бенчмарк.
    memcpy(dst, src, n);
#endif
}

// ---------------------------------------------------------------------------
// Helpers for the big copies (r_upload_copy_threads).
//
// level.geom pushes 1869 MB through this ring in ~1 MB pieces and measures 2.8
// GB/s doing it, against 5.7 GB/s for the same ring fed from hot heap bytes: one
// core reading cold page-cache pages while writing write-combined memory is the
// ceiling, and it is not the ring's. The pieces are independent, so split each
// one across a few helpers and leave everything else -- ring offsets, segment
// waits, command recording -- on the calling thread, which is the part with the
// history of races (see the recursive mutex in the header).
//
// Helpers spin before sleeping: during a load the next piece arrives a few
// microseconds later and a futex round trip per piece would eat the win. They
// idle on a condition variable afterwards, so a pool left alive costs nothing.
//
// The payload is a chunk INDEX and a body, not a (dst, src) pair: the same pool
// serves the ring copy and the DDS repack, and a load that spawns a fresh set of
// threads per texture pays more in thread creation than the split ever wins
// back (561 repacks x ~1 ms). VK::FarmRun (vk_parallel.h) is the door in.
// ---------------------------------------------------------------------------
class CWorkFarm
{
public:
    using Body = std::function<void(u32)>;

    ~CWorkFarm() { Shutdown(); }

    // Runs body(i) for i in [0, chunks) on up to `helpers` threads plus the
    // caller, and returns when every chunk has finished.
    void Run(u32 chunks, u32 helpers, const Body& body);
    void Shutdown();

private:
    static constexpr u32 kSpin = 2000;   // pause iterations before a helper sleeps

    void Drain(const Body& body, u32 chunks);
    void WorkerLoop();

    // One job at a time, whatever thread asks. The chunk cursor and the job
    // descriptor are single-slot, so two callers would deal each other's chunks
    // -- the texture streamer can promote an image from its own thread while the
    // loader is filling the ring.
    std::mutex              m_caller;

    std::mutex              m_mx;
    std::condition_variable m_cvJob, m_cvIdle;
    xr_vector<std::thread>  m_pool;
    std::atomic<bool>       m_quit{false};

    // Job descriptor: written AND read under m_mx -- a helper that wakes late must
    // never see half of one job and half of the next.
    const Body*      m_body   = nullptr;
    u32              m_chunks = 0;
    std::atomic<u32> m_gen{0};      // one bump per job
    std::atomic<u32> m_next{0};     // next chunk to claim
    std::atomic<u32> m_done{0};     // chunks finished
    std::atomic<u32> m_active{0};   // helpers inside a job -- the next job waits for zero
};

void CWorkFarm::Drain(const Body& body, u32 chunks)
{
    for (;;) {
        const u32 i = m_next.fetch_add(1, std::memory_order_relaxed);
        if (i >= chunks) break;
        body(i);
        m_done.fetch_add(1, std::memory_order_release);
    }
}

void CWorkFarm::WorkerLoop()
{
    set_current_thread_name("VK WorkFarm");
    u32 seen = 0;
    for (;;) {
        const Body* body; u32 chunks;
        {
            for (u32 s = 0; s < kSpin && m_gen.load(std::memory_order_acquire) == seen
                                      && !m_quit.load(std::memory_order_relaxed); ++s)
                cpu_relax();

            std::unique_lock<std::mutex> lk(m_mx);
            m_cvJob.wait(lk, [&] { return m_quit.load(std::memory_order_relaxed)
                                       || m_gen.load(std::memory_order_relaxed) != seen; });
            if (m_quit.load(std::memory_order_relaxed)) return;
            seen   = m_gen.load(std::memory_order_relaxed);
            body   = m_body; chunks = m_chunks;
            m_active.fetch_add(1, std::memory_order_relaxed);
        }
        Drain(*body, chunks);
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (m_active.fetch_sub(1, std::memory_order_acq_rel) == 1) m_cvIdle.notify_one();
        }
    }
}

void CWorkFarm::Run(u32 chunks, u32 helpers, const Body& body)
{
    if (chunks == 0) return;
    if (helpers == 0 || chunks == 1) {
        for (u32 i = 0; i < chunks; ++i) body(i);
        return;
    }

    std::lock_guard<std::mutex> one(m_caller);
    {
        std::unique_lock<std::mutex> lk(m_mx);
        while (m_pool.size() < helpers) m_pool.emplace_back([this] { WorkerLoop(); });
        // Nobody may still be inside the previous job: a helper claiming late would
        // take one of THIS job's chunk indices with the previous job's body.
        m_cvIdle.wait(lk, [this] { return m_active.load(std::memory_order_relaxed) == 0; });
        m_body = &body; m_chunks = chunks;
        m_next.store(0, std::memory_order_relaxed);
        m_done.store(0, std::memory_order_relaxed);
        m_gen.fetch_add(1, std::memory_order_release);
    }
    m_cvJob.notify_all();

    Drain(body, chunks);
    // Return only when every chunk is finished -- `body` lives on the caller's
    // stack and the helpers are still holding a pointer to it.
    while (m_done.load(std::memory_order_acquire) < chunks) cpu_relax();
}

void CWorkFarm::Shutdown()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_quit.store(true, std::memory_order_relaxed);
    }
    m_cvJob.notify_all();
    // A joinable std::thread left over at process exit calls std::terminate.
    for (auto& th : m_pool) if (th.joinable()) th.join();
    m_pool.clear();
}

CWorkFarm s_workFarm;

constexpr size_t kCopyGrain    = 64 * 1024;    // claim unit; keeps every dst 64-byte aligned
constexpr size_t kCopyMinSplit = 256 * 1024;   // below this the handshake costs more than the copy

void RingCopy(u8* dst, const u8* src, size_t n)
{
    u32 helpers = ps_r_upload_copy_threads < 0 ? 0u : (u32)ps_r_upload_copy_threads;
    if (helpers > 15) helpers = 15;
    // ⚠ Do NOT drop the split for a caller that is itself one of many threads (the
    // r_tex_materialize workers looked like an obvious case). This copy is bound by
    // page faults on the SOURCE, not by the store, so the split is what makes it
    // fast: measured 18.5 GB/s with four helpers, 6.4 with them fighting sixteen
    // busy workers for cores, and 3.9 GB/s single-threaded.

    if (helpers == 0 || n < kCopyMinSplit) { StreamBlock(dst, src, n); store_fence(); return; }

    const u32 chunks = u32((n + kCopyGrain - 1) / kCopyGrain);
    s_workFarm.Run(chunks, helpers, [dst, src, n](u32 i) {
        const size_t off = size_t(i) * kCopyGrain;
        StreamBlock(dst + off, src + off, _min(kCopyGrain, n - off));
        // Streaming stores are weakly ordered and a release store does NOT order
        // them: fence before publishing the chunk, not once at the end.
        store_fence();
    });
}
}   // anonymous namespace

// The pool, for anything else in the load that splits into independent chunks
// (see vk_parallel.h). Same contract as above: returns when every chunk is done.
namespace VK {
void FarmRun(u32 chunks, u32 helpers, const std::function<void(u32)>& body)
{
    s_workFarm.Run(chunks, helpers, body);
}
}   // namespace VK

// Joined from CVulkanCommandManager::Destroy so the helpers die with the device.
namespace VK { void UploadCopyFarmShutdown() { s_workFarm.Shutdown(); } }

namespace VK { namespace UploadProf {

float s_wrapWaitMs = 0.f, s_copyMs = 0.f, s_recordMs = 0.f, s_reopenWaitMs = 0.f;
// End+submit of the accumulated batch. Called on every segment entry (that is the
// point -- see upload-ring-is-pcie-bound), so a load pays it a few hundred times
// and it was the last untimed thing between "upload" and the sum of its parts.
float s_flushMs = 0.f;
u32   s_flushes = 0;
u32   s_wraps = 0, s_stagings = 0, s_reopens = 0, s_segSubmits = 0;
u32   s_segEnters = 0, s_waits = 0, s_bigStagings = 0;   // where the ring wait actually goes
u64   s_bigBytes = 0;
u64   s_copyBytes = 0;

void Dump()
{
    Msg("[load step]   upload split: %u stagings (%llu MB) = ring wait %.0f ms (%u rewinds, %u segment submits) + cmd-reopen wait %.0f ms (%u reopens) + host memcpy %.0f ms (%.0f MB/s) + record %.0f ms",
        s_stagings, (unsigned long long)(s_copyBytes >> 20), s_wrapWaitMs, s_wraps, s_segSubmits, s_reopenWaitMs, s_reopens, s_copyMs,
        s_copyMs > 0.f ? (double)(s_copyBytes >> 20) * 1000.0 / s_copyMs : 0.0, s_recordMs);
    Msg("[load step]   upload ring: %u segment enters, %u blocking waits, %u stagings >= 1 segment (%llu MB of %llu) | end+submit %.0f ms (%u batches)",
        s_segEnters, s_waits, s_bigStagings, (unsigned long long)(s_bigBytes >> 20), (unsigned long long)(s_copyBytes >> 20),
        s_flushMs, s_flushes);
    s_wrapWaitMs = s_copyMs = s_recordMs = s_reopenWaitMs = 0.f;
    s_wraps = s_stagings = s_reopens = s_segSubmits = 0;
    s_segEnters = s_waits = s_bigStagings = 0;
    s_bigBytes = 0;
    s_copyBytes = 0;
    s_flushMs = 0.f; s_flushes = 0;
}
}}   // namespace VK::UploadProf

VkDeviceSize CVulkanCommandManager::StageBytes(const void* data, VkDeviceSize size)
{
    if (size > m_StagingSize) return UINT64_MAX;            // caller handles oversize

    // Align the ring offset to 16 bytes. vkCmdCopyBufferToImage requires bufferOffset
    // be a multiple of the texel block size (16 for BC3/BC5/BC7, 8 for BC1, 4 for
    // BGRA8) AND — on a transfer-only queue — a multiple of 4. A 16-aligned base
    // keeps every tightly-packed mip offset (base + k*blockSize) compliant. Buffers
    // don't need it but the ≤15-byte waste is irrelevant.
    // 64, not 16: the ≤63-byte waste buys a destination aligned to a whole
    // write-combine buffer for RingCopy's streaming stores. Every texel block size
    // in play (4/8/16) divides 64, so the copy-region offsets stay legal.
    VkDeviceSize off = (m_StagingHead + 63) & ~VkDeviceSize(63);

    if (off + size > m_StagingSize) {       // rewind to base (0, already aligned)
        off = 0;
        ++VK::UploadProf::s_wraps;
    }

    // Reusing bytes means waiting for whoever read them last -- but ONLY when the
    // head ENTERS a segment. Appending inside the segment we already own touches
    // fresh bytes and needs no wait at all; checking per staging instead of per
    // crossing submitted and drained on every single upload (measured: 3424
    // reopens, ring wait 388 -> 1087 ms). See kStagingSegments in the header.
    {
        const VkDeviceSize segSize = m_StagingSize / kStagingSegments;
        const u32 firstSeg = (u32)(off / segSize);
        const u32 lastSeg  = (u32)_min((VkDeviceSize)(kStagingSegments - 1), (off + size - 1) / segSize);

        // Submit whenever this staging ENTERS a new segment -- including the case
        // where it merely spills over the boundary (firstSeg == m_SegCur but
        // lastSeg beyond it). Keying on firstSeg alone missed those: measured 533
        // segment entries against only 105 submits, i.e. four out of five entries
        // took the GPU nothing new to chew on.
        if (firstSeg != m_SegCur || lastSeg != m_SegCur) {
            FlushUploads();
            ++VK::UploadProf::s_segSubmits;
        }

        if (lastSeg != firstSeg) { ++VK::UploadProf::s_bigStagings; VK::UploadProf::s_bigBytes += (u64)size; }
        for (u32 sg = firstSeg; sg <= lastSeg; ++sg) {
            if (sg == m_SegCur) continue;            // already ours on this pass round the ring
            ++VK::UploadProf::s_segEnters;
            const u64 v = m_SegValue[sg];
            if (v) {
                if (v > m_UploadValue) FlushUploads();   // its batch was still open
                if (v <= m_UploadValue && m_UploadTimeline != VK_NULL_HANDLE) {
                    CTimer _t; _t.Start();
                    VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
                    wi.semaphoreCount = 1;
                    wi.pSemaphores    = &m_UploadTimeline;
                    wi.pValues        = &v;
                    vkWaitSemaphores(VulkanHW.m_Device, &wi, 2000000000ULL);
                    const float w = _t.GetElapsed_ms_total();
                    VK::UploadProf::s_wrapWaitMs += w;
                    if (w > 0.05f) ++VK::UploadProf::s_waits;
                }
            }
        }
        m_SegCur = lastSeg;
        EnsureUploadCmdOpen();

        // Mark on EVERY staging, not just on entry: an append can land in a later
        // batch than the one that entered the segment (any FlushUploads in between
        // opens a new one), and the next pass round the ring must wait for the
        // NEWEST batch that reads these bytes, not the first.
        for (u32 sg = firstSeg; sg <= lastSeg; ++sg) m_SegValue[sg] = m_UploadValue + 1;
    }
    CTimer _tc; _tc.Start();
    RingCopy(m_StagingPtr + off, (const u8*)data, (size_t)size);
    vmaFlushAllocation(VulkanHW.m_Allocator, m_StagingAlloc, off, size);  // no-op if HOST_COHERENT

    VK::UploadProf::s_copyMs += _tc.GetElapsed_ms_total();
    VK::UploadProf::s_copyBytes += (u64)size;
    ++VK::UploadProf::s_stagings;
    m_StagingHead = off + size;
    return off;
}


void CVulkanCommandManager::UploadBuffer(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size)
{
    VK::Vram::Scope _vram_scope("Staging");
    std::lock_guard<std::recursive_mutex> lock(m_UploadMutex);   // shared upload cmd/ring — see header
    if (g_bDeviceLost || dst == VK_NULL_HANDLE || !data || size == 0 || !m_StagingPtr) return;

    // Pathological oversize (> whole ring): one-off temp staging, synchronous on the
    // transfer queue. Never happens for real vertex/index buffers; defensive only.
    if (size > m_StagingSize) {
        Msg("![Vulkan] UploadBuffer: %zu bytes exceeds %zu-byte ring — one-off staging", (size_t)size, (size_t)m_StagingSize);
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer tmp; VmaAllocation tmpAlloc; VmaAllocationInfo ti = {};
        if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &tmp, &tmpAlloc, &ti) != VK_SUCCESS) return;
        memcpy(ti.pMappedData, data, (size_t)size);
        vmaFlushAllocation(VulkanHW.m_Allocator, tmpAlloc, 0, size);
        FlushUploadsAndWait();              // drain the ring batch first (ordering)
        EnsureUploadCmdOpen();
        VkBufferCopy r = { 0, dstOffset, size };
        vkCmdCopyBuffer(m_UploadCmd, tmp, dst, 1, &r);
        FlushUploadsAndWait();              // submit + wait so tmp is safe to free
        VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, tmp, tmpAlloc);
        return;
    }

    const VkDeviceSize off = StageBytes(data, size);

    VK::Prof::Checkpoint(m_UploadCmd, "Upload:buffer");
    VkBufferCopy region = {};
    region.srcOffset = off;
    region.dstOffset = dstOffset;
    region.size      = size;
    vkCmdCopyBuffer(m_UploadCmd, m_StagingBuf, dst, 1, &region);
}

void CVulkanCommandManager::UploadImage(VkImage dst, const void* data, VkDeviceSize size,
                                        const VkBufferImageCopy* regions, u32 regionCount,
                                        u32 mipLevels, u32 arrayLayers)
{
    VK::Vram::Scope _vram_scope("Staging");
    std::lock_guard<std::recursive_mutex> lock(m_UploadMutex);   // shared upload cmd/ring — see header
    if (g_bDeviceLost || dst == VK_NULL_HANDLE || !data || size == 0 || !regions || regionCount == 0 || !m_StagingPtr)
        return;

    // Oversize (> whole ring): one-off temp staging + synchronous submit, mirroring
    // UploadBuffer's oversize path. This DOES happen — a 4096² UNCOMPRESSED mod
    // texture is 89 MB vs the 64 MB ring; the old "skip" left the VkImage with
    // undefined (black) texels → the terrain "black grass channel" bug.
    VkBuffer      tmpBuf   = VK_NULL_HANDLE;
    VmaAllocation tmpAlloc = VK_NULL_HANDLE;
    VkDeviceSize  off      = 0;
    if (size > m_StagingSize) {
        Msg("![Vulkan] UploadImage: %zu bytes exceeds %zu-byte staging ring — one-off staging", (size_t)size, (size_t)m_StagingSize);
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo ti = {};
        if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &tmpBuf, &tmpAlloc, &ti) != VK_SUCCESS) {
            Msg("![Vulkan] UploadImage: one-off staging alloc FAILED — texture skipped");
            return;
        }
        memcpy(ti.pMappedData, data, (size_t)size);
        vmaFlushAllocation(VulkanHW.m_Allocator, tmpAlloc, 0, size);
        FlushUploadsAndWait();              // drain the ring batch first (ordering)
        EnsureUploadCmdOpen();
    } else {
        off = StageBytes(data, size);       // opens cmd, rebases below
    }

    CTimer _tRec; _tRec.Start();

    // Rebase the caller's copy regions (offsets are relative to `data`) into the
    // ring (one-off buffer keeps them as-is: off = 0).
    xr_vector<VkBufferImageCopy> r(regions, regions + regionCount);

    for (auto& reg : r) reg.bufferOffset += off;

    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = mipLevels;
    range.layerCount = arrayLayers;

    auto barrier = [&](VkImageLayout oldL, VkImageLayout newL,
                       VkPipelineStageFlags2 srcS, VkAccessFlags2 srcA,
                       VkPipelineStageFlags2 dstS, VkAccessFlags2 dstA) {
        VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = srcS; b.srcAccessMask = srcA;
        b.dstStageMask = dstS; b.dstAccessMask = dstA;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;   // CONCURRENT image → no ownership transfer
        b.image = dst; b.subresourceRange = range;
        VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(m_UploadCmd, &di);
    };

    // UNDEFINED → TRANSFER_DST, copy, → SHADER_READ_ONLY. The final transition uses
    // dstStage = NONE: a transfer-only queue can't name FRAGMENT_SHADER. Visibility to
    // the graphics sampler comes from the upload timeline (graphics waits at
    // FRAGMENT_SHADER) — valid because the image is CONCURRENT (no ownership transfer).
    // NOTE: this is why this barrier is hand-rolled and NOT VK::ImageBarrier — the
    // latter derives FRAGMENT_SHADER for SHADER_READ, which is illegal on this queue.
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_NONE, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VK::Prof::Checkpoint(m_UploadCmd, tmpBuf ? "Upload:image1off" : "Upload:image");
    vkCmdCopyBufferToImage(m_UploadCmd, tmpBuf ? tmpBuf : m_StagingBuf, dst,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regionCount, r.data());

    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE, 0);

    if (tmpBuf) {
        FlushUploadsAndWait();              // submit + wait so the one-off staging is safe to free
        VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, tmpBuf, tmpAlloc);
    }
    VK::UploadProf::s_recordMs += _tRec.GetElapsed_ms_total();
}


void CVulkanCommandManager::FlushUploads()
{
    std::lock_guard<std::recursive_mutex> lock(m_UploadMutex);   // shared upload cmd/ring — see header
    if (g_bDeviceLost || !m_UploadOpen) return;
    CTimer _tFlush; _tFlush.Start();
    struct _flushScope {
        CTimer& t;
        ~_flushScope() { VK::UploadProf::s_flushMs += t.GetElapsed_ms_total(); ++VK::UploadProf::s_flushes; }
    } _fs{ _tFlush };
    vkEndCommandBuffer(m_UploadCmd);
    m_UploadOpen = false;

    const u64 signalVal = ++m_UploadValue;

    VkTimelineSemaphoreSubmitInfo tl = {};
    tl.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tl.signalSemaphoreValueCount = 1;
    tl.pSignalSemaphoreValues    = &signalVal;

    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &tl;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &m_UploadCmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &m_UploadTimeline;

    m_UploadCmdValue[m_UploadCmdCur] = signalVal;   // this buffer is busy until the timeline reaches it
    VkResult res;
    {
        std::lock_guard<std::mutex> queueLock(g_VkQueueMx);   // RENDER_THREAD_PLAN step 10: one lock for every queue submit/present/wait
        res = vkQueueSubmit(VulkanHW.m_TransferQueue, 1, &si, VK_NULL_HANDLE);
    }
    if (res != VK_SUCCESS) {
        // m_UploadValue was already incremented but will never be signaled now —
        // every later graphics frame waiting on it would wedge the queue. Roll the
        // counter back so waits target the last value that actually signals.
        Msg("!Vulkan FlushUploads: transfer submit error %d (rolling timeline back to %llu)", res, m_UploadValue - 1);
        m_UploadCmdValue[m_UploadCmdCur] = 0;   // never signalled -- do not wait for it
        --m_UploadValue;
        if (res == VK_ERROR_DEVICE_LOST) VK::Prof::DumpCheckpoints("FlushUploads");
    }
}

void CVulkanCommandManager::FlushUploadsAndWait()
{
    std::lock_guard<std::recursive_mutex> lock(m_UploadMutex);   // held across submit+wait so the ring reset is atomic
    FlushUploads();
    if (m_UploadTimeline == VK_NULL_HANDLE || m_UploadValue == 0) return;

    VkSemaphoreWaitInfo wi = {};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores    = &m_UploadTimeline;
    wi.pValues        = &m_UploadValue;
    VkResult wr = vkWaitSemaphores(VulkanHW.m_Device, &wi, 2000000000ULL);
    if (wr != VK_SUCCESS) {
        // Copies may still be in flight — rewinding the ring now would let new
        // stagings overwrite data the GPU is reading. Keep the head; log loudly.
        Msg("!Vulkan FlushUploadsAndWait: timeline wait %s (%d, value %llu) — ring NOT rewound",
            wr == VK_TIMEOUT ? "TIMEOUT" : "error", wr, m_UploadValue);
        VK::Prof::DumpCheckpoints("FlushUploadsAndWait");
        return;
    }

    m_StagingHead = 0;   // every submitted copy is done — the whole ring is free again
}


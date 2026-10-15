// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "vk_core.h"
#include <mutex>

// Command buffer management (triple buffering)
class CVulkanCommandManager
{
public:
    static constexpr u32 FRAMES_IN_FLIGHT = VK_FRAMES_IN_FLIGHT;

private:
    VkCommandPool   m_CommandPools[FRAMES_IN_FLIGHT];
    VkCommandBuffer m_CommandBuffers[FRAMES_IN_FLIGHT];
    // Second graphics segment per frame-in-flight (async frame-split): when the frame
    // is split so compute overlaps graphics, the scene records into m_CommandBuffers
    // and the post (tonemap+UI) into m_PostBuffers, submitted as two queue-ordered
    // batches. Same per-slot pool; the frame fence gates reuse of BOTH.
    VkCommandBuffer m_PostBuffers[FRAMES_IN_FLIGHT] = {};
    u32             m_CurrentFrame = 0;

    // Dedicated pool+buffer for one-shot immediate operations (uploads, layout transitions)
    // This is separate from the per-frame render command buffers to avoid conflicts.
    VkCommandPool   m_ImmediatePool = VK_NULL_HANDLE;
    VkCommandBuffer m_ImmediateCmd  = VK_NULL_HANDLE;
    VkFence         m_ImmediateFence = VK_NULL_HANDLE;

    // --- Async buffer uploader (dedicated transfer queue + staging ring + timeline) ---
    // Copies are recorded into m_UploadCmd against a persistent host-mapped staging
    // ring, submitted on VulkanHW.m_TransferQueue signaling m_UploadTimeline. The
    // per-frame graphics submit waits on that timeline (added inside Submit), so the
    // GPU orders uploads before the draws that read them — no CPU stall per upload.
    // Ownership across families is handled by CONCURRENT sharing on upload targets
    // (see CVulkanBuffer::Create), so no explicit queue-ownership-transfer barriers.
    VkCommandPool   m_UploadPool     = VK_NULL_HANDLE;   // on transfer family
    VkCommandBuffer m_UploadCmd      = VK_NULL_HANDLE;
    bool            m_UploadOpen     = false;            // m_UploadCmd has pending copies
    // ONE upload command buffer meant every reopen after a flush waited for the
    // WHOLE timeline (see EnsureUploadCmdOpen) -- a full drain wearing another
    // name, which is why finer ring segments moved the stall from "ring wait"
    // into "cmd-reopen wait" instead of removing it. A small ring of buffers,
    // each remembering the value its own submit signals, makes a reopen wait
    // only for what THAT buffer is still executing.
    static constexpr u32 kUploadCmds = 4;
    VkCommandBuffer m_UploadCmds[kUploadCmds]      = {};
    u64             m_UploadCmdValue[kUploadCmds]  = {};   // timeline value of that buffer's last submit
    u32             m_UploadCmdCur                 = 0;
    VkSemaphore     m_UploadTimeline = VK_NULL_HANDLE;   // monotonic, signaled per flush
    u64             m_UploadValue    = 0;                // last value submitted / to wait on
    VkBuffer        m_StagingBuf     = VK_NULL_HANDLE;
    VmaAllocation   m_StagingAlloc   = VK_NULL_HANDLE;
    u8*             m_StagingPtr     = nullptr;          // persistently mapped
    VkDeviceSize    m_StagingSize    = 0;
    VkDeviceSize    m_StagingHead    = 0;

    // The ring is written by the CPU and read by the transfer queue, so reusing
    // a byte means waiting for the batch that read it. Waiting for ALL of them
    // (the old ring-wrap drain) serialises the two sides completely: measured
    // 66 wraps x ~5.9 ms = 388 ms of a texture phase, at ~10.7 GB/s -- i.e. the
    // PCIe copy time itself, spent with the CPU standing still. A BIGGER ring
    // does not help (fewer, longer waits sum to the same). Tracking which batch
    // last wrote each segment does: crossing into a segment submits what has
    // accumulated (so the GPU starts) and waits only for that segment's own
    // batch, which by then is several segments old and usually finished.
    static constexpr u32 kStagingSegments = 8;
    u64             m_SegValue[kStagingSegments] = {};   // timeline value of the batch that last wrote segment i
    u32             m_SegCur = 0;                        // segment the head currently sits in

    // --- Graphics frame timeline (async compute ordering) ---
    // Signaled by EVERY graphics submit with a monotonically increasing value. The
    // async compute submit waits on the value observed at the START of the frame,
    // i.e. "everything the previous frames submitted has completed". Without it the
    // compute queue could overwrite a volume that the previous frame's tonemap or
    // particle pass is still reading. Waiting on the PREVIOUS frame costs nothing
    // against the current one — which is the frame we actually want to overlap.
    VkSemaphore     m_FrameTimeline  = VK_NULL_HANDLE;
    u64             m_FrameValue     = 0;

    // The whole upload path (m_UploadCmd recording + staging ring + m_UploadValue)
    // is touched from BOTH the main render thread (per-frame FlushUploads/Submit)
    // AND the seqParallel worker thread (spawn-time texture/buffer loads route
    // through UploadImage/UploadBuffer). A VkCommandBuffer may not be recorded by
    // one thread while another ends/resets/submits it — doing so corrupts driver
    // state (observed: c0000005 inside the NV driver at vkCmdPipelineBarrier2 while
    // an NPC streamed in). This serializes every upload op. Recursive because
    // StageBytes (on ring-wrap) and FlushUploadsAndWait re-enter locked methods on
    // the same thread.
    std::recursive_mutex m_UploadMutex;

    void EnsureUploadCmdOpen();
    // Copy `size` bytes into the staging ring (opening the upload cmd); returns the
    // ring byte-offset, or UINT64_MAX if `size` exceeds the whole ring.
    VkDeviceSize StageBytes(const void* data, VkDeviceSize size);

public:
    void Create();
    void Destroy();

    // Per-frame render command buffer (used by render loop only)
    VkCommandBuffer Begin();
    bool End(VkCommandBuffer cmd);
    // waitAsyncCompute: add the async-compute timeline to the wait list (default true
    // for the single/last submit). The SCENE segment of a frame-split passes false so
    // the async work overlaps it instead of blocking it — only the POST segment waits.
    bool Submit(VkCommandBuffer cmd, VkSemaphore waitSemaphore, VkSemaphore signalSemaphore, VkFence fence,
                bool waitAsyncCompute = true);

    // Begin the frame's SECOND graphics segment (async frame-split). Call after
    // submitting the scene segment; tonemap+UI record into the returned buffer, which
    // End() submits with imageAvailable/renderFinished/fence. Same in-flight slot.
    VkCommandBuffer BeginSecondSegment();

    // One-shot immediate command buffer (safe to call during rendering)
    VkCommandBuffer BeginImmediate();
    void            EndAndSubmitImmediate(VkCommandBuffer cmd);

    // Async buffer upload: stage `size` bytes from `data` and record a transfer-queue
    // copy into `dst` at `dstOffset`. Non-blocking (stalls only if the staging ring
    // wraps mid-batch). The next graphics submit waits on the upload timeline.
    void UploadBuffer(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size);

    // Async image upload: stage `size` bytes (all mips/layers, tightly packed) and
    // record on the transfer queue: UNDEFINED→TRANSFER_DST, copy `regions`, then
    // →SHADER_READ_ONLY_OPTIMAL. `regions[].bufferOffset` are relative to `data`
    // (the uploader rebases them into the staging ring). Leaves the image in
    // SHADER_READ_ONLY_OPTIMAL; the graphics frame's timeline wait (FRAGMENT_SHADER)
    // makes the copy+transition visible to samplers. Image MUST be CONCURRENT.
    void UploadImage(VkImage dst, const void* data, VkDeviceSize size,
                     const VkBufferImageCopy* regions, u32 regionCount,
                     u32 mipLevels, u32 arrayLayers);

    void FlushUploads();         // submit pending copies (async); call once per frame before Submit
    void FlushUploadsAndWait();  // submit + block until complete; use at end of level load

    // Two-slot pipeline: the ring slot is assigned per frame at extract (RV().ringSlot) and set here by Begin_Render on the
    // render thread; GetCurrentFrame() is therefore the slot of the frame the render thread is recording. Never read it on main.
    void SetCurrentFrame(u32 f) { m_CurrentFrame = f % FRAMES_IN_FLIGHT; }
    u32 GetCurrentFrame() const { return m_CurrentFrame; }

    // Graphics frame timeline — see the members. The value is what the async compute
    // submit waits on; read it at frame start, before this frame's submits bump it.
    VkSemaphore GetFrameTimeline() const { return m_FrameTimeline; }
    u64         GetFrameTimelineValue() const { return m_FrameValue; }

    VkCommandBuffer GetCurrentCommandBuffer() const { return m_CommandBuffers[m_CurrentFrame]; }
    VkCommandPool   GetCurrentPool() const { return m_CommandPools[m_CurrentFrame]; }
    VkCommandPool   GetPool(u32 index) const { return m_CommandPools[index]; }
};

// Глобальный экземпляр
extern CVulkanCommandManager CommandManager;

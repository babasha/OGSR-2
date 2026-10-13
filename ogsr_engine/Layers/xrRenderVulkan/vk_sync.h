// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "vk_core.h"

// Synchronization для одного frame-in-flight.
// NOTE: the "render finished" semaphore is NOT here — it must be per swapchain
// IMAGE (CVulkanSwapchain::m_RenderFinished), not per frame-in-flight, to satisfy
// VUID-vkQueueSubmit-pSignalSemaphores-00067. imageAvailable + inFlightFence stay
// per-frame: the fence gates their reuse.
struct FrameSync
{
    VkSemaphore imageAvailable;  // Signaled когда swapchain image acquired
    VkFence     inFlightFence;   // CPU-GPU sync
};

// Synchronization management (triple buffering)
class CVulkanSync
{
public:
    static constexpr u32 FRAMES_IN_FLIGHT = VK_FRAMES_IN_FLIGHT;

private:
    FrameSync m_FrameSync[FRAMES_IN_FLIGHT];

public:
    void Create();
    void Destroy();

    FrameSync& GetCurrentFrame(u32 frameIndex) { return m_FrameSync[frameIndex]; }

    bool WaitForFence(u32 frameIndex);
    bool ResetFence(u32 frameIndex);
};

// Глобальный экземпляр
extern CVulkanSync Sync;

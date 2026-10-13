// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// CFrameRingAllocator — single persistent-mapped VkBuffer split into
// FRAMES_IN_FLIGHT equal regions.  Each frame gets its own region so
// GPU from frame N-2 can still read its slice while CPU writes into N.
// Used in Step B to triple-buffer the GlobalLighting UBO and bone SSBO,
// eliminating the vkDeviceWaitIdle call in rvk.cpp.

#pragma once
#include "vk_core.h"

namespace VK
{

// Result of a single allocation from the ring buffer.
struct RingAlloc
{
    VkDeviceSize offset = 0;                // absolute byte offset in the VkBuffer
    void*        ptr    = nullptr;          // CPU-mapped pointer to the data slot
    VkBuffer     buffer = VK_NULL_HANDLE;   // the ring buffer handle

    bool IsValid() const { return buffer != VK_NULL_HANDLE; }
};

class CFrameRingAllocator
{
    static constexpr u32          FRAMES_IN_FLIGHT = VK_FRAMES_IN_FLIGHT;
    static constexpr VkDeviceSize TOTAL_SIZE        = 6u * 1024u * 1024u;  // 6 MB (evenly divisible by 3)

    VkBuffer      m_Buffer     = VK_NULL_HANDLE;
    VmaAllocation m_Allocation = VK_NULL_HANDLE;
    void*         m_MappedBase = nullptr;
    bool          m_IsCoherent = false;  // true if HOST_COHERENT (no flush needed)

    VkDeviceSize  m_RegionSize = 0;  // TOTAL_SIZE / FRAMES_IN_FLIGHT (always 256-aligned)
    VkDeviceSize  m_FrameBase  = 0;  // frameIndex * m_RegionSize
    VkDeviceSize  m_BumpOffset = 0;  // next allocation cursor (absolute byte offset)
    u32           m_FrameIndex = 0;

public:
    // Create the backing buffer.  Must be called after VMA is initialised.
    void Create();

    // Destroy the backing buffer.  Call before vmaDestroyAllocator / vkDestroyDevice.
    void Destroy();

    // Reset the bump cursor to the start of this frame's region.
    // Call once per frame, right after the descriptor pool reset.
    void BeginFrame(u32 frameIndex);

    // Allocate `size` bytes aligned to `alignment` within the current frame's region.
    // Default alignment = 256 (satisfies minUniformBufferOffsetAlignment on all GPUs).
    // Returns an invalid RingAlloc{} and asserts in debug if the region is exhausted.
    RingAlloc Allocate(VkDeviceSize size, VkDeviceSize alignment = 256);

    VkBuffer     GetBuffer()     const { return m_Buffer; }
    VkDeviceSize GetRegionSize() const { return m_RegionSize; }
    bool         IsCreated()     const { return m_Buffer != VK_NULL_HANDLE; }

    // Bytes already allocated in the current frame's region (for stats/debugging).
    VkDeviceSize UsedBytes() const { return m_BumpOffset - m_FrameBase; }
};

} // namespace VK

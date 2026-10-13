// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_ring_allocator.h"
#include "HW_Vulkan.h"

namespace VK
{

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------
void CFrameRingAllocator::Create()
{
    VK::Vram::Scope _vram_scope("Staging");
    VERIFY(m_Buffer == VK_NULL_HANDLE);

    m_RegionSize = TOTAL_SIZE / FRAMES_IN_FLIGHT;

    VkBufferCreateInfo bufCI = {};
    bufCI.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size        = TOTAL_SIZE;
    // UBO + SSBO usage so allocations can satisfy either descriptor type
    bufCI.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                      | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                      | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // HOST_ACCESS_SEQUENTIAL_WRITE → CPU-write-only pattern (UBO/SSBO updates)
    // MAPPED_BIT → persistent mapping, no explicit vkMapMemory each frame
    VmaAllocationCreateInfo allocCI = {};
    allocCI.usage = VMA_MEMORY_USAGE_AUTO;
    allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo allocInfo = {};
    VK_CHECK_CRITICAL(VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bufCI, &allocCI,
                                      &m_Buffer, &m_Allocation, &allocInfo));

    m_MappedBase = allocInfo.pMappedData;
    VERIFY(m_MappedBase);

    // Check if memory is host-coherent (no explicit flush needed)
    VkMemoryPropertyFlags memFlags = 0;
    vmaGetAllocationMemoryProperties(VulkanHW.m_Allocator, m_Allocation, &memFlags);
    m_IsCoherent = (memFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    m_FrameIndex = 0;
    m_FrameBase  = 0;
    m_BumpOffset = 0;

    Msg("[Vulkan] CFrameRingAllocator: %.1f MB total, %u regions x %.1f KB each, coherent=%s",
        TOTAL_SIZE    / (1024.0f * 1024.0f),
        FRAMES_IN_FLIGHT,
        m_RegionSize  / 1024.0f,
        m_IsCoherent  ? "yes" : "no");
}

// ---------------------------------------------------------------------------
// Destroy
// ---------------------------------------------------------------------------
void CFrameRingAllocator::Destroy()
{
    if (m_Buffer == VK_NULL_HANDLE) return;

    VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, m_Buffer, m_Allocation);
    m_Buffer     = VK_NULL_HANDLE;
    m_Allocation = VK_NULL_HANDLE;
    m_MappedBase = nullptr;
    m_RegionSize = 0;
    m_FrameBase  = 0;
    m_BumpOffset = 0;
}

// ---------------------------------------------------------------------------
// BeginFrame — reset bump cursor to this frame's region
// ---------------------------------------------------------------------------
void CFrameRingAllocator::BeginFrame(u32 frameIndex)
{
    VERIFY(frameIndex < FRAMES_IN_FLIGHT);

    // Diagnostics: log usage of the frame that just finished
    const VkDeviceSize used = UsedBytes();
    if (used > 0)
    {
        const float pct = (float)used / (float)m_RegionSize * 100.f;

        // Warn when usage exceeds 75%
        if (pct > 75.f)
        {
            Msg("![RING] HIGH usage: %llu / %llu bytes (%.1f%%) frame=%u",
                (unsigned long long)used, (unsigned long long)m_RegionSize,
                pct, m_FrameIndex);
        }

        // Periodic log every 300 frames (roughly every 5 seconds at 60fps)
        static u32 s_logCounter = 0;
        if (++s_logCounter >= 300)
        {
            s_logCounter = 0;
            Msg("[RING] usage: %llu / %llu bytes (%.1f%%) frame=%u",
                (unsigned long long)used, (unsigned long long)m_RegionSize,
                pct, m_FrameIndex);
        }
    }

    // Flush the previous frame's writes if memory is non-coherent
    if (!m_IsCoherent && m_BumpOffset > m_FrameBase)
    {
        vmaFlushAllocation(VulkanHW.m_Allocator, m_Allocation,
                           m_FrameBase, m_BumpOffset - m_FrameBase);
    }

    m_FrameIndex = frameIndex;
    m_FrameBase  = frameIndex * m_RegionSize;
    m_BumpOffset = m_FrameBase;
}

// ---------------------------------------------------------------------------
// Allocate
// ---------------------------------------------------------------------------
RingAlloc CFrameRingAllocator::Allocate(VkDeviceSize size, VkDeviceSize alignment)
{
    if (m_Buffer == VK_NULL_HANDLE || size == 0)
        return {};

    VERIFY(alignment > 0 && (alignment & (alignment - 1)) == 0);  // must be power of 2

    // Align up the current bump offset
    const VkDeviceSize aligned = (m_BumpOffset + alignment - 1) & ~(alignment - 1);

    if (aligned + size > m_FrameBase + m_RegionSize)
    {
        Msg("!CFrameRingAllocator: frame region exhausted (frame=%u, used=%llu/%llu)",
            m_FrameIndex, (unsigned long long)UsedBytes(),
            (unsigned long long)m_RegionSize);
        VERIFY(!"CFrameRingAllocator: frame region exhausted");
        return {};
    }

    m_BumpOffset = aligned + size;

    RingAlloc a;
    a.offset = aligned;
    a.ptr    = static_cast<char*>(m_MappedBase) + aligned;
    a.buffer = m_Buffer;
    return a;
}

} // namespace VK

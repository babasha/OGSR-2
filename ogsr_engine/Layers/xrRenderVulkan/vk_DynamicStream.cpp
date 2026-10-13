// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_R_Backend.h"
#include "HW_Vulkan.h"
#include "vk_command_buffer.h"   // CommandManager.GetCurrentFrame() — in-flight slot

// ============================================================================
// _VertexStream_vk Implementation
// ============================================================================

// Per-frame-in-flight REGION size for dynamic vertex buffer: 4MB (matches D3D renderer).
// This is enough for ~130K vertices at 32 bytes/vertex (typical FVF::L size).
// The actual buffer is DYNAMIC_VB_REGION * FRAMES_IN_FLIGHT so each in-flight frame
// owns its own region and CPU writes never race the GPU still reading an older frame.
static constexpr VkDeviceSize DYNAMIC_VB_REGION = 4 * 1024 * 1024;
static constexpr u32          kFramesInFlight   = CVulkanCommandManager::FRAMES_IN_FLIGHT;

_VertexStream_vk::_VertexStream_vk()
    : m_Buffer(VK_NULL_HANDLE)
    , m_Allocation(VK_NULL_HANDLE)
    , m_MappedData(nullptr)
    , m_Size(0)
    , m_RegionSize(0)
    , m_Position(0)
    , m_CurrentSlot(~0u)
    , m_DiscardID(0)
#ifdef DEBUG
    , dbg_lock(0)
#endif
{
}

_VertexStream_vk::~_VertexStream_vk()
{
    Destroy();
}

void _VertexStream_vk::Create()
{
    VK::Vram::Scope _vram_scope("DynStream");
    if (m_Buffer != VK_NULL_HANDLE) {
        Msg("![Vulkan] _VertexStream_vk::Create() - buffer already created");
        return;
    }

    m_RegionSize = (u32)DYNAMIC_VB_REGION;
    m_Size       = m_RegionSize * kFramesInFlight;   // one region per in-flight frame

    // Create buffer info
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = m_Size;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // VMA allocation info - HOST_VISIBLE with persistent mapping
    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;

    // Create buffer
    VkResult result = VK::Vram::CreateBuffer(
        VulkanHW.m_Allocator,
        &bufferInfo,
        &allocInfo,
        &m_Buffer,
        &m_Allocation,
        nullptr
    );

    if (result != VK_SUCCESS) {
        Msg("![Vulkan] Failed to create dynamic vertex buffer: %d", result);
        return;
    }

    // Get persistent mapped pointer
    VmaAllocationInfo allocInfoResult;
    vmaGetAllocationInfo(VulkanHW.m_Allocator, m_Allocation, &allocInfoResult);
    m_MappedData = allocInfoResult.pMappedData;

    if (!m_MappedData) {
        Msg("![Vulkan] Dynamic vertex buffer mapping failed");
        VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, m_Buffer, m_Allocation);
        m_Buffer = VK_NULL_HANDLE;
        m_Allocation = VK_NULL_HANDLE;
        return;
    }

    m_Position = 0;
    m_CurrentSlot = ~0u;   // force a region reset on the first Lock of the next frame
    m_DiscardID = 0;

    Msg("[Vulkan] Dynamic vertex buffer created: %d KB total (%d KB region x %u frames-in-flight, persistent mapped)",
        m_Size / 1024, m_RegionSize / 1024, kFramesInFlight);
}

void _VertexStream_vk::Destroy()
{
    if (m_Buffer != VK_NULL_HANDLE) {
        // No need to unmap - VMA handles it automatically with MAPPED_BIT
        VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, m_Buffer, m_Allocation);
        m_Buffer = VK_NULL_HANDLE;
        m_Allocation = VK_NULL_HANDLE;
        m_MappedData = nullptr;
        m_Size = 0;
        m_RegionSize = 0;
        m_Position = 0;
        m_CurrentSlot = ~0u;
    }
}

void _VertexStream_vk::reset_begin()
{
    // Called at frame start - nothing to do for Vulkan
    // (D3D uses this to reset old buffers)
}

void _VertexStream_vk::reset_end()
{
    // Called at frame end - nothing to do for Vulkan
}

void* _VertexStream_vk::Lock(u32 vl_Count, u32 Stride, u32& vOffset)
{
#ifdef DEBUG
    VERIFY(dbg_lock == 0);
    dbg_lock++;
#endif

    if (!m_MappedData) {
        Msg("![Vulkan] _VertexStream_vk::Lock() - buffer not created or mapping failed");
        vOffset = 0;
        return nullptr;
    }

    // Calculate bytes needed — must fit within ONE per-frame region.
    u32 bytes_need = vl_Count * Stride;
    R_ASSERT2(
        (bytes_need <= m_RegionSize) && vl_Count,
        make_string("bytes_need = %d, region = %d, vl_Count = %d", bytes_need, m_RegionSize, vl_Count)
    );

    // Select this frame's region. GetCurrentFrame() is stable across a frame, and
    // WaitForFence(slot) in CRender::Begin already proved the GPU finished the previous
    // submission that read this region — so writing it here never races a frame still
    // in flight. (Same invariant as the bone SSBO / sky descriptor.)
    const u32 slot       = CommandManager.GetCurrentFrame();
    const u32 regionBase = slot * m_RegionSize;
    const u32 regionEnd  = regionBase + m_RegionSize;

    // First Lock of a new frame: open the region fresh (DISCARD at region base).
    if (slot != m_CurrentSlot) {
        m_CurrentSlot = slot;
        m_Position = regionBase;
        m_DiscardID++;
    }

    // NOOVERWRITE: next stride-aligned vertex boundary at-or-after the current position.
    // Rounding UP guarantees byteStart == vOffset * Stride exactly, so the absolute
    // vertex offset handed to the draw matches the pointer we return.
    u32 vStart    = (m_Position + Stride - 1) / Stride;   // ceil-divide to a vertex index
    u32 byteStart = vStart * Stride;

    // DISCARD: if this lock would overrun the region, wrap to the region base.
    if (byteStart + bytes_need > regionEnd) {
        vStart    = (regionBase + Stride - 1) / Stride;   // first vertex boundary >= regionBase
        byteStart = vStart * Stride;
        m_DiscardID++;
    }

    m_Position = byteStart;   // Unlock flushes [m_Position, bytes_written) then advances it
    vOffset    = vStart;      // ABSOLUTE whole-buffer vertex index (region offset baked in)
    return (u8*)m_MappedData + byteStart;
}

void _VertexStream_vk::Unlock(u32 Count, u32 Stride)
{
#ifdef DEBUG
    VERIFY(dbg_lock == 1);
    dbg_lock--;
#endif

    // Calculate bytes written
    u32 bytes_written = Count * Stride;

    // Flush memory range to make writes visible to GPU
    // (Required for HOST_VISIBLE memory that is not HOST_COHERENT)
    VmaAllocationInfo allocInfo;
    vmaGetAllocationInfo(VulkanHW.m_Allocator, m_Allocation, &allocInfo);

    // Check if memory is not coherent (requires manual flush)
    VkMemoryPropertyFlags memFlags;
    vmaGetMemoryTypeProperties(VulkanHW.m_Allocator, allocInfo.memoryType, &memFlags);

    if (!(memFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        // Manual flush required
        VkResult result = vmaFlushAllocation(
            VulkanHW.m_Allocator,
            m_Allocation,
            m_Position,
            bytes_written
        );

        if (result != VK_SUCCESS) {
            Msg("![Vulkan] Failed to flush dynamic vertex buffer: %d", result);
        }
    }

    // Update position for next lock
    m_Position += bytes_written;
}

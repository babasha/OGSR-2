// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_buffer.h"
#include "HW_Vulkan.h"
#include "vk_command_buffer.h"
#include "vk_vram_stats.h"      // Vram::CurrentTag — which subsystem asked for this buffer

extern float ps_r_buf_priority;  // VK_EXT_memory_priority for device-local buffers (vk_console_min.cpp)

#ifdef __SWITCH__
extern "C" void xr_nx_indirect_register(uint64_t buffer, void* ptr, uint64_t size);
extern "C" int xr_nx_indirect_unregister(uint64_t buffer);
#endif

namespace VK
{

// Threshold для staging buffer (64 KB)
static constexpr VkDeviceSize STAGING_THRESHOLD = 64 * 1024;

// Constructor
CVulkanBuffer::CVulkanBuffer()
{
}

// Destructor
CVulkanBuffer::~CVulkanBuffer()
{
    Destroy();
}

// Создание буфера
void CVulkanBuffer::Create(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage, bool gpuOnly,
                           bool computeShared, bool hostRead)
{
    if (m_Buffer != VK_NULL_HANDLE) {
        Msg("![Vulkan] Buffer already created, call Destroy first");
        return;
    }

    if (size == 0) {
        Msg("![Vulkan] Cannot create buffer with size 0");
        return;
    }

    m_Size = size;
    m_Usage = usage;
    m_MemUsage = memUsage;

    // Buffer create info
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // VMA allocation info
    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = memUsage;

    // ⭐⭐⭐VK_EXT_memory_priority — EVICTION ORDER, AND IT WAS MISSING HERE ENTIRELY.
    // Images built through the shared helper are tagged 1.0 ("never evict before
    // streamable textures", vk_image.cpp) and streamable world textures 0.25
    // (vk_texture.cpp), which reads like a complete policy — but BUFFERS set nothing
    // and therefore landed on VMA's 0.5 default. A device-local buffer is not a
    // second-class citizen of a frame: the world cull meta array is 830 149 entries
    // x 80 B = 63 MB and is scanned TWICE per frame, and the vertex/index pools are
    // read by every draw. There is nothing to re-fetch them from either — a demoted
    // texture costs a mip, a demoted SSBO costs PCIe on every single read.
    //
    // ⚠⚠ THIS IS A POLICY FIX, NOT A MEASURED WIN — DO NOT QUOTE MILLISECONDS FOR IT.
    // It was written while chasing World/Cull, which spent one evening reading 6.9 ms
    // instead of its usual 0.23 on the same level and the same entry count. Lowering
    // VRAM pressure (texture_lod 0 -> 3) appeared to cure it 30x, which looked like
    // proof of eviction. It was not: that state turned out to be INTERMITTENT and
    // clustered in TIME (good at 14:50 and 15:05, bad 15:26-16:21, good afterwards) —
    // the "cure" simply coincided with the window ending.
    // The follow-up killed the theory outright. With r_vram_ballast_mb the heap was
    // driven to 112-114% of budget ON PURPOSE, and at priority 0.5 (the old default)
    // World/Cull stayed at 0.22 ms. The knob does not control the pathology, and the
    // pathology is still unexplained.
    // What stands on its own is the ASYMMETRY: images ask for 1.0 and buffers asked for
    // nothing, so a buffer the GPU scans twice a frame ranked below an attachment it may
    // touch once. Fixing that is right whether or not it ever shows up in a frame time.
    //
    // Device-local gets 1.0, matching attachments: the streaming system can and does
    // rebuild a texture at a lower mip, so textures at 0.25 are the right thing to
    // spill first, and nothing below that line should be a buffer the GPU reads every
    // frame. Host-visible/staging keeps the default — it lives in system memory by
    // construction, so an eviction priority means nothing there.
    // r_buf_priority makes this an A/B rather than an assertion: 1.0 ships, 0.5 restores
    // VMA's default (the pre-25-08 behaviour) so the pathology can be reproduced on the
    // same binary. Read here, at allocation time — set it BEFORE `load`.
    if (gpuOnly || memUsage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE)
        allocInfo.priority = ps_r_buf_priority;

    if (gpuOnly) {
        // GPU-only: DEVICE_LOCAL is REQUIRED and no host access is requested. The
        // default storage-buffer path below forces HOST_ACCESS_SEQUENTIAL_WRITE,
        // which makes VMA pick HOST_VISIBLE memory — the 256MB BAR heap, or worse,
        // SYSTEM RAM once BAR is full: every shader access then rides PCIe (the
        // Pripyat tree-bin 20-30ms pathology). CPU writes go through Upload/Fill.
        allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    } else {
    // Для uniform/storage buffers включаем HOST_VISIBLE для persistent mapping
    // Storage buffers (e.g. bone SSBO) also need CPU write access for per-frame updates
    if (usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    // Для staging buffers (TRANSFER_SRC) или HOST memory - нужен host access
    if ((usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || memUsage == VMA_MEMORY_USAGE_AUTO_PREFER_HOST) {
        allocInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }
    // Readback target: the CPU reads this mapping, so it must be CACHED. RANDOM
    // and SEQUENTIAL_WRITE are mutually exclusive in VMA — drop the write hint.
    if (hostRead) {
        allocInfo.flags &= ~VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        allocInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }
    }

    // ВАЖНО: Для VERTEX/INDEX буферов добавляем TRANSFER_DST для staging uploads
    if ((usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) &&
        !(usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    // Upload targets are filled by the async uploader on the dedicated TRANSFER
    // queue and read on the GRAPHICS queue. With distinct families that's a
    // cross-family access — use CONCURRENT sharing so the contents survive without
    // explicit queue-ownership-transfer barriers (negligible cost for buffers).
    // Only when a TRANSFER_DST target AND the families actually differ.
    // Families this buffer is shared with, built once: graphics is always index 0,
    // then transfer (upload target) and/or compute (async compute) when they are
    // distinct families. A single-entry list stays EXCLUSIVE — CONCURRENT with one
    // family is not legal, and there would be nothing to share with anyway.
    u32 families[3] = { VulkanHW.m_GraphicsFamily, 0, 0 };
    u32 familyCount = 1;
    if ((bufferInfo.usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) &&
        VulkanHW.m_TransferFamily != VulkanHW.m_GraphicsFamily)
        families[familyCount++] = VulkanHW.m_TransferFamily;
    if (computeShared) {
        bool already = false;                       // may already be listed as graphics/transfer
        for (u32 i = 0; i < familyCount; ++i)
            if (families[i] == VulkanHW.m_ComputeFamily) { already = true; break; }
        if (!already) families[familyCount++] = VulkanHW.m_ComputeFamily;
    }
    if (familyCount > 1) {
        bufferInfo.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        bufferInfo.queueFamilyIndexCount = familyCount;
        bufferInfo.pQueueFamilyIndices   = families;
    }

    VK_CHECK(VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bufferInfo, &allocInfo,
                             &m_Buffer, &m_Allocation, nullptr));

#ifdef __SWITCH__
    // Indirect-command buffers must start ZEROED on Horizon. vkCmd*IndirectCount does not exist on Maxwell, so the
    // renderer's calls are replaced by plain multi-draws of ALL maxDrawCount commands (xr_nx_throw.cpp) -- including the
    // tail the cull never wrote. Unified memory comes back full of old heap contents, and draws built from that garbage
    // (random index counts/offsets) fault the GPU, which takes the console down at the first world frame (30-09).
    // A zeroed command is an empty draw; a leftover one from an earlier frame is a real, harmless one.
    if ((usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) && m_Allocation) {
        void* p = nullptr;
        if (vmaMapMemory(VulkanHW.m_Allocator, m_Allocation, &p) == VK_SUCCESS && p) {
            memset(p, 0, size_t(size));
            vmaFlushAllocation(VulkanHW.m_Allocator, m_Allocation, 0, VK_WHOLE_SIZE);
            // Stays mapped: the IndirectCount replacement reads the GPU's last count straight from here.
            xr_nx_indirect_register(uint64_t(reinterpret_cast<uintptr_t>(m_Buffer)), p, uint64_t(size));
        } else {
            Msg("![Vulkan] indirect buffer (%llu KB) is not host-mappable -- left uninitialised", (unsigned long long)(size >> 10));
        }
    }
#endif

    // Tag the VMA allocation by usage so any leaked buffer is identifiable in the
    // vmaDestroyAllocator leak dump (see vma_impl.cpp). Negligible cost.
    if (m_Allocation) {
        const char* tag =
            (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ? "buf:storage" :
            (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? "buf:uniform" :
            (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)  ? "buf:vertex"  :
            (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)   ? "buf:index"   :
            (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)   ? "buf:staging" : "buf:other";
        vmaSetAllocationName(VulkanHW.m_Allocator, m_Allocation, tag);

        // ⭐WHERE A BIG BUFFER ACTUALLY LANDED. VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE is a
        // PREFERENCE: when VRAM is short it silently places the allocation in host memory
        // and every read then crosses PCIe. Nothing in the API says so, and the symptom
        // does not look like memory at all — it looks like one compute pass being
        // inexplicably slow and, worse, CONSTANT (a pass that reads N bytes over PCIe
        // costs the same no matter what the scene does).
        //
        // That is not hypothetical here: World/HZBcull scans the 830 k-entry world meta
        // array (80 B each = 66 MB) twice a frame and has been sitting at a suspiciously
        // flat 6.6 ms across two profiling sessions, while 132 MB/frame at real VRAM
        // bandwidth would be ~0.25 ms. 6.6 ms works out to ~20 GB/s — PCIe, not VRAM.
        // This line is what turns that arithmetic from a guess into a fact.
        //
        // Only for buffers big enough to matter, so a level load prints a handful of
        // lines rather than hundreds.
        //
        // ⚠ The distinction that makes this instrument usable rather than alarming:
        // "in HOST memory" is a PROBLEM only for a buffer that ASKED for device memory.
        // Staging and per-frame CPU-written rings live in host memory BY DESIGN, and a
        // first version of this line flagged them too — which buried the real hits under
        // false ones. `!` is now raised only when the request was device-local and the
        // allocation is not, i.e. when VRAM actually ran out.
        //
        // The Vram scope tag says WHOSE buffer it is (the same tag the VRAM accounting
        // buckets by). Without it the log reports "12 MB of vertex data in host memory"
        // three times and there is no way to tell which subsystem to go look at.
        if (size >= (8ull << 20)) {
            VkMemoryPropertyFlags mp = 0;
            vmaGetAllocationMemoryProperties(VulkanHW.m_Allocator, m_Allocation, &mp);
            const bool devLocal = (mp & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
            const bool hostVis  = (mp & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
            const bool wantDev  = gpuOnly || memUsage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            const char* scope   = Vram::CurrentTag() ? Vram::CurrentTag() : "-";
            Msg("%s[VK Vram] %-11s %-10s %4llu MB -> %s%s%s",
                (wantDev && !devLocal) ? "!" : "",
                scope, tag, (unsigned long long)(size >> 20),
                devLocal ? "DEVICE_LOCAL" : "HOST",
                (devLocal && hostVis) ? " +HOST_VISIBLE (BAR)" : "",
                wantDev ? " [wanted device]" : " [host by design]");
        }
    }

    // Если буфер создан с MAPPED_BIT, получаем mapped pointer
    if (allocInfo.flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) {
        VmaAllocationInfo allocInfoResult;
        vmaGetAllocationInfo(VulkanHW.m_Allocator, m_Allocation, &allocInfoResult);
        m_Mapped = allocInfoResult.pMappedData;
    }
}

// Уничтожение буфера
void CVulkanBuffer::Destroy()
{
    if (m_Buffer == VK_NULL_HANDLE) {
        return;
    }

    // Unmap if mapped (skip for persistent-mapped uniform/storage buffers — VMA handles them)
    if (m_Mapped != nullptr && !(m_Usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))) {
        Unmap();
    }

#ifdef __SWITCH__
    if (xr_nx_indirect_unregister(uint64_t(reinterpret_cast<uintptr_t>(m_Buffer))))
        vmaUnmapMemory(VulkanHW.m_Allocator, m_Allocation); // balance the persistent map taken in Create()
#endif
    VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, m_Buffer, m_Allocation);

    m_Buffer = VK_NULL_HANDLE;
    m_Allocation = VK_NULL_HANDLE;
    m_Size = 0;
    m_Mapped = nullptr;
}

void CVulkanBuffer::AdoptFrom(CVulkanBuffer& other)
{
    if (this == &other) return;
    Destroy();
    m_Buffer     = other.m_Buffer;
    m_Allocation = other.m_Allocation;
    m_Size       = other.m_Size;
    m_Mapped     = other.m_Mapped;
    m_Usage      = other.m_Usage;
    m_MemUsage   = other.m_MemUsage;
    other.m_Buffer     = VK_NULL_HANDLE;
    other.m_Allocation = VK_NULL_HANDLE;
    other.m_Size       = 0;
    other.m_Mapped     = nullptr;
}

// Upload данных
void CVulkanBuffer::Upload(const void* data, VkDeviceSize size, VkDeviceSize offset)
{
    if (!IsValid()) {
        Msg("![Vulkan] Upload: buffer is not valid (Create() was not called or failed)");
        return;
    }

    if (!data) {
        Msg("![Vulkan] Upload: data is null");
        return;
    }

    if (offset + size > m_Size) {
        Msg("![Vulkan] Upload: offset + size exceeds buffer size");
        return;
    }

    // Проверяем, является ли буфер host-visible (можно ли Map)
    VmaAllocationInfo allocInfo;
    vmaGetAllocationInfo(VulkanHW.m_Allocator, m_Allocation, &allocInfo);

    VkMemoryPropertyFlags memFlags;
    vmaGetMemoryTypeProperties(VulkanHW.m_Allocator, allocInfo.memoryType, &memFlags);

    bool isHostVisible = (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

    // Если буфер НЕ host-visible (DEVICE_LOCAL only) - всегда используем staging
    if (!isHostVisible) {
        UploadViaStaging(data, size, offset);
        return;
    }

    // Буфер host-visible - можем мапить напрямую
    // Для небольших данных - прямой memcpy
    if (size < STAGING_THRESHOLD) {
        void* mapped = Map();
        if (!mapped) {
            Msg("![Vulkan] Failed to map buffer for upload");
            // Fallback to staging
            UploadViaStaging(data, size, offset);
            return;
        }

        memcpy((u8*)mapped + offset, data, size);
        Flush();

        // Unmap только если не persistent-mapped uniform buffer
        if (!(m_Usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
            Unmap();
        }
    }
    else {
        // Для больших данных - staging buffer (быстрее для transfer)
        UploadViaStaging(data, size, offset);
    }
}

// Upload через staging — теперь асинхронно на выделенной transfer-очереди.
// Раньше тут на КАЖДЫЙ upload создавался+уничтожался staging-буфер и делался
// immediate submit+wait (полный стол). Теперь — общий async-аплоадер
// (персистентный staging-ring + timeline), без per-upload аллокаций и без стола:
// следующий graphics-сабмит ждёт копию через upload-timeline (см. CommandManager).
void CVulkanBuffer::UploadViaStaging(const void* data, VkDeviceSize size, VkDeviceSize offset)
{
    if (!IsValid()) {
        Msg("![Vulkan] UploadViaStaging: destination buffer is not valid");
        return;
    }
    CommandManager.UploadBuffer(m_Buffer, offset, data, size);
}

// Map memory
void* CVulkanBuffer::Map()
{
    if (!IsValid()) {
        Msg("![Vulkan] Map: buffer is not valid (Create() was not called or failed)");
        return nullptr;
    }

    if (m_Mapped != nullptr) {
        // Already mapped (persistent-mapped uniform buffer)
        return m_Mapped;
    }

    // A gpuOnly buffer (allocInfo.requiredFlags = DEVICE_LOCAL, no host access) has
    // no host-visible memory to map at all. Upload() already tests this and reroutes
    // through staging, but Map() did not — so a direct caller reached vmaMapMemory on
    // DEVICE_LOCAL-only memory (VUID-vkMapMemory-memory-00682, caught 16-08: "memory
    // has type 1 which has properties VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT"). Whatever
    // pointer that returns is not legally writable, so fail it here instead: every
    // caller already handles a null Map() by falling back to staging.
    {
        VmaAllocationInfo ai{};
        vmaGetAllocationInfo(VulkanHW.m_Allocator, m_Allocation, &ai);
        VkMemoryPropertyFlags memFlags = 0;
        vmaGetMemoryTypeProperties(VulkanHW.m_Allocator, ai.memoryType, &memFlags);
        if ((memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                Msg("![Vulkan] Map() on a DEVICE_LOCAL-only buffer — refused (use Upload(), which "
                    "stages). This is reported once; the caller must not map GPU-only memory.");
            }
            return nullptr;
        }
    }

    void* data = nullptr;
    VkResult result = vmaMapMemory(VulkanHW.m_Allocator, m_Allocation, &data);

    if (result != VK_SUCCESS) {
        Msg("![Vulkan] vmaMapMemory failed: %d", result);
        return nullptr;
    }

    m_Mapped = data;
    return data;
}

// Unmap memory
void CVulkanBuffer::Unmap()
{
    if (!IsValid()) {
        return;
    }

    if (m_Mapped == nullptr) {
        return;
    }

    // Не unmap persistent-mapped uniform/storage buffers
    if (m_Usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        return;
    }

    vmaUnmapMemory(VulkanHW.m_Allocator, m_Allocation);
    m_Mapped = nullptr;
}

// Flush (для non-coherent memory)
void CVulkanBuffer::Flush()
{
    if (!IsValid()) {
        return;
    }

    VkResult result = vmaFlushAllocation(VulkanHW.m_Allocator, m_Allocation, 0, VK_WHOLE_SIZE);
    if (result != VK_SUCCESS) {
        Msg("![Vulkan] vmaFlushAllocation failed: %d", result);
    }
}

// Invalidate (для non-coherent memory)
void CVulkanBuffer::Invalidate()
{
    if (!IsValid()) {
        return;
    }

    VkResult result = vmaInvalidateAllocation(VulkanHW.m_Allocator, m_Allocation, 0, VK_WHOLE_SIZE);
    if (result != VK_SUCCESS) {
        Msg("![Vulkan] vmaInvalidateAllocation failed: %d", result);
    }
}

// Get buffer device address
VkDeviceAddress CVulkanBuffer::GetDeviceAddress() const
{
    if (!IsValid()) return 0;
    VkBufferDeviceAddressInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = m_Buffer;
    return vkGetBufferDeviceAddress(VulkanHW.m_Device, &info);
}

} // namespace VK

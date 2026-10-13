// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "vk_core.h"
#include "HW_Vulkan.h"   // VulkanHW.m_Device — DescriptorWriter::Flush is header-inline
#include <initializer_list>

namespace VK
{

// ============================================================================
// Set layout / pool / allocation helpers
//
// The counterpart to DescriptorWriter (below): the writer collapsed the UPDATE
// side, these collapse the CREATE side — the ~15-line `VkDescriptorSetLayoutBinding
// b[N]{}` + pool sizes + VkDescriptorSetAllocateInfo tail every pass hand-rolls.
//
// Bindings are POSITIONAL: types[i] describes binding i with descriptorCount 1,
// which is what every set in this renderer already does. Pool sizes are summed
// FROM that same list, so a pool can no longer drift behind the bindings it has
// to satisfy — the create-side twin of the hazard the writer closed.
//
// A set that needs binding flags (bindless / PARTIALLY_BOUND), a non-1 descriptor
// count, or FREE_DESCRIPTOR_SET stays hand-rolled; these cover the plain case.
// All log a tagged error and return VK_NULL_HANDLE / false on failure.
// ============================================================================

VkDescriptorSetLayout MakeSetLayout(std::initializer_list<VkDescriptorType> types,
                                    VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT,
                                    const char* tag = "set");

// Pool sized for `setCount` copies of `types`.
VkDescriptorPool MakeDescriptorPool(std::initializer_list<VkDescriptorType> types, u32 setCount,
                                    const char* tag = "pool");

// Allocate `count` sets of ONE layout out of `pool` into `outSets`.
bool AllocSets(VkDescriptorPool pool, VkDescriptorSetLayout layout, u32 count,
               VkDescriptorSet* outSets, const char* tag = "sets");

// layout + pool + `count` sets in one call — the whole tail of a typical init.
// For several set GROUPS out of one pool, call the three above separately.
bool MakeDescriptorSets(std::initializer_list<VkDescriptorType> types, u32 count,
                        VkDescriptorSetLayout& outLayout, VkDescriptorPool& outPool,
                        VkDescriptorSet* outSets,
                        VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT,
                        const char* tag = "descriptors");


// ============================================================================
// DescriptorWriter - fluent builder for batched descriptor updates
//
// Replaces the hand-rolled `VkWriteDescriptorSet w[N]{}` + running `count`
// idiom that every module had a copy of. Besides the boilerplate, that idiom
// carried a real hazard: N is written by hand and drifts behind the binding
// list, so an added binding writes past the end of the array and the process
// dies during init with a garbage descriptor (see the EnvLight scar at
// binding 32/33). Here the capacity is a template parameter checked on every
// append, so overflow is a VERIFY at the append, not stack corruption later.
//
// Stack-allocated, zero heap; one instance writes ONE set. Flush() commits
// every recorded write in a single vkUpdateDescriptorSets and resets, so a
// writer can be refilled in a loop.
//
//   VK::DescriptorWriter(set)
//       .UniformBuffer(0, ubo.GetHandle(), sizeof(UBO))
//       .ImageSampler(1, view, sampler)
//       .StorageImage(2, dstView)
//       .Flush();
// ============================================================================
template <u32 MAX_WRITES>
class TDescriptorWriter
{
    VkDescriptorSet        m_Set;
    VkWriteDescriptorSet   m_Writes[MAX_WRITES];
    VkDescriptorBufferInfo m_BufferInfos[MAX_WRITES];
    VkDescriptorImageInfo  m_ImageInfos[MAX_WRITES];
    u32                    m_Count = 0;

    // Every field is assigned here, so the arrays need no up-front zeroing —
    // which matters for the wide instantiations (48 slots = ~5 KB).
    VkWriteDescriptorSet& Add(u32 binding, VkDescriptorType type)
    {
        VERIFY(m_Count < MAX_WRITES);
        VkWriteDescriptorSet& w = m_Writes[m_Count];
        w.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.pNext            = nullptr;
        w.dstSet           = m_Set;
        w.dstBinding       = binding;
        w.dstArrayElement  = 0;
        w.descriptorCount  = 1;
        w.descriptorType   = type;
        w.pImageInfo       = nullptr;
        w.pBufferInfo      = nullptr;
        w.pTexelBufferView = nullptr;
        return w;
    }

public:
    explicit TDescriptorWriter(VkDescriptorSet set) : m_Set(set) {}

    // ---- buffers -----------------------------------------------------------
    TDescriptorWriter& UniformBuffer(u32 binding, VkBuffer buf,
                                     VkDeviceSize size = VK_WHOLE_SIZE, VkDeviceSize offset = 0)
    {
        return Buffer(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, { buf, offset, size });
    }
    TDescriptorWriter& StorageBuffer(u32 binding, VkBuffer buf,
                                     VkDeviceSize size = VK_WHOLE_SIZE, VkDeviceSize offset = 0)
    {
        return Buffer(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, { buf, offset, size });
    }

    // ---- images ------------------------------------------------------------
    TDescriptorWriter& ImageSampler(u32 binding, VkImageView view, VkSampler sampler,
                                    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        return Image(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, { sampler, view, layout });
    }
    TDescriptorWriter& StorageImage(u32 binding, VkImageView view,
                                    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL)
    {
        return Image(binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, { VK_NULL_HANDLE, view, layout });
    }

    // ---- prebuilt infos / runtime type -------------------------------------
    // For the sites that already keep a VkDescriptorImageInfo[] around, or that
    // pick the descriptor type from a table at runtime. The info is COPIED, so
    // the caller's temporary needn't outlive the call.
    TDescriptorWriter& Buffer(u32 binding, VkDescriptorType type, const VkDescriptorBufferInfo& info)
    {
        Add(binding, type).pBufferInfo = &(m_BufferInfos[m_Count] = info);
        ++m_Count;
        return *this;
    }
    // `arrayElement` addresses one slot of a descriptor ARRAY (bindless tables).
    TDescriptorWriter& Image(u32 binding, VkDescriptorType type, const VkDescriptorImageInfo& info,
                             u32 arrayElement = 0)
    {
        VkWriteDescriptorSet& w = Add(binding, type);
        w.dstArrayElement = arrayElement;
        w.pImageInfo = &(m_ImageInfos[m_Count] = info);
        ++m_Count;
        return *this;
    }

    u32 Count() const { return m_Count; }

    void Flush()
    {
        if (m_Count > 0)
            vkUpdateDescriptorSets(VulkanHW.m_Device, m_Count, m_Writes, 0, nullptr);
        m_Count = 0;
    }
};

// Default capacity covers every set in this renderer but EnvLight's (34 bindings),
// which names a wider instantiation explicitly. 24 slots is ~2.7 KB of stack —
// paid only inside the function that writes a set, never held across a frame.
using DescriptorWriter = TDescriptorWriter<24>;

} // namespace VK

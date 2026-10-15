// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_descriptors.h"
#include "HW_Vulkan.h"

namespace VK
{

// ============================================================================
// Set layout / pool / allocation helpers — see vk_descriptors.h
// ============================================================================
namespace
{
    // Wide enough for every set in this renderer (EnvLight's 34 is the record).
    constexpr u32 kMaxBindings = 64;
}

VkDescriptorSetLayout MakeSetLayout(std::initializer_list<VkDescriptorType> types,
                                    VkShaderStageFlags stages, const char* tag)
{
    const u32 n = (u32)types.size();
    VERIFY(n > 0 && n <= kMaxBindings);

    VkDescriptorSetLayoutBinding b[kMaxBindings]{};
    for (u32 i = 0; i < n; ++i)
    {
        b[i].binding         = i;
        b[i].descriptorType  = types.begin()[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags      = stages;
    }

    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = n;
    lci.pBindings    = b;

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    const VkResult res = vkCreateDescriptorSetLayout(VulkanHW.m_Device, &lci, nullptr, &layout);
    if (res != VK_SUCCESS)
    {
        Msg("![VK] set layout '%s' create failed (vk=%d)", tag ? tag : "?", (int)res);
        return VK_NULL_HANDLE;
    }
    return layout;
}

VkDescriptorPool MakeDescriptorPool(std::initializer_list<VkDescriptorType> types, u32 setCount,
                                    const char* tag)
{
    const u32 n = (u32)types.size();
    VERIFY(n > 0 && n <= kMaxBindings && setCount > 0);

    // Sum the bindings per distinct type (first-seen order) — the counts every
    // site used to write by hand next to the binding list.
    VkDescriptorPoolSize sizes[kMaxBindings]{};
    u32 kinds = 0;
    for (u32 i = 0; i < n; ++i)
    {
        const VkDescriptorType t = types.begin()[i];
        u32 k = 0;
        for (; k < kinds; ++k)
            if (sizes[k].type == t) break;
        if (k == kinds) { sizes[kinds].type = t; sizes[kinds].descriptorCount = 0; ++kinds; }
        sizes[k].descriptorCount += setCount;
    }

    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets       = setCount;
    pci.poolSizeCount = kinds;
    pci.pPoolSizes    = sizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    const VkResult res = vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &pool);
    if (res != VK_SUCCESS)
    {
        Msg("![VK] descriptor pool '%s' create failed (vk=%d)", tag ? tag : "?", (int)res);
        return VK_NULL_HANDLE;
    }
    return pool;
}

bool AllocSets(VkDescriptorPool pool, VkDescriptorSetLayout layout, u32 count,
               VkDescriptorSet* outSets, const char* tag)
{
    VERIFY(outSets && count > 0 && count <= kMaxBindings);

    VkDescriptorSetLayout layouts[kMaxBindings];
    for (u32 i = 0; i < count; ++i) layouts[i] = layout;

    VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool     = pool;
    dai.descriptorSetCount = count;
    dai.pSetLayouts        = layouts;

    const VkResult res = vkAllocateDescriptorSets(VulkanHW.m_Device, &dai, outSets);
    if (res != VK_SUCCESS)
    {
        Msg("![VK] descriptor sets '%s' alloc failed (vk=%d)", tag ? tag : "?", (int)res);
        return false;
    }
    return true;
}

bool MakeDescriptorSets(std::initializer_list<VkDescriptorType> types, u32 count,
                        VkDescriptorSetLayout& outLayout, VkDescriptorPool& outPool,
                        VkDescriptorSet* outSets, VkShaderStageFlags stages, const char* tag)
{
    outLayout = MakeSetLayout(types, stages, tag);
    if (outLayout == VK_NULL_HANDLE) return false;

    outPool = MakeDescriptorPool(types, count, tag);
    if (outPool == VK_NULL_HANDLE) return false;

    return AllocSets(outPool, outLayout, count, outSets, tag);
}

} // namespace VK


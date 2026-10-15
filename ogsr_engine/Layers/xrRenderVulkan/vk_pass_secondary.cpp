// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_pass_secondary.h"
#include "vk_command_buffer.h"   // CVulkanCommandManager::FRAMES_IN_FLIGHT
#include "HW_Vulkan.h"           // VulkanHW.m_Device / m_GraphicsFamily

namespace VK
{
namespace SecCB
{
namespace
{
constexpr u32 kFrames = CVulkanCommandManager::FRAMES_IN_FLIGHT;

struct Lane
{
    VkCommandPool            pool = VK_NULL_HANDLE;
    xr_vector<VkCommandBuffer> bufs;   // выданные за всю жизнь пула
    u32                      used = 0; // сколько из них выдано В ЭТОМ кадре
};

Lane s_lanes[kFrames][kMaxLanes];
u32  s_frame = 0;
bool s_failed = false;   // один раз пожаловались и больше не пытаемся

bool EnsurePool(Lane& l)
{
    if (l.pool != VK_NULL_HANDLE)
        return true;
    if (s_failed)
        return false;

    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.queueFamilyIndex = VulkanHW.m_GraphicsFamily;
    // TRANSIENT + сброс ПУЛА целиком: буферы живут один кадр, по-одному их
    // сбрасывать дороже и незачем.
    ci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (VK_SUCCESS != vkCreateCommandPool(VulkanHW.m_Device, &ci, nullptr, &l.pool))
    {
        s_failed = true;
        Msg("![VK SecCB] vkCreateCommandPool failed — secondary recording stays off");
        return false;
    }
    return true;
}
} // namespace

void BeginFrame(u32 frameIndex)
{
    s_frame = frameIndex % kFrames;
    for (u32 i = 0; i < kMaxLanes; ++i)
    {
        Lane& l = s_lanes[s_frame][i];
        if (l.pool == VK_NULL_HANDLE || l.used == 0)
        {
            l.used = 0;
            continue;
        }
        // ⚠Сбрасывать пул можно только когда кадр, писавший в него, уже
        // отработал. Это гарантирует ТОТ ЖЕ забор, что охраняет primary этого
        // слота in-flight: CRender::Begin ждёт его до всего остального.
        vkResetCommandPool(VulkanHW.m_Device, l.pool, 0);
        l.used = 0;
    }
}

VkCommandBuffer Acquire(u32 lane)
{
    if (lane >= kMaxLanes)
        return VK_NULL_HANDLE;
    Lane& l = s_lanes[s_frame][lane];
    if (!EnsurePool(l))
        return VK_NULL_HANDLE;

    if (l.used >= l.bufs.size())
    {
        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = l.pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (VK_SUCCESS != vkAllocateCommandBuffers(VulkanHW.m_Device, &ai, &cb))
        {
            s_failed = true;
            Msg("![VK SecCB] vkAllocateCommandBuffers failed — secondary recording stays off");
            return VK_NULL_HANDLE;
        }
        l.bufs.push_back(cb);
    }

    VkCommandBuffer cb = l.bufs[l.used++];

    // ⚠renderPass = NULL и БЕЗ RENDER_PASS_CONTINUE: пасс сам открывает свою
    // область через vkCmdBeginRendering, а не продолжает чужую.
    VkCommandBufferInheritanceInfo inh{};
    inh.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;

    VkCommandBufferBeginInfo bi{};
    bi.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bi.pInheritanceInfo = &inh;
    if (VK_SUCCESS != vkBeginCommandBuffer(cb, &bi))
    {
        s_failed = true;
        Msg("![VK SecCB] vkBeginCommandBuffer failed — secondary recording stays off");
        return VK_NULL_HANDLE;
    }
    return cb;
}

void Finish(VkCommandBuffer cb)
{
    if (cb != VK_NULL_HANDLE)
        vkEndCommandBuffer(cb);
}

void Destroy()
{
    for (u32 f = 0; f < kFrames; ++f)
        for (u32 i = 0; i < kMaxLanes; ++i)
        {
            Lane& l = s_lanes[f][i];
            if (l.pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(VulkanHW.m_Device, l.pool, nullptr);
            l.pool = VK_NULL_HANDLE;
            l.bufs.clear();
            l.used = 0;
        }
    s_failed = false;
}
} // namespace SecCB
} // namespace VK

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "vk_core.h"

namespace VK
{

// A slot in a per-frame ring buffer: buffer + absolute byte offset + CPU-mapped pointer.
struct RingAlloc
{
    VkDeviceSize offset = 0;                // absolute byte offset in the VkBuffer
    void*        ptr    = nullptr;          // CPU-mapped pointer to the data slot
    VkBuffer     buffer = VK_NULL_HANDLE;   // the ring buffer handle

    bool IsValid() const { return buffer != VK_NULL_HANDLE; }
};

} // namespace VK

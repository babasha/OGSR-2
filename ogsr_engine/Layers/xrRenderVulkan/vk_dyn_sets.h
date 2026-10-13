// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// Dynamic-UBO descriptor sets with an implicit zero offset.
//
// WHY: on our NVK for the Switch (GM20B, pre-Turing) a plain UNIFORM_BUFFER from an
// ordinary descriptor set reaches the shader's constant bank (cbuf) ONLY in the
// fragment stage (nvk_nir_lower_descriptors.c build_cbuf_map); in VS/TES/compute every
// read is a load from memory. A UNIFORM_BUFFER_DYNAMIC goes to cbuf in EVERY stage, and
// its address is known on the CPU, so binding it costs a direct SET_CONSTANT_BUFFER
// instead of the indirect pushbuf fetch (an extra GPFIFO segment) a plain UBO takes on
// every set rebind. nfsmw-nx measured +18-23 % moving constants from memory to a dynamic
// UBO on the same hardware.
//
// HOW: the few sets whose UBOs became dynamic are Register()ed with their dynamic count.
// Every vkCmdBindDescriptorSets in the renderer goes through the wrapper below (stdafx.h
// includes this right after <vulkan/vulkan.h>); a call that passes NO dynamic offsets
// for a registered set gets zeros. So the ~176 bind sites -- many receive the set as a
// plain VkDescriptorSet argument several calls away from where it was fetched -- stay
// untouched, and none of them can be missed: on NVK a missing dynamic offset is a read
// through a null pDynamicOffsets, not a validation warning.
//
// The descriptor itself still carries the real offset (slot stride etc.), so a zero
// dynamic offset reads exactly what the plain UBO read.
//
// `-no_dyn_ubo` on the command line (Switch: "dynubo=0" in the one-shot nx_opts.txt)
// keeps the old plain-UBO layouts, for an A/B. Decided once, before the first layout.
#pragma once
#include <atomic>
#include <cstring>

namespace VK::DynSets
{
inline constexpr uint32_t kMaxSets    = 32;   // registered sets alive at once (EnvLight 3, grass gen 2)
inline constexpr uint32_t kMaxOffsets = 16;   // dynamic offsets one bind may need to fill in

struct Entry
{
    std::atomic<VkDescriptorSet> set{ VK_NULL_HANDLE };
    std::atomic<uint32_t>        dyn{ 0 };
};
inline Entry                 g_entries[kMaxSets];
inline std::atomic<uint32_t> g_used{ 0 };     // high-water mark of g_entries; 0 = the wrapper's fast path

// Registration happens at init / level load on the main thread while the render thread
// may be recording: the set handle is published LAST (release), so a reader that sees it
// sees its count. A set is registered before anything binds it and unregistered only
// after the device stopped using it (pool destroy / level unload), so no reader can race
// a handle being retired.
inline void Register(VkDescriptorSet set, uint32_t dynCount)
{
    if (set == VK_NULL_HANDLE || dynCount == 0) return;
    for (uint32_t i = 0; i < kMaxSets; ++i) {
        if (g_entries[i].set.load(std::memory_order_relaxed) != VK_NULL_HANDLE) continue;
        g_entries[i].dyn.store(dynCount, std::memory_order_relaxed);
        g_entries[i].set.store(set, std::memory_order_release);
        uint32_t used = g_used.load(std::memory_order_relaxed);
        while (used < i + 1 && !g_used.compare_exchange_weak(used, i + 1)) {}
        return;
    }
    Msg("![VK DynSets] registry full (%u) -- set left unregistered", kMaxSets);
    VERIFY(false);
}

inline void Unregister(VkDescriptorSet set)
{
    if (set == VK_NULL_HANDLE) return;
    const uint32_t used = g_used.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < used; ++i)
        if (g_entries[i].set.load(std::memory_order_relaxed) == set) {
            g_entries[i].set.store(VK_NULL_HANDLE, std::memory_order_release);
            g_entries[i].dyn.store(0, std::memory_order_relaxed);
        }
}

inline uint32_t DynCountOf(VkDescriptorSet set, uint32_t used)
{
    for (uint32_t i = 0; i < used; ++i)
        if (g_entries[i].set.load(std::memory_order_acquire) == set)
            return g_entries[i].dyn.load(std::memory_order_relaxed);
    return 0;
}

// The A/B switch. Read from the command line once; every module that creates a layout
// asks this, so all of them agree for the whole run.
inline bool Enabled()
{
    static const bool on = (strstr(Core.Params, "-no_dyn_ubo") == nullptr);
    return on;
}

// UNIFORM_BUFFER_DYNAMIC when the path is on, the plain type otherwise -- for layouts,
// pool sizes and descriptor writes, which must all agree.
inline VkDescriptorType UboType()
{
    return Enabled() ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}
} // namespace VK::DynSets

// The wrapper every vkCmdBindDescriptorSets in the renderer resolves to (macro below).
// `(vkCmdBindDescriptorSets)(...)` -- the parenthesised name -- calls the real entry point:
// a function-like macro is only expanded when its name is followed by '('.
inline void VK_CmdBindDescriptorSetsDyn(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
                                        uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* sets,
                                        uint32_t dynCount, const uint32_t* dyn)
{
    // Only a call that passes NO offsets is completed. One that passes some knows about dynamic
    // descriptors and owns the whole list (the skinned passes bind the bone window this way) --
    // no site binds a registered set together with another dynamic set in one call.
    const uint32_t used = (dynCount == 0) ? VK::DynSets::g_used.load(std::memory_order_acquire) : 0u;
    if (used != 0) {
        uint32_t need = 0;
        for (uint32_t s = 0; s < setCount; ++s)
            need += VK::DynSets::DynCountOf(sets[s], used);
        if (need != 0) {
            static const uint32_t kZeros[VK::DynSets::kMaxOffsets] = {};
            if (need > VK::DynSets::kMaxOffsets) {
                Msg("![VK DynSets] bind needs %u dynamic offsets (max %u)", need, VK::DynSets::kMaxOffsets);
                VERIFY(false);
                return;
            }
            (vkCmdBindDescriptorSets)(cmd, bindPoint, layout, firstSet, setCount, sets, need, kZeros);
            return;
        }
    }
    (vkCmdBindDescriptorSets)(cmd, bindPoint, layout, firstSet, setCount, sets, dynCount, dyn);
}
#define vkCmdBindDescriptorSets(...) VK_CmdBindDescriptorSetsDyn(__VA_ARGS__)

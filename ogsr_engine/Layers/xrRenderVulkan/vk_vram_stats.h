// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once

// NOTE: no includes — this header rides in the PCH (stdafx.h) right after
// vulkan/vma, and only needs their types plus xrCore's u64/s64.

// ============================================================================
// VRAM attribution (streaming-world Stage E-1).
//
// The [VK Perf] line says HOW MUCH device memory is used (VMA heap budgets) but
// not WHO holds it — on Pripyat ~5.4GB is non-texture and unattributed. This
// registry splits every VMA buffer/image allocation into a named subsystem
// bucket so the budget work (and future cuts) aim at real numbers.
//
// How attribution works:
//  - `Vram::Scope _vs("Trees");` at a subsystem's build/create entry tags every
//    allocation made beneath it (thread-local, nestable).
//  - vmaCreate/Destroy(Buffer|Image) call sites are replaced with the
//    signature-compatible wrappers below (pure find-replace), which record the
//    real allocation size and remember the owning tag per handle — destroy
//    sites don't need to know the tag.
//  - Images created via VK::CreateImage (vk_image.h) with no active scope fall
//    into "RT/untagged" (that helper serves render targets); everything else
//    unattributed lands in "untagged".
//
// Reporting (vk_profiler::MaybeLog): r_profiler>=1 → compact top-N line every
// 5s; r_profiler>=3 (or perf mark) → full sorted table.
// ============================================================================

namespace VK
{
namespace Vram
{

// Scoped subsystem tag. Pass a string LITERAL (the pointer is stored).
struct Scope
{
    const char* prev;
    explicit Scope(const char* tag);
    ~Scope();
};

const char* CurrentTag(); // active scope tag or nullptr

// Manual accounting for memory that doesn't flow through the wrappers
// (e.g. swapchain images owned by the driver are NOT tracked at all).
void Track(const char* tag, u64 bytes);
void Untrack(const char* tag, u64 bytes);

// r_vram_ballast_mb — allocate N MB of device-local VRAM that nothing reads, purely to
// push the heap toward its budget on demand. The eviction pathology this exists to study
// is INTERMITTENT (same level, same 94% of budget, World/Cull measured 0.23 ms and
// 6.91 ms hours apart), and a pathology that cannot be summoned cannot be used to judge
// a fix. Allocated after the level's real buffers, freed with the level. 0 = off.
void AllocBallast();
void FreeBallast();

// Signature-compatible tracked wrappers over vmaCreateBuffer/vmaDestroyBuffer/
// vmaCreateImage/vmaDestroyImage — converting a call site is a find-replace.
VkResult CreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* bci, const VmaAllocationCreateInfo* aci, VkBuffer* buf, VmaAllocation* alloc, VmaAllocationInfo* info);
void DestroyBuffer(VmaAllocator allocator, VkBuffer buf, VmaAllocation alloc);
VkResult CreateImage(VmaAllocator allocator, const VkImageCreateInfo* ici, const VmaAllocationCreateInfo* aci, VkImage* img, VmaAllocation* alloc, VmaAllocationInfo* info);
void DestroyImage(VmaAllocator allocator, VkImage img, VmaAllocation alloc);

// Device-local heap totals in MB (the same numbers [VK VRAM] prints). For code
// that must reason about headroom BEFORE it allocates — notably the DLSS/NGX
// feature creation, whose allocations bypass VMA entirely and are therefore
// invisible to every bucket above (they land in "untracked").
void HeapTotalsMB(double& usedMB, double& budgetMB);

// Log output. Compact = one "[VK VRAM]" line, top buckets + untagged + total.
// Full = sorted table, one line per bucket with live count.
void LogCompact();
void LogFull();

// Destroy the size-segregated small-allocation pools (see vk_vram_stats.cpp) —
// must run before vmaDestroyAllocator.
void DestroySmallPools(VmaAllocator allocator);

// Full VMA allocator JSON (every pool/block/allocation with names) →
// $app_data_root$\vma_stats.json. The scalpel for the "who owns the GB-scale
// vma-slack" question the bucket totals can't answer. Called by r_vram_dump and
// automatically when the DLSS VRAM rescue gives up.
void DumpVmaJson();

} // namespace Vram
} // namespace VK

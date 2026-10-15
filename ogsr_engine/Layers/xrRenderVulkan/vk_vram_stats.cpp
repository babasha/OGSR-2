// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// VRAM attribution registry — see vk_vram_stats.h.

#include "stdafx.h"
#include "vk_vram_stats.h"
#include "HW_Vulkan.h"

// r_vram_small_images — 0 skips the small-image pool path (and the driver probe in
// front of it). Global scope on purpose: a namespace-scope extern mangles
// differently and silently fails to bind.
extern int ps_r_vram_small_images;

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <algorithm>

namespace VK
{
// Counters shared with the texture-load profiler (vk_texture.cpp): the image
// allocation is its `create` column, and this is what sits in front of it.
namespace TexLoadProf {
extern std::atomic<u64> s_createProbeClk;
extern std::atomic<u32> s_createSmall;
}

namespace Vram
{

namespace
{
constexpr u32 kMaxTags = 96;

struct TagSlot
{
    const char* name{};
    std::atomic<s64> bytes{0};
    std::atomic<s64> peak{0};
    std::atomic<u32> count{0};
};

TagSlot s_tags[kMaxTags];
std::atomic<u32> s_tagCount{0};
std::mutex s_lock; // guards tag interning + the handle map (allocation-time only)

// live handle -> (tag index, real size); shared by buffers and images (handles
// are unique per type, and a collision would need identical u64 values anyway)
std::unordered_map<u64, std::pair<u32, u64>> s_live;

thread_local const char* t_scope = nullptr;

u32 intern(const char* tag)
{
    if (!tag)
        tag = "untagged";
    const u32 n = s_tagCount.load(std::memory_order_acquire);
    for (u32 i = 0; i < n; ++i)
        if (s_tags[i].name == tag || 0 == strcmp(s_tags[i].name, tag))
            return i;
    std::lock_guard<std::mutex> g(s_lock);
    const u32 n2 = s_tagCount.load(std::memory_order_relaxed);
    for (u32 i = n; i < n2; ++i)
        if (s_tags[i].name == tag || 0 == strcmp(s_tags[i].name, tag))
            return i;
    if (n2 >= kMaxTags)
        return 0; // overflow → first bucket; bump kMaxTags if this ever fires
    s_tags[n2].name = tag;
    s_tagCount.store(n2 + 1, std::memory_order_release);
    return n2;
}

void add(u32 idx, s64 bytes, s32 count)
{
    TagSlot& t = s_tags[idx];
    const s64 now = t.bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    t.count.fetch_add((u32)count, std::memory_order_relaxed);
    s64 pk = t.peak.load(std::memory_order_relaxed);
    while (now > pk && !t.peak.compare_exchange_weak(pk, now, std::memory_order_relaxed))
        ;
}

void track_handle(u64 handle, const char* tag, u64 bytes)
{
    const u32 idx = intern(tag);
    add(idx, (s64)bytes, +1);
    std::lock_guard<std::mutex> g(s_lock);
    s_live[handle] = {idx, bytes};
}

void untrack_handle(u64 handle)
{
    u32 idx;
    u64 bytes;
    {
        std::lock_guard<std::mutex> g(s_lock);
        auto it = s_live.find(handle);
        if (it == s_live.end())
            return; // created before tracking existed (or not ours) — ignore
        idx = it->second.first;
        bytes = it->second.second;
        s_live.erase(it);
    }
    add(idx, -(s64)bytes, -1);
}

u64 alloc_size(VmaAllocator allocator, VmaAllocation alloc, const VmaAllocationInfo* info)
{
    if (info)
        return info->size;
    VmaAllocationInfo ai{};
    vmaGetAllocationInfo(allocator, alloc, &ai);
    return ai.size;
}
} // namespace

Scope::Scope(const char* tag) : prev(t_scope) { t_scope = tag; }
Scope::~Scope() { t_scope = prev; }

const char* CurrentTag() { return t_scope; }

void Track(const char* tag, u64 bytes) { add(intern(tag), (s64)bytes, +1); }
void Untrack(const char* tag, u64 bytes) { add(intern(tag), -(s64)bytes, -1); }

// ---------------------------------------------------------------------------
// Size-segregated small pools. Sub-1MB long-lived allocations (prop IB/VBs,
// small textures) used to land in the shared 64-256 MB blocks BETWEEN huge
// load-time transients; when the transients died, the 2-5 surviving KB-scale
// allocations pinned each block — measured 1740 MB of unreleasable slack on
// Pripyat (vma_stats.json 18-07: 27 blocks x 64 MB, live "buf:index=0.0MB").
// Routing small allocations into their own 8 MB-block pools (per memory type)
// lets the big blocks empty out and actually return to the OS.
// ---------------------------------------------------------------------------
namespace
{
constexpr VkDeviceSize kSmallAllocMax = 1ull << 20;   // < 1 MB → small pool
constexpr VkDeviceSize kSmallBlock    = 8ull << 20;

std::mutex s_smallMx;
std::unordered_map<u32, VmaPool> s_smallPools;   // memoryTypeIndex → pool

VmaPool SmallPoolFor(VmaAllocator allocator, u32 memTypeIndex)
{
    std::lock_guard<std::mutex> lk(s_smallMx);
    auto it = s_smallPools.find(memTypeIndex);
    if (it != s_smallPools.end())
        return it->second;
    VmaPoolCreateInfo pci{};
    pci.memoryTypeIndex = memTypeIndex;
    pci.blockSize       = kSmallBlock;
    VmaPool pool = VK_NULL_HANDLE;
    if (vmaCreatePool(allocator, &pci, &pool) != VK_SUCCESS)
        pool = VK_NULL_HANDLE;   // cached: don't retry a failing type every call
    s_smallPools[memTypeIndex] = pool;
    return pool;
}

// Eligible = plain sub-allocation the caller didn't pin elsewhere.
bool SmallEligible(VkDeviceSize size, const VmaAllocationCreateInfo* aci)
{
    return size < kSmallAllocMax && aci->pool == VK_NULL_HANDLE &&
           !(aci->flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);
}
} // namespace

void DestroySmallPools(VmaAllocator allocator)
{
    std::lock_guard<std::mutex> lk(s_smallMx);
    for (auto& kv : s_smallPools)
        if (kv.second != VK_NULL_HANDLE)
            vmaDestroyPool(allocator, kv.second);
    s_smallPools.clear();
}

VkResult CreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* bci, const VmaAllocationCreateInfo* aci, VkBuffer* buf, VmaAllocation* alloc, VmaAllocationInfo* info)
{
    if (SmallEligible(bci->size, aci)) {
        u32 memType = 0;
        if (vmaFindMemoryTypeIndexForBufferInfo(allocator, bci, aci, &memType) == VK_SUCCESS) {
            if (VmaPool pool = SmallPoolFor(allocator, memType)) {
                VmaAllocationCreateInfo aciSmall = *aci;
                aciSmall.pool = pool;
                const VkResult r2 = vmaCreateBuffer(allocator, bci, &aciSmall, buf, alloc, info);
                if (r2 == VK_SUCCESS && *buf != VK_NULL_HANDLE) {
                    track_handle((u64)*buf, t_scope, alloc_size(allocator, *alloc, info));
                    return r2;
                }
                // any failure → fall through to the default path below
            }
        }
    }
    const VkResult res = vmaCreateBuffer(allocator, bci, aci, buf, alloc, info);
    if (res == VK_SUCCESS && *buf != VK_NULL_HANDLE)
        track_handle((u64)*buf, t_scope, alloc_size(allocator, *alloc, info));
    return res;
}

void DestroyBuffer(VmaAllocator allocator, VkBuffer buf, VmaAllocation alloc)
{
    if (buf != VK_NULL_HANDLE)
        untrack_handle((u64)buf);
    vmaDestroyBuffer(allocator, buf, alloc);
}

VkResult CreateImage(VmaAllocator allocator, const VkImageCreateInfo* ici, const VmaAllocationCreateInfo* aci, VkImage* img, VmaAllocation* alloc, VmaAllocationInfo* info)
{
    // Query the real allocation size WITHOUT creating the image (core 1.3) —
    // small textures (UI icons, prop maps at their streaming floor) pin shared
    // blocks exactly like small buffers do.
    // The query is a driver call on EVERY image, and a level load makes ~2500 of
    // them; it is timed apart from the allocation it precedes (see the `create
    // split` line) so "create 130 ms" can be told apart from "the probe in front
    // of it costs 130 ms". r_vram_small_images 0 skips the whole small-pool path.
    if (ps_r_vram_small_images && aci->pool == VK_NULL_HANDLE && !(aci->flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT)) {
        const u64 _p0 = CPU::GetCLK();
        VkDeviceImageMemoryRequirements dimr{ VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS };
        dimr.pCreateInfo = ici;
        VkMemoryRequirements2 mr2{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        vkGetDeviceImageMemoryRequirements(VulkanHW.m_Device, &dimr, &mr2);
        VK::TexLoadProf::s_createProbeClk += CPU::GetCLK() - _p0;
        if (SmallEligible(mr2.memoryRequirements.size, aci)) {
            ++VK::TexLoadProf::s_createSmall;
            u32 memType = 0;
            if (vmaFindMemoryTypeIndexForImageInfo(allocator, ici, aci, &memType) == VK_SUCCESS) {
                if (VmaPool pool = SmallPoolFor(allocator, memType)) {
                    VmaAllocationCreateInfo aciSmall = *aci;
                    aciSmall.pool = pool;
                    const VkResult r2 = vmaCreateImage(allocator, ici, &aciSmall, img, alloc, info);
                    if (r2 == VK_SUCCESS && *img != VK_NULL_HANDLE) {
                        track_handle((u64)*img, t_scope, alloc_size(allocator, *alloc, info));
                        return r2;
                    }
                }
            }
        }
    }
    const VkResult res = vmaCreateImage(allocator, ici, aci, img, alloc, info);
    if (res == VK_SUCCESS && *img != VK_NULL_HANDLE)
        track_handle((u64)*img, t_scope, alloc_size(allocator, *alloc, info));
    return res;
}

void DestroyImage(VmaAllocator allocator, VkImage img, VmaAllocation alloc)
{
    if (img != VK_NULL_HANDLE)
        untrack_handle((u64)img);
    vmaDestroyImage(allocator, img, alloc);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
namespace
{
struct Row
{
    const char* name;
    s64 bytes, peak;
    u32 count;
};

u32 snapshot(Row* rows)
{
    const u32 n = s_tagCount.load(std::memory_order_acquire);
    u32 m = 0;
    for (u32 i = 0; i < n; ++i)
    {
        const s64 b = s_tags[i].bytes.load(std::memory_order_relaxed);
        const s64 p = s_tags[i].peak.load(std::memory_order_relaxed);
        if (b == 0 && p == 0)
            continue;
        rows[m++] = {s_tags[i].name, b, p, s_tags[i].count.load(std::memory_order_relaxed)};
    }
    std::sort(rows, rows + m, [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
    return m;
}

void heap_totals(double& usedMB, double& budgetMB, double* slackMB = nullptr)
{
    usedMB = budgetMB = 0;
    if (slackMB) *slackMB = 0;
    if (!VulkanHW.m_Allocator)
        return;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(VulkanHW.m_Allocator, budgets);
    const VkPhysicalDeviceMemoryProperties* mp{};
    vmaGetMemoryProperties(VulkanHW.m_Allocator, &mp);
    for (u32 h = 0; h < mp->memoryHeapCount; ++h)
        if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            usedMB += double(budgets[h].usage) / (1024.0 * 1024.0);
            budgetMB += double(budgets[h].budget) / (1024.0 * 1024.0);
            // Slack = VkDeviceMemory VMA holds minus what live allocations occupy —
            // the fragmentation the driver counts as used but nothing owns. The
            // architectural target: AAA engines run this at 100-200 MB, we grew GBs.
            if (slackMB && budgets[h].statistics.blockBytes > budgets[h].statistics.allocationBytes)
                *slackMB += double(budgets[h].statistics.blockBytes - budgets[h].statistics.allocationBytes) / (1024.0 * 1024.0);
        }
}
} // namespace

// Public read of the same device-local totals LogCompact prints. Exists because
// NGX allocates OUTSIDE VMA: the only way to size its footprint is to read the
// heap before and after CreateFeature and subtract (see vk_dlss.cpp).
void HeapTotalsMB(double& usedMB, double& budgetMB) { heap_totals(usedMB, budgetMB); }

void LogCompact()
{
    Row rows[kMaxTags];
    const u32 m = snapshot(rows);
    s64 tracked = 0;
    for (u32 i = 0; i < m; ++i)
        tracked += rows[i].bytes;

    char line[1024];
    int off = 0;
    const u32 top = std::min(m, 12u);
    for (u32 i = 0; i < top && off < (int)sizeof(line) - 48; ++i)
        off += _snprintf(line + off, sizeof(line) - off - 1, "%s=%lld ", rows[i].name, rows[i].bytes >> 20);
    line[off > 0 ? off : 0] = 0;

    double usedMB, budgetMB, slackMB;
    heap_totals(usedMB, budgetMB, &slackMB);
    Msg("[VK VRAM] %s| tracked %lld MB of %.0f used (%.0f budget) — untracked %.0f MB (vma-slack %.0f)",
        line, tracked >> 20, usedMB, budgetMB, usedMB - double(tracked >> 20), slackMB);
}

void DumpVmaJson()
{
    if (!VulkanHW.m_Allocator) return;
    char* stats = nullptr;
    vmaBuildStatsString(VulkanHW.m_Allocator, &stats, VK_TRUE);
    if (!stats) { Msg("![VK VRAM] vmaBuildStatsString failed"); return; }
    const size_t len = strlen(stats);
    if (IWriter* W = FS.w_open("$app_data_root$", "vma_stats.json")) {
        W->w(stats, (u32)len);
        FS.w_close(W);
        Msg("[VK VRAM] full allocator dump -> vma_stats.json (%zu KB)", len >> 10);
    } else
        Msg("![VK VRAM] cannot open vma_stats.json for write");
    vmaFreeStatsString(VulkanHW.m_Allocator, stats);
}

void LogFull()
{
    Row rows[kMaxTags];
    const u32 m = snapshot(rows);
    s64 tracked = 0;
    for (u32 i = 0; i < m; ++i)
        tracked += rows[i].bytes;

    double usedMB, budgetMB;
    heap_totals(usedMB, budgetMB);
    Msg("[VK VRAM] ---- attribution: %u buckets, tracked %lld MB / used %.0f MB (budget %.0f) ----", m, tracked >> 20, usedMB, budgetMB);
    for (u32 i = 0; i < m; ++i)
        Msg("[VK VRAM]   %-18s %6lld MB  (peak %6lld MB, live %u)", rows[i].name, rows[i].bytes >> 20, rows[i].peak >> 20, rows[i].count);
    Msg("[VK VRAM]   %-18s %6.0f MB  (driver/swapchain/pipelines + pre-tracking allocs)", "<untracked>", usedMB - double(tracked >> 20));
}

} // namespace Vram
} // namespace VK

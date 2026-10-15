// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// Load-time parallel-for. The post-visual passes walk hundreds of thousands of
// index-independent items (tree instances, cull entries, detail slots) on ONE
// thread while the other fifteen sit idle — the level is loading, nothing else
// runs. Same shape as the crown bake in vk_TreeManager: split the range, join,
// merge afterwards in index order so the result never depends on thread order.
//
// The body must write only its own indices and read only shared-immutable data;
// there is no synchronisation here by design.

#pragma once

#include <thread>
#include <functional>

namespace VK
{

// Contiguous split: body(lo, hi) is called once per worker with a half-open
// range. Tiny ranges run inline — 16 thread creations cost ~1 ms, which is not
// worth paying for a handful. The unit is whatever the caller counts (rows,
// instances, entries), so the threshold stays deliberately low.
template <class F>
inline void ParallelChunks(int count, F&& body)
{
    if (count <= 0) return;
    u32 nThr = std::thread::hardware_concurrency();
    nThr = _min(_max(1u, nThr), 16u);
    if (count <= 64 || nThr <= 1) { body(0, count); return; }

    xr_vector<std::thread> pool;
    pool.reserve(nThr);
    const int chunk = (count + int(nThr) - 1) / int(nThr);
    for (u32 w = 0; w < nThr; ++w)
    {
        const int lo = int(w) * chunk, hi = _min(lo + chunk, count);
        if (lo >= hi) break;
        pool.emplace_back([&body, lo, hi] { body(lo, hi); });
    }
    for (auto& th : pool) th.join();
}

// The same idea for work that repeats hundreds of times inside one phase, where
// creating sixteen threads per call would cost more than the split wins: a
// PERSISTENT pool (the one the staging ring already uses, vk_command_buffer.cpp)
// runs body(i) for i in [0, chunks) on `helpers` threads plus the caller and
// returns when the last chunk is done. Jobs are serialised per pool, so a caller
// from another thread waits rather than stealing chunks.
void FarmRun(u32 chunks, u32 helpers, const std::function<void(u32)>& body);

}   // namespace VK

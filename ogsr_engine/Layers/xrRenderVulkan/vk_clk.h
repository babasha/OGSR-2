// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// A clock for load-time counters that fire hundreds of thousands of times.
//
// CTimer is wrong for those twice over. It TRUNCATES: GetElapsed_ms_total()
// goes through duration_cast<microseconds>, so a measurement of 0.4 us reads as
// zero, and 450k of them read as zero milliseconds while costing 225. And it is
// not free: getElapsedTime() is virtual and reads QPC, ~50 ns a sample, which a
// per-visual counter pays 1.8 million times.
//
// So accumulate raw rdtsc (~10 ns, no truncation) and convert ONCE at dump
// time. The conversion factor is measured against QPC over a span of seconds
// (the level load), not sampled in a window we would have to sleep through:
// both counters are read at the anchor and again at the dump, and the ratio of
// the two deltas is the answer. Invariant TSC makes that ratio constant.
//
// See [[microsecond-truncation-invented-a-phase]]: this file exists because a
// timer invented a 225 ms "loop overhead" that was never there.

#pragma once

namespace VK
{
namespace detail
{
struct ClkAnchorT
{
    u64 qpc, clk;
    ClkAnchorT() : qpc(CPU::QPC()), clk(CPU::GetCLK()) {}
};

inline const ClkAnchorT& ClkAnchorRef()
{
    static const ClkAnchorT a;
    return a;
}
}   // namespace detail

// Start the span the ratio is measured over. Call once, early (level load
// start); harmless to call again. Without it the first ClkToMs() pays for a
// short spin instead.
inline void ClkAnchor() { detail::ClkAnchorRef(); }

// Milliseconds per rdtsc tick. Multiply an accumulated tick delta by this.
inline float ClkToMs()
{
    static float k = 0.f;   // kept from the longest span seen so far

    const detail::ClkAnchorT& a = detail::ClkAnchorRef();
    const u64 freq = CPU::QPCFreq();
    const u64 dq   = CPU::QPC() - a.qpc;

    if (dq >= freq / 100)   // >= 10 ms of span: trustworthy, refine
    {
        const u64 dc = CPU::GetCLK() - a.clk;
        if (dc) k = float(1000.0 * double(dq) / (double(freq) * double(dc)));
    }
    else if (k == 0.f)
    {
        // Nobody anchored in time. Spin 2 ms rather than return a made-up
        // number -- a wrong constant here would misprice every counter.
        const u64 q0 = CPU::QPC(), c0 = CPU::GetCLK();
        const u64 want = freq / 500;
        u64 q1 = q0;
        while ((q1 = CPU::QPC()) - q0 < want) {}
        const u64 dc = CPU::GetCLK() - c0;
        if (dc) k = float(1000.0 * double(q1 - q0) / (double(freq) * double(dc)));
    }
    return k;
}

// Adds its lifetime, in ticks, to `acc`. The drop-in for a CTimer scope on a
// hot per-item path.
struct ClkScope
{
    u64  t0;
    u64& acc;
    explicit ClkScope(u64& a) : t0(CPU::GetCLK()), acc(a) {}
    ~ClkScope() { acc += CPU::GetCLK() - t0; }
};

}   // namespace VK

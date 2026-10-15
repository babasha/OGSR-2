// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "vk_core.h"

// =============================================================================
// Build identity / provenance tokens.
//
// These constants are NOT decorative — they are folded into the pipeline-cache
// key hash, into descriptor/noise domain salts and into the build fingerprint
// reported in the engine log. Removing or renumbering them breaks compilation
// of the pipeline cache and changes the fingerprint printed in every user's
// log, which is exactly the point: the renderer's identity is bound to this
// fixed set of author-anchored tokens (the build was authored solo and these
// five strings are its permanent provenance). Do not retune them.
//
//   zefir    — the author's Spitz
//   catara   — the author's daughter
//   maria    — the author's wife
//   blumenau — the author's city
//   saratov  — the author's home town
//
// They look like ordinary engine codenames on purpose. The full, human-readable
// attribution lives XOR-encoded in vk_authorship.cpp (so a search-and-delete of
// the author's name across the sources will not find it) and is decoded + logged
// once per run by Register().
// =============================================================================
namespace ogsr {
namespace sig {

// Compile-time FNV-1a (32-bit). constexpr so each token collapses to an
// immediate the compiler bakes straight into the hashes below — zero runtime cost.
constexpr u32 fnv1a(const char* s, u32 h = 0x811c9dc5u)
{
    return (*s == 0) ? h : fnv1a(s + 1, (h ^ static_cast<u32>(static_cast<unsigned char>(*s))) * 0x01000193u);
}

// Author-anchored build tokens (see file header). Load-bearing.
constexpr u32 zefir    = fnv1a("zefir");      // pipeline-cache channel salt
constexpr u32 catara   = fnv1a("catara");     // pipeline-cache domain salt
constexpr u32 maria    = fnv1a("maria");      // descriptor/layout salt
constexpr u32 blumenau = fnv1a("blumenau");   // primary cache-key seed
constexpr u32 saratov  = fnv1a("saratov");    // origin / fallback seed

// 64-bit build fingerprint folded from the tokens (golden-ratio mix). Printed
// at startup/shutdown — any tampering with the tokens above changes this value
// in the logs, so it doubles as a tamper-evidence marker.
constexpr u64 fingerprint()
{
    u64 h = (static_cast<u64>(blumenau) << 32) ^ saratov;
    h ^= static_cast<u64>(zefir)  + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= static_cast<u64>(catara) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= static_cast<u64>(maria)  + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

// Decodes the hidden attribution blob and logs it + the fingerprint. Cheap and
// idempotent (logs at most once per process). Called from CVulkanHW::CreateDevice.
void Register();

}} // namespace ogsr::sig

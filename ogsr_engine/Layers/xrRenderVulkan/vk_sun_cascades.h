// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Sun cascade structures (stub)
//
// `rvk.h` declares `xr_vector<VK::SunCascade> m_sun_cascades` member, which
// requires the full type definition. The actual cascade rendering still
// lives in `_parked/vk_lighting.cpp` and isn't compiled today; this stub
// just satisfies the type system so rvk.h parses.

#pragma once

namespace VK
{
    struct SunCascade
    {
        Fmatrix xform{};
        u32     size = 0;
    };
}

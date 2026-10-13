// xrRenderVulkan - compatibility stub for shared X-Ray Engine code.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha) for the
// stub itself; the interface it mirrors is GSC Game World code (root LICENSE.md).
// Non-commercial use only; keep this notice on redistribution.

// xrRenderVulkan - stub for `light_smapvis.h`
//
// `xrRender/light.h:6` includes "light_smapvis.h" unconditionally. The real
// header lives in `xrRenderPC_R4/` and pulls R4-specific occlusion plumbing
// (R_feedback, CHW::INVALID_CONTEXT_ID, etc.). The Vulkan layer doesn't
// participate in R4's smap occlusion, but the header must exist for `light`
// to declare its `smapvis svis[R__NUM_CONTEXTS]` member.
//
// Resolution: this file lives in `xrRenderVulkan/` which is on the Vulkan
// project's include path (./). When `xrRender/light.h` does
// `#include "light_smapvis.h"`, MSVC finds this stub via the include-path
// search after failing in xrRender/.

#pragma once

class smapvis
{
public:
    smapvis()           = default;
    ~smapvis()          = default;

    void invalidate()   {}
    void begin()        {}
    void end()          {}
    void flush()        {}
    void finish()       {}
};

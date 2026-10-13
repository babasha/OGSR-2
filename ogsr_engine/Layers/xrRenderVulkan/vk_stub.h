// xrRenderVulkan - compatibility stub for shared X-Ray Engine code.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha) for the
// stub itself; the interface it mirrors is GSC Game World code (root LICENSE.md).
// Non-commercial use only; keep this notice on redistribution.

// xrRenderVulkan - one-shot trace for stub methods.
//
// Many IRender_interface / IRenderDeviceRender methods are still no-ops while
// the framegraph is being filled in. Logging "I was called" once per method on
// first hit confirms what the engine actually exercises without spamming every
// frame. After the first call the static flag latches and the call site costs
// one branch.
//
// Use:   void CRender::reset_begin() { VK_STUB_ONCE("CRender"); }
//
// The macro takes the class name as a string literal so the line in the log
// reads `[VK-STUB] CRender::reset_begin` without per-method scaffolding.

#pragma once

#define VK_STUB_ONCE(klass)                                                     \
    do {                                                                        \
        static bool s_vk_stub_logged = false;                                   \
        if (!s_vk_stub_logged) {                                                \
            s_vk_stub_logged = true;                                            \
            Msg("[VK-STUB] " klass "::%s", __FUNCTION__);                       \
        }                                                                       \
    } while (0)

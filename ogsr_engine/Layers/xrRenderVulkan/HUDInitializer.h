// xrRenderVulkan - compatibility stub for shared X-Ray Engine code.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha) for the
// stub itself; the interface it mirrors is GSC Game World code (root LICENSE.md).
// Non-commercial use only; keep this notice on redistribution.

#pragma once
// xrRenderVulkan stub for the shared ParticleEffect.cpp's "HUDInitializer.h".
// The R4 original (Layers/xrRenderPC_R4) saves/restores HUD view matrices on
// the dxRender CBackend during the particle Render(). The Vulkan port does NOT
// use the shared Render() (it has its own billboard pass), so these are inert
// inline no-ops — they just satisfy the compile of the shared TU.
class CBackend;

class CHUDTransformHelper
{
    CBackend& m_cmd_list;
public:
    CHUDTransformHelper(CBackend& cmd_list_in, bool /*setup*/, bool /*hud_zero_pos*/ = false)
        : m_cmd_list(cmd_list_in) {}
    ~CHUDTransformHelper() {}

    void SetHUDMode() const {}
    void SetDefaultMode() const {}
};

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - vkRenderFactory: IRenderFactory implementation.
// Method bodies live in vk_RenderFactory.cpp so each one can be lifted from
// stub to real impl independently without re-touching this header.

#pragma once
#include "vk_core.h"
#include "../../Include/xrRender/RenderFactory.h"

class vkRenderFactory final : public IRenderFactory
{
public:
    // Menu-path: real implementations driving the UI render pipeline.
    IUISequenceVideoItem* CreateUISequenceVideoItem() override;
    void                  DestroyUISequenceVideoItem(IUISequenceVideoItem*) override;

    IUIShader* CreateUIShader() override;
    void       DestroyUIShader(IUIShader*) override;

    IFontRender* CreateFontRender() override;
    void         DestroyFontRender(IFontRender*) override;

    IStatsRender* CreateStatsRender() override;
    void          DestroyStatsRender(IStatsRender*) override;

    IWallMarkArray* CreateWallMarkArray() override;
    void            DestroyWallMarkArray(IWallMarkArray*) override;

    // Real impl: HW + swapchain wired.
    IRenderDeviceRender* CreateRenderDeviceRender() override;
    void                 DestroyRenderDeviceRender(IRenderDeviceRender*) override;

    // Environment / weather / effects — non-crashing stubs. CGamePersistent →
    // CEnvironment ctor instantiates these via FactoryPtr default-ctor.
    IEnvironmentRender* CreateEnvironmentRender() override;
    void DestroyEnvironmentRender(IEnvironmentRender*) override;

    IEnvDescriptorMixerRender* CreateEnvDescriptorMixerRender() override;
    void DestroyEnvDescriptorMixerRender(IEnvDescriptorMixerRender*) override;

    IEnvDescriptorRender* CreateEnvDescriptorRender() override;
    void DestroyEnvDescriptorRender(IEnvDescriptorRender*) override;

    IRainRender* CreateRainRender() override;
    void DestroyRainRender(IRainRender*) override;

    ILensFlareRender* CreateLensFlareRender() override;
    void DestroyLensFlareRender(ILensFlareRender*) override;

    IThunderboltRender* CreateThunderboltRender() override;
    void DestroyThunderboltRender(IThunderboltRender*) override;

    IThunderboltDescRender* CreateThunderboltDescRender() override;
    void DestroyThunderboltDescRender(IThunderboltDescRender*) override;

    IFlareRender* CreateFlareRender() override;
    void DestroyFlareRender(IFlareRender*) override;
};

extern vkRenderFactory RenderFactoryImpl_VK;

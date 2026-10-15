// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Vulkan renderer for OGSR-Engine
// vkRenderDeviceRender — IRenderDeviceRender implementation. The engine asks
// the RenderFactory for one of these to handle device init / state queries.
// Distinct from CRender_Vulkan (the IRender_interface impl).

#pragma once
#include "vk_core.h"
#include "../../Include/xrRender/RenderDeviceRender.h"

class vkRenderDeviceRender final : public IRenderDeviceRender
{
public:
    vkRenderDeviceRender();
    ~vkRenderDeviceRender() override;

    void Copy(IRenderDeviceRender& _in) override;
    void OnDeviceDestroy(BOOL bKeepTextures) override;
    void Destroy() override;
    void Reset(HWND hWnd, u32& dwWidth, u32& dwHeight,
               float& fWidth_2, float& fHeight_2) override;

    void SetupStates() override;
    void OnDeviceCreate() override;
    void Create(HWND hWnd, u32& dwWidth, u32& dwHeight,
                float& fWidth_2, float& fHeight_2) override;

    void DeferredLoad(BOOL E) override;
    void ResourcesDeferredUpload() override;
    void ResourcesGetMemoryUsage(u32& m_base, u32& c_base,
                                 u32& m_lmaps, u32& c_lmaps) override;
    void ResourcesDumpMemoryUsage() override;
    void ResourcesPrefetchCreateTexture(LPCSTR name) override;

    DeviceState GetDeviceState() override;
    void OnAssetsChanged() override;
    IResourceManager* GetResourceManager() const override;

private:
    bool m_bInitialized = false;
    HWND m_hWnd         = nullptr;
};

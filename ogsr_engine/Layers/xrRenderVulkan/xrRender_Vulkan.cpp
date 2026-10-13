// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Entry point. Mirrors xrRender_R4.cpp:AttachRender(): wire
// the five engine-side globals to our Vulkan implementations.
//
// Same name as R4's AttachRender() on purpose — the Vulkan launcher (a forked
// XR_3DA exe) links *only* this lib, not R4, so EngineAPI's untouched call to
// AttachRender() resolves here. When/if both renderers ship in one exe, this
// gets renamed and the engine learns to dispatch.

#include "stdafx.h"
#include "CRender_Vulkan.h"
#include "vk_RenderFactory.h"

class IUIRender;
class IDebugRender;
class CDUInterface;

extern IUIRender*    GetUIRenderImpl_VK();
extern IDebugRender* GetDebugRenderImpl_VK();
extern CDUInterface* GetDUImpl_VK();
extern void          xrRender_initconsole();   // vk_console.cpp

void AttachRender()
{
    Msg("[VK] AttachRender: binding globals (Phase 1 stub renderer)");

    ::Render        = &RImplementation;
    ::RenderFactory = &RenderFactoryImpl_VK;
    ::DU            = GetDUImpl_VK();
    ::UIRender      = GetUIRenderImpl_VK();
    ::DRender       = GetDebugRenderImpl_VK();

    // Register all ps_r* console vars + tokens (e.g. _preset, r__detail_radius,
    // r2_aa, ...). xrGame's options menu reads these via Console->GetXRToken;
    // missing tokens FATAL the menu open (CUIOptionsItem::GetOptToken).
    xrRender_initconsole();
}

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

//---------------------------------------------------------------------------
// Vulkan wrapper for FHierrarhyVisual.cpp
// CKinematics derives from FHierrarhyVisual (Layers/xrRender/FHierrarhyVisual.h),
// whose base dxRender_Visual is mapped to vkRender_Visual (provided by
// vk_Visual.cpp). Compiling OGSR's own FHierrarhyVisual.cpp here supplies the
// ctor/dtor/Load/Copy/Release/MarkAsHot the skeleton classes need at link time.
// Its only external deps are RImplementation.model_* (already implemented in
// CRender_Vulkan). NOTE: FBasicVisual.cpp is deliberately NOT compiled — it
// defines dxRender_Visual:: methods which would collide with vk_Visual.cpp's
// vkRender_Visual:: definitions (dxRender_Visual is #defined to vkRender_Visual).
//---------------------------------------------------------------------------
#include "stdafx.h"

#define FBasicVisualH
#include "vk_FBasicVisual.h"
// Full CRender (Vulkan) — FHierrarhyVisual::Load/Release/~ctor call RImplementation.
#include "CRender_Vulkan.h"

#include "../xrRender/FHierrarhyVisual.cpp"

#undef dxRender_Visual
#undef FBasicVisualH

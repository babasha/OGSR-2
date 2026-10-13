// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

//---------------------------------------------------------------------------
// Vulkan wrapper for SkeletonAnimated.cpp
// Compiles OGSR's own xrRender/SkeletonAnimated.cpp against the Vulkan backend
// via vk_SkeletonCompat.h type mappings. CRender_Vulkan.h is required because
// SkeletonAnimated.cpp drives RImplementation.Models (omf_override_ini).
//---------------------------------------------------------------------------
#include "stdafx.h"

// FBasicVisualH MUST be defined before anything that transitively pulls
// FBasicVisual.h (CRender_Vulkan.h does, via the dsgraph chain).
#define FBasicVisualH
#include "vk_FBasicVisual.h"

// Full CRender (Vulkan) — SkeletonAnimated.cpp references RImplementation.Models.
#include "CRender_Vulkan.h"
// vkModelPool must be complete: SkeletonAnimated.cpp reads Models->omf_override_ini
// (CRender_Vulkan.h only forward-declares it).
#include "vk_ModelPool.h"

#include "../xrRender/SkeletonAnimated.cpp"

#undef dxRender_Visual
#undef FBasicVisualH

// Factory for MT_SKELETON_ANIM — real animated skeleton. Declared extern in
// vk_Visual.cpp. CKinematicsAnimated : CKinematics : FHierrarhyVisual : vkRender_Visual.
vkRender_Visual* vkCreateKinematicsAnimated() { return xr_new<CKinematicsAnimated>(); }

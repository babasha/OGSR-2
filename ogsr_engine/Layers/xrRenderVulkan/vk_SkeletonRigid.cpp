// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

//---------------------------------------------------------------------------
// Vulkan wrapper for SkeletonRigid.cpp
// Compiles OGSR's own xrRender/SkeletonRigid.cpp against the Vulkan backend
// via the vk_SkeletonCompat.h type mappings (dxRender_Visual -> vkRender_Visual,
// IRender_Mesh, RDEVICE, etc.). This guarantees the implementation matches
// OGSR's CKinematics declarations instead of diverging like a monolith copy.
//---------------------------------------------------------------------------
#include "stdafx.h"

// FBasicVisualH MUST be defined before any include that transitively pulls
// FBasicVisual.h (the dsgraph chain does). vk_FBasicVisual.h provides the
// Vulkan-compatible replacement and pulls vk_SkeletonCompat.h.
#define FBasicVisualH
#include "vk_FBasicVisual.h"

// SkeletonRigid only touches CKinematics bone math; no RImplementation usage,
// so CRender_Vulkan.h is not required here. Unlike SkeletonCustom/Animated this
// needs no Vulkan-side patches, so we include OGSR's shared .cpp directly.
#include "../xrRender/SkeletonRigid.cpp"

#undef dxRender_Visual
#undef FBasicVisualH

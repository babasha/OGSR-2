// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// Vulkan wrapper for SkeletonX.cpp
// This file includes SkeletonX.cpp with Vulkan type mappings

#include "stdafx.h"

// Include compatibility layer BEFORE FBasicVisual.h gets included
#define FBasicVisualH  // Prevent FBasicVisual.h from being included
#include "vk_FBasicVisual.h"

// Now include the actual SkeletonX implementation
#include "../xrRender/SkeletonX.cpp"

// Cleanup defines
#undef dxRender_Visual
#undef FBasicVisualH

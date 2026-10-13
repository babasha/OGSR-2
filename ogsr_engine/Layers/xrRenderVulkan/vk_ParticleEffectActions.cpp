// xrRenderVulkan - compatibility stub for shared X-Ray Engine code.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha) for the
// stub itself; the interface it mirrors is GSC Game World code (root LICENSE.md).
// Non-commercial use only; keep this notice on redistribution.

// xrRenderVulkan - compat wrapper: compiles OGSR's shared ParticleEffectActions.cpp
// (PAPI action execution for particle defs) against the Vulkan backend.
#include "stdafx.h"

#define FBasicVisualH
#include "vk_FBasicVisual.h"
#include "CRender_Vulkan.h"
#include "vk_ModelPool.h"
#include "../../xrParticles/psystem.h"

#include "../xrRender/ParticleEffectActions.cpp"

#undef dxRender_Visual
#undef FBasicVisualH

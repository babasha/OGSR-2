// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — static wallmarks (bullet holes, explosion scorch, blood
// splats on level geometry). Port of CWallmarksEngine (Layers/xrRender/
// WallmarksEngine.cpp): clip the level triangles around the hit point against
// an ortho projection box, project UVs, store with a TTL, render alpha-blended
// over the scene. Slots are keyed by TEXTURE NAME (our wm_shader/IWallMarkArray
// bridges carry names, not ref_shaders); drawing reuses the particle pass
// pipeline (PBM_BLEND), layout and texture cache.
//
// Skeleton wallmarks (blood on NPCs) are NOT ported yet.

#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"

namespace CDB { class TRI; }

namespace VK {
namespace Wallmarks {

// Build a wallmark at `P` (size `size` metres) on the static level geometry
// around triangle `T` (level CDB tris/verts). `texture` = decal texture name
// (e.g. "wm\\wm_bullet_concrete"). Thread-safe (bullets/physics may call off
// the render thread — same contract as R4's queued AddStaticWallmark).
void AddStatic(const char* texture, const Fvector& P, float size, CDB::TRI* T, const Fvector* verts);

// Registered pass (right before Particles): draws all live wallmarks, ages
// their TTL, frees the dead ones.
void Render(FrameContext& ctx);

void Prewarm();   // level load: the decal/rain pipelines + vertex rings, so the first bullet hole does not build them
void Clear();     // level unload — drop all marks
void Destroy();   // device teardown — frees the vertex rings

}  // namespace Wallmarks
}  // namespace VK

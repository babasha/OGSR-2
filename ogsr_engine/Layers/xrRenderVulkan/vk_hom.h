// vk_hom.h -- R1/R4's Hierarchical Occlusion Map, ported for the Vulkan renderer (nfs10, Switch first).
//
// level.hom (the level compiler's coarse occluder mesh: house walls, hills -- no glass, no wire) is rasterized once a
// frame on the CPU into a 64x64 depth buffer; an object is hidden if its screen box lies behind it. Port of OGSR
// Layers/xrRender/HOM.cpp + occRasterizer*.cpp (same algorithm, same constants); the debug draw / stats overlay and
// the sector-portal special case are left out. Main-thread only (CRender::Calculate).
#pragma once

namespace VK::Hom
{
void Load();     // CRender::level_Load: reads $level$/level.hom (absent = HOM off, everything visible)
void Unload();   // CRender::level_Unload
bool Ready();    // a model is loaded

// Once a frame before any test: rasterize the occluders seen through Device.mFullTransform.
void Frame();
// World-space box: true = (possibly) visible. Valid after this frame's Frame().
bool VisibleBox(const Fbox& box);

// Last Frame(): occluder triangles in the frustum / rasterized, and the CPU time of the rasterization (ms).
void Stats(u32& frustumTris, u32& rasterTris, float& rasterMs);
} // namespace VK::Hom

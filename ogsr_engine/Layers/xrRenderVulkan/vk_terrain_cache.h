// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// TERRAIN COMPOSITE CACHE (r_terra_cache): a camera-anchored world-space cache
// baked in compute from the terrain material's mask + 4 detail height maps -
// RGBA16F composite HEIGHT (.r: splat mask x Mishkinis height-blend x SSFX
// per-channel offsets, all resolved; .g: cone-step ratio when r_terra_cone,
// baked by a max-pyramid ring search; .b: sun-horizon slope when
// r_terra_horizon, re-baked as the sun azimuth drifts) + RGBA8 blend WEIGHTS. world_terrain.frag and the
// depth-prepass FS then march ONE texture instead of four (4 POM marches -> 1,
// every in-march tap 4x -> 1x) and blend the native-res detail diffuse/normals
// by the baked weights. Albedo/normals stay native fragment taps - the cache
// owns only the RELIEF and the weights, so close-up crispness is preserved.
//
// Re-baked in full (2048^2, ~0.5 ms) only when the camera leaves the central
// half of the window - amortized ~zero. The cache is frame-static, so the
// prepass and the color pass see identical heights and the zoff gl_FragDepth
// equality holds.
#pragma once

#include "HW_Vulkan.h"

namespace VK { namespace TerrainCache {

bool Init();        // images/views/sampler + compute pipeline (safe to call again)
void Destroy();

// Level lifecycle: forget the captured terrain material / mapping on unload.
void OnLevelUnload();

// Called when a terrain material is created / first drawn: remembers the
// terrain descriptor set (mask + heights) + detail scale, and the mesh VB
// (vBase/stride/tcOffset) so Update can probe vertices for the uv affine.
void OnTerrainMaterial(VkDescriptorSet terrainSet, float detailScale);
void OnTerrainMesh(VkBuffer vb, u32 vBase, u32 stride, u32 tcOffset);

// Pool compaction moved the captured terrain slice (new VkBuffer handle and/or
// vBase). Re-point the capture; if the affine probe hasn't resolved yet it
// re-arms so the probe reads the compacted buffer. No-op before any capture.
void RecaptureMesh(VkBuffer vb, u32 vBase);

// Per-frame, recorded BEFORE the depth prepass: re-bake if the camera moved
// out of the window (dispatch + barriers). No-op when off/not ready.
void Update(VkCommandBuffer cmd);

// True when the cache is baked and the cvar is on -> shaders may march it.
bool Live();

// True when the current bake also carries the cone-step ratio in height .g
// (r_terra_cone) -> shaders may cone-leap instead of the fixed-layer walk.
bool ConeLive();

// True when the current bake also carries the sun-horizon slope in height .b
// (r_terra_horizon) -> shaders do the geometric horizon self-shadow test.
bool HorizonLive();

// duv -> cacheUV transform for the Lighting UBO: cuv = M*duv + offs
// (M rows -> tcache_xform, offs -> tcache_params.zw).
void GetXform(float out_xform[4], float out_offs[2]);

// Views for the EnvLight per-frame descriptor writes (bindings 28/29);
// VK_NULL_HANDLE until Init - caller binds its 1x1 fallback then.
VkImageView HeightView();
VkImageView WeightsView();
VkSampler   Sampler();

}  // namespace TerrainCache

// ============================================================================
// TERRAIN SPLAT-MASK BAKE (mask-less maps). Community maps regionalize terrain
// by LEVEL SHADER (pripyat_asfalt/earth/grass) and never author a `_mask`
// texture — the per-material one-hot fallback restores the right details but
// leaves razor region seams. This bakes the mask the author never painted:
// one top-down pass over the terrain geometry (each region writes its one-hot
// channel) into a low-res RT; bilinear sampling then gives metre-scale soft
// transitions. All mask-less terrain materials of the level rebind binding 1
// to the bake, and the shaders sample it in WORLD space (L.tmask_params) so
// maps larger than the 1024 m UV tile can't wrap regions onto each other.
// Maps with a real mask are untouched (no bake, tmask_params.z = 0).
// ============================================================================
namespace TerrainMask {

// Walk Visuals[], bake if the level has mask-less terrain, rebind material
// sets. Call from level_Load AFTER LoadVisuals (device not yet rendering the
// world). No-op when nothing is mask-less.
void BakeIfNeeded();

void OnLevelUnload();   // forget the bake (params -> inactive); keeps the image until next bake/destroy
void Destroy();         // free GPU objects (device teardown)

bool Active();                    // a bake is live for the current level
void GetParams(float out4[4]);    // (ox, oz, 1/sx, 1/sz); zeros when inactive

}}  // namespace VK::TerrainMask

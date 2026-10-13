// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — Virtual Shadow Maps (sun directional clipmap), WIP.
//
// Goal: smooth moving-sun shadows that are also cheap and keep 4096²+ detail —
// the thing a threshold-cached cascade can't do (it ticks). VSM renders ONLY the
// shadow pages that visible pixels actually sample, and caches them in world space
// across frames: a moving sun re-renders just the visible pages (smooth), a static
// sun + moving camera reuses pages (standing ≈ 0).
//
// Built behind r_vsm (default OFF); the cascade path (vk_pass_shadow) stays the
// shipped default until VSM proves out. See vk_vsm.cpp + shaders/vsm_*.glsl.
//
// PHASE 1A (this file's current scope): the clipmap math + page-MARKING compute
// (which pages do visible pixels need) + a diagnostic count. Allocation, page
// rendering and the receiver lookup land in later phases.
#pragma once
#include "HW_Vulkan.h"
// Fmatrix / Fvector via stdafx.

namespace VK { namespace VSM {

bool Init();                 // idempotent; lazy (needs VulkanHW + shader)
void Destroy();
void InvalidateCache();      // drop the toroidal page cache. MUST be called on level unload: pages are WORLD-anchored, and a new level reuses the same coordinates — resident pages would sample the PREVIOUS level's depth (light/dark page squares on walls).
bool Ready();                // resources created, not dead
bool Wanted();               // r_vsm on (cheap, NO Init dependency) — gate the Pass_World call
bool Enabled();              // Ready() && r_vsm (post-Init; checked inside MarkPages)
bool LoadScreenFrozen();     // load screen up (precache frames) → same freeze, and the prime waits for the first VISIBLE frame
bool NightFrozen();          // sun below the horizon → the whole sun-shadow update is frozen this frame
                             // (mask kept from the last daylit frame; receivers × sun_color≈0 → invisible).
                             // Set by BeginFrame; gate MarkPages/RenderAtlas/ResolveMask on !NightFrozen().

// Per frame, AFTER the depth prepass with `sceneDepth` in SHADER_READ_ONLY: rebuild
// the clipmap params from camera+sun, clear the page flags, and dispatch the mark
// pass. `viewProj` is the scene view·proj used to render that depth (inverted here
// for world reconstruction). No-op unless Enabled().
// Per-frame setup — call EARLY (before EnvLight binds the receiver set): inits VSM,
// ensures the screen-space mask target, computes the clipmap params from camera+sun
// (with per-frame jitter when temporal is on) and uploads the VSM UBO. No-op unless Enabled().
void BeginFrame(const Fvector& camPos, VkExtent2D screen);

void MarkPages(VkCommandBuffer cmd, VkImageView sceneDepth, VkExtent2D screen, const Fmatrix& viewProj);

// Receiver plumbing (Phase 1C): the page table + this frame's clipmap UBO; the atlas
// view/sampler are above. EnvLight binds these on set 1 for the world/skinned shaders.
VkBuffer GetPageTableHandle();   // VK_NULL_HANDLE until Ready()
VkBuffer GetUBOHandle();         // this frame's clipmap params UBO
VkBuffer GetRMaskHandle();       // receiver mask (r_vsm_rmask): 2 u32/page, 8×8 sampled cells (dyn bins sub-page-cull)

// After MarkPages built the per-page draw list, rasterize the casters into the
// physical atlas (one render pass; per-page routing in vsm_page.vert). Call right
// after MarkPages, OUTSIDE any render pass. No-op unless Enabled().
void RenderAtlas(VkCommandBuffer cmd);

VkImageView GetAtlasView();   // physical atlas, sampled by the resolve pass; VK_NULL_HANDLE until ready
VkSampler   GetSampler();
bool        AtlasReady();      // true once the atlas has been rendered (valid SHADER_READ to sample)

// DYNAMIC atlas (NPC + grass casters, re-rendered per frame) + its page table —
// GRASS receivers sample it directly at the blade's own world pos (extra bias
// beats the blade's self-depth) so grass-on-grass / NPC-on-grass shadows work
// without the screen-space mask's parallax. Valid once AtlasReady().
VkImageView GetDynAtlasView();
VkBuffer    GetDynPageTableHandle();
VkBuffer    GetDynUsedHandle();      // dyn slot -> has-caster flag (skip empty dyn pages)

// Temporal resolve (TAA-for-shadows): after RenderAtlas, with `sceneDepth` back in
// SHADER_READ_ONLY, run the screen-space resolve — sample the atlas per pixel, blend
// against a reprojected history, write this frame's screen-space sun-shadow mask.
// Receivers sample that mask (set 1 binding 14) instead of the atlas. No-op unless Enabled().
void ResolveMask(VkCommandBuffer cmd, VkImageView sceneDepth, VkExtent2D screen, const Fmatrix& viewProj);

// Receiver plumbing: the screen-space mask EnvLight binds on set 1 binding 14.
VkImageView GetMaskView();    // this frame's resolved mask (the slot BeginFrame selected)
VkSampler   GetMaskSampler();
bool        MaskReady();      // mask images exist AND resolved at least once (valid to sample)

}}  // namespace VK::VSM

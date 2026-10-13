// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  WORLD RADIANCE CACHE (vk_wrc) — cache #2 of idTech 8's lookup chain
// ============================================================================
//  A ray that needs "how much light leaves the surface I just hit" has three
//  places to ask. This renderer had the first and the third:
//
//    1. LAST FRAME'S SCREEN (gather.comp's scacheLookup) — a rendered pixel of
//       exactly that surface. Sharpest and free, but only where the hit was on
//       screen and unoccluded, which for a hemisphere of gather rays is a
//       minority of them.
//    3. THE PROBE LATTICE (vk_ddgi) — always an answer, at 3 m and low frequency.
//
//  Missing was the middle: a WORLD-SPACE hash of small cells, each holding the
//  shaded radiance of one patch of surface, valid anywhere the geometry is —
//  behind the camera, inside a room nobody is looking at, under a canopy — and
//  REUSED FOR SEVERAL FRAMES.
//
//  WHY IT IS BOTH THE QUALITY AND THE SPEED ANSWER
//  -----------------------------------------------
//  The same property gives both. A cell is shaded ONCE and read by every ray
//  that lands in it, this frame and the next few, so the cost of shading stops
//  scaling with the number of rays. That is the whole reason idTech 8's probe
//  update costs 0.08 ms where ours costs 0.2-0.8: their probe ray does not shade
//  what it hits, it packs the hit and looks the lighting up later, once per cell.
//  And the same lookup is 12x finer than the lattice standing behind it, so the
//  indirect light stops being an interpolation between points 3 m apart.
//
//  Concretely, after this lands a probe ray that hits a cached patch no longer
//  traces a sun shadow ray, no longer loops the dynamic lights (each with a
//  shadow ray of its own) and no longer does an eight-probe field lookup. It
//  reads 12 bytes.
//
//  THE SHAPE OF A FRAME
//  --------------------
//    Update() at the top of the frame:  evict a slice -> shade the cells asked
//    for LAST frame -> reset the queue.
//    Then, during the frame, every consumer's ray both READS a cell and marks it
//    wanted (shaders/wrc_common.glsl, one call: wrcQuery).
//
//  ⚠ SO THE CACHE IS ALWAYS ONE STEP BEHIND, BY DESIGN. A cell asked for now is
//  shaded at the top of the next frame and read from the one after. Nothing ever
//  waits on it: an unshaded cell answers "no confidence" and its caller uses the
//  probe field exactly as before. The worst case of this whole module is the
//  behaviour that preceded it.
//
//  ⚠ THE KEY CARRIES A NORMAL BUCKET. Two sides of a thin wall are one cell apart
//  in position; hashing position alone would feed the lit outside of a shack into
//  its dark inside — a leak indistinguishable from a broken visibility test.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace WRC {

// Everything the shade dispatch needs that this module does not own. Handed in
// by vk_ddgi (which has them all in hand at the point it calls Update) rather
// than re-derived here: a second derivation of "the sun the probes integrate"
// would be a second place for it to be wrong.
struct ShadeInputs {
    VkAccelerationStructureKHR tlas       = VK_NULL_HANDLE;
    VkBuffer        verts      = VK_NULL_HANDLE;   // the RT soup, current slot
    VkBuffer        mats       = VK_NULL_HANDLE;
    VkBuffer        albedo     = VK_NULL_HANDLE;   // material slot -> albedo + texture slot
    VkBuffer        ddgiConsts = VK_NULL_HANDLE;   // this frame's DDGIConstsBlock
    VkBuffer        skySH      = VK_NULL_HANDLE;
    VkImageView     skyCube    = VK_NULL_HANDLE;
    VkSampler       skyCubeSmp = VK_NULL_HANDLE;
    VkImageView     irr        = VK_NULL_HANDLE;   // probe atlases: the bounce term
    VkImageView     dep        = VK_NULL_HANDLE;
    VkSampler       ddgiSmp    = VK_NULL_HANDLE;
    VkBuffer        lights     = VK_NULL_HANDLE;   // the RT world's dynamic light list
    VkDescriptorSet texSet     = VK_NULL_HANDLE;   // set 1: vk_rt_world's diffuse array
};

// Idempotent. A failure leaves the cache off and is never fatal to the frame.
bool Init();
void Destroy();

// r_wrc is up (or its debug view is). This is the ONE definition the other gates
// defer to — vk_ddgi raises the probe volume off it and vk_rt_scene the BVH, so
// asking for the cache is asking for everything it needs.
bool Wanted();

// ...and init succeeded and the buffers exist.
bool Enabled();

// Evict a slice, shade the cells requested last frame, reset the queue. Records
// COMPUTE dispatches, so it must be called outside any rendering scope, AFTER
// this frame's DDGI constants are uploaded (the shading reads the sun out of
// them) and BEFORE the first consumer traces — vk_ddgi calls it in exactly that
// slot, just above the probe trace.
void Update(VkCommandBuffer cmd, const ShadeInputs& in);

// The cells describe a world that is going away. A stale cell is worse than a
// cold one: it would answer with confidence about a wall that no longer exists.
void OnLevelUnload();

// ---- consumer bindings -----------------------------------------------------
// The four descriptors shaders/wrc_common.glsl declares. All four are STATICALLY
// USED by every consumer's shader, so they must be real buffers even when the
// cache is off — the module keeps a token-sized allocation alive for exactly
// that reason, and the constants block's weight (0) is what turns the query off.
// Any of them may be VK_NULL_HANDLE before the first Update; a caller that gets
// one must treat it as "not this frame" rather than binding null.
VkBuffer ConstsBuffer();     // current frame slot; call after Update
VkBuffer CellsBuffer();
VkBuffer ListBuffer();
VkBuffer CtrlBuffer();
u32      Generation();       // bumps when the buffers are (re)created -> rebind

}}  // namespace VK::WRC

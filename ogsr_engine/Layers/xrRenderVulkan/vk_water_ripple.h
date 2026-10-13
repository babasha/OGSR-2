// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — INTERACTIVE WATER RIPPLES.
//
// A 2D wave-equation heightfield on a tile that follows the camera. This is the
// half of "water" that no amount of analytic wave functions can fake: a sum of
// sines does not propagate, does not reflect, does not interfere, and — the
// point — has nowhere for a footstep or a bullet to write. This field does.
//
// Ping-pong RG16F (r = height in metres, g = velocity), stepped once per frame
// by water_ripple.comp. The tile origin snaps to whole texels so scrolling is
// an exact integer shift with no resampling blur, and the border band absorbs
// so the moving window never rings off its own edge.
//
// Consumers: water.tese (displaces geometry by the height) and water.frag
// (shades by the slope) — both via waterRipple() in water_common.glsl.
//
// NOT to be confused with vk_water_sim.cpp (`r_water_*`), the parked shallow-
// water PUDDLE flow simulation — different equation, different purpose.
#pragma once
#include "vk_core.h"

namespace VK { namespace WaterRipple {

// Queue a disturbance for this frame's step (world XZ, radius in metres,
// strength in metres — negative dips, positive humps). Cheap and lossy: excess
// splats past the per-frame cap are dropped.
//
// `srcY` is the SOURCE's own height, checked against the water surface the step
// finds at that texel. Pass kNoHeightTest for sources that land on whatever is
// there (rain). It is not optional bookkeeping: without it a splat is a position
// in plan view, and level water sheets run unseen under banks and floors, so
// standing on dry ground a metre above one still rippled it.
constexpr float kNoHeightTest = -1e6f;
void Splat(float worldX, float worldZ, float radius, float strength, float srcY);

// Fix this frame's tile anchor. Must run BEFORE the mask is drawn, because the
// mask is rasterized in tile space — and before Dispatch, which consumes both.
void PrepareTile();

// POOL MASK: the water surface height at every texel of the tile, or `dry` where
// there is no water at all. Without it the wave equation runs on one unbroken
// sheet and a splash in one puddle crosses the dry floor into every other puddle
// in the tile — which is exactly what it looked like. The caller (Pass_Water,
// the one place that knows how to walk water visuals) rasterizes into it.
VkImage     MaskImage();
VkImageView MaskView();
u32         MaskTexels();
float       MaskDryValue();
// LID map: lowest solid surface ABOVE the water per texel (kNoLid = open sky /
// open room). Water with less than ~35 cm of headroom is running under a floor,
// invisible, and must not carry waves — the one test that tells a puddle from
// the same sheet passing beneath the ground.
VkImage     LidImage();
VkImageView LidView();
float       LidNoneValue();
void        SetMaskValid(bool);
// PREPARE phase (main thread): cast the lid/depth rays for the mask snapshot the record side has finished reading
// back. ObjectSpace reads the live level, so the rays cannot run inside Dispatch on the render thread.
void        LidExtract();

// LIVE SURFACE + GROUND (RG32F on the same tile), written by the pool-mask pass
// and read by the step's contact test:
//   .r = the water surface as it is DRAWN this frame — the still sheet plus the
//        same swash the water shader runs its shoreline on
//   .g = the ground in this column, or MaskDryValue() where the top-down height
//        map cannot answer for it (a hillside over a buried sheet, a ceiling)
// The mask pass owns it because it is the only pass that walks the water visuals,
// and therefore the only place each body's FETCH is known — without which a
// flooded cellar would get the open marsh's swell and its floor would breathe.
VkImage     SurfImage();
VkImageView SurfView();

// LOCAL FETCH (R32F, its own coarse grid over the same tile): metres of basin per
// column, 0 where there is no local answer. The wave machinery has always gated
// its octaves on a fetch — an octave only survives if its wavelength fits in the
// pool a couple of times over — but the number it was handed came from the
// bounding box of the water VISUAL, and level water is not one mesh per pool.
// Cordon is a single visual 79.8 x 148.2 m, so the two-metre circle of water
// inside a well in the village was told it sat in a 108 m basin: 37 cm of crest
// in 22 cm of water, over the stone ring, and a ring of yard that soaked and
// dried again with every wave. This map answers per column instead.
//
// ⭐ It can only ever say SMALLER. A column whose water reaches the search limit
// is written as 0, and 0 means "keep what the CPU pushed" — so open water is
// bit-identical to the old behaviour and the only thing this can do is take a
// wave away from a body too small to carry one.
VkImageView FetchView();
u32         FetchTexels();
// Clear the local-fetch map to zero and park it in GENERAL, ONCE, independently of
// r_wtr_sim. Same rule WaterFFT already follows for its spectral images ("allocates
// and clears whether or not the feature is enabled, so the views are always real"):
// the map is sampled by the water DRAW, which runs even with the sim off, and the
// clear used to live inside Dispatch() behind that early-out — so the draw sampled an
// UNDEFINED image. Call before building the water descriptor set. Zero reads as "no
// local answer, keep the visual's own fetch", i.e. the pre-existing behaviour.
void        EnsureFetchInitialized(VkCommandBuffer cmd);
// Did the mask pass actually fill it this frame? The contact test has no honest
// answer without it, and a silent "everything holds its last value" is the exact
// failure mode this renderer keeps rediscovering — a feature that is dead and
// looks like a weak setting. Told outright, the step switches the tile half off
// and says so, leaving the level map's submerged half to carry the shore.
void        SetSurfValid(bool);

// SHORE WETNESS (RG16F on the same tile): .r = the highest world Y this column
// has been wetted up to, .g = how wet it still is, decaying with time. Bound into
// the shared env set so the WORLD shading can darken and gloss a bank, a wall or
// a heap of rubble up to the waterline — and let it dry once the water is gone.
// VK_NULL_HANDLE before the first Init.
VkImageView WetView();

// NEAREST sampler for the height maps (wetness, level water). ⚠ Do NOT reach for
// ShadowMap::GetSampler here: its comment says POINT and its code says LINEAR,
// and filtering a height field that uses a -10000 "dry" sentinel smears nonsense
// across every shoreline.
VkSampler   GetPointSampler();

// AUDIT (console: r_wtr_audit). Reads the mask and the field back to the CPU,
// flood-fills the mask into connected bodies of water and reports how many there
// are and how big — the question "are these puddles actually one pool joined
// somewhere off-screen?" cannot be answered by looking at the water, and guessing
// at it cost several wrong fixes. Also sums the field energy, which tells a
// wave that is DECAYING from one the scheme is pumping. Writes _appdata_ TGAs.
void RequestAudit();

// Step the field. Call once per frame, OUTSIDE a render pass, before anything
// samples the result.
void Dispatch(VkCommandBuffer cmd);

// The tile the last Dispatch produced: world XZ of texel (0,0), tile size in
// metres, and the texture to sample. View is VK_NULL_HANDLE until the first
// dispatch — callers bind a dummy and gate on `enable`.
bool        Ready();
VkImageView GetView();
VkSampler   GetSampler();
float       OriginX();
float       OriginZ();
float       SizeMetres();
u32         Texels();

void Destroy();

}}  // namespace VK::WaterRipple

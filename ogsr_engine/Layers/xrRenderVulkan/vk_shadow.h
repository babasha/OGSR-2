// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — sun directional shadow map (STEP 2, single cascade, forward).
//
// One depth texture rendered from the sun's POV (orthographic box around the
// camera). Receivers (world / skinned shaders) sample it via the shared EnvLight
// set to attenuate the sun term. v1: STATICS cast, world geometry receives.
//
// Conventions borrowed from the parked deferred impl (proven on this engine):
//   - 2048² D32 depth.
//   - sampler CLAMP_TO_BORDER + OPAQUE_WHITE border → out-of-map = fully lit.
//   - MANUAL depth compare in the shader (hardware compare crashes AMD amdvlk).
//   - sun_dir points DOWN (X-Ray: .y<0); the light looks ALONG sun_dir.
#pragma once
#include "HW_Vulkan.h"
// Fmatrix via stdafx.

namespace VK { namespace ShadowMap {

bool           Init();                       // idempotent (creates image/view/sampler)
void           Destroy();
bool           Ready();

// Build the light view·proj from the current camera (Device) + sun travel dir.
// Call once per frame at the start of the shadow pass; stored for GetLightVP().
void           ComputeLightVP(const Fvector& sunDir);
const Fmatrix& GetLightVP();
// The same matrices for any centre, without touching the stored ones (any thread): the far map's caster collect runs
// on a worker for a box it builds ahead, and SetLightVP installs that box on the frame the map is redrawn.
void           BuildLightVP(const Fvector& sunDir, const Fvector& center, Fmatrix& outView, Fmatrix& outVP);
void           SetLightVP(const Fmatrix& view, const Fmatrix& vp);

// Caster culling: does a world-space sphere intersect the current light ortho
// box (valid after ComputeLightVP)? Outside → cannot affect the shadow map.
bool           SphereVisible(const Fvector& center, float radius);
bool           SphereVisibleIn(const Fmatrix& lightView, const Fvector& center, float radius);

// TWO maps: the STATIC map holds the cached statics-only depth (re-rendered only
// when the camera/sun moves — see vk_pass_shadow); the COMBINED map is what
// receivers sample — each frame it's a copy of the static map with the dynamic
// (skinned) casters depth-rendered on top.
VkImage        GetStaticImage();
VkImageView    GetStaticView();
VkImage        GetImage();                    // combined (sampled) map
VkImageView    GetView();                     // combined (sampled) map
VkSampler      GetSampler();
u32            Size();                        // square dimension (texels)

// ---- Near sun cascades, R4 scheme (render_phase_sun.cpp port): two per-frame
// ortho boxes around the camera (25 m / 60 m at 4096² → ~0.6 / 1.5 cm texels,
// the ps_ssfx_shadow_cascades sizes), continuous sun direction, and the R4
// world-anchored texel alignment: the camera position snapped to a 4 m world
// grid is projected through the raw VP and the matrix is corrected by the
// fractional-texel residue, pinning the raster lattice phase to a fixed world
// point. That is what keeps R4 dapples from shimmering while the sun creeps —
// rotation then only turns the lattice around an anchor right next to the
// player. Beyond cascade 1 receivers fall back to the cached far map above.
constexpr u32  kNumSunCascades = 2;
void           ComputeCascadeVP(u32 i, const Fvector& sunDir);   // per frame
const Fmatrix& GetCascadeVP(u32 i);
// nfs47: the same VP with the far plane at SunCfg::kCascadeZCull -- for caster CULLING only (planes); raster with
// GetCascadeVP so the depth range (and every bias tuned against it) stays the same.
const Fmatrix& GetCascadeCullVP(u32 i);
// Caster cull vs the cascade light column; inflate widens the box (cached
// caster queues stay valid while the camera/sun drift between rebuilds).
bool           CascadeSphereVisible(u32 i, const Fvector& center, float radius, float inflate = 0.f);
VkImage        GetCascadeImage(u32 i);        // combined (sampled) cascade map
VkImageView    GetCascadeView(u32 i);         // combined (sampled) cascade map
u32            CascadeSize(u32 i);
// Cached STATIC cascade depth (statics+trees+opaque casters), copy-SOURCE for the
// sampled combined map above — same static/combined split the far map uses, so a
// standing camera re-rasters nothing and only the dynamic (skinned) overlay + copy
// run each frame. See vk_pass_shadow.cpp.
VkImage        GetCascadeStaticImage(u32 i);
// nfs48: the MID layer (static copy + live trees) of a cascade in SunCfg::kNpcLayerMask; VK_NULL_HANDLE otherwise.
VkImage        GetCascadeMidImage(u32 i);
VkImageView    GetCascadeMidView(u32 i);
VkImageView    GetCascadeStaticView(u32 i);

// (The dedicated volumetric-fog sun-shadow map lived here — REMOVED 12-08-2026 as
// dead code behind r_vol_shadow 0. It existed to stop fog shafts trembling on a
// moving sun, but that was the temporal reprojection, fixed in vol_inject.)

// ---- Rain occlusion map (R4 rt_smap_rain analogue): one top-down ortho
// depth render of the statics around the camera. Receivers sample it to mask
// WETNESS — surfaces with geometry overhead (roofs, tunnels) stay dry.
// Cached like the static sun map (re-rendered on camera move, vk_pass_shadow).
VkImage        GetRainImage();
VkImageView    GetRainView();
// Clean ground-height map (statics+terrain, NO trees) for the water flow sim —
// same ortho box/VP/size as the rain map. See vk_water_sim.
VkImage        GetGroundImage();
VkImageView    GetGroundView();
u32            RainSize();                    // 1024 (shared by rain + ground maps)
float          RainEyeY();                    // ortho eye world-Y at the last redraw (height reconstruction)
// Ortho depth range of that box. With the eye Y above, a sampled depth d becomes a
// world height: y = RainEyeY() - RainZNear() - d * (RainZFar() - RainZNear()).
// The froxel fog reconstructs MICRO relief from it (ruts/dips at 14.6 cm/texel,
// five times finer than its own baked terrain field) — see r_vol_mist_micro.
float          RainZNear();
float          RainZFar();
void           ComputeRainVP();               // straight-down ortho box at the camera
const Fmatrix& GetRainVP();
bool           RainSphereVisible(const Fvector& center, float radius);

// ---- Dynamic light shadows (STEP 3b): spot POOL + 1 point cube per frame. ----
// Spot pool: ONE atlas of SpotAtlasX×SpotAtlasY tiles (SpotSize² each), one tile
// per pooled spot light (Lights::kMaxShadowSpots). vk_pass_shadow owns the tile
// assignment/caching; consumers look tiles up via SpotShadow_TileOfLight.
VkImage        GetSpotImage();                // CLEAN atlas (static copy + NPC overlay) — surfaces
VkImageView    GetSpotView();
// STATIC layer (statics + trees only) — cached per tile until the light moves;
// copy source for the clean atlas, never sampled (rests in TRANSFER_SRC).
VkImage        GetSpotStaticImage();
VkImageView    GetSpotStaticView();
// Per-tile copy of the clean tile + grass casters on top — the visible beam /
// fog sample this; surfaces blend both (r_spot_grass_shadow).
VkImage        GetSpotBeamImage();
VkImageView    GetSpotBeamView();
u32            SpotSize();                    // tile size (1024)
u32            SpotAtlasX();                  // tiles per row (4)
u32            SpotAtlasY();                  // rows (2)
const Fmatrix& GetSpotVP();                   // the COOKIE spot's VP (flashlight beam texture)
void           SetSpotVP(const Fmatrix& vp);
// The part of every tile the spot maps are rendered into (r_spot_scale, latched by the
// shadow pass each frame so the receivers' UBO matches the raster): 1 = the whole tile.
float          SpotTileScale();
void           SetSpotTileScale(float s);
Fmatrix        ComputeSpotVPFor(const Fvector& pos, const Fvector& dir, float range, float cone);
void           SetSpotTileVP(u32 tile, const Fmatrix& vp);
const Fmatrix& GetSpotTileVP(u32 tile);

// Point POOL: D32 cube ARRAY (kMaxShadowPoints cubes, 6 faces each, 90° persp).
// Face order = Vulkan cube layers (+X,-X,+Y,-Y,+Z,-Z), D3D orientation.
constexpr float kPointNear = 0.1f;
VkImage        GetPointImage();
VkImageView    GetPointCubeView();                    // CUBE_ARRAY view (sampling)
VkImageView    GetPointFaceView(u32 cube, u32 face);  // 2D layer view (rendering)
VkImage        GetPointStaticImage();                 // cached statics-only cube array (copy SRC)
VkImageView    GetPointStaticFaceView(u32 cube, u32 face);
u32            PointSize();                            // 512
Fmatrix        ComputePointFaceVP(const Fvector& pos, float range, u32 face);

}}  // namespace VK::ShadowMap

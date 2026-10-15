// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — dynamic light (STEP 3, forward point/spot lights).
//
// vkLight is the real IRender_Light the game drives (campfires, flashlight,
// muzzle flashes, anomalies, lamps — all spawn through CRender::light_create
// and immediately call the setters). Every instance lives in a global registry;
// each frame EnvLight::Update calls Lights::Collect to pick the nearest active
// lights and uploads them in the shared Lighting UBO, where the forward
// fragment shaders (world / skinned / grass / trees) accumulate them.
//
// Not implemented (v1): per-light shadows, volumetric, flares, light textures.
#pragma once
#include <mutex>
#include "HW_Vulkan.h"
#include "../../xr_3da/Render.h"   // IRender_Light

namespace VK {

class vkLight final : public IRender_Light
{
public:
    u32     type   = POINT;       // LT enum (DIRECT treated as inactive, OMNIPART as POINT)
    bool    active = false;
    bool    hud    = false;
    bool    shadow = false;       // game asked for a shadow-casting light (flashlight etc.)
    bool    volumetric = false;   // game asked for a visible beam (R4 lamp shafts) — stored for diagnostics/future
    bool    synthBeam  = false;   // OUR lightplanes-derived beam light (SynthCones): the fog
                                  // boost applies, but the FL cone path must NOT double-draw it
    bool    flashlight = false;   // handheld/worn torch (CTorch head-lamp / weapon light) vs a
                                  // fixture (hanging lamp / searchlight / campfire). Torches get
                                  // the restrained R4 fog treatment (no lamp beam boost) so a
                                  // head-lamp shone at the camera isn't a searchlight glare.
    Fvector pos{ 0.f, 0.f, 0.f };
    Fvector dir{ 0.f, 0.f, 1.f };
    float   range  = 8.f;
    float   cone   = deg2rad(120.f);   // full apex angle (spot)
    Fcolor  color{};
    shared_str texture;                // spot projection texture (flashlight cookie)
    u32     uid = 0;                   // stable identity for the render side (never reused, unlike the address)
    // Does the lamp reach the terrain (vk_light.cpp LampSectors)? Decided where the lamp
    // stands and kept until it moves -- a fixture is asked once, a torch as it walks.
    // Written by CollectFrame under the registry mutex.
    Fvector terrainAt{ flt_max, flt_max, flt_max };
    bool    reachesTerrain = true;
    u32     sectorBelow = u32(-1), sectorAbove = u32(-1);   // what the last test saw (r_light_debug)

    vkLight();
    ~vkLight() override;

    void  set_type(LT t)                    override { type = u32(t); }
    void  set_active(bool b)                override { active = b; }
    bool  get_active()                      override { return active; }
    void  set_shadow(bool b)                override { shadow = b; }
    bool  get_shadow()                      override { return shadow; }
    void  set_volumetric(bool b)            override { volumetric = b; }
    bool  get_volumetric()                  override { return volumetric; }
    void  set_volumetric_intensity(float)   override {}
    void  set_volumetric_distance(float)    override {}
    void  set_flashlight(bool b)            override { flashlight = b; }
    void  set_flare(bool)                   override {}
    bool  get_flare()                       override { return false; }
    void  set_position(const Fvector& P)    override { pos = P; }
    void  set_rotation(const Fvector& D, const Fvector&) override { if (D.magnitude() > 1e-5f) { dir = D; dir.normalize(); } }
    void  set_cone(float angle)             override { cone = angle; }
    void  set_range(float r)                override { range = r; }
    float get_range() const                 override { return range; }
    void  set_virtual_size(float)           override {}
    void  set_texture(LPCSTR n)             override { texture = (n && n[0]) ? n : nullptr; }
    void  set_color(const Fcolor& c)        override { color = c; }
    void  set_color(float r, float g, float b) override { color.set(r, g, b, 1.f); }
    Fcolor get_color() const                override { return color; }
    void  set_hud_mode(bool b)              override { hud = b; }
    bool  get_hud_mode()                    override { return hud; }
    void  set_moveable(bool)                override {}
    bool  get_moveable()                    override { return true; }
};

namespace Lights {

constexpr u32 kMaxLights = 16;          // UBO array size — foliage + the non-clustered fallback path
constexpr u32 kMaxClusterLights = 256;  // SSBO array size — the clustered forward path (vk_clustered)
constexpr u32 kMaxShadowSpots  = 8;     // spot shadow POOL: tiles in the spot atlas (4×2 of 1024²).
                                        // Every pooled spot gets its own shadow map, so several
                                        // headlights/searchlights + the flashlight all shadow at once.
constexpr u32 kMaxShadowPoints = 4;     // point shadow POOL: cubes in the point cube ARRAY (512²×6 each).
                                        // Every pooled point (campfire/lamp) gets its own shadow cube,
                                        // so several fires with NPCs sitting around all shadow at once.

// GPU-side light record — must match the Lighting UBO `lights[]` entry layout.
struct GpuLight {
    float pos[4];     // xyz = world position, w = range
    float color[4];   // rgb = colour,         w = 1 spot / 0 point
    float dir[4];     // xyz = spot direction, w = cos(cone/2)
};

// Per-frame light set + the shadow-casting picks (budget: a POOL of up to
// kMaxShadowSpots spots + 1 point cube). `gpu` holds up to kMaxClusterLights
// (clustered path uploads them all to the SSBO); the UBO + foliage read only
// the first kMaxLights. All indices index this same array.
struct FrameLights {
    GpuLight gpu[kMaxClusterLights];
    // CPU-side parallel flags (NOT uploaded — GpuLight layout is shared with the
    // UBO/cluster SSBO): 1 = the game flagged the light volumetric (R4 renders a
    // visible beam for these). The fog inject encodes it into ITS OWN light copy.
    u8       volFlag[kMaxClusterLights] = {};
    // 1 = SynthCones beam-backing light: fog boost YES, FL-path analytic cone NO
    // (SynthCones draws its own geometry-derived cone for these).
    u8       synthFlag[kMaxClusterLights] = {};
    // 1 = handheld/worn torch (CTorch / weapon light): the fog inject skips the
    // lamp beam boost so a head-lamp shone at the camera stays the soft R4 shaft.
    u8       flashFlag[kMaxClusterLights] = {};
    // Source light per gpu[] entry — an OPAQUE KEY (the light's uid, see KeyOf), compare only.
    // RENDER_THREAD_PLAN step 4: it used to be the vkLight* and the shadow pass read the live
    // light through it; everything the render side needs is copied below instead.
    const void* src[kMaxClusterLights] = {};
    Fvector  srcPos[kMaxClusterLights];       // raw l->pos
    Fvector  srcDir[kMaxClusterLights];       // raw l->dir (not normalised, may be ~0)
    float    srcRange[kMaxClusterLights] = {}; // raw l->range (gpu[].pos[3] is the boosted / deadbanded one)
    float    srcCone[kMaxClusterLights] = {};  // l->cone, full apex angle
    u32      count    = 0;
    // Spot shadow POOL: gpu[] indices of the shadow-flagged spots that get a
    // tile this frame, nearest-first (narrow beams keep their 4× advantage).
    u32      poolCount = 0;
    int      poolGi[kMaxShadowSpots] = {};
    int      spotIdx  = -1;   // gpu[] index of the COOKIE spot (flashlight beam texture);
                              // falls back to poolGi[0] when no pooled spot has a texture
    // Point shadow POOL: gpu[] indices of the shadow-flagged points that get a
    // cube this frame, nearest-first.
    u32      poolCountP = 0;
    int      poolGiP[kMaxShadowPoints] = {};
    int      pointIdx = -1;   // gpu[] index of the nearest pooled point (legacy single-pick alias), -1 = none
    // bit i (i < kMaxLights): gpu[i] stands in another sector than the ground above it
    // (basement, bunker) -- the terrain leaves it out (LightUBO::lamp_terrain).
    u32      offTerrainMask = 0;
    // Source params of the cookie pick / nearest point pick:
    Fvector  spotPos{};  Fvector spotDir{ 0.f, 0.f, 1.f }; float spotRange = 0.f; float spotCone = 0.f;
    Fvector  pointPos{}; float pointRange = 0.f;
    shared_str spotTexture;   // the cookie spot's texture (flashlight beam pattern)
};

// Collect the nearest active lights around `eye` and pick the shadowed ones.
// Idempotent per Device.dwFrame — Pass_SunShadow calls it first (to render the
// dynamic shadow maps), EnvLight::Update reuses the same result for the UBO.
const FrameLights& CollectFrame(const Fvector& eye);

// Two-slot pipeline: the SynthCones backing lights are WRITTEN on the render thread (Pass_LightCones: pos/dir/range/active)
// while CollectFrame READS every registered light on the main thread (extract of the next frame). The registry mutex
// already guards CollectFrame's scan; the writers take it around their field updates.
std::mutex& RegistryMutex();

// Opaque identity of a light for FrameLights::src / SpotShadow_TileOfLight (nullptr -> nullptr).
inline const void* KeyOf(const vkLight* l) { return l ? reinterpret_cast<const void*>(uintptr_t(l->uid)) : nullptr; }

// Build this frame's FrameLights NOW. Called at the end of CRender::Calculate -- after the game
// has moved every light for the frame (CWeapon::RenderLight runs inside Calculate) -- so the
// record passes only ever read the copy. CollectFrame stays as the lazy fallback for frames
// without a Calculate (menu, editor).
void Extract();

}  // namespace Lights
}  // namespace VK

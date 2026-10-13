#pragma once
// vk_sun_cascades_cfg.h (nfs47) -- every tunable of the sun shadow maps in one place. Before this the numbers lived in
// #ifdef __SWITCH__ blocks across vk_shadow.cpp and vk_pass_shadow.cpp.
//
// One code path, two PROFILES picked by the console variable r_sun_casc_lite (vk_console_min.cpp):
//   0 = kStock -- the PC cascades as they always were (default).
//   1 = kLite  -- the handheld set tuned on the Switch (nfs43-48); the Switch startup preset sets it.
// The profile is LATCHED once, at ShadowMap::Init (the cascade image sizes and the MID images depend on it); changing the
// variable later takes effect on the next renderer start.
//
// Per near cascade there are two layers:
//   STATIC cache  -- statics, rigid opaque casters and the trees past r_wind_shadow_dist (in a REST pose in kLite).
//                    Re-rastered only when the camera moved past cascRedrawDist, the sun turned past r_shadow_casc_sun,
//                    or the visual set changed. While it is valid the cascade VP is FROZEN.
//   LIVE layer    -- copy(static) + NPC, rigid dynamics, trees within r_wind_shadow_dist with wind.
//                    nfs48: a cascade in npcLayerMask splits it in two: MID = copy(static) + trees/grass every
//                    r_sun_cad_c0 frames, COMBINED = copy(mid) + NPCs/rigid every r_sun_cad_npc frames. Others rebuild
//                    one live layer every r_sun_cad_c1 frames (cascade 1).
// The far map (cascade 2) is one cached layer, redrawn on farRedrawDist / farSunDotRedraw.
//
// cascadeZCull: how far DOWN-sun the caster cull reaches (light-view z, the camera is z = 0). The raster projection
// keeps kCascadeZBehind..kCascadeZAhead so biases stay the same; a receiver uses a cascade only while its z is below
// the cut (nx_sun_shadow.glsl NX_C0_ZMAX / NX_C1_ZMAX = CullNdc(kLite.cascadeZCull[i]) -- keep them in step; with kStock
// the cut is the whole column, the R1 receivers then just hand over to the next map a little early).

namespace VK
{
namespace SunCfg
{
struct Profile
{
    u32   cascadeSize[2];        // texels
    float cascadeBox[2];         // full ortho width, m
    float cascRedrawDist[2];     // static cache: camera drift that forces a re-raster, m
    float cascadeZCull[2];       // nfs47: caster column down-sun, m
    float farRedrawDist;         // far map static cache drift, m
    float farSunDotRedraw;       // far map static cache sun turn (cos)
    u32   skinnedCascadeMask;    // which near cascades the NPCs cast into
    bool  farDynamicCasters;     // nfs43: NPCs/rigid dynamics in the far map (false = its combined map is only re-copied on a static redraw)
    bool  cachedTreesRestPose;   // nfs47: cached tree layers without wind
    u32   npcLayerMask;          // nfs48: cascades with a MID layer (static -> mid (trees) -> combined (NPC))
    bool  staggerStaticRedraw;   // nfs48: at most one cascade cache re-raster per frame
    u32   farSize;               // 08-10: far map texels (kLite 1024: its redraw was 4 ms statics + 6.8 ms crowns of GPU fill at 2048)
};

static constexpr Profile kStock = {
    { 4096, 2048 }, { 25.f, 60.f }, { 2.f, 4.f }, { 460.f, 460.f },   // z cut = kCascadeZAhead: no cut
    10.f, 0.99999847f,                                                 // cos(0.1 deg)
    3u, true, false, 0u, false, 2048u,
};
// nfs43 (user 03-10): cascade 0 = NPCs within 10 m (nfs48: 512 -> 448, NPC shadow a bit softer), cascade 1 = live trees at
// 768 (7.8 cm); the far map is 320 m wide (08-10: at 1024, 31 cm a texel -- it only shades past cascade 1).
static constexpr Profile kLite = {
    { 448, 768 }, { 20.f, 60.f }, { 2.f, 6.f }, { 60.f, 150.f },
    40.f, 0.99984770f,                                                 // cos(1 deg)
    1u, false, true, 1u, true, 1024u,
};

const Profile& Get();   // the latched profile (vk_shadow.cpp)
void           Latch(); // ShadowMap::Init: read r_sun_casc_lite, r_sun_far_size
u32            FarSize(); // the latched far map size (r_sun_far_size, else Profile::farSize)

// Caster column up-sun: 160 m is plenty -- a caster that far up the ray needs to be ~40 m tall (at 15 deg sun) to shadow
// the box at all. Negative z is fine for an ortho projection.
constexpr float kCascadeZBehind = -160.f;
constexpr float kCascadeZAhead  = 460.f;                   // raster depth range down-ray
// Ortho depth is (z - near) / (far - near) in [0, 1].
constexpr float CullNdc(float z) { return (z - kCascadeZBehind) / (kCascadeZAhead - kCascadeZBehind); }
} // namespace SunCfg
} // namespace VK

#version 450
#extension GL_GOOGLE_include_directive : require
#include "vsm_common.glsl"   // VSM clipmap math (vsmSelect/vsmPageIndex) for the smooth atlas occlusion
#include "froxel.glsl"        // exp-Z slice <-> view-Z mapping (shared with tonemap + particle probe)
#include "shadow_math.glsl"   // spotLinZ/cascTap — set-agnostic, shared with shadow_common.glsl
#include "gi_sh.glsl"         // the froxel GI field: what its two images mean, and the phase integral
// xrRenderVulkan — froxel volumetric INJECTION (vk_volumetrics, P1).
//
// One thread per froxel. Reconstruct the froxel's world position from the camera
// basis (DeriveProjTerms scheme — camDir + right*tanX*ndc.x + top*tanY*ndc.y,
// scaled by the exponential view-Z), evaluate height/base fog density, then the
// sun in-scatter = Henyey-Greenstein phase * cascade sun-shadow * sun colour
// (+ a flat sky-ambient fill), all * density. Output: rgb = in-scatter,
// a = extinction (= density). vol_integrate then marches Z over this.
//
// Occlusion = the sun CASCADE maps (cascSample/cascTap ported from
// world_lmap.frag) — the r_vsm-OFF path + the permanent fallback. VSM-atlas
// sampling is the P4 upgrade.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;


#include "vol_params.glsl"   // Vol UBO (set 0 b0) — matches VK::Volumetrics::VolUBO

layout(set = 0, binding = 1) uniform sampler2D uShadowNear; // cascade 0
layout(set = 0, binding = 2) uniform sampler2D uShadowC1;   // cascade 1
layout(set = 0, binding = 3) uniform sampler2D uShadow;     // far cached map
layout(set = 0, binding = 4, rgba16f) uniform writeonly image3D uScatter;
layout(set = 0, binding = 5) uniform sampler2D uRainMap;    // top-down statics depth (sky visibility)
layout(set = 0, binding = 6) uniform sampler3D uHistory;    // prev frame's blended scatter (temporal)
// VSM static atlas occlusion (smooth, no cache tick) — gated by gridParams.w.
layout(set = 0, binding = 7) uniform sampler2D uVsmAtlas;   // static VSM atlas (opaque + trees)
layout(set = 0, binding = 8) readonly buffer VsmPageTable { uint vsmPageTable[]; }; // virtual page → atlas slot
layout(set = 0, binding = 9) uniform VsmClipmap {
    mat4 view;                 // world → sun light space
    vec4 level[VSM_LEVELS];    // xy = level origin (light XY of texel 0,0), z = extent (m)
    vec4 zparams;              // x = zNear, y = 1/(zFar-zNear), z = depth bias
} vsmC;
// binding 10 — RESERVED hole (was the dedicated fog sun-shadow, removed 12-08-2026).
// Not declared here on purpose: the C++ side still BINDS it (the set layout list is
// positional, so removing the slot would renumber 11..19), and a bound descriptor
// that no shader declares is perfectly legal.
layout(set = 0, binding = 11) uniform sampler2D   uSpotShadow;  // spot (flashlight) shadow — occlude the fog cone
layout(set = 0, binding = 12) uniform samplerCubeArray uPointShadow; // point shadow cube POOL
layout(set = 0, binding = 13) uniform sampler3D   uSmokeMedia;  // Stage-1 VMS: splatted+resolved smoke (rgb=albedo, a=density)
// DYNAMIC VSM atlas (NPC / grass / near wind-swaying trees, re-rendered per frame).
// Without it the fog only saw the STATIC atlas — every caster inside the wind-hybrid
// radius (~40 m) simply DID NOT EXIST for the god rays: sun shafts pushed straight
// through near tree crowns and trunks. Same demand-page table as the resolve.
layout(set = 0, binding = 14) uniform sampler2D uVsmAtlasDyn;
layout(set = 0, binding = 15) readonly buffer VsmPageTableDyn { uint vsmPageTableDyn[]; };
layout(set = 0, binding = 16) readonly buffer VsmDynUsed      { uint vsmDynUsed[]; };  // dyn slot -> has-caster flag (skip empty pages)
// PREVIOUS frame's scene depth (inject runs BEFORE this frame's prepass, so last
// frame's depth is still intact in the attachment) — DEPTH REJECTION: a froxel
// whose centre lies BEHIND the opaque geometry of its own view column is invisible,
// yet its in-scatter leaks onto walls via the trilinear volume fetch (slice spans
// the wall → the sunlit-outdoors half glows THROUGH it). Reprojected via
// prevViewProj (the same transform the temporal blend uses).
layout(set = 0, binding = 17) uniform sampler2D uSceneDepthPrev;
layout(set = 0, binding = 18) uniform sampler2D uTerrainH;   // baked TERRAIN height (ground anchor)
// GRASS CANOPY field, baked once per level from the level's own detail-slot grid:
// R = canopy height (× canopy.y metres), G = coverage 0..1. See BakeGrassCanopy.
layout(set = 0, binding = 19) uniform sampler2D uCanopy;
// STEP 6 — the GI field resampled into THIS frustum (vk_gi_froxel). Same normalised
// frustum coordinates as this grid, at half its resolution in every axis, so a
// froxel reads its own air with one trilinear tap. rgb of L0 = mean radiance,
// a = the field weight (0 outside the probe volume → keep the analytic fill);
// rgb of L1 = the directional band. See gi_sh.glsl for what the numbers mean.
layout(set = 0, binding = 20) uniform sampler3D uGiL0;
layout(set = 0, binding = 21) uniform sampler3D uGiL1;

const float PI = 3.14159265;

// --- P3: cheap animated 3D value noise (drifting dust / "living air") -----------
float hash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
float vnoise3(vec3 x)
{
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i + vec3(0,0,0)), hash13(i + vec3(1,0,0)), f.x),
                   mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x),
                   mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}
float fbm3(vec3 p) { return 0.62 * vnoise3(p) + 0.38 * vnoise3(p * 2.3 + 11.7); }

// --- Sun shadow, R4 cascade scheme (ported from world_lmap.frag) -------------
// cascTap → shadow_math.glsl (included at the top). cascSample below is
// deliberately NOT the shared one: the fog wants a WIDE, soft penumbra.
float cascSample(sampler2D smap, mat4 vp, vec3 wp, float bias_)
{
    vec3 n = (vp * vec4(wp, 1.0)).xyz;          // ortho → already NDC
    vec2 uv = n.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.01 || uv.x > 0.99 || uv.y < 0.01 || uv.y > 0.99 || n.z <= 0.0 || n.z >= 1.0)
        return -1.0;
    float ref = n.z - bias_;
    // WIDE 3x3 PCF (spacing = r_vol_soft texels): a SOFT penumbra. The fog doesn't
    // need crisp shadows — a soft shadow makes the foliage dapple a gentle gradient
    // instead of sharp speckles, so the cascade cache TICK (sun creep) barely shows.
    vec2  tx  = (1.0 / vec2(textureSize(smap, 0))) * max(V.zParams.w, 0.5);
    float s = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
        s += cascTap(smap, uv + vec2(float(dx), float(dy)) * tx, ref);
    return s * (1.0 / 9.0);
}
// VSM atlas occlusion (port of vsm_resolve sampleVSM — STATIC atlas + the DYNAMIC
// atlas on pages with binned casters):
// world → light → clipmap level → page → atlas slot → 3x3 PCF depth compare. The
// VSM updates SMOOTHLY per frame (no cascade cache TICK) → stable shafts. Returns
// lit 1..0, or -1.0 when this point has NO resident VSM page → caller falls back to
// the cascade (covers off-screen gaps in the view-dependent VSM).
float sampleVSMStatic(vec3 wp)
{
    vec3 lp = (vsmC.view * vec4(wp, 1.0)).xyz;
    vec2 luv; ivec2 page;
    int  L = vsmSelect(lp.xy, vsmC.level, luv, page);
    if (L < 0) return -1.0;                       // outside the clipmap → cascade fallback
    // Coarser-level fallback (mirrors vsm_resolve): the throttle LOD bias marks pages
    // coarser and the dirty budget can unmap one for a frame — walk up before giving
    // the froxel to the cascade path. Steady state exits on the first iteration.
    uint slot = VSM_UNMAPPED;
    for (; L < VSM_LEVELS; ++L) {
        vec2 t = (lp.xy - vsmC.level[L].xy) / vsmC.level[L].z;
        page   = clamp(ivec2(floor(t * float(VSM_PAGES_AXIS))), ivec2(0), ivec2(VSM_PAGES_AXIS - 1));
        slot   = vsmPageTable[vsmPageIndex(L, page)];
        if (slot < uint(VSM_MAX_PHYS_S)) { luv = t; break; }
    }
    if (L >= VSM_LEVELS) return -1.0;              // page not resident → cascade fallback

    // DYNAMIC atlas page at the same virtual index (near wind-trees / NPC / grass) —
    // only pages a caster actually binned into (dynUsed) pay the extra taps.
    uint slotD = vsmPageTableDyn[vsmPageIndex(L, page)];
    bool hasD  = slotD < uint(VSM_MAX_PHYS) && vsmDynUsed[slotD] != 0u;

    vec2  pageLocal = luv * float(VSM_PAGES_AXIS) - vec2(page);
    vec2  base  = vec2(float(slot % uint(VSM_ATLAS_W_S)), float(slot / uint(VSM_ATLAS_W_S)));
    vec2  baseD = vec2(float(slotD % uint(VSM_ATLAS_W)), float(slotD / uint(VSM_ATLAS_W)));
    float zHere = (lp.z - vsmC.zparams.x) * vsmC.zparams.y;
    // AIR bias (atmoR.w, r_vol_vsm_bias; 0 = legacy). A froxel is in the air — there
    // is NO self-shadow acne to hide, so it doesn't need the surface receivers' 0.6 m
    // terrain slack. That slack lit a 0.6 m shell of fog BEHIND every occluder, i.e.
    // straight through thin roofs / trunks / walls. A tight bias only needs to cover
    // the D16 quantization (~3 cm) + the write-side raster bias.
    float bias  = (V.atmoR.w > 0.0) ? V.atmoR.w : vsmC.zparams.z;
    float biasD = vsmC.zparams.w;
    const vec2  dim   = vec2(float(VSM_ATLAS_W_S), float(VSM_ATLAS_H_S));
    const vec2  dimD  = vec2(float(VSM_ATLAS_W),   float(VSM_ATLAS_H));
    const float tp    = 1.0 / float(VSM_PAGE_SIZE);
    const float inset = 0.5 * tp;
    float lit = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        vec2 pl = clamp(pageLocal + vec2(float(dx), float(dy)) * tp, vec2(inset), vec2(1.0 - inset));
        float occ = texture(uVsmAtlas, (base + pl) / dim).r;
        bool  sh  = (zHere - bias > occ);
        if (hasD && !sh)
            sh = (zHere - biasD > texture(uVsmAtlasDyn, (baseD + pl) / dimD).r);
        lit += sh ? 0.0 : 1.0;
    }
    return lit * (1.0 / 9.0);
}

// (sampleFogShadow lived here — REMOVED 12-08-2026 with the dedicated fog sun-shadow
// map it read. Sun occlusion now comes from the VSM atlas, falling back to the
// cascades. Binding 10 is a reserved hole; see vk_volumetrics.cpp.)

float skyVis(vec3 wp);   // defined below (top-down statics depth, 1 = open sky)

// Occlusion-source tag of the LAST sunShadow call (leak forensics, r_vol_debug 4):
// 0 = cascade / fog-shadow path, 1 = VSM atlas hit, 2 = VSM page miss -> skyVis.
int g_occSrc = 0;

float sunShadow(vec3 worldPos)
{
    // Occlusion source (gridParams.w): 1 = VSM atlas, 0 = cascade. (Mode 2, the
    // dedicated per-frame fog shadow, was removed 12-08-2026.) Falls through to the
    // cascade where there is no coverage (no VSM page / r_vsm off).
    float mode = V.gridParams.w;
    if (mode > 0.5) {
        // VSM ATLASES (static + dynamic) are the occluder. Under VSM the near cascades
        // are NOT rendered for the fog (vk_pass_shadow cascForVol → cascRaster false),
        // so their combined maps hold stale/frozen depth — DON'T fall through to them.
        // A froxel with NO resident VSM page falls back to the top-down SKY-VISIBILITY
        // map: pages are marked by VISIBLE pixels only, so air whose sun-column hits no
        // on-screen surface (behind houses, under an off-screen roof) misses — the old
        // "treat as LIT" answer painted bright god rays through that geometry. skyVis
        // is view-independent and already bound: under a roof/canopy → dark (correct-ish),
        // open air → lit (identical to the old policy).
        float v = sampleVSMStatic(worldPos);
        g_occSrc = (v >= 0.0) ? 1 : 2;
        return (v >= 0.0) ? v : skyVis(worldPos);
    }
    // Smaller bias than the surface receivers: a froxel is in AIR (no self-shadow
    // acne to hide), so a tight bias just reduces light leaking THROUGH walls.
    float s = cascSample(uShadowNear, V.sun_near_vp, worldPos, 0.00015);
    if (s >= 0.0) return s;
    s = cascSample(uShadowC1, V.sun_c1_vp, worldPos, 0.00025);
    if (s >= 0.0) return s;

    vec4 c = V.sun_vp * vec4(worldPos, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 ndc = c.xyz / c.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    float ref = ndc.z - 0.0008;
    return cascTap(uShadow, uv, ref);
}

// Sky visibility (top-down statics depth): 1 = open to the sky, 0 = under a roof.
// Occludes the unconditional ambient term so the air inside a building doesn't glow
// with outdoor haze (the sun beam through the window still lights via the cascade).
float skyVis(vec3 wp)
{
    vec3 n = (V.rain_vp * vec4(wp, 1.0)).xyz;   // ortho → already NDC
    vec2 uv = n.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || n.z <= 0.0 || n.z >= 1.0)
        return 1.0;                              // outside the map = open sky
    float ref = n.z - 0.002;
    vec2  px  = 1.0 / vec2(textureSize(uRainMap, 0));
    return 0.25 * (cascTap(uRainMap, uv + vec2(-0.5, -0.5) * px, ref)
                 + cascTap(uRainMap, uv + vec2( 0.5, -0.5) * px, ref)
                 + cascTap(uRainMap, uv + vec2(-0.5,  0.5) * px, ref)
                 + cascTap(uRainMap, uv + vec2( 0.5,  0.5) * px, ref));
}

// World height of the TOP-MOST surface in the rain map (ortho, straight down).
// fog6.z = eyeY - zNear, fog6.w = zFar - zNear (see ShadowMap::RainZNear/Far).
float rainSurfaceY(vec2 uv)
{
    return V.fog6.z - textureLod(uRainMap, uv, 0.0).r * V.fog6.w;
}

// MICRO relief for the ground mist: how much this spot dips below what immediately
// surrounds it, in metres. The baked terrain field is 71 cm/texel over the whole
// level — enough to tell a valley from a ridge, but it averages a rut or a shell
// crater away to nothing. The RAIN map is 14.6 cm/texel over ±75 m around the
// camera, i.e. 5x finer exactly where the player can see fog pooling at their feet.
//
// ⚠ It stores the TOP-MOST surface — roofs, canopies, crates. Reading it as "the
// ground" is the same mistake that once floated the fog up onto rooftops. Hence the
// consistency gate: use it ONLY where it agrees with the baked terrain height, i.e.
// where the thing directly overhead IS the ground. Under a roof or a tree it
// disagrees by metres and the micro term switches itself off.
float microDip(vec3 wp, float ground)
{
    vec3 n = (V.rain_vp * vec4(wp, 1.0)).xyz;    // ortho → already NDC
    vec2 uv = n.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || n.z <= 0.0 || n.z >= 1.0)
        return 0.0;                              // outside the ±75 m box
    float h0 = rainSurfaceY(uv);
    if (abs(h0 - ground) > 1.5) return 0.0;      // roof / canopy / crate — not our ground
    // Neighbourhood at ~2 m (14 texels): the scale of a rut, ditch or wheel track.
    vec2 px = 1.0 / vec2(textureSize(uRainMap, 0));
    const vec2 kNb[4] = vec2[4](vec2(14.0, 0.0), vec2(-14.0, 0.0), vec2(0.0, 14.0), vec2(0.0, -14.0));
    float sum = 0.0;
    for (int i = 0; i < 4; ++i) sum += rainSurfaceY(clamp(uv + kNb[i] * px, vec2(0.0), vec2(1.0)));
    return sum * 0.25 - h0;                      // >0 = this spot sits in a dip
}

// Ground height (world Y) under an arbitrary world position, read from the terrain
// field baked once per level. Height fog is measured from the GROUND, not from the
// camera: fog.y (eye.y - 2) is only the fallback for positions outside the baked map
// or before it exists — with a camera-relative anchor the whole layer travels with
// the player, so hollows never fill and a rooftop lifts the fog with you.
// ⚠ Extracted from main() so the CAMERA can be evaluated by exactly the same rule as
// a froxel (the immersion fade below needs it); two copies of this WILL drift apart.
// tuvOut = the sampled UV in the height map, <0 when there is no usable read.
float terrainGround(vec3 wp, out vec2 tuvOut)
{
    tuvOut = vec2(-1.0);
    if (V.terra.w <= 0.5) return V.fog.y;   // no field baked at all → legacy eye-relative
    // World-anchored last resort: the floor of the baked box. NEVER fall back to
    // anything camera-relative here — an eye-relative anchor over a map hole makes
    // that column follow the camera upward, which is a pillar of full-density fog
    // rising out of the hole to any altitude ("fog to space" over the houses).
    float g = V.terra.x - (V.terra.y + V.terra.z);
    vec4 tc = V.terra_vp * vec4(wp, 1.0);
    if (tc.w <= 0.0) return g;
    vec2 t = (tc.xy / tc.w) * 0.5 + 0.5;
    t.y = 1.0 - t.y;
    if (any(lessThan(t, vec2(0.0))) || any(greaterThan(t, vec2(1.0)))) return g;
    tuvOut = t;
    float td = textureLod(uTerrainH, t, 0.0).r;
    // 1.0 = cleared texel. X-Ray levels CUT the terrain under buildings and roads, so
    // those footprints are holes in this map. Heal them from the surroundings: take
    // the smallest depth (= highest surface) among a few rings, so the fog sheet stays
    // continuous across a building instead of dropping out or spiking. Only the miss
    // path pays for the taps.
    if (td >= 0.99999) {
        vec2 px = 1.0 / vec2(textureSize(uTerrainH, 0));
        const vec2 kRing[8] = vec2[8](
            vec2( 16.0, 0.0), vec2(-16.0, 0.0), vec2(0.0,  16.0), vec2(0.0, -16.0),
            vec2( 40.0, 40.0), vec2(-40.0, 40.0), vec2(40.0, -40.0), vec2(-40.0, -40.0));
        for (int i = 0; i < 8; ++i)
            td = min(td, textureLod(uTerrainH, t + kRing[i] * px, 0.0).r);
    }
    if (td < 0.99999) g = V.terra.x - (V.terra.y + td * V.terra.z);
    return g;
}

// GRASS CANOPY occlusion of the SUN term. Returns lit 1..0.
//
// Why this exists at all, given grass IS a shadow caster: as an occluder it only
// reaches ~48 m from the camera (near band into the dynamic atlas, far band rigid
// into the static cache), it is a centimetre-scale stipple that the 3x3 PCF at
// froxel scale averages straight back into "about half lit", and it sways, so the
// little it does contribute shimmers frame to frame. All three are the same mistake:
// treating a field of grass as a crowd of individual blades. It is a MEDIUM — the
// question is not "is this froxel behind a blade" but "how much grass did the
// sunlight cross to get here".
//
// Beer-Lambert along the SLANT path to the sun: a froxel one blade-height down under
// a low sun sits behind metres of grass, which is exactly why a real field goes dark
// at grazing light while the air above it stays bright. Height/coverage come from the
// level's authored detail grid, so this works at ANY distance, never flickers, and
// costs one bilinear fetch — taken only below the tallest canopy on the level.
float canopyOcclusion(vec3 wp, float hGround, vec3 toSun)
{
    if (V.canopy.x <= 0.001 || hGround >= V.canopy.z) return 1.0;
    vec2 uv = vec2(wp.x * V.canopy_uv.x + V.canopy_uv.y,
                   wp.z * V.canopy_uv.z + V.canopy_uv.w);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    vec2  c   = textureLod(uCanopy, uv, 0.0).rg;
    float top = c.r * V.canopy.y;            // canopy height above the ground, metres
    float cov = c.g;                          // mean coverage of the slot, 0..1
    if (cov <= 0.004 || hGround >= top) return 1.0;
    // Metres of canopy still above this froxel, walked along the ray to the sun. The
    // elevation floor keeps a sunset from producing a kilometre-long path; the clamp
    // is what bounds it in practice (a shallow sun should saturate, not explode).
    float depth = top - hGround;
    float path  = min(depth / max(toSun.y, 0.12), 25.0);
    return exp(-V.canopy.x * cov * path);
}

// HOLLOW GATE (fog5.w, metres) — shared by the mist bank and the immersion fade.
// Real ground fog pools where the air is still and cold: valleys, riverbeds, the dip
// behind a ridge. Compare this position's ground against a WIDE neighbourhood in the
// baked field; a positive drop means the terrain here sits below what surrounds it, a
// ridge gives a negative one. 0 = no gating (fog everywhere at a uniform thickness,
// i.e. weather-wide fog). Returns 1 (= no gating) whenever it cannot decide.
float hollowGate(vec2 tuv, float ground, float hGround)
{
    if (V.fog5.w <= 0.01 || tuv.x < 0.0 || V.fog5.x <= 1e-6 || hGround >= 12.0) return 1.0;
    vec2 px = 1.0 / vec2(textureSize(uTerrainH, 0));
    const vec2 kFar[4] = vec2[4](vec2(64.0, 0.0), vec2(-64.0, 0.0), vec2(0.0, 64.0), vec2(0.0, -64.0));
    float sum = 0.0, n = 0.0;
    for (int i = 0; i < 4; ++i) {
        float td = textureLod(uTerrainH, clamp(tuv + kFar[i] * px, vec2(0.0), vec2(1.0)), 0.0).r;
        if (td < 0.99999) { sum += V.terra.x - (V.terra.y + td * V.terra.z); n += 1.0; }
    }
    return (n > 0.5) ? smoothstep(0.0, V.fog5.w, sum / n - ground) : 1.0;
}

// Henyey-Greenstein phase: peaks at cosT=+1 for g>0 (forward scatter = the bright
// halo when looking toward the sun).
float hgPhase(float cosT, float g)
{
    float g2 = g * g;
    float d  = 1.0 + g2 - 2.0 * g * cosT;
    return (1.0 - g2) / (4.0 * PI * pow(max(d, 1e-4), 1.5));
}

// V-1 — DUAL-LOBE phase. Real fog/haze scatters strongly FORWARD (the halo around
// the sun) but also keeps a weaker BACKWARD lobe (the glow you see with the sun
// behind you). A single HG lobe can only do one of them, so the sun term never
// dominated the flat ambient veil at any angle — the fog read as a uniform pall
// instead of light with direction. Two lobes cost one extra HG evaluation.
float dualLobe(float cosT, float gF, float gB, float wB)
{
    return mix(hgPhase(cosT, gF), hgPhase(cosT, gB), clamp(wB, 0.0, 1.0));
}

// Spot shadow POOL — 3x3 PCF in the light's own atlas tile (4x2 of 1024², see
// shadow_common.glsl), LINEAR-depth compare with a world epsilon. Occludes the
// fog cone of EVERY pooled spot so beams stop at walls / grass cuts them.
// spotLinZ → shadow_math.glsl (included at the top).
float spotShadowF(vec3 wp, float range, int tile)
{
    vec4 c = V.spot_pool_vp[tile] * vec4(wp, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 ndc = c.xyz / c.w;
    vec2 uv = ndc.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    float f    = max(range, 1.0);
    float zRef = spotLinZ(ndc.z, f) - 0.08;
    const vec2 kTileScale = vec2(0.25, 0.5);
    vec2  texel = 1.0 / vec2(textureSize(uSpotShadow, 0));
    vec2  tBase = vec2(float(tile & 3), float(tile >> 2)) * kTileScale;
    vec2  tMin  = tBase + texel * 1.5;
    vec2  tMax  = tBase + kTileScale - texel * 1.5;
    vec2  auv   = tBase + uv * kTileScale;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
        sum += (zRef <= spotLinZ(texture(uSpotShadow, clamp(auv + vec2(x, y) * texel, tMin, tMax)).r, f)) ? 1.0 : 0.0;
    return sum * (1.0 / 9.0);
}
// Point POOL (cube array) shadow — 1 tap. `cube` = the light's cube-array index.
float pointShadowF(vec3 wp, vec3 lp, float range, int cube)
{
    // LINEAR-depth compare, world epsilon (same fix as the spot pool) — the old
    // 0.01 NDC bias grew to metres at the range edge and ate far NPC shadows.
    vec3 d = wp - lp;
    float z = max(max(abs(d.x), abs(d.y)), abs(d.z));
    const float n = 0.1;                 // kPointNear
    float f = max(range, 1.0);
    float zMap = texture(uPointShadow, vec4(d, float(cube))).r;
    zMap = n * f / max(f - zMap * (f - n), 1e-4);
    return (z - 0.08 <= zMap) ? 1.0 : 0.0;
}

// Terrain/static occlusion for an UN-shadowed lamp (r_light_occ) — ported verbatim
// from light_shade.glsl so the fog/smoke matches the surfaces: if the light is BURIED
// below the top-down surface at its XZ (a lamp/campfire under a roof or in a basement),
// a froxel at/above that surface is only reachable THROUGH the geometry → occlude. This
// is what stops an indoor caster from lighting the smoke/fog OUTSIDE the house.
float lightTerrainOcc(vec3 wp, vec3 lpos)
{
    if (V.light_occ.x < 0.5) return 1.0;
    vec4 lc = V.rain_vp * vec4(lpos, 1.0);
    if (lc.w <= 0.0) return 1.0;
    vec2 luv = lc.xy * 0.5 + 0.5; luv.y = 1.0 - luv.y;
    if (any(lessThan(luv, vec2(0.0))) || any(greaterThan(luv, vec2(1.0)))) return 1.0;
    if (lc.z <= texture(uRainMap, luv).r + V.light_occ.y) return 1.0;   // light at/above the surface → no occlusion
    vec4 fc = V.rain_vp * vec4(wp, 1.0);
    if (fc.w <= 0.0) return 1.0;
    vec2 fuv = fc.xy * 0.5 + 0.5; fuv.y = 1.0 - fuv.y;
    if (any(lessThan(fuv, vec2(0.0))) || any(greaterThan(fuv, vec2(1.0)))) return 1.0;
    float fragBelow = fc.z - texture(uRainMap, fuv).r;                  // >0 = froxel under the surface
    return 1.0 - V.light_occ.w * (1.0 - smoothstep(0.0, V.light_occ.z, max(fragBelow, 0.0)));
}

// P2 — local lights (flashlight CONE, lamp/campfire/anomaly HALO) scattering in the
// fog. Distance falloff × HG phase × (spot) cone gate × occlusion (budget spot/point
// shadow, else the r_light_occ heightfield so indoor lamps don't leak through walls).
vec3 localLights(vec3 world, vec3 viewDir)
{
    int n = int(V.lightParams.x);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        vec3  toL   = V.lights[i].pos.xyz - world;
        float dist  = length(toL);
        float range = V.lights[i].pos.w;
        if (range <= 0.0 || dist >= range) continue;
        vec3  Ld    = toL / max(dist, 1e-3);
        // Narrow beams get the windowed falloff (far half of the beam still
        // glows in the fog) — matches the surface shaders (light_shade.glsl).
        float atten;
        if (V.lights[i].dir.w > 0.87 && (int(V.lights[i].color.w + 0.5) & 1) == 1) {
            atten = 1.0 - (dist * dist) / (range * range);
            atten *= atten;
        } else {
            atten = 1.0 - dist / range;
            atten *= atten;                               // smooth falloff
        }
        // color.w: bit0 = spot, bit1 = VOLUMETRIC-flagged lamp (R4 shows a beam
        // for these — pole lamps / headlights); boost their in-scatter so the
        // shaft reads even in light haze. bits 2+ = pool index+1 (spot tile OR
        // point cube — a light is one or the other, so the bits never collide).
        int   lw    = int(V.lights[i].color.w + 0.5);
        bool  isSpot = (lw & 1) == 1;
        if (isSpot) {                                     // spot cone
            float cosCone = V.lights[i].dir.w;
            float d = dot(-Ld, V.lights[i].dir.xyz);
            if (d < cosCone) continue;
            atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.5), d);
        }
        if ((lw & 2) == 2) atten *= 3.0;                  // volumetric lamp beam boost
        int pool = (lw >> 2) - 1;                         // pool index (bits 2+, idx+1)
        if (isSpot) {
            if (pool >= 0) atten *= spotShadowF(world, range, pool);
        } else if (pool >= 0) {
            atten *= pointShadowF(world, V.lights[i].pos.xyz, range, pool);   // pooled campfire cube
        } else {
            // Un-shadowed OMNI lamp → no leak through roof/floor (matches
            // light_shade.glsl): the top-down heightfield buries basement lamps.
            atten *= lightTerrainOcc(world, V.lights[i].pos.xyz);
        }
        // Local lights use their OWN anisotropy (lightParams.z), gentler than the
        // sun's sharp forward peak (V.fog.w) — a torch shone AT the camera would
        // otherwise spike into a blinding, HDR-desaturated (cold-white) glare.
        // clamp keeps hgPhase valid if the value ever arrives out of range.
        float gL = clamp(V.lightParams.z, 0.0, 0.95);
        float ph = hgPhase(dot(viewDir, Ld), gL);          // scatter toward the camera
        acc += V.lights[i].color.rgb * (atten * ph);
    }
    return acc * V.lightParams.y;                          // r_vol_lights boost
}

// Stage-2: smoke media density at an arbitrary WORLD point (inverse of the splat/inject
// froxel basis — same math as vol_splat). 0 outside the grid frustum. Used to march
// optical depth toward the sun for smoke self-shadow.
float smokeDensityAt(vec3 wpos)
{
    vec3  rel = wpos - V.camPos.xyz;
    float vz  = dot(rel, V.camDir.xyz);
    if (vz <= V.zParams.x || vz >= V.zParams.y) return 0.0;
    float ndcx = dot(rel, V.camRightT.xyz) / (vz * dot(V.camRightT.xyz, V.camRightT.xyz));
    float ndcy = dot(rel, V.camTopT.xyz)   / (vz * dot(V.camTopT.xyz,   V.camTopT.xyz));
    if (abs(ndcx) > 1.0 || abs(ndcy) > 1.0) return 0.0;
    vec2  smuv = vec2(ndcx * 0.5 + 0.5, (1.0 - ndcy) * 0.5);
    float smw  = Froxel_SliceFromViewZ(vz, V.zParams.x, V.zParams.z);
    return texture(uSmokeMedia, vec3(smuv, smw)).a;
}

// DEPTH-REJECTION visibility of a world point vs the PREV frame's scene depth
// (r_vol_depth_reject): 0 = the point is hidden behind the opaque geometry of its
// own view column, 1 = visible / can't tell (off-screen, behind cam, sky). The
// compare uses the FARTHEST depth in a ~froxel-sized neighbourhood, so columns
// partially over an opening (window/doorway) keep their beams. Reprojected with
// prevViewProj (the transform the temporal blend already uses); camPos.w/camDir.w
// carry the projection's _43/_33: viewZ = P43 / (ndcZ - P33).
float depthVis(vec3 wp)
{
    vec4 rc = V.prevViewProj * vec4(wp, 1.0);
    if (rc.w <= 0.0) return 1.0;
    vec2 ruv = (rc.xy / rc.w) * 0.5 + 0.5;
    ruv.y = 1.0 - ruv.y;
    if (any(lessThan(ruv, vec2(0.001))) || any(greaterThan(ruv, vec2(0.999)))) return 1.0;
    vec2 rr = vec2(0.35) / V.gridParams.xy;
    vec4 g0 = textureGather(uSceneDepthPrev, ruv - rr, 0);
    vec4 g1 = textureGather(uSceneDepthPrev, ruv + rr, 0);
    float dmax = max(max(max(g0.x, g0.y), max(g0.z, g0.w)),
                     max(max(g1.x, g1.y), max(g1.z, g1.w)));
    if (dmax >= 0.99999) return 1.0;   // sky in the neighbourhood → column open
    float vzScene = V.camPos.w / min(dmax - V.camDir.w, -1e-6);
    // The HUD WEAPON writes into this same depth buffer, at arm's length, over a big
    // slab of screen. Rejecting against it carved a weapon-shaped hole through the
    // whole fog column — and since the depth is the PREVIOUS frame's, the hole lagged
    // the gun and smeared into a trail whenever the camera swung. Nothing at 2 m is a
    // legitimate occluder for a fog column anyway (that stretch of air scatters almost
    // nothing), so treat a near surface as "can't tell" and keep the fog.
    if (vzScene < 2.0) return 1.0;
    // sky_ambient.w (r_vol_surf_clip) pulls the cut IN FRONT of the surface: the
    // "lit shell" of air hugging a sun-facing THIN wall is unfixable on the VSM
    // side (the atlas write bias + D16 quantization ≈ the wall's own thickness, so
    // no receiver bias can tell inside from outside) — but that air sits within
    // centimetres of a VISIBLE surface, where fog contributes nothing legit anyway.
    return (rc.w > vzScene + V.sun_dir.w - V.sky_ambient.w) ? 0.0 : 1.0;
}

void main()
{
    ivec3 id = ivec3(gl_GlobalInvocationID);
    int dimX = int(V.gridParams.x), dimY = int(V.gridParams.y), dimZ = int(V.gridParams.z);
    if (id.x >= dimX || id.y >= dimY || id.z >= dimZ) return;

    float nearZ = V.zParams.x, logFN = V.zParams.z;

    // Froxel sample world pos with the sub-froxel JITTER (temporal.xyz, Halton per
    // frame) → supersamples the shadow/density. cur (below) uses THIS.
    vec2 uv  = (vec2(id.xy) + 0.5 + V.temporal.xy) / vec2(dimX, dimY);
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    float viewZ = Froxel_ViewZFromSlice((float(id.z) + 0.5 + V.temporal.z) / float(dimZ), nearZ, logFN);
    vec3 ray   = V.camDir.xyz + V.camRightT.xyz * ndc.x + V.camTopT.xyz * ndc.y;
    vec3 world = V.camPos.xyz + ray * viewZ;

    // UNJITTERED froxel CENTRE — used ONLY for the history reprojection. The history
    // is indexed at integer froxel centres, so reprojecting the JITTERED world made
    // the lookup wobble by the jitter every frame → it never settled = "trembling".
    // Reprojecting the stable centre lets the jittered samples accumulate cleanly.
    vec2 uvC  = (vec2(id.xy) + 0.5) / vec2(dimX, dimY);
    vec2 ndcC = vec2(uvC.x * 2.0 - 1.0, 1.0 - 2.0 * uvC.y);
    float viewZC = Froxel_ViewZFromSlice((float(id.z) + 0.5) / float(dimZ), nearZ, logFN);
    vec3 worldCenter = V.camPos.xyz + (V.camDir.xyz + V.camRightT.xyz * ndcC.x + V.camTopT.xyz * ndcC.y) * viewZC;

    // Sky visibility (0 under a roof, 1 open) drives BOTH the indoor density boost
    // and the ambient occlusion — sampled once.
    float skyv = skyVis(world);

    // Base extinction: base × exponential height falloff above the anchor.
    // This drives the TRANSMITTANCE (scene dimming) — kept at base so small rooms
    // aren't over-darkened (over-darkening is why the fog read as "just darker").
    vec2  tuv;                                     // this froxel's UV in the baked height map (<0 = no usable read)
    float ground = terrainGround(world, tuv);      // see terrainGround(): ground-anchored, never camera-relative
    // ---- V-1: TWO MEDIA LAYERS ------------------------------------------------
    // One exponential layer cannot be both "a barely-there haze that still catches
    // god rays" and "milk pooling in the hollow" — those want opposite densities.
    // Tuning for the first gives sigma_t ~0.005, at which no amount of gain makes a
    // valley read as fog; tuning for the second turns the whole map into soup. So
    // the medium is now a SUM of two independent layers, each with its own density,
    // thickness and phase, added into the same sigma_s/sigma_t the grid already
    // marches (zero cost to the integrator, it never learns there are two):
    //   DUST  (fog.x / fog.z / fog.w)  — thin, tall, strongly forward-scattering:
    //                                    shafts in clear weather, "volume" in the air.
    //   MIST  (fog5)                   — dense, ground-hugging, closer to isotropic:
    //                                    the actual fog bank. Off by default (fog5.x = 0),
    //                                    so with r_vol_mist 0 this is byte-identical.
    float hGround  = max(0.0, world.y - ground);
    float extDust  = V.fog.x * exp(-hGround * V.fog.z);
    float extMist  = 0.0;

    // Hollow gate — computed here (not inside the mist block) because the micro term
    // needs it too: without it a puddle of mist appears in every rut ON A MOUNTAIN.
    // V-2 bypass (fog7.w, driven by the weather's fog_density): a real fog day is not
    // confined to the valleys. Lift the gate TOWARD 1 rather than narrowing its ramp —
    // narrowing turns a smoothstep into a step, and that shipped clear-edged holes in
    // the middle of dense fog wherever the ground sat level with its surroundings.
    float lowGate = mix(hollowGate(tuv, ground, hGround), 1.0, clamp(V.fog7.w, 0.0, 1.0));

    // ---- V-1b: DENSE FROM OUTSIDE, LIVEABLE FROM INSIDE (fog7) -------------------
    // The bank reads beautifully from a hilltop — cloud lying at the foot of the
    // slope — and is unpleasant to stand in: once you are inside, optical depth
    // accumulates from zero metres and every direction saturates to milk within a few
    // steps. That is physically right, but this fog is SCENERY, so the density the
    // player swims in is deliberately decoupled from the density the player looks at.
    // The target is expressed as a multiple of the DUST layer (r_vol_mist_inside) —
    // "inside the bank the air is only a little thicker than everywhere else" — and
    // arrives already divided by the mist density, as a scale in 0..1 (1 = off).
    //   fog7.z = 1 → IMMERSION fade: keyed on how deep the CAMERA sits in the layer,
    //                so the whole bank opens up once you are in it and is untouched
    //                while you look at it from above.
    //   fog7.y > 0 → NEAR fade: keyed on distance from the camera, so only the metres
    //                around the player clear out while the bank keeps its body
    //                further away. Independent of where the camera is.
    // Applied to the BANK only: the micro mist pooling in the ruts is already thin and
    // is exactly the thing you are meant to still see at your feet.
    float mistScale = 1.0;
    if (V.fog7.x < 0.999 && V.fog5.x > 1e-6) {
        float sImm = 1.0;
        if (V.fog7.z > 1e-6 && V.terra.w > 0.5) {
            // The same layer profile, evaluated at the eye. NO fbm height-warp here:
            // the warp is per-froxel shape, and feeding it into a GLOBAL scale would
            // make the whole valley pulse as the billows drift past the camera.
            // Cost is near zero despite running per froxel — every invocation reads
            // the same handful of texels, so it is a texture-cache hit.
            vec2  ctuv;
            float cGround = terrainGround(V.camPos.xyz, ctuv);
            float cH      = max(0.0, V.camPos.y - cGround);
            // Same weather bypass as the froxel gate above — otherwise, on a fog day
            // that fills the ridges, the camera would still be judged "outside the
            // bank" on high ground and the fade would never engage where it is needed.
            float cGate   = mix(hollowGate(ctuv, cGround, cH), 1.0, clamp(V.fog7.w, 0.0, 1.0));
            // ⚠ The height falloff here is DELIBERATELY NOT the layer's own thickness
            // (fog5.y), which is what the first cut used — and it made the effect
            // non-monotone in the worst way: step onto a 6 m roof, or jump off
            // anything raised, and the eye left a 4 m layer, the thinning switched
            // off, and the world got DENSER one metre higher than it was below. A
            // rooftop in the valley is not "viewing the bank from outside".
            // Deciding whether the camera is in the same low ground as the bank is the
            // HOLLOW GATE's job (and it does it well — a hilltop reads 0 through it).
            // All this term has to answer is "am I above the whole thing", which lives
            // on a scale of tens of metres: fog7.z = 1/r_vol_mist_inside_h.
            float cDens   = exp(-cH * V.fog7.z) * cGate;
            // Smooth in the camera's height, so descending a slope ramps the bank out
            // gradually instead of snapping the moment you cross into it.
            sImm = mix(1.0, V.fog7.x, smoothstep(0.05, 0.5, cDens));
        }
        float sNear = 1.0;
        if (V.fog7.y > 0.01)
            sNear = mix(V.fog7.x, 1.0, smoothstep(0.0, V.fog7.y, distance(world, V.camPos.xyz)));
        mistScale = min(sImm, sNear);   // whichever opens the air up more wins
    }

    if (V.fog5.x > 1e-6) {
        // BILLOWS, not a lid. A pure exponential in height has a mathematically smooth
        // top, and at the density a fog bank needs that surface reads as a WALL of
        // grey standing in the hollow — the layer is a solid, not a cloud. (The
        // existing r_vol_noise cannot fix it: V-0 deliberately moved that noise onto
        // the SCATTERING coefficient, where it shapes light, not shape.) So the mist
        // gets its own fbm applied to its HEIGHT: the layer's top surface rises and
        // falls by a fraction of its own thickness, which is what makes fog read as
        // lying in a hollow rather than filling it to a level line. Drifts slower than
        // the dust — a fog bank moves with the valley air, not with the wind.
        float mh = hGround;
        if (V.fog3.z > 0.001) {
            float t  = V.noiseParams.z * V.noiseParams.w * 0.35;
            vec3  np = world * V.fog3.w + vec3(t, t * 0.2, t * 0.6);
            // (fbm-0.5) in units of layer thickness (1/fog5.y): +/- fog3.z thicknesses.
            mh = max(0.0, hGround - (fbm3(np) - 0.5) * 2.0 * V.fog3.z / max(V.fog5.y, 1e-3));
        }
        // Base layer density at this height, BEFORE the terrain gating below. The micro
        // term needs it: mist pooling in a wheel rut is a local, self-contained thing
        // and must not depend on whether that rut happens to lie in a valley.
        extMist = V.fog5.x * exp(-mh * V.fog5.y) * lowGate * mistScale;
    }

    // ---- MICRO MIST: an INDEPENDENT third contribution, not a modifier ------------
    // Deliberately outside the mist block above, and carrying its own ABSOLUTE density
    // rather than a multiplier. Two earlier shapes both failed in game:
    //   x extMist * k  — the relief gate had already zeroed extMist on flat ground, so
    //                    0 x 24 = 0: a rut could only hold mist inside a valley.
    //   x mistBase * k — tied to r_vol_mist, so the only way to see fog in the ruts was
    //                    a bank so thick the player could not stand in it.
    // Thin air pooling in a wheel track is its own phenomenon: it should be available
    // with the fog bank turned OFF entirely. Cost is gated by height — the taps only
    // run for froxels low enough for any dip to reach them.
    if (V.fog6.y > 0.001 && hGround < 4.0) {
        float dip = microDip(world, ground);
        if (dip > 0.0) {
            // The mist FILLS the dip up to its rim: `fill` is 1 at the bottom and 0
            // level with the surrounding ground, so the depth of the hole IS the height
            // of the fog in it. The band has a 25 cm floor purely for legibility — a
            // 5 cm ribbon is thinner than a froxel beyond a few metres and would never
            // survive the grid (its DENSITY still scales with the real depth via the
            // smoothstep, so a shallow rut stays subtler than a crater).
            float band = max(dip, 0.25);
            float fill = clamp(1.0 - hGround / band, 0.0, 1.0);
            // NOT gated by the hollow map: tried that, and the user's verdict was to go
            // back — a rut holds mist wherever it is. If ruts on high ground ever need
            // to stay dry, multiply by `lowGate` here (it is already computed above).
            extMist += V.fog6.y * fill * smoothstep(0.0, V.fog6.x, dip);
        }
    }
    float extinction = extDust + extMist;
    // P3 — animated dust/mist: slowly-drifting 3D noise for wisps/pockets in the lit
    // fog. V-0: it now modulates the SCATTERING coefficient, not the extinction
    // (fog3.y = r_vol_noise_scatter). Noise on extinction makes the AIR'S OPACITY
    // flicker: the temporal pass reprojects a froxel CENTRE while the noise field
    // drifts through the world on a wall clock, so history and current never agree
    // and the transmittance keeps hissing. On the scattering side the same wisps read
    // as shape in the LIGHT, where a residual shimmer is far less visible — and the
    // transmittance stays smooth, which is what the composite multiplies the scene by.
    float noiseMul = 1.0;
    if (V.noiseParams.x > 0.001) {
        float t  = V.noiseParams.z * V.noiseParams.w;
        vec3  np = world * V.noiseParams.y + vec3(t, t * 0.4, t * 0.8);
        noiseMul = max(1.0 + V.noiseParams.x * (fbm3(np) - 0.5) * 2.0, 0.0);
    }
    if (V.fog3.y < 0.5) { extDust *= noiseMul; extMist *= noiseMul; extinction *= noiseMul; noiseMul = 1.0; }   // legacy: noise on thickness

    // V-0 — ALBEDO: the scattering coefficient finally has its own life. Until now
    // sigma_s was IDENTICALLY sigma_t (albedo == 1 everywhere, hardcoded), so "how
    // much light this air throws at the eye" and "how much of the world it hides"
    // were one number: thinning the fog to clear the distance also killed the shafts,
    // and thickening it for shafts turned the scene into grey soup. r_vol_albedo (1 =
    // the old behaviour) splits them, which is also what the "god rays without fog"
    // hybrid needs — a thin, high-albedo, strongly forward-scattering medium.
    // Per-layer sigma_s: the sun term below weights each layer by its OWN scattering
    // coefficient AND its own phase function, which is the whole point of splitting
    // them — the thin dust can keep a razor forward lobe (a visible shaft) while the
    // mist bank scatters near-isotropically (a soft glowing body) in the same froxel.
    // Everything non-directional (ambient, lamps) sees only the total.
    float sigmaSDust = extDust * V.fog3.x * noiseMul;
    float sigmaSMist = extMist * V.fog3.x * noiseMul;
    float sigmaS     = sigmaSDust + sigmaSMist;
    // Indoor density boost (fog2.z, ×(1+indoor) under a roof) makes the thin diffuse
    // AMBIENT haze visible across a short interior sightline. It is applied ONLY to
    // the ambient term: the SUN BEAM and LOCAL LIGHTS are strong DIRECTIONAL sources,
    // and boosting THEM ×(1+indoor) indoors accumulated over the Z-march into a WHITE
    // BLOWOUT (the campfire column / a god-ray through a doorway). Those use the BASE
    // extinction so bright sources stay bounded; the ambient haze keeps the interior
    // atmosphere. NOTE outdoors skyv=1 → boost=0 → ambDens==extinction → this is
    // byte-identical to the old outdoor look (only interiors change).
    float ambDens = sigmaS * (1.0 + V.fog2.z * (1.0 - skyv));

    // Sun in-scatter (cascade-occluded → the beam stops at walls/roof, passing
    // only through openings like windows) + sky ambient (occluded by sky visibility
    // → interiors don't glow uniformly, so the sun beam stands out).
    vec3  viewDir = normalize(ray);
    vec3  toSun   = normalize(-V.sun_dir.xyz);
    float cosT    = dot(viewDir, toSun);
    float occ     = sunShadow(world);
    // Grass canopy. Composed with min(), NOT a product: inside the ~48 m band where
    // grass is also in the shadow atlas these two describe the SAME occluder, and
    // multiplying would double-darken exactly where the player stands. Taking the
    // stronger one keeps the transition across that boundary invisible.
    occ = min(occ, canopyOcclusion(world, hGround, toSun));
    // Ambient: FULL outdoors (skyv 1), but only a FLOOR fraction (fog2.y) under a
    // roof — interiors keep visible haze (real rooms aren't pitch black) while the
    // sun beam from a window still stands out against the dimmer indoor fill.
    float ambK    = V.fog2.y + (1.0 - V.fog2.y) * skyv;
    vec3  ambient = V.sky_ambient.rgb * ambK;

    // ---- STEP 6: THE GI FIELD REPLACES THAT FILL, IT DOES NOT ADD TO IT ---------
    // The two lines above are an ESTIMATE of the same quantity the field measures:
    // how much light reaches this bit of air from everywhere that is not the sun.
    // One is a per-frame constant times a top-down sky-visibility scalar; the other
    // is eight probes that traced the actual room. Adding them would count the sky
    // twice and wash the interior out — the same call ddgi_receiver.glsl makes for
    // surfaces, and for the same reason.
    //
    // ⭐ WHAT THIS IS FOR: `ambK` is the whole reason interiors read as a uniform
    // grey pall. A single scalar cannot say "bright near the window, dark at the
    // back of the room" — it can only dim the ENTIRE room by one number. The field
    // can, because it has a value per froxel; and its band-1 term can say WHICH WAY
    // the light comes from, which is what makes the air near an opening glow when
    // you look toward it (r_gi_froxel_dir).
    //
    // ⚠ The field weight in a is not decoration: outside the probe volume the
    // lookup is black by construction, and mixing toward it there would paint the
    // far half of the frustum dark with a boundary sweeping across the frame.
    if (V.gi.x > 0.0) {
        vec3  giUVW = vec3(uv, clamp((float(id.z) + 0.5 + V.temporal.z) / float(dimZ), 0.0, 1.0));
        vec4  giL0  = texture(uGiL0, giUVW);
        float giW   = giL0.a * V.gi.x;
        if (giW > 0.0) {
            // The phase's own mean cosine, dialled by r_gi_froxel_dir. The DUST lobe's
            // g is used for a term that rides both layers: the mist's g only differs
            // where the mist is dense, and there the ambient is a soft body anyway.
            const float g = V.fog.w * V.gi.y;
            // gi.z (r_gi_froxel_gain) puts the field on the same scale as the fill it
            // displaces — the probes integrate the SH9 sky at the SURFACE receivers'
            // scale, while this fill is the weather's flat ambient times r_vol_ambient,
            // and nothing makes those two agree. Scaling the whole answer keeps the
            // band-0/band-1 ratio, so the directionality is unaffected by calibration.
            vec3 giAmb = GiField_Inscatter(giL0.rgb, texture(uGiL1, giUVW).rgb, viewDir, g) * V.gi.z;
            ambient = mix(ambient, giAmb, giW);
        }
    }
    // The SUN term gets its own boost (fog2.w) so the directional shaft pops THROUGH
    // the ambient haze. Transmittance (a) uses the base extinction.
    // V-1 — MULTIPLE SCATTERING (Wrenninge's octave approximation). Single scatter
    // saturates dense fog at the source radiance and leaves it FLAT: light that would
    // in reality bounce several times inside the medium — filling shadowed air with a
    // soft internal glow — simply does not exist, so thick fog reads as dirty grey
    // rather than luminous haze. Each octave halves the scattering weight and the
    // phase eccentricity (blurring toward isotropic) and SOFTENS the shadow term
    // (occ^0.5, ^0.25 …), which is what lets light bleed into shadowed fog. Weights
    // are normalised, so this redistributes energy into shape instead of adding gain.
    vec3  sunScatter     = vec3(0.0);   // DUST lobe (also what the smoke media reuses below)
    vec3  sunScatterMist = vec3(0.0);   // MIST lobe — same octaves, its own eccentricity
    {
        const int  kMaxOct = 4;
        int   oct   = clamp(int(V.fog4.x + 0.5), 1, kMaxOct);
        bool  twoLayers = (sigmaSMist > 1e-7);
        float wSca  = 1.0, aPhase = 1.0, aOcc = 1.0, wSum = 0.0;
        for (int i = 0; i < oct; ++i) {
            float ph = dualLobe(cosT, V.fog.w * aPhase, V.fog4.y * aPhase, V.fog4.z);
            // Octave 0 keeps the crisp shadow (the shaft edge); deeper ones wash out.
            float o  = (i == 0) ? occ : pow(max(occ, 1e-4), aOcc);
            sunScatter += V.sun_color.rgb * (ph * o) * wSca;
            if (twoLayers) {
                float phM = dualLobe(cosT, V.fog5.z * aPhase, V.fog4.y * aPhase, V.fog4.z);
                sunScatterMist += V.sun_color.rgb * (phM * o) * wSca;
            }
            wSum  += wSca;
            wSca  *= 0.5; aPhase *= 0.5; aOcc *= 0.5;
        }
        float inv = V.fog2.w / max(wSum, 1e-4);
        sunScatter *= inv; sunScatterMist *= inv;
    }
    // Sun beam + local lights at BASE extinction (no indoor ×boost → no blowout);
    // the ambient haze at the indoor-boosted density (the visible interior glow).
    // Local lights carry their OWN boost (r_vol_lights) so lamps can be dialed.
    vec3  localL     = localLights(world, viewDir);
    // Kept as named terms so r_vol_term can isolate them (see the selector below).
    vec3  termSun    = (sunScatter * sigmaSDust + sunScatterMist * sigmaSMist) * V.fog2.x;
    vec3  termAmb    = ambient    * V.fog2.x * ambDens;
    vec3  termLocal  = localL * sigmaS;
    vec3  termAtmo   = vec3(0.0);
    vec3  inscatter  = termSun + termAmb + termLocal;

    // Atmospheric scattering (r_atmo): Rayleigh (blue, wavelength-dependent) + Mie
    // (forward halo toward the sun) in-scatter of the SUN's light along the sightline.
    // Distant surfaces gain a BLUE veil away from the sun and a WARM halo toward it
    // (aerial perspective) — and since sun_color is the time-of-day colour, dawn/dusk
    // read warm automatically. Sun-shadowed (stops at terrain), rides the base air
    // density so it accumulates with distance through the Z-integration.
    if (V.atmo.x > 0.0) {
        float cosT = dot(viewDir, toSun);
        float pR   = 0.05968310 * (1.0 + cosT * cosT);   // Rayleigh phase 3/(16π)(1+cos²)
        float pM   = hgPhase(cosT, V.atmo.w);            // Mie forward halo
        vec3  atmoScatter = V.sun_color.rgb * occ
                          * (V.atmoR.rgb * (pR * V.atmo.y) + vec3(pM * V.atmo.z));
        // Rides the DUST layer only. Rayleigh is scattering off air molecules; the
        // ground-mist layer is water droplets (Mie, achromatic) and has no business
        // multiplying a blue-tinted term — hanging aerial perspective on the mist
        // would paint a valley fog bank blue with distance. With r_vol_mist 0 the
        // dust density IS the old total, so this is unchanged for the single layer.
        termAtmo   = atmoScatter * V.fog2.x * sigmaSDust;
        inscatter += termAtmo;
    }

    // ---- Stage-1 VMS: smoke as participating media. Sample the splatted+resolved
    // smoke media at this froxel (same normalized frustum coords as the fog, so it
    // lines up). Smoke adds its OWN extinction and scatters the SAME light as fog
    // (sun beam + ambient + local lights), tinted by the particles' own albedo —
    // so smoke catches god-rays / the flashlight cone and self-occludes via the
    // extinction the integrate pass marches. v1: scatter uses the lit terms without
    // the indoor boost (bright sources stay bounded, same reasoning as the sun beam).
    if (V.smokeParams.x > 0.0) {
        vec3  smUVW = vec3(uv, (float(id.z) + 0.5 + V.temporal.z) / float(dimZ));
        vec4  sm    = texture(uSmokeMedia, smUVW);
        float smokeDens = sm.a * V.smokeParams.x;

        // Stage-2 SELF-SHADOW: march the smoke media from here toward the sun and
        // accumulate optical depth → the cloud's sun side stays bright, its far/deep
        // side darkens (gives the cloud real volume/form). Only smoky froxels march
        // (cheap); gated on r_vol_smoke_shadow (smokeParams.z), step = smokeParams.w.
        float sunVis = 1.0;
        if (V.smokeParams.z > 0.0 && sm.a > 1e-5) {
            float mstep = V.smokeParams.w;
            // Seed with THIS froxel's own density so even a thin / single-cell cloud
            // self-attenuates the sun (a smoky cell dims the light passing through it) —
            // the march on top adds the directional gradient (far/deep side darker) when
            // the cloud is thick enough to span several cells toward the sun.
            float tau = sm.a;
            vec3  sp  = world;
            for (int s = 0; s < 6; ++s) { sp += toSun * mstep; tau += smokeDensityAt(sp); }
            sunVis = exp(-tau * mstep * V.smokeParams.z);
        }

        if (V.smokeParams.y > 0.5) {
            // DEBUG (r_vol_smoke_debug): isolate the smoke in the composite. 1 = colour
            // (albedo glow), 2 = density heatmap (grey), 3 = SELF-SHADOW sunVis (blue =
            // shadowed/deep side, warm = sun-lit side) → shows the Stage-2 gradient
            // directly. Empty froxels stay 0 → scene shows through clean.
            float dd      = sm.a;
            float present = (dd > 1e-4) ? 1.0 : 0.0;
            vec3  col;
            if      (V.smokeParams.y > 2.5) col = mix(vec3(0.0, 0.06, 0.5), vec3(1.0, 0.25, 0.0), clamp((1.0 - sunVis) * 4.0, 0.0, 1.0)) * (5.0 * present);  // shadow heatmap: blue=lit, red=self-shadowed (×4 so faint shows)
            else if (V.smokeParams.y > 1.5) col = vec3(dd) * 25.0;
            else                            col = (sm.rgb + 0.15) * dd * 25.0;
            inscatter  = col;
            extinction = dd;
        } else if (smokeDens > 1e-5) {
            // Sun term attenuated by the smoke toward the sun; ambient + local lights full.
            vec3 lit = (sunScatter * sunVis + ambient) * V.fog2.x + localL;
            inscatter  += sm.rgb * lit * smokeDens;
            extinction += smokeDens;
        }
    }

    // ---- DEPTH REJECTION (r_vol_depth_reject, sun_dir.w = slack in metres, 0 = off).
    // Two kills, both against the PREV frame's depth (see depthVis above main):
    //  (1) THIS frame's light sample at the JITTERED position: the ±0.5-froxel
    //      temporal jitter pushes samples of a wall-straddling froxel BEHIND the
    //      wall (sunlit outdoors) — the EMA then accumulates those bright outliers
    //      into a visible punch-through even when the froxel CENTRE is legally
    //      visible. Zeroing per-sample makes the straddler converge to the visible
    //      fraction of its air — supersampling-correct partial coverage.
    //  (2) the blended history at the CENTRE: a froxel fully hidden behind geometry
    //      goes dark immediately (no multi-frame EMA fade-out of an old glow).
    float visJ = 1.0, visC = 1.0;
    if (V.sun_dir.w > 0.0) {
        visJ = depthVis(world);
        visC = depthVis(worldCenter);
    }
    inscatter *= visJ;

    // ---- Leak forensics (sun_color.w = r_vol_debug; tonemap shows the RAW volume):
    //  3 = froxel sun visibility as grey air (occ; constant density, rejection shown
    //      as a dim blue tint so "shadowed" and "rejected" read differently);
    //  4 = occlusion source: green = VSM page hit, red = miss -> skyVis fallback,
    //      blue = cascade/fog-shadow path. Brightness follows the lit factor.
    float dbgMode = V.sun_color.w;
    if (dbgMode > 2.5) {
        vec3 paint;
        if (dbgMode > 3.5)
            paint = (g_occSrc == 2) ? vec3(1.0, 0.05, 0.05)
                  : (g_occSrc == 1) ? vec3(0.05, 1.0, 0.05) : vec3(0.1, 0.1, 1.0);
        else
            paint = vec3(1.0);
        inscatter = paint * (0.03 * occ + 0.001);
        if (visJ < 0.5) inscatter = vec3(0.0005, 0.0005, 0.004);   // depth-rejected → dim blue
        extinction = 0.0;   // no self-absorption → the paint reads at full range
    }

    // r_vol_term — TERM ISOLATION (fog4.w). The fog's look is a sum of five sources
    // and tuning it blind means changing whichever one happens to be loudest. Show
    // exactly one: 1 sun beam, 2 sky ambient, 3 local lights, 4 atmosphere
    // (Rayleigh/Mie), 5 smoke media. Whatever is still bright when the others are
    // muted is what actually paints the fog — everything else is a knob that cannot
    // move the picture. (Smoke is the remainder: it is folded into inscatter after
    // the named terms.)
    {
        int term = int(V.fog4.w + 0.5);
        if (term > 0) {
            vec3 termSmoke = inscatter - (termSun + termAmb + termLocal + termAtmo);
            inscatter = (term == 1) ? termSun
                      : (term == 2) ? termAmb
                      : (term == 3) ? termLocal
                      : (term == 4) ? termAtmo
                                    : termSmoke;
        }
    }
    vec4 cur = vec4(inscatter, extinction);

    // ---- Temporal accumulation: reproject this froxel's world pos into the PREV
    // frame's volume and EMA-blend (alpha = temporal.w). Converges the jittered
    // samples into a clean, dense volume; rejected (off-screen / behind cam) → cur.
    float alpha = V.temporal.w;
    if (alpha > 0.0) {
        vec4 pc = V.prevViewProj * vec4(worldCenter, 1.0);
        if (pc.w > 0.0) {
            vec2 puv = (pc.xy / pc.w) * 0.5 + 0.5;
            puv.y = 1.0 - puv.y;
            float pvz = dot(worldCenter - V.prevCamPos.xyz, V.prevCamDir.xyz);   // prev view-space depth
            float pw  = Froxel_SliceFromViewZ(max(pvz, 1e-3), V.prevCamPos.w, V.prevCamDir.w);
            if (all(greaterThanEqual(vec3(puv, pw), vec3(0.0))) &&
                all(lessThanEqual   (vec3(puv, pw), vec3(1.0)))) {
                // Full EMA blend (no rejection): the moving-sun shadow crawl through
                // foliage is exactly the noise temporal accumulation is meant to
                // average into a stable shaft. Rejecting it (drop history on change)
                // re-exposed the crawl = "trembling". Off-screen → reprojection bounds
                // above already fall back to `cur`, so disocclusion can't ghost badly.
                vec4 hist = textureLod(uHistory, vec3(puv, pw), 0.0);
                cur = mix(cur, hist, alpha);
            }
        }
    }

    // Centre-rejection applied AFTER the history blend: the stored history goes dark
    // there immediately (no multi-frame EMA fade-out of a through-wall glow).
    // Debug modes keep it off so rejected froxels stay visible as the dim blue tint.
    if (dbgMode < 2.5) cur.rgb *= visC;

    // GROUND-ANCHOR FORENSICS (terra.w == 2, r_vol_ground_debug): paint the froxel's
    // height ABOVE THE BAKED TERRAIN instead of light, and pin the extinction so the
    // pattern is actually visible through the composite. Read with r_vol_debug 1.
    //   terrain-shaped bands that stay put as you walk = the anchor is alive;
    //   a flat wash that slides with the camera        = the map read is failing and
    //                                                    the shader is on the fallback.
    // Green channel flags which branch produced `ground` (1 = sampled map, 0 = fallback).
    if (V.terra.w > 3.5) {
        // GRASS-CANOPY forensics (r_vol_ground_debug 3, view with r_vol_debug 1).
        // Proves the baked field is REAL before anyone tunes r_vol_canopy — the
        // terrain bake once cost a whole session to a map that was silently all-1.0,
        // and "the log printed a count" proved nothing.
        //   GREEN, brighter = taller canopy × coverage
        //   BLUE            = the detail grid says bare ground here
        //   RED             = outside the detail grid entirely
        vec2 cuv = vec2(world.x * V.canopy_uv.x + V.canopy_uv.y,
                        world.z * V.canopy_uv.z + V.canopy_uv.w);
        vec3 col;
        if (any(lessThan(cuv, vec2(0.0))) || any(greaterThan(cuv, vec2(1.0))))
            col = vec3(0.2, 0.0, 0.0);
        else {
            vec2 c = textureLod(uCanopy, cuv, 0.0).rg;
            col = (c.g <= 0.004) ? vec3(0.0, 0.0, 0.15)
                                 : vec3(0.02) + vec3(0.0, clamp(c.r * V.canopy.y * 0.5, 0.0, 0.5) * c.g, 0.0);
        }
        cur = vec4(col, 0.05);
    }
    else if (V.terra.w > 2.5) {
        // MICRO-RELIEF forensics (r_vol_ground_debug 2, view with r_vol_debug 1).
        // Answers the only question that matters before tuning the micro term: is
        // there any small-scale relief IN THE DATA at all, or is the ground a flat
        // sheet at 14.6 cm/texel too? Painted at the froxels just above the surface:
        //   GREEN, brighter = deeper dip (this spot sits below its 2 m neighbourhood)
        //   BLUE            = the rain map disagrees with the baked terrain by >1.5 m
        //                     (roof / canopy / crate) → the micro term refuses to read it
        //   RED             = outside the rain map's ±75 m box
        //   BLACK           = ground that really is flat here — nothing to pool into
        vec3  n   = (V.rain_vp * vec4(world, 1.0)).xyz;
        vec2  ruv = n.xy * 0.5 + 0.5; ruv.y = 1.0 - ruv.y;
        bool  outside = (ruv.x < 0.0 || ruv.x > 1.0 || ruv.y < 0.0 || ruv.y > 1.0 || n.z <= 0.0 || n.z >= 1.0);
        float rH  = outside ? 0.0 : rainSurfaceY(ruv);
        vec3  col;
        if (outside)                        col = vec3(0.2, 0.0, 0.0);
        else if (abs(rH - ground) > 1.5)    col = vec3(0.0, 0.0, 0.2);
        // Dim grey base so FLAT ground reads as "measured, nothing there" instead of
        // being indistinguishable from a dead debug view; green rides on top of it.
        else                                col = vec3(0.02) + vec3(0.0, clamp(microDip(world, ground) * 10.0, 0.0, 0.25), 0.0);
        // Paint the WHOLE column (ground_debug 1 does the same): the integrator sums
        // along the ray, so tagging only the sliver of air at the surface contributes
        // almost nothing. Pin a small extinction — with T decaying, the column
        // converges to ~col/0.05 instead of accumulating over the full far distance.
        cur = vec4(col, 0.05);
    }
    else if (V.terra.w > 1.5) {
        float h = clamp((world.y - ground) * 0.1, 0.0, 1.0);   // 0..10 m above ground
        // green = the height came from the baked map (not the eye-relative legacy
        // anchor and not the box-floor last resort — both of those read as a hole).
        float onMap = (ground != V.fog.y && ground > V.terra.x - (V.terra.y + V.terra.z) + 0.01) ? 1.0 : 0.0;
        cur = vec4(h, onMap, 1.0 - h, V.fog.x);
    }

    // ---- NaN FIREWALL — last thing before the store, and the store feeds an
    // integrator with INFINITE memory: the EMA above blends history in every
    // frame and Execute copies this very image back INTO that history, so one
    // non-finite froxel latches forever — and the trilinear history fetch then
    // seeds its neighbours, so the artifact GROWS. On screen that is hard-edged
    // black blocks at froxel scale, flickering, usually after a level load (the
    // composite adds vol.rgb straight into the frame). The sources are transient
    // — garbage read during load, a degenerate frame — but the accumulator made
    // them permanent. clamp() bounds Inf; the select kills NaN (clamp alone does
    // NOT: min/max against NaN is undefined in SPIR-V and can pass it through).
    cur = mix(clamp(cur, vec4(0.0), vec4(16384.0)), vec4(0.0), isnan(cur));
    imageStore(uScatter, id, cur);
}

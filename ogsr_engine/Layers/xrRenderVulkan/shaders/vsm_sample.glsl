// xrRenderVulkan — VSM receiver lookup. TWO receiver paths live here:
//
// 1) SURFACES (world/terrain/skinned/tree — anything in the depth prepass): the
//    screen-space MASK. A compute pass (vsm_resolve.comp) does the world->light->
//    page->atlas lookup ONCE per pixel at prepass depth, temporally accumulates
//    it, and receivers sample the result by screen UV — one tap (vsmSunShadow).
//    Bound on set VSM_SET binding 14 (EnvLight).
//
// 2) GRASS (NOT in the prepass): DIRECT atlas lookup at the blade's own world
//    position (vsmSunShadowGrassDirect, guarded by VSM_GRASS_DIRECT — only
//    detail.frag defines it). The mask is WRONG for grass by construction: at a
//    blade pixel it describes the surface BEHIND the blade, so terrain shadows
//    20 m away painted themselves onto bushes (the 2026-07-04 "shadows through
//    the grass" saga; diagnosed with r_grass_debug). Costs the full page lookup
//    + 3x3 PCF per fragment — the price of a correct 3D sample.
//
// Both gated by L.shadow_params.w (receiver decides VSM vs cascade fallback).
#ifndef VSM_SAMPLE_GLSL
#define VSM_SAMPLE_GLSL

// The mask lives on the shared EnvLight descriptor set — bound at set 1 for world/grass
// shaders, set 2 for skinned/tree. Define VSM_SET before #include to pick the index.
#ifndef VSM_SET
#define VSM_SET 1
#endif
layout(set = VSM_SET, binding = 14) uniform sampler2D uVsmMask;   // R = sun lit factor (temporally resolved)

// screenUV = gl_FragCoord.xy * (1/screenDims). Returns lit factor 1 = lit .. 0 = shadowed.
float vsmSunShadow(vec2 screenUV)
{
    return texture(uVsmMask, screenUV).r;
}

// GRASS receiver: DIRECT static-atlas lookup at the blade's own world position.
// The screen-space mask CANNOT serve grass: it is resolved at PREPASS depth, where
// grass doesn't exist, so a blade pixel reads the shadow of whatever surface is
// BEHIND it — squat in front of a bush and a tree shadow lying on the ground 20 m
// away paints itself onto the bush (user-confirmed via r_grass_debug 1..3: both
// mask channels carry it, by construction). Fix = do the world->light->page->atlas
// lookup the resolve does, but at vWPos. BOTH atlases: static (trees/buildings) at
// the normal receiver bias, and the DYNAMIC atlas (grass/NPC casters) with an EXTRA
// ~0.3 m bias — the blade is IN that atlas, so the extra slack stops it acne-ing on
// its own depth while shadows from clumps/NPCs further along the sun ray survive.
// No temporal EMA here: the clipmap origin jitter wiggles edges by a texel/frame,
// but 3x3 PCF + wind sway + alpha noise hide it.
// Guarded: only detail.frag defines VSM_GRASS_DIRECT (bindings 15/16 have carried
// the real page table + clipmap UBO on the EnvLight set since Phase 1C; 23/24 = the
// static/dyn atlases, 25 = the dyn page table; white fallback until AtlasReady).
#ifdef VSM_GRASS_DIRECT
#include "vsm_common.glsl"
layout(set = VSM_SET, binding = 23) uniform sampler2D uVsmAtlasS;
layout(set = VSM_SET, binding = 24) uniform sampler2D uVsmAtlasD;
layout(set = VSM_SET, binding = 15) readonly buffer VsmPageTableEnv    { uint vsmPageTableEnv[]; };
layout(set = VSM_SET, binding = 25) readonly buffer VsmPageTableDynEnv { uint vsmPageTableDynEnv[]; };
layout(set = VSM_SET, binding = 16) uniform VsmClipmapEnv {
    mat4 view;                 // world -> sun light space
    vec4 level[VSM_LEVELS];    // xy = level origin, z = extent (m)
    vec4 zparams;              // x = zNear, y = 1/(zFar-zNear), z = bias
} vsmCE;

// Lit factor 1..0 at world position wp (mirrors vol_inject sampleVSMStatic).
// Outside the clipmap / page resident in neither atlas -> lit (resolve's policy).
// selfBiasM = anti-acne slack (m along the sun ray) for the DYN atlas taps: a
// casting blade vs its own rasterized depth + PCF neighbour taps span tens of cm
// in light depth. Occluders closer than this don't shadow (self + same-clump);
// dappling clumps/NPCs do. Live: r_grass_self_bias -> L.spot_params.w.
// dynHit (out) = fraction of taps shadowed by the DYN atlas alone (grass/NPC
// casters) while the static atlas leaves them lit — r_grass_debug 11 paints it red.
float vsmSunShadowGrassDirectDbg(vec3 wp, float selfBiasM, out float dynHit)
{
    dynHit = 0.0;
    vec3 lp = (vsmCE.view * vec4(wp, 1.0)).xyz;
    vec2 luv; ivec2 page;
    int  lvl = vsmSelect(lp.xy, vsmCE.level, luv, page);
    if (lvl < 0) return 1.0;
    // Walk COARSER until the STATIC page is mapped (throttle LOD bias marks coarser
    // levels; a dirty-budget deferral unmaps a page for a frame or two) — mirrors the
    // vsm_resolve fallback. Steady state exits on the first iteration.
    uint slotS = VSM_UNMAPPED;
    for (; lvl < VSM_LEVELS; ++lvl) {
        vec2 t = (lp.xy - vsmCE.level[lvl].xy) / vsmCE.level[lvl].z;
        page   = clamp(ivec2(floor(t * float(VSM_PAGES_AXIS))), ivec2(0), ivec2(VSM_PAGES_AXIS - 1));
        slotS  = vsmPageTableEnv[vsmPageIndex(lvl, page)];
        if (slotS < uint(VSM_MAX_PHYS_S)) { luv = t; break; }
    }
    if (lvl >= VSM_LEVELS) return 1.0;
    int  idx   = vsmPageIndex(lvl, page);
    uint slotD = vsmPageTableDynEnv[idx];
    bool hasS  = slotS < uint(VSM_MAX_PHYS_S);
    bool hasD  = slotD < uint(VSM_MAX_PHYS);
    if (!hasS && !hasD) return 1.0;

    vec2  pageLocal = luv * float(VSM_PAGES_AXIS) - vec2(page);
    vec2  baseS = vec2(float(slotS % uint(VSM_ATLAS_W_S)), float(slotS / uint(VSM_ATLAS_W_S)));
    vec2  baseD = vec2(float(slotD % uint(VSM_ATLAS_W)),   float(slotD / uint(VSM_ATLAS_W)));
    float zHere = (lp.z - vsmCE.zparams.x) * vsmCE.zparams.y;
    float bias  = vsmCE.zparams.z;   // static atlas: terrain acne slack (0.6 m at defaults)
    // Dyn atlas: tiny epsilon base (zparams.w — no ground in that atlas to acne on)
    // + the self-shadow slack. The old base was the STATIC bias = 0.6 m along the sun
    // ray, which silently rejected every dyn occluder below knee height (see resolve).
    float biasD = vsmCE.zparams.w + selfBiasM * vsmCE.zparams.y;
    const vec2  dimS  = vec2(float(VSM_ATLAS_W_S), float(VSM_ATLAS_H_S));
    const vec2  dimD  = vec2(float(VSM_ATLAS_W),   float(VSM_ATLAS_H));
    const float tp    = 1.0 / float(VSM_PAGE_SIZE);
    const float inset = 0.5 * tp;
    float lit = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        vec2 pl = clamp(pageLocal + vec2(float(dx), float(dy)) * tp, vec2(inset), vec2(1.0 - inset));
        bool shS = false, shD = false;
        // textureLod(..., 0.0) rather than texture(): both VSM atlases are created with
        // mipLevels = 1 (vk_vsm.cpp), so the two are the same fetch — but the explicit
        // form carries no implicit derivative, which makes this whole function callable
        // from a VERTEX shader. The crown cube tier needs exactly that: it shades once
        // per cube instead of once per fragment, and a fragment-only lookup would have
        // forced either a third copy of this walk or per-pixel cost it cannot afford.
        if (hasS) shS = (zHere - bias  > textureLod(uVsmAtlasS, (baseS + pl) / dimS, 0.0).r);
        if (hasD) shD = (zHere - biasD > textureLod(uVsmAtlasD, (baseD + pl) / dimD, 0.0).r);
        if (shD && !shS) dynHit += 1.0 / 9.0;
        lit += (shS || shD) ? 0.0 : 1.0;
    }
    return lit * (1.0 / 9.0);
}

float vsmSunShadowGrassDirect(vec3 wp, float selfBiasM)
{
    float dh;
    return vsmSunShadowGrassDirectDbg(wp, selfBiasM, dh);
}
#endif // VSM_GRASS_DIRECT

#endif

// xrRenderVulkan — grass (detail) graphics push constants (VS+FS).
//
// THE WHOLE BLOCK, shared by detail.vert and detail.frag — not a hand-copied
// twin per stage. Layout MUST match DetailGfxPushConstants (vk_DetailManager.h,
// 208 B, static_assert'ed there). This block used to be declared TWICE with
// already-diverging field comments — the exact shape of the hand-mirrored
// std430 block mine this renderer has paid for before (a field inserted
// mid-block silently shifts every offset after it). Same cure as the trees:
// one header, one truth (see tree_gfx_push.glsl).
#ifndef DETAIL_GFX_PUSH_GLSL
#define DETAIL_GFX_PUSH_GLSL

layout(push_constant) uniform DetailConstants {
    mat4 mViewProj;         // world → clip
    vec4 wind_params;       // (wind_direction, wind_velocity, treeAmplitude, per-type wind scale: 0 = DO_NO_WAVING) — vertex-only
    vec4 wsetup_grass;      // SSFX grass tunables (animspeed, turbulence, push, wave) — vertex-only
    vec4 wind_anim;         // Environment.wind_anim drift (xyz) + w = minWindSpeed — vertex-only
    vec4 vConsts;           // (r_grass_aref, r_grass_asharp, 0, 0) — fragment-only; zw spare
    vec4 vInteractors[4];   // xyz = pos, w = radius (0 = slot unused) — vertex-only
    vec4 vSunColor;         // env sun colour (rgb), pre-boosted (r_sun_boost)
    // (vHemiColor used to sit here: filled and linearised by the CPU every
    // frame, read by NOTHING — the grass hemi side is lit through skyAmbientUp
    // + L.ambient, not through a pushed colour. Deleted 26-08, 224 → 208 B.)
} pc;

#endif // DETAIL_GFX_PUSH_GLSL

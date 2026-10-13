// nx_wet.glsl (10-10) -- rain wetness for the R1 twins (WORLD_FLAT statics, TERRAIN_FLAT terrain): the R3 / Screen Space
// Shaders logic (gamedata/shaders/r3/rain_patch_normal.ps + rain_apply_gloss.ps) without their G-buffer passes.
//   cover = the straight-down rain map (one bilinear compare; R3: a 4-tap PCF) x the baked sky occlusion x10 (R3's
//           INDOOR_LEAKS_FIX: gbd.hemi * 10 -- no rain under a roof the rain map is too coarse to see)
//   wet   = wetness x cover x up-facing (R3 puts the gloss on horizontal surfaces only) x range
// and wet = a darker albedo (R3 ColorIntensity) + gloss, here a fresnel sheen of the rainy sky
// (no ripples / wall streams: R3's ripple and water-fall bumps are texture fetches more).
// v317: everything under the open sky gets wet, not only the up-facing ground -- dry walls, grass and crowns next to a
// soaked road read wrong. Off the ground the cover is the baked sky occlusion alone (no rain-map fetch, no sheen):
//   walls   x NX_WET_SIDE (a wall takes the rain at an angle; the ceilings of overhangs, n.y < 0, stay dry)
//   foliage x NX_WET_FOLIAGE, per VERTEX in tree_r1.vert / detail_r1v.vert (their light is per vertex): no pixel cost
// Dry (L.rain_params.y == 0: no rain yet, or r_rain 0): one uniform branch, nothing else runs.
// CONTRACT: after light_ubo.glsl and shadow_common.glsl (L, uRainMap, cascTap); with NX_WET_VERTEX defined, after
// light_ubo.glsl only (the foliage term, nothing that reads the rain map).
#ifndef NX_WET_GLSL
#define NX_WET_GLSL

#define NX_WET_SIDE    0.5
#define NX_WET_FOLIAGE 0.8

// 45..60 m (v313, was 150): the far wet ground is a few pixels of darker grey in the rain haze, and the terrain is most of
// the screen -- the wet terrain was +0.4 units at the peak of a rainy firefight (57.8 fps). No sqrt.
float nxWetRange(vec3 wp)
{
    const vec3 d2 = wp - L.eye_pos.xyz;
    return 1.0 - smoothstep(2025.0, 3600.0, dot(d2, d2));
}

// The light multiplier of a wet crown / grass tuft (1 = dry), per vertex. hemi = its baked openness.
float nxWetFoliageLight(vec3 wp, float hemi)
{
    const float w = L.rain_params.y;
    if (w < 0.01)
        return 1.0;
    const float wet = w * NX_WET_FOLIAGE * clamp(hemi * 10.0, 0.0, 1.0) * nxWetRange(wp);
    return 1.0 - max(L.rain_params.z, 0.0) * wet;   // r_wet_darken
}

#ifndef NX_WET_VERTEX
// 0..1: how wet the point is. n = the surface normal (world), hemi = its baked sky occlusion (lightmap / vertex).
float nxWetAmount(vec3 wp, vec3 n, float hemi)
{
    const float w = L.rain_params.y;   // the wetness accumulator (vk_env_light: sqrt of the soak)
    if (w < 0.01)
        return 0.0;
    const float side = NX_WET_SIDE * smoothstep(-0.5, 0.0, n.y);   // walls; 0 on ceilings
    if (side <= 0.0)
        return 0.0;
    const float range = nxWetRange(wp);
    if (range <= 0.0)
        return 0.0;
    const float up = smoothstep(0.35, 0.8, n.y);
    float mapCover = 1.0;   // outside the rain map: open sky
    if (up > 0.0) {
        const vec3 c  = (L.rain_vp * vec4(wp, 1.0)).xyz;   // ortho -> already NDC
        const vec2 uv = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
        if (uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0 && c.z > 0.0 && c.z < 1.0)
            mapCover = cascTap(uRainMap, uv, c.z - 0.0015);
    }
    // the ground takes the rain map; a wall keeps NX_WET_SIDE (the map sees the roof above it and would call it dry)
    return w * clamp(hemi * 10.0, 0.0, 1.0) * range * mix(side, mapCover, up);
}

// The wet look on a lit colour (before fog): darker, plus the sheen of the rainy sky a water film has -- on the ground
// only, not down a wall. (v313: no sun glint -- under rain clouds the sun is all but gone, and its pow() was paid on
// every wet pixel.)
vec3 nxWetApply(vec3 col, float wet, vec3 wp, vec3 n)
{
    if (wet <= 0.0)
        return col;
    col *= 1.0 - max(L.rain_params.z, 0.0) * wet;   // r_wet_darken (0.4); < 0 = the wet debug view, no darkening
    const float up = smoothstep(0.35, 0.8, n.y);
    if (up <= 0.0)
        return col;
    const vec3  V    = normalize(L.eye_pos.xyz - wp);
    const float nv   = max(dot(n, V), 0.0);
    const float fres = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
    // the rainy sky a puddle shows is the horizon: the fog colour is it
    return col + wet * L.fog_color.rgb * (fres * 0.6 * up);
}
#endif   // NX_WET_VERTEX

#endif

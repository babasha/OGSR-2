// xrRenderVulkan - shared dynamic-light shading (point/spot, clustered or UBO
// fallback). Was copy-pasted into world_lmap / world_terrain / world_vlit.
// #include AFTER light_ubo.glsl, shadow_common.glsl (spot/pointShadowF) AND
// cluster_lights.glsl (clusterOfFragment + the SSBOs); after world_variants.glsl
// when the shader has the world variants (SPEC_LAMPSH), otherwise the lamp
// shadow lookups are always compiled in (skinned).
#ifndef LIGHT_SHADE_GLSL
#define LIGHT_SHADE_GLSL

#include "light_atten.glsl"   // dynLightAtten — falloff/cone/N.L, shared with the RT world

#ifdef WORLD_VARIANTS_GLSL
#define LAMP_SHADOWS SPEC_LAMPSH
#else
#define LAMP_SHADOWS true
#endif

#if defined(WORLD_FLAT) || defined(TERRAIN_FLAT)
// 10-09 (Switch, r_flash_mask): the flashlight's shadow x cookie, resolved once per 2x2 into the half-res sun mask's A
// (nx_sunmask.frag) -- the texel whose stored depth is closest to this pixel's, the same pick as nxSunMaskShadow. The
// R1 statics/terrain then shade the torch on the LIGHT variant: the LAMPSH lookups cost them 1-2 ms (v281).
float torchFromMask()
{
    const vec2 uv = gl_FragCoord.xy * 0.5 / vec2(textureSize(uVsmMask, 0));
    const vec4 a  = textureGather(uVsmMask, uv, 3);
    const vec4 dz = abs(textureGather(uVsmMask, uv, 2) - vec4(gl_FragCoord.z));
    float best = dz.x, t = a.x;
    if (dz.y < best) { best = dz.y; t = a.y; }
    if (dz.z < best) { best = dz.z; t = a.z; }
    if (dz.w < best) { t = a.w; }
    return t;
}
#endif

// Shade ONE dynamic point/spot light: linear-squared falloff, N.L diffuse, smooth
// spot cone, + the two shadow-budget lights' shadow maps & cookie. Fields are
// passed in so this serves BOTH the UBO array (fallback) and the clustered SSBO.
// gi = the light's GLOBAL index (for the shadow picks).
vec3 shadeDynLight(vec4 lpos, vec4 lcol, vec4 ldir, vec3 wp, vec3 N, int gi, int sIdx, int pIdx)
{
    // Falloff + cone + wrapped N·L → light_atten.glsl, shared with the RT world so
    // the bounce agrees with the light it is a bounce of. Everything below this
    // line is the part that needs shadow maps, which the probe pass replaces with
    // a shadow RAY.
    vec3  ld;
    float d, ndl;
    float att = dynLightAtten(lpos, lcol, ldir, wp, N, ld, d, ndl);
    if (att <= 0.0) return vec3(0.0);
    vec3 tint = lcol.rgb;
    if (!LAMP_SHADOWS) {   // no lamp has a map or cookie this frame -- or the torch's are in the sun mask
#if defined(WORLD_FLAT) || defined(TERRAIN_FLAT)
        if (gi == sIdx && L.lamp_terrain.y > 0.5) att *= torchFromMask();
#endif
        return tint * (att * max(ndl, 0.0));
    }
    if (lcol.w > 0.5) {
        // Spot shadow POOL: every pooled spot samples its own atlas tile —
        // several headlights/searchlights + the flashlight all shadow at once.
        int tile = spotTileOf(gi);
        if (tile >= 0) att *= spotShadowF(wp, lpos.w, tile);
        // Flashlight cookie (R4 projective light texture): the beam pattern
        // projected through the cookie light's tile VP (L.spot_vp).
        if (gi == sIdx && L.shadow_params.z > 0.5) {
            vec4 cc = L.spot_vp * vec4(wp, 1.0);
            if (cc.w > 0.0) {
                vec2 cuv = (cc.xy / cc.w) * 0.5 + 0.5;
                cuv.y = 1.0 - cuv.y;
                tint *= textureLod(uSpotCookie, clamp(cuv, 0.0, 1.0), 0.0).rgb;
            }
        }
    }
    else {
        // Point shadow POOL: a pooled point samples its own cube.
        int cube = pointCubeOf(gi);
        if (cube >= 0) att *= pointShadowF(wp, lpos.xyz, lpos.w, cube);
    }
    return tint * (att * max(ndl, 0.0));
}

// Dynamic light accumulation. Clustered (r_clustered): iterate only the froxel's
// lights from the SSBO. Fallback: the per-fragment loop over the UBO's 16.
// skip = lights[0..15] to leave out (bit i = light i); the terrain passes
// L.lamp_terrain.x, everything else 0 (folds away).
vec3 dynLightsSkip(vec3 wp, vec3 N, uint skip)
{
    int sIdx = int(L.shadow_params.x);
    int pIdx = int(L.shadow_params.y);
    vec3 acc = vec3(0.0);
    if (L.cluster_params.w > 0.5) {
        int ci  = clusterOfFragment(wp, L.eye_pos.xyz, L.cam_dir.xyz, gl_FragCoord.xy, L.ao_params.xy, L.cluster_params, L.cluster_params2);
        int mxp = int(L.cluster_params2.w);
        int cnt = int(clusterGrid[ci]);
        for (int k = 0; k < cnt; ++k) {
            int gi = int(clusterIndex[ci * mxp + k]);
            if (gi < 16 && ((skip >> uint(gi)) & 1u) != 0u) continue;
            acc += shadeDynLight(clusterLights[gi].pos, clusterLights[gi].color, clusterLights[gi].dir, wp, N, gi, sIdx, pIdx);
        }
    } else {
        int n = int(L.counts.x + 0.5);
        for (int i = 0; i < n; ++i) {
            if (((skip >> uint(i)) & 1u) != 0u) continue;
            acc += shadeDynLight(L.lights[i].pos, L.lights[i].color, L.lights[i].dir, wp, N, i, sIdx, pIdx);
        }
    }
    return acc;
}

vec3 dynLights(vec3 wp, vec3 N)        { return dynLightsSkip(wp, N, 0u); }
// The terrain: lamps standing in another sector than the ground above them (a basement
// lamp under the village) are left out -- the CPU decides that once per lamp.
vec3 dynLightsTerrain(vec3 wp, vec3 N) { return dynLightsSkip(wp, N, uint(L.lamp_terrain.x + 0.5)); }

#endif // LIGHT_SHADE_GLSL

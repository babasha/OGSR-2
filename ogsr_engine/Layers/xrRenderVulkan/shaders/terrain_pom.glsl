// ---- Terrain POM core - SHARED by the COLOR pass (world_terrain.frag) and the
// DEPTH-PREPASS fragment shader (world_terrain_depth.frag). Both write the same
// displaced gl_FragDepth (SSFX "Depth Offset" port) and the color pass depth-tests
// LEQUAL against what the prepass wrote, so EVERY input that feeds the offset
// (parallax hit heights, mud bias) must be computed IDENTICALLY in both. That is
// why the marches + mud setup live here and not in the color shader.
// Requires, declared BEFORE inclusion:
//   - light_ubo.glsl (L) and snow_displace.glsl (SnowDeformOn/SnowDeformPress)
//   - sampler2D uMask (set0 b1) + uDhR/uDhG/uDhB/uDhA (set0 b11..14)
#include "world_variants.glsl"   // SPEC_POM (Inc 1) — gates the terrain POM march below

// MUD deformation (r_mud_deform): a constant per-fragment press bias sunk into the
// POM heightfield, so foot prints get REAL parallax depth in the volumetric ground
// (berm rim = negative press = raised). Constant along the march -> it cancels in
// the self-shadow/AO differences and costs nothing per step.
float g_mudBias = 0.0;

// ---- TERRAIN COMPOSITE CACHE (r_terra_cache): a camera-anchored world-space
// clipmap baked by terrain_cache.comp once per camera-window move - RG16F
// composite HEIGHT (.r; mask x height-blend x CH_OFF resolved) + cone-step
// ratio (.g, terrain_cache_cone.comp) + RGBA8 blend weights. When live (L.tcache_params.x > 0.5) the POM path marches THIS one
// texture instead of 4 per-channel maps, and the fragment blends the native
// detail taps by the BAKED weights (g_cacheBW). Both passes (color + depth
// prepass) read the same frame-static cache -> identical gl_FragDepth holds.
layout(set = ENV_SET, binding = 28) uniform sampler2D uTCacheH;   // composite height
layout(set = ENV_SET, binding = 29) uniform sampler2D uTCacheW;   // blend weights

vec4 g_cacheBW = vec4(1.0, 0.0, 0.0, 0.0);   // baked blend weights at the POM hit
bool g_cacheOn = false;                       // cached path taken this fragment

// Full 2x2 + offset (terrain uv mapping may be rotated/swapped vs world axes):
// cuv = M * duv + b, M rows in tcache_xform, b in tcache_params.zw.
vec2  tcacheUV(vec2 duv) { return vec2(dot(L.tcache_xform.xy, duv), dot(L.tcache_xform.zw, duv)) + L.tcache_params.zw; }
float tcacheH(vec2 duv)  { return clamp(textureLod(uTCacheH, tcacheUV(duv), 0.0).r - g_mudBias, 0.0, 1.0); }

// SSFX TERRAIN_POM_PLANE: the march works in 0..0.5 depth — heights ABOVE the
// plane never displace (the ray starts inside them), only the LOW half sinks.
// Using the full 0..1 range (the old behaviour) doubled the apparent relief and
// "inflated" the asphalt plates into pillows.
const float POM_PLANE = 0.5;
// Per-channel height offsets (SSFX ssfx_terrain_offset, set per level by
// ssfx_terrain_parallax.script). Values = l05_bar VERIFIED against GAMMA's
// live user.ltx: (-0.07, -0.05, -0.13, 0). Asphalt sits only 0.05 down -
// ABOVE the earth (-0.13); the old -0.25 sank it 0.12 BELOW earth = inverted
// double-height cliffs at every pothole edge. TODO terrain_details.ltx.
// Per-level SSFX channel depth offsets — from the UBO (terrain_details.ltx via
// vk_env_light; was a hardcoded l05_bar constant). The composite-cache bake
// receives the SAME values through its push constants (vk_terrain_cache pulls
// EnvLight::TerrainChOff()), so cached and direct marches stay in agreement.
#define CH_OFF (L.ch_off)

// Splat-blended detail height (used by the POM self-shadow / contact-AO marches).
float detailH(vec2 duv, vec4 mask, float lod)
{
    if (g_cacheOn) return tcacheH(duv);   // composite cache: 1 tap instead of 4
    float h = clamp(textureLod(uDhR, duv, lod).r + CH_OFF.x, 0.0, 1.0) * mask.r
            + clamp(textureLod(uDhG, duv, lod).r + CH_OFF.y, 0.0, 1.0) * mask.g
            + clamp(textureLod(uDhB, duv, lod).r + CH_OFF.z, 0.0, 1.0) * mask.b
            + clamp(textureLod(uDhA, duv, lod).r + CH_OFF.w, 0.0, 1.0) * mask.a;
    return clamp(h - g_mudBias, 0.0, 1.0);
}

// Composite-cache march: ONE ray against the baked surface (mask weight already
// resolved at bake -> m = 1, offsets baked). Same SSFX PLANE-0.5 depth curve.
//
// CONE-STEP path (r_terra_cone, L.tcache_params.y): the bake stores in .g a
// conservative "empty cone" ratio (cache-UV distance per height-unit of
// clearance) - each iteration leaps the guaranteed-free gap instead of walking
// fixed layers. Grazing rays stop stair-stepping / missing thin crack walls,
// and typical convergence is 4-8 taps (height+cone come in the SAME tap).
// g_mudBias shifts the whole field uniformly -> the baked cone still holds.
vec2 pomMarchCache(vec2 duv, vec2 Pmax, int steps, out float outH)
{
    float curD  = 0.0;
    vec2  curUV = duv;
    float stepD = POM_PLANE / float(steps);   // refine bracket (both paths)
    if (L.tcache_params.y > 0.5) {
        // duv -> cacheUV scale (max column length: conservative if anisotropic).
        float uvScale = max(length(vec2(L.tcache_xform.x, L.tcache_xform.z)),
                            length(vec2(L.tcache_xform.y, L.tcache_xform.w)));
        float pmaxC = length(Pmax) * uvScale;     // cacheUV travelled per unit ray depth
        for (int i = 0; i < 24; ++i) {
            vec2  hc = textureLod(uTCacheH, tcacheUV(curUV), 0.0).rg;
            float h  = clamp(hc.r - g_mudBias, 0.0, 1.0);
            float sd = (POM_PLANE - h) - curD;    // clearance down to the surface
            if (sd <= 0.0015 || curD >= POM_PLANE) break;
            // Leap staying inside the cone: pmaxC*dlt <= cone*(sd - dlt).
            float dlt = hc.g * sd / max(pmaxC + hc.g, 1e-6);
            // Forced 12% progress vs tiny cones (relaxed - the refine cleans up).
            dlt = clamp(max(dlt, sd * 0.12), 0.0, POM_PLANE - curD);
            curD += dlt; curUV -= Pmax * dlt;
            stepD = dlt;
        }
    } else {
        // Fixed-layer walk (r_terra_cone 0 fallback).
        vec2  dUV  = Pmax * stepD;
        float curH = POM_PLANE - tcacheH(curUV);
        for (int i = 0; i < 64; ++i) {
            if (i >= steps || curD >= curH) break;
            curUV -= dUV; curD += stepD;
            curH = POM_PLANE - tcacheH(curUV);
        }
    }
    vec2 sUV = Pmax * stepD; float sD = stepD;
    for (int j = 0; j < 6; ++j) {     // binary refine of the intersection
        sUV *= 0.5; sD *= 0.5;
        if (curD < POM_PLANE - tcacheH(curUV)) { curUV -= sUV; curD += sD; }
        else                                   { curUV += sUV; curD -= sD; }
    }
    outH = tcacheH(curUV);
    return curUV;
}

// Mud footprint setup (must match between color and depth prepass - feeds
// g_mudBias which shifts the POM heightfield): read the persistent press field
// (vk_deform, stamped once per foot PLANT) on SOFT splat channels. Soil softness
// varies per material (earth deep, gravel shallow, asphalt none) and rain SOFTENS
// it. Outputs the raw press + carve/grime amounts for the color-pass shading.
void terrainMudSetup(vec4 mask, vec3 wp, out float mudPress, out float mudCarve, out float mudGrime)
{
    mudPress = 0.0; mudCarve = 0.0; mudGrime = 0.0;
    float mudWet = clamp(L.rain_params.y, 0.0, 1.0);
    if (L.pom_params5.z > 0.0 && SnowDeformOn()) {
        float soft  = dot(mask, vec4(0.85, 0.0, 1.0, 0.35))   // R grass, G asphalt, B earth, A gravel
                    * (1.0 + mudWet * 0.8);                   // rain loosens the soil
        float grime = mask.g * mudWet * 0.7;                  // wet mud tracked onto asphalt (shading only)
        float gate  = (1.0 - clamp(L.sf_params.w, 0.0, 1.0))
                    * (1.0 - smoothstep(45.0, 60.0, distance(L.eye_pos.xyz, wp)));
        if (max(soft, grime) * gate > 0.003) {
            mudPress  = SnowDeformPress(wp);
            mudCarve  = soft * gate * L.pom_params5.z;
            mudGrime  = grime * gate * L.pom_params5.z;
            // POM carve: dent sinks, BERM boosted x2.4 so the squeezed-out rim reads.
            float biasP = (mudPress > 0.0) ? mudPress : mudPress * 2.4;
            g_mudBias = clamp(clamp(biasP, -1.0, 1.0) * mudCarve * L.pom_params7.z, -0.9, 0.9);
        }
    }
}

// The 4 per-channel flat heights at one uv (CH_OFF + mud bias applied) — the
// no-march hits and the absent-channel heights of the per-channel march path.
vec4 detailHits4(vec2 duv, float lod)
{
    return vec4(clamp(clamp(textureLod(uDhR, duv, lod).r + CH_OFF.x, 0.0, 1.0) - g_mudBias, 0.0, 1.0),
                clamp(clamp(textureLod(uDhG, duv, lod).r + CH_OFF.y, 0.0, 1.0) - g_mudBias, 0.0, 1.0),
                clamp(clamp(textureLod(uDhB, duv, lod).r + CH_OFF.z, 0.0, 1.0) - g_mudBias, 0.0, 1.0),
                clamp(clamp(textureLod(uDhA, duv, lod).r + CH_OFF.w, 0.0, 1.0) - g_mudBias, 0.0, 1.0));
}

// Per-channel SSFX-style march (TerrainParallax): marches ONE channel's OWN
// height field with the splat weight folded into the depth plane (SSFX
// `curr_Height = PLANE - curr_Sam * S.mask`) - a low-weight material sits
// deeper, the dominant one keeps its FULL-depth relief. This is exactly what
// the old single averaged-field march flattened away in mixed zones.
vec2 pomMarchChannel(sampler2D hmap, float m, float choff, vec2 duv, vec2 Pmax, int steps, float lod, out float outH)
{
    #define CH_H(uv) clamp(clamp(textureLod(hmap, uv, lod).r + choff, 0.0, 1.0) - g_mudBias, 0.0, 1.0)
    // SSFX depth curve: curr_Height = PLANE - h*mask, ray from 0 — a texel at or
    // above the plane (h*m >= 0.5) is hit immediately = ZERO offset. Total march
    // budget = PLANE (0.5), not 1.0.
    float layerH = POM_PLANE / float(steps);
    vec2  dUV    = Pmax * layerH;
    float curD   = 0.0;
    vec2  curUV  = duv;
    float curH   = POM_PLANE - CH_H(curUV) * m;
    for (int i = 0; i < 64; ++i) {
        if (i >= steps || curD >= curH) break;
        curUV -= dUV; curD += layerH;
        curH = POM_PLANE - CH_H(curUV) * m;
    }
    vec2 sUV = dUV; float sD = layerH;
    for (int j = 0; j < 6; ++j) {     // binary refine of the intersection
        sUV *= 0.5; sD *= 0.5;
        if (curD < POM_PLANE - CH_H(curUV) * m) { curUV -= sUV; curD += sD; }
        else                                    { curUV += sUV; curD -= sD; }
    }
    outH = CH_H(curUV);
    #undef CH_H
    return curUV;
}

// 4 INDEPENDENT marches (SSFX architecture) instead of one averaged field.
// Outputs per-channel parallax-offset DETAIL uvs + per-channel hit heights
// (these feed the height-based blend), plus sun self-shadow, contact AO and
// the mask-blended height (feeds micro-AO / puddles / the depth offset). All
// height taps use an explicit lod so the marches are derivative-safe inside
// the loops. `aux` = also compute self-shadow/contact-AO (the depth prepass
// passes false - it only needs the hit heights).
void terrainPOM4(vec2 duv, vec4 mask, vec3 N, vec3 wp,
                 out vec2 uvs[4], out vec4 hits,
                 out float outShadow, out float outAO, out float outH, bool aux)
{
    vec2  dtsz = vec2(textureSize(uDhR, 0));
    vec2  ddx  = dFdx(duv) * dtsz, ddy = dFdy(duv) * dtsz;
    float lod  = max(0.5 * log2(max(dot(ddx, ddx), dot(ddy, ddy))), 0.0) + L.pom_params2.x;

    // Composite cache live AND this fragment inside the baked camera window
    // (2% inset covers the max parallax offset; outside -> per-channel path,
    // same rule in both passes so the zoff depth equality holds).
    vec2 cuv0 = tcacheUV(duv);
    g_cacheOn = L.tcache_params.x > 0.5
             && all(greaterThan(cuv0, vec2(0.02))) && all(lessThan(cuv0, vec2(0.98)));
    uvs[0] = duv; uvs[1] = duv; uvs[2] = duv; uvs[3] = duv;
    outShadow = 1.0; outAO = 1.0;

    // Whether the march below actually runs is decided BEFORE the flat-path
    // height taps: the marching paths overwrite them unread, so taking them up
    // front was 2 wasted taps on exactly the most expensive pixels. Every input
    // here is shared by both passes (UBO + duv + wp) -> the zoff depth equality
    // holds.
    // r_pom_height is shared with wall POM (world metres there); terrain reads it
    // in detail-uv units x2 - SSFX-depth relief without touching the wall knob.
    float amp   = L.pom_params.x * 2.0;
    float dist  = length(L.eye_pos.xyz - wp);
    float range = L.pom_params.z * 1.6;      // r_pom_far x1.6 ~= SSFX TERRAIN_POM_RANGE 20
    float fade  = 1.0 - smoothstep(range * 0.8, range, dist);
    // Cache live: fade the march out toward the WINDOW edge instead of falling
    // back to the per-channel path outside it. Once r_pom_far outgrew the ±24 m
    // window (vk_terrain_cache kHalf — its "POM range ~19 m fits" note was
    // written for r_pom_far 12), the 24..48 m ring was paying 4 independent
    // marches + the 8x4-tap shadow fallback every frame for ~a texel of visible
    // offset. The ramp ends AT the 2% inset, so marched (inside) and flat
    // (outside) pixels meet at zero amplitude - no seam - and the ~4 m band
    // absorbs window recenters. r_terra_cache 0 keeps the old full-range
    // per-channel fallback for A/B.
    if (L.tcache_params.x > 0.5) {
        float edge = min(min(cuv0.x, 1.0 - cuv0.x), min(cuv0.y, 1.0 - cuv0.y));
        amp *= clamp((edge - 0.02) * 12.5, 0.0, 1.0);   // 0 at the inset, full at 10% in
    }
    // SPEC_POM off = the terrain-POM-off variant (r_pom_terrain disabled this frame):
    // the 4-channel march below (+ its VGPRs) is dead-code-eliminated. The flat
    // hits/detH setup still runs — puddles/splat/zoff need it. zoff requires
    // r_pom_terrain, so SPEC_POM=false ⟺ zoff off ⟺ no gl_FragDepth path → depth
    // stays consistent.
    bool doMarch = SPEC_POM && L.pom_params4.x >= 0.5
                && amp > 0.0 && fade > 0.002 && amp * fade > 1e-5;
    if (!doMarch) {   // flat (normal-map only): plain height taps at duv
        if (g_cacheOn) {
            hits = vec4(tcacheH(duv));
            g_cacheBW = textureLod(uTCacheW, tcacheUV(duv), 0.0);
        } else {
            hits = detailHits4(duv, lod);
        }
        outH = dot(hits, mask);
        return;
    }
    // NOTE: the wall-POM floor/ceil damp (r_pom_floor 0.75) is NOT applied to the
    // offset any more - terrain is ALWAYS a floor and SSFX runs it at full height
    // (GAMMA ssfx_terrain_pom height 0.04 = our 0.02 x2, undamped). orient is kept
    // only to soften the self-shadow/AO below.
    float orient = (N.y >= 0.0)
        ? mix(1.0, L.pom_params3.w, clamp( N.y, 0.0, 1.0))   // up-facing (floor)
        : mix(1.0, L.pom_params3.z, clamp(-N.y, 0.0, 1.0));  // down-facing (ceiling)
    amp *= fade;

    // Screen-space cotangent frame (terrain has no per-vertex tangents).
    vec3 dp1 = dFdx(wp), dp2 = dFdy(wp);
    vec2 du1 = dFdx(duv), du2 = dFdy(duv);
    vec3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
    vec3 T = dp2p * du1.x + dp1p * du2.x;
    vec3 B = dp2p * du1.y + dp1p * du2.y;
    float inv = inversesqrt(max(dot(T, T), dot(B, B)));
    T *= inv; B *= inv;

    vec3 V   = normalize(L.eye_pos.xyz - wp);
    vec3 Vts = vec3(dot(V, T), dot(V, B), dot(V, N));
    // SSFX grazing handling: NO fade-out, only the +0.41 denominator bias (accept
    // mild swim for depth). The old smoothstep(0.12,0.45) killed the parallax at
    // exactly the grazing angle the floor is viewed from -> flat ground.
    vec2 Pmax = (Vts.xy / (abs(Vts.z) + 0.41)) * amp;

    int steps = int(clamp(mix(L.pom_params.y, 12.0, abs(Vts.z)) * mix(0.5, 1.0, fade), 8.0, 64.0));

    if (g_cacheOn) {
        // COMPOSITE CACHE: one march against the baked surface; blend weights
        // re-sampled at the parallax hit so the material boundary shifts with it.
        float h1;
        vec2 uv1 = pomMarchCache(duv, Pmax, steps, h1);
        uvs[0] = uv1; uvs[1] = uv1; uvs[2] = uv1; uvs[3] = uv1;
        hits = vec4(h1);
        g_cacheBW = textureLod(uTCacheW, tcacheUV(uv1), 0.0);
    } else {
        // March each present channel independently against its OWN height map.
        // Uniform ground (one mask channel) still costs a single march. Absent
        // channels keep their FLAT height (it still steers the height blend
        // through maxw), so tap all four first.
        hits = detailHits4(duv, lod);
        if (mask.r >= 0.005) uvs[0] = pomMarchChannel(uDhR, mask.r, CH_OFF.x, duv, Pmax, steps, lod, hits.r);
        if (mask.g >= 0.005) uvs[1] = pomMarchChannel(uDhG, mask.g, CH_OFF.y, duv, Pmax, steps, lod, hits.g);
        if (mask.b >= 0.005) uvs[2] = pomMarchChannel(uDhB, mask.b, CH_OFF.z, duv, Pmax, steps, lod, hits.b);
        if (mask.a >= 0.005) uvs[3] = pomMarchChannel(uDhA, mask.a, CH_OFF.w, duv, Pmax, steps, lod, hits.a);
    }
    outH = dot(hits, mask);

    if (!aux) return;   // depth prepass: hit heights only

    // Self-shadow / contact AO stay on the blended field at the mask-weighted
    // hit UV (low-frequency effects - per-channel would be 4x the taps).
    vec2 curUV = uvs[0] * mask.r + uvs[1] * mask.g + uvs[2] * mask.b + uvs[3] * mask.a;

    // Sun self-shadow: short height march toward the light in tangent space.
    // HORIZON MAP path (r_terra_horizon, tcache_params.y = 2): the bake stored
    // the directional horizon toward the current sun azimuth in height .b -
    // a geometric tan(sun) vs tan(occluder) test replaces the 8-tap march, so
    // crack self-shadowing is exact for the full 32-texel reach (the march
    // only saw 0.6*Pmax ahead) and costs ONE tap.
    if (L.pom_params2.z > 0.0) {
        vec3 Ld  = normalize(-L.sun_dir.xyz);
        vec3 Lts = vec3(dot(Ld, T), dot(Ld, B), dot(Ld, N));
        if (Lts.z > 0.02 && dot(Lts.xy, Lts.xy) > 1e-6) {
            if (g_cacheOn && L.tcache_params.y > 1.5) {
                float hor = textureLod(uTCacheH, tcacheUV(curUV), 0.0).b;   // dh per cacheUV
                float uvScale = max(length(vec2(L.tcache_xform.x, L.tcache_xform.z)),
                                    length(vec2(L.tcache_xform.y, L.tcache_xform.w)));
                // Occluder slope in tangent space: heights live at z = h*amp
                // (duv units), horizontal cache-UV -> duv is /uvScale. The
                // (1+str) boost aligns the shadow with the PERCEIVED relief -
                // parallax reads ~2x deeper than the geometric centimetres, so
                // the honest tangent test alone barely ever shadows; str
                // (r_pom_shadow) scales the reach, its 0..1 part the darkness.
                float tanOcc = hor * uvScale * amp * (1.0 + L.pom_params2.z);
                float tanSun = Lts.z / max(length(Lts.xy), 1e-4);
                float lit = (tanOcc > 1e-5) ? smoothstep(tanOcc * 0.55, tanOcc * 1.2, tanSun) : 1.0;
                outShadow = mix(1.0, lit, clamp(L.pom_params2.z, 0.0, 1.0) * orient);
            } else {
                vec2  sdir  = normalize(Lts.xy);
                float reach = length(Pmax) * 0.6;
                float occ   = 0.0;
                for (int s = 1; s <= 8; ++s)
                    occ = max(occ, detailH(curUV + sdir * reach * (float(s) * 0.125), mask, lod) - outH);
                outShadow = clamp(1.0 - occ * L.pom_params2.z * 16.0 * (1.0 - Lts.z) * orient, 0.0, 1.0);
            }
        }
    }
    // Contact AO from surrounding height pits.
    if (L.pom_params2.w > 0.0) {
        float reach = length(Pmax) * 0.5 + 1.0 / max(dtsz.x, 1.0);
        float aoSum =
              max(0.0, detailH(curUV + vec2( reach, 0.0), mask, lod) - outH)
            + max(0.0, detailH(curUV + vec2(-reach, 0.0), mask, lod) - outH)
            + max(0.0, detailH(curUV + vec2(0.0,  reach), mask, lod) - outH)
            + max(0.0, detailH(curUV + vec2(0.0, -reach), mask, lod) - outH);
        outAO = clamp(1.0 - (aoSum * 0.25) * L.pom_params2.w * 5.0, 0.35, 1.0);
        outAO = mix(1.0, outAO, orient);
    }
}

// SSFX terrain DEPTH OFFSET (deffer_terrain_high_flat_d.ps "Depth Offset" port):
// push the fragment depth along the view ray by the POM hit height, so depth
// consumers (GTAO from the prepass depth, the VSM screen resolve) see the CARVED
// micro-surface - AO and sun shadows wrap INTO the cracks instead of reading a
// flat plane. SSFX: offset_pos = pos + eyeVec * (d1 / min(d2, -0.3)), with
// d1 = dot(N, N*(1-h*1.5)*-0.11) = -0.11*(1-1.5h). We keep only the "sink"
// half (max 0): never lift toward the camera, which preserves the depth_greater
// early-Z promise and avoids terrain poking through props resting on it.
// Beyond the POM march range hits are the plain height samples (cheap) - like
// SSFX, the offset applies at ALL distances.
float terrainZoffDepth(mat4 mvp, vec3 wp, vec3 N, float pixelH, float geomDepth)
{
    float str = L.zoff_params.x;             // r_pom_zoff (0 = off; pipeline choice gates too)
    if (str <= 0.0) return geomDepth;
    vec3  eyeVec = normalize(wp - L.eye_pos.xyz);
    float d1 = (1.0 - pixelH * 1.5) * -0.11 * str;
    float d2 = min(dot(eyeVec, N), -0.3);    // SSFX slope limiter
    float push = max(d1 / d2, 0.0);          // sink only (metres along the view ray)
    if (push <= 0.0) return geomDepth;
    vec4 clip = mvp * vec4(wp + eyeVec * push, 1.0);
    return clamp(clip.z / max(clip.w, 1e-6), 0.0, 1.0);
}

#version 450
#extension GL_GOOGLE_include_directive : require
#include "light_ubo.glsl"       // DynLight + Lighting UBO (set 1 b0) + set-1 samplers (b1..13)
#include "world_variants.glsl"   // SPEC_POM/SNOW/WET/IBL/DEBUG variant spec constants (Inc 1)
#include "wet_common.glsl"       // vHash/vNoise/puddlesMaskProc/rippleLayer/rainRipples
#define VSM_GRASS_DIRECT         // also pull in vsmSunShadowGrassDirect (crisp atlas at own world pos) for beam-gap recovery
#include "vsm_sample.glsl"       // vsmSunShadow (set 1 b14) + vsmSunShadowGrassDirect (b15/16/23/24/25)
#include "cluster_lights.glsl"   // clustered forward (set 1 b17..19, r_clustered)
#include "shadow_common.glsl"    // spotShadowF/pointShadowF/cascTap/cascSample/sunShadow
#include "env_common.glsl"       // gtaoVis/gtaoBentN/coloredAO/skyAmbient/rainVis
#include "surface_field.glsl"    // SF_Concavity — real ground dips for puddle placement (r_puddle_geo)
#include "light_shade.glsl"      // shadeDynLight/dynLights/dynLightsTerrain
#include "flow_sim_sample.glsl"  // simWater*/simFlow/groundHm/waterDebugColor/flowWaves
#include "wetness.glsl"          // applyWetnessTerrain + applyWetnessCore
#include "surface_field.glsl"    // SF_* (smart heightmap; r_sf_debug viz)
#include "surface_class.glsl"    // SC_* surface classification (r_sf_debug 5)
#include "snow_displace.glsl"    // SnowFootprint (fragment footprint dimple)
#include "froxel.glsl"           // Froxel_SliceFromViewZ — probe the shaft in-scatter for the sun-beam ground deposit
#include "ddgi_receiver.glsl"    // ddgiAmbient — probe field in the ambient (set 1 b34..36)
#include "rr_guides.glsl"        // DLSS Ray Reconstruction guide outputs (locations 1..3)

// Sun-beam ground-deposit scaffolding (uVolume + shaftOverhead) — PARKED; see the file.
#include "terrain_frag_beam.glsl"

// EARLY_ZTEST twin of the R1 terrain (world_terrain_flat_earlyz.frag.spv, -DTERRAIN_FLAT -DEARLY_ZTEST): the depth
// test runs BEFORE this shader, so terrain fragments the depth prepass already hid never launch. The alpha-test
// discard alone made the draw late-Z. Only legal without depth write (the prepass laid the terrain's depth with the
// same tessellated VS) -- the same contract as the statics *_earlyz twins (world_frag_body.glsl).
#ifdef EARLY_ZTEST
layout(early_fragment_tests) in;
#endif

// World pass - TERRAIN splatting fragment shader (R4 deffer_terrain_high /
// CBlender_BmmD): albedo = 2*base*detail, detail blended by the RGBA splat mask.
// Detail channels: R grass, G asphalt, B earth, A gravel. Adds per-channel detail
// normal maps, micro contact-AO, dry sun-gloss, SSS puddles. Shared lighting/
// shadow/wet helpers live in the includes above; the terrain-specific pieces
// are split into terrain_frag_*.glsl textual includes (helpers below, plus the
// 4 main() stage bodies at the bottom) — this file keeps only the interface:
// set-0 samplers, push constants, the vertex IO and the ZOFF depth export.

layout(set = 0, binding = 0)  uniform sampler2D uBase;   // terrain diffuse
layout(set = 0, binding = 1)  uniform sampler2D uMask;   // RGBA splat weights
layout(set = 0, binding = 2)  uniform sampler2D uDtR;    // grass   detail
layout(set = 0, binding = 3)  uniform sampler2D uDtG;    // asphalt detail
layout(set = 0, binding = 4)  uniform sampler2D uDtB;    // earth   detail
layout(set = 0, binding = 5)  uniform sampler2D uDtA;    // gravel/yantar detail
layout(set = 0, binding = 6)  uniform sampler2D uLmap;
layout(set = 0, binding = 7)  uniform sampler2D uDnR;    // grass   normal (R4: n = tex.wzy*2-1, gloss = tex.r)
layout(set = 0, binding = 8)  uniform sampler2D uDnG;    // asphalt normal
layout(set = 0, binding = 9)  uniform sampler2D uDnB;    // earth   normal
layout(set = 0, binding = 10) uniform sampler2D uDnA;    // gravel  normal
layout(set = 0, binding = 11) uniform sampler2D uDhR;    // grass   height (SSFX-style, .r = elevation)
layout(set = 0, binding = 12) uniform sampler2D uDhG;    // asphalt height
layout(set = 0, binding = 13) uniform sampler2D uDhB;    // earth   height
layout(set = 0, binding = 14) uniform sampler2D uDhA;    // gravel  height

layout(push_constant) uniform PushConstants {
    mat4  mvp;
    vec2  uvScale;
    float alphaRef;
    float detailScale;
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 1) in  vec2 vDetailUV;
layout(location = 2) in  vec2 vLmapUV;
layout(location = 3) in  vec3 vWorldPos;
layout(location = 4) in  vec3 vNormal;
layout(location = 0) out vec4 outColor;

// ---- Terrain POM (SSFX-style REAL heightfield parallax) - the marches, mud
// bias and the SSFX depth offset live in terrain_pom.glsl, SHARED with the
// depth-prepass FS (world_terrain_depth.frag) so both passes write the exact
// same displaced gl_FragDepth. Compiled with -DZOFF -> the variant that exports
// the displaced depth (world_terrain_zoff.frag.spv).
#include "terrain_pom.glsl"

#ifdef ZOFF
layout(depth_greater) out float gl_FragDepth;   // sink-only offset keeps early-Z
#endif

// Terrain-specific surface helpers (textual includes).
#include "terrain_frag_detail_normal.glsl"   // detailNormal(): 4-channel bump blend + cotangent frame
#include "terrain_frag_puddle.glsl"          // sssPuddle(): SSFX heightfield puddle placement

// main() is assembled from 4 stage bodies (textual includes — locals flow
// across stages; each file header lists what it defines for the ones below).
#ifdef TERRAIN_FLAT
#define NX_SUNMASK_READ
#include "nx_sun_shadow.glsl"   // nfs46: the half-res sun mask reader
#include "nx_wet.glsl"          // 10-10: rain wetness (the R3 logic, no G-buffer)
#include "nx_fog.glsl"          // 10-10: the fog fades into the sky behind (r_fog_sky)
// R1 terrain sun shadow (Switch v102): one textureGather (2x2 bilinear PCF, cascTap) per cascade instead of sunShadow()'s
// 2x2 spread of four -- 16 texels -> 4 per pixel over most of the screen. Same cascade order, biases and insets.
float sunShadowFast(vec3 wp)
{
    vec3 n  = (L.sun_near_vp * vec4(wp, 1.0)).xyz;   // ortho -> already NDC
    vec2 uv = vec2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
    if (uv.x >= 0.01 && uv.x <= 0.99 && uv.y >= 0.01 && uv.y <= 0.99 && n.z > 0.0 && n.z < NX_C0_ZMAX)   // nfs47
        return cascTap(uShadowNear, uv, n.z - 0.0004);
    n  = (L.sun_c1_vp * vec4(wp, 1.0)).xyz;
    uv = vec2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
    if (uv.x >= 0.01 && uv.x <= 0.99 && uv.y >= 0.01 && uv.y <= 0.99 && n.z > 0.0 && n.z < NX_C1_ZMAX)   // nfs47
        return cascTap(uShadowC1, uv, n.z - 0.0006);
    vec4 c = L.sun_vp * vec4(wp, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 ndc = c.xyz / c.w;
    uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    return cascTap(uShadow, uv, ndc.z - 0.0015);
}
#endif

void main()
{
    // -nx_bisect2 "(T: const colour)" (Switch, vk_env_light sets zoff_params.w = 7 for ONE frame): geometry, tessellation
    // and depth stay, the splat shader is skipped.
    if (L.zoff_params.w > 6.5 && L.zoff_params.w < 7.5) { outColor = vec4(0.22, 0.24, 0.16, 1.0); return; }
#ifdef TERRAIN_FLAT
    // r_world_flat 1 twin (Switch, v100): R1-class terrain, the twin of WORLD_FLAT for statics. The full body below cost
    // ~7.5 ms on GM20B (4 POM marches, detail normals/heights, mud/puddles/snow/gloss/IBL/DDGI/AO taps). Kept: splat mask,
    // base, the 4 details, the same albedo (2*base*detail) and the SAME light terms the full path ends up with on the
    // console -- sky fill from the lightmap occlusion, sun N.L x the cascade shadow (sunShadow, as the full path with VSM
    // off), flat ambient, dynamic lights -- and the fog. The 13-tap rain-map ambient gate becomes the lightmap occlusion.
    {
        // -nx_bisect2 sub-experiments (v101, zoff_params.w): 8 no dyn lights, 9 no sun shadow, 10 no details (no mask,
        // no 4 detail fetches), 11 no lightmap, 12 no sky SH, 13 the old 4-gather sunShadow(). 0 = normal.
        int te = int(L.zoff_params.w + 0.5);
        if (te == 0) te = int(L.lamp_terrain.z + 0.5);   // 10-09 r_terra_probe: the same sub-experiments, terrain only
        if (te == 7) { outColor = vec4(0.22, 0.24, 0.16, 1.0); return; }   // r_terra_probe 7: the splat shader skipped
        vec3 geomN = normalize(vNormal);
        vec4 base = texture(uBase, vUV, L.spot_flash.z);
#ifndef NO_DISCARD   // nx_statab 37 (v144, world_terrain_flat_nokill.frag.spv): the R1 terrain unable to kill
        if (pc.alphaRef >= 0.0 && base.a < pc.alphaRef) discard;
#endif
        vec3 detail = vec3(0.5);
        if (te != 10) {
            vec4 mask  = texture(uMask, terrainMaskUV(vUV, vWorldPos));
            float wsum = dot(mask, vec4(1.0));
            mask = (wsum > 1e-4) ? (mask / wsum) : vec4(1.0, 0.0, 0.0, 0.0);
#ifdef TERRAIN_FAR
            // v150 far terrain (world_terrain_far.frag.spv, nx_statab 47 = on ALL terrain to price it): far away the
            // detail mips have already averaged each layer to its mean colour -- the coarsest mip IS that mean. Same
            // address for every pixel (no detail UV interpolant, no derivatives), so the 4 fetches are cache hits.
            detail = textureLod(uDtR, vec2(0.5), 16.0).rgb * mask.r
                   + textureLod(uDtG, vec2(0.5), 16.0).rgb * mask.g
                   + textureLod(uDtB, vec2(0.5), 16.0).rgb * mask.b
                   + textureLod(uDtA, vec2(0.5), 16.0).rgb * mask.a;
#else
            detail = texture(uDtR, vDetailUV, L.spot_flash.z).rgb * mask.r
                   + texture(uDtG, vDetailUV, L.spot_flash.z).rgb * mask.g
                   + texture(uDtB, vDetailUV, L.spot_flash.z).rgb * mask.b
                   + texture(uDtA, vDetailUV, L.spot_flash.z).rgb * mask.a;
#endif
        }
        vec3 albedo = 2.0 * base.rgb * detail;
        vec4  lm      = vec4(0.5, 0.5, 0.5, 1.0);
        if (te != 11) lm = texture(uLmap, vLmapUV);
        float hemiOcc = dot(lm.rgb, vec3(1.0 / 3.0));
        float nl      = max(dot(geomN, normalize(-L.sun_dir.xyz)), 0.0);
        float sunSh   = 1.0;
        // puddle_geo.w > 0.5: no sun shadow maps this frame (r_sun_shadows 0 only clears them) -> "lit", without reading.
        // nfs46: puddle_geo.w < -0.5 = read the half-res sun mask (no cover fallback here: the R1 terrain has no baked sun term)
        if (te != 9 && L.puddle_geo.w < -0.5) { float cov; sunSh = (nl > 0.005) ? nxSunMaskShadow(gl_FragCoord.xy, gl_FragCoord.z, cov) : 0.0; }
        else if (te != 9 && L.puddle_geo.w < 0.5) sunSh = (nl > 0.005) ? ((te == 13) ? sunShadow(vWorldPos) : sunShadowFast(vWorldPos)) : 0.0;
        vec3 sky = vec3(0.5);
        if (te != 12) sky = skyAmbient(geomN);
        vec3 lighting = sky * (hemiOcc * L.sky_params.y)
                      + L.sun_color.rgb * (nl * sunSh)
                      + L.ambient.rgb * hemiOcc;
#ifndef TERRAIN_FAR   // far terrain: past the flashlight's reach (Switch r_light_cap 0 leaves no other lamp)
        if (te != 8) lighting += dynLightsTerrain(vWorldPos, geomN);
#endif
        vec3 col  = albedo * lighting;
        col = nxWetApply(col, nxWetAmount(vWorldPos, geomN, hemiOcc), vWorldPos, geomN);   // 10-10: wet in the rain
        float fog = clamp(length(vWorldPos - L.eye_pos.xyz) * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
        if (fog > 0.0) col = mix(col, nxFogColor(vWorldPos, fog), fog);   // 10-10 r_fog_sky: into the sky behind
        outColor  = vec4(col, base.a);
        return;
    }
#endif
#include "terrain_frag_parallax.glsl"   // 1: mask/mud/POM/zoff depth/refraction/base + early debug views
#include "terrain_frag_albedo.glsl"     // 2: detail splat -> albedo, detail normal, micro-AO, mud, puddles
#include "terrain_frag_lighting.glsl"   // 3: snow, sun + beam, ambient/DDGI, lighting sum
#include "terrain_frag_final.glsl"      // 4: gloss, wetness, IBL, fog, RR guides, outColor
}

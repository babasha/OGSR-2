#extension GL_GOOGLE_include_directive : require
#include "world_bindless.glsl"   // r_world_bindless twins (-DWORLD_BINDLESS): first -- its #extension precedes every declaration
#include "light_ubo.glsl"       // DynLight + Lighting UBO (set 1 b0) + set-1 samplers (b1..13)
#include "world_variants.glsl"   // SPEC_POM/SNOW/WET/IBL/DEBUG variant spec constants (Inc 1)
#include "wet_common.glsl"       // vHash/vNoise/puddlesMaskProc/rippleLayer/rainRipples
#include "vsm_sample.glsl"       // vsmSunShadow (set 1 b14)
#include "cluster_lights.glsl"   // clustered forward (set 1 b17..19, r_clustered)
#include "shadow_common.glsl"    // spotShadowF/pointShadowF/cascTap/cascSample/sunShadow
#include "env_common.glsl"       // gtaoVis/gtaoBentN/coloredAO/skyAmbient/rainVis
#include "light_shade.glsl"      // shadeDynLight/dynLights
#include "flow_sim_sample.glsl"  // simWater*/simFlow/groundHm/waterDebugColor/flowWaves
#include "wetness.glsl"          // applyWetness (lmap/vlit) + applyWetnessCore
#include "bump_common.glsl"      // uTexBumpN (set 0 b4): material normal + gloss
#include "surface_field.glsl"    // SF_* (smart heightmap; r_sf_debug viz)
#include "surface_class.glsl"    // SC_* surface classification (r_sf_debug 5)
#include "tex_feedback.glsl"     // txfbReport — texture-streaming GPU feedback (set 1 b30)
#include "ddgi_receiver.glsl"    // ddgiAmbient — probe field in the ambient (set 1 b34..36)
#include "rr_guides.glsl"
#ifdef WORLD_FLAT
#define NX_SUNMASK_READ                 // nfs46: + the half-res mask reader
#include "nx_sun_shadow.glsl"   // nfs43: live sun shadow for the R1 statics
#include "nx_wet.glsl"          // 10-10: rain wetness (the R3 logic, no G-buffer)
#include "nx_fog.glsl"          // 10-10: the fog fades into the sky behind (r_fog_sky)
#endif
//        // DLSS Ray Reconstruction guide outputs (locations 1..3)

// EARLY_ZTEST twin (glslc -DEARLY_ZTEST → *_earlyz.frag.spv): force the depth
// test BEFORE this shader runs. The txfbReport atomic is an FS side effect, and
// without this the driver must LATE-Z the draw (6-8x invocations vs visible,
// Кордон 23-07-2026). With it, occluded fragments never execute — feedback
// stays per-frame at zero occluded cost. ONLY legal on pipelines that don't
// write depth (early tests would write before the AT discard) — the pipeline
// cache binds this twin exactly for the no-z-write statics variants
// (Key::noZWrite / Key::atEqual, see EarlyTwin in vk_pipeline_cache.cpp).
#ifdef EARLY_ZTEST
layout(early_fragment_tests) in;
#endif

// World pass, BOTH static-lighting variants (-DWORLD_VLIT picks vert-lit):
//   lmap — albedo x baked LIGHTMAP (hemi/AO)
//   vlit — albedo x baked VERTEX lighting (offline point lights + bounce, no sun)
// plus, identically for both: dynamic R4-style sun (per-pixel N·L x shadow map),
// env hemisphere sky fill, rain wetness and distance fog.
//
// The two used to be separate 470/399-line files that shared ~245 lines verbatim —
// POM, every debug view, wetness, IBL, glass and fog were maintained twice, and the
// vlit copy's comments had already decayed into "see world_lmap.frag" pointers. The
// ONLY real difference is where the baked occlusion scalar comes from; everything
// below keys off `bakeOcc` and is variant-agnostic. Shared lighting/shadow/wet
// helpers live in the includes above; only POM and main() stay here.

#ifndef WORLD_BINDLESS   // the bindless twins take these from uWorldTex[] (world_bindless.glsl)
layout(set = 0, binding = 0) uniform sampler2D uTexDiffuse;
layout(set = 0, binding = 1) uniform sampler2D uTexDetail;
layout(set = 0, binding = 2) uniform sampler2D uTexLmap;   // vlit: unused (white fallback)
layout(set = 0, binding = 3) uniform sampler2D uTexBumpX;   // .a = height (POM); flat=1 -> no parallax
#define WORLD_DETAIL_SCALE pc.detailScale
#define WORLD_STREAM_ID    pc.streamID
#endif

layout(push_constant) uniform PushConstants {
    mat4  mvp;
    vec2  uvScale;
    float alphaRef;
    float detailScale;
    float dynHemi;       // sky-ambient gate: 1.0 statics, ray-traced 0..1 for dynamics
    // 84..116 = per-frame tess block (TES); the FS only needs the feedback id past it.
    layout(offset = 116) uint streamID;   // texture-streaming feedback slot (0xFFFFFFFF = none)
} pc;

// Varyings. The two variants carry DIFFERENT baked data, so the slot assignment
// differs — it must keep matching world_{lmap,vlit}_vert_body.glsl exactly.
#ifdef WORLD_VLIT
layout(location = 0) in  vec2  vUV;
layout(location = 1) in  vec2  vDetailUV;
layout(location = 2) in  vec3  vBakedColor;  // offline point lights + bounce (NO direct sun)
layout(location = 3) in  float vSunMask;     // (legacy per-vertex sun mask; sun is now dynamic)
layout(location = 4) in  vec3  vWorldPos;
layout(location = 5) in  vec3  vNormal;
layout(location = 7) in  float vBakedHemi;   // NORMAL.a = baked sky access (see world_vlit_vert_body)
#else
layout(location = 0) in  vec2  vUV;
layout(location = 1) in  vec2  vDetailUV;
layout(location = 2) in  vec2  vLmapUV;
layout(location = 3) in  vec3  vWorldPos;
layout(location = 4) in  vec3  vNormal;
#endif
layout(location = 0) out vec4  outColor;

// Material roughness. Used by BOTH the IBL/sun-spec term and the RR guide, from
// ONE definition on purpose: a guide that describes different roughness than the
// shading used is worse than no guide at all, and two copies of an expression
// this tunable would drift the first time either is touched.
float worldRoughness(float matGloss, float glossScale)
{
    return clamp(0.85 - clamp(matGloss * glossScale, 0.0, 1.0) * 0.75, 0.1, 0.95);
}

// Height from the `#` alpha (geometry-based, R4's displacement source - NOT
// albedo, which on multi-tone brickwork makes dark-vs-light bricks into false
// cliffs). HIGH-PASSED against a blurred baseline: keeps only the detail (mortar
// grooves), centered at 0.5 so it carves both ways. gain deepens it.
float pomDepth(vec2 uv, float lod, float baseline)
{
    float h = textureLod(uTexBumpX, uv, lod).a;
    return clamp(0.5 - (h - baseline) * 4.0, 0.0, 1.0);
}

// Parallax occlusion (relief) mapping: steep linear march + binary refine, in a
// per-pixel screen-derivative tangent frame (Schueler) - no per-vertex tangent.
// Gated to materials with a real `#` (flat fallback alpha=1 -> skipped).
vec2 parallaxUV(vec2 uv, vec3 N, vec3 wp, out vec3 outN, out float outShadow, out float outAO)
{
    outN = N;          // default: unperturbed geometric normal (early-outs below)
    outShadow = 1.0;   // default: no self-shadow
    outAO = 1.0;       // default: no contact AO
    float amp = L.pom_params.x;
    if (amp <= 0.0) return uv;
    if (textureLod(uTexBumpX, vec2(0.5), 8.0).a > 0.985) return uv;   // flat material -> no POM
    float dist = length(L.eye_pos.xyz - wp);
    float fade = 1.0 - smoothstep(L.pom_params.z * 0.5, L.pom_params.z, dist);   // full to far*0.5, gone by far
    amp *= fade;
    // Per-orientation strength: walls/fences (N.y~0) full; floors (N.y->+1) ->
    // r_pom_floor; ceilings (N.y->-1) -> r_pom_ceil.
    float orient = (N.y >= 0.0)
        ? mix(1.0, L.pom_params3.w, clamp( N.y, 0.0, 1.0))   // floor (up-facing)
        : mix(1.0, L.pom_params3.z, clamp(-N.y, 0.0, 1.0));  // ceiling (down-facing)
    amp *= orient;
    if (amp <= 1e-5) return uv;

    vec2 tsz  = vec2(textureSize(uTexBumpX, 0));
    vec2 ddx  = dFdx(uv) * tsz, ddy = dFdy(uv) * tsz;
    float lod = max(0.5 * log2(max(dot(ddx, ddx), dot(ddy, ddy))), 0.0) + L.pom_params2.x;
    float baseline = textureLod(uTexBumpX, uv, lod + 3.0).a;   // low-freq baseline for the high-pass

    // Cotangent frame from world-pos + uv derivatives (maps tangent -> world).
    vec3 dp1 = dFdx(wp),  dp2 = dFdy(wp);
    vec2 du1 = dFdx(uv),  du2 = dFdy(uv);
    vec3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
    vec3 T = dp2p * du1.x + dp1p * du2.x;
    vec3 B = dp2p * du1.y + dp1p * du2.y;
    float inv = inversesqrt(max(dot(T, T), dot(B, B)));
    T *= inv; B *= inv;

    // r_pom_height is WORLD metres, converted to THIS material's UV units via
    // the UV→world Jacobian (sqrt of world-area per UV-area). A fixed UV
    // amplitude looked right on densely-mapped brick but on cliffs/rocks —
    // whose UVs stretch metres per texel — the same 0.02 UV meant METRE-deep
    // relief: the march shredded them into "spikes". World-normalized depth
    // keeps walls as tuned and collapses on stretched mappings by itself.
    // The cap bounds tiny-density UVs (decal-scale) to the old-look ballpark.
    float uvArea = abs(du1.x * du2.y - du1.y * du2.x);
    float wPerUV = sqrt(length(cross(dp1, dp2)) / max(uvArea, 1e-12));
    amp = min(amp / max(wPerUV, 1e-3), 0.035);

    vec3 V   = normalize(L.eye_pos.xyz - wp);
    vec3 Vts = vec3(dot(V, T), dot(V, B), dot(V, N));
    vec2 Pmax = (Vts.xy / max(abs(Vts.z), 0.3)) * amp;   // total UV shift at full depth

    // More steps at grazing angle; FEWER as POM fades with distance.
    int steps = int(clamp(mix(L.pom_params.y, 12.0, abs(Vts.z)) * mix(0.5, 1.0, fade), 8.0, 64.0));
    float layerH = 1.0 / float(steps);
    vec2 dUV = Pmax * layerH;

    // Steep linear march to the first layer below the surface.
    float curD = 0.0;
    vec2  curUV = uv;
    float curH = pomDepth(curUV, lod, baseline);
    for (int i = 0; i < 64; ++i) {
        if (i >= steps || curD >= curH) break;
        curUV -= dUV;
        curD  += layerH;
        curH   = pomDepth(curUV, lod, baseline);
    }
    // Binary-search refine (relief mapping): removes the up-close stair-step shimmer.
    vec2 sUV = dUV; float sD = layerH;
    for (int j = 0; j < 6; ++j) {
        sUV *= 0.5; sD *= 0.5;
        if (curD < pomDepth(curUV, lod, baseline)) { curUV -= sUV; curD += sD; }
        else                                       { curUV += sUV; curD -= sD; }
    }

    // Perturbed normal from the height gradient at the hit point.
    float tU = exp2(lod) / tsz.x, tV = exp2(lod) / tsz.y;
    float hu = textureLod(uTexBumpX, curUV + vec2(tU, 0.0), lod).a
             - textureLod(uTexBumpX, curUV - vec2(tU, 0.0), lod).a;
    float hv = textureLod(uTexBumpX, curUV + vec2(0.0, tV), lod).a
             - textureLod(uTexBumpX, curUV - vec2(0.0, tV), lod).a;
    float ns = L.pom_params2.y * 12.0 * fade * orient;
    vec3 nTS = normalize(vec3(-hu * ns, -hv * ns, 1.0));
    outN = normalize(T * nTS.x + B * nTS.y + N * nTS.z);

    // Self-shadow: horizon scan toward the sun through the height field.
    if (L.pom_params2.z > 0.0) {
        vec3 Ld  = normalize(-L.sun_dir.xyz);   // to-sun (sun_dir travels downward)
        vec3 Lts = vec3(dot(Ld, T), dot(Ld, B), dot(Ld, N));
        vec2 lxy = Lts.xy;
        if (Lts.z > 0.02 && dot(lxy, lxy) > 1e-6) {
            vec2  sdir  = normalize(lxy);
            float reach = (exp2(lod) / min(tsz.x, tsz.y)) * 6.0;
            float h0    = textureLod(uTexBumpX, curUV, lod).a;
            float occ   = 0.0;
            for (int s = 1; s <= 8; ++s) {
                float hs = textureLod(uTexBumpX, curUV + sdir * reach * (float(s) * 0.125), lod).a;
                occ = max(occ, hs - h0);
            }
            outShadow = clamp(1.0 - occ * L.pom_params2.z * 20.0 * (1.0 - Lts.z) * orient, 0.0, 1.0);
        }
    }
    // POM-AO: view-independent contact occlusion from the heightfield.
    if (L.pom_params2.w > 0.0) {
        float aoReach = (exp2(lod) / min(tsz.x, tsz.y)) * 4.0;
        float h0 = textureLod(uTexBumpX, curUV, lod).a;
        float aoSum =
              max(0.0, textureLod(uTexBumpX, curUV + vec2( aoReach, 0.0), lod).a - h0)
            + max(0.0, textureLod(uTexBumpX, curUV + vec2(-aoReach, 0.0), lod).a - h0)
            + max(0.0, textureLod(uTexBumpX, curUV + vec2(0.0,  aoReach), lod).a - h0)
            + max(0.0, textureLod(uTexBumpX, curUV + vec2(0.0, -aoReach), lod).a - h0);
        outAO = clamp(1.0 - (aoSum * 0.25) * L.pom_params2.w * 6.0, 0.35, 1.0);
        outAO = mix(1.0, outAO, orient);   // ceilings: dial contact AO down too
    }
    return curUV;
}

#ifdef CLUSTER_FADE
#ifdef WORLD_VLIT
layout(location = 6) flat in uint vFadeBits;
#else
layout(location = 5) flat in uint vFadeBits;
#endif
// 4x4 Bayer screen-door for the LOD crossfade: the appearing cluster keeps
// dither < fadeIn while its parent keeps dither >= fadeOut; both fades come
// from the SAME sphere/error values in the cull, so coverage sums to 1.
float clusterDither()
{
    const float kBayer[16] = float[16](0.,8.,2.,10., 12.,4.,14.,6., 3.,11.,1.,9., 15.,7.,13.,5.);
    ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    return (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
}
#endif

void main()
{
#ifdef CLUSTER_FADE
    {
        uint fA = (vFadeBits >> 20) & 63u;   // fade-in vs parent (63 = solid)
        uint fB = (vFadeBits >> 26) & 63u;   // fade-out to children (0 = none)
        if (fA < 63u || fB > 0u) {
            float d = clusterDither();
            if (d >= float(fA) / 63.0 || d < float(fB) / 63.0) discard;
        }
    }
#endif
    // POM: offset the base UV before sampling; shift the detail UV by the same
    // world-space delta. Lightmap UV is NOT parallaxed (low-freq baked light).
    // SPEC_POM: true only for materials with a real `#` height (mat->tessellated).
    // Flat materials bake the false variant → the whole parallaxUV call and its VGPR
    // footprint vanish (same result the runtime flat-probe gave, off the register budget).
    // Shading normal, flipped toward the VIEWER when it points away: X-Ray statics
    // include one-sided zero-thickness sheets rendered two-sided (village plank
    // walls) — seen from behind, the vertex normal points THROUGH the sheet toward
    // the sun, so the interior "wall" shaded as sun-lit and no shadow bias could
    // save it (receiver depth ≈ caster depth, coplanar sheets). NOT gl_FrontFacing:
    // X-Ray content's winding is uncorrelated with its vertex normals (the D3D
    // renderer never used facing), so trusting it inverted CORRECT normals — the
    // wallpapered interior quad of the same wall lit up whole. dot(N, toEye) is
    // winding-agnostic: it only answers "does this normal face the visible side".
    vec3 vN = normalize(vNormal);
    if (dot(vN, L.eye_pos.xyz - vWorldPos) < 0.0) vN = -vN;
    vec3 pomN = vN; float pomShadow = 1.0, pomAO = 1.0;
    vec2 pUV = vUV;
    if (SPEC_POM)
        pUV = parallaxUV(vUV, vN, vWorldPos, pomN, pomShadow, pomAO);
    vec2 pDetailUV = vDetailUV + (pUV - vUV) * WORLD_DETAIL_SCALE;

#ifdef WORLD_FLAT
    // -nx_bisect2 statics sub-experiments (Switch, 01-10; vk_env_light sets zoff_params.w for ONE frame, 0 = normal):
    // 1 no diffuse texture (cutout materials keep the fetch for their alpha test), 2 diffuse mip +1, 3 no lightmap,
    // 4 no dynamic lights, 5 unlit (texture only), 6 constant colour (no texture, no lighting) = raster floor.
    int nxe = int(L.zoff_params.w + 0.5);
#else
    const int nxe = 0;
#endif
    vec4 base;
    if (nxe == 2)
        base = texture(uTexDiffuse, pUV, 1.0);
    else if ((nxe == 1 || nxe == 6) && pc.alphaRef < 0.0)
        base = vec4(0.5, 0.5, 0.5, 1.0);
    else
        base = texture(uTexDiffuse, pUV, L.spot_flash.z);   // DLSS mip bias (0 native)
    // Alpha UNBIASED for the cutout test — biased alpha mips shrink coverage on
    // fences/grates/foliage cards (black-hole artifacts under DLSS upscaling).
    if (L.spot_flash.z != 0.0) base.a = texture(uTexDiffuse, pUV).a;
    // Streaming feedback: the LOD this sample actually wanted (unclamped — negative
    // = resident mips are too coarse), before the cutout discard so even mostly-
    // discarded fences report. Includes the DLSS bias the color sample used.
    // SPEC_FEEDBACK = streamer master gate (off ⇒ the atomic is DCE'd away).
    // Occlusion cost is handled by the EARLY_ZTEST twin above, not by gating.
    if (SPEC_FEEDBACK)
        txfbReport(WORLD_STREAM_ID, textureQueryLod(uTexDiffuse, pUV).y + L.spot_flash.z);
    if (pc.alphaRef >= 0.0 && base.a < pc.alphaRef) discard;

    // EMISSIVE-ADDITIVE (aref == -3 ONLY: effects\glow halos, selflight): unlit
    // texture ADDS over the scene; lighting/fog don't apply. ANGLE FADE for the
    // crossed halo quads (edge-on quad → 0). Lightplanes are aref -4, below.
    if (pc.alphaRef < -2.5 && pc.alphaRef > -3.5) {
        vec3  Vv   = normalize(L.eye_pos.xyz - vWorldPos);
        float face = abs(dot(normalize(vNormal), Vv));
        float w    = face * face;
        outColor = vec4(base.rgb * 2.0 * w, base.a * w);
        return;
    }

    // r_ssao_debug 1: show the AO factor this pixel ACTUALLY multiplies its ambient
    // by — i.e. gtaoVis(), strength exponent included. It used to sample uAO raw,
    // which made r_ssao_strength look DEAD in this view (the pow lives in gtaoVis)
    // and sent a whole session chasing a knob that was working fine. The unmodified
    // buffer is still available: the readback stats and ssao_dump.bin read s_img[0]
    // directly and are untouched by the exponent.
    if (SPEC_DEBUG && L.ao_params.w > 0.5) {
        outColor = vec4(vec3(gtaoVis()), base.a);
        return;
    }

    // r_wet_debug 1 (darken arrives negative): rain-map visibility.
    if (SPEC_DEBUG && L.rain_params.z < 0.0) {
        outColor = vec4(vec3(rainVis(vWorldPos)), base.a);
        return;
    }

    // r_bump_debug: 1 = decoded WORLD normal, 2 = material gloss. The R4 unpack is
    // .wzy for the normal and .x for gloss; a mod texture authored as a plain RGB
    // normal map comes out of that swizzle inverted, which is trivial to spot as a
    // colour field and nearly impossible to spot as "the walls light oddly".
    if (SPEC_DEBUG && L.bump_params.y > 0.5) {
        float dbgG;
        vec3  dbgN = bumpNormal(normalize(vNormal), vWorldPos, pUV, 1.0, dbgG);
        outColor = (L.bump_params.y > 1.5) ? vec4(vec3(dbgG), base.a)
                                           : vec4(dbgN * 0.5 + 0.5, base.a);
        return;
    }

    // r_puddle_debug: sim on -> depth/flow; else the SSS puddle coverage (grayscale).
    int pdbg = int(L.pom_params5.w + 0.5);
    if (SPEC_DEBUG && pdbg > 0) {
        outColor = (L.pom_params6.x > 0.5)
            ? vec4(waterDebugColor(vWorldPos, pdbg), base.a)
            : vec4(vec3(puddlesMaskProc(vWorldPos.xz, clamp(L.rain_params.y * L.pom_params7.y * 1.5, 0.0, 1.0), L.pom_params7.w)), base.a);
        return;
    }

    // r_pom_debug 1: POM occlusion mask (contact AO x sun self-shadow).
    if (SPEC_DEBUG && L.pom_params3.x > 0.5) {
        outColor = vec4(vec3(pomAO * pomShadow), base.a);
        return;
    }

    // r_clustered_debug: per-cluster dynamic-light-count heatmap (blue 0 .. red many).
    if (SPEC_DEBUG && L.cluster_params.w > 1.5) {
        int cnt = clusterLightCount(vWorldPos, L.eye_pos.xyz, L.cam_dir.xyz, gl_FragCoord.xy, L.ao_params.xy, L.cluster_params, L.cluster_params2);
        outColor = vec4(clusterHeat(cnt), base.a);
        return;
    }

    // r_sf_debug: Surface Field viz (1 height, 2 slope, 3 curvature, 4 sky, 5 canopy).
    int sfdbg = int(L.sf_params.y + 0.5);
    if (SPEC_DEBUG && sfdbg > 0) {
        vec3 nn = normalize(vNormal);
        outColor = vec4((sfdbg == 5) ? SC_DebugColor(SC_Refine(SC_STATIC, nn))   // lmap = building, vlit = prop
                                      : SF_DebugColor(vWorldPos, nn, sfdbg), base.a);
        return;
    }

#ifdef WORLD_FLAT
    // r_world_flat 1 (WORLD_FLAT twin of this shader): R1 lighting. The original R1 renderer lit a lightmapped static as
    //     light = hemi + sun_color * N.L * lmap.a (+ baked static lights + dynamic lights)      out = diffuse * light
    // with the SUN'S SHADOW BAKED INTO THE LIGHTMAP ALPHA (the level's lmap#N_2 alpha is exactly that mask: building and road
    // footprints) and only the sun's colour/direction live. So static-on-static sun shadows cost one texel fetch and no
    // shadow map. No detail, no POM/bump, no IBL/GI/AO/wet/snow/SSAO. vlit statics have no baked sun mask in the data, so
    // their sky access stands in for it.
    {
        if (nxe >= 5) { outColor = vec4(base.rgb, base.a); return; }
        vec3  sunTo = normalize(-L.sun_dir.xyz);
        float nl    = max(dot(vN, sunTo), 0.0);
    #ifdef WORLD_VLIT
        float hemi   = clamp(vBakedHemi, 0.0, 1.0);
        float sunVis = hemi;
        vec3  baked  = vBakedColor * 1.5;
    #else
        vec4  lmv    = vec4(0.5, 0.5, 0.5, 1.0);
        if (nxe != 3) lmv = texture(uTexLmap, vLmapUV);
        float hemi   = dot(lmv.rgb, vec3(1.0 / 3.0));
        float sunVis = clamp(lmv.a * 2.0, 0.0, 1.0);
        vec3  baked  = vec3(0.0);
    #endif
        // nfs43: where a live sun map covers the point it replaces the baked mask (which was baked for one fixed sun);
        // beyond the far map the baked lightmap alpha stays. puddle_geo.w > 0.5 = no maps this frame.
        if (L.puddle_geo.w < 0.5 && nl > 0.005) {
            float cov;
            // nfs46: puddle_geo.w < -0.5 = the half-res sun mask was resolved this frame -> one depth-aware read.
            const float live = (L.puddle_geo.w < -0.5) ? nxSunMaskShadow(gl_FragCoord.xy, gl_FragCoord.z, cov)
                                                       : nxSunShadowCov(vWorldPos, cov);
            sunVis = mix(sunVis, live, cov);
        }
        vec3 light = L.sun_color.rgb * (nl * sunVis)
                   + skyAmbient(vN) * (hemi * L.sky_params.y)
                   + L.ambient.rgb * hemi
                   + baked
                   + vec3(0.10);                      // minimum light: an unlit room is dark, not black
        if (nxe != 4) light += dynLights(vWorldPos, vN);
        vec3 col = base.rgb * light * 2.0;   // R1: out = diffuse * light * 2 (same x2 the full path has in albedo)
        col = nxWetApply(col, nxWetAmount(vWorldPos, vN, hemi), vWorldPos, vN);   // 10-10: wet in the rain
        // 10-10: the distance fog, as the full path below and the R1 terrain/crowns/grass have it -- the fog ends at the far
        // plane (Environment_misc: fog_distance <= far_plane_render - 10), so the world fades out instead of being cut
        float fog = clamp(length(vWorldPos - L.eye_pos.xyz) * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
        if (fog > 0.0) col = mix(col, nxFogColor(vWorldPos, fog), fog);   // r_fog_sky: into the sky behind
        outColor = vec4(col, base.a);
        return;
    }
#endif
    vec3 detail = texture(uTexDetail, pDetailUV, L.spot_flash.z).rgb;
    vec3 albedo = 2.0 * base.rgb * detail;

    // THE baked occlusion scalar — the one place the two variants genuinely differ.
    // Everything downstream (sky fill, IBL sky visibility, r_ao_flat, r_shade_debug)
    // reads `bakeOcc` and needs to know nothing about where it came from. The SUN is
    // fully dynamic in BOTH, R4-style: per-pixel N.L against the live sun direction x
    // the shadow map. sun_color/ambient arrive FINAL from vk_env_light.
#ifdef WORLD_VLIT
    // vlit has no lightmap, but it is NOT missing the data: X-Ray bakes the sky
    // access into the packed NORMAL's alpha (v_static_color `Nh : NORMAL //
    // (nx,ny,nz,hemi occlusion)`), which deffer_base_flat.ps reads straight back as
    // `h = I.position.w`. This is the lmap path's lightmap occlusion twin, so it
    // enters the sky term with the SAME weight (no extra 0.5).
    //
    // It used to be guessed from the baked vertex COLOUR instead — but that channel
    // holds offline POINT lights, which outdoors is nothing at all: measured over
    // level.geom, mean luminance is 0.006 on Cordon and exactly 0.000 on Pripyat
    // (its vertex colour AND sun mask are 100% zero). So the guess collapsed to its
    // own 0.15 floor for every vert-lit static on the map, open sky or basement
    // alike — benches, kerbs and fences rendered as if standing in the dark.
    float bakeOcc = clamp(vBakedHemi, 0.0, 1.0);
#else
    // Lightmap keeps HEMI/AO duty only: lm.rgb -> occlusion scalar.
    vec4  lm      = texture(uTexLmap, vLmapUV);
    float bakeOcc = dot(lm.rgb, vec3(1.0 / 3.0));
#endif
    vec3  geomN   = vN;   // flat, backface-flipped - for the sky fill (sharp cube -> perturbed = mirror)
    vec3  Nw      = pomN;   // POM-perturbed normal -> sun + dyn lights catch the relief
    // MATERIAL normal + gloss (`<bump>.dds`). REFINES the POM normal rather than
    // replacing it: parallax carries the big relief, the map carries the grain.
    // Materials without one bind a flat/zero-gloss 1x1, so this is a no-op for them.
    float matGloss = 0.0;
    Nw = bumpNormal(Nw, vWorldPos, pUV, L.bump_params.x, matGloss);
    // SNOW (Surface Field consumer, r_snow): whiten by surface type x slope x sky
    // exposure - flat up-facing roofs/floors accumulate, walls/ceilings/steep none.
    // SF_SkyExposure() is ~9 rainVis gathers — skip the whole snow query when there
    // is none this frame (summer: sf_params.w == 0). Identical result: the term was
    // multiplied by clamp(sf_params.w,0,1) == 0 anyway, only the taps are saved.
    float snow = 0.0;
    if (SPEC_SNOW && L.sf_params.w > 0.0)
        snow = SC_SnowAmount(SC_Refine(SC_STATIC, geomN), geomN, SF_SkyExposure(vWorldPos)) * clamp(L.sf_params.w, 0.0, 1.0);
    albedo = mix(albedo, vec3(0.90, 0.93, 0.97), snow);
    Nw = normalize(mix(Nw, geomN, snow));   // snow smooths the microrelief -> less "texturey"

    float sunMask = max(dot(Nw, normalize(-L.sun_dir.xyz)), 0.0);
    if (sunMask > 0.005) {
        // VSM (r_vsm) vs the cascade path - gated per frame by shadow_params.w.
        float sunSh = (L.shadow_params.w > 0.5) ? vsmSunShadow(gl_FragCoord.xy * L.ao_params.xy)
                                                : sunShadow(vWorldPos);
        sunMask *= sunSh * pomShadow;   // sun shadow x POM groove self-shadow
    }

    // Hemisphere sky fill (R4 hmodel) replaces the flat hemi term.
    vec3  occ      = coloredAO(gtaoVis(), albedo) * pomAO * ssilBoost();   // GTAO x POM AO x SSIL bounce (ambient only)
    // dynHemi < -0.5 = dynamic visual (sign carries the "model xform follows" flag
    // for the VS); the real ray-traced sky visibility is -dynHemi-1.
    float dynHemiL = (pc.dynHemi < -0.5) ? (-pc.dynHemi - 1.0) : pc.dynHemi;
    if (SPEC_DEBUG && L.pom_params3.y > 0.5) { occ = vec3(1.0); bakeOcc = 1.0; dynHemiL = 1.0; }   // r_ao_flat debug
    // Ambient takes the POM-perturbed normal now (see the note in world_terrain.frag):
    // the "sharp cube -> perturbed = mirror" reason for pinning it to geomN died with
    // the switch to SH9 irradiance, which is smooth by construction.
    //
    // The sky-hemisphere term, before AO. Named because the DDGI probe field
    // REPLACES exactly this quantity (r_ddgi_light) — see ddgi_receiver.glsl for
    // why replacing and not adding, and why the baked scalar stays on top of it.
    // With SPEC_DDGI off the ternary folds away and the sum below is the one that
    // was here before the volume existed.
    // ⚠ The scalar keeps its ORIGINAL factor order (float addition/multiplication
    // is not associative — the same rule that keeps the two sums below spelled out
    // per variant). bakeSky is the sky-ACCESS scalar on its own, for DDGI.
    vec3  skyFill  = skyAmbient(gtaoBentN(Nw)) * (bakeOcc * L.sky_params.y * dynHemiL);
    float bakeSky  = bakeOcc * dynHemiL;
    vec3  ambFill  = SPEC_DDGI ? ddgiAmbient(skyFill, vWorldPos, Nw,
                                             normalize(L.eye_pos.xyz - vWorldPos), bakeSky)
                               : skyFill;

    // ⚠ THE FLAT FILL IS THE SAME SKY, COUNTED A SECOND TIME (r_gi_flat).
    // `L.ambient.rgb` is the weather's constant fill — an estimate of light
    // arriving from the sky, which is exactly what the probe field measures for
    // real. While both are in the sum, GI only steers PART of the fill and its
    // effect is diluted by a term that knows nothing about geometry: measured at
    // 24/255 of change in the ambient term arriving as 3/255 in the frame.
    // ddgi_params.w fades the flat term out in proportion to how much of the
    // ambient the field is actually answering — the same "replace, do not add"
    // rule ddgi_receiver.glsl applies to the analytic sky, finally applied to the
    // OTHER sky term too. 0 keeps the old sum bit-for-bit; the shipping default is 1,
    // so with the field bound the flat term is fully displaced.
    // ⚠ THE DISPLACEMENT HAS TO BE PER-PIXEL, BECAUSE THE FIELD'S ANSWER IS.
    // ddgi_params.w rides giWeight, which says "the field is bound THIS FRAME", not
    // "the field reaches THIS pixel". Left frame-uniform it took the flat fill away
    // everywhere, while past the volume ambFill had already faded back to skyFill -
    // so distant geometry lost the fill and gained no GI in its place. Scaling it by
    // ddgiVolumeFade, the SAME factor ddgiAmbient crossfades the lookup with, hands
    // the fill back exactly where the field stops answering. Inside the volume that
    // factor is 1 and the sum is unchanged, so this can only act out at the edge.
    // r_shade_debug 8 is the view that shows this term as shipped.
    const float flatK = SPEC_DDGI ? (1.0 - L.ddgi_params.w * ddgiVolumeFade(vWorldPos)) : 1.0;

    // The two sums are spelled out per variant rather than folded into one with a
    // conditional first term: float addition is not associative, so sharing the
    // expression would have to pick ONE order and silently shift the other variant's
    // last bits. vlit's extra term is the baked vertex lighting lmap has no analogue for.
    // AO on the ambient: full on the analytic sky, r_ao_gi of it on the traced share
    // (ddgiAmbientAO — the field/gather already carries its own visibility).
    const vec3 occAmb = SPEC_DDGI ? ddgiAmbientAO(occ, vWorldPos) : occ;
#ifdef WORLD_VLIT
    vec3 lighting = vBakedColor * 1.5
                  + L.sun_color.rgb  * sunMask
                  + ambFill * occAmb
                  + L.ambient.rgb * occ * skyAmbientGate(vWorldPos) * flatK   // flat sky fill gated by sky visibility (no indoor leak), faded by the field
                  + dynLights(vWorldPos, Nw);
#else
    vec3 lighting = ambFill * occAmb
                  + L.sun_color.rgb  * sunMask
                  + L.ambient.rgb * occ * skyAmbientGate(vWorldPos) * flatK   // flat sky fill gated by sky visibility (no indoor leak), faded by the field
                  + dynLights(vWorldPos, Nw);
#endif

    // r_shade_debug (zoff_params.z): lighting-component isolation. Same view
    // numbers in world_lmap / world_vlit / world_terrain:
    //  1 albedo  2 baked lightmap/vertex data  3 hemi-occ scalar  4 GTAO
    //  5 ambient sky gate  6 sky hemisphere colour  7 total lighting (no albedo)
    //  8 flat-ambient term AS SHIPPED (r_gi_flat folded in)  9 sun  10 wetness
    // 11 the ambient term AS SHIPPED — analytic sky fill with r_ddgi_light 0, the
    //    probe field with it up. The A/B for the whole of step 2 is this view
    //    against view 6 (which stays analytic) in ONE run.
    int ldbg = int(L.zoff_params.z + 0.5);
    if (SPEC_DEBUG && ldbg > 0) {
        vec3 dbg =
            (ldbg == 1) ? albedo :
#ifdef WORLD_VLIT
            (ldbg == 2) ? vBakedColor :
#else
            (ldbg == 2) ? lm.rgb :
#endif
            (ldbg == 3) ? vec3(bakeOcc * dynHemiL) :
            (ldbg == 4) ? vec3(gtaoVis()) :
            (ldbg == 5) ? vec3(skyAmbientGate(vWorldPos)) :
            (ldbg == 6) ? skyAmbient(gtaoBentN(Nw)) :
            (ldbg == 7) ? lighting :
            (ldbg == 8) ? L.ambient.rgb * occ * skyAmbientGate(vWorldPos) * flatK :
            (ldbg == 9) ? L.sun_color.rgb * sunMask :
            (ldbg == 11) ? ambFill :
                          vec3(clamp(L.rain_params.y, 0.0, 1.0) * rainVis(vWorldPos));
        outColor = vec4(dbg, 1.0);
        return;
    }

    // Rain wetness: darken + sky reflection where the rain map says open sky.
    // SPEC_WET: false in dry weather → the whole procedural wet path (noise/ripples/
    // reflection) is compiled out. applyWetness already returns 0 when dry, so identical.
    vec3 wetRefl = vec3(0.0);
    if (SPEC_WET)
        wetRefl = applyWetness(albedo, vWorldPos, Nw, sunMask, gtaoBentN(geomN), gtaoVisRaw());

    // Sky specular IBL + sun GGX glint (r_ibl): dielectric grazing sky reflection
    // on every surface + a real sun highlight (the world sun path was diffuse-only).
    // Dielectric F0; roughness = matte base, glossier in wet weather on up-faces.
    // Occlusion-gated so it can't glow out of crevices. The strong puddle mirror
    // stays in applyWetness (this is the subtle base reflection under it).
    // r_ibl master gate hoisted up: iblSpecular/sunSpec self-early-out at ibl_params.x,
    // but the setup around them (Vv, skyVis, wetF's ~9 rainVis gathers, roughI) ran
    // regardless. Skip it all when IBL is off (default) — output identical (0 below).
    vec3  specIBL = vec3(0.0);
    float specE   = 0.0;   // reflected fraction — taken OFF the diffuse below
    if (SPEC_IBL && L.ibl_params.x > 0.004) {
        vec3  Vv      = normalize(L.eye_pos.xyz - vWorldPos);
        // sky reflection only where the surface SEES sky (baked occlusion × dynHemi).
        float skyVis  = smoothstep(0.12, 0.5, clamp(bakeOcc * dynHemiL, 0.0, 1.0));
        // ROUGHNESS FROM THE MATERIAL (r_gloss_scale). This was a flat 0.55 for the
        // entire world — plaster, rusted steel and glass reflecting the sky
        // identically, which is what made r_ibl read as one lacquer film instead of
        // material highlights. matGloss comes from the bump's .x (R4 gloss).
        // DRY BASE ONLY. This used to drop roughness 0.55 -> 0.25 on every up-facing
        // surface the moment it rained, which is two mistakes at once: it applied ONE
        // material to the entire world (statics carry no gloss map in this path, so
        // the roughness here is a stand-in, not data), and it duplicated the wet
        // reflection that applyWetness already owns behind r_wet_refl. Two systems
        // brightening the same pixels is why rain washed the frame out. Wet belongs to
        // applyWetness; this stays the honest dry sky term.
        float roughI  = worldRoughness(matGloss, L.bump_params.z);
        vec3  Rv      = reflect(-Vv, Nw);
        // Directional visibility along the REFLECTED ray, not just AO: the same
        // bent-normal cone the puddle mirror uses. Plain AO let the sky glow out of
        // crevices and from under overhangs, which is a second source of flat wash.
        float sOcc    = specOcclusion(gtaoBentN(geomN), Rv, gtaoVisRaw());
        float vis     = sOcc * skyVis;
        specIBL       = iblSpecular(Nw, Vv, roughI, vec3(0.04)) * vis;
        specIBL      += L.sun_color.rgb * sunSpec(Nw, Vv, normalize(-L.sun_dir.xyz), roughI, vec3(0.04)) * sunMask;
        specE         = iblSpecWeight(Nw, Vv, roughI, vec3(0.04)) * vis;
    }

    // Distance fog (R4): fade to the env haze colour with view distance.
    // Energy: the mirrored fraction is not also transmitted (see iblSpecWeight).
    vec3 col = albedo * lighting * (1.0 - specE) + wetRefl + specIBL;
    float fog = clamp(length(vWorldPos - L.eye_pos.xyz) * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
    col = mix(col, L.fog_color.rgb, fog);

    // GLASS (alphaRef == -2, late blended pass) — R4 model_env_lq.ps verbatim:
    // colour = light × lerp(ENV REFLECTION, texture, texture.a), BLEND alpha =
    // the texture's own alpha × fog². Clean glass (a≈0) is nearly invisible —
    // just a faint sky sheen; dirt streaks (a≈1) show the lit texture. The R4
    // "glassiness" IS the reflection, not a milky opacity cap.
    float outA = base.a;
    // (aref -4 lit-blend lightplanes sheets are never drawn — the render queue
    // drops them; Pass_LightCones draws real volumetric beams instead.)
    if (pc.alphaRef < -1.5) {
        // GLASS — R4 base (env reflection + texture alpha) upgraded past R4:
        //  * r_glass_opacity (pom_params5.y) ceiling — mod DDS sanity clamp;
        //  * FRESNEL (Schlick): head-on the pane is at its most transparent,
        //    grazing angles turn it into a mirror (coverage AND reflection rise);
        //  * SUN GLINT: mirror flash of the sun in the pane, shadow-gated.
        float aG   = min(base.a, L.pom_params5.y);
        vec3  V    = normalize(vWorldPos - L.eye_pos.xyz);
        vec3  Rv   = reflect(V, geomN);
        float NoV  = clamp(dot(geomN, -V), 0.0, 1.0);
        float fres = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);
        float xf   = clamp(L.sky_params.x, 0.0, 1.0);
        vec3  env  = textureLod(uSky0, Rv, 1.0).rgb;
        if (xf > 0.01) env = mix(env, textureLod(uSky1, Rv, 1.0).rgb, xf);
        vec3 glint   = L.sun_color.rgb * (pow(max(dot(Rv, normalize(-L.sun_dir.xyz)), 0.0), 128.0) * 2.0 * sunMask);
        // Reflection is modulated by the LOCAL lighting (R4: light*base*2) — a pane
        // in a dark room reflects dimly; skipping this washed indoor panes WHITE.
        vec3 reflCol = env * lighting * (0.5 + fres) + glint;
        // The fresnel term only ADDS coverage when the reflection is actually
        // BRIGHT — a dark indoor reflection must not lay a dark film over the
        // pane (read as "tinted glass"). Composite dirt + reflection weights.
        float fCov = fres * clamp(dot(reflCol, vec3(0.6)), 0.0, 1.0);
        float aOut = clamp(aG + fCov * (1.0 - aG), 1e-4, 1.0);
        vec3  gcol = (albedo * lighting * aG + reflCol * fCov * (1.0 - aG)) / aOut;
        col  = mix(gcol, L.fog_color.rgb, fog);
        outA = aOut * (1.0 - fog) * (1.0 - fog);
    }
    if (SPEC_DEBUG && L.ibl_params.w > 0.5) { outColor = vec4(specIBL, 1.0); return; }   // r_ibl_debug
    if (SPEC_DEBUG) {
        vec4 ptdbg = pointDebugOverlay(vWorldPos);
        if (ptdbg.a > 0.5) { outColor = vec4(ptdbg.rgb, 1.0); return; }
    }
    // ---- RR GUIDE G-BUFFER ------------------------------------------------
    // Nw / albedo / matGloss are the values THIS pixel shaded with, which is the
    // whole point of emitting the guide from the colour pass instead of a cheap
    // re-raster: a second pass would hand RR the vertex normal and the base
    // texture, and RR would then subtract a material the frame never used.
    // ⚠GLASS (alphaRef < -1.5) is excluded. It is blended over whatever is behind
    // it, so its albedo does not describe the pixel — the guide keeps the opaque
    // surface underneath, which is the one RR has to demodulate.
    if (SPEC_RRGUIDE && pc.alphaRef >= -1.5) {
        rrWriteGuides(Nw, worldRoughness(matGloss, L.bump_params.z), albedo,
                      vec3(0.04),   // dielectric F0 — the same constant the IBL/sun spec uses
                      normalize(L.eye_pos.xyz - vWorldPos));
    }
    outColor = vec4(col, outA);
}

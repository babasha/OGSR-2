// xrRenderVulkan - shared environment terms: GTAO visibility/bent-normal, colored
// AO, hemisphere sky ambient, and rain-map visibility. Was copy-pasted into
// world_lmap / world_terrain / world_vlit. #include AFTER light_ubo.glsl AND
// shadow_common.glsl (rainVis uses cascTap).
#ifndef ENV_COMMON_GLSL
#define ENV_COMMON_GLSL

// Diffuse sky ambient (SH9 + fallbacks) lives in its own header so the stages
// that cannot include THIS one (grass / tree / skinned — they lack shadow_common,
// which rainVis needs) share the same implementation instead of copying it.
#include "sky_ambient.glsl"
#include "ao_common.glsl"
#include "common_math.glsl"   // EnvBRDFApprox / ndc2uv — pure, no bindings
#include "ao_sampled.glsl"    // gtaoVis / gtaoVisK / ssilBoost — need light_ubo's uAO/uIL/L

// gtaoVis / ssilBoost → ao_sampled.glsl (below), shared with the foliage passes
// that cannot include THIS header.

// Raw GTAO visibility (NO strength exponent) — for the specular-occlusion cone,
// which wants the geometric openness, not the artistically deepened diffuse AO.
// Returns 1.0 (fully open) when AO is off so spec occlusion becomes a no-op.
float gtaoVisRaw()
{
    if (L.ao_params.z <= 0.0) return 1.0;
    return clamp(textureLod(uAO, gl_FragCoord.xy * L.ao_params.xy, 0.0).r, 0.0, 1.0);
}

// GTAO bent normal (gba of the AO RT, world-space): the average UNOCCLUDED
// direction. Sky fill sampled along it pulls ambient from where the hemisphere
// is open. Falls back to the geometric normal when AO is off or degenerate.
//
// ⚠️ RETIRED to the geometric normal (2026-07-01, the полосы saga verdict).
// The dumped bent normal carries screen-fixed quantization stripes inherited
// from the depth-RECONSTRUCTED normal, and EVERY consumer-side gate failed in
// the field: an alignment deadband re-printed the stripes (the recon normal is
// systematically ~15-25° off geomN at grazing → the blend factor itself rode
// the stripes), and an AO.r-driven gate merely relocated them into occluded
// areas (under benches/NPCs/sheds — exactly where the gate lets bentN through).
// Any nonzero bentN use = stripes somewhere; its visual value was documented
// as subtle ("не особо вижу разницу", 2026-06-16). So: geometric normal,
// always. The gba data still ships in the AO RT; the clean re-enable path, if
// ever wanted, is encoding the bent TILT (bentN − reconN) in the GTAO pass so
// the recon normal's common-mode striping cancels, then applying that tilt to
// the receiver's geomN here.
vec3 gtaoBentN(vec3 fallbackN)
{
    return fallbackN;
}

// SSIL — one-bounce indirect-light boost for the AMBIENT term (SSFX combine_1.ps:
// `hdiffuse *= IL factor`). Multiplies the SAME hemisphere/flat ambient that AO
// darkens — NEVER the sun or dynamic lights. → ssilBoost in ao_sampled.glsl.

// coloredAO → ao_common.glsl (included above; the foliage passes share it).

// skyAmbient() / skyAmbientUp() → sky_ambient.glsl; shIrradianceC() → sky_sh.glsl.

// Rain visibility: 1 = open to the sky (gets rained on), 0 = covered (roof,
// tunnel). Bilinear-weighted compare (cascTap) on the straight-down rain map;
// outside the map = open sky (border depth 1.0 -> lit). WIDE blur softens the
// BLOCKY tree-canopy occlusion ("wetness in squares").
float rainVis(vec3 wp)
{
    vec3 n = (L.rain_vp * vec4(wp, 1.0)).xyz;   // ortho -> already NDC
    vec2 uv = n.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || n.z <= 0.0 || n.z >= 1.0)
        return 1.0;
    float ref = n.z - 0.0015;
    vec2  px  = 1.0 / vec2(textureSize(uRainMap, 0));
    float s = cascTap(uRainMap, uv, ref) * 2.0
            + cascTap(uRainMap, uv + vec2( 3.0, 0.0) * px, ref)
            + cascTap(uRainMap, uv + vec2(-3.0, 0.0) * px, ref)
            + cascTap(uRainMap, uv + vec2(0.0,  3.0) * px, ref)
            + cascTap(uRainMap, uv + vec2(0.0, -3.0) * px, ref)
            + cascTap(uRainMap, uv + vec2( 2.0,  2.0) * px, ref)
            + cascTap(uRainMap, uv + vec2(-2.0,  2.0) * px, ref)
            + cascTap(uRainMap, uv + vec2( 2.0, -2.0) * px, ref)
            + cascTap(uRainMap, uv + vec2(-2.0, -2.0) * px, ref);
    return s * (1.0 / 10.0);
}

// Sky-visibility gate for the FLAT env ambient. The flat term (L.ambient.rgb) is
// the sky-coloured fill added to every surface; ungated it LEAKS indoors so houses
// and basements read as "lit by the sky". L.ambient.w = r_ambient_sky_gate (0 = old
// ungated look, 1 = interiors get NO flat sky fill — only baked hemi + local
// lights). The sky HEMISPHERE term is already baked-hemi gated.
//
// Ambient light is HEMISPHERIC: a point under a tree crown or a pipe rack still
// catches sky light from the open sides, so gating by the NARROW straight-up
// rain query printed every overhead object's footprint on the ground as a
// pitch-black blob (the crown/pipe "чёрные пятна", 2026-07-09 — the baked
// lightmap already kills the hemisphere term there, and the narrow gate killed
// the flat term too). So the gate estimates hemispheric openness instead: the
// local query plus 3 rings (~2/4/6 m) of cheap compares with ~3.5 m height
// slack. A finite blob in an open yard recovers most of its side light; a real
// interior stays covered on every ring and gates exactly as before.
float skyAmbientGate(vec3 wp)
{
    float g = clamp(L.ambient.w, 0.0, 1.0);
    if (g < 0.001) return 1.0;
    vec3 n = (L.rain_vp * vec4(wp, 1.0)).xyz;   // ortho -> already NDC
    vec2 uv = n.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || n.z <= 0.0 || n.z >= 1.0)
        return 1.0;
    float ref  = n.z - 0.0015;
    float refW = ref - 0.01;   // ring slack: ignore occluders <~3.5 m above the receiver
    vec2  px   = 1.0 / vec2(textureSize(uRainMap, 0));
    float visC = cascTap(uRainMap, uv, ref);   // straight-up (bilinear compare)
    // 3 rings x 4 taps at ~2 / 4 / 6 m (14.6 cm texels; border depth 1.0 = open).
    const vec2 R[12] = vec2[12](
        vec2( 14.0, 0.0), vec2(-14.0, 0.0), vec2(0.0,  14.0), vec2(0.0, -14.0),
        vec2( 20.0, 20.0), vec2(-20.0, 20.0), vec2(20.0, -20.0), vec2(-20.0, -20.0),
        vec2( 41.0, 0.0), vec2(-41.0, 0.0), vec2(0.0,  41.0), vec2(0.0, -41.0));
    float w = 0.0;
    for (int i = 0; i < 12; ++i)
        w += step(refW, textureLod(uRainMap, uv + R[i] * px, 0.0).r);
    w *= 1.0 / 12.0;
    float vis = clamp(0.4 * visC + 0.6 * w, 0.0, 1.0);   // zenith cone + open sides
    return mix(1.0, vis, g);
}

// ── Sky specular IBL (vk_ibl) ────────────────────────────────────────────────
// The world is not PBR-authored, so roughness/F0 are synthesized by the caller
// (heuristic: base-rough world, smoother where wet/glass). The prefiltered sky
// cube (binding 26) supplies the roughness-blurred reflection; the split-sum
// specular term uses Karis's analytic environment-BRDF (no LUT).

// EnvBRDFApprox (Karis 2014) → common_math.glsl (included above; it is a pure
// function, so the foliage passes share the same one).

// Specular reflection of the sky. N/V world-space (V = surface→eye, unit),
// roughness in [0,1], F0 = base reflectance (0.04 dielectric). Returns 0 when
// IBL is off / no probe yet (ibl_params.x). Strength = ibl_params.y.
// The FRACTION of incoming light the same term reflects away — the diffuse must be
// scaled by (1 - this). Physically obvious and easy to skip: a surface that mirrors
// 10% of the sky does not also transmit that 10% into its albedo. We skipped it, so
// turning IBL on ADDED energy to every lit pixel, and a physically correct reflection
// ended up reading as "the whole frame got whiter" — which is exactly the bug this
// pass fixes. Cheap: EnvBRDFApprox is the same maths iblSpecular already does.
float iblSpecWeight(vec3 N, vec3 V, float roughness, vec3 F0)
{
    if (L.ibl_params.x < 0.004) return 0.0;
    float NoV = clamp(dot(N, V), 0.0, 1.0);
    vec3  ab  = EnvBRDFApprox(F0, roughness, NoV);
    return clamp(dot(ab, vec3(1.0 / 3.0)) * L.ibl_params.y * L.ibl_params.x, 0.0, 1.0);
}

vec3 iblSpecular(vec3 N, vec3 V, float roughness, vec3 F0)
{
    // ibl_params.x = enable × fade-in (0..1): a ~1 s ramp when the probe first
    // becomes ready hides the "pop" of the reflection snapping on at load.
    if (L.ibl_params.x < 0.004) return vec3(0.0);
    float NoV  = clamp(dot(N, V), 0.0, 1.0);
    vec3  R    = reflect(-V, N);
    float lod  = clamp(roughness, 0.0, 1.0) * L.ibl_params.z;
    vec3  pref = textureLod(uSkySpec, R, lod).rgb;
    return pref * EnvBRDFApprox(F0, roughness, NoV) * L.ibl_params.y * L.ibl_params.x;
}

// Direct sun GGX highlight — the world sun path is diffuse-only, so glossy/wet
// surfaces never glint the sun. Ld = direction TO the sun. Multiply the result
// by sun_color and the sun shadow (sunMask) at the call site.
vec3 sunSpec(vec3 N, vec3 V, vec3 Ld, float roughness, vec3 F0)
{
    if (L.ibl_params.x < 0.004) return vec3(0.0);
    vec3  H   = normalize(V + Ld);
    float NoH = clamp(dot(N, H), 0.0, 1.0);
    float VoH = clamp(dot(V, H), 0.0, 1.0);
    float a   = max(roughness * roughness, 0.002);
    float d   = (NoH * NoH * (a * a - 1.0) + 1.0);
    float D   = (a * a) / (3.14159265 * d * d);
    vec3  F   = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);
    return vec3(D * 0.25) * F * L.ibl_params.x;   // × sun_color × sunMask outside; ×fade
}

#endif // ENV_COMMON_GLSL

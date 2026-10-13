// xrRenderVulkan - shared wet-surface shading (rain darken + puddle sky reflection
// + sun glint). Was three drifting copies of applyWetness across world_lmap /
// world_vlit (identical) and world_terrain (divergent). Decomposed here:
//   applyWetnessCore   - the reflection math, identical in all three (takes the
//                        already-computed wetK + puddle coverage pud)
//   puddleCoverage     - lmap/vlit procedural puddle placement
//   applyWetness       - lmap/vlit entry point (down-facing kill, pud internal)
//   applyWetnessTerrain- terrain entry point (no down-facing kill; pud passed in
//                        from the caller's per-pixel sssPuddle)
// #include AFTER light_ubo.glsl, wet_common.glsl, env_common.glsl (rainVis) and
// flow_sim_sample.glsl (simWaterSoft/simFlow).
#ifndef WETNESS_GLSL
#define WETNESS_GLSL

// SF_Concavity for the real-dip placement below. Included HERE, not by the callers:
// world_lmap/world_vlit pull surface_field.glsl in AFTER this file, so relying on
// their include would leave the symbol undeclared. env_common.glsl (rainVis) comes
// before wetness.glsl in all three, which is what surface_field actually needs.
#include "surface_field.glsl"

#include "shore_wet.glsl"   // shoreWetness() — wet by CONTACT, not by rain

// lmap/vlit puddle coverage: STATIC geometry (concrete slabs, asphalt platforms,
// floors) — everything that is not splat terrain. This path had none of what the
// terrain path grew: no height fill, no real dips, no border hardness, just a soft
// noise blob times a slope mask. Soft blobs of partial coverage read as nothing once
// the shading requires a real `pud`, which is why asphalt could look permanently
// puddle-free while the terrain beside it pooled. SSFX's own static shader
// (deffer_impl_flat.ps) is NOT soft either: it drives the mask through
// smoothstep(0, 0.09, x) — border hardness 0.7 — so a pool has a defined edge.
// `wet` is rain_params.y AFTER the rainVis multiply; `upness` = clamp(N.y,0,1).
float puddleCoverage(vec3 wp, vec3 N, float wet, float upness)
{
    float slope = clamp((1.0 - max(abs(N.x), abs(N.z)) - 0.9) * 13.0, 0.0, 1.0);
    float cov   = clamp(wet * L.pom_params7.y * 1.5, 0.0, 1.0);   // grows/recedes; x1.5 = distinct, not fields
    float pud;
    if (L.pom_params7.x > 0.5) {
        pud = puddlesMaskProc(wp.xz, cov, L.pom_params7.w);
        // Real ground dips (r_puddle_geo), same signal the terrain uses: water goes
        // where the ground is actually concave, not where the noise happens to peak.
        float geo = L.puddle_geo.x;
        if (geo > 0.001)
            pud = mix(pud, max(pud * 0.25, SF_Concavity(wp)), geo);
        // SSFX G_PUDDLES_BORDER_HARDNESS 0.7 -> smoothstep(0, 0.09, pud): a pool with
        // an edge instead of a smear. Strict mode only — this is their number.
        if (L.pom_params7.x > 1.5)
            pud = smoothstep(0.0, 0.09, pud);
        pud *= slope;
    } else if (L.pom_params6.x > 0.5) {
        pud = smoothstep(0.04, 0.12, simWaterSoft(wp)) * upness;
    } else {
        pud = 0.0;
    }
    return clamp(pud, 0.0, 1.0);
}

// Shared wet reflection core: caller supplies wetK (wetness factor) and pud
// (puddle coverage, already in [0,1]); this darkens albedo (inout) and returns
// the additive puddle/glint reflection. Math is byte-identical to all three
// pre-refactor copies.
// Bent-normal specular occlusion (UE/Jimenez SIGGRAPH'16): treat AO as a
// visibility CONE around the bent normal — its half-angle WIDENS as ao→0
// (solid-angle exact: cosα = 1−ao). If the reflection ray R points OUTSIDE that
// open cone (i.e. into geometry), fade the environment reflection out. ao=1 →
// cone = full hemisphere → no occlusion. Pure math (bentN/ao supplied by caller).
// Kills wet/puddle reflections glowing out of crevices and from under overhangs
// where the open sky can't physically reach.
float specOcclusion(vec3 bentN, vec3 R, float ao)
{
    float cosCone = clamp(1.0 - ao, 0.0, 1.0);
    float d       = dot(normalize(bentN), normalize(R));
    return smoothstep(cosCone - 0.15, cosCone + 0.15, d);
}

vec3 applyWetnessCore(inout vec3 albedo, vec3 wp, vec3 N, float wetK, float pud, float sunMask, vec3 bentN, float ao)
{
    // DARKEN: FULL in deep puddles (pud^2 -> body fills AFTER the shine), ~NONE open.
    albedo *= 1.0 - L.rain_params.z * wetK * mix(0.05, 1.0, pud * pud);

    vec3  toEye    = L.eye_pos.xyz - wp;
    float dist     = length(toEye);
    // RANGE (r_wet_dist). This was 70->35 m, which is why our ground read wet under
    // the player's feet and bone dry thirty steps out — on open terrain that reads as
    // "only the bit around me got rained on". SSFX fades its wet gloss over 250->200 m
    // (rain_patch_normal.ps), i.e. the whole visible field stays wet. The ripple math
    // keeps its own tight fade (18->8 m), so the extra range costs one cube tap on
    // wet pixels, not the expensive part.
    float wetFar   = max(L.puddle_geo.y, 20.0);
    float reflFade = smoothstep(wetFar, wetFar * 0.5, dist);
    if (wetK * reflFade < 0.004) return vec3(0.0);

    float t = L.sky_params.w;
    // Ripples ONLY in puddles (still ground off-puddle).
    float ripFade = smoothstep(18.0, 8.0, dist) * clamp(L.rain_params.x * 1.5 + 0.1, 0.0, 1.0)
                  * smoothstep(0.05, 0.35, pud);
    vec2  vel   = (L.pom_params6.x > 0.5) ? simFlow(wp) : vec2(0.0);
    float velMS = length(vel) * 150.0;
    vec2  scrl  = (velMS > 0.01) ? normalize(vel) * (t * velMS * 0.25) : vec2(0.0);
    // SSFX-STRICT (r_puddle_sss 2). Our look was tuned as a WATER BODY: a dark cool
    // tint that fills at pud^2 behind a fresnel-dominated mirror. SSFX draws a GLOSS
    // PATCH instead (deffer_terrain_high_flat_d.ps / deffer_impl_flat.ps): neutral
    // tint applied LINEARLY, the normal snapped up twice as fast, reflection a flat
    // 0.4 cap that the deferred SSR mirrors, plus a global gloss lift on all wet
    // ground. The difference only shows at PARTIAL coverage — and partial is what
    // real ground mostly has: at pud 0.3 our tint enters at 0.09 and fresnel kills
    // the head-on view, so it reads "wet, no puddles", while SSFX already shows a
    // distinct pool. Strict = their constants verbatim, so the two can be A/B'd.
    bool  strict = L.pom_params7.x > 1.5;
    // Puddle = flat water mirror (flatten normal toward up).
    float flat_w = strict ? clamp(pud * pud * 2.0, 0.0, 1.0) : clamp(pud * pud, 0.0, 1.0);
    vec3  Nbase = mix(N, vec3(0.0, 1.0, 0.0), flat_w);
    vec3  Nr = Nbase;
    float crest = 0.0;
    if (ripFade > 0.01) {
        float rainAmp = 0.40 + 0.40 * clamp(L.rain_params.x, 0.0, 1.0);
        vec2 rip = rainRipples(wp.xz - scrl, t * 0.7) * (rainAmp * ripFade);
        if (velMS > 0.1)
            rip += rainRipples(wp.xz * 1.6 - scrl * 1.6, t * 0.9) * (clamp(velMS * 0.12, 0.0, 0.5) * ripFade);
        Nr = normalize(vec3(Nbase.x + rip.x, Nbase.y, Nbase.z + rip.y));
        crest = clamp(length(rip) * 2.5, 0.0, 1.0);
    }
    vec3 V = normalize(toEye);
    vec3 R = reflect(-V, Nr);
    float fres = pow(1.0 - clamp(dot(V, Nr), 0.0, 1.0), 3.0);
    float xf = clamp(L.sky_params.x, 0.0, 1.0);
    // Sharp sky in puddles (LOD0); blurred off-puddle (where reflK ~ 0 anyway).
    float reflLod = mix(5.0, 0.0, pud);
    vec3 sky = textureLod(uSky0, R, reflLod).rgb;
    if (xf > 0.01) sky = mix(sky, textureLod(uSky1, R, reflLod).rgb, xf);
    // TINT. Strict = SSFX G_PUDDLES_TINT (0.66,0.63,0.6), lerped LINEARLY by coverage
    // — a neutral darkening that shows from the first hint of a pool. Ours is a cool
    // water body that fills at pud^2 ("shine first, then the body").
    albedo = strict ? mix(albedo, albedo * vec3(0.66, 0.63, 0.60), clamp(pud, 0.0, 1.0))
                    : mix(albedo, albedo * vec3(0.34, 0.40, 0.46) * (1.0 - 0.25 * pud), pud * pud);
    // REFLECTION. Strict follows SSFX's gbuffer gloss: G = max(G, pud*0.4) plus the
    // global G_PUDDLES_TERRAIN_EXTRA_WETNESS (saturate(wet*2)*0.15) that makes ALL
    // rained-on ground faintly glossy, not just the pools. In a DEFERRED renderer
    // that gloss is a hard specular lobe; ours was scaled down TWICE on the way out
    // (a 0.35 fresnel floor AND r_wet_refl 0.6), so at coverage 0.5 the puddle showed
    // about 7% of the sky and read as a transparent film. Strict reflects at SSFX
    // scale: floor 0.55, and r_wet_refl re-centred on 1.0 so the knob still tunes
    // taste without halving the effect by default.
    // ⚠ POOL and SHEEN are DIFFERENT TERMS - merging them turned the whole ground
    // into a mirror. SSFX's 0.4 / 0.15 are GLOSS values feeding a specular BRDF: a
    // 0.15 lobe is a sheen that shows at grazing angles and around highlights. We
    // multiply by a full sky cube, where 0.15 means "15% mirror, head-on included",
    // so giving the SUM a 0.55 floor lit every wet texel on the level. Standing water
    // does mirror when you look straight down, so the POOL keeps the floor; wet
    // ground that is merely damp gets fresnel-ONLY, which is how wet asphalt actually
    // behaves - dark underfoot, shining as it turns away from you.
    float pudRefl   = min(pud, 1.0) * 0.4 * (0.55 + 0.45 * fres);
    float sheenRefl = clamp(wetK * 2.0, 0.0, 1.0) * 0.10 * fres;
    float reflScale = reflFade * clamp(L.rain_params.w * 1.6, 0.0, 2.0);
    float puddleK = strict
        ? (pudRefl + sheenRefl) * reflScale
        : wetK * pud * (0.45 + 0.55 * fres) * reflFade * clamp(L.rain_params.w, 0.0, 2.0);
    // SUN GLINT (SSFX specular_phong) - ripples shatter it to sparkles.
    vec3  Ld    = normalize(-L.sun_dir.xyz);
    vec3  Hh    = normalize(Ld + V);
    float glint = pow(max(dot(Nr, Hh), 0.0), 220.0) * pud * sunMask;
    float foam = smoothstep(3.0, 6.0, velMS) * pud * ripFade * 0.25;
    // RING WAVE crests - bright leading edge of each ripple (visible drop waves).
    vec3 crestCol = (L.sun_color.rgb + L.ambient.rgb) * (crest * pud * 0.07);
    // Bent-normal spec occlusion (r_spec_occ = L.pom_params6.w, 0 = off): applies
    // ONLY to the ENV sky reflection — sun glint / foam / ripple crests have their
    // own visibility (sun shadow), so leave them untouched.
    float specOcc = (L.pom_params6.w > 0.0) ? mix(1.0, specOcclusion(bentN, R, ao), L.pom_params6.w) : 1.0;
    // ENERGY (strict): what the WATER mirrors away is not also seen through it, so
    // the reflection replaces the bottom instead of being laid over it — that is what
    // stops a puddle reading as clean glass on dirt. Driven by the POOL term only:
    // damp ground has no body to hide, and attenuating it by the sheen was part of
    // what made the whole terrain look flooded.
    if (strict) albedo *= 1.0 - clamp(pudRefl * reflScale * specOcc, 0.0, 0.85);
    return sky * (puddleK * specOcc) + L.sun_color.rgb * (glint * 3.0) + vec3(foam) + crestCol;
}

// FOAM the surge left behind, on ground the sheet has just drained off.
//
// Kept OUTSIDE applyWetnessCore on purpose: foam is the opposite material to
// everything that function models. Soaked ground is dark and glossy; a bubble raft
// is pale and dead matte. Fed through the core it would come out as a shiny white
// smear — so it lightens the albedo (in place, before the caller shades it) and
// returns a small ambient lift of its own.
//
// Only UP-facing surfaces: foam settles, it does not cling to a wall.
//
// Nothing is ADDED to the light — the albedo is lightened in place and the
// caller's own diffuse shading lights it. Foam on a shore at dusk is grey, and a
// self-lit white patch would be the one thing that gives away that it is painted.
void applyShoreFoam(inout vec3 albedo, vec3 wp, vec3 N)
{
    float f = shoreFoam(wp) * smoothstep(0.15, 0.55, N.y);
    if (f <= 0.003) return;
    albedo = mix(albedo, vec3(0.82, 0.83, 0.80), f * 0.85);
}

// world_lmap / world_vlit: down-facing kill in wetK, pud from puddleCoverage.
vec3 applyWetness(inout vec3 albedo, vec3 wp, vec3 N, float sunMask, vec3 bentN, float ao)
{
    float wet   = L.rain_params.y;
    float shore = shoreWet(wp);
    if (wet < 0.005 && shore < 0.005) return vec3(0.0);
    wet *= rainVis(wp);
    float upness = clamp(N.y, 0.0, 1.0);
    // Kill wetness on DOWN-facing surfaces (ceilings/overhang undersides).
    float wetK = wet * mix(0.35, 1.0, upness) * smoothstep(-0.15, 0.05, N.y);
    // Water contact does NOT get the down-facing kill: the underside of a jetty
    // or a slab lying half in a river is soaked, and rain is the only reason that
    // rule existed. It also does not create puddles — a wet wall is not a pool.
    wetK = max(wetK, shore);
    // Same debug view as detail/tree, so one screenshot compares all three paths:
    // if the ground reads red and the reeds do not, the fault is in THEIR shader,
    // not in the map both of them sample.
    if (shoreWetDebug()) { albedo = vec3(shore, shore * 0.3, 0.0); return vec3(0.0); }
    // SOAKED IS DARK. The core keeps off-puddle darkening at 5% on purpose —
    // rain-damp asphalt reads by GLOSS, and darkening it fully made the whole
    // world muddy. Water contact is a different state: ground that a river has
    // been standing on is wet THROUGH, and tone is the cue people actually read.
    albedo *= 1.0 - 0.55 * shore;
    float pud  = puddleCoverage(wp, N, wet, upness);
    vec3  lit  = applyWetnessCore(albedo, wp, N, wetK, pud, sunMask, bentN, ao);
    applyShoreFoam(albedo, wp, N);
    return lit;
}

// world_terrain: pud arrives from the caller's per-pixel sssPuddle. Terrain's
// wetK has NO down-facing smoothstep and pud is gated by upness here - matches
// the pre-refactor terrain applyWetness exactly.
vec3 applyWetnessTerrain(inout vec3 albedo, vec3 wp, vec3 N, float pudIn, float sunMask, vec3 bentN, float ao)
{
    float wet   = L.rain_params.y;
    float shore = shoreWet(wp);
    if (wet < 0.005 && shore < 0.005) return vec3(0.0);
    wet *= rainVis(wp);
    float upness = clamp(N.y, 0.0, 1.0);
    float wetK = max(wet * mix(0.35, 1.0, upness), shore);
    // Same debug view as the lmap/vlit path — it was missing here, so TERRAIN
    // never painted in r_wtr_wet -1/-2 and the one surface the complaint was
    // actually about could not be read off the screenshot.
    if (shoreWetDebug()) { albedo = vec3(shore, shore * 0.3, 0.0); return vec3(0.0); }
    albedo *= 1.0 - 0.55 * shore;          // soaked is DARK — see applyWetness
    float pud  = clamp(pudIn, 0.0, 1.0) * upness;
    vec3  lit  = applyWetnessCore(albedo, wp, N, wetK, pud, sunMask, bentN, ao);
    applyShoreFoam(albedo, wp, N);
    return lit;
}

#endif // WETNESS_GLSL

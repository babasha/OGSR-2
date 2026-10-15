// world_terrain.frag main() body 1/4 — textual include INSIDE main(), not a
// standalone shader. Splat mask + mud setup + the 4 POM marches + the ZOFF
// depth export + puddle refraction + base sample/alpha test + the early debug
// views (r_ssao_debug / r_wet_debug / r_pom_debug / r_clustered_debug / r_sf_debug).
// Defines locals the later bodies read: geomN, mask, mudPress/mudCarve/mudGrime,
// mudWater, mudWet, pomShadow, pomAO, detH, pUVs[4], pomHits, pUV, base, pud.

    vec3 geomN = normalize(vNormal);
    // Splat mask (macro weights). Sample BEFORE parallax - the offset is tiny in
    // macro UV. Normalize so the 4 weights sum to 1; empty/missing -> even blend,
    // all-zero -> pure grass (never black).
    vec4 mask = texture(uMask, terrainMaskUV(vUV, vWorldPos));
    float wsum = dot(mask, vec4(1.0));
    mask = (wsum > 1e-4) ? (mask / wsum) : vec4(1.0, 0.0, 0.0, 0.0);

    // ---- MUD footprints (r_mud_deform): read the persistent press field (vk_deform,
    // stamped once per foot PLANT — boot-shaped, treaded) on SOFT splat channels and
    // sink it into the POM heightfield below. Soil softness varies per material
    // (earth deep, gravel shallow, asphalt none) and rain SOFTENS it (deeper prints
    // on wet ground). Wet asphalt still SHOWS prints (muddy boots track grime) but
    // never carves. Snow owns the prints once it covers the ground.
    float mudPress, mudCarve, mudGrime;      // signed press / print carve / asphalt grime
    float mudWater = 0.0;   // water gathered in the dent (fed into pud below)
    float mudWet   = clamp(L.rain_params.y, 0.0, 1.0);
    terrainMudSetup(mask, vWorldPos, mudPress, mudCarve, mudGrime);   // sets g_mudBias (shared w/ depth prepass)

    // Real-heightfield terrain POM in DETAIL space (the volumetric ground). Base/
    // mask stay at vUV (macro is low-freq -> not parallaxed -> stable at grazing).
    // 4 INDEPENDENT per-channel marches (SSFX) -> each material keeps full relief.
    float pomShadow, pomAO, detH;
    vec2 pUVs[4];
    vec4 pomHits;
    terrainPOM4(vDetailUV, mask, geomN, vWorldPos, pUVs, pomHits, pomShadow, pomAO, detH, true);
#ifdef ZOFF
    // SSFX depth offset (r_pom_zoff): sink the fragment depth into the POM cracks.
    // MUST equal what world_terrain_depth.frag wrote in the prepass (same inputs,
    // same math) - the LEQUAL depth test keeps the pixel only on equality.
    gl_FragDepth = max(gl_FragCoord.z, terrainZoffDepth(pc.mvp, vWorldPos, geomN, detH, gl_FragCoord.z));
#endif
    vec2 pUV = vUV;

    // PUDDLE COVERAGE (0..1), computed ONCE for the whole shader: the refraction
    // bend below and the wet shading in the albedo body used to evaluate
    // sssPuddle separately with bit-identical inputs — and each evaluation is 5
    // noise octaves plus, with r_puddle_geo, an SF_Concavity (~9 rain-map
    // gathers). SSS procedural placement default; flow sim is parked.
    float pud = (L.pom_params7.x > 0.5) ? sssPuddle(geomN, vWorldPos, detH)
              : (L.pom_params6.x > 0.5) ? smoothstep(0.04, 0.12, simWaterSoft(vWorldPos))
              : 0.0;

    // PUDDLE REFRACTION (SSFX N_refra): where a puddle covers this pixel, bend the
    // bottom (base+detail) UV by the water-surface ripple. Gate tight (near + raining).
    if (L.pom_params7.x > 0.5 && L.rain_params.y > 0.01 && L.rain_params.z >= 0.0) {
        float rf = smoothstep(18.0, 8.0, distance(L.eye_pos.xyz, vWorldPos)) * smoothstep(0.2, 0.6, pud);
        if (rf > 0.01) {
            vec2 ro = rainRipples(vWorldPos.xz, L.sky_params.w * 0.7) * (0.006 * rf);
            pUV += ro;
            vec2 rod = ro * pc.detailScale;
            pUVs[0] += rod; pUVs[1] += rod; pUVs[2] += rod; pUVs[3] += rod;
        }
    }

    vec4 base = texture(uBase, pUV, L.spot_flash.z);   // DLSS mip bias (0 native)
    if (pc.alphaRef >= 0.0 && base.a < pc.alphaRef) discard;

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
    // r_wet_debug 1: rain-map visibility.
    if (SPEC_DEBUG && L.rain_params.z < 0.0) {
        outColor = vec4(vec3(rainVis(vWorldPos)), base.a);
        return;
    }
    // r_pom_debug 1: POM occlusion mask (contact AO x self-shadow);
    // 2: SELF-SHADOW only (horizon-map vs march A/B without the AO noise).
    if (SPEC_DEBUG && L.pom_params3.x > 0.5) {
        outColor = vec4(vec3(L.pom_params3.x > 1.5 ? pomShadow : pomAO * pomShadow), base.a);
        return;
    }
    // r_clustered_debug: per-cluster dynamic-light-count heatmap.
    if (SPEC_DEBUG && L.cluster_params.w > 1.5) {
        int cnt = clusterLightCount(vWorldPos, L.eye_pos.xyz, L.cam_dir.xyz, gl_FragCoord.xy, L.ao_params.xy, L.cluster_params, L.cluster_params2);
        outColor = vec4(clusterHeat(cnt), base.a);
        return;
    }

    // r_sf_debug: Surface Field viz (1 height, 2 slope, 3 curvature, 4 sky, 5 canopy).
    int sfdbg = SPEC_DEBUG ? int(L.sf_params.y + 0.5) : 0;   // SPEC_DEBUG off -> all sf-debug branches DCE
    if (sfdbg > 0) {
        vec3 nn = normalize(vNormal);
        outColor = vec4((sfdbg == 5) ? SC_DebugColor(SC_Refine(SC_GROUND, nn))   // terrain = ground
                                      : SF_DebugColor(vWorldPos, nn, sfdbg), base.a);
        return;
    }

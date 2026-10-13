// world_terrain.frag main() body 2/4 — textual include INSIDE main(). Detail
// splat + height blend -> albedo; lightmap; detail normal mapping; micro
// contact-AO; mud print shading; puddle coverage; puddle/terrain debug views.
// Reads: mask, pUVs, pomHits, detH, geomN, mud*, base, pud. Defines for the
// later bodies: bw, albedo, lm, hemiOcc, Nw, cav, glossT, microAO, microH;
// folds mudWater into pud.

    vec4 dR = texture(uDtR, pUVs[0], L.spot_flash.z);   // DLSS mip bias (0 native)
    vec4 dG = texture(uDtG, pUVs[1], L.spot_flash.z);   // each channel at its OWN parallaxed uv
    vec4 dB = texture(uDtB, pUVs[2], L.spot_flash.z);
    vec4 dA = texture(uDtA, pUVs[3], L.spot_flash.z);

    // HEIGHT-BASED detail blend (Mishkinis "advanced terrain texture splatting"):
    // bias each splat weight by that channel's REAL detail height, so the RAISED
    // material wins per-texel -> sharp interlocking transitions (gravel poking
    // through grass) instead of a soft linear cross-fade. Channels the mask doesn't
    // carry are gated out; degenerate -> falls back to the plain mask. `bw` then
    // drives BOTH the diffuse and the detail normal so the relief follows the same
    // boundaries. (Missing height maps = flat 0.5 -> gracefully ~linear.)
    vec4 chH = pomHits;   // per-channel hit heights straight from the 4 marches
    vec4 bw = mask;
    if (g_cacheOn) {
        bw = g_cacheBW;   // composite cache: weights baked (same r_terra_blend rule) at bake time
    } else if (L.zoff_params.y > 0.005) {
        // r_terra_blend (zoff_params.y): 0 = plain mask cross-fade — the GAMMA/
        // SSFX look (their HeightBlending() ships commented out, asphalt fades
        // smoothly into soil); >0 = Mishkinis height blend with this transition
        // width (0.25 = the old sharp interlocking edges).
        float blendDepth = L.zoff_params.y;
        vec4  wh   = mask + chH;
        float maxw = max(max(wh.x, wh.y), max(wh.z, wh.w));
        vec4  b    = max(wh - (maxw - blendDepth), 0.0) * step(vec4(1e-4), mask);
        float bsum = dot(b, vec4(1.0));
        if (bsum > 1e-5) bw = b / bsum;
    }
    vec3 detail = dR.rgb * bw.r + dG.rgb * bw.g + dB.rgb * bw.b + dA.rgb * bw.a;
    // detH (REAL detail height at the parallax hit) comes from terrainPOM.

    vec3 albedo = 2.0 * base.rgb * detail;

    // Lightmap (hemi/AO) + DYNAMIC R4-style sun (per-pixel N.L x shadow map).
    vec4  lm      = texture(uLmap, vLmapUV);
    float hemiOcc = dot(lm.rgb, vec3(1.0 / 3.0));
    vec3  Nw      = geomN;                 // base normal; detailNormal perturbs it near so sun/dyn catch the relief
    // Detail normal mapping: the primary ground-relief source. Faded to 0 by
    // distance so far terrain skips the 4 taps + AO + gloss entirely.
    // RANGE (r_terrain_detail_dist). This was a hard 45->25 m fade, which is why our
    // asphalt goes flat matte paint a few dozen metres out: past it the 4 bump taps
    // are skipped, so the ground loses its normal map, its cavity AO AND its gloss
    // (glossT stays 0 -> no dry specular at all). SSFX samples the bump at ANY
    // distance — only the POM MARCH attenuates (their dist_att, 20 m) — so their
    // road keeps its grain and its glint to the horizon.
    float dnFar  = max(L.puddle_geo.z, 30.0);
    float dnStr  = L.pom_params4.y * smoothstep(dnFar, dnFar * 0.55, distance(L.eye_pos.xyz, vWorldPos));
    float cav    = 0.0;
    float glossT = 0.0;
    if (dnStr > 0.0)
        Nw = detailNormal(geomN, pUVs, bw, dnStr, cav, glossT);

    // Micro contact AO: darken grooves. Occlusion = max of the normal cavity
    // (1-n.z) and the detail height pit (1-h^2). Applied to albedo (R4 base*=1-AO).
    float aoStr   = L.pom_params4.z;
    float occlT   = max(cav, 1.0 - detH * detH);
    float microAO = 1.0 - aoStr * clamp(occlT, 0.0, 1.0);
    albedo *= microAO;

    // Mud print shading. The soil is PRESSED IN, not painted over: the print reads
    // through geometry (POM carve + tessellation), the gradient-normal dimple and
    // micro-AO; albedo only gets a subtle packed-moisture darkening (more when wet).
    // The berm is the same soil pushed up — normals/geometry carry it, no tint.
    // Wet ASPHALT is the exception: tracked-on grime IS a dark film on top.
    if ((mudCarve > 0.0 || mudGrime > 0.0) && abs(mudPress) > 0.004) {
        float ew  = L.deform_tex.w;              // world metres per deform texel
        float pXp = SnowDeformPress(vWorldPos + vec3(ew, 0.0, 0.0));
        float pXm = SnowDeformPress(vWorldPos - vec3(ew, 0.0, 0.0));
        float pZp = SnowDeformPress(vWorldPos + vec3(0.0, 0.0, ew));
        float pZm = SnowDeformPress(vWorldPos - vec3(0.0, 0.0, ew));
        vec2 slope = vec2(pXp - pXm, pZp - pZm) * (0.05 * min(mudCarve, 1.2) / (2.0 * ew));
        Nw = normalize(Nw + vec3(slope.x, 0.0, slope.y));
        float dent  = clamp(mudPress, 0.0, 1.0);
        float packD = dent * min(mudCarve, 1.2) * (0.18 + 0.42 * mudWet);   // packed moisture only
        albedo *= 1.0 - min(packD, 0.55);
        glossT  = max(glossT, dent * min(mudCarve, 1.2) * (0.15 + 0.55 * mudWet));   // wet compaction sheen
        albedo  = mix(albedo, albedo * vec3(0.55, 0.52, 0.50), min(dent * mudGrime, 0.6));   // asphalt grime
        // GRADUAL water fill: the field decays 1 -> 0, so the local MAX press is an
        // AGE proxy — a fresh print (max≈1) is dry, water seeps in as it ages and
        // drains away before the print itself fades. Deepest parts fill first.
        float maxP = max(max(max(pXp, pXm), max(pZp, pZm)), mudPress);
        float age  = (1.0 - smoothstep(0.60, 0.88, maxP)) * smoothstep(0.10, 0.28, maxP);
        mudWater   = age * smoothstep(0.35, 0.75, mudPress / max(maxP, 1e-3))
                   * (0.2 + 0.8 * mudWet) * min(mudCarve, 1.0);
    }

    // Puddle coverage `pud` comes from the parallax body (computed once, shared
    // with the refraction bend there).
    float microH = min(detH, 1.0 - cav);
    // Water gathered in footprint dents (computed above with the print's AGE — it
    // seeps in gradually, not on the first frame): full wet shading, reflections +
    // ripples. applyWetnessTerrain gates by global wetness, so dry weather keeps
    // prints as damp mud only.
    pud = max(pud, mudWater);

    // r_puddle_debug: 1 = coverage; 2 = per-pixel detail micro-height.
    int pdbg = SPEC_DEBUG ? int(L.pom_params5.w + 0.5) : 0;   // SPEC_DEBUG off -> puddle-debug branches DCE
    if (pdbg > 0) {
        outColor = (L.pom_params6.x > 0.5) ? vec4(waterDebugColor(vWorldPos, pdbg), base.a)
                                           : vec4(vec3(pdbg >= 2 ? microH : pud), base.a);
        return;
    }

    // r_terrain_debug: 1 world normal, 2 micro-AO, 3 detail height, 4 mud field.
    int tdbg = SPEC_DEBUG ? int(L.pom_params4.w + 0.5) : 0;   // SPEC_DEBUG off -> all terrain-debug branches DCE
    if (tdbg == 1) { outColor = vec4(Nw * 0.5 + 0.5, base.a); return; }
    if (tdbg == 2) { outColor = vec4(vec3(microAO),  base.a); return; }
    if (tdbg == 3) { outColor = vec4(vec3(detH),     base.a); return; }
    // 8/9 = SUN-BEAM GROUND DEPOSIT probe (r_sun_beam_ground): 8 = the raw froxel
    // in-scatter THIS pixel samples from uVolume (×8, greyscale). BLACK under a visible
    // shaft ⇒ the volume read is broken (binding 27 / layout / froxel-w), NOT a threshold
    // issue. If it shows the shaft ⇒ lower r_sun_beam_ground_thr. 9 = the deposit gate in
    // RED over the raw in-scatter in GREEN (red where the shaft is depositing).
    if (tdbg == 8 || tdbg == 9) {
        float beam = shaftOverhead(vWorldPos);   // shaft in-scatter found overhead (toward the sun)
        if (tdbg == 8) { outColor = vec4(vec3(beam * 8.0), base.a); return; }
        float gt = smoothstep(L.beam2.w, L.beam2.w * 2.0, beam);
        outColor = vec4(gt, beam * 4.0, 0.0, base.a);
        return;
    }
    // 5..7 = SUN-BEAM RECOVERY probe (r_sun_beam): 5 = crisp atlas directLit at THIS
    // world pos (what the shaft samples), 6 = the smeared screen mask surfaces use,
    // 7 = the recovered gap painted RED (directLit lit where the mask is dark = where
    // the ground re-lights under a beam). All-black in 7 under a visible shaft ⇒ the
    // atlas doesn't resolve the gap on the ground at this page resolution.
    if (tdbg >= 5 && L.shadow_params.w > 0.5) {
        float m  = vsmSunShadow(gl_FragCoord.xy * L.ao_params.xy);
        float dc = distance(L.eye_pos.xyz, vWorldPos);
        float dl = (dc < L.beam_params.y) ? vsmSunShadowGrassDirect(vWorldPos, L.beam_params.w) : m;
        if (tdbg == 5) { outColor = vec4(vec3(dl), base.a); return; }
        if (tdbg == 6) { outColor = vec4(vec3(m),  base.a); return; }
        float gap = clamp(dl - m, 0.0, 1.0);
        outColor = vec4(mix(vec3(m * 0.5), vec3(1.0, 0.0, 0.0), gap), base.a);
        return;
    }
    if (tdbg == 4) {   // deform-field overlay at TRUE world position (bypasses POM —
                       // if this sits under the boot but the visible dent doesn't,
                       // the shift is the POM parallax, not the stamp position)
        float p = SnowDeformOn() ? SnowDeformPress(vWorldPos) : 0.0;
        vec3  c = mix(vec3(0.35), (p >= 0.0) ? vec3(1.0, 0.1, 0.1) : vec3(0.1, 0.3, 1.0),
                      clamp(abs(p) * 1.5, 0.0, 1.0));
        outColor = vec4(c, base.a);
        return;
    }

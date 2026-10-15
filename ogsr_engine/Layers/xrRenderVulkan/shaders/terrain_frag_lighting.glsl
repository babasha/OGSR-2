// world_terrain.frag main() body 3/4 — textual include INSIDE main(). Snow
// cover; sun N.L x shadow + beam-gap recovery + the parked beam ground deposit;
// hemisphere/DDGI ambient; the lighting sum; r_shade_debug views.
// Reads: geomN, Nw, albedo, hemiOcc, pomShadow, pomAO, detH. Defines for the
// final body: snow, rawNL, sunMask, beamGround, occ, hemiOccL, ambFill, flatK, lighting.

    // SNOW (Surface Field consumer, r_snow): whiten by surface type x slope x sky
    // exposure - flat up-facing ground accumulates, steep/under-cover none.
    // The WHOLE block sits under SPEC_SNOW: the no-snow variant used to keep the
    // covThr noise (3 vNoise octaves), the albedo/Nw mixes and the SnowFootprint
    // machinery live for a value that was constant 0. The runtime sf_params.w
    // gate stays inside because SF_SkyExposure() is ~9 rainVis gathers and a
    // winter pipeline still renders frames before the first flake settles.
    float snow = 0.0;
    if (SPEC_SNOW) {
        float snowBase = 0.0;
        if (L.sf_params.w > 0.0)
            snowBase = SC_SnowAmount(SC_Refine(SC_GROUND, geomN), geomN, SF_SkyExposure(vWorldPos));
        // Organic growth: vary the coverage ONSET per world-location (multi-scale noise) so
        // partial snow fills in irregular PATCHES, not per-terrain-triangle. At full coverage
        // (sf_params.w -> 1) every threshold is met -> uniform snow.
        float covThr = (vNoise(vWorldPos.xz * 0.13) * 0.55 + vNoise(vWorldPos.xz * 0.43) * 0.30
                      + vNoise(vWorldPos.xz * 1.30) * 0.15) * 0.75;
        snow = snowBase * smoothstep(covThr, covThr + 0.22, clamp(L.sf_params.w, 0.0, 1.0));
        albedo = mix(albedo, vec3(0.90, 0.93, 0.97), snow);
        Nw = normalize(mix(Nw, geomN, snow));   // snow smooths the microrelief -> less "texturey"
        if (L.deform_tex.x < 1.5)               // MESH mode: the snow mesh draws the dents on top
            SnowFootprint(albedo, Nw, vWorldPos, snow);   // pressed-snow prints at recent foot contacts
    }

    float rawNL   = max(dot(Nw, normalize(-L.sun_dir.xyz)), 0.0);   // unshadowed sun N.L
    float sunMask = rawNL;
    if (sunMask > 0.005) {
        float sunSh = (L.shadow_params.w > 0.5) ? vsmSunShadow(gl_FragCoord.xy * L.ao_params.xy)
                                                : sunShadow(vWorldPos);
        // BEAM-GAP RECOVERY (r_sun_beam, VSM only): the screen mask above is temporally
        // smeared (vsm_resolve EMA), so a thin sun gap through a crown — which the
        // volumetric shaft shows crisply — gets closed here and the ground stays dark
        // under a visible beam. Re-sample the crisp atlas at THIS world pos (same source
        // the shaft uses) and take the max, re-opening the gap so the ground lights up in
        // agreement with the shaft. Skip already-lit ground (no gap to recover) and far
        // ground (non-resident pages -> the direct sample defaults to lit). beamKick adds
        // a small extra sun in the recovered gap so the landing reads as a bright splash.
        float beamKick = 1.0;
        if (L.shadow_params.w > 0.5 && L.beam_params.x > 0.0 && sunSh < 0.98) {
            float d    = distance(L.eye_pos.xyz, vWorldPos);
            float fade = (1.0 - smoothstep(L.beam_params.y * 0.7, L.beam_params.y, d)) * L.beam_params.x;
            if (fade > 0.001) {
                float directLit = vsmSunShadowGrassDirect(vWorldPos, L.beam_params.w);
                float gap = clamp(directLit - sunSh, 0.0, 1.0);   // the gap the temporal mask swallowed
                sunSh    = mix(sunSh, max(sunSh, directLit), fade);
                beamKick = 1.0 + gap * fade * (L.beam_params.z - 1.0);
            }
        }
        sunMask *= sunSh * pomShadow * beamKick;
    }

    // SUN-BEAM GROUND DEPOSIT (r_sun_beam_ground, L.beam2). The visible god-ray is lit
    // air your view ray gathered through the crown gap → the integrated in-scatter at
    // THIS pixel is bright, even though the sun is geometrically occluded on the ground
    // here (proven r_terrain_debug 7: no atlas gap). So don't fake a glow — deposit the
    // sun the SHADOW removed, gated by the beam and shaped by the surface's own N.L, so
    // the terrain lights exactly like the sunlit ground beside it, just switched on by
    // the shaft. Folded into the sun bucket below → gets ×albedo = real sun on the
    // ground, not haze. Aesthetic (no direct light physically reaches this point).
    float beamGround = 0.0;   // PARKED: L.beam2.z (r_sun_beam_ground) defaults 0 → block skipped
    if (L.beam2.z > 0.0 && rawNL > 0.005) {
        float beam = shaftOverhead(vWorldPos);   // brightest shaft in-scatter above this point
        float gate = smoothstep(L.beam2.w, L.beam2.w * 2.0, beam);
        beamGround = clamp(gate * L.beam2.z, 0.0, 1.0) * max(rawNL - sunMask, 0.0);
    }

    // Hemisphere sky fill (R4 hmodel). sun_color/ambient arrive final from vk_env_light.
    vec3  occ      = coloredAO(gtaoVis(), albedo) * pomAO * ssilBoost();   // GTAO x POM AO x SSIL bounce (ambient only)
    float hemiOccL = hemiOcc;
    if (SPEC_DEBUG && L.pom_params3.y > 0.5) { occ = vec3(1.0); hemiOccL = 1.0; }   // r_ao_flat debug
    // Ambient takes the DETAIL normal (Nw), not the flat geometric one. It used to
    // take geomN for a concrete reason — skyAmbient was a point sample of a sharp
    // single-mip cube, so perturbing its direction per pixel turned the sky fill
    // into a mirror. SH9 irradiance is smooth by construction, so that constraint
    // is gone, and terrain relief finally modulates the ambient instead of only
    // the sun. This is what makes slopes read as shaped at dusk, when the sun is
    // below the horizon and ambient is nearly the only light there is.
    //
    // The DDGI probe field REPLACES this term when r_ddgi_light is up (see
    // ddgi_receiver.glsl). Terrain is where the swap is most visible: it is the
    // large, mostly flat, mostly up-facing surface that a coloured wall or a rusted
    // hull actually bounces onto, and the analytic fill has no way to know that.
    vec3 skyFill = skyAmbient(gtaoBentN(Nw)) * (hemiOccL * L.sky_params.y);
    vec3 ambFill = SPEC_DDGI ? ddgiAmbient(skyFill, vWorldPos, Nw,
                                           normalize(L.eye_pos.xyz - vWorldPos), hemiOccL)
                             : skyFill;
    // ⚠ Same rule as world_frag_body: the flat weather fill is the SAME sky the
    // probe field measures, so it fades out in proportion to how much of the
    // ambient the field is answering (r_gi_flat -> ddgi_params.w). Terrain is the
    // surface where this matters most — it is the biggest continuous area in the
    // frame and the one the fill flattens hardest.
    // ⚠ Scaled by ddgiVolumeFade for the reason spelled out in world_frag_body:
    // the field answers PER PIXEL and stops at the volume edge, so a frame-uniform
    // displacement strips the fill off everything beyond it. Terrain is where that
    // showed worst - it is the surface that reaches furthest past the cascades.
    const float flatK = SPEC_DDGI ? (1.0 - L.ddgi_params.w * ddgiVolumeFade(vWorldPos)) : 1.0;
    // AO on the ambient: full on the analytic sky, r_ao_gi of it on the traced share
    // (ddgiAmbientAO, same rule as world_frag_body).
    const vec3 occAmb = SPEC_DDGI ? ddgiAmbientAO(occ, vWorldPos) : occ;
    vec3 lighting = ambFill * occAmb
                  + L.sun_color.rgb  * (sunMask + beamGround)   // + sun-beam ground deposit
                  + L.ambient.rgb * occ * skyAmbientGate(vWorldPos) * flatK   // flat sky fill, faded by the field
                  + dynLightsTerrain(vWorldPos, Nw);

    // r_shade_debug (zoff_params.z) — same view numbers as world_lmap.frag.
    int ldbg = SPEC_DEBUG ? int(L.zoff_params.z + 0.5) : 0;   // SPEC_DEBUG off -> shade-debug branches DCE
    if (ldbg > 0) {
        vec3 dbg =
            (ldbg == 1) ? albedo :
            (ldbg == 2) ? lm.rgb :
            (ldbg == 3) ? vec3(hemiOccL) :
            (ldbg == 4) ? vec3(gtaoVis()) :
            (ldbg == 5) ? vec3(skyAmbientGate(vWorldPos)) :
            (ldbg == 6) ? skyAmbient(gtaoBentN(geomN)) :
            (ldbg == 7) ? lighting :
            (ldbg == 8) ? L.ambient.rgb * occ * skyAmbientGate(vWorldPos) * flatK :
            (ldbg == 9) ? L.sun_color.rgb * sunMask :
            (ldbg == 11) ? ambFill :          // the ambient AS SHIPPED (probe field when r_ddgi_light > 0)
                          vec3(clamp(L.rain_params.y, 0.0, 1.0) * rainVis(vWorldPos));
        outColor = vec4(dbg, base.a);
        return;
    }

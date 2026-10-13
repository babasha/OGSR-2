// world_terrain.frag main() body 4/4 — textual include INSIDE main(). Dry sun
// gloss; rain wetness; sky specular IBL; distance fog; IBL/point debug
// overlays; DLSS-RR guide writes; the final outColor.
// Reads: albedo, lighting, Nw, glossT, sunMask, snow, pud, base, microH.

    // Dry sun gloss (R4 gloss in bump .r): material-aware Blinn specular -
    // asphalt/gravel glint, grass matte. Fades out as the ground wets.
    vec3  drySpec  = vec3(0.0);
    float glossStr = L.pom_params5.x;
    if (glossStr > 0.0 && glossT > 0.0 && sunMask > 0.005) {
        vec3  Ld   = normalize(-L.sun_dir.xyz);
        vec3  V    = normalize(L.eye_pos.xyz - vWorldPos);
        vec3  H    = normalize(Ld + V);
        float g    = clamp(glossT, 0.0, 1.0);
        float shin = mix(16.0, 160.0, g);             // glossier -> tighter highlight
        float s    = pow(max(dot(Nw, H), 0.0), shin) * g;
        float dry  = (1.0 - clamp(L.rain_params.y, 0.0, 1.0)) * (1.0 - snow);   // snow also kills the dry gloss
        drySpec    = L.sun_color.rgb * (s * glossStr * sunMask * dry);
    }

    // Rain wetness (terrain: pud passed in; asphalt reflectivity term was unused).
    vec3 wetRefl = vec3(0.0);
    if (SPEC_WET)
        wetRefl = applyWetnessTerrain(albedo, vWorldPos, Nw, pud, sunMask, gtaoBentN(geomN), gtaoVisRaw());

    // Sky specular IBL (r_ibl): grazing sky reflection on the ground. SKY ONLY —
    // terrain's own drySpec already owns the sun highlight (no double glint).
    // Ground is MATTE when dry (dirt/grass ≈ no gloss); a constant reflection on
    // near-flat ground reads as "lacquer/varnish" at grazing angles. So keep it very
    // rough + scale the strength by WETNESS — the sky sheen appears only as the ground
    // gets wet (physically right), and dry terrain gets just a faint soft sky tint.
    // Wet gloss gated by rainVis (ground under sheds/canopy stays matte) and toned
    // down so wet ground reads as a soft sheen, not a mirror.
    // r_ibl master gate hoisted up: iblSpecular self-early-outs at ibl_params.x, but
    // Vv/wetF's rainVis gathers/roughI ran regardless. Skip when IBL is off (default)
    // — output identical (iblSpecular returned 0 below the same threshold).
    vec3  specIBL = vec3(0.0);
    float specE   = 0.0;   // reflected fraction — taken OFF the diffuse below
    if (SPEC_IBL && L.ibl_params.x > 0.004) {
        vec3  Vv      = normalize(L.eye_pos.xyz - vWorldPos);
        // DRY BASE ONLY. The wet half used to live here too (roughness 0.9 -> 0.5,
        // strength scaled by wetness), on top of the wet reflection applyWetness
        // already produces — the same pixels brightened twice every time it rained.
        // One owner per effect: wet ground is applyWetness's (r_wet_refl), this is
        // the faint dry sky tint that keeps matte dirt from looking cut out.
        const float roughI = 0.9;
        vec3  Rv      = reflect(-Vv, Nw);
        float vis     = specOcclusion(gtaoBentN(geomN), Rv, gtaoVisRaw()) * 0.08;
        specIBL       = iblSpecular(Nw, Vv, roughI, vec3(0.04)) * vis;
        specE         = iblSpecWeight(Nw, Vv, roughI, vec3(0.04)) * vis;
    }

    // Distance fog (R4).
    // Energy: the mirrored fraction is not also transmitted (see iblSpecWeight).
    vec3 col = albedo * lighting * (1.0 - specE) + drySpec + wetRefl + specIBL;
    float fog = clamp(length(vWorldPos - L.eye_pos.xyz) * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
    col = mix(col, L.fog_color.rgb, fog);

    if (SPEC_DEBUG && L.ibl_params.w > 0.5) { outColor = vec4(specIBL, base.a); return; }   // r_ibl_debug
    if (SPEC_DEBUG) {
        vec4 ptdbg = pointDebugOverlay(vWorldPos);
        if (ptdbg.a > 0.5) { outColor = vec4(ptdbg.rgb, base.a); return; }
    }

    // ---- RR GUIDE G-BUFFER ------------------------------------------------
    // Terrain is most of an outdoor frame, so this is the single most valuable
    // guide writer in the renderer. Nw here is the full shading normal (detail
    // bump + mud dents + snow smoothing) and albedo has micro-AO / grime / snow
    // already folded in — exactly what the lighting was multiplied by.
    // ⚠ROUGHNESS: terrain carries no gloss map. 0.9 is the constant the IBL term
    // above uses for it, and glossT is the only per-pixel sheen this shader knows
    // (detail-bump gloss + wet compaction), so the guide tracks that and nothing
    // else — inventing a second roughness here would put the guide at odds with
    // the specular the frame actually drew.
    if (SPEC_RRGUIDE) {
        float rrRough = clamp(0.9 - 0.55 * clamp(glossT, 0.0, 1.0), 0.2, 0.95);
        rrWriteGuides(Nw, rrRough, albedo, vec3(0.04),
                      normalize(L.eye_pos.xyz - vWorldPos));
    }
    outColor = vec4(col, base.a);

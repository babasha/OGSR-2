// Component of world_terrain.frag.glsl — textual include, not a standalone shader.
// Needs terrain_pom.glsl (POM_PLANE) + wet_common/surface_field included first.

// SSFX-faithful puddle placement (deffer_terrain_high_flat_d.ps "Puddles slope
// mask and height"): the water RISES UP THE POM HEIGHTFIELD — `puddle = water
// level - texel height` — so rain floods the cracks and potholes FIRST and only
// spills over the raised plates in a downpour. detH = the mask-blended parallax
// hit height (incl. the per-channel CH_OFF sinks: asphalt/mud sit low -> trap
// water, exactly like SSFX ssfx_terrain_offset). The procedural macro mask
// keeps large-scale variation (SSFX modulates by s_puddles_mask the same way)
// so the whole yard doesn't flood uniformly.
float sssPuddle(vec3 N, vec3 wp, float detH)
{
    float wet = clamp(L.rain_params.y, 0.0, 1.0);
    if (wet <= 0.0) return 0.0;
    float slope = clamp((1.0 - max(abs(N.x), abs(N.z)) - 0.9) * 13.0, 0.0, 1.0);
    if (slope <= 0.0) return 0.0;
    // SSFX: height_str = saturate(wetness * PLANE * limit - PixelHeight); x4 ramp.
    // r_puddle_level 0.5 default maps to the SSFX water limit 1.0.
    float level   = wet * POM_PLANE * clamp(L.pom_params7.y * 2.0, 0.0, 2.0);
    float puddles = clamp((level - detH) * 4.0, 0.0, 1.0);
    if (puddles <= 0.0) return 0.0;
    // Macro body variation: soft (0.35 floor), not the old binary placement —
    // the heightfield decides WHERE inside a body the water actually sits.
    float macro = 0.35 + 0.65 * puddlesMaskProc(wp.xz, clamp(wet * 1.4, 0.0, 1.0), L.pom_params7.w);
    // REAL DIPS (r_puddle_geo). Everything above is texture-scale: the water level is
    // compared against detail micro-height (centimetres, tiling every metre or two),
    // and WHERE the pools go is decided by noise. SSFX gets away with that because a
    // per-level artist mask paints the hollows in; without one, a metre-deep dip in
    // the asphalt stays dry while the noise puts water on the crown of the road.
    // SF_Concavity is the top-down curvature of the actual ground (sky-exposure
    // gated, so nothing pools under a roof), which is the signal that was missing.
    // The noise keeps a floor so a genuinely flat yard still breaks up rather than
    // sheeting over uniformly.
    float geo = L.puddle_geo.x;
    if (geo > 0.001)
        macro = mix(macro, max(macro * 0.25, SF_Concavity(wp)), geo);
    return puddles * macro * slope;
}

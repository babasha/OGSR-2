#ifndef GI_SURFACE_GLSL
#define GI_SURFACE_GLSL
// ===========================================================================
//  xrRenderVulkan — THE RADIANCE LEAVING A SURFACE PATCH. One copy, two callers.
// ===========================================================================
//  Split out of ddgi_trace.comp.glsl when the world radiance cache (wrc_shade)
//  needed to compute the SAME quantity for a cell that a probe ray computes for
//  its hit. That is not a convenience: the cache exists to REPLACE that shading
//  for most rays, so the two have to be the same number to the last bit. If they
//  drifted, switching the cache on would move the frame's brightness and every
//  A/B after it would be measuring the drift instead of the cache.
//
//  Same rule, same reason as ddgi_sky.glsl (which the gather and the probes share
//  so a MISS cannot be worth different amounts to each) — see the header there.
//
//  THREE TERMS, AND EVERY ONE OF THEM IS A LOOKUP OR A SINGLE RAY:
//    * sun, through ONE any-hit shadow ray, plus the SH9 sky along the normal —
//      the same sky the raster hands that surface;
//    * the dynamic lights of the RT world, each with its own shadow ray;
//    * the light that ALREADY arrived here, read out of the probe field as it
//      stood last frame. This is idTech 8's "previous frame irradiance volumes
//      for multiple bounces", and it is what separates one bounce then darkness
//      from an interior that actually glows.
//
//  Requires, in this order, before the #include:
//    light_atten.glsl + RT_LIGHTS + RT_B_LIGHTS, rt_shade.glsl,
//    ddgi_common.glsl with DDGI_SAMPLE, ddgi_sky.glsl.
//
//  `V` is the direction the radiance leaves towards (the view vector of whoever
//  is asking). It only steers the probe lookup's view bias; a cache cell, which
//  has no single viewer, passes its own normal.
// ===========================================================================
// ⚠⚠⚠ `skyIrr` IS THE TERM THIS RENDERER'S GI NEVER HAD. The sun gets a shadow
// ray; the sky gets NOTHING — rtShadeHitSky adds the full SH9 sky irradiance to
// every hit, so a wall inside a shack is lit as if it stood in an open field.
// Both the probe field and the cache inherit that, and the only thing holding
// interiors down was the BAKED LIGHTMAP re-applied as a multiplier
// (r_ddgi_light_bake, default 1). Turn that off and interiors go white — which is
// exactly what happened the first time anybody tried it, and it is a missing
// occlusion term, not a bad multiplier.
//
// A visibility ray per HIT is unaffordable (the gather alone makes ~800k a frame).
// A visibility ray per CACHE CELL is not: a cell is shaded ~40x less often than it
// is read. So the caller that can pay measures the term and passes it here, and the
// one that cannot passes ddgiSkyIrradiance(h.nrm) and behaves exactly as before.
//
// ⚠⚠⚠ IT IS A COLOUR, NOT A SCALAR, AND THAT IS THE WHOLE POINT. The first version
// took a visibility FRACTION and applied it here as `ddgiSkyIrradiance(h.nrm) *
// skyVis` — the hemisphere-average sky, dimmed. That cannot represent a WINDOW. A
// room lit through one small opening takes its light from a few square degrees of
// sky in ONE direction, and a scalar throws the direction away: a bright patch of
// sky in a window and a dim zenith collapse to the same number. Measured on Dark
// Valley: that room came out BLACK however the fraction was tuned, and the same
// room with skyVis = 1 came out lit like an open field. Two wrong answers with
// nothing in between, which is what a missing integral looks like from outside.
//
// The rays that measured the fraction were already being traced. Summing the sky's
// RADIANCE along the ones that escape, instead of counting them, costs one texture
// tap per miss and makes the term the integral it always claimed to be. Identical
// to the old behaviour for a UNIFORM sky, by construction: cosine-weighted sampling
// makes (1/N)*sum(L) the estimator of exactly what shIrradianceC returns (E/pi), so
// a uniform sky with open/N directions clear gives L * open/N — the old product.
// ⚠⚠⚠ `fieldBounce` IS A REPLACEMENT KNOB, NOT A STRENGTH. It scales the term that
// reads last frame's probe field for the further bounces — and a caller whose
// `skyIrr` was integrated over the hemisphere ALREADY HAS THAT LIGHT IN IT. The
// hemisphere ray that lands on a wall brings back what the wall radiates; adding
// the field lookup on top counts every indirect photon twice, and with cell-to-cell
// feedback the double count compounds instead of just doubling. Measured 22-08: the
// interior went from black to uniformly overbright in one step, which is what a
// double-counted bounce looks like from the inside.
//
// So: pass ddgi.params.w when the hemisphere was NOT integrated (the probe rays),
// and params.w * (1 - skyvis_bounce) when it was (the cache). Same "replace, do not
// add" rule ddgi_receiver.glsl applies to the sky and r_gi_flat to the flat fill.
vec3 giShadeSurface(RTHit h, vec3 V, vec3 skyIrr, float fieldBounce)
{
    // The ray range is the BLAS bubble's, not the light's: past its boundary a
    // shadow ray that hits nothing means "nothing built", not "nothing there".
    const float tmax = ddgi.sunDir.w;

    vec3 radiance = rtShadeHitSky(h, ddgi.sunDir.xyz, ddgi.sunColor.rgb, skyIrr, tmax);

    if (ddgi.casc.z > 0)
        radiance += h.albedo * rtShadeLights(h.pos, h.nrm, ddgi.casc.z, tmax);

    // Converges: each re-bounce is multiplied by an albedo below 1, so the series
    // sums to 1/(1-albedo), and the caller's clamp catches whatever is left.
    if (fieldBounce > 0.0)
        radiance += h.albedo * ddgiSampleIrradiance(h.pos, h.nrm, V) * fieldBounce;

    return radiance;
}

#endif  // GI_SURFACE_GLSL

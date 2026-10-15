#ifndef GI_SH_GLSL
#define GI_SH_GLSL
// xrRenderVulkan — the GI field in TWO-BAND SH, and the one place that says what
// its numbers mean. Shared by the two producer dispatches (gi_probe_sh, gi_froxel)
// and by every consumer of the froxel volume (fog inject, particles).
//
// WHY A SECOND REPRESENTATION OF A FIELD WE ALREADY HAVE
// ------------------------------------------------------
// ddgi_common's lookup answers a SURFACE's question: "irradiance arriving at P
// along normal N", and it costs eight probes x (one octahedral irradiance tap +
// one distance tap) = 16 texture reads. That price is right for one shaded pixel
// and absurd for 590k froxels of fog, and it is the wrong SHAPE besides — a
// volume has no normal, so half the weighting in ddgiSampleCascade (the wrap /
// backface term) is meaningless in air.
//
// So the field is resampled ONCE per frame into the view frustum, in a form that
// answers every direction at once. idTech 8 does the same thing and for the same
// reason (SIGGRAPH 2025, sec. 8: "froxel-version of the irradiance volumes,
// 2-band SH, reused by ALL transparencies"). One trilinear tap then serves the
// fog, the smoke sprites and anything else that is not a G-buffer surface.
//
// THE UNITS, WHICH ARE THE WHOLE GAME
// -----------------------------------
// ddgi_blend_irr stores the COSINE-WEIGHTED MEAN RADIANCE, not the physical
// irradiance — it divides by the cosine lobe's own integral (pi). Write that
// stored function of direction as
//
//     E(N) = c + (2/3) * (d . N)                                            (1)
//
// where L(w) = c + d . w is the two-band radiance field this file carries. The
// factor 2/3 is the cosine lobe's band-1 transfer (A1/A0 = (2pi/3)/pi), and it is
// the ONLY thing standing between "the probes' convention" and "a medium's".
// Two consequences fall straight out of (1) and are used below:
//
//   * the mean of E over the sphere IS the mean radiance c. No pi anywhere —
//     the division already happened in the blend pass;
//   * projecting E onto the band-1 basis gives <E*N> = (2/9) d, hence the 4.5
//     in gi_probe_sh. Get that constant wrong and the field still LOOKS lit; it
//     just points its light in a direction of the wrong length, which is exactly
//     the class of bug no screenshot can catch.
//
// WHAT IS STORED, AND WHY THE BAND-1 TERM IS ACHROMATIC
// -----------------------------------------------------
// c is per channel (vec3). d is stored ONCE, for luminance, and re-tinted with
// c's own colour on evaluation. A per-channel d would be 3 more floats per
// froxel — a third volume — to express light whose COLOUR differs by direction
// inside one froxel. At a 1..16 m probe lattice smoothed over eight probes there
// is no such variation to express: what does exist is a bright direction (the
// window, the sky above the clearing) and it carries the colour of the fill it
// belongs to. So the volume is two RGBA16F images rather than three, and the
// consumers take one less tap.
//
//     rgba of image 0 = c.rgb, and the FIELD WEIGHT in a (see below)
//     rgb  of image 1 = d (luminance), already converted out of the probes'
//                       convention by gi_probe_sh — consumers never see the 4.5.
//
// ⚠ THE WEIGHT IS NOT AN ALPHA, IT IS "DOES THE FIELD REACH HERE".
// The probe volume is finite (ddgiVolumeFade), and outside it the lookup returns
// BLACK by construction. A consumer that mixed toward the field unconditionally
// would paint everything past the outermost cascade pitch dark, with the boundary
// sweeping across the frame as the player walks. Every consumer must lerp from
// its own analytic fill by this weight — the same contract ddgi_receiver states
// for surfaces, restated here because the froxel consumers do not include it.
struct GiProbeSH { vec4 c0; vec4 d1; };   // c0.rgb = c, c0.a = 1 (live), d1.xyz = d

// Colour of the band-1 lobe: c's own hue, so a single achromatic d lights a warm
// interior warm. Guarded on luminance, which is what d was projected against.
vec3 GiField_Tint(vec3 c) { return c / max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-5); }

// L(w) — radiance arriving from direction w (w points FROM the sample TOWARDS the
// source, the same convention the probes integrate).
vec3 GiField_Radiance(vec3 c, vec3 d, vec3 w)
{
    return max(c + GiField_Tint(c) * dot(d, w), vec3(0.0));
}

// What a MEDIUM scatters toward the eye out of this field: the phase-weighted
// integral of L over the sphere. For any normalised phase function with mean
// cosine g,   integral L(w) p(w . v) dw  =  c + g * (d . v),  where v is the
// direction the scattered light travels along, i.e. the view ray. Isotropic media
// (g = 0) collapse to c, which is the correct isotropic answer and not an
// approximation of one.
vec3 GiField_Inscatter(vec3 c, vec3 d, vec3 viewDir, float g)
{
    return max(c + GiField_Tint(c) * (g * dot(d, viewDir)), vec3(0.0));
}

// What a SURFACE with normal N would read — equation (1) above, i.e. exactly what
// the probe lookup would have returned, so a consumer that switches from the
// lattice to this volume does not also switch brightness. Nothing samples this
// yet; it is here because the froxel volume is the cheap answer for translucents
// that DO have a normal (water, glass seen from inside a beam), and the constant
// belongs next to the one it must agree with.
vec3 GiField_Irradiance(vec3 c, vec3 d, vec3 N)
{
    return max(c + GiField_Tint(c) * ((2.0 / 3.0) * dot(d, N)), vec3(0.0));
}

#endif // GI_SH_GLSL

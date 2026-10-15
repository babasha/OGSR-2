// xrRenderVulkan — SHORE BREAK: the wave that arrives, stands up, breaks white
// and runs up the sand.
//
// ⚠ WHY THE EARLIER ATTEMPTS COULD NOT LOOK LIKE THIS. Both the swell and the
// first swash were functions of WORLD XZ against the WIND. A wind wave does not
// know where the beach is, so the whole waterline lifted and dropped together —
// which reads as the pond breathing, never as a wave arriving. And the shore has
// no representation anywhere in the level: no spline, no distance field, nothing
// authored.
//
// ⭐ It does not need one. A wave entering the shallows stops caring about the
// wind and starts caring about the BOTTOM, and the bottom is already measured:
// the ripple tile stores the ground height per 12.5 cm texel, so the still water
// depth `d` is one subtraction away. Every quantity below is a function of d
// alone — and ISO-DEPTH LINES ARE THE SHORELINE. A crest whose phase runs with d
// therefore arrives parallel to the beach, in every bay, around every spit and
// down every inlet, with nothing authored per level and no shoreline geometry.
#ifndef WATER_SHORE_GLSL
#define WATER_SHORE_GLSL

// Live surface + ground, per tile texel (vk_water_ripple / water_mask.frag):
//   .r = the water surface as drawn, .g = the ground in this column
// ⚠ POINT-sampled — both channels are world heights with a -10000 dry sentinel.
//
// WATER_SHORE_NO_MAP: the pool-mask pass PRODUCES this image and therefore must
// not declare a sampler on it (it is a colour attachment there, and its set has
// no such binding). It knows the ground from its own height-map decode anyway and
// only needs the break function below.
#ifndef WATER_SHORE_NO_MAP
// .r = live surface, .g = ground, .b = STILL sheet level. All three per texel.
layout(set = 0, binding = 5) uniform sampler2D uWaterSurf;

// The tile's answer at a world position: .x = still sheet level, .y = ground,
// .z = 1 when both are known. ⚠ Not the pool mask, for a specific reason: the
// mask's clearance rule DISCARDS the strip where the bank stands a few
// centimetres out of the water — precisely the swash zone — so a depth built on
// it would be blind over the only ground the run-up ever touches.
//
// ⚠ And neither height comes from the geometry. Deriving the still level as
// "this vertex minus its wave" feeds the wave back into the phase that produced
// it: at the shoreline the displacement is most of a metre, so the depth would be
// wrong by most of a metre exactly where the model is most sensitive to it.
vec3 waterShoreProbe(vec2 xz)
{
    if (W.p6.w <= 0.0) return vec3(0.0);
    vec2 uv = (xz - W.p6.yz) / W.p6.w;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return vec3(0.0);
    vec4 s = textureLod(uWaterSurf, uv, 0.0);
    if (s.b < -9000.0 || s.g < -9000.0) return vec3(0.0);
    return vec3(s.b, s.g, 1.0);
}

// STILL water depth in metres. NEGATIVE above the waterline, which is what lets
// one function describe both the wave offshore and the sheet running over bare
// sand. -1e9 where the tile knows nothing.
float waterShoreDepth(vec2 xz)
{
    vec3 p = waterShoreProbe(xz);
    return (p.z < 0.5) ? -1e9 : (p.x - p.y);
}
#endif

float wsHash(float n) { return fract(sin(n * 91.3458) * 47453.5453); }

// THE BREAK. Returns:
//   .x = metres the surface stands above its still level here, right now
//   .y = 0..1 whitewater — the broken crest and the foam sheet behind it
//
// `amp`    the offshore wave height (m)
// `runup`  how far up the beach the spent wave climbs (m) — r_wtr_swash
// `period` seconds between crests — r_wtr_swash_t
// `lam`    metres between crests — r_wtr_surf_len
vec2 waterShoreBreak(float d, float t, float amp, float runup, float period, float lam)
{
    // ⚠⚠ NO INCIDENT WAVE, NO RUN-UP. Setting the offshore height to zero used to
    // be a DEGENERATE case rather than an off switch, and it produced the worst
    // artefact of the lot: H -> 0 makes the breaking criterion `d / H` enormous
    // everywhere except within a centimetre of the waterline, so the surf zone
    // collapses to a hairline strip — and the FULL run-up amplitude was still
    // applied inside it. The result is a rolled tube of water hugging the shore, a
    // berm following the waterline exactly. ("Какой-то бугор раз в сколько-то
    // секунд.") Run-up is DRIVEN by the wave that broke; without one there is
    // nothing to run up, and the scaling below makes small waves give short
    // tongues instead of the same tongue through a narrower gate.
    if (amp <= 0.001 || runup <= 0.001) return vec2(0.0);
    runup *= clamp(amp / 0.25, 0.0, 1.6);

    if (d < -runup - 0.35) return vec2(0.0);
    const float TAU = 6.2831853;
    // Out in deep water the shore wave has to be GONE, and gone smoothly: a hard
    // cutoff draws a straight line along a depth contour, which is one of the few
    // shapes a real body of water never has.
    const float deep = 1.0 - smoothstep(7.0, 13.0, d);
    if (deep <= 0.0) return vec2(0.0);

    // PHASE RUNS SHOREWARD. A point of constant phase moves toward smaller d as
    // time grows — the crest travels IN. This one line is the whole difference
    // between a wave and a pond going up and down.
    float ph  = t / max(period, 0.5) - d / max(lam, 1.0);
    float cyc = floor(ph);
    float s   = ph - cyc;
    // SETS. Every wave the same size is the second-most obvious tell after a
    // waterline that does not travel.
    float set = 0.45 + 1.05 * wsHash(cyc);

    // SHOALING (Green's law, H ~ d^-1/4): the crest gathers and stands up as the
    // bottom rises to meet it. This is what makes the wave read as ARRIVING.
    float shoal = clamp(pow(1.7 / max(d, 0.10), 0.30), 0.85, 3.2);
    float H     = amp * set * shoal;

    // BREAKING. A wave breaks when its height is about 0.78 of the depth beneath
    // it; past that the crest cannot stand and collapses into a bore. `surf` is
    // how far into that collapse this point is: 0 offshore, 1 in the whitewater.
    float surf = smoothstep(1.55, 0.62, max(d, 0.0) / max(H, 0.02));

    // Two profiles, blended by how broken the wave is here.
    //  offshore — a peaky crest with long flat troughs (a sine reads as corrugation)
    //  inshore  — fast up, slow drain: the asymmetry of real swash
    float pOff = pow(0.5 - 0.5 * cos(TAU * s), 2.6);
    float up   = smoothstep(0.0, 0.09, s);
    float pIn  = up * (1.0 - smoothstep(0.09, 0.82, s));
    float prof = mix(pOff, pIn, surf);

    // Height: the offshore crest becomes the run-up sheet as it breaks. Above the
    // waterline only the run-up term survives, and it has to be able to reach:
    // `runup` is measured from the still line, so the sheet covers ground up to
    // that height when the profile peaks.
    float eta = mix(H, max(runup * set, H), surf) * prof;

    // WHITEWATER. Made at the front of the bore and left behind it, thinning as
    // the sheet drains. A tongue running over bare sand is essentially all foam,
    // which is the second term.
    float w = surf * up * (1.0 - smoothstep(0.05, 0.62, s));
    w = max(w, surf * smoothstep(0.35, 0.0, d) * prof * 1.15);
    return vec2(eta * deep, clamp(w, 0.0, 1.0) * deep);
}

#endif // WATER_SHORE_GLSL

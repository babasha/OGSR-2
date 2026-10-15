// xrRenderVulkan — WATER: the surface field, shared by the tessellation
// evaluation stage (which DISPLACES by it) and the fragment stage (which
// SHADES by it). One source, or the geometry and the lighting disagree about
// where the wave is.
#ifndef WATER_COMMON_GLSL
#define WATER_COMMON_GLSL

layout(set = 0, binding = 1) uniform WaterParams {
    vec4 p0;   // x = time (s), y = swell HEIGHT (m), z = base wavenumber (1/m), w = speed
    vec4 p1;   // xy = wind dir (unit), z = extinction k (1/m), w = rain amount 0..1
    vec4 p2;   // xyz = murk colour, w = reflection strength
    vec4 p3;   // x = detail fade dist (m), y = base roughness, z = glitter, w = debug
    vec4 p4;   // x = shore fade (m), y = foam strength, z = foam width (m), w = micro slope
    vec4 p5;   // x = tessMax, y = tessNear (m), z = tessFar (m), w = displacement gain
    vec4 p6;   // x = ripple sim on, yz = tile origin (world XZ), w = tile size (m)
    // x = sim slope gain, y = sim height gain, z = 1/texels,
    // w = REFERENCE SHEET HEIGHT: the world Y of the water plane nearest the
    //     camera. The near-field grid falls back to it where the pool mask has no
    //     water, so its mesh stays continuous across a shoreline.
    vec4 p7;
    // Camera world position. Duplicated from the shared lighting UBO on purpose:
    // the tessellation control stage needs it for the LOD distance and does not
    // include light_ubo.glsl (which would drag every sampler binding into a
    // stage that samples nothing).
    vec4 p8;   // xyz = camera world position
    // Refraction + caustics. x = refraction strength (UV metres per unit slope),
    // y = caustic strength, z = caustic sharpness, w = 1/scene-copy scale.
    vec4 p9;
    // x = RUN-UP in metres: how far up the beach a spent wave climbs.
    // y = shelter amount: how much a roof overhead is allowed to calm the water.
    // z = murk multiplier for sheltered (stagnant) water.
    // w = seconds between arriving crests.
    vec4 p10;
    // SHORE BREAK (water_shore.glsl). x = metres between crests, y = whitewater
    // strength, z = offshore wave height for the break (m), w = master enable.
    vec4 p11;
    // SPECTRAL WAVES (vk_water_fft). xyz = the three cascade tile sizes in metres
    // — one world XZ becomes three sets of tile coordinates and all three are
    // needed; w = how much of the transformed field to use (0 = none, and the
    // analytic swell carries everything exactly as it did before).
    vec4 p12;
    // x = the field holds a finished transform (NOT merely "the cvar is on"),
    // y = whitewater gain on the Jacobian, z = the body span in metres at which
    // the spectral field reaches full strength, w = slope gain.
    vec4 p13;
    // Straight-down ortho view-proj of the rain/sky-occlusion map. Duplicated
    // from the lighting UBO for the same reason the camera position is: the
    // tessellation stages cannot include light_ubo.glsl (it carries functions
    // that use fragment-only builtins), and BOTH stages have to agree on where
    // the wave is — including where there is no wave because there is a ceiling.
    mat4 rainVP;
} W;

// Interactive ripple field (vk_water_ripple): .r = height (m), .g = velocity.
// A 1x1 zero texture until the sim runs, so the samples below cost nothing.
//
// OPTIONAL. The pool-mask pass includes this file for the SWELL alone — it feeds
// the sim's own contact test, which already holds the field in a storage image
// and would double-count it — and it has no ripple binding to offer. Defining
// WATER_NO_RIPPLE drops both the sampler and the tap.
#ifndef WATER_NO_RIPPLE
layout(set = 0, binding = 2) uniform sampler2D uRipple;
#endif

// SPECTRAL WAVE FIELD (vk_water_fft): one ARRAY LAYER per cascade, each a
// periodic tile sampled by world XZ / its own size.
//   uWFFTDisp  = (Dx, height, Dz, foam)      — what a vertex is moved BY
//   uWFFTDeriv = (dh/dx, dh/dz, Jacobian, -) — what the surface is SHADED by
//
// OPTIONAL for the same reason the ripple field is: the pool-mask pass includes
// this file for the swell alone and has neither binding to offer. WATER_NO_FFT
// drops the samplers and every tap on them, and the analytic swell is left to
// carry the field — which for the mask's purposes (where is the waterline) it
// does well enough, since the shoreline there is driven by the shore break.
#ifndef WATER_NO_FFT
layout(set = 0, binding = 6) uniform sampler2DArray uWFFTDisp;
layout(set = 0, binding = 7) uniform sampler2DArray uWFFTDeriv;
#endif

// LOCAL FETCH (water_fetch.comp): metres of basin per tile texel, 0 where there
// is no local answer. Bound in BOTH set 0 layouts this file is compiled against —
// the water pass's own set and the pool-mask pass's small one — because the mask
// pass writes the record the wetness runs on and has to agree with the surface
// about how big the wave is. See waterLocalFetch below.
layout(set = 0, binding = 8) uniform sampler2D uWaterFetchMap;

// Value noise — local so the tessellation stages, which do not include
// wet_common.glsl, can use it too.
float wHash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float wNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(wHash(i), wHash(i + vec2(1, 0)), f.x),
               mix(wHash(i + vec2(0, 1)), wHash(i + vec2(1, 1)), f.x), f.y);
}

// ---------------------------------------------------------------------------
// FETCH — the length of water the wind has to work on. This is the thing the
// wave field had no idea about: it was a function of world XZ alone, so a
// flooded cellar five metres across got the same 28-metre open-water swell as
// the Cordon pond, and since a quarter of one wavelength spans the whole room,
// the surface heaved up and down as one sheet. That reads as surf indoors.
//
// A wave only exists if its length FITS in the basin several times over;
// anything longer is not a wave there, it is the water level. So each octave is
// gated by how many of its wavelengths span the pool. Killing the long octaves
// scales the amplitude down for free — waterSwell normalises by the sum of ALL
// octave weights, so what dies is subtracted from the height as well.
//
// fetch <= 0 means "unknown / open water": every octave lives, old behaviour.
// ---------------------------------------------------------------------------
float waterFit(float f, float fetch)
{
    if (fetch <= 0.0) return 1.0;
    float n = fetch * f * 0.15915494;      // fetch / wavelength (lambda = 2pi/f)
    return smoothstep(0.55, 1.9, n);       // full strength once ~2 fit across
}

// How much of the LONG swell survives in this basin, 0..1. Only the first five
// octaves count: the metre-and-up waves are the ones that run up a shore, bare
// the bottom and break into foam. Capillary ripple does none of that, so it must
// not vote — otherwise a cellar with a live centimetre ripple still reads as
// having surf.
float waterSwellEnergy(float fetch)
{
    if (fetch <= 0.0) return 1.0;
    float a = 1.0, f = W.p0.z, live = 0.0, wsum = 0.0;
    for (int i = 0; i < 5; ++i) {
        live += a * waterFit(f, fetch);
        wsum += a;
        a *= 0.76;
        f *= 1.55;
    }
    return live / max(wsum, 1e-4);
}

// ---------------------------------------------------------------------------
// THE FETCH THIS TEXEL ACTUALLY HAS, as opposed to the one its MESH has.
//
// `bboxFetch` is what the CPU pushes per surface: sqrt(area) of the water
// visual's bounding box. On a level whose water is one mesh — Cordon's is one
// visual spanning 79.8 x 148.2 m — that number is the same for the river and for
// the two-metre circle of water inside a well, and the well got the river's
// swell: 37 cm of crest in 22 cm of water, over the ring and into the yard.
//
// The map read here is measured from the POOL MASK (water_fetch.comp), which
// knows where each body ends at 12.5 cm. It can only ever say "smaller": every
// texel whose water reaches the search limit is written as 0, and 0 means "no
// local answer, keep what the CPU said". Open water is therefore bit-identical
// to the old behaviour, and this cannot flatten a sea — only a puddle.
//
// ⭐ MIN OVER A 3x3, NOT A BILINEAR TAP. The value is metres with 0 for "no
// answer", so filtering it would mix a real basin size with a sentinel and
// invent basins that are neither. Taking the smallest non-zero neighbour instead
// dilates the answer by one coarse texel, which is what covers the sheet's edge:
// the surface is drawn a little outside the last WET texel (the level sheet runs
// on under the bank), and without the dilation that rim would keep the open
// river's wave and tear a crest off the calm pool next to it.
float waterLocalFetch(vec2 xz, float bboxFetch)
{
    // ⚠ p6.x FIRST, and it is not belt-and-braces. With the sim off there is no
    // tile, no mask and no fetch map — and the descriptor then holds the same
    // stand-in every optional slot in this pass uses, the scene DEPTH view. Its
    // values are 0..1, which read as "every pool on the level is half a metre
    // across" and would flatten the sea. A dummy that samples as zero would be
    // harmless; this one is not, so the gate has to be here.
    if (W.p6.x < 0.5 || W.p6.w <= 0.0) return bboxFetch;
    vec2 uv = (xz - W.p6.yz) / W.p6.w;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return bboxFetch;
    vec2  ts   = vec2(textureSize(uWaterFetchMap, 0));
    float best = 0.0;
    for (int i = 0; i < 9; ++i) {
        vec2  o = vec2(float(i % 3) - 1.0, float(i / 3) - 1.0);
        float f = textureLod(uWaterFetchMap, uv + o / ts, 0.0).r;
        if (f > 0.0) best = (best > 0.0) ? min(best, f) : f;
    }
    if (best <= 0.0) return bboxFetch;
    // bboxFetch <= 0 is "open water / unknown" from the CPU side; the local
    // measurement is then the only answer there is.
    return (bboxFetch > 0.0) ? min(bboxFetch, best) : best;
}

// ---------------------------------------------------------------------------
// SHELTER — the other half of "the cellar is simulating surf". Wind waves need
// WIND, and there is none under a roof. The wave field knew nothing about roofs
// either, so a flooded basement got the same weather-driven swell as the open
// marsh above it.
//
// The sky-occlusion map (the one the rain wetness and the ambient gate already
// use) answers this directly. The query is HEMISPHERIC, not straight up, for the
// reason the ambient gate learned the hard way: a pond under a pipe rack or a
// tree crown still has wind arriving from the open sides, and a narrow overhead
// test would print every overhead object's footprint on the water as a dead calm
// blob. Local tap plus three rings at ~2/4/6 m with height slack — a real
// interior is covered on every ring, a yard recovers most of its openness.
//
// The sampler is a PARAMETER so the fragment stage can pass the uRainMap it
// already has from light_ubo.glsl while the tessellation stage passes its own
// declaration, and the tessellation CONTROL stage, which includes this file but
// calls none of this, needs no rain binding at all.
float waterSkyOpen(sampler2D rainMap, vec3 wp)
{
    vec3 n = (W.rainVP * vec4(wp, 1.0)).xyz;   // ortho -> already NDC
    vec2 uv = n.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || n.z <= 0.0 || n.z >= 1.0)
        return 1.0;                            // outside the map = open sky
    float ref  = n.z - 0.0015;
    float refW = ref - 0.01;                   // ignore occluders under ~3.5 m
    vec2  px   = 1.0 / vec2(textureSize(rainMap, 0));
    float visC = step(ref, textureLod(rainMap, uv, 0.0).r);
    const vec2 R[12] = vec2[12](
        vec2( 14.0, 0.0), vec2(-14.0, 0.0), vec2(0.0,  14.0), vec2(0.0, -14.0),
        vec2( 20.0, 20.0), vec2(-20.0, 20.0), vec2(20.0, -20.0), vec2(-20.0, -20.0),
        vec2( 41.0, 0.0), vec2(-41.0, 0.0), vec2(0.0,  41.0), vec2(0.0, -41.0));
    float w = 0.0;
    for (int i = 0; i < 12; ++i)
        w += step(refW, textureLod(rainMap, uv + R[i] * px, 0.0).r);
    w *= 1.0 / 12.0;
    return clamp(0.35 * visC + 0.65 * w, 0.0, 1.0);
}

// The same thing scaled by r_wtr_shelter (0 = roofs do not calm the water, the
// old behaviour). 1 = fully sheltered water goes still.
float waterShelter(sampler2D rainMap, vec3 wp)
{
    float s = clamp(W.p10.y, 0.0, 1.0);
    if (s < 0.001) return 1.0;
    return mix(1.0, waterSkyOpen(rainMap, wp), s);
}

// GUST PATCHES. The single thing that separates "a water texture" from water:
// wind does not arrive evenly. It comes in cat's paws — patches of roughened
// surface tens of metres across that DRIFT downwind, with glassy slicks between
// them. Without this the amplitude is constant over the whole pond and the eye
// reads the surface as one stamped pattern, however good the wave function is.
float waterGust(vec2 p, float t, vec2 wind)
{
    vec2 d = wind * t * 0.6;                       // patches travel with the wind
#ifdef WATER_GUST_ONE
    float g = wNoise(p * 0.03 - d * 0.03);              // one ~33 m cell (the lean twin)
#else
    float g = wNoise(p * 0.018 - d * 0.018) * 0.62      // ~55 m cells
            + wNoise(p * 0.055 - d * 0.055) * 0.38;     // ~18 m cells
#endif
    return 0.30 + 1.15 * g * g;                    // squared: slicks stay properly calm
}

// ---------------------------------------------------------------------------
// SWELL. Sum of octaves, but not the naive one: each octave is exp(sin(x)-1),
// which has sharp crests and flat troughs (a plain sine reads as corrugated
// iron), and each octave's domain is WARPED by the slope of the one before it,
// which destroys the regular interference grid that gives summed sines away.
// Octave directions fan around the wind by the golden angle so no two ever
// line up. Returns height in metres (.x) and the analytic slope (.yz) — the
// slope is what the shading needs and it must be the true derivative, or the
// sun glitter turns into noise.
// ---------------------------------------------------------------------------
vec3 waterSwell(vec2 p, float t, float amp, float freq, vec2 wind, float lod, float fetch)
{
    if (amp <= 0.0) return vec3(0.0);
    float h = 0.0;
    vec2  g = vec2(0.0);
    float a = 1.0, f = freq, wsum = 0.0, ang = 0.0;
    vec2  d0 = normalize(wind + vec2(1e-5, 0.0));
    vec2  pp = p;
    // NINE octaves at x1.55, not six at x1.3: the old chain spanned barely 3.7x
    // in wavelength (18 m down to 5 m), and a spectrum that narrow reads as one
    // repeated wave no matter how it is warped. This one runs ~30 m to ~0.7 m.
    // WATER_SWELL_OCT trims the short end for the lean (Switch) twin; the long
    // octaves carry nearly all the weight, so the surface keeps its shape.
#ifndef WATER_SWELL_OCT
#define WATER_SWELL_OCT 9
#endif
    for (int i = 0; i < WATER_SWELL_OCT; ++i) {
        float c = cos(ang), s = sin(ang);
        vec2  dir = vec2(d0.x * c - d0.y * s, d0.x * s + d0.y * c);
        // Deep-water dispersion: phase speed = sqrt(g/k), so the long swell
        // outruns the chop instead of the whole field sliding as one sheet.
        float w  = sqrt(9.81 * f) * W.p0.w;
        float x  = dot(dir, pp) * f + t * w;
        float e  = exp(sin(x) - 1.0);          // 0..1
        float de = e * cos(x);
        // Octave i survives only while its wavelength is still resolvable AND
        // still fits in the body of water it is supposed to be crossing.
        float li = clamp(lod * (1.0 - float(i) * 0.10), 0.0, 1.0) * waterFit(f, fetch);
        h += a * e * li;
        g += dir * (a * de * f * li);
        // Domain warp (the "FBM" part): ride the previous octave's slope.
        pp -= dir * (de * a * 0.5 / f);
        wsum += a;
        a *= 0.76;
        f *= 1.55;
        ang += 2.39996;                        // golden angle — never repeats
    }
    float inv = amp / max(wsum, 1e-4);
    return vec3(h * inv, g * inv);
}

// ---------------------------------------------------------------------------
// SWASH — the sheet of water that runs UP a shore and drains back.
//
// This is not the swell and it cannot be made out of the swell. A wave field is
// symmetric in time: it rises and falls on the same clock, everywhere at once,
// and it has no leading edge. Swash is the opposite on all three counts — it
// arrives as a FRONT, it rushes up in about a second and takes five to drain,
// and it comes in SETS, so one surge in six or seven reaches much further than
// the rest. Those three asymmetries are the entire read; without them a shoreline
// that moves just looks like the pond is breathing.
//
// Returns:  .x = metres of run-up above the still surface, here, now
//           .y = 0..1 how foamy that water is — peaks at the front and dissolves
//                through the backwash, which is what turns a moving edge into surf
//
// `reach` is r_wtr_swash and is now METRES, not a gain. It used to multiply the
// swell height and get added to a DEPTH to widen an alpha fade; at its old default
// of 2.2 that quietly turned a 19 cm wave into 42 cm of "water" standing on the
// bank, with nothing on screen to account for it.
vec2 waterSwash(vec2 p, float t, float reach, float period, vec2 wind, float energy)
{
    if (reach <= 0.001 || energy <= 0.001) return vec2(0.0);
    const float TAU = 6.2831853;
    // The front SWEEPS. Phase runs with time and against the direction the waves
    // travel, so the run-up arrives at an angle and races along the beach instead
    // of the whole shore lifting as one slab. ~90 m between surges along the shore.
    float ph = t / max(period, 0.5) - dot(p, normalize(wind + vec2(1e-5, 0.0))) * 0.011;
    // ...and it is not a metronome. A slow noise on the phase bends the front and
    // desynchronises one stretch of shore from the next.
    ph += wNoise(p * 0.013) * 0.55;

    float cyc = floor(ph);
    float s   = ph - cyc;                    // 0..1 through one swash cycle
    // SETS. Surge n reaches a different distance from surge n+1, and the big ones
    // are what leave a wet strip high up that then has time to dry.
    float amp = 0.40 + 1.05 * wHash(vec2(cyc, 17.3));

    // Fast up, slow back, then a pause before the next one.
    float up   = smoothstep(0.0, 0.11, s);
    float down = 1.0 - smoothstep(0.11, 0.82, s);
    float h    = reach * energy * amp * up * down;

    // Foam is made at the FRONT — the bore breaking as it climbs — and then rides
    // the sheet back down, thinning. Brightest just after the surge, gone before
    // the next one arrives.
    float foam = up * (1.0 - smoothstep(0.08, 0.60, s));
    return vec2(h, clamp(foam, 0.0, 1.0));
}

// CAPILLARY RIPPLE — the centimetre texture that keeps standing water alive in
// dead calm. Slope only: at ~1 mm of height it is invisible as geometry and
// only matters to the normal. Deliberately wind-INDEPENDENT (the swell dies
// with the wind, this does not).
vec2 waterMicro(vec2 p, float t, float amp, float nearFade)
{
    if (amp <= 0.0 || nearFade <= 0.0) return vec2(0.0);
    vec2  d = vec2(0.0);
    float a = amp, f = 3.7, ang = 0.7;
    for (int i = 0; i < 3; ++i) {
        vec2 k = vec2(cos(ang), sin(ang)) * f;
        float ph = dot(p, k) + t * (1.9 + 0.8 * float(i));
        d += k * (a * cos(ph));
        a   *= 0.55;
        f   *= 2.3;
        ang += 2.6;
    }
    return d * nearFade;
}

// INTERACTIVE RIPPLES. Height (.x) and slope (.yz) of the simulated field at a
// world position, faded out at the tile border so the moving window has no
// visible edge. Zero outside the tile — the analytic swell carries the far
// field on its own.
vec3 waterRipple(vec2 xz)
{
#ifdef WATER_NO_RIPPLE
    return vec3(0.0);
#else
    if (W.p6.x < 0.5) return vec3(0.0);
    vec2 uv = (xz - W.p6.yz) / max(W.p6.w, 0.001);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return vec3(0.0);
    vec2  e    = min(uv, vec2(1.0) - uv);
    float edge = smoothstep(0.0, 0.05, min(e.x, e.y));
    if (edge <= 0.0) return vec3(0.0);

    float ts = W.p7.z;                        // 1 / texels
    float h  = textureLod(uRipple, uv, 0.0).r;
    float hL = textureLod(uRipple, uv - vec2(ts, 0.0), 0.0).r;
    float hR = textureLod(uRipple, uv + vec2(ts, 0.0), 0.0).r;
    float hD = textureLod(uRipple, uv - vec2(0.0, ts), 0.0).r;
    float hU = textureLod(uRipple, uv + vec2(0.0, ts), 0.0).r;
    float mPerTexel = W.p6.w * ts;
    vec2  slope = vec2(hR - hL, hU - hD) / (2.0 * max(mPerTexel, 1e-4));
    return vec3(h * edge, slope * edge);
#endif
}

// The whole surface at a world position: height (.x, metres) + slope (.yz).
// ---------------------------------------------------------------------------
// SPECTRAL FIELD (Tessendorf). Three periodic tiles summed by world position.
//
// The per-cascade weight is waterFit on the cascade's OWN size, which is the
// same test every analytic octave already passes: a wave whose length does not
// fit across the pool a couple of times is not a wave there, it is the water
// level. So a 250 m cascade is simply absent from a flooded cellar while the 8 m
// one still lives there — the gate is per band, not one master fade, because
// "how big is this water" has a different answer for each band.
//
// `mix` comes back as 0..1: how much of the wind-wave band the spectral field
// has taken over. The analytic swell is scaled DOWN by it rather than being added
// to — the two model the same waves, and running both at full strength would not
// be twice the sea, it would be the same sea twice with the phases fighting.
// ---------------------------------------------------------------------------
vec4 waterFFTAll(vec2 xz, float fetch, float calm, out vec2 slope, out float mixOut)
{
    slope = vec2(0.0);
    mixOut = 0.0;
#ifdef WATER_NO_FFT
    return vec4(0.0);
#else
    if (W.p13.x < 0.5 || W.p12.w <= 0.0 || calm <= 0.001) return vec4(0.0);
    // OVERALL fetch gate, on top of the per-cascade fit below. The per-band test
    // alone would still let the finest cascade into a bathtub — its waves are
    // centimetres long and they "fit" anywhere. But an open-sea spectrum is the
    // wrong model for a puddle at ANY wavelength, so there is a size below which
    // this whole thing stays out (r_wtr_fft_fetch, metres). This is the switch
    // that keeps the model that knows nothing about walls out of the cellar.
    float open = (fetch <= 0.0) ? 1.0 : smoothstep(0.35 * W.p13.z, W.p13.z, fetch);
    if (open <= 0.001) return vec4(0.0);
    calm *= open;
    // Explicit array rather than dynamic subscripting of the vec4: the loop index
    // is a variable, and this is the form every driver agrees about.
    float Ls[3] = float[3](W.p12.x, W.p12.y, W.p12.z);
    vec4  acc   = vec4(0.0);
    float fitSum = 0.0;
    for (int c = 0; c < 3; ++c) {
        float L = Ls[c];
        if (L <= 0.0) continue;
        float fit = waterFit(6.2831853 / L, fetch);
        fitSum += fit;
        float w = fit * calm;
        if (w <= 0.001) continue;
        vec3 uv = vec3(xz / L, float(c));
        // ⚠ textureLod, not texture(). These arrays have no mip chain, so the two
        // pick the same texel — but the taps sit inside a loop whose `continue`
        // depends on the fetch, which varies per pixel, and an IMPLICIT lod under
        // non-uniform control flow has undefined derivatives. Nothing to gain and
        // a rule to break. It is also what every other sampler in this file does.
        acc   += textureLod(uWFFTDisp,  uv, 0.0) * w;
        slope += textureLod(uWFFTDeriv, uv, 0.0).xy * w;
    }
    mixOut = clamp(calm * fitSum * 0.3333333, 0.0, 1.0) * clamp(W.p12.w, 0.0, 1.0);
    return acc;
#endif
}

// HORIZONTAL displacement only — for the vertex stages, which are the only ones
// that can act on it. (A fragment shader is already at a fixed world position;
// shading the displaced surface at its undisplaced coordinates is the standard
// approximation and is why the slope above is sampled the same way.)
vec2 waterFFTChop(vec2 xz, float fetch, float calm)
{
#ifdef WATER_NO_FFT
    return vec2(0.0);
#else
    vec2 sl; float mx;
    vec4 f = waterFFTAll(xz, fetch, calm, sl, mx);
    return f.xz * W.p12.w;
#endif
}

// WHITEWATER from the Jacobian: where the horizontal displacement has folded the
// surface over itself, which is what a breaking wave IS. Only the fragment stage
// wants this, and only when the gain is up, so it is its own tap.
float waterFFTFoam(vec2 xz, float fetch, float calm)
{
#ifdef WATER_NO_FFT
    return 0.0;
#else
    if (W.p13.y <= 0.0) return 0.0;
    vec2 sl; float mx;
    vec4 f = waterFFTAll(xz, fetch, calm, sl, mx);
    return clamp(f.w * W.p13.y, 0.0, 1.0);
#endif
}

// `dist` is the distance to the camera and drives both LOD fades; `fetch` is the
// span of THIS body of water (0 = open water) and gates which octaves can exist
// in it at all. Passed as an argument rather than read from the push block so
// the tessellation CONTROL stage, which includes this file for the LOD constants
// but never evaluates the field, does not have to declare a push block it has no
// range for.
// .xyz = height + slope of the WHOLE surface (swell + gusts + ripple + micro).
// .w   = the SWELL's height alone. Not a convenience: the shoreline run-up must
//        be driven by the long wave and nothing else. Fed the total instead, the
//        interactive ripple — multiplied by r_wtr_sim_height, which is a look
//        knob people set to 4 — swung the waterline by a metre, and every trough
//        of your own footstep rings punched the water layer out of existence.
//        "The wave goes completely transparent, as if I'd swept the water away."
vec4 waterField(vec2 xz, float dist, float fetch, float calm)
{
    float lod  = clamp(1.0 - dist / max(W.p3.x, 1.0), 0.0, 1.0) * 0.85 + 0.15;
    // Gusts scale the swell, NOT the capillary ripple: a slick is smooth at the
    // wave scale but still has surface texture, which is what a real calm patch
    // looks like. `calm` (shelter) scales the wind-driven swell the same way and
    // for the same reason — and, like the gust, leaves the capillary ripple and
    // the interactive sim alone: still water indoors is not a mirror, it still
    // has a millimetre of texture and it still takes rings from a footstep.
    float gust = waterGust(xz, W.p0.x, W.p1.xy);
    // The spectral field first: what it takes over, the analytic swell gives up.
    vec2  fsl;
    float fmix;
    vec4  f    = waterFFTAll(xz, fetch, calm, fsl, fmix);
    vec3  s    = waterSwell(xz, W.p0.x, W.p0.y * gust * calm * (1.0 - fmix), W.p0.z, W.p1.xy, lod, fetch);
    vec3  r    = waterRipple(xz);
#ifdef WATER_NO_MICRO
    vec2  mic  = vec2(0.0);
#else
    vec2  mic  = waterMicro(xz, W.p0.x, W.p4.w, clamp(1.0 - dist / 25.0, 0.0, 1.0));
#endif
    // NOT modulated by the gust. The gust field was invented to break up a nine-
    // octave sum that was otherwise identical everywhere; a Phillips spectrum has
    // that variation in it already, and multiplying a field by a moving envelope
    // costs the exact agreement between its height and the slope stored beside it.
    float fh = f.y * W.p12.w;
    return vec4(s.x + fh + r.x * W.p7.y,
                s.yz + fsl * W.p13.w + r.yz * W.p7.x + mic,
                // The LONG wave alone — swell plus spectral, no ripple and no
                // capillary. This drives the shoreline run-up, and once the
                // spectral field has taken the swell's place it has to inherit
                // its job too, or the water goes quiet at the bank the moment
                // the better waves switch on.
                s.x + fh);
}

// ---------------------------------------------------------------------------
// CAUSTICS. Sunlight refracted by the surface converges on the bottom; where
// the surface is CONCAVE the rays bunch up and paint the bright web, where it
// is convex they spread and the bottom darkens. That convergence is the
// divergence of the slope field — so it comes straight out of the same wave
// function, no texture and no photon pass.
//
// The pattern belongs at the point where the ray ENTERED the water, not where
// it lands, so the bottom position is walked back along the sun direction by
// the depth. That is what makes the web slide correctly as the sun moves.
float waterCaustic(vec2 bottomXZ, float depth, vec3 sunDir, float dist, float fetch, float calm)
{
    if (W.p9.y <= 0.001 || depth <= 0.01) return 0.0;
    // Refracted direction, flattened: water bends the sun toward the vertical
    // by ~1/1.33, which is exactly why a low sun still paints a web.
    vec2  drift = (abs(sunDir.y) > 0.05) ? sunDir.xz / abs(sunDir.y) * 0.75 : vec2(0.0);
    vec2  p     = bottomXZ + drift * depth;
    const float eps = 0.35;
    vec2 g0 = waterField(p,                   dist, fetch, calm).yz;
    vec2 gx = waterField(p + vec2(eps, 0.0),  dist, fetch, calm).yz;
    vec2 gz = waterField(p + vec2(0.0, eps),  dist, fetch, calm).yz;
    float div = ((gx.x - g0.x) + (gz.y - g0.y)) / eps;      // ~ laplacian of the height
    // Ray-bundle area scales as |1 - depth*div|; its reciprocal is the light
    // concentration. Clamped, or a caustic singularity blows out to infinity.
    float conc = 1.0 / max(abs(1.0 - depth * div * 0.25), 0.15);
    return pow(clamp(conc - 1.0, 0.0, 6.0), W.p9.z) * W.p9.y;
}

#endif // WATER_COMMON_GLSL

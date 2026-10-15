// xrRenderVulkan - volumetric raymarched clouds (Schneider / Nubis lineage).
//
// Replaces the painted cube clouds and the flat scrolling R4 cloud dome with a real
// 3D medium: density from tileable noise volumes shaped by a weather map, lit by a
// secondary march toward the sun, composited over the procedural sky.
//
// WHY THIS SHAPE OF SOLUTION
// The old plan (2026-07-05) had to start by DETECTING and dissolving the clouds
// painted into the weather cubemap — a brightness/blueness heuristic that would need
// retuning per weather preset. With the procedural sky (r_sky_proc) the cubemap is
// not sampled at all, so that entire step evaporated: there is nothing to suppress.
//
// The lighting is the point of the whole exercise. Sun colour comes from
// SunTransmittance() and ambient from AtmosphereRadiance() — the SAME model the dome
// is drawn with — so at dusk the reddened low sun lights the UNDERSIDES of the clouds
// warm while their tops stay cool from the blue zenith, and thin edges glow where the
// forward scattering lobe points at the viewer. That is what makes a sunset sky read
// as dramatic rather than as a gradient, and it is impossible with authored colours
// that measure neutral grey.
//
// ATTRIBUTION. Three pieces here are adapted from Federico Vaccaro's
// OpenGL-VolumetricClouds (https://github.com/RobertBeckebans/OpenGL-VolumetricClouds,
// MIT License, Copyright (c) 2018 Federico Vaccaro): the three-way cloud-type gradient
// blend, the six-direction cone kernel for the light march, and the horizon fog
// falloff. Each is marked at its use site. That project in turn credits Nadir Roman
// Guerrero, WFP (roar11.com), reinder, Rikard Olajos and Clay John.
//
// Requires (provided by the includer):
//   - atmosphere.glsl  (AtmosphereRadiance / SunTransmittance / atmoRaySphere)
//   - noise_common.glsl for nc_remap
//   - the CL uniform block + uCloudShape / uCloudDetail / uCloudWeather samplers
#ifndef CLOUDS_GLSL
#define CLOUDS_GLSL

// Height fraction [0,1] within the cloud band, on the PLANET SHELL (not a flat slab)
// — clouds must converge toward the horizon the way real ones do, and a flat slab
// famously ends in a hard visible edge partway to the horizon.
float cl_heightFraction(vec3 p)
{
    float alt = length(p) - ATMO_R_PLANET;
    return clamp((alt - CL.band.x) / max(CL.band.y - CL.band.x, 1.0), 0.0, 1.0);
}

// Vertical density profile, selected by cloud TYPE. This is what gives "clouds at
// different heights" from a single marched band: at type 0 the medium hugs the
// bottom as a flat sheet (stratus), at 1 it fills the column and towers (cumulus),
// with stratocumulus in between.
//
// The three-gradient BLEND (rather than the two-way mix this originally used) is
// from Federico Vaccaro's OpenGL-VolumetricClouds (MIT, (c) 2018), itself following
// the Nubis/Decima SIGGRAPH 2017 talk. Blending three profiles by weight instead of
// nesting two mixes keeps the transition monotonic — the nested form briefly LOST
// density in the middle of the type range, printing a band of thin cloud wherever
// the weather map happened to sit near type 0.5.
#define CL_GRAD_STRATUS       vec4(0.00, 0.10, 0.20, 0.30)
#define CL_GRAD_STRATOCUMULUS vec4(0.02, 0.20, 0.48, 0.625)
#define CL_GRAD_CUMULUS       vec4(0.00, 0.1625, 0.88, 0.98)

float cl_heightGradient(float h, float type)
{
    float fStratus = 1.0 - clamp(type * 2.0, 0.0, 1.0);
    float fStrCum  = 1.0 - abs(type - 0.5) * 2.0;
    float fCumulus = clamp(type - 0.5, 0.0, 1.0) * 2.0;

    vec4 g = fStratus * CL_GRAD_STRATUS
           + fStrCum  * CL_GRAD_STRATOCUMULUS
           + fCumulus * CL_GRAD_CUMULUS;

    return smoothstep(g.x, g.y, h) - smoothstep(g.z, g.w, h);
}

// Cloud density at a world point. `cheap` skips the detail volume — used by the
// light march, where the high-frequency erosion is invisible but would triple cost.
float cl_density(vec3 p, float lod, bool cheap)
{
    float h = cl_heightFraction(p);
    if (h <= 0.0 || h >= 1.0) return 0.0;

    // Wind: the whole field drifts, and higher slices drift FASTER (vertical shear).
    // Without shear a towering cloud translates rigidly and reads as a moving prop.
    vec3 wind = vec3(CL.wind.x, 0.0, CL.wind.y) * CL.wind.z * CL.timeP.x;
    vec3 pw   = p + wind * (1.0 + h * 0.35);

    vec2 wuv = pw.xz * CL.scales.z;
    vec3 wm  = texture(uCloudWeather, wuv).rgb;
    // Coverage, gently modulated by the large-scale band so cloud gathers into
    // systems. This used to MULTIPLY the two [0,1] fields outright, which crushed
    // effective coverage to ~0.16 — and since the survival threshold below is
    // (1 - coverage), that put the bar at 0.84 while a typical base value is ~0.67.
    // Nothing ever passed: the deck was mathematically empty everywhere.
    float coverage = clamp(wm.r * mix(0.7, 1.0, wm.b) * CL.shape.x, 0.0, 1.0);
    if (coverage <= 0.002) return 0.0;
    float type = wm.g;

    // Third texture coordinate is the NORMALISED BAND HEIGHT, not world Y (as the
    // reference does). World Y spans only ~0.36 of a texture period across a 4.5 km
    // deck, so the volume barely varied vertically; height fraction maps the deck
    // onto exactly one period and the clouds get real vertical structure.
    vec2  uvh = pw.xz * CL.scales.x;
    vec4  sn  = textureLod(uCloudShape, vec3(uvh, h), lod);
    float fbm = dot(sn.gba, vec3(0.625, 0.25, 0.125));
    float base = nc_remap(sn.r, fbm - 1.0, 1.0, 0.0, 1.0);

    // Gradient lift (reference: density/heightFraction). Dividing by the height
    // fraction pushes the low band toward saturation instead of leaving base hovering
    // just under the coverage threshold — this is the other half of why nothing was
    // surviving. Clamped denominator so the very bottom does not blow up.
    base = clamp(base * cl_heightGradient(h, type) / max(h, 0.15), 0.0, 1.0);

    // Coverage as a RESCALE, not a multiply: multiplying fades whole clouds out
    // uniformly (they go translucent), rescaling shrinks them while keeping the
    // survivors solid — which is how real coverage works.
    float d = nc_remap(base, 1.0 - coverage, 1.0, 0.0, 1.0);
    if (d <= 0.0) return 0.0;

    if (!cheap) {
        vec3  dn  = textureLod(uCloudDetail, vec3(pw.xz * CL.scales.y, h), 0.0).rgb;
        float det = dot(dn, vec3(0.625, 0.25, 0.125));
        // Wispy, torn bottoms; billowy cauliflower tops.
        det = mix(det, 1.0 - det, clamp(h * 10.0, 0.0, 1.0));
        d = nc_remap(d * 2.0, det * CL.shape.y, 1.0, 0.0, 1.0);
    }
    return clamp(d, 0.0, 1.0) * CL.shape.z;
}

// Henyey-Greenstein.
float cl_hg(float mu, float g)
{
    float gg = g * g;
    return (1.0 - gg) / (4.0 * 3.14159265 * pow(1.0 + gg - 2.0 * g * mu, 1.5));
}

// Dual-lobe phase: a strong FORWARD lobe (the silver lining when you look toward the
// sun through a thin edge) plus a weaker BACK lobe (the glow when the sun is behind
// you). A single lobe misses one of the two effects, and both are things people
// recognise in a real sky even if they cannot name them.
float cl_phase(float mu)
{
    return mix(cl_hg(mu, CL.light.x), cl_hg(mu, -CL.light.y), 0.5);
}

// Fixed low-discrepancy directions for the light-march CONE. From Federico Vaccaro's
// OpenGL-VolumetricClouds (MIT, (c) 2018), which took them from the Nubis lineage.
const vec3 CL_CONE_KERNEL[6] = vec3[6](
    vec3( 0.38051305,  0.92453449, -0.02111345),
    vec3(-0.50625799, -0.03590792, -0.86163418),
    vec3(-0.32509218, -0.94557439,  0.01428793),
    vec3( 0.09026238, -0.27376545,  0.95755165),
    vec3( 0.28128598,  0.42443639, -0.86065785),
    vec3(-0.16852403,  0.14748697,  0.97460106));

// Optical depth toward the sun, sampled inside a WIDENING CONE rather than along a
// straight line. Light arriving at a point in a cloud has not travelled one ray — it
// has scattered in from a solid angle around the sun direction, and the further back
// you look the wider that region is. A straight march therefore reads as hard,
// stripey self-shadowing; spreading the samples into a cone that opens with distance
// gives the soft, rounded shading real cumulus have, for the same six samples.
//
// The cone offsets are scaled by the step length (an adaptation — the reference uses
// fixed world units, which only holds at its particular cloud scale; ours are
// cvar-driven so the cone has to follow them).
float cl_lightMarch(vec3 p, vec3 toSun)
{
    const int kSteps = 6;
    const float kConeGrow = 1.0 / 6.0;

    float stepLen = (CL.band.y - CL.band.x) / float(kSteps) * 0.55;
    float coneR   = 1.0;
    float tau = 0.0;
    vec3  q = p;

    for (int i = 0; i < kSteps; ++i) {
        vec3 s = q + CL_CONE_KERNEL[i] * (coneR * float(i) * stepLen * 0.5);
        // Cheap sampling until enough density has accumulated to matter — the
        // reference's `density > 0.3` trick, kept: detail erosion is invisible in a
        // shadow lookup until the shadow is already deep.
        // ALWAYS the cheap sample here. High-frequency erosion is invisible in a
        // shadow lookup, and this runs 6x per dense primary step — it was the single
        // largest cost in the march.
        tau += cl_density(s, 1.0, true) * stepLen;
        q   += toSun * stepLen;
        coneR   += kConeGrow;
        stepLen *= 1.35;   // grow: distant occlusion needs no precision
    }
    return tau;
}

// Beer + powder. Beer alone makes cloud edges BRIGHTER than their cores, which is
// backwards for the side facing the sun — real cloud edges are darker there because
// less multiple scattering reaches them. Powder restores that.
vec3 cl_lightEnergy(float tau, float density, float mu)
{
    // Multiple-scattering approximation (Wrenninge/Hillaire): sum a few octaves with
    // progressively lower extinction. Single scattering alone makes thick cloud read
    // as flat black instead of the luminous grey a real storm cell has.
    float e = 0.0;
    float a = 1.0, b = 1.0, c = 1.0;
    for (int i = 0; i < 3; ++i) {
        e += a * exp(-tau * CL.light.z * b) * cl_phase(mu * c);
        a *= 0.5; b *= 0.5; c *= 0.8;
    }
    float powder = 1.0 - exp(-density * 2.0);
    return vec3(e * mix(1.0, powder, CL.light.w));
}

// Raymarch the cloud band along `dir`. Returns rgb = scattered light, a = coverage
// (1 - transmittance) so the caller can composite over the sky.
vec4 CloudsRaymarch(vec3 dir, vec3 toSun, float eyeAlt, float dither)
{
    if (CL.shape.x <= 0.001) return vec4(0.0);

    vec3 ro = vec3(0.0, ATMO_R_PLANET + max(eyeAlt, 1.0), 0.0);

    // Entry/exit of the shell band.
    vec2 hitB = atmoRaySphere(ro, dir, ATMO_R_PLANET + CL.band.x);
    vec2 hitT = atmoRaySphere(ro, dir, ATMO_R_PLANET + CL.band.y);
    float tStart, tEnd;
    if (eyeAlt < CL.band.x) {
        // Below the deck (the normal case): enter at the bottom shell, leave at the top.
        if (hitB.y <= 0.0) return vec4(0.0);       // ray never reaches the deck
        tStart = hitB.y;
        tEnd   = hitT.y;
    } else if (eyeAlt > CL.band.y) {
        if (hitT.y <= 0.0) return vec4(0.0);
        tStart = hitT.x; tEnd = (hitB.x > 0.0) ? hitB.x : hitT.y;
    } else {
        tStart = 0.0;
        tEnd   = (hitB.y > 0.0) ? hitB.x : hitT.y;
    }
    if (tEnd <= tStart) return vec4(0.0);

    // EARLY-OUT on horizon haze, before marching anything. Rays near the horizon are
    // simultaneously the most expensive (the shell chord runs for hundreds of km) and
    // the least visible (they are seen through so much atmosphere that the result is
    // pure haze). Computing the fade first and bailing turns the worst pixels on
    // screen into a handful of instructions. Without this a fullscreen sky — the main
    // menu, for instance — marches every one of ~4M pixels at full depth.
    float fog = 1.0 - exp(-tStart * 0.00002 * (tStart / max(CL.band.x * 3.0, 1.0)));
    if (fog > 0.97) return vec4(0.0);

    // Cap the marched distance: near the horizon the shell chord runs for hundreds of
    // km, and marching it is both ruinous and pointless (it is all haze by then).
    tEnd = min(tEnd, tStart + CL.march.z);

    // Scale the step budget by how much of this ray will survive the haze. A pixel
    // that ends up 80% hazed cannot show the detail a full-budget march would buy, so
    // spending it there is pure waste — and those are precisely the long, expensive
    // low-elevation rays.
    int   steps   = int(CL.march.x * mix(0.35, 1.0, 1.0 - fog));
    steps = max(steps, 16);
    float stepLen = (tEnd - tStart) / float(steps);
    // Dither the start by a fraction of a step. Without this the fixed sample planes
    // print as concentric rings across the sky — the classic raymarch banding.
    float t = tStart + stepLen * dither;

    float mu = dot(dir, toSun);

    vec3  sunCol = SunTransmittance(toSun, CL.atmo.z, 0.0) * CL.light2.x;
    // Ambient: sky above (blue/bright) and the darker light returning from below.
    vec3  ambTop = AtmosphereRadiance(vec3(0.0, 1.0, 0.0), toSun, CL.atmo.y, CL.atmo.w, CL.atmo.z, 0.0);
    vec3  ambBot = ambTop * 0.35;

    vec3  scattered = vec3(0.0);
    float transmit  = 1.0;
    float ext       = CL.light.z;

    // Two-speed march: stride cheaply through empty sky, drop to fine steps on
    // contact with density, and back off again after a run of empties. A uniform fine
    // march would spend nearly all its samples on nothing.
    int   emptyRun = 0;
    float bigStep  = stepLen * 4.0;
    bool  coarse   = true;

    for (int i = 0; i < 256; ++i) {
        // Bail at 0.05 rather than 0.01: below that the remaining contribution is
        // under half a percent of the pixel, and the tail is exactly where the march
        // is deepest inside cloud (so each step costs a full light march).
        if (i >= steps || t > tEnd || transmit < 0.05) break;
        vec3 p = ro + dir * t;

        if (coarse) {
            float dc = cl_density(p, 1.0, true);
            if (dc > 0.0) { coarse = false; t -= bigStep; emptyRun = 0; continue; }
            t += bigStep;
            continue;
        }

        // Detail erosion only where it can be resolved. Past ~15 km a detail texel is
        // far smaller than a pixel, so the fetch buys nothing but aliasing — and the
        // horizon half of the sky is all beyond that.
        float d = cl_density(p, 0.0, t > 15000.0);
        if (d <= 0.0) {
            if (++emptyRun > 8) { coarse = true; }
            t += stepLen;
            continue;
        }
        emptyRun = 0;

        float tau = cl_lightMarch(p, toSun);
        float h   = cl_heightFraction(p);

        vec3 lum = sunCol * cl_lightEnergy(tau, d, mu)
                 + mix(ambBot, ambTop, h) * CL.light2.y;

        // Energy-conserving integration of the segment (Hillaire): integrate the
        // scattering analytically across the step instead of a point sample x length.
        // The point-sample form makes result depend on step count, so quality settings
        // change brightness — the exact trap the fog arc documented.
        float sigma = d * ext;
        float clampSigma = max(sigma, 1e-5);
        float stepTrans = exp(-clampSigma * stepLen);
        vec3  integ = (lum * d - lum * d * stepTrans) / clampSigma;
        scattered += transmit * integ;
        transmit  *= stepTrans;

        t += stepLen;
    }

    // Horizon haze fade (computed above, and used there to skip the march entirely
    // when it would be fully hazed out). Cloud near the horizon is seen through an
    // enormous slab of atmosphere and dissolves into it; without this the deck stays
    // crisp right down to the skyline and reads as a painted backdrop.
    // Falloff after Vaccaro's computeFogAmount (MIT, (c) 2018).
    float alpha = (1.0 - clamp(transmit, 0.0, 1.0)) * (1.0 - clamp(fog, 0.0, 1.0));

    return vec4(scattered, alpha);
}

// High CIRRUS: a cheap 2D layer on its own, much higher dome. Volumetric detail is
// wasted on ice sheets at 9 km — but the PARALLAX between this and the volumetric
// deck below is a large part of what makes a sky read as having depth, so it earns
// its place. Reuses the existing scrolling cloud texture.
vec4 CloudsCirrus(vec3 dir, vec3 toSun, float dither)
{
    if (CL.cirrus.x <= 0.001 || dir.y <= 0.02) return vec4(0.0);

    vec3 ro = vec3(0.0, ATMO_R_PLANET, 0.0);
    vec2 hit = atmoRaySphere(ro, dir, ATMO_R_PLANET + CL.cirrus.y);
    if (hit.y <= 0.0) return vec4(0.0);
    vec3 p = ro + dir * hit.y;

    vec2 wind = CL.wind.xy * CL.wind.z * CL.timeP.x * 1.8;   // high winds run faster
    vec2 uv   = (p.xz + wind) * CL.cirrus.z;

    float a = texture(uClouds0, uv).a;
    a += texture(uClouds1, uv * 2.3 + vec2(0.37, 0.11)).a * 0.5;
    a = clamp((a - 0.35) * 1.9, 0.0, 1.0) * CL.cirrus.x;
    // Fade into the horizon haze — a hard cirrus edge at the horizon looks pasted on.
    a *= smoothstep(0.02, 0.25, dir.y);

    vec3 col = SunTransmittance(toSun, CL.atmo.z, 0.0) * CL.light2.x
             * mix(0.5, 1.4, clamp(cl_phase(dot(dir, toSun)) * 6.0, 0.0, 1.0));
    col += AtmosphereRadiance(vec3(0.0, 1.0, 0.0), toSun, CL.atmo.y, CL.atmo.w, CL.atmo.z, 0.0) * CL.light2.y;

    return vec4(col, a);
}

#endif // CLOUDS_GLSL

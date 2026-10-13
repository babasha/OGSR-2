// xrRenderVulkan - physically-based sky: single-scattering Rayleigh + Mie.
//
// WHY THIS EXISTS (measured, 2026-07-20). At a late sunset the weather config hands
// the renderer this:
//     sun_color=(0.009,0.004,0.002)  hemi=(0.437,0.432,0.419)
//     ambient=(0.020,0.020,0.020)    sky_color=(0.944,0.944,0.944)  sun elev=+1.4°
// Not one warm value, not one directional value — everything neutral grey, and the
// sun zeroed while it is still visually above the horizon. X-Ray zeroes it there
// because the classic renderer had no way to do twilight. So no amount of correct
// receiver-side maths can make dusk look like dusk: the LIGHT ITSELF isn't in the
// data. Terrain came out uniformly grey because the inputs were uniformly grey.
//
// This model takes only GEOMETRY (sun direction, which the config gets right) and
// derives radiance from physics. Sunset then falls out on its own: at low elevation
// the sun's path through the atmosphere is ~40x longer, Rayleigh scatters the short
// wavelengths out of it (leaving red/orange), and the Mie lobe piles a warm halo
// around the sun while the opposite sky stays blue. That is real azimuthal structure
// — which is exactly what the SH probe was measuring as absent (aniso 0.107, and its
// direction pointing straight up).
//
// Because the SAME function feeds the dome draw AND the light probe
// (ibl_prefilter.comp), the sky the player sees and the light the world receives are
// the same thing by construction. That is the invariant this whole arc kept breaking:
// first the half-cube remap was missing from the probe, then sky_rotation, then
// sky_color. Deriving both from one function ends that class of bug.
//
// Model: the standard compact single-scattering integral (O'Neil / Nishita lineage,
// as popularised by wwwtyro/glsl-atmosphere). Primary ray marched through an
// exponential atmosphere; at each step a secondary march toward the sun gives the
// optical depth light travelled to get there. Single scattering only — no multiple
// scattering, so deep twilight is darker than reality, which the intensity knob and
// the night floor absorb.
#ifndef ATMOSPHERE_GLSL
#define ATMOSPHERE_GLSL

// Earth-scale defaults. Metres.
const float ATMO_R_PLANET = 6371000.0;
const float ATMO_R_ATMOS  = 6471000.0;   // +100 km
// Scattering coefficients at sea level (1/m). Rayleigh is wavelength^-4: blue
// scatters ~5x more than red, which is why the sky is blue and the low sun is red.
const vec3  ATMO_BETA_R   = vec3(5.5e-6, 13.0e-6, 22.4e-6);
const float ATMO_BETA_M   = 21e-6;       // Mie: aerosols, wavelength-neutral, forward-peaked
const float ATMO_H_R      = 8000.0;      // Rayleigh scale height
const float ATMO_H_M      = 1200.0;      // Mie scale height (haze hugs the ground)

// Ray vs sphere centred at the origin; returns (near, far), or (1e9,-1e9) on miss.
vec2 atmoRaySphere(vec3 ro, vec3 rd, float r)
{
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r * r;
    float d = b * b - c;
    if (d < 0.0) return vec2(1e9, -1e9);
    d = sqrt(d);
    return vec2(-b - d, -b + d);
}

// Sky radiance for a view direction.
//   rayDir      : world view direction (unit, Y-up)
//   sunDir      : world direction TO the sun (unit; = -env sun_dir, which travels down)
//   intensity   : overall scale (r_sky_intensity) — the model is in physical-ish
//                 units, this maps it onto the game's exposure
//   mieG        : Mie anisotropy (~0.76 = strong forward halo)
//   turbidity   : aerosol multiplier on Mie (1 = clean air, >1 = hazy/dusty)
//   altitude    : observer height above sea level (m)
vec3 AtmosphereRadiance(vec3 rayDir, vec3 sunDir, float intensity,
                        float mieG, float turbidity, float altitude)
{
    const int iSteps = 16;   // primary march
    const int jSteps = 8;    // light march

    vec3 ro = vec3(0.0, ATMO_R_PLANET + max(altitude, 1.0), 0.0);

    vec2 hit = atmoRaySphere(ro, rayDir, ATMO_R_ATMOS);
    if (hit.y <= hit.x) return vec3(0.0);
    // Stop at the planet if the ray points into the ground: below the horizon there
    // is no sky to integrate, and marching through the planet produces a bright
    // nonsense band right where the horizon line should be.
    vec2 ground = atmoRaySphere(ro, rayDir, ATMO_R_PLANET);
    float tMax = hit.y;
    if (ground.y > 0.0 && ground.x > 0.0) tMax = min(tMax, ground.x);
    float tMin = max(hit.x, 0.0);

    float betaM = ATMO_BETA_M * max(turbidity, 0.0);

    float stepP = (tMax - tMin) / float(iSteps);
    float tP    = tMin + stepP * 0.5;

    vec3  sumR = vec3(0.0);
    vec3  sumM = vec3(0.0);
    float odR  = 0.0;   // accumulated optical depth along the primary ray
    float odM  = 0.0;

    for (int i = 0; i < iSteps; ++i) {
        vec3  p = ro + rayDir * tP;
        float h = length(p) - ATMO_R_PLANET;

        float hR = exp(-h / ATMO_H_R) * stepP;
        float hM = exp(-h / ATMO_H_M) * stepP;
        odR += hR;
        odM += hM;

        // Optical depth from this sample toward the sun.
        vec2  lh = atmoRaySphere(p, sunDir, ATMO_R_ATMOS);
        float stepL = lh.y / float(jSteps);
        float tL = stepL * 0.5;
        float odRL = 0.0, odML = 0.0;
        bool  shadowed = false;

        for (int j = 0; j < jSteps; ++j) {
            vec3  q  = p + sunDir * tL;
            float hL = length(q) - ATMO_R_PLANET;
            if (hL < 0.0) { shadowed = true; break; }   // the planet itself blocks the light
            odRL += exp(-hL / ATMO_H_R) * stepL;
            odML += exp(-hL / ATMO_H_M) * stepL;
            tL   += stepL;
        }

        if (!shadowed) {
            // Transmittance sun -> sample -> eye.
            vec3 tau = ATMO_BETA_R * (odR + odRL) + vec3(betaM) * 1.1 * (odM + odML);
            vec3 att = exp(-tau);
            sumR += att * hR;
            sumM += att * hM;
        }
        tP += stepP;
    }

    float mu  = dot(rayDir, sunDir);
    float g   = clamp(mieG, 0.0, 0.99);
    float gg  = g * g;
    // Rayleigh phase 3/(16pi) (1 + cos^2)
    float pR  = 3.0 / (16.0 * 3.14159265) * (1.0 + mu * mu);
    // Henyey-Greenstein-ish Mie phase (Cornette-Shanks form): the tight forward lobe
    // that puts the warm aureole around a low sun.
    float pM  = 3.0 / (8.0 * 3.14159265) * ((1.0 - gg) * (1.0 + mu * mu))
              / ((2.0 + gg) * pow(1.0 + gg - 2.0 * g * mu, 1.5));

    return intensity * (sumR * ATMO_BETA_R * pR + sumM * betaM * pM);
}

// Transmittance of sunlight reaching the observer — i.e. the SUN'S OWN COLOUR after
// the atmosphere has eaten it. This is what makes a low sun red, and it is derived
// from elevation alone, so it stays correct at the hours where the weather config
// simply gives up and writes zero.
vec3 SunTransmittance(vec3 sunDir, float turbidity, float altitude)
{
    const int jSteps = 16;
    vec3 ro = vec3(0.0, ATMO_R_PLANET + max(altitude, 1.0), 0.0);

    vec2 lh = atmoRaySphere(ro, sunDir, ATMO_R_ATMOS);
    if (lh.y <= 0.0) return vec3(0.0);

    // Sun below the geometric horizon: the direct beam is blocked by the planet.
    vec2 g = atmoRaySphere(ro, sunDir, ATMO_R_PLANET);
    if (g.y > 0.0 && g.x > 0.0) return vec3(0.0);

    float stepL = lh.y / float(jSteps);
    float tL = stepL * 0.5;
    float odR = 0.0, odM = 0.0;
    for (int j = 0; j < jSteps; ++j) {
        vec3  q  = ro + sunDir * tL;
        float hL = max(length(q) - ATMO_R_PLANET, 0.0);
        odR += exp(-hL / ATMO_H_R) * stepL;
        odM += exp(-hL / ATMO_H_M) * stepL;
        tL  += stepL;
    }
    return exp(-(ATMO_BETA_R * odR + vec3(ATMO_BETA_M * max(turbidity, 0.0)) * 1.1 * odM));
}

#endif // ATMOSPHERE_GLSL

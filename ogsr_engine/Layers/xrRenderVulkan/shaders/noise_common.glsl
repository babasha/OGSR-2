// xrRenderVulkan - tileable Perlin / Worley noise for the volumetric cloud bake.
//
// Everything here is TILEABLE by construction: the cloud raymarch samples these
// volumes with REPEAT wrapping over kilometres of sky, so a seam would print a
// visible grid across the whole cloudscape. Worley tiles by wrapping the cell
// lattice; Perlin tiles by wrapping the gradient lattice. Both take the period as
// an argument — never hardcode it, the shape and detail volumes use different ones.
#ifndef NOISE_COMMON_GLSL
#define NOISE_COMMON_GLSL

vec3 nc_hash33(vec3 p)
{
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453123);
}

float nc_hash13(vec3 p)
{
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}

// Worley (cellular) noise, INVERTED so 1 = cell centre (billowy blob) and 0 = cell
// edge. That polarity is what makes it read as cauliflower cumulus rather than as a
// net of cracks.
float nc_worley(vec3 p, float period)
{
    p *= period;
    vec3 i = floor(p);
    vec3 f = p - i;
    float minDist = 1e9;
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        vec3 off  = vec3(x, y, z);
        vec3 cell = i + off;
        cell = mod(cell + period, period);          // wrap -> tileable
        vec3 pt = off + nc_hash33(cell) - f;
        minDist = min(minDist, dot(pt, pt));
    }
    return 1.0 - clamp(sqrt(minDist), 0.0, 1.0);
}

// Multi-octave Worley: adds the smaller-scale bumps that erode a smooth blob into
// something that reads as cloud at close range.
float nc_worleyFbm(vec3 p, float period)
{
    return nc_worley(p, period)        * 0.625
         + nc_worley(p, period * 2.0)  * 0.25
         + nc_worley(p, period * 4.0)  * 0.125;
}

vec3 nc_grad(vec3 cell)
{
    return normalize(nc_hash33(cell) * 2.0 - 1.0);
}

// Tileable Perlin.
float nc_perlin(vec3 p, float period)
{
    p *= period;
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);   // quintic smoothstep

    float n = 0.0;
    for (int z = 0; z <= 1; ++z)
    for (int y = 0; y <= 1; ++y)
    for (int x = 0; x <= 1; ++x) {
        vec3 off  = vec3(x, y, z);
        vec3 cell = mod(i + off + period, period);
        float d   = dot(nc_grad(cell), f - off);
        float w   = mix(1.0 - u.x, u.x, off.x)
                  * mix(1.0 - u.y, u.y, off.y)
                  * mix(1.0 - u.z, u.z, off.z);
        n += d * w;
    }
    return n * 0.5 + 0.5;
}

float nc_perlinFbm(vec3 p, float period, int octaves)
{
    float sum = 0.0, amp = 0.5, per = period;
    for (int i = 0; i < octaves; ++i) {
        sum += nc_perlin(p, per) * amp;
        per *= 2.0;
        amp *= 0.5;
    }
    return sum;
}

// Remap a value from one range to another — the workhorse of the Nubis cloud model,
// where coverage and erosion are both expressed as "rescale what's left".
float nc_remap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (clamp(v, lo, hi) - lo) / max(hi - lo, 1e-5) * (nhi - nlo);
}

// Perlin-Worley: Perlin's connected, wispy structure with Worley's billows dilated
// into it. This is the base shape channel (Schneider / Nubis).
float nc_perlinWorley(vec3 p, float period)
{
    float pn = nc_perlinFbm(p, period, 4);
    float wf = nc_worleyFbm(p, period);
    return nc_remap(pn, wf - 1.0, 1.0, 0.0, 1.0);
}

#endif // NOISE_COMMON_GLSL

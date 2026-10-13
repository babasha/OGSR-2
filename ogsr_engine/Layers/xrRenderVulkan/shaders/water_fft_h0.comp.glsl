#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — TESSENDORF STAGE 1: the INITIAL SPECTRUM h0(k).
//
// Run once, and again only when the wind or the tuning changes — it is the
// expensive-looking one (a pow, an exp and two hashes per texel) and the one
// thing in the chain that does not depend on time.
//
//   h0(k) = (1/sqrt2) (xi_r + i xi_i) sqrt(P(k))
//
// with P the Phillips spectrum: energy concentrated around the wavelength the
// wind can actually raise (V^2/g), falling off as k^-4 above it, cut off below a
// few centimetres, and weighted by how well the wave lines up with the wind.
//
// ⚠ THE SECOND HALF OF THE OUTPUT IS NOT AN INDEPENDENT DRAW. The evolution step
// needs conj(h0(-k)), and it must be the SAME random numbers the -k texel will
// draw for itself, or the spectrum stops being Hermitian. That is not a cosmetic
// concern: the whole reason two real fields can share one complex FFT downstream
// is that a Hermitian spectrum transforms to a real field. Break the symmetry and
// the height leaks into the displacement channel as noise. Hence `mir`.
#include "water_fft_common.glsl"

layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0, rgba32f) uniform writeonly image2DArray uH0;  // xy = h0(k), zw = conj(h0(-k))

// ⚠ SCALARS ONLY. A vec2 in a push block is std430-aligned to 8 bytes and shifts
// every field behind it relative to the C++ struct — this renderer has lost a
// whole feature to exactly that, silently, for days. Two floats cost nothing.
layout(push_constant) uniform PC {
    float base;        // largest cascade's tile size, metres
    float ratio;       // per-cascade shrink factor
    float windX;
    float windZ;       // wind VELOCITY (m/s), not a direction — its length sets the spectrum peak
    float amp;         // Phillips A
    float smallCut;    // l: waves shorter than this are killed (m)
    float dirPow;      // how sharply the spectrum favours the wind direction
    float seed;
    int   cascades;
} pc;

// Hash → uniform [0,1). Deliberately a plain integer-free float hash: the same
// texel must produce the same number on every GPU, or a cascade re-generated
// mid-session would visibly jump.
float wfHash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Box-Muller: two independent unit normals from two uniforms.
vec2 wfGauss(vec3 s)
{
    float u1 = max(wfHash(s), 1e-6);
    float u2 = wfHash(s + 17.317);
    float r  = sqrt(-2.0 * log(u1));
    float th = WFFT_TAU * u2;
    return vec2(r * cos(th), r * sin(th));
}

float phillips(vec2 k, vec2 w, float V)
{
    float kk = dot(k, k);
    if (kk < 1e-12) return 0.0;
    float km = sqrt(kk);
    float L  = V * V / WFFT_G;                    // longest wave this wind can raise
    float kL = km * L;
    float p  = pc.amp * exp(-1.0 / (kL * kL)) / (kk * kk);
    float d  = dot(k / km, w / V);
    // Waves running ACROSS the wind are weak, waves running INTO it weaker still
    // — but not zero. Folding them to zero (the |cos|^n form on its own) makes the
    // field mirror-symmetric about the wind axis, which reads as a corduroy of
    // parallel ridges instead of a sea.
    p *= pow(abs(d), pc.dirPow) * (d < 0.0 ? 0.12 : 1.0);
    p *= exp(-kk * pc.smallCut * pc.smallCut);    // capillary cutoff
    return p;
}

void main()
{
    ivec3 id = ivec3(gl_GlobalInvocationID);
    if (id.x >= WFFT_N || id.y >= WFFT_N || id.z >= pc.cascades) return;

    float L  = wfftLen(pc.base, pc.ratio, id.z);
    // Index 0..N-1 stands for wave numbers -N/2 .. N/2-1.
    vec2  nm = vec2(id.xy) - float(WFFT_N) * 0.5;
    vec2  k  = WFFT_TAU * nm / L;
    float km = length(k);

    vec4 outv = vec4(0.0);
    vec2 w    = vec2(pc.windX, pc.windZ);
    float V   = length(w);

    float kMin = wfftKMin(pc.base, pc.ratio, id.z);
    float kMax = wfftKMax(pc.base, pc.ratio, id.z, pc.cascades);
    // 0.999 because the cascade's own fundamental IS kMin and floating point
    // should not be allowed to drop the longest wave in the tile.
    if (V > 0.05 && km >= kMin * 0.999 && km < kMax) {
        // The mirrored texel — the one that will call itself +k when it runs.
        ivec2 mir = ivec2((WFFT_N - id.x) & WFFT_MASK, (WFFT_N - id.y) & WFFT_MASK);
        float ofs = pc.seed + float(id.z) * 91.73;
        vec2  gP  = wfGauss(vec3(vec2(id.xy), ofs));
        vec2  gM  = wfGauss(vec3(vec2(mir),   ofs));
        vec2  h0p = gP * sqrt(phillips( k, w, V) * 0.5);
        vec2  h0m = gM * sqrt(phillips(-k, w, V) * 0.5);
        outv = vec4(h0p, cConj(h0m));
    }
    imageStore(uH0, id, outv);
}

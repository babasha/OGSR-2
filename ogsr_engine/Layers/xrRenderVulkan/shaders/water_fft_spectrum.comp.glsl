#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — TESSENDORF STAGE 2: evolve h0 to time t and pack for the FFT.
//
//   h(k,t) = h0(k) e^{i w t} + conj(h0(-k)) e^{-i w t}
//
// with the dispersion w(k) = sqrt(g k tanh(k D)). The tanh is what makes long
// waves in shallow water slow down — at D = 60 m it is 1.0 for everything the
// two fine cascades carry and only bends the longest swell, which is exactly the
// right amount of physics for the price.
//
// ⭐ w IS QUANTISED to a multiple of 2*pi/T. Every wave's period then divides T,
// so the ENTIRE field repeats exactly every T seconds and nothing drifts out of
// phase over a long session. Costs one floor().
//
// ⭐ TWO REAL FIELDS PER COMPLEX FFT. h, Dx and Dz are three real fields, so a
// naive implementation runs three complex transforms and throws half of each
// away. Their spectra are Hermitian, and the inverse transform of (F + iG) for
// Hermitian F and G is exactly f + i g — so one transform carries two fields.
// Layer 2c holds h + i*Dx and comes back as (h, Dx) in one texel; layer 2c+1
// holds Dz. That is two transforms per cascade instead of three, and the reason
// stage 1 had to be careful about the Hermitian symmetry.
#include "water_fft_common.glsl"

layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0, rgba32f) uniform readonly  image2DArray uH0;
layout(set = 0, binding = 1, rg32f)   uniform writeonly image2DArray uSpec;

layout(push_constant) uniform PC {      // ⚠ scalars only — see water_fft_h0
    float base;
    float ratio;
    float time;        // seconds
    float repeat;      // the field loops exactly every this many seconds
    float depth;       // water depth for the dispersion relation (m)
    int   cascades;
} pc;

void main()
{
    ivec3 id = ivec3(gl_GlobalInvocationID);
    if (id.x >= WFFT_N || id.y >= WFFT_N || id.z >= pc.cascades) return;

    int   c  = id.z;
    float L  = wfftLen(pc.base, pc.ratio, c);
    vec2  nm = vec2(id.xy) - float(WFFT_N) * 0.5;
    vec2  k  = WFFT_TAU * nm / L;
    float km = length(k);

    vec4 h0  = imageLoad(uH0, id);
    vec2 h0p = h0.xy;                    // h0(k)
    vec2 h0m = h0.zw;                    // conj(h0(-k)), already conjugated at generation

    // Dispersion. The tanh argument is clamped: at k*D > ~20 it is 1.0 to within
    // float precision and the exp() inside starts to overflow for the fine cascade.
    float w  = sqrt(WFFT_G * km * tanh(clamp(km * pc.depth, 0.001, 20.0)));
    float w0 = WFFT_TAU / max(pc.repeat, 1.0);
    w = floor(w / w0) * w0;

    vec2 e = cExp(w * pc.time);
    vec2 h = cMul(h0p, e) + cMul(h0m, cConj(e));

    // HORIZONTAL DISPLACEMENT. D(k) = -i * (k/|k|) * h(k): the crest pulls water
    // in from both flanks, which sharpens the peak and flattens the trough. This
    // is the entire visual difference from the analytic model.
    vec2 kn = (km > 1e-6) ? k / km : vec2(0.0);
    vec2 dx = cMul(vec2(0.0, -kn.x), h);
    vec2 dz = cMul(vec2(0.0, -kn.y), h);

    // Pack h + i*Dx. Multiplying a complex number by i is (x,y) -> (-y,x), so the
    // sum is (h.x - dx.y, h.y + dx.x).
    imageStore(uSpec, ivec3(id.xy, 2 * c),     vec4(h.x - dx.y, h.y + dx.x, 0.0, 0.0));
    // Dz alone. The imaginary half of this one is spare — the slopes and the
    // Jacobian come out of finite differences in the assemble stage instead,
    // which the band limiting makes accurate enough to not be worth a third
    // transform: a cascade's SHORTEST kept wave is the next cascade's tile size,
    // so it still spans N*ratio (~45) of this cascade's own texels. Differencing
    // a wave sampled 45 times per period is not where the error is.
    imageStore(uSpec, ivec3(id.xy, 2 * c + 1), vec4(dz, 0.0, 0.0));
}

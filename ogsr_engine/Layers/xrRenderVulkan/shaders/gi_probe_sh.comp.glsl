#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — step 6a: project every DDGI probe's octahedral irradiance map
// into TWO-BAND SH, once per frame, into a small buffer.
//
// WHY THIS DISPATCH EXISTS AT ALL (it is an optimisation, and a large one)
// -----------------------------------------------------------------------
// The froxel pass below it needs a DIRECTIONAL answer per froxel, and the only
// directional thing a probe owns is its 6x6 octahedral map. Reading that map
// per froxel means six taps per probe x eight probes = 48 texture reads for one
// froxel, and there are ~590k of them: ~28M taps a frame to resample a field that
// changes at 1 metre.
//
// Probes are the small end of that product. There are 16x8x16 x 5 cascades =
// 10240 of them, each 36 texels, so projecting ALL of them costs 0.37M taps —
// two orders of magnitude less than doing it lazily at the consumer, and it is
// also strictly MORE accurate: the projection sees the probe's whole distribution
// instead of six point samples of it.
//
// After this, a froxel's job is eight buffer reads of 32 bytes and eight distance
// taps — the visibility test, which cannot be precomputed because it depends on
// where the froxel is.
//
// ⚠ ONE THREAD PER PROBE, ACROSS ALL CASCADES. Not just the cascade the trace
// updated this frame: the froxel field is resampled from scratch every frame at a
// new camera position, so every cascade it might read has to be current in this
// buffer. Projecting the other four costs nothing (see above) and skipping them
// would leave a froxel that crossed a cascade boundary reading a projection from
// however many frames ago the round-robin last visited it.

#define DDGI_SET      0
#define DDGI_B_CONSTS 0
#include "ddgi_common.glsl"
#include "gi_sh.glsl"

// ⚠ THE BINDING NUMBERS BELONG TO THE FROXEL PASS, NOT TO THIS ONE. Both
// dispatches share ONE descriptor set layout (see vk_gi_froxel.cpp); this shader
// simply does not declare bindings 2 and 4..6, which is legal — a bound descriptor
// that no shader names is fine, and one layout means one set, written once per
// frame, that cannot drift away from its twin.
layout(set = 0, binding = 1) uniform sampler2D uIrr;   // the probe irradiance atlas
layout(std430, set = 0, binding = 3) writeonly buffer ProbeSH { GiProbeSH sh[]; };

layout(local_size_x = 64) in;

const int N    = DDGI_IRR_N;       // 6 interior texels per side
const int TILE = DDGI_IRR_N + 2;   // 8 with the border

void main()
{
    const int probe  = int(gl_GlobalInvocationID.x);
    const int probes = ddgi.casc.x * ddgi.casc.y;      // cascades x probes per cascade
    if (probe >= probes) return;

    const int   ppr      = ddgi.layout_.x;
    const ivec2 tileBase = ivec2(probe % ppr, probe / ppr) * TILE + ivec2(1);   // +1 skips the border

    // Band 0 and band 1 of the STORED function E(N) = c + (2/3)(d.N).
    //
    // ⚠ The texels are weighted EQUALLY. The octahedral map is not exactly
    // area-preserving (a corner texel subtends ~1.3x a centre one), so this is a
    // few-percent bias on a quantity the probes themselves resolve at metres —
    // deliberately not worth a per-texel solid angle, and stated here so nobody
    // rediscovers it as a bug. It is a BIAS, not noise: it cannot flicker.
    vec3  e0 = vec3(0.0);
    vec3  e1 = vec3(0.0);            // luminance-weighted direction, see gi_sh.glsl
    for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
        const vec3  e   = texelFetch(uIrr, tileBase + ivec2(x, y), 0).rgb;
        const vec3  dir = ddgiTexelDir(ivec2(x, y), N);
        e0 += e;
        e1 += dot(e, vec3(0.2126, 0.7152, 0.0722)) * dir;
    }
    const float inv = 1.0 / float(N * N);
    const vec3  c   = e0 * inv;
    // <E * N> = (2/9) d  =>  d = 4.5 <E * N>. The consumers never see this factor:
    // what leaves here is the RADIANCE band-1 vector, which is what a medium's
    // phase integral wants. (Derivation in gi_sh.glsl — do not re-derive it in a
    // third place.)
    const vec3  d   = (e1 * inv) * 4.5;

    sh[probe].c0 = vec4(c, 1.0);
    sh[probe].d1 = vec4(d, 0.0);
}

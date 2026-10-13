#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — TESSENDORF STAGE 3: the inverse FFT itself.
//
// One workgroup per LINE (a row on the first pass, a column on the second), all
// eight butterfly stages run inside shared memory, result written back over the
// input. A 256-point complex line is 2 KB of LDS, so the whole transform happens
// without a single round trip to memory — which is why this is a 256 tile and
// not a 512 one.
//
// In place is safe: each workgroup owns its entire line and touches nothing else.
// The two passes are separate dispatches with a barrier between them, so the
// column pass reads a fully written row pass.
//
// Decimation in time: bit-reverse on the way in, then log2(N) stages of
// increasing span. The twiddle sign is POSITIVE — this is the INVERSE transform
// (spectrum → space) and Tessendorf's h(x) = sum_k H(k) e^{ikx} carries no 1/N,
// so there is deliberately no normalisation anywhere in here.
#include "water_fft_common.glsl"

// N/2 threads, one butterfly each per stage.
layout(local_size_x = 128) in;

// Binding 1 and not 0: all four stages share ONE descriptor set layout (h0,
// spectrum, displacement, derivative), so each shader declares its resources at
// the slot that resource lives in, not at the slot that happens to be free here.
layout(set = 0, binding = 1, rg32f) uniform image2DArray uData;

layout(push_constant) uniform PC {      // ⚠ scalars only — see water_fft_h0
    int axis;      // 0 = transform rows, 1 = transform columns
} pc;

shared vec2 sh[WFFT_N];

uint bitrev8(uint x)
{
    x = ((x & 0x55u) << 1) | ((x & 0xAAu) >> 1);
    x = ((x & 0x33u) << 2) | ((x & 0xCCu) >> 2);
    x = ((x & 0x0Fu) << 4) | ((x & 0xF0u) >> 4);
    return x & 0xFFu;
}

ivec3 coord(uint i, uint line, uint layer)
{
    return (pc.axis == 0) ? ivec3(int(i), int(line), int(layer))
                          : ivec3(int(line), int(i), int(layer));
}

void main()
{
    uint line  = gl_WorkGroupID.x;
    uint layer = gl_WorkGroupID.y;
    uint t     = gl_LocalInvocationID.x;

    // Load bit-reversed: element i of the input lands at position rev(i).
    for (uint r = 0u; r < 2u; ++r) {
        uint i = t + r * 128u;
        sh[bitrev8(i)] = imageLoad(uData, coord(i, line, layer)).xy;
    }
    memoryBarrierShared();
    barrier();

    for (uint len = 2u; len <= uint(WFFT_N); len <<= 1u) {
        uint hf  = len >> 1u;
        uint blk = t / hf;              // which sub-transform this thread's pair is in
        uint j   = t % hf;              // position within it
        uint i0  = blk * len + j;
        uint i1  = i0 + hf;
        // Positive angle = inverse transform.
        float ang = WFFT_TAU * float(j) / float(len);
        vec2  tw  = vec2(cos(ang), sin(ang));
        // The (blk, j) mapping is a bijection onto disjoint PAIRS, so this thread
        // is the only one touching i0 and i1 this stage — read and write need no
        // barrier between them, only one at the end of the stage.
        vec2 a = sh[i0];
        vec2 b = cMul(sh[i1], tw);
        sh[i0] = a + b;
        sh[i1] = a - b;
        memoryBarrierShared();
        barrier();
    }

    for (uint r = 0u; r < 2u; ++r) {
        uint i = t + r * 128u;
        imageStore(uData, coord(i, line, layer), vec4(sh[i], 0.0, 0.0));
    }
}

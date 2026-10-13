#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — TESSENDORF STAGE 4: unpack the transformed tiles into the two
// textures the water shaders actually sample.
//
//   uDisp  (Dx, h, Dz, foam)   — what the VERTEX stages add to a water vertex
//   uDeriv (dh/dx, dh/dz, J,  ) — what the FRAGMENT stage shades with
//
// Three things happen here that are easy to get wrong:
//
// ⭐ THE CHECKERBOARD SIGN. Index 0..N-1 in the spectrum stands for wave numbers
//    -N/2..N/2-1, and shifting the origin like that multiplies the transform by
//    e^{-i pi (x+z)} = (-1)^(x+z). Skip it and the field comes out as a
//    high-frequency lattice of alternating spikes that looks like broken hardware.
//    It has to be applied to every sample BEFORE differencing, since neighbouring
//    texels carry OPPOSITE signs.
//
// ⭐ THE JACOBIAN. Horizontal displacement can fold the surface over itself; the
//    determinant of the displacement's Jacobian says by how much. J near 1 is
//    undisturbed water, J below 0 is water that has turned inside out — which in
//    the real thing is a wave that has broken. That is where whitewater goes, and
//    it comes out of the same maths for free. It is a far better foam signal than
//    "the vertex is high and steep", which is what the analytic path has to use.
//
// ⭐ WRAP, don't clamp. The tile is periodic in both axes by construction, so a
//    neighbour off the edge is the texel on the far side. Clamping instead would
//    put a seam of wrong slope around all four borders of every cascade.
#include "water_fft_common.glsl"

layout(local_size_x = 16, local_size_y = 16) in;

// Slots 1..3 of the shared four-binding set — see water_fft_fft.
layout(set = 0, binding = 1, rg32f)   uniform readonly  image2DArray uSpec;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2DArray uDisp;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2DArray uDeriv;

layout(push_constant) uniform PC {      // ⚠ scalars only — see water_fft_h0
    float base;
    float ratio;
    float chop;        // lambda: how far the crest pulls water in horizontally
    int   cascades;
} pc;

// (Dx, h, Dz) at a texel, sign-corrected and wrapped.
vec3 sample3(ivec2 p, int c)
{
    p &= ivec2(WFFT_MASK);
    float s = (((p.x + p.y) & 1) == 0) ? 1.0 : -1.0;
    vec2 a = imageLoad(uSpec, ivec3(p, 2 * c)).xy;        // (h, Dx)
    vec2 b = imageLoad(uSpec, ivec3(p, 2 * c + 1)).xy;    // (Dz, spare)
    return vec3(a.y, a.x, b.x) * s;
}

void main()
{
    ivec3 id = ivec3(gl_GlobalInvocationID);
    if (id.x >= WFFT_N || id.y >= WFFT_N || id.z >= pc.cascades) return;

    int   c    = id.z;
    float L    = wfftLen(pc.base, pc.ratio, c);
    float step = L / float(WFFT_N);          // metres per texel
    float inv2 = 1.0 / (2.0 * step);

    vec3 d  = sample3(id.xy,                  c);
    vec3 dR = sample3(id.xy + ivec2(1, 0),    c);
    vec3 dL = sample3(id.xy - ivec2(1, 0),    c);
    vec3 dU = sample3(id.xy + ivec2(0, 1),    c);
    vec3 dD = sample3(id.xy - ivec2(0, 1),    c);

    float hx = (dR.y - dL.y) * inv2;
    float hz = (dU.y - dD.y) * inv2;

    float jxx = 1.0 + pc.chop * (dR.x - dL.x) * inv2;
    float jzz = 1.0 + pc.chop * (dU.z - dD.z) * inv2;
    float jxz =       pc.chop * (dU.x - dD.x) * inv2;
    float jzx =       pc.chop * (dR.z - dL.z) * inv2;
    float J   = jxx * jzz - jxz * jzx;

    // Folded water only. Stretched water (J > 1) is the back of a wave and has no
    // business being white.
    float foam = clamp(1.0 - J, 0.0, 4.0);

    imageStore(uDisp,  id, vec4(d.x * pc.chop, d.y, d.z * pc.chop, foam));
    imageStore(uDeriv, id, vec4(hx, hz, J, 0.0));
}

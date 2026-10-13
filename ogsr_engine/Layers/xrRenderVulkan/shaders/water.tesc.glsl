#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — WATER tessellation control.
//
// Stock water meshes are a handful of enormous quads: the census logs 1952
// triangles over an 84 m radius on Cordon and ~110 over 40 m on the marshes,
// i.e. 4-7 METRES per triangle. There is nothing there to displace, which is
// why the wave started life as a per-pixel normal only. This stage manufactures
// the missing vertices: distance-adaptive subdivision so the near surface
// becomes real geometry and the far surface stays two triangles.
//
// Edge factors are computed symmetrically from the two endpoints of each edge,
// so neighbouring patches always agree and the seams cannot crack.
#include "water_common.glsl"   // W.p5 = tessMax / near / far / displacement gain

layout(vertices = 3) out;

layout(location = 0) in vec3 vWorldPos[];
layout(location = 1) in vec3 vNormal[];
layout(location = 2) in vec2 vUV[];

layout(location = 0) out vec3 tWorldPos[];
layout(location = 1) out vec3 tNormal[];
layout(location = 2) out vec2 tUV[];

// ⚠⚠ THE MIDPOINT IS THE WRONG POINT. Stock water is not a grid: this level's
// sheet is 138 triangles over 962 x 792 metres, so a single edge can be two
// hundred metres long — and the MIDPOINT of the edge you are standing on is a
// hundred metres away. The old metric read that as "far", handed back tess 1, and
// the surface directly under the camera was never subdivided at all. Every wave
// the tessellation was added to displace was being displaced on a mesh that had
// not been subdivided, which is why the water was a mirror with a texture on it.
//
// Two changes: measure to the CLOSEST POINT on the edge, and budget by how many
// METRES OF SURFACE one segment should cover rather than by a distance ramp — a
// factor of 24 means something quite different on a 2 m edge and a 200 m one.
float edgeFactor(vec3 a, vec3 b, vec3 eye)
{
    vec3  ab = b - a;
    float u  = clamp(dot(eye - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    float d  = distance(a + ab * u, eye);
    float len = length(ab);
    // Metres per segment we would like: fine close in, coarse out at tessFar.
    float k   = clamp((d - W.p5.y) / max(W.p5.z - W.p5.y, 0.01), 0.0, 1.0);
    float seg = mix(0.30, 7.0, k * k);
    // W.p5.x is the CEILING now (r_wtr_tess), not the factor itself. 64 is the
    // hardware limit, so a 200 m edge still cannot resolve finer than ~3 m — the
    // long swell becomes real relief, the chop stays a per-pixel normal.
    return clamp(len / max(seg, 0.05), 1.0, clamp(W.p5.x, 1.0, 64.0));
}

void main()
{
    tWorldPos[gl_InvocationID] = vWorldPos[gl_InvocationID];
    tNormal[gl_InvocationID]   = vNormal[gl_InvocationID];
    tUV[gl_InvocationID]       = vUV[gl_InvocationID];

    if (gl_InvocationID == 0) {
        vec3 eye = W.p8.xyz;
        if (W.p5.x <= 1.0) {
            gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0;
            gl_TessLevelOuter[2] = 1.0; gl_TessLevelInner[0] = 1.0;
        } else {
            float e0 = edgeFactor(vWorldPos[1], vWorldPos[2], eye);
            float e1 = edgeFactor(vWorldPos[2], vWorldPos[0], eye);
            float e2 = edgeFactor(vWorldPos[0], vWorldPos[1], eye);
            gl_TessLevelOuter[0] = e0;
            gl_TessLevelOuter[1] = e1;
            gl_TessLevelOuter[2] = e2;
            gl_TessLevelInner[0] = (e0 + e1 + e2) / 3.0;
        }
    }
}

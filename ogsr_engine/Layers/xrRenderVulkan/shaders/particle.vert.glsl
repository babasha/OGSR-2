#version 450
#extension GL_GOOGLE_include_directive : require
#include "froxel.glsl"   // exp-Z slice <-> view-Z mapping (shared with vol_inject + tonemap)

// Particle billboard vertex shader (OGSR Vulkan).
// Vertices are FVF::LIT {vec3 pos; D3DCOLOR color; vec2 uv} built CPU-side in
// world space (PAPI particle positions). Push constant carries the combined
// view-projection (raw X-Ray row-major Fmatrix → GLSL reads it transposed, so
// `viewProj * v` matches X-Ray's `v * M`, same convention as world.vert).
//
// It also carries the froxel grid's camera basis / exp-Z params so we can map the
// vertex to its volumetric-fog froxel and forward the slice W to the fragment
// shader (Stage-0 volumetric lighting of smoke). camPosNear/camDirLogFN are only
// meaningful when the particle pass pushes them (volParams.x > 0); the rain /
// wallmark passes that share this VS push only viewProj, so the froxel W they emit
// is unused garbage — their fragment shaders never read location 2.

layout(location = 0) in vec3 inPosition;   // world-space position
layout(location = 1) in vec4 inColor;      // D3DCOLOR via B8G8R8A8_UNORM → logical RGBA
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4  fragColor;
layout(location = 1) out vec2  fragUV;
layout(location = 2) out float fragVolW;   // froxel Z slice for the volumetric light probe

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 camPosNear;    // xyz = camera world pos, w = froxel near Z
    vec4 camDirLogFN;   // xyz = camera forward (unit), w = log2(far/near)
    vec4 volParams;     // x = light-probe strength, yz = 1/extent, w = radiance clamp (fragment)
} pc;

void main()
{
    gl_Position = pc.viewProj * vec4(inPosition, 1.0);
    fragColor   = inColor;
    fragUV      = inUV;

    // exp-Z froxel slice (EXACT inverse of vol_inject's forward viewZ mapping +
    // the tonemap composite's vw = log2(zview/near)/logFN). The screen XY of the
    // froxel is reconstructed in the fragment shader from gl_FragCoord, so the VS
    // only needs the linear view depth here.
    float near  = max(pc.camPosNear.w, 1e-4);
    float viewZ = dot(inPosition - pc.camPosNear.xyz, pc.camDirLogFN.xyz);
    fragVolW    = Froxel_SliceFromViewZ(max(viewZ, near), near, max(pc.camDirLogFN.w, 1e-4));
}

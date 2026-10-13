#version 450
// xrRenderVulkan — LOD imposter (FLOD billboard) vertex shader.
// Positions are baked WORLD-space (facet vertex + camera-ward shift), built on
// the CPU each frame; just project them. Matrix is the raw X-Ray viewProj
// (M*v == v*F convention), pushed as a single mat4.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PC {
    mat4 mViewProj;
    vec4 tint;
    vec4 fog_color;
    vec4 fog_params;
    vec4 eye_pos;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vWPos;   // aPos is already world-space

void main()
{
    gl_Position = pc.mViewProj * vec4(aPos, 1.0);
    vUV    = aUV;
    vColor = aColor;
    vWPos  = aPos;
}

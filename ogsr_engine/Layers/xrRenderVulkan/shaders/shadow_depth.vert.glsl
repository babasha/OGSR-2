#version 450
// xrRenderVulkan — sun shadow caster (depth-only). Renders static world geometry
// from the sun's POV. Position is at offset 0 for every level vertex layout
// (FLOAT3), so one input attribute covers all strides. No fragment shader: the
// pipeline writes depth only. lightMVP = objectXform · lightViewProj (row-major,
// transposed by GLSL → `lightMVP * pos` == X-Ray `pos · lightMVP`).
layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PC { mat4 lightMVP; } pc;

void main()
{
    gl_Position = pc.lightMVP * vec4(aPos, 1.0);
}

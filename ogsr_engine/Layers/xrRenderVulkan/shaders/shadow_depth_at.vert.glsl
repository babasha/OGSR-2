#version 450
// xrRenderVulkan — alpha-tested shadow caster (world statics: bushes, grates).
// Same as shadow_depth.vert plus the base UV so the fragment can punch out
// transparent texels — solid-quad shadows turn into the foliage imprint.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV_short;   // SHORT2 SSCALED @ tcOffset (24/28)

layout(push_constant) uniform PC {
    mat4  lightMVP;
    vec2  uvScale;     // 1/1024 for level statics
    float alphaRef;
} pc;

layout(location = 0) out vec2 vUV;

void main()
{
    gl_Position = pc.lightMVP * vec4(aPos, 1.0);
    vUV = aUV_short * pc.uvScale;
}

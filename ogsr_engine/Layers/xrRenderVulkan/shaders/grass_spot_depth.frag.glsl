#version 450
// xrRenderVulkan — grass-caster alpha test for the SPOT shadow map (depth-only).
// Punch-out quads: without the test the cast shadow is the whole rectangle.
// Same 0.5 cutoff as the color pass / VSM grass casters. See vk_pass_shadow.cpp.
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D uDiffuse;   // per-type grass diffuse (CDetailManager gfx set)

void main()
{
    if (texture(uDiffuse, vUV).a < 0.5) discard;
}

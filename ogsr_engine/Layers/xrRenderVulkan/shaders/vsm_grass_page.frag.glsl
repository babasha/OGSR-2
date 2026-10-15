#version 450
// xrRenderVulkan — VSM grass-caster alpha test (depth-only). Grass billboards are
// punch-out quads: without this the shadow is the whole rectangle ("box"). Sample the
// grass diffuse and discard below the same cutoff the color pass uses (0.5) so the cast
// shadow follows the blade cutout. No colour output — depth-only pass. See vk_vsm.cpp.
layout(location = 0) in vec2 vUV;
layout(set = 1, binding = 0) uniform sampler2D uDiffuse;   // per-type grass diffuse (CDetailManager gfx set)

void main()
{
    if (texture(uDiffuse, vUV).a < 0.5) discard;
}

#version 450
// xrRenderVulkan — alpha-tested shadow caster fragment: discard transparent
// texels so depth (= the shadow) keeps the foliage silhouette. No color output.

layout(location = 0) in vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2D uDiffuse;   // WorldMaterial set, binding 0

layout(push_constant) uniform PC {
    mat4  lightMVP;
    vec2  uvScale;
    float alphaRef;
} pc;

void main()
{
    if (texture(uDiffuse, vUV).a < pc.alphaRef)
        discard;
}

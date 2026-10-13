#version 450
// xrRenderVulkan — tree shadow caster fragment: punch out transparent leaf
// texels so the crown's shadow is leafy, not a solid card. No color output.

layout(location = 0) in vec2 vUV;

layout(set = 1, binding = 0) uniform sampler2D uDiffuse;   // per-group leaf texture

layout(push_constant) uniform PC {
    mat4  mViewProj;
    float uvScale;
    float alphaRef;
} pc;

void main()
{
    if (texture(uDiffuse, vUV).a < pc.alphaRef)
        discard;
}

#version 450
// xrRenderVulkan — VSM tree-caster alpha test (depth-only). Punch out transparent leaf
// texels so the crown's cast shadow is leafy, not a solid card. Same as tree_depth.frag
// but with the VSM page push layout (uvScale already applied in the VS). No colour output.
layout(location = 0) in vec2 vUV;
layout(set = 1, binding = 0) uniform sampler2D uDiffuse;   // per-group leaf texture
layout(push_constant) uniform PC { float uvScale; float alphaRef; uint cap; uint pad; } pc;

void main()
{
    if (texture(uDiffuse, vUV).a < pc.alphaRef) discard;
}

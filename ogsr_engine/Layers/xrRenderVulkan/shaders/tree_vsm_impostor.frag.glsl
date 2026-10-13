#version 450
// xrRenderVulkan — VSM tree IMPOSTOR alpha test (depth-only). Sample the baked
// per-species silhouette atlas (R8 coverage) and punch out the empty texels so the
// impostor's cast shadow keeps the crown's leafy holes (some dappling), not a solid
// card. No colour output. See tree_vsm_impostor.vert / CTreeManager::BuildImpostorAtlas.
layout(location = 0) in vec2 vUV;
layout(set = 1, binding = 2) uniform sampler2D uSilhouette;

layout(push_constant) uniform PC {
    vec4  sunDir;
    vec4  wind_params;
    vec4  wsetup_trees;
    vec4  wind_anim;
    uint  cap; uint numMeshes; float alphaRef; uint facetCount;
} pc;

void main()
{
    if (texture(uSilhouette, vUV).r < pc.alphaRef) discard;
}

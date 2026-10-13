#version 450
// xrRenderVulkan — crown SILHOUETTE bake FS (r_vsm_tree_impostor, load-time only).
// Punch out transparent leaf texels (same alpha test as tree_depth.frag); every
// surviving fragment writes full coverage. NO depth attachment → front+back leaves
// both write 1.0, so the cell holds the crown's alpha-tested SILHOUETTE (union).
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D uDiffuse;   // per-species crown leaf texture

layout(push_constant) uniform PC {
    mat4  mvp;
    float uvScale;
    float alphaRef;
} pc;

layout(location = 0) out vec4 outCov;   // R8 atlas: only .r stored

void main()
{
    if (texture(uDiffuse, vUV).a < pc.alphaRef) discard;
    outCov = vec4(1.0);
}

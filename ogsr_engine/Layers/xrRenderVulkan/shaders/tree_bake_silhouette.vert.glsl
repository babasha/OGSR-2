#version 450
// xrRenderVulkan — crown SILHOUETTE bake VS (r_vsm_tree_impostor, load-time only).
// Renders a unique tree/crown MESH orthographically in its own MODEL space (no
// per-instance xform) from one azimuth into a cell of the silhouette atlas. The
// caller pushes an ortho view·proj framed on the mesh's local bounds. Alpha test
// happens in the FS (same crown diffuse as the shadow caster) so the baked cell is
// the crown's leafy coverage, not a solid card.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV_raw;    // SHORT2 SSCALED
#ifndef XR_SHORT_ATTR_DECLARED
#define XR_SHORT_ATTR_DECLARED
layout(constant_id = 200) const float XR_SHORT_ATTR = 1.0; // 32767 where SSCALED is missing and SHORT2 comes as SNORM (vk_gfx_pipeline.cpp)
#endif
#define aUV (aUV_raw * XR_SHORT_ATTR)

layout(push_constant) uniform PC {
    mat4  mvp;        // ortho view·proj (X-Ray row-major; upload as-is → GLSL M*v == v*VP)
    float uvScale;    // 1/2048 (matches the crown UV quantization)
    float alphaRef;
} pc;

layout(location = 0) out vec2 vUV;

void main()
{
    gl_Position = pc.mvp * vec4(aPos, 1.0);
    vUV = aUV * pc.uvScale;
}

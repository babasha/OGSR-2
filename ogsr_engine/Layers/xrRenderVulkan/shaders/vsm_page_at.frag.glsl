#version 450
// xrRenderVulkan — VSM page ALPHA-TEST discard (r_vsm_at). Depth-only pass:
// the only job is to punch the cutout holes into the page depth exactly like
// shadow_depth_at.frag does for the sun maps. Set 1 = the material's set
// (WorldMaterialCache layout; binding 0 = diffuse).
layout(location = 0) in vec2 vUV;

layout(set = 1, binding = 0) uniform sampler2D uDiffuse;

layout(push_constant) uniform PC {
    vec2  uvScale;   // consumed by the vertex stage
    float aref;
    float _pad;
} pc;

void main()
{
    if (texture(uDiffuse, vUV).a < pc.aref) discard;
}

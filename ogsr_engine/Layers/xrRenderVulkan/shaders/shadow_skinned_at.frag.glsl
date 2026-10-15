#version 450
// xrRenderVulkan — alpha-test fragment for the skinned depth PREPASS.
// MUST match skinned.frag's discard threshold exactly (a < 0.25): the color
// pass re-rasterizes with LEQUAL, so prepass coverage == color coverage.

layout(location = 0) in vec2 vUV;

layout(set = 1, binding = 0) uniform sampler2D uTexDiffuse;

void main()
{
    if (texture(uTexDiffuse, vUV).a < 0.25)
        discard;
}

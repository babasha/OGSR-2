#version 450
// xrRenderVulkan — skinned NPC motion vectors (MV Phase 2a). Pairs with
// motion_vec_skinned.vert. Turns the cur/prev clip positions into a screen-space
// motion vector, using the SAME convention as the fullscreen camera pass
// (motion_vec.frag): MV = prevUV − curUV, D3D y-up ndc → UV.

layout(location = 0) in vec4 vCurClip;
layout(location = 1) in vec4 vPrevClip;
layout(location = 2) in vec2 vUV;

layout(set = 1, binding = 0) uniform sampler2D uTexDiffuse;   // same diffuse as skinned.frag

layout(location = 0) out vec2 outMV;

vec2 ndc2uv(vec2 n) { return vec2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y); }

void main()
{
    // Alpha-test at the SAME threshold as skinned.frag — so hair/strap cutout
    // holes write the BACKGROUND's motion, not the NPC silhouette's.
    if (texture(uTexDiffuse, vUV).a < 0.25)
        discard;

    vec2 curUV  = ndc2uv(vCurClip.xy / vCurClip.w);
    vec2 prevUV = (vPrevClip.w > 1e-6) ? ndc2uv(vPrevClip.xy / vPrevClip.w) : curUV;
    outMV = prevUV - curUV;
}

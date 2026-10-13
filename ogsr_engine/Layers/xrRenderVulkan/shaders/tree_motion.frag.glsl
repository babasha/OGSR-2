#version 450
// xrRenderVulkan — tree wind-sway motion vectors (fragment). Pairs with
// tree_motion.vert. Turns the cur/prev clip positions into a screen-space motion
// vector (MV = prevUV − curUV), alpha-testing the leaf punch-out at the SAME cutoff
// as tree.frag so the transparent gaps keep the background's motion.

layout(location = 0) in vec4 vCurClip;
layout(location = 1) in vec4 vPrevClip;
layout(location = 2) in vec2 vUV;

layout(set = 1, binding = 0) uniform sampler2D uDiffuse;   // per-group tree diffuse (same set as forward)

layout(push_constant) uniform PC {
    mat4  curVP;
    mat4  prevVP;
    float uvScale;
    float alphaRef;           // leaf cutout — must match tree.frag
    float _pad0;
    float _pad1;
    vec4  wind_params;
    vec4  wsetup_trees;
    vec4  wind_anim;
    vec4  wind_params_prev;
    vec4  wsetup_trees_prev;
    vec4  wind_anim_prev;
} pc;

layout(location = 0) out vec2 outMV;

vec2 ndc2uv(vec2 n) { return vec2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y); }

void main()
{
    if (texture(uDiffuse, vUV).a < pc.alphaRef)   // leaf punch-out → keep background MV
        discard;

    vec2 curUV  = ndc2uv(vCurClip.xy / vCurClip.w);
    vec2 prevUV = (vPrevClip.w > 1e-6) ? ndc2uv(vPrevClip.xy / vPrevClip.w) : curUV;
    outMV = prevUV - curUV;
}

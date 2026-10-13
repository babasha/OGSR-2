#version 450
// xrRenderVulkan — grass wind-sway motion vectors (fragment). Pairs with
// detail_motion.vert. Turns the cur/prev clip positions into a screen-space motion
// vector (MV = prevUV − curUV, same convention as motion_vec.frag), alpha-testing
// the blade cutout so the transparent gaps keep the background's motion instead of
// the quad silhouette's.

layout(location = 0) in vec4 vCurClip;
layout(location = 1) in vec4 vPrevClip;
layout(location = 2) in vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2D uDiffuse;   // grass per-type diffuse (same set as forward)

layout(location = 0) out vec2 outMV;

vec2 ndc2uv(vec2 n) { return vec2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y); }

void main()
{
    // Blade cutout — discard the transparent gaps so they keep the camera/static MV
    // the fullscreen pass wrote, not the grass quad's.
    if (texture(uDiffuse, vUV).a < 0.5)
        discard;

    vec2 curUV  = ndc2uv(vCurClip.xy / vCurClip.w);
    vec2 prevUV = (vPrevClip.w > 1e-6) ? ndc2uv(vPrevClip.xy / vPrevClip.w) : curUV;
    outMV = prevUV - curUV;
}

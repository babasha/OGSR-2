#version 450
// xrRenderVulkan — GLASS refraction, vertex. The late-glass panes are re-drawn
// into the particle heat-haze distortion RT (rg = UV offset around neutral 0.5);
// the tonemap then bends whatever is BEHIND the pane (sceneUV += (rg-0.5)*0.05)
// — the "old uneven glass" refraction. Same stride-32 world vertex, only the
// position + base UV are consumed (works for both tcOffset 24/28 sub-layouts —
// the pipeline's attribute offset picks the right UV). Depth-tested against the
// finished opaque scene (glass writes no depth, the background does).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV_short;   // SHORT2 SSCALED (uv * 1024)

layout(push_constant) uniform PC {
    mat4  mvp;        // model -> clip (per pane; dynamics carry their xform in it)
    float strength;   // r_glass_refr
} pc;

layout(location = 0) out vec2 vUV;

void main()
{
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vUV = inUV_short * (1.0 / 1024.0);
}

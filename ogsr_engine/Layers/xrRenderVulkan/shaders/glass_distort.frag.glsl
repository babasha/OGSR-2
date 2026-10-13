#version 450
// xrRenderVulkan — GLASS refraction, fragment. Procedural low-frequency waviness
// (+ a finer ripple) in the pane's UV space = the uneven hand-made glass of old
// village windows. Output rg = 0.5 + offset; the tonemap scales the deviation by
// kDistortAmount (0.05), so 0.04 here ≈ up to ~4-6 px of warp at 2K. Alpha 0.85
// lets a trace of any heat-haze behind the pane survive the overwrite.
layout(push_constant) uniform PC {
    mat4  mvp;
    float strength;   // r_glass_refr (0 disables the draw C++-side)
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 o;

void main()
{
    vec2 w = vec2(sin(vUV.x * 37.0 + vUV.y * 11.0), cos(vUV.y * 29.0 - vUV.x * 13.0))
           + 0.5 * vec2(sin(vUV.y * 113.0 + vUV.x * 41.0), cos(vUV.x * 97.0));
    o = vec4(0.5 + w * 0.04 * pc.strength, 0.5, 0.85);
}

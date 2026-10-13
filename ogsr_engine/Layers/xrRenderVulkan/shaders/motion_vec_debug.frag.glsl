#version 450
// xrRenderVulkan — motion-vector debug visualisation (r_mv_debug).
//
// Overwrites the swapchain with a false-colour view of the motion-vector target
// so the MV reconstruction (sign / Y-flip / magnitude) can be verified by eye:
//   - still camera + static scene → flat grey (0.5, 0.5)
//   - pan right → red rises;  pan left → red falls
//   - pan up    → green rises; pan down → green falls
//   - magnitude scaled by r_mv_debug_scale (UV motion per frame is tiny).

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uMV;   // RG16F screen-space motion (UV space)

layout(push_constant) uniform PC {
    vec4 p;   // x = display scale
} pc;

void main()
{
    vec2 ts = vec2(textureSize(uMV, 0));
    vec2 uv = gl_FragCoord.xy / ts;
    vec2 mv = texture(uMV, uv).rg * pc.p.x;
    outColor = vec4(clamp(0.5 + mv.x, 0.0, 1.0),
                    clamp(0.5 + mv.y, 0.0, 1.0),
                    0.5, 1.0);
}

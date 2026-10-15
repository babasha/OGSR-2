#version 450

// Particle billboard fragment shader (OGSR Vulkan).
// Samples the particle sprite and modulates by the per-vertex PAPI colour.
// Blending (additive / alpha / multiply) is configured per-effect on the
// pipeline; this shader just produces tex*color. OGSR's forward path is LDR
// (renders straight to the swapchain), so — unlike the monolith HDR port —
// there is NO sRGB→linear pow(2.2) step here (matches the world shaders).
//
// Stage-0 volumetric lighting: when the particle pass enables it (volParams.x > 0,
// world-phase smoke only — never HUD/distort, and only when r_vol is on) the smoke
// is brightened by the LOCAL froxel in-scatter at its position. That term already
// carries the sun shaft, flashlight cone, campfire glow, HG phase and shadows, so
// the smoke catches the light instead of reading as a flat unlit sticker. The probe
// is the density-independent radiance (in-scatter / extinction); it is ADDITIVE and
// scaled by coverage, so smoke never gets darker than before.

layout(location = 0) in vec4  fragColor;
layout(location = 1) in vec2  fragUV;
layout(location = 2) in float fragVolW;

layout(set = 0, binding = 0) uniform sampler2D texSampler;
layout(set = 1, binding = 0) uniform sampler3D uScatter;   // local froxel in-scatter (rgb) + extinction (a)

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 camPosNear;
    vec4 camDirLogFN;
    vec4 volParams;     // x = light-probe strength, yz = 1/extent, w = radiance clamp
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 texColor = texture(texSampler, fragUV);
    outColor = texColor * fragColor;

    // Drop fully-transparent texels (cheap fill saving; black additive texels
    // contribute nothing anyway).
    if (outColor.a < 0.0039)
        discard;

    if (pc.volParams.x > 0.0) {
        vec2 uv  = gl_FragCoord.xy * pc.volParams.yz;          // screen UV (matches the tonemap composite)
        vec4 s   = texture(uScatter, vec3(uv, clamp(fragVolW, 0.0, 1.0)));
        vec3 rad = s.rgb / max(s.a, 1e-3);                     // density-independent local radiance
        rad = min(rad, vec3(pc.volParams.w));                  // bound it (r_vol_smoke_clamp) so smoke doesn't blow to white next to a campfire / sun beam
        outColor.rgb += rad * (pc.volParams.x * outColor.a);   // additive, coverage-weighted
    }
}

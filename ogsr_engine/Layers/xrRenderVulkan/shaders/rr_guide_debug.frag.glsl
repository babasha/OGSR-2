#version 450
// xrRenderVulkan — DLSS Ray Reconstruction guide G-buffer viewer (r_dlss_rr_debug).
//
// Overwrites the scene with one guide channel so a WRONG guide is something you
// SEE rather than something you infer from a mushy upscale. RR's failure mode is
// quiet: hand it a flat albedo and it decides the texture detail is ray noise and
// irons it out, which looks like "DLSS is soft today" and not like "the guide is
// a constant". This view is how that stays diagnosable.
//
//   1 = diffuse albedo   — must look like the level's TEXTURES with no lighting:
//                          no sun, no shadows, no bloom. Any shading visible here
//                          is light that RR will fail to remove.
//   2 = specular albedo  — near-black on matte dielectrics, brightening at grazing
//                          angles (it is EnvBRDFApprox2 of F0 = 0.04).
//   3 = normal           — the usual x/y/z → r/g/b hemisphere. FLAT MID-GREY means
//                          "nothing wrote a normal here" (0,0,0 → 0.5,0.5,0.5),
//                          which is exactly how an unconverted geometry class
//                          announces itself.
//   4 = roughness        — normal.w. Terrain ~0.9 (dark-ish white), gloss maps darker.
//
// ⚠The 0.5 grey of an unwritten albedo texel and a genuinely grey material look
// the same. Use view 3 to tell them apart: an unwritten texel has NO normal.

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uNormalRough;   // RGBA16F: normal.xyz + linear roughness .w
layout(set = 0, binding = 1) uniform sampler2D uDiffAlbedo;    // RGBA8
layout(set = 0, binding = 2) uniform sampler2D uSpecAlbedo;    // RGBA8

layout(push_constant) uniform PC {
    vec4 p;   // x = mode (1..4), zw = 1/destination extent
} pc;

void main()
{
    // ⚠Normalize by the DESTINATION extent, not by textureSize: this draws
    // post-tonemap onto the display-res swapchain while the guides are render-res,
    // so dividing by the guide's own size would zoom the view by the DLSS ratio.
    vec2 uv = gl_FragCoord.xy * pc.p.zw;

    int  mode = int(pc.p.x + 0.5);
    vec4 nr   = texture(uNormalRough, uv);
    vec3 col;

    if (mode == 2)      col = texture(uSpecAlbedo, uv).rgb;
    else if (mode == 3) col = nr.xyz * 0.5 + 0.5;
    else if (mode == 4) col = vec3(nr.w);
    else                col = texture(uDiffAlbedo, uv).rgb;

    // Drawn AFTER the tonemap, straight onto the swapchain (the r_mv_debug shape).
    // That is the whole reason for this placement: a debug view routed through the
    // tone curve shows a number the exposure moved, and "is the albedo right" is a
    // question about the number, not about how it looks after exposure.
    outColor = vec4(col, 1.0);
}

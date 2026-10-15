#version 450
// xrRenderVulkan — editor overlay lines (Spike 2), fragment. Flat vertex color
// with a per-fragment radial ground fade so the grid stays crisp/bright near the
// centre and fades out at the edges (kills the horizon moiré of many thin lines
// packing into a few pixels), like a DCC viewport grid. Interpolated per fragment
// so a long line is bright where it passes near the centre and fades at its ends.
// Bloom is disabled in -vk_editor so the bright lines stay sharp.
layout(location = 0) in  vec4 vColor;
layout(location = 1) in  vec3 vWorld;
layout(location = 0) out vec4 outColor;

void main()
{
    float r = length(vWorld.xz);              // ground-plane distance from the grid centre
    const float FADE_START = 18.0;            // full strength within this radius
    const float FADE_END   = 38.0;            // gone by here
    float fade = clamp(1.0 - (r - FADE_START) / (FADE_END - FADE_START), 0.0, 1.0);
    outColor = vec4(vColor.rgb, vColor.a * fade);
}

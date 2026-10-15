#version 460
// xrRenderVulkan — DDGI probe debug view, fragment half: turn the quad into a disc.
// A square would read as a texel of something; a disc reads as a probe.
layout(location = 0) in  vec3 vCol;
layout(location = 1) in  vec2 vUV;
layout(location = 0) out vec4 oCol;

void main()
{
    const float r = dot(vUV, vUV);
    if (r > 1.0) discard;
    // A darker rim, so overlapping probes stay countable instead of merging into
    // one blob — the view is often used to answer "how many are in this room".
    const float edge = smoothstep(1.0, 0.55, r);
    oCol = vec4(vCol * mix(0.35, 1.0, edge), 1.0);
}

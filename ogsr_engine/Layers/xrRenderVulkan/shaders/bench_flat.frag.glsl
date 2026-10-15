#version 450
// -nx_bench: flat colour, no fetch -- the ROP / fill-rate probe.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.25, 0.5, 0.75, 1.0); }

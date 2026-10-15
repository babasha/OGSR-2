#version 460
// xrRenderVulkan — DDGI ray debug view, fragment half.
layout(location = 0) in  vec3 vCol;
layout(location = 0) out vec4 oCol;
void main() { oCol = vec4(vCol, 1.0); }

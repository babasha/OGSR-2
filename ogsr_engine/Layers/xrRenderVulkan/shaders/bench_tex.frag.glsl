#version 450
// -nx_bench: one bilinear fetch from a 1024^2 RGBA8 texture, mapped 1:1-ish over the screen -- the texture-rate probe.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() { outColor = texture(uTex, vUV * 1.1); }

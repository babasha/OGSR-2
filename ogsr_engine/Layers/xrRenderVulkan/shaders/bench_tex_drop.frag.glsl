#version 450
// -nx_bench: fetch + alpha test that discards EVERY fragment (texture alpha is 0.8, threshold 0.9) -- what an alpha-tested
// leaf card costs when nothing survives (the tree crown case).
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main()
{
    vec4 t = texture(uTex, vUV * 1.1);
    if (t.a < 0.9) discard;
    outColor = t;
}

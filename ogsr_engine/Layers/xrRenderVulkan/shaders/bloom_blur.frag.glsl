#version 450
// Separable 9-tap gaussian blur for the bloom chain (R4 bloom_filter port).
// Run twice: horizontal (dir = (1,0)) then vertical (dir = (0,1)).

layout(set = 0, binding = 0) uniform sampler2D uSrc;

layout(push_constant) uniform PC {
    vec4 dir;   // xy = blur direction in texels ((1,0) or (0,1))
} pc;

layout(location = 0) out vec4 outColor;

const float W[5] = float[](0.227027, 0.194594, 0.121622, 0.054054, 0.016216);

void main()
{
    vec2 sz = vec2(textureSize(uSrc, 0));
    vec2 uv = gl_FragCoord.xy / sz;
    vec2 step = pc.dir.xy / sz;

    vec3 acc = texture(uSrc, uv).rgb * W[0];
    for (int i = 1; i < 5; ++i) {
        acc += texture(uSrc, uv + step * float(i)).rgb * W[i];
        acc += texture(uSrc, uv - step * float(i)).rgb * W[i];
    }
    outColor = vec4(acc, 1.0);
}

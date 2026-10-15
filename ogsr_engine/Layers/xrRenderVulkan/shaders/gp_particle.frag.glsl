#version 450

// GPU particle billboard fragment shader.
// Procedural soft circle sprite with alpha blending.

layout(location = 0) in vec4  fragColor;
layout(location = 1) in vec2  fragUV;

layout(location = 0) out vec4 outColor;

void main()
{
    // Procedural soft circle
    vec2  uv_c    = fragUV - 0.5;
    float dist    = length(uv_c);
    float alpha   = 1.0 - smoothstep(0.3, 0.5, dist);

    if (alpha < 0.01) discard;

    outColor = vec4(fragColor.rgb, fragColor.a * alpha);
}

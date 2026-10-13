#version 450

// Rain drop STREAKS — procedural shape (OGSR Vulkan).
//
// The fx_rain texture's alpha reads ~0 through our loader (its rgb is an SSFX
// refraction normal map; the alpha channel proved unusable — r_rain_debug
// solid streaks showed while textured ones didn't). The streak shape is
// cheaper than the lookup anyway: soft across the width, faded at both ends.
// Colour/alpha come from the per-vertex tint (vk_rain.cpp: fog+hemi mix).

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(binding = 0) uniform sampler2D texSampler;   // bound for layout parity; unused

layout(location = 0) out vec4 outColor;

void main()
{
    // UV: x across the streak (0..1), y along it (0..1) — both uv_set
    // variants of the quad keep that orientation.
    float across = sin(3.14159265 * fragUV.x);
    float along  = sin(3.14159265 * fragUV.y);
    // ^4 across = sharp thin core with soft edges (x^2 read as fat "tracers").
    float shape  = (across * across * across * across) * (0.5 + 0.5 * along);

    // fragColor.a == 1.0 is the CPU debug flag (r_rain_debug 1): solid quad.
    float a = (fragColor.a >= 0.999) ? 0.85 : shape * fragColor.a;
    outColor = vec4(fragColor.rgb, a);
    if (outColor.a < 0.0039)
        discard;
}

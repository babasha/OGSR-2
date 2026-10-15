#version 450

// Rain drop streaks + splashes (OGSR Vulkan).
//
// The SSFX rain textures (fx\fx_rain) are NOT colour sprites: rgb is a
// refraction NORMAL map (the DX shader offsets the screen buffer by it) and
// only the ALPHA carries the streak/splash shape. Multiplying the rgb in like
// the generic particle shader tints everything purple-grey and the drops
// vanish. Here the shape comes from tex.a alone and the COLOUR comes from the
// per-vertex tint (vk_rain.cpp computes a fog+hemi mix — slightly brighter
// than the background haze, the visibility the DX path gets from refraction).

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(binding = 0) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main()
{
    float shape = texture(texSampler, fragUV).a;
    // fragColor.a == 1.0 is the CPU debug flag (r_rain_debug 1): draw the quad
    // SOLID — splits "texture alpha is empty" from "geometry/cull is wrong".
    float a = (fragColor.a >= 0.999) ? 0.85 : shape * fragColor.a;
    outColor = vec4(fragColor.rgb, a);
    if (outColor.a < 0.0039)
        discard;
}

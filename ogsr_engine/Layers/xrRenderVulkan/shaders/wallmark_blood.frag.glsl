#version 450

// Blood decal fragment shader — port of R4's effects_wallmark_blood.ps
// (Screen Space Shaders, ascii1457). Unlike generic wallmarks (modulate2x —
// too dark on dark clothing), blood alpha-blends its own colour. The alpha
// REMAP (a*1.7-0.7) discards everything below a≈0.41 — that's the SSS fix for
// the junky fringe alpha in the wm textures.

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(binding = 0) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

// ssfx_blood_decals defaults (x = colour intensity, y = opacity).
const float kIntensity = 0.75;
const float kOpacity   = 0.75;

void main()
{
    // Skeleton wallmarks don't clip their triangles — UVs past [0,1] would
    // smear the clamped texture border.
    if (fragUV.x < 0.0 || fragUV.x > 1.0 || fragUV.y < 0.0 || fragUV.y > 1.0)
        discard;

    vec4 t = textureLod(texSampler, fragUV, 0.0);   // top mip — close-range decals

    t.rgb *= kIntensity;
    float a = clamp(t.a * 1.7 - 0.7, 0.0, 1.0) * kOpacity;
    a *= fragColor.a;                               // TTL fade / translucency cap

    if (a < 0.0039)
        discard;
    outColor = vec4(t.rgb * fragColor.rgb, a);
}

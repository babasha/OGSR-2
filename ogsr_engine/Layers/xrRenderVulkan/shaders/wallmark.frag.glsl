#version 450

// Wallmark (decal) fragment shader — R4 parity (effects_wallmark.s):
// wallmarks blend MODULATE2X (DST_COLOR, SRC_COLOR → out = 2*src*dst), NOT
// alpha. The wm_* textures are authored for that: their "transparent" regions
// are NEUTRAL GREY (0.5 → 2*0.5*dst = dst unchanged) and the alpha channel is
// junk — alpha-blending them is what painted white halos around blood splats.
// Fade (TTL / translucency, vertex alpha 1 fresh → 0 dead) lerps the sample
// back to the neutral grey instead of changing coverage.

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(binding = 0) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main()
{
    // Outside the decal projection box: skeleton wallmarks don't clip their
    // triangles (unlike the static RecurseTri path), so UVs can run past
    // [0,1] — CLAMP_TO_EDGE would smear the texture border there.
    if (fragUV.x < 0.0 || fragUV.x > 1.0 || fragUV.y < 0.0 || fragUV.y > 1.0)
        discard;

    // Top mip only: body decals map to small/stretched triangles → low mips
    // average the splat with its neutral background. Decals are close-range.
    vec4 texColor = textureLod(texSampler, fragUV, 0.0);

    vec3 c = texColor.rgb * fragColor.rgb;
    outColor.rgb = mix(vec3(0.5), c, fragColor.a);   // fade to multiply-neutral
    outColor.a   = 1.0;
}

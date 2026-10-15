#version 450
// xrRenderVulkan — LOD imposter fragment shader.
// Sample the level_lods atlas, alpha-test the billboard cutout. The atlas image
// already carries baked tree colour/lighting, so the per-vertex tint is just a
// pass-through (white in v1).

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

// mViewProj used by the vertex stage; tint = env lighting factor for the pre-lit
// atlas (clamp(hemi + sun*0.5, 0, 1)) so distant trees track time-of-day.
// fog_* / eye_pos = R4 distance fog (imposters are far → they sink into the haze).
layout(push_constant) uniform PC {
    mat4 mViewProj;
    vec4 tint;
    vec4 fog_color;
    vec4 fog_params;   // x=-near*r, y=near, z=far, w=r; fog = saturate(dist*w + x)
    vec4 eye_pos;
} pc;

#include "lod_dither.glsl"   // the crossfade noise -- shared with the crown mesh (tree.frag / tree_r1.frag)
#define ENV_SET 2
#include "light_ubo.glsl"     // 10-10: the env set (set 2, vk_LODManager) -- L + the sky cubes for nx_fog
#include "nx_fog.glsl"       // 10-10: the fog fades into the sky behind (r_fog_sky)

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vWPos;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 d = texture(uAtlas, vUV);
    if (d.a < 0.5)
        discard;
    // nfs23: R1 crossfade as a dither (interleaved gradient noise) -- vColor.a = billboard opacity in the band
    // The crown mesh keeps exactly the complementary pixels (tree.frag / tree_r1.frag) -- same lodDither().
    if (vColor.a < 0.999) {
        if (lodDither() >= vColor.a) discard;
    }
    vec3 col = d.rgb * vColor.rgb * pc.tint.rgb;

    // Distance fog (R4) — see world_lmap.frag.
    float fog = clamp(length(vWPos - pc.eye_pos.xyz) * pc.fog_params.w + pc.fog_params.x, 0.0, 1.0);
    // 10-10 r_fog_sky: a fogged billboard on a hill is the sky behind it, not a grey cutout popping in as the camera turns
    if (fog > 0.0) col = mix(col, (L.fog_sky.w > 0.0) ? nxFogColor(vWPos, fog) : pc.fog_color.rgb, fog);

    outColor = vec4(col, 1.0);
}

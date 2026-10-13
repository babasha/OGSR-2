#version 450
#extension GL_GOOGLE_include_directive : require
// nfs31 R1 crown FS (Switch): the crown light and fog come from tree.vert.glsl -DTREE_R1 (tree_r1.vert.spv); here only
// the diffuse fetch and alpha test of tree.frag, then diff * light and the fog mix. The register footprint of the
// r_tree_flat probe, which in the nfs30 heavy point was 2.2 ms cheaper than the full crown shader.
#define ENV_SET 2
#include "light_ubo.glsl"        // L.spot_flash.z (DLSS mip bias, 0 here), L.fog_color
#include "tree_gfx_push.glsl"    // pc.alphaRef

layout(set = 1, binding = 0) uniform sampler2D uDiffuse;

layout(location = 0) in vec2  vUV;
layout(location = 1) in vec3  vLight;
layout(location = 2) in vec3  vFogC;   // 10-10: nxFogColor x fog (tree.vert); vLight already carries (1 - fog)
layout(location = 4) flat in float vLodFade;   // the billboard's share of this crown (tree.vert)
layout(location = 0) out vec4 outColor;

#include "lod_dither.glsl"

void main()
{
    vec4 diff = texture(uDiffuse, vUV, L.spot_flash.z);
    if (L.spot_flash.z != 0.0) diff.a = texture(uDiffuse, vUV).a;   // alpha unbiased, as tree.frag
    if (diff.a < pc.alphaRef)
        discard;
    if (vLodFade > 0.0 && lodDither() < vLodFade)   // R1 crossfade: these pixels are the billboard's
        discard;
    outColor = vec4(diff.rgb * vLight + vFogC, 1.0);   // = mix(diff * light, fog colour, fog)
}

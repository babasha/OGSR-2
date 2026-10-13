#version 450
#extension GL_GOOGLE_include_directive : require
// nfs32 R1V grass FS (Switch): light, alpha mip-fade and fog come from detail.vert.glsl -DDETAIL_R1V; here only the
// diffuse fetch and alpha test of detail.frag, then diff * light and the fog mix (detail.frag -DDETAIL_R1, per vertex).
#include "light_ubo.glsl"          // L.spot_flash.z (DLSS mip bias, 0 here), L.fog_color (set 1 b0)
#include "detail_gfx_push.glsl"    // pc.vConsts (r_grass_aref)

layout(set = 0, binding = 0) uniform sampler2D uDiffuse;

layout(location = 0) in vec2  vUV;
layout(location = 1) in vec3  vLight;
layout(location = 2) in float vFade;
layout(location = 3) in vec3  vFogC;   // 10-10: nxFogColor x fog (detail.vert); vLight already carries (1 - fog)
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 diff = texture(uDiffuse, vUV, L.spot_flash.z);
    if (L.spot_flash.z != 0.0) diff.a = texture(uDiffuse, vUV).a;   // alpha unbiased, as detail.frag
    diff.a *= vFade;
    const float aref = pc.vConsts.x > 0.001 ? pc.vConsts.x : 0.5;
    if (diff.a < aref)
        discard;
    outColor = vec4(diff.rgb * vLight + vFogC, 1.0);   // = mix(diff * light, fog colour, fog)
}

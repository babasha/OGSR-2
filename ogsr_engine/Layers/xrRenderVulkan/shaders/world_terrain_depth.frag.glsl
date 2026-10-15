#version 450
#extension GL_GOOGLE_include_directive : require
#include "light_ubo.glsl"        // L (set 1 b0) - eye/pom/mud params
#include "snow_displace.glsl"    // SnowDeformOn/SnowDeformPress (mud bias)

// DEPTH-PREPASS fragment shader for terrain, used only when r_pom_zoff is on
// (SSFX "Depth Offset" port). Runs the SAME 4-channel POM march as the color
// pass (terrain_pom.glsl) and sinks gl_FragDepth into the parallax cracks, so
// every prepass-depth consumer - GTAO, the VSM screen resolve, half-res depth -
// sees the CARVED micro-surface: AO darkens the pits and sun shadows wrap into
// them, which is what makes SSFX/Anomaly ground read as cut-in, not painted.
// The color pass (world_terrain_zoff.frag) recomputes the identical depth so
// its LEQUAL test passes on equality. Same terrain set 0 + EnvLight set 1 +
// push constants the terrain depth pipeline already binds.

layout(set = 0, binding = 1)  uniform sampler2D uMask;   // RGBA splat weights
layout(set = 0, binding = 11) uniform sampler2D uDhR;    // grass   height
layout(set = 0, binding = 12) uniform sampler2D uDhG;    // asphalt height
layout(set = 0, binding = 13) uniform sampler2D uDhB;    // earth   height
layout(set = 0, binding = 14) uniform sampler2D uDhA;    // gravel  height

layout(push_constant) uniform PushConstants {
    mat4  mvp;
    vec2  uvScale;
    float alphaRef;
    float detailScale;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec2 vDetailUV;
layout(location = 2) in vec2 vLmapUV;
layout(location = 3) in vec3 vWorldPos;
layout(location = 4) in vec3 vNormal;

#include "terrain_pom.glsl"

layout(depth_greater) out float gl_FragDepth;   // sink-only offset keeps early-Z

void main()
{
    vec3 geomN = normalize(vNormal);
    vec4 mask  = texture(uMask, terrainMaskUV(vUV, vWorldPos));
    float wsum = dot(mask, vec4(1.0));
    mask = (wsum > 1e-4) ? (mask / wsum) : vec4(1.0, 0.0, 0.0, 0.0);

    float mudPress, mudCarve, mudGrime;
    terrainMudSetup(mask, vWorldPos, mudPress, mudCarve, mudGrime);   // sets g_mudBias

    float pomShadow, pomAO, detH;
    vec2 pUVs[4];
    vec4 pomHits;
    terrainPOM4(vDetailUV, mask, geomN, vWorldPos, pUVs, pomHits, pomShadow, pomAO, detH, false);

    gl_FragDepth = max(gl_FragCoord.z, terrainZoffDepth(pc.mvp, vWorldPos, geomN, detH, gl_FragCoord.z));
}

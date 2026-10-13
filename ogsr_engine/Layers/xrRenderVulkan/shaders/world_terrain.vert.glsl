#version 450
#extension GL_GOOGLE_include_directive : require
#include "light_ubo.glsl"       // Lighting UBO (set 1 b0) - read sf_params.w (snow coverage) in the VS
#include "snow_displace.glsl"   // SnowDisplace (geometric snow volume)

// World pass - TERRAIN splatting variant. Same vertex layout as world_lmap
// (stride 32, tcOffset 24). Outputs base UV (also the splat-mask UV), detail UV,
// lightmap UV, world pos, world normal. SNOW: the vertex is raised along its normal
// by the local snow depth (SnowDisplace) so snow has real volume - applied here in
// the COLOR pass (B1; depth/shadow passes follow once z-behaviour is confirmed).

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV_short;     // base UV (SHORT2 SSCALED)
layout(location = 2) in vec4 inTangent;      // .a = du (sub-pixel U fraction)
layout(location = 3) in vec4 inBinormal;     // .a = dv (sub-pixel V fraction)
layout(location = 4) in vec2 inLmapUV_short; // lightmap UV (SHORT2 SSCALED)
layout(location = 5) in vec4 inNormal;       // D3DCOLOR @ 12 (BGRA in memory)

layout(push_constant) uniform PushConstants {
    mat4  mvp;
    vec2  uvScale;
    float alphaRef;
    float detailScale;
} pc;

layout(location = 0) out vec2 vUV;       // base + mask UV
layout(location = 1) out vec2 vDetailUV; // detail UV (base * detailScale)
layout(location = 2) out vec2 vLmapUV;
layout(location = 3) out vec3 vWorldPos; // terrain is world-space (identity model)
layout(location = 4) out vec3 vNormal;   // world-space normal (dynamic lights)

void main()
{
    vec3 N  = inNormal.bgr * 2.0 - 1.0;            // D3DCOLOR BGRA -> xyz
    // Snow blanket volume. In MESH mode (deform_tex.x>=2) the dense snow mesh owns the
    // near snow geometry, so the terrain stays at bare height (no double surface / z-fight).
    vec3 wp = (L.deform_tex.x >= 1.5) ? inPos
            : SnowDisplace(inPos, N, clamp(L.sf_params.w, 0.0, 1.0));   // raise by snow depth

    gl_Position = pc.mvp * vec4(wp, 1.0);
    vWorldPos   = wp;
    vNormal     = N;

    vec2 uv   = inUV_short + vec2(inTangent.a, inBinormal.a);
    vUV       = uv * pc.uvScale;
    vDetailUV = vUV * pc.detailScale;
    vLmapUV   = inLmapUV_short * (1.0 / 32768.0);
}

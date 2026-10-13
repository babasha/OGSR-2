#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — WATER tessellation evaluation: the stage that makes the wave
// GEOMETRY instead of a lighting trick. Barycentric interpolation of the patch,
// then a vertical displacement by the same field the fragment stage shades
// with (water_common.glsl) — the analytic swell plus the interactive ripple
// sim, so a wake from a footstep is a real bump in the silhouette.
//
// Displacement fades out with distance in step with the tessellation factor:
// past tessFar there are no vertices left to displace, and a surface that is
// half-displaced at the LOD seam shows a crease.
#include "water_common.glsl"

layout(triangles, fractional_odd_spacing, ccw) in;

// Sky-occlusion map (set 1 / binding 9), declared here rather than pulled in
// with light_ubo.glsl — that header carries functions using fragment-only
// builtins and will not compile in this stage. The matrix that goes with it
// rides the water UBO (W.rainVP). Without this the geometry would keep heaving
// under a roof while the shading went flat, and the surface would disagree with
// itself about where the wave is.
layout(set = 1, binding = 9) uniform sampler2D uRainMap;

layout(location = 0) in vec3 tWorldPos[];
layout(location = 1) in vec3 tNormal[];
layout(location = 2) in vec2 tUV[];

layout(push_constant) uniform PC {
    mat4 mvp;
    // PER-SURFACE, unlike everything in the UBO: x = the span of THIS body of
    // water in metres (0 = open water). The fragment stage declares the same
    // block — the displaced geometry and the shaded normal have to agree about
    // which octaves exist here, or a cellar would be flat to the light and still
    // heaving in silhouette.
    vec4 basin;
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

vec3 bary3(vec3 a, vec3 b, vec3 c) { return gl_TessCoord.x * a + gl_TessCoord.y * b + gl_TessCoord.z * c; }
vec2 bary2(vec2 a, vec2 b, vec2 c) { return gl_TessCoord.x * a + gl_TessCoord.y * b + gl_TessCoord.z * c; }

void main()
{
    vec3 wp = bary3(tWorldPos[0], tWorldPos[1], tWorldPos[2]);
    vNormal = normalize(bary3(tNormal[0], tNormal[1], tNormal[2]));
    vUV     = bary2(tUV[0], tUV[1], tUV[2]);

    float dist = distance(wp, W.p8.xyz);
    // Same fade the tessellation factor uses, so displacement dies exactly
    // where the vertices that carry it do.
    float fade = clamp((W.p5.z - dist) / max(W.p5.z - W.p5.y, 0.01), 0.0, 1.0);
    if (W.p5.w > 0.0 && fade > 0.0) {
        float calm = waterShelter(uRainMap, wp);
        // The body under THIS vertex, not the bounding box of the mesh it came
        // in on — one visual carries every pool on the level. Same call, same
        // answer, in every stage that touches the wave.
        float fetch = waterLocalFetch(wp.xz, pc.basin.x);
        vec4 f = waterField(wp.xz, dist, fetch, calm);
        wp.y += f.x * W.p5.w * fade * fade;
        // CHOPPINESS. The one thing this stage could never do before: the crest
        // is sharp because the water either side of it moves INWARD, and that is
        // a horizontal move of the vertex, not a taller vertical one. Faded with
        // the same square as the height so the LOD seam stays invisible.
        wp.xz += waterFFTChop(wp.xz, fetch, calm) * (fade * fade);
    }

    vWorldPos   = wp;
    gl_Position = pc.mvp * vec4(wp, 1.0);
}

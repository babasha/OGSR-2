#version 450
#extension GL_GOOGLE_include_directive : require
#include "light_ubo.glsl"      // L.eye_pos (camera), L.sf_params.w (snow), L.deform_count (prints)
#include "snow_displace.glsl"  // SnowDeformPressBroad (mud: press-gated subdivision)

// World pass - TERRAIN tessellation control. Pass-through control points +
// distance-adaptive edge factors, GATED by snow presence: only subdivide where
// there is snow AND footprints to deform (else level 1 = no extra cost). No PN
// (terrain is already a heightfield); flat subdivision is enough to resolve the
// ~20 cm footprint depressions the coarse base mesh can't. Edge factors come from
// the two endpoints of each edge, so adjacent patches agree -> crack-free.

layout(vertices = 3) out;

layout(location = 0) in vec2 vUV[];
layout(location = 1) in vec2 vDetailUV[];
layout(location = 2) in vec2 vLmapUV[];
layout(location = 3) in vec3 vWorldPos[];
layout(location = 4) in vec3 vNormal[];

layout(location = 0) out vec2 tUV[];
layout(location = 1) out vec2 tDetailUV[];
layout(location = 2) out vec2 tLmapUV[];
layout(location = 3) out vec3 tWorldPos[];
layout(location = 4) out vec3 tNormal[];

layout(push_constant) uniform PushConstants {
    mat4  mvp;
    vec2  uvScale;
    float alphaRef;
    float detailScale;
} pc;

const float SNOW_TESS_MAX  = 28.0;   // max subdivision at point-blank range (finer = smoother dents)
const float SNOW_TESS_NEAR = 8.0;    // full-factor distance (m)
const float SNOW_TESS_FAR  = 32.0;   // factor reaches 1 here (footprints fade out beyond)

float edgeFactor(vec3 a, vec3 b) {
    float d = distance(0.5 * (a + b), L.eye_pos.xyz);
    float t = clamp((SNOW_TESS_FAR - d) / max(SNOW_TESS_FAR - SNOW_TESS_NEAR, 0.01), 0.0, 1.0);
    return max(1.0, SNOW_TESS_MAX * t);
}

// Any press (dent OR berm) along an edge? Sampled at the endpoints + midpoint with
// the BROAD kernel. EDGE-symmetric by construction: both neighbouring patches test
// the SAME three points -> agree on the factor -> crack-free press gating.
bool edgePress(vec3 a, vec3 b) {
    return abs(SnowDeformPressBroad(a)) > 0.02
        || abs(SnowDeformPressBroad(b)) > 0.02
        || abs(SnowDeformPressBroad(0.5 * (a + b))) > 0.02;
}

void main() {
    tUV[gl_InvocationID]       = vUV[gl_InvocationID];
    tDetailUV[gl_InvocationID] = vDetailUV[gl_InvocationID];
    tLmapUV[gl_InvocationID]   = vLmapUV[gl_InvocationID];
    tWorldPos[gl_InvocationID] = vWorldPos[gl_InvocationID];
    tNormal[gl_InvocationID]   = vNormal[gl_InvocationID];
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

    if (gl_InvocationID == 0) {
        // Subdivide only where there is something to carve. SNOW: coverage present ->
        // distance-adaptive everywhere near (prints + drifts). MUD (texture mode, no
        // snow needed): per-EDGE press test so only patches actually containing prints
        // pay for tessellation. MESH mode (deform_tex.x>=2): the snow mesh owns the
        // dents -> terrain stays flat.
        bool texMode = L.deform_tex.x > 0.5 && L.deform_tex.x < 1.5;
        bool snowOn  = L.sf_params.w > 0.01 && L.deform_count.w > 0.5 && L.deform_tex.x < 1.5;
        bool mudOn   = texMode && L.pom_params5.z > 0.0;
        if (!snowOn && !mudOn) {
            gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = 1.0;
            gl_TessLevelInner[0] = 1.0;
        } else if (snowOn) {
            gl_TessLevelOuter[0] = edgeFactor(vWorldPos[1], vWorldPos[2]);
            gl_TessLevelOuter[1] = edgeFactor(vWorldPos[2], vWorldPos[0]);
            gl_TessLevelOuter[2] = edgeFactor(vWorldPos[0], vWorldPos[1]);
            gl_TessLevelInner[0] = max(gl_TessLevelOuter[0], max(gl_TessLevelOuter[1], gl_TessLevelOuter[2]));
        } else {
            gl_TessLevelOuter[0] = edgePress(vWorldPos[1], vWorldPos[2]) ? edgeFactor(vWorldPos[1], vWorldPos[2]) : 1.0;
            gl_TessLevelOuter[1] = edgePress(vWorldPos[2], vWorldPos[0]) ? edgeFactor(vWorldPos[2], vWorldPos[0]) : 1.0;
            gl_TessLevelOuter[2] = edgePress(vWorldPos[0], vWorldPos[1]) ? edgeFactor(vWorldPos[0], vWorldPos[1]) : 1.0;
            // Interior-only prints (all edges clean): inner subdivision alone is
            // crack-free — border edges have no interior verts on either side.
            vec3  c      = (vWorldPos[0] + vWorldPos[1] + vWorldPos[2]) / 3.0;
            float inner  = max(gl_TessLevelOuter[0], max(gl_TessLevelOuter[1], gl_TessLevelOuter[2]));
            if (abs(SnowDeformPressBroad(c)) > 0.02) inner = max(inner, edgeFactor(c, c));
            gl_TessLevelInner[0] = inner;
        }
    }
}

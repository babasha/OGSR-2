// World pass — tessellation control, BOTH static-lighting variants
// (-DWORLD_VLIT picks vert-lit). R4 TESS_PN+HM port, r3\dx11\tess.hs.
// Pass-through control points + distance-adaptive edge factors + the
// PN-triangle patch (cubic position / quadratic normal control points,
// tess.h ComputePNPatch). Edge factors are computed symmetrically from the
// two endpoints of each edge, so adjacent patches always agree → crack-free
// borders. ONLY the pass-through interface differs between the variants —
// the factor and patch maths are identical, so they live here once.

layout(vertices = 3) out;

// Must stay in lockstep with world_vert_body.glsl's outputs and
// world_tese_body.glsl's inputs.
#ifdef WORLD_VLIT
layout(location = 0) in vec2  vUV[];
layout(location = 1) in vec2  vDetailUV[];
layout(location = 2) in vec3  vBakedColor[];
layout(location = 3) in float vSunMask[];
layout(location = 4) in vec3  vWorldPos[];
layout(location = 5) in vec3  vNormal[];
layout(location = 7) in float vBakedHemi[];   // baked sky access (NORMAL.a)

layout(location = 0) out vec2  tUV[];
layout(location = 1) out vec2  tDetailUV[];
layout(location = 2) out vec3  tBakedColor[];
layout(location = 3) out float tSunMask[];
layout(location = 4) out vec3  tWorldPos[];
layout(location = 5) out vec3  tNormal[];
layout(location = 7) out float tBakedHemi[];
#else
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
#endif

// PN-triangle control points (world space). Per-patch interface — separate
// location space from the per-vertex outputs, but numbered above them anyway.
layout(location = 8)  patch out vec3 pnB210;
layout(location = 9)  patch out vec3 pnB120;
layout(location = 10) patch out vec3 pnB021;
layout(location = 11) patch out vec3 pnB012;
layout(location = 12) patch out vec3 pnB102;
layout(location = 13) patch out vec3 pnB201;
layout(location = 14) patch out vec3 pnB111;
layout(location = 15) patch out vec3 pnN110;
layout(location = 16) patch out vec3 pnN011;
layout(location = 17) patch out vec3 pnN101;

layout(push_constant) uniform PushConstants {
    mat4  mvp;
    vec2  uvScale;
    float alphaRef;
    float detailScale;
    float dynHemi;
    float tessMax;     // max subdivision at point-blank range (0 = off)
    float tessNear;    // full-factor distance (m)
    float tessFar;     // factor reaches 1 / displacement fades out (m)
    vec4  eyeHeight;   // xyz = camera world pos, w = displacement amplitude (m)
    float pnScale;     // PN-triangle curvature amount (0 = off, 1 = full R4)
} pc;

float edgeFactor(vec3 a, vec3 b)
{
    float d = distance(0.5 * (a + b), pc.eyeHeight.xyz);
    float t = clamp((pc.tessFar - d) / max(pc.tessFar - pc.tessNear, 0.01), 0.0, 1.0);
    return max(1.0, pc.tessMax * t);
}

void main()
{
#ifdef WORLD_VLIT
    tUV[gl_InvocationID]         = vUV[gl_InvocationID];
    tDetailUV[gl_InvocationID]   = vDetailUV[gl_InvocationID];
    tBakedColor[gl_InvocationID] = vBakedColor[gl_InvocationID];
    tSunMask[gl_InvocationID]    = vSunMask[gl_InvocationID];
    tWorldPos[gl_InvocationID]   = vWorldPos[gl_InvocationID];
    tNormal[gl_InvocationID]     = vNormal[gl_InvocationID];
    tBakedHemi[gl_InvocationID]  = vBakedHemi[gl_InvocationID];
#else
    tUV[gl_InvocationID]       = vUV[gl_InvocationID];
    tDetailUV[gl_InvocationID] = vDetailUV[gl_InvocationID];
    tLmapUV[gl_InvocationID]   = vLmapUV[gl_InvocationID];
    tWorldPos[gl_InvocationID] = vWorldPos[gl_InvocationID];
    tNormal[gl_InvocationID]   = vNormal[gl_InvocationID];
#endif
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

    if (gl_InvocationID == 0) {
        // gl_TessLevelOuter[i] controls the edge OPPOSITE control point i.
        gl_TessLevelOuter[0] = edgeFactor(vWorldPos[1], vWorldPos[2]);
        gl_TessLevelOuter[1] = edgeFactor(vWorldPos[2], vWorldPos[0]);
        gl_TessLevelOuter[2] = edgeFactor(vWorldPos[0], vWorldPos[1]);
        gl_TessLevelInner[0] = max(gl_TessLevelOuter[0],
                               max(gl_TessLevelOuter[1], gl_TessLevelOuter[2]));

        // R4 ComputePNPatch. Edge control points project the 1/3 points onto
        // the corner tangent planes; the quadratic normal coefficients guard
        // against degenerate (zero-length) edges, which R4 didn't.
        if (pc.pnScale > 0.0) {
            vec3 P0 = vWorldPos[0], P1 = vWorldPos[1], P2 = vWorldPos[2];
            vec3 N0 = normalize(vNormal[0]);
            vec3 N1 = normalize(vNormal[1]);
            vec3 N2 = normalize(vNormal[2]);

            pnB210 = (2.0 * P0 + P1 - dot(P1 - P0, N0) * N0) / 3.0;
            pnB120 = (2.0 * P1 + P0 - dot(P0 - P1, N1) * N1) / 3.0;
            pnB021 = (2.0 * P1 + P2 - dot(P2 - P1, N1) * N1) / 3.0;
            pnB012 = (2.0 * P2 + P1 - dot(P1 - P2, N2) * N2) / 3.0;
            pnB102 = (2.0 * P2 + P0 - dot(P0 - P2, N2) * N2) / 3.0;
            pnB201 = (2.0 * P0 + P2 - dot(P2 - P0, N0) * N0) / 3.0;
            vec3 E = (pnB210 + pnB120 + pnB021 + pnB012 + pnB102 + pnB201) / 6.0;
            vec3 V = (P0 + P1 + P2) / 3.0;
            pnB111 = E + (E - V) * 0.5;

            float fV12 = 2.0 * dot(P1 - P0, N0 + N1) / max(dot(P1 - P0, P1 - P0), 1e-8);
            pnN110 = normalize(N0 + N1 - fV12 * (P1 - P0));
            float fV23 = 2.0 * dot(P2 - P1, N1 + N2) / max(dot(P2 - P1, P2 - P1), 1e-8);
            pnN011 = normalize(N1 + N2 - fV23 * (P2 - P1));
            float fV31 = 2.0 * dot(P0 - P2, N2 + N0) / max(dot(P0 - P2, P0 - P2), 1e-8);
            pnN101 = normalize(N2 + N0 - fV31 * (P0 - P2));
        } else {
            pnB210 = pnB120 = pnB021 = pnB012 = pnB102 = pnB201 = pnB111 = vec3(0.0);
            pnN110 = pnN011 = pnN101 = vec3(0.0);
        }
    }
}

// World pass — tessellation evaluation, BOTH static-lighting variants
// (-DWORLD_VLIT picks vert-lit). Only the interpolated interface differs; the
// PN+HM displacement and crack-avoidance scheme below are shared. R4 port,
// r3\dx11\tess.ds + tess.h). PN-triangle smoothing (cubic Bezier position,
// quadratic normal) followed by heightmap displacement along that normal;
// height comes from the bump-error texture's alpha (`<bump>#.dds`).
//
// Crack avoidance is R4's scheme: vertices ON the patch border never move,
// and interior vertices within 10% barycentric of an edge are snapped ONTO
// the edge before displacing — the displacement "rim" then lands on the
// shared edge from both sides, so neighbouring patches (and neighbouring
// materials) meet without holes.
//
// The depth prepass runs this exact module too (same SPIR-V, no fragment
// stage), so the prepass depth matches the displaced color geometry
// bit-for-bit — that is what makes inward PN curvature safe under the
// LOAD+LEQUAL color pass. The shading normal stays the LINEAR interpolation
// (R4 parity: tess.ds leaves M1..M3 untouched); the PN normal only steers
// the displacement.

layout(triangles, fractional_odd_spacing, ccw) in;

layout(set = 0, binding = 3) uniform sampler2D uTexBumpX;   // .a = height

// Must stay in lockstep with world_tesc_body.glsl's outputs and
// world_frag_body.glsl's inputs.
#ifdef WORLD_VLIT
layout(location = 0) in vec2  tUV[];
layout(location = 1) in vec2  tDetailUV[];
layout(location = 2) in vec3  tBakedColor[];
layout(location = 3) in float tSunMask[];
layout(location = 4) in vec3  tWorldPos[];
layout(location = 5) in vec3  tNormal[];
layout(location = 7) in float tBakedHemi[];
#else
layout(location = 0) in vec2 tUV[];
layout(location = 1) in vec2 tDetailUV[];
layout(location = 2) in vec2 tLmapUV[];
layout(location = 3) in vec3 tWorldPos[];
layout(location = 4) in vec3 tNormal[];
#endif

layout(location = 8)  patch in vec3 pnB210;
layout(location = 9)  patch in vec3 pnB120;
layout(location = 10) patch in vec3 pnB021;
layout(location = 11) patch in vec3 pnB012;
layout(location = 12) patch in vec3 pnB102;
layout(location = 13) patch in vec3 pnB201;
layout(location = 14) patch in vec3 pnB111;
layout(location = 15) patch in vec3 pnN110;
layout(location = 16) patch in vec3 pnN011;
layout(location = 17) patch in vec3 pnN101;

#ifdef WORLD_VLIT
layout(location = 0) out vec2  vUV;
layout(location = 1) out vec2  vDetailUV;
layout(location = 2) out vec3  vBakedColor;
layout(location = 3) out float vSunMask;
layout(location = 4) out vec3  vWorldPos;
layout(location = 5) out vec3  vNormal;
layout(location = 7) out float vBakedHemi;
#else
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec2 vDetailUV;
layout(location = 2) out vec2 vLmapUV;
layout(location = 3) out vec3 vWorldPos;
layout(location = 4) out vec3 vNormal;
#endif

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

void main()
{
    vec3 uvw = gl_TessCoord;
    // 0 ⇔ this vertex lies exactly on the patch border (tessellator edge
    // verts get exact-zero coords) — those stay pinned to the flat surface.
    float edge0 = min(uvw.x, min(uvw.y, uvw.z));

    // R4 redistribution: interior verts within 10% barycentric of an edge
    // snap exactly ONTO the edge (fK solves for the smallest component = 0).
    if (edge0 != 0.0 && ((1.0 / 3.0) - edge0) > 0.01) {
        float fK = (edge0 < 0.1) ? (1.0 / 3.0) / ((1.0 / 3.0) - edge0) : 1.0;
        uvw = mix(vec3(1.0 / 3.0), uvw, fK);
    }

#ifdef WORLD_VLIT
    vUV         = tUV[0]         * uvw.x + tUV[1]         * uvw.y + tUV[2]         * uvw.z;
    vDetailUV   = tDetailUV[0]   * uvw.x + tDetailUV[1]   * uvw.y + tDetailUV[2]   * uvw.z;
    vBakedColor = tBakedColor[0] * uvw.x + tBakedColor[1] * uvw.y + tBakedColor[2] * uvw.z;
    vSunMask    = tSunMask[0]    * uvw.x + tSunMask[1]    * uvw.y + tSunMask[2]    * uvw.z;
    vBakedHemi  = tBakedHemi[0]  * uvw.x + tBakedHemi[1]  * uvw.y + tBakedHemi[2]  * uvw.z;
#else
    vUV       = tUV[0]       * uvw.x + tUV[1]       * uvw.y + tUV[2]       * uvw.z;
    vDetailUV = tDetailUV[0] * uvw.x + tDetailUV[1] * uvw.y + tDetailUV[2] * uvw.z;
    vLmapUV   = tLmapUV[0]   * uvw.x + tLmapUV[1]   * uvw.y + tLmapUV[2]   * uvw.z;
#endif
    vec3 wpLinear = tWorldPos[0] * uvw.x + tWorldPos[1] * uvw.y + tWorldPos[2] * uvw.z;
    vec3 N    = normalize(tNormal[0] * uvw.x + tNormal[1] * uvw.y + tNormal[2] * uvw.z);
    vNormal   = N;   // shading normal stays linear (R4 parity)

    float d    = distance(wpLinear, pc.eyeHeight.xyz);
    float fade = clamp((pc.tessFar - d) / max(pc.tessFar - pc.tessNear, 0.01), 0.0, 1.0);

    vec3 wp    = wpLinear;
    vec3 Ndisp = N;   // displacement direction (PN quadratic normal when PN is on)

    // R4 ComputePatchVertex: cubic Bezier position + quadratic normal.
    // Faded by distance so the curvature melts to flat at tessFar (no pop).
    if (pc.pnScale > 0.0) {
        float x = uvw.x, y = uvw.y, z = uvw.z;
        vec3 nN0 = normalize(tNormal[0]);
        vec3 nN1 = normalize(tNormal[1]);
        vec3 nN2 = normalize(tNormal[2]);
        vec3 pnPos = tWorldPos[0] * (x * x * x) + tWorldPos[1] * (y * y * y) + tWorldPos[2] * (z * z * z)
                   + pnB210 * (3.0 * x * x * y) + pnB120 * (3.0 * x * y * y)
                   + pnB021 * (3.0 * y * y * z) + pnB012 * (3.0 * y * z * z)
                   + pnB102 * (3.0 * x * z * z) + pnB201 * (3.0 * x * x * z)
                   + pnB111 * (6.0 * x * y * z);
        vec3 pnN = nN0 * (x * x) + nN1 * (y * y) + nN2 * (z * z)
                 + pnN110 * (x * y) + pnN011 * (y * z) + pnN101 * (x * z);
        float k = pc.pnScale * fade;
        wp    = mix(wpLinear, pnPos, k);
        Ndisp = normalize(mix(N, pnN, k));
    }

    if (edge0 != 0.0 && pc.tessMax > 0.0) {
        float h = textureLod(uTexBumpX, vUV, 0.0).a;
        wp += Ndisp * (h * pc.eyeHeight.w * fade);
    }

    if (edge0 == 0.0) wp = wpLinear;   // border verts pinned flat (R4 anti-crack)

    vWorldPos   = wp;
    gl_Position = pc.mvp * vec4(wp, 1.0);
}

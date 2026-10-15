#version 450
#extension GL_GOOGLE_include_directive : require
#include "light_ubo.glsl"      // L (eye_pos, snow, deform stamps)
#include "snow_displace.glsl"  // SnowFootprintCarve

// World pass - TERRAIN tessellation evaluation. The control points already carry
// the COARSE snow VOLUME (the VS displaced them by SnowDisplace); here we subdivide
// and add the FINE footprint detail (depression + ridge) the coarse mesh can't show.
// Crack avoidance (R4): border verts stay pinned to the linear surface, and interior
// verts within 10% barycentric of an edge snap onto it, so neighbouring patches meet
// without holes. The depth-prepass terrain pipeline runs this exact module too (same
// SPIR-V, no FS) -> prepass depth matches the color geometry -> no z-fight.

layout(triangles, fractional_odd_spacing, ccw) in;

layout(set = 0, binding = 1) uniform sampler2D uMask;   // splat weights: soil softness for the mud carve

layout(location = 0) in vec2 tUV[];
layout(location = 1) in vec2 tDetailUV[];
layout(location = 2) in vec2 tLmapUV[];
layout(location = 3) in vec3 tWorldPos[];
layout(location = 4) in vec3 tNormal[];

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec2 vDetailUV;
layout(location = 2) out vec2 vLmapUV;
layout(location = 3) out vec3 vWorldPos;
layout(location = 4) out vec3 vNormal;

layout(push_constant) uniform PushConstants {
    mat4  mvp;
    vec2  uvScale;
    float alphaRef;
    float detailScale;
} pc;

const float SNOW_TESS_FAR = 32.0;

void main() {
    vec3 uvw = gl_TessCoord;
    float edge0 = min(uvw.x, min(uvw.y, uvw.z));
    // R4 redistribution: interior verts within 10% barycentric of an edge snap onto it.
    if (edge0 != 0.0 && ((1.0 / 3.0) - edge0) > 0.01) {
        float fK = (edge0 < 0.1) ? (1.0 / 3.0) / ((1.0 / 3.0) - edge0) : 1.0;
        uvw = mix(vec3(1.0 / 3.0), uvw, fK);
    }

    vUV       = tUV[0]       * uvw.x + tUV[1]       * uvw.y + tUV[2]       * uvw.z;
    vDetailUV = tDetailUV[0] * uvw.x + tDetailUV[1] * uvw.y + tDetailUV[2] * uvw.z;
    vLmapUV   = tLmapUV[0]   * uvw.x + tLmapUV[1]   * uvw.y + tLmapUV[2]   * uvw.z;
    vec3 wp   = tWorldPos[0] * uvw.x + tWorldPos[1] * uvw.y + tWorldPos[2] * uvw.z;
    vec3 N    = normalize(tNormal[0] * uvw.x + tNormal[1] * uvw.y + tNormal[2] * uvw.z);
    vNormal   = N;

    // Footprint carve (fine detail on top of the coarse snow surface). Faded by
    // distance. Carve ALL verts INCLUDING patch borders: the carve is a pure function
    // of world XZ (mask is macro-res, identical along a shared edge) and neighbouring
    // patches generate identical shared-edge verts -> crack-free. Pinning borders flat
    // while the interior dipped was what produced the sliver SPIKES.
    // MESH mode (deform_tex.x>=2): the dense snow mesh owns the dents -> terrain flat.
    if (L.deform_tex.x > 0.5 && L.deform_tex.x < 1.5) {
        // Texture path: REAL geometric deformation for snow AND bare-ground mud.
        float d    = distance(wp, L.eye_pos.xyz);
        float fade = clamp((SNOW_TESS_FAR - d) / max(SNOW_TESS_FAR - 1.0, 0.01), 0.0, 1.0);
        float pb   = SnowDeformPressBroad(wp);
        if (abs(pb) > 1e-3 && fade > 0.0) {
            float dentP   = max(pb, 0.0);
            float bermP   = max(-pb, 0.0);
            float snowCov = clamp(L.sf_params.w, 0.0, 1.0);
            float snowD   = (snowCov > 0.01 && L.deform_count.w > 0.5) ? L.deform_tex.z : 0.0;
            // Mud: the SOIL itself is pressed in — depth scales with the splat
            // softness (earth/grass deep, gravel shallow, asphalt none) and rain
            // loosens it. Berm boosted: displaced soil piles visibly at the rim.
            vec4  m  = textureLod(uMask, terrainMaskUV(vUV, wp), 0.0);
            float ws = dot(m, vec4(1.0));
            m = (ws > 1e-4) ? (m / ws) : vec4(1.0, 0.0, 0.0, 0.0);
            float wet  = clamp(L.rain_params.y, 0.0, 1.0);
            float soft = dot(m, vec4(0.85, 0.0, 1.0, 0.35)) * (1.0 + wet * 0.8);
            float mudD = (1.0 - snowCov) * soft * (0.12 * L.pom_params7.z) * min(L.pom_params5.z, 2.0);
            float depthM = max(snowD, mudD);
            wp += N * ((bermP * 2.4 - dentP) * depthM * fade);
        }
    } else if (L.deform_tex.x < 0.5) {
        // Analytic stamp path (legacy, r_snow_deform_tex 0).
        float d    = distance(wp, L.eye_pos.xyz);
        float fade = clamp((SNOW_TESS_FAR - d) / max(SNOW_TESS_FAR - 1.0, 0.01), 0.0, 1.0);
        wp += N * (SnowFootprintCarve(wp.xz) * fade);
    }

    vWorldPos   = wp;
    gl_Position = pc.mvp * vec4(wp, 1.0);
}

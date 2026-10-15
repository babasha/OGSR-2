#version 450
#extension GL_GOOGLE_include_directive : require
#define ENV_SET 0
#include "light_ubo.glsl"       // L (UBO set0 b0) + uGround (b13) + uDeform (b20)
#include "snow_displace.glsl"   // SnowDepthAt, SnowDeformPress

// SNOW MESH (VHM-style): a dense player-centred grid rendered as the snow surface.
// Decoupled from the coarse terrain mesh, so it has enough vertices to show crisp,
// SMOOTH footprint dents (the coarse terrain + tessellation could not). It samples
// the clean ground-height map for the terrain it sits on, raises by the snow blanket,
// and presses dents from the persistent deform texture (vk_deform). Procedural grid
// (no VBO): gl_VertexIndex -> quad -> corner. Prepass-safe by being its own pass.

layout(push_constant) uniform PC {
    mat4  mvp;
    vec4  p0;   // x=center.x, y=center.z, z=half (m), w=N (grid dim)
    vec4  p1;   // x=maxDepth (m), y=eyeY, z=cellWorld (m), w=unused
} pc;

layout(location = 0) out vec3  vWorldPos;
layout(location = 1) out vec3  vNormal;
layout(location = 2) out float vEdge;
layout(location = 3) out float vSlope;   // local height-field steepness (walls/fences -> high)

// Terrain world-Y from the RAIN map (binding 9 — the ground map binding 13 comes out
// EMPTY; the rain map has statics+terrain, and no trees when it's not raining). The
// rain ortho: eye at pc.p1.y (= ShadowMap::RainEyeY, the redraw eye-Y so there is NO
// camera-drift error), zNear 1, zFar 350 -> worldY = eyeY - 1 - d*349.
float groundY(vec2 wxz) {
    // DEBUG (r_snow_mesh 2): ignore the map, lay the sheet flat ~at foot level (eyeY is
    // cam.y+100) so we can see whether the mesh draws at all.
    if (pc.p1.w > 0.5) return pc.p1.y - 101.7;
    vec4 c  = L.rain_vp * vec4(wxz.x, 0.0, wxz.y, 1.0);
    vec2 uv = c.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    float d = textureLod(uRainMap, clamp(uv, vec2(0.0), vec2(1.0)), 0.0).r;
    // Backstop: a cleared / off-map texel (d~1) would fling the vertex far down and
    // stretch a giant triangle. Clamp to a sane band below the eye.
    return clamp(pc.p1.y - 1.0 - d * 349.0, pc.p1.y - 150.0, pc.p1.y);
}

float edgeFadeAt(vec2 local, float halfM) {
    float r = max(abs(local.x), abs(local.y)) / max(halfM, 0.01);
    return 1.0 - smoothstep(0.82, 1.0, r);   // fade displacement->0 at the rim
}

// Full snow surface height (ground + blanket - dent), faded at the grid rim so it
// descends to bare terrain and meets the flat far snow without a seam.
float snowH(vec2 wxz, vec2 center, float halfM, float maxD) {
    float g       = groundY(wxz);
    float fade    = edgeFadeAt(wxz - center, halfM);
    float cov     = clamp(L.sf_params.w, 0.0, 1.0);
    float blanket = SnowDepthAt(vec3(wxz.x, 0.0, wxz.y), vec3(0.0, 1.0, 0.0), cov);
    float press   = SnowDeformPress(vec3(wxz.x, 0.0, wxz.y));
    return g + (blanket - press * maxD) * fade;
}

void main() {
    int N     = int(pc.p0.w + 0.5);
    int quads = N - 1;
    int q     = gl_VertexIndex / 6;
    int corner= gl_VertexIndex - q * 6;
    int qx    = q % quads;
    int qz    = q / quads;
    // two CCW triangles per quad: (0,0)(1,0)(0,1) and (1,0)(1,1)(0,1)
    ivec2 off = ivec2(0, 0);
    if      (corner == 1) off = ivec2(1, 0);
    else if (corner == 2) off = ivec2(0, 1);
    else if (corner == 3) off = ivec2(1, 0);
    else if (corner == 4) off = ivec2(1, 1);
    else if (corner == 5) off = ivec2(0, 1);
    ivec2 gi = ivec2(qx, qz) + off;

    vec2  center = pc.p0.xy;
    float halfM  = pc.p0.z;
    float maxD   = pc.p1.x;
    vec2  local  = (vec2(gi) / float(quads) - 0.5) * (2.0 * halfM);
    vec2  wxz    = center + local;

    // Position uses the exact height. The NORMAL uses a WIDE stencil (0.5 m) so it's the
    // smooth slope, not the coarse rain-map height texels (~15 cm) which otherwise show
    // up as an axis-aligned GRID. Fine dent/ripple detail is added per-pixel in the frag.
    const float ne = 0.5;
    float H  = snowH(wxz,                   center, halfM, maxD);
    float Hx = snowH(wxz + vec2(ne, 0.0),   center, halfM, maxD);
    float Hz = snowH(wxz + vec2(0.0, ne),   center, halfM, maxD);
    vNormal   = normalize(vec3(-(Hx - H) / ne, 1.0, -(Hz - H) / ne));
    vWorldPos = vec3(wxz.x, H, wxz.y);
    vEdge     = edgeFadeAt(local, halfM);
    // Steepness of the underlying surface. The rain-map height includes statics, so a
    // wall/fence makes a big jump here -> the fragment discards the snow (no draping over
    // walls). Footprint dents are shallow vs the 0.5 m stencil, so they stay under the gate.
    vSlope    = max(abs(Hx - H), abs(Hz - H)) / ne;
    gl_Position = pc.mvp * vec4(vWorldPos, 1.0);
}

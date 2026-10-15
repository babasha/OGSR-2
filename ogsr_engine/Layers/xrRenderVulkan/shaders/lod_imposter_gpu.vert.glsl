#version 450
// xrRenderVulkan — GPU LOD imposter vertex shader (r_lods_gpu): vertex pulling.
// No vertex input; gl_VertexIndex enumerates 6 verts per visible quad appended
// by lod_cull.comp. Corners come straight from the LodEntry SSBO; the camera-ward
// half-radius shift (pulls the flat billboard out of its own tree mesh) is
// recomputed here from eye_pos — exactly what the CPU path baked per vertex.
// Outputs match lod_imposter.frag (reused unchanged).

struct LodFacet {
    vec4 n;                               // xyz facet normal
    vec4 c0; vec4 c1; vec4 c2; vec4 c3;   // corners: xyz world pos, w = atlas u
    vec4 vs;                              // atlas v for corners 0..3
};
struct LodEntry {
    vec4 sphere;                          // xyz centre, w radius
    LodFacet f[8];
};

// Set 0 = atlas (fragment); set 1 = pulled geometry.
layout(set = 1, binding = 0) readonly buffer Lods  { LodEntry lods[]; };
layout(set = 1, binding = 1) readonly buffer Insts { uint insts[]; };   // (lodIndex << 3) | facet

layout(push_constant) uniform PC {
    mat4 mViewProj;
    vec4 tint;
    vec4 fog_color;
    vec4 fog_params;
    vec4 eye_pos;
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vWPos;

// Quad v0,v1,v2 + v0,v2,v3 — same emit order as the CPU path (cull NONE).
const int kTri[6] = int[6](0, 1, 2, 0, 2, 3);

void main()
{
    uint vi     = uint(gl_VertexIndex);
    uint packed = insts[vi / 6u];
    uint l      = packed >> 3u;
    uint fi     = packed & 7u;
    int  corner = kTri[vi % 6u];

    vec4  s     = lods[l].sphere;
    vec3  toObj = s.xyz - pc.eye_pos.xyz;
    vec3  shift = (toObj / max(length(toObj), 1e-3)) * (-0.5 * s.w);   // pull toward camera

    LodFacet F = lods[l].f[fi];
    vec4 cs[4] = vec4[4](F.c0, F.c1, F.c2, F.c3);
    vec4 cc    = cs[corner];

    vec3 pos = cc.xyz + shift;
    gl_Position = pc.mViewProj * vec4(pos, 1.0);
    vUV    = vec2(cc.w, F.vs[corner]);
    // nfs23: R1 crossfade -- alpha 0 at D_A (where lod_cull starts the billboard) to 1 at D_A * band (where the
    // group's meshes stop, tree_cull_v3). eye_pos.w = base D, tint.w = band (0 = none), fog_color.w = mean radius.
    float fade = 1.0;
    if (pc.tint.w > 1.0001) {
        float k = 1.0;
        if (pc.fog_color.w > 0.0) {
            k = clamp(sqrt(max(s.w, 0.0) / pc.fog_color.w), 0.6, 2.0);
            k = floor(k * 64.0 + 0.5) / 64.0;
        }
        const float dA = pc.eye_pos.w * k, dB = dA * pc.tint.w;
        fade = clamp((length(toObj) - dA) / max(dB - dA, 1e-3), 0.0, 1.0);
    }
    vColor = vec4(1.0, 1.0, 1.0, fade);   // atlas already carries baked colour (see CPU path)
    vWPos  = pos;
}

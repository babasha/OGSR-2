#version 450
#extension GL_GOOGLE_include_directive : require
// GPU LOD-imposter cull (r_lods_gpu) — replaces the CPU per-frame FLOD walk in
// CLODManager::Render. One thread per FLOD: distance gate (close = full mesh) →
// 6-plane frustum → Hi-Z occlusion (shared grass/world pyramid; same conservative
// math as world_cull_hzb.comp incl. the focal-scale footprint fix) → best-facing
// facet pick (8 dots) → append a packed {lod,facet} instance + bump the indirect
// vertexCount by 6. The draw side pulls the quad corners from the same LodEntry
// SSBO (lod_imposter_gpu.vert), so no per-frame CPU vertex building at all.
//
// LodEntry layout MUST match GpuLodEntry in vk_LODManager.cpp (784 B, std430).
#include "hzb_test.glsl"       // HZB_SphereOccluded — shared with world_cull_hzb / tree_cull
layout(local_size_x = 256) in;

struct LodFacet {
    vec4 n;                    // xyz facet normal (from OGF_LODDEF2, computed at load)
    vec4 c0; vec4 c1; vec4 c2; vec4 c3;   // corners: xyz world pos, w = atlas u
    vec4 vs;                   // atlas v for corners 0..3
};
struct LodEntry {
    vec4 sphere;               // xyz centre, w radius (vis.sphere)
    LodFacet f[8];
};

layout(set = 0, binding = 0) readonly  buffer Lods  { LodEntry lods[]; };
layout(set = 0, binding = 1) writeonly buffer Insts { uint insts[]; };   // (lodIndex << 3) | facet
struct DrawCmd { uint vertexCount; uint instanceCount; uint firstVertex; uint firstInstance; };
layout(set = 0, binding = 2) buffer Indirect { DrawCmd cmd; };           // reset to {0,1,0,0} pre-dispatch
layout(set = 0, binding = 3) uniform sampler2D u_HZB;   // Hi-Z pyramid (MAX depth, GENERAL); dummy when hzbOn=0

layout(push_constant) uniform PC {
    mat4 viewProj;          // X-Ray viewProj: viewProj * vec4(p,1) = clip
    vec4 frustumPlanes[6];  // CPU-extracted, normalized (VK::ExtractFrustumPlanes)
    vec4 cameraPos;         // xyz eye, w = imposter min distance (closer = skip)
    vec4 hzbParams;         // x = focal scale 1/tan(fovY/2); nfs21: y = mean FLOD radius (0 = no size LOD), zw = k clamp
    uint total;             // number of LodEntry records
    uint hzbOn;             // 1 = HZB pyramid valid this frame
    uint _p0; uint _p1;
} pc;

void main()
{
    uint l = gl_GlobalInvocationID.x;
    if (l >= pc.total) return;

    vec4  s = lods[l].sphere;
    vec3  c = s.xyz;
    float r = s.w;

    // ---- Distance gate: near FLODs are drawn as full meshes by the tree pass ----
    vec3  d    = c - pc.cameraPos.xyz;
    float dist = length(d);
    // nfs21: R1-style size-scaled handover -- the same quantised k the trees of this group are cut with (tree_cull).
    float minD = pc.cameraPos.w;
    if (pc.hzbParams.y > 0.0) {
        float k = clamp(sqrt(max(r, 0.0) / pc.hzbParams.y), pc.hzbParams.z, pc.hzbParams.w);
        minD *= floor(k * 64.0 + 0.5) / 64.0;
    }
    if (dist < minD) return;

    // ---- Frustum cull (6 planes vs sphere) ----
    for (int i = 0; i < 6; ++i)
        if (dot(pc.frustumPlanes[i].xyz, c) + pc.frustumPlanes[i].w < -r) return;

    // ---- Hi-Z occlusion cull (shared test, hzb_test.glsl) ----
    // Imposters are the far half of the tree set, so this is where occlusion has
    // the most to remove — and also where the unclamped mip used to bite: an
    // imposter close to kImposterMinDist with a big crown sphere drove the mip
    // past the end of the pyramid (undefined texelFetch).
    if (pc.hzbOn != 0u
        && HZB_SphereOccluded(u_HZB, pc.viewProj, c, r, pc.cameraPos.xyz, pc.hzbParams.x))
        return;   // fully behind the farthest surface in its footprint

    // ---- Best-facing facet: max dot(camera→object, facet normal) ----
    vec3 Ldir = d / max(dist, 1e-3);
    uint best = 0u; float bestDot = -1e9;
    for (uint i = 0u; i < 8u; ++i) {
        float dt = dot(Ldir, lods[l].f[i].n.xyz);
        if (dt > bestDot) { bestDot = dt; best = i; }
    }

    // ---- Append: 6 verts per visible quad; slot = quad index ----
    uint slot = atomicAdd(cmd.vertexCount, 6u) / 6u;
    insts[slot] = (l << 3u) | best;
}

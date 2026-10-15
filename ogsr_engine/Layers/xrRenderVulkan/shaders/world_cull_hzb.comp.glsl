#version 450
#extension GL_GOOGLE_include_directive : require
// GPU-driven world forward culling WITH Hi-Z occlusion (Phase A of cluster cull).
// Same per-mesh frustum cull as world_cull.comp, plus an HZB occlusion test that
// drops meshes fully hidden behind nearer scene geometry — so the heavy forward
// COLOR pass skips them. The DEPTH PREPASS still draws the full frustum set (it
// builds the Hi-Z), so the pyramid is complete and this never over-culls.
//
// Output goes to a SEPARATE indirect/count region (cmds2/counts2) consumed only
// by DrawColor when r_hzb_cull is on; the depth prepass keeps the frustum set.
//
// Frustum planes + viewProj + cameraPos all travel in the push, mirroring the
// proven grass path in detail_generate.comp (same push shape, same HZB sample
// math: Y-flip, mip select by screen footprint, conservative 4-corner MAX, near
// face). Planes are the CPU-extracted, normalized set (VK::ExtractFrustumPlanes).
layout(local_size_x = 256) in;

#include "cluster_meta.glsl"   // struct Meta — matches VK::WorldGPU::GpuMeshMeta
#include "draw_cmd.glsl"       // struct Cmd — matches VkDrawIndexedIndirectCommand
#include "hzb_test.glsl"       // HZB_SphereOccluded — shared with tree_cull / lod_cull

layout(set = 0, binding = 0) readonly buffer Metas { Meta metas[]; };
layout(set = 0, binding = 1)          buffer Cmds  { Cmd  cmds[];  };
layout(set = 0, binding = 2)          buffer Count { uint counts[]; };
layout(set = 0, binding = 3) uniform sampler2D u_HZB;   // Hi-Z pyramid (MAX depth, GENERAL layout)
layout(set = 0, binding = 4) readonly buffer Bases { uint groupBase[]; };   // per-group cmd-region base
// Stage B page streaming — MUST mirror world_cull.comp (prepass and color sets
// have to pick the same cut). Requests/touched are emitted by world_cull only.
layout(set = 0, binding = 5) readonly buffer SBits { uint streamBits[]; };
layout(set = 0, binding = 6) readonly buffer SBase { uint pageSlotBase[]; };

layout(push_constant) uniform PC {
    mat4 viewProj;          // X-Ray viewProj uploaded so viewProj * vec4(p,1) = clip
    vec4 frustumPlanes[6];  // CPU-extracted, normalized (VK::ExtractFrustumPlanes)
    vec4 cameraPos;         // .xyz world camera position (near-face pull + LOD projection)
    vec4 viewDir;           // .xyz normalized camera forward (view-Z LOD projection)
    vec4 lodParams;         // x = pxScale/thresholdPx, y = min dist clamp, z = fade band, w = 1/tan(fovY/2)
    uint numGroups;
    float ssaCull;          // SSA cull: 2·pxScale/r_ssa_px, 0 = off (was unused0)
    uint total;             // number of cullable entries (meshes + r_cluster clusters)
    uint _pad;
} pc;

#include "world_cull_common.glsl"   // projErr + emitDrawCmd — the prepass and color culls MUST agree


void main()
{
    uint l = gl_GlobalInvocationID.x;
    if (l >= pc.total) return;

    Meta m = metas[l];
    vec3  c = m.sphere.xyz;
    float r = m.sphere.w;

    // ---- Frustum cull (6 planes vs sphere; same test as world_cull.comp) ----
    for (int i = 0; i < 6; ++i)
        if (dot(pc.frustumPlanes[i].xyz, c) + pc.frustumPlanes[i].w < -r) return;   // outside

    // ---- SSA cull (r_ssa_px) — MUST match world_cull.comp (see rationale there):
    // plain whole meshes (flags bit1) under the projected-diameter threshold.
    if (pc.ssaCull > 0.0 && (m.flags & 2u) != 0u) {
        float dz = max(pc.lodParams.y, dot(pc.viewDir.xyz, c - pc.cameraPos.xyz) - r);
        if (r * pc.ssaCull < dz) return;
    }

    // ---- Stage B residency (identical to world_cull.comp) ----
    uint sb = (streamBits[l >> 4u] >> ((l & 15u) * 2u)) & 3u;
    if ((sb & 1u) == 0u) return;
    bool sleaf = (sb & 2u) != 0u;

    // ---- LOD DAG cut (identical math to world_cull.comp incl. crossfade) ----
    float sp = projErr(m.lodSelf,   m.selfError);
    float pp = projErr(m.lodParent, m.parentError);
    float band = pc.lodParams.z;
    uint fA = 63u, fB = 0u;
    if (band <= 0.0 || (m.flags & 1u) != 0u) {
        if ((sp > 1.0 && !sleaf) || pp <= 1.0) return;
    } else {
        if (pp <= 1.0 || (sp > 1.0 + band && !sleaf)) return;
        fA = uint(clamp((pp - 1.0) / band, 0.0, 1.0) * 63.0 + 0.5);
        fB = sleaf ? 0u : uint(clamp((sp - 1.0) / band, 0.0, 1.0) * 63.0 + 0.5);
        if (fA == 0u) return;
    }

    // ---- Hi-Z occlusion cull (conservative; shared test, hzb_test.glsl) ----
    // MID-TRANSITION entries (crossfade) are EXEMPT: the prepass (frustum set)
    // draws the parent+children pair with complementary dither masks, and if the
    // color set occlusion-culls only ONE half of that pair (their spheres differ),
    // the other half's discarded pixels stay black → view-dependent dark/flickering
    // facades. Transitions live for fractions of a second — skipping their
    // occlusion test costs nothing.
    if (fA == 63u && fB == 0u
        && HZB_SphereOccluded(u_HZB, pc.viewProj, c, r, pc.cameraPos.xyz, pc.lodParams.w))
        return;   // fully behind the farthest surface in its footprint

    // ---- Visible: append the indirect draw command to the entry's group region ----
    emitDrawCmd(m, l, fA, fB);
}

#version 450
#extension GL_GOOGLE_include_directive : require
// GPU-driven world forward culling. One invocation per cullable ENTRY — a whole
// static mesh or, when r_cluster split it, one ~128-tri cluster of it (the shader
// doesn't distinguish; a cluster is just a small mesh) — sphere vs the camera's
// 6 frustum planes → atomically append a VkDrawIndexedIndirectCommand into the
// entry's material-group region. Regions are exact prefix sums (groupBase[g]),
// not uniform maxGroupMesh strides: clusters skew group sizes far too much.
// Mirrors world geometry being world-space (no transform): the draw needs only
// indexCount/firstIndex/vertexOffset. Single dispatch routes all groups via the
// per-entry group index (no per-group dispatch).
layout(local_size_x = 256) in;

#include "cluster_meta.glsl"   // struct Meta — matches VK::WorldGPU::GpuMeshMeta
#include "draw_cmd.glsl"       // struct Cmd — matches VkDrawIndexedIndirectCommand

layout(set = 0, binding = 0) readonly buffer Metas  { Meta metas[]; };
layout(set = 0, binding = 1)          buffer Cmds   { Cmd  cmds[];  };
layout(set = 0, binding = 2)          buffer Count  { uint counts[]; };
layout(set = 0, binding = 3) readonly buffer Bases  { uint groupBase[]; };   // per-group cmd-region base
// ---- Stage B page streaming (vk_cluster_stream) ----
// streamBits: 2 bits/entry packed 16-per-u32 — bit0 drawable (page + whole
// cohort resident), bit1 streaming leaf (children not fully resident: draw me
// even when the cut wants finer, and request the missing pages).
// pageSlotBase: per-page PAIRS — [2p] firstIndex base (ib_first is page-local),
// [2p+1] firstVertex base in stride units (slice 2: repacked entries carry
// page-local first_vertex; raw/identity pages keep base 0 = absolute).
layout(set = 0, binding = 4) readonly buffer SBits   { uint streamBits[]; };
layout(set = 0, binding = 5) readonly buffer SBase   { uint pageSlotBase[]; };
layout(set = 0, binding = 6)          buffer SReq    { uint reqCount; uint reqItems[]; };
layout(set = 0, binding = 7)          buffer STouch  { uint touched[]; };   // page bitset (LRU feedback)
const uint kReqCap = 4096u;   // = ClusterStream kMaxRequests

layout(push_constant) uniform PC {
    vec4 planes[6];
    vec4 cameraPos;      // .xyz world camera (LOD projection)
    vec4 viewDir;        // .xyz normalized camera forward (view-Z LOD projection)
    vec4 lodParams;      // x = pxScale/thresholdPx, y = min distance clamp
    uint numGroups;
    float ssaCull;       // SSA cull: 2·pxScale/r_ssa_px, 0 = off (was unused0)
    uint total;          // number of cullable entries
    uint _pad;
} pc;

#include "world_cull_common.glsl"   // projErr + emitDrawCmd — the prepass and color culls MUST agree


void main()
{
    uint l = gl_GlobalInvocationID.x;
    if (l >= pc.total) return;

    Meta m = metas[l];
    vec3 c = m.sphere.xyz;
    float r = m.sphere.w;
    for (int i = 0; i < 6; ++i)
        if (dot(pc.planes[i].xyz, c) + pc.planes[i].w < -r) return;   // outside this plane

    // SSA cull (r_ssa_px): plain WHOLE meshes (flags bit1) never cluster/LOD —
    // they rendered full geometry to the horizon, and that sub-pixel triangle
    // soup cost 8x in quad helpers (r_fsinv_split, 23-07-2026). Skip the mesh
    // once its sphere's projected DIAMETER drops under the threshold:
    // 2·r·pxScale/viewZ < ssaPx  ⟺  r·ssaCull < viewZ. Clusters are exempt —
    // a large surface's small clusters must not evaporate piecewise.
    if (pc.ssaCull > 0.0 && (m.flags & 2u) != 0u) {
        float dz = max(pc.lodParams.y, dot(pc.viewDir.xyz, c - pc.cameraPos.xyz) - r);
        if (r * pc.ssaCull < dz) return;
    }

    // ---- Stage B residency ----
    uint sb = (streamBits[l >> 4u] >> ((l & 15u) * 2u)) & 3u;
    if ((sb & 1u) == 0u) {
        // DIRECT DEMAND (17-07, white-polygon fix): parent-driven requests
        // cannot reach a cohort whose DAG links are degenerate — the self-loop
        // cut leaves stall-chain / merged-prop groups with NO parent to carry
        // a leaf bit, so they'd stay dead forever. If the cut NEEDS this level
        // here (parent too coarse, own error fine), name the entry ITSELF; the
        // CPU decode fetches its member cohort. Top bit = direct-request flag
        // (urgency is 7 bits on both request paths).
        float spd = projErr(m.lodSelf,   m.selfError);
        float ppd = projErr(m.lodParent, m.parentError);
        if (ppd > 1.0 && spd <= 1.0) {
            uint slot = atomicAdd(reqCount, 1u);
            if (slot < kReqCap)
                reqItems[slot] = (l & 0xFFFFFFu) | 0x80000000u | (uint(clamp(ppd * 8.0, 0.0, 127.0)) << 24);
        }
        return;                         // a coarser ancestor covers meanwhile (when one exists)
    }
    bool sleaf = (sb & 2u) != 0u;       // children missing → act as the cut leaf

    // ---- LOD DAG cut (with optional crossfade band, pc.lodParams.z) ----
    // Hard cut: draw iff selfProj <= 1 < parentProj. With a band, visibility
    // widens to [parentProj > 1, selfProj <= 1+band] and two 6-bit fades ride
    // in firstInstance: the fragment keeps fadeOut <= dither < fadeIn — the
    // parent's fadeOut equals its children's fadeIn (same sphere/error inputs),
    // so the screen-door masks are exactly complementary. Alpha-tested
    // materials (flags bit0) always hard-cut: their depth pipeline can't dither.
    // A streaming LEAF ignores the "descend" side (children can't show) and
    // draws fully opaque (fB = 0) — the absent children discard nothing.
    float sp = projErr(m.lodSelf,   m.selfError);
    float pp = projErr(m.lodParent, m.parentError);

    // Page request: this entry's level is in the cut (pp > 1) and we'll want
    // the children soon (0.7 prefetch margin) but they aren't resident. CPU
    // maps entry id -> child group -> missing pages. Clamp BEFORE the uint
    // conversion (float->uint of a huge value is undefined in GLSL).
    if (sleaf && sp > 0.7 && pp > 1.0) {
        uint slot = atomicAdd(reqCount, 1u);
        if (slot < kReqCap)
            reqItems[slot] = (l & 0xFFFFFFu) | (uint(clamp(sp * 32.0, 0.0, 127.0)) << 24);
    }

    float band = pc.lodParams.z;
    uint fA = 63u, fB = 0u;
    if (band <= 0.0 || (m.flags & 1u) != 0u) {
        if ((sp > 1.0 && !sleaf) || pp <= 1.0) return;
    } else {
        if (pp <= 1.0 || (sp > 1.0 + band && !sleaf)) return;
        fA = uint(clamp((pp - 1.0) / band, 0.0, 1.0) * 63.0 + 0.5);
        fB = sleaf ? 0u : uint(clamp((sp - 1.0) / band, 0.0, 1.0) * 63.0 + 0.5);
        if (fA == 0u) return;   // not visible yet
    }

    atomicOr(touched[m._p1 >> 5u], 1u << (m._p1 & 31u));   // page LRU feedback

    emitDrawCmd(m, l, fA, fB);
}

#version 450
#extension GL_GOOGLE_include_directive : require
// GPU-driven world culling, TWO-PHASE occlusion (r_hzb_prepass).
//
// WHAT PROBLEM THIS SOLVES. The single-phase path can only occlusion-cull the
// COLOUR pass: its pyramid is a MAX reduction of the FINISHED depth prepass, so
// the prepass itself must draw the full frustum set (it IS the pyramid's source).
// Measured on pripyat_full, that means the prepass rasters 5069 entries where
// 2755 are visible — 45% waste. Two-phase breaks the circle with LAST frame's
// pyramid:
//
//   EARLY : cull vs the PREVIOUS frame's Hi-Z (+ that frame's viewProj).
//           Survivors -> draw set A -> depth prepass part 1.
//           Everything the test REJECTED is appended to a compact LATE LIST
//           instead of being dropped.
//   (CPU rebuilds the pyramid from the partial depth)
//   LATE  : re-test ONLY the late list vs the fresh pyramid, via
//           vkCmdDispatchIndirect over the group count this shader accumulated.
//           Survivors -> draw set B -> depth prepass part 2.
//
// WHY IT CANNOT PUNCH HOLES. Correctness does NOT depend on the previous frame's
// pyramid or camera being right — only on EVERY early reject being re-tested. A
// stale/wrong prev-frame guess costs efficiency (an entry lands in phase 2 that
// could have been in phase 1, or vice versa) and nothing else. The late list is
// the only thing that must not lose an entry, so an overflowing append falls back
// to drawing the entry immediately in phase 1. Over-cull is a visible hole;
// under-cull is only cost — every doubt resolves toward drawing.
//
// WHY THE COLOUR PASS CAN THEN REUSE A u B. The prepass depth is exactly A u B,
// so drawing A u B in colour satisfies "colour subset of prepass" with equality,
// and WorldGPU::CullColor's separate full-buffer scan (a second 66 MB read of the
// 830k-entry meta array, for 5069 useful entries) disappears entirely. That, not
// the prepass raster, is the bulk of what two-phase buys here.
//
// ONE SHADER, TWO MODES — deliberately. The cut (frustum + SSA + residency + LOD
// DAG + crossfade) is the part that historically diverged between copies; every
// comment in world_cull.comp and world_cull_hzb.comp says "MUST match". Here the
// LATE pass does not re-evaluate the cut AT ALL: phase 1 packs the surviving
// entry id and its two crossfade fades into the same 32-bit word the draw command
// carries in firstInstance, and phase 2 unpacks it. Divergence is impossible by
// construction, not by discipline.
//
// The two modes write to DIFFERENT indirect/count buffers. That is a descriptor
// difference, not a shader one: the caller binds one of two sets with an
// IDENTICAL layout (WorldGPU s_setTwo[0] -> A, s_setTwo[1] -> B).
layout(local_size_x = 256) in;

#include "cluster_meta.glsl"   // struct Meta — matches VK::WorldGPU::GpuMeshMeta
#include "draw_cmd.glsl"       // struct Cmd — matches VkDrawIndexedIndirectCommand
#include "hzb_test.glsl"       // HZB_SphereOccluded — shared with world_cull_hzb / tree_cull / lod_cull

layout(set = 0, binding = 0) readonly buffer Metas  { Meta metas[]; };
layout(set = 0, binding = 1)          buffer Cmds   { Cmd  cmds[];  };
layout(set = 0, binding = 2)          buffer Count  { uint counts[]; };
layout(set = 0, binding = 3) readonly buffer Bases  { uint groupBase[]; };   // per-group cmd-region base
// ---- Stage B page streaming (vk_cluster_stream) — see world_cull.comp ----
layout(set = 0, binding = 4) readonly buffer SBits  { uint streamBits[]; };
layout(set = 0, binding = 5) readonly buffer SBase  { uint pageSlotBase[]; };
layout(set = 0, binding = 6)          buffer SReq   { uint reqCount; uint reqItems[]; };
layout(set = 0, binding = 7)          buffer STouch { uint touched[]; };     // page bitset (LRU feedback)
layout(set = 0, binding = 8) uniform sampler2D u_HZB;   // depth pyramid (MAX, GENERAL layout)
// The late list. Its first three words ARE the vkCmdDispatchIndirect arguments
// for the LATE pass, so the CPU never learns the count: it records
// vkCmdDispatchIndirect(lateBuf, 0) and the GPU has already filled in x.
layout(set = 0, binding = 9)          buffer Late {
    uint dispatchX, dispatchY, dispatchZ;   // (x,1,1) — x grows one per 256 appends
    uint lateCount;                         // items actually written
    uint lateItems[];                       // packed firstInstance words (entry | fA | fB)
};

// ⚠ TWO CAMERAS IN ONE PASS. The EARLY pass decides the CUT (frustum, SSA, LOD
// DAG) with THIS frame's camera — the cut is what gets drawn, it is not
// historical — but it runs the occlusion test against LAST frame's pyramid, which
// only means anything under LAST frame's matrix and eye. So viewProj/hzbEye are
// the pyramid's frame while planes/cameraPos/viewDir are this frame's. In the
// LATE pass all of them are this frame's. Folding the two eyes into one field
// (they differ only by one frame of movement) would still be SAFE — phase 2
// re-tests every early reject — but it would silently mis-select the near face
// and quietly cost culls, which is exactly the kind of thing nobody finds later.
// ⚠ hzbEye is THREE SCALARS: a vec3 here pads to 16 B and pushes the block to
// 256 (the desktop limit, and above the 128 B every device guarantees).
layout(push_constant) uniform PC {
    mat4 viewProj;          // EARLY: the PREVIOUS frame's; LATE: this frame's
    vec4 planes[6];         // this frame's frustum, always
    vec4 cameraPos;         // .xyz THIS frame's camera (LOD/SSA view-Z metric)
    vec4 viewDir;           // .xyz normalized camera forward (view-Z LOD projection)
    vec4 lodParams;         // x = pxScale/thresholdPx, y = min dist clamp, z = fade band, w = 1/tan(fovY/2)
    uint numGroups;
    float ssaCull;          // SSA cull: 2·pxScale/r_ssa_px, 0 = off
    uint total;             // number of cullable entries (EARLY dispatch size)
    uint mode;              // 0 = EARLY (scan all, append rejects), 1 = LATE (scan the list)
    uint lateCap;           // lateItems capacity — MUST be a multiple of 256 (see mainLate)
    uint hzbValid;          // 0 = no usable pyramid this pass -> skip the test entirely
    float hzbEyeX, hzbEyeY, hzbEyeZ;   // eye that goes WITH viewProj (near-face pull)
} pc;

#include "world_cull_common.glsl"   // projErr + emitDrawCmd — shared with world_cull / world_cull_hzb

const uint kReqCap    = 4096u;   // = ClusterStream kMaxRequests
const uint kModeEarly = 0u;

vec3 hzbEye() { return vec3(pc.hzbEyeX, pc.hzbEyeY, pc.hzbEyeZ); }

// Draw this entry now: the LATE pass takes the same route, so the two phases
// cannot disagree about what a draw command looks like.
void emitPacked(uint packed)
{
    uint l  = packed & 0xFFFFFu;
    uint fA = (packed >> 20) & 63u;
    uint fB = (packed >> 26) & 63u;
    emitDrawCmd(metas[l], l, fA, fB);
}

// ============================================================================
//  LATE — re-test what EARLY rejected against the pyramid rebuilt from phase 1.
// ============================================================================
void mainLate()
{
    uint gid = gl_GlobalInvocationID.x;
    // Two bounds, and both matter. lateCount trims the partial last workgroup in
    // the normal case. On OVERFLOW lateCount runs past the capacity (the atomic
    // keeps counting after the writes stop), and what keeps gid inside the array
    // then is dispatchX: it is only bumped for slots below the cap, so the
    // dispatch spans ceil(cap/256)*256 threads — which equals cap exactly, hence
    // the "lateCap must be a multiple of 256" contract on the push field.
    if (gid >= lateCount) return;

    uint packed = lateItems[gid];
    uint l      = packed & 0xFFFFFu;
    Meta m      = metas[l];

    // The cut already passed in EARLY — only occlusion is re-decided here, and
    // now against a pyramid that holds THIS frame's phase-1 depth. Anything this
    // drops is genuinely behind phase-1 geometry, so it contributed nothing to
    // the depth buffer and leaves nothing written-but-unshaded.
    if (pc.hzbValid != 0u
        && HZB_SphereOccluded(u_HZB, pc.viewProj, m.sphere.xyz, m.sphere.w,
                              hzbEye(), pc.lodParams.w))
        return;

    emitPacked(packed);
}

// ============================================================================
//  EARLY — the full scan. Identical cut to world_cull.comp (including the page
//  requests and the LRU feedback: this pass REPLACES that dispatch, so it must
//  keep feeding the streamer or pages stop arriving), plus the occlusion split.
// ============================================================================
void mainEarly()
{
    uint l = gl_GlobalInvocationID.x;
    if (l >= pc.total) return;

    Meta m = metas[l];
    vec3  c = m.sphere.xyz;
    float r = m.sphere.w;
    for (int i = 0; i < 6; ++i)
        if (dot(pc.planes[i].xyz, c) + pc.planes[i].w < -r) return;   // outside this plane

    // SSA cull (r_ssa_px) — plain whole meshes only; clusters are exempt so a
    // large surface cannot evaporate piecewise. See world_cull.comp.
    if (pc.ssaCull > 0.0 && (m.flags & 2u) != 0u) {
        float dz = max(pc.lodParams.y, dot(pc.viewDir.xyz, c - pc.cameraPos.xyz) - r);
        if (r * pc.ssaCull < dz) return;
    }

    // ---- Stage B residency + direct demand (verbatim from world_cull.comp) ----
    uint sb = (streamBits[l >> 4u] >> ((l & 15u) * 2u)) & 3u;
    if ((sb & 1u) == 0u) {
        float spd = projErr(m.lodSelf,   m.selfError);
        float ppd = projErr(m.lodParent, m.parentError);
        if (ppd > 1.0 && spd <= 1.0) {
            uint slot = atomicAdd(reqCount, 1u);
            if (slot < kReqCap)
                reqItems[slot] = (l & 0xFFFFFFu) | 0x80000000u | (uint(clamp(ppd * 8.0, 0.0, 127.0)) << 24);
        }
        return;
    }
    bool sleaf = (sb & 2u) != 0u;

    float sp = projErr(m.lodSelf,   m.selfError);
    float pp = projErr(m.lodParent, m.parentError);

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
        if (fA == 0u) return;
    }

    atomicOr(touched[m._p1 >> 5u], 1u << (m._p1 & 31u));   // page LRU feedback

    // ---- The occlusion split ----
    // Crossfading pairs are EXEMPT (same reason as world_cull_hzb.comp): the
    // parent and its children draw with complementary dither masks, and deferring
    // only ONE half of the pair to phase 2 would put them in different draws with
    // a pyramid rebuilt in between — the half-lit facade failure. Transitions last
    // fractions of a second; drawing them in phase 1 costs nothing.
    bool testable = (pc.hzbValid != 0u) && (fA == 63u) && (fB == 0u);
    if (testable && HZB_SphereOccluded(u_HZB, pc.viewProj, c, r, hzbEye(), pc.lodParams.w))
    {
        // Hidden against LAST frame's depth — defer. If the list is full we draw
        // it right here instead: a lost entry would be a hole, a redundant draw
        // is only cost.
        uint packed = l | (fA << 20) | (fB << 26);
        uint o = atomicAdd(lateCount, 1u);
        if (o < pc.lateCap) {
            lateItems[o] = packed;
            // One more 256-wide workgroup for the indirect dispatch, bumped by
            // whoever takes the first slot of each group. Cheaper than a separate
            // "prepare args" dispatch and exact for any count.
            if ((o & 255u) == 0u) atomicAdd(dispatchX, 1u);
            return;
        }
        emitDrawCmd(m, l, fA, fB);   // list full — draw it now rather than lose it
        return;
    }

    emitDrawCmd(m, l, fA, fB);
}

void main()
{
    if (pc.mode == kModeEarly) mainEarly();
    else                       mainLate();
}

#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM caster binning over the CLUSTER-LOD world set (Phase 3).
// Replaces vsm_bin.comp (per-mesh ShadowGPU casters) when r_vsm_cluster is on:
// one thread per CANDIDATE — a CPU-precomputed subset of the WorldGPU cull
// entries that can EVER cast (parentError > errB(0); the ortho LOD cut is
// camera-independent, so the set is static per level + error budget). Big maps
// (800k+ entries) thus dispatch and reserve buffers only for the ~cut-sized
// candidate list.
//
// LOD cut per CLIPMAP LEVEL (the UE ortho recipe): a level's texel world size is
// constant → draw an entry into level L iff selfError <= errB(L) < parentError,
// errB(L) = level extent / VSM_VIRTUAL_RES × k. Camera distance does not
// participate — the cut per level never changes as the player moves, so the
// toroidal static cache stays valid (LOD causes ZERO extra invalidations).
// Coarse clipmap levels thus render coarse DAG levels — the geometry win.
//
// Output contract (vsm_page.vert unchanged): collect the DIRTY allocated pages
// the entry's light-space sphere overlaps (only levels passing the entry's LOD
// cut) into a shared casterPages ARENA — pass 1 counts, one atomicAdd on the
// arena cursor (stats[4]) reserves an exact contiguous run, pass 2 writes. No
// per-entry cap: terrain chunks overlapping 140+ dirty pages during sun
// round-robin are never truncated; stats[5] counts arena-full drops (never at
// the current 8.4M-slot arena). ONE indirect draw per casting candidate, with
// instanceCount = page count and firstInstance = the arena base, appended to
// the candidate's BUFFER-COMBO region (comboBase — capacity = the combo's
// candidate count, so an entry emitting ≤ 1 cmd can never overflow).
//
// Alpha-tested entries never reach this shader (the CPU candidate walk skips
// flags bit0 — AT statics don't cast into the VSM static atlas, same coverage
// as the per-mesh path).
#include "vsm_common.glsl"

layout(local_size_x = 64) in;

#include "cluster_meta.glsl"   // struct Meta — matches VK::WorldGPU::GpuMeshMeta
layout(set = 0, binding = 0) readonly buffer Metas { Meta metas[]; };

#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 1
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)

layout(set = 0, binding = 2) readonly buffer PageTable  { uint pageTable[]; };
layout(set = 0, binding = 3) buffer CasterPages { uint casterPages[]; };          // shared arena (cursor = stats[4])
layout(set = 0, binding = 4) buffer Indirect    { uint indirect[]; };             // VkDrawIndexedIndirectCommand stream (5 u32 ea)
layout(set = 0, binding = 5) buffer ComboCount  { uint comboCount[]; };           // per buffer-combo: # commands
layout(set = 0, binding = 6) buffer Stats       { uint stats[]; };                // [0]=draws [1]=instances [2]=maxPages [3]=lodCulled [4]=arena cursor [5]=arena drops
layout(set = 0, binding = 7) readonly buffer SlotDirty { uint slotDirty[]; };
layout(set = 0, binding = 8) readonly buffer Bases     { uint comboBase[]; };     // per-combo cmd-region base
// Stage B page streaming (see world_cull.comp): bit0 drawable, bit1 leaf.
layout(set = 0, binding = 9)  readonly buffer SBits { uint streamBits[]; };
layout(set = 0, binding = 10) readonly buffer SBase { uint pageSlotBase[]; };
layout(set = 0, binding = 11) readonly buffer Cands { uint candIdx[]; };          // (combo<<20)|entry

layout(push_constant) uniform Push {
    uint  candCount;
    uint  arenaSlots;    // casterPages capacity (overflow guard)
    float errK;          // quality k: LOD error budget in level texels (r_vsm_cluster_lod)
} pc;

void main()
{
    uint ci = gl_GlobalInvocationID.x;
    if (ci >= pc.candCount) return;
    uint packed = candIdx[ci];
    uint c      = packed & 0x000FFFFFu;
    uint combo  = packed >> 20u;

    Meta m = metas[c];

    // Stage B residency: non-resident entries are covered by a coarser leaf;
    // a streaming leaf casts even past its error budget (children not resident).
    uint sb = (streamBits[c >> 4u] >> ((c & 15u) * 2u)) & 3u;
    if ((sb & 1u) == 0u) return;
    bool sleaf = (sb & 2u) != 0u;

    vec2  lp = (vsm.view * vec4(m.sphere.xyz, 1.0)).xy;
    float R  = m.sphere.w;

    // Pass 1: COUNT the dirty allocated pages, per level, gated by the LOD cut.
    uint cnt = 0u;
    bool lodDropped = false;
    for (int L = 0; L < VSM_LEVELS; ++L) {
        // Ortho LOD cut for this level: constant world-error budget.
        float errB = vsm.level[L].z / float(VSM_VIRTUAL_RES) * pc.errK;
        if ((m.selfError > errB && !sleaf) || m.parentError <= errB) { lodDropped = true; continue; }

        VSM_PAGE_RANGE(L, lp, R, lo, hi, p0, p1);
        for (int py = p0.y; py <= p1.y; ++py)
        for (int px = p0.x; px <= p1.x; ++px) {
            uint slot = pageTable[vsmPageIndex(L, ivec2(px, py))];
            if (slot == VSM_UNMAPPED) continue;
            if (slotDirty[slot] == 0u) continue;   // cached this frame → keep its depth
            ++cnt;
        }
    }
    atomicMax(stats[2], cnt);
    if (cnt == 0u) { if (lodDropped) atomicAdd(stats[3], 1u); return; }

    // Reserve an exact contiguous arena run for this entry's page list.
    uint base = atomicAdd(stats[4], cnt);
    if (base >= pc.arenaSlots) { atomicAdd(stats[5], 1u); return; }
    uint inst = min(cnt, pc.arenaSlots - base);

    // Pass 2: WRITE the same pages (identical tests — nothing raced between the
    // passes: pageTable/slotDirty are read-only for this whole dispatch).
    uint w = 0u;
    for (int L = 0; L < VSM_LEVELS; ++L) {
        float errB = vsm.level[L].z / float(VSM_VIRTUAL_RES) * pc.errK;
        if ((m.selfError > errB && !sleaf) || m.parentError <= errB) continue;

        VSM_PAGE_RANGE(L, lp, R, lo, hi, p0, p1);
        for (int py = p0.y; py <= p1.y; ++py)
        for (int px = p0.x; px <= p1.x; ++px) {
            uint slot = pageTable[vsmPageIndex(L, ivec2(px, py))];
            if (slot == VSM_UNMAPPED) continue;
            if (slotDirty[slot] == 0u) continue;
            if (w < inst) casterPages[base + w] = slot;
            ++w;
        }
    }

    atomicAdd(stats[1], inst);
    atomicAdd(stats[0], 1u);

    uint slotG = atomicAdd(comboCount[combo], 1u);
    uint cbase = (comboBase[combo] + slotG) * 5u;       // exact region: capacity == combo candidate count
    indirect[cbase + 0] = m.indexCount;
    indirect[cbase + 1] = inst;
    indirect[cbase + 2] = pageSlotBase[2u * m._p1] + m.ibFirst;   // page-local -> pool offset (Stage B)
    indirect[cbase + 3] = pageSlotBase[2u * m._p1 + 1u] + m.firstVertex;   // + page VB base (slice 2); >=0 → uint bits == int
    indirect[cbase + 4] = base;                         // firstInstance → arena run
}

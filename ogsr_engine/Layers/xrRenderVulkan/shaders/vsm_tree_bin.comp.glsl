#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM TREE-caster binning, near/far WIND HYBRID (r_vsm_tree_wind).
// One thread per tree: find the allocated clipmap pages its world sphere overlaps and
// emit ONE indexed-indirect draw (instanceCount = page count). TWO dispatches per frame
// over the same shader, selected by push.mode:
//   mode 0  STATIC atlas: FAR trees only (nearFlags[c]==0), DIRTY pages only (binding 6
//           slotDirty — the toroidal cache filter), rigid raster. [Phase 1]
//   mode 1  DYNAMIC atlas: NEAR trees only (nearFlags[c]!=0), ALL resident pages (the
//           dyn atlas re-renders every frame), marks dynUsed (binding 7) so the resolve's
//           r_vsm_dyn_gate keeps their taps. Rendered with live wind → smooth sway.
// nearFlags (binding 8) is CPU-computed with hysteresis (single source of truth — the
// CPU also emits residency invalidation circles when a tree crosses the boundary).
// See vk_vsm.cpp / vk_TreeManager_Render.cpp.
#include "vsm_common.glsl"

layout(local_size_x = 64) in;

struct TreeMeta { vec3 sphere_P; float sphere_R; uint index_count, ib_first, first_vertex, pad; };
layout(set = 0, binding = 0) readonly buffer Meta { TreeMeta meta[]; };
#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 1
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)
layout(set = 0, binding = 2) readonly buffer PageTable  { uint pageTable[]; };   // virtual page -> slot / UNMAPPED (static or dyn table per mode)
layout(set = 0, binding = 3) buffer CasterPages { uint casterPages[]; };         // shared ARENA: (treeIdx<<13)|slot; cursor = stats[6] (static+dyn dispatches share it)
layout(set = 0, binding = 4) buffer Indirect    { uint indirect[]; };            // 5 u32 per tree (per-mode buffer); firstInstance = arena base
layout(set = 0, binding = 5) buffer Stats       { uint stats[]; };               // [0]=draws [1]=instances [2]=maxPages [3]=dynInst [4]=hzbCulled [5]=rmaskCulled [6]=ARENA CURSOR [7]=arena drops
layout(set = 0, binding = 6) readonly buffer SlotDirty { uint slotDirty[]; };     // STATIC dirty set (mode 0 only)
layout(set = 0, binding = 7) buffer DynUsed     { uint dynUsed[]; };             // dyn slot -> has-caster flag (mode 1 only; resolve gate)
layout(set = 0, binding = 8) readonly buffer NearFlags { uint nearFlags[]; };     // tree -> 1 if in the NEAR (dynamic wind) set
layout(set = 0, binding = 9) readonly buffer PageMax   { float pageMax[]; };       // shadow-HZB: per STATIC slot, max prior depth (1.0 = no occlusion)
layout(set = 0, binding = 10) readonly buffer StaticPT { uint staticPT[]; };       // virtual page -> STATIC slot (Option A: dyn near-trees query static occluders)
layout(set = 0, binding = 11) readonly buffer RMask    { uint rmask[]; };          // receiver mask: 2 u32/page, 8×8 sampled cells (r_vsm_rmask, mode 1 only)
layout(set = 0, binding = 12) buffer TreeBits { uint treeBits[]; };                // diag bitsets: [0..W) dyn per-frame, [W..2W) static-accum (W = pc.bitBase words)

layout(push_constant) uniform Push { uint casterCount; uint arenaSlots; uint mode; uint hzbOn; float margin; uint rmaskOn; uint bitBase; } pc;

// One accepted (tree, page) test, shared by the COUNT and WRITE passes — all inputs
// (pageTable/slotDirty/rmask/pageMax) are read-only for this whole dispatch, so the
// two passes see identical results. Returns the page slot or VSM_UNMAPPED, and only
// bumps the cull diagnostics on the counting pass (countStats).
uint acceptPage(int L, ivec2 p, vec2 lo, vec2 hi, float nearestD, bool countStats)
{
    uint vpi  = vsmPageIndex(L, p);
    uint slot = pageTable[vpi];
    if (slot == VSM_UNMAPPED) return VSM_UNMAPPED;
    if (pc.mode == 0u && slotDirty[slot] == 0u) return VSM_UNMAPPED;   // static cache: dirty pages only
    // RECEIVER MASK (r_vsm_rmask, UE5 idea): drop the (tree, page) pair when the
    // tree's light-XY rect misses every 8×8 cell visible receivers sample in this
    // page. DYN pass only (mode 1): a mask-culled STATIC page would cache incomplete.
    if (pc.rmaskOn != 0u && pc.mode == 1u) {
        ivec2 c0 = clamp(ivec2(floor((lo - vec2(p)) * 8.0)), ivec2(0), ivec2(7));
        ivec2 c1 = clamp(ivec2(floor((hi - vec2(p)) * 8.0)), ivec2(0), ivec2(7));
        uvec2 rm = vsmRectMask8(c0, c1);
        if (((rmask[vpi * 2u] & rm.x) | (rmask[vpi * 2u + 1u] & rm.y)) == 0u) {
            if (countStats) atomicAdd(stats[5], 1u);
            return VSM_UNMAPPED;
        }
    }
    // shadow-HZB: skip a page whose cached STATIC occluders are all NEARER to the light
    // than the tree's front. mode 0: occluder slot == this slot; mode 1 (Option A): the
    // STATIC page for the same world page, via staticPT.
    if (pc.hzbOn != 0u) {
        uint sslot = (pc.mode == 0u) ? slot : staticPT[vpi];
        if (sslot != VSM_UNMAPPED && nearestD > pageMax[sslot] + pc.margin) {
            if (countStats) atomicAdd(stats[4], 1u);
            return VSM_UNMAPPED;
        }
    }
    return slot;
}

void main()
{
    uint c = gl_GlobalInvocationID.x;
    if (c >= pc.casterCount) return;

    TreeMeta m  = meta[c];
    vec3     lc = (vsm.view * vec4(m.sphere_P, 1.0)).xyz;
    vec2     lp = lc.xy;
    float    R  = m.sphere_R;
    // shadow-HZB: the tree's nearest-to-light normalized depth (sphere front point). If this is
    // farther than a page's cached MAX depth, the whole tree is behind the occluders in that page.
    float    nearestD = (lc.z - R - vsm.zparams.x) * vsm.zparams.y;
    // Near/far split: each tree casts into exactly ONE atlas per frame. Test BIT 0 only —
    // the upper bits carry the crown-hull tiers (bit1 = near-hull, bit2 = far-hull, see
    // vsm_hull_cmd.comp) and must not flip a far tree into the dynamic pass.
    bool pass = (pc.mode == 1u) ? ((nearFlags[c] & 1u) != 0u) : ((nearFlags[c] & 1u) == 0u);

    // Pass 1: COUNT the accepted pages. The old per-tree 512-slot slicing made
    // casterPages the single biggest VRAM line item (trees × cap × 4 B = 396 MB on a
    // 193k-tree map) — big enough to spill the VRAM budget and push tree buffers onto
    // PCIe (the 25-40 ms Bins/Tree* zones). The shared arena reserves EXACTLY what a
    // frame bins (~thousands of pairs), like the cluster bin's arena.
    uint cnt = 0u;
    if (pass)
    for (int L = 0; L < VSM_LEVELS; ++L) {
        VSM_PAGE_RANGE(L, lp, R, lo, hi, p0, p1);
        for (int py = p0.y; py <= p1.y; ++py)
        for (int px = p0.x; px <= p1.x; ++px)
            if (acceptPage(L, ivec2(px, py), lo, hi, nearestD, true) != VSM_UNMAPPED) ++cnt;
    }
    if (cnt > 0u) atomicMax(stats[2], cnt);   // gated: 193k same-address no-op atomics are not free

    // Reserve an exact contiguous arena run, then pass 2: WRITE (treeIdx<<13)|slot —
    // the page/hull/vox/impostor VS unpack both from the entry (the old contract
    // derived the tree from gl_InstanceIndex / cap, which an arena can't provide).
    uint abase = 0u;
    uint inst  = 0u;
    if (cnt > 0u) {
        abase = atomicAdd(stats[6], cnt);
        if (abase < pc.arenaSlots) {
            inst = min(cnt, pc.arenaSlots - abase);
            uint w = 0u;
            for (int L = 0; L < VSM_LEVELS; ++L) {
                VSM_PAGE_RANGE(L, lp, R, lo, hi, p0, p1);
                for (int py = p0.y; py <= p1.y; ++py)
                for (int px = p0.x; px <= p1.x; ++px) {
                    uint slot = acceptPage(L, ivec2(px, py), lo, hi, nearestD, false);
                    if (slot == VSM_UNMAPPED) continue;
                    if (w < inst) {
                        casterPages[abase + w] = (c << 13u) | (slot & 0x1FFFu);
                        if (pc.mode == 1u) atomicOr(dynUsed[slot], 1u);   // dyn page gets real caster depth
                    }
                    ++w;
                }
            }
        } else {
            atomicAdd(stats[7], 1u);   // arena full → tree drops this frame (diag)
        }
    }

    // ALWAYS write the command (inst=0 for skipped trees) — stale counts must not draw.
    uint base = c * 5u;                          // VkDrawIndexedIndirectCommand = 5 u32
    indirect[base + 0] = m.index_count;          // indexCount
    indirect[base + 1] = inst;                   // instanceCount
    indirect[base + 2] = m.ib_first;             // firstIndex
    indirect[base + 3] = m.first_vertex;         // vertexOffset (>=0 -> uint bits == int)
    indirect[base + 4] = abase;                  // firstInstance -> this tree's arena run
    if (inst > 0u) { atomicAdd(stats[0], 1u); atomicAdd(stats[1], inst); }
    if (pc.mode == 1u && inst > 0u) atomicAdd(stats[3], inst);   // dyn-pass instances (hybrid diagnostic)
    // Diag bitsets: which DISTINCT trees shadowed through the dyn atlas this frame
    // (bits [0..W), cleared per frame) vs entered the cached static atlas (bits
    // [W..2W), accumulated — static bins only touch dirty pages, so per-frame
    // counts there are meaningless).
    if (inst > 0u)
        atomicOr(treeBits[(pc.mode == 1u ? 0u : pc.bitBase) + (c >> 5u)], 1u << (c & 31u));
}

#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM caster binning + draw build. One thread per opaque static
// caster: find the ALLOCATED clipmap pages its light-space sphere overlaps, store
// that page list, and emit ONE indexed-indirect draw for the caster with
// instanceCount = its page count (each instance routes to one page in vsm_page.vert,
// gl_InstanceIndex -> casterPages[] -> page slot). The command is written into the
// caster's ShadowGPU GROUP region so the render can bind one vb/ib per group. This
// makes the per-page render an instanced draw (caster x its pages), avoiding the
// P×C brute force. Casters come from ShadowGPU. See vk_vsm.cpp. PHASE 1B-render.
#include "vsm_common.glsl"

layout(local_size_x = 64) in;

// ShadowGPU GpuCasterMeta (std430, 48 B): sphere + draw ranges.
struct CasterMeta {
    vec3 sphere_P; float sphere_R;
    uint index_count, ib_first, first_vertex, group;
    uint lod_first, lod_count, pad0, pad1;
};
layout(set = 0, binding = 0) readonly buffer Meta { CasterMeta meta[]; };

#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 1
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)

layout(set = 0, binding = 2) readonly buffer PageTable  { uint pageTable[]; };     // virtual page -> slot / UNMAPPED
layout(set = 0, binding = 3) buffer CasterPages { uint casterPages[]; };           // c*PAGES_CAP + i -> page slot
layout(set = 0, binding = 4) buffer Indirect    { uint indirect[]; };              // VkDrawIndexedIndirectCommand stream (5 u32 ea)
layout(set = 0, binding = 5) buffer GroupCount   { uint groupCount[]; };           // per ShadowGPU group: # commands
layout(set = 0, binding = 6) buffer Stats        { uint stats[]; };                // [0]=draws [1]=instances [2]=maxPages [3]=groupOverflow
layout(set = 0, binding = 7) readonly buffer SlotDirty { uint slotDirty[]; };       // slot -> 1 if dirty this frame (Phase 1b: only render dirty pages)

layout(push_constant) uniform Push {
    uint  casterCount;
    uint  groupStride;
    float camX; float camY; float camZ;   // camera world pos (caster-LOD distance test)
    float lodDist;                         // distance beyond which a caster draws its COARSE slice (0 = off)
} pc;

void main()
{
    uint c = gl_GlobalInvocationID.x;
    if (c >= pc.casterCount) return;

    CasterMeta m  = meta[c];
    vec2       lp = (vsm.view * vec4(m.sphere_P, 1.0)).xy;
    float      R  = m.sphere_R;

    // Collect the allocated pages the sphere covers, into this caster's slice.
    uint cnt = 0u;
    for (int L = 0; L < VSM_LEVELS; ++L) {
        VSM_PAGE_RANGE(L, lp, R, lo, hi, p0, p1);
        for (int py = p0.y; py <= p1.y; ++py)
        for (int px = p0.x; px <= p1.x; ++px) {
            uint slot = pageTable[vsmPageIndex(L, ivec2(px, py))];
            if (slot == VSM_UNMAPPED) continue;
            if (slotDirty[slot] == 0u) continue;   // page is cached this frame → don't re-draw the caster into it
            if (cnt < uint(VSM_PAGES_CAP))
                casterPages[c * uint(VSM_PAGES_CAP) + cnt] = slot;   // thread owns this slice → no atomic
            ++cnt;
        }
    }
    atomicMax(stats[2], cnt);
    if (cnt == 0u) return;

    uint inst = min(cnt, uint(VSM_PAGES_CAP));
    atomicAdd(stats[1], inst);

    // One indexed-indirect draw for this caster, into its group's region.
    uint slotG = atomicAdd(groupCount[m.group], 1u);
    if (slotG >= pc.groupStride) { atomicAdd(stats[3], 1u); return; }   // group region full → drop
    atomicAdd(stats[0], 1u);
    // caster-LOD: distant casters emit their COARSE slice (== full for MT_NORMAL, so no shadow
    // loss; real cut on the 704 progressive terrain/big meshes). Multiplied by the caster's page
    // count (instanced across all its pages) → the main VSMrender lever.
    uint idxCount = m.index_count;
    uint idxFirst = m.ib_first;
    if (pc.lodDist > 0.0 && distance(m.sphere_P, vec3(pc.camX, pc.camY, pc.camZ)) > pc.lodDist) {
        idxCount = m.lod_count;
        idxFirst = m.lod_first;
    }

    uint base = (m.group * pc.groupStride + slotG) * 5u;               // VkDrawIndexedIndirectCommand = 5 u32
    indirect[base + 0] = idxCount;                      // indexCount
    indirect[base + 1] = inst;                          // instanceCount
    indirect[base + 2] = idxFirst;                      // firstIndex
    indirect[base + 3] = m.first_vertex;                // vertexOffset (>=0 → uint bits == int)
    indirect[base + 4] = c * uint(VSM_PAGES_CAP);       // firstInstance → casterPages base for this caster
}

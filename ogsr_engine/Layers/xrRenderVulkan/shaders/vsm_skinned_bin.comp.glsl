#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM skinned-caster binning. One thread per visible NPC leaf:
// find the ALLOCATED clipmap pages its world bounding sphere overlaps, store the
// slot list, and emit ONE indexed-indirect draw (instanceCount = page count; each
// instance routes to a page in vsm_skinned_page.vert). Mirrors vsm_bin.comp, but
// per-leaf — skinned meshes aren't in the static shared pools, so there are no
// groups: one command per caster at indirect[c]. See vk_vsm.cpp.
#include "vsm_common.glsl"

layout(local_size_x = 64) in;

struct SkinMeta { vec3 sphere_P; float sphere_R; uint index_count, ib_first, first_vertex, pad; };
layout(set = 0, binding = 0) readonly buffer Meta { SkinMeta meta[]; };
#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 1
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)
layout(set = 0, binding = 2) readonly buffer PageTable  { uint pageTable[]; };   // virtual page -> slot / UNMAPPED
layout(set = 0, binding = 3) buffer CasterPages { uint casterPages[]; };         // c*CAP + i -> slot
layout(set = 0, binding = 4) buffer Indirect    { uint indirect[]; };            // 5 u32 per caster (VkDrawIndexedIndirectCommand)
layout(set = 0, binding = 5) buffer Stats       { uint stats[]; };               // [0]=draws [1]=instances [2]=maxPages
layout(set = 0, binding = 6) buffer DynUsed     { uint dynUsed[]; };             // dyn slot -> 1 if any dynamic caster binned into it (the resolve skips untouched dyn pages)

layout(push_constant) uniform Push { uint casterCount; uint cap; } pc;

void main()
{
    uint c = gl_GlobalInvocationID.x;
    if (c >= pc.casterCount) return;

    SkinMeta m  = meta[c];
    vec2     lp = (vsm.view * vec4(m.sphere_P, 1.0)).xy;
    float    R  = m.sphere_R;

    uint cnt = 0u;
    for (int L = 0; L < VSM_LEVELS; ++L) {
        VSM_PAGE_RANGE(L, lp, R, lo, hi, p0, p1);
        for (int py = p0.y; py <= p1.y; ++py)
        for (int px = p0.x; px <= p1.x; ++px) {
            uint slot = pageTable[vsmPageIndex(L, ivec2(px, py))];
            if (slot == VSM_UNMAPPED) continue;
            if (cnt < pc.cap) {
                casterPages[c * pc.cap + cnt] = slot;   // thread owns this slice → no atomic
                atomicOr(dynUsed[slot], 1u);            // this dyn page will get real caster depth
            }
            ++cnt;
        }
    }
    atomicMax(stats[2], cnt);

    uint inst = min(cnt, pc.cap);
    uint base = c * 5u;                          // VkDrawIndexedIndirectCommand = 5 u32
    indirect[base + 0] = m.index_count;          // indexCount
    indirect[base + 1] = inst;                   // instanceCount
    indirect[base + 2] = m.ib_first;             // firstIndex
    indirect[base + 3] = m.first_vertex;         // vertexOffset (>=0 → uint bits == int)
    indirect[base + 4] = c * pc.cap;             // firstInstance → casterPages base for this leaf
    if (inst > 0u) { atomicAdd(stats[0], 1u); atomicAdd(stats[1], inst); }
}

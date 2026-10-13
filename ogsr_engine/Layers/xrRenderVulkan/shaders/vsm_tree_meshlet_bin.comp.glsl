#version 450
#extension GL_GOOGLE_include_directive : require
// ⛔ NO NET PERF GAIN on dGPU (2026-07-02) — meshlet-cull kept but DISABLED (r_vsm_meshlet=0). VSMrender
//    is fill-bound so cutting vertices didn't help. Correct, picture identical. See vk_console_min.cpp + memory.
// xrRenderVulkan — VSM TREE meshlet-cull binning (Phase B, r_vsm_meshlet). STAGE 2.
//
// Stage 1 (vsm_tree_bin.comp) already listed, per tree c, the atlas page slots its
// world sphere overlaps: casterPages[c*cap + i] (i < pageCount, pageCount lives in the
// stage-1 indirect at [c*5+1]). This stage refines that to the CLUSTER level: one
// workgroup per tree, threads stride over the tree's meshlets; each (meshlet,page)
// whose WORLD sphere still overlaps the page rect emits ONE VkDrawIndexedIndirectCommand
// drawing just that meshlet routed to that page. Commands are appended into the tree's
// GROUP section (pooled across the group's trees) so the render can DrawIndexedIndirectCount
// per group (VB/texture/pipeline bind stays per group). firstInstance packs tree|slot for
// the meshlet page VS. See vk_TreeManager_Render.cpp (VsmBinMeshlet) + tree_vsm_meshlet_body.glsl.
#include "vsm_common.glsl"

layout(local_size_x = 64) in;

struct TreeMeta   { vec3 sphere_P; float sphere_R; uint index_count, ib_first, first_vertex, pad; };
struct TreeInst   { mat4 xform; float c_scale_hemi; float c_bias_hemi; uint p0, p1; };
struct Meshlet    { vec3 center; float radius; uint first_index, index_count, p0, p1; };
struct Range      { uint base, count; };

layout(set = 0, binding =  0) readonly buffer Meta        { TreeMeta meta[]; };
#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 1
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)
layout(set = 0, binding =  2) readonly buffer Xform       { TreeInst inst[]; };
layout(set = 0, binding =  3) readonly buffer Meshlets    { Meshlet meshlets[]; };
layout(set = 0, binding =  4) readonly buffer TreeRange   { Range treeRange[]; };
layout(set = 0, binding =  5) readonly buffer CasterPages { uint casterPages[]; };     // c*cap + i -> slot
layout(set = 0, binding =  6) readonly buffer PageList    { uvec4 pageList[]; };        // slot -> (L, page.x, page.y, _)
layout(set = 0, binding =  7) readonly buffer PageInd     { uint pageInd[]; };          // stage-1 indirect: [c*5+1] = pageCount
layout(set = 0, binding =  8) writeonly buffer OutCmd     { uint outCmd[]; };           // VkDrawIndexedIndirectCommand[] (5 u32)
layout(set = 0, binding =  9) buffer GroupCount           { uint groupCount[]; };
layout(set = 0, binding = 10) readonly buffer TreeGroup   { uint treeGroup[]; };
layout(set = 0, binding = 11) readonly buffer GroupInfo   { uvec2 groupInfo[]; };       // (cmdBase, cmdCap) per group
layout(set = 0, binding = 12) buffer Stats                { uint stats[]; };            // [0]=commands [1]=overflow

layout(push_constant) uniform Push { uint casterCount; uint cap; float slop; uint pad; } pc;

void main()
{
    uint c = gl_WorkGroupID.x;
    if (c >= pc.casterCount) return;

    uint pcount = pageInd[c * 5u + 1u];   // pages this tree bound in stage 1 (instanceCount)
    if (pcount == 0u) return;
    uint pbase  = pageInd[c * 5u + 4u];   // stage-1 firstInstance = the tree's casterPages ARENA run
    Range rg = treeRange[c];
    if (rg.count == 0u) return;

    mat4  X        = inst[c].xform;
    float maxScale = max(length(X[0].xyz), max(length(X[1].xyz), length(X[2].xyz)));
    uint  vbase    = meta[c].first_vertex;
    uint  g        = treeGroup[c];
    uint  gbase    = groupInfo[g].x;
    uint  gcap     = groupInfo[g].y;

    for (uint mi = gl_LocalInvocationID.x; mi < rg.count; mi += 64u)
    {
        Meshlet ml = meshlets[rg.base + mi];
        vec3  wc = (X * vec4(ml.center, 1.0)).xyz;
        vec2  lc = (vsm.view * vec4(wc, 1.0)).xy;        // meshlet centre in light XY
        float R  = ml.radius * maxScale + pc.slop;       // conservative (scale + wind slop)
        float R2 = R * R;

        for (uint pi = 0u; pi < pcount; ++pi)
        {
            uint  slot = casterPages[pbase + pi] & 0x1FFFu;   // arena entry = (tree<<13)|slot
            uvec4 pg   = pageList[slot];
            int   L    = int(pg.x);
            ivec2 page = ivec2(pg.yz);
            vec2  origin = vsm.level[L].xy;
            float pw     = vsm.level[L].z / float(VSM_PAGES_AXIS);
            vec2  pmin   = origin + vec2(page) * pw;
            vec2  pmax   = pmin + vec2(pw);
            vec2  d      = lc - clamp(lc, pmin, pmax);    // circle-vs-rect in light XY
            if (dot(d, d) > R2) continue;

            uint idx = atomicAdd(groupCount[g], 1u);
            if (idx >= gcap) { atomicAdd(stats[1], 1u); continue; }   // group full -> truncate (render clamps to cap)
            uint o = (gbase + idx) * 5u;
            outCmd[o + 0] = ml.index_count;               // indexCount
            outCmd[o + 1] = 1u;                           // instanceCount
            outCmd[o + 2] = ml.first_index;               // firstIndex (into meshlet IB)
            outCmd[o + 3] = vbase;                        // vertexOffset (>=0 -> uint bits == int)
            outCmd[o + 4] = (c << 13) | (slot & 0x1FFFu); // firstInstance = PACK(tree, slot)
            atomicAdd(stats[0], 1u);
        }
    }
}

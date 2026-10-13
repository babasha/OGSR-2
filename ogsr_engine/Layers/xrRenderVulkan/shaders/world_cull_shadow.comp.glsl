#version 450
#extension GL_GOOGLE_include_directive : require
// GPU-driven SHADOW culling of the cluster-LOD world set (Phase 3). One
// invocation per cullable ENTRY (mesh or cluster of its LOD DAG — same meta
// SSBO as world_cull.comp) — sphere vs the sun target's 6 ortho planes →
// append a VkDrawIndexedIndirectCommand into the entry's group region of the
// TARGET's slice of the shadow indirect buffer.
//
// LOD cut for an ORTHO light (the UE recipe): projected edge scales are
// constant, so the perspective projErr test degenerates to a pure WORLD-error
// threshold — draw iff selfError <= errBudget < parentError, where errBudget =
// target texel world size × quality k. Camera distance does NOT participate:
// the cut is constant per target, so a cached shadow map's content never
// changes with camera motion (LOD is invalidation-free by construction).
//
// No HZB, no crossfade (depth-only target, screen-door dither is meaningless
// there). Alpha-tested entries (flags bit0) are culled like the opaques when
// pc.atOn != 0 (their group regions get commands; DrawShadow binds the AT
// depth pipeline + the material's diffuse for the discard). atOn == 0 keeps
// the legacy split: AT casters stay on the CPU FlushDepth cutout path.
layout(local_size_x = 256) in;

#include "cluster_meta.glsl"   // struct Meta — matches VK::WorldGPU::GpuMeshMeta
#include "draw_cmd.glsl"       // struct Cmd — matches VkDrawIndexedIndirectCommand

layout(set = 0, binding = 0) readonly buffer Metas  { Meta metas[]; };
layout(set = 0, binding = 1)          buffer Cmds   { Cmd  cmds[];  };   // TGT_COUNT × total, exact prefix regions per target
layout(set = 0, binding = 2)          buffer Count  { uint counts[]; };  // TGT_COUNT × numGroups
layout(set = 0, binding = 3) readonly buffer Bases  { uint groupBase[]; };
// Stage B page streaming (see world_cull.comp): bit0 drawable, bit1 leaf.
// Shadows draw whatever is resident — the main view drives the requests.
layout(set = 0, binding = 4) readonly buffer SBits { uint streamBits[]; };
layout(set = 0, binding = 5) readonly buffer SBase { uint pageSlotBase[]; };

layout(push_constant) uniform PC {
    vec4  planes[6];     // sun target ortho frustum (world space)
    float errBudget;     // texelWorld × k — the constant LOD cut for this target
    uint  target;        // shadow target index (slices cmds/counts)
    uint  numGroups;
    uint  total;         // number of cullable entries
    uint  atOn;          // 1 = cull alpha-tested entries too (GPU AT casters)
} pc;

void main()
{
    uint l = gl_GlobalInvocationID.x;
    if (l >= pc.total) return;

    Meta m = metas[l];
    if ((m.flags & 1u) != 0u && pc.atOn == 0u) return;   // legacy: alpha-tested → CPU cutout path

    vec3 c = m.sphere.xyz;
    float r = m.sphere.w;
    for (int i = 0; i < 6; ++i)
        if (dot(pc.planes[i].xyz, c) + pc.planes[i].w < -r) return;

    // Stage B residency: non-resident entries are covered by a coarser leaf.
    uint sb = (streamBits[l >> 4u] >> ((l & 15u) * 2u)) & 3u;
    if ((sb & 1u) == 0u) return;
    bool sleaf = (sb & 2u) != 0u;

    // Ortho DAG cut: the finest cut whose world error fits under the texel
    // budget. Complementarity holds bitwise: a child's parentError equals its
    // parent's selfError, so exactly one of the pair passes at any budget.
    // A streaming leaf draws even when its own error overshoots the budget
    // (its finer replacement isn't resident yet).
    if ((m.selfError > pc.errBudget && !sleaf) || m.parentError <= pc.errBudget) return;

    uint g    = m.group;
    uint o    = atomicAdd(counts[pc.target * pc.numGroups + g], 1u);
    uint base = pc.target * pc.total + groupBase[g];
    cmds[base + o].indexCount    = m.indexCount;
    cmds[base + o].instanceCount = 1u;
    cmds[base + o].firstIndex    = pageSlotBase[2u * m._p1] + m.ibFirst;   // page-local -> pool offset
    cmds[base + o].vertexOffset  = int(pageSlotBase[2u * m._p1 + 1u] + m.firstVertex);   // + page VB base (slice 2)
    cmds[base + o].firstInstance = 0u;
}

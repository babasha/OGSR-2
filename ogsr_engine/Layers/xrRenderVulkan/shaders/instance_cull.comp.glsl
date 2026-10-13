#version 450
// xrRenderVulkan — GPU cull for host-pushed rigid INSTANCES (vk_instance_gpu).
//
// Mirrors shadow_cull.comp, with one structural difference: here the geometry is
// LOCAL and SHARED between instances, so the cull sphere is stored in MODEL space
// and transformed by this instance's matrix right here. That is what makes "the
// object moved" a 64-byte SSBO write instead of a geometry re-bake — the meta and
// the groups never change when something is dragged in the editor.

layout(local_size_x = 64) in;

struct Meta {
    vec3 sphere_P; float sphere_R;   // MODEL space
    uint index_count; uint ib_first; uint first_vertex; uint group;
};

layout(set = 0, binding = 0, std430) readonly  buffer MetaBuf  { Meta metas[]; };
layout(set = 0, binding = 1, std430) readonly  buffer XformBuf { mat4 xforms[]; };
layout(set = 0, binding = 2, std430) writeonly buffer CmdBuf   { uint cmds[]; };
layout(set = 0, binding = 3, std430)           buffer CntBuf   { uint counts[]; };
layout(set = 0, binding = 4, std430) readonly  buffer BaseBuf  { uint groupBase[]; };

layout(push_constant) uniform PC {
    vec4 planes[6];    // world-space frustum planes of this target
    uint target;
    uint total;        // instance count (= meta count = xform count)
    uint numGroups;
    // 1 = drop alpha-tested groups (the depth-only SHADOW pipelines cannot discard,
    // so those casters stay on the CPU AT path). The COLOUR target passes 0: the
    // world FS applies alphaRef from the material tail, so colour needs no split.
    uint skipAlphaTested;
} pc;

// Alpha-testedness is a property of the GROUP, not the instance — one mesh has one
// material — so it rides the high bit of groupBase instead of costing every meta
// entry 16 more bytes of padding. Offsets are < 2^31 by construction.
const uint kGroupATBit   = 0x80000000u;
const uint kGroupBaseMask = 0x7FFFFFFFu;

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.total) return;

    Meta m = metas[i];

    uint gb = groupBase[m.group];
    if (pc.skipAlphaTested != 0u && (gb & kGroupATBit) != 0u) return;

    // Model -> world. Same convention as tree.vert: a raw X-Ray row-major Fmatrix
    // loaded into a column-major mat4 transposes, so `M * v` == X-Ray `v * F`.
    vec3 wp = (xforms[i] * vec4(m.sphere_P, 1.0)).xyz;

    // R == 0 means "no bounds" -> always visible, matching Rigid_RenderShadow's
    // `if (bs.R > 0.f)` guard. The radius is deliberately NOT rescaled by the
    // matrix, also to match that path — a scaled instance culls by its unscaled
    // radius on both, so this stage is a pure perf change with no visual delta.
    if (m.sphere_R > 0.0) {
        for (int p = 0; p < 6; ++p)
            if (dot(pc.planes[p].xyz, wp) + pc.planes[p].w < -m.sphere_R) return;
    }

    uint slot = pc.target * pc.numGroups + m.group;
    uint o    = atomicAdd(counts[slot], 1u);

    // Exact prefix-sum regions (WorldGPU style) instead of a maxGroupMesh grid:
    // with one entry per INSTANCE a grid would cost numGroups*maxGroup*targets
    // commands, where the prefix sum costs exactly targets*total.
    uint w = (pc.target * pc.total + (gb & kGroupBaseMask) + o) * 5u;
    cmds[w + 0] = m.index_count;
    cmds[w + 1] = 1u;              // instanceCount
    cmds[w + 2] = m.ib_first;      // firstIndex
    cmds[w + 3] = m.first_vertex;  // vertexOffset (always positive here)
    cmds[w + 4] = i;               // firstInstance -> gl_InstanceIndex in the VS
}

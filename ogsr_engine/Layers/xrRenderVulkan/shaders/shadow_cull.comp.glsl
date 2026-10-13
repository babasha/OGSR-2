#version 450
#extension GL_GOOGLE_include_directive : require
// GPU-driven shadow caster culling. ONE dispatch per target over ALL opaque
// static casters: each invocation tests its caster's sphere vs the target's 6
// light-frustum planes and, if visible, atomically appends a
// VkDrawIndexedIndirectCommand into its group's (target,group) region. The
// caster carries its group index, so a single dispatch routes every group's
// output (was 42/target → 1/target).
//
// caster-LOD: each caster carries a coarse draw range (lodFirst/lodCount). When
// camLod.w (LOD distance) > 0 and the caster is farther than it from the camera,
// the coarse range is emitted instead of the full one — distant shadow casters
// (esp. progressive terrain/big meshes) draw far fewer triangles. Statics are
// world-space (no transform); the draw needs only first/count/vertexOffset.
layout(local_size_x = 256) in;

struct Meta {
    vec4 sphere;
    uint indexCount; uint ibFirst; uint firstVertex; uint group;
    uint lodFirst;   uint lodCount; uint _pad0; uint _pad1;   // coarse LOD slice
};
#include "draw_cmd.glsl"       // struct Cmd — matches VkDrawIndexedIndirectCommand

layout(set = 0, binding = 0) readonly buffer Metas { Meta metas[]; };
layout(set = 0, binding = 1)          buffer Cmds  { Cmd  cmds[];  };
layout(set = 0, binding = 2)          buffer Count { uint counts[]; };

layout(push_constant) uniform PC {
    vec4 planes[6];
    uint target;        // 0=far, 1=cascade0, 2=cascade1 (region = target*numGroups + group)
    uint numGroups;
    uint maxGroupMesh;  // region stride: each (target,group) cmd region holds maxGroupMesh slots
    uint total;         // number of casters
    vec4 camLod;        // xyz = camera pos, w = LOD distance (0 = caster-LOD off)
} pc;

void main()
{
    uint l = gl_GlobalInvocationID.x;
    if (l >= pc.total) return;

    Meta m = metas[l];
    vec3 c = m.sphere.xyz;
    float r = m.sphere.w;
    for (int i = 0; i < 6; ++i)
        if (dot(pc.planes[i].xyz, c) + pc.planes[i].w < -r) return;   // outside this plane

    // caster-LOD: distant casters draw their coarse slice; near keep full detail.
    uint firstIndex = m.ibFirst;
    uint indexCount = m.indexCount;
    if (pc.camLod.w > 0.0 && distance(c, pc.camLod.xyz) > pc.camLod.w) {
        firstIndex = m.lodFirst;
        indexCount = m.lodCount;
    }

    uint ci   = pc.target * pc.numGroups + m.group;   // (target,group) region index
    uint o    = atomicAdd(counts[ci], 1u);
    uint base = ci * pc.maxGroupMesh;                 // region sized maxGroupMesh ≥ group meshCount
    cmds[base + o].indexCount    = indexCount;
    cmds[base + o].instanceCount = 1u;
    cmds[base + o].firstIndex    = firstIndex;
    cmds[base + o].vertexOffset  = int(m.firstVertex);
    cmds[base + o].firstInstance = 0u;
}

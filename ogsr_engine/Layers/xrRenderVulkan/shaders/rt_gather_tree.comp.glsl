#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
// xrRenderVulkan — RT WORLD GATHER, TREES. The static twin of this shader
// (rt_gather.comp) copies world-space triangles as they lie in the pools; a tree
// is one mesh drawn thousands of times, so every vertex has to be put in the
// world by its INSTANCE TRANSFORM on the way out.
//
// ⭐ WHY THE FOREST IS WORTH ITS OWN PASS. Neither of the other two sources has
// a single leaf in it: crowns carry no collision (so level.cform never saw them)
// and trees are not in the static render set (this manager draws them alone). In
// a level that is mostly forest, a probe ray that misses the trees sees terrain
// and sky — which is most of the light in the frame arriving from the wrong
// place, and no amount of work downstream can put it back.
//
// ⚠ NO WIND. The raster sways these vertices in its vertex shader from the wind
// state; the RT copy is the rest pose. A branch that moves 20 cm does not move
// the indirect light it bounces, and a snapshot rebuilt every few seconds could
// not follow the sway anyway.

layout(local_size_x = 64) in;

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PoolWords { uint w[]; };

// Mirrors VK::RTWorld::RtGroup (48 B) — shared with the static gather, which is
// why tree groups are appended to the same table.
struct RtGroup {
    uvec2 vbAddr;
    uvec2 ibAddr;
    uint  stride;
    uint  tcOffset;
    uint  flags;      // bit0 = 32-bit index pool, bit1 = alpha-tested material
    uint  mat;
    float uvScale;    // 1/2048 for trees (FTreeVisual quantisation)
    uint  _pad0, _pad1, _pad2;
};
const uint kGrpIB32 = 1u;

// Mirrors VK::GpuTreeMeta (32 B) and VK::GpuTreeInstance (80 B).
struct TreeMeta { vec3 sphere_P; float sphere_R; uint indexCount; uint ibFirst; uint firstVertex; uint flags; };
struct TreeInst { mat4 xform; vec4 hemi; };

layout(std430, set = 0, binding = 0) readonly buffer TMeta   { TreeMeta metas[]; };
layout(std430, set = 0, binding = 1) readonly buffer TXform  { TreeInst inst[];  };
layout(std430, set = 0, binding = 2) readonly buffer Groups  { RtGroup groups[]; };
layout(std430, set = 0, binding = 3) readonly buffer Jobs    { uvec4 jobs[]; };   // x tree, y first tri, z group
layout(std430, set = 0, binding = 4) writeonly buffer OutPos { float outPos[]; };
layout(std430, set = 0, binding = 5) writeonly buffer OutAtt { uint  outAttr[]; };

layout(push_constant) uniform PC {
    uint jobCount;
    uint triCap;
    uint _flags;
    uint _pad;
} pc;

void main()
{
    const uint j = gl_WorkGroupID.x;
    if (j >= pc.jobCount) return;

    const uint t0      = jobs[j].y;
    const uint gi      = jobs[j].z;
    const TreeMeta m   = metas[jobs[j].x];
    const RtGroup  g   = groups[gi];
    const uint tris    = m.indexCount / 3u;
    if (g.stride == 0u) return;

    // ⭐ X-Ray's Fmatrix is ROW-major with rows i/j/k/c, which is exactly what
    // std430 hands GLSL as the COLUMNS of a mat4 — so `xform * vec4(p,1)` is the
    // same product the tree vertex shader computes as `t.xform * vec4(aPos,1)`.
    const mat4 M = inst[jobs[j].x].xform;

    PoolWords vb = PoolWords(g.vbAddr);
    PoolWords ib = PoolWords(g.ibAddr);
    const uint vw   = g.stride >> 2u;
    const uint uvw  = g.tcOffset >> 2u;
    const bool ib32 = (g.flags & kGrpIB32) != 0u;

    for (uint t = gl_LocalInvocationID.x; t < tris; t += 64u) {
        const uint tri = t0 + t;
        if (tri >= pc.triCap) break;
        const uint o = tri * 9u;

        for (uint c = 0u; c < 3u; ++c) {
            const uint ii = m.ibFirst + t * 3u + c;
            uint idx;
            if (ib32) idx = ib.w[ii];
            else {
                const uint word = ib.w[ii >> 1u];
                idx = ((ii & 1u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);
            }
            const uint v = (m.firstVertex + idx) * vw;
            const vec3 lp = vec3(uintBitsToFloat(vb.w[v + 0u]),
                                 uintBitsToFloat(vb.w[v + 1u]),
                                 uintBitsToFloat(vb.w[v + 2u]));
            const vec3 wp = (M * vec4(lp, 1.0)).xyz;
            outPos[o + c * 3u + 0u] = wp.x;
            outPos[o + c * 3u + 1u] = wp.y;
            outPos[o + c * 3u + 2u] = wp.z;

            const uint p  = vb.w[v + uvw];
            const vec2 uv = vec2(float(int(p << 16) >> 16), float(int(p) >> 16)) * g.uvScale;
            outAttr[tri * 4u + 1u + c] = packHalf2x16(uv);
        }
        outAttr[tri * 4u + 0u] = g.mat;
    }
}

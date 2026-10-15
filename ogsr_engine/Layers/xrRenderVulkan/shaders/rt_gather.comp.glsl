#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
// xrRenderVulkan — RT WORLD GATHER: copy the selected render-geometry triangles
// into the flat soup the BLAS is built from. See vk_rt_world.h for why the RT
// world is a SNAPSHOT of the pools rather than a BLAS over them.
//
// One WORKGROUP per selected entry (a mesh or one cluster of its LOD DAG), one
// thread per triangle, striding. Entries are small (clusters cap at ~128 tris)
// and the pass runs once per bubble rebuild, not per frame.
//
// ⚠ THE POOLS ARE READ BY ADDRESS, NOT THROUGH DESCRIPTORS. A level has dozens
// of vertex/index pools plus the two cluster page pools; every one of them would
// otherwise need a descriptor slot in a bindless array. The per-group table
// carries the two addresses instead (uvec2 — the uint64 arithmetic extension
// buys nothing here since the address is only ever cast to a pointer).
//
// ⚠ RESIDENCY IS RE-TESTED HERE, NOT TRUSTED FROM THE CPU. Selection runs on a
// worker a frame or more earlier, and a page can be evicted in between — its
// slot then holds ANOTHER page's indices, which would put geometry from
// somewhere else in the level into the BVH. The 2-bit state read here is the
// same buffer the cull shader reads, uploaded in the same command buffer, so
// the snapshot can never disagree with what the raster considers installed.
// A dropped entry leaves DEGENERATE triangles (three identical vertices): they
// occupy their slot in the soup, cost nothing to build, and can never be hit.

layout(local_size_x = 64) in;

#include "cluster_meta.glsl"   // struct Meta — matches VK::WorldGPU::GpuMeshMeta

// The pools, as plain word arrays: the vertex stride and the texcoord offset are
// per-group and always multiples of 4, so word indexing covers both layouts.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PoolWords { uint w[]; };

// Mirrors VK::RTWorld::RtGroup (48 B).
struct RtGroup {
    uvec2 vbAddr;
    uvec2 ibAddr;
    uint  stride;     // vertex stride in bytes (position is at offset 0, always)
    uint  tcOffset;   // byte offset of TEXCOORD0 (SHORT2)
    uint  flags;      // bit0 = 32-bit index pool, bit1 = alpha-tested material
    uint  mat;        // RT material slot
    float uvScale;    // SHORT2 -> UV (1/1024 statics, 1/2048 trees) — applied HERE
    uint  _pad0, _pad1, _pad2;
};
const uint kGrpIB32 = 1u;

layout(std430, set = 0, binding = 0) readonly buffer Metas   { Meta metas[]; };
layout(std430, set = 0, binding = 1) readonly buffer SBase   { uint pageSlotBase[]; };
layout(std430, set = 0, binding = 2) readonly buffer SBits   { uint streamBits[]; };
layout(std430, set = 0, binding = 3) readonly buffer Groups  { RtGroup groups[]; };
layout(std430, set = 0, binding = 4) readonly buffer Jobs    { uvec2 jobs[]; };   // x = entry, y = first triangle
layout(std430, set = 0, binding = 5) writeonly buffer OutPos { float outPos[]; };
layout(std430, set = 0, binding = 6) writeonly buffer OutAtt { uint  outAttr[]; };

layout(push_constant) uniform PC {
    uint jobCount;
    uint triCap;    // soup capacity in triangles (bounds every write)
    uint flags;     // bit0 = the residency bits are real (else everything is resident)
    uint _pad;
} pc;
const uint kHaveBits = 1u;

// A slot that has no geometry to hold. NaN rather than zero: the spec calls a
// triangle with a NaN vertex INACTIVE and leaves it out of the structure
// entirely, where three zeroes would still be a (never-hittable) primitive
// piling up at the world origin.
void writeDegenerate(uint tri)
{
    const uint o = tri * 9u;
    const float qnan = uintBitsToFloat(0x7FC00000u);
    for (uint i = 0u; i < 9u; ++i) outPos[o + i] = qnan;
    outAttr[tri * 4u + 0u] = 0u;
    outAttr[tri * 4u + 1u] = 0u;
    outAttr[tri * 4u + 2u] = 0u;
    outAttr[tri * 4u + 3u] = 0u;
}

void main()
{
    const uint j = gl_WorkGroupID.x;
    if (j >= pc.jobCount) return;

    const uint e       = jobs[j].x;
    const uint outBase = jobs[j].y;
    const Meta m       = metas[e];
    const uint tris    = m.indexCount / 3u;

    // Still resident? (Same test as world_cull.comp, same buffer, same frame.)
    bool live = true;
    if ((pc.flags & kHaveBits) != 0u) {
        const uint sb = (streamBits[e >> 4u] >> ((e & 15u) * 2u)) & 3u;
        live = (sb & 1u) != 0u;
    }
    const RtGroup g = groups[m.group];
    if (g.stride == 0u) live = false;          // group without a device address

    if (!live) {
        for (uint t = gl_LocalInvocationID.x; t < tris; t += 64u)
            if (outBase + t < pc.triCap) writeDegenerate(outBase + t);
        return;
    }

    // Page-local -> pool-absolute, exactly as emitDrawCmd does for the raster.
    uint idxBase = m.ibFirst;
    uint vtxBase = m.firstVertex;
    const uint p = m._p1;
    if (2u * p + 1u < pageSlotBase.length()) {
        idxBase += pageSlotBase[2u * p];
        vtxBase += pageSlotBase[2u * p + 1u];
    }

    PoolWords vb = PoolWords(g.vbAddr);
    PoolWords ib = PoolWords(g.ibAddr);
    const uint vw   = g.stride >> 2u;          // vertex stride in words
    const uint uvw  = g.tcOffset >> 2u;        // texcoord word inside the vertex
    const bool ib32 = (g.flags & kGrpIB32) != 0u;

    for (uint t = gl_LocalInvocationID.x; t < tris; t += 64u) {
        const uint tri = outBase + t;
        if (tri >= pc.triCap) break;
        const uint o = tri * 9u;

        for (uint c = 0u; c < 3u; ++c) {
            const uint ii = idxBase + t * 3u + c;
            uint idx;
            if (ib32) idx = ib.w[ii];
            else {
                const uint word = ib.w[ii >> 1u];
                idx = ((ii & 1u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);
            }
            const uint v = (vtxBase + idx) * vw;
            outPos[o + c * 3u + 0u] = uintBitsToFloat(vb.w[v + 0u]);
            outPos[o + c * 3u + 1u] = uintBitsToFloat(vb.w[v + 1u]);
            outPos[o + c * 3u + 2u] = uintBitsToFloat(vb.w[v + 2u]);
            // SHORT2 -> UV -> half2. Scaling here is what lets one hit shader read
            // geometry from sources that quantise their texcoords differently.
            const uint p = vb.w[v + uvw];
            const vec2 uv = vec2(float(int(p << 16) >> 16), float(int(p) >> 16)) * g.uvScale;
            outAttr[tri * 4u + 1u + c] = packHalf2x16(uv);
        }
        outAttr[tri * 4u + 0u] = g.mat;
    }
}

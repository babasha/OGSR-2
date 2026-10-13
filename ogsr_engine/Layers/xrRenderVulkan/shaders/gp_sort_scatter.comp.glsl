#version 450
#extension GL_GOOGLE_include_directive : require

// Phase 4 alpha sort, pass 3/3: SCATTER. Reads the unsorted world-alpha copy
// from the sort scratch, claims an output slot in its depth bucket (bucket 0 =
// farthest) and writes aliveList[0..counters[1]) back-to-front in place.
// Within-bucket order is arbitrary (~1.4% depth steps — invisible on soft
// alpha smoke). The additive back region and the HUD region are untouched.

#include "gp_common.glsl"

layout(local_size_x = 64) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    uint cnt = min(counters[1], pc.maxParticles);
    if (gid >= cnt) return;

    uint idx = sortData[uint(GP_SORT_BUCKETS) + gid];

    vec3  rel   = pool[idx].pos_age.xyz - pc.camPos.xyz;
    float viewZ = dot(rel, pc.camForward.xyz);

    uint slot = atomicAdd(sortData[gp_sortBucket(viewZ)], 1u);
    if (slot < cnt) aliveList[slot] = idx;   // bounds guard (races can't exceed cnt, but be safe)
}

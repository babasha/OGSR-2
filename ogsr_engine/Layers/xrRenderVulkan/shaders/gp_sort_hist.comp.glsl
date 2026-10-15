#version 450
#extension GL_GOOGLE_include_directive : require

// Phase 4 alpha sort, pass 1/3: depth-bucket HISTOGRAM.
//
// One thread per world-alpha alive entry (aliveList[0..counters[1])): copy the
// entry into the sort scratch (the scatter pass must read the UNSORTED list
// while it rewrites aliveList in place) and atomic-count its view-Z bucket.
// Camera basis comes from the push (camPos/camForward — filled since Phase 4).

#include "gp_common.glsl"

layout(local_size_x = 64) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    uint cnt = min(counters[1], pc.maxParticles);
    if (gid >= cnt) return;

    uint idx = aliveList[gid];
    sortData[uint(GP_SORT_BUCKETS) + gid] = idx;   // unsorted copy for the scatter

    vec3  rel   = pool[idx].pos_age.xyz - pc.camPos.xyz;
    float viewZ = dot(rel, pc.camForward.xyz);
    atomicAdd(sortData[gp_sortBucket(viewZ)], 1u);
}

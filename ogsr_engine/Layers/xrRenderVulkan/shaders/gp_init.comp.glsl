#version 450
#extension GL_GOOGLE_include_directive : require

// One-time pool init: every slot dead, every slot free, counters primed.
// Run once (guarded on the CPU) before the first emit.

#include "gp_common.glsl"

layout(local_size_x = 64) in;

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.maxParticles) return;

    pool[i].pos_age.w = -1.0;   // dead
    freeList[i]       = i;      // every slot is free

    if (i < uint(GP_MAX_PROGRAMS))
        progAlive[i] = 0u;      // #5: per-program alive counts start empty

    if (i == 0u) {
        counters[0] = pc.maxParticles;  // freeCount = all slots free
        counters[1] = 0u;               // aliveCount
    }
}

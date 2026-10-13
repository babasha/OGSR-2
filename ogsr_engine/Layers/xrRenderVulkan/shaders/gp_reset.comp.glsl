#version 450
#extension GL_GOOGLE_include_directive : require

// Per-frame: clear the 4 alive counters before gp_sim rebuilds aliveList.
// [1]=world-alpha [2]=world-add [3]=hud-alpha [4]=hud-add.
// freeCount (counters[0]) is persistent and must NOT be touched here.

#include "gp_common.glsl"

layout(local_size_x = 1) in;

void main()
{
    counters[1] = 0u;   // world alpha
    counters[2] = 0u;   // world additive
    counters[3] = 0u;   // HUD alpha
    counters[4] = 0u;   // HUD additive

    // Phase 4: clear the alpha-sort depth histogram (single thread, 512 stores
    // — trivial; nothing reads it until gp_sort_hist after this frame's sim).
    for (uint b = 0u; b < uint(GP_SORT_BUCKETS); ++b)
        sortData[b] = 0u;
}

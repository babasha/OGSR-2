#version 460
#extension GL_GOOGLE_include_directive : require
// ===========================================================================
//  xrRenderVulkan — WORLD RADIANCE CACHE: eviction. A hash table that only ever
//  inserts fills up, and a full table stops answering.
// ===========================================================================
//  The player walks; the cells behind them are never asked about again but keep
//  their slots, and after a few minutes every probe chain in the table is full
//  of places that no longer matter. The symptom would be a cache that works for
//  thirty seconds and then quietly degrades to the probe field — the worst kind
//  of bug, because nothing breaks.
//
//  So a slice of the table is swept every frame and any cell nobody has asked
//  about for r_wrc_ttl frames is released. A slice rather than the whole table
//  because the sweep touches memory linearly and the table is tens of megabytes:
//  one eighth per frame is a few milliseconds of bandwidth spread over eight
//  frames instead of a spike in one.
//
//  ⚠ THIS PASS IS ALSO WHY A CLAIMED SLOT IS SAFE TO READ. Releasing a cell
//  clears its `shaded` flag as well as its key, so a slot handed to a new key
//  can never be found holding the PREVIOUS occupant's radiance already marked
//  valid. The claim path in wrc_common relies on exactly that, and it is the
//  reason the two stores are here and not there.
//
//  ⚠ IT RUNS BEFORE THE SHADE PASS, so a cell can be evicted between the frame
//  that asked for it and the frame that would have shaded it. wrc_shade checks
//  for that; with any sane TTL it costs one wasted queue slot, once.
// ===========================================================================

#define WRC_SET      0
#define WRC_B_CONSTS 0
#define WRC_B_CELLS  1
#define WRC_B_LIST   2
#define WRC_B_CTRL   3
#include "wrc_common.glsl"

layout(push_constant) uniform PC {
    uvec4 range;   // x = first slot of this frame's slice, y = slots in it, z = TTL (frames), w unused
} pc;

layout(local_size_x = 64) in;

void main()
{
    const uint gid = gl_GlobalInvocationID.x;
    if (gid >= pc.range.y) return;

    const uint i = (pc.range.x + gid) & wrc.ctl.x;
    if (wrcCell[i].key == 0u) return;

    // `touch` is written from wrc.ctl.y, which the CPU keeps strictly positive and
    // monotonic, so this subtraction cannot wrap for a live cell.
    const uint age = wrc.ctl.y - wrcCell[i].touch;
    if (age < pc.range.z) return;

    wrcCell[i].shaded = 0u;
    wrcCell[i].key    = 0u;
}

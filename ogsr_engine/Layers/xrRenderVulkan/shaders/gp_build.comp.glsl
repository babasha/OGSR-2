#version 450
#extension GL_GOOGLE_include_directive : require

// Build FOUR VkDrawIndirectCommands from the per-group alive counts, one per
// (projection × blend) combo: world-alpha, world-add, hud-alpha, hud-add. Run
// by a single thread after gp_sim, with a barrier before the indirect read.
//
// aliveList = two maxP regions (world [0,maxP), HUD [maxP,2maxP)). Within each,
// alpha fills from the front, additive from the back. firstInstance selects the
// range (gl_InstanceIndex = firstInstance + i indexes aliveList in the vert).

#include "gp_common.glsl"

layout(local_size_x = 1) in;

void main()
{
    uint maxP = pc.maxParticles;
    uint wAlpha = counters[1], wAdd = counters[2];
    uint hAlpha = counters[3], hAdd = counters[4];

    // cmd 0 — world alpha: vertexCount, instanceCount, firstVertex, firstInstance
    drawCmd[0]  = 6u;  drawCmd[1]  = wAlpha; drawCmd[2]  = 0u; drawCmd[3]  = 0u;
    // cmd 1 — world additive (back of the world region)
    drawCmd[4]  = 6u;  drawCmd[5]  = wAdd;   drawCmd[6]  = 0u; drawCmd[7]  = maxP - wAdd;
    // cmd 2 — HUD alpha (front of the HUD region)
    drawCmd[8]  = 6u;  drawCmd[9]  = hAlpha; drawCmd[10] = 0u; drawCmd[11] = maxP;
    // cmd 3 — HUD additive (back of the HUD region)
    drawCmd[12] = 6u;  drawCmd[13] = hAdd;   drawCmd[14] = 0u; drawCmd[15] = 2u * maxP - hAdd;
}

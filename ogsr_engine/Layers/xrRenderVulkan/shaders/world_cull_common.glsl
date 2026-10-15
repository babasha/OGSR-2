// xrRenderVulkan — the parts of GPU world culling that the DEPTH-PREPASS cull and
// the HZB COLOR cull must decide identically:
//
//   world_cull.comp      frustum + LOD cut -> the prepass draw set (also builds Hi-Z)
//   world_cull_hzb.comp  the same cut, plus an occlusion test -> the color draw set
//
// The color set MUST be a subset of the prepass set. If the two disagree on the LOD
// cut, a mesh appears in color that the prepass never wrote depth for (or vice versa)
// — which reads as geometry punching through, or vanishing, at a LOD boundary. Both
// files carried their own copy of this and both said "MUST match world_cull.comp".
//
// CONTRACT: the including shader declares the push block `pc` with vec4 lodParams /
// viewDir / cameraPos, the `cmds[]` / `counts[]` / `groupBase[]` / `pageSlotBase[]`
// buffers, and the Meta type (cluster_meta.glsl). The two shaders bind those at
// DIFFERENT binding numbers and pc carries different extra fields — which is exactly
// why this is an include with a contract rather than one shader with an #ifdef.
#ifndef WORLD_CULL_COMMON_GLSL
#define WORLD_CULL_COMMON_GLSL

// Projected error / threshold: <= 1 means this LOD is fine at this distance.
// VIEW-Z, not Euclidean distance: perspective projection divides by view depth, and
// Euclidean distance overshoots it toward the screen edges (by 1/cos of the off-axis
// angle) — which picked coarser LODs exactly where perspective STRETCHES geometry.
// UE's Nanite uses view-Z for the same reason. Monotonicity across the DAG is
// preserved: the parent sphere contains the child sphere, so
// viewZ(parent)-rP <= viewZ(child)-rC still holds (containment bounds the dot).
float projErr(vec4 s, float e)
{
    float d = max(pc.lodParams.y, dot(pc.viewDir.xyz, s.xyz - pc.cameraPos.xyz) - s.w);
    return e * pc.lodParams.x / d;
}

// Append this entry's VkDrawIndexedIndirectCommand to its material-group region.
// Regions are exact prefix sums (groupBase[g]), not uniform strides — clusters skew
// group sizes far too much. World geometry is world-space (no transform), so the draw
// needs only indexCount/firstIndex/vertexOffset. firstInstance smuggles the entry id
// (20 bits) plus the two crossfade fades the vertex shader unpacks.
void emitDrawCmd(Meta m, uint l, uint fA, uint fB)
{
    uint g    = m.group;
    uint o    = atomicAdd(counts[g], 1u);
    uint base = groupBase[g];
    cmds[base + o].indexCount    = m.indexCount;
    cmds[base + o].instanceCount = 1u;
    cmds[base + o].firstIndex    = pageSlotBase[2u * m._p1] + m.ibFirst;   // page-local -> pool offset
    cmds[base + o].vertexOffset  = int(pageSlotBase[2u * m._p1 + 1u] + m.firstVertex);   // + page VB base (slice 2)
    cmds[base + o].firstInstance = l | (fA << 20) | (fB << 26);   // entry id (20b) + crossfade fades
}

#endif // WORLD_CULL_COMMON_GLSL

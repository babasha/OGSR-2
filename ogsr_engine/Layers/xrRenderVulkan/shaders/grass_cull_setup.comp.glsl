#version 450
// xrRenderVulkan — GrassCull SETUP: exclusive prefix-sum of the per-(light,type)
// counts into a packed arena layout, and emit one VkDrawIndexedIndirectCommand
// per cell. Runs single-threaded over the (numLights * typeCount) cells — at
// most 16*64 = 1024 iterations, trivial. After the COUNT pass has tallied
// cellCount[] this fixes each cell's arena base (firstInstance) so the SCATTER
// pass and the depth draw address the same compact region. See grass_cull.comp.
layout(local_size_x = 1) in;

layout(set = 0, binding = 0) readonly buffer Count    { uint cellCount[]; };
layout(set = 0, binding = 1) buffer Cursor            { uint cellCursor[]; };    // = base (scatter start)
layout(set = 0, binding = 2) buffer Indirect          { uint cmd[]; };           // 5 u32 / cell (VkDrawIndexedIndirectCommand)
layout(set = 0, binding = 3) readonly buffer TypeInfo { uint typeIndexCount[]; };

layout(push_constant) uniform PC {
    uint numLights;
    uint typeCount;
    uint maxCull;       // arena capacity (instances)
    uint pad0;
} pc;

void main()
{
    uint nCells = pc.numLights * pc.typeCount;
    uint base = 0u;
    for (uint cell = 0u; cell < nCells; ++cell) {
        uint type = cell - (cell / pc.typeCount) * pc.typeCount;   // cell % typeCount
        uint cnt  = cellCount[cell];
        if (base > pc.maxCull)         base = pc.maxCull;          // arena full
        if (base + cnt > pc.maxCull)   cnt  = pc.maxCull - base;   // clamp tail

        cellCursor[cell]  = base;                                  // scatter writes from here
        cmd[cell * 5u + 0u] = typeIndexCount[type];                // indexCount
        cmd[cell * 5u + 1u] = cnt;                                 // instanceCount
        cmd[cell * 5u + 2u] = 0u;                                  // firstIndex
        cmd[cell * 5u + 3u] = 0u;                                  // vertexOffset
        cmd[cell * 5u + 4u] = base;                                // firstInstance -> arena offset

        base += cnt;
    }
}

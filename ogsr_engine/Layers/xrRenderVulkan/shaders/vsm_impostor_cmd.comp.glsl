#version 450
// xrRenderVulkan — VSM impostor indirect builder (r_vsm_tree_impostor). One thread
// per tree: translate the crown page-bin's VkDrawIndexedIndirectCommand (5 u32:
// indexCount, instanceCount, firstIndex, vertexOffset, firstInstance) into a
// NON-indexed VkDrawIndirectCommand (4 u32: vertexCount, instanceCount, firstVertex,
// firstInstance) for the billboard quad. instanceCount (= pages the tree bins into)
// and firstInstance (= treeIdx*cap → casterPages base) are copied verbatim so the
// impostor VS reuses the exact page routing; vertexCount is forced to 6 (2 tris).
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) readonly  buffer SrcBuf { uint src[]; };   // VkDrawIndexedIndirectCommand[count]
layout(set = 0, binding = 1, std430) writeonly buffer DstBuf { uint dst[]; };   // VkDrawIndirectCommand[count]

layout(push_constant) uniform PC { uint count; } pc;

void main()
{
    uint t = gl_GlobalInvocationID.x;
    if (t >= pc.count) return;
    uint s = t * 5u;   // indexed cmd stride
    uint d = t * 4u;   // non-indexed cmd stride
    dst[d + 0u] = 6u;          // vertexCount  (quad = 2 tris)
    dst[d + 1u] = src[s + 1u]; // instanceCount (pages bound — 0 for far/idle trees → free)
    dst[d + 2u] = 0u;          // firstVertex
    dst[d + 3u] = src[s + 4u]; // firstInstance (= treeIdx*cap → casterPages slice base)
}

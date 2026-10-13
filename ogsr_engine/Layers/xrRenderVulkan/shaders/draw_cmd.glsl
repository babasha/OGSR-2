// xrRenderVulkan — GPU-written indirect draw command.
//
// Layout MUST match VkDrawIndexedIndirectCommand exactly: the cull compute
// shaders append these into a buffer that vkCmdDrawIndexedIndirectCount reads
// directly, so field order and signedness are the Vulkan spec's, not ours
// (vertexOffset is SIGNED — int32_t — while every neighbour is uint32_t).
#ifndef DRAW_CMD_GLSL
#define DRAW_CMD_GLSL

struct Cmd { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };

#endif // DRAW_CMD_GLSL

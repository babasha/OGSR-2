#version 450
// xrRenderVulkan — editor overlay lines (Spike 2). World-space colored lines for
// grid / gizmos / selection boxes. `mvp` is the camera view-proj (world -> clip;
// any model xform is folded in by the caller, as X-Ray does for dynamics).
// See EDITOR_ON_VULKAN_ROADMAP.md §11.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;   // R8G8B8A8_UNORM (byte0 = R)

layout(push_constant) uniform PC { mat4 mvp; } pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vWorld;   // world position (for the per-fragment ground fade)

void main()
{
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vColor = inColor;
    vWorld = inPos;
}

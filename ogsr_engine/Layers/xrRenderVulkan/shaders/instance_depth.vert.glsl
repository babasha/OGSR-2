#version 450
// xrRenderVulkan — depth-only shadow caster VS for host-pushed rigid INSTANCES.
//
// The cull compute wrote firstInstance = instance index into each indirect
// command, so gl_InstanceIndex selects this instance's world matrix from the
// transform SSBO — the same trick tree_cull.comp/tree.vert already use. Geometry
// stays LOCAL and shared across instances; that is the entire point of this path
// (see vk_instance_gpu.h).
//
// Vertex layout matches the host model VB: FLOAT3 position at offset 0, stride
// supplied by the pipeline variant (32 for converted .ogf models).

layout(location = 0) in vec3 aPos;

layout(set = 0, binding = 0, std430) readonly buffer XformBuf { mat4 xforms[]; };

layout(push_constant) uniform PC {
    mat4 mViewProj;   // LIGHT view*proj for this target
} pc;

void main()
{
    gl_Position = pc.mViewProj * (xforms[gl_InstanceIndex] * vec4(aPos, 1.0));
}

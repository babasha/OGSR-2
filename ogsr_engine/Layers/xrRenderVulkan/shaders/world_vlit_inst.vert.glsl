#version 450
// Thin wrapper — instanced variant for the host-driven scene (vk_instance_gpu).
// INSTANCED swaps the push-constant model rows for four INSTANCE-rate vertex
// attributes on binding 1, so one indirect draw covers every instance of a mesh.
// Same FS, same pipeline layout, same descriptor sets.
#extension GL_GOOGLE_include_directive : require
#define INSTANCED
#define WORLD_VLIT 1
#include "world_vert_body.glsl"

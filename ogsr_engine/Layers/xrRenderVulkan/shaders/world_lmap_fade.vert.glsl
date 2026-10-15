#version 450
// Cluster-LOD crossfade variant (r_cluster_fade): decodes the cull-packed fade
// bits from gl_InstanceIndex and screen-door-dithers the LOD transition band.
// Used ONLY by the GPU-driven world path (vk_world_gpu DrawColor/DrawDepth).
#extension GL_GOOGLE_include_directive : require
#define CLUSTER_FADE 1
#include "world_vert_body.glsl"

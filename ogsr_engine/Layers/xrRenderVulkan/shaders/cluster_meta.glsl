// xrRenderVulkan — one cullable ENTRY of the GPU-driven world: either a whole
// mesh (material fragment) or one cluster of its LOD DAG (r_cluster).
//
// Layout MUST match VK::WorldGPU::GpuMeshMeta (vk_world_gpu.h, 80 B, std430),
// which vk_cluster_stream also builds pages from. _p1 carries the entry's
// streaming PAGE id (0 = identity page: non-clustered meshes whose ibFirst stays
// absolute in their own pool IB); flags bit0 = hard cut (alpha-tested material,
// its depth path can't dither), bit1 = plain whole mesh (SSA-cullable).
//
// It was declared five times — world_cull, world_cull_hzb, world_cull_shadow,
// vsm_bin_cluster and world_cluster_debug — with no link to the C++ definition
// or to each other. A field added to GpuMeshMeta had to be mirrored in all five;
// miss one and it reads the next entry's data, silently.
#ifndef CLUSTER_META_GLSL
#define CLUSTER_META_GLSL

struct Meta {
    vec4 sphere;        // cull sphere (frustum/HZB)
    vec4 lodSelf;       // birth-group sphere (self LOD test)
    vec4 lodParent;     // parent-group sphere (parent LOD test)
    uint indexCount; uint ibFirst; uint firstVertex; uint group;
    float selfError; float parentError; uint flags; uint _p1;
};

#endif // CLUSTER_META_GLSL

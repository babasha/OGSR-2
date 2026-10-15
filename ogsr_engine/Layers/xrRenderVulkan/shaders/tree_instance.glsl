// xrRenderVulkan — per-tree instance data, shared by every tree pass.
//
// GPU-driven indirect draw: the cull compute writes firstInstance = global tree
// index into each draw command, so gl_InstanceIndex selects this tree's world
// transform from the SSBO below.
//
// Layout MUST match VK::GpuTreeInstance (vk_TreeManager.h, 80 B). This struct was
// declared TEN times — forward, depth, motion-vectors, hull/voxel debug and the
// five VSM caster bodies — all byte-identical but with nothing tying them to the
// C++ side or to each other. Changing GpuTreeInstance meant finding all ten; miss
// one and it reads a neighbouring tree's transform with no diagnostic.
#ifndef TREE_INSTANCE_GLSL
#define TREE_INSTANCE_GLSL

struct TreeInstance {
    mat4  xform;        // 64 B per-instance world transform
    float c_scale_hemi; //  4 B
    float c_bias_hemi;  //  4 B
    uint  _p0;          //  4 B wind class (2=foliage, 1=trunk, 0=rigid)
    uint  _p1;          //  4 B
};
layout(set = 0, binding = 0, std430) readonly buffer XformBuf {
    TreeInstance inst[];
};

#endif // TREE_INSTANCE_GLSL

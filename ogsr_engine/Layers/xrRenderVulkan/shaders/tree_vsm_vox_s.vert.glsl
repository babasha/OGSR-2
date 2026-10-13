#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — crown voxel-cloud caster VS, STATIC atlas wrapper (FAR tier,
// r_vsm_tree_hull 2). Wind push is all zeros → rigid, cache-stable toroidal pages.
#define TV_MAX_PHYS VSM_MAX_PHYS_S
#define TV_ATLAS_W  VSM_ATLAS_W_S
#define TV_ATLAS_H  VSM_ATLAS_H_S
#include "tree_vsm_vox_body.glsl"

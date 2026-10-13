#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — crown voxel-cloud caster VS, DYNAMIC atlas wrapper (near tier, wind).
#define TV_MAX_PHYS VSM_MAX_PHYS
#define TV_ATLAS_W  VSM_ATLAS_W
#define TV_ATLAS_H  VSM_ATLAS_H
#include "tree_vsm_vox_body.glsl"

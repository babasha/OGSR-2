#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM tree MESHLET page VS, DYNAMIC atlas: near trees, LIVE WIND
// (re-rendered every frame -> smooth coherent sway). Body: tree_vsm_meshlet_body.glsl.
// See vsm_tree_meshlet_bin.comp.glsl + vk_TreeManager_Render.cpp (VsmRenderMeshletDyn).
#define TV_MAX_PHYS VSM_MAX_PHYS
#define TV_ATLAS_W  VSM_ATLAS_W
#define TV_ATLAS_H  VSM_ATLAS_H
#include "tree_vsm_meshlet_body.glsl"

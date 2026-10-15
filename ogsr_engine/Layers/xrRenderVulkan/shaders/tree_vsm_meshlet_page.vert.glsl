#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM tree MESHLET page VS, STATIC (toroidal-cached) atlas: far trees,
// RIGID (wind push = zeros). Near trees with wind go through tree_vsm_meshlet_page_dyn.vert.
// Body: tree_vsm_meshlet_body.glsl. See vsm_tree_meshlet_bin.comp.glsl.
#define TV_MAX_PHYS VSM_MAX_PHYS_S
#define TV_ATLAS_W  VSM_ATLAS_W_S
#define TV_ATLAS_H  VSM_ATLAS_H_S
#include "tree_vsm_meshlet_body.glsl"

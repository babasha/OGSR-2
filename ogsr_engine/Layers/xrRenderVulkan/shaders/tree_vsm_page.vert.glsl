#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM tree page VS, STATIC (toroidal-cached) atlas wrapper: far trees,
// RIGID (wind push = zeros — cached pages must be frame-invariant). Near trees with wind
// go through tree_vsm_page_dyn.vert instead. Body: tree_vsm_page_body.glsl.
#define TV_MAX_PHYS VSM_MAX_PHYS_S
#define TV_ATLAS_W  VSM_ATLAS_W_S
#define TV_ATLAS_H  VSM_ATLAS_H_S
#include "tree_vsm_page_body.glsl"

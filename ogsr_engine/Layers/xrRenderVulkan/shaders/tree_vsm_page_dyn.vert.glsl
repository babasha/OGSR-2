#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM tree page VS, DYNAMIC atlas wrapper: NEAR trees
// (r_vsm_tree_wind_dist), re-rendered EVERY frame with live wind → the tree shadow
// sways smoothly and coherently (one time slice per frame — no per-page "jelly" the
// old static-page wind test had). Body: tree_vsm_page_body.glsl.
#define TV_MAX_PHYS VSM_MAX_PHYS
#define TV_ATLAS_W  VSM_ATLAS_W
#define TV_ATLAS_H  VSM_ATLAS_H
#include "tree_vsm_page_body.glsl"

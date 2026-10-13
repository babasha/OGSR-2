#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM grass casters into the DYNAMIC atlas (near hybrid half: L0
// pairs, re-rendered every frame WITH live SSFX wind so the cast shadow sways with
// the visible blade — same living-shadow treatment as the near wind trees).
// All real logic in vsm_grass_page_body.glsl (shared with the static wrapper).
#include "vsm_common.glsl"
#define GP_ATLAS_W VSM_ATLAS_W
#define GP_ATLAS_H VSM_ATLAS_H
#define GP_WIND 1
#include "vsm_grass_page_body.glsl"

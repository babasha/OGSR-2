#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM grass casters into the STATIC atlas (far hybrid half: L1/L2
// dirty-page pairs, rendered RIGID — no wind — so the toroidal cache stays valid;
// sway is unreadable in a shadow past ~12 m anyway). Drawn inside the static pass
// after opaque + far trees. All real logic in vsm_grass_page_body.glsl.
#include "vsm_common.glsl"
#define GP_ATLAS_W VSM_ATLAS_W_S
#define GP_ATLAS_H VSM_ATLAS_H_S
#define GP_WIND 0
#include "vsm_grass_page_body.glsl"

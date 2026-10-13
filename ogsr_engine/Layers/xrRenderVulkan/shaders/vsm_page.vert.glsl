#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM page rasterization (depth-only). Each instance of a caster
// is ONE page: gl_InstanceIndex -> casterPages[] -> physical slot -> pageList[] ->
// (level, page). World vertex -> light space -> page-local NDC, placed into the
// page's atlas sub-rect, and gl_ClipDistance clips to the page rect so geometry
// outside it doesn't bleed into neighbouring pages. Depth = global clipmap Z (so
// every page stores depth in the same space the receiver compares against).
#include "vsm_common.glsl"

layout(location = 0) in vec3 aPos;   // world-space position (statics are world-baked, offset 0 / FLOAT3)

layout(set = 0, binding = 0) readonly buffer PageList    { uvec4 pageList[]; };   // slot -> (level, px, py, _)
layout(set = 0, binding = 1) readonly buffer CasterPages { uint  casterPages[]; };// instance -> physical slot
#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 2
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)

out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; };

#define VSM_ROUTE_WP       aPos
#define VSM_ROUTE_ATLAS_W  VSM_ATLAS_W_S
#define VSM_ROUTE_ATLAS_H  VSM_ATLAS_H_S

void main()
{
    uint slot = casterPages[gl_InstanceIndex];
    if (slot >= uint(VSM_MAX_PHYS_S)) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }  // defensive: cull

    // Page routing (clip planes + atlas sub-rect) — shared by all VSM casters.
#include "vsm_page_route.glsl"
}

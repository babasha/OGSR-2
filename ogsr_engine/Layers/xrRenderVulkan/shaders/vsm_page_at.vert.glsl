#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM page rasterization, ALPHA-TESTED variant (r_vsm_at).
// vsm_page.vert + the static-mesh UV attribute passed through to the discard
// fragment stage. UVs are SHORT2 SSCALED (quantized ×1024) exactly like every
// other static depth-AT path — pc.uvScale (1/1024) restores texture space.
#include "vsm_common.glsl"

layout(location = 0) in vec3 aPos;   // world-space position (statics are world-baked, offset 0 / FLOAT3)
layout(location = 1) in vec2 aUV;    // SHORT2 SSCALED @ the material group's tcOffset

layout(set = 0, binding = 0) readonly buffer PageList    { uvec4 pageList[]; };   // slot -> (level, px, py, _)
layout(set = 0, binding = 1) readonly buffer CasterPages { uint  casterPages[]; };// instance -> physical slot
#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 2
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)

layout(push_constant) uniform PC {
    vec2  uvScale;             // 1/1024 (SHORT2 quantization)
    float aref;                // read by the fragment stage
    float _pad;
} pc;

layout(location = 0) out vec2 vUV;

out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; };

#define VSM_ROUTE_WP       aPos
#define VSM_ROUTE_ATLAS_W  VSM_ATLAS_W_S
#define VSM_ROUTE_ATLAS_H  VSM_ATLAS_H_S

void main()
{
    vUV = aUV * pc.uvScale;

    uint slot = casterPages[gl_InstanceIndex];
    if (slot >= uint(VSM_MAX_PHYS_S)) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }  // defensive: cull

    // Page routing (clip planes + atlas sub-rect) — shared by all VSM casters.
#include "vsm_page_route.glsl"
}

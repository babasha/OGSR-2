// xrRenderVulkan — VSM tree MESHLET-caster page rasterization body (Phase B, r_vsm_meshlet).
// Same depth-only + per-page routing as tree_vsm_page_body.glsl, but each draw is ONE
// meshlet routed to ONE page: (tree, slot) are decoded from gl_InstanceIndex (== the
// firstInstance the meshlet bin packed), not from casterPages[]. Two wrappers pick the
// atlas via macros, exactly like the non-meshlet body:
//   tree_vsm_meshlet_page.vert.glsl      STATIC atlas (TV_* = VSM_*_S) — far trees, RIGID
//   tree_vsm_meshlet_page_dyn.vert.glsl  DYNAMIC atlas (TV_* = VSM_*)  — near trees, WIND
// See vsm_tree_meshlet_bin.comp.glsl + vk_TreeManager_Render.cpp (VsmRenderMeshlet).
#include "vsm_common.glsl"
#include "ssfx_tree_wind.glsl"   // zero params (static) -> zero displacement

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;    // SHORT2 SSCALED

#include "tree_instance.glsl"   // TreeInstance + XformBuf (set 0 b0) — matches VK::GpuTreeInstance

layout(set = 2, binding = 0) readonly buffer PageList { uvec4 pageList[]; };
#define VSM_PARAMS_SET     2
#define VSM_PARAMS_BINDING 2
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)

#include "tree_vsm_push.glsl"   // VSM caster push — shared by the four caster bodies

layout(location = 0) out vec2 vUV;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; };

#define VSM_ROUTE_WP       wp
#define VSM_ROUTE_ATLAS_W  TV_ATLAS_W
#define VSM_ROUTE_ATLAS_H  TV_ATLAS_H

void main()
{
    vUV = aUV * pc.uvScale;
    uint fi      = uint(gl_InstanceIndex);   // = firstInstance (instanceCount == 1)
    uint treeIdx = fi >> 13;
    uint slot    = fi & 0x1FFFu;
    if (slot >= uint(TV_MAX_PHYS)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        gl_ClipDistance[0] = gl_ClipDistance[1] = gl_ClipDistance[2] = gl_ClipDistance[3] = -1.0;
        return;
    }
    mat4 X = inst[treeIdx].xform;
    vec3 wp = (X * vec4(aPos, 1.0)).xyz;
    // World-space wind displacement (dyn wrapper; static passes zeros -> rigid).
    {
        float baseY = X[3].y;
        float H     = wp.y - baseY;
        wp += ssfxTreeWindWorld(wp, H, vUV.y, inst[treeIdx]._p0, baseY,
                                pc.wind_params, pc.wsetup_trees, pc.wind_anim);
    }

    // Page routing (clip planes + atlas sub-rect) — shared by all VSM casters.
#include "vsm_page_route.glsl"
}

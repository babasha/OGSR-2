// xrRenderVulkan — VSM tree-caster page rasterization body (depth-only, alpha-tested
// in frag). Shared by TWO wrappers that pick the target atlas via macros:
//   tree_vsm_page.vert.glsl      STATIC atlas (TV_* = VSM_*_S) — far trees, RIGID
//                                (wind push = 0; cached toroidal pages must be stable)
//   tree_vsm_page_dyn.vert.glsl  DYNAMIC atlas (TV_* = VSM_*)  — NEAR trees, WIND
//                                (re-rendered every frame → smooth coherent sway)
// Mirrors tree_depth.vert's transform; each instance is one atlas PAGE:
// gl_InstanceIndex → casterPages[] → slot; tree index = gl_InstanceIndex / cap picks
// the per-tree xform. Then route to the page (ortho + atlas sub-rect + clip), same as
// vsm_page.vert. See vk_TreeManager_Render.cpp (near/far hybrid = r_vsm_tree_wind).
#include "vsm_common.glsl"
#include "ssfx_tree_wind.glsl"   // same wind as the forward tree; zero params → zero displacement

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;    // SHORT2 SSCALED

#include "tree_instance.glsl"   // TreeInstance + XformBuf (set 0 b0) — matches VK::GpuTreeInstance

layout(set = 2, binding = 0) readonly buffer PageList    { uvec4 pageList[]; };
layout(set = 2, binding = 1) readonly buffer CasterPages { uint  casterPages[]; };
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
    uint entry = casterPages[gl_InstanceIndex];   // arena entry = (treeIdx<<13)|slot (vsm_tree_bin)
    uint slot  = entry & 0x1FFFu;
    if (slot >= uint(TV_MAX_PHYS)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        gl_ClipDistance[0] = gl_ClipDistance[1] = gl_ClipDistance[2] = gl_ClipDistance[3] = -1.0;
        return;
    }
    uint treeIdx = entry >> 13u;
    mat4 X = inst[treeIdx].xform;
    vec3 wp = (X * vec4(aPos, 1.0)).xyz;
    // World-space wind displacement (dyn wrapper; static passes zeros → rigid).
    {
        float baseY = X[3].y;
        float H     = wp.y - baseY;
        wp += ssfxTreeWindWorld(wp, H, vUV.y, inst[treeIdx]._p0, baseY,
                                pc.wind_params, pc.wsetup_trees, pc.wind_anim);
    }

    // Page routing (clip planes + atlas sub-rect) — shared by all VSM casters.
#include "vsm_page_route.glsl"
}

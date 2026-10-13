// xrRenderVulkan — VSM crown-HULL caster VS BODY (r_vsm_tree_hull). Shared by two
// wrappers that pick the target atlas via TV_* macros (same pattern as
// tree_vsm_page_body.glsl):
//   tree_vsm_hull.vert.glsl    DYNAMIC atlas — near hull tier, live wind push
//   tree_vsm_hull_s.vert.glsl  STATIC atlas  — far hull tier (r_vsm_tree_hull 2),
//                              zero wind push → rigid (cached toroidal pages)
// The AC-Shadows-style shadow LOD: crowns beyond r_vsm_tree_hull_dist cast from baked
// opaque ellipsoid lobes instead of the alpha-tested crown mesh — no texture fetch, no
// discard, early-Z, ~100× fewer verts. Page routing is IDENTICAL to
// tree_vsm_page_body.glsl (gl_InstanceIndex → casterPages → slot; tree = index/cap);
// the hull rides the same casterPages slices the crown bin filled (vsm_hull_cmd.comp
// copies instanceCount/firstInstance verbatim). tc_y is a constant (no UVs) → uniform
// lobe motion, flow-map still varies per lobe.
#include "vsm_common.glsl"
#include "ssfx_tree_wind.glsl"

layout(location = 0) in vec3 aPos;

#include "tree_instance.glsl"   // TreeInstance + XformBuf (set 0 b0) — matches VK::GpuTreeInstance

layout(set = 2, binding = 0) readonly buffer PageList    { uvec4 pageList[]; };
layout(set = 2, binding = 1) readonly buffer CasterPages { uint  casterPages[]; };
#define VSM_PARAMS_SET     2
#define VSM_PARAMS_BINDING 2
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)

#include "tree_vsm_push.glsl"   // VSM caster push — shared by the four caster bodies

out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; };

#define VSM_ROUTE_WP       wp
#define VSM_ROUTE_ATLAS_W  TV_ATLAS_W
#define VSM_ROUTE_ATLAS_H  TV_ATLAS_H

void main()
{
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
    {
        float baseY = X[3].y;
        float H     = wp.y - baseY;
        wp += ssfxTreeWindWorld(wp, H, 0.35, inst[treeIdx]._p0, baseY,
                                pc.wind_params, pc.wsetup_trees, pc.wind_anim);
    }

    // Page routing (clip planes + atlas sub-rect) — shared by all VSM casters.
#include "vsm_page_route.glsl"
}

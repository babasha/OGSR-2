#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — crown-HULL debug overlay VS (r_vsm_tree_hull_debug). Draws the
// baked hull lobes of HULL-TIER trees translucently over the forward scene so the
// r_vsm_tree_hull_dist boundary is visible in-world: pink lobes = the tree casts
// its shadow from the hull, unmarked trees = full crown mesh. Rides the forward
// tree pass: same pipeline layout (TreeGfxPush already pushed, set0 xform+waves
// already bound); one direct draw per hull tree with firstInstance = tree index.
// Same ssfxTreeWind as the VSM hull caster (tc_y 0.35) so the lobes track the sway.

layout(location = 0) in vec3 aPos;   // hull VB — tight vec3
layout(location = 0) out vec3 vWorld;

#include "tree_instance.glsl"   // TreeInstance + XformBuf (set 0 b0) — matches VK::GpuTreeInstance

#include "tree_gfx_push.glsl"   // TreeGfxPush (160 B) — matches VK::TreeGfxPush

#include "ssfx_tree_wind.glsl"

void main()
{
    TreeInstance t = inst[gl_InstanceIndex];   // firstInstance = tree index
    vec4 worldPos = t.xform * vec4(aPos, 1.0);
    float baseY = t.xform[3].y;
    float H     = worldPos.y - baseY;
    worldPos.xyz += ssfxTreeWindWorld(worldPos.xyz, H, 0.35, t._p0, baseY,
                                      pc.wind_params, pc.wsetup_trees, pc.wind_anim);
    vWorld      = worldPos.xyz;   // face-shading normal from screen-space derivatives (FS)
    gl_Position = pc.mViewProj * worldPos;
}

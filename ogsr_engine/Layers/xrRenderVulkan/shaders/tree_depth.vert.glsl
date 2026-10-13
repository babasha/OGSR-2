#version 450
// xrRenderVulkan — tree shadow caster (depth-only, alpha-tested in frag).
// Same instancing as tree.vert, but pc.mViewProj is the LIGHT view·proj. Applies
// the SAME SSFX wind displacement as tree.vert (world-space, VP-independent) so
// the shadow tracks the swaying geometry — a rigid shadow on a bent trunk shows
// a black "untextured" self-shadow band.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;    // SHORT2 SSCALED

#include "tree_instance.glsl"   // TreeInstance + XformBuf (set 0 b0) — matches VK::GpuTreeInstance

#include "tree_gfx_push.glsl"   // TreeGfxPush (160 B) — matches VK::TreeGfxPush

#include "ssfx_tree_wind.glsl"

layout(location = 0) out vec2 vUV;

void main()
{
    TreeInstance t = inst[gl_InstanceIndex];
    vec4 worldPos = t.xform * vec4(aPos, 1.0);

    // Same wind as the forward pass (world-space displacement).
    float baseY = t.xform[3].y;
    float H     = worldPos.y - baseY;
    float tc_y = aUV.y * pc.uvScale;
    worldPos.xyz += ssfxTreeWindWorld(worldPos.xyz, H, tc_y, t._p0, baseY,
                                      pc.wind_params, pc.wsetup_trees, pc.wind_anim);

    gl_Position = pc.mViewProj * worldPos;
    vUV = aUV * pc.uvScale;
}

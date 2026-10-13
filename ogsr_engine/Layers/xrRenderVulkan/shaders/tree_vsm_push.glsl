// xrRenderVulkan — tree VSM caster push constants.
//
// Shared by the four VSM caster bodies (page, meshlet, hull, voxel). No mViewProj:
// these route through the clipmap page (vsm_page_route.glsl) instead of a single
// view-proj. The WIND fields must stay at the same offsets as TreeGfxPush's so a
// shadow caster sways with its tree.
#ifndef TREE_VSM_PUSH_GLSL
#define TREE_VSM_PUSH_GLSL

layout(push_constant) uniform PC {
    float uvScale; float alphaRef; uint cap; uint pad;
    vec4  wind_params;   // wind (dyn wrapper only): all 0 when off → no displacement
    vec4  wsetup_trees;
    vec4  wind_anim;
} pc;

#endif // TREE_VSM_PUSH_GLSL

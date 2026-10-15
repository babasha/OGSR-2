// xrRenderVulkan — tree graphics push constants (VS+FS).
//
// Layout MUST match VK::TreeGfxPush (vk_TreeManager.h, 160 B, static_assert'ed
// there). Shared by the forward pass, the depth/shadow caster and the hull debug
// view: the caster reuses the whole block so the WIND fields land at the same
// offsets as the forward pass — a mismatch there bends the shadow differently
// from the tree. Declared three times before this.
//
// mViewProj is world->clip for the forward pass and the LIGHT view*proj for the
// caster; the rest is identical.
#ifndef TREE_GFX_PUSH_GLSL
#define TREE_GFX_PUSH_GLSL

layout(push_constant) uniform PC {
    mat4  mViewProj;    // world → clip
    float uvScale;      // 1/2048 — tree UV quant (FTreeVisual_quant = 32768/16)
    float alphaRef;     // fragment alpha cutoff
    float lodBand;      // R1 crossfade band D_B/D_A: > 1 = the crown dithers OUT over the band its billboard dithers in (0 = off)
    float shadeDist;    // r_tree_shade_dist: metres past which tree.frag drops its subtle terms (0 = never)
    vec4  vSunColor;    // env sun colour (rgb); w = the crossfade's base D (r_tree_mesh_dist)
    vec4  vHemiColor;   // env hemi colour (rgb)
    vec4  wind_params;  // SSFX (wind_direction, wind_velocity, _, _)
    vec4  wsetup_trees; // SSFX (branchSpeed, trunkSpeed, bend, minWindSpeed)
    vec4  wind_anim;    // Environment.wind_anim drift (xyz)
} pc;

#endif // TREE_GFX_PUSH_GLSL

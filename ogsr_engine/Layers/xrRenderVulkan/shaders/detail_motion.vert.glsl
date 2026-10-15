#version 450
// xrRenderVulkan — grass wind-sway motion vectors. Pairs with detail_motion.frag.
// Re-draws the visible grass into the MV target, reprojecting the CURRENT and the
// PREVIOUS wind-displaced pose so blades swaying in the wind carry their true screen
// motion — the fullscreen depth-reconstruction MV pass only sees camera motion and
// misses the sway. Wind decode is IDENTICAL to detail.vert (keep them in sync); the
// current pose must bit-match detail.vert so depth LEQUAL passes over the grass the
// forward pass already wrote.

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec2  aUV;
layout(location = 2) in float aHeight;
layout(location = 3) in vec4  aInstRow0;
layout(location = 4) in vec4  aInstRow1;
layout(location = 5) in vec4  aInstRow2;
layout(location = 6) in vec4  aInstColor;   // (sun, trail, objId, hemi) — unused here

layout(push_constant) uniform DetailMotionConstants {
    mat4 curVP;             // 0    this frame's view-proj, UNJITTERED (jitter-free MV)
    mat4 prevVP;            // 64   previous frame's view-proj, UNJITTERED
    vec4 wind_params;       // 128  cur (.w = per-type wind scale, patched per draw)
    vec4 wsetup_grass;      // 144  SSFX (animspeed, turbulence, push, wave)
    vec4 wind_anim;         // 160  cur drift (Environment.wind_anim) + w = minWindSpeed
    vec4 wind_params_prev;  // 176  previous frame's wind params
    vec4 wind_anim_prev;    // 192  previous frame's drift
    vec4 jitter;            // 208  xy = this frame's sub-pixel jitter (D3D-NDC)
} pc;

#include "ssfx_wind.glsl"   // SSFX flow-map wind (s_waves @ set0 binding1; SSFX_WIND_SET defaults to 0)

layout(location = 0) out vec4 vCurClip;
layout(location = 1) out vec4 vPrevClip;
layout(location = 2) out vec2 vUV;       // frag alpha-test (blade cutout)

void main()
{
    // Reconstruct the 3×4 instance transform (as in detail.vert).
    mat4x3 inst = mat4x3(
        aInstRow0.xyz, aInstRow1.xyz, aInstRow2.xyz,
        vec3(aInstRow0.w, aInstRow1.w, aInstRow2.w));
    vec3 base = inst * vec4(aPos, 1.0);   // world position BEFORE wind (wind sample point)

    // Current pose — exactly detail.vert's displacement → depth bit-matches.
    WindSetup Wc  = ssfx_wind_setup(pc.wind_params, pc.wsetup_grass, pc.wind_anim.w);
    vec3 curWorld = base + ssfx_wind_grass(base, aHeight, Wc, pc.wind_anim.xy) * pc.wind_params.w;

    // Previous pose — last frame's wind params + drift (the sway delta). Same
    // per-type wind scale (wind_params.w): DO_NO_WAVING is frame-stable.
    WindSetup Wp   = ssfx_wind_setup(pc.wind_params_prev, pc.wsetup_grass, pc.wind_anim_prev.w);
    vec3 prevWorld = base + ssfx_wind_grass(base, aHeight, Wp, pc.wind_anim_prev.xy) * pc.wind_params.w;

    // ⚠ ApplyInteraction is NOT replicated here, and it is NOT inert: the CPU
    // feeds player + NPC interactors every frame, so blades bent aside by an
    // actor rasterize at a depth this pass doesn't match — their sway MV is
    // lost (LEQUAL fails). Known gap: fixing it needs the interactors in this
    // pass too, and this push block is already 208 B of the guaranteed budget.
    // (The trail press-down that was also mentioned here is deleted for real —
    // it never had a writer; see detail.vert.)

    vCurClip    = pc.curVP  * vec4(curWorld,  1.0);   // unjittered → clean MV
    vPrevClip   = pc.prevVP * vec4(prevWorld, 1.0);
    // Rasterize jittered (depth bit-match with the forward grass pass) via the clip
    // translation; MV above stays jitter-free.
    gl_Position = vCurClip;
    gl_Position.xy += pc.jitter.xy * gl_Position.w;
    vUV         = aUV;
}

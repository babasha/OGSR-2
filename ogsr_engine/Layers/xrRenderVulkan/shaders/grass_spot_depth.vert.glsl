#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — grass casters into the SPOT shadow map (depth-only).
// A headlight/searchlight/flashlight beam crossing a grass field shone straight
// THROUGH it: the spot map held statics + NPCs + trees but no grass, so blades
// neither cut the visible volumetric cone nor left cutouts in the light pool.
// Same vertex inputs as the visible grass (binding 0 = mesh pos+uv+height,
// binding 1 = per-instance transform rows at INSTANCE rate, from the detail
// manager's 1-frame-stale GPU-driven buffer) but a plain light view-proj —
// the spot map is one whole target, no page routing. SSFX wind IS applied
// (same flow map s_waves at set 0 binding 1, same 1-frame-stale push values):
// a beam-lit swaying blade must carry its cutout with it, unlike the VSM sun
// casters where a static contact shadow is enough. See vk_pass_shadow.cpp.
layout(location = 0) in vec3  aPos;        // binding 0: grass mesh vertex (pos @0)
layout(location = 1) in vec2  aUV;         // binding 0: uv @12 (alpha-test cutout)
layout(location = 2) in float aHeight;     // binding 0: height @20 (wind stiffness)
layout(location = 3) in vec4  aInstRow0;   // binding 1: instance transform rows
layout(location = 4) in vec4  aInstRow1;
layout(location = 5) in vec4  aInstRow2;

layout(location = 0) out vec2 vUV;

layout(push_constant) uniform PC {
    mat4 vp;             // spot light view-proj
    vec4 lightPosRange;  // xyz = light pos, w = range — whole-instance cull
    vec4 wind_params;    // (wind_direction, wind_velocity, treeAmplitude, PER-TYPE wind scale)
    vec4 wsetup_grass;   // SSFX (animspeed, turbulence, push, wave)
    vec4 wind_anim;      // Environment.wind_anim drift (xyz) + w = minWindSpeed
} pc;

#include "ssfx_wind.glsl"   // s_waves @ set 0 binding 1 (the detail manager's per-type set)

void main()
{
    vUV = aUV;
    // Whole-instance cull: a tuft farther than range+2 m from the light can't
    // shadow anything it lights — park it outside the clip volume.
    vec3 base = vec3(aInstRow0.w, aInstRow1.w, aInstRow2.w);
    vec3 dv   = base - pc.lightPosRange.xyz;
    float r   = pc.lightPosRange.w + 2.0;
    if (dot(dv, dv) > r * r) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    mat4x3 m = mat4x3(aInstRow0.xyz, aInstRow1.xyz, aInstRow2.xyz, base);
    vec3 worldPos = m * vec4(aPos, 1.0);
    // Same SSFX wind the colour pass applies (detail.vert) so the cast cutout
    // tracks the visible blade; wind_params.w = per-type DO_NO_WAVING scale.
    WindSetup W = ssfx_wind_setup(pc.wind_params, pc.wsetup_grass, pc.wind_anim.w);
    worldPos   += ssfx_wind_grass(worldPos, aHeight, W, pc.wind_anim.xy) * pc.wind_params.w;
    gl_Position = pc.vp * vec4(worldPos, 1.0);
}

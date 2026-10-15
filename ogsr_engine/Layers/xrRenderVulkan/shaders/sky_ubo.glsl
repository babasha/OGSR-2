// xrRenderVulkan - sky pass parameter block (set 0, binding 4).
//
// This was a PUSH CONSTANT block. It outgrew that: push constants are only
// guaranteed to 128 bytes by the Vulkan spec, and AMD exposes exactly 128. Adding
// the procedural-sky params had already taken it to 144 — over the limit on any AMD
// GPU, working only because the dev machine is NVIDIA (256). The volumetric cloud
// params would have doubled the overflow, so the block moved to a UBO where size is
// a non-issue. Layout must match SkyUBO in vk_pass_sky.cpp.
//
// Shared by sky.vert (camera basis only) and sky.frag (everything).
#ifndef SKY_UBO_GLSL
#define SKY_UBO_GLSL

layout(set = 0, binding = 4) uniform SkyParams {
    vec4 camRightTan_rot;   // .xyz = vCameraRight * tan(fov/2) * aspect, .w = sky_rotation
    vec4 camUpTan_weight;   // .xyz = vCameraTop   * tan(fov/2),          .w = cube cross-fade
    vec4 camForward_alt;    // .xyz = vCameraDirection (unit),            .w = eye altitude (m)
    vec4 skyColor_pad;      // .xyz = sky_color tint (legacy cube path)
    vec4 sunDir_pad;        // .xyz = sun TRAVEL dir (to-sun = -sunDir)
    vec4 sunColor_pad;      // .xyz = env sun colour (legacy cube path)
    vec4 cloudsColor;       // .rgb = clouds_color tint, .w = weight (legacy 2D clouds)
    vec4 cloudParams;       // x = scroll time, y = legacy 2D clouds on, z = intensity
    vec4 atmoParams;        // PROCEDURAL SKY: x = enable, y = intensity, z = turbidity, w = Mie g

    // ── Volumetric clouds (r_clouds_vol). Named to match clouds.glsl's `CL.` reads.
    vec4 band;      // x = deck bottom (m), y = deck top (m)
    vec4 shape;     // x = coverage, y = detail erosion strength, z = density
    vec4 scales;    // x = shape volume scale, y = detail volume scale, z = weather map scale
    vec4 wind;      // xy = wind direction (unit), z = speed (m/s)
    vec4 light;     // x = forward g, y = back g, z = extinction, w = powder amount
    vec4 light2;    // x = sun strength, y = ambient strength
    vec4 march;     // x = primary steps, z = max marched distance (m)
    vec4 cirrus;    // x = amount, y = altitude (m), z = uv scale
    vec4 atmo;      // mirror of atmoParams for the cloud lighting reads
    vec4 timeP;     // x = animation time (s)
    vec4 fogColor;  // 10-10: rgb = the distance fog colour (linear, = L.fog_color), w = horizon band (r_sky_fog_horizon, 0 = off)
} S;

// clouds.glsl addresses the cloud fields through `CL` so it stays readable as its own
// model rather than as "sky pass field number nine".
#define CL S

#endif // SKY_UBO_GLSL

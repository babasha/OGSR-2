// xrRenderVulkan — SSFX wind model (port of ScreenSpaceShaders "10 - Wind"
// screenspace_wind.h, by ascii1457). Flow-map driven vegetation wind, sampled in
// the VERTEX stage. The `animXY` argument is the accumulated wind drift
// (Environment.wind_anim.xy) — pass the CURRENT drift for the rendered pose and
// the PREVIOUS-frame drift (wind_anim_old) to get the previous pose for motion
// vectors (SSFX's `prev` flag). Shared by detail.vert (forward) + the grass MV
// pass. Requires the flow map bound at set SSFX_WIND_SET (default 0), binding 1
// — vsm_grass_page has page data at set 0, so it #defines SSFX_WIND_SET 1 (the
// detail manager's per-type set, where s_waves lives at binding 1).

#ifndef SSFX_WIND_SET
#define SSFX_WIND_SET 0
#endif
layout(set = SSFX_WIND_SET, binding = 1) uniform sampler2D s_waves;   // wind_wave.dds flow map (RGB)

struct WindSetup {
    vec2  dir;        // wind direction (unit)
    float speed;      // 0..1 wind speed (min-floored)
    float animspeed;  // drift scale
    float turbulence; // flow xy intensity
    float push;       // flow z → push along dir
    float wave;       // vertical wave
};

// wind_params = (wind_direction, wind_velocity, treeAmplitude, _); wsetup_grass =
// (animspeed, turbulence, push, wave); minSpeed = ssfx_wsetup_trees.w floor.
WindSetup ssfx_wind_setup(vec4 wind_params, vec4 wsetup_grass, float minSpeed)
{
    WindSetup w;
    float r = -wind_params.x + 1.57079;
    w.dir   = vec2(cos(r), sin(r));
    w.speed = max(minSpeed, clamp(wind_params.y * 0.001, 0.0, 1.0));
    w.animspeed  = wsetup_grass.x;
    w.turbulence = wsetup_grass.y;
    w.push       = wsetup_grass.z;
    w.wave       = wsetup_grass.w;
    return w;
}

// World-space grass displacement at `pos` (base world position, before wind),
// `H` = vertex height factor (stiffer near the root). `animXY` = wind drift.
vec3 ssfx_wind_grass(vec3 pos, float H, WindSetup W, vec2 animXY)
{
    float HLimit = clamp(H * H - 0.01, 0.0, 1.0) * clamp(1.0 - H * 0.1, 0.0, 1.0);
    vec2  Offset = -animXY * W.animspeed;
    vec3  Flow   = textureLod(s_waves, (pos.xz + Offset) * 0.018, 0.0).xyz;
    vec2  GrassMotion = (Flow.xy * 2.0 - 1.0) * W.turbulence;
    vec2  WindDir     = Flow.z * W.dir * W.push;
    return vec3(GrassMotion.x + WindDir.x, Flow.z * W.wave, GrassMotion.y + WindDir.y) * W.speed * HLimit;
}

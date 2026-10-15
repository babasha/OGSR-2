// Component of world_terrain.frag.glsl — textual include, not a standalone shader.
// Sun-beam GROUND DEPOSIT scaffolding: the froxel volume sampler + shaftOverhead().

// Integrated froxel volume (rgb = in-scatter, a = transmittance) on the shared EnvLight
// set. The sun-beam GROUND DEPOSIT (r_sun_beam_ground) probes it at this pixel to find
// where a visible shaft lands and deposits sun on the terrain there. Bound to a valid
// (eager) 3D view every frame; L.beam2.z = 0 disables the read.
layout(set = ENV_SET, binding = 27) uniform sampler3D uVolume;

// [PARKED 2026-07-06 — r_sun_beam_ground default 0, inert. Kept as scaffolding; see the
//  PARKED note in vk_console_min.cpp. Do not delete.]
// SUN-BEAM ground deposit signal — VIEW-INDEPENDENT. The volumetric in-scatter is
// view-dependent (HG phase + view-ray integration) → deposits flickered with the camera.
// Instead march a few short steps from the ground point UP toward the SUN and sample the
// crisp sun SHADOW (VSM atlas). The ground itself is occluded (r_terrain_debug 7), but a
// point a metre or two up toward the sun is INSIDE the beam column where a crown gap lets
// light through → lit. Under a thin gap the sun is found close overhead (strong pool);
// under a thick crown it stays shadowed for metres (dark) → this is exactly dappled sun
// through the canopy, and it does NOT depend on where the camera looks. Weight nearer
// hits more (the beam reaches closer to the ground). Cost-gated by distance (heavy atlas
// taps) and VSM-only (needs the crisp atlas the fog uses).
float shaftOverhead(vec3 wp)
{
    if (L.shadow_params.w <= 0.5) return 0.0;                     // VSM only
    if (distance(L.eye_pos.xyz, wp) > L.beam_params.y) return 0.0; // reuse r_sun_beam_dist as the cost gate
    vec3  toSun = normalize(-L.sun_dir.xyz);
    float best  = 0.0;
    for (int i = 1; i <= 4; ++i) {
        float h = float(i) * 0.9;                                 // 0.9 .. 3.6 m up toward the sun
        float v = vsmSunShadowGrassDirect(wp + toSun * h, L.beam_params.w);   // 1 lit .. 0 shadowed
        best = max(best, v * (1.0 - h * 0.22));                   // nearer sun-hit = stronger beam reaching down
    }
    return best;
}

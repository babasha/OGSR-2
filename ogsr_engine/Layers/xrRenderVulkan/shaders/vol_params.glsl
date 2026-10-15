// xrRenderVulkan — volumetric fog parameters (vk_volumetrics VolUBO).
//
// Layout MUST match VK::Volumetrics::VolUBO (vk_volumetrics.cpp). Both the INJECT
// and INTEGRATE passes bind the same buffer at set 0 binding 0 — integrate used to
// declare a hand-copied 20-field PREFIX of it, which is only correct while every new
// field is appended at the TAIL (the source even carried a "integrate prefix-safe"
// note next to one). One insertion in the middle would have shifted every later
// field there with no diagnostic, so the two now share this single declaration.
//
#ifndef VOL_PARAMS_GLSL
#define VOL_PARAMS_GLSL

struct VolLight { vec4 pos; vec4 color; vec4 dir; };  // pos.w=range, color.w=1 spot/0 point, dir.w=cos(cone/2)

layout(set = 0, binding = 0) uniform Vol {
    vec4 camPos;       // xyz world camera pos
    vec4 camDir;       // xyz camera forward (unit)
    vec4 camRightT;    // xyz right * tan(fovX/2)
    vec4 camTopT;      // xyz top   * tan(fovY/2)
    vec4 sun_dir;      // xyz sun TRAVEL dir (down); to-sun = -sun_dir
    vec4 sun_color;    // rgb
    vec4 sky_ambient;  // rgb flat fill
    mat4 sun_vp;       // far cascade VP
    mat4 sun_near_vp;  // cascade 0 VP
    mat4 sun_c1_vp;    // cascade 1 VP
    vec4 gridParams;   // x=dimX y=dimY z=dimZ
    vec4 zParams;      // x=near y=far z=log2(far/near)
    vec4 fog;          // x=baseDensity y=heightBase z=heightFalloff w=HG_g
    vec4 fog2;         // x=intensity y=ambient floor z=indoor density boost w=sun-beam boost
    mat4 rain_vp;      // top-down ortho VP for the sky-visibility (ambient) occlusion
    mat4 prevViewProj; // temporal reprojection (prev frame world→clip)
    vec4 prevCamPos;   // xyz prev cam pos, w = prev near
    vec4 prevCamDir;   // xyz prev cam forward, w = prev log2(far/near)
    vec4 temporal;     // xyz = froxel jitter (−0.5..0.5), w = history blend (0 = off)
    vec4 lightParams;  // P2: x = count, y = boost, z = unused (-1), w = point-shadowed idx (-1)
    VolLight lights[8];
    vec4 noiseParams;  // P3: x = amount, y = scale, z = speed, w = time
    // Spot shadow POOL: per-tile VP. A light's tile rides in color.w bits 2+
    // (tile+1, <<2) — every pooled spot's fog cone is cut by its own tile.
    mat4 spot_pool_vp[8];
    vec4 smokeParams;  // Stage-1 VMS: x = smoke-inject strength (0 = no injected smoke)
    vec4 light_occ;    // r_light_occ: x = enable, y = bury bias, z = frag-below band, w = strength
    // Atmospheric scattering (r_atmo): physical Rayleigh (blue) + Mie (forward halo)
    // in-scatter of the sun → aerial perspective. Appended last (integrate prefix-safe).
    vec4 atmo;         // x = enable, y = Rayleigh strength, z = Mie strength, w = Mie g
    vec4 atmoR;        // rgb = Rayleigh scattering tint (blue-heavy), w unused
    mat4 terra_vp;     // world → terrain-height-map clip (baked once per level)
    vec4 terra;        // x = map eye Y, y = zNear, z = zRange, w = valid (0 = eye-relative fallback)
    vec4 fog3;         // V-0: x = albedo (sigma_s = sigma_t * albedo), y = noise on scatter (0 = legacy: on extinction), zw reserved
    vec4 fog4;         // V-1: x = multiple-scattering octaves (1 = single scatter), y = backward lobe g, z = backward lobe weight, w = term isolation
    vec4 fog5;         // V-1: GROUND-MIST layer — x = density at ground, y = 1/thickness (m), z = HG g, w = hollow bias (m; 0 = layer is everywhere)
    vec4 fog6;         // V-1: MICRO relief from the rain map — x = dip depth scale (m), y = strength (0 = off), z = eyeY - zNear, w = zFar - zNear
    vec4 fog7;         // V-1b: mist thinning — x = density scale INSIDE (1 = off), y = near-camera radius (m; 0 = off), z = immersion falloff rate 1/m (0 = immersion off), w = V-2 hollow-gate bypass (0..1)
    vec4 canopy;       // GRASS CANOPY — x = extinction strength (0 = off), y = metres per height unit, z = tallest canopy (m), w unused
    vec4 canopy_uv;    // world XZ → canopy uv: u = x*canopy_uv.x + canopy_uv.y, v = z*canopy_uv.z + canopy_uv.w
    // STEP 6 — the GI field in froxel form (vk_gi_froxel). x = how much of the
    // ambient in-scatter the field owns (0 = the flat sky fill, unchanged), y = how
    // much of its directional band the phase function sees, z = the calibration gain
    // against that fill. Appended at the TAIL, which is the only safe place: the
    // integrate pass binds this same buffer and declares a PREFIX of it.
    vec4 gi;
} V;

#endif // VOL_PARAMS_GLSL

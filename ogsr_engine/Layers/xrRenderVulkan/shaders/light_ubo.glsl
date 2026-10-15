// xrRenderVulkan - shared per-frame lighting descriptor set (set 1).
//
// The DynLight struct, the big `Lighting` UBO (set 1, binding 0) and the set-1
// sampler bindings (1..13) were copy-pasted verbatim into world_lmap /
// world_terrain / world_vlit and their comments drifted out of sync. This is the
// SINGLE SOURCE OF TRUTH: change the UBO layout here once. Layout must match the
// std140 fill in vk_env_light.{h,cpp}. #include AFTER `#version`, and BEFORE any
// helper that reads `L` (shadow_common / env_common / light_shade / wetness).
#ifndef LIGHT_UBO_GLSL
#define LIGHT_UBO_GLSL

// The EnvLight descriptor set binds at set 1 (world / grass) and set 2 (skinned /
// tree) - the SAME shared layout (vk_env_light.h). Define ENV_SET before #include
// to pick the index (default 1). Lets tree/detail reuse this single UBO source.
#ifndef ENV_SET
#define ENV_SET 1
#endif

struct DynLight {
    vec4 pos;     // xyz = world position, w = range
    vec4 color;   // rgb = colour,         w = 1 spot / 0 point
    vec4 dir;     // xyz = spot direction, w = cos(cone/2)
};

layout(set = ENV_SET, binding = 0) uniform Lighting {
    vec4 sun_dir;        // xyz = travel dir (downward)
    vec4 sun_color;      // rgb (final: r_sun_boost premultiplied)
    vec4 hemi_color;     // rgb
    vec4 ambient;        // rgb (final: r_ambient_floor added)
    mat4 sun_vp;         // sun light view-proj (far cached map lookup)
    vec4 counts;         // x = dynamic light count
    DynLight lights[16];
    mat4 spot_vp;        // spot (flashlight) shadow view-proj
    vec4 shadow_params;  // x=spot-shadowed light idx (-1 none), y=point-shadowed idx, z=cookie on, w=VSM gate
    mat4 sun_near_vp;    // sun cascade 0 view-proj (25 m, per-frame, R4 scheme)
    mat4 sun_c1_vp;      // sun cascade 1 view-proj (60 m, per-frame)
    vec4 fog_color;      // rgb haze colour (env)
    vec4 fog_params;     // x=-near*r, y=near, z=far, w=r; fog = saturate(dist*w + x)
    vec4 eye_pos;        // xyz camera world pos
    vec4 sky_params;     // x=cube cross-fade weight, y=ambient scale, z=sample LOD, w=time
    vec4 ao_params;      // x=1/screenW, y=1/screenH, z=AO strength (0=off), w=ssao debug
    mat4 rain_vp;        // straight-down ortho VP for the rain occlusion map
    vec4 rain_params;    // x=rain density, y=wetness, z=darken (<0 = wet debug), w=reflection scale
    mat4 scene_vp;       // (SSR puddles - declared for layout match, unused in fragment)
    vec4 cam_dir;
    vec4 cam_rightT;
    vec4 cam_topT;
    vec4 pom_params;     // x=POM amplitude (UV), y=max steps, z=fade dist (m), w=on
    vec4 pom_params2;    // x=blur, y=normal, z=self-shadow, w=contact AO
    vec4 pom_params3;    // x=debug view, y=ao_flat, z=ceil strength, w=floor strength
    vec4 pom_params4;    // x=terrain POM enable, y=detail-normal, z=micro-AO, w=terrain debug
    vec4 pom_params5;    // x=terrain gloss, y=glass opacity, z=mud footprint strength (r_mud_deform), w=puddle debug
    vec4 pom_params6;    // x=water-sim enable, y=murk, z=refract
    vec4 pom_params7;    // SSS puddles: x=enable, y=level (coverage), z=mud print POM carve depth (r_mud_depth), w=macro scale
    vec4 cluster_params; // x=sliceScale, y=sliceBias, z=near, w=enable (0 off, 1 on, 2 debug)
    vec4 cluster_params2;// x=gridX, y=gridY, z=gridZ, w=maxLightsPerCluster
    vec4 lamp_terrain;   // x = bitmask over lights[0..15]: the lamp stands in another sector than the ground above it
                         //     (basement, bunker) -> it never reaches the terrain (vk_light.cpp);
                         // y = 1: the flashlight's shadow x cookie is in the half-res sun mask's A (r_flash_mask, the R1
                         //     receivers read it there and stay on the light variant);
                         // z = r_terra_probe (the R1 terrain's sub-experiments 7..13 on the terrain alone); w unused
    vec4 sf_params;      // Surface Field: x=enable (r_sf), y=debug view (r_sf_debug 0..5), z=derive eps m (r_sf_eps), w=snow amount (r_snow)
    // Snow footprint deformation: a ring of recent foot contacts (vk_env_light fills
    // from Skinned_CollectFeet). snow_displace.glsl (VS) carves the snow at these.
    vec4 deform_count;       // x=active count, y=press depth (m), z=ridge height (m), w=enable
    vec4 deform_stamps[256]; // xy=world XZ, z=radius (m), w=strength 0..1 (time-decayed, ~1 min trail)
    // Texture-based deform (r_snow_deform_tex): a dense persistent press field in a
    // player-centred ortho box (vk_deform), sampled instead of the stamp loop.
    mat4 deform_vp;          // world -> deform-texture NDC (straight-down ortho)
    vec4 deform_tex;         // x=enable, y=1/size, z=max depth (m), w=world metres per texel
    // Spot-light extras. x = grass shadow strength on surfaces (r_spot_grass_shadow):
    // spotShadowF blends the clean spot map (b2) with the spot+grass beam map (b22).
    vec4 spot_params;
    // Spot shadow POOL (4×2 atlas of 1024² tiles): per-light tile assignment
    // (packed 4 lights per vec4, value = tile+1, 0 = none) + per-tile view·proj.
    vec4 spot_assign[4];
    mat4 spot_pool_vp[8];
    // Point shadow POOL: per-light cube-array index (+1, packed 4/vec4, 0 = none).
    vec4 point_assign[4];
    // Sky specular IBL (vk_ibl): x = enable+ready, y = spec strength (r_ibl_spec),
    // z = max roughness mip, w = debug view (r_ibl_debug).
    vec4 ibl_params;
    // Sun-beam ground recovery (r_sun_beam): the volumetric shaft samples the crisp
    // VSM ATLAS but surfaces sample the temporally-SMEARED screen mask, so a thin sun
    // gap through a crown lands on the ground as nothing. Surfaces re-sample the crisp
    // atlas at their own world pos and take the max, recovering the gap so the ground
    // lights up in agreement with the shaft. x = recovery strength (0 = off), y = max
    // distance (m), z = extra-sun boost in the recovered gap (cinematic splash),
    // w = atlas self-bias (m along the sun ray). Appended last (prefix-safe).
    vec4 beam_params;
    // Sun-beam GROUND DEPOSIT (r_sun_beam_ground): x = froxel nearZ, y = log2(far/near),
    // z = deposit strength (0 = off), w = in-scatter luminance threshold. Paired with the
    // integrated froxel volume on binding 27 (declared by receivers that use the deposit).
    vec4 beam2;
    // Per-tile FLASHLIGHT grass-shadow strength (r_flashlight_grass). A handheld/worn
    // torch (CTorch/weapon) should paint CRISP grass-blade shadows in its ground pool
    // (the night "wow"), while wide fixtures keep the subtle spot_params.x blend. x =
    // 8-bit mask (bit t set → pooled tile t's owner is a flashlight), y = flashlight
    // grass strength (0..1). spotShadowF picks y for flashlight tiles, spot_params.x else.
    // z = texture mip-LOD bias, log2(render/display) ≤ 0 — set when DLSS renders below
    // display res so material fetches keep display-res texture detail (0 otherwise).
    // |w| = the corner of each atlas tile the spot maps are rendered into (r_spot_scale,
    // 1 = the whole tile), w < 0 = the one-gather 2x2 PCF instead of 3x3 (r_spot_pcf 0).
    vec4 spot_flash;
    // Terrain DEPTH OFFSET (r_pom_zoff, SSFX port): x = strength (0 = off, 1 = SSFX
    // 0.11 m). Sinks terrain gl_FragDepth into the POM cracks in BOTH the depth
    // prepass and the color pass, so GTAO / the VSM screen resolve shade INTO the
    // relief. y = r_terra_blend (terrain detail-blend depth: 0 = GAMMA plain-mask
    // cross-fade, >0 = Mishkinis height-blend width). z/w reserved. Appended last.
    vec4 zoff_params;
    // TERRAIN COMPOSITE CACHE (r_terra_cache, vk_terrain_cache): camera-anchored
    // baked height+weights clipmap on ENV bindings 28/29. tcache_xform maps
    // detail-uv -> cache-uv (cuv = duv * xy + zw); tcache_params.x = live (0/1,
    // 0 while the cache is unbaked or the cvar is off); .y = march features of
    // the CURRENT bake: 0 = fixed layers, 1 = cone-step (.g), 2 = cone + sun
    // horizon self-shadow (.b, r_terra_horizon). Appended last.
    vec4 tcache_xform;
    vec4 tcache_params;
    // Per-level terrain channel depth offsets (SSFX ssfx_terrain_offset, read
    // from gamedata\config\terrain_details.ltx by vk_env_light). R/G/B/A detail
    // heights shift by these before the POM march. Appended last (prefix-safe).
    vec4 ch_off;
    // BAKED terrain splat mask (mask-less maps, VK::TerrainMask): world XZ ->
    // mask UV affine, maskUV = (wp.xz - xy) * zw. .z == 0 -> no bake (sample the
    // material's own mask at vUV as always). Appended last (prefix-safe).
    vec4 tmask_params;
    // Diffuse sky irradiance via SH9 (vk_ibl + sky_sh_project.comp).
    //   x = SH strength: r_sky_sh × "coefficients have been projected" (0 = use the
    //       prefiltered-cube fallback instead),
    //   y = sky_rotation (rad) — needed ONLY by the raw-cube fallback, which still
    //       samples the weather cubes in their authoring space,
    //   z = probe top mip (the fallback's diffuse LOD), w reserved.
    // Appended last (prefix-safe).
    vec4 sh_params;
    // Puddles in REAL ground dips (r_puddle_geo). The SSFX recipe places puddles by
    // texture micro-height + a per-level artist mask, so a metre-scale hollow in the
    // asphalt collects nothing — the water level only ever sees centimetre texture
    // relief. x blends the Surface Field's concavity (SF_Concavity, the top-down map)
    // into the macro placement instead. Appended last (prefix-safe).
    vec4 puddle_geo;         // x = geo blend 0..1, y = wet reflection range (m), z = terrain detail range (m), w = 0 normal | 1 + v when sun shadow maps are off (v = camera sun visibility 0..1)
    // Material normal+gloss on statics (bump_common.glsl). x = normal strength
    // (r_bump), y = debug view (r_bump_debug: 1 world normal, 2 gloss), z = gloss
    // scale into IBL roughness (r_gloss_scale), w reserved. Appended last.
    vec4 bump_params;
    // SHORE WETNESS: xy = world XZ of the wetness tile's texel (0,0),
    // z = 1 / tile size in metres, w = strength (0 = off). Appended last.
    vec4 shorewet;
    // LEVEL WATER MAP: xy = world XZ of texel (0,0), z = 1 / size in metres,
    // w = enable. Appended last.
    vec4 waterlvl;
    // SWASH FOAM left on bared ground. x = strength (0 = off), y = the exponent
    // that turns the wetness map's own decay into a two-second foam clock (see
    // shoreFoam), z = time (s) for the drift, w reserved. Appended last.
    vec4 shorefoam;
    // DDGI probe volume in the LIGHTING (vk_ddgi, bindings 34/35/36). x = how much
    // of the sky-hemisphere fill the probe field replaces (r_ddgi_light x "the
    // volume is live"; 0 = the analytic path, untouched), y = how strongly the
    // BAKED sky access still multiplies the probe answer (r_ddgi_light_bake) —
    // the lightmap is the only thing in this renderer that knows about tree
    // crowns, which the collision BVH the probes trace does not contain.
    // z = how much of that GI term comes from the FINAL GATHER (binding 37,
    // vk_pass_gather) instead of the probe lookup — the same quantity resolved per
    // pixel by a traced ray, so it is a blend between two estimates and not an
    // addition. 0 whenever the gather has produced no image, which is what keeps a
    // receiver from sampling a target that has never been written.
    // w reserved. Appended last (prefix-safe).
    vec4 ddgi_params;
    // FINAL GATHER upsample (r_gather_upsample). x = 1 → bilateral 4-tap upsample of
    // the half-res gather (weights by the view-depth the resolve puts in its alpha),
    // 0 → the single bilinear tap this renderer used before.
    // y = 1 → the gather also published a 2-BAND SH this frame (r_gather_sh, binding
    // 38) and the receiver re-evaluates it for its own shading normal instead of
    // reading one flat colour; 0 whenever that image is not bound.
    // z = how strongly the directional term counts (r_gather_sh_k) — 1 is the
    // calibrated value, above it the reconstruction is deliberately exaggerated to
    // make the term visible in an A/B. w reserved. Appended last (prefix-safe).
    vec4 gather_params;
    // Sky SH9 coefficients mirrored from the binding-31 SSBO (same 9 x vec4 layout); valid when sh_params.w == 1.
    // A fragment shader's UBO is a hardware cbuf on GM20B, the SSBO is 9 global loads per pixel. Appended last.
    vec4 sky_sh[9];
    vec4 fog_sky;        // 10-10: rgb = sky tint (sky_color x 1.7, linear), w = horizon band; 0 = flat fog_color (nx_fog.glsl)
} L;

// Splat-mask UV: baked world-space mask (mask-less maps) or the material's own
// repeating detail-uv mask. Shared by the terrain color/depth/tess stages so
// they all cut the SAME regions.
vec2 terrainMaskUV(vec2 uv, vec3 wp)
{
    return (L.tmask_params.z > 0.0) ? (wp.xz - L.tmask_params.xy) * L.tmask_params.zw : uv;
}

// Atlas tile of collected light `gi` (-1 = the light casts no spot shadow).
int spotTileOf(int gi)
{
    if (gi < 0 || gi >= 16) return -1;
    return int(L.spot_assign[gi >> 2][gi & 3] + 0.5) - 1;
}
// Cube-array index of collected light `gi` (-1 = no point shadow).
int pointCubeOf(int gi)
{
    if (gi < 0 || gi >= 16) return -1;
    return int(L.point_assign[gi >> 2][gi & 3] + 0.5) - 1;
}

layout(set = ENV_SET, binding = 1)  uniform sampler2D   uShadow;       // far sun map (cached)
layout(set = ENV_SET, binding = 2)  uniform sampler2D   uSpotShadow;   // spot (flashlight) shadow map
layout(set = ENV_SET, binding = 3)  uniform samplerCubeArray uPointShadow;  // point shadow cube POOL
layout(set = ENV_SET, binding = 4)  uniform sampler2D   uShadowNear;   // sun cascade 0 (~0.61 cm texels)
layout(set = ENV_SET, binding = 5)  uniform sampler2D   uShadowC1;     // sun cascade 1 (~1.46 cm texels)
layout(set = ENV_SET, binding = 6)  uniform samplerCube uSky0;         // sky ambient cube 0 (weather A)
layout(set = ENV_SET, binding = 7)  uniform samplerCube uSky1;         // sky ambient cube 1 (weather B)
layout(set = ENV_SET, binding = 8)  uniform sampler2D   uAO;           // GTAO (half-res, bilinear upsample)
layout(set = ENV_SET, binding = 9)  uniform sampler2D   uRainMap;      // top-down rain occlusion (wetness mask)
layout(set = ENV_SET, binding = 10) uniform sampler2D   uSpotCookie;   // flashlight beam texture (cookie)
layout(set = ENV_SET, binding = 11) uniform sampler2D   uWater;        // water depth (flow sim, metres)
layout(set = ENV_SET, binding = 12) uniform sampler2D   uFlow;         // water velocity (flow sim, uv/sec)
layout(set = ENV_SET, binding = 13) uniform sampler2D   uGround;       // top-down ground-height (statics+terrain, no trees)
layout(set = ENV_SET, binding = 20) uniform sampler2D   uDeform;       // snow deform press field (vk_deform; r_snow_deform_tex)
// SSIL one-bounce indirect light (half-res, r_ssil; black until ready). Declared
// here (harmless in VS/TES — just a binding) but consumed only in fragment stages
// via ssilBoost() in env_common.glsl / the local copies (which use gl_FragCoord,
// so the FUNCTION cannot live in this stage-shared header).
layout(set = ENV_SET, binding = 21) uniform sampler2D   uIL;
// Spot BEAM map: the spot map + grass casters (see vk_pass_shadow). spotShadowF
// blends it with the clean uSpotShadow so grass shadows surfaces partially.
layout(set = ENV_SET, binding = 22) uniform sampler2D   uSpotShadowGrass;
// Sky specular IBL: prefiltered environment cube (roughness mips), sampled by
// iblSpecular() in env_common.glsl. Grey fallback until vk_ibl has a probe.
layout(set = ENV_SET, binding = 26) uniform samplerCube uSkySpec;
// Diffuse sky irradiance coefficients (9 × vec4, std430), projected from the
// world-space probe once per weather change. Read by skyAmbient() in
// env_common.glsl. Bound to a dummy buffer until the first projection lands —
// sh_params.x gates the read, so the dummy is never actually consumed.
layout(std430, set = ENV_SET, binding = 31) readonly buffer SkySH {
    vec4 c[9];
} skySH;

#endif // LIGHT_UBO_GLSL

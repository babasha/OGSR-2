// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — shared per-frame environment lighting UBO.
//
// One small uniform buffer (sun/hemi/ambient from CEnvDescriptorMixer, the same
// source the sky reads) made available as a descriptor set to every forward pass
// that shades with real light: the world pass binds it at set 1, the skinned pass
// at set 2. The set LAYOUT is shared (a descriptor set can bind to any set index
// whose pipeline-layout slot uses the same layout), so all passes read identical
// lighting and the env is sampled once per frame.
//
// Per in-flight slot: its own UBO region + descriptor set, written only after the
// slot's fence proved the GPU finished the previous frame-slot read (same
// discipline as the bone SSBO / sky sets). Groundwork for STEP 2 (shadows): the
// light-space matrix + shadow sampler will join this set.
#pragma once
#include "HW_Vulkan.h"
#include "vk_light.h"   // Lights::GpuLight (dynamic point/spot lights, STEP 3)
#include <cstddef>      // offsetof (LightUBO layout guards)

namespace VK { namespace EnvLight {

constexpr u32 kMaxGpuLights = Lights::kMaxLights;   // nearest active dynamic lights per frame

// Must match the `Lighting` UBO block in the world / skinned fragment shaders.
struct LightUBO {
    float sun_dir[4];     // xyz = travel dir (downward, .y<0); direction-to-sun = -sun_dir
    float sun_color[4];   // rgb
    float hemi_color[4];  // rgb (w = R2 correction)
    float ambient[4];     // rgb
    float sun_vp[16];     // sun light view·proj (row-major) for shadow lookup (STEP 2)
    float counts[4];      // x = active dynamic light count (as float), yzw unused
    Lights::GpuLight lights[kMaxGpuLights];   // 3 vec4 each (STEP 3)
    float spot_vp[16];    // spot (flashlight) shadow view·proj (STEP 3b)
    float shadow_params[4]; // x = spot-shadowed gpu[] index (-1 none), y = point-shadowed index, z unused
    float sun_near_vp[16];  // sun cascade 0 view·proj (25 m, R4 scheme) — appended
                            // at the END so foliage prefix declarations stay valid
    float sun_c1_vp[16];    // sun cascade 1 view·proj (60 m)
    // Distance fog (R4 combine_1.ps port): the "wet air" / haze that fades the
    // street into fog_color with distance. Appended last (prefix-safe).
    float fog_color[4];     // rgb (env), w unused
    float fog_params[4];    // x=-near*r, y=near, z=far, w=r=1/(far-near); fog = saturate(dist*w + x)
    float eye_pos[4];       // xyz = camera world pos (for dist = length(worldPos - eye)), w unused
    // Hemisphere sky ambient (R4 hmodel.h): the sky cubes are bound at set
    // bindings 6/7; this gives the per-frame cross-fade + tuning knobs.
    float sky_params[4];    // x = cube cross-fade weight, y = ambient scale, z = sample LOD (blur), w unused
    // GTAO (binding 8, vk_pass_ssao): receivers sample the half-res AO at
    // gl_FragCoord × xy and multiply their hemi+ambient terms (R4 combine_1.ps
    // parity). z = strength EXPONENT (pow(ao, z): 0 = off, 1 = raw, 2-3 deeper),
    // w = debug flag (world shaders draw the raw AO map).
    float ao_params[4];     // x = 1/screenW, y = 1/screenH, z = strength exp, w = debug
    // Rain wetness (binding 9 = top-down rain occlusion map, vk_shadow):
    // receivers darken albedo + add a sky reflection where wetness × rain-map
    // visibility says the surface is rained on. Appended last (prefix-safe).
    float rain_vp[16];      // straight-down ortho view·proj for the rain map lookup
    float rain_params[4];   // x = rain_density, y = wetness_factor, z = darken, w = reflection scale
    // Scene camera (tonemap SSR puddles): this frame's view·proj + the
    // frustum-ray basis (SSAO DeriveProjTerms scheme) for depth→world.
    float scene_vp[16];
    float cam_dir[4];       // xyz = camera forward (unit), w = proj _33
    float cam_rightT[4];    // xyz = right × tan(fovX/2),   w = proj _43
    float cam_topT[4];      // xyz = top × tan(fovY/2),     w unused
    // Parallax occlusion mapping (world lmap/vlit fragment): x = UV-space
    // march amplitude, y = max steps, z = distance fade (m), w = on/off.
    float pom_params[4];
    float pom_params2[4];   // x = blur (extra mip LOD), y = normal, z = self-shadow, w = contact AO
    float pom_params3[4];   // x = debug view, y = ao_flat, z = ceil strength, w = floor strength
    float pom_params4[4];   // x = terrain POM enable, y = detail-normal strength, z = micro-AO, w = debug view
    float pom_params5[4];   // x = terrain dry gloss strength, y = glass opacity ceiling, z = mud footprint strength (r_mud_deform), w = puddle debug
    float pom_params6[4];   // x = water-sim enable (puddles come from the flow sim), y = murk, z = refract, w unused
    // SSS per-pixel puddles (SSFX deffer_terrain_high_flat port): water as a rising
    // LEVEL vs the detail micro-height — terrain texture relief (ruts) pools first.
    float pom_params7[4];   // x = enable, y = water level, z = RESERVED (was micro-height contrast, removed; zeroed on fill), w = macro mask scale
    // Clustered forward (vk_clustered): the froxel-grid parameters the fragment
    // needs to find its cluster. The light list itself lives in SSBOs bound at
    // set bindings 17 (lights) / 18 (grid) / 19 (indices). Appended last (prefix-safe).
    float cluster_params[4];  // x = sliceScale, y = sliceBias, z = near (m), w = enable (r_clustered)
    float cluster_params2[4]; // x = grid X, y = grid Y, z = grid Z, w = max lights / cluster (debug scale)
    // Lamps that never reach the terrain: x = bitmask over lights[0..15], set for a
    // lamp standing in another sector than the ground above it (a basement / bunker
    // lamp under the village). Decided once per lamp on the CPU (vk_light.cpp).
    float lamp_terrain[4];    // x = bitmask (exact in a float: 16 bits), yzw unused
    // Surface Field ("smart heightmap"): metre-scale ground height/slope/curvature/
    // exposure derived from the rain+ground maps (shaders/surface_field.glsl).
    float sf_params[4];       // x = enable (r_sf), y = debug view (r_sf_debug 0..5), z = derive eps (m, r_sf_eps), w = snow amount (r_snow)
    // Snow footprint deformation: ring of recent foot contacts (filled from
    // Skinned_CollectFeet). Read by snow_displace.glsl (VS) to carve footprints.
    float deform_count[4];     // x = active count, y = press depth (m), z = ridge height (m), w = enable
    float deform_stamps[256][4];// xy = world XZ, z = radius (m), w = strength 0..1 (256 + time-decay = ~1 min trail)
    // Texture-based deform (r_snow_deform_tex): a dense persistent press field in a
    // player-centred ortho box (vk_deform, EnvLight binding 20). Appended last.
    float deform_vp[16];       // world -> deform-texture NDC (straight-down ortho)
    float deform_tex[4];       // x = enable, y = 1/size, z = max depth (m), w = world metres per texel
    // Spot-light extras. x = grass shadow strength on SURFACES
    // (r_spot_grass_shadow): spotShadowF blends the clean spot map with the
    // spot+grass beam map — 0 = grass never shadows surfaces (sterile pool),
    // 1 = the old one-map full blanket that ate the headlight's ground pool.
    float spot_params[4];
    // Spot shadow POOL (vk_pass_shadow): per-collected-light atlas tile and the
    // per-tile view·proj. spot_assign packs 4 lights per vec4 (value = tile+1,
    // 0 = the light has no tile) — shaders: spot_assign[gi>>2][gi&3].
    float spot_assign[4][4];
    float spot_pool_vp[Lights::kMaxShadowSpots][16];
    // Point shadow POOL: per-collected-light cube-array index (+1, packed
    // 4/vec4, 0 = no cube). pointShadowF samples uPointShadow[cube].
    float point_assign[4][4];
    // Sky specular IBL (vk_ibl, binding 26 = prefiltered cube). x = enable+ready
    // (0 off / no probe yet), y = spec strength (r_ibl_spec), z = max roughness
    // mip, w = debug view (r_ibl_debug). Appended last (prefix-safe).
    float ibl_params[4];
    // Sun-beam ground recovery (r_sun_beam): surfaces re-sample the crisp VSM atlas
    // at their own world pos and max() it with the smeared screen mask so a thin sun
    // gap through a crown lands on the ground instead of dissolving. x = recovery
    // strength (0 = off), y = max distance (m), z = extra-sun boost in the recovered
    // gap, w = atlas self-bias (m). Appended last (prefix-safe).
    float beam_params[4];
    // Sun-beam GROUND DEPOSIT (r_sun_beam_ground): x = froxel nearZ, y = log2(far/near)
    // (exp-Z terms for Froxel_SliceFromViewZ), z = deposit strength (0 = off / r_vol off),
    // w = in-scatter luminance threshold. Paired with binding 27 (integrated volume).
    float beam2[4];
    // Per-tile FLASHLIGHT grass-shadow strength (r_flashlight_grass): x = 8-bit mask
    // (bit t → pooled tile t's owner is a handheld torch), y = flashlight grass strength.
    // Flashlight tiles paint crisp grass dapples in their ground pool (night wow); wide
    // fixtures keep the subtle spot_params.x blend. Appended last (prefix-safe).
    float spot_flash[4];
    // Terrain DEPTH OFFSET (r_pom_zoff, SSFX port): x = strength (0 = off, 1 = SSFX
    // 0.11 m along the view ray). y/z/w reserved. Appended last (prefix-safe).
    float zoff_params[4];
    // TERRAIN COMPOSITE CACHE (r_terra_cache, vk_terrain_cache): detail-uv ->
    // cache-uv transform (cuv = duv*xy + zw) + x = cache live flag. Appended last.
    float tcache_xform[4];
    float tcache_params[4];
    // Per-level terrain channel depth offsets (SSFX ssfx_terrain_offset):
    // R/G/B/A detail heights shift by these before the POM march, so e.g. the
    // asphalt channel sits sunken below the soil. Read per level from
    // gamedata\config\terrain_details.ltx. Appended last (prefix-safe).
    float ch_off[4];
    // BAKED terrain splat mask (mask-less maps, VK::TerrainMask): world XZ ->
    // mask UV affine (ox, oz, 1/sx, 1/sz); z == 0 -> no bake. Appended last.
    float tmask_params[4];
    // Diffuse sky irradiance via SH9 (vk_ibl + sky_sh_project.comp), read by
    // skyAmbient() in sky_ambient.glsl. x = r_sky_sh × "coefficients projected"
    // (0 = fall back to the prefiltered probe's top mip), y = sky_rotation (rad,
    // used only by the raw-cube last resort), z = probe top mip, w reserved.
    // Appended last (prefix-safe).
    float sh_params[4];
    // Puddles in REAL ground dips (r_puddle_geo): blends the Surface Field concavity
    // into the puddle macro placement, which otherwise only knows texture-scale
    // micro-height. Appended last (prefix-safe).
    float puddle_geo[4];      // x = geo blend 0..1, y = wet reflection range (m, r_wet_dist), z = terrain detail range (m), w reserved
    // Material normal+gloss on statics (`<bump>.dds`, set 0 binding 4). x = normal
    // strength (r_bump), y = debug view (r_bump_debug), z = gloss scale feeding the
    // IBL roughness (r_gloss_scale), w reserved. Appended last (prefix-safe).
    float bump_params[4];
    // SHORE WETNESS (binding 32). Water that has TOUCHED a surface leaves it dark
    // and glossy, and it dries afterwards — every wet surface in this renderer used
    // to be wet because of rain and nothing else. x/y = world XZ of the tile's
    // texel (0,0), z = 1/tile size in metres, w = strength (0 = off).
    // Appended last (prefix-safe).
    float shorewet[4];
    // LEVEL WATER MAP (binding 33). xy = world XZ of texel (0,0), z = 1/size in
    // metres, w = the dry sentinel. Makes wetness a fact about the WORLD instead
    // of about how close the player got. Appended last (prefix-safe).
    float waterlvl[4];
    // SWASH FOAM on bared ground. x = strength (r_wtr_foam_land), y = the exponent
    // that reads a two-second foam clock out of the wetness map's own decay
    // (= r_wtr_wet_dry / foam life — see shoreFoam in shore_wet.glsl), z = time in
    // seconds for the drift, w reserved. Appended last (prefix-safe).
    float shorefoam[4];
    // DDGI PROBE VOLUME IN THE LIGHTING (vk_ddgi; atlases at bindings 34/35/36).
    // x = how much of the sky-hemisphere fill the probe field replaces
    //     (r_ddgi_light × "the volume is live"; 0 = the analytic path untouched —
    //     a REPLACEMENT and not an addition, because a probe's irradiance already
    //     contains the sky the analytic term estimates; see ddgi_receiver.glsl).
    // y = how strongly the BAKED sky access still multiplies the probe answer
    //     (r_ddgi_light_bake). The lightmap is the only thing in this renderer
    //     that knows about tree crowns, and crowns carry no collision, so they are
    //     absent from the BVH the probes trace.
    // z/w reserved. Appended last (prefix-safe).
    float ddgi_params[4];
    // FINAL GATHER upsample. x = r_gather_upsample (1 = bilateral 4-tap, 0 = the old
    // single bilinear tap); y = 1 when the gather also published its 2-BAND SH this
    // frame (r_gather_sh, binding 38) so the receiver re-evaluates it for its own
    // shading normal; z = how strongly that directional term counts (r_gather_sh_k,
    // 1 = the calibrated value). y and z are 0 whenever the SH image is not bound — the
    // receivers must never run the bilateral against an alpha nobody wrote.
    // y/z/w reserved. Appended last (prefix-safe).
    float gather_params[4];
    // Sky SH9 irradiance coefficients (9 x vec4, same layout as the binding-31 SSBO), valid when sh_params[3] == 1:
    // a fragment shader's UBO is a hardware cbuf on GM20B, the SSBO is 9 global loads per pixel. Appended last.
    float sky_sh[9][4];
    // 10-10 the fog dissolves into the SKY (r_fog_sky, Switch preset): rgb = the sky draw's tint (sky_color x 1.7,
    // linear), w = its horizon band (r_sky_fog_horizon, > 0 while on; 0 = the flat fog_color, as before). Appended last.
    float fog_sky[4];
};
static_assert(sizeof(LightUBO) == 128 + 16 + 48 * kMaxGpuLights + 80 + 64 + 64 + 48 + 16 + 16 + 80 + 112 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 * 256 + 64 + 16 + 16 + 64 + 64 * Lights::kMaxShadowSpots + 64 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 16 + 144 + 16,
              "LightUBO must match the GLSL Lighting block");
// Per-field offset guards. The size-only assert above still passes if two equal-
// sized fields are swapped; these pin the layout at structural anchors and across
// the POM/cluster tail (where new params get appended), so a reorder/resize is a
// build error. Offsets relative to neighbours stay valid if kMaxGpuLights changes.
static_assert(offsetof(LightUBO, lights)  == 144, "LightUBO layout drift before lights[]");
static_assert(offsetof(LightUBO, spot_vp) - offsetof(LightUBO, lights) == 48 * kMaxGpuLights, "LightUBO lights[] size drift");
static_assert(offsetof(LightUBO, pom_params5)    - offsetof(LightUBO, pom_params)     == 64,  "LightUBO POM block drift");
static_assert(offsetof(LightUBO, cluster_params) - offsetof(LightUBO, pom_params)     == 112, "LightUBO POM->cluster tail drift");
static_assert(offsetof(LightUBO, lamp_terrain)   - offsetof(LightUBO, cluster_params) == 32,  "LightUBO cluster->light_occ drift");
static_assert(offsetof(LightUBO, sf_params)      - offsetof(LightUBO, lamp_terrain)   == 16,  "LightUBO light_occ->sf_params drift");
static_assert(offsetof(LightUBO, deform_count)   - offsetof(LightUBO, sf_params)      == 16,  "LightUBO sf_params->deform_count drift");
static_assert(offsetof(LightUBO, deform_stamps)  - offsetof(LightUBO, deform_count)   == 16,  "LightUBO deform_count->stamps drift");

bool                  Init();                 // idempotent; safe to call from multiple pass inits
void                  Destroy();
VkDescriptorSetLayout GetSetLayout();          // the shared set layout (binding 0 = UBO, FRAGMENT)
// Fill slot's UBO from env; remember it as the current set.
// `cmd` (optional) is the frame's command buffer and must be OUTSIDE any dynamic-
// rendering scope: given one, Update also records the sky-probe prefilter refresh
// (IBL::Update) into it. Call sites that cannot promise that pass nothing — the probe
// then keeps last frame's content. See vk_ibl.h for why this is no longer a blocking
// immediate submit.
// RENDER_THREAD_PLAN step 12: the ObjectSpace ray of the sun-visibility probe, PREPARE phase (extract side).
void                  ExtractSunProbe();
void                  Update(u32 slot, VkCommandBuffer cmd = VK_NULL_HANDLE);
// nfs18: the sky irradiance arriving from straight up (skyAmbientUp() of the shaders), evaluated on the CPU from
// the SH9 coefficients this frame's Update() mirrored into the UBO. false = not available yet (no SH projection).
bool SkyAmbientUpCPU(Fvector& out);
VkDescriptorSet       GetCurrentSet();         // set chosen by the last Update() this frame
bool                  SkinnedLampShadows();    // a lamp has a map or cookie this frame (skinned keeps WS_LAMPSH)
// Crown ubershader variant bits for this frame (TreeSpec in vk_TreeManager.h,
// tree_variants.glsl in the shader). Built inside Update() from the very UBO
// fields the crown FS branches on, so the mask cannot disagree with the shader —
// the same contract PipelineCache::SetFrameSpecMask has for the world uber-FS.
u32                   TreeSpecMask();
const float*          TerrainChOff();          // per-level SSFX channel depth offsets (terrain_details.ltx), float[4]
// Sun direction for anything that DRAWS the sun (sky disc, volumetric sun beam).
// Identical to CurrentEnv->sun_dir except during a thunderbolt, when the engine
// hijacks that field to point at the strike — see CEnvironment::ThunderboltFlash.
const Fvector&        SunDirVisual();
// The sky-ambient strength every forward receiver applies as L.sky_params.y.
// Exposed because the DDGI probes have to integrate the sky at the SAME scale as
// the term their field replaces — otherwise r_ddgi_light would be an exposure
// knob rather than a GI blend (see vk_ddgi.cpp).
float                 AmbientScale();

}}  // namespace VK::EnvLight

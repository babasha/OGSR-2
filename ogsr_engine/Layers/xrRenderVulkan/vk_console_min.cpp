// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// Vulkan port of xrRender_console.cpp. Adapted from
// Layers/xrRender/xrRender_console.cpp — variables + CMD registrations are
// shared; the few R4-only command classes (CCC_DumpResources, CCC_OCC_Enable,
// CCC_VideoMemoryStats, ...) that reach into RImplementation / HW.stats_manager
// are stripped — Settings menu only needs the value/token registrations.
#include "stdafx.h"

#include "../xrRender/xrRender_console.h"
#include "vk_profiler.h"   // VK::Prof::RequestMark — the `vk_perf` MARK command
#include "vk_texture_stream.h"   // VK::TextureStreamer — video_memory_stats
#include "vk_vram_stats.h"       // VK::Vram::DumpVmaJson — r_vram_dump
#include "vk_water_ripple.h"     // VK::WaterRipple::Splat — the r_wtr_drop probe
#include "vk_UIPipeline.h"       // VulkanUI::TraceUIPass — the `ui_pass_trace` command
#include "vk_pass_registry.h"  // VK::DumpPassDeps — the `vk_pass_deps` command

// DLSS preset enum values used in default initializers; pull them in directly
// rather than depending on the Vulkan-side DLSS wrapper.
enum {
    NVSDK_NGX_DLSS_Hint_Render_Preset_Default = 0,
    NVSDK_NGX_DLSS_Hint_Render_Preset_F       = 6,
    NVSDK_NGX_DLSS_Hint_Render_Preset_J       = 10,
    NVSDK_NGX_DLSS_Hint_Render_Preset_K       = 11,
};
// Ray Reconstruction runs a DIFFERENT model family with its own preset enum —
// mirrored here for the same reason as above. Presets A/B/C were removed by
// NVIDIA and everything past E is documented as "reverts to default behavior",
// so D and E are the only two values worth offering.
enum {
    NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default = 0,
    NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D       = 4,   // default transformer
    NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E       = 5,   // latest transformer
};

u32 ps_r_pp_aa_mode = DLSS;

// Preset K = the DLSS 4 transformer model (best quality, esp. Perf/UltraPerf —
// UltraPerf otherwise DEFAULTS to the old CNN preset F = «мыло»). 0 = driver default.
u32 ps_r_dlss_preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
constexpr xr_token dlss_mode_token[]{
    {"st_opt_dlss_default", NVSDK_NGX_DLSS_Hint_Render_Preset_Default}, // default behavior, may or may not change after OTA
    {"st_opt_dlss_f", NVSDK_NGX_DLSS_Hint_Render_Preset_F},
    {"st_opt_dlss_j", NVSDK_NGX_DLSS_Hint_Render_Preset_J},
    {"st_opt_dlss_k", NVSDK_NGX_DLSS_Hint_Render_Preset_K},
    {},
};

u32 ps_preset = 5;   // Switch по умолчанию: дальше работаем ОТ пола, а не от «как было»
// Switch (03-10-2026; бывший Potato 18-09 + консольный набор, который x_ray.cpp
// выставлял под __SWITCH__) — ступень НИЖЕ Minimum. Код один на ПК и Switch;
// Switch = платформа + этот пресет (rspec_vk_switch.ltx). Minimum остаётся
// играбельным низким тиром для слабого ПК.
// 📏Замер Potato: default 15.70 / minimum 7.89 / potato 5.51 мс, GPU 1.06 мс против CPU 4.00.
// ⚠Токен идёт ПЕРВЫМ: список читается сверху вниз как «от дешёвого к дорогому».
// "Potato" в конце — синоним для старых user.ltx (сохраняется уже как Switch: имя берётся по первому совпадению).
constexpr xr_token qpreset_token[] = {{"Switch", 5}, {"Minimum", 0}, {"Low", 1}, {"Default", 2}, {"High", 3}, {"Extreme", 4}, {"Potato", 5}, {nullptr, 0}};

u32 ps_r_ao_quality = 0;
constexpr xr_token qssao_token[] = {{"st_opt_off", 0},
                                    {"st_opt_low", 1},
                                    {"st_opt_medium", 2},
                                    {"st_opt_high", 3},
                                    {nullptr, 0}};

// Vulkan GTAO (vk_pass_ssao): live debug view + strength, no restart needed.
int   ps_r_ssao_debug    = 0;     // 1 = draw the raw AO map instead of the scene
// Global GTAO on/off (r_ssao): 0 SKIPS the whole pass (depth flip + Execute) →
// zero GPU cost, binding 8 falls back to white. For A/B perf measurement and a
// real off switch (r2_ssao 0 is treated as "medium", not off).
int   ps_r_ssao_enable   = 1;
// Strength is an exponent on the AO value (0 = off, 1 = raw GTAO). Default 2:
// our AO input is the depth prepass (statics+trees, no grass/NPCs), and the
// forward path applies AO to a smaller ambient share than R4's deferred
// hemisphere — the deepened curve compensates to a comparable look.
float ps_r_ssao_strength = 2.0f;

// Vulkan SSIL — screen-space indirect lighting (one-bounce SSGI). The "IL +
// COLOR" complement to GTAO's "AO": adds the coloured light bouncing off nearby
// lit surfaces. FOLDED INTO the GTAO horizon march (the occluder that raises the
// horizon also bounces its colour), gathered from the PREVIOUS frame's lit scene
// (history buffer — also the temporal foundation). Requires r_ssao on (shared
// pass). Reach = the GTAO radius.
// DEFAULT ON (2026-07-04): temporal accumulation (r_ssil_temporal, below) resolves
// the half-res banding, user-verified clean in-game → shipped on. r_ssil 0 to A/B
// against the no-bounce look. AO is unaffected either way.
int   ps_r_ssil_enable   = 1;     // r_ssil — global on/off (gates the prev-colour taps in the GTAO march)
int   ps_r_ssil_debug    = 0;     // r_ssil_debug — 1 = show ONLY the indirect bounce field
// Strength multiplies the gathered radiance at composite. IL is an occluder-colour
// AVERAGE gated by (1-AO)² (≈0 on open surfaces, only fills real recesses), added
// on top of the forward ambient — keep it subtle so it tints corners without a
// global brightness boost. Raise to taste.
float ps_r_ssil_strength = 0.5f;

// r_ssil_temporal — temporal accumulation for the GTAO pass (the missing piece
// that lets r_ssil ship ON). The horizon-folded IL bands without it because the
// 4-slice gather is directionally quantized AND static per frame (so a plain EMA
// of identical frames is a no-op). With temporal > 0 the gather noise is rotated
// PER FRAME (so each frame's bands differ) and the result is reprojected via the
// motion vectors ([[vk_motionvec]]) and EMA-blended — the per-frame patterns
// average into a smooth fill. AO shares the same march, so it is accumulated too
// (≥ the current spatial-only quality, just temporally stable). The value IS the
// history weight α (0 = OFF → byte-identical to the spatial-only path; ~0.9 =
// strong smoothing). Needs r_motion_vectors on for reprojection (falls back to
// same-pixel EMA when MV is unavailable).
// DEFAULT ON 0.9 (2026-07-04): this is what lets r_ssil ship on (kills the IL bands);
// verified clean, no ghosting. Set 0 to inspect the raw spatial-only IL.
float ps_r_ssil_temporal = 0.9f;

// r_ssao_temporal — the SAME temporal accumulation, but as a first-class control
// for the AO channel (so it works WITHOUT r_ssil). Value = history weight α. The
// gather noise (slice direction + radial phase) is rotated per frame → each frame
// is a DIFFERENT realisation of the AO → MV-reprojected EMA averages them into a
// stable, low-noise result, which also lets the sample count drop (r2_ssao) without
// banding showing. 0 = OFF (byte-identical spatial path); ~0.85 = clean+stable.
// Needs r_motion_vectors on for moving-camera reprojection (else same-pixel EMA —
// fine when still). AO and IL share one jitter+history; the effective α used by the
// blur is max(r_ssao_temporal, r_ssil_temporal).
// DEFAULT ON 0.85 (2026-06-21): measured ~free (no SSAO cost change vs 0) and it
// keeps AO stable + clean at the now-default LOW sample count (see SampleCount()).
// Reprojection rides r_motion_vectors (also default ON); MV off → same-pixel EMA.
float ps_r_ssao_temporal = 0.85f;

// r_ssao_reproj — HOW the AO/IL history is brought into this frame's pixels.
//   1 (default) = reproject from the prepass depth through the PREVIOUS frame's
//       view-proj inside ssao_blur (exact for statics, computed THIS frame), and
//       reject the history where its stored view-depth disagrees with the
//       reprojected one (disocclusion). Independent of the MV pass.
//   0 = legacy: sample the motion-vector target. ⚠ That target is written by
//       MotionVecDyn AFTER the World pass, so the SSAO blur reads the field of the
//       PREVIOUS frame — a one-frame lag that, under mouse look (velocity changes
//       every frame), leaves the history offset by the acceleration and the α≈0.9
//       EMA smears it over ~10 frames: AO peels off corners and creeps back.
//       Kept for an in-run A/B.
int   ps_r_ssao_reproj = 2;

// GTAO grazing-angle fade threshold (N·V). The depth-reconstructed normal is
// unreliable at grazing angles, where a flat OPEN surface fails to cancel its own
// horizon → residual self-occlusion (AO<1) that r_ssao_strength's pow amplifies.
// Below this N·V the AO fades back to fully open; head-on surfaces keep full AO
// (real contact shade). 0 = off; higher = fades more of the grazing floor.
// (The related bent-normal striping has its own fix: the geomN deadband in
// env_common.glsl gtaoBentN — so this knob may now afford a lower value.)
float ps_r_ssao_bias = 0.30f;

// ---------------------------------------------------------------------------
// RTAO — hardware ray-traced ambient occlusion (vk_rt_scene + vk_pass_rtao).
//
// Traced against a camera-local BLAS cut out of level.cform: the whole level is
// 61 M collision triangles on pripyat_full, so only a bubble around the player
// is ever built, and it is rebuilt on a worker when the camera leaves its inner
// margin. The result is MIN-blended over the GTAO map, so screen-space AO still
// covers everything the static BVH cannot see (NPCs, doors, weapons).
//
// r_rtao / r_rtao_debug are deliberately NoSave: a gate that quietly sticks in
// user.ltx is how four r_ssao knobs ended up silently on during a measurement
// (see [[gate-cvars-must-not-be-savable]]).
int   ps_r_rtao         = 0;      // master gate (0 = GTAO only, as before)
int   ps_r_rtao_debug   = 0;      // 1 = overwrite instead of min-blend (shows the traced term alone)
int   ps_r_rtao_rays    = 2;      // rays per half-res pixel (temporal EMA does the rest)
float ps_r_rtao_radius  = 1.0f;   // ray length in metres — the AO "reach". Was 2.0 with a BINARY hit: a flat 2 m dark band along every wall (06-09)
float ps_r_rtao_dist    = 40.0f;  // no RTAO past this view distance (clamped to what the bubble covers)
float ps_r_rtao_bias    = 0.05f;  // ray-origin offset along N (m): the BVH is COLLISION geometry, it does not sit exactly on the rendered surface
float ps_r_rtao_power   = 1.0f;   // pow() shaping of the visibility term
// 06-09, "RTAO adds too much black" — the three departures from production RTAO
// (see the rules block in shaders/rtao.frag.glsl): the traced term now weights a
// hit by (1 - t/R)^falloff instead of counting it whole, ignores the alpha-tested
// cutout instance (mask 0x01 — a crown was a solid ball), treats a back-face hit
// as "the ray started inside a wall", and pre-compensates the consumer's
// r_ssao_strength exponent so the visibility integral is not squared on the way
// to the shading.
float ps_r_rtao_range   = 64.0f;  // half-extent of the camera bubble the BLAS covers, metres
int   ps_r_rtao_maxtris = 1500;   // hard cap on the soup, in THOUSANDS of triangles
int   ps_r_rtao_filter  = 1;      // drop collision-only tris (suppress_shadows = invisible walls)

// ---------------------------------------------------------------------------
// WHICH WORLD THE RAYS SEE (vk_rt_world)
//
// level.cform, the COLLISION model (the fallback): resident, world-space, flat, one game
//     material per face. What every RT path started on.
// the RENDER geometry (the source) — the same vertex/index pools the forward world pass
//     draws from, cut at an RT-specific LOD error and copied into the soup by a
//     compute pass. Brings what collision does not have: geometry with no
//     collision at all (decoration), the REAL diffuse texture at the hit's own
//     UVs instead of one colour per material class, and no invisible blockers.
//
// r_rt_lod is that cut's error scale: an entry is fine at distance d when its
// error <= d / r_rt_lod, so BIGGER = finer geometry further out = more triangles.
// The raster's equivalent works out around 600-1200 (viewport height over the
// pixel threshold); the RT world is sampled by rays that never see a silhouette
// up close, so it can sit far below that.
// r_rt_dyn (06-09): NPCs, weapons, doors and props in the BVH. A per-frame BLAS
// built over the compute pre-skinning pool (vk_rt_dyn), under instance mask 0x04
// in a TLAS of its own that also carries the static bubble's instances. RTAO
// traces it; the GI rays keep the static TLAS. Needs r_preskin and r_rtao —
// with r_rtao 0 nothing is built and the PreSkin pass keeps its 1-weight skip.
int   ps_r_rt_dyn = 1;
float ps_r_rt_lod = 200.f;

// ⭐⭐⭐TREES GET THEIR OWN, SHORTER BUBBLE — AND IT IS THE SINGLE BIGGEST RT LEVER
// IN A FOREST. Measured on pripyat_full 24-08: the cut inside r_rtao_range 64 was
// 6 798 opaque triangles and 1 269 653 CUTOUT ones — 99.5% of the structure was
// 890 tree instances, because trees enter with NO level of detail at all (see the
// TREES block in vk_rt_world::Select: one mesh, no DAG, take it or leave it).
// Every ray then leaves traversal at every leaf card to ask the alpha test for a
// texel, and the whole GI chain pays: WRC/Shade, Gather/Trace and DDGI/Trace at
// once. Shrinking the WHOLE bubble to 32 m bought +16 fps in that scene — but it
// also threw away the BUILDINGS, which cost nothing (6.8k tris) and are exactly
// what a town level needs occluding. So the radius that matters is per-CLASS:
// statics keep r_rtao_range, foliage gets this.
// ⚠ 32 m is not arbitrary — it is longer than every consumer's ray: the gather
// reaches r_gather_len (24 m) and a cache cell's hemisphere r_wrc_skyvis_len
// (12 m). Only the probe/sun rays, which run to the bubble edge, can tell the
// difference, and for those a missing distant crown reads as more open sky.
// 0 = no separate limit (the pre-24-08 behaviour, trees to the bubble edge).
float ps_r_rt_tree_range = 32.f;

// ---------------------------------------------------------------------------
// RT INDIRECT LIGHT (vk_pass_rtgi) — step 0: the shadeable hit
//
// DDGI needs a probe ray to come back with RADIANCE, not just "hit / miss", and
// the BLAS stores no attributes. The pieces that fill that gap — geometric normal
// from the soup, albedo from the collision material, sun visibility from a second
// ray — live in shaders/rt_shade.glsl, and this preview is how they are checked
// before a probe grid hides them behind an irradiance atlas.
//
// r_rtgi_debug also KEEPS THE BUBBLE BUILT on its own, so the preview works with
// r_rtao 0 (otherwise an empty scene would look like a broken shader).
int   ps_r_rtgi_debug      = 0;     // 0 off, 1 shaded, 2 albedo, 3 normal, 4 hit distance, 5 DDGI irradiance, 6 albedo * irradiance
float ps_r_rtgi_debug_dist = 200.f; // primary-ray range (m); the bubble clamps it anyway

// ---------------------------------------------------------------------------
// DDGI probe volume (vk_ddgi) — one bounce of sun/sky light off the static world.
//
// The volume follows the camera TOROIDALLY: probe history is attached to world
// positions, not to slots, so walking scrolls whole planes in rather than smearing
// every probe's history one cell sideways.
//
// ⚠The dimension/spacing/ray knobs REALLOCATE the atlases (and idle the device),
// so they are for tuning between runs, not for A/B inside one.
// ⭐The cost is not the rays (~0.5 M/frame at defaults, and this hardware traces
// ~13 M/ms); it is the two blend dispatches, which touch every probe every frame.
// ⭐⭐⭐GI IS ON BY DEFAULT AS OF 23-08-2026, AND THAT IS A DELIBERATE FLIP.
//
// Thirteen defaults moved together — r_rt_src, r_ddgi, r_ddgi_light, r_ddgi_spacing,
// r_ddgi_cascades, r_gather, r_gather_light, r_gi_flat, r_wrc, r_wrc_skyvis,
// r_wrc_skyvis_bounce, r_wrc_shade_max, r_rt_albedo (and, since step 6, r_gi_froxel —
// which follows them because it is the same field, resampled, not a new opinion about
// it). They are ONE configuration, not thirteen opinions: each was measured against
// the others over the session recorded
// in [[interior-sky-through-aperture]], and changing one alone will look broken.
// Most of them are NoSave, which is why they had to be retyped every launch — the
// values belong in source, where they are versioned and visible in a diff, and NOT
// in user.ltx, which is the trap that cost this arc two sessions.
//
// ⚠ REVERT IS ONE WORD: `r_gi off` tears the whole chain down (cost baseline),
// `r_gi 0` keeps it running with zero contribution (image baseline). Neither needs
// a rebuild, and both still work exactly as before.
//
// ⚠ NOT VALIDATED ON OPEN TERRAIN AT r_ddgi_spacing 1.0. The 2 m spacing once
// printed lattice blotches on the ground (probe planes just under the terrain trace
// nothing and go dark) and 1 m is finer still. It looked clean on Dark Valley, but
// one level is not a test. If blotches appear: r_ddgi_spacing 3.0 restores the old
// outdoor behaviour and costs the interiors.
int   ps_r_ddgi             = 1;      // master gate
// CASCADES + INTERLEAVING (idTech 8's answer, and the same trade taken here).
// Nested windows, each twice the spacing and twice the reach of the last, and only
// ONE of them traced and blended per frame. Per-frame work is therefore one
// cascade's worth no matter how many exist — which is how DOOM The Dark Ages spends
// 0.08 ms on six cascades where a full every-frame update of one grid cost 0.65.
// ⚠ Cascades go OUTWARD only: cascade 0 keeps the coarsest spacing known to work,
// because a finer one puts probe planes under the terrain and prints lattice
// blotches (measured, see r_ddgi_spacing).
int   ps_r_ddgi_cascades    = 4;      // 4 since 24-08 - the engine asked for it: see the WASTED REACH line in vk_ddgi. The 5th rung is only consulted past 64 m, where the BLAS bubble ends, so it can report nothing but unoccluded sky; dropping it is a 25% shorter round-robin for no change in what is measured. +1.5 fps, A/B/A.
int   ps_r_ddgi_dim_xz      = 16;     // probes along X and Z, PER CASCADE
int   ps_r_ddgi_dim_y       = 8;      // probes along Y, per cascade
// 64, not 32: with a cascade costing 1/8 of the old full update there is room to buy
// back the noise, which is what lets the hysteresis drop and the field react faster.
int   ps_r_ddgi_rays        = 64;     // rays per probe per update
// ⚠3 m, not 2, and the difference is visible: at 2 m the probe planes land close
// enough to the terrain that many probes end up just UNDER it, where they trace
// nothing but immediate hits and go dark. Their weight leaks into the lookup and
// prints lattice-shaped blotches on the ground — A/B'd in one run (2 m blotchy,
// 3 m and 4 m clean), so this is a measurement, not a taste. The real fix is
// probe relocation + classification; until that exists, 3 m is the honest default.
// 32 probes * 3 m = 96 m across, still inside the 128 m BLAS bubble.
float ps_r_ddgi_spacing     = 1.0f;   // metres between probes IN CASCADE 0 (doubles per cascade)
// 0.95, not 0.97: a cascade is now re-measured only every r_ddgi_cascades frames, so
// the same history weight would triple the wall-clock convergence time. The extra
// noise this admits is paid for by the doubled ray count.
float ps_r_ddgi_hysteresis  = 0.95f;  // history weight; lower = faster, noisier
float ps_r_ddgi_norm_bias   = 0.25f;  // lookup offset along the surface normal (m)
float ps_r_ddgi_view_bias   = 0.10f;  // lookup offset along the view ray (m)
float ps_r_ddgi_depth_sharp = 50.f;   // cos^k lobe for the distance moments
float ps_r_ddgi_probe_dist  = 24.f;   // distance ceiling stored per probe (m)
float ps_r_ddgi_energy      = 1.0f;   // scale on the gathered radiance
float ps_r_ddgi_clamp       = 8.0f;   // per-ray radiance clamp (kills fireflies before the mean)
// ⭐⭐GI-ONLY reflectance scale. The multi-bounce equilibrium of a room is its
// aperture influx x 1/(1-albedo), so an interior lives or dies on this number:
// 0.3 amplifies 1.43x, 0.55 amplifies 2.2x. S.T.A.L.K.E.R. diffuse maps were
// authored for a renderer with NO GI, where light was a constant added on top, so
// they are painted dark on purpose — feeding them to a physical integrator
// under-reflects every interior. 1.0 = the texture untouched (the A/B). See the
// long note over rtAlbedoBoost in shaders/rt_shade.glsl.
// ⚠1.3 СТОЯЛО ЗДЕСЬ ОДИН ЧАС И БЫЛО СНЯТО: его подобрали при r_ddgi_spacing 3.0,
// то есть против ПРОТЕКАЮЩЕЙ решётки, когда в комнату лилось наружное небо, а
// собственный GI был мёртв. С живой решёткой это число ничего не значит.
//
// ✅1.8 ПОСТАВЛЕНО 23-08 И НА ЭТОТ РАЗ НА ЗДОРОВОЙ СИСТЕМЕ. Условия калибровки,
// без которых число снова обесценится: r_ddgi_spacing 1.0, вся цепочка на своих
// дефолтах (= r_gi 3), r_wrc_skyvis 8 живой, и — главное — вид r_shade_debug 11
// ПОДТВЕРДИЛ, что свет из окна в ambient уже есть. То есть подбиралась ЯРКОСТЬ
// правильного ответа, а не компенсация протечки: при 1.0 комната физически честна
// и почти черна, потому что плоская заливка вытеснена (r_gi_flat 1) и остаётся
// albedo обоев ~0.25, а равновесие комнаты = приток x 1/(1 - albedo).
// ⚠ КАЛИБРОВАНО В ОДНОЙ КОМНАТЕ, ДНЁМ. Улица под этим числом глазами не
// проверялась: снаружи тот же множитель усиливает отскок от земли и листвы, и
// если день покажется пересвеченным — снижать до 1.4-1.5, а не крутить экспозицию.
float ps_r_rt_albedo        = 1.8f;   // reflectance scale for GI bounces ONLY (never the raster)
// ⭐WHERE r_rt_albedo APPLIES - the half it was missing. Its own comment says the
// right value is a property of the ART, and the art here is two different jobs:
// exteriors were authored against a sky that lit them anyway, interiors against a
// constant ambient the engine added for free. So one global number cannot serve
// both - raise it enough to make a room work and the street blows out, which is
// why it has shipped at the neutral 1.0 since the arc began.
// The trace measures whether a point has a CEILING within r_gi_indoor_dist and
// scales the boost by that, so open ground and forest (a canopy sits far above the
// reach) come out BIT-IDENTICAL and only enclosed space is corrected.
// ⚠ NOT physics: it is content compensation, and it is a knob for exactly that
// reason. The 0.9/channel cap in rt_shade.glsl still stops the feedback ringing.
// ⚠DEFAULT IS NOW ON. Picked by eye by the user on their own saves: 1.6 reads right
// in daylight, 4 is wanted in the dark - and the reason those differ is measured, not
// guessed (see vk_ddgi.cpp: the flat sky barely moves overnight, the SUN goes to zero,
// and with it the window patch and everything the lit yard bounced back in).
// ⭐Off is one word: r_gi_indoor 1.
float ps_r_gi_indoor      = 1.6f;   // boost at full daylight
float ps_r_gi_indoor_night= 4.0f;   // boost with the sun below the horizon
float ps_r_gi_indoor_dist = 4.0f;   // metres straight up that count as "a ceiling"
// MULTIPLE BOUNCES FOR THE PRICE OF A TEXTURE FETCH. A probe ray that hits something
// reads the light ALREADY arriving there out of last frame's field, instead of
// tracing for it. Without this every enclosed space converges to almost nothing
// after one bounce: a wall in shadow contributes only its sky term. idTech 8 ships
// exactly this ("previous frame irradiance volumes for multiple bounces").
// It cannot run away — each re-bounce is multiplied by an albedo < 1, so the series
// converges to 1/(1-albedo), and the per-ray clamp catches the tail.
float ps_r_ddgi_bounce      = 1.0f;   // 0 = single bounce (the A/B), 1 = full feedback
// DYNAMIC LIGHTS IN THE RT WORLD (step 3). Until this existed the probes knew the
// sun and the sky and nothing else, so a torch, a campfire, an anomaly or a muzzle
// flash lit nothing that could bounce — in a game played at night with a flashlight
// that is most of the interesting light in the frame.
// ⚠ NOT the cluster grid: that binning is frustum-tied and a probe ray leaves the
// frustum immediately. This takes the engine's nearest-first WORLD-space list whole.
// idTech 8 builds a cascaded world grid because it bins 4000 lights; we collect a
// few dozen, and a flat loop beats any structure built to accelerate one.
// ⭐Each light gets EXACT visibility from a short shadow ray — the raster has to
// ration atlas tiles between a handful of pooled lights, so here the RT path is
// strictly better than the raster it feeds.
int   ps_r_ddgi_lights      = 16;     // max lights a probe ray shades with (0 = off, the A/B)
// ---------------------------------------------------------------------------
// PROBE RELOCATION (r_ddgi_reloc) — a probe slides inside its own cell towards
// open air instead of standing wherever the lattice happened to put it.
//
// ⭐WHY IT IS THE FIRST THING MISSING FROM OUR DDGI. A probe buried in geometry
// measures nothing but immediate hits, goes dark, and is then weighted out of
// every lookup near it — so the surfaces there get NO answer, not a coarse one.
// That is one defect wearing two costumes: indoors, a 4 m room holds no valid
// probe at 3 m spacing (measured, Dark Valley 22-08 — the room only came back at
// r_ddgi_spacing 1.0, which costs two thirds of the reach); outdoors, going finer
// than 3 m drops probe planes just UNDER the terrain and prints lattice blotches.
// Relocation is what lets the spacing be chosen for REACH instead of for the
// worst-placed probe.
//
// ⚠⚠TOGGLING IT WIPES THE FIELD, ON PURPOSE. The atlas and the offsets are one
// measurement: every tile was integrated from wherever its probe stood, so
// switching relocation off with the atlas intact would place the readers back on
// the lattice while the data still describes displaced probes — a state the
// renderer is never in otherwise, and the kind of half-torn-down configuration
// that has already produced two bogus A/B results in this arc. vk_ddgi answers a
// toggle by clearing and re-measuring; it costs one cascade round.
// ⚠DEFAULT 1 SINCE 27-08, FOR THE INTERIORS. Indoors the coarse cascades (1-2 at
// the shipping 1 m spacing) put most probes inside walls or outside the building
// — the measured cause of "light stops working a few metres away". Relocation is
// the mechanism built for exactly that rescue. Outdoors the forest audit showed
// it barely intervenes (buried 0.4% -> 0.0%, mean offset 3 cm, 0.02 ms), so the
// flip risks the case it was built for, not the one that was already healthy.
// ⛔Interior effect not yet eyeballed at flip time — verify via the
// `[VK DDGI] relocation:` log line (buried should be NON-zero near a building)
// and r_ddgi_show_probes 2 / r_ddgi_show_xray 1. Revert is r_ddgi_reloc 0.
int   ps_r_ddgi_reloc       = 1;      // probes slide out of geometry (0 = stay on the lattice)
// The "wall in my face" radius, as a fraction of THIS cascade's spacing. A hit
// closer than this counts towards the buried test, so it scales with the cell:
// 0.25 at 3 m is 75 cm, and a probe with most of its rays stopping inside 75 cm
// is not in open air.
float ps_r_ddgi_reloc_near  = 0.25f;
// How much of the ray set has to be that close before the winding-free test calls
// the probe buried.
// ⚠ Deliberately high. 0.6 means "most of the sphere is wall", and the number that
// sets the floor is a probe sitting just ABOVE a floor: nearly its whole lower
// hemisphere stops inside the near radius, which is already ~0.5 with nothing
// wrong. Anything under that would relocate probes that are perfectly placed.
float ps_r_ddgi_reloc_frac  = 0.6f;
float ps_r_ddgi_reloc_step  = 0.15f;  // travel per update, in spacings (the iteration re-measures from the new spot)
// Hard cap on the offset, in spacings. Past ~0.45 the probe leaves its own cell,
// and the trilinear weights — which stay on the lattice by construction — would
// be interpolating a probe that is no longer between them.
float ps_r_ddgi_reloc_max   = 0.45f;
// ⭐THE MAIN RULE, not a refinement: this share of rays hitting a NEARBY backface
// means the probe is inside something, and the nearest such hit says exactly how
// thick that something is — so the probe steps out in one move instead of feeling
// its way. 0 turns it off and falls back to the winding-free test above, which is
// strictly weaker (it digs under terrain, see the shader header). Default 0.25 =
// Majercik's threshold, and it is safe here only because the shader counts CLOSE
// backfaces: one-sided interior walls mean a probe in the middle of a room sees
// distant backfaces all around it and must not be called buried.
// ⚠ Only as good as the source winding, and that was MEASURED rather than assumed:
// on render geometry (r_rt_src 1) the audit reports 60% backface over all solid
// hits but 0.0% over DOWNWARD ones. Random winding would put both near 50%, so the
// sign is honest — the 60% is a world full of one-sided surfaces seen from their
// blind side, which is why only NEARBY backfaces count. On r_rt_src 0 (collision)
// the winding is not dependable; watch the same two numbers and set this to 0 if
// the downward rate is not near zero.
float ps_r_ddgi_reloc_back  = 0.25f;
// Pull back to the lattice per update once the probe is comfortable WITH MARGIN.
// Slower than the outward step on purpose: a probe that oscillates across the
// buried threshold resets its own history every cascade round.
float ps_r_ddgi_reloc_decay = 0.9f;
// ---------------------------------------------------------------------------
// VISIBILITY CONFIDENCE (DDGI::kDDGIConf) — what the lookup answers when EVERY probe
// around a point fails the Chebyshev test.
//
// ⭐THE DEFECT IT CLOSES. The per-probe weights keep floors (0.2 in the wrap term,
// 0.05 in the Chebyshev) so that ONE bad probe cannot black-hole a lookup — but
// the final sumIrr/sumW normalisation rescales those floors back to full
// brightness when ALL EIGHT probes are rejected. The answer is then "whichever
// reject was crushed the least", and at the far end of a corridor that winner is
// the probe standing OUTSIDE the building: in-wall probes have near-zero variance
// and crush to nothing, the open-air one keeps a big variance and survives — so
// the corridor's end GLOWS with open sky instead of going dark (the user-reported
// symptom this closes, 27-08; the sibling of the PROBE LEAK note in the r_ddgi_*
// defaults comment above).
//
// The gate: ddgiSampleCascade keeps the best Chebyshev visibility any consulted
// probe claimed and fades the cascade's answer to black below this threshold —
// "enclosed, no data" instead of a confident average of eight rejects. The two
// regimes are orders of magnitude apart (healthy ~1: an unoccluded probe skips
// the test entirely; all-rejected 1e-4..1e-2), so the default sits between them
// with margin on both sides. Darkness there is honest, and it composes: the
// per-pixel gather ray and the WRC still add whatever light genuinely reaches.
// ---------------------------------------------------------------------------
// EDGE-OF-WINDOW ENERGY CEILING (28-08). The measured leak: an interior wall
// 25 m out sits in cascade 2's two-cell fade band, so the cross-fade hands 96%
// of the answer to cascade 3 — an 8 m lattice with no probe inside any room,
// answering "street" (0.889) where the fine cascade honestly said 0.06-0.11.
// The fix caps the COARSE cascade's luminance at k x the FINE one's before the
// mix: outdoors the cascades agree and the cap never fires (the fade keeps its
// anti-ring job); indoors the disagreement IS the leak. k = 2 leaves headroom
// for honest density differences. 0 = off, bit-exact old blend (the A/B).
float ps_r_ddgi_edge_clamp  = 2.f;
// ---------------------------------------------------------------------------
// RTXGI CONFORMANCE (review 23-09). Each one closes a place where this DDGI
// departed from Majercik 2019 / RTXGI; each is a switch whose 0 is the old path,
// so every change can be A/B'd against itself in one run.
//
// r_ddgi_crush_tri: crush the wrap x Chebyshev weight and apply the trilinear
// weight AFTER it (RTXGI order). 0 = crush the product, which bends the trilinear
// kernel toward the nearest probe and prints the lattice.
int   ps_r_ddgi_crush_tri   = 1;
// r_ddgi_fixed_rays: extra rays per probe in ONE UNROTATED set, read only by
// relocation and classification (RTXGI_DDGI_NUM_FIXED_RAYS = 32). Geometry-only —
// no shading — so they cost a closest-hit query each. They stop the buried/not
// decision from flipping with the per-frame rotation (each flip threw the probe's
// history away). 0 = relocation reads the rotated set, as before. ⚠reallocates.
int   ps_r_ddgi_fixed_rays  = 32;
// r_ddgi_sched: which cascade updates each frame. 0 = round-robin (every cascade
// every N frames); 1 = cascade 0 — the one the player stands in and sees most of —
// on every other frame, the outer ones sharing the rest.
int   ps_r_ddgi_sched       = 1;
// Its long-range partner (r_gather_field_clamp): past the finest cascades the
// field is street-lit at ANY confidence, so where the gather prints the
// fallback straight to the pixel it is capped by the BAKED HEMI — the offline
// per-texel enclosure the statics normal prepass carries in its tag alpha
// (r_ssao_static_normals; see probeFallbackCapped in gather.comp for the whole
// story — a day of runtime-cache ceilings was replaced by this one multiply).
//   cap = k × hemi × full-sky irradiance;  k ≈ 3 = the multi-bounce gain of an
// enclosure at the boosted albedo (the constant the WRC budget clamp derived).
// 0 = off (the A/B).
float ps_r_gather_field_clamp = 3.0f;
// ---------------------------------------------------------------------------
// THE GATHER'S OWN FIREFLY CEILING (r_gather_clamp) — the per-ray radiance clamp,
// split off from r_ddgi_clamp because the two are calibrated for DIFFERENT sample
// counts and were sharing one number by accident of history:
//
//   ddgi_trace.comp   64 rays per probe update, then an EMA over updates
//   wrc_shade.comp     8 rays per cell shade,   then an EMA
//   gather.comp        ONE ray per pixel (r_gather_rays, idTech 8 ships one)
//
// A ceiling of 8.0 trims a 64-ray mean politely. At one ray it is a licence: with
// a typical outdoor ambient of 0.1-0.3, one sample allowed to be 8.0 IS the pixel,
// and r_gather_temporal 0.92 then keeps it on screen for a dozen frames. Diagnosed
// 28-08 on a house wall seen past a bush — the wall "hissed with white dots", and
// `r_gather_rays 4` removed them outright. That is the signature of an uncut TAIL,
// not of a biased estimator: four rays do not make the outlier rarer, they divide
// it by four. The screen cache makes the tail heavy in the first place — a hit on
// a sunlit leaf returns a RENDERED HDR pixel, tens of units, and the cutout
// structure of foliage is exactly where the one ray's answer is a coin flip.
// The clamp is applied to the blended rad3, so it covers the cache chain too.
// 0 = inherit r_ddgi_clamp, i.e. the old behaviour bit-exact (the A/B).
float ps_r_gather_clamp     = 1.5f;
// Spatial firefly rejection in the resolve (r_gather_ffreject) = k of the sigma
// clip. The 5x5 bilateral averages its taps; averaging is not a robust estimator,
// so one surviving outlier out of 25 still lands ~1/25 of its brightness on the
// pixel and the upsample then spreads it over a 4x4 screen block — a "dot", not
// noise. This caps every tap (and the centre) at mean + k*sigma of a 3x3 luminance
// window whose own mean/sigma are computed with one round of sigma clipping, so
// the outlier cannot inflate the ceiling that is meant to catch it. k = 2 leaves
// genuine contrast alone (a real edge sits well inside 2 sigma of a 3x3 window).
// 0 = off, the plain weighted mean (the A/B).
float ps_r_gather_ffreject  = 2.0f;
// NORMAL WEIGHT IN THE RESOLVE (r_gather_nrmw) = the exponent of pow(dot(N0,Ni), k).
// The one input idTech 8's denoiser has and ours did not: their spatial pass weights
// by NORMAL and depth and hit-distance delta, ours only ever had the last two. The
// consequence is not subtle — with no way to tell "a different surface" from "a
// different answer", the filter leans on the hit-distance gate, and that gate is a
// one-sample quantity correlated with the very value being filtered (a ray that
// reached the sky is both far and bright). Behind a bush, where hit distances are a
// coin flip between leaf and sky, it therefore rejects neighbours for disagreeing
// rather than for being another surface, and preserves the noise it should average.
// With a real normal test that job can go back to the normal, and r_gather_hitgate
// can be turned down. k = 32 is the usual SVGF-family exponent (128 is sharper).
// ⚠ Degrades to 1 wherever the G-buffer has no normal (foliage, grass, terrain write
// no tag): a missing normal must not veto a tap, or the filter goes blind on exactly
// the surfaces it already struggles with. 0 = off, the old weights bit-exact.
float ps_r_gather_nrmw      = 32.f;
// PREFILTER ON THE SCREEN CACHE (r_gather_scache_blur): 1 = the cached hit is a 5-tap
// average of last frame's pixel instead of the single texel. The screen cache is the
// sharpest and heaviest-tailed link in the chain — a rendered HDR pixel carries
// specular, sun glints and texture detail that a DIFFUSE bounce has no use for, and
// at one ray per pixel that detail arrives as variance, not as detail. Lumen
// prefilters its screen-space radiance cache for this exact reason. s_prevColor is
// already a half-res blit, which is one filtering step — this is the second, and it
// costs 4 taps only on rays that actually hit and find the cache valid.
// ⚠ The depth test stays on the CENTRE tap: widening it would let the cache answer
// for a surface it never validated. 0 = the single texel (the A/B).
int   ps_r_gather_scache_blur = 1;
// ---------------------------------------------------------------------------
// THE GI PIXEL DEBUGGER (r_gi_probe, vk_gi_probe.cpp) — the first DDGI
// instrument built for the LOG rather than for the screen. Every other one
// (probe dots, debug views, shade overlays) needs a person looking at pixels;
// this one dumps, for the surface under the crosshair, the full numeric chain:
// world position, the serving cascade, all eight probes with their trilinear/
// wrap/Chebyshev weights and irradiance (+ buried/moved flags), the field
// before and after the confidence gate, the resolved gather, the final HDR
// colour, the applied exposure and the indoor boost. Aim, set 1, paste the
// [VK GIprobe] block — "why is this wall bright" becomes ten numbers instead
// of a description of a screenshot. Prints ~once a second while on.
// While it is on, the relocation audit window is also held open (vk_ddgi.cpp),
// so the local probe census keeps printing alongside.
int   ps_r_gi_probe         = 0;
// ---------------------------------------------------------------------------
// SEEING THE PROBES (r_ddgi_show_*). Two views, and they exist because every
// statement this arc makes — "the probe is in the wall", "the room has no valid
// probe", "the field is fine and the receiver drops it" — is a statement about
// things with no pixels. The only instrument before them was a percentage over
// 2048 probes, which printed 0.0% for a room whose whole question was three of
// them: too coarse to answer the only thing being asked.
//
// r_ddgi_show_probes: 1 = each probe as a dot coloured by WHAT IT KNOWS (its
//   irradiance towards the camera — a black dot is a probe that converged to
//   nothing, i.e. the bug), 2 = coloured by STATE: red buried, yellow moved this
//   update, blue displaced and settled, green untouched on its lattice point.
int   ps_r_ddgi_show_probes = 0;
// r_ddgi_show_rays: the actual rays of the cascade updated THIS frame, drawn from
//   the probe to where each stopped. 1 = coloured by the radiance they brought
//   home, 2 = a power ramp (blue→green→red) by luminance, which reads better when
//   the question is "how much" rather than "what hue".
// ⚠ With several cascades the view alternates between them frame to frame — that
//   flicker IS the interleaved update schedule, not a fault in the view.
int   ps_r_ddgi_show_rays = 0;
// ⭐THE ONE THAT MATTERS FOR DIAGNOSIS: draw without a depth test, so probes
// SEALED INSIDE WALLS are visible. Those are the probes the whole relocation step
// exists for, and ordinary depth testing hides precisely them.
int   ps_r_ddgi_show_xray = 0;
float ps_r_ddgi_show_dist = 12.f;    // draw probes within this radius of the camera (m)
float ps_r_ddgi_show_size = 0.006f;  // dot size as a fraction of the screen (constant with distance)
float ps_r_ddgi_show_rays_scale = 1.f;   // multiplies the radiance before it is coloured
// Rays are 64 per probe, so they get their own, much smaller radius — otherwise
// one step into a room asks for a quarter of a million line segments.
float ps_r_ddgi_show_rays_dist = 6.f;
// STEP 2 — the field in the actual lighting (world_lmap / world_vlit / world_terrain).
// r_ddgi_light REPLACES the sky-hemisphere fill with the probe lookup rather than
// adding to it: a probe's irradiance already contains the sky the analytic term
// estimates, so adding would count it twice. The replacement is only calibrated
// because the probes integrate the SAME SH9 sky at the SAME scale (see
// ddgi_trace.comp.glsl) — otherwise this would be an exposure knob in disguise.
// As a 0..1 blend it is the A/B, inside ONE run, that this renderer requires.
// ⭐Raising this RAISES THE VOLUME ITSELF (see Wanted() in vk_ddgi.cpp and RTWanted()
// in vk_rt_scene.cpp) — asking for the field in the lighting is asking for the field.
// Leaving the producer/consumer split visible cost a debugging session: toggling the
// consumer with the producer off changed nothing and read as a broken feature.
// r_ddgi alone still runs the volume WITHOUT lighting anything (r_rtgi_debug 5/6).
float ps_r_ddgi_light       = 1.0f;   // 0 = the analytic sky fill, untouched; 1 = the probe field
// How much the BAKED lightmap still multiplies the probe answer. The collision BVH
// the probes trace has no tree crowns (foliage carries no collision), and the baked
// lightmap is the only thing in this renderer that does — so at 1 a forest floor
// stays shaded. The cost is that the same multiply re-applies static-wall occlusion
// the probes ALREADY model, which double-darkens interiors. Which end is worse is a
// thing to look at; hence a dial, not a constant.
float ps_r_ddgi_light_bake  = 1.0f;   // 0 = trust the probes alone, 1 = keep all baked shadowing

// ---------------------------------------------------------------------------
//  STEP 4 — FINAL GATHER (vk_pass_gather), after idTech 8's `Final Gather ->
//  Denoise -> Upscale`. One ray per screen pixel that SHADES NOTHING: the hit
//  multiplies its albedo by the probe field's irradiance there, so the sun, the
//  sky, the dynamic lights and the previous bounce are all inherited from the
//  volume rather than recomputed. What the ray buys is per-pixel VISIBILITY —
//  contact darkening and colour bleeding the 3 m probe lattice can never resolve.
//
//  ⭐r_gather is the PRODUCER and r_gather_light the CONSUMER, and the same trap
//  applies here as to r_ddgi/r_ddgi_light: turning up the consumer alone would
//  find no image and read as a broken feature. Both are NoSave — an A/B gate that
//  quietly sticks in user.ltx poisons every later measurement.
int   ps_r_gather          = 1;       // master gate (0 = the probe lookup, untouched)
// ✅3 SINCE 28-08, AND THE NUMBER THAT DECIDED IT CAME FROM r_gather_profile.
// idTech 8 ships ONE ray, and we copied that — but they also spend ~0.5 ms on 2M
// rays (4K, half-res collect) while we spent 0.85 ms on 256k: our ray was ~13x
// dearer, so "one ray" meant something completely different here. It showed as
// boiling on shaded walls that no filter could fix, because no filter invents the
// sample that was not taken.
//
// 📏The automatic sweep (village save, 640x360 collect) priced every link, and two
// numbers ended the argument: the lazy lattice saved 0.282 ms, and ONE EXTRA RAY
// COSTS 0.113 ms — not the 0.42 a hand-driven A/B had suggested in another scene.
// So rays 3 costs +0.226 ms and the lazy fix already paid 0.282 of it: the frame
// at three rays is CHEAPER than it was at one this morning (0.675 vs 0.731 ms).
// The user's own verdict at three rays, before any of this: "всё чистенько".
int   ps_r_gather_rays     = 3;       // rays per gather pixel (see the sweep above)
float ps_r_gather_len      = 24.f;    // how far a gather ray reaches (m)
float ps_r_gather_dist     = 60.f;    // no gather past this view distance — beyond it the probe lookup
float ps_r_gather_bias     = 0.05f;   // ray-origin offset along N (m)
int   ps_r_gather_scale    = 2;       // 1 = half res, 2 = quarter, ... (the resolution knob idTech 8 calls "quality")
// QUARTER RES SINCE 24-08, MEASURED A/B/A IN ONE FROZEN RUN on pripyat_full:
// 34.5 -> 38.5 fps going to 2 and back down on the return leg, for a mean |delta|
// of 4/255 in the frame - and most of THAT is the denoiser's own frame-to-frame
// noise, not the resolution. One ray per pixel was never the sharp term; the
// bilateral upsample (r_gather_upsample) and the 0.92 history are what carry the
// detail, and both work as well off a quarter-res source.
// ================================ PARKED 2026-08-23 ================================
// THE 2-BAND SH (r_gather_sh / r_gather_sh_k / r_gather_sh_temporal) — PARKED by the
// user the day it landed: "визуально мне больше нравится r_gather_sh_k 0", i.e. the
// directional term looked WORSE than not having it. DEFAULT 0. ⛔DO NOT DELETE — the
// mechanism is proven and the machinery is what a fix would be built on.
//
// ⭐THE VERDICT AGREES WITH THE INSTRUMENT, so this is not a taste call to relitigate:
// r_gather_debug 5 measured |d| / L0 at a median of 0.65, which says the L1 carries
// mostly NOISE rather than direction. Noise modulated by a normal map does not read as
// light — it reads as plastic, and on POM it reads as jagged rock (the user's word for
// a windowsill at k = 3: "скалообразным"). The eye and the number said the same thing.
//
// What IS proven, and what a later attempt does not have to redo:
//   * the estimator is right — deviation perpendicular to N, so switching it on cannot
//     move the frame's mean (measured 0.002 of 255 against a 0.435 drift floor);
//   * the whole chain reaches the frame — trace -> denoise -> upsample -> receiver,
//     landing on the POM-perturbed shading normal, confirmed by eye indoors;
//   * validation is clean, the cost is known (+0.35 ms, +63 MB at half res).
//
// ▶WHAT WOULD UNPARK IT — variance in L1, not strength (turning k down is just k = 0
// arrived at slowly):
//   1. an à-trous / strided spatial pass for L1 — the same 25 taps over 13x13 instead
//      of 5x5. Directionality is low-frequency by nature, so this is nearly free;
//   2. blue noise instead of IGN in the ray sampling (gap #3 of the idTech comparison —
//      it helps the whole gather, not only this);
//   3. a second ray, if the two above are not enough.
// The test is one run: if the median of r_gather_debug 5 moves from 0.65 to ~0.25, the
// term has started carrying direction and k = 1 is worth another look. If it does not,
// this representation does not survive one ray per pixel — which is also an answer.
// See the memory note gather-sh-per-pixel-probe.md for the full measurement set.
// ===================================================================================
// ⭐r_gather_sh — THE GATHER AS A PER-PIXEL IRRADIANCE PROBE, which is how idTech 8
// stores it (2-band SH, 3 x RGBA16F). One RGB value per pixel is the answer for the
// FLAT surface the depth buffer holds, so every bump a normal map carves into that
// surface receives identical ambient — the detail an ambient term exists to reveal
// is exactly the detail one colour cannot hold. With this on the pass also writes an
// L1 deviation vector per colour channel and the receiver re-evaluates the gather
// for the normal it actually shades with.
// ⭐THE DEVIATION IS PERPENDICULAR TO THE GATHER'S OWN NORMAL, so the reconstruction
// returns L0 exactly where the shading normal is the geometric one: turning this on
// CANNOT change the mean of the frame, only redistribute light across normal-mapped
// detail. That is the property to measure — a shift in the mean means a bug, not a
// stronger effect.
// ⚠It TRIPLES the pass's RGBA16F footprint (three tiles x raw + ping-pong) and adds
// 3 taps per denoiser sample and 12 per shaded pixel, so it is off by default and
// flipping it REBUILDS the targets, exactly like r_gather_scale.
int   ps_r_gather_sh       = 0;
// How strongly the directional term counts. 1 = the calibrated value (the
// reconstruction is exact for a linear field there); above it the term is
// deliberately exaggerated to make an A/B visible, and that IS an exposure change
// because the receiver's positivity limiter starts to bite. 0 = L0 alone, i.e. the
// same image r_gather_sh 0 produces, but still paying for the buffers.
float ps_r_gather_sh_k     = 1.0f;
// r_ao_gi (06-09, the RTAO "too much black" audit, item 4): the ambient was
// AO x (DDGI/gather) x POM AO x baked hemi — the traced term already carries its
// own visibility, so the corner under a per-pixel gather counted its occlusion
// twice. The receivers now weight the AO on the TRACED share of the ambient by
// this (ddgiAmbientAO, ddgi_receiver.glsl); the analytic sky keeps the full AO.
// 1 = the old product (the A/B), 0 = no AO on the traced term at all.
float ps_r_ao_gi           = 0.5f;
// ⚠THE DIRECTIONAL TERM NEEDS FOUR TIMES THE HISTORY THE RADIANCE DOES, and that
// is measured, not assumed: with the radiance's own 0.92 the strength view
// (r_gather_debug 5) came back at the limiter's ceiling across the whole frame —
// the channel was carrying noise instead of direction. One cosine-sampled ray
// reports a deviation about 1.6x the mean it sits beside, and 0.92 leaves only
// ~24 effective samples. Where light comes FROM also changes far more slowly than
// how much of it arrives, so this is the one term that can afford the smearing.
float ps_r_gather_sh_temporal = 0.98f;
float ps_r_gather_temporal = 0.92f;   // history weight; one ray per pixel needs a LOT of it
int   ps_r_gather_radius   = 2;       // spatial filter radius, texels (2 = 5x5)
// THE TWO WEIGHT KNOBS OF THE RESOLVE, and the measurement that produced them.
//
// 24.08, in one run: r_gather_radius 2 and 3 are INDISTINGUISHABLE, while 0 and 1
// are clearly noisier. A spatial filter that stops improving past two texels is
// not aperture-limited, it is WEIGHT-limited - and that single fact killed the
// plan to build an a-trous kernel first, because a wider kernel inherits exactly
// the same ceiling. Measure the weights instead.
//
// r_gather_plane - how much the depth weight compares a tap against the PLANE
// through the centre rather than against the centre's depth. 0 = as shipped. This
// is the stronger suspect: a floor seen at a grazing angle has neighbours at
// genuinely different depths, so the shipped test decays with ring number on the
// biggest surface in the frame - which is the saturation the measurement found.
// ✅VERDICT 28-08, IN GAME: the HIT-DISTANCE GATE (a one-sample quantity correlated
// with the value being filtered, so it conserved variance instead of averaging it)
// is OFF for good. A/B on a house wall seen past a bush, one ray per pixel: with the
// gate off the wall is "заметно стабильнее, шлейфы намного меньше" and the bush
// stops boiling against it. Nothing else in that A/B moved the picture — the
// normal weight and the screen-cache prefilter were both invisible, which is
// exactly what they should be (see r_gather_nrmw: on a flat wall every neighbour
// shares the centre's normal, so the weight is 1 and changes nothing — its job is
// to make turning THIS gate off safe, by leaving a real geometric test behind).
// (r_gather_hitgate, the switch that flipped it, is gone.)
float ps_r_gather_plane    = 0.0f;
// ---- HOW FAST THE LIGHT ARRIVES, and why it was never about the tracing -----
// r_gather_temporal is a FIXED weight, and a fixed weight asks the wrong question
// on a pixel's first frame. At 0.92 a freshly revealed pixel takes ~12 frames per
// e-fold and ~30 to settle, so the light visibly grows in for half a second every
// time the player turns — while the very first sample was already an unbiased
// estimate of the same number. The accumulation was not converging, it was fading
// in.
//
// The resolve makes the weight n/(n+1) over the frames a pixel has ACTUALLY
// accumulated, handing over to r_gather_temporal once n passes 1/(1-alpha).
// ⭐Steady state, noise floor and cost are all unchanged: the only difference is
// that frames 1..12 are a progressive mean instead of a fraction of one.
// The counter is only worth having if it is RESET when the history belongs to
// something else, and nothing checked before: "prevUV is on screen" accepts the
// occluder a pixel was disoccluded from, at 92% weight, which is the smear when
// the player walks. The test's RELATIVE depth tolerance is 0.10 (vk_pass_gather.cpp); the frame's
// turn angle is added to it on the CPU, so it does not have to be loose enough to
// survive a mouse flick.
// ⚠THE FOREST IS THE COST. With r_rt_src 1 about 99% of the BVH's triangles are
// alpha-tested leaf cards, and every one of them asks a texture on the way
// through. Dropping the cutout structure (mask 0x01 only) is the single lever
// that moves this pass by an order of magnitude — at the price of a canopy that
// no longer shades the ground it stands over.
int   ps_r_gather_foliage  = 1;       // 1 = trace the cutout structure too, 0 = opaque world only
// ⭐SPLITTING THAT LEVER IN TWO. `r_gather_foliage 0` removes the cutout structure
// ENTIRELY, so the -0.229 ms it measured on 28-08 is TRAVERSING a million leaf
// cards AND asking a texture at each of them, added together. Only the second half
// is what opacity micromaps — or the software stand-in below — would ever remove,
// so a build of either would be buying a term nobody has measured. These modes are
// the split, and modes 2/3 are also candidates in their own right.
//   0 = the exact per-texel test (as shipped)   1 = accept everything (cutouts solid)
//   2 = fixed-probability accept, no texture    3 = coverage from a coarse mip, sampled
int   ps_r_gather_atest      = 0;
float ps_r_gather_atest_cov  = 0.35f;  // mode 2's coverage — 0.32 is what [VK TreeCards] measured over 91 foliage kinds
float ps_r_gather_atest_lod  = 0.f;    // floor under the exact test's mip (mode 0) / mode 3's coverage read
float ps_r_gather_atest_dist = 0.f;    // hits nearer than this keep the exact test; 0 = the cheap mode everywhere
float ps_r_gather_light    = 1.0f;    // 0 = the probe lookup, 1 = the gathered answer (the A/B)
int   ps_r_gather_debug    = 0;       // 1 = resolved, 2 = raw (pre-denoise), 3 = hit distance, 4 = SH direction, 5 = SH strength, 6 = accumulated frame count (r_gather_adapt)

// ⭐THE SCREEN IS THE SHARPEST CACHE THERE IS, AND IT IS ALREADY PAID FOR. Before
// a gather hit falls back to the probe lattice it asks the PREVIOUS FRAME: if that
// point was on screen and nothing moved in front of it, the pixel already holds
// its fully lit outgoing radiance — sun, shadow, dynamic lights and all — at pixel
// resolution instead of the lattice's 3 m. This is cache #1 of idTech 8's chain
// (theirs: screen → hashed world cache → volumes; ours has no middle link yet).
// A miss costs nothing: the probe answer was going to be used anyway.

float ps_r_gather_scache_tol = 0.04f;  // its depth test, RELATIVE to distance: 4% at 25 m = 1 m
// ⭐⭐⭐THE SCREEN CACHE BYPASSES THE REFLECTANCE COMPENSATION, AND INDOORS THAT
// WAS THE MISSING LIGHT (found 27-08 with the r_gi_probe pixel debugger, in
// numbers). A cached hit returns a RENDERED pixel — raw albedo x lighting —
// while the lattice and the world cache both return albedo x rtAlbedoBoost
// (r_rt_albedo 1.8 x indoor boost 1.6-4), the content hack that makes Stalker's
// dark diffuse maps sustain an interior equilibrium. Indoors nearly the whole
// hemisphere of a gather ray lands on on-screen surfaces, so the cache answered
// most rays with the UNBOOSTED radiance and the gather converged to the raster's
// dark equilibrium: dark walls -> dark screen -> the cache feeds the darkness
// back. 📏Wall by the door, dusk: scache 0 -> gather == field (0.041);
// scache 1 -> gather 0.014, the wall at 0.007. The boost machinery was being
// short-circuited for exactly the surfaces it was built for.
// The cache is weighted out by the point's ENCLOSEDNESS — the same
// rtIndoorUp measure that turns the boost on — so outdoors (enclosedness 0)
// the cache keeps its pixel-perfect wins and nothing changes at all.

// ⭐STEP 6: THE FIELD IN FROXEL FORM — the first consumer of this chain that is
// not a G-buffer surface. Everything above answers "what light arrives at this
// PIXEL"; the fog, the smoke and every other translucent ask about a bit of AIR,
// and until now they were answered with a per-frame constant. idTech 8 keeps a
// froxel-resolution copy of its irradiance volumes in two-band SH and states that
// ALL transparencies read it; this is that copy (vk_gi_froxel).
//
// ⚠ IT RIDES THE FOG GRID, so it is built only while r_vol is on — the module
// says so out loud in the log rather than sitting there silently doing nothing,
// which is the failure mode of every feature behind two gates.
float ps_r_gi_froxel     = 1.0f;   // how much of a medium's ambient the field owns (0 = the flat fill, the A/B)
// The band-1 (directional) half, as a multiplier on the medium's own phase
// anisotropy. At 0 the air is lit by the field's MEAN radiance — correct, and
// flat. At 1 a froxel next to a window scatters more when you look toward the
// window, which is the whole reason for keeping a second band at all.
float ps_r_gi_froxel_dir = 1.0f;
// ⚠ THE GAIN EXISTS BECAUSE THE TWO SIDES ARE NOT THE SAME QUANTITY, and pretending
// otherwise would make the blend an exposure knob wearing a GI costume. The probes
// integrate the SH9 sky at the surface receivers' scale; the fog's flat fill is the
// weather descriptor's ambient colour times r_vol_ambient. Those agree only by
// coincidence. The self-audit prints BOTH numbers on one line — the field's mean
// radiance and the flat fill it displaced — so this is set from a measurement and
// not from taste.
float ps_r_gi_froxel_gain = 1.0f;
int   ps_r_gi_froxel_stats = 0;    // keep the self-audit line past its ~900-frame window

// ⭐⭐CACHE #2: THE WORLD RADIANCE CACHE — the middle link of idTech 8's chain and
// the one this renderer did not have. A hash of small world cells, each holding
// the SHADED radiance of one patch of surface, filled once and read by every ray
// that lands in it over the next several frames.
//
// It is the quality answer and the speed answer at the same time, because it is
// the same property twice: a cell is 12x finer than the 3 m probe lattice it
// stands in front of, AND shading it once for a thousand rays is what lets a
// probe ray stop shading at all. (Their probe update costs 0.08 ms against our
// 0.2-0.8 for exactly this reason: the ray looks the lighting up, it does not
// compute it.)
//
// ⚠ ALWAYS ONE STEP BEHIND, BY CONSTRUCTION. A cell asked for this frame is
// shaded at the top of the next and read from the one after. An unshaded cell
// answers "no confidence" and its caller falls back to the probe field, so the
// worst case of the whole feature is the behaviour that preceded it.
float ps_r_wrc          = 1.0f;    // master weight: 0 = off (the A/B), 1 = the cache answers
float ps_r_wrc_cell     = 0.25f;   // level-0 cell size (m) — idTech 8 ships 25 cm
// Cells grow with distance so one stays roughly one constant patch of SCREEN.
// Without it the far half of the bubble allocates cells nothing reads twice, and
// a cache that is never read twice costs strictly more than no cache at all.
// ⚠⚠ THE FIRST DEFAULT (0.0075) MADE THE WHOLE LADDER INERT. The base size wins
// until `d * slope > base`, i.e. past 33 m — and the gather's cutoff is 30, so
// every cell in the entire working set was the finest possible and the table
// filled up. At 0.04 the rungs land at 12.5 m and 25 m, INSIDE the region that
// uses them. vk_wrc logs the ladder in metres so this cannot go quiet again.
float ps_r_wrc_lod      = 0.04f;   // cell metres per metre of distance
int   ps_r_wrc_levels   = 5;       // highest LOD level (cell size = base << level)
int   ps_r_wrc_cells    = 20;      // log2 of the table capacity — 20 = 1M cells = 32 MB ⚠ reallocates
int   ps_r_wrc_chain    = 6;       // slots searched on a hash collision; past it the result is dropped
// ⚠ THE AMORTISATION LEVER. Higher = each cell is re-shaded less often, so the
// shade pass gets cheaper and moving shadows lag further behind in the indirect.
int   ps_r_wrc_interval = 16;      // frames between re-shades of one cell (16 since 24-08: 8 re-shaded 23k cells a frame for 13 reads each - the amortisation this cache exists for was not happening)
int   ps_r_wrc_ttl      = 90;      // frames with no request before a cell is evicted
int   ps_r_wrc_shade_max = 65536;  // cells shaded per frame (the work cap; idTech 8 shades ~20k)
float ps_r_wrc_blend    = 0.35f;   // EMA of a re-shade against what the cell held (1 = take it whole)
int   ps_r_wrc_debug    = 0;       // 1 = colour per cell, 2 = only the cache, 3 = red where it replaces the lattice answer
int   ps_r_wrc_stats    = 0;       // periodic hit rate / overflow line — the only honest "is it working"

// ⚠⚠⚠THE TERM THIS RENDERER'S GI NEVER HAD. The sun gets a shadow ray; the SKY
// gets nothing — rtShadeHitSky adds the full SH9 sky irradiance to every hit, so
// a wall inside a shack is lit as if it stood in an open field. The only thing
// holding interiors down was the BAKED LIGHTMAP re-applied on top
// (r_ddgi_light_bake, default 1); switch that off and rooms go white, which is a
// missing occlusion term reporting itself, not a bad multiplier.
//
// ⭐⭐A hemisphere ray per HIT is unaffordable (the gather alone makes ~800k a
// frame). Per CACHE CELL it is nothing: a cell is shaded ~40x less often than it
// is read (measured: 37-53 reads per shaded cell). This is the first thing the
// world cache buys that is not just time — an extra ray becomes affordable
// because of the amortisation, and that is the better argument for a cache.
//
// ⚠PAIR IT WITH A LOWER r_ddgi_light_bake. With both at full the interior is
// occluded TWICE: once by the measured hemisphere, once by the baked one.
int   ps_r_wrc_skyvis     = 8;     // hemisphere-visibility rays per cell shade (0 = the old unoccluded sky)
float ps_r_wrc_skyvis_len = 12.f;  // ⚠ MEDIUM range: past the BVH bubble a miss means "nothing built"

// ⭐⭐⭐THE idTech 8 CELL, VERBATIM: direct lights + previous frame's irradiance
// volumes, and NO rays of the cell's own. Their aperture integral lives in one
// place — the probes, where 64 rays x 0.95 hysteresis make ~2500 effective
// samples and the answer is read TRILINEARLY. Ours lived in every 25 cm cell
// separately: 8 rays x 0.35 EMA is ~38 effective samples of an integral whose
// whole energy sits in a window's few percent of the hemisphere, and two
// neighbouring cells rolling independent dice about it IS the world-anchored
// bright/dark "dirt" indoors. This switch removes the noise's SOURCE; skyvis
// rays above only average it harder (1/sqrt(N), no floor, linear cost).
// MODE 2 (28-08, "Ступень B"): cache-to-cache bounce. A cell with solid cover
// overhead takes its ambient from its OWN hemisphere — sky radiance along the
// rays that escape + what the neighbour cells they hit already radiate — and
// the probe field never enters the chain: dark rooms bounce off dark rooms,
// the coarse lattice ("the lights are on in that distant house") is out of the
// loop entirely. Open-sky cells keep the mode-1 field read, so outdoors keeps
// the field's trilinear smoothness and zero per-cell noise. The 22-08
// compounding was the field ADDED on top of an integrated hemisphere — mode 2
// REMOVES the field instead, and the 0.9 albedo cap (rt_shade.glsl) plus
// r_wrc_skyvis_bounce keep the cell<->cell loop gain below unity. ⚠ The known
// risk re-admitted here is the old per-cell dice (8 rays x EMA ~38 effective
// samples) for COVERED cells only — if the world-anchored bright/dark cell
// pattern returns indoors, raise r_wrc_skyvis before touching anything else.
// DEFAULT 2 SINCE 28-08, MEASURED THE SAME DAY (live A/B, one run, one weather):
// the canonical wall's cell fell 0.39 -> 0.09-0.16 (the inside-the-room truth is
// 0.081), a 60 s plateau showed convergence DOWN not compounding, gpu_total was
// 13.6-16.5 ms in BOTH modes, and the mode-1<->mode-2 pixel diff sat BELOW the
// frame-to-frame noise of a single mode. Indoors the three GI layers agreed to
// one number (cell 0.038 = field 0.043 = gather 0.043) with no cell pattern.
int   ps_r_wrc_sky_field  = 2;     // 1 = the cell's ambient is a probe-field read; 2 = covered cells integrate their own hemisphere off the cache (no field)
// MEASURED, THEN FLIPPED (24-08): the default is 1. The MC loop above was measured at 31 ms of a
// 60 ms frame on pripyat_full - HALF THE FRAME - because ~23 000 cells a frame
// x 8 hemisphere rays land in a canopy of 1.27 M alpha-tested leaf cards, and
// every candidate leaves traversal for a texture fetch. Switching the cell to a
// probe-field read took the scene 17 -> 24 fps on its own. The A/B (0) is still
// here, and the argument for 1 was never only speed: see the note above on why
// 8 independent dice per 25 cm cell ARE the indoor dirt.

// ⭐⭐⭐THE OTHER HALF OF THE SAME RAY. A sky-visibility ray that ESCAPES now brings
// the sky's radiance back (22-08, see gi_surface.glsl) — but one that HITS still
// brings nothing, and "hits something" is not the same as "sees no light". Standing
// in a shack at midday, most directions out of the window land on the sunlit yard
// within a few metres: the brightest surface in the neighbourhood, counted as an
// occluder. That is why the room got its window's sky and still no bounce off the
// ground outside.
//
// The fix is the cache asking the cache: swap the binary rtOccluded for a closest
// hit and read what the world cache already holds at that point (falling back to
// the probe field, exactly as the gather's `lattice` does). The ray was traced
// either way; what it costs is closest-hit instead of any-hit, plus one hash walk.
//
// ⚠ONE bounce only, and lagged: the cell it reads was shaded on an earlier frame,
// which is the same contract the rest of this cache runs on.

// ⭐THE ENERGY CEILING ON THE sky_field CELL (28-08). The idTech8 cell above reads
// its ambient from the probe field — trilinear, smooth, and past the fine
// cascades' reach (~15 m) WRONG about interiors: a distant room reads the 4-8 m
// lattice whose probes stand in the open, so "the lights are on in that house".
// Measured on ONE interior wall in the spawn village: 0.081 from inside (cascade
// 0 serves it) vs 0.889 from 25 m away (cascade 2 -> 3). Every test was working —
// Chebyshev cut 7/8 street probes, the conf gate was legitimately silent — the
// coarse lattice simply has no probe IN the room to answer with.
//
// The clamp bounds what a cell may TAKE from the field by what its own hemisphere
// measures actually getting in: k x lum(sky through the openings + already-shaded
// neighbour cells). A CEILING, not a multiplier — outdoors and at windows the
// budget exceeds the spend and the 8-ray noise touches nothing (which is what
// buries the old skyvis dirt), and the hemisphere is gated to cells with solid
// cover overhead and traced opaque-only, so the forest never pays the 31 ms this
// loop once cost. Steady-state indirect in an enclosure <= injected/(1-albedo);
// boosted albedo caps at 0.9, hence 2. 0 = off, the sky_field path bit-exact.
float ps_r_wrc_field_clamp = 2.f;   // ceiling = k x measured injection; 0 = off (the A/B)
// ASYMMETRIC RE-SHADE EMA (28-08). 📏Diag mode on the canonical wall's level-2
// cell (34 m): the 8-ray hemisphere catches the room's aperture with 0..4 rays,
// so the sky term arrives quantised in sky/8 steps (0.00/0.14/0.28/0.42/0.56)
// and a symmetric EMA rides the shot noise up as readily as down — the cell
// breathed 0.10-0.27 where the fine-level answer is 0.08, and the receiver's
// field ceiling inherited the inflation ("the windows still glow from 50 m").
// Brightening at a quarter of the darkening rate parks the cell near the low
// quantile — the honest side for an enclosure. Steady inputs (sun, open sky,
// the field read) only feel a slower ramp on GENUINE brightening. 1 = symmetric.
float ps_r_wrc_ema_up      = 0.25f;
// Bloom add strength in the tonemap composite (was a constexpr 0.8 with no
// console reach — "is this glow the bloom?" could not be asked in-game; 28-08 a
// white blob in a distant dark window needed exactly that question). 0 = off.
float ps_r_blm             = 0.8f;

int   ps_r_tonemap_lean = 0;   // r_tonemap_lean: 1 = lean composite (no water/rain/volume/bloom/CAS/debug), no bloom pass

// Final 8-bit output dither amplitude (in swapchain LSBs), applied at the very
// end of the tonemap. The swapchain is B8G8R8A8_UNORM: slow lighting gradients
// (shaded ambient on flat asphalt, sky-ambient on distant slopes, dusk sky)
// quantize into visible contour stripes no matter how clean the source buffers
// are. TPDF dither turns the steps into imperceptible grain. 0 = off (A/B),
// 1 = textbook ±1 LSB.
float ps_r_dither = 1.0f;

// Vulkan motion vectors — screen-space (prevUV − curUV) reconstructed from the
// prepass depth + the previous frame's view-proj. Foundation for DLSS/FSR
// upscaling, frame-gen and the path-tracer denoiser. Phase 1 = camera + static
// world (the dominant motion); self-moving geometry is a later phase. Default
// ON (cheap fullscreen pass, no result consumer yet — harmless until DLSS lands).
int   ps_r_motion_vectors  = 1;     // r_motion_vectors — global MV pass on/off
u32   ps_r_dlss            = 0;     // r_dlss — DLSS Super Resolution; needs r_motion_vectors + NGX available
u32   ps_r_dlss_quality    = 0;     // r_dlss_quality — 0=DLAA(native),1=Quality,2=Balanced,3=Performance,4=UltraPerf (render<display upscale = the fps win)
float ps_r_dlss_sharp      = 0.3f;  // r_dlss_sharp — CAS sharpen on the DLSS output in the tonemap (0 = off; NGX dropped built-in sharpening)
int   ps_r_dlss_debug      = 0;     // r_dlss_debug — 1=CAS delta heatmap, 2=split (left=no-DLSS bilinear, right=composite), 3=gate flag (green=DLSS output live, red=plain scene)
int   ps_r_dlss_exp        = 1;     // r_dlss_exp — 1 = feed DLSS the real tonemap exposure (1x1 texture; fixes black undershoot blotches on distant foliage), 0 = NGX AutoExposure flag (old)
float ps_r_dlss_bias       = 1.0f;  // r_dlss_bias — scale of the material mip-LOD bias under upscaling (1 = full log2(render/display); sharpens alpha mips → foliage coverage holes; 0 = off A/B)
int   ps_r_dlss_sl         = 0;     // r_dlss_sl — Stage B: route the SR evaluate through Streamline sl.dlss (FG prerequisite); 0 = raw NGX (proven default)
int   ps_r_dlss_fg         = 0;     // r_dlss_fg — Stage C: DLSS Frame Generation (MFG); needs r_dlss 1 + r_dlss_sl 1 + Reflex (auto). 0 = off
int   ps_r_dlss_fg_mult    = 2;     // r_dlss_fg_mult — frame multiplier 2..6 (2 = 1 interpolated frame; up to 6x if the GPU supports MFG)
int   ps_r_dlss_fg_debug   = 0;     // r_dlss_fg_debug — 1 = periodic [VK SL FG] log of DLSS-G state (status/frames presented/VRAM) + Reflex state (low-latency avail/latency); 2 = every frame

// ⭐r_dlss_avail — the NGX capability probe's answer, published as a console
// entry so the OPTIONS SCREEN can ask it. The renderer already refuses to run
// DLSS on hardware that cannot, but a settings row that toggles and changes
// nothing is exactly the kind of placebo this pass exists to remove: the menu
// hides the whole DLSS group when this is 0 (`data-need="r_dlss_avail"`).
// ⚠Not savable — it describes THIS machine, and a value carried over from
// another one would be a lie.
// ⚠⚠THREE states, not two: -1 = the probe has not run yet. user.ltx is read
// BEFORE the Vulkan device exists, so a two-state flag made every startup print
// "DLSS unavailable on this GPU" while restoring a perfectly good DLSS setting
// on a card that supports it — a false alarm in the log of every single run.
// The menu reads this through GetBool, where -1 counts as "yes"; by the time a
// menu exists the probe has always answered.
int   ps_r_dlss_avail      = -1;

// AMD FidelityFX Super Resolution 3.1.4. Default OFF like every upscaler here:
// it changes what the whole frame is drawn at, and that is not a thing to turn
// on behind the player back on first launch.
// ⚠r_fsr_avail is NoSave for the same reason r_dlss_avail is: it is a property
// of THIS machine, not of this player taste, and a stale 1 in user.ltx on a
// machine where the backend failed would gate the menu row on a lie.
int   ps_r_fsr         = 0;      // master gate
int   ps_r_fsr_quality = 1;      // 0 native AA (1.0x), 1 quality (1.5x), 2 balanced, 3 performance, 4 ultra
int   ps_r_fsr_avail   = 0;      // set by the backend probe
float ps_r_fsr_sharp   = 0.4f;   // RCAS sharpening after the upscale, 0 = off
int   ps_r_fsr_debug   = 0;      // FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING (needs a context rebuild)

// ---- Ray Reconstruction (DLSS-D) -------------------------------------------
// ⚠RR OVERRIDES Super Resolution — it is not an extra stage after it. With
// r_dlss_rr 1 the SR feature is released and RR runs in its slot, reading the
// guide G-buffer (vk_rr_guides: albedo + normal + roughness at RENDER res) on
// top of colour/depth/MV. Needs r_dlss 1: the whole jitter + motion-vector +
// output-target machinery is shared, and RR without it has no inputs.
int   ps_r_dlss_rr         = 0;
// ⚠RR has its OWN preset enum, unrelated to r_dlss_preset (K=11 is an SR model
// and means nothing here). Only D (4) and E (5) are real; the SDK documents
// every other letter as "reverts to default behavior". E is the newest and the
// only one that accepts the depth-of-field guide.
u32   ps_r_dlss_rr_preset  = (u32)NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E;
// Same three-state contract as r_dlss_avail (-1 = probe has not run yet), same
// reason it must not be saved: it describes THIS GPU/driver.
int   ps_r_dlss_rr_avail   = -1;
// r_dlss_rr_debug — 1 = diffuse albedo, 2 = specular albedo, 3 = normal,
// 4 = roughness. Overwrites the scene with the guide so a wrong guide is VISIBLE
// instead of being inferred from a mushy image. The SDK's own overlay
// (CTRL+ALT+F12 on the dev DLL) shows what NGX received; this shows what we sent.
// r_dlss_rr_raw — while RR is actually running, bypass the FINAL GATHER's own
// denoiser (temporal history + spatial filter) so RR receives the raw per-pixel
// rays it was trained on instead of a signal we already smoothed. NVIDIA's name for
// the colour input is literally "Noisy Ray Traced Input Color"; a denoiser reads
// SMOOTH error as lighting and preserves it, which is why our own filter's
// low-frequency blotches survive RR untouched. This is the switch that turns RR
// from "a different upscaler" into "the denoiser".
// ⚠Default 0: at half gather resolution with one ray per pixel, whether RR can
// carry the raw signal is a question for measurement, not for confidence.
// ⚠No effect unless RR is live — flipping it with RR off would just un-denoise.
// ⭐r_dlss_vram — MB of VRAM ONE NGX feature is assumed to need. 0 (default) = derive
// it from the display size (vk_dlss.cpp NgxNeedMB): the estimate ~80 MB per display
// megapixel for Super Resolution, ~230 for Ray Reconstruction / Neural Rendering.
// ⚠It buys two things at once: the renderer refuses to BUILD a feature the card has
// no room for (silently paying 20x the frame time is the failure this replaces — see
// the 28-08 19:37 log), and the texture streamer keeps that much free afterwards so it
// cannot refill the card over the top of a feature it cannot see.
int   ps_r_dlss_vram       = 0;
int   ps_r_dlss_rr_raw     = 0;
int   ps_r_dlss_rr_debug   = 0;

// ---- Neural Rendering (DLSS-NR) --------------------------------------------
// The nvngx_dlssnr.dll network from the 310.8 snippet. Third occupant of the ONE
// slot SR and RR share, and it OVERRIDES both. Its inputs are SR's (colour, depth,
// motion vectors) — it reworks the image rather than reconstructing the lighting,
// so unlike RR it needs no guide G-buffer and runs on any scene here.
int   ps_r_dlss_nr         = 0;
// ⚠There is NO public header for this feature, so there is no NVSDK_NGX_Feature_*
// constant to write here. The ID lives in the reserved range past
// RayReconstruction (13); 14 is what the driver's own telemetry feature-name table
// implies (it runs ... DEEPDVC | DLSSD | DLSSNR). This is a cvar and not a constant
// so a wrong guess costs a console line instead of a rebuild — EnsureFeatureNR
// latches after ONE refusal and un-latches when this value changes.
u32   ps_r_dlss_nr_featid  = 14;
// DLSSNR.Hint.Render.Preset. 0 = whatever the driver/OTA picks. The letter→number
// mapping is not published for this network either, hence the wide range.
u32   ps_r_dlss_nr_preset  = 0;
// Same three-state contract as r_dlss_rr_avail (-1 = the probe has not run), and
// NoSave for the same reason: it describes THIS GPU/driver, not this player's taste.
int   ps_r_dlss_nr_avail   = -1;
// Look knobs the network exposes (DLSSNR.Style / Intensity / LocalStructureStrength
// / LocalToneStrength / SkinStructureStrength). ⚠The floats default to -1 = "do not
// send this key at all". A knob we leave unset keeps the network's own default; a
// knob we set to a guessed neutral value OVERRIDES that default with our guess, and
// with no documentation for their ranges a guess is all a default here could be.
// ⚠r_dlss_nr_cfg — WHICH creation keys to send, as a bitmask. This exists because
// the create call is the one thing that cannot be read out of the binary: the
// snippet answers FAIL_InvalidParameter without naming the parameter, and every
// other way of guessing costs a rebuild and a level load. Bits:
//   1  standard Width/Height/OutWidth/OutHeight
//   2  standard PerfQualityValue — the snippet's vocabulary has no quality tier at
//      all (it has ScalingRatio instead), so this is a prime suspect
//   4  standard DLSS_Feature_Create_Flags (IsHDR | MVLowRes)
//   8  DLSSNR.ScalingRatio ALWAYS, 1.0 included (it used to be sent only when upscaling)
//   16 DLSSNR.Hint.Render.Preset + DLSSNR.Style
//   32 the size keys the ADDON uses (InputWidth / OutputWidth / Output.Width /
//      Upscaling / Scale). ⚠None of these appear in the snippet's own string table —
//      they are the addon's private config, not this network's API. Off by default.
// CreationNodeMask, VisibilityNodeMask, DLSSNR.Width/Height and DLSSNR.Enabled are
// unconditional: every NGX create needs the node masks, and those three are the only
// sizing/enable keys the snippet itself actually references.
u32   ps_r_dlss_nr_cfg       = 1 | 4 | 8 | 16;
// ⚠Read at NGX INIT, so it only takes effect on the next launch — and it is what
// makes the DLSS-NR network say whether it ran or silently passed the frame through.
int   ps_r_dlss_nr_debug     = 1;
u32   ps_r_dlss_nr_style     = 0;
float ps_r_dlss_nr_intensity = -1.f;
float ps_r_dlss_nr_structure = -1.f;
float ps_r_dlss_nr_tone      = -1.f;
float ps_r_dlss_nr_skin      = -1.f;

// ⭐r_render_scale — internal resolution as a fraction of the display, for the
// case DLSS cannot cover (no NGX, or the player wants it off). The renderer has
// had a render-extent≠display-extent path since DLSS landed and every pass sizes
// itself from it; the tonemap composites at display res and samples the smaller
// buffers with normalised UVs, so this is a plain bilinear upscale of a cheaper
// frame. 1 = native. Ignored while DLSS is upscaling — that owns the extent.
float ps_r_render_scale    = 1.0f;
int   ps_r_world_flat      = 0;   // read when the world pipelines are built (level load): changing it later needs a reload
// r_light_cap: nearest dynamic lights kept per frame (shading + shadows); a flashlight always stays.
// 256 = Lights::kMaxClusterLights, i.e. no cap beyond the cluster SSBO's own (stock). The Switch preset sets 64.
int   ps_r_light_cap        = 256;
int   ps_r_shadow_lite      = 0;   // r_shadow_lite: R1-hybrid shadows (characters = capsule shadows, no point-light shadow cubes). 0 = the shadows as before
float ps_r_capsule_shadow   = 0.65f; // r_capsule_shadow: darkening strength of the capsule shadows
int   ps_r_depth_prepass    = 1;   // r_depth_prepass: 0 = the depth prepass draws nothing (colour passes write depth). R1 had none.
float ps_r_tree_mesh_dist  = 250.f;   // r_tree_mesh_dist: full tree mesh inside this range, FLOD billboard past it (Switch preset: 40)
int   ps_r_tree_prepass    = 1;       // r_tree_prepass: 0 = no tree depth prepass (the colour pass lays the crown depth itself)
int   ps_r_grass           = 1;
int   ps_r_trees           = 1;

// ---------------------------------------------------------------------------
// r_scene_color_32 - THE HDR SCENE TARGET'S FORMAT. The one bandwidth knob that
// every single world pass pays into.
// ---------------------------------------------------------------------------
// SceneColor is the destination of ~33 render sites (world, skinned, grass,
// trees, LODs, sky, shafts, water, particles, decals, GI composite...) and the
// source of the tonemap, bloom, water refraction and the SSIL history. At
// R16G16B16A16_SFLOAT that is 64 bits per texel; at 2560x1600 one copy is
// 32.8 MB, and every blended pass reads AND writes it.
//   1 = B10G11R11_UFLOAT_PACK32 (32 bpp) - exactly half the colour traffic and
//       half the footprint, same 5-bit exponent as fp16 so the RANGE is
//       identical (max ~65024, min normal 2^-14). What is lost is mantissa:
//       6/6/5 bits instead of 10, and the alpha channel.
//   0 = R16G16B16A16_SFLOAT, the historical target.
// 📏MEASURED 26-08, RTX 5070 Laptop, pripyat_full at 2560x1440, and the number
// that matters is the one that DID NOT move. Packed vs R16F, back to back:
//   gpu_total   14.05-14.25  vs  14.30 ms
//   World/Color  2.51-2.72   vs  2.50 ms   <- the pass that WRITES this target
//   World/Statics 2.46-2.67  vs  2.46 ms
//   Tonemap      0.21-0.29   vs  0.28 ms   <- the pass that READS it
//   RT bucket     650 MB     vs  710 MB    <- the only real win here: -60 MB
// ⇒ HALVING THE COLOUR TRAFFIC BOUGHT ZERO FRAME TIME ON THIS GPU. The passes
// that touch this image are ALU/geometry bound, not bandwidth bound, which is
// the same story the draw-count work told: World is 11.6 of 14.3 ms and VSM is
// 4.2-4.5 of that. Do not reach for this expecting frames on a discrete card.
// ⭐WHERE IT IS STILL EXPECTED TO PAY: the 780M iGPU, where the screen passes
// are 63 of 75 ms on shared LPDDR5. NOT measured there yet — if someone claims
// a win from this, the only machine that can produce one is that one.
// ⛔A/B BY SCREENSHOT ACROSS THE TWO RUNS IS INVALID and was not used: the game
// clock drifted a minute between loads (06:19 vs 06:20) and dawn relit the whole
// frame, so a pixel diff reads ~30/255 mean and means nothing. What was checked
// instead: zero validation errors in either run, and a 1:1 crop of the dawn sky
// gradient (where 6/6/5 mantissa would band first) came back smooth.
// ⚠NO ALPHA, AND THAT IS ONLY SAFE BECAUSE NOTHING READS IT: audited 26-08 -
// every sampler of this image takes .rgb, and no blend state in the renderer
// uses DST_ALPHA / ONE_MINUS_DST_ALPHA (the water/particle premultiplied blends
// all key off SOURCE alpha, which comes from the fragment shader). Add a pass
// that wants destination alpha here and it silently reads 1.0.
// ⚠UFLOAT CLAMPS NEGATIVES TO ZERO. No pass writes negative colour today (no
// SUBTRACT / REVERSE_SUBTRACT blend op exists in the renderer either), but a
// future one would lose the sign silently rather than loudly.
// ⚠⚠READ ONCE, AT THE FIRST SceneColor::Format() - a change needs a RESTART.
// The format is baked into every scene pipeline at build time and there is no
// global pipeline-rebuild hook; see the block comment in vk_scene_color.cpp.
// ⚠NoSave even though 1 is now the SHIPPING default (user confirmed 26-08 after
// looking at the frame): the only reason to ever touch this is a machine-specific
// experiment, and an experiment's value frozen into user.ltx outlives every later
// default silently. Hand-editing user.ltx still works — NoSave only blocks WRITES.
int   ps_r_scene_color_32  = 1;

int   ps_r_mv_debug        = 0;     // r_mv_debug       — false-colour overlay (verify sign/Y-flip/magnitude)
// r_mv_prepass — motion vectors are produced BY THE DEPTH PREPASS instead of by a
// second set of overlay rasters after the world pass.
//   1 = the prepass carries an RG16F attachment cleared to a sentinel. Self-moving
//       geometry that already has a fragment shader there (tree crowns for the leaf
//       alpha-test) writes its true screen motion in the same raster that lays its
//       depth; statics write nothing. One fullscreen pass right after the prepass
//       fills every sentinel texel by reconstruction from depth + prevVP. So the MV
//       field is COMPLETE BEFORE SSAO (which is what let the AO temporal history
//       stop reading a one-frame-stale field — see r_ssao_reproj), and the tree MV
//       overlay's whole re-raster of the canopy disappears.
//   0 = the shipping path: fullscreen reconstruction + overlay rasters, all after
//       the world pass (MotionVecDyn). Kept for an in-run A/B.
// ⚠ Live toggle: the prepass scope always declares the colour attachment (the view
// is null when off), so no pipeline is rebuilt when this flips.
int   ps_r_mv_prepass      = 1;
float ps_r_mv_debug_scale  = 30.0f; // r_mv_debug_scale — overlay magnitude (per-frame UV motion is tiny)

// SSFX tree wind (live tuning). Trunk bend defaulted DOWN from the SSFX 0.5 —
// our trees are tall, the H² trunk term made them "sway like sails" at 0.5.
float ps_r_wind_tree_bend    = 0.18f; // r_wind_tree_bend    — trunk sway intensity (SSFX wsetup_trees.z)
float ps_r_wind_tree_anim    = 11.0f; // r_wind_tree_anim    — branch/leaf flutter speed (SSFX wsetup_trees.x)
float ps_r_wind_tree_trunk   = 0.15f; // r_wind_tree_trunk   — trunk anim speed (SSFX wsetup_trees.y)
float ps_r_wind_tree_flutter = 4.0f;  // r_wind_tree_flutter — crown/leaf flutter amplitude (our extra, SSFX has none)
float ps_r_wind_tree_crown   = 4.0f;  // r_wind_tree_crown   — height (m) over which leaf flutter fades in from the tree base; 0 = off. (Dead until 2026-07-02 — pushed but never consumed by a shader.) Fixes the "sail": flutter amplitude is ABSOLUTE world metres, so without the gate a 2 m fir's foliage gets the same displacement as a 20 m crown and balloons. Tall crowns (above 4 m) are untouched.
// nfs48: sun-cascade live layers rebuild every N frames (1 = every frame): NPC/rigid layer of cascade 0, its trees, cascade 1.
// r_tree_shadow_lod: sliding-window LOD of the CACHED tree casters, multiplier on the colour thresholds (0 = full mesh).
// Stock = every layer every frame, full-mesh cached trees. Switch preset: r_sun_cad_c0 2, r_sun_cad_c1 3, r_tree_shadow_lod 1.
int   ps_r_sun_cad_npc = 1, ps_r_sun_cad_c0 = 1, ps_r_sun_cad_c1 = 1;
float ps_r_tree_shadow_lod = 0.0f;
// nfs43-48 handheld sun-cascade profile (vk_sun_cascades_cfg.h): 0 = stock PC cascades, 1 = the Switch set (small
// cascades, z-cut caster column, NPCs only in cascade 0 / not in the far map, rest-pose cached trees, MID layer,
// staggered cache redraws). LATCHED at ShadowMap::Init (image sizes) -- set it before the renderer starts.
int   ps_r_sun_casc_lite = 0;
// r_sun_far_size (10-10): far sun map texels, latched at ShadowMap::Init (0 = the profile's: 2048 stock, 1024 kLite). The map
// is 320 m wide and only shades past the cascades; its redraw (camera 40 m / sun 1 deg on the Switch) is GPU fill.
int   ps_r_sun_far_size = 0;
// r_sun_far_trees (10-10): tree crowns in the far sun map. 0 = none: on the Switch they were 4.2 of the ~6.6 ms of a far
// redraw (alpha-tested fill, r_tree_shadow_lod did not move it); the cascades keep the near trees' shadows. Read at the
// next far redraw.
int   ps_r_sun_far_trees = 1;
// r_sun_far_tree_dist (10-10): with r_sun_far_trees, only the crowns within this many metres of the far map's centre (0 =
// all). The 320 m box is mostly its outer ring, and at 512 the crowns were vertex work (3.8 ms), not fill: a 100 m circle
// is ~30 % of the box. Past it the crowns lose their self-shadow, in the fog on the handheld screen.
float ps_r_sun_far_tree_dist = 0.f;
// nfs46: half-res screen-space sun mask for the R1 (r_world_flat) receivers. 0 = per-pixel lookup (stock). Switch: 1.
int   ps_r_sun_mask = 0;
// v104: r_sun_shadows 0 -> characters/HUD take the ObjectSpace sun-visibility probe instead of full sun (puddle_geo.w).
// 0 = stock (fully sunlit). Switch: 1.
int   ps_r_sun_vis_probe = 0;
// nfs4: R4 per-leaf geometry LOD of progressive skeletons (sliding windows). 1 = on (R4 behaviour; before nfs4 every
// pass drew window 0 at any range). r_skin_lod_k multiplies the R4 SSA thresholds (4 = switch at half the distance; Switch 4).
int   ps_r_skin_lod = 1;
float ps_r_skin_lod_k = 1.0f;
// r_skin_st_lod (07-10): distance (m, at 67 deg fov) where an ST skinned leaf -- no LOD of its own -- drops to the first of
// the windows built at load (vkSkeletonX_ST::AppendIndexLods); each next window at twice the distance. 0 = off, and then
// none are built either (read at model load). Switch preset 6.
float ps_r_skin_st_lod = 0.f;
// r_skin_lod_bg (09-10): LOD windows of our own (vkSkinLodWin), built on the skin worker for every non-HUD skinned leaf of
// 300+ triangles -- stalker bodies, corpses and mutants (PM: instead of the mod's sliding windows, which barely thin
// anything out), and the weapons in NPC hands, artefacts and devices (ST: v302 counted them at most of the skinned
// triangles of the Cordon's camps -- a sig552 leaf 19k, 20 blackdrip artefacts 140k). Distance (m, at 67 deg fov) of the
// first window, each next at twice that. An ST leaf with the r_skin_st_lod windows keeps those. Until the windows are
// built, and at 0, the OGF's own. 0 = off, and then none are built either (read at model load). Switch 6.
float ps_r_skin_lod_bg = 0.f;
// r_skin_lod_wpn (10-10): distance (m, at 67 deg fov) of a WEAPON's first background window (a dynamics\weapons model: the guns in
// NPC hands and on the ground; they also get two windows more, 6% and 3%), each next at twice that. A gun is far denser than a
// body of its size: by the body ladder a winchester1300 drew 111k triangles to 6 m. 0 = the r_skin_lod_bg ladder. Switch 1.
float ps_r_skin_lod_wpn = 0.f;
// r_skin_hud_lod (10-10): the first-person hands/weapon at a FIXED window of their own (vkSkinLodWin; 1 = 50%, 2 = 25%; 0 = the
// full model, as always before). Read at model load as well: the HUD models get their windows built only while it is > 0.
// The A/B in hudwpn_v321: holstering the rifle took w.skinned 1.28 -> 0.12 units (~1.9 ms of a Switch frame).
int   ps_r_skin_hud_lod = 0;
// r_hud_vsun (10-10, Switch preset 1): the first-person hands/weapon take the sun shadow PER VERTEX (skinned.vert, hudMode 2),
// not 4 cascade taps on every HUD pixel -- the fixed part of what the weapon in hand cost (w.skinned 0.12 holstered, ~0.85
// with a 15k-triangle gun). The light through the foliage stays on the hands, a little softer.
int   ps_r_hud_vsun = 0;
// r_skin_prepass_dist (07-10): NPCs this near (m) go into the depth prepass (was the constant 60). 0 = none.
float ps_r_skin_prepass_dist = 60.f;
int   ps_r_hud_prepass = 0;     // the first-person hands/weapon into the prepass depth: early-Z for the world under them
// r_skin_ubo: the skinned vertex passes (colour / sun+spot+point casters / NPC prepass / NPC normals) read the bones
// from a 1024-bone dynamic-UBO window of the bone buffer instead of the SSBO (*_ubo.vert.spv, skin_bones.glsl). On
// pre-Turing NVK (Switch) that is the constant bank instead of memory loads; on desktop NVIDIA divergent constant-bank
// indexing serialises, so the PC stays on the SSBO. Switchable live (both pipeline sets exist): Switch preset 1,
// one-shot nx_skinab.txt alternates it per [nx frame] window.
int   ps_r_skin_ubo = 0;
// nfs5/10/14: dynamic renderables collection filter (CRender::Calculate). 0 = bare frustum (old), 1 = + R4 r_ssaDISCARD of
// small-and-far objects, 2 = + occlusion of skeletons (level HOM, then cform rays). ⚠2 also drops the hidden skeleton's
// SHADOW (one collection serves every view). Switch: 2.
int   ps_r_dyn_occ = 1;
int   ps_r_dyn_occ_async = 1;   // r_dyn_occ 2: its rays on a core-2 worker, answers applied the next frame (07-10)
// nfs15: particle effects dropped below this screen radius (px); 0 = the R4 SSA rule like everything else. Switch: 8.
float ps_r_dyn_occ_ppx = 0.f;
float ps_r_wind_shadow_dist  = 40.0f; // r_wind_shadow_dist  — radius (m) where tree SHADOWS sway (near=per-frame, far=cached); 0 = all static (cheapest)

// Forward draw distance for trees that an FLOD container ALREADY draws as a
// billboard past VK::kImposterMinDist (250 m). Until this existed the tree cull
// had no distance term at all, so out there the full mesh and the imposter both
// rendered — the same tree twice. Only billboard-backed trees are affected; the
// rest keep their mesh at any range. 0 = off (restores the old double-draw).
// Values below kImposterMinDist are floored to it — see TreeFlodCutDist().
float ps_r_tree_dist         = 250.0f; // r_tree_dist

// ---------------------------------------------------------------------------
// r_tree_vox_dist — the MIDDLE rung of the tree LOD ladder (added 24-08-2026).
//
// mesh [0 .. r_tree_vox_dist)  ->  baked VOXEL CUBES [.. r_tree_dist)  ->  FLOD
// billboard [.. infinity). Until this existed the ladder had a hole: the mesh ran
// all the way to 250 m because that is the first range where a billboard stand-in
// exists, and r_tree_dist is floored to it for exactly that reason.
//
// The cubes are not new work — the crown voxel cloud (r_vsm_tree_hull_vox, 5 baked
// levels with per-voxel colour, continuous size and a dithered level crossfade) has
// been baked at load since the VSM caster arc and was reachable only through the
// r_vsm_tree_hull_debug viewmode. This cvar turns that viewmode into the shipping
// mid-range representation.
//
// WHY IT IS CHEAPER, in the terms r_tree_shade_dist above spells out: the tree pass
// is FILL-bound and its fill is alpha-tested leaf cards — every one of them samples
// a texture and may `discard`, which also costs the whole pass reliable early-Z. A
// cube is OPAQUE, samples NOTHING (its colour is baked per voxel) and writes depth
// early. The same swap pays again in the depth prepass, in motion vectors and in the
// shadow atlas, because all of them rasterize the same crowns.
//
// Only CROWNS are voxelized (windClass 2), so trunks keep their mesh at every range
// and the silhouette never loses its stem. Trees without a baked cloud keep the mesh
// too — TREE_VOX_BACKED is stamped only where cubes provably exist, the same "cut by
// AVAILABILITY OF A STAND-IN, never by distance alone" rule that r_tree_dist follows.
//
// ⛔⛔ MEASURED 24-08 AND IT DOES NOT PAY. DEFAULT 0. DO NOT RE-CHASE WITHOUT A NEW
// ARGUMENT. Two in-run sweeps on pripyat_full (forest road, world frozen, only this
// cvar moving), after the shading was moved off the fragment stage:
//
//     r_tree_vox_dist   0     60     30     15     0 (back)
//     fps              40     41     40     40     42
//     gpu_total ms   24.0   23.5   24.0   23.7   22.8
//     Trees ms       8.65   8.59   8.84   9.43   6.95
//
// The tier is CORRECT — 305 draws / 19.5 k cubes replaced 220 crowns, trunks kept
// their mesh, the picture holds up — it simply is not cheaper. Pulling the handover
// all the way to 15 m puts the NEAREST, most expensive crowns on cubes and the frame
// still does not move, which is the strongest form of the negative result: the cube
// cloud loses to the leaf-card mesh at EVERY range, not just where cards are cheap.
//
// WHY, in the terms r_tree_shade_dist above already establishes: the mesh out there is
// a handful of cards whose expensive per-pixel terms r_tree_shade_dist has already
// stripped, and it has early-Z from the depth prepass. A crown of cubes is hundreds of
// unsorted opaque boxes at 18 vertices each, NOT in the prepass, and its overdraw is
// paid inside this pass. Halving the fragment cost (the whole shade moved to the vertex
// stage, 24-08) took the 15 m case from +3.75 ms to +0.8 ms and still did not turn it
// positive. The July session reached the same verdict from the shadow side and swapped
// per-voxel cubes for BRICKS; the same arithmetic applies here.
//
// 0 costs NOTHING: the ~12 MB cube cloud is only uploaded when this is non-zero, and
// the bake is skipped too. Turning it on needs a level reload; turning it off is live.
float ps_r_tree_vox_dist     = 0.0f;   // r_tree_vox_dist

// ---------------------------------------------------------------------------
// r_buf_priority — the VK_EXT_memory_priority value device-local buffers ask for.
// ---------------------------------------------------------------------------
// The pathology: with the heap near its budget the driver demotes device-local buffers
// to system memory, and a compute pass that scans one then runs at PCIe speed. Measured
// 25-08: World/Cull 7.00 ms, and 0.23 ms once the pressure was relieved — a 30x swing
// in a pass whose input did not change by one entry.
//   1.0 = the fix (buffers rank with attachments, above the 0.25 of streamable
//   textures); 0.5 = VMA's default, i.e. the old behaviour.
// Read at ALLOCATION time, so set it BEFORE `load` — it does nothing live. NoSave: an
// A/B knob that stuck in user.ltx would silently bias every later run.
float ps_r_buf_priority    = 1.0f;

// Hi-Z occlusion cull for the forward tree set — drops trees fully hidden behind
// nearer geometry (hills, buildings). Shares the pyramid Pass_World builds from
// this frame's prepass depth; if that build did not happen, the test self-disables
// rather than sampling a stale (previous-frame) pyramid.
int ps_r_tree_hzb            = 1;      // r_tree_hzb

// r_tree_zwrite — 1 = the forward tree colour pass writes depth (historical), 0 = it
// only TESTS depth. See r_z_prepass above for why this matters: a fragment shader that
// contains `discard` plus depth-write disables early-Z outright, and then the whole
// depth complexity of the canopy gets shaded instead of just what is visible. Trees are
// already drawn into the camera depth prepass with the same VP/alpha-ref/wind, so the
// write here is redundant work on top of that.
// ⚠ The colour set is deliberately a SUPERSET of the prepass set, so a few edge trees
// have no prepass depth; with writes off they lay none down for later passes either.
// Live (both pipelines are built at load) — flip it and watch Trees in [VK Perf].
int ps_r_tree_zwrite         = 1;      // r_tree_zwrite

float ps_r_grass_thin        = 0.0f;   // nfs25 r_grass_thin: grass full-density radius (m), thinned past it
float ps_r_tree_lod_band     = 1.33f;  // nfs23 r_tree_lod_band: mesh/billboard crossfade band D_B/D_A (R1 64/48), 1 = hard
int   ps_r_tree_lod_size     = 0;      // nfs21 r_tree_lod_size: R1-style size-scaled mesh/billboard handover
// Switch-preset quality levers (the PC default keeps the full path). All read when the pipelines are built at level load.
int   ps_r_grass_gen         = 0;      // r_grass_gen: 0 full generator, 1 lean, 2 lean2, 3 tuft cache (>= 1 also needs r_grass_r1 >= 1)
int   ps_r_grass_r1          = 0;      // r_grass_r1: 0 full grass FS, 1 R1 FS, 2 R1 lit per vertex
int   ps_r_tree_r1           = 0;      // r_tree_r1: 1 = R1 crown (light per vertex)
int   ps_r_tree_r1_dyn       = 1;      // r_tree_r1_dyn: the R1 crown also takes the dynamic lights (per vertex, no shadow) instead of falling back to tree.frag
// r_tree_r1_wet (10-10): the R1 crown also in the rain (wet world state): v317 darkens it per vertex (nx_wet.glsl). 0 = the wet
// state sends every crown to the full tree.frag -- on the Switch the crowns went 0.45 -> 1.5-3.6 units the moment it rained.
int   ps_r_tree_r1_wet       = 0;
int   ps_r_tree_sky_push     = 0;      // r_tree_sky_push: 1 = crown sky term from the push constant (tree_skypush.frag)
int   ps_r_tree_crown_bake   = 1;      // r_tree_crown_bake: 1 = always bake the crowns, 0 = only when a reader exists at load
int   ps_r_tree_swi          = 0;      // nfs21 r_tree_swi: progressive tree meshes pick their sliding window (R4 calcLOD)
float ps_r_tree_swi_lod      = 1.0f;   // nfs21 r_tree_swi_lod: the r__geometry_lod the tree windows use (R4 g_fSCREEN)
float ps_r_tree_sky_boost    = 1.0f;   // nfs19 r_tree_sky_boost: crown sky fill multiplier (tree_v3, TREE_SKY_PUSH)

// THE CROWN UBERSHADER, SPLIT. Every frame the tree pass binds the crown
// compiled with exactly the shading terms the world can still produce, instead of
// the old single ubershader with all twelve in it.
//
// WHY A COMPILED VARIANT AND NOT A CVAR INSIDE THE SHADER — this is the whole
// argument, and this pass has now paid to learn it three times. The crown costs
// per WAVE: one surviving lane makes the entire wave execute the entire shader,
// so a runtime `if` that most lanes skip removes NOTHING (r_tree_alpharef and a
// 17% cut in surviving fragments each moved this zone by zero; `r_gather_light 0`
// skipped a term at runtime for 0.02 ms, while compiling the identical term out
// returned 0.45). A specialization constant is folded when the PIPELINE is built,
// so the term's taps, ALU and registers all go — and because the driver compiles
// VS and FS as a pair, so does the vertex work behind any varying the FS stops
// reading (crown VS 56 -> 32 registers when the gi term went).
//
// WHAT IT REMOVES IN PLAIN WEATHER: snow, shore/rain wetness, SSIL, the dynamic
// light walk, and both debug families — six of twelve terms, none of which can
// paint anything in a dry sunlit forest with no lamp in reach.
//
// The mask is built by VK::EnvLight::TreeSpecMask() from the SAME UBO fields the
// shader's branches test (vk_env_light.cpp, next to the world uber-FS mask that
// works the same way), which is what stops the two from disagreeing. Variants are
// built ONE per frame in the background and the crown draws the full shader until
// its variant lands — a mask that is not ready is slower, never wrong. The bits
// that flip during play (weather, a lamp coming into reach, nightfall) are
// prewarmed, so "it started raining" is not a hitch.
// ⚠ Bit layout lives in three places that must agree: tree_variants.glsl,
// TreeSpec in vk_TreeManager.h, and TreeSpecMask() in vk_env_light.cpp.

// ASK THE DRIVER WHAT IT COMPILED (VK_KHR_pipeline_executable_properties). 1 makes
// every pipeline built from here on carry VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT,
// after which `vk_pipestats` prints registers / occupancy / spill / instruction
// count per shader stage for the pipelines it knows about.
//
// WHY IT EXISTS: the crown per-term sweep priced ONE varying read and five ALU ops
// at 0.45 ms — far more than the removed work can explain, and the signature of an
// occupancy threshold rather than of the work itself. "Smells like a register
// cliff" is a guess, and this project has lost weeks to guesses about the GPU that
// a number would have settled in one run. Nothing else here reads registers.
//
// ⚠⚠ IT MUST BE ON *BEFORE* THE PIPELINE IS CREATED. Statistics exist only for
// pipelines built with the capture bit, so for anything built at level load this
// has to be set before `load <level>` — setting it afterwards yields an empty
// report, and vk_pipestats says so rather than printing nothing.
// ⚠ Not free: the driver keeps compile artefacts alive for every captured
// pipeline. Measurement mode, default 0, NoSave.
int ps_r_pipe_stats          = 0;      // r_pipe_stats

// TEXTURE AUDIT — which of the level textures pay for a channel nothing reads.
// The engine already repacks BC3 -> BC4 for two classes (hemi lightmaps whose bake
// is in alpha, and m_AlphaOnly height maps), but both are gated by CLASS, so the
// rest of the level BC3 has never been looked at. Two things are worth counting:
// rgb empty (the alpha half IS a BC4 block) and alpha fully opaque (the colour
// half IS a BC1 block) — both halve the texture, bit for bit, no re-encode.
// ⚠⚠ Reads EVERY block, deliberately: a sparse sample of "does this texture use
// alpha" is the same mistake that told the tree card survey 11.1% when the answer
// was 0.2%. So it is not free, and it must be on BEFORE the level loads.
// Read the tally with vk_texaudit. Measurement mode, default 0, NoSave.
int ps_r_tex_audit           = 0;      // r_tex_audit

// SAMPLE THE PROBE FIELD ONCE PER TREE INSTEAD OF ONCE PER VERTEX.
// tree_gi.comp writes two field samples per tree (crown bottom and top) and
// the crown VS reads them and lerps by height (the per-vertex lookup stays as the
// fallback while the compute pass is unavailable).
//
// WHY, and it is the first crown lever this session that is aimed at a MEASURED
// place rather than a plausible one: vk_pipestats printed the crown pipelines and
// found the cost in the VERTEX shader. With the FS no longer consuming vGI.rgb the
// driver deletes ddgiFieldWeighted from tree.vert outright — VS registers 56 -> 32,
// binary 20224 -> 5376 B — and THAT is the 0.45 ms the per-term sweep had been
// chasing. The screen-space gather tap everyone suspected was worth 0.03 ms.
// ddgiSampleIrradiance is ~16 atlas taps; a canopy draws ~10^6 vertices and the
// field varies over metres, so a crown gets essentially one answer either way.
// The vertical gradient is kept (two samples + lerp), which is what makes this a
// picture-neutral change rather than a quality trade.
//
// ✅DEFAULT 1 since 26-08: the register readout confirmed the mechanism (crown VS
// 56 -> 32 registers, binary 20224 -> 4224 B, the field lookup genuinely gone from
// the vertex shader) and the author confirmed the canopy by eye. The compute that
// feeds it costs 0.05 ms.
// Range past which tree.frag stops computing its SUBTLE per-pixel terms: the
// screen-space SSIL bounce, the sky-cube canopy sheen, the dynamic point/spot
// light walk and the shoreline wetness lookup. The sun, its shadow, ambient, AO,
// snow and fog are NEVER dropped — those are the terms you can actually see at
// range, and thinning them would read as trees changing colour as you approach.
//
// WHY THE FRAGMENT SHADER IS THE LEVER: the tree pass is FILL-bound, not
// geometry-bound. That was measured, twice, the hard way — meshlet culling cut
// caster vertices and moved the pass by zero, and the crown hull's whole point is
// "no texture fetch / no discard". So what moves Trees is the cost of a fragment
// that SURVIVED the alpha test, and out at 150 m a crown is a handful of pixels
// each while there are hundreds of crowns — the far band is where the fill goes.
//
// Fades out over the last quarter of the band rather than switching, or the forest
// grows a visible ring that slides as the camera moves. 0 = off (full shading at
// every distance, the pre-2026-08-13 behaviour).
float ps_r_tree_shade_dist   = 100.0f; // r_tree_shade_dist
// r_tree_stats — THE TREE LEDGER. 0 = off (default, costs nothing), 1 = the cull
// funnel + per-pass hardware counters, 2 = + the per-pass GPU sub-zones
// (Trees/Cull, Trees/Mesh, Trees/Vox, Depth/Trees, MV/Trees — those also need
// r_profiler 2, which is what actually opens them).
//
// WHY THIS EXISTS. Trees are the biggest single system in this frame and the worst
// instrumented one: the `Trees` profiler zone covers ONLY the forward colour pass,
// while the same canopy is re-rastered into the depth prepass, the motion vectors,
// the shadow cascades and the VSM atlas. Every tree optimisation so far was steered
// by an INSTANCE COUNT, and instance counts have pointed the wrong way twice:
//   • r_tree_hzb was accepted on "-18% of instances" with no millisecond attached;
//   • the cube LOD tier was built on "far crowns are many" and died on "far crowns
//     are not where the cost is" — a fact one histogram would have shown for free.
// So the ledger counts TRIANGLES and FRAGMENTS next to every instance count, and
// bins the survivors by SCREEN SIZE, which is the axis a LOD decision is really
// made on. See vk_TreeManager_Stats.cpp. `vk_trees` prints it on demand.
//
// ⚠ Layer 1 is cheap but not free: the cull compute aggregates per workgroup in
// shared memory and flushes ~48 atomics per 256 trees. Leave it OFF for the
// millisecond A/B itself and read the counts from a separate stats run — the same
// discipline r_profiler 2 already needs.
int   ps_r_tree_stats        = 0;      // r_tree_stats
// r_tree_depth_sort — draw the CAMERA depth prepass's trees in near→far depth slabs
// instead of texture-group order. 0 = off (historical), N = slab count (8-16 typical).
//
// WHY. The tree ledger measured the tree half of the depth prepass at 34 M fragment
// shader invocations to cover a ~3.7 M pixel screen. The colour pass cannot be helped
// by ordering — it draws onto finished depth — but the prepass is what BUILDS that
// depth, so nothing shields it and its order decides how much of its own overdraw it
// can reject. It currently draws grouped by TEXTURE, which is arbitrary in depth: a
// near crown routinely arrives after the far ones it would have occluded, and their
// alpha-tested fragments get shaded for nothing.
//
// WHY SLABS AND NOT A FULL SORT. Sorting strictly by distance maximises rejection and
// destroys batching — every draw could then need its own VB/IB and descriptor bind.
// Slabs keep whole texture groups batched inside each depth layer, which is most of
// the occlusion for a fraction of the state changes. The cvar IS the slab count so
// the trade can be swept instead of guessed; [VK TreeCPU] prints both draws and binds.
//
// ⚠ SAFE BY CONSTRUCTION: this only PERMUTES draws. Depth is written with LEQUAL, so
// the finished buffer is the per-pixel minimum in any order — bit-identical either way.
// ⚠ Camera prepass only. Shadow casters pass 0: "front to back" is measured from the
// camera, and a light does not share it.
//
// ⛔📏 MEASURED 25-08, DEFAULT STAYS 0 — IT WORKS AND IT DOES NOT PAY. Sweep on
// pripyat_full, world frozen, alternating 0/8/0/16/0:
//     sort 0  -> prepass fs-inv 34.6 / 34.2 / 34.0 / 34.1 M   Depth/Trees 0.89-1.32 ms
//     sort 8  -> 29.0 / 27.6 / 26.0 M
//     sort 16 -> 25.2 / 25.2 / 25.2 M  (-26%, dead stable, and it returns to 34.1 on 0)
//     Depth/Trees: 0.96-1.63 ms — indistinguishable, the spread WITHIN each mode is
//     larger than the difference between them. Colour fs-inv unchanged at 77.7 M,
//     confirming the ordering argument (same depth buffer either way).
// ⇒ The rejection is real and repeatable; the prepass simply is not fragment-bound.
// At 1706 individual vkCmdDrawIndexed for ~1.2 ms it costs ~0.7 us per draw, i.e. it
// is DRAW-CALL bound — which is also why raising state binds 137 -> 637 changed
// nothing. The lever that would move this zone is fewer draws (GPU cull + indirect
// multi-draw, the way the colour path already works), not a better order.
// Kept because it is a live, correct knob for hardware or scenes where the prepass IS
// fill-bound — but do not re-chase it here expecting milliseconds.
int   ps_r_tree_depth_sort   = 0;      // r_tree_depth_sort
// r_tree_depth_gpu — cull the CAMERA depth prepass's trees on the GPU and draw them
// indirect, instead of walking every instance on the CPU and issuing one draw each.
// 1 = on (default), 0 = the historical CPU walk, for A/B.
//
// WHY. The ledger measured the old path at 193 471 instances scanned to issue 1 706
// draws — 99.1% of the walk existing to say "no" — and r_tree_depth_sort then proved
// the resulting zone is DRAW-CALL bound, not fill bound: cutting its fragment count by
// 26% moved the milliseconds by zero. Fewer draws is therefore the lever, and the
// colour path already had the machinery (tree_cull.comp + DrawIndexedIndirectCount).
// This points it at the prepass: one dispatch, ~92 indirect draws, no CPU walk.
//
// ⚠ The SET is decided by the same shader as the colour cull, in CULL_PREPASS mode —
// which is what keeps colour a superset of the prepass (colour cuts from the sphere's
// near side, the prepass from its centre). Getting that backwards renders black tree
// silhouettes, so the two live four lines apart in tree_cull.comp.glsl.
// ⚠ Camera prepass ONLY — the shadow targets have their own switch below.
int   ps_r_tree_depth_gpu    = 1;      // r_tree_depth_gpu
// r_tree_shadow_gpu — the same treatment for the SUN and RAIN shadow maps: cull each
// target on the GPU and draw it indirect instead of walking every instance on the CPU
// once per target. 1 = on (default), 0 = the historical CPU walk, for A/B.
//
// WHY. The prepass walk was one scan of the level per frame. The shadow maps are five
// more whenever a shadow cache misses: `[VK TreeCPU]` measured 967 k – 1.2 M instances
// scanned to issue ~10 000 draws, 99.0% of it a rejection, all on the main thread
// inside CPU_RECORD where it reads as plain "recording".
//
// ⚠⚠ WHAT THE NUMBER IS AND IS NOT, because the first reading of it was wrong. Those
// 5-6 walks are NOT a per-frame cost in every configuration: with a still camera and
// VSM owning the sun, the caches hold and only ~1 sampled frame in 10 shows more than
// the single prepass walk. This removes a recurring SPIKE — and the reason the shadow
// caches have to stay lazy — rather than a steady tax. To A/B it, put the walks back on
// the per-frame path first (`r_shadow_casc_cache 0`, or move the camera), otherwise
// both sides of the comparison measure a frame that never walks anything.
//
// ⚠ The predicate is NOT the camera one. tree_cull.comp runs in CULL_SHADOW mode: six
// planes of the LIGHT's box including the near one, +radius and no slack (matching
// ShadowMap::*SphereVisible exactly), plus the camera-distance band that splits a
// cascade's cached far layer from its per-frame near/wind layer. No Hi-Z (that pyramid
// is the camera's depth) and no distance cut (a caster has no billboard stand-in inside
// a shadow map — cutting one deletes its shadow).
// ⚠ Costs ~23 MB of GPU-only indirect commands at 193 k trees, allocated lazily on the
// first shadow dispatch — a level whose shadow path never runs pays nothing.
// ⚠ The flashlight SPOT tiles are excluded on purpose: perspective cones that refresh
// on light movement rather than per frame, and each would want its own command section.
int   ps_r_tree_shadow_gpu   = 1;      // r_tree_shadow_gpu
// r_vsm_tree_wind — near/far WIND HYBRID (UE5 WPO-disable-distance pattern): trees within
// r_vsm_tree_wind_dist cast into the DYNAMIC atlas every frame WITH live wind → their
// shadows sway smoothly (one time slice per frame); farther trees stay rigid in the
// toroidal static cache (imperceptible at distance). Boundary crossings invalidate the
// tree's static pages the same frame (no ghosts). Replaces the old TEST mode that applied
// wind to STATIC pages — staggered round-robin refreshes froze each page at a different
// wind phase = the "jelly" shadows. Costs: near trees re-raster into the dyn atlas per
// frame (bounded by the distance). Default ON (VSM itself is the opt-in Ultra path).
int   ps_r_vsm_tree_wind      = 1;
// DYNAMIC-pass cadence (decoupled, Level 1): the sun/clipmap + the cheap STATIC atlas run
// EVERY frame (wall/terrain/ground shadows track the sun smoothly, no strobe); only the
// EXPENSIVE dynamic pass (NPC + grass + near-tree wind crowns) updates every Nth frame, its
// atlas preserved (world-anchored) between. 0/1 = every frame (off); 2-3 ≈ 30-45 Hz; dyn
// shadows go 1..N-1 frames stale (EMA r_vsm_ta_blend_dyn smooths). Default off.
int   ps_r_vsm_cadence        = 1;
// Camera-motion deadband for the dyn cadence: the frozen dyn atlas is world-anchored → only
// aligned while (nearly) stationary, so the skip is allowed ONLY when the camera moved less
// than this (metres/frame) AND barely turned. Walking closer/farther otherwise scrolls dyn
// pages + batches near-set transitions → the crown/NPC shadows flicker. Moving = full-rate
// dyn (correct, no flicker); standing = the cadence perf saving. 0 = never skip while moving.
float ps_r_vsm_cadence_still  = 0.02f;
// Second half of the cadence alignment gate: the frozen dyn atlas is valid only for the SUN
// angle it was rendered at. The living sun — especially env time-of-day KEYFRAME STEPS (diag
// measured ~18-22° jumps across a few frames) — misaligns it → the "тени пропали на кадр"
// flicker. Force a full-rate dyn update when the sun rotated more than this (degrees) since the
// last dyn update. Small enough to catch steps (normal drift is ~0.03°/3-frames), large enough
// not to fire on smooth motion. Window page-snaps are always caught regardless. 0 = sun gate off.
float ps_r_vsm_cadence_sun    = 0.30f;
// Near-set radius (m) — LATERAL light-space distance from the camera to the tree's shadow
// COLUMN (not trunk distance: a low sun lands a tree's shadow tens of metres down-sun, and
// what must sway is the shadow NEAR THE PLAYER regardless of where its tree stands). Trees
// whose column passes within this radius (and within 3× world distance) cast dynamically.
float ps_r_vsm_tree_wind_dist = 40.0f;
// Impostor crown shadows: replace the near-tree DYNAMIC crown MESH raster with ONE baked
// sun-facing billboard/tree (2 tris vs thousands of crown verts every frame under wind).
// Kills the geometry/raster-bound VSM/DynTrees cost. Trades dappling fidelity for speed —
// eyeball. Default 0 (PENDING quality/perf sign-off). _scale tunes the shadow footprint.
int   ps_r_vsm_tree_impostor       = 0;
float ps_r_vsm_tree_impostor_scale = 1.0f;
// Crown-HULL caster LOD (the "AC Shadows" shadow LOD, 2026-07-11): near trees BEYOND
// r_vsm_tree_hull_dist cast into the dyn atlas from a baked low-poly OPAQUE hull
// (ellipsoid lobes over k-means vertex clusters) instead of the full alpha-tested crown
// mesh; only the closest trees keep the real crown (dappling perfect where the player
// looks). Unlike the rejected FLAT impostor, the hull is a 3D volume — correct on walls
// and at any sun elevation. This is the first lever aimed at the PROVEN alpha-test-fill
// bound of VSM/DynTrees (no texture fetch / no discard / early-Z). Geometry is always
// baked at load (~a few hundred KB) → toggles LIVE for the A/B. Default 0 pending the
// perf/quality sign-off (A/B: stand in a forest, r_profiler 1, flip r_vsm_tree_hull,
// watch VSM/DynTrees + the shadow look; r_vsm_debug 1 logs "hull=N" in the hybrid line).
// 2 = ALSO hull the FAR/STATIC foliage beyond _dist (crowns only, same lateral metric):
// closes the non-monotonic LOD hole (a 60 m tree casting a MORE detailed shadow than a
// 25 m one) and cuts the full-crown re-raster on dirty cache refreshes (sun-move spikes).
// NOTE: flipping 2 on/off converts the static cache via invalidation circles (≤4/frame)
// + the moving-sun round-robin — expect the far shadows to transition over ~10-30 s.
// DEFAULT ON since 12-08-2026, together with r_vsm: this is the whole point of
// making VSM primary. Mode 1 = near DYNAMIC casters beyond _dist swap the alpha-
// tested crown for the opaque hull — exactly the layer measured at 6.34 ms under
// the cascades. Mode 2 additionally hulls the far/static foliage; left off for now
// because flipping it re-converts the static cache over ~10-30 s (see above), so it
// deserves its own A/B rather than riding in on this change.
int   ps_r_vsm_tree_hull      = 1;
float ps_r_vsm_tree_hull_dist = 18.0f;
// Crown VOXELIZATION (baked at load → level reload to change): 0 = PCA ellipsoid LOBES
// only (the original "AC Shadows" clusters). >0 bakes TWO things per crown, UE Nanite-
// foliage style: (a) a modest merged shell for the depth-only SHADOW caster (solid
// silhouette, resolution auto-clamped ~8–20 regardless of this value), and (b) the
// VISUAL voxel CLOUD — individual small colored cubes on the leaf cards (per-voxel
// palette/AO/height tint, size jitter), kHullLods LOD levels with the voxel edge
// DOUBLING per level. The value = level-0 cells across the crown's largest axis
// (64 ≈ 10–20 cm cubes on a full tree — the "Witcher 4 demo" look; higher = finer).
int   ps_r_vsm_tree_hull_vox  = 64;
// LIVE coarseness bias for the voxel viewmode: added to the screen-size-picked LOD level
// (0 = as graded; negative → force finer/smaller; positive → coarser/bigger). No rebake.
int   ps_r_vsm_tree_hull_lod  = 0;
// Target PROJECTED voxel size in PIXELS — the rendered cube edge is CONTINUOUS
// (dist × this angular size, clamped below by the finest baked cell), so voxels grow
// smoothly from tiny near to huge far with no size steps; the baked grids only supply
// positions/density (largest cell ≤ the current cube size). Smaller = finer/more
// instances; live, no rebake.
float ps_r_vsm_tree_hull_vox_px = 4.0f;
// Distance GAIN for the target pixel size: +this many px per 100 m. 0 = constant screen
// size (uniform fine grain everywhere — reads as if "nothing changes" while flying);
// >0 = far crowns become VISIBLY chunkier blocks, a continuous small→huge gradient.
float ps_r_vsm_tree_hull_vox_far = 4.0f;
// UE-style SHADOW voxel LOD (brick caster): page texels per voxel edge. The caster
// picks the baked grid whose cell ≈ stex × the page texel of the tree's clipmap level
// (texel(L) = r_vsm_base·2^L / 4096, level from the same lateral shadow-column metric
// the residency uses) — the shadow view drives the LOD, NOT the camera (UE
// NaniteClusterCulling LODScale). Near rings → finest bake (real leaf gaps), far rings
// → coarse bricks that stay sub-texel-ish. Smaller = finer/costlier. Live.
float ps_r_vsm_tree_hull_vox_stex = 3.0f;
// Safety floor on the caster voxel edge (m) for weak HW — 0 disables. Live.
float ps_r_vsm_tree_hull_vox_sfloor = 0.05f;
// Hard cap on the caster slice per tree (BRICK count of the picked baked level, ~×14
// fewer units than cubes at 6 verts each); telemetry `slice=` in [VK VoxLOD]. Live.
int ps_r_vsm_tree_hull_vox_cap = 1500;
// STATIC-tier proxy pick: 0 (default) = the merged watertight SHELL (~96 tris/tree —
// the blind-test-verified far representation; crowns there get meshlet per-page
// culling, bricks do NOT, so full brick slices into every dirty page measurably LOST
// to the shell on living-sun cache refreshes: VSM/Static 0.42-0.60 vs 0.13-0.44);
// 1 = bricks in the static tier too (visual parity with the dyn band; costs the
// missing per-page brick cull — the planned stage-2 refinement). Live.
int ps_r_vsm_tree_hull_vox_static = 0;
// Stage-2 per-page brick cull (vsm_vox_cull.comp) — the UE-parity piece: only bricks
// overlapping a shadow page rasterize into it (compacted (tree,page) draws + a
// surviving-brick remap list), instead of the full slice sweeping the VS for every
// page of the tree's column. This is what the crown path already had via the meshlet
// stage 2 — and what makes static-tier bricks (vox_static 1) affordable. Live.
int ps_r_vsm_tree_hull_vox_cull = 1;
// UE WPODisableDistance analog (needs r_vsm_tree_hull 2): past the hull boundary a
// brick's wind amp has ramped to 0 across the crossfade band — the shadow is rigid,
// so the tree is DEMOTED from the per-frame dyn atlas into the cached static tier.
// The dyn near set shrinks to the real-crown ring + the band (the main perf win);
// wind ALU on voxel casters drops to zero (the VS skips ssfxTreeWind at amp 0). Live;
// toggling converts the cache through the usual invalidation circles over a few sec.
int ps_r_vsm_tree_hull_vox_wpo = 1;
// SHADOW crossfade band (voxel caster): fraction of r_vsm_tree_hull_dist just inside the
// boundary where a tree's shadow hands over to true leaves in TWO stages (fade 1 → 0):
// the outer half thins the cubes one-by-one in leaf-coverage order (the UE close-up
// look — "extras removed", survivors sit on the leaf clumps), the inner half adds the
// real alpha-tested crown while the remaining cubes dissolve out. 0.5 default so both
// stages read (at _dist 18 m the band spans 9–18 m of lateral shadow-column distance).
float ps_r_vsm_tree_hull_band = 0.5f;
// Dithered LOD crossfade: within this fraction of the handover distance BEFORE a level
// switch, both levels draw with complementary screen-door masks (fine dissolves out,
// coarse dissolves in — exactly one survives per pixel), so flying the camera never
// pops between voxel sizes. Instance cost doubles only inside the band. 0 = hard cut.
// Same infra the future SHADOW-side voxel LOD will need (depth-only discard works too).
float ps_r_vsm_tree_hull_vox_fade = 0.25f;
// Voxel viewmode ("voxels instead of leaves"): crowns drawn as their baked voxel cloud
// (face-shaded colored cubes; falls back to the shaded hull overlay when _vox = 0).
// 1 = only hull-TIER trees (shows the _dist boundary); 2 = EVERY tree with a baked
// cloud, so you can walk up to a near tree and inspect its voxel crown.
int   ps_r_vsm_tree_hull_debug = 0;
// ============================================================================================
// ⛔ "VSM tree perf" arc — MEASURED NO NET GAIN on dGPU (2026-07-02), KEPT BUT DISABLED, DO NOT RE-CHASE.
// Both cvars below default 0 = OFF (zero cost, picture identical). Code is intentionally retained (not
// deleted) — it is correct and verified, just not a win here. WHY it didn't pay (see memory
// [[vulkan-vsm-meshlet-plan]] for the full A/B): the near-tree VSM cost is REAL VISIBLE shadow coverage
// of big near crowns, not occludable overdraw — meshlet-cull only cut vertices (pass is FILL-bound → 0),
// HZB only culls the ~6% hidden behind static geometry while its reduce pass costs more than it saves
// (clean stationary A/B: HZB on ≈ off, net −0.15ms). The real levers (coarser near-tree pages / fewer
// near trees) were rejected on purpose — near shadows the player examines must stay pixel-perfect.
// → Leave OFF. Possible future value ONLY on iGPU (bandwidth-bound) or dense interiors. Don't retest on dGPU.
// ============================================================================================
// r_vsm_meshlet — per-page MESHLET culling of VSM tree casters (Phase A+B). Dices trees into ~128-tri
// clusters at load; a 2nd bin stage draws only clusters overlapping each atlas page. Correct, picture
// identical. 2026-07-05: VSM/DynTrees is geometry-bound (VRS/HZB no help) → meshlet cull is the one
// caster-tri lever, so DEFAULT ON on the chance it helps dense forest; user measured NEUTRAL in a
// moderate view. NET-0/NET− per the old audit → if VSM/DynTrees ever WORSENS with it, set 0 to revert.
//
// ⭐📏DEFAULT FLIPPED TO 0 ON 25-08, AND THE DECIDING NUMBER IS NOT THE MILLISECONDS — IT IS
// THE 458 MB. Its two per-page command buffers are `cmdBytes` each and on pripyat_full that
// is 229 MB x 2 (`[VK Vram] Trees buf:storage 229 MB` twice). Measured that day, loading
// with it OFF: VRAM 6454/6724 (96% of budget) -> 5963/6621 (90%), i.e. ~490 MB returned and
// the headroom nearly tripled. The frame itself was a wash, exactly as the audit above and
// the 13-08 re-measure both said: VSM/Bins 2.05-2.16 -> 1.48-1.88, World/VSMrender
// 2.7 -> 3.15-3.36, gpu_total inside run-to-run noise.
//
// A wash that costs 458 MB is not neutral. VRAM headroom is what keeps the driver from
// demoting hot buffers to system memory, and that demotion was worth 20 ms of frame on this
// very level (see the priority block in vk_buffer.cpp). Trading half a gigabyte for zero
// milliseconds is the wrong side of that trade, and on an 8 GB card it is the difference
// between fitting and thrashing.
// ⇒ Set 1 only to re-test the feature, and only on a machine with VRAM to spare.
int   ps_r_vsm_meshlet        = 0;
// r_vsm_hzb — shadow-HZB occlusion cull (static + Option A dyn-vs-static-occluder). Reduce maxes prior
// static-page depth; caster bins skip pairs fully behind cached walls/terrain. Correct, picture identical.
// NO NET GAIN (reduce costs > the little hidden it culls). Default 0 = OFF, kept for reference / interiors.
int   ps_r_vsm_hzb            = 0;
// r_vsm_hzb_margin — depth slack for the HZB occluder (stale-sun safety). Only used when r_vsm_hzb=1 (OFF).
float ps_r_vsm_hzb_margin     = 0.002f;

// Vulkan lighting normalization knobs (live, no restart). Both used to be
// literals scattered across the scene shaders (LDR-era compensation that
// predates the HDR tonemap): sun ×1.25 in world/terrain/vlit/skinned/grass
// and +0.05 ambient floors. They now apply ONCE on the CPU (vk_env_light UBO
// fill + the grass/tree sun pushes), so shaders consume final values and the
// look can be A/B'd in-game: r_sun_boost 1 + r_ambient_floor 0 = raw env values.
float ps_r_sun_boost     = 1.25f;
float ps_r_ambient_floor = 0.05f;

// LINEAR COLOUR PIPELINE — 0 = the historical gamma-space pipeline (albedo sampled
// raw from UNORM textures, lighting maths done on gamma-encoded values, tonemap
// output passed through unencoded); 1 = physically correct linear (Colour textures
// loaded as _SRGB so the sampler decodes on fetch, CPU-side light/env colours
// linearised on upload, and the tonemap applies the real sRGB OETF at the end).
//
// NOT live-switchable: the colourspace is baked into each texture's VkFormat at load
// time, so a change only affects textures loaded afterwards and needs a level reload
// to be coherent. Kept as a cvar anyway because it is the only honest A/B for a change
// that touches every lit pixel — and because the mod's art was authored against the
// gamma-space look, so "correct" and "what it used to look like" genuinely differ.
int ps_r_linear_color = 0;

// NIGHT SUN FREEZE: when the sun gives effectively no light (below the horizon),
// its shadows contribute nothing (every sun receiver multiplies by ~0 sun_color),
// so re-rasterizing them is pure waste. This freezes the VSM DYNAMIC atlas (NPC /
// grass / near-tree crowns — the dominant per-frame shadow cost) at night, reusing
// the existing cadence freeze. r_sun_night_freeze 0 = off; r_sun_night_lum = the
// sun_color luminance below which "night" kicks in (0.02 ≈ deep dusk).
int   ps_r_sun_night_freeze = 1;
float ps_r_sun_night_lum    = 0.05f;   // secondary: freeze if sun_color luminance drops below this
float ps_r_sun_night_alt    = 0.02f;   // primary: freeze when to-sun.y (sun altitude) drops below this (below horizon)

// Grass alpha-test cutoff (live). Lower = fatter/denser blades (less "see-through"),
// higher = thinner. Was hard-coded 0.5 in detail.frag → blades too thin, you could
// see the ground through the grass. 0.33 keeps more of each blade body.
float ps_r_grass_aref    = 0.33f;
// Mip-compensated grass alpha ("alpha sharpen"): boosts sampled alpha by ~N per mip
// level so distant grass keeps its coverage instead of dissolving see-through (alpha
// mips average toward 0). 0 = off (old look), higher = denser far grass. Live.
float ps_r_grass_asharp  = 0.35f;
// Glass opacity CEILING (live). The R4 formula takes both the pane's translucency
// AND the env-reflection share from the TEXTURE alpha — but this mod's glass DDS
// carry alpha ≈ 1 (repacked opaque), which reads as "just a texture". The ceiling
// clamps that: 0.55 = clearly-glass look, 1.0 = R4-faithful (pure texture alpha).
float ps_r_glass_opacity = 0.55f;

// Glass refraction ("uneven old pane" wobble): the late-glass panes re-draw into
// the heat-haze distortion RT and the tonemap bends the scene behind them.
// Strength multiplier; 0 = off. Live.
float ps_r_glass_refr    = 0.5f;   // user-tuned default (2026-07-02)

// COMPUTE PRE-SKINNING (r_preskin). Skin every visible skeleton leaf ONCE per
// frame in a compute pass into a shared world-space pool, then let the ~10
// consumer passes (depth prepass, AO normals, colour, sun cascades, point cube,
// spot tiles, VSM pages, glass) draw that pool instead of re-running the 1-4
// bone blend in each of their vertex shaders. Live; leaves that don't fit the
// pool silently fall back to the old path.
//
// STILL UNPROVEN — the first A/B (l01_escape, 2026-07-23) came out net negative
// but measured the WRONG WORKLOAD: of 835 sampled skeletons, 46% had 1-3 bones
// and 84% had no animation playing (doors, lamps, physics crates, dropped guns);
// only ~4.5% were NPC-scale (42-62 bones). 1-weight geometry can never win here
// — its classic shader is already a single matrix fetch — so the pool was mostly
// paying for leaves with nothing to save. Skinned_PreSkin now skips skinMode<2,
// which removes that loss; the real question (an animated NPC crowd) is still
// open. Numbers from that run, for reference: World/Skinned 0.31->0.31 and
// Shadow/Dyn* unchanged, SunShadow -0.22 ms and World/Depth -0.15 ms against
// +0.30 ms for the compute pass.
//
// Re-measure with MANY ANIMATED NPCs on screen before trusting either verdict,
// and watch [VK PreSkin] skipped1W / maxBones to confirm the scene is actually
// NPC-heavy. Note the frame is GPU-bound on statics (World/Statics ~5.3 ms of a
// 17 ms frame), so the whole skinned budget is ~1.5 ms — the ceiling is small.
int   ps_r_preskin       = 1;

// Animated cloud layer in the sky pass (R4 RenderClouds port): two scrolling
// cloud textures composited over the static cubemap sky. r_clouds on/off,
// r_clouds_intensity = additive brightness, r_clouds_speed = UV scroll rate.
int   ps_r_clouds           = 1;
float ps_r_clouds_intensity = 1.0f;
float ps_r_clouds_speed     = 1.0f;

// r_light_debug — dump the dynamic-light registry + collected set to the log every
// ~2 s (diagnose "lamp lights in R4 but not here": absent from registry = game never
// created it; in registry but not collected = cull; collected = shading side).
int   ps_r_light_debug   = 0;

// REAL volumetric light cones (vk_pass_lightcones): every volumetric-flagged
// SPOT light gets an analytic raymarched beam built from its true pos/dir/
// cone/range/colour — replaces the R4 `models\lightplanes` texture-sheet fakes.
int   ps_r_light_cones        = 1;      // master
float ps_r_light_cone_density = 1.2f;   // in-scatter strength (live)
float ps_r_light_cone_len     = 2.5f;   // beam length = light range × this (live)
float ps_r_light_cone_glare   = 2.0f;   // extra flare looking into the beam (live)
float ps_r_light_cone_narrow  = 0.55f;  // visible beam = lit cone × this (bright core look)

// Beam luminance in HDR scene units (2.2 ≈ sun-lit level). The honest media
// model makes beams physically vanish against a daylit background — raise
// this for the R4-style "always visible" look, lower for realism.
float ps_r_light_cone_lum = 2.2f;

// Beams synthesized from `models\lightplanes` fan geometry (car headlights,
// searchlights, halogen lamps — carriers R4 gives NO dynamic light, the beam
// is baked into the model). See vkSynthBeam / SynthCones.
int   ps_r_light_cone_synth = 1;

// Vertical lift (m) for the synthesized beam apex: the fan quads hang a few
// centimetres BELOW the lamp centre, so the cone started just under the
// headlight. Live-tunable.
float ps_r_light_cone_lift = 0.11f;   // user-tuned on the zaz headlight

// Lamp-face radius multiplier for synthesized beams. The frustum base radius
// comes from the fan's own width at the lamp — 1.0 = as authored; raise for
// a fatter "whole headlight glows" look, 0 → point cone.
float ps_r_light_cone_base = 0.2f;   // user-tuned on the zaz headlight

// Synthesized beams are REAL lights: reach = fan length × this (a headlight
// shines tens of metres, not the 5 m fan the artist modelled) — drives both
// the spot light range and the visible beam length (depth still cuts it at
// geometry). power = surface-light intensity (0 = visual beam only).
float ps_r_light_cone_reach = 6.0f;   // user: 4.0 felt short for a headlight
float ps_r_light_cone_power = 1.0f;

// Synth beams draw only a short lamp-face glow: full within ~15 cm of the lamp,
// then exp(-d/this) to fully transparent — the LONG beam shape comes from the
// froxel fog / particles (like the flashlight and sun shafts), not from a
// painted-on milk cone.
float ps_r_light_cone_fade = 0.8f;

// Per-step shadow-tap disc radius (texels) in the visible-beam raymarch — an
// area-light penumbra. The spot map resolves grass blades at mm texels, so a
// hard point tap paints razor "threads" through a grass field; the 24 march
// steps integrate the jittered disc into a soft penumbra (no temporal needed).
float ps_r_light_cone_soft = 6.0f;

// Analytic visible cones for ordinary dynamic torches (CTorch — the player's
// and NPCs' head-lamps). Default 0 = R4 behaviour: a torch is just a lit spot
// whose volumetric shaft comes from the froxel fog (r_vol), NOT a hard searchlight
// cone. The synthesized lamp/headlight beams (from lightplanes geometry) are a
// SEPARATE path and are unaffected by this flag. 1 = also draw the analytic cone
// for torches (old behaviour — NPC head-lamps read as projectors at night).
int   ps_r_light_cone_torch = 0;

// Bright lamp-face glow for a FLASHLIGHT aimed at the camera (R4 lens-flare analog).
// The torch cone is off (not a searchlight), but an NPC pointing its head-lamp AT you
// should show a bright "bulb" — otherwise the beam lights the scene while the lamp
// reads as switched off. Facing-gated (0 when aimed away → your own torch never flares
// in-face). 0 = off. Higher = brighter bulb. (5.0 was too hot — 2.0 reads as a lamp.)
float ps_r_flashlight_glow = 2.0f;

// Grass casters into the SPOT shadow map: blades cut a beam's light pool and
// its visible volumetric cone (headlight/searchlight/flashlight through a
// grass field). Same 1-frame-stale GPU instance buffer the VSM grass uses.
int   ps_r_spot_grass    = 1;

// Grass casters into the SUN cascades (r_sun_grass): swaying blades cast real
// sun shadows — drawn into the per-frame dynamic overlay (with the NPCs and
// wind trees) of BOTH near cascades. dist = instance-cull radius around the
// camera; blades beyond it are subpixel in the map anyway.
int   ps_r_sun_grass      = 1;
float ps_r_sun_grass_dist = 30.f;

// Grass casters into the POINT (campfire) cubes: blades around a near fire cut
// its light into swaying dapples (surfaces + froxel fog). Adds a cadence
// re-render for in-view fires closer than ~40 m — same LOD as the NPC overlay.
int   ps_r_point_grass    = 1;

int   ps_r_grass_cull_debug = 0;   // [VK GrassCull] per-call lights/types/cells log

// Spot shadow POOL size: how many spots get their own shadow tile per frame
// (atlas holds 8). Nearest-first, narrow beams keep a 4× distance advantage.
// Tiles are cached — a static lamp renders its tile once — so 8 costs little.
int   ps_r_spot_pool = 8;

// The flashlight on the Switch (10-09): the spot maps rendered into a corner of each
// atlas tile (1 = the whole 1024 tile; 0.5 = 512 px, a quarter of the raster and of
// the tile copies), the receivers' PCF (1 = 3x3, 0 = one 2x2 gather), and whether a
// flashlight competes for a shadow tile at all (0 = lit without a map or cookie).
float ps_r_spot_scale = 1.0f;
int   ps_r_spot_pcf = 1;
int   ps_r_flash_shadow = 1;
float ps_r_spot_npc_dist = 0.f;   // NPC casters in a flashlight tile only within this many metres of it (0 = its range)
int   ps_r_flash_mask = 0;      // the torch shadow x cookie resolved into the half-res sun mask for the R1 receivers (Switch)

// Campfire/brazier glow tuning (VOLUMETRIC-flagged point lights only — fires,
// pole lamps; plain table lamps stay untouched). boost = light intensity ×,
// range = reach ×. Applied consistently to shading, fog AND the shadow cube.
// range default 2 = stock fires light twice as far (user-tuned 2026-07-03).
float ps_r_point_boost = 1.0f;
float ps_r_point_range = 2.0f;

// Point-shadow DEBUG overlay: paint opaque receivers by pooled-point coverage —
// GREEN = lit by a pooled point light, RED = that light's shadow on this pixel.
// Instantly shows whether a campfire casts a light pool and an NPC shadow in it.
int   ps_r_point_debug = 0;

// Grass self-shadow anti-acne slack (m, along the sun ray): grass receivers sample
// the DYN VSM atlas (which contains the blades themselves) at their own position —
// occluders closer than this don't shadow (kills self-acne), further ones do
// (clump-on-clump dapples, NPC shadows). Lower = fuller grass shadows but risk of
// dark sparkle; higher = cleaner but shadows start further from the caster's base.
float ps_r_grass_self_bias = 0.15f;

// Sun-beam ground recovery (r_sun_beam). The volumetric shaft samples the crisp VSM
// ATLAS; surfaces sample the temporally-SMEARED screen mask (vsm_resolve), which
// closes the thin sun gap a crown casts — so the beam "dissolves" on the ground
// instead of landing as a lit pool. Terrain re-samples the crisp atlas at its own
// world pos and takes the max, recovering the gap so the ground lights up in
// agreement with the shaft; boost adds a small extra sun kick in the recovered gap
// for a cinematic splash (bloom then picks up the bright pool). dist caps it to near
// ground (resident pages + where it reads); bias = anti-acne slack (m along sun ray).
// Only active under r_vsm (crisp atlas source). 0 = off (old smeared-mask behaviour).
// ================================ PARKED 2026-07-06 ================================
// SUN-BEAM ground-lighting arc (r_sun_beam* family) — PARKED by user ("что-то не то").
// Goal was: a sunbeam cutting through a tree should "разбиваться о терейн" (land as a lit
// pool) instead of dissolving. Three approaches tried, ALL default 0 now (inert), code
// kept behind the cvars as scaffolding (do NOT delete — see the memory note + world_
// terrain.frag / tonemap.frag / vk_env_light.cpp PARKED comments):
//   (1) r_sun_beam        — crisp-atlas gap RECOVERY on terrain. Dead end: r_terrain_debug
//                           7 proved the ground is genuinely sun-occluded (no atlas gap).
//   (2) r_sun_beam_splash — tonemap in-scatter/surface splash. Read as AIR haze, not ground.
//   (3) r_sun_beam_ground — forward deposit (albedo×sun) driven by a shadow-march overhead.
//                           View-independent dapple, but user still "не то". Parked here.
// r_terrain_debug 5/6/7 (recovery probes) + 8/9 (deposit probes) left wired for a revisit.
// ===================================================================================
// Terrain recovery (r_sun_beam): DEFAULT 0 — proven a no-op in dappled-crown scenes
// (r_terrain_debug 7 showed gap≈0: the ground under a visible shaft is genuinely sun-
// occluded, so the crisp atlas agrees with the smeared mask — nothing to recover).
// Kept behind the cvar for wide gaps where the atlas DOES resolve a lit patch.
float ps_r_sun_beam        = 0.0f;
float ps_r_sun_beam_dist   = 40.0f;
float ps_r_sun_beam_boost  = 1.6f;
float ps_r_sun_beam_bias   = 0.10f;

// God-ray GROUND SPLASH (r_sun_beam_splash, tonemap.frag). The real fix for "луч
// растворяется у земли": the visible shaft is lit air your view ray gathered through
// the crown gap — bright integrated in-scatter that, at the ground, reads as thin
// haze. Where that beam luminance outshines the (shadowed, dark) ground it lands on,
// add the excess back boosted so the contact pops and bloom flares it into a splash.
// strength scales it (0 = off); thr = the in-scatter luminance below which it's just
// ambient fog and stays untouched (raise if general fog brightens; lower for subtler
// beams to splash). Aesthetic, not a relight — the sun does not physically reach there.
float ps_r_sun_beam_splash     = 0.0f;   // superseded by the forward GROUND deposit below (kept for A/B)
float ps_r_sun_beam_splash_thr = 0.12f;

// Grass lighting-component debug (detail.frag): paint the grass ONLY with one
// isolated term, greyscale — attributes any pattern "seen through the bush" to
// its real source. 1=applied sun shadow, 2=VSM mask R (full, ground-behind),
// 3=mask B (static-only), 4=channel pick weights, 5=GTAO-behind, 6=SSIL-behind,
// 7=baked slot sun occlusion, 8=baked slot hemi, 9=full ambient, 10=dyn lights,
// 11=shadow source split (grey=static shadow, RED=dyn/grass-atlas shadow).
int   ps_r_grass_debug = 0;

// Point shadow POOL size: how many campfires/lamps get their own shadow cube
// per frame (array holds kMaxShadowPoints). Cubes are 6× a spot tile, so the
// cap is smaller; cached + LOD-gated like spots (static fire = one render).
int   ps_r_point_pool = 4;

// Grass shadow strength on SURFACES (spotShadowF blends the clean spot map
// with the spot+grass beam map). 0 = grass never shadows surfaces (sterile
// pool), 1 = full blanket (dense grass eats the headlight's ground pool);
// mid = translucent dapples — light scatters through grass IRL.
float ps_r_spot_grass_shadow = 0.55f;

// Grass shadow strength for FLASHLIGHT tiles only (handheld/worn torch — CTorch
// head-lamp, weapon light), SEPARATE from the wide-fixture blend above. A torch
// beam raking through a grass field at night should throw CRISP blade shadows on
// the ground (the "wow"), so flashlight tiles default to FULL grass shadow (1.0)
// while lamps/searchlights stay at the subtle r_spot_grass_shadow. Needs r_spot_grass.
float ps_r_flashlight_grass = 1.0f;

// Respect the DO_NO_WAVING detail flag (1) or force SSFX wind on every detail type
// (0, the old behaviour). R4 keeps flagged micro-plants (tiny shoots in asphalt)
// static; without this they stretched under the tree/grass wind.
int   ps_r_grass_nowave  = 1;

// Rain wetness knobs (live): darken = how much wet albedo darkens (0 = off,
// 0.4 ≈ wet asphalt), refl = sky-reflection strength on wet surfaces.
// r_wet_debug 1 = world shaders draw the wet mask (wetness × rain-map
// visibility) grayscale — white = soaked, black = dry/covered.
float ps_r_wet_darken = 0.4f;
// r_wet_force (10-10, NoSave): -1 = the wetness accumulator (soak ~45 s of full rain, dry ~3 min); 0..1 = the wetness the
// shaders get, at once -- an A/B or a look at the wet surfaces without waiting for the rain.
float ps_r_wet_force = -1.f;
float ps_r_wet_refl   = 0.6f;
int   ps_r_wet_debug  = 0;
// r_rain_debug 1 = rain streaks drawn SOLID RED (triage: geometry vs texture).
int   ps_r_rain_debug = 0;
// r_rain 0 = master OFF: skips the whole Rain pass (drops/splashes/thunderbolt)
// AND forces wetness/density to 0 (dry surfaces, no rain occlusion map). Does
// NOT change the weather — just suppresses the rain effect for testing.
int   ps_r_rain_enable = 1;
// r_rain_drops (10-10): the most rain drops at full density (0 = stock: 2000, 1000 under three cores). Every drop that wraps
// round the camera casts a ray into the level (dxRainRender::Calculate) on the main thread.
int   ps_r_rain_drops = 0;
// r_rain_rq_static (10-10): those rays test the static level only, not the dynamic objects too (a drop then falls through
// an NPC -- unseen in a downpour). 0 = stock (rqtBoth).
int   ps_r_rain_rq_static = 0;
// r_rain_hcache (10-10, Switch preset 1): with r_rain_rq_static the drop rays (births + wrap) are answered from a height cache
// of 0.5 m cells (CEffect_Rain::RayPickCached), each cell one vertical ray the first time -- the static level does not move.
// The rays were ~5.8 % of the main thread in a big rainy firefight on the Switch.
int   ps_r_rain_hcache = 0;
// r_rain_sun — how strongly the streaks catch the DIRECTIONAL light (sun, moon,
// lightning). Drops were tinted by hemi alone, which is flat-lit rain and, worse,
// rain a thunderbolt cannot touch: the bolt boosts sun/sky/fog colour and swings
// sun_dir to the strike (thunderbolt.cpp), and hemi is the one field it never
// writes. Water cylinders scatter FORWARD, so real rain lights up when you look
// toward the light and goes near-black when you look away — one forward lobe on
// the sun direction buys both the backlit downpour and the free lightning flash.
// 0 = the old flat hemi look.
float ps_r_rain_sun = 0.35f;

// Global render profiler (vk_profiler). 0 = no [VK Perf] logging, 1 = periodic
// (~5s) GPU/CPU/VRAM log, 2 = + the live ImGui overlay (Phase 2). `vk_perf`
// forces an immediate MARK snapshot regardless of this value.
// GPU-driven particles (gpu_particles_roadmap.md). Phase 1 — one hardcoded
// effect (Source+Gravity+KillOld) simulated + drawn entirely on the GPU.
// 0 = off (CPU PAPI path draws everything), 1 = GPU test effect at the camera.
int   ps_r_gpu_particles     = 0;       // r_gpu_particles — master GPU-particles switch
int   ps_r_gpu_particles_max = 1 << 16; // r_gpu_particles_max — particle pool cap (64K)
int   ps_r_gpu_particles_sort = 1;      // r_gpu_particles_sort — Phase 4 depth sort of alpha smoke (back-to-front)
float ps_r_gpu_particles_life_cap = 5.0f;  // r_gpu_particles_life_cap — clamp GPU particle lifetime (s): "immortal"
                                           // flames (KillOld 1000s) would hold their #5 alive budget forever and
                                           // starve every other instance of the program. 0 = off. Applied at
                                           // translate time (cached per program — takes effect on restart/new level).

int   ps_r_profiler = 1;

// Variable Rate Shading (vk_vrs, depth-driven). 0 = off, 1 = mild, 2 = aggressive.
// near/far = distance (m) thresholds: below near stays 1x1, above far goes coarsest.
// (Live — tune without restart. Level 2 pulls them ~closer.)
int   ps_r_vrs      = 0;
int   ps_r_vrs_force = 0;   // diag: force pipeline-rate NxN on the world pass (2 or 4), ignoring the SRI
// Prepass-covered statics color path: depth write OFF (prepass depth is final).
// THE early-Z fix: the world uber-FS statically contains `discard`, and
// discard + z-write ON disables early-Z entirely — the whole frustum's depth
// complexity was being shaded (gpuStatics 10.8M FS inv vs ~3.5M visible,
// r_fsinv_split, Кордон 23-07-2026). Live A/B.
int   ps_r_z_prepass = 1;
// Screen-size (SSA) cull of plain whole meshes in the GPU world cull: skip a
// mesh once its bounding sphere projects under N pixels of DIAMETER. The
// r_fsinv_split hunt (23-07-2026, Кордон) showed distant small props (<256 tris
// never cluster/LOD) rendering full geometry to the horizon = sub-pixel
// triangles = 8x quad-helper cost (gpuMesh 8.24M FS inv vs 2.26M visible
// samples). Clustered/DAG geometry is exempt. 0 = off. Live.
float ps_r_ssa_px = 2.0f;
// Diag: split the world-color FS-invocation counter 4 ways (CPU statics flush /
// GPU statics / dynamics / skinned) to attribute overshading. Replaces the
// single [VK VRS] invocations line with [VK FSinv] while on. Live.
int   ps_r_fsinv_split = 0;
float ps_r_vrs_near = 40.0f;
float ps_r_vrs_far  = 75.0f;

// Frustum culling of world statics (default ON). The big win vs R4 — we used to
// submit the entire level every frame. r_cull 0 = brute-force all (A/B).
int   ps_r_cull = 1;

// GPU-driven sun shadow casters (vk_shadow_gpu): opaque statics are compute-culled
// per cascade and drawn via vkCmdDrawIndexedIndirectCount; the CPU queue then draws
// only the alpha-tested cutout casters. Default ON. r_gpu_shadows 0 = old CPU
// FlushDepth-all path (A/B for the SunShadow profiler zone).
int   ps_r_gpu_shadows = 1;

// caster-LOD for GPU-driven sun shadows: casters farther than r_shadow_lod_dist
// metres from the camera draw their COARSE sliding-window slice (progressive
// terrain/big meshes → far fewer triangles in the shadow map). r_shadow_lod 0 =
// full detail everywhere (A/B). Only affects the GPU shadow path.
int   ps_r_shadow_lod      = 1;
float ps_r_shadow_lod_dist = 30.0f;

// Cascade shadow-map STATIC cache (vk_pass_shadow): the statics-only cascade depth
// is re-rastered only when the camera moves past kCascRedrawDist OR the sun rotates
// past r_shadow_casc_sun degrees; otherwise it's reused and only the skinned dynamics
// are overlaid (kills the ~3 ms/frame Shadow/Casc0 raster while standing). 0 = old
// per-frame raster (smoothest sun creep, full cost). r_shadow_casc_sun is the sun
// step that forces a redraw — SMALLER = smoother shadow motion as the sun moves, at
// the cost of more frequent (but cheaply amortized) redraws.
int   ps_r_shadow_casc_cache = 1;
// Far sun map: on a camera/sun cache miss collect its casters on a worker (core 2) and keep the old map until done (0 = in
// the render thread, the frame of the miss; see s_FarJobQueue in vk_pass_shadow.cpp).
int   ps_r_shadow_far_async  = 1;
float ps_r_shadow_casc_sun   = 0.05f;

// SUN SHADOWS, master off switch (vk_pass_shadow).
//
// ⚠⚠Зачем отдельный квар, если есть `r_shadows Off`. Потому что тир Off гасит
// ТОЛЬКО VSM (ps_r_vsm = 0) и передаёт солнце классическому каскадному пути,
// а у того гейта не было вообще: Pass_SunShadow выходил раньше времени лишь
// когда не инициализированы картинка или шейдер. 18-09-2026 это поймано
// глазами — «выключил все тени, а они на экране», и профайлер подтвердил:
// SunShadow 1.85 мс оставалось самой дорогой зоной кадра на самом низком тире.
//
// ⭐Реализация ничего не изобретает: квар подмешивается в УЖЕ существующий
// ночной гейт `sunLit`. Тот путь проверен — он пропускает растр и копии, но
// оставляет клиры, переходы слоёв и зоны профайлера на месте, а очищенная
// карта глубины (клир 1.0) читается приёмником как «всё освещено». Шейдеры
// править не нужно.
int   ps_r_sun_shadows = 1;

// Virtual Shadow Maps (vk_vsm) — sun directional clipmap with sparse physical pages:
// only pages sampled by visible pixels are rendered, and they're cached across
// frames in world space. Smooth moving-sun shadows (only visible pages re-render)
// + standing ≈ 0. r_vsm_debug logs per-frame page mark/alloc counts.
//
// DEFAULT ON since 12-08-2026 — VSM is now the PRIMARY sun-shadow path. The reason
// is measured, not aesthetic: under the classic cascades the per-frame near-tree
// caster layer (Sh/TreeDyn0/1) cost 6.34 ms = 65% of the whole SunShadow budget,
// and the cascade path has NO caster LOD for trees at all. Every tree caster-LOD
// mechanism in this renderer (crown hulls, voxel clouds, impostors, meshlet+HZB
// binning) lives on the VSM side, so the cheap fix and the shadow system are the
// same decision. See [[vulkan-tree-forward-no-lod-double-draw]].
// ⚠ The cascade path is NOT dead and must keep working — see the note at
// `cascRaster` in vk_pass_shadow.cpp: it is the volumetric-fog occluder while the
// VSM atlas warms up, and the froxel shader falls back to it for pages that are
// not resident yet.
int   ps_r_vsm       = 1;
int   ps_r_vsm_debug = 0;
// VSM receiver depth-compare bias (normalized clipmap Z, range ~2000 m). Larger =
// less acne but more light-leak (small/thin caster shadows fade). Live-tunable.
int   ps_r_vsm_grass_static = 1;  // far-grass hybrid: L1/L2 (12..48 m) grass RIGID into the cached static atlas
                                  // (dirty pages only ≈ free standing); L0 (±12 m) stays dynamic with live wind.
                                  // 0 = old behaviour: ALL grass every-frame dynamic, L0..L2.
float ps_r_vsm_bias  = 0.0003f;   // STATIC atlas (terrain in it → needs acne slack; 0.0003 = 0.6 m at the ±1000 m z-range)
                                  // This is only the CAP of the slope-scaled bias.
float ps_r_vsm_bias_dyn = 0.00002f;   // DYNAMIC atlas (casters only, ground never in it → epsilon over D16 quantization
                                      // + write-side raster bias; the shared 0.6 m ate grass shadows below knee height)
float ps_r_vsm_raster_bias  = 0.5f;   // atlas WRITE depth bias, constant (D16 units ≈ 3 cm each). The old 1.5 pushed
                                      // every caster ~5 cm deep — most of a plank wall's thickness gone before any
                                      // receiver got a vote. Live; changing it drops the page cache (crisp A/B).
float ps_r_vsm_raster_slope = 1.5f;   // atlas WRITE depth bias, slope part (caster-side grazing acne; old 2.5).
float ps_r_vol_surf_clip = 0.15f;     // depth-rejection FRONT shell (m): also kill in-scatter within X m BEFORE the
                                      // visible surface. The "lit shell" hugging thin sun-facing walls can't be fixed
                                      // by any receiver bias (atlas write bias + D16 quantization ≈ plank thickness);
                                      // fog centimetres from a wall carries no legit light anyway. 0 = off.
float ps_r_vol_depth_reject = 0.12f;  // froxel depth-rejection slack (m): fog BEHIND the geometry of its own view
                                      // column gets no in-scatter — kills the sunlit-outdoors glow the trilinear
                                      // volume fetch smears onto walls around windows. 0 = off (legacy).
                                      // MUST be well under half a froxel slice (~1 m at 8 m with the scene-far
                                      // grid): the leaking froxel is the wall-STRADDLING one, its centre sits
                                      // only ~0.1-0.5 m behind the surface — a bigger slack filters nothing.
// VSM clipmap detail: base extent (m) of clipmap level 0 → finest texel = base/4096.
// 24 m ≈ the old 4096² cascade (5.9 mm), balanced. Smaller = sharper but more pages
// (the 2048-page atlas can overflow on wide vistas → distant pages drop, graceful).
// Live-tunable; meant to back a future "VSM detail Low/Med/High" graphics slider.
float ps_r_vsm_base  = 24.0f;   // finest texel 5.9mm ≈ old cascade; temporal accumulation makes this coarse base look as clean as 12 did (user-verified) → cheap default. Future graphics slider Low/Med/High = 32/24/16.
// VSM temporal accumulation (TAA-for-shadows): EMA-blend a reprojected history in the
// screen-space resolve → smooths the moving-sun shadow-edge crawl and the staggered
// round-robin page refreshes. (No clipmap jitter — it would mismatch the toroidal
// page cache; the window is page-snapped instead.) Live.
int   ps_r_vsm_temporal = 1;
// History weight (EMA alpha): higher = smoother/stabler but more ghosting under motion;
// lower = crisper but more residual crawl. 0.9 ≈ ~10-frame convergence. Live-tunable.
float ps_r_vsm_ta_blend = 0.9f;
// History weight on pixels the DYNAMIC atlas shadows (wind-swaying crowns, NPCs): their
// casters move every frame, so the full 0.9 EMA drags a ~10-frame smear ("jelly") behind
// the shadow. Low weight = crisp sway; the edge moves anyway, so the texel crawl the EMA
// exists to hide is imperceptible there. Static-shadow pixels keep r_vsm_ta_blend. Live.
float ps_r_vsm_ta_blend_dyn = 0.35f;
// Foliage-shadow "каша" fix (3 knobs on the temporal resolve). The 0.9 EMA above is great
// for STILL scenes but smears high-frequency leaf/bush shadows into mush under CAMERA
// motion (bilinear history reproject blurs a bit more each frame; the distance reject only
// catches disocclusion, not same-surface shadow change). These target that without losing
// the still-scene detail:
// (1) neighbourhood clamp — history bound to the current shadow ±tol (kills the trail, keeps
//     sub-texel jitter averaging). 0 = off (raw EMA). ~0.2-0.3 = crisp, higher = softer.
float ps_r_vsm_ta_clamp = 0.24f;
// (2) motion-adaptive — fade the history weight to a floor by reprojected screen motion:
//     ta_motion = px of motion to reach the floor; ta_motion_floor = weight there. Still
//     camera keeps the full EMA; moving camera stops compounding the blur.
float ps_r_vsm_ta_motion = 6.0f;
float ps_r_vsm_ta_motion_floor = 0.30f;
// (3) DLSS-aware — when r_dlss is on, DLSS ALSO temporally resolves the shadow (baked into
//     the colour it upscales) → scale the VSM history weight down to avoid a double blur.
float ps_r_vsm_ta_blend_dlss = 0.6f;
// SOFT SHADOWS (stochastic PCSS) — replaces the fixed 3x3 PCF in vsm_resolve with a
// blocker search + a filter disc sized by the blocker distance, so a shadow is HARD at
// the contact point and softens with the caster's height. Both discs are Vogel spirals
// rotated per pixel and per frame: the error lands as noise, which the temporal resolve
// right below already exists to average away. r_vsm_soft = filter tap count (0 = the
// legacy PCF path, byte-identical). See vsm_resolve.comp.glsl for the derivation.
// ⭐Tap counts. Were 12/8 while the cone was 2 deg wide and the disc needed the coverage;
// at the 0.2 deg below the filter disc is ~5 cm and 8/4 already sample it densely. Fewer
// taps on a SMALLER disc is also cheaper twice over: less arithmetic, and the taps land
// in the same texture cache lines instead of scattering across a metre.
int   ps_r_vsm_soft        = 8;
int   ps_r_vsm_soft_search = 4;
// Sun cone HALF-angle in degrees — the ONE knob that sets penumbra width, linearly:
// w = tan(angle) * blockerDistance, capped at tan(angle) * r_vsm_soft_range.
//
// ⚠⚠THIS DEFAULT WAS 2.0 AND THE SHADOWS WERE INVISIBLE. USER-REPORTED 19-08, confirmed
// by A/B in one command (r_vsm_soft 0 → shadows snap back instantly). 2.0 deg is 7.5x the
// real sun (0.265) and puts the cap at a 1.05 m RADIUS — a 2.1 m wide smear. Worse than
// the width alone: 12 taps over a 1 m disc sit ~0.6 m apart, so the estimate is mostly
// noise, and the temporal resolve below (blend 0.9, carry 0.98) grinds that into grey mush.
// The blocker search averages over a disc of the same radius, so a trunk at 2 m and a
// crown at 15 m end up in one number.
//
// 0.2 is a deliberate artistic choice — very slightly SHARPER than the true sun. It caps
// the penumbra at ~5 cm with r_vsm_soft_range 15: contact shadows read as crisp, and the
// softening stays where it belongs (crowns, distant roofs). Raise toward 0.6 for visibly
// cinematic; 0.265 is the physical value. ⛔Do not go back above ~1.0 without re-checking
// the tap count — the two are coupled through disc area.
float ps_r_vsm_soft_angle  = 0.2f;
// Max blocker search distance (m). Caps the widest penumbra (angle * range) AND sizes the
// blocker-search disc, so lowering it both bounds the smear and keeps the blocker estimate
// local. A caster further than this softens no further — a graceful clamp, not a dropout.
// ⚠NOT a pure quality knob despite what rspec_vk_*.ltx does with it: it multiplies into
// penumbra WIDTH, so moving it changes the look, not just the cost.
float ps_r_vsm_soft_range  = 15.0f;
// Neighbourhood clamp used INSTEAD of r_vsm_ta_clamp while soft is on: the 0.24 above was
// tuned against deterministic taps and would pin the history to the stochastic noise.
// ⭐Was 0.55, calibrated for the 2 deg disc's noise. On a 5 cm disc that much slack is pure
// smear — 0.55 on a 0..1 shadow lets the history sit half the range away from the current
// value without being clipped, which reads as a trail, not a penumbra. Tightening THIS is
// what actually restored edge definition for the user — more than narrowing the cone did.
float ps_r_vsm_soft_clamp  = 0.28f;
// Grass casts VSM shadows (near + L0 only; reads the GPU-driven detail CASTER buffer
// 1 frame stale). DEFAULT ON since 2026-07-03: the two blockers that parked it are gone —
// dyn pages now resolve with the low adaptive history weight (r_vsm_ta_blend_dyn kills the
// "see-through" ghosting) and the caster buffer is frustum-independent (blades behind the
// camera still cast). Pages also get SSFX wind now — the shadow sways with the blade.
int   ps_r_vsm_grass      = 1;
float ps_r_vsm_grass_dist = 20.0f;   // max grass cast distance from camera (m)
float ps_r_vsm_npc_dist   = 50.0f;   // max NPC shadow-cast distance into the VSM atlas (m); 0 = no cull (NPC shadows tiny past ~50m)
float ps_r_vsm_lod_dist   = 0.0f;    // VSM caster-LOD: opaque casters draw coarse slice past this (m); 0 = off (measured marginal in village, like the cascade; kept for open maps)
int   ps_r_vsm_mark_half  = 1;       // page-mark at half-res (4x fewer threads/atomics); 0 = full-res
// Resolve skips the 9 dyn-atlas PCF taps on pages no NPC/grass caster binned into (the dyn
// alloc claims a slot for EVERY visible page, so most dyn pages are cleared-empty). Big
// bandwidth save on the resolve (esp. iGPU). 0 = old behavior: sample the dyn atlas on every
// resident page (A/B knob). Verified in-game 2026-07-01 (red-overlay debug r_vsm_debug_dyn:
// NPC shadows intact, flagged pages match the visible shadows) → DEFAULT ON.
int   ps_r_vsm_dyn_gate   = 1;
// Debug visualizer: everything shadowed by the VSM DYNAMIC atlas (NPC/grass casters) is
// tinted RED by the tonemap (mask.B, written raw by the resolve, UNGATED by r_vsm_dyn_gate).
// Answers "are NPC shadows actually there?" under any weather/lighting.
int   ps_r_vsm_debug_dyn  = 0;
// VSM static-atlas CACHE: toroidal per-page residency for the static (opaque + tree) atlas —
// each frame only pages that scrolled into the window (or hit their round-robin refresh while
// the sun moves) re-render; the rest keep their cached depth (the dynamic NPC/grass atlas
// still re-renders every frame). DEFAULT ON (1): the shipping path. Measured 2026-06-19 A/B in
// the same scene with r_vol on: VSMrender 14.2 -> 2.4 ms, gpu_total 23.3 -> 12.5 ms, fps 43 -> 83.
// 0 = the render-all-every-frame baseline (A/B knob). Live.
int   ps_r_vsm_cache     = 1;
// Round-robin refresh period (frames) for the moving-sun case (toroidal cache): each
// frame ~1/N of the resident static pages re-render to track the creeping sun smoothly (no
// jump). Smaller = fresher but costlier; only active while the sun moves (paused sun → 0). Live.
int   ps_r_vsm_cache_refresh = 8;
// THROTTLE (UE5 VirtualShadowMapThrottle port): feed last frame's World/VSMrender GPU ms
// back into a clipmap LOD bias — over budget marks pages 1-2 levels COARSER (each level
// quarters the marked pages = the only lever that cuts alpha-test FILL), under budget
// recovers slowly. Engages only when the scene actually exceeds the budget; 0 = off (A/B).
int   ps_r_vsm_throttle        = 1;
float ps_r_vsm_throttle_budget = 3.0f;   // target VSMrender ms (default clips only the heavy scenes/spikes)
float ps_r_vsm_throttle_max    = 2.0f;   // max bias in clipmap levels (2 = up to 16x fewer pages)

// GAZE refresh (vsm_mark + vsm_resid): while the sun moves, static-atlas pages the
// player is actually looking at re-render on a cadence proportional to their
// on-screen footprint (sampled-pixel count from the mark) — big/near shadows glide
// every frame like the dynamic atlas, small/far ones tick on the round-robin where
// the jump is sub-noticeable. Budget = max gaze pages/frame; px = mark samples
// (half-res: screen px / 4) for the every-frame tier, cadence = ceil(px/hits).
int   ps_r_vsm_gaze_pages      = 96;
int   ps_r_vsm_gaze_px         = 4096;

// GPU-driven world forward pass (vk_world_gpu): static opaque/AT meshes are
// compute-culled + drawn via indirect (1 draw/material group) instead of the
// per-object CPU queue, which also dedups the hierarchy double-submit. Cuts CPU
// draw-call count massively → fps win on CPU-bound / detail-heavy levels.
// r_gpu_world 0 = old CPU path (A/B). Default ON: measured ~2× fps in the village
// (CPU ~halved, World/Statics ~5× lower) + user-verified visually identical.
int   ps_r_gpu_world = 1;

// Cluster (meshlet) granularity for the GPU world cull (vk_world_gpu): static
// meshes above r_cluster_tris triangles are split at level load into ~128-tri
// clusters (meshoptimizer), so the compute frustum + Hi-Z cull operates on
// cluster spheres instead of whole material fragments — a building wall no
// longer draws entirely because one corner peeks into view. Foundation of the
// cluster-LOD (Nanite-like) system, Phase 1. Takes effect on level (re)load.
int   ps_r_cluster = 1;
int   ps_r_cluster_tris = 256;   // 256 = 2+ meshlets → the mesh can LOD at all
// Phase 2 — cluster DAG LOD: clusters carry a simplification error + their
// birth-group sphere; the cull shader picks the DAG cut whose projected error
// stays under r_cluster_lod PIXELS (live knob: smaller = finer, 1.0 ≈ visually
// lossless; 4.0 with the dither crossfade reads clean and lets trims/frames
// dissolve into facades at gameplay distances — user-tuned on Кордон).
// r_cluster_debug: 1 = clusters as flat colors (UE-style cluster
// view), 2 = colored wireframe over the scene — both show density fall off
// with distance in real time.
float ps_r_cluster_lod = 4.0f;
int   ps_r_cluster_debug = 0;
// Phase 3 — cluster-LOD SHADOW casters. For an ORTHO light the DAG cut is a
// constant world-error budget = target texel size × r_vsm_cluster_lod (in
// texels, live) — camera distance does not participate, so cached shadow
// content (VSM static pages, cascade static maps) never changes as the player
// moves: shadow LOD is invalidation-free by construction.
// r_shadow_cluster: sun far map + cascades draw the cluster cut instead of the
// per-mesh ShadowGPU set. r_vsm_cluster: the VSM static-atlas bin walks cluster
// entries with a per-clipmap-level budget (coarse levels render coarse DAG
// levels — the geometry win). Both fall back to the per-mesh path when off or
// when the cluster set is unavailable.
int   ps_r_shadow_cluster   = 1;
// r_gpu_shadows_at: alpha-tested casters (fences, bush/crown cutouts) draw
// through the SAME cluster shadow cull/indirect path as the opaques — the cull
// stops skipping meta flag bit0 and DrawShadow binds the material's diffuse for
// the discard (mirrors WorldGPU::DrawDepth). With it, the CPU shadow caster
// queues (far/cascade/fog/rain-AT/spot/point statics) stop being built at all.
// 0 = legacy split: GPU opaques + CPU FlushDepth(alphaTestedOnly) cutouts.
int   ps_r_gpu_shadows_at   = 1;
// r_vsm_at: alpha-tested statics cast into the VSM STATIC atlas (they never did
// before — fences/grates had no shadow under r_vsm). Cluster path only: the AT
// groups become per-material buffer combos and draw with a discard page
// pipeline (vsm_page_at) binding the material's diffuse. Flip invalidates the
// toroidal cache (page content changes). A/B live.
int   ps_r_vsm_at           = 1;
int   ps_r_vsm_cluster      = 1;
float ps_r_vsm_cluster_lod  = 1.0f;
// Crossfade band for DAG-cut transitions, as a fraction of the px threshold:
// within the band BOTH levels draw with complementary Bayer screen-door masks
// (prepass + color dither identically), so the swap reads as a short dissolve
// instead of a pop. 0 = hard cut (old behavior). Live.
float ps_r_cluster_fade = 0.25f;
// Phase 2.5 — component merge: SOLID fragments of one building whose AABBs
// touch are merged into a single cluster DAG, so low-poly parts (doors, window
// frames, trims) dissolve into the facade at distance instead of collapsing
// onto themselves (their own DAG has nowhere to dissolve to). Alpha-tested
// meshes stay on the per-mesh path. Takes effect on level (re)load; the DAG
// disk cache is keyed on this flag.
int   ps_r_cluster_merge = 1;

// r_cluster_cache — 1 = use the on-disk cluster DAG cache, 0 = always full-rebake.
// The rebake path is the only one that has ever corrupted memory during a load
// (see the VisualGuard note in rvk_loader.cpp), and with the cache working it is
// almost never taken — which also makes it almost impossible to reproduce. This
// forces it.
int   ps_r_cluster_cache = 1;

// Clustered forward (Forward+, vk_clustered): a compute pass bins the active
// dynamic lights into a 16x9x24 froxel grid; each fragment iterates only the
// few lights touching its cluster instead of all 16 with zero culling. Raises
// the light cap 16 -> 256 AND makes shaded pixels cheaper (the structural gap
// vs R4 deferred). DEFAULT ON (2026-07-04): Release A/B verified identical picture,
// no perf regression, and it unlocks the 256-light cap. r_clustered 0 falls back to
// the exact old per-fragment 16-light loop. v1 covers world (lmap/vlit/terrain) +
// skinned; foliage stays on the 16-light path. r_clustered_debug draws a per-cluster
// light-count heatmap (validates the cull).
int   ps_r_clustered       = 1;
int   ps_r_clustered_debug = 0;

// Framegraph depth-thrash coalescing (vk_barriers ImageState): the prepass depth
// is sampled in a RUN by SSAO -> VRS -> VSM-mark -> VSM-resolve. Each used to do
// its own ATTACHMENT<->SHADER_READ round-trip (4 round-trips = 8 depth barriers
// serializing the GPU). With this ON the depth flips to SHADER_READ once, all
// consumers run, then it flips back once (VSM::RenderAtlas uses its own atlas
// depth, so it rides inside the read window). r_fg_coalesce 0 = per-consumer
// round-trips via the SAME tracker (≈ the old behaviour, marginally stricter/safer
// barriers). DEFAULT ON (2026-07-05): user ran with it in-game, renders correctly.
// Fallback r_fg_coalesce 0 kept as the A/B safety net.
int   ps_r_fg_coalesce     = 1;

// Declared pass dependencies (vk_pass_deps.h): what a violation costs. Passes
// declare the named resources they read/write, and on the first frame the registry
// checks that nothing reads what a LATER pass produces (unless the read is marked
// History). 1 = FATAL, which is the entire point — that mistake shows up as a
// one-frame-stale picture, never as an error, so it has to stop the run. 0 = log
// it and carry on: the escape hatch for when the DECLARATION is the wrong half and
// you need the game up to fix it. `vk_pass_deps` prints the whole table.
int   ps_r_fg_validate     = 1;

// Where the INTER-PASS barrier comes from. 0 = the one conservative
// SceneAttachmentBarrier between every pass pair (a global memory barrier over
// colour+depth, what shipped before the declarations existed). 1 = DERIVED from
// the declarations: each pass Requires only the scene attachments it declared,
// through the same ImageState tracker r_fg_coalesce uses — so the compute-only
// head of the frame (PreSkin/DDGI/SunShadow) gets no barrier at all and the
// compositing tail's dozen depth READS coalesce to one transition instead of a
// dozen dependencies on writes that stopped at the LODs pass, and the two passes
// that SAMPLE the depth (water, light cones) share one flip instead of
// round-tripping twice each. 2 = 1 plus a one-shot audit line + the table.
// DEFAULT 0, deliberately: derivation REMOVES synchronization, and a missing
// barrier is a race that does not reproduce on demand. The structural blocker is
// gone (every pass now moves the scene attachments through g_FrameGraph, so the
// tracked layout cannot lie), so what is left is measurement: flip to 2 with
// -vk_validation for one in-level run, read the numbers, then default it on.
int   ps_r_fg_barriers     = 0;

// Запись пассов во ВТОРИЧНЫЕ командные буферы (vk_pass_secondary).
// 0 = как было: все пятнадцать пассов пишут в один primary, строго
//     последовательно -- это 1.0-1.4 мс, крупнейшая статья CPU-кадра.
// 1 = каждый пасс пишет в СВОЙ вторичный буфер, primary проигрывает их по
//     порядку через vkCmdExecuteCommands. Запись пока на главном потоке:
//     этап вводит механизм и стоимость самих вторичных буферов, БЕЗ гонок.
// ⚠Держать 0 по умолчанию, пока не пройден прогон с -vk_validation.
int   ps_r_mt_record       = 0;

// Логирование Streamline (vk_sl.cpp). 0 = eOff (тишина в логе), 1 = eVerbose.
// ⛔НЕ рычаг производительности: поток `sl.log` жжёт 0.13 core-ms/кадр и при
// eOff, и при eDefault (A/B 18-09) — он занят не логированием.
// ⚠Читается ОДИН раз, при slInit — смена на живом уровне ничего не даёт.
int   ps_r_sl_log          = 0;

// Async compute (vk_async): record compute work into a SECOND command buffer on the
// dedicated compute queue, ordered to graphics via a timeline semaphore, so compute
// overlaps graphics raster. INCREMENT 1 = machinery + INERT probe (empty compute
// submit the graphics frame waits on) to prove the cross-queue path is stable on
// this GPU before real passes move over. Default 0 (dormant = exact single-queue
// path). Needs a dedicated compute family (else no-op). See vk_async.h.
int   ps_r_async           = 0;

// Sky specular IBL (vk_ibl): prefiltered sky-cube reflections (roughness mips) +
// a real sun GGX glint on the forward surfaces (the world sun path was diffuse-
// only). r_ibl_spec scales it, r_ibl_debug = spec field only.
//
// ⛔ DEFAULT OFF SINCE 2026-07-25 — PARKED, NOT SHIPPABLE ON THIS CONTENT. Do not
// flip this back to 1 without first fixing everything below; the machinery is sound,
// the INPUTS are not, and the result reads as a wax coating over the whole world.
//
// The blocker is that a specular term needs ROUGHNESS per material and this content
// has none. Measured, not assumed (all 118 <bump>.dds in the install, gloss = the R
// channel): p50 0.031 / p90 0.094 / p95 0.161 / p99 0.322. X-Ray authored that channel
// for R2's far weaker specular, so it sits in the bottom sixth of [0,1] and carries
// almost no signal. Every consumer therefore INVENTS a roughness, and they disagree:
//
//   statics  world_lmap/vlit_frag_body  0.85 - gloss*scale*0.75  -> in practice 0.78-0.85
//   NPCs     skinned.frag               0.6 flat  (cloth, leather, skin, metal alike)
//   trees    tree.frag                  mix(0.7, 0.5, wetF)
//   terrain  world_terrain.frag         its own
//
// F0 is 0.04 everywhere, so metal does not exist as a material either — the whole
// world is one dielectric with a uniform 4% sheen. User-visible verdict (25-07): a
// fence matte and the fence a metre away glossy, tree trunks glossy, NPCs "как воском
// облитые". Turning r_ibl_spec down does not rescue it: sheen reads as CONTRAST against
// a dark albedo, so it stays visible until the knob hits exactly 0.
//
// Two further defects found while diagnosing, both still unfixed:
//   * env_common.glsl:200 — sunSpec multiplies by ibl_params.x but NOT .y, so
//     r_ibl_spec silently does not scale the SUN highlight. The knob lies about its own
//     scope, which is why dialling it read as "no effect".
//   * skinned.frag:518 — NPCs gate the reflection by pc.hemi, a PER-OBJECT scalar,
//     while statics gate per-pixel by sOcc*skyVis. An NPC reflects sky off the side
//     that is pressed against a wall.
//
// Reviving this needs material CLASSES, which do exist even though roughness maps do
// not: the texture path already encodes them (mtl\ wood\ crete\ ston\ glas\ grnd\ act\)
// and is read at material load. A prefix->{roughness, F0} table feeding ALL FOUR paths
// from one place, plus the same sky-visibility gate everywhere, is the real fix.
// Cost of leaving it off is zero: the prefilter is gated by `if (ps_r_ibl ||
// ps_r_sky_sh)` in vk_env_light.cpp:1001, so nothing is computed for a disabled feature.
int   ps_r_ibl             = 0;
float ps_r_ibl_spec        = 1.0f;
int   ps_r_ibl_debug       = 0;

// Diffuse sky irradiance via SH9 (vk_ibl + sky_sh_project.comp). The sky ambient
// used to be ONE texel of the weather cube fetched along the surface normal at LOD
// 0 — a point sample of a sharp skybox, not an irradiance integral. Three failures
// compounded: it could not be blurred (the cubes ship single-mip BC), so terrain and
// world had to feed it the FLAT geometric normal or the fill turned into a mirror,
// killing all relief; and because X-Ray cubes are authored in the half-cube space
// (horizon at the bottom edge), an up-facing normal always landed on the zenith
// texel — the warm horizon band that carries nearly all the energy at dusk was
// unreachable. Net effect at sunset: a beautifully graded sky over uniformly-lit
// ground, which is exactly what it looked like.
//
// The fix projects the world-space probe onto 9 SH coefficients once per weather
// change (~6k texel fetches, riding the prefilter's existing immediate submit) and
// evaluates them per pixel in a few MADs. Being a real cosine-weighted integral it
// is directional in azimuth for free, and being smooth by construction it finally
// lets the DETAIL normal drive ambient. r_sky_sh 0 = A/B (falls back to the
// prefiltered probe's roughest mip, which is still azimuth-correct).
//
// r_sky_sh_ground scales the below-horizon hemisphere during projection: the sky
// cube's skirt is not sky radiance, it is where the ground is, so taking it at full
// strength would light everything from underneath. ~0.3 reads as a plausible ground
// bounce (it is still the right colour to bounce — the ground is lit by this sky).
int   ps_r_sky_sh          = 1;
float ps_r_sky_sh_ground   = 0.3f;
// r_sky_sh_debug — dump each projection's coefficients: L0 (omnidirectional level),
// L1 (the linear band: how much irradiance VARIES with direction, and toward what),
// and aniso = |L1|/L0. This is the measurement that separates "the maths downstream
// is wrong" from "the sky the probe sees has no direction in it" — if aniso is near
// zero at dusk, no receiver-side fix can make terrain directional, and the missing
// energy is elsewhere (e.g. the sun disk/aureole, which sky.frag adds ADDITIVELY at
// draw time and is therefore absent from the cubemap the probe is built from).
int   ps_r_sky_sh_debug    = 0;

// PROCEDURAL SKY (r_sky_proc) — single-scattering Rayleigh+Mie, atmosphere.glsl.
//
// Measured root cause (2026-07-20): at a late sunset the weather config supplies
// sun_color=(0.009,0.004,0.002), hemi/ambient/sky_color all NEUTRAL GREY, and the sun
// zeroed at elevation +1.4° (X-Ray does this because the classic renderer could not do
// twilight). So dusk had no warm light and no directional light anywhere in the data,
// and the world rendered uniformly grey — faithfully. No receiver-side fix could reach
// it; the light itself was missing.
//
// This derives sky radiance from GEOMETRY (sun elevation) instead of authored colours.
// Sunset then emerges from physics: the low sun's path through air is ~40x longer,
// Rayleigh strips the blue out of the beam (leaving red), and the Mie forward lobe
// wraps a warm aureole around it while the anti-solar sky stays blue — real azimuthal
// structure, which is exactly what the SH probe measured as absent.
//
// The SAME function feeds the dome AND the light probe (ibl_prefilter), so "the sky
// you see" and "the light you get" cannot drift apart. r_sky_proc 0 = legacy cubemap.
int   ps_r_sky_proc        = 0;      // default OFF until verified — big look change
float ps_r_sky_intensity   = 22.0f;  // maps the model's physical units onto game exposure
float ps_r_sky_turbidity   = 1.0f;   // aerosol multiplier: 1 = clean air, >1 = hazy/dusty
float ps_r_sky_mie_g       = 0.76f;  // Mie anisotropy — tightness of the sun's aureole
// Drive the DIRECTIONAL sun colour from the same atmosphere model instead of the
// config's sun_color. This is what actually re-lights dusk: the config says 0.009,
// physics says "warm and low". Needs r_sky_proc.
int   ps_r_sky_sun_from_atmo = 1;
float ps_r_sky_sun_scale     = 4.0f; // sun irradiance scale (transmittance is 0..1)

// VOLUMETRIC CLOUDS (r_clouds_vol) — raymarched 3D medium, clouds.glsl + vk_clouds.
//
// Replaces BOTH cloud sources at once: the ones painted into the weather cubemap and
// the flat scrolling R4 cloud dome. The painted ones need no suppression heuristic
// (which is how this was originally planned) because the procedural sky never samples
// that cube; and the scrolling dome is skipped in the shader when this is on — one
// kind of cloud, never two.
//
// Lit by the SAME atmosphere model that draws the sky, which is the entire point: at
// dusk the reddened low sun lights cloud UNDERSIDES warm while their tops stay cool
// from the blue zenith, and thin edges glow through the forward scattering lobe.
// Authored cloud colours (which measure neutral grey at that hour) cannot do this.
int   ps_r_clouds_vol           = 0;      // default OFF — opt-in, and the most expensive effect here
float ps_r_clouds_coverage      = 0.55f;  // 0 clear .. 1 overcast (also the shader's off switch)
float ps_r_clouds_density       = 1.0f;
float ps_r_clouds_detail        = 0.35f;  // edge erosion strength
float ps_r_clouds_bottom        = 1500.f; // deck base (m)
float ps_r_clouds_top           = 6000.f; // deck top (m) — the band the type gradient spans
float ps_r_clouds_shape_scale   = 0.00008f;
float ps_r_clouds_detail_scale  = 0.0008f;
float ps_r_clouds_weather_scale = 0.000012f;
float ps_r_clouds_wind_dir      = 45.f;   // heading (deg)
float ps_r_clouds_wind_speed    = 12.f;   // m/s
float ps_r_clouds_phase_g       = 0.72f;  // forward lobe (silver lining toward the sun)
float ps_r_clouds_phase_g_back  = 0.35f;  // back lobe (glow with the sun behind you)
float ps_r_clouds_extinction    = 0.08f;
float ps_r_clouds_powder        = 0.7f;   // dark-edge term; 0 = pure Beer (edges wrongly bright)
float ps_r_clouds_sun           = 12.f;
float ps_r_clouds_ambient       = 1.0f;
int   ps_r_clouds_steps         = 48;     // primary march budget — THE perf knob (was 96: unaffordable at fullscreen sky)
float ps_r_clouds_max_dist      = 30000.f;// marched chord cap. 90k made low-elevation steps ~1 km — clouds fell between samples
float ps_r_clouds_cirrus        = 0.5f;   // high 2D ice layer: the parallax that reads as "different heights"
float ps_r_clouds_cirrus_alt    = 9000.f;
float ps_r_clouds_cirrus_scale  = 0.00004f;
// r_clouds_debug — show the marched coverage unlit: 1 = raw alpha, 2 = alpha x20.
// The question a blank-looking sky raises is whether the model produces ANY density,
// and guessing at that has already cost round trips. This answers it directly.
int   ps_r_clouds_debug         = 0;
// r_clouds_weather — drive coverage from the weather's own cloud weight
// (clouds_color.w) instead of the flat cvar. Without it an hour the mod authored as
// solid overcast renders as the same scattered puffs as a clear one, because the
// procedural deck never looked at the weather at all.
int   ps_r_clouds_weather       = 1;

// Hi-Z occlusion cull for the GPU-driven static color pass (vk_world_gpu, Phase A
// of cluster cull). Pass_World builds a depth pyramid from this frame's PREPASS
// depth, then a compute pass frustum+occlusion-tests the static set into a second
// indirect buffer drawn only in the (heavy forward) color pass — meshes fully
// behind nearer geometry skip shading. The depth prepass still draws the full
// frustum set (builds the pyramid), so this never over-culls. Needs r_gpu_world 1.
// No effect if the occlusion pipeline failed to build.
// Default ON since 17-07: the footprint math got its missing focal scale
// (lodParams.w) — the old understated footprint picked too fine a mip and
// false-culled (vanishing walls/terrain tiles, why this stayed situational).
// Measured on Pripyat: color-pass draws 11118 → 3704 (×3 fewer). 0 to A/B.
int   ps_r_hzb_cull = 1;

// TWO-PHASE occlusion (world_cull_two.comp): occlusion-cull the DEPTH PREPASS as
// well, by testing against LAST frame's pyramid and then re-testing only what that
// rejected against a pyramid rebuilt mid-prepass. Correctness does not rest on the
// previous frame being right — every early reject is re-tested — so a wrong guess
// costs culls, never holes.
// ⚠ SUPERSEDES r_hzb_cull, does not stack with it: the two phases together produce
// the colour set as well (prepass = EARLY u LATE = colour), so CullColor's separate
// scan of all 830k entries — 66 MB read to emit ~5k draws, and 6.6 ms of it once
// VRAM ran short — is not run at all. With this on, r_hzb_cull does nothing.
// ⚠ Ignored while r_compose clones are active (they overwrite the EARLY set); the
// renderer says so in the log rather than going quietly inert.
// Default 1 since 26-08. In a healthy frame both scans cost ~0.46 ms, so this
// buys only ~0.2 ms; the real value is halving the meta-scan traffic, which
// halves the damage when buffer eviction strikes (the famous 13.6 -> 6.65 A/B
// was measured INSIDE that pathology). Counters match the single-phase set
// exactly (early=36921, late=0); neutral on the 780M.
int   ps_r_hzb_prepass = 1;
int   ps_r_world_bindless = 1;      // opaque GPU-world statics in material-agnostic groups on the bindless twins (latched at level load)
// ⭐МАТРИЦА ИЗ ЦИКЛА ПО ГРУППАМ (1 = вынесена, дефолт; 0 = как было, для A/B).
// DrawColor всегда пушил mvp только при смене layout, а DrawDepth/DrawShadow --
// НА КАЖДУЮ ИЗ 639 ГРУПП (64 байта solid, 80 байт AT). Матрица в цикле одна и та
// же, меняется только aref, поэтому солидным группам вызов не нужен вовсе, а
// AT-группам хватает 4 байт по смещению 72. Пуш-константы живут до привязки
// конвейера с НЕСОВМЕСТИМЫМ layout -- отсюда ключ «пушено для этого layout».
// ⛔📏ИЗМЕРЕНО 19-09, ДВА ПРОГОНА (Кордон, fpslock60, фазы по 20 с, в т.ч. с
// `r_shadow_casc_cache 0`, чтобы теневой цикл крутился каждый кадр): РАЗНИЦЫ НЕТ.
// `World` 0.69-0.72 против 0.70-0.77, `SunShadow` 0.07-0.08 в обоих — при разбросе
// ВНУТРИ одной настройки 0.57-0.79. Убранные ~1300 вызовов на кадр стоят меньше
// порога прибора на этой машине (RTX 5070 Laptop).
// ✅Правка оставлена (а не откачена, как мемо PipelineCache::Get) по трём причинам:
// она делает четыре цикла ОДИНАКОВЫМИ — DrawColor так писал матрицу всегда;
// скрытой опасности у неё нет, в отличие от мемо (проверено -vk_validation:
// ни одного нового VUID, и картинка совпала с контрольной парой — mean|diff|
// 5.55 против контрольных 4.57/5.57); а смысл — Switch, где вызов драйвера дороже.
// ⚠Квар NOSAVE и остаётся именно ради того, чтобы этот A/B можно было повторить
// НА ARM, где и ожидается эффект.
int   ps_r_world_push_hoist = 1;
// ⭐r_bones_mt: CalculateBones всех скелетов кадра — предпроходом по пулу TTAPI,
// до цикла Skinned_UploadBones. 📏Цель — 0.44-0.48 core-ms главного потока
// (третья статья кадра после планировщика и записи World).
// ⚠⚠ДЕФОЛТ 0: тело зовёт костные колбэки и колбэки конца анимации, то есть
// игровой код, безопасность которого чтением НЕ доказывается. Включать под
// прогон с `-vk_validation` и наблюдением за крашем — так же, как этап Б.
int   ps_r_bones_mt = 0;
int   ps_r_wm_async = 1;   // blood decals on NPCs built on a core-2 worker, not waited for (07-10); 0 = the second thread, joined
// CPU particle sim of the visible systems on the TTAPI pool (CRender_Vulkan.cpp), like R4's
// calculate_particles_async. 0 = serial on the main thread (the port as it was).
int   ps_r_particles_mt = 2;   // 2 = on the core-2 worker WHILE the main thread collects the scene (07-10)
// r_particles_core: the core of that worker. 📏11-10 (gc11, sampled fights x2): core 1 instead of 2 changed nothing --
// the main thread drained 2.5% of its time either way, the fight fps the same. Kept for experiments.
int   ps_r_particles_core = 2;
// Hardware pipeline-statistics on World/Depth and World/Statics: IA prims, VS/FS
// invocations, clipped prims and the two tessellation counters. Answers "is this
// zone geometry-bound or fragment-bound", which frame time alone cannot. 0 = off.
int   ps_r_world_geom_stats = 0;
// GPU-driven LOD imposters (vk_LODManager): compute cull (distance + frustum +
// shared HZB) + vertex-pulling draw replace the CPU per-frame FLOD walk that
// cost 6-9.4 ms on Pripyat. 0 = the original CPU path (instant A/B, no rebuild).
int   ps_r_lods_gpu = 1;

// Froxel volumetric lighting (vk_volumetrics, r_vol) — P1. A 3D froxel grid over
// the frustum: compute injects sun in-scatter (Henyey-Greenstein phase × cascade
// sun-shadow) + height/base fog, integrates it front-to-back, and the tonemap
// composites scene*transmittance + in-scatter (HDR, pre-tonemap). = god rays
// through geometry + depth fog (the Metro base look). Default OFF. r_vol_height 0
// = uniform fog; r_vol_debug shows the raw integrated in-scatter pattern.
// ⚠ The five numbers below were RE-TUNED FROM SCRATCH on 2026-07-24, in-game, after
// the fog finally received the sky hemisphere instead of the ambient floor (see
// r_vol_ambient). Every value tuned before that was compensating for air that could
// not scatter sky light at all, so the old set is not a starting point — it is the
// shape of the bug. Do not "restore" them.
int   ps_r_vol           = 1;       // ON by default — shipped feature (god-rays + depth fog + indoor haze), ~1.5ms
float ps_r_vol_density   = 0.005f;  // DUST layer extinction (V-1 layer 1): thin "volume in the air", NOT a fog bank — see r_vol_mist for that
float ps_r_vol_height    = 0.40f;   // dust height falloff above the BAKED GROUND (0 = uniform)
float ps_r_vol_g         = 0.88f;   // Henyey-Greenstein anisotropy (forward scatter) — user-verified: "makes the rays brighter near the ground"
float ps_r_vol_intensity = 3.0f;    // in-scatter brightness multiplier (raised: fog must GLOW more than it dims to read as haze)
float ps_r_vol_amb       = 0.60f;   // indoor ambient floor: fraction of sky ambient kept under a roof (0=pitch-dark interior, 1=no occlusion)
// Fog AMBIENT (sky-fill) in-scatter tint scale. The fog's ambient term used to
// inherit the SURFACE receiver floor (r_ambient_floor) — a flat white lift that
// keeps lit geometry off pure black. In the fog that neutral floor integrates
// along the sightline into a whitish haze that LIFTS the whole night scene (the
// air self-glows even with the sun down). The fog now tracks the env's real
// ambient (dark/blue at night) scaled by this instead: 1.0 = env ambient as-is,
// lower = darker night air, 0 = no ambient fog (sun beam + local lights only).
// Scale for the fog's SKY in-scatter. Rescaled with the source: the term used to be
// the ambient FLOOR (~0.02 in a measured frame) and now carries the real hemisphere
// (~0.42), i.e. ~20x more light, so the old scale would blow the fog out. Anything
// tuned before that change (a saved 4.0, say) must be re-tuned around this default.
float ps_r_vol_ambient   = 0.15f;
// Indoor density boost, ×(1+this) under a roof, applied to the ambient term only.
// It existed because the fog's sky term was the ~0.02 ambient floor: indoors that
// was nothing at all, so the air needed a x7 crutch to show up. With the real
// hemisphere feeding it the crutch over-fogs every room — re-tuned to 0 in-game.
// Kept as a knob (weather with heavy interior haze may want it back).
float ps_r_vol_indoor    = 0.0f;
float ps_r_vol_sun       = 6.0f;    // sun-beam in-scatter boost: directional shaft brightness (pops the god-ray through the ambient haze)

// Atmospheric scattering (r_atmo): physical Rayleigh (blue distance) + Mie (warm
// sun halo) in-scatter in the froxel fog = aerial perspective. NEEDS r_vol on.
// DEFAULT ON (2026-07-05): verified in-game ("вроде прикольно", nice lightning
// interaction). Sun-relative → dawn/dusk warm. r_atmo_rayleigh/_mie tune it live.
int   ps_r_atmo          = 1;
float ps_r_atmo_rayleigh = 2.0f;    // blue-distance strength
float ps_r_atmo_mie      = 0.6f;    // warm sun-halo strength
float ps_r_atmo_mie_g    = 0.76f;   // Mie forward anisotropy

// Auto-exposure TEMPORAL adaptation (eye adaptation): time constant in seconds for
// the exposure to ease toward the metered target. 0 = instant (old snappy behaviour
// where the image visibly darkens/brightens as you tilt between sky and ground).
// ⚠SPLIT SINCE 27-08: this one now governs only exposure going DOWN (the image
// darkening — turning towards a window/the sky), r_exp_adapt_up governs it going
// UP. The eye adapts to darkness far slower than to light, and the asymmetry is
// also what defuses the wall-by-the-window creep: the slow direction is exactly
// the one where the meter is walking the wall up to the clamp ceiling, so the
// creep becomes a lazy drift the next pan cancels quickly.
float ps_r_exp_adapt     = 1.0f;
float ps_r_exp_adapt_up  = 2.5f;    // seconds, exposure RISING (image brightening); pairs with r_exp_adapt
// Auto-exposure user knobs (vk_exposure.h reads these; tonemap + bloom share them).
// r_expo: EV-style compensation multiplier on the metered exposure (1 = neutral).
// r_expo_gray: metering target override; 0 = auto (0.58 gamma / 0.18 linear).
// r_expo_min/_max: exposure clamp range (night floor / brightening ceiling).
float ps_r_expo          = 1.0f;
float ps_r_expo_gray     = 0.0f;
float ps_r_expo_min      = 0.80f;
float ps_r_expo_max      = 2.20f;
// METERING ESTIMATOR. The old meter was the 1×1 top mip — the
// ARITHMETIC mean of the whole frame — and an arithmetic mean belongs to its
// outliers: a bright window in 2% of a dark room's pixels moves it severalfold,
// so crossing a doorway (or panning between walls of different brightness)
// swings the target and the whole image ramps for r_exp_adapt seconds. That was
// the user's "exposure jumps between rooms" (27-08); the same defect outdoors is
// the sky-tilt breathing the clamp already bounds. The right moves were named in
// the exposure note the day the clamp was diagnosed: log-average, de-weight the
// outliers — this is that fix: a log-average (geometric mean) over the mip 4
// below the top (16..31 texels a side, a few-KB readback), blended with the
// arithmetic mean by r_expo_meter_mix — an outlier counts by its share of the
// frame, not by its magnitude.
int   ps_r_expo_interval = 1;  // build the scene mip chain + meter only every Nth frame (Switch: the chain feeds nothing else)
// HOW MUCH THE METER IS ALLOWED TO IGNORE (r_expo_meter_mix, mode 1 only).
// Pure log-average turned out to be the OPPOSITE failure, seen in-game the same
// evening it shipped: stand by a window with a dark wall filling the frame and
// the meter — deliberately discounting the small bright patch — exposes for the
// WALL, which creeps up to the r_expo_max ceiling ("стена высветляется"); pan
// until the window grows in frame and the exposure "snaps back to normal". The
// user preferred the OLD meter's look ("намного лучше"), just darker than they
// want. Both are the same dial: how strongly a small bright region pulls the
// exposure down. So mode 1 is now a LOG-SPACE BLEND between the two estimators:
//   0.0 = the arithmetic mean (old look: window counts in full, interiors dark,
//         doorway crossings jumpy),
//   1.0 = the log-average (window ignored: interiors bright, walls wash out
//         near a window).
// 0.3 keeps most of the window's authority (no wall washout) while capping its
// grip enough that interiors sit brighter and doorway swings shrink — the swing
// scales as (old swing)^(1-mix). Dial by eye in-game; the liked value gets
// baked here. NoSave, same reason as the mode.
float ps_r_expo_meter_mix = 0.3f;
int   ps_r_expo_debug    = 0;       // 1 = log the estimators + applied exposure ~once a second
// ⭐THE METER KNOWS WHERE IT IS (06-09-2026, see CameraEnclosure in vk_pass_tonemap).
// Five CPU rays up from the camera against the level cform say "roof"; under one
// the estimate slides to the median (the window stops voting: an honest interior
// is 30-40x darker than its window and any meter that lets the window vote pins
// the room at r_expo_min) and the ceiling rises by r_expo_indoor_max. In the open
// both are identity, so the street keeps the outdoor meter look bit for bit.
// The user picked exactly this pair by hand in a house on 27-08 (median + max 3
// = "намного лучше"); this makes it automatic and leaves the street alone.
// NoSave both: tuning knobs, and a stuck one would poison the next A/B.
float ps_r_expo_indoor_max  = 2.0f;   // r_expo_max x this when enclosed (2.2 -> 4.4 by default; the user's 3 -> 6)
float ps_r_expo_indoor_dist = 6.0f;   // metres up that count as a roof (camera ~1.7 m + a 3 m storey + slack)
float ps_r_vol_lights    = 2.5f;    // P2: local light (flashlight/lamp/campfire) in-scatter in fog — glow/cone strength; 0 = off
// Forward-scatter anisotropy for LOCAL lights only (torches/lamps/campfires),
// SEPARATE from the sun's r_vol_g (0.80). The sun's sharp forward peak is the
// god-ray halo; reusing it for local lights made a torch shone AT the camera
// spike into a blinding, HDR-desaturated (cold-white) glare — an NPC head-lamp
// read as a searchlight, not the soft R4 shaft. 0.35 = gentle bias (R4-like).
float ps_r_vol_lights_g  = 0.35f;
// Fog in-scatter multiplier for HANDHELD/WORN torches only (CTorch head-lamp,
// weapon light) — kept SEPARATE from fixtures (hanging lamps, searchlights,
// campfires) which keep the ×3 lamp beam boost. In practice this is the NPC
// head-lamp knob: the actor's own torch (volumetric_for_actor) and weapon lights
// (volumetric_enabled) default OFF, so only AI head-lamps scatter in the fog. A
// group of standing NPCs was flooding the air with light (scene read over-bright),
// so the shaft is dialed well below a fixture's: 0.4 = restrained R4 haze. Raise
// for a punchier beam; 0 = torches light surfaces but cast no fog shaft.
float ps_r_torch_vol     = 0.4f;
float ps_r_vol_smoke     = 1.0f;    // Stage-0: light smoke billboards with the froxel in-scatter (sun shaft/flashlight/campfire catch the smoke); 0 = off (old flat look)
float ps_r_vol_smoke_clamp = 6.0f;  // Stage-0: upper bound on the per-froxel radiance added to smoke (keeps it from blowing to white next to a campfire/sun beam)
float ps_r_vol_smoke_inject  = 1.0f; // Stage-1 VMS: inject smoke-particle density into the froxel grid → smoke becomes real participating media (lit/shadowed like fog); 0 = off
float ps_r_vol_smoke_density = 1.0f; // Stage-1 VMS: density mass per particle opacity (splat scale) — thicker/thinner injected smoke
int   ps_r_vol_smoke_debug   = 0;    // VMS debug: 0 off / 1 smoke colour glow (albedo) / 2 density heatmap (grey) / 3 self-shadow sunVis (blue=shadowed, warm=sun-lit)
int   ps_r_ptcl_mt           = 0;    // 12-10-2026 Switch: 1 = particle billboards filled by a core-1 worker while the main thread does the other prepares (vk_pass_particles.cpp). OFF: core 1 is not free then (render thread 0x2D records there, the second thread runs there too) -- at 0x2E the worker hardly started, at 0x2C it took the time from the second thread (wait_2nd +1.3 ms, fps 57.5 -> 56.5)
int   ps_r_core1_prio        = 0x2E; // 12-10-2026 Switch: priority of the core-1 workers the main thread waits for (bones r_bones_mt 2, particles r_ptcl_mt). 0x2C (above the render thread) cut submit 2.3 -> 1.4 ms but the second thread, on core 1 too, lost it: wait_2nd +1.3 ms, fps 57.5 -> 56.5 (12 bandits). Kept 0x2E
float ps_r_bones_lod_dist    = 0.f;  // 12-10-2026 Switch: world skeletons farther than this (m) get their bones every other frame, 30 Hz (vk_pass_skinned.cpp BonesLodSkip); 0 = off
float ps_r_vol_smoke_dist_full = 12.0f; // Stage-1 VMS LOD: radius (m) of FULL-quality volumetric smoke (close-up "beautiful" body); past it density fades to billboard by r_vol_smoke_dist.
float ps_r_vol_smoke_dist    = 40.0f; // Stage-1 VMS: max distance (m) smoke injects as volumetric media (tied to the terrain detail bubble — capped at r__detail_radius); beyond it the billboard alone represents it (far froxels = coarse muddy blobs). Fades from dist_full → here.
float ps_r_vol_smoke_footprint = 1.0f; // Stage-1.1 VMS: max smoke splat footprint in froxel cells (0 = point splat / cheapest; 1 = 3³ gaussian blob; 2 = 5³). Soft + denser; cost ~ particles × (2·fp+1)³ — watch the VolSmoke profiler zone.
float ps_r_vol_smoke_shadow  = 1.0f; // Stage-2 VMS: smoke SELF-SHADOW strength (cloud sun-side bright, far/deep side dark — gives it volume). 0 = off (flat lit). Only smoky froxels march toward the sun.
float ps_r_vol_smoke_shadow_step = 0.6f; // Stage-2 VMS: self-shadow sun-march step (world m); 6 steps total. Bigger = longer reach/softer, smaller = tighter.
float ps_r_vol_noise     = 0.55f;   // P3: animated 3D noise on the fog density → drifting dust/mist ("living air"); 0 = off
float ps_r_vol_noise_scale = 0.40f; // P3 noise frequency (world units; higher = finer motes)
float ps_r_vol_noise_speed = 0.10f; // P3 drift speed of the dust
float ps_r_vol_soft      = 2.5f;    // cascade shadow PCF blur radius (texels): soft penumbra in fog so the cache TICK (sun creep through foliage) barely shows; 0.5 = crisp
// r_fog_dist — range scale for the FORWARD distance fog (the legacy R4 haze in the
// world shaders: colour = mix(colour, fog_color, dist·k)). It is a THIRD fog on top
// of the froxel volume and the Rayleigh/Mie atmosphere, it is purely distance-based
// (no height profile — it cannot be anchored to the ground, so it always "follows"
// the camera), and it had no cvar at all: every r_vol_*/r_atmo experiment left it
// untouched. 1 = weather value, >1 pushes the haze back, 0 = off (A/B).
// Default 4 since 25-08, paired with rs_vis_distance 8: the far plane now draws ~3.2 km,
// and drawn world that arrives painted in fog colour is worth nothing. 4 lifts the haze
// off the distant treeline and city while leaving the true horizon hazy.
// ⚠ NOT scaled to match the far plane: this fog is also the weather's MOOD. Pushed all
// the way out it turns a foggy morning into a clear one, and on a forest point it tinted
// the whole scene — these params feed the atmosphere, not just the distance blend.
// ⚠ This is the THIRD haze in the frame (froxel r_vol + Rayleigh/Mie atmosphere are the
// others) and the only distance-only one, so it is what reads as "fog that follows me".
// With r_vol ON the two stack — nothing gates this one on the volume. Unifying them
// (froxel near, this one past the volume's far edge) is the open work.
float ps_r_fog_dist      = 4.0f;
// r_sky_fog_horizon (10-10, Switch preset): the sky fades to the fog colour below this elevation (sin of the angle, 0 =
// off) -- where r_fog_dist 1 closes the fog at the far plane, the clipped edge of the world is fog_color and has to meet a
// sky of the same colour, or the fogged hills pop as grey silhouettes when the camera turns. Off on the PC (fog_dist 4
// does not reach the far plane there).
float ps_r_sky_fog_horizon = 0.0f;
// r_fog_sky (10-10, Switch preset): 1 = the distance fog fades into the SKY along the view ray (the weather cube x the sky
// tint, the same horizon band; nx_fog.glsl), not into one flat fog_color -- the far edge the plane clip sweeps as the camera
// turns is then the sky behind it, not a grey silhouette popping in. 0 = flat fog_color (the PC default).
int   ps_r_fog_sky = 0;
// r_vol_ground_debug — forensics for the baked terrain height field: the inject
// paints each froxel's height ABOVE THE GROUND (red 0 m → blue 10 m) instead of
// light, with green = 1 where the value came from the map rather than the
// eye-relative fallback. View it with r_vol_debug 1. Terrain-shaped bands that stay
// put while you walk = the anchor works; a flat wash sliding with the camera = the
// map read is failing.
int   ps_r_vol_ground_debug = 0;
// ── V-0: the froxel fog's radiometric foundation ────────────────────────────────
// r_vol_albedo — single-scatter albedo: sigma_s = sigma_t * albedo. It was hardcoded
// to 1, which welded "how brightly this air glows" to "how much of the world it
// hides" — the reason thin air could not carry visible shafts. 1 = old behaviour;
// lower = more absorbing (smoke/dust). The "god rays in clear weather" hybrid wants
// a THIN, HIGH-albedo, strongly forward-scattering medium.
float ps_r_vol_albedo    = 1.0f;
// ── V-1: shape. Where the fog stops being a flat pall and starts being lit air. ──
// r_vol_ms — multiple-scattering octaves (Wrenninge). Single scatter saturates dense
// fog at the source radiance, so shadowed air stays flat and reads as dirty grey:
// the light that would really bounce several times inside the medium is simply
// missing. Each octave halves the scattering weight and the phase eccentricity and
// softens the shadow term, which is what fills shadowed fog with a soft glow.
// Weights are normalised (shape, not gain). 1 = single scatter (old look).
int   ps_r_vol_ms        = 3;
// r_vol_g_back / r_vol_g_mix — the BACKWARD phase lobe. One HG lobe can only do the
// forward halo, so the sun never dominated the ambient veil at other angles and the
// fog looked directionless. Negative g = backward.
float ps_r_vol_g_back    = -0.30f;
float ps_r_vol_g_mix     = 0.25f;
// ── V-1: TWO MEDIA LAYERS (r_vol_mist*) ────────────────────────────────────────
// The froxel medium used to be ONE exponential layer, which cannot be both things
// the scene needs. Tuned thin enough that clear weather stays clear (the in-game
// answer was r_vol_density 0.005, "very light, just some volume for the scene"),
// no gain will ever make a valley read as a fog bank; tuned thick enough for the
// bank, the whole map turns to soup. So a SECOND, independent layer is summed into
// the same sigma_s/sigma_t: dense, ground-hugging, with its own phase. The grid and
// the integrator never learn there are two — cost is a few ALU in the inject.
//   r_vol_density/_height/_g = layer 1 "DUST": thin, tall, sharp forward lobe → the
//     god rays that should be visible even without fog.
//   r_vol_mist*              = layer 2 "MIST": the actual fog bank.
// 0 = off → byte-identical to the single-layer look, so this is a clean A/B.
float ps_r_vol_mist        = 0.0f;   // mist extinction AT GROUND LEVEL (compare: dust is 0.005; a real bank is 0.1-0.5)
float ps_r_vol_mist_h      = 4.0f;   // mist layer thickness in METRES (e-fold above the baked terrain height)
// Mist phase. Dense water droplets scatter far less directionally than thin dust —
// a fog bank glows as a body instead of throwing a razor shaft, and giving it the
// dust's 0.88 forward lobe is exactly what makes cheap volumetrics look like smoke.
float ps_r_vol_mist_g      = 0.55f;
// r_vol_mist_relief — metres of terrain drop over which the mist fades IN. 0 = the
// layer sits everywhere at a uniform thickness (a weather-wide fog). Above 0 the
// inject compares this froxel's baked ground against a ~32 m neighbourhood and
// keeps the mist only where the terrain sits BELOW its surroundings: milk in the
// hollows and riverbeds, clear air on the ridge. Costs 4 height-map taps, and only
// for froxels the layer actually reaches.
float ps_r_vol_mist_relief = 0.0f;
// r_vol_mist_micro — the SMALL scale, which the level-wide field physically cannot
// reach: at 71 cm/texel a wheel rut or a shell crater is averaged into flat ground.
// The RAIN map is 1024² over ±75 m around the player = 14.6 cm/texel, five times
// finer, and it already gets rebuilt as you walk — so the mist can thicken inside
// ruts, ditches and craters near the camera. Strength = how many times denser at
// full dip depth (0 = off); _micro_h = the dip depth in metres at which it saturates
// (0.25 = ankle-deep). ⚠ The rain map stores the TOP-MOST surface, so the shader
// only trusts it where it agrees with the baked ground — under a roof or a canopy
// this term switches itself off rather than floating mist onto the roof.
// r_vol_mist_micro is an ABSOLUTE extinction — the density of the air at the bottom
// of a fully-resolved dip — NOT a multiplier on r_vol_mist. That independence is the
// whole point and was learned in game: as a multiplier, the only way to get visible
// fog into the wheel ruts was a bank so thick the player could not stand in it. It
// also works with r_vol_mist 0, which is a look in its own right: clear air over the
// field, mist lying only in the tracks and ditches.
// _micro_h is the dip depth (m) at which the effect saturates; a wheel rut measures a
// few centimetres, so 0.10 = "a rut counts fully", 0.5 = "only real ditches count".
float ps_r_vol_mist_micro   = 0.0f;
float ps_r_vol_mist_micro_h = 0.10f;
// r_vol_mist_noise — the mist layer's SHAPE. An exponential falloff has a perfectly
// smooth top surface, and at fog-bank density that surface reads as a wall of grey
// standing in the hollow: the layer looks like a solid filled to a level line rather
// than fog lying in the terrain. This warps the layer's HEIGHT with fbm, so its top
// rises and falls by ±(this many) layer thicknesses. Note r_vol_noise cannot do this
// job — V-0 moved that one onto the scattering coefficient on purpose, where it
// shapes light rather than shape. Defaults ON (the layer itself is off by default,
// so r_vol_mist stays the clean A/B) and drifts slowly, like valley air.
float ps_r_vol_mist_noise       = 0.6f;
float ps_r_vol_mist_noise_scale = 0.05f;   // 1/m — ~20 m billows
// ── V-1b: the bank is SCENERY — dense to look at, liveable to stand in ────────────
// A fog bank that reads well from a hilltop (cloud lying at the foot of the slope) is
// unpleasant to walk into: inside it, optical depth accumulates from zero metres and
// every direction saturates to milk within a few steps. Physically correct, and not
// what the fog is here for. So the density the player SWIMS IN is decoupled from the
// density the player LOOKS AT — the bank stays a picture, and stepping into it costs
// visibility without taking it away.
//   r_vol_mist_fade   0 = off (byte-identical to the plain layer)
//                     1 = IMMERSION: thin the whole bank once the CAMERA is in it
//                         (this is the literal request: "walk in and it eases off")
//                     2 = NEAR: thin only the metres around the camera, so the bank
//                         keeps its body further out and you stand in a soft clearing
//                     3 = both (whichever thins more wins)
//   r_vol_mist_inside = what the mist thins DOWN to, as a multiple of the DUST layer
//                       (r_vol_density). 2 = "twice the general haze"; expressing it
//                       against dust rather than as a fraction keeps the knob's
//                       meaning when the bank density changes. 0 = mist vanishes
//                       entirely inside; a value ≥ mist/dust means no thinning at all.
//   r_vol_mist_near   = radius (m) of the mode-2 clearing.
// ⚠ Applies to the BANK only. The micro mist in the ruts (r_vol_mist_micro) is
// deliberately untouched: it is thin already and it is precisely what you are meant
// to keep seeing at your feet.
int   ps_r_vol_mist_fade   = 1;
float ps_r_vol_mist_inside = 2.0f;
float ps_r_vol_mist_near   = 45.0f;
// r_vol_mist_inside_h — the height over which "I am inside the bank" fades out.
// ⚠ NOT the layer thickness, and tying it to that was a real bug: with a 4 m layer,
// stepping onto a 6 m roof (or jumping off anything raised) took the eye "out" of the
// bank, switched the thinning off, and made the world DENSER a metre higher than it
// was below. Whether the camera shares the low ground with the bank is the hollow
// gate's job; this only has to answer "am I above the whole thing", which is a
// question on the scale of tens of metres — a hilltop, not a porch.
float ps_r_vol_mist_inside_h = 25.0f;
// ── GRASS CANOPY in the sun term (r_vol_canopy) ──────────────────────────────────
// The fog's sun in-scatter used to walk straight through a field of grass, and no
// amount of shadow-map work fixes that: grass only casts within ~48 m of the camera,
// at froxel scale its shadow is a stipple that PCF averages back into "half lit", and
// it sways, so what little survives shimmers. A field of grass is not a crowd of
// blades, it is a MEDIUM — so the sun term is attenuated by how much canopy the light
// had to cross (Beer-Lambert along the slant path to the sun), from a field baked
// once per level out of the level's own detail-slot grid. Works at ANY distance,
// never flickers, costs one texture fetch below the canopy line.
//   r_vol_canopy   = extinction per metre of fully-covered canopy (0 = off, clean A/B)
//   r_vol_canopy_h = taste multiplier on the authored model height (the canopy the
//                    light sees is not the blade tip — 1.0 = as modelled)
// ⚠ Composed with the shadow-atlas occlusion via min(), not multiply: inside 48 m
// both describe the SAME grass, and a product would double-darken right where the
// player stands. Verify the baked field FIRST with r_vol_ground_debug 3 + r_vol_debug 1.
float ps_r_vol_canopy   = 1.2f;
float ps_r_vol_canopy_h = 1.0f;
// ── V-2: WEATHER DRIVES THE MEDIUM (r_vol_weather) ───────────────────────────────
// The froxel fog used to read three things from the weather (sun dir, sun colour,
// hemisphere) and take its DENSITY from a cvar, while the forward distance haze is
// weather-driven end to end. Change the weather and one fog thickened while the
// other stood still — two systems disagreeing in the same frame, and no knob could
// reconcile them because they had no shared input. These fields were already in the
// descriptor, unread:
//   r_vol_w_flat    clouds_color.w → blends the PHASE toward isotropic. This is the
//                   one that actually reads as overcast: dimming is eaten by
//                   auto-exposure (the r_vol_sun 12 lesson), flattening the lobe is a
//                   change in SHAPE — the shafts dissolve into an even glow.
//   r_vol_w_clouds  …plus a modest damp of the sun term, r_vol_w_sky lifts the sky
//                   term to match. ⚠ The weather's own sun_color already dims in
//                   overcast profiles, so both stack with it — keep them modest.
//   r_vol_w_fog_dust  fog_density scales the DUST layer. ⚠ Read this before touching
//                   the two knobs above: fog_density is NOT "how much ground fog", it
//                   is where the forward haze STARTS (fog_near = (1-density)*0.85*
//                   fog_distance). Its honest counterpart is the thin tall layer,
//                   which is itself the distance haze — and matching those two is the
//                   whole reason V-2 exists.
//   r_vol_w_fog     extra MIST from fog_density. DEFAULT 0, on purpose: hanging the
//                   ground bank on that parameter shipped soup. A dry, hazy morning
//                   sits at fog_density 0.90, which doubled the bank AND lifted it out
//                   of the hollows — a fog day invented out of a number that never
//                   claimed one. Raise it only if a weather set really does mean
//                   ground fog by it.
//   r_vol_w_rain    rain/wetness ADD to the bank — THIS is the bank's weather driver,
//                   because ground fog is about moisture, not about sight distance.
//                   Added, not replaced: a dry level keeps the hand-tuned look.
// The hollow gate (r_vol_mist_relief) is relaxed in proportion to the WEATHER'S SHARE
// of the bank: only fog the rain grew ignores the terrain, so a dry level keeps its
// valley pattern exactly and a wet one climbs onto the ridges.
// Transitions are smoothed by an accumulator (~45 s soak / ~3 min dry) — weather
// changes are ramps, not steps. r_vol_weather 2 logs the inputs once a second, which
// is the fast way to see whether THIS weather even carries them.
int   ps_r_vol_weather    = 1;
float ps_r_vol_w_clouds   = 0.5f;
float ps_r_vol_w_sky      = 0.5f;
float ps_r_vol_w_flat     = 0.7f;
float ps_r_vol_w_fog      = 0.0f;
float ps_r_vol_w_fog_dust = 0.5f;
float ps_r_vol_w_rain     = 0.15f;
// r_vol_w_wet_force — test hook. The rain-driven bank is the half of V-2 a dry level
// can never show, and "wait for the weather script to bring rain" is a bad way to
// verify a feature. -1 = use the real weather; 0..1 = pretend that wetness.
float ps_r_vol_w_wet_force = -1.0f;
// r_vol_term — isolate ONE in-scatter source in the froxel fog: 1 sun beam, 2 sky
// ambient, 3 local lights, 4 atmosphere (Rayleigh/Mie), 5 smoke media, 0 = normal.
// The fog is a sum of five sources and tuning it blind means pushing on whichever
// one happens to be quiet: r_vol_sun x4 and two phase-model upgrades all landed on
// the SUN term and were invisible away from the sun, which says something else is
// painting the picture. This answers which, in one look.
int   ps_r_vol_term      = 0;
// r_vol_upsample — reconstruction filter for the froxel volume in the composite.
// The grid is 256×144, i.e. ~7-8 screen pixels per froxel; the old composite took ONE
// trilinear tap with a screen-STATIC dither, so the volume's own resolution showed
// through as blocks (obvious with r_vol_ta 0) and the temporal pass had to hide it.
// 1 = rotated 4-tap tent over ±0.5 froxel (3 extra 3D taps, ~0.05 ms at 1080p); the
// pattern also rotates per frame WHEN an upscaler resolves, turning DLSS into free
// supersampling of the volume instead of a preserver of fixed grain. 0 = old path.
int   ps_r_vol_upsample  = 1;
int   ps_r_vol_ta        = 1;       // temporal accumulation (jitter + reproject prev frame): smooths the froxel grid → clean dense fog
float ps_r_vol_ta_blend  = 0.92f;   // history weight (EMA): higher = smoother but more ghosting on motion
// (r_vol_shadow and its dedicated per-frame fog sun-shadow map were REMOVED
// 12-08-2026, per the TODO that stood here: the "trembling shafts" it was built for
// were the temporal reprojection, fixed in vol_inject.)
int   ps_r_vol_debug     = 0;       // view the raw integrated in-scatter

// Dynamic-light terrain/static occlusion (vk_shadow ground-height map + a per-light
// height-march in the forward shaders). Stops un-shadowed point lights from
// lighting through the ground/walls — a basement lamp no longer lights the earth +
// fence overhead. Scales O(1) per light to ANY light count (unlike per-light shadow
// maps). Needs the top-down ground map rendered every frame (cached on camera move).
// Default ON; r_light_occ 0 = off (A/B / if the map redraw is too costly).
int   ps_r_light_occ       = 1;

// World heightmap tessellation (R4 TESS_HM port, live): bump-mapped statics
// displace along the normal by the `<bump>#` alpha height near the camera.
// r_tess 0 routes everything back to the flat pipelines. max = subdivision
// factor at point-blank; near/far = distance band over which the factor (and
// the displacement amplitude) fades to flat; height scales R4's 0.07 m
// amplitude. Consumed by RenderQueue::Flush (vk_render_queue.cpp).
// Default OFF: vertex tessellation inflates on this content (the `#` alpha is
// unsigned parallax/error-height, not a centered displacement map). POM
// (r_pom) is the relief mechanism for now; re-enable r_tess once the "done
// right" roadmap lands (high-pass centered height + tess depth-prepass).
float ps_r_tess        = 0.0f;
float ps_r_tess_max    = 8.0f;
float ps_r_tess_near   = 3.0f;
float ps_r_tess_far    = 18.0f;
float ps_r_tess_height = 1.0f;
// Parallax occlusion mapping (per-pixel brick/cobble relief — the right tool
// for un-authored content, vs vertex tessellation). Reads the same `<bump>#`
// height (alpha) as tess but per-pixel: carves grooves, never inflates. Only
// world lmap/vlit materials with a real `#` height (flat-bump = no-op).
// height = UV-space march amplitude; steps = max ray samples; far = fade dist.
int   ps_r_pom        = 1;
float ps_r_pom_height = 0.02f;
float ps_r_pom_steps  = 24.0f;
float ps_r_pom_far    = 30.0f;
// Extra mip blur on the POM heightfield: removes fine albedo speckle (which
// the diffuse-luminance height would otherwise turn into "spikes" on brick
// faces) while the large-scale mortar grid survives. Visible texture stays
// sharp — only the displacement is smoothed. Raise if bricks look spiky.
float ps_r_pom_blur   = 0.5f;
// Strength of the POM normal perturbation (relief catches sun/dyn/hemi light).
// 0 = flat lighting (offset only), 1 = default, higher = deeper-looking grooves.
float ps_r_pom_normal = 1.0f;
// POM self-shadow strength: contact shadows the relief casts in its own grooves
// toward the sun. 0 = off, 1 = default. Adds 8 height taps on sun-facing pixels.
float ps_r_pom_shadow = 3.0f;
// POM contact AO: view-independent groove darkening on the AMBIENT term, so the
// relief reads with depth even out of direct sun. 0 = off, 1 = default.
float ps_r_pom_ao     = 0.3f;
// r_pom_debug 1 = world shaders draw the POM occlusion mask (AO × self-shadow)
// grayscale — white = lit/open, dark = occluded grooves. Like r_ssao_debug but
// for the per-pixel POM contributions (which the GTAO debug view can't show).
int   ps_r_pom_debug  = 0;
// r_ao_flat 1 = DEBUG: force ALL ambient occlusion to neutral on world lmap/vlit
// — GTAO, POM-AO, the baked lightmap hemi-occlusion AND the dynamic hemi gate.
// Shows the scene with perfectly flat ambient (looks "wrong" by design) so you
// can see how much each occlusion source contributes. The baked lightmap is the
// dominant interior AO and this is the only way to neutralize it at runtime.
int   ps_r_ao_flat    = 0;
// r_shade_debug — lighting-component isolation in world_lmap/world_vlit/world_terrain:
// 1 albedo, 2 baked lmap/vertex data, 3 hemi-occ scalar, 4 GTAO, 5 ambient sky gate,
// 6 sky hemisphere colour, 7 total lighting (no albedo), 8 flat-ambient term, 9 sun
// term, 10 wetness factor, 11 the ambient term AS SHIPPED (analytic with
// r_ddgi_light 0, the DDGI probe field with it up — 11 against 6 is the GI A/B).
// 0 = off.
int   ps_r_shade_debug = 0;
// Per-orientation POM strength. Walls/fences (horizontal normal) always full;
// floors (normal up) scale toward r_pom_floor; ceilings (normal down) toward
// r_pom_ceil. Defaults: floor 0.75, ceiling 0.25 (overhead POM reads strong).
float ps_r_pom_ceil   = 0.25f;
float ps_r_pom_floor  = 0.75f;
// Terrain POM is EXPERIMENTAL / off by default: the ground is viewed at grazing
// angles almost always and its base texture is high-contrast, so POM there
// "swims"/mirrors. Walls/fences/floors-of-structures (lmap/vlit) keep POM.
int   ps_r_pom_terrain = 0;
// Terrain POM DEPTH OFFSET (SSFX deffer_terrain_high_flat_d.ps port): sink the
// terrain gl_FragDepth into the parallax cracks in the depth prepass AND the
// color pass, so GTAO and the VSM screen resolve see the carved micro-surface —
// AO darkens the pits, sun shadows wrap into them ("cut-in" ground, not painted).
// 1 = SSFX strength (0.11 m max sink along the view ray). Needs r_pom_terrain 1.
float ps_r_pom_zoff = 1.0f;
// TERRAIN COMPOSITE CACHE (vk_terrain_cache): compute-bakes the 4-channel splat
// ground (mask x heights x offsets x height-blend) into a camera-anchored
// height+weights clipmap once per camera-window move; the terrain POM then
// marches ONE texture instead of four (color pass AND depth prepass). Pure
// perf/architecture — the picture should be near-identical to the 4-march path.
// 0 = off (old path), 1 = on when the cache module is alive.
int   ps_r_terra_cache = 1;
// Cone-step marching on the composite cache: the bake also stores a
// conservative "empty cone" ratio per texel (height .g) and the POM march
// leaps guaranteed-free gaps instead of walking 24 fixed layers - crisper
// cracks at grazing angles (no stair-stepping / missed thin walls), usually
// cheaper. 0 = fixed-layer walk on the cache (A/B fallback).
int   ps_r_terra_cone = 1;
// Terrain detail-blend transition depth. 0 = plain mask cross-fade — asphalt
// fades SMOOTHLY into soil, the GAMMA/SSFX look (their HeightBlending() ships
// commented out). >0 = Mishkinis height blend: the RAISED material wins
// per-texel with this transition width — sharp interlocking edges (0.25 = the
// old hard look; the sharpness was OURS, not GAMMA's).
float ps_r_terra_blend = 1.f;
// Sun-horizon channel in the cache bake (needs r_terra_cone's pass): per-texel
// max slope toward the CURRENT sun azimuth -> POM self-shadow becomes a
// geometric one-tap tan(sun) vs tan(horizon) test instead of an 8-tap march
// (exact over 32 texels, re-baked when the sun drifts ~2.5 deg). Strength
// still rides r_pom_shadow. 0 = old march (A/B fallback).
int   ps_r_terra_horizon = 1;
// Terrain DETAIL NORMAL MAPPING strength (independent of POM). Perturbs the
// ground normal from the per-channel <detail>_bump maps (grass/asphalt/earth/
// gravel), blended by the splat mask — feeds sun + dyn lights only (the sharp
// sky cube stays on the flat geometric normal, else up-facing ground mirrors).
// No UV march → no grazing-angle "swim". 0 = off; 1 = full.
float ps_r_terrain_normal = 3.0f;
// Terrain MICRO contact AO: darkens micro-grooves using the detail-normal tilt
// (cavity) AND the detail height (R4 terrain AO = detail diffuse alpha). Cheap,
// no UV march, no swim — deepens the relief the detail normals create. 0 = off.
float ps_r_terrain_ao    = 0.125f;   // SSFX TERRAIN_POM_AO parity
// Terrain debug view: 0 off, 1 = world normal (Nw*0.5+0.5), 2 = micro-AO,
// 3 = detail height (splat-blended detail alpha).
int   ps_r_terrain_debug = 0;
// Terrain DRY sun gloss: a material-aware specular highlight from the bump .r
// channel (R4 gloss). Asphalt/gravel catch the sun even when dry; grass stays
// matte. Fades out as the ground wets (the wet reflection takes over). 0 = off.
float ps_r_terrain_gloss = 2.0f;
int   ps_r_puddle_debug  = 0;       // puddle/water debug: 0 off, 1 = coverage, 2 = micro-height / sim flow

// SURFACE FIELD (the "smart heightmap"): metre-scale ground height/slope/curvature/
// exposure/canopy derived from the rain+ground maps (shaders/surface_field.glsl).
// r_sf = master enable for future consumers (snow/fog/water); r_sf_debug visualizes
// the field; r_sf_eps tunes the finite-difference scale.
int   ps_r_sf        = 0;       // r_sf — master enable (consumers later)
int   ps_r_sf_debug  = 0;       // r_sf_debug — 0 off,1 height,2 slope,3 curvature,4 sky,5 canopy
float ps_r_sf_eps    = 1.5f;    // r_sf_eps — derive finite-difference epsilon (metres)
float ps_r_snow      = 0.f;     // r_snow — TARGET snow coverage 0..1 (Surface Field consumer; whitens by type x slope x sky)
float ps_r_snow_rate = 0.25f;   // r_snow_rate — snow accumulate/melt speed per second (eases the actual amount toward r_snow)
int   ps_r_snow_deform        = 1;      // r_snow_deform — footprint deformation (carve prints into the snow volume)
float ps_r_snow_deform_depth  = 0.14f;  // r_snow_deform_depth — print press depth (m)
float ps_r_snow_deform_radius = 0.22f;  // r_snow_deform_radius — print radius (m)
float ps_r_snow_deform_time   = 60.f;   // r_snow_deform_time — print lifetime (sec); time-decay so the trail lasts ~a minute
int   ps_r_snow_deform_tex    = 1;      // r_snow_deform_tex — dense persistent deform TEXTURE (vk_deform) vs the stamp loop
int   ps_r_snow_mesh          = 0;      // r_snow_mesh — dense player-centred snow surface mesh (VHM-style; needs deform_tex)
float ps_r_snow_berm          = 0.2f;   // r_snow_berm — displaced-snow BERM height around prints (fraction of dent depth)
float ps_r_snow_ripple        = 0.9f;   // r_snow_ripple — wind-ripple/sastrugi relief strength on the open snow (0 = off)
float ps_r_snow_rough         = 0.5f;   // r_snow_rough — trail/print IMPERFECTION (per-print width/depth + wavy edges)

// MUD footprints: the SAME persistent deform texture (vk_deform), read by the
// terrain shader on SOFT splat channels (earth/grass press, asphalt doesn't) —
// feet compress the POM ground and leave dark wet prints, no snow required.
float ps_r_mud_deform = 1.f;    // r_mud_deform — mud footprint strength (0 = off; shading + normal dimple)
float ps_r_mud_depth  = 0.45f;  // r_mud_depth — POM carve depth in prints (fraction of the detail height range)

// SSS PUDDLES (SSFX deffer_terrain_high_flat port): the puddle look. Distinct
// procedural puddle bodies (stand-in for SSFX's per-level artist puddles_mask) that
// GROW with the wetness accumulator and RECEDE as it dries; slope-masked to flat
// ground; reflection / drop-ripples / sun glint applied per pixel. r_puddle_sss =
// master; r_puddle_level = coverage (more/larger puddles); r_puddle_scale = size.
int   ps_r_puddle_sss    = 1;
float ps_r_puddle_level  = 0.5f;    // puddle coverage (higher = more/larger puddles, lower = fewer)
// Macro placement scale (procedural stand-in for SSFX's per-level puddles_mask).
// World frequency of the puddle-body noise: bigger = smaller/tighter puddles,
// smaller = broader pools. ~1.0 ≈ 5-6 m puddles. This is what gives DISTINCT
// puddles instead of a uniform wet sheet on levels without an artist mask.
float ps_r_puddle_scale  = 1.0f;
// r_puddle_geo — put puddles in REAL ground dips. The SSFX recipe places them by
// texture micro-height times a per-level HAND-PAINTED mask, so on a level without
// that mask a metre-scale hollow in the asphalt collects nothing: the water level
// only ever sees centimetre texture relief, and the macro placement is noise that
// knows nothing about the ground. This blends in the Surface Field's concavity
// (SF_Concavity — top-down curvature, sky-exposure gated) so water pools where the
// ground is actually concave. 0 = pure SSFX/noise placement, 1 = follow the dips.
float ps_r_puddle_geo    = 0.75f;
// r_wet_dist — how far (m) wet shading survives. Ours faded over 70->35 m, so the
// ground was wet underfoot and dry thirty steps out: on open terrain that reads as
// a wet patch following the player rather than a rained-on world. SSFX fades its
// wet gloss over 250->200 m (rain_patch_normal.ps). The ripple field keeps its own
// tight fade, so the extra range is one cube tap on already-wet pixels.
float ps_r_wet_dist      = 200.0f;
// r_terrain_detail_dist — how far (m) the terrain detail NORMAL / cavity AO / gloss
// survive. Was a hard 45->25 m fade: past it the ground drops its bump taps, so the
// asphalt loses grain, micro-AO and its specular glint in one step and reads as flat
// matte paint. SSFX samples the bump at any distance (only their POM march stops, at
// 20 m). Costs 4 taps per terrain pixel inside the range — lower it if it shows up.
float ps_r_terrain_detail_dist = 120.0f;
// r_bolt_flash — how much a lightning strike lifts the HEMISPHERE light. The engine
// spends a bolt by boosting sun_color and swinging sun_dir at the strike, which grows
// a second sun (with god rays) wherever the renderer draws the sun — several per clap,
// held on screen by the volume's temporal history. We hold the real sun direction
// instead (SunDirVisual), which would cost the flash entirely, and pay it back here:
// a strike is a huge distant AREA light, so it belongs in the sky term. 0 = no flash.
float ps_r_bolt_flash = 1.0f;
// Material normal + gloss on STATICS (`<bump>.dds`, see bump_common.glsl). This path
// had neither: wall relief came only from the `#` height through POM, and the sky
// specular ran on one invented roughness for the whole world.
// r_bump       — normal-map strength (0 = off, the pre-existing look).
// r_bump_debug — 1 draws the decoded WORLD normal, 2 the gloss. The R4 unpack is
//                .wzy/.x; a mod texture authored as plain RGB would come out with
//                inverted normals, and that is far easier to see as a colour field
//                than as "something is off with the walls".
// r_gloss_scale— how hard the material gloss drives the IBL roughness (R4 called
//                this r2_gloss_factor). ⚠ INERT while r_ibl is 0 (the shipped
//                default): roughness is consumed ONLY inside the SPEC_IBL block, so
//                with no specular term there is nothing for it to modulate. Left
//                wired for whoever revives IBL — see the r_ibl note above.
//
// r_bump itself stays ON and is INDEPENDENT of IBL: bumpNormal perturbs the shading
// normal that sun, hemi and dynamic lights all consume, not just the reflection. That
// half of this feature is verified good (user, 25-07: surface grain on walls reads);
// only the gloss half died with r_ibl.
float ps_r_bump        = 1.0f;
int   ps_r_bump_debug  = 0;
float ps_r_gloss_scale = 1.0f;

// =========================================================================
// EXPERIMENTAL / PARKED: WATER FLOW SIMULATION (compute, vk_water_sim).
// Off by default — NOT part of the shipped SSS puddle look. A shallow-water field
// on the rain ortho box: rain feeds it, water flows downhill and pools, evaporation
// drains it; when r_water_sim is on it becomes the puddle source + a volumetric
// water render. Kept for a future revisit (SSS+RDR2 water). All r_water_* below.
// =========================================================================
int   ps_r_water_sim    = 0;   // water flow sim OFF by default (experimental; revisit for SSS+RDR2 water)
float ps_r_water_rain   = 0.4f;
// Leak RATE (exponential drain ∝ water amount). Flat ground settles at depth
// ≈ rainRate/leak (shallow, self-limiting — no flood, no knife-edge); dips fed
// by runoff settle deeper. Higher = drier/shallower everywhere. ~3–8 is sane.
// Exponential leak. With the velocity sim the FLOW drains flats into dips, so the
// leak only needs to dry things over time — keep it LOW so water accumulates and
// stays visible (high leak = water evaporates before it can pool/stream).
// Flat-ground equilibrium depth ≈ rain/leak — keep leak high enough that light
// rain leaves flats only DAMP (sub-threshold), while dips collect runoff into
// small visible puddles. Too low = whole ground floods like a downpour.
float ps_r_water_evap   = 3.0f;
// Downhill ACCELERATION (gravity) for the velocity sim — how fast water runs /
// how strongly streams form (replaces the old relaxation factor). Higher = faster.
float ps_r_water_flow   = 0.5f;
// Volumetric water render (tonemap raymarch): r_water_murk = absorption per metre
// (higher = murkier / floor hidden sooner / "deeper" feel); r_water_refract =
// how much the surface ripples bend the view of the bottom.
float ps_r_water_murk   = 1.2f;
float ps_r_water_refract = 0.02f;

// =========================================================================
// WATER BODIES (vk_pass_water) — level ponds / rivers / flooded basements.
// A DIFFERENT thing from the r_water_* family above (that one is the parked
// puddle flow sim); hence the separate `r_wtr_*` prefix. Before this pass
// existed, `effects\water` geometry rode the opaque vert-lit path and — with
// zero baked vertex light and zero baked sky access on a water polygon —
// rendered pure BLACK on every level.
// =========================================================================
int   ps_r_wtr        = 1;      // master enable
// LEAN twin (Switch): water_lean.frag.spv (water.frag -DWATER_LEAN), no tessellation,
// no refraction copy, no near-field grid, and no rain-map render for the shelter
// query. The look knobs below still apply. Read per frame, so it A/Bs live.
int   ps_r_wtr_lean   = 0;
int   ps_r_wtr_debug  = 0;      // 1 flat magenta (does the pass run), 2 column depth, 3 wave normal, 4 alpha, 5 Fresnel
// Wave SLOPE amplitude (not height — the surface is a per-pixel normal field).
// Scaled at runtime by wind velocity and the weather's own m_fWaterIntensity.
float ps_r_wtr_wave   = 0.26f;
// What a DEAD CALM still leaves of that wave. wind_velocity is zero for most of
// this game's day, and the old implicit floor (0.35) took 0.19 m down to 0.071 —
// measured, in the [VK Water] wind line. Water is never actually glass.
float ps_r_wtr_calm   = 0.75f;
float ps_r_wtr_scale  = 0.22f;  // base wave frequency (1/m) — lower = longer swells
float ps_r_wtr_speed  = 1.0f;   // dispersion speed multiplier
// Extinction per metre of water column (Beer-Lambert). The single biggest
// "which game is this" knob: 0.2 = a clear mountain lake, 0.9 = the Zone's
// green-brown murk where the bottom vanishes at knee depth.
float ps_r_wtr_murk   = 0.9f;
float ps_r_wtr_refl   = 1.0f;   // sky reflection strength
float ps_r_wtr_glint  = 1.0f;   // sun glitter strength
float ps_r_wtr_detail = 60.0f;  // distance (m) over which the short chop fades out (anti-shimmer LOD)
float ps_r_wtr_rough  = 0.045f; // base specular roughness (grows with distance in the shader)
// Murk colour — what the body scatters back. Green-brown by default: the
// standing water of the Zone, not a swimming pool.
Fvector3 ps_r_wtr_color{ 0.085f, 0.135f, 0.105f };
// SHORELINE. Absorption alone leaves the water/terrain seam a hard cut, because
// Fresnel puts a floor under the alpha (at a grazing angle even a millimetre of
// water stays a mirror). r_wtr_shore fades the whole layer out over the last
// centimetres of depth so the edge dissolves; the foam band then hides what is
// left of it — a shore reads as a shore because of the scum line.
float ps_r_wtr_shore  = 0.22f;   // fade-out depth (m)
// SWASH: how far the waterline rides up and down with the passing wave. This is
// what makes the water WASH OVER the sand instead of ending at a painted line.
// A multiple of the wave height, so a calm pond barely moves and a rough one
// visibly climbs the beach.
// SWASH — the run-up. ⚠ These are METRES now, not a gain on the swell height.
// The old meaning multiplied the wave and was added to a DEPTH to widen an alpha
// fade, so 2.2 turned a 19 cm wave into 42 cm of "water" standing on the bank with
// nothing on screen to account for it. The surge is its own function now (front,
// fast up / slow drain, in sets) — see waterSwash in water_common.glsl.
// ⛔ PARKED 05-08 with the whitewater, same call: the shore break is OFF by
// default and the waterline is static again. Everything below still works and is
// two numbers away — r_wtr_surf_h 0.30 brings the arriving crest back, and this
// one the tongue that runs up the sand.
float ps_r_wtr_swash  = 0.0f;    // metres the biggest surges climb above the still line
float ps_r_wtr_swash_t = 6.0f;   // seconds between arriving crests
// SHORE BREAK (water_shore.glsl). Phased by the still water DEPTH, not by the
// wind — which is what makes a crest arrive parallel to the beach in every bay
// without a shoreline being authored anywhere. Iso-depth lines are the shoreline.
//
// ⛔ PARKED 04-08 at the author's call: "пока что решил отключить, может потом
// доделаю". The whitewater never looked right on this content — the run-up tongue
// reads as white sheeting over grass rather than as surf, and the last round left
// it plausible but not good. The MOTION it drives is kept (the waterline still
// arrives and drains, and the wetness still follows it); only the white is off.
// Turn it back on with r_wtr_surf 1 — nothing else needs changing.
float ps_r_wtr_surf     = 0.0f;   // whitewater strength
float ps_r_wtr_surf_h   = 0.0f;   // offshore height of the arriving wave (m) — PARKED
float ps_r_wtr_surf_len = 11.0f;  // metres between crests
// ⚠ Metres of water in the RUN-UP SHEET. A swash tongue is a couple of
// centimetres deep and you see the sand through it; the run-up HEIGHT is how far
// it climbs, not how much water is standing there. Feeding the height into the
// depth fade put a 45 cm slab of opaque water on the bank with a vertical face.
float ps_r_wtr_film     = 0.05f;
// Foam the backwash leaves ON THE GROUND once the sheet has drained off it. Costs
// no extra channel: the wetness map's decay already carries "how long ago", and an
// exponent rescales that clock (see shoreFoam).
//
// ⛔ PARKED 04-08 with r_wtr_surf, same call and same reason. The mechanism is
// sound and cheap; what it paints on this content is not. r_wtr_foam_land 0.55
// brings it back.
float ps_r_wtr_foam_land = 0.0f;
float ps_r_wtr_foam_life = 2.5f;   // seconds it takes to dissolve
float ps_r_wtr_foam   = 0.45f;   // foam strength (0 = off)
float ps_r_wtr_foam_w = 0.45f;   // foam band depth (m)
// MICRO-RIPPLE: short capillary waves, always present regardless of weather.
// The swell dies with the wind; this does not — without it a windless hour
// renders a mirror-flat sheet. Fades out over the first ~25 m (below a pixel
// past that, and pure shimmer if kept).
float ps_r_wtr_micro  = 0.012f;
// How much the weather's wind moves the SWELL. 1 = calm ~0.45x, storm ~2.6x;
// 0 = ignore the wind and always use r_wtr_wave as-is.
float ps_r_wtr_wind   = 1.0f;
// FETCH — how literally to take the SIZE of each body of water. The wave field
// is a function of world position, so without this every puddle, cellar and
// water butt got the same 28-metre open-water swell, complete with run-up and
// a shoreline foam line. A wave needs room: octaves whose wavelength does not
// fit in the surface's own bounding box several times over are dropped, which
// leaves a small pool with nothing but capillary ripple and the interactive sim.
// 1 = take the box at face value; >1 = pretend pools are larger (rougher);
// 0 = off, every pool is open sea again (the old look, for comparison).
float ps_r_wtr_fetch  = 1.0f;
// ⚠ ...AND "the surface's own bounding box" is where that idea leaked. Level
// water is not one mesh per pool: Cordon is ONE water visual, 1952 triangles,
// 79.8 x 148.2 m, so the fetch above came out as 108 m for every drop of water on
// the level — including the two-metre circle inside a well in the newbie village.
// Measured there with r_wtr_audit (16-08): pool 2.9 m2, span 2.0 x 1.9 m, still
// sheet at y=-19.98, and the wetness map holding marks up to -19.55. The drawn
// surface was climbing 37 cm above its own level in 22 cm of water — over the
// stone ring, out into the yard as a floating square, and soaking a ring of dirt
// that dried and re-wetted with every crest.
//
// This gates the fix: a compute pass (water_fetch.comp) measures each column's
// own body of water out of the POOL MASK, which knows where water ends at 12.5 cm,
// and the consumers take the smaller of that and the box. It can only ever say
// SMALLER — anything reaching the search radius is written as "no answer" and
// keeps the box — so open water is untouched by construction.
// 0 = back to the bounding box alone (the A/B).
int   ps_r_wtr_fetch_local = 1;
// SHELTER — the other half of the same problem: wind waves need WIND, and there
// is none under a roof. Queried from the sky-occlusion map, hemispherically (a
// pond under a pipe rack still catches wind from the sides, so a straight-up
// test would stamp dead-calm blobs under every overhead object). Scales the
// swell only: sheltered water keeps its capillary ripple and still takes rings
// from a footstep, which is what standing water indoors actually looks like.
// 0 = a roof does nothing, the old look.
float ps_r_wtr_shelter = 1.0f;
// Murk multiplier for sheltered water. A pond is flushed by rain, inflow and
// wind mixing and stays comparatively clear; a flooded cellar has none of that,
// so the silt stays in suspension and the bottom goes dim within centimetres.
// Multiplies r_wtr_murk, scaled by the same shelter query. 1 = indoor water is
// as clear as a pond (the old look).
float ps_r_wtr_murk_still = 7.0f;

// ---- SPECTRAL WAVES (vk_water_fft) ----------------------------------------
// Tessendorf's FFT ocean: the sea built in the Fourier domain from the Phillips
// spectrum, inverse-transformed to a displacement map on the GPU every frame.
// It exists for ONE thing the nine-octave analytic swell cannot do at any
// setting — the HORIZONTAL displacement that makes a crest sharp. A sum of
// sines displaced along Y has round tops however tall you make it.
int   ps_r_wtr_fft        = 1;
// Largest cascade's tile, metres, and the shrink to the next one. 250 / 0.175
// gives 250 m, 43.8 m and 7.7 m — each carrying only the band between its own
// size and the next one down, so nothing is represented twice and the small
// tile's repeat never shows.
float ps_r_wtr_fft_size   = 250.f;
float ps_r_wtr_fft_ratio  = 0.175f;
// ⚠ A BASE wind speed in m/s, NOT a scale on the weather's. Measured, not
// assumed: wind_velocity in this content is 0 for most of the day, and the
// analytic swell was effectively dead for exactly that reason until it was
// given a floor. The weather modulates around this (0.65x .. 1.55x); it cannot
// zero it. This is the main knob for "how rough is the sea".
// ⭐ 5 m/s, and the number was MEASURED, not picked. The Phillips peak sits at
// lambda = 2*pi*sqrt(2)*V^2/g, so the wind speed is really a knob for WAVE
// LENGTH: at 7 m/s the peak is a 44 m swell, which on a 60 m pond is not a wave
// at all, it is the water level tilting. At 5 m/s the peak lands at ~23 m and
// the 7.7..44 m cascade carries 7x the energy of the long one — waves you can
// see across a pond. Height is then set independently by the amplitude below,
// which is exactly why Phillips has both knobs.
float ps_r_wtr_fft_wind   = 5.f;
// Phillips A, in units of 1e-6. 28 puts the significant wave height at 0.29 m in
// dead calm and 0.75 m in a storm — measured by summing the spectrum over the
// same three cascade grids the shader uses. It has to be this large BECAUSE the
// wind was lowered: energy in this spectrum goes as roughly V^4, so moving the
// peak down to a useful wavelength costs a factor of ~30 in height.
float ps_r_wtr_fft_amp    = 28.f;
float ps_r_wtr_fft_chop   = 1.1f;    // lambda: how far a crest pulls water in sideways
float ps_r_wtr_fft_depth  = 60.f;    // sea depth for the dispersion relation (m)
// The whole field repeats EXACTLY this often: every wave's frequency is snapped
// to a multiple of 2*pi/repeat, so nothing drifts out of phase over a long
// session and the sea at minute 40 is the sea at minute 0.
float ps_r_wtr_fft_repeat = 200.f;
float ps_r_wtr_fft_small  = 0.06f;   // capillary cutoff (m) — kills the sub-centimetre noise
float ps_r_wtr_fft_dir    = 2.f;     // how sharply the spectrum favours the wind direction
float ps_r_wtr_fft_gain   = 1.f;     // how much of the transformed field to use (0 = none)
float ps_r_wtr_fft_foam   = 0.7f;    // whitewater from the Jacobian (folded-over water)
// The body span at which a cascade reaches full strength — the spectral field is
// gated per band by the SAME fit test the analytic octaves use, so a 250 m
// cascade is simply absent from a flooded cellar while the 8 m one still lives
// there. This is what stops an open-sea model from putting surf in a basement.
float ps_r_wtr_fft_fetch  = 60.f;
float ps_r_wtr_fft_slope  = 1.f;     // shading response, independent of the displacement

// ---- INTERACTIVE RIPPLES (vk_water_ripple) --------------------------------
// A 2D wave-equation heightfield on a tile that follows the camera. The half of
// "water" no analytic wave can fake: a sum of sines cannot propagate, reflect,
// interfere — or REACT. Footsteps and rain drops write into this field and the
// waves travel outward on their own.
int   ps_r_wtr_sim        = 1;
float ps_r_wtr_sim_size   = 64.f;    // tile edge (m); 512 texels -> 12.5 cm each
// Velocity retention PER STEP at 60 Hz — the LIFETIME of a ripple. Damping
// multiplies velocity, so an oscillating wave's amplitude decays as damp^30 per
// second: 0.995 leaves 86% after a second, 0.97 only 40%.
//
// This knob spent two days doing a job that was never its own. "A splash in my
// puddle shows up in the far ones" was fought here, by making waves die before
// they could travel — and it worked, at the price of the thing the sim exists
// for: at 0.97 a footstep ring was gone before the eye caught it. The two
// demands are not reconcilable on one dial, because they are not the same
// question. Lifetime is time; a puddle boundary is SPACE.
//
// The boundary now lives where it belongs — r_wtr_sim_lid, which asks the
// collision model which water is roofed by a floor and therefore is not a puddle
// at all. With the wave properly confined, lifetime is free to be generous
// again, which is what this value is: rings that spread, reflect off the rim and
// take a few seconds to fade.
float ps_r_wtr_sim_damp   = 0.995f;
float ps_r_wtr_sim_speed  = 0.35f;   // stiffness; the explicit scheme blows up past 0.5
// POOL BOUNDARIES. The sim is one heightfield over a 64 m tile, so without a mask
// of where the water actually IS, a splash in one puddle propagates across the dry
// floor and surfaces in every other puddle in range. The mask (drawn each frame
// from the water meshes, straight down) turns each body of water into its own
// pool with reflecting rims. 0 = the old unbounded field.
int   ps_r_wtr_sim_pools  = 1;
// LID SOURCE. The mask says where the water IS; what separates puddles is where
// the water can be SEEN, and that is decided by the FLOORS above it. Level water
// is a couple of huge sheets that dive under the buildings, so a cellar's
// "puddles" are one slab surfacing through an uneven floor — connected, and a
// wave crossing between them is correct physics that no mask may forbid.
//
// The rasterized lid was meant to catch this and cannot: after pool compaction
// the statics are drawn only by the GPU-driven paths, and the CPU queue it walks
// held five leftovers ("lid map: 5 static(s)"). So the lid asks the COLLISION MODEL
// instead — one short ray up from the surface per texel, immune to which path
// draws the world.
// Ray budget per frame. A CDB trace is a couple of microseconds, so this is the
// dial between "the lid lags behind a sprint" and "filling it hitches the frame".
int   ps_r_wtr_sim_lid_rays = 1500;
// SHALLOW WATER. The same sweep that finds the floor above also finds the bottom
// below, which gives the water COLUMN — and a wave's speed is sqrt(g*h). Letting
// the stiffness follow the depth makes a ripple slow down as the bottom rises,
// turn to face the shore and bunch up on it, none of which a constant-speed field
// can do. It also retires shoreAbsorb: a wave stops at the rim because it runs
// out of depth, not because a multiplier was told to stop it.
int   ps_r_wtr_sim_depth     = 1;
// Depth at which the wave runs at the full r_wtr_sim_speed. Not a physical
// constant: the grid's CFL limit caps the absolute speed well below sqrt(g*h) at
// this texel size, so what is reproduced is the SHAPE of the relationship —
// shallow is slower than deep by the right ratio — not the metres per second.
// Set it from the profile of the BOTTOM, not from the mean: the lid reports 74%
// of open water under 20 cm on this content, so a mean of 0.36 m (dragged up by
// a deep minority) still ran three quarters of the pool slowed down. This is
// puddle scale. Everything at or above it runs at full speed; below it the wave
// shoals — so the value is really "where does the rim begin".
float ps_r_wtr_sim_depth_ref = 0.15f;
float ps_r_wtr_sim_bed       = 0.99f;   // per-step damping at zero depth (1 = none)
// SHORE ABSORPTION, applied once per dry side of a texel per step. A rim that
// merely reflects loses nothing: one splash bounces around the pool until the
// whole sheet is standing waves — and since a cellar's "puddles" are one 20 x 13 m
// slab surfacing in several places, that meant jumping in one of them rippled all
// of them. Bleeding energy at the shore keeps the disturbance where it was made.
// 1 = the old reflecting rim.
float ps_r_wtr_sim_shore  = 0.90f;
// REACH: metres of full-strength ripple around the player, fading to nothing at
// twice that. The audit proved the far "puddles" are the SAME body of water as
// the near one — a cellar floor is one 20 x 13 m slab surfacing in patches — so a
// wave reaching them is physically correct and no boundary test can forbid it.
// The eye still expects a splash to stay where it was made, so the disturbance is
// bounded in space. 0 = unbounded (waves cross the whole sheet again).
float ps_r_wtr_sim_reach  = 6.0f;
float ps_r_wtr_sim_slope  = 1.0f;    // how much of the sim reaches the NORMAL
float ps_r_wtr_sim_height = 1.0f;    // ...and the geometric DISPLACEMENT
float ps_r_wtr_step       = 0.075f;  // dip a footstep punches into the surface (m)
// BOW WAVE, as a multiple of the footstep dip. A body wading does not only make
// holes, it displaces water forward — and every source in this sim was a
// symmetric dip, so nothing was ever pushed. Scales with how fast you are moving.
// 0 = off (waves only where the feet land, the old behaviour).
float ps_r_wtr_bow        = 1.2f;
// SHORE WETNESS. Water that touches a bank, a wall or a heap of rubble leaves it
// dark and glossy, and it dries afterwards — until now every wet surface in this
// renderer was wet because of RAIN, so a river ran past bone-dry ground. The
// tile remembers CONTACT (see vk_water_ripple), which is what lets a receding
// waterline leave a wet strip that fades instead of switching off.
float ps_r_wtr_wet      = 1.0f;    // strength (0 = off)
float ps_r_wtr_wet_dry  = 25.0f;   // seconds from soaked to dry
// CAPILLARY RISE: metres of damp above the line the water actually TOUCHED.
//
// ⚠ It used to mean something much larger and much cruder — a flat band added to
// the still waterline everywhere the sheet reached in plan view, which is a
// contour line, not a shore: at 0.35 on a 1:10 beach it painted three and a half
// metres of sand wet that no wave had been near, from the first frame, for ever.
// The run-up strip is now measured (the wave that is drawn against the ground
// that is there), so this went back to being what it says: the few centimetres
// that soak UP a stem, a stone or a bank above the water's own line.
float ps_r_wtr_wet_lift = 0.06f;

// ---- TESSELLATION ---------------------------------------------------------
// Stock water meshes are 4-7 METRES per triangle (see the [VK Water] census),
// so there is nothing to displace until the surface is subdivided. tessMax is
// spent quadratically toward the camera; past tess_far the surface is flat and
// the wave is a normal again.
// CEILING on the subdivision of one edge, not the factor itself — water.tesc
// budgets by metres of surface per segment now, because a factor means something
// quite different on a 2 m edge and on the 200 m ones this game's sheets have.
// 64 is the hardware limit; below ~32 the long swell stops resolving at all.
float ps_r_wtr_tess      = 64.f;
float ps_r_wtr_tess_near = 8.f;
float ps_r_wtr_tess_far  = 70.f;
float ps_r_wtr_disp      = 1.0f;     // metres of displacement per unit wave height
// NEAR-FIELD GRID: quads per side of the dense camera-locked water mesh (0 = off,
// the level's own polygons only). ⚠ This is the piece that makes a wave possible
// at all: stock water is a handful of triangles up to 200 m across, and hardware
// tessellation caps at 64 per edge, i.e. ~3 m per segment — coarser than the waves
// themselves. 128 over the 64 m ripple tile is 50 cm a quad.
int   ps_r_wtr_grid      = 128;

// ---- REFRACTION + CAUSTICS ------------------------------------------------
// Until now the bottom arrived as the destination of the blend: correct, but a
// blend cannot BEND what is behind it, so the riverbed sat dead straight under
// a rippling surface. The pass now takes a half-res copy of the scene and
// samples it itself — which also buys per-channel absorption (red dies first)
// and a place to put the caustics.
float ps_r_wtr_refract   = 0.035f;   // UV offset per unit surface slope
// PSKIP THE REFRACTION SNAPSHOT WHEN NO WATER PIXEL SURVIVED THE LAST FRAME.
// The snapshot is a blit of the WHOLE HDR target down to half res plus two layout
// transitions on the frame's main colour attachment, and it ran unconditionally:
// pripyat_full ships two water plates ~1 km across sitting 16 m under the road, so
// their bounding spheres reach the frustum from anywhere and nothing ever culled
// them. An occlusion query around the water draw answers the real question - see
// the note over s_occlPool in vk_pass_water.cpp. 0 = the old unconditional copy.
int   ps_r_wtr_occl      = 1;
// Caustics: the sunlight web on the bottom. Computed from the DIVERGENCE of the
// same wave field (no texture, no photon pass) — where the surface is concave
// the refracted rays bunch up.
float ps_r_wtr_caustic   = 0.55f;
float ps_r_wtr_caustic_p = 1.6f;     // exponent — higher = thinner, sharper web

// Bent-normal specular occlusion of wet/puddle sky reflections (UE-style). 0 = off,
// 1 = full; fades reflections in crevices / under overhangs where sky can't reach.
float ps_r_spec_occ     = 1.0f;

// PN-triangle silhouette curvature (R4 TESS_PN). OFF by default: it rounds
// hard-surface props (barrels/crates/walls) by inflating along smoothed
// vertex normals — meant for organic curves, looks wrong on world geometry.
// Was previously read UNINITIALIZED (push offset 112 sat outside the 112-byte
// range and was never pushed → garbage curvature warped tessellated props).
float ps_r_tess_pn     = 0.0f;

// Common
extern int psSkeletonUpdate;

float ps_r__WallmarkTTL = 60.f;

float ps_r__GLOD_ssa_start = 256.f;
float ps_r__GLOD_ssa_end = 64.f;

float ps_r__LOD = 0.5f;

float ps_r__ssaDISCARD = 3.5f; // RO

int ps_r__tf_Anisotropic{16};

float ps_lens_flare_sun_blend{};

Fvector3 ps_ssfx_gloss_minmax{0.6f, 0.9f, 0.0f}; // Minimum value of gloss, Maximum value of gloss, Extra gloss to the weapons HUD elements when raining
Fvector4 ps_ssfx_lightsetup_1{0.35f, 0.5f, 1.0f, 1.0f}; // intensity of specular lighting, Porcentage of the specular color. ( 0 = 0% | 1 = 100% ), Automatic adjustment of gloss based on wetness (0 or 1), Value to control the maximum value of gloss when full wetness is reached. ( 0 = 0% | 1 = 100% )
float ps_ssfx_gloss_factor{}; //Управляется из IGame_Persistent::UpdateRainGloss()

Fvector4 ps_ssfx_wetsurfaces_1_cfg{1.5f, 1.4f, 0.7f, 1.25f}; //ripples_size, ripples_speed, ripples_min_speed, ripples_intensity
Fvector4 ps_ssfx_wetsurfaces_2_cfg{1.2f, 1.5f, 0.2f, 0.7f}; // waterfall_size, waterfall_speed, waterfall_min_speed, waterfall_intensity
Fvector4 ps_ssfx_wetsurfaces_1{}, ps_ssfx_wetsurfaces_2{}; // Управляется из IGame_Persistent::UpdateRainGloss()

Fvector4 ps_ssfx_hud_drops_1_cfg{3.0f, 1.f, 1.f, 50.f}; // Quantity of drops, Refrelction intensity, Refraction intensity, Speed of the drops animation
Fvector4 ps_ssfx_hud_drops_2_cfg{50.f, 50.f, 0.75f, 2.f}; // Drops build up speed, Drying speed, Size of the drops, Raindrops gloss intensity
Fvector4 ps_ssfx_hud_drops_1{}, ps_ssfx_hud_drops_2{}; // Значениями этих векторов управляет IGame_Persistent::UpdateHudRaindrops()

Fvector4 ps_ssfx_rain_1{10.0f, 0.02f, 5.f, 2.f}; // Len, Width, Speed, Quality

Fvector4 ps_ssfx_grass_interactive{1.f, static_cast<float>(GRASS_SHADER_DATA_COUNT), 2000.f, 1.0f};
Fvector4 ps_ssfx_int_grass_params_1{2.0f, 1.0f, 1.0f, 25.f};
Fvector4 ps_ssfx_int_grass_params_2{1.0f, 5.0f, 1.0f, 1.0f};

int ps_r__detail_radius = 100;

float ps_current_detail_density = 0.6;
float ps_current_detail_scale = 1.f;
float ps_r2_no_details_radius = 0.f;
float ps_r2_no_rain_radius = 0.f;

// textures
int psTextureLOD = 0;

// Texture streamer (vk_texture_stream.{h,cpp}).
//  r_txstream          — 1 enables dynamic per-frame mip promote/demote (opt-in;
//                        the load-time budget cap + memory_priority are always on).
//  r_txstream_budget   — texture VRAM budget in MB (0 = auto: device budget − headroom
//                        − non-texture usage).
//  r_txstream_headroom — MB of device VRAM kept free (never filled by textures) so
//                        render targets / FG buffers / spikes always fit.
// Default ON since 16-07: Stage A verified in-game (Pripyat marathon 15-07) —
// GPU feedback + idle-free swaps + VMA-aware budget + 256px visible floor. On
// light maps the streamer idles by design (everything fits, zero swaps).
int ps_r_txstream          = 1;
int ps_r_txstream_budget   = 0;
int ps_r_txstream_headroom = 512;
//  r_txstream_loadcap  — LOAD TIME, not VRAM: the largest base dimension a
//                        budget-fit texture may arrive at DURING a level load
//                        (0 = off). The existing budget-fit only demotes when the
//                        card is full, so on an 8 GB card a level pulls in every
//                        texture at full res: measured on pripyat_full 17-08,
//                        2458 files = 3558 MB read + 2556 MB uploaded = 8.4 s of a
//                        21 s load. Mip 0 alone is ~3/4 of a chain's bytes, so
//                        arriving coarse and letting the feedback-driven promote
//                        path (StreamStep) sharpen what is actually on screen buys
//                        most of that back. Quality heals in seconds of play.
// r_tex_prefetch — level texture prefetch (VK::TexPrefetch, vk_texture.cpp).
// The loader opens ~2460 .dds one at a time; on pripyat_full that is 2860 of the
// 6565 ms texture phase spent inside FS.r_open on ONE thread. The set is known
// as soon as the level shader table is parsed, so workers open them ahead.
// 0 = old serial behaviour (kept for A/B — the walk itself is unchanged).
// r_tex_prefetch_mb caps the bytes parked ahead of the walk; workers stall at the
// cap and resume as the walk consumes. Floor 64 MB.
// r_upload_wc_copy — 1 = streaming (non-temporal) stores when filling the upload
// staging ring, 0 = plain CRT memcpy. The ring is mapped WRITE-COMBINING, where
// `rep movsb` measured 1.97 GB/s and made the staging copy the single largest item
// left in a level load's texture phase. Kept as a switch because it is a CPU-level
// micro-optimisation whose payoff is hardware-dependent — A/B it, don't assume.
int ps_r_upload_wc_copy = 1;

// r_upload_cached — 1 = allocate the upload staging ring in HOST_CACHED memory
// instead of the usual write-combined one. Read ONCE, when the command manager
// builds the ring, so it only takes effect from user.ltx / a restart.
int ps_r_upload_cached = 0;

// r_upload_copy_threads — extra threads that help fill the staging ring, 0 = the
// caller copies alone. Only pieces of 256 KB and up are split. Measured on
// pripyat_full's 1869 MB of geometry: 2 / 4 / 8 helpers all land within noise of
// each other (432 / 406 / 420 ms) because the copy is bound by its SOURCE, not by
// the writes. Helpers idle on a condition variable, so this costs nothing outside
// a load.
int ps_r_upload_copy_threads = 4;

// r_geom_lead — 1 = stage level.geom out of the texture prefetch's whole-file view
// (its pages are being faulted in by sixteen workers), 0 = out of the loader's own
// sliding window, which faults every 4 KB on the loading thread. Here to A/B the
// two in one binary; there is no reason to run with it off.
int ps_r_geom_lead = 1;

// r_vis_warm — 1 = sixteen workers fault the visuals blob in alongside the walk
// that reads it (it is a span of the mapped level file, so every 4 KB otherwise
// costs the loading thread a fault). Interleaved pairs: visual phase -46/-37/-86 ms.
int ps_r_vis_warm = 1;

// r_tex_repack_threads — helpers for the BC3->BC4 lightmap gather (0 = the loading
// thread alone). A level's hemi maps are 4096^2, and the gather is 16 bytes in / 8
// out per block with nothing to synchronise, so it splits cleanly. Uses the
// persistent staging-ring pool, not fresh threads per texture.
// r_tex_prefetch_lmaps_first -- 1 = the prefetch parks the level's lightmaps
// before the 2400 diffuse bases. The walk needs a lightmap in its first
// milliseconds and used to miss all 27 of them (432 MB read twice).
int ps_r_tex_prefetch_lmaps_first = 1;

// r_prewarm_async -- 1 = the WET/SNOW weather pipeline variants are compiled on a
// worker thread instead of one per precache frame, and the precache stops waiting
// for them. Measured on pripyat_full: 28 precache frames become the 12-frame floor
// (~12 ms a frame of load screen spent compiling variants nothing draws until it
// rains). 0 = the old 1/frame pace on the render thread, for the A/B.
int ps_r_prewarm_async = 1;

// r_thm_cache -- 1 = remember what a texture's .thm said. The file is opened and
// parsed TWICE per material: once by the prefetch worker expanding the base into
// its file set, once by the walk creating the material. Keyed by base name + level
// tag (a .thm can resolve level-locally). 0 = the old re-read, for the A/B.
int ps_r_thm_cache = 1;

// r_clpage_map -- 1 = the pinned cluster pages are uploaded straight out of a
// mapping of the page cache file, with the pages faulted in by workers first.
// The old path fseek/fread 336 MB of a 2.2 GB file into a scratch buffer on the
// loading thread and then copied that into staging: two copies and a serial read.
// 0 = the fread path, for the A/B.
int ps_r_clpage_map = 1;

// r_tex_residency_threads — helpers that read a residency plan's .dds files before
// the images are built (0 = the old serial read inside each build). The tree path
// raises 42 materials to a 1024 floor in one batch and measured read 45 of its
// 57 ms: 42 archive entries LZO-decompressed one after another on the thread the
// level is waiting on. The image create + staging copy stay serial.
int ps_r_tex_residency_threads = 8;

int ps_r_tex_repack_threads = 4;

// r_tex_materialize - 1 = a prefetch worker takes a texture all the way (read,
// parse, repack, image, upload, publish into the cache the walk looks in) instead
// of reading the file and parking the bytes. Parking only ever moved the READ off
// the loading thread: of the 451 ms the walk spent on textures, the read was 59 and
// the other 392 (repack, image creation, staging copy) stayed on the one thread the
// level waits for, while sixteen workers sat blocked on the parking budget.
// 0 = the old park-and-hand-over path; 1 = build from the first job; 2 = read and
// park during the geometry stage, then build once the walk starts.
//
// ⛔ DEFAULT 0 — MEASURED LOSS, kept as the instrument that proved it. The walk's
// texture time does fall (538 -> 105 ms), and the load gets LONGER anyway:
//   mode 1: geometry 352 -> 832 ms. Texture uploads mixed into the geometry stage
//           drag the shared ring from 18.5 to 8.4 GB/s; geometry hands back every
//           millisecond the walk saved (3182 vs 2972 ms total).
//   mode 2: the walk keeps its texture time (596-656 vs 538). Sixteen workers each
//           fill the ring single-file (one upload mutex) while the machine is busy,
//           and the ring copy falls to 6.4 GB/s -- see RingCopy in vk_command_buffer.
// The lesson: that 538 ms is not CPU work waiting for cores, it is a page-fault-
// bound copy into one ring that ALREADY uses every core, one copy at a time.
// What is still worth trying is the other split: workers parse/repack/create, the
// walk keeps the upload (its measured ceiling is the 258 ms that is not upload).
int ps_r_tex_materialize = 0;

// r_tex_mat_threads - how many workers may be BUILDING a texture at once. The
// pool is sixteen because reading is what it was sized for; building ends in the
// upload ring, which is one mutex, and a machine with every core busy is a bad
// place to fill it. 0 = no limit.
int ps_r_tex_mat_threads = 0;

// r_tex_repack_scratch — 1 = the gather writes into one buffer per thread, grown to
// the largest image and kept, instead of a fresh xr_malloc per texture. Half a
// gigabyte of destination per load is otherwise faulted in a page at a time and
// handed straight back.
int ps_r_tex_repack_scratch = 0;

int ps_r_tex_prefetch    = 1;
int ps_r_tex_prefetch_mb = 1024;

int ps_r_txstream_loadcap  = 0;

// r_vram_small_images - 1 routes sub-1MB images into the 8 MB-block small pools,
// which costs a driver size probe (vkGetDeviceImageMemoryRequirements) in front of
// EVERY non-dedicated image allocation. 0 skips both. The pools exist because small
// long-lived allocations pinned 64 MB blocks (1740 MB of unreleasable slack on
// Pripyat, 18-07); the knob is here to price the probe against that.
int ps_r_vram_small_images = 1;
//  r_txstream_reserve  — MB of device VRAM that must be FREE after a level finishes
//                        loading. If less is free, the streamer trims the largest
//                        world textures (at the load-end idle point) until it is.
//                        Leaves room for Streamline Frame-Gen buffers / spikes so a
//                        heavy map (Pripyat) doesn't crash Streamline at first present.
int ps_r_txstream_reserve  = 1024;

// Cluster-LOD page streaming (vk_cluster_stream, Stage B of the streaming world).
//  r_clpage        — 1 = keep only a budgeted pool of cluster-IB pages resident
//                    (pinned roots/coarse always in VRAM; GPU feedback streams the
//                    rest from the cluster cache file, coarser DAG levels cover
//                    while pages arrive). 0 = eager install-all (pre-Stage-B
//                    behavior/VRAM). Read at level load.
//  r_clpage_budget — resident page-pool budget in MB (pinned pages always fit).
// Default ON since 16-07: verified on Pripyat (resident ~85 vs 326 MB, request
// feedback converges to zero, no holes/flicker; self-loop cut closed the last
// stuck-request tail). 0 = eager install-all remains the parity fallback.
int ps_r_clpage        = 1;
int ps_r_clpage_budget = 160;
// Pool compaction (Stage B increment (б)): after WorldGPU::Build the vertex/
// index slices of fully-repacked clustered meshes are duplicates of the
// ClusterStream page pools — rebuild the level VB/IB pools with only the
// slices CPU paths still need (plain meshes, alpha-tested casters, terrain,
// trees, CPU leaves) and free the rest. While active, the always-CPU static
// shadow consumers (rain/ground map, spot/point lights) draw the statics via
// the cluster shadow targets, and the CPU fallbacks (r_gpu_world 0,
// r_gpu_shadows 0, r_shadow_cluster 0) are forced to the GPU paths — set
// r_pool_compact 0 (+ level reload) for a true A/B of those. Read at load.
int ps_r_pool_compact  = 1;

// Stage D world composition: total chunk count (home + clones of the loaded
// level tiled on a grid). 0/1 = off. Render-only replication slice — clones
// have no collision/AI; statics only (trees/grass/LODs home-only for now).
int ps_r_compose = 0;

float ps_r2_img_exposure = 1.0f; // r2-only
float ps_r2_img_gamma = 1.0f; // r2-only
float ps_r2_img_saturation = 1.0f; // r2-only

Fvector ps_r2_img_cg{0.5f, 0.5f, 0.5f};

#include "../../xr_3da/XR_IOConsole.h"
#include "../../xr_3da/xr_ioc_cmd.h"

float ps_particle_update_coeff = 0.3f;

// `ps_r_sunshafts_intensity` is owned by xrEngine (Environment_misc.cpp).
extern float ps_r_sunshafts_intensity;

// The Switch preset (rspec_vk_switch.ltx) sets levers no other preset mentions (R1 grass/crowns, lite cascades, NPC
// occlusion...). Picking Switch and then High would leave them on, so the stock value of every command named in that
// file is captured once, before user.ltx runs (`_preset_defaults` from execUserScript, or lazily on the first
// `_preset`), and every non-Switch preset replays those values before loading its own file.
static xr_vector<std::pair<xr_string, xr_string>> s_presetDefaults;
static bool s_presetDefaultsCaptured = false;

static void CapturePresetDefaults()
{
    if (s_presetDefaultsCaptured)
        return;
    s_presetDefaultsCaptured = true;
    string_path cfg;
    FS.update_path(cfg, fsgame::game_configs, "rspec_vk_switch.ltx");
    IReader* F = FS.r_open(cfg);
    if (!F)
    {
        Msg("! [VK preset] cannot read '%s' -- switching away from the Switch preset will not restore its levers", cfg);
        return;
    }
    string1024 line;
    while (!F->eof())
    {
        F->r_string(line, sizeof(line));
        const char* b = line;
        while (*b == ' ' || *b == '\t') ++b;
        if (!*b || *b == ';' || *b == '#')
            continue;
        string256 name;
        u32 n = 0;
        while (b[n] && b[n] != ' ' && b[n] != '\t' && n + 1 < sizeof(name)) { name[n] = b[n]; ++n; }
        name[n] = 0;
        IConsole_Command* C = Console->GetCommand(name);
        if (!C)
            continue;
        IConsole_Command::TStatus st;
        C->Status(st);
        if (!st[0] || std::any_of(s_presetDefaults.begin(), s_presetDefaults.end(), [&](const auto& e) { return e.first == name; }))
            continue;
        s_presetDefaults.emplace_back(name, st);
    }
    FS.r_close(F);
    Msg("~ [VK preset] captured stock values of %u Switch-preset commands", (u32)s_presetDefaults.size());
}

class CCC_PresetDefaults : public IConsole_Command
{
public:
    CCC_PresetDefaults(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR) { CapturePresetDefaults(); }
};

class CCC_Preset : public CCC_Token
{
public:
    CCC_Preset(LPCSTR N, u32* V, const xr_token* T) : CCC_Token(N, V, T){};

    // ⚠⚠THE OLD FILE NAMES WERE THE PROBLEM, not the mechanism. `rspec_*.ltx`
    // ships inside the mod's archives and was written for the DX renderer: the
    // bulk of what it sets (r2_sun_quality, r__smap_cascade0_size,
    // r4_enable_tessellation, r2_volumetric_lights …) is not read by a single
    // line of Layers/xrRenderVulkan, so picking a preset moved a page of
    // settings and changed no pixels — the same placebo the rest of this screen
    // suffered from. A NEW name (rspec_vk_*) cannot be shadowed by an archive
    // and says which renderer it belongs to.
    //
    // ⭐The file is content on purpose: what "High" means is a judgement about
    // this renderer's costs, and it will keep moving. Editing five text files
    // beats editing five arrays and rebuilding.
    virtual void Execute(LPCSTR args)
    {
        CapturePresetDefaults();
        CCC_Token::Execute(args);
        string_path leaf;
        if (*value != 5)
        {
            for (const auto& e : s_presetDefaults)
            {
                string2048 c;
                xr_sprintf(c, "%s %s", e.first.c_str(), e.second.c_str());
                Console->Execute(c);
            }
        }

        switch (*value)
        {
        case 0: xr_strcpy(leaf, "rspec_vk_minimum.ltx"); break;
        case 1: xr_strcpy(leaf, "rspec_vk_low.ltx"); break;
        case 2: xr_strcpy(leaf, "rspec_vk_default.ltx"); break;
        case 3: xr_strcpy(leaf, "rspec_vk_high.ltx"); break;
        case 4: xr_strcpy(leaf, "rspec_vk_extreme.ltx"); break;
        case 5: xr_strcpy(leaf, "rspec_vk_switch.ltx"); break;
        default: return;
        }

        string_path _cfg;
        FS.update_path(_cfg, fsgame::game_configs, leaf);
        // ⚠A warning, not a refusal: `exist` answers from the file registry and
        // the path here is absolute, so a false negative is possible — and
        // refusing on it would break a preset that is actually there. cfg_load
        // is harmless on a missing file (it opens nothing); this line is what
        // explains the silence afterwards.
        if (!FS.exist(_cfg))
            Msg("! [VK preset] '%s' not found by the file registry — if nothing changes, that is why", _cfg);

        string_path cmd;
        strconcat(sizeof(cmd), cmd, "cfg_load", " ", _cfg);
        Msg("~ [VK preset] %s", leaf);
        Console->Execute(cmd);
    }
};

// Vulkan stub — VMA owns all memory; expose a dump via vmaCalculateStatistics
// in a follow-up pass if needed. R4 version queries D3DPOOL_* counters.
class CCC_VideoMemoryStats : public IConsole_Command
{
public:
    CCC_VideoMemoryStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { VK::TextureStreamer::Instance().DumpStats(); }
};

// `r_vram_dump` — full VMA allocator JSON (every pool/block/allocation with
// names) to _appdata_\vma_stats.json. The scalpel for "who owns the vma-slack".
class CCC_VramDump : public IConsole_Command
{
public:
    CCC_VramDump(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { VK::Vram::DumpVmaJson(); }
};

// `vk_perf` — force the global profiler to print an immediate [VK Perf] MARK
// snapshot (per-pass GPU/CPU ms + VRAM + rain/wet state) the instant it's typed.
// Type it the moment FPS sits to capture exactly that frame's breakdown.
class CCC_VkPerf : public IConsole_Command
{
public:
    CCC_VkPerf(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR /*args*/) { VK::Prof::RequestMark(); }
};

#include "CRender_Vulkan.h"   // RImplementation.Trees — the `vk_trees` ledger command
#include "vk_TreeManager.h"
#include "vk_texture.h"       // VK::TexAudit::Dump — the `vk_texaudit` tally

// `vk_trees` — print the tree ledger NOW (r_tree_stats must be on, since the
// counters it reads are only filled while it is). Same reason as `vk_perf`: an
// A/B sweep wants the numbers at the instant a knob was changed, not up to five
// seconds later averaged across both settings.
class CCC_VkTrees : public IConsole_Command
{
public:
    CCC_VkTrees(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR /*args*/)
    {
        if (!RImplementation.Trees) { Msg("[VK Trees] no tree manager on this level"); return; }
        if (ps_r_tree_stats <= 0) {
            // Turning it on here rather than refusing: the request already says what the
            // user wants, and the ledger needs a frame of GPU work before it can answer.
            ps_r_tree_stats = 1;
            Msg("[VK Trees] r_tree_stats was 0 — enabled it; the ledger prints from the next frame");
        }
        RImplementation.Trees->StatsRequestMark();
    }
};

// `vk_texaudit` — print the texture audit (r_tex_audit must have been on while the
// level loaded; the dump says so when it was not, rather than printing zeros).
class CCC_VkTexAudit : public IConsole_Command
{
public:
    CCC_VkTexAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR /*args*/) { VK::TexAudit::Dump(); }
};

// `vk_pipestats` — print what the driver actually compiled the crown pipelines
// into (registers, occupancy, spill), for the ones this level built. Needs
// r_pipe_stats 1 set BEFORE the level loaded; the dump says so when it wasn't.
class CCC_VkPipeStats : public IConsole_Command
{
public:
    CCC_VkPipeStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR /*args*/)
    {
        if (!RImplementation.Trees) { Msg("[VK PipeStats] no tree manager on this level"); return; }
        if (ps_r_pipe_stats <= 0)
            Msg("![VK PipeStats] r_pipe_stats is 0 — the pipelines below were built WITHOUT capture; "
                "set it before `load <level>` and reload, or the report is empty by construction");
        RImplementation.Trees->DumpPipelineStats();
    }
};

// `vk_pass_deps` — print the frame's declared produce/consume table (which pass
// writes each named resource, which passes read it, which reads are History) and
// re-run the ordering check. The one place to look when asking "who produces this,
// and is my new pass in the right slot?".
class CCC_VkPassDeps : public IConsole_Command
{
public:
    CCC_VkPassDeps(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR) { VK::DumpPassDeps(); }
};

// Screenshot ON DEMAND. The renderer has always been able to capture the
// swapchain (CRender::Screenshot, writes $screenshots$/ss_*.tga) but the only
// trigger was a KEY BIND — useless to a scripted run, which has no keyboard.
// With this, a harness driving the client through XROS_CLIENT_CONSOLE can look
// at what it just built. `vk_shot <name>` writes that exact path instead.
// Throw a stone in the pond from the console: a disturbance a few metres in
// front of the camera. The one-line way to see whether the ripple sim is alive
// without wading into water first.
// r_wtr_audit — read the ripple mask + field back and report the CONNECTED
// bodies of water around the camera. Answers "are these puddles one pool joined
// off-screen?", which staring at the water cannot.
class CCC_WtrAudit : public IConsole_Command
{
public:
    CCC_WtrAudit(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR) { VK::WaterRipple::RequestAudit(); Msg("[VK Ripple] audit requested"); }
};

class CCC_WtrDrop : public IConsole_Command
{
public:
    CCC_WtrDrop(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        float dist = 4.f, str = 0.25f;
        if (args && args[0]) sscanf_s(args, "%f %f", &dist, &str);
        Fvector d = Device.vCameraDirection; d.y = 0.f;
        if (d.square_magnitude() > 1e-4f) d.normalize(); else d.set(0.f, 0.f, 1.f);
        const Fvector& c = Device.vCameraPosition;
        // A deliberate probe: it should land wherever you aim it, so it opts out
        // of the source-height test the same way rain does.
        VK::WaterRipple::Splat(c.x + d.x * dist, c.z + d.z * dist, 1.0f, -str,
                               VK::WaterRipple::kNoHeightTest);
        Msg("[VK Ripple] drop at (%.1f, %.1f) r=1.0 s=%.2f", c.x + d.x * dist, c.z + d.z * dist, str);
    }
};

class CCC_VkUiPassTrace : public IConsole_Command
{
public:
    CCC_VkUiPassTrace(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        u32 lines = 0;
        if (!args || 1 != sscanf(args, "%u", &lines)) { InvalidSyntax(); return; }
        VulkanUI::TraceUIPass(lines);
        Msg("~ [VK UIpass] tracing the next %u transition(s)", lines);
    }
    virtual void Info(TInfo& I) { strcpy_s(I, "<lines> -- traces that many Vulkan UI render-pass transitions"); }
};

// ===========================================================================
//  r_gi <0..3> — THE WHOLE INDIRECT-LIGHT CHAIN ON ONE LADDER.
// ===========================================================================
//  Five cvars had to be typed in the right order to see any of this, and every
//  A/B that mattered was "the same five, one of them different". That is not a
//  convenience problem: a five-part incantation typed by hand is how a run ends
//  up measuring the wrong configuration and nobody notices for two sessions
//  (r_wrc's tuning defaults spent exactly that long being overridden in silence).
//
//  Each STEP ADDS THE NEXT LINK of idTech 8's lookup chain, so the honest A/B for
//  any one of them is the step below it:
//
//    0  everything off — the raster's own ambient, the baseline
//    1  probe volume    (vk_ddgi)        — the 3 m lattice, cache #3
//    2  + final gather  (vk_pass_gather) — 1 ray per pixel; the screen cache (#1)
//                                          rides along, it is on by default
//    3  + world cache   (vk_wrc)         — cache #2, the 25 cm hash
//
//  So `r_gi 2` then `r_gi 3` isolates the world radiance cache and NOTHING else,
//  which is the comparison every question about it has come down to.
//
//  ⚠ Give it a second between steps. Stepping down tears the targets and the BVH
//  bubble down with it, and the rebuild takes longer than one frame — a snapshot
//  taken immediately measures the reload, not the feature.
class CCC_GI : public IConsole_Command
{
public:
    CCC_GI(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        // `off` is the COST baseline: everything down, producers included. Separate
        // from level 0 because the two answer different questions and conflating
        // them is what made every image A/B here measure a rebuild instead.
        if (args && (0 == _stricmp(args, "off"))) {
            ps_r_ddgi = 0; ps_r_ddgi_light = 0.f;
            ps_r_gather = 0; ps_r_gather_light = 0.f;
            ps_r_wrc = 0.f;
            ps_r_gi_froxel = 0.f;
            Msg("[VK GI] r_gi off -> everything torn down (targets, probe volume, BVH bubble). "
                "coming back costs a rebuild - use `r_gi 0` to compare IMAGES.");
            return;
        }

        int lvl = -1;
        if (!args || !args[0] || 1 != sscanf_s(args, "%d", &lvl)) {
            // No argument REPORTS rather than complains: half the value of this
            // command is answering "what is actually on right now" without reading
            // five separate cvars back one at a time.
            Msg("r_gi <0..3|off>: 0 baseline (chain hot, no contribution) | 1 probes"
                " | 2 + final gather | 3 + world cache | off = tear it all down");
            Msg("[VK GI] now: ddgi %d/%.2f, gather %d/%.2f, wrc %.2f, froxel field %.2f",
                ps_r_ddgi, ps_r_ddgi_light, ps_r_gather, ps_r_gather_light,
                ps_r_wrc, ps_r_gi_froxel);
            return;
        }
        // Out of range CLAMPS AND SAYS SO. Refusing outright is what `r_gi 4` got,
        // and the usage line it printed scrolled past unread while the chain stayed
        // off — a whole run spent on a typo. Every cvar in this console clamps to
        // its range; being the one command that does not is its own trap. Loud, not
        // silent, is the difference between a clamp and a half-executed command.
        const int want = lvl;
        lvl = _max(0, _min(3, lvl));
        if (lvl != want) Msg("![VK GI] r_gi %d is not a level — clamped to %d (the top of the ladder)", want, lvl);

        // ⭐⭐⭐ LEVEL 0 SWITCHES OFF THE CONTRIBUTION, NOT THE MACHINERY, and that is
        // the difference between an A/B that works and one that measures a reload.
        //
        // The first version cleared the producer gates too, which tore down the
        // gather's targets AND the BVH bubble. Toggling 0 and back then spent the
        // next second rebuilding an acceleration structure, refilling three probe
        // cascades and re-converging a temporal filter with a 0.92 history weight —
        // and the frame anyone looked at during that was neither state. The log of
        // the run that prompted this says it out loud: after the toggle the gather
        // reported `cutoff 15 m` and `screen cache WANTED BUT NO HISTORY`.
        //
        // Weights alone flip in ONE frame, with every producer still hot, which is
        // the only honest way to compare two images in a renderer where weather and
        // actor position drift between runs.
        // ⚠ The GPU still pays the full cost at level 0. This is an IMAGE baseline;
        // `r_gi off` is the cost one.
        ps_r_ddgi_light   = (lvl >= 1) ? 1.f : 0.f;
        ps_r_gather_light = (lvl >= 2) ? 1.f : 0.f;
        // The froxel field is a CONSUMER of the lattice, not another cache in the
        // chain, so it does not get a rung of its own — it comes up with the probes
        // and goes down with them. Level 0 must zero it like every other weight, or
        // the "contribution 0" baseline would still have GI in the air.
        ps_r_gi_froxel    = (lvl >= 1) ? 1.f : 0.f;
        if (lvl >= 1) ps_r_ddgi = 1;
        if (lvl >= 2) ps_r_gather = 1;
        if (lvl >= 3) ps_r_wrc = 1.f;
        // The cache is inert at level 0 anyway (both weights above are zero, so
        // nothing it answers reaches a pixel), so it is left RUNNING rather than
        // torn down — its table is 32 MB and rebuilding it means a device idle.
        if (lvl >= 1 && lvl < 3) ps_r_wrc = 0.f;

        static const char* kWhat[4] = {
            "baseline — chain still RUNNING, contribution 0 (use `r_gi off` for the cost baseline)",
            "probe volume (3 m lattice, cache #3)",
            "+ final gather (1 ray/pixel, screen cache #1 rides along)",
            "+ world radiance cache (25 cm hash, cache #2)"
        };
        Msg("[VK GI] r_gi %d -> %s%s", lvl, kWhat[lvl], lvl >= 1 ? ", BVH = render geometry" : "");
    }
};

// ================================================================ r_shadows ===
//
// ⭐ONE ROW FOR THE WHOLE SHADOW GROUP. The advanced page carries eighteen VSM
// entries and every one of them is a real cost, but nobody outside this file
// knows that `r_vsm_tree_wind_dist` is the radius at which a crown stops being
// read from the static cache and starts being re-rasterised every frame. A
// laptop needs that radius small and should not have to learn why.
//
// 📏Why this group and not another: measured on a Radeon 780M (pripyat_full,
// 2560x1600) VSM was 14.62 ms of a 75 ms frame — mark 2.46 + render 8.57 +
// resolve 3.59 — the largest block after the screen-space passes, and the ONLY
// large block that does not shrink when the render resolution does. The shadow
// atlas has a size of its own; r_render_scale never touches it.
//
// ⚠⚠STATUS IS DERIVED, NEVER STORED — the rule CCC_AAMode already follows here.
// Every entry below is savable on its own, so a stored tier token would be a
// SECOND authority over the same values: hand-edit user.ltx (or move one slider
// on the advanced page) and the two disagree, with the load order deciding which
// one the player actually gets. Deriving it also lets the row answer "Custom",
// which is the honest answer after somebody moves one slider by hand.
// ⚠That is also why the constructor calls SetCanSave(false): the default
// IConsole_Command::Save would write `r_shadows High` into user.ltx, and it
// sorts BEFORE the r_vsm_* lines only by accident of the alphabet.
namespace {

// The tier table. Read down a column to see one preset; read across a row to see
// what one entry costs across the range. ⚠Every value here is inside its CMD4
// clamp — this writes the variables DIRECTLY (like CCC_GI) and nothing would
// catch a violation: r_vsm_soft 0..32, _search 2..16, _range 2..80, base 8..64,
// tree_hull 0..2, _hull_dist 4..200, _wind_dist 8..200, grass_dist 4..48,
// npc_dist 0..500, cadence 0..4, _still 0..1, cache_refresh 1..64,
// throttle_budget 0.5..20, _max 0..4, shadow_lod_dist 5..200, spot_pool 1..8,
// point_pool 1..4.
struct ShadowTier
{
    const char* token;      // what the menu row and the console print
    const char* caption;    // one line on what this tier trades away
    int   vsm;              // r_vsm — master
    int   soft;             // r_vsm_soft: 0 = legacy 3x3 PCF, >0 = stochastic PCSS taps
    int   softSearch;       // r_vsm_soft_search — blocker-search taps
    float softRange;        // r_vsm_soft_range — widest penumbra, m
    int   hzb;              // r_vsm_hzb — occlusion-cull the casters
    float base;             // r_vsm_base — finest clipmap texel, m (bigger = coarser = cheaper)
    int   treeHull;         // r_vsm_tree_hull — 0 full crown / 1 far only / 2 everywhere
    float treeHullDist;     // r_vsm_tree_hull_dist — crown-mesh tier radius, m
    int   treeWind;         // r_vsm_tree_wind — near crowns sway ⇒ they live in the DYN atlas
    float treeWindDist;     // r_vsm_tree_wind_dist — THE static-handover radius for trees
    int   grass;            // r_vsm_grass — grass casts into the sun atlas at all
    float grassDist;        // r_vsm_grass_dist
    float npcDist;          // r_vsm_npc_dist
    int   cadence;          // r_vsm_cadence — dyn pass every Nth frame (0/1 = every frame)
    float cadenceStill;     // r_vsm_cadence_still — how still the camera must be to skip
    int   cacheRefresh;     // r_vsm_cache_refresh
    int   throttle;         // r_vsm_throttle — cost-feedback LOD bias
    float throttleBudget;   // r_vsm_throttle_budget — target VSMrender ms
    float throttleMax;      // r_vsm_throttle_max — how far the bias may go
    float lodDist;          // r_shadow_lod_dist — coarse caster geometry past this
    int   sunGrass;         // r_sun_grass
    int   spotGrass;        // r_spot_grass
    int   pointGrass;       // r_point_grass
    int   spotPool;         // r_spot_pool
    int   pointPool;        // r_point_pool
    int   nightFreeze;      // r_sun_night_freeze
};

// ⚠"Off" still writes the whole row. A player who turns shadows off and back on
// must land on a KNOWN tier rather than on whatever the previous one left
// behind — and the derived Status below needs something to match against.
//
// ⭐The three rows that matter on a weak GPU, in measured order:
//   soft 0      — legacy 3x3 PCF instead of a stochastic PCSS kernel costing
//                 taps+search per SCREEN pixel. Paid in World/VSMresolve (3.59 ms).
//   treeWind 0  — every crown inside treeWindDist is re-rasterised each frame in
//                 + small       the dyn atlas instead of being read from the static
//                 treeWindDist  cache. VSM/DynTrees was 4.20 of the 8.57 ms VSMrender.
//   cadence 2-3 — the dyn pass runs every Nth frame while the camera is still.
// ⚠throttleMax is raised, not lowered, on the cheap tiers: r_vsm_throttle is a
// cost-FEEDBACK bias and on the 780M it was already pinned at its 2.0 cap with
// VSMrender at 8.57 ms against a 3.0 ms budget. Capping it lower does nothing;
// letting it go further is the point.
constexpr ShadowTier kShadowTiers[] = {
    // Off — the master down. The rest is still filled in, see above.
    {"Off",    "sun shadows off",
     0,  0, 2,  8.f, 1, 32.f, 2, 10.f, 0,  8.f, 0,  8.f, 25.f, 3, 0.25f, 16, 1, 2.f, 4.f, 15.f, 0, 0, 0, 1, 1, 1},
    // Low — the laptop tier: PCF, no grass shadow anywhere, every crown a hull
    // served from the static cache, dyn pass at a third of the frame rate.
    {"Low",    "laptop: PCF, no grass casters, hull crowns, 1/3 dyn rate",
     1,  0, 2,  8.f, 1, 32.f, 2, 10.f, 0,  8.f, 0,  8.f, 25.f, 3, 0.25f, 16, 1, 2.f, 4.f, 15.f, 0, 0, 0, 2, 1, 1},
    {"Medium", "coarse PCSS, grass still out of the sun atlas",
     1,  4, 2, 12.f, 1, 28.f, 2, 14.f, 1, 20.f, 0, 16.f, 35.f, 2, 0.10f, 12, 1, 3.f, 3.f, 20.f, 0, 0, 0, 3, 2, 1},
    // High — what this build shipped as its default before the tiers existed.
    {"High",   "PCSS, grass casts into the sun atlas",
     1,  8, 4, 15.f, 0, 24.f, 1, 18.f, 1, 40.f, 1, 32.f, 50.f, 1, 0.02f,  8, 1, 4.f, 2.f, 30.f, 1, 1, 1, 6, 3, 1},
    {"Ultra",  "full crown meshes, widest penumbra, no cost throttle",
     1, 16, 6, 20.f, 0, 16.f, 0, 24.f, 1, 60.f, 1, 48.f, 80.f, 1, 0.00f,  4, 0, 6.f, 2.f, 50.f, 1, 1, 1, 8, 4, 1},
};
constexpr int kShadowTierCount = int(sizeof(kShadowTiers) / sizeof(kShadowTiers[0]));

void ApplyShadowTier(const ShadowTier& t)
{
    ps_r_vsm                 = t.vsm;
    ps_r_vsm_soft            = t.soft;
    ps_r_vsm_soft_search     = t.softSearch;
    ps_r_vsm_soft_range      = t.softRange;
    ps_r_vsm_hzb             = t.hzb;
    ps_r_vsm_base            = t.base;
    ps_r_vsm_tree_hull       = t.treeHull;
    ps_r_vsm_tree_hull_dist  = t.treeHullDist;
    ps_r_vsm_tree_wind       = t.treeWind;
    ps_r_vsm_tree_wind_dist  = t.treeWindDist;
    ps_r_vsm_grass           = t.grass;
    ps_r_vsm_grass_dist      = t.grassDist;
    ps_r_vsm_npc_dist        = t.npcDist;
    ps_r_vsm_cadence         = t.cadence;
    ps_r_vsm_cadence_still   = t.cadenceStill;
    ps_r_vsm_cache_refresh   = t.cacheRefresh;
    ps_r_vsm_throttle        = t.throttle;
    ps_r_vsm_throttle_budget = t.throttleBudget;
    ps_r_vsm_throttle_max    = t.throttleMax;
    ps_r_shadow_lod_dist     = t.lodDist;
    ps_r_sun_grass           = t.sunGrass;
    ps_r_spot_grass          = t.spotGrass;
    ps_r_point_grass         = t.pointGrass;
    ps_r_spot_pool           = t.spotPool;
    ps_r_point_pool          = t.pointPool;
    ps_r_sun_night_freeze    = t.nightFreeze;
}

// ⭐Which tier the CURRENT values are. Only the entries that actually separate
// the tiers are compared: matching on all twenty-six would report "Custom" the
// moment anything unrelated moved, and a row whose whole job is to say where you
// are must not cry wolf. `Off` is decided by the master alone, so switching the
// group off from its own checkbox still lands the row on a named tier.
int CurrentShadowTier()
{
    if (!ps_r_vsm)
        return 0;   // Off
    for (int i = 1; i < kShadowTierCount; ++i)
    {
        const ShadowTier& t = kShadowTiers[i];
        const bool sameBase = ps_r_vsm_base > t.base - 0.01f && ps_r_vsm_base < t.base + 0.01f;
        if (ps_r_vsm_soft == t.soft && ps_r_vsm_grass == t.grass
            && ps_r_vsm_tree_hull == t.treeHull && ps_r_vsm_tree_wind == t.treeWind
            && ps_r_vsm_cadence == t.cadence && ps_r_vsm_hzb == t.hzb && sameBase)
            return i;
    }
    return -1;   // Custom
}

} // namespace

class CCC_Shadows : public IConsole_Command
{
public:
    CCC_Shadows(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = TRUE;
        SetCanSave(false);   // see the "derived, never stored" note above
    }

    // ⚠The options screen picks the selected row by comparing this string to the
    // row's value, so it must print a TOKEN and nothing else. A sentence here
    // leaves the list with nothing selected, which reads on screen as "the
    // setting reset itself".
    virtual void Status(TStatus& S)
    {
        const int i = CurrentShadowTier();
        xr_strcpy(S, i >= 0 ? kShadowTiers[i].token : "Custom");
    }

    virtual void Info(TInfo& I) { xr_strcpy(I, "shadow quality tier: Off / Low / Medium / High / Ultra"); }

    // What the console's tab-completion offers. The options list gets its rows
    // from data-values in the markup, which is also where their Russian captions
    // live — a token is machine-readable and makes a poor label.
    virtual void fill_tips(vecTips& tips, u32)
    {
        for (const ShadowTier& t : kShadowTiers)
            tips.push_back(t.token);
    }

    virtual void Execute(LPCSTR args)
    {
        if (!args || !args[0])
        {
            // No argument REPORTS rather than complains — same reasoning as r_gi.
            // Half the value of a group command is answering "what is on right
            // now" without reading twenty-six entries back one at a time.
            const int cur = CurrentShadowTier();
            Msg("r_shadows <Off|Low|Medium|High|Ultra>  (now: %s)", cur >= 0 ? kShadowTiers[cur].token : "Custom");
            Msg("[VK Shadows] vsm %d | filter %s (soft %d, search %d) | hull %d @ %.0fm | wind %d @ %.0fm | "
                "grass %d @ %.0fm | npc %.0fm | cadence %d/%.2f | hzb %d | base %.1f | throttle %d (%.1f ms, max %.1f)",
                ps_r_vsm, ps_r_vsm_soft ? "PCSS" : "PCF 3x3", ps_r_vsm_soft, ps_r_vsm_soft_search,
                ps_r_vsm_tree_hull, ps_r_vsm_tree_hull_dist, ps_r_vsm_tree_wind, ps_r_vsm_tree_wind_dist,
                ps_r_vsm_grass, ps_r_vsm_grass_dist, ps_r_vsm_npc_dist,
                ps_r_vsm_cadence, ps_r_vsm_cadence_still, ps_r_vsm_hzb, ps_r_vsm_base,
                ps_r_vsm_throttle, ps_r_vsm_throttle_budget, ps_r_vsm_throttle_max);
            return;
        }

        // "Custom" is what Status PRINTS when the values match no tier, so it
        // has to be a row in the menu list or the list would show nothing
        // selected after any hand tweak -- which reads on screen as "the
        // setting reset itself". Picking it back is therefore a no-op: it is
        // a description of where you are, not a place to go.
        if (0 == _stricmp(args, "Custom"))
        {
            Msg("~ [VK Shadows] Custom - the entries below this row are the truth; pick a tier to overwrite them");
            return;
        }

        for (int i = 0; i < kShadowTierCount; ++i)
        {
            if (0 != _stricmp(args, kShadowTiers[i].token))
                continue;
            ApplyShadowTier(kShadowTiers[i]);
            Msg("~ [VK Shadows] %s - %s", kShadowTiers[i].token, kShadowTiers[i].caption);
            return;
        }
        // ⚠Loud, not silent. A half-parsed group command leaves the player
        // looking at a row that moved and a picture that did not.
        Msg("! [VK Shadows] '%s' is not a tier. Use: Off | Low | Medium | High | Ultra", args);
    }
};

class CCC_VkShot : public IConsole_Command
{
public:
    CCC_VkShot(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (!::Render) { Msg("![VK] vk_shot: no renderer"); return; }
        ::Render->Screenshot(IRender_interface::SM_NORMAL, (args && args[0]) ? args : nullptr);
    }
};

// An integer cvar that is NEVER written to user.ltx. Debug views are set for one
// measurement and forgotten; the client saves the console on exit, so a savable
// one strands the next launch in whatever the last experiment left behind (the
// water pass shipped magenta this way once).
class CCC_IntegerNoSave : public CCC_Integer
{
public:
    CCC_IntegerNoSave(LPCSTR N, int* V, int _min, int _max) : CCC_Integer(N, V, _min, _max)
    {
        SetCanSave(FALSE);
    }
};

// The same for a float, and for a second reason: a cvar that is really a TUNING
// CONSTANT of some scheme (a damping factor, a stability limit) has its true home
// in the source, next to the reasoning for the number. Make it savable and the
// first value ever written to user.ltx outlives every later default silently —
// the machine keeps running the old physics while the code, the comments and the
// notes all describe the new. That cost a day: r_wtr_sim_damp was lowered
// 0.9985 -> 0.97 to stop one footstep ringing a whole 20 x 13 m slab, and the
// only machine it mattered on went on loading 0.9985 from its own config.
class CCC_FloatNoSave : public CCC_Float
{
public:
    CCC_FloatNoSave(LPCSTR N, float* V, float _min, float _max) : CCC_Float(N, V, _min, _max)
    {
        SetCanSave(FALSE);
    }
};

class CCC_SunshaftsIntensity : public CCC_Float
{
public:
    CCC_SunshaftsIntensity(LPCSTR N, float* V, float _min, float _max) : CCC_Float(N, V, _min, _max)
    {
        SetCanSave(FALSE);
    }
};

// GPU-particles Phase 3 — `gp_mirror <effect>` translates a real .pe into the
// GPU action program and runs it on the camera-pinned test path; no arg reverts
// to the authored campfire. Forward-declared to keep the GP module headers out.
namespace VK { namespace GPUParticles { bool MirrorEffect(const char* name); void DebugEffect(const char* name); void DumpStats(); } }
class CCC_GP_Mirror : public IConsole_Command
{
public:
    CCC_GP_Mirror(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR args)
    {
        string_path name;
        xr_strcpy(name, args ? args : "");
        // Trim leading spaces the console may leave before the argument.
        char* p = name;
        while (*p == ' ') ++p;
        VK::GPUParticles::MirrorEffect(p);
    }
};

// `gp_stats` — per-program budget/alive counters to the log.
class CCC_GP_Stats : public IConsole_Command
{
public:
    CCC_GP_Stats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR /*args*/) { VK::GPUParticles::DumpStats(); }
};

// `gp_debug <effect>` — dump the .pe def + its GPU translation to the log.
class CCC_GP_Debug : public IConsole_Command
{
public:
    CCC_GP_Debug(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR args)
    {
        string_path name;
        xr_strcpy(name, args ? args : "");
        char* p = name;
        while (*p == ' ') ++p;
        if (!*p) { Msg("![VK GP] gp_debug: usage 'gp_debug <effect>' (see gp_list)"); return; }
        VK::GPUParticles::DebugEffect(p);
    }
};

// GPU-particles Phase 3 step 2 — `gp_spawn <effect>` places a persistent WORLD
// emitter of the effect at the current camera position (does not follow the
// camera); `gp_spawn_clear` removes all such emitters. Many can coexist.
namespace VK { namespace GPUParticles { bool SpawnEffect(const char* name); void ClearSpawns(); } }
class CCC_GP_Spawn : public IConsole_Command
{
public:
    CCC_GP_Spawn(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR args)
    {
        string_path name;
        xr_strcpy(name, args ? args : "");
        char* p = name;
        while (*p == ' ') ++p;
        VK::GPUParticles::SpawnEffect(p);
    }
};
class CCC_GP_SpawnClear : public IConsole_Command
{
public:
    CCC_GP_SpawnClear(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR /*args*/) { VK::GPUParticles::ClearSpawns(); }
};

// `gp_list [substr]` — print loaded .pe effect names (optionally filtered) so
// you can discover real names to feed `gp_mirror`. Library enumeration lives in
// the compat TU (vk_PSLibrary.cpp); call it via its plain extern.
void VK_ParticleEffectFillName(xr_vector<shared_str>& dest);
class CCC_GP_List : public IConsole_Command
{
public:
    CCC_GP_List(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    virtual void Execute(LPCSTR args)
    {
        string256 filt;
        xr_strcpy(filt, args ? args : "");
        char* f = filt; while (*f == ' ') ++f;
        _strlwr(f);

        xr_vector<shared_str> names;
        VK_ParticleEffectFillName(names);

        u32 shown = 0;
        const u32 cap = 300;
        for (const shared_str& s : names) {
            if (!s.size()) continue;
            if (f[0]) {
                string256 lower; xr_strcpy(lower, s.c_str()); _strlwr(lower);
                if (!strstr(lower, f)) continue;
            }
            if (shown < cap) Msg("  %s", s.c_str());
            ++shown;
        }
        Msg("[VK GPUParticles] gp_list: %u effect(s)%s%s%s (of %u total)%s",
            shown, f[0] ? " matching '" : "", f[0] ? f : "", f[0] ? "'" : "",
            (u32)names.size(), shown > cap ? " — output capped at 300" : "");
    }
};

// ============================================================ r_aa_mode ====
//
// ⚠⚠THE ROW THAT LIED. `r_aa_mode` was a token over ps_r_pp_aa_mode, offering
// off / DLSS / FSR2 / TAA / SMAA — and NOT ONE LINE of the Vulkan renderer ever
// read that variable. Picking "DLSS" in the options screen changed a number in
// memory, saved it to user.ltx, and left the image untouched. (Two of the four
// techniques it offered do not exist in this renderer at all.)
//
// What does work is a pair: `r_dlss` (master) and `r_dlss_quality`
// (0=DLAA … 4=UltraPerf, i.e. how far below the display the scene renders).
// A player has no business knowing that the second means nothing while the first
// is off, so this command owns both and Status() derives the row back from them
// — which is what keeps the menu honest after a cfg_load or a console poke.
enum
{
    AA_OFF = 0,
    AA_DLAA = 1,       // render == display, DLSS used purely as an antialiaser
    AA_DLSS_Q = 2,
    AA_DLSS_B = 3,
    AA_DLSS_P = 4,
    AA_DLSS_UP = 5,
};
constexpr xr_token aa_mode_token[] = {
    {"st_opt_off", AA_OFF},
    {"dlaa", AA_DLAA},
    {"dlss_quality", AA_DLSS_Q},
    {"dlss_balanced", AA_DLSS_B},
    {"dlss_performance", AA_DLSS_P},
    {"dlss_ultra_perf", AA_DLSS_UP},
    {},
};

class CCC_AAMode : public CCC_Token
{
public:
    CCC_AAMode(LPCSTR N, u32* V, const xr_token* T) : CCC_Token(N, V, T) {}

    void Execute(LPCSTR args) override
    {
        // ⚠Every user.ltx in existence holds one of the OLD names (this install
        // has `r_aa_mode st_opt_dlss`). Rejecting them would spray InvalidSyntax
        // over every startup and, worse, leave the setting at whatever the
        // default was. Map them instead: anything that used to mean "some kind of
        // temporal AA is on" now means DLSS on, at the quality already stored.
        u32 want = u32(-1);
        for (const xr_token* t = aa_mode_token; t->name; ++t)
            if (_stricmp(t->name, args) == 0)
                want = (u32)t->id;

        if (want == u32(-1))
        {
            if (_stricmp(args, "st_opt_dlss") == 0 || _stricmp(args, "st_opt_taa") == 0 || _stricmp(args, "st_opt_smaa") == 0 ||
                _stricmp(args, "st_opt_fsr2") == 0)
                want = (ps_r_dlss_quality <= 4) ? u32(ps_r_dlss_quality + 1) : u32(AA_DLAA);
            else
            {
                InvalidSyntax();
                return;
            }
        }

        Apply(want);
    }

    // ⭐Derived, never stored. The two variables underneath are also plain
    // console entries (r_dlss / r_dlss_quality are how the renderer was A/B'd
    // for months and they stay), so anything that writes them directly must not
    // be able to leave this row showing something else.
    void Status(TStatus& S) override
    {
        u32 v = u32(AA_OFF);
        if (ps_r_dlss)
            v = (ps_r_dlss_quality <= 4) ? u32(ps_r_dlss_quality + 1) : u32(AA_DLAA);
        *value = v;
        for (const xr_token* t = aa_mode_token; t->name; ++t)
            if ((u32)t->id == v)
            {
                xr_strcpy(S, t->name);
                return;
            }
        xr_strcpy(S, "st_opt_off");
    }

private:
    void Apply(u32 v)
    {
        *value = v;
        if (v == AA_OFF)
        {
            ps_r_dlss = 0;
        }
        else
        {
            ps_r_dlss = 1;
            ps_r_dlss_quality = v - 1; // AA_DLAA → 0 (native), … AA_DLSS_UP → 4
            // ⚠DLSS is fed by the motion-vector pass; without it Dlss::Upscaling()
            // stays false and the whole thing silently degrades to "nothing
            // happened". Turning the prerequisite on with the feature is the
            // difference between a setting and a riddle.
            if (!ps_r_motion_vectors)
            {
                ps_r_motion_vectors = 1;
                Msg("~ [VK AA] r_motion_vectors turned on — DLSS needs it");
            }
            if (ps_r_dlss_avail == 0) // 0 = probed and said no; -1 = not probed yet (startup)
                Msg("~ [VK AA] DLSS requested, but NGX reports it unavailable on this GPU/driver — the renderer will keep it off");
        }
    }
};

void xrRender_initconsole()
{
    CMD3(CCC_Preset, "_preset", &ps_preset, qpreset_token);
    CMD1(CCC_PresetDefaults, "_preset_defaults");

    CMD4(CCC_Integer, "rs_skeleton_update", &psSkeletonUpdate, 2, 128);

    // ⚠vk_shot lives OUTSIDE the DEBUG block, and it used to sit inside it next
    // to dump_resources -- the same trap fs_extract fell into. The command
    // exists so a scripted run can look at what it built, and the builds worth
    // looking at are RELEASE ones: that is where the mod's content is, and where
    // a UI port is checked. Registered in DEBUG only, it was missing from every
    // run that needed it and reported itself as "Unknown command".
    CMD1(CCC_VkShot, "vk_shot");
    CMD1(CCC_VkUiPassTrace, "ui_pass_trace");

    CMD4(CCC_Float, "r__wallmark_ttl", &ps_r__WallmarkTTL, 1.0f, 10.f * 60.f);

    CMD4(CCC_Float, "r__geometry_lod", &ps_r__LOD, 0.1f, 1.0f);

    CMD4(CCC_Integer, "r__detail_radius", &ps_r__detail_radius, 20, 300);   // nfs24: min 20 so the Switch preset can set ~45 m (default unchanged)
    // Vulkan grass reseeds itself on the next CPU update tick, so a plain float is enough
    // (R4 pinged RImplementation.Details->need_init here).
    CMD4(CCC_Float, "r__detail_density", &ps_current_detail_density, 0.2f, 0.9f);
    CMD4(CCC_Float, "r__detail_scale", &ps_current_detail_scale, 0.7f, 1.5f);

    CMD4(CCC_Float, "r2_no_details_radius", &ps_r2_no_details_radius, 0.f, 5.f);
    CMD4(CCC_Float, "r2_no_rain_radius", &ps_r2_no_rain_radius, 0.f, 5.f);

    CMD4(CCC_Integer, "r__tf_aniso", &ps_r__tf_Anisotropic, 1, 16);

    CMD4(CCC_Float, "r__ssa_glod_start", &ps_r__GLOD_ssa_start, 128, 1024);
    CMD4(CCC_Float, "r__ssa_glod_end", &ps_r__GLOD_ssa_end, 16, 256);

    CMD4(CCC_Float, "r2_ssa_discard", &ps_r__ssaDISCARD, 0.5f, 10);

    // Sunshafts
    CMD4(CCC_SunshaftsIntensity, "r_sunshafts_intensity", &ps_r_sunshafts_intensity, 0.0f, 5.0f); // Dbg

    CMD3(CCC_Token, "r2_ssao", &ps_r_ao_quality, qssao_token);
    // ⚠⚠NoSave SINCE 27-08, THE THIRD LATCH-UP. A stale `r_ssao 0` in user.ltx
    // kept coming back (nothing in code writes it — the console path saved it
    // once and the file re-applied it every launch), silently killing GTAO/SSIL
    // — and once, the whole GI gate chain (see ao-switch-silently-killed-gi).
    // Fourth instance of [[gate-cvars-must-not-be-savable]]. The stale line must
    // also be removed from user.ltx once, or it keeps applying on load anyway.
    CMD4(CCC_IntegerNoSave, "r_ssao", &ps_r_ssao_enable, 0, 1);  // global GTAO on/off (perf A/B + real off)
    // ⚠⚠BOTH NoSave, and both were found IN user.ltx on 20-08 -- `r_ssao_debug 6`
    // and `r_ssao_static_normals 1`, left there by a tuning run days earlier. The
    // debug one is the expensive half: while it is non-zero every 300th frame
    // reads back 921600 texels and writes a 1.8 MB ssao_dump.bin to disk, a ~130 ms
    // hitch that had been happening in EVERY session with nobody asking for it.
    // The A/B one silently overrode its own code default of 0, so "default
    // behaviour" had not actually been running either.
    // ⚠Removing savability does NOT remove the lines already in user.ltx -- they
    // still execute at start; those two were deleted by hand.
    CMD4(CCC_IntegerNoSave, "r_ssao_debug", &ps_r_ssao_debug, 0, 6);   // 1=AO map, 2=depth, 3=normal, 4=fully-open AO, 5=normal SOURCE map (black=recon, grey=statics, white=NPC), 6=recon-vs-real angle (avg x45 = degrees)
    CMD4(CCC_Float, "r_ssao_strength", &ps_r_ssao_strength, 0.f, 4.f);
    CMD4(CCC_Float, "r_ssao_temporal", &ps_r_ssao_temporal, 0.f, 0.97f);  // AO temporal accumulation α (0=off; jitter+MV-reprojected EMA; needs r_motion_vectors for moving cam)
    CMD4(CCC_IntegerNoSave, "r_ssao_reproj", &ps_r_ssao_reproj, 0, 2);    // history reprojection: 2=auto (the MV target when r_mv_prepass resolved it this frame, else depth+prevVP), 1=always depth+prevVP in the blur, 0=always the MV target
    CMD4(CCC_Float, "r_ssao_bias", &ps_r_ssao_bias, 0.f, 0.9f);           // grazing-fade N·V threshold (fades AO→open at grazing → kills flat-floor bands at any strength; 0=off)

    // RTAO (hardware ray query over a camera-local BLAS of level.cform).
    CMD4(CCC_IntegerNoSave, "r_rtao", &ps_r_rtao, 0, 1);                   // master gate — NoSave so it can never stick in user.ltx during an A/B
    CMD4(CCC_IntegerNoSave, "r_rtao_debug", &ps_r_rtao_debug, 0, 1);       // 1 = show the traced term alone (overwrite instead of min over GTAO)
    CMD4(CCC_Integer, "r_rtao_rays", &ps_r_rtao_rays, 1, 16);              // rays per half-res pixel
    CMD4(CCC_Float, "r_rtao_radius", &ps_r_rtao_radius, 0.1f, 16.f);       // ray length (m)
    CMD4(CCC_Float, "r_rtao_dist", &ps_r_rtao_dist, 2.f, 200.f);           // view-distance cutoff (m)
    CMD4(CCC_Float, "r_rtao_bias", &ps_r_rtao_bias, 0.f, 0.5f);            // ray-origin offset along N (m)
    CMD4(CCC_Float, "r_rtao_power", &ps_r_rtao_power, 0.1f, 4.f);          // pow() shaping
    CMD4(CCC_Float, "r_rtao_range", &ps_r_rtao_range, 16.f, 256.f);        // BLAS bubble half-extent (m) — a change forces a rebuild
    CMD4(CCC_Integer, "r_rtao_maxtris", &ps_r_rtao_maxtris, 64, 8000);     // soup cap, thousands of tris
    CMD4(CCC_Integer, "r_rtao_filter", &ps_r_rtao_filter, 0, 1);           // drop suppress_shadows (collision-only) tris

    CMD4(CCC_Integer, "r_rt_dyn", &ps_r_rt_dyn, 0, 1);                     // skeletons (NPC/weapons/doors/props) in the BVH for RTAO (vk_rt_dyn)
    CMD4(CCC_Float, "r_rt_lod", &ps_r_rt_lod, 10.f, 2000.f);               // RT LOD error scale (bigger = finer, more triangles)
    CMD4(CCC_Float, "r_rt_tree_range", &ps_r_rt_tree_range, 0.f, 256.f);   // foliage-only bubble half-extent (m); 0 = the full r_rtao_range — a change forces a rebuild

    CMD4(CCC_IntegerNoSave, "r_rtgi_debug", &ps_r_rtgi_debug, 0, 6);       // RT preview: 1 shaded, 2 albedo, 3 normal, 4 distance, 5 DDGI irradiance, 6 albedo*irradiance (NoSave — it overwrites the frame)
    CMD4(CCC_Float, "r_rtgi_debug_dist", &ps_r_rtgi_debug_dist, 2.f, 400.f); // primary-ray range (m)

    // ⚠⚠⚠THE WHOLE r_ddgi_* GROUP IS NoSave, AND IT TOOK FOUR SEPARATE INCIDENTS
    // TO GET HERE. Same scar as the r_wrc_* block below, one arc later.
    //   1. r_ddgi_lights 0 stuck in user.ltx and the probes shaded with no dynamic
    //      lights for hours while the log said `cap 0` and nobody read it.
    //   2. r_ddgi_light_bake 0.3 stuck the same way, and because its partner
    //      r_wrc_skyvis is NoSave, the pair CAME APART on every restart.
    //   3. Removing those two from user.ltx handed r_ddgi_light_bake back its
    //      default 1.0, which multiplies the probe answer by bakeSky — zero in any
    //      interior — and GI went bit-identical between r_gi 0 and r_gi 3.
    //   4. r_ddgi_spacing 3.0 was written back by a session that was still running
    //      when the source default became 1.0, so the next launch silently got a
    //      lattice too coarse for interiors — which does not read as "dark", it
    //      reads as PROBE LEAK: probes standing outside the walls pass the
    //      Chebyshev test and pour open sky into the room.
    // The values are ONE measured configuration and they live in source, where a
    // diff shows them. A config file that can override half of them is not a
    // convenience, it is a way to run an experiment you did not design.
    CMD4(CCC_IntegerNoSave, "r_ddgi", &ps_r_ddgi, 0, 1);                   // probe volume master gate
    // ⚠reallocates the atlases; each cascade doubles spacing and reach. Ceiling is 6
    // (was 4) so a FINE r_ddgi_spacing can keep its reach: reach = dim_xz * spacing *
    // 2^(casc-1), so the 1 m spacing an interior needs costs 3 cascades' worth of
    // range unless the ladder is longer. See the INDOORS note in ddgi_common.glsl.
    CMD4(CCC_IntegerNoSave, "r_ddgi_cascades", &ps_r_ddgi_cascades, 1, 6);
    CMD4(CCC_IntegerNoSave, "r_ddgi_dim_xz", &ps_r_ddgi_dim_xz, 4, 64);          // ⚠reallocates the atlases (per cascade)
    CMD4(CCC_IntegerNoSave, "r_ddgi_dim_y", &ps_r_ddgi_dim_y, 2, 32);            // ⚠reallocates the atlases (per cascade)
    CMD4(CCC_IntegerNoSave, "r_ddgi_rays", &ps_r_ddgi_rays, 8, 64);              // ⚠reallocates the ray buffer
    CMD4(CCC_FloatNoSave, "r_ddgi_spacing", &ps_r_ddgi_spacing, 0.5f, 8.f);      // metres between probes
    CMD4(CCC_FloatNoSave, "r_ddgi_hysteresis", &ps_r_ddgi_hysteresis, 0.f, 0.995f);
    CMD4(CCC_FloatNoSave, "r_ddgi_norm_bias", &ps_r_ddgi_norm_bias, 0.f, 2.f);
    CMD4(CCC_FloatNoSave, "r_ddgi_view_bias", &ps_r_ddgi_view_bias, 0.f, 2.f);
    CMD4(CCC_FloatNoSave, "r_ddgi_depth_sharp", &ps_r_ddgi_depth_sharp, 1.f, 200.f);
    CMD4(CCC_FloatNoSave, "r_ddgi_probe_dist", &ps_r_ddgi_probe_dist, 1.f, 200.f);
    CMD4(CCC_FloatNoSave, "r_ddgi_energy", &ps_r_ddgi_energy, 0.f, 8.f);
    CMD4(CCC_FloatNoSave, "r_ddgi_clamp", &ps_r_ddgi_clamp, 0.1f, 64.f);
    CMD4(CCC_FloatNoSave, "r_rt_albedo", &ps_r_rt_albedo, 0.25f, 3.f);      // GI-only reflectance scale (1 = the texture as authored)
    CMD4(CCC_FloatNoSave, "r_gi_indoor", &ps_r_gi_indoor, 1.f, 6.f);            // reflectance boost under a ceiling, at full daylight (1 = off)
    CMD4(CCC_FloatNoSave, "r_gi_indoor_night", &ps_r_gi_indoor_night, 1.f, 6.f); // the same, with the sun gone; the engine mixes by outdoor illumination
    CMD4(CCC_FloatNoSave, "r_gi_indoor_dist", &ps_r_gi_indoor_dist, 0.f, 16.f); // how far straight up the ceiling test looks (m)
    CMD4(CCC_FloatNoSave, "r_ddgi_bounce", &ps_r_ddgi_bounce, 0.f, 1.f);         // multi-bounce feedback from last frame's field
    // ⚠⚠⚠THESE TWO WERE SAVABLE AND IT COST A SESSION. r_ddgi_lights and
    // r_ddgi_light_bake landed in user.ltx from an A/B run and then OVERRODE the
    // source defaults (16 and 1.0) at every later launch — the log said
    // `dynamic lights in the RT world: 0 shaded (of 0 collected, cap 0)` for hours
    // while everyone read it as a feature that did nothing. Worse, r_ddgi_light_bake
    // has to be tuned AGAINST r_wrc_skyvis (both occlude interiors, one measured and
    // one baked), and r_wrc_skyvis is NoSave — so the pair silently CAME APART on
    // every restart: the bake stuck at its tuned value, the sky-vis reset to 2.
    // A knob that must be held in step with a NoSave knob cannot itself be savable.
    // Third instance of [[gate-cvars-must-not-be-savable]]; see the same note over
    // the r_wrc_* block below.
    CMD4(CCC_IntegerNoSave, "r_ddgi_lights", &ps_r_ddgi_lights, 0, 64);    // dynamic lights the probe rays shade with (0 = sun+sky only)
    // PROBE RELOCATION. All NoSave, and the gate especially: a stale 1 in user.ltx
    // would make every later launch measure a field nobody asked for, and the wipe
    // it triggers would look like a hitch of unknown origin.
    CMD4(CCC_IntegerNoSave, "r_ddgi_reloc", &ps_r_ddgi_reloc, 0, 1);              // probes slide inside their cell towards open air
    CMD4(CCC_FloatNoSave, "r_ddgi_reloc_near", &ps_r_ddgi_reloc_near, 0.02f, 0.5f);   // "wall in my face" radius, in spacings
    CMD4(CCC_FloatNoSave, "r_ddgi_reloc_frac", &ps_r_ddgi_reloc_frac, 0.05f, 1.f);    // share of rays that close before it counts as buried
    CMD4(CCC_FloatNoSave, "r_ddgi_reloc_step", &ps_r_ddgi_reloc_step, 0.01f, 0.45f);  // travel per update, in spacings
    CMD4(CCC_FloatNoSave, "r_ddgi_reloc_max", &ps_r_ddgi_reloc_max, 0.f, 0.49f);      // cap on |offset|, in spacings
    CMD4(CCC_FloatNoSave, "r_ddgi_reloc_back", &ps_r_ddgi_reloc_back, 0.f, 1.f);      // 0 = ignore the winding; >0 = believe backface hits
    CMD4(CCC_FloatNoSave, "r_ddgi_reloc_decay", &ps_r_ddgi_reloc_decay, 0.f, 1.f);    // drift back to the lattice when comfortable
    CMD4(CCC_IntegerNoSave, "r_ddgi_crush_tri", &ps_r_ddgi_crush_tri, 0, 1);      // trilinear weight applied after the crush (RTXGI order), 0 = crush the product
    CMD4(CCC_IntegerNoSave, "r_ddgi_fixed_rays", &ps_r_ddgi_fixed_rays, 0, 32);   // ⚠reallocates — unrotated relocation/classification rays per probe (0 = use the rotated set)
    CMD4(CCC_IntegerNoSave, "r_ddgi_sched", &ps_r_ddgi_sched, 0, 1);              // 0 = round-robin cascades, 1 = cascade 0 every other frame
    CMD4(CCC_FloatNoSave, "r_ddgi_edge_clamp", &ps_r_ddgi_edge_clamp, 0.f, 16.f); // cap the coarse cascade at k x the fine one in the cross-fade — kills "the distant room glows street" at the window edge (0 = off, the A/B)
    CMD4(CCC_FloatNoSave, "r_gather_field_clamp", &ps_r_gather_field_clamp, 0.f, 16.f); // cap the gather's raw-field pixels at k x the world cache's answer — same leak past the finest cascades (0 = off, the A/B)
    CMD4(CCC_IntegerNoSave, "r_gi_probe", &ps_r_gi_probe, 0, 1);                  // GI pixel debugger: numeric dump of the crosshair surface to the log, ~1/s
    // SEEING THE PROBES. NoSave like every other debug view here — a view left on
    // in user.ltx would draw over the next session's screenshots.
    CMD4(CCC_IntegerNoSave, "r_ddgi_show_probes", &ps_r_ddgi_show_probes, 0, 3);      // 1 = colour by irradiance, 2 = by state, 3 = power ramp
    CMD4(CCC_IntegerNoSave, "r_ddgi_show_rays", &ps_r_ddgi_show_rays, 0, 2);          // 1 = radiance, 2 = power ramp
    CMD4(CCC_IntegerNoSave, "r_ddgi_show_xray", &ps_r_ddgi_show_xray, 0, 1);          // 1 = ignore depth (see probes inside walls)
    CMD4(CCC_FloatNoSave, "r_ddgi_show_dist", &ps_r_ddgi_show_dist, 1.f, 200.f);      // probe draw radius (m)
    CMD4(CCC_FloatNoSave, "r_ddgi_show_size", &ps_r_ddgi_show_size, 0.001f, 0.05f);   // dot size, fraction of screen
    CMD4(CCC_FloatNoSave, "r_ddgi_show_rays_scale", &ps_r_ddgi_show_rays_scale, 0.f, 64.f);
    CMD4(CCC_FloatNoSave, "r_ddgi_show_rays_dist", &ps_r_ddgi_show_rays_dist, 0.5f, 64.f);  // ray draw radius (m)
    // Step 2 — the field in the lighting. NoSave for the same reason r_ddgi is:
    // a gate that quietly sticks in user.ltx would silently poison every later A/B.
    CMD4(CCC_FloatNoSave, "r_ddgi_light", &ps_r_ddgi_light, 0.f, 1.f);          // 0 = analytic sky fill, 1 = probe field
    CMD4(CCC_FloatNoSave, "r_ddgi_light_bake", &ps_r_ddgi_light_bake, 0.f, 1.f);  // baked lightmap as a multiplier on the probe answer
    // Step 4 — the final gather. Producer and consumer are both NoSave gates.
    // ⚠⚠⚠ EVERY r_gather_* KNOB IS NoSave, TUNING INCLUDED, AND THAT IS DELIBERATE.
    // 24.08: a session spent measuring this pass left r_gather_temporal 0.97 and
    // r_gather_radius 3 in user.ltx, and the NEXT run silently started from them -
    // reported as "the build got brighter and slower to settle". While a feature is
    // being measured, a value riding in a config poisons every later A/B, which is
    // the whole reason the feature is being measured. Same trap as sv_auth_required,
    // r_wtr_sim_damp and the r_wrc_* tuning before it; see the memory note
    // gate-cvars-must-not-be-savable.
    // ⚠ NoSave stops the WRITE, not the READ: lines already in user.ltx are still
    // executed at startup, so removing the knob from the file is a separate step.
    CMD4(CCC_IntegerNoSave, "r_gather", &ps_r_gather, 0, 1);                    // master gate (needs r_ddgi — the field IS its radiance cache)
    CMD4(CCC_FloatNoSave, "r_gather_light", &ps_r_gather_light, 0.f, 1.f);      // 0 = probe lookup, 1 = the gathered answer
    CMD4(CCC_IntegerNoSave, "r_gather_debug", &ps_r_gather_debug, 0, 6);        // 1 resolved, 2 raw, 3 hit distance, 4 SH direction, 5 SH strength, 6 accumulated frame count (overwrites the frame)
    CMD4(CCC_IntegerNoSave, "r_gather_rays", &ps_r_gather_rays, 1, 8);                // rays per gather pixel
    CMD4(CCC_FloatNoSave, "r_gather_clamp", &ps_r_gather_clamp, 0.f, 64.f);           // per-ray radiance ceiling for the GATHER alone (0 = inherit r_ddgi_clamp, the A/B) — one ray needs a far tighter cut than a 64-ray probe
    CMD4(CCC_FloatNoSave, "r_gather_ffreject", &ps_r_gather_ffreject, 0.f, 8.f);      // k of the resolve's sigma clip: caps each spatial tap at mean + k*sigma of a sigma-clipped 3x3 (0 = off, the plain weighted mean)
    CMD4(CCC_FloatNoSave, "r_gather_nrmw", &ps_r_gather_nrmw, 0.f, 128.f);            // exponent of the resolve's NORMAL weight, the input idTech 8 has and we did not (0 = off, the A/B); with it up, r_gather_hitgate can come down
    CMD4(CCC_IntegerNoSave, "r_gather_scache_blur", &ps_r_gather_scache_blur, 0, 1);  // 1 = cached hit is a 5-tap average of last frame's pixel (kills specular/sun-glint variance a diffuse bounce cannot use), 0 = the single texel
    CMD4(CCC_FloatNoSave, "r_gather_len", &ps_r_gather_len, 1.f, 128.f);              // gather ray reach (m)
    CMD4(CCC_FloatNoSave, "r_gather_dist", &ps_r_gather_dist, 2.f, 200.f);            // view-distance cutoff (m)
    CMD4(CCC_FloatNoSave, "r_gather_bias", &ps_r_gather_bias, 0.f, 0.5f);             // ray-origin offset along N (m)
    CMD4(CCC_IntegerNoSave, "r_gather_scale", &ps_r_gather_scale, 1, 4);              // ⚠reallocates the targets: 1 = half res, 2 = quarter
    // ⚠NoSave, and for the usual reason: it is an A/B gate, and one that quietly
    // sticks in user.ltx poisons every later measurement. ⚠REALLOCATES the targets.
    CMD4(CCC_IntegerNoSave, "r_gather_sh", &ps_r_gather_sh, 0, 1);              // ⛔PARKED at 0 (see the PARKED banner above): 1 = 2-band SH per pixel
    // ⭐r_gather_sh_k IS THE A/B KNOB, not r_gather_sh: flipping the gate reallocates
    // the targets, which tears the whole gather down for a frame or two, and a shot
    // taken then measures the teardown. k = 0 reconstructs L0 exactly, so it is the
    // same image the gate-off path produces, reached without touching a resource.
    CMD4(CCC_FloatNoSave, "r_gather_sh_k", &ps_r_gather_sh_k, 0.f, 4.f);        // strength of the directional term; 1 = calibrated, >1 = exaggerated for looking at, 0 = the honest A/B
    CMD4(CCC_Float, "r_ao_gi", &ps_r_ao_gi, 0.f, 1.f);                           // AO weight on the traced ambient share; 1 = old full product, 0 = none
    CMD4(CCC_FloatNoSave, "r_gather_sh_temporal", &ps_r_gather_sh_temporal, 0.f, 0.99f); // the SH's own history weight (never below r_gather_temporal)
    CMD4(CCC_FloatNoSave, "r_gather_temporal", &ps_r_gather_temporal, 0.f, 0.98f);    // history weight (0 = spatial only)
    CMD4(CCC_FloatNoSave, "r_gather_plane", &ps_r_gather_plane, 0.f, 1.f);       // 0 = depth-only weight as shipped, 1 = plane-aware
    CMD4(CCC_IntegerNoSave, "r_gather_radius", &ps_r_gather_radius, 0, 3);            // spatial filter radius in texels
    CMD4(CCC_IntegerNoSave, "r_gather_foliage", &ps_r_gather_foliage, 0, 1);          // trace the alpha-tested cutouts (the forest) or skip them
    CMD4(CCC_IntegerNoSave, "r_gather_atest", &ps_r_gather_atest, 0, 3);              // how the cutout alpha test is answered: 0 exact, 1 solid, 2 fixed probability, 3 coarse-mip coverage
    CMD4(CCC_FloatNoSave, "r_gather_atest_cov", &ps_r_gather_atest_cov, 0.f, 1.f);    // mode 2's accept probability
    CMD4(CCC_FloatNoSave, "r_gather_atest_lod", &ps_r_gather_atest_lod, 0.f, 6.f);    // mip floor: raises texture-cache residency, at the price of a thinner crown
    CMD4(CCC_FloatNoSave, "r_gather_atest_dist", &ps_r_gather_atest_dist, 0.f, 200.f);// keep the exact test within this many metres
    CMD4(CCC_FloatNoSave, "r_gather_scache_tol", &ps_r_gather_scache_tol, 0.005f, 0.25f);  // ⚠ too loose = light through walls, too tight = the cache never hits
    // Step 6 — the field in froxel form, for the media that have no normal.
    // NoSave for the same reason every knob of an unfinished feature is (see the
    // scar spelled out under r_wrc below): a default edited in this file must reach
    // the run, and user.ltx wins over it forever once it has been written there.
    CMD4(CCC_FloatNoSave, "r_gi_froxel", &ps_r_gi_froxel, 0.f, 1.f);            // 0 = the flat sky fill (the A/B), 1 = the field owns a medium's ambient
    CMD4(CCC_FloatNoSave, "r_gi_froxel_dir", &ps_r_gi_froxel_dir, 0.f, 1.f);    // how much of the directional (band-1) term the phase function sees
    CMD4(CCC_FloatNoSave, "r_gi_froxel_gain", &ps_r_gi_froxel_gain, 0.f, 8.f);  // calibration against the flat fill it replaces — read both numbers off the audit line, don't guess
    CMD4(CCC_IntegerNoSave, "r_gi_froxel_stats", &ps_r_gi_froxel_stats, 0, 1);  // keep the reach / mean-radiance audit line running
    // Step 5 — the world radiance cache (cache #2). ONE knob raises everything it
    // needs: r_wrc alone brings up the probe volume and the BVH bubble, because
    // both of those gates defer to WRC::Wanted().
    CMD4(CCC_FloatNoSave, "r_wrc", &ps_r_wrc, 0.f, 1.f);                        // 0 = off (the A/B), 1 = the cache answers
    CMD4(CCC_IntegerNoSave, "r_wrc_debug", &ps_r_wrc_debug, 0, 3);              // 1 = colour per cell, 2 = only the cache, 3 = where it DISAGREES with the lattice (all through r_gather_debug 2)
    CMD4(CCC_IntegerNoSave, "r_wrc_stats", &ps_r_wrc_stats, 0, 1);              // hit rate / overflow to the log every ~2 s
    // ⚠⚠⚠ EVERY ONE OF THESE IS NoSave, AND THAT IS A SCAR, NOT A STYLE. Registered
    // savable, they were written into user.ltx on the first run — and from then on
    // that file OVERRODE the defaults in this source. A whole tuning pass (record
    // width, LOD ladder, capacity, chain) was rebuilt, deployed and measured, and
    // the log came back reporting the OLD numbers, because user.ltx quietly won.
    // The same trap as [[gate-cvars-must-not-be-savable]], one arc later.
    // A knob of a feature still being measured must never travel in a config.
    CMD4(CCC_FloatNoSave, "r_wrc_cell", &ps_r_wrc_cell, 0.05f, 4.f);            // level-0 cell size (m)
    CMD4(CCC_FloatNoSave, "r_wrc_lod", &ps_r_wrc_lod, 0.f, 0.1f);               // cell metres per metre of distance (0 = one size everywhere)
    CMD4(CCC_IntegerNoSave, "r_wrc_levels", &ps_r_wrc_levels, 0, 7);            // highest LOD level
    CMD4(CCC_IntegerNoSave, "r_wrc_cells", &ps_r_wrc_cells, 14, 21);            // ⚠ reallocates: log2 of the table (20 = 1M cells = 32 MB)
    CMD4(CCC_IntegerNoSave, "r_wrc_chain", &ps_r_wrc_chain, 1, 16);             // slots searched on a collision
    CMD4(CCC_IntegerNoSave, "r_wrc_interval", &ps_r_wrc_interval, 1, 64);       // frames between re-shades of one cell (the amortisation lever)
    CMD4(CCC_IntegerNoSave, "r_wrc_ttl", &ps_r_wrc_ttl, 4, 4096);               // frames with no request before a cell is evicted
    CMD4(CCC_IntegerNoSave, "r_wrc_shade_max", &ps_r_wrc_shade_max, 1024, 262144);  // ⚠ reallocates: cells shaded per frame
    CMD4(CCC_FloatNoSave, "r_wrc_blend", &ps_r_wrc_blend, 0.02f, 1.f);          // EMA of a re-shade (1 = no history)
    // Sky visibility — the occlusion term the sun has always had and the sky never
    // did. 0 restores the old unoccluded sky exactly (the A/B). ⚠ pair with a lower
    // r_ddgi_light_bake, or interiors are occluded twice.
    CMD4(CCC_IntegerNoSave, "r_wrc_skyvis", &ps_r_wrc_skyvis, 0, 64);           // hemisphere rays per CELL shade (⚠ cost is linear; see vk_wrc.cpp for why 8 was not enough)
    CMD4(CCC_FloatNoSave, "r_wrc_skyvis_len", &ps_r_wrc_skyvis_len, 0.5f, 64.f);// their reach (m) — medium, not infinite
    CMD4(CCC_IntegerNoSave, "r_wrc_sky_field", &ps_r_wrc_sky_field, 0, 2);      // 1 = idTech 8's cell: ambient is a probe-field READ, no per-cell rays ⇒ no per-cell RNG ⇒ no cell dirt; 2 = cache-to-cache: covered cells integrate their own hemisphere off the cache, the field never enters (open-sky cells keep mode 1)
    CMD4(CCC_FloatNoSave, "r_wrc_field_clamp", &ps_r_wrc_field_clamp, -8.f, 8.f); // ceiling on that read: k x the cell's measured injection ("distant house lights"); 0 = off (the A/B); NEGATIVE = diag (cells store budget breakdown, frame looks wrong on purpose)
    CMD4(CCC_FloatNoSave, "r_wrc_ema_up", &ps_r_wrc_ema_up, 0.01f, 1.f);          // brightening EMA rate as a fraction of the darkening one — parks cells at the low quantile of the 8-ray shot noise (1 = symmetric, the A/B)
    CMD4(CCC_FloatNoSave, "r_blm", &ps_r_blm, 0.f, 2.f);                          // bloom add strength in the composite (0.8 = the shipped constant, 0 = bloom off — the "is this glow the bloom?" switch)
    // ...and ONE command that drives the whole chain, because five cvars typed by
    // hand is how a run silently measures the wrong thing. See CCC_GI above.
    CMD1(CCC_GI, "r_gi");

    // ⭐The same shape as r_gi, for the group that measures 14.62 ms on a 780M.
    // ⚠NOT savable — the tier is DERIVED from the r_vsm_* entries, every one of
    // which persists on its own. See CCC_Shadows above.
    CMD1(CCC_Shadows, "r_shadows");
    CMD4(CCC_Float, "r_dither", &ps_r_dither, 0.f, 2.f);                  // final 8-bit output dither, LSBs (kills gradient contouring/"полосы" on flat surfaces; 0=off A/B)
    CMD4(CCC_Integer, "r_ssil", &ps_r_ssil_enable, 0, 1);        // SSIL (folded into GTAO) on/off (A/B; needs r_ssao on)
    CMD4(CCC_Integer, "r_ssil_debug", &ps_r_ssil_debug, 0, 1);   // 1 = show ONLY the indirect bounce field
    CMD4(CCC_Float, "r_ssil_strength", &ps_r_ssil_strength, 0.f, 8.f);  // IL intensity multiplier
    CMD4(CCC_Float, "r_ssil_temporal", &ps_r_ssil_temporal, 0.f, 0.97f);  // GTAO temporal accumulation α (0=off; lets r_ssil ship clean)
    CMD4(CCC_Integer, "r_motion_vectors", &ps_r_motion_vectors, 0, 1);  // screen-space MV pass on/off (DLSS/FSR/PT foundation)
    // Read-only in spirit: the NGX probe writes it, the options screen reads it.
    // NoSave — it describes THIS GPU and must never travel in a config.
    CMD4(CCC_IntegerNoSave, "r_dlss_avail", &ps_r_dlss_avail, -1, 1);

    // ⭐FSR 3.1.4 — the same seam as DLSS, for the hardware NGX cannot serve.
    // ⚠r_fsr_quality reallocates every scene target (it moves the render extent):
    // live, but it costs a device idle, so it is not a slider to drag.
    CMD4(CCC_IntegerNoSave, "r_fsr_avail", &ps_r_fsr_avail, 0, 1);
    CMD4(CCC_Integer, "r_fsr", &ps_r_fsr, 0, 1);
    CMD4(CCC_Integer, "r_fsr_quality", &ps_r_fsr_quality, 0, 4);
    CMD4(CCC_Float,   "r_fsr_sharp", &ps_r_fsr_sharp, 0.f, 1.f);
    CMD4(CCC_IntegerNoSave, "r_fsr_debug", &ps_r_fsr_debug, 0, 1);
    // Internal resolution as a fraction of the display, for when DLSS is off or
    // absent. Floor 0.5 = quarter the pixels; below that the bilinear upscale
    // stops being "cheaper" and starts being "broken".
    CMD4(CCC_Float, "r_render_scale", &ps_r_render_scale, 0.5f, 1.0f);
    // HDR scene target format: 1 = B10G11R11 (32 bpp, half the colour bandwidth
    // of every world pass), 0 = R16G16B16A16_SFLOAT. Read ONCE at renderer init
    // - set it in user.ltx / on the command line and RESTART, the console has no
    // way to rebuild the ~25 pipelines that bake the format in.
    CMD4(CCC_IntegerNoSave, "r_scene_color_32", &ps_r_scene_color_32, 0, 1);
    CMD4(CCC_Integer, "r_dlss", (int*)&ps_r_dlss, 0, 1);                 // DLSS Super Resolution; needs r_motion_vectors + NGX available
    CMD4(CCC_Integer, "r_dlss_quality", (int*)&ps_r_dlss_quality, 0, 4); // 0=DLAA 1=Quality 2=Balanced 3=Performance 4=UltraPerf (render<display upscale)
    CMD4(CCC_Integer, "r_dlss_preset", (int*)&ps_r_dlss_preset, 0, 15);  // DLSS model preset hint: 0=driver default, 6=F (CNN), 10=J, 11=K (transformer, default)
    CMD4(CCC_Float,   "r_dlss_sharp", &ps_r_dlss_sharp, 0.0f, 1.0f);     // CAS sharpen on the DLSS output (tonemap), 0 = off
    // ⚠NoSave: mode 4 replaces the WHOLE composite with a dim grey classifier view
    // (works with DLSS off too) — saved to user.ltx it survived the session and came
    // back as "the game is dark and black-and-white" a day later (27-08). A debug
    // instrument must never outlive the session that armed it.
    CMD4(CCC_IntegerNoSave, "r_dlss_debug", &ps_r_dlss_debug, 0, 4);     // 1=CAS heatmap, 2=split no-DLSS|composite, 3=gate flag, 4=input sanitizer (NaN/neg/huge in the render-res scene)
    CMD4(CCC_Integer, "r_dlss_exp", &ps_r_dlss_exp, 0, 1);               // 1=real exposure texture (default), 0=AutoExposure flag
    CMD4(CCC_Float,   "r_dlss_bias", &ps_r_dlss_bias, 0.0f, 1.0f);       // material mip-bias scale under upscaling (0=off A/B)
    CMD4(CCC_Integer, "r_dlss_sl", &ps_r_dlss_sl, 0, 1);                 // SR via Streamline sl.dlss (Stage B, FG prerequisite)
    CMD4(CCC_Integer, "r_dlss_fg", &ps_r_dlss_fg, 0, 1);                 // DLSS Frame Generation (Stage C, MFG); needs r_dlss + r_dlss_sl
    CMD4(CCC_Integer, "r_dlss_fg_mult", &ps_r_dlss_fg_mult, 2, 6);       // FG frame multiplier 2..6 (2x default, up to 6x MFG)
    CMD4(CCC_Integer, "r_dlss_fg_debug", &ps_r_dlss_fg_debug, 0, 2);     // DLSS-G + Reflex state log (1=periodic, 2=every frame)
    // Ray Reconstruction. ⚠r_dlss_rr_avail is NoSave for the same reason as
    // r_dlss_avail: it is a property of this machine, not of this player's taste.
    // ⚠⚠r_dlss_rr_debug is NoSave too — a debug view stuck in user.ltx is exactly
    // how r_ssao_debug 6 silently cost every run a 130 ms hitch for weeks.
    CMD4(CCC_IntegerNoSave, "r_dlss_rr_avail", &ps_r_dlss_rr_avail, -1, 1);
    CMD4(CCC_Integer, "r_dlss_rr", &ps_r_dlss_rr, 0, 1);                       // Ray Reconstruction; OVERRIDES Super Resolution, needs r_dlss 1
    CMD4(CCC_Integer, "r_dlss_rr_preset", (int*)&ps_r_dlss_rr_preset, 0, 5);   // RR model hint: 0=driver default, 4=D, 5=E (default)
    CMD4(CCC_Integer, "r_dlss_rr_raw", &ps_r_dlss_rr_raw, 0, 1);              // feed RR the UNdenoised gather (RR is the denoiser); no-op unless RR runs
    CMD4(CCC_Integer, "r_dlss_vram", &ps_r_dlss_vram, 0, 8192);               // MB one NGX feature needs kept free; 0 = estimate from display size (room gate + streamer reserve)
    CMD4(CCC_IntegerNoSave, "r_dlss_rr_debug", &ps_r_dlss_rr_debug, 0, 4);     // guide view: 1=diff albedo 2=spec albedo 3=normal 4=roughness
    // Neural Rendering. ⚠r_dlss_nr_avail is NoSave for the same reason as the two
    // availability flags above. ⚠r_dlss_nr_featid is NOT NoSave on purpose: once the
    // right ID is found on this driver it should survive the restart that proves it.
    CMD4(CCC_IntegerNoSave, "r_dlss_nr_avail", &ps_r_dlss_nr_avail, -1, 1);
    CMD4(CCC_Integer, "r_dlss_nr", &ps_r_dlss_nr, 0, 1);                        // Neural Rendering; OVERRIDES RR and SR, needs r_dlss 1
    CMD4(CCC_Integer, "r_dlss_nr_featid", (int*)&ps_r_dlss_nr_featid, 1, 31);   // NGX feature ID (no public constant); 14 = the driver telemetry table's implication
    CMD4(CCC_Integer, "r_dlss_nr_preset", (int*)&ps_r_dlss_nr_preset, 0, 15);   // DLSSNR.Hint.Render.Preset, 0 = driver default
    CMD4(CCC_Integer, "r_dlss_nr_cfg", (int*)&ps_r_dlss_nr_cfg, 0, 127); // ⚠127, not 63: bit 64 (the DLSSD creation keys) is a valid bit, and a max that excludes it silently rejects the winning value on reload         // which creation keys to send (bitmask; see the block comment)
    CMD4(CCC_Integer, "r_dlss_nr_debug", &ps_r_dlss_nr_debug, 0, 1);            // forward NGX's own log lines (takes effect at next launch)
    CMD4(CCC_Integer, "r_dlss_nr_style", (int*)&ps_r_dlss_nr_style, 0, 15);     // DLSSNR.Style
    CMD4(CCC_Float, "r_dlss_nr_intensity", &ps_r_dlss_nr_intensity, -1.f, 1.f); // <0 = leave the network default alone
    CMD4(CCC_Float, "r_dlss_nr_structure", &ps_r_dlss_nr_structure, -1.f, 1.f); // DLSSNR.LocalStructureStrength
    CMD4(CCC_Float, "r_dlss_nr_tone", &ps_r_dlss_nr_tone, -1.f, 1.f);           // DLSSNR.LocalToneStrength
    CMD4(CCC_Float, "r_dlss_nr_skin", &ps_r_dlss_nr_skin, -1.f, 1.f);           // DLSSNR.SkinStructureStrength
    CMD4(CCC_Integer, "r_mv_debug", &ps_r_mv_debug, 0, 1);              // false-colour MV overlay (grey=still, R=+x, G=+y)
    CMD4(CCC_Integer, "r_mv_prepass", &ps_r_mv_prepass, 0, 1);          // MV written by the depth prepass (complete before SSAO, no overlay re-raster); 0 = shipping overlay path
    CMD4(CCC_Float, "r_mv_debug_scale", &ps_r_mv_debug_scale, 1.f, 500.f); // MV overlay magnitude scale
    CMD4(CCC_Float, "r_wind_tree_bend", &ps_r_wind_tree_bend, 0.f, 2.f);   // tree trunk sway intensity (0 = rigid)
    CMD4(CCC_Float, "r_wind_tree_anim", &ps_r_wind_tree_anim, 0.f, 40.f);  // tree branch/leaf flutter speed
    CMD4(CCC_Float, "r_wind_tree_trunk", &ps_r_wind_tree_trunk, 0.f, 2.f); // tree trunk anim speed
    CMD4(CCC_Float, "r_wind_tree_flutter", &ps_r_wind_tree_flutter, 0.f, 16.f); // crown/leaf flutter amplitude
    CMD4(CCC_Float, "r_wind_tree_crown", &ps_r_wind_tree_crown, 0.f, 30.f);     // height where leaf flutter fades in
    CMD4(CCC_Integer, "r_sun_cad_npc", &ps_r_sun_cad_npc, 0, 8);   // nfs48: cascade-0 NPC/rigid layer every N frames (0 = only with the static cache)
    CMD4(CCC_Integer, "r_sun_cad_c0", &ps_r_sun_cad_c0, 0, 8);     // nfs48: cascade-0 live trees every N frames (0 = only with the static cache)
    CMD4(CCC_Integer, "r_sun_cad_c1", &ps_r_sun_cad_c1, 0, 8);     // nfs48: cascade-1 live layer every N frames (0 = only with the static cache)
    CMD4(CCC_Float, "r_tree_shadow_lod", &ps_r_tree_shadow_lod, 0.f, 8.f);   // nfs48: cached tree casters LOD (0 = full)
    CMD4(CCC_Integer, "r_sun_casc_lite", &ps_r_sun_casc_lite, 0, 1);   // nfs43-48 handheld cascade profile, latched at ShadowMap::Init
    CMD4(CCC_Integer, "r_sun_far_size", &ps_r_sun_far_size, 0, 4096);     // far sun map texels, latched at ShadowMap::Init (0 = profile; Switch 512)
    CMD4(CCC_Integer, "r_sun_far_trees", &ps_r_sun_far_trees, 0, 1);      // tree crowns in the far sun map (0 = none)
    CMD4(CCC_Float, "r_sun_far_tree_dist", &ps_r_sun_far_tree_dist, 0.f, 1000.f);   // ... only within this distance (0 = all)
    CMD4(CCC_Integer, "r_sun_mask", &ps_r_sun_mask, 0, 1);             // nfs46 half-res sun mask for the R1 receivers
    CMD4(CCC_Integer, "r_sun_vis_probe", &ps_r_sun_vis_probe, 0, 1);   // r_sun_shadows 0: characters use the sun-visibility probe
    CMD4(CCC_Integer, "r_skin_lod", &ps_r_skin_lod, 0, 1);             // nfs4 R4 skeleton geometry LOD
    CMD4(CCC_Float, "r_skin_lod_k", &ps_r_skin_lod_k, 0.25f, 16.f);    // nfs7 SSA threshold multiplier (Switch 4)
    CMD4(CCC_Float, "r_skin_st_lod", &ps_r_skin_st_lod, 0.f, 200.f);   // ST skinned leaves: LOD windows from this distance (Switch 6)
    CMD4(CCC_Float, "r_skin_lod_bg", &ps_r_skin_lod_bg, 0.f, 200.f);   // non-HUD skinned leaves: LOD windows built in the background, from this distance (Switch 6)
    CMD4(CCC_Float, "r_skin_lod_wpn", &ps_r_skin_lod_wpn, 0.f, 200.f);   // weapons: their own LOD ladder from this distance, 2 deeper windows (Switch 1)
    CMD4(CCC_Integer, "r_skin_hud_lod", &ps_r_skin_hud_lod, 0, 5);   // first-person hands/weapon: fixed LOD window (1 = 50%, 2 = 25%; 0 = full)
    CMD4(CCC_Integer, "r_hud_vsun", &ps_r_hud_vsun, 0, 1);   // HUD hands/weapon: the sun shadow per vertex
    CMD4(CCC_Float, "r_skin_prepass_dist", &ps_r_skin_prepass_dist, 0.f, 200.f);   // NPCs in the depth prepass up to this range
    CMD4(CCC_Integer, "r_hud_prepass", &ps_r_hud_prepass, 0, 1);      // hands/weapon depth in the prepass (world under them skips shading)
    CMD4(CCC_Integer, "r_skin_ubo", &ps_r_skin_ubo, 0, 1);             // bones from a dynamic-UBO window (constant bank on NVK)
    CMD4(CCC_Integer, "r_dyn_occ", &ps_r_dyn_occ, 0, 2);               // nfs5/14 dynamic collection: 1 ssaDISCARD, 2 + skeleton occlusion
    CMD4(CCC_Integer, "r_dyn_occ_async", &ps_r_dyn_occ_async, 0, 1);   // the occlusion rays off the main thread (core 2), a frame late; 0 = in the collection loop
    CMD4(CCC_Float, "r_dyn_occ_ppx", &ps_r_dyn_occ_ppx, 0.f, 64.f);    // nfs15 particle screen-radius discard (px, 0 = SSA rule)
    CMD4(CCC_Float, "r_wind_shadow_dist", &ps_r_wind_shadow_dist, 0.f, 160.f);  // tree shadow wind radius (0 = all static)
    CMD4(CCC_Float, "r_tree_dist", &ps_r_tree_dist, 0.f, 5000.f);               // forward tree mesh range where an FLOD billboard takes over (0 = off, draws both)
    CMD4(CCC_Float, "r_tree_vox_dist", &ps_r_tree_vox_dist, 0.f, 5000.f);   // forward crown range where the BAKED VOXEL CUBES replace the leaf-card mesh (0 = off); trunks always keep their mesh
    CMD4(CCC_FloatNoSave, "r_buf_priority", &ps_r_buf_priority, 0.f, 1.f);         // VK_EXT_memory_priority for device-local buffers; 1.0 = ship, 0.5 = pre-25-08 default. Read at ALLOCATION time
    CMD4(CCC_Integer, "r_tree_hzb", &ps_r_tree_hzb, 0, 1);                      // Hi-Z occlusion cull for the forward tree set
    CMD4(CCC_Integer, "r_tree_zwrite", &ps_r_tree_zwrite, 0, 1);                // 0 = tree colour tests depth but does not write it (early-Z survives the leaf discard); live A/B
    CMD4(CCC_Integer, "r_tree_lod_size", &ps_r_tree_lod_size, 0, 1);   // nfs21
    CMD4(CCC_Integer, "r_grass_gen", &ps_r_grass_gen, 0, 3);
    CMD4(CCC_Integer, "r_grass_r1", &ps_r_grass_r1, 0, 2);
    CMD4(CCC_Integer, "r_tree_r1", &ps_r_tree_r1, 0, 1);
    CMD4(CCC_Integer, "r_tree_r1_dyn", &ps_r_tree_r1_dyn, 0, 1);   // 1 = a torch/lamp in reach keeps the R1 crown (per-vertex light)
    CMD4(CCC_Integer, "r_tree_r1_wet", &ps_r_tree_r1_wet, 0, 1);   // 1 = the rain keeps the R1 crown (wet = darker, per vertex)
    CMD4(CCC_Integer, "r_tree_sky_push", &ps_r_tree_sky_push, 0, 1);
    CMD4(CCC_Integer, "r_tree_crown_bake", &ps_r_tree_crown_bake, 0, 1);
    CMD4(CCC_Float, "r_tree_lod_band", &ps_r_tree_lod_band, 1.f, 2.f);   // nfs23
    CMD4(CCC_Float, "r_grass_thin", &ps_r_grass_thin, 0.f, 100.f);       // nfs25
    CMD4(CCC_Integer, "r_tree_swi", &ps_r_tree_swi, 0, 1);             // nfs21
    CMD4(CCC_Float, "r_tree_swi_lod", &ps_r_tree_swi_lod, 0.1f, 4.f);  // nfs21
    CMD4(CCC_Float, "r_tree_sky_boost", &ps_r_tree_sky_boost, 0.25f, 4.f);   // nfs19: crown sky fill x (ZL+ZR+D-pad Right on the Switch)
    CMD4(CCC_Float, "r_tree_shade_dist", &ps_r_tree_shade_dist, 0.f, 5000.f);   // range past which tree.frag drops SSIL / sky sheen / dynamic lights / shore wetness (0 = off)
    CMD4(CCC_Integer, "r_tree_stats", &ps_r_tree_stats, 0, 2);                  // tree ledger: 1 = cull funnel + hardware counters, 2 = + per-pass GPU sub-zones (with r_profiler 2)
    CMD4(CCC_Integer, "r_tree_depth_sort", &ps_r_tree_depth_sort, 0, 64);       // camera depth prepass in near->far depth slabs (0 = texture-group order); value = slab count
    CMD4(CCC_Integer, "r_tree_depth_gpu", &ps_r_tree_depth_gpu, 0, 1);          // GPU-cull + indirect the camera depth prepass (0 = CPU walk, one draw per tree)
    CMD4(CCC_Integer, "r_tree_shadow_gpu", &ps_r_tree_shadow_gpu, 0, 1);        // GPU-cull + indirect the sun/rain shadow targets (0 = CPU walk, one per target)
    CMD4(CCC_IntegerNoSave, "r_pipe_stats", &ps_r_pipe_stats, 0, 1);            // ask the DRIVER for registers/occupancy per pipeline (VK_KHR_pipeline_executable_properties). MUST be on BEFORE the pipeline is built; read with vk_pipestats
    CMD4(CCC_IntegerNoSave, "r_tex_audit", &ps_r_tex_audit, 0, 1);              // scan every BC3 block at load: how much VRAM is paying for a channel nothing reads. MUST be on BEFORE the level loads; read with vk_texaudit
    CMD1(CCC_VkTexAudit,  "vk_texaudit");                                       // print that tally
    CMD1(CCC_VkPipeStats, "vk_pipestats");                                      // print those statistics for the crown pipelines
    CMD1(CCC_VkTrees, "vk_trees");                                              // print the ledger immediately (enables r_tree_stats if it was off)
    CMD4(CCC_Integer, "r_vsm_tree_wind",      &ps_r_vsm_tree_wind,      0, 1);            // near/far hybrid: near trees sway in the dyn atlas
    CMD4(CCC_Integer, "r_vsm_meshlet",        &ps_r_vsm_meshlet,        0, 1);            // per-page meshlet cull of VSM tree casters (Phase B)
    CMD4(CCC_Integer, "r_vsm_hzb",            &ps_r_vsm_hzb,            0, 1);            // shadow-HZB occlusion cull of VSM casters (kills VSMrender overdraw)
    CMD4(CCC_Float,   "r_vsm_hzb_margin",     &ps_r_vsm_hzb_margin,     0.0f, 0.05f);     // depth slack for the shadow-HZB occluder (stale-sun safety)
    CMD4(CCC_Float,   "r_vsm_tree_wind_dist", &ps_r_vsm_tree_wind_dist, 8.0f, 200.0f);    // near set radius (m)
    CMD4(CCC_Integer, "r_vsm_tree_impostor",       &ps_r_vsm_tree_impostor,       0, 1);         // near crown mesh → baked sun-facing billboard in the dyn atlas
    CMD4(CCC_Float,   "r_vsm_tree_impostor_scale", &ps_r_vsm_tree_impostor_scale, 0.3f, 3.0f);   // billboard size vs crown sphere (shadow footprint)
    CMD4(CCC_Integer, "r_vsm_tree_hull",      &ps_r_vsm_tree_hull,      0, 2);                   // caster LOD: 1 = near trees beyond _dist cast the baked opaque crown hull; 2 = far/static foliage too (monotonic LOD)
    CMD4(CCC_Float,   "r_vsm_tree_hull_dist", &ps_r_vsm_tree_hull_dist, 4.0f, 200.0f);           // crown-mesh tier radius (m); the rest of the near set uses the hull
    CMD4(CCC_Integer, "r_vsm_tree_hull_vox",  &ps_r_vsm_tree_hull_vox,  0, 128);                 // crown voxelization: 0 = lobes only, >0 = level-0 voxel-cloud resolution (cells across the crown); baked → level reload
    CMD4(CCC_Integer, "r_vsm_tree_hull_lod",  &ps_r_vsm_tree_hull_lod, -4, 8);                   // LIVE LOD bias for the voxel viewmode (- = finer, + = coarser); no rebake
    CMD4(CCC_Float,   "r_vsm_tree_hull_vox_px", &ps_r_vsm_tree_hull_vox_px, 1.0f, 32.0f);        // target projected voxel size in px (Nanite screen-error LOD cut); live
    CMD4(CCC_Float,   "r_vsm_tree_hull_vox_far", &ps_r_vsm_tree_hull_vox_far, 0.0f, 32.0f);      // +px per 100m: far cubes visibly chunkier (0 = constant screen size); live
    CMD4(CCC_Float,   "r_vsm_tree_hull_vox_stex", &ps_r_vsm_tree_hull_vox_stex, 0.5f, 16.0f);    // SHADOW voxel LOD: page texels per voxel edge (UE shadow-view metric); live
    CMD4(CCC_Float,   "r_vsm_tree_hull_vox_sfloor", &ps_r_vsm_tree_hull_vox_sfloor, 0.0f, 2.0f); // SHADOW-only safety floor on voxel edge (m); live
    CMD4(CCC_Integer, "r_vsm_tree_hull_vox_cap",  &ps_r_vsm_tree_hull_vox_cap, 200, 20000);      // SHADOW-only max bricks per tree slice (perf); live
    CMD4(CCC_Integer, "r_vsm_tree_hull_vox_wpo",  &ps_r_vsm_tree_hull_vox_wpo, 0, 1);            // UE WPO-off: rigid voxel trees → static cache (needs hull 2); live
    CMD4(CCC_Integer, "r_vsm_tree_hull_vox_static", &ps_r_vsm_tree_hull_vox_static, 0, 1);       // static-tier proxy: 0=shell (cheap, per-page-friendly) 1=bricks; live
    CMD4(CCC_Integer, "r_vsm_tree_hull_vox_cull",  &ps_r_vsm_tree_hull_vox_cull, 0, 1);          // stage-2 per-page brick compaction (UE-parity cull); live
    CMD4(CCC_Float,   "r_vsm_tree_hull_band",   &ps_r_vsm_tree_hull_band, 0.02f, 0.9f);          // shadow crossfade band: crown↔voxels dither inside this fraction of _dist; live
    CMD4(CCC_Float,   "r_vsm_tree_hull_vox_fade", &ps_r_vsm_tree_hull_vox_fade, 0.0f, 0.6f);     // dithered voxel-LOD crossfade band, fraction of handover dist (0 = hard cut); live
    CMD4(CCC_Integer, "r_vsm_tree_hull_debug", &ps_r_vsm_tree_hull_debug, 0, 2);                 // 1 = shaded hull over hull-tier trees (_dist boundary); 2 = over EVERY hulled tree (walk-up voxel inspect)
    CMD4(CCC_Float, "r_sun_boost", &ps_r_sun_boost, 0.f, 4.f);
    CMD4(CCC_Integer, "r_linear_color", &ps_r_linear_color, 0, 1);                // linear colour pipeline; LATCHED per process (ColorSpace::Active) — takes effect on next game start
    CMD4(CCC_Integer, "r_sun_night_freeze", &ps_r_sun_night_freeze, 0, 1);        // freeze the sun-shadow (VSM) update when the sun is down (no light → no cost)
    CMD4(CCC_Float,   "r_sun_night_lum",    &ps_r_sun_night_lum,    0.f, 0.5f);   // sun_color luminance threshold for "night" (secondary)
    CMD4(CCC_Float,   "r_sun_night_alt",    &ps_r_sun_night_alt,   -0.2f, 0.5f);  // to-sun.y (sun altitude) below which it's "night" (primary; 0 = horizon)
    CMD4(CCC_Float, "r_grass_aref", &ps_r_grass_aref, 0.05f, 0.9f);
    CMD4(CCC_Float, "r_grass_asharp", &ps_r_grass_asharp, 0.0f, 2.0f);   // mip alpha compensation (far grass density)
    CMD4(CCC_Integer, "r_light_debug", &ps_r_light_debug, 0, 1);         // dump dynamic lights to the log (~2 s)
    CMD4(CCC_Integer, "r_light_cones",        &ps_r_light_cones,        0, 2);          // real volumetric beams for volumetric spots (2 = bounding-sphere debug)
    CMD4(CCC_Float,   "r_light_cone_density", &ps_r_light_cone_density, 0.0f, 5.0f);    // beam in-scatter strength
    CMD4(CCC_Float,   "r_light_cone_len",     &ps_r_light_cone_len,     0.5f, 10.0f);   // beam length = range × this
    CMD4(CCC_Float,   "r_light_cone_glare",   &ps_r_light_cone_glare,   0.0f, 8.0f);    // looking-into-the-beam flare
    CMD4(CCC_Float,   "r_light_cone_narrow",  &ps_r_light_cone_narrow,  0.1f, 1.0f);    // visible beam vs lit cone angle
    CMD4(CCC_Float,   "r_light_cone_lum",     &ps_r_light_cone_lum,     0.1f, 12.0f);   // beam luminance (HDR units; raise for day visibility)
    CMD4(CCC_Integer, "r_light_cone_synth",   &ps_r_light_cone_synth,   0, 1);          // beams synthesized from lightplanes model geometry
    CMD4(CCC_Float,   "r_light_cone_lift",    &ps_r_light_cone_lift,    0.0f, 0.5f);    // vertical apex lift for synthesized beams (m)
    CMD4(CCC_Float,   "r_light_cone_base",    &ps_r_light_cone_base,    0.0f, 4.0f);    // lamp-face radius multiplier (frustum base)
    CMD4(CCC_Float,   "r_light_cone_reach",   &ps_r_light_cone_reach,   1.0f, 10.0f);   // beam/light reach = fan length × this
    CMD4(CCC_Float,   "r_light_cone_power",   &ps_r_light_cone_power,   0.0f, 10.0f);   // synthesized-light surface intensity (0 = beam only)
    CMD4(CCC_Float,   "r_light_cone_fade",    &ps_r_light_cone_fade,    0.05f, 10.0f);  // synth lamp-face glow fade length (m)
    CMD4(CCC_Integer, "r_spot_grass",         &ps_r_spot_grass,         0, 1);          // grass casters into the spot shadow map (beam cutouts)
    CMD4(CCC_Integer, "r_sun_grass",          &ps_r_sun_grass,          0, 1);          // grass casters into the sun cascades (swaying blade shadows)
    CMD4(CCC_Float,   "r_sun_grass_dist",     &ps_r_sun_grass_dist,     5.f, 100.f);    // sun grass-shadow cull radius around the camera (m)
    CMD4(CCC_Integer, "r_point_grass",        &ps_r_point_grass,        0, 1);          // grass casters into the campfire point cubes (dapples)
    CMD4(CCC_Integer, "r_grass_cull_debug",   &ps_r_grass_cull_debug,   0, 1);          // [VK GrassCull] per-call lights/types/cells log
    CMD4(CCC_Float,   "r_light_cone_soft",    &ps_r_light_cone_soft,    0.0f, 32.0f);   // beam shadow penumbra radius (spot-map texels)
    CMD4(CCC_Integer, "r_light_cone_torch",   &ps_r_light_cone_torch,   0, 1);          // analytic cone for ordinary torches (0 = R4: froxel shaft only, no searchlight cone)
    CMD4(CCC_Float,   "r_flashlight_glow",    &ps_r_flashlight_glow,    0.0f, 20.0f);   // bright lamp-face glow for a torch aimed at the camera (0 = off)
    CMD4(CCC_Float,   "r_spot_grass_shadow",  &ps_r_spot_grass_shadow,  0.0f, 1.0f);    // grass shadow strength on surfaces (0 off, 1 full blanket)
    CMD4(CCC_Float,   "r_flashlight_grass",   &ps_r_flashlight_grass,   0.0f, 1.0f);    // grass shadow strength for FLASHLIGHT tiles only (night wow; lamps keep r_spot_grass_shadow)
    CMD4(CCC_Integer, "r_spot_pool",          &ps_r_spot_pool,          1, 8);          // spot shadow tiles per frame (each light = own map)
    CMD4(CCC_Float,   "r_spot_scale",         &ps_r_spot_scale,         0.25f, 1.0f);   // the part of each spot tile the map is rendered into (resolution)
    CMD4(CCC_Integer, "r_spot_pcf",           &ps_r_spot_pcf,           0, 1);          // spot shadow receivers: 1 = 3x3 PCF, 0 = one 2x2 gather
    CMD4(CCC_Integer, "r_flash_shadow",       &ps_r_flash_shadow,       0, 1);          // 0 = flashlights get no shadow tile (and no cookie)
    CMD4(CCC_Float,   "r_spot_npc_dist",      &ps_r_spot_npc_dist,     -1.0f, 100.0f);  // NPCs cast into a flashlight tile only this near it (0 = the light range, -1 = none)
    CMD4(CCC_Integer, "r_flash_mask",         &ps_r_flash_mask,         0, 1);          // torch shadow x cookie via the sun mask (R1 statics/terrain stay on the light variant)
    CMD4(CCC_Integer, "r_point_pool",         &ps_r_point_pool,         1, 4);          // point shadow cubes per frame (each campfire = own cube)
    CMD4(CCC_Integer, "r_point_debug",        &ps_r_point_debug,        0, 1);          // debug overlay: green=lit by pooled point, red=its shadow
    CMD4(CCC_Integer, "r_grass_debug",        &ps_r_grass_debug,        0, 11);         // grass component isolation (1..11, see decl)
    CMD4(CCC_Float,   "r_grass_self_bias",    &ps_r_grass_self_bias,    0.0f, 1.0f);    // grass-on-grass anti-acne slack (m along sun ray)
    CMD4(CCC_Float,   "r_sun_beam",           &ps_r_sun_beam,           0.0f, 1.0f);    // ground beam-gap recovery strength (0 = off)
    CMD4(CCC_Float,   "r_sun_beam_dist",      &ps_r_sun_beam_dist,      5.0f, 120.0f);  // beam-recovery max distance (m)
    CMD4(CCC_Float,   "r_sun_beam_boost",     &ps_r_sun_beam_boost,     1.0f, 4.0f);    // extra sun in the recovered gap (cinematic splash)
    CMD4(CCC_Float,   "r_sun_beam_bias",      &ps_r_sun_beam_bias,      0.0f, 1.0f);    // beam-recovery atlas self-bias (m along sun ray)
    CMD4(CCC_Float,   "r_sun_beam_splash",     &ps_r_sun_beam_splash,     0.0f, 8.0f);   // god-ray ground-splash strength (0 = off)
    CMD4(CCC_Float,   "r_sun_beam_splash_thr", &ps_r_sun_beam_splash_thr, 0.0f, 2.0f);   // in-scatter luminance threshold before the splash kicks in
    CMD4(CCC_Float,   "r_point_boost",        &ps_r_point_boost,        0.1f, 8.0f);    // campfire/brazier glow intensity multiplier (volumetric points)
    CMD4(CCC_Float,   "r_point_range",        &ps_r_point_range,    0.5f, 4.0f);    // campfire/brazier light reach multiplier (volumetric points)
    CMD4(CCC_Float,   "r_glass_opacity", &ps_r_glass_opacity, 0.05f, 1.0f); // glass opacity ceiling (1 = texture alpha as in R4)
    CMD4(CCC_Float,   "r_glass_refr",    &ps_r_glass_refr,    0.0f,  3.0f); // glass refraction wobble strength (0 = off)
    CMD4(CCC_Integer, "r_preskin",       &ps_r_preskin,       0, 1);        // compute pre-skinning: skin once per frame, not once per pass
    CMD4(CCC_Integer, "r_clouds",           &ps_r_clouds,           0, 1);         // animated cloud layer on/off
    CMD4(CCC_Float,   "r_clouds_intensity", &ps_r_clouds_intensity, 0.0f, 4.0f);   // cloud additive brightness
    CMD4(CCC_Float,   "r_clouds_speed",     &ps_r_clouds_speed,     0.0f, 8.0f);   // cloud UV scroll speed
    CMD4(CCC_Integer, "r_grass_nowave", &ps_r_grass_nowave, 0, 1);
    CMD4(CCC_Float, "r_ambient_floor", &ps_r_ambient_floor, 0.f, 0.5f);
    CMD4(CCC_Float, "r_wet_darken", &ps_r_wet_darken, 0.f, 1.f);
    CMD4(CCC_FloatNoSave, "r_wet_force", &ps_r_wet_force, -1.f, 1.f);   // -1 = accumulator; 0..1 = forced wetness (A/B)
    CMD4(CCC_Float, "r_wet_refl", &ps_r_wet_refl, 0.f, 3.f);
    CMD4(CCC_Integer, "r_wet_debug", &ps_r_wet_debug, 0, 1);
    CMD4(CCC_Integer, "r_rain_debug", &ps_r_rain_debug, 0, 1);
    CMD4(CCC_Integer, "r_rain", &ps_r_rain_enable, 0, 1);   // master rain on/off (effect only, not weather)
    CMD4(CCC_Integer, "r_rain_drops", &ps_r_rain_drops, 0, 4000);         // max rain drops (0 = stock 2000/1000)
    CMD4(CCC_Integer, "r_rain_rq_static", &ps_r_rain_rq_static, 0, 1);    // drop rays against the static level only
    CMD4(CCC_Integer, "r_rain_hcache", &ps_r_rain_hcache, 0, 1);    // those static drop rays from a 0.5 m height cache
    CMD4(CCC_Float, "r_rain_sun", &ps_r_rain_sun, 0.f, 3.f); // streaks catch sun/moon/lightning (0 = flat hemi)

    // GPU-driven particles (gpu_particles_roadmap.md Phase 1). r_gpu_particles 1
    // runs the GPU-resident test effect (emit/simulate/draw) at the camera.
    CMD4(CCC_Integer, "r_gpu_particles", &ps_r_gpu_particles, 0, 1);
    CMD4(CCC_Integer, "r_gpu_particles_max", &ps_r_gpu_particles_max, 1024, 1 << 24);
    CMD4(CCC_Integer, "r_gpu_particles_sort", &ps_r_gpu_particles_sort, 0, 1);
    CMD4(CCC_Float,   "r_gpu_particles_life_cap", &ps_r_gpu_particles_life_cap, 0.0f, 300.0f);
    CMD1(CCC_GP_Mirror, "gp_mirror");       // Phase 3: mirror a real .pe onto the camera emitter
    CMD1(CCC_GP_Spawn,  "gp_spawn");        // Phase 3.2: place a persistent world emitter
    CMD1(CCC_GP_SpawnClear, "gp_spawn_clear"); // Phase 3.2: remove world emitters
    CMD1(CCC_GP_List,   "gp_list");         // Phase 3: list loaded .pe names (optional substr filter)
    CMD1(CCC_GP_Debug,  "gp_debug");        // dump a .pe def + its GPU translation to the log
    CMD1(CCC_GP_Stats,  "gp_stats");        // per-program budget/alive counters

    // Global render profiler (vk_profiler): r_profiler 0/1/2, vk_perf = MARK dump.
    CMD4(CCC_Integer, "r_profiler", &ps_r_profiler, 0, 2);
    CMD1(CCC_VkPerf,  "vk_perf");

    // Variable Rate Shading (vk_vrs): 0 off / 1 mild / 2 aggressive + distance thresholds.
    CMD4(CCC_Integer, "r_vrs", &ps_r_vrs, 0, 2);
    CMD4(CCC_Integer, "r_vrs_force", &ps_r_vrs_force, 0, 4);
    CMD4(CCC_Integer, "r_fsinv_split", &ps_r_fsinv_split, 0, 1);   // diag: 4-way FS-invocation attribution in World/Color
    CMD4(CCC_Integer, "r_z_prepass", &ps_r_z_prepass, 0, 1);       // statics color: no z-write -> early-Z with discard (live A/B)
    CMD4(CCC_Float,   "r_ssa_px", &ps_r_ssa_px, 0.f, 16.f);        // SSA cull of plain meshes, projected-diameter px (0 = off, live)
    CMD4(CCC_Float, "r_vrs_near", &ps_r_vrs_near, 0.f, 300.f);
    CMD4(CCC_Float, "r_vrs_far",  &ps_r_vrs_far,  0.f, 500.f);

    // Frustum culling of world statics (A/B with r_cull 0).
    CMD4(CCC_Integer, "r_cull", &ps_r_cull, 0, 1);

    // GPU-driven sun shadow casters (vk_shadow_gpu) — A/B with r_gpu_shadows 0.
    CMD4(CCC_Integer, "r_sun_shadows", &ps_r_sun_shadows, 0, 1);
    CMD4(CCC_Integer, "r_gpu_shadows", &ps_r_gpu_shadows, 0, 1);

    // caster-LOD for GPU shadows: coarse geometry for distant casters (A/B with r_shadow_lod 0).
    CMD4(CCC_Integer, "r_shadow_lod", &ps_r_shadow_lod, 0, 1);
    CMD4(CCC_Float, "r_shadow_lod_dist", &ps_r_shadow_lod_dist, 5.0f, 200.0f);

    // Cascade static-map cache (A/B with r_shadow_casc_cache 0 = old per-frame raster);
    // r_shadow_casc_sun = sun-rotation degrees that forces a redraw (smaller = smoother).
    CMD4(CCC_Integer, "r_shadow_casc_cache", &ps_r_shadow_casc_cache, 0, 1);
    CMD4(CCC_Integer, "r_shadow_far_async",  &ps_r_shadow_far_async,  0, 1);
    CMD4(CCC_Float,   "r_shadow_casc_sun",   &ps_r_shadow_casc_sun,   0.005f, 1.0f);

    // Virtual Shadow Maps (WIP, default OFF). r_vsm_base = clipmap detail (m, smaller=sharper).
    CMD4(CCC_Integer, "r_vsm",          &ps_r_vsm,          0, 1);
    CMD4(CCC_Integer, "r_vsm_debug",    &ps_r_vsm_debug,    0, 1);
    CMD4(CCC_Float,   "r_vsm_base",     &ps_r_vsm_base,     8.0f, 64.0f);
    CMD4(CCC_Float,   "r_vsm_bias",     &ps_r_vsm_bias,     0.0f, 0.02f);
    CMD4(CCC_Float,   "r_vsm_bias_dyn", &ps_r_vsm_bias_dyn, 0.0f, 0.02f);
    CMD4(CCC_Float,   "r_vsm_raster_bias",  &ps_r_vsm_raster_bias,  0.0f, 16.0f);  // atlas write bias const (D16 units)
    CMD4(CCC_Float,   "r_vsm_raster_slope", &ps_r_vsm_raster_slope, 0.0f, 16.0f);  // atlas write bias slope
    CMD4(CCC_Float,   "r_vol_depth_reject", &ps_r_vol_depth_reject, 0.0f, 5.0f);   // froxel depth-rejection slack m (0 = off)
    CMD4(CCC_Float,   "r_vol_surf_clip",    &ps_r_vol_surf_clip,    0.0f, 2.0f);   // depth-rejection front shell m (0 = off)
    CMD4(CCC_Integer, "r_vsm_grass_static", &ps_r_vsm_grass_static, 0, 1);   // far-grass static-cache hybrid (0 = all-dynamic L0..L2)
    CMD4(CCC_Integer, "r_vsm_temporal", &ps_r_vsm_temporal, 0, 1);
    CMD4(CCC_Float,   "r_vsm_ta_blend", &ps_r_vsm_ta_blend, 0.0f, 0.98f);
    CMD4(CCC_Float,   "r_vsm_ta_blend_dyn", &ps_r_vsm_ta_blend_dyn, 0.0f, 0.98f);
    CMD4(CCC_Float,   "r_vsm_ta_clamp",        &ps_r_vsm_ta_clamp,        0.0f, 1.0f);   // (1) history clamp tol (0 = off)
    CMD4(CCC_Float,   "r_vsm_ta_motion",       &ps_r_vsm_ta_motion,       0.5f, 64.0f);  // (2) px of reproj motion to reach the floor
    CMD4(CCC_Float,   "r_vsm_ta_motion_floor", &ps_r_vsm_ta_motion_floor, 0.0f, 0.98f);  // (2) history weight at full motion
    CMD4(CCC_Float,   "r_vsm_ta_blend_dlss",   &ps_r_vsm_ta_blend_dlss,   0.0f, 1.0f);   // (3) history-weight scale when r_dlss on
    CMD4(CCC_Integer, "r_vsm_soft",        &ps_r_vsm_soft,        0, 32);        // stochastic PCSS filter taps (0 = legacy 3x3 PCF)
    CMD4(CCC_Integer, "r_vsm_soft_search", &ps_r_vsm_soft_search, 2, 16);        // blocker-search taps
    CMD4(CCC_Float,   "r_vsm_soft_angle",  &ps_r_vsm_soft_angle,  0.1f, 8.0f);   // sun cone HALF-angle, degrees (0.265 = physical)
    CMD4(CCC_Float,   "r_vsm_soft_range",  &ps_r_vsm_soft_range,  2.0f, 80.0f);  // max blocker distance m = widest penumbra
    CMD4(CCC_Float,   "r_vsm_soft_clamp",  &ps_r_vsm_soft_clamp,  0.0f, 1.0f);   // temporal clamp while soft is on (noise must pass)
    CMD4(CCC_Integer, "r_vsm_grass",      &ps_r_vsm_grass,      0, 1);
    CMD4(CCC_Float,   "r_vsm_grass_dist", &ps_r_vsm_grass_dist, 4.0f, 48.0f);
    CMD4(CCC_Integer, "r_vsm_cadence",    &ps_r_vsm_cadence,    0, 4);   // DYN pass update every Nth frame (0/1 = off; 2-3 ≈ 30-45Hz); static+sun always live
    CMD4(CCC_Float,   "r_vsm_cadence_still", &ps_r_vsm_cadence_still, 0.0f, 1.0f);   // dyn-skip only when cam moved < this (m/frame); moving = full-rate (no flicker)
    CMD4(CCC_Float,   "r_vsm_cadence_sun",   &ps_r_vsm_cadence_sun,   0.0f, 45.0f);  // force dyn update when sun rotated > this (deg) since last update (env-step flicker fix)
    CMD4(CCC_Float,   "r_vsm_npc_dist",   &ps_r_vsm_npc_dist,   0.0f, 500.0f);
    CMD4(CCC_Float,   "r_vsm_lod_dist",   &ps_r_vsm_lod_dist,   0.0f, 500.0f);
    CMD4(CCC_Integer, "r_vsm_mark_half",  &ps_r_vsm_mark_half,  0, 1);
    CMD4(CCC_Integer, "r_vsm_dyn_gate",   &ps_r_vsm_dyn_gate,   0, 1);
    CMD4(CCC_Integer, "r_vsm_debug_dyn",  &ps_r_vsm_debug_dyn,  0, 4);   // red overlay: dyn-atlas (NPC/grass) shadows; 2 = RAW occlusion (no static/orientation gate); 3 = PRESENCE (any dyn depth, no z-test); 4 = SUN VISIBILITY (red = shadow system says sun-lit)
    CMD4(CCC_Integer, "r_vsm_cache",      &ps_r_vsm_cache,      0, 1);
    CMD4(CCC_Integer, "r_vsm_cache_refresh", &ps_r_vsm_cache_refresh, 1, 64);
    CMD4(CCC_Integer, "r_vsm_throttle",        &ps_r_vsm_throttle, 0, 1);                 // cost-feedback LOD bias (A/B)
    CMD4(CCC_Float,   "r_vsm_throttle_budget", &ps_r_vsm_throttle_budget, 0.5f, 20.0f);   // target VSMrender ms
    CMD4(CCC_Float,   "r_vsm_throttle_max",    &ps_r_vsm_throttle_max, 0.0f, 4.0f);       // max bias (clipmap levels)
    CMD4(CCC_Integer, "r_vsm_gaze_pages",      &ps_r_vsm_gaze_pages, 0, 1024);            // gaze budget, pages/frame
    CMD4(CCC_Integer, "r_vsm_gaze_px",         &ps_r_vsm_gaze_px, 64, 65536);             // mark samples for every-frame tier

    // GPU-driven world forward pass (vk_world_gpu) — A/B with r_gpu_world 0.
    CMD4(CCC_Integer, "r_gpu_world", &ps_r_gpu_world, 0, 1);
    // Cluster-granularity world cull (Phase 1 of the cluster-LOD system) —
    // takes effect on level (re)load. r_cluster_tris = min mesh size to split.
    CMD4(CCC_Integer, "r_cluster",      &ps_r_cluster,      0, 1);
    CMD4(CCC_Integer, "r_cluster_tris", &ps_r_cluster_tris, 128, 65536);
    CMD4(CCC_Integer, "r_cluster_cache", &ps_r_cluster_cache, 0, 1);   // 0 = always rebake the DAG (repro aid)

    // Cluster DAG LOD (Phase 2): px error threshold (live) + debug view (live).
    CMD4(CCC_Float,   "r_cluster_lod",   &ps_r_cluster_lod,   0.05f, 64.0f);
    CMD4(CCC_Integer, "r_cluster_debug", &ps_r_cluster_debug, 0, 4);   // 3 = path view (red plain / green per-mesh / blue component), 4 = LOD health (red = can never coarsen)
    CMD4(CCC_Float,   "r_cluster_fade",  &ps_r_cluster_fade,  0.0f, 1.0f);
    // Phase 2.5: merge touching solid fragments into one DAG (level reload).
    CMD4(CCC_Integer, "r_cluster_merge", &ps_r_cluster_merge, 0, 1);
    // Phase 3: cluster-LOD shadow casters (live gates) + LOD budget in target
    // texels (live; VSM invalidates its static cache on change).
    CMD4(CCC_Integer, "r_shadow_cluster",  &ps_r_shadow_cluster,  0, 1);
    // Alpha-tested casters via the cluster shadow indirect path (live A/B).
    CMD4(CCC_Integer, "r_gpu_shadows_at",  &ps_r_gpu_shadows_at,  0, 1);
    // Alpha-tested statics into the VSM static atlas (live A/B; cache reset on flip).
    CMD4(CCC_Integer, "r_vsm_at",          &ps_r_vsm_at,          0, 1);
    CMD4(CCC_Integer, "r_vsm_cluster",     &ps_r_vsm_cluster,     0, 1);
    CMD4(CCC_Float,   "r_vsm_cluster_lod", &ps_r_vsm_cluster_lod, 0.1f, 16.0f);

    // Clustered forward / Forward+ (vk_clustered) — A/B with r_clustered 0.
    // r_clustered_debug 1 = per-cluster light-count heatmap on the world.
    CMD4(CCC_Integer, "r_clustered",       &ps_r_clustered,       0, 1);
    CMD4(CCC_Integer, "r_clustered_debug", &ps_r_clustered_debug, 0, 1);
    // Framegraph: coalesce the SSAO/VRS/VSM depth-read round-trips into one (A/B with 0).
    CMD4(CCC_Integer, "r_fg_coalesce",     &ps_r_fg_coalesce,     0, 1);
    // Declared pass dependencies: 1 = a read-before-write is FATAL, 0 = log only.
    CMD4(CCC_Integer, "r_fg_validate",     &ps_r_fg_validate,     0, 1);
    // Inter-pass barriers: 0 = one conservative global barrier per pair,
    // 1 = derived from the declared reads/writes, 2 = derived + audit log.
    CMD4(CCC_Integer, "r_fg_barriers",     &ps_r_fg_barriers,     0, 2);
    // Вторичные командные буферы для записи пассов (шаг к записи в несколько потоков).
    // 0 = запись прямо в primary (как было всегда), 1 = каждый пасс в свой
    // вторичный буфер на главном потоке (этап А, механизм), 2 = помеченные
    // MarkPassParallel пассы пишутся НА ВОРКЕРАХ (этап Б). NoSave: этап Б ещё
    // не проверен на всех сценах, и залипший 2 в user.ltx превратил бы отладку
    // чужого краша в поиск призраков.
    CMD4_NOSAVE(CCC_Integer, "r_mt_record",       &ps_r_mt_record,       0, 2);
    CMD4(CCC_Integer, "r_sl_log",          &ps_r_sl_log,          0, 1);
    CMD1(CCC_VkPassDeps, "vk_pass_deps");
    // Async compute: run compute on a dedicated queue overlapping graphics (vk_async).
    CMD4(CCC_Integer, "r_async",           &ps_r_async,           0, 1);
    // Sky specular IBL (vk_ibl): prefiltered sky reflections + sun GGX glint.
    CMD4(CCC_Integer, "r_ibl",             &ps_r_ibl,             0, 1);
    CMD4(CCC_Float,   "r_ibl_spec",        &ps_r_ibl_spec,        0.f, 4.f);
    CMD4(CCC_Integer, "r_ibl_debug",       &ps_r_ibl_debug,       0, 1);
    // Diffuse sky irradiance via SH9 (0 = A/B against the prefiltered probe mip).
    CMD4(CCC_Integer, "r_sky_sh",          &ps_r_sky_sh,          0, 1);
    CMD4(CCC_Float,   "r_sky_sh_ground",   &ps_r_sky_sh_ground,   0.f, 1.f);
    CMD4(CCC_Integer, "r_sky_sh_debug",    &ps_r_sky_sh_debug,    0, 1);
    // Procedural Rayleigh+Mie sky: feeds the dome AND the light probe from one model.
    CMD4(CCC_Integer, "r_sky_proc",        &ps_r_sky_proc,        0, 1);
    CMD4(CCC_Float,   "r_sky_intensity",   &ps_r_sky_intensity,   0.f, 200.f);
    CMD4(CCC_Float,   "r_sky_turbidity",   &ps_r_sky_turbidity,   0.f, 20.f);
    CMD4(CCC_Float,   "r_sky_mie_g",       &ps_r_sky_mie_g,       0.f, 0.99f);
    CMD4(CCC_Integer, "r_sky_sun_from_atmo", &ps_r_sky_sun_from_atmo, 0, 1);
    CMD4(CCC_Float,   "r_sky_sun_scale",   &ps_r_sky_sun_scale,   0.f, 40.f);
    // Volumetric clouds (raymarched). Needs r_sky_proc for its lighting to make sense.
    CMD4(CCC_Integer, "r_clouds_vol",           &ps_r_clouds_vol,           0, 1);
    CMD4(CCC_Float,   "r_clouds_coverage",      &ps_r_clouds_coverage,      0.f, 1.f);
    CMD4(CCC_Float,   "r_clouds_density",       &ps_r_clouds_density,       0.f, 8.f);
    CMD4(CCC_Float,   "r_clouds_detail",        &ps_r_clouds_detail,        0.f, 1.f);
    CMD4(CCC_Float,   "r_clouds_bottom",        &ps_r_clouds_bottom,        200.f, 12000.f);
    CMD4(CCC_Float,   "r_clouds_top",           &ps_r_clouds_top,           400.f, 20000.f);
    CMD4(CCC_Float,   "r_clouds_shape_scale",   &ps_r_clouds_shape_scale,   0.000001f, 0.01f);
    CMD4(CCC_Float,   "r_clouds_detail_scale",  &ps_r_clouds_detail_scale,  0.000001f, 0.05f);
    CMD4(CCC_Float,   "r_clouds_weather_scale", &ps_r_clouds_weather_scale, 0.0000001f, 0.01f);
    CMD4(CCC_Float,   "r_clouds_wind_dir",      &ps_r_clouds_wind_dir,      0.f, 360.f);
    CMD4(CCC_Float,   "r_clouds_wind_speed",    &ps_r_clouds_wind_speed,    0.f, 200.f);
    CMD4(CCC_Float,   "r_clouds_phase_g",       &ps_r_clouds_phase_g,       0.f, 0.99f);
    CMD4(CCC_Float,   "r_clouds_phase_g_back",  &ps_r_clouds_phase_g_back,  0.f, 0.99f);
    CMD4(CCC_Float,   "r_clouds_extinction",    &ps_r_clouds_extinction,    0.001f, 2.f);
    CMD4(CCC_Float,   "r_clouds_powder",        &ps_r_clouds_powder,        0.f, 1.f);
    CMD4(CCC_Float,   "r_clouds_sun",           &ps_r_clouds_sun,           0.f, 200.f);
    CMD4(CCC_Float,   "r_clouds_ambient",       &ps_r_clouds_ambient,       0.f, 20.f);
    CMD4(CCC_Integer, "r_clouds_steps",         &ps_r_clouds_steps,         16, 256);
    CMD4(CCC_Float,   "r_clouds_max_dist",      &ps_r_clouds_max_dist,      5000.f, 400000.f);
    CMD4(CCC_Float,   "r_clouds_cirrus",        &ps_r_clouds_cirrus,        0.f, 2.f);
    CMD4(CCC_Float,   "r_clouds_cirrus_alt",    &ps_r_clouds_cirrus_alt,    2000.f, 20000.f);
    CMD4(CCC_Float,   "r_clouds_cirrus_scale",  &ps_r_clouds_cirrus_scale,  0.0000001f, 0.01f);
    CMD4(CCC_Integer, "r_clouds_debug",         &ps_r_clouds_debug,         0, 2);
    CMD4(CCC_Integer, "r_clouds_weather",       &ps_r_clouds_weather,       0, 1);

    // Hi-Z occlusion cull of the GPU-driven static color pass (vk_world_gpu).
    CMD4(CCC_Integer, "r_hzb_cull", &ps_r_hzb_cull, 0, 1);
    CMD4(CCC_Integer, "r_tonemap_lean", &ps_r_tonemap_lean, 0, 1);   // lean tonemap composite (Switch: 1.7 ms of dead shader code at 720p)
    // Two-phase: cull the DEPTH PREPASS too, and take over the colour set with it
    // (supersedes r_hzb_cull — see the declaration).
    CMD4(CCC_Integer, "r_hzb_prepass", &ps_r_hzb_prepass, 0, 1);
    CMD4(CCC_Integer, "r_world_bindless", &ps_r_world_bindless, 0, 1);   // ~600 per-material world groups -> ~40 (applies at the next level load)
    CMD4_NOSAVE(CCC_Integer, "r_world_push_hoist", &ps_r_world_push_hoist, 0, 1);   // A/B: viewProj pushed per layout flip (1) vs per group (0)
    CMD4_NOSAVE(CCC_Integer, "r_bones_mt", &ps_r_bones_mt, 0, 2);
    CMD4(CCC_Integer, "r_wm_async", &ps_r_wm_async, 0, 1);   // blood decals on NPCs: 1 core-2 worker, not waited for; 0 second thread, joined
    CMD4(CCC_Integer, "r_particles_mt", &ps_r_particles_mt, 0, 2);   // CPU particle sim: 2 = overlapped with the scene collection (core-2 worker + pool), 1 = fork-join on the TTAPI pool (R4 calculate_particles_async), 0 = serial   // 1 = every skeleton on the TTAPI pool, 2 = only those with no bone callbacks (no xrGame code on a worker)
    CMD4(CCC_Integer, "r_particles_core", &ps_r_particles_core, 0, 2);   // the core of the r_particles_mt 2 worker
    CMD4(CCC_Integer, "r_world_geom_stats", &ps_r_world_geom_stats, 0, 2);   // diag: HW counters on World/Depth + World/Statics (mutually exclusive with r_tree_stats / r_fsinv_split)

    // GPU-driven LOD imposters (vk_LODManager): 0 = CPU walk (A/B).
    CMD4(CCC_Integer, "r_lods_gpu", &ps_r_lods_gpu, 0, 1);

    // Froxel volumetric lighting (vk_volumetrics) — P1: sun god rays + depth fog.
    CMD4(CCC_Integer, "r_vol",           &ps_r_vol,           0, 1);
    CMD4(CCC_Float,   "r_vol_density",   &ps_r_vol_density,   0.0f, 1.0f);
    CMD4(CCC_Float,   "r_vol_height",    &ps_r_vol_height,    0.0f, 2.0f);
    CMD4(CCC_Float,   "r_vol_g",         &ps_r_vol_g,         0.0f, 0.95f);
    CMD4(CCC_Float,   "r_vol_intensity", &ps_r_vol_intensity, 0.0f, 8.0f);
    // Atmospheric scattering (r_atmo) — Rayleigh/Mie aerial perspective (needs r_vol).
    CMD4(CCC_Integer, "r_atmo",          &ps_r_atmo,          0, 1);
    CMD4(CCC_Float,   "r_atmo_rayleigh", &ps_r_atmo_rayleigh, 0.0f, 8.0f);
    CMD4(CCC_Float,   "r_atmo_mie",      &ps_r_atmo_mie,      0.0f, 8.0f);
    CMD4(CCC_Float,   "r_atmo_mie_g",    &ps_r_atmo_mie_g,    0.0f, 0.95f);
    // Auto-exposure temporal adaptation (eye adaptation) — seconds; 0 = instant.
    // Split: r_exp_adapt = exposure falling (image darkening), r_exp_adapt_up =
    // exposure rising. Set them equal to restore the old symmetric behaviour.
    CMD4(CCC_Float,   "r_exp_adapt",     &ps_r_exp_adapt,     0.0f, 5.0f);
    CMD4(CCC_Float,   "r_exp_adapt_up",  &ps_r_exp_adapt_up,  0.0f, 8.0f);
    // Auto-exposure user knobs (all live; see vk_exposure.h).
    CMD4(CCC_Float,   "r_expo",          &ps_r_expo,          0.1f, 4.0f);
    CMD4(CCC_Float,   "r_expo_gray",     &ps_r_expo_gray,     0.0f, 1.0f);
    CMD4(CCC_Float,   "r_expo_min",      &ps_r_expo_min,      0.1f, 2.0f);
    CMD4(CCC_Float,   "r_expo_max",      &ps_r_expo_max,      0.5f, 8.0f);
    // Metering estimator + its instrument. NoSave both: a meter mode stuck in
    // user.ltx would poison every later exposure A/B, and a debug stream left on
    // would spam the next session's log (see the r_expo_meter note at the top).
    CMD4(CCC_Integer, "r_expo_interval", &ps_r_expo_interval, 1, 8);   // scene mips + exposure meter every Nth frame (the adaptation keeps its speed)
    CMD4(CCC_FloatNoSave, "r_expo_meter_mix", &ps_r_expo_meter_mix, 0.f, 1.f);  // mode 1 dial: 0 = old arithmetic look (dark, jumpy), 1 = pure log-average (bright, walls wash out near windows)
    CMD4(CCC_IntegerNoSave, "r_expo_debug", &ps_r_expo_debug, 0, 1);   // [VK Expo] estimators + applied, ~1/s
    extern int g_vk_ui_dump;
    CMD4(CCC_IntegerNoSave, "vk_ui_dump", &g_vk_ui_dump, 0, 1);        // [VK-UI dump] every UI draw of the next frame
    CMD4(CCC_FloatNoSave, "r_expo_indoor_max",  &ps_r_expo_indoor_max,  1.f, 4.f);   // ceiling multiplier on r_expo_max when enclosed
    CMD4(CCC_FloatNoSave, "r_expo_indoor_dist", &ps_r_expo_indoor_dist, 1.f, 30.f);  // how far up (m) the roof rays look
    CMD4(CCC_Float,   "r_vol_amb",       &ps_r_vol_amb,       0.0f, 1.0f);
    CMD4(CCC_Float,   "r_vol_ambient",   &ps_r_vol_ambient,   0.0f, 4.0f);   // fog sky-fill tint scale (lower = darker night air; was tied to r_ambient_floor)
    CMD4(CCC_Float,   "r_vol_indoor",    &ps_r_vol_indoor,    0.0f, 16.0f);
    CMD4(CCC_Float,   "r_vol_sun",       &ps_r_vol_sun,       0.0f, 16.0f);
    CMD4(CCC_Float,   "r_vol_lights",    &ps_r_vol_lights,    0.0f, 16.0f);
    CMD4(CCC_Float,   "r_vol_lights_g",  &ps_r_vol_lights_g,  0.0f, 0.95f);   // local-light forward scatter (torch-into-camera glare; sep. from sun r_vol_g)
    CMD4(CCC_Float,   "r_torch_vol",     &ps_r_torch_vol,     0.0f, 8.0f);    // fog shaft strength for handheld torches only (fixtures keep the ×3 lamp boost)
    CMD4(CCC_Float,   "r_vol_smoke",     &ps_r_vol_smoke,     0.0f, 16.0f);
    CMD4(CCC_Float,   "r_vol_smoke_clamp", &ps_r_vol_smoke_clamp, 0.0f, 64.0f);
    CMD4(CCC_Float,   "r_vol_smoke_inject",  &ps_r_vol_smoke_inject,  0.0f, 16.0f);
    CMD4(CCC_Float,   "r_vol_smoke_density", &ps_r_vol_smoke_density, 0.0f, 16.0f);
    CMD4(CCC_Integer, "r_vol_smoke_debug",   &ps_r_vol_smoke_debug,   0, 3);
    CMD4(CCC_Integer, "r_ptcl_mt",           &ps_r_ptcl_mt,           0, 1);
    CMD4(CCC_Integer, "r_core1_prio",        &ps_r_core1_prio,        0x2C, 0x3B);
    CMD4(CCC_Float,   "r_bones_lod_dist",    &ps_r_bones_lod_dist,    0.f, 500.f);
    CMD4(CCC_Float,   "r_vol_smoke_dist_full", &ps_r_vol_smoke_dist_full, 0.0f, 300.0f);
    CMD4(CCC_Float,   "r_vol_smoke_dist",    &ps_r_vol_smoke_dist,    0.0f, 300.0f);
    CMD4(CCC_Float,   "r_vol_smoke_footprint", &ps_r_vol_smoke_footprint, 0.0f, 3.0f);
    CMD4(CCC_Float,   "r_vol_smoke_shadow",  &ps_r_vol_smoke_shadow,  0.0f, 8.0f);
    CMD4(CCC_Float,   "r_vol_smoke_shadow_step", &ps_r_vol_smoke_shadow_step, 0.05f, 4.0f);
    CMD4(CCC_Float,   "r_vol_noise",       &ps_r_vol_noise,       0.0f, 1.0f);
    CMD4(CCC_Float,   "r_vol_noise_scale", &ps_r_vol_noise_scale, 0.02f, 2.0f);
    CMD4(CCC_Float,   "r_vol_noise_speed", &ps_r_vol_noise_speed, 0.0f, 1.0f);
    CMD4(CCC_Float,   "r_vol_soft",      &ps_r_vol_soft,      0.5f, 8.0f);
    CMD4(CCC_Integer, "r_vol_ta",        &ps_r_vol_ta,        0, 1);
    CMD4(CCC_Integer, "r_vol_upsample",  &ps_r_vol_upsample,  0, 1);   // volume reconstruction filter in the composite (0 = old 1-tap)
    CMD4(CCC_Float,   "r_fog_dist",      &ps_r_fog_dist,      0.0f, 8.0f);   // forward distance-fog range scale (0 = off, 1 = weather)
    CMD4(CCC_Float,   "r_sky_fog_horizon", &ps_r_sky_fog_horizon, 0.0f, 1.0f);   // sky -> fog colour below this elevation (0 = off)
    CMD4(CCC_Integer, "r_fog_sky", &ps_r_fog_sky, 0, 1);   // 1 = the fog fades into the sky along the view ray
    CMD4(CCC_Integer, "r_vol_ground_debug", &ps_r_vol_ground_debug, 0, 3);   // 1 = height above baked terrain, 2 = micro-relief in the rain map, 3 = baked grass canopy (view with r_vol_debug 1)
    CMD4(CCC_Float,   "r_vol_albedo",        &ps_r_vol_albedo,        0.0f, 1.0f);  // single-scatter albedo (sigma_s = sigma_t x albedo)
    CMD4(CCC_Integer, "r_vol_ms",            &ps_r_vol_ms,            1, 4);        // multiple-scattering octaves (1 = single scatter)
    CMD4(CCC_Float,   "r_vol_g_back",        &ps_r_vol_g_back,       -0.9f, 0.0f);  // backward phase lobe g
    CMD4(CCC_Float,   "r_vol_g_mix",         &ps_r_vol_g_mix,         0.0f, 1.0f);  // backward lobe weight (0 = single lobe)
    CMD4(CCC_Float,   "r_vol_mist",          &ps_r_vol_mist,          0.0f, 2.0f);   // V-1 layer 2: ground-mist density at ground level (0 = single layer)
    CMD4(CCC_Float,   "r_vol_mist_h",        &ps_r_vol_mist_h,        0.25f, 64.0f); // ground-mist thickness (m)
    CMD4(CCC_Float,   "r_vol_mist_g",        &ps_r_vol_mist_g,        0.0f, 0.95f);  // ground-mist phase (droplets scatter less directionally than dust)
    CMD4(CCC_Float,   "r_vol_mist_relief",   &ps_r_vol_mist_relief,   0.0f, 40.0f);  // terrain drop (m) the mist fades in over (0 = layer is everywhere)
    CMD4(CCC_Float,   "r_vol_mist_noise",       &ps_r_vol_mist_noise,       0.0f, 2.0f);    // mist height-warp: billowing top instead of a flat lid (0 = smooth exponential)
    CMD4(CCC_Float,   "r_vol_mist_noise_scale", &ps_r_vol_mist_noise_scale, 0.005f, 0.5f);  // size of the billows (1/m; 0.05 = ~20 m)
    CMD4(CCC_Float,   "r_vol_mist_micro",    &ps_r_vol_mist_micro,    0.0f, 4.0f);   // ABSOLUTE density of mist pooling in ruts/craters, independent of r_vol_mist (0 = off)
    CMD4(CCC_Float,   "r_vol_mist_micro_h",  &ps_r_vol_mist_micro_h,  0.02f, 2.0f);  // dip depth (m) at which the micro effect saturates
    CMD4(CCC_Integer, "r_vol_mist_fade",     &ps_r_vol_mist_fade,     0, 3);         // thin the bank you are STANDING IN: 0 off, 1 immersion, 2 near-camera, 3 both
    CMD4(CCC_Float,   "r_vol_mist_inside",   &ps_r_vol_mist_inside,   0.0f, 64.0f);  // what it thins down to, as a MULTIPLE OF r_vol_density (2 = twice the general haze)
    CMD4(CCC_Float,   "r_vol_mist_near",     &ps_r_vol_mist_near,     1.0f, 200.0f); // radius (m) of the mode-2 clearing around the camera
    CMD4(CCC_Float,   "r_vol_mist_inside_h", &ps_r_vol_mist_inside_h, 1.0f, 200.0f); // height (m) the immersion fades out over — a hilltop, NOT a rooftop
    CMD4(CCC_Float,   "r_vol_canopy",        &ps_r_vol_canopy,        0.0f, 8.0f);   // grass canopy occlusion of the fog's sun term (0 = off, grass invisible to the sun as before)
    CMD4(CCC_Float,   "r_vol_canopy_h",      &ps_r_vol_canopy_h,      0.1f, 4.0f);   // canopy height multiplier over the authored detail-model height
    CMD4(CCC_Integer, "r_vol_weather",       &ps_r_vol_weather,       0, 2);         // V-2: weather drives the medium (0 = manual cvars only, 1 = on, 2 = on + log the inputs)
    CMD4(CCC_Float,   "r_vol_w_clouds",      &ps_r_vol_w_clouds,      0.0f, 1.0f);   // overcast damping of the fog's sun term (1 = full overcast kills it)
    CMD4(CCC_Float,   "r_vol_w_sky",         &ps_r_vol_w_sky,         0.0f, 4.0f);   // overcast lift of the sky term
    CMD4(CCC_Float,   "r_vol_w_flat",        &ps_r_vol_w_flat,        0.0f, 1.0f);   // overcast flattens the phase toward isotropic (the knob that actually READS as overcast)
    CMD4(CCC_Float,   "r_vol_w_fog",         &ps_r_vol_w_fog,         0.0f, 2.0f);   // extra MIST from fog_density (0 = off; fog_density means sight distance, not ground fog)
    CMD4(CCC_Float,   "r_vol_w_fog_dust",    &ps_r_vol_w_fog_dust,    0.0f, 8.0f);   // dust density gain at fog_density 1 (small: dust makes rays, not fog)
    CMD4(CCC_Float,   "r_vol_w_rain",        &ps_r_vol_w_rain,        0.0f, 2.0f);   // mist density added at full wetness
    CMD4(CCC_Float,   "r_vol_w_wet_force",   &ps_r_vol_w_wet_force,  -1.0f, 1.0f);   // test hook: -1 = real weather, 0..1 = pretend this wetness (see the rain fog without waiting for rain)
    CMD4(CCC_Integer, "r_vol_term",          &ps_r_vol_term,          0, 5);        // isolate one in-scatter source (1 sun 2 ambient 3 lights 4 atmo 5 smoke)
    CMD4(CCC_Float,   "r_vol_ta_blend",  &ps_r_vol_ta_blend,  0.0f, 0.98f);
    CMD4(CCC_Integer, "r_vol_debug",     &ps_r_vol_debug,     0, 4);   // 1 = raw in-scatter, 3 = froxel sun visibility, 4 = occlusion source (green VSM / red miss->skyVis / blue cascade)

    // Dynamic-light terrain/static occlusion (ground-height map + per-light march).
    CMD4(CCC_Integer, "r_light_occ", &ps_r_light_occ, 0, 1);
    CMD4(CCC_IntegerNoSave, "r_world_flat", &ps_r_world_flat, 0, 1);   // PROBE: statics shaded as diffuse x baked light only (R1-class). Set BEFORE the level loads.
    CMD4(CCC_IntegerNoSave, "r_light_cap", &ps_r_light_cap, 0, 256);   // see vk_light.cpp CollectFrame
    CMD4(CCC_Integer, "r_shadow_lite", &ps_r_shadow_lite, 0, 1);       // R1-hybrid option: characters cast capsule shadows instead of entering the sun cascades; no point-light shadow cubes
    CMD4(CCC_Float, "r_capsule_shadow", &ps_r_capsule_shadow, 0.f, 1.f);
    CMD4(CCC_IntegerNoSave, "r_depth_prepass", &ps_r_depth_prepass, 0, 1);   // 0 = skip the prepass draws (statics, trees, NPCs); the scope still clears depth
    CMD4(CCC_FloatNoSave, "r_tree_mesh_dist", &ps_r_tree_mesh_dist, 10.f, 1000.f);   // where the full tree mesh hands over to the billboard
    CMD4(CCC_IntegerNoSave, "r_tree_prepass", &ps_r_tree_prepass, 0, 1);              // 0 = skip the trees' camera depth prepass
    CMD4(CCC_Integer, "r_grass", &ps_r_grass, 0, 1);   // 0 = skip the Grass pass entirely (it had no gate; r__detail_radius cannot go below 70 m)
    CMD4(CCC_Integer, "r_trees", &ps_r_trees, 0, 1);   // 0 = skip the Trees pass entirely (Switch experiment: Trees cost ~12 ms of GPU)

    // World heightmap tessellation (live, no restart) — see vk_render_queue.cpp.
    CMD4(CCC_Float, "r_tess", &ps_r_tess, 0.f, 1.f);
    CMD4(CCC_Float, "r_tess_max", &ps_r_tess_max, 1.f, 64.f);
    CMD4(CCC_Float, "r_tess_near", &ps_r_tess_near, 0.f, 50.f);
    CMD4(CCC_Float, "r_tess_far", &ps_r_tess_far, 1.f, 100.f);
    CMD4(CCC_Float, "r_tess_height", &ps_r_tess_height, 0.f, 4.f);
    CMD4(CCC_Float, "r_tess_pn", &ps_r_tess_pn, 0.f, 1.f);
    CMD4(CCC_Integer, "r_pom", &ps_r_pom, 0, 1);
    CMD4(CCC_Float, "r_pom_height", &ps_r_pom_height, 0.f, 0.1f);
    CMD4(CCC_Float, "r_pom_steps", &ps_r_pom_steps, 8.f, 64.f);
    CMD4(CCC_Float, "r_pom_far", &ps_r_pom_far, 1.f, 50.f);
    CMD4(CCC_Float, "r_pom_blur", &ps_r_pom_blur, 0.f, 4.f);
    CMD4(CCC_Float, "r_pom_normal", &ps_r_pom_normal, 0.f, 4.f);
    CMD4(CCC_Float, "r_pom_shadow", &ps_r_pom_shadow, 0.f, 3.f);
    CMD4(CCC_Float, "r_pom_ao", &ps_r_pom_ao, 0.f, 3.f);
    CMD4(CCC_Integer, "r_pom_debug", &ps_r_pom_debug, 0, 2);    // 1 = AO x shadow, 2 = self-shadow only
    CMD4(CCC_Integer, "r_ao_flat", &ps_r_ao_flat, 0, 1);   // debug: kill ALL ambient occlusion (incl. baked lmap)
    CMD4(CCC_Integer, "r_shade_debug", &ps_r_shade_debug, 0, 11);   // debug: isolate lighting components (see ps_r_shade_debug)
    CMD4(CCC_Float, "r_pom_ceil", &ps_r_pom_ceil, 0.f, 1.f);     // POM strength on ceilings (down-facing)
    CMD4(CCC_Float, "r_pom_floor", &ps_r_pom_floor, 0.f, 1.f);   // POM strength on floors (up-facing)
    CMD4(CCC_Integer, "r_pom_terrain", &ps_r_pom_terrain, 0, 1); // terrain POM (experimental, default off)
    CMD4(CCC_Float,   "r_pom_zoff", &ps_r_pom_zoff, 0.f, 3.f);   // terrain POM depth offset (SSFX; 1 = 0.11 m sink)
    CMD4(CCC_Integer, "r_terra_cache", &ps_r_terra_cache, 0, 1); // terrain composite cache (1 march vs 4; perf)
    CMD4(CCC_Integer, "r_terra_cone", &ps_r_terra_cone, 0, 1);   // cone-step march on the cache (vs fixed layers)
    CMD4(CCC_Float, "r_terra_blend", &ps_r_terra_blend, 0.f, 1.f); // detail blend: 0 = GAMMA soft mask, 0.25 = sharp height-blend
    CMD4(CCC_Integer, "r_terra_horizon", &ps_r_terra_horizon, 0, 1); // baked sun-horizon self-shadow (vs 8-tap march)
    CMD4(CCC_Float, "r_terrain_normal", &ps_r_terrain_normal, 0.f, 3.f); // terrain detail normal-mapping strength
    CMD4(CCC_Float, "r_terrain_ao", &ps_r_terrain_ao, 0.f, 1.f);         // terrain micro contact AO strength
    CMD4(CCC_Integer, "r_terrain_debug", &ps_r_terrain_debug, 0, 9);     // ...5 beam directLit,6 beam mask,7 beam gap(red),8 ground-deposit sampled in-scatter,9 deposit gate(red)/inscat(green)
    CMD4(CCC_Float, "r_terrain_gloss", &ps_r_terrain_gloss, 0.f, 2.f);   // terrain dry sun-gloss strength
    CMD4(CCC_Integer, "r_puddle_debug", &ps_r_puddle_debug, 0, 2);       // 0 off, 1 coverage, 2 micro-height/flow
    CMD4(CCC_Integer, "r_puddle_sss", &ps_r_puddle_sss, 0, 2);           // 0 off, 1 our tuned water body, 2 SSFX-strict shading
    CMD4(CCC_Float, "r_puddle_level", &ps_r_puddle_level, 0.f, 1.f);     // puddle coverage (more/larger puddles)
    CMD4(CCC_Float, "r_puddle_scale", &ps_r_puddle_scale, 0.1f, 6.f);    // puddle size (bigger = smaller pools)
    CMD4(CCC_Float, "r_puddle_geo", &ps_r_puddle_geo, 0.f, 1.f);         // puddles follow REAL ground dips (Surface Field concavity)
    CMD4(CCC_Float, "r_wet_dist", &ps_r_wet_dist, 20.f, 400.f);          // range (m) wet shading survives to (SSFX ~200)
    CMD4(CCC_Float, "r_terrain_detail_dist", &ps_r_terrain_detail_dist, 30.f, 400.f); // range (m) terrain detail normal/AO/gloss survive to
    CMD4(CCC_Float, "r_bolt_flash", &ps_r_bolt_flash, 0.f, 4.f);         // lightning lifts the sky/hemi light (0 = no flash)
    CMD4(CCC_Float, "r_bump", &ps_r_bump, 0.f, 2.f);                     // static material normal-map strength (0 = off)
    CMD4(CCC_Integer, "r_bump_debug", &ps_r_bump_debug, 0, 2);           // 1 = decoded world normal, 2 = material gloss
    CMD4(CCC_Float, "r_gloss_scale", &ps_r_gloss_scale, 0.f, 4.f);       // material gloss -> IBL roughness (R4 r2_gloss_factor)
    CMD4(CCC_Integer, "r_sf", &ps_r_sf, 0, 1);                  // Surface Field master enable
    CMD4(CCC_Integer, "r_sf_debug", &ps_r_sf_debug, 0, 5);      // 0 off,1 height,2 slope,3 curvature,4 sky,5 canopy
    CMD4(CCC_Float, "r_sf_eps", &ps_r_sf_eps, 0.25f, 8.f);      // derive finite-difference epsilon (m)
    CMD4(CCC_Float, "r_snow", &ps_r_snow, 0.f, 1.f);            // TARGET snow coverage (Surface Field consumer)
    CMD4(CCC_Float, "r_snow_rate", &ps_r_snow_rate, 0.f, 5.f);  // snow accumulate/melt speed (per sec)
    CMD4(CCC_Integer, "r_snow_deform", &ps_r_snow_deform, 0, 1);              // footprint deformation enable
    CMD4(CCC_Float, "r_snow_deform_depth", &ps_r_snow_deform_depth, 0.f, 0.5f);   // print press depth (m)
    CMD4(CCC_Float, "r_snow_deform_radius", &ps_r_snow_deform_radius, 0.05f, 1.f); // print radius (m)
    CMD4(CCC_Float, "r_snow_deform_time", &ps_r_snow_deform_time, 1.f, 300.f);     // print lifetime (sec, time-decay)
    CMD4(CCC_Integer, "r_snow_deform_tex", &ps_r_snow_deform_tex, 0, 1);           // dense deform texture (vk_deform) vs stamp loop
    CMD4(CCC_Integer, "r_snow_mesh", &ps_r_snow_mesh, 0, 2);                       // dense snow surface mesh (1=on, 2=debug magenta)
    CMD4(CCC_Float, "r_snow_berm", &ps_r_snow_berm, 0.f, 2.f);                     // displaced-snow berm height around prints
    CMD4(CCC_Float, "r_snow_ripple", &ps_r_snow_ripple, 0.f, 4.f);                 // wind-ripple relief strength on open snow
    CMD4(CCC_Float, "r_snow_rough", &ps_r_snow_rough, 0.f, 1.f);                   // trail/print imperfection (width/depth/edge noise)
    CMD4(CCC_Float, "r_mud_deform", &ps_r_mud_deform, 0.f, 2.f);                   // mud footprints on soft terrain (0 = off)
    CMD4(CCC_Float, "r_mud_depth", &ps_r_mud_depth, 0.f, 1.f);                     // mud print POM carve depth (fraction)
    CMD4(CCC_Integer, "r_water_sim", &ps_r_water_sim, 0, 1);             // water flow sim master enable
    CMD4(CCC_Float, "r_water_rain", &ps_r_water_rain, 0.f, 5.f);         // sim rain input rate (depth/s)
    CMD4(CCC_Float, "r_water_evap", &ps_r_water_evap, 0.f, 20.f);        // sim leak rate (exp drain ∝ amount)
    CMD4(CCC_Float, "r_water_flow", &ps_r_water_flow, 0.f, 4.f);         // sim downhill accel (flow speed / streams)
    CMD4(CCC_Float, "r_water_murk", &ps_r_water_murk, 0.f, 8.f);         // volumetric absorption /m (deeper feel)
    CMD4(CCC_Float, "r_water_refract", &ps_r_water_refract, 0.f, 0.2f);  // bottom refraction strength

    // Water BODIES (vk_pass_water) — see the ps_r_wtr_* block for the split
    // between this and the r_water_* puddle sim above.
    CMD4(CCC_Integer, "r_wtr",        &ps_r_wtr,        0, 1);
    CMD4(CCC_Integer, "r_wtr_lean",   &ps_r_wtr_lean,   0, 1);
    // NOT savable: a debug view is set for one measurement and the client writes
    // user.ltx on exit — that is how a magenta-water build "randomly" survives a
    // restart. Same rule as every other look-at-it-once cvar in this engine.
    // ⚠ EXTEND THE RANGE WITH THE MODE. `r_wtr_debug 6` was silently REFUSED for
    // a while because the range still said 0..5, and the screenshot that came
    // back was an ordinary one — a debug view that lies about being on is worse
    // than no debug view.
    CMD4(CCC_IntegerNoSave, "r_wtr_debug", &ps_r_wtr_debug, 0, 9);   // 6 = fetch/shelter, 7 = pool mask, 8 = ripple field, 9 = spectral field
    CMD4(CCC_Float,   "r_wtr_wave",   &ps_r_wtr_wave,   0.f,  1.5f);
    CMD4(CCC_Float,   "r_wtr_calm",   &ps_r_wtr_calm,   0.f,  1.0f);
    CMD4(CCC_Float,   "r_wtr_scale",  &ps_r_wtr_scale,  0.02f, 4.f);
    CMD4(CCC_Float,   "r_wtr_speed",  &ps_r_wtr_speed,  0.f,  4.f);
    CMD4(CCC_Float,   "r_wtr_murk",   &ps_r_wtr_murk,   0.f,  6.f);
    CMD4(CCC_Float,   "r_wtr_refl",   &ps_r_wtr_refl,   0.f,  3.f);
    CMD4(CCC_Float,   "r_wtr_glint",  &ps_r_wtr_glint,  0.f,  8.f);
    CMD4(CCC_Float,   "r_wtr_detail", &ps_r_wtr_detail, 5.f,  400.f);
    CMD4(CCC_Float,   "r_wtr_rough",  &ps_r_wtr_rough,  0.005f, 0.5f);
    CMD4(CCC_Vector3, "r_wtr_color",  &ps_r_wtr_color,  (Fvector3{}), (Fvector3{ 1.f, 1.f, 1.f }));
    CMD4(CCC_Float,   "r_wtr_shore",  &ps_r_wtr_shore,  0.f, 3.f);    // waterline fade-out depth (m)
    CMD4(CCC_Float,   "r_wtr_foam",   &ps_r_wtr_foam,   0.f, 2.f);    // shore foam strength (0 = off)
    CMD4(CCC_Float,   "r_wtr_foam_w", &ps_r_wtr_foam_w, 0.02f, 3.f);  // foam band depth (m)
    CMD4(CCC_Float,   "r_wtr_micro",  &ps_r_wtr_micro,  0.f, 0.3f);   // capillary ripple, weather-independent
    CMD4(CCC_Float,   "r_wtr_wind",   &ps_r_wtr_wind,   0.f, 3.f);    // how hard the weather's wind drives the swell
    // Interactive ripple sim + tessellation.
    CMD4(CCC_Integer, "r_wtr_sim",        &ps_r_wtr_sim,        0, 1);
    CMD4(CCC_Float,   "r_wtr_sim_size",   &ps_r_wtr_sim_size,   16.f, 256.f);
    // NOT savable, all four: these are the constants OF THE SCHEME, not settings.
    // They decide whether a ripple stays in the puddle it was made in, and they get
    // retuned in the source as the sim is understood better — so the source must
    // win. A saved copy silently pins the machine to whatever number was current
    // the first time it quit cleanly, which is exactly how "I walk in one puddle
    // and every puddle ripples" came back after being fixed and measured.
    CMD4(CCC_FloatNoSave, "r_wtr_sim_damp",  &ps_r_wtr_sim_damp,  0.9f, 0.9999f);
    CMD4(CCC_FloatNoSave, "r_wtr_sim_speed", &ps_r_wtr_sim_speed, 0.01f, 0.5f);
    CMD4(CCC_FloatNoSave, "r_wtr_sim_shore", &ps_r_wtr_sim_shore, 0.5f, 1.f);  // 1 = reflecting rim (waves fill the pool)
    CMD4(CCC_FloatNoSave, "r_wtr_sim_reach", &ps_r_wtr_sim_reach, 0.f, 64.f);  // 0 = ripples cross the whole sheet
    // A fallback gate, set once to answer "is the mask doing anything?" — same rule
    // as every other look-at-it-once cvar.
    CMD4(CCC_IntegerNoSave, "r_wtr_sim_pools", &ps_r_wtr_sim_pools, 0, 1);   // 0 = ripples cross between puddles again
    CMD4(CCC_IntegerNoSave, "r_wtr_sim_lid_rays", &ps_r_wtr_sim_lid_rays, 0, 20000);
    CMD4(CCC_IntegerNoSave, "r_wtr_sim_depth",     &ps_r_wtr_sim_depth,     0, 1);
    CMD4(CCC_FloatNoSave,   "r_wtr_sim_depth_ref", &ps_r_wtr_sim_depth_ref, 0.05f, 8.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_sim_bed",       &ps_r_wtr_sim_bed,       0.5f, 1.f);
    CMD4(CCC_Float,   "r_wtr_sim_slope",  &ps_r_wtr_sim_slope,  0.f, 4.f);
    CMD4(CCC_Float,   "r_wtr_sim_height", &ps_r_wtr_sim_height, 0.f, 4.f);
    CMD4(CCC_Float,   "r_wtr_step",       &ps_r_wtr_step,       0.f, 0.5f);
    CMD4(CCC_FloatNoSave, "r_wtr_bow",    &ps_r_wtr_bow,        0.f, 6.f);
    // NEGATIVE = debug view: the world, the grass and the foliage all paint
    // shoreWetness() straight to the screen (red = wet). The only link in this
    // chain that has never been observed is what the function RETURNS.
    // -1 = debug view of the COMBINED wetness, -2 = the STATIC level map alone.
    // -1 both halves, -2 the level map alone, -3 its raw contents, -4 the tile alone.
    CMD4(CCC_FloatNoSave, "r_wtr_wet",      &ps_r_wtr_wet,      -4.f, 2.f);
    CMD4(CCC_FloatNoSave, "r_wtr_wet_dry",  &ps_r_wtr_wet_dry,  0.f, 600.f);
    CMD4(CCC_FloatNoSave, "r_wtr_wet_lift", &ps_r_wtr_wet_lift, 0.f, 0.5f);
    CMD4(CCC_Integer, "r_wtr_grid",       &ps_r_wtr_grid,       0,   384);
    CMD4(CCC_Float,   "r_wtr_tess",       &ps_r_wtr_tess,       0.f, 64.f);
    CMD4(CCC_Float,   "r_wtr_tess_near",  &ps_r_wtr_tess_near,  1.f, 100.f);
    CMD4(CCC_Float,   "r_wtr_tess_far",   &ps_r_wtr_tess_far,   5.f, 400.f);
    CMD4(CCC_Float,   "r_wtr_disp",       &ps_r_wtr_disp,       0.f, 4.f);
    CMD4(CCC_Float,   "r_wtr_swash",     &ps_r_wtr_swash,     0.f, 3.f);   // metres of run-up
    CMD4(CCC_Float,   "r_wtr_swash_t",   &ps_r_wtr_swash_t,   1.f, 30.f);  // seconds between crests
    CMD4(CCC_Float,   "r_wtr_surf",      &ps_r_wtr_surf,      0.f, 3.f);   // whitewater
    CMD4(CCC_Float,   "r_wtr_surf_h",    &ps_r_wtr_surf_h,    0.f, 2.f);   // offshore wave height
    CMD4(CCC_Float,   "r_wtr_surf_len",  &ps_r_wtr_surf_len,  2.f, 60.f);  // metres between crests
    CMD4(CCC_Float,   "r_wtr_film",      &ps_r_wtr_film,      0.01f, 0.5f); // run-up sheet depth
    CMD4(CCC_Float,   "r_wtr_foam_land", &ps_r_wtr_foam_land, 0.f, 2.f);
    CMD4(CCC_Float,   "r_wtr_foam_life", &ps_r_wtr_foam_life, 0.2f, 20.f);
    CMD4(CCC_Float,   "r_wtr_fetch",     &ps_r_wtr_fetch,     0.f, 8.f);   // 0 = every pool is open sea
    // NoSave, like every other gate in this file: a stale 0 in user.ltx would put
    // the well's swell back months from now and look like a fresh regression.
    CMD4(CCC_IntegerNoSave, "r_wtr_fetch_local", &ps_r_wtr_fetch_local, 0, 1);
    CMD4(CCC_Float,   "r_wtr_shelter",   &ps_r_wtr_shelter,   0.f, 1.f);   // 0 = a roof no longer calms the water
    // ---- spectral waves (vk_water_fft) ------------------------------------
    // NOT SAVABLE, the whole family. These are the constants OF A MODEL that is
    // brand new and will be retuned in source as it is understood — the same
    // rule the ripple sim's scheme constants live under, and for the same
    // reason: a value pinned in user.ltx the first time the game quit cleanly
    // silently outranks every later fix, and this project has lost days to
    // exactly that (r_wtr_sim_damp, twice). They graduate to savable when the
    // defaults stop moving.
    CMD4(CCC_IntegerNoSave, "r_wtr_fft",        &ps_r_wtr_fft,        0, 1);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_size",   &ps_r_wtr_fft_size,   32.f, 2000.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_ratio",  &ps_r_wtr_fft_ratio,  0.05f, 0.6f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_wind",   &ps_r_wtr_fft_wind,   0.5f, 40.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_amp",    &ps_r_wtr_fft_amp,    0.f, 100.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_chop",   &ps_r_wtr_fft_chop,   0.f, 3.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_depth",  &ps_r_wtr_fft_depth,  0.5f, 500.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_repeat", &ps_r_wtr_fft_repeat, 10.f, 3600.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_small",  &ps_r_wtr_fft_small,  0.001f, 5.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_dir",    &ps_r_wtr_fft_dir,    0.f, 12.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_gain",   &ps_r_wtr_fft_gain,   0.f, 4.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_foam",   &ps_r_wtr_fft_foam,   0.f, 4.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_fetch",  &ps_r_wtr_fft_fetch,  1.f, 600.f);
    CMD4(CCC_FloatNoSave,   "r_wtr_fft_slope",  &ps_r_wtr_fft_slope,  0.f, 4.f);
    CMD4(CCC_Float,   "r_wtr_murk_still", &ps_r_wtr_murk_still, 1.f, 12.f); // murk multiplier for standing indoor water
    CMD4(CCC_Float,   "r_wtr_refract",   &ps_r_wtr_refract,   0.f, 0.3f);
    CMD4(CCC_Integer, "r_wtr_occl",      &ps_r_wtr_occl,      0, 1);        // skip the refraction snapshot while no water pixel is visible (occlusion query, one frame late)
    CMD4(CCC_Float,   "r_wtr_caustic",   &ps_r_wtr_caustic,   0.f, 4.f);
    CMD4(CCC_Float,   "r_wtr_caustic_p", &ps_r_wtr_caustic_p, 0.2f, 6.f);
    CMD1(CCC_WtrDrop, "r_wtr_drop");
    CMD1(CCC_WtrAudit, "r_wtr_audit");
    CMD4(CCC_Float, "r_spec_occ", &ps_r_spec_occ, 0.f, 1.f);             // bent-normal spec occlusion of wet reflections

    // ⭐The one antialiasing/upscaling row a player sees. See CCC_AAMode: it owns
    // r_dlss + r_dlss_quality, both of which stay registered below for A/B work.
    CMD3(CCC_AAMode, "r_aa_mode", &ps_r_pp_aa_mode, aa_mode_token);
    // ⚠The captions the player reads are NOT these token ids — the options screen
    // supplies them per row (`data-values` in main_menu.rml), the same way it does
    // for the numeric entries like r_dlss_preset. Token ids stay machine-readable
    // so a console line and a saved config keep meaning what they meant.

    CMD1(CCC_VideoMemoryStats, "video_memory_stats");

    // Screen Space Shaders
    CMD4(CCC_Vector4, "ssfx_wetsurfaces_1", &ps_ssfx_wetsurfaces_1_cfg, (Fvector4{0.01f, 0.01f, 0.01f, 0.01f}), (Fvector4{2.0f, 2.0f, 2.0f, 2.0f}));
    CMD4(CCC_Vector4, "ssfx_wetsurfaces_2", &ps_ssfx_wetsurfaces_2_cfg, (Fvector4{0.01f, 0.01f, 0.01f, 0.01f}), (Fvector4{2.0f, 2.0f, 2.0f, 2.0f}));

    CMD4(CCC_Vector3, "ssfx_gloss_minmax", &ps_ssfx_gloss_minmax, (Fvector3{}), (Fvector3{1.0, 1.0, 1.0}));

    CMD4(CCC_Vector4, "ssfx_lightsetup_1", &ps_ssfx_lightsetup_1, (Fvector4{}), (Fvector4{1.0f, 1.0f, 1.0f, 1.0f}));

    CMD4(CCC_Vector4, "ssfx_hud_drops_1", &ps_ssfx_hud_drops_1_cfg, (Fvector4{}), (Fvector4{100.f, 100.f, 100.f, 100.f}));
    CMD4(CCC_Vector4, "ssfx_hud_drops_2", &ps_ssfx_hud_drops_2_cfg, (Fvector4{}), (Fvector4{100.f, 100.f, 100.f, 100.f}));

    CMD4(CCC_Vector4, "ssfx_rain_1", &ps_ssfx_rain_1, (Fvector4{}), (Fvector4{10.f, 5.f, 5.f, 2.f}));

    CMD4(CCC_Vector4, "ssfx_grass_interactive", &ps_ssfx_grass_interactive, (Fvector4{}), (Fvector4{1.f, static_cast<float>(GRASS_SHADER_DATA_COUNT), 5000.f, 1.f}));
    CMD4(CCC_Vector4, "ssfx_int_grass_params_1", &ps_ssfx_int_grass_params_1, (Fvector4{}), (Fvector4{5.f, 5.f, 5.f, 60.f}));
    CMD4(CCC_Vector4, "ssfx_int_grass_params_2", &ps_ssfx_int_grass_params_2, (Fvector4{}), (Fvector4{5.f, 20.f, 1.f, 5.f}));

    CMD4(CCC_Vector3, "ssfx_color_grading", &ps_r2_img_cg, (Fvector3{}), (Fvector3{1.f, 1.f, 1.f}));

    CMD4(CCC_Float, "ssfx_exposure", &ps_r2_img_exposure, 0.5f, 1.5f);
    CMD4(CCC_Float, "ssfx_gamma", &ps_r2_img_gamma, 0.5f, 1.5f);
    CMD4(CCC_Float, "ssfx_saturation", &ps_r2_img_saturation, 0.5f, 1.5f);

    // Texture quality slider — reinstated for the Vulkan streamer. Skips the top N
    // mips of WORLD/MODEL diffuse at load (UI/detail/lmap/terrain stay full-res). Each
    // step ≈ −75% of that texture's VRAM. Live: takes effect on the next level load
    // (already-resident textures keep their mips until reloaded / streamed).
    CMD4(CCC_Integer, "texture_lod", &psTextureLOD, 0, 3);

    // Texture streamer knobs (see globals above).
    CMD4(CCC_Integer, "r_txstream",          &ps_r_txstream,          0, 1);
    CMD4(CCC_Integer, "r_txstream_budget",   &ps_r_txstream_budget,   0, 16384);
    CMD4(CCC_Integer, "r_txstream_headroom", &ps_r_txstream_headroom, 0, 4096);
    CMD4(CCC_Integer, "r_txstream_reserve",  &ps_r_txstream_reserve,  0, 8192);
    CMD4(CCC_Integer, "r_txstream_loadcap",  &ps_r_txstream_loadcap,  0, 4096);
    CMD4(CCC_Integer, "r_vram_small_images",  &ps_r_vram_small_images,  0, 1);   // small-image pools + their size probe
    CMD4(CCC_Integer, "r_tex_prefetch",      &ps_r_tex_prefetch,      0, 1);       // parallel level-texture prefetch
    CMD4(CCC_Integer, "r_tex_prefetch_mb",   &ps_r_tex_prefetch_mb,   64, 4096);   // bytes parked ahead of the visual walk
    CMD4(CCC_Integer, "r_upload_wc_copy",    &ps_r_upload_wc_copy,    0, 1);      // streaming stores into the write-combined staging ring
    CMD4(CCC_Integer, "r_upload_cached",     &ps_r_upload_cached,     0, 1);      // HOST_CACHED staging ring (needs a restart)
    CMD4(CCC_Integer, "r_upload_copy_threads", &ps_r_upload_copy_threads, 0, 15);  // helper threads for the big staging copies
    CMD4(CCC_Integer, "r_geom_lead",         &ps_r_geom_lead,         0, 1);      // stage geometry out of the prefetch's already-faulted view
    CMD4(CCC_Integer, "r_vis_warm",          &ps_r_vis_warm,          0, 1);      // prefault the visuals blob on workers before the walk
    CMD4(CCC_Integer, "r_tex_prefetch_lmaps_first", &ps_r_tex_prefetch_lmaps_first, 0, 1);  // lightmaps ahead of the diffuse bases
    CMD4(CCC_Integer, "r_prewarm_async",     &ps_r_prewarm_async,     0, 1);      // weather variants off the render thread
    CMD4(CCC_Integer, "r_thm_cache",         &ps_r_thm_cache,         0, 1);      // remember parsed .thm per base name
    CMD4(CCC_Integer, "r_clpage_map",        &ps_r_clpage_map,        0, 1);      // pinned cluster pages upload from a mapping
    CMD4(CCC_Integer, "r_tex_residency_threads", &ps_r_tex_residency_threads, 0, 16); // helpers that read a residency plan's .dds files
    CMD4(CCC_Integer, "r_tex_repack_threads", &ps_r_tex_repack_threads, 0, 15);   // helpers for the BC3->BC4 lightmap gather
    CMD4(CCC_Integer, "r_tex_materialize",    &ps_r_tex_materialize,    0, 2);    // 1 = workers build textures, 2 = only once the walk starts
    CMD4(CCC_Integer, "r_tex_mat_threads",    &ps_r_tex_mat_threads,    0, 16);   // concurrent texture builders (0 = all)
    CMD4(CCC_Integer, "r_tex_repack_scratch", &ps_r_tex_repack_scratch, 0, 1);    // reuse one gather destination per thread
    CMD1(CCC_VramDump,         "r_vram_dump");

    // Cluster-LOD page streaming (Stage B; read at level load).
    CMD4(CCC_Integer, "r_clpage",        &ps_r_clpage,        0, 1);
    CMD4(CCC_Integer, "r_clpage_budget", &ps_r_clpage_budget, 32, 4096);
    CMD4(CCC_Integer, "r_pool_compact",  &ps_r_pool_compact,  0, 1);
    CMD4(CCC_Integer, "r_compose",       &ps_r_compose,       0, 25);

    CMD4(CCC_Float, "particle_update_mod", &ps_particle_update_coeff, 0.04f, 10.f);
}

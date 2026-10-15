#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require   // the RT diffuse array is indexed per invocation
#extension GL_GOOGLE_include_directive : require
// ===========================================================================
//  xrRenderVulkan — WORLD RADIANCE CACHE: the shading pass. One thread per
//  ACTIVE CELL, not per ray. This is the dispatch the whole cache exists for.
// ===========================================================================
//  Every other GI pass in this renderer shades AT THE RAY: a probe ray hits a
//  wall and computes sun + shadow + lights + bounce right there, for that one
//  ray, and throws the answer away when the frame ends. A thousand rays landing
//  on the same square of concrete pay for it a thousand times.
//
//  Here the rays only left a NOTE (wrc_common's wrcQuery: "this patch was
//  wanted"), and this pass answers the notes — once per patch, and only for the
//  patches something actually asked about. idTech 8 shades ~20k cells a frame
//  this way and reuses each for several frames; that amortisation, not a cheaper
//  shading formula, is why their probes cost 0.08 ms.
//
//  ⭐ THE SHADING ITSELF IS NOT SPECIAL, AND MUST NOT BE. giShadeSurface() is
//  the very function ddgi_trace.comp calls for its own hits — shared through
//  gi_surface.glsl precisely so the cached answer and the traced one cannot
//  drift. A cell is just a hit that several rays agreed on.
//
//  ⚠ THE WORK IS BOUNDED, THE DEMAND IS NOT. Cells past r_wrc_shade_max simply
//  wait for the next frame: they keep answering "not shaded" and their callers
//  keep using the probe field. A cap that silently dropped work would be
//  invisible; the overflow counter in the control block is what makes it a
//  number instead of a shrug.
// ===========================================================================

#define RT_B_TLAS   0
#define RT_B_VERTS  1
#define RT_B_MATS   2
#define RT_B_ALBEDO 3
#define RT_TEX_SET  1     // vk_rt_world: the world diffuse array (per-texel albedo + cutout alpha)
#define RT_B_LIGHTS 9
#define RT_LIGHTS
#include "light_atten.glsl"   // dynLightAtten — the SAME falloff/cone the raster uses
#include "rt_shade.glsl"

#define DDGI_SET      0
#define DDGI_B_CONSTS 4
#define DDGI_B_IRR    7
#define DDGI_B_DEP    8
#define DDGI_SAMPLE
#include "ddgi_common.glsl"
#define DDGI_B_SKYSH   5
#define DDGI_B_SKYCUBE 6
#include "ddgi_sky.glsl"

#include "gi_surface.glsl"    // giShadeSurface — shared with the probe trace

#define WRC_SET      0
#define WRC_B_CONSTS 10
#define WRC_B_CELLS  11
#define WRC_B_LIST   12
#define WRC_B_CTRL   13
#include "wrc_common.glsl"

layout(local_size_x = 64) in;

void main()
{
    // GI-only reflectance scale (r_rt_albedo), shipped as an int x256 in the slot
    // that used to be reserved. Set before any trace: rtTraceClosest reads it.
    rtAlbedoBoost = float(ddgi.casc.w) * (1.0 / 256.0);
    const uint gid = gl_GlobalInvocationID.x;

    // The queue can ask for more than the cap allows; entries past it were never
    // written, so the count has to be clamped before anything indexes the list.
    const uint n = min(wrcCtrl.listCount, wrc.ctl.z);
    if (gid == 0u) wrcCtrl.shadedLast = n;
    if (gid >= n) return;

    const uint idx = wrcList[gid];
    if (idx > wrc.ctl.x) return;                 // never trust an index out of a buffer

    // Evicted between the request and now (the decay sweep runs just before this
    // dispatch). Shading it would fill a slot that another key already owns.
    if (wrcCell[idx].key == 0u) return;

    const uint surf = wrcCell[idx].surf;
    RTHit h;
    h.hit    = true;
    h.pos    = vec3(wrcCell[idx].px, wrcCell[idx].py, wrcCell[idx].pz);
    h.nrm    = wrcUnpackNrm(surf);
    h.albedo = wrcUnpackAlb(surf);

    // INDOOR REFLECTANCE COMPENSATION (r_gi_indoor) - see rtIndoorUp. One ray per
    // SHADED CELL, jittered by the cell index so neighbouring cells sample different
    // directions of the cap and the boost varies smoothly across a doorway instead
    // of stepping. Applies to every hit this cell rays make below.
    if (ddgi.indoor.x > 1.001) {
        const vec2 j = vec2(fract(float(idx) * 0.7548776662), fract(float(idx) * 0.5698402909));
        rtAlbedoBoost *= mix(1.0, ddgi.indoor.x, rtIndoorUp(h.pos + h.nrm * 0.05, ddgi.indoor.y, j));
    }
    h.dist   = 0.0;                              // unused by the shading; a hit is a hit

    // ---- SKY VISIBILITY: the term the per-ray path cannot afford ------------
    // rtShadeHitSky hands every hit the full SH9 sky, with no occlusion test at
    // all — so a wall inside a shack is lit like open field, and the only thing
    // holding interiors down has been the baked lightmap re-applied on top
    // (r_ddgi_light_bake). Turn that off and rooms go white; it was never a
    // multiplier, it was the missing occlusion.
    //
    // ⭐⭐ THIS IS WHERE THE CACHE FIRST PAYS FOR ITSELF. A hemisphere ray per HIT
    // is out of the question — the gather alone makes ~800k of them a frame. A
    // hemisphere ray per CELL costs ~18k, because a cell is shaded roughly forty
    // times less often than it is read. The amortisation buys a term, not just
    // time, and that is a better argument for a radiance cache than the time was.
    //
    // ⚠ MEDIUM RANGE, NOT INFINITE. Past the bubble a miss means "nothing built",
    // so a long ray would report open sky through a building that exists. What
    // this measures is hemisphere occlusion over r_wrc_skyvis_len — the same
    // quantity the lightmap's hemi channel carries, which is exactly the point.
    // ⭐⭐ THE RAY CARRIES A COLOUR BACK, NOT A VOTE. Counting escapes and scaling
    // the hemisphere average by the fraction is what made a room with one window
    // come out black: the fraction is tiny AND the direction is discarded, so the
    // bright square of sky in the opening is averaged into the same dim number as
    // a cell facing a wall. Accumulating ddgiSkyRadiance along the directions that
    // actually escape costs one texture tap per miss — the ray is traced either
    // way — and the result IS the integral, aperture and all. See gi_surface.glsl.
    vec3 skyIrr = ddgiSkyIrradiance(h.nrm);
    // How much of the hemisphere integral actually carries bounced light. Lives out
    // here because the field-bounce term below has to be faded by exactly this much.
    float bounceApplied = 0.0;
    const int svRays = int(wrc.ctl2.w);
    // r_wrc_field_clamp state (sky_field mode only). fieldK < 0 = the field term
    // stays inside giShadeSurface exactly as before; >= 0 = it was sampled out
    // here (fieldIrr) and the ceiling is already folded into fieldK.
    float fieldK   = -1.0;
    vec3  fieldIrr = vec3(0.0);
    // Diagnostic mode (r_wrc_field_clamp < 0): see the note inside the branch.
    const bool diag = wrc.params2.w < 0.0;
    vec3 diagOut = vec3(0.0);
    if (wrc.params2.z > 0.5) {
        // ---- r_wrc_sky_field: THE CELL ROLLS NO RAYS AT ALL --------------------
        // idTech 8's cell shading, verbatim: direct lights + previous frame's
        // irradiance volumes, nothing else. The aperture integral lives in ONE
        // place — the probes, whose 64 rays x 0.95 hysteresis make ~2500 effective
        // samples against ~38 for the loop below (8 rays x 0.35 EMA), and whose
        // answer is read TRILINEARLY — so two neighbouring cells cannot disagree
        // the way two independent 8-ray estimates do. The world-anchored
        // bright/dark cell pattern indoors IS that disagreement; this branch
        // removes its source instead of averaging it harder.
        //   skyIrr = 0: the field already carries the sky seen through the
        //   aperture (a probe ray that escapes samples ddgiSkyRadiance), so any
        //   sky term added here would be counted twice.
        //   bounceApplied stays 0: the field read in giShadeSurface
        //   (ddgi.params.w) stops being an optional feedback and becomes THE
        //   ambient. r_ddgi_bounce keeps its meaning — 0 still cuts the loop.
        skyIrr = vec3(0.0);

        // ---- MODE 2 (r_wrc_sky_field 2): CACHE-TO-CACHE BOUNCE ----------------
        // The clamp below asks "may the field be this bright HERE?" — mode 2
        // stops asking the field anything. A covered cell's ambient IS its own
        // hemisphere: the sky its rays see through the openings plus what the
        // neighbour cells they land on already radiate (the same wrcQuery chain
        // every gather ray walks). Dark rooms bounce off dark rooms, and the
        // coarse lattice — whose 4-8 m probes stand in the open, "the lights are
        // on in that house" — never enters the chain at all. Open-sky cells (the
        // cover gate fails) keep the field read: outdoors the field is right,
        // trilinear, and noise-free, which is everything mode 1 was chosen for.
        const bool cacheBounce = wrc.params2.z > 1.5;

        // ---- r_wrc_field_clamp: AN ENERGY CEILING ON THAT FIELD READ ----------
        // The field is trilinear and smooth — and, past the fine cascades' reach
        // (~15 m), WRONG about interiors: a distant room reads the 4-8 m lattice
        // whose probes stand in the open, so "the lights are on in that house".
        // Measured 27-08 on one and the same interior wall: 0.081 from inside
        // (cascade 0) against 0.889 from 25 m away (cascade 2 -> 3). This does
        // not fix the lattice; it bounds what the cell may TAKE from it by what
        // its own hemisphere says actually gets in:
        //
        //   budget = clampK x lum( sky along the rays that escape
        //                        + already-shaded neighbour cells on the rays
        //                          that hit, i.e. light arriving by bounce )
        //
        // ⚠ NO FIELD FALLBACK IN THE BUDGET (the `lat` term of the MC branch
        // below): the field is the very quantity being clamped, and letting it
        // into the budget would launder the brightness the ceiling exists to
        // bound. An unshaded neighbour contributes zero — conservative for the
        // first moments while the cache warms, then honest: brightness can only
        // enter a room traceably, from the sky through its openings or from a
        // neighbour patch that already accounted for its own.
        //
        // ⭐ A CEILING, NOT A MULTIPLIER — and that distinction is what keeps the
        // skyvis dirt buried (the world-anchored bright/dark cell pattern that
        // made sky_field the default, see above). Outdoors and near windows the
        // budget exceeds the spend and the 8-ray noise touches NOTHING; the
        // ceiling only bites where the field is far above the measured
        // injection, and there the result is dark-on-dark. Steady state in an
        // enclosure is bounded by injected/(1-albedo); with the boosted albedo
        // near its 0.9 cap that is ~2x injection — the default clampK.
        // ⚠⚠ GATED, AND OPAQUE-ONLY, because the naive hemisphere here was
        // MEASURED AT 31 MS of a 60 ms frame (the very reason sky_field became
        // the default): 8 closest rays per cell drowning in 1.27M alpha-tested
        // leaf cards, every candidate leaving traversal for a texture fetch.
        // Two masks keep that from coming back:
        //  (1) only a cell with SOLID COVER overhead runs the hemisphere at all
        //      — one rtIndoorUp-class ray (mask 0x01 + ForceOpaque, the cheapest
        //      ray in the frame). An open-sky cell's budget would dwarf any
        //      field read anyway, so skipping it changes nothing but the bill.
        //  (2) the hemisphere itself traces mask 0x01: a leaf card is not a wall
        //      (the relocation pass draws the same line), so the forest never
        //      pays and the clamp simply never fires under a canopy.
        // A NEGATIVE k is the DIAGNOSTIC MODE: the ceiling is computed with |k|,
        // but the cell stores its own budget breakdown instead of radiance —
        // R = the sky part of the injection, G = the shaded-neighbour part,
        // B = 1 when the cover gate passed (0 = the hemisphere never ran).
        // [VK GIprobe]'s "WRC cell" line then prints one cell's verdict under the
        // crosshair. The frame looks wrong while it is on — every reader gets the
        // breakdown as colour — which is exactly why it is a mode and not a stat.
        const float clampK = abs(wrc.params2.w);
        // Mode 2 needs the hemisphere regardless of the clamp: it is not a meter
        // there, it is the ambient itself — and it must run even with the field
        // bounce at zero (ddgi.params.w), because the field is not what feeds it.
        if (svRays > 0 && (cacheBounce || (clampK > 0.0 && ddgi.params.w > 0.0))) {
            atomicAdd(wrcCtrl.clampSeen, 1u);
            const vec3  ro = h.pos + h.nrm * 0.02;
            const float len = max(wrc.params2.x, 0.5);
            const vec2  jc = vec2(fract(float(idx) * 0.3183098862), fract(float(idx) * 0.8660254038));
            if (rtIndoorUp(ro, max(len, 4.0), jc) > 0.5) {
                atomicAdd(wrcCtrl.clampCovered, 1u);
                uint rnd = wrcMix(idx ^ wrcMix(wrc.ctl.y));
                const vec3 up = abs(h.nrm.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
                const vec3 T  = normalize(cross(up, h.nrm));
                const vec3 B  = cross(h.nrm, T);
                const float bounceW = clamp(wrc.params2.y, 0.0, 1.0);
                vec3 injSky = vec3(0.0);
                vec3 injNb  = vec3(0.0);
                for (int i = 0; i < svRays; ++i) {
                    const float u1 = float(rnd & 0xFFFFu) * (1.0 / 65536.0);
                    const float u2 = float(rnd >> 16)     * (1.0 / 65536.0);
                    rnd = wrcMix(rnd + 0x9E3779B9u);
                    const float rr  = sqrt(u1);
                    const float phi = 6.28318530718 * u2;
                    const vec3  d   = normalize(T * (rr * cos(phi)) + B * (rr * sin(phi))
                                              + h.nrm * sqrt(max(0.0, 1.0 - u1)));
                    const RTHit hs = rtTraceClosestMasked(ro, d, 0.01, len, 0x01u);
                    if (!hs.hit) { injSky += ddgiSkyRadiance(d); continue; }
                    // The query also REGISTERS the neighbour cell, and that is
                    // what makes the budget converge room-by-room inward from
                    // the openings — one re-shade interval per hop.
                    vec3 cached; uint ckey;
                    const float cw = wrcQuery(hs.pos, hs.nrm, hs.albedo, cached, ckey);
                    injNb += cached * (cw * bounceW);
                }
                injSky *= 1.0 / float(svRays);
                injNb  *= 1.0 / float(svRays);

                const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
                if (cacheBounce) {
                    // The measured injection IS the ambient; the field never
                    // enters. No clampK on anything: k modelled the multi-bounce
                    // gain of an enclosure for a BUDGET, but here the further
                    // bounces arise physically — neighbour feeds neighbour, one
                    // re-shade interval per hop — and the 0.9 albedo cap
                    // (rt_shade.glsl) holds the loop gain below unity. The
                    // compounding measured 22-08 was the FIELD added on top of an
                    // integrated hemisphere; the field is exactly what is gone.
                    // An unshaded neighbour still contributes ZERO — the clamp's
                    // no-field-fallback rule, for the same laundering reason —
                    // so a deep room starts dark and converges inward from its
                    // openings instead of starting street-bright and never
                    // coming down. r_wrc_skyvis_bounce (already inside injNb)
                    // stays the damper on the cell<->cell loop.
                    skyIrr = injSky + injNb;
                    fieldK = 0.0;   // fieldIrr stays 0: the field term below is a no-op
                    // Same audit counters, same reading across modes: budget =
                    // the measured ambient, spend = what the field WOULD have
                    // delivered, bitten = the field would have been brighter.
                    const float amb = dot(skyIrr, lw);
                    float spend = 0.0;
                    if (ddgi.params.w > 0.0)
                        spend = dot(ddgiSampleIrradiance(h.pos, h.nrm, h.nrm), lw) * ddgi.params.w;
                    if (spend > amb) atomicAdd(wrcCtrl.clampBitten, 1u);
                    atomicAdd(wrcCtrl.clampBudgetM, uint(min(amb,   16.0) * 1000.0));
                    atomicAdd(wrcCtrl.clampSpendM,  uint(min(spend, 16.0) * 1000.0));
                } else {
                    fieldIrr = ddgiSampleIrradiance(h.pos, h.nrm, h.nrm);
                    const float spend  = dot(fieldIrr, lw) * ddgi.params.w;
                    // ⚠ k SCALES THE SKY TERM ONLY. k models the multi-bounce gain an
                    // enclosure applies to PRIMARY injection (1/(1-albedo)); a shaded
                    // neighbour's radiance is already the equilibrium value with that
                    // gain inside it. Multiplying the neighbours by k too was measured
                    // (28-08, diag mode on the test wall: sky 0.0-0.14, neighbours
                    // 0.0-0.24, gate 1.0) to hold a SELF-SUSTAINING bright fixpoint:
                    // interior cells at 0.6 fed each other budgets of ~0.6x2 >= the
                    // field spend, so nothing ever got cut. With the neighbours at
                    // weight 1 the loop gain drops below unity and the bright
                    // equilibrium collapses; the dark (honest) one is what remains.
                    const float budget = dot(injSky, lw) * clampK + dot(injNb, lw);
                    fieldK = ddgi.params.w;
                    if (spend > budget) {
                        fieldK *= budget / max(spend, 1e-6);
                        atomicAdd(wrcCtrl.clampBitten, 1u);
                    }
                    atomicAdd(wrcCtrl.clampBudgetM, uint(min(budget, 16.0) * 1000.0));
                    atomicAdd(wrcCtrl.clampSpendM,  uint(min(spend,  16.0) * 1000.0));
                }
                diagOut = vec3(dot(injSky, lw), dot(injNb, lw), 1.0);
            }
        }
    } else if (svRays > 0) {
        // Cheap decorrelation: the cell index mixed with the frame. Per-cell rather
        // than per-thread so a cell varies its directions between RE-SHADES, which
        // is what lets the EMA below converge instead of re-averaging one fixed set.
        uint rnd = wrcMix(idx ^ wrcMix(wrc.ctl.y));
        const vec3 up = abs(h.nrm.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        const vec3 T  = normalize(cross(up, h.nrm));
        const vec3 B  = cross(h.nrm, T);
        const vec3 o  = h.pos + h.nrm * 0.02;
        const float len = max(wrc.params2.x, 0.5);
        // r_wrc_skyvis_bounce: what a ray that HITS is worth. Zero is the binary
        // occluder this started as; above zero the ray asks the cache what that
        // surface is radiating, which is the ONLY way the sunlit yard five metres
        // outside the window stops counting as a wall. See the note in
        // vk_console_min.cpp over ps_r_wrc_skyvis_bounce.
        const float bounceW = clamp(wrc.params2.y, 0.0, 1.0);
        bounceApplied = bounceW;
        vec3 sum = vec3(0.0);
        for (int i = 0; i < svRays; ++i) {
            const float u1 = float(rnd & 0xFFFFu) * (1.0 / 65536.0);
            const float u2 = float(rnd >> 16)     * (1.0 / 65536.0);
            rnd = wrcMix(rnd + 0x9E3779B9u);
            // Cosine-weighted, because the term it scales is a cosine-weighted
            // integral: a uniform hemisphere would over-count the horizon.
            const float rr  = sqrt(u1);
            const float phi = 6.28318530718 * u2;
            const vec3  d   = normalize(T * (rr * cos(phi)) + B * (rr * sin(phi))
                                      + h.nrm * sqrt(max(0.0, 1.0 - u1)));
            if (bounceW <= 0.0) {
                // Any-hit: cheaper, and all the binary term ever needed.
                if (!rtOccluded(o, d, 0.01, len)) sum += ddgiSkyRadiance(d);
                continue;
            }
            // Closest-hit, because a hit now has to be IDENTIFIED, not just detected.
            const RTHit hs = rtTraceClosest(o, d, 0.01, len);
            if (!hs.hit) { sum += ddgiSkyRadiance(d); continue; }

            // ⚠ THE SAME CHAIN THE GATHER USES, AND DELIBERATELY SO. `lattice` is the
            // probe field's answer for that patch; the cache overrides it where it has
            // one. Spelling it differently here would make the second bounce disagree
            // with the first about how bright the world is, and the disagreement would
            // read as an exposure shift the moment this knob moved off zero.
            vec3 cached; uint ckey;
            const float cw  = wrcQuery(hs.pos, hs.nrm, hs.albedo, cached, ckey);
            const vec3  lat = hs.albedo * ddgiSampleIrradiance(hs.pos, hs.nrm, -d);
            sum += mix(lat, cached, cw) * bounceW;
        }
        // Cosine-weighted sampling => (1/N)*sum(L) estimates E/pi, which is exactly
        // what shIrradianceC (and therefore ddgiSkyIrradiance) returns. So a UNIFORM
        // sky with open/N directions clear reproduces the old `irradiance * skyVis`
        // product; everything this changes is the case where the sky is not uniform
        // across the visible directions — which is every aperture.
        skyIrr = sum * (1.0 / float(svRays));
    }

    // The hemisphere above already carries the bounced light for the fraction of it
    // that was integrated, so the field-bounce term is faded out by exactly that much
    // — replace, do not add. See the fieldBounce note in gi_surface.glsl.
    // Diagnostic mode: the breakdown replaces the radiance, raw — no EMA, no
    // firefly clamp — so what the dump prints is THIS shade's measurement, not a
    // history of several. See the diag note above.
    if (diag && wrc.params2.z > 0.5) {
        wrcCell[idx].rad    = wrcPackRad(diagOut);
        wrcCell[idx].shaded = wrc.ctl.y + 1u;
        return;
    }

    vec3 r;
    if (fieldK >= 0.0) {
        // The clamp path: the SAME term giShadeSurface would add (same lookup,
        // same V = h.nrm), sampled out here so the ceiling can be applied to it.
        // With r_wrc_field_clamp at 0 fieldK stays -1 and the else below is the
        // old arithmetic bit for bit.
        r = giShadeSurface(h, h.nrm, skyIrr, 0.0);
        r += h.albedo * fieldIrr * fieldK;
    } else {
        r = giShadeSurface(h, h.nrm, skyIrr, ddgi.params.w * (1.0 - bounceApplied));
    }

    // Mode 2 reads cells FROM cells, so one poisoned answer would be re-inhaled
    // by every neighbour's bounce ray and the EMA would never let go — the
    // latched-loop class (SSIL/NaN). RGB9E5 cannot HOLD a NaN, but PACKING one
    // produces finite garbage at any brightness, so the guard has to run before
    // the pack, not after. Dropping the store just re-queues the cell:
    // `shaded` keeps saying "no answer yet" and readers keep falling back.
    if (any(isnan(r)) || any(isinf(r))) return;

    // The same clamp the probe rays apply, for the same reason and then some: a
    // firefly in a probe lives for the ~30 frames of its hysteresis, but a firefly
    // in a CELL is read by every ray that lands there until the cell is re-shaded,
    // so it would be a bright dot fixed to a place in the world.
    const float m = max(max(r.r, r.g), r.b);
    if (m > ddgi.params.z) r *= ddgi.params.z / m;

    // Blend against what the cell already held. A cell is re-shaded every few
    // frames, and its two neighbours in time can differ by a shadow edge sweeping
    // across it — the EMA is what turns that into a fade instead of a flicker.
    // A cell shaded for the FIRST time takes the new value whole: blending it
    // against the zero it was cleared to would print a dark patch that takes
    // several re-shades to disappear.
    //
    // ⭐ ASYMMETRIC (r_wrc_ema_up, params3.x): brightening moves at a fraction of
    // the darkening rate. 📏Measured 28-08 (diag mode, the canonical wall's
    // level-2 cell from 34 m): the 8-ray hemisphere catches the aperture with
    // 0..4 rays, so the sky term arrives QUANTISED in steps of sky/8
    // (0.00/0.14/0.28/0.42/0.56) and a symmetric EMA rides that shot noise up as
    // readily as down — the cell breathed 0.10-0.27 where the fine-level answer
    // is 0.08, and the receiver's field ceiling (r_gather_field_clamp) inherited
    // the inflation: "the windows still glow from 50 m". Slowing only the upward
    // moves parks the cell near the LOW quantile — the honest side for an
    // enclosure, where overshoot is exactly what prints "the lights are on".
    // Steady inputs (open sky, direct sun, the trilinear field read) have no
    // such quantisation and only feel a slower ramp on GENUINE brightening
    // (a lamp lit, a door opened) — a few re-shade intervals instead of one.
    // 1 = symmetric, the old arithmetic bit-exact.
    const uint sh = wrcCell[idx].shaded;
    if (sh != 0u) {
        const vec3  old = wrcUnpackRad(wrcCell[idx].rad);
        float w = clamp(wrc.params.w, 0.02, 1.0);
        const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
        // ⚠⚠ COARSE LEVELS ONLY (06-09). Measured in the canonical Cordon room, one
        // run, same wall: with the asymmetry on every level the NEAR room sat at
        // cell 0.008-0.014 / field 0.02 / wall 0.003; r_wrc_ema_up 1 lifted it to
        // cell 0.05-0.08 / field 0.06 / wall 0.016 - x3-5 on the whole interior.
        // The low-quantile parking that cures "the lights are on from 50 m" is a
        // x4 darkening bias up close, where the 8-ray aperture lottery is the
        // SAME shot noise but the receiver reads the cell directly. Level 0-1
        // (inside ~25 m) stays symmetric; the distant-room cure keeps level >= 2.
        if (dot(r, lw) > dot(old, lw) && wrcLevel(h.pos) >= 2) w *= clamp(wrc.params3.x, 0.01, 1.0);
        r = mix(old, r, w);
    }

    wrcCell[idx].rad = wrcPackRad(r);
    // `shaded != 0` IS the cell's validity flag — the one thing every reader
    // tests before trusting the radiance above. No reader runs inside this
    // dispatch, so the two stores need no ordering between them; the pass's
    // barrier publishes both together, which is what makes the flag honest.
    wrcCell[idx].shaded = wrc.ctl.y + 1u;
}

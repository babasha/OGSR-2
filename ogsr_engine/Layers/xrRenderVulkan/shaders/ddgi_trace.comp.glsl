#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require   // the RT diffuse array is indexed per invocation
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — DDGI probe trace: one ray per thread, radiance + distance out.
//
// This is where the whole arc pays off. The ray leaves a probe in a direction
// from the rotated Fibonacci set, and rt_shade.glsl turns whatever it hits into
// RADIANCE — geometric normal from the soup, albedo from the collision material,
// sun visibility from a second ray. That function is the same one the debug
// preview was checked with, which is the entire reason step 0 came first.
//
// A MISS is not a failure: it means the ray left the world and saw sky, which is
// exactly the dominant light source outdoors. Its distance is stored at the
// ceiling so the visibility statistics read it as "nothing in the way".
//
// ⭐THE SKY IS THE SAME SKY THE SURFACES USE. A miss returns the SH9 irradiance
// (shaders/sky_sh.glsl, binding 6) — the very function skyAmbient() calls in
// every forward receiver, scaled by the same L.sky_params.y. That is not a
// refinement, it is what makes step 2 possible at all: the probe field REPLACES
// the analytic sky fill in the lighting, so if the two disagreed about the sky's
// magnitude, switching GI on would read as an exposure change and no A/B could
// say whether the GI itself was any good. It also buys a directional sky for
// free — a wall facing a sunset now bounces sunset-coloured light, where the old
// single hemi_color made every probe see the same grey ball.
//
// ⚠A probe that sits INSIDE geometry traces nothing but immediate hits and goes
// dark. With r_ddgi_reloc on, ddgi_relocate.comp reads the rays THIS pass writes
// and slides such a probe out of the solid; the position below already carries
// that offset. With it off the probe stays buried, and the Chebyshev test in
// ddgiSampleIrradiance is all that keeps it from poisoning its neighbours —
// which also means the surfaces around it get no answer at all.

#define RT_B_TLAS   0
#define RT_B_VERTS  1
#define RT_B_MATS   2
#define RT_B_ALBEDO 3
#define RT_TEX_SET  1     // vk_rt_world: the world diffuse array, when the BVH is render geometry
#define RT_B_LIGHTS 10
#define RT_LIGHTS
#include "light_atten.glsl"   // dynLightAtten — the SAME falloff/cone the raster uses
#include "rt_shade.glsl"

#define DDGI_SET      0
#define DDGI_B_CONSTS 4
#define DDGI_B_IRR    8
#define DDGI_B_DEP    9
#define DDGI_SAMPLE
#include "ddgi_common.glsl"

layout(std430, set = 0, binding = 5) writeonly buffer DDGIRays { vec4 rayOut[]; };
// Per-ray hints for probe relocation (binding 15) — see the write at the bottom.
layout(std430, set = 0, binding = 15) writeonly buffer DDGIRelocHints { uint hintOut[]; };

// The sky, exactly as the final gather sees it — one include, so the two cannot
// drift about what "the sky" is worth (shaders/ddgi_sky.glsl).
#define DDGI_B_SKYSH   6
#define DDGI_B_SKYCUBE 7
#include "ddgi_sky.glsl"

// The radiance leaving a hit — sun through a shadow ray, the SH9 sky along the
// normal, the RT world's dynamic lights, and the previous frame's field for the
// further bounces. Shared with wrc_shade.comp, which computes exactly this for a
// CACHE CELL: the cache replaces this call for most rays, so the two have to be
// the same number or turning the cache on would move the frame's exposure.
#include "gi_surface.glsl"

// ⭐THE WORLD RADIANCE CACHE, AND THE REASON IT EXISTS AT ALL. idTech 8's probe
// ray does not shade what it hits — it packs the hit into 128 bits, and the
// lighting is computed later, once per CELL, and reused by every ray that landed
// there over several frames. That is the whole difference between their 0.08 ms
// probe update and our 0.2-0.8: not a cheaper formula, an amortised one.
#define WRC_SET      0
#define WRC_B_CONSTS 11
#define WRC_B_CELLS  12
#define WRC_B_LIST   13
#define WRC_B_CTRL   14
#include "wrc_common.glsl"

layout(local_size_x = 64) in;

void main()
{
    // GI-only reflectance scale (r_rt_albedo), shipped as an int x256 in the slot
    // that used to be reserved. Set before any trace: rtTraceClosest reads it.
    rtAlbedoBoost = float(ddgi.casc.w) * (1.0 / 256.0);
    const int rays  = ddgi.dims.w;
    const int total = ddgi.casc.y * rays;      // ONE cascade's worth of work per frame
    const int gid   = int(gl_GlobalInvocationID.x);
    if (gid >= total) return;

    const int casc  = ddgi.layout_.w;          // the cascade this frame updates
    const int probe = gid / rays;
    const int ray   = gid - probe * rays;

    const ivec3 slot = ddgiSlotCoord(probe);
    const ivec3 absC = ddgiSlotToAbs(slot, ddgi.base[casc].xyz);
    const int   gp   = ddgiGlobalProbe(casc, probe);
    // ⚠ A SLOT THAT SCROLLED IN THIS UPDATE TRACES FROM THE BARE LATTICE. Its
    // stored offset belongs to the PREVIOUS occupant of this slot and describes
    // open space at a completely different world position; relocation zeroes it
    // later this frame, but the rays leave now. Same reasoning as the zero
    // hysteresis the blend passes give such a slot.
    const bool  isNew = ddgiSlotIsNew(absC, casc);
    const vec4  st    = isNew ? vec4(0.0) : ddgiProbeState(gp);
    const vec3  P     = ddgiLatticePos(absC, casc) + st.xyz;
    const vec3  dir   = ddgiProbeRayDir(ray);

    // ---- FIXED RAYS: geometry only ------------------------------------------
    // Rays [0, F) exist for relocation and classification, which read nothing but
    // the distance and the solid/backface hint, and the blends never integrate
    // them. So they skip the whole shading path — no shadow ray, no light loop, no
    // field lookup, no cache query — and cost one closest-hit query each.
    const bool fixedRay = ray < ddgiFixedRays();

    // ---- CLASSIFICATION: an inactive probe skips its rotated rays -----------
    // Nothing solid or leafy within its cell box (the fixed rays said so last
    // update), so no surface interpolates it; it keeps its history and is
    // re-measured once per r_ddgi_classify_period updates. The fixed rays still
    // fly every update: they are how the probe would find out it is not idle.
    if (!fixedRay && !isNew && ddgiSkipInactive(probe, int(round(st.w)))) return;

    // INDOOR REFLECTANCE COMPENSATION (r_gi_indoor). One opaque ray up, per PROBE
    // RAY, each with its own fixed sample of the cap - 64 binary answers integrate
    // into this probe true blocked fraction inside the sum the probe was computing
    // anyway. Deterministic in the ray index rather than jittered per frame: the
    // quantity is pure geometry, and a temporally noisy boost would be visible as
    // the probe hysteresis chasing it.
    // Fixed rays shade nothing, so they skip this ray too; the jitter index counts
    // rotated rays only, which with F = 0 is the old sequence exactly.
    if (!fixedRay && ddgi.indoor.x > 1.001) {
        const int  rr = ray - ddgiFixedRays();
        const vec2 j = vec2(float(rr) / float(max(ddgiBlendRays(), 1)), fract(float(rr) * 0.6180339887));
        rtAlbedoBoost *= mix(1.0, ddgi.indoor.x, rtIndoorUp(P, ddgi.indoor.y, j));
    }

    // ⚠ NOT scaled per cascade: the ray range is bounded by the BLAS bubble, not by
    // the cascade. Past the bubble a miss means "nothing built", not "nothing there",
    // so reaching further would only let the outer cascade report open sky where a
    // building stands. Probes out there degrade to "unoccluded sky", which the baked
    // lightmap multiplier then brings back to roughly the analytic answer.
    const float tmax = ddgi.sunDir.w;
    RTHit h = rtTraceClosest(P, dir, 0.0, tmax);

    vec3 radiance;
    float dist;
    if (fixedRay) {
        radiance = vec3(0.0);   // never blended, and the ray view skips it
        dist     = h.hit ? h.dist : ddgiProbeDist(casc);
    } else if (h.hit) {
        // Sun through a shadow ray + the SH9 sky along the hit's normal + the RT
        // world's dynamic lights + the previous frame's field for the further
        // bounces. All four live in gi_surface.glsl now, because the world radiance
        // cache has to compute the identical quantity for a cell.
        //
        // ⚠ THE BOUNCE TERM IS A WAR against the blend passes, which write these
        // atlases later in this same command buffer. Covered by the ComputeBarrier
        // between trace and blend: a write-after-read needs only the execution
        // dependency, which two COMPUTE stages either side of it already have.
        //
        // ...unless the cache already knows this patch. Asking costs one hash and
        // one probe chain walk; the answer replaces a shadow ray, a loop over the
        // dynamic lights (each with a shadow ray of its own) and an eight-probe
        // field lookup. The query also REGISTERS the cell, so a miss now is a hit
        // in two frames' time — which is why this is worth doing even on the frame
        // it does nothing.
        vec3 cached; uint ckey;
        const float cw = wrcQuery(h.pos, h.nrm, h.albedo, cached, ckey);
        if (cw >= 0.999) {
            radiance = cached;                       // nothing is shaded on this ray
        } else {
            // ⚠ AN INTERMEDIATE WEIGHT PAYS FOR BOTH. r_wrc_light between 0 and 1
            // is an A/B lever, not a setting: it blends the cache against a full
            // shade and therefore costs strictly more than either. The shipping
            // path is 0 (cache off) or 1 (cache answers).
            // Unoccluded sky, i.e. the term as it stood before the cache measured
            // it: a probe ray cannot afford a hemisphere of its own on top of the
            // one it already is. The WRC branch above is where the real integral
            // comes from, and the cell answer replaces this one outright.
            //
            // ⚠⚠ r_ddgi_hit_sky — WHERE THE HIT'S SKY COMES FROM. The field this
            // hit reads for its bounce already HOLDS the sky: the probes' own misses
            // brought it in, occluded by whatever stands between them and the sky.
            // Adding the unoccluded SH9 sky on top (the old path) counts it twice
            // in the open and not at all correctly indoors — a wall inside a shack
            // was lit as if it stood in a field, which is what the baked-lightmap
            // multiplier in the receivers was holding down. RTXGI's hit is direct
            // light + albedo x field and nothing else; the sky enters only through
            // misses. Beyond the volume the field is black by design, so the
            // analytic sky takes back exactly the share the field cannot answer —
            // the same fade every receiver uses — and with r_ddgi_bounce 0 (no
            // field) the hit keeps the analytic sky whole, as before.
            vec3  skyIrr = ddgiSkyIrradiance(h.nrm);
            float fieldW = ddgi.params.w;
            if (ddgi.fix.x != 0) {
                fieldW = ddgi.params.w * ddgiVolumeFade(h.pos);
                skyIrr *= 1.0 - fieldW;
            }
            radiance = giShadeSurface(h, -dir, skyIrr, fieldW);
            if (cw > 0.0) radiance = mix(radiance, cached, cw);
        }

        dist = h.dist;
    } else {
        radiance = ddgiSkyRadiance(dir);
        // ⚠ PER CASCADE, not the raw r_ddgi_probe_dist: a coarse cascade whose
        // ceiling is shorter than its own cell diagonal reports "occluded" for the
        // far half of every cell it owns. See ddgiProbeDist in ddgi_common.glsl.
        dist     = ddgiProbeDist(casc);   // depth ceiling: "nothing in the way"
    }

    // Clamp before the average, not after: one fireball ray out of 32 would
    // otherwise survive the mean and then live for ~30 frames in the hysteresis.
    const float clampTo = ddgi.params.z;
    const float m = max(max(radiance.r, radiance.g), radiance.b);
    if (m > clampTo) radiance *= clampTo / m;

    rayOut[gid] = vec4(radiance * ddgi.skyColor.w, min(dist, ddgiProbeDist(casc)));

    // ---- what probe relocation needs, and why it is its own channel ----------
    // Majercik's convention packs the backface bit into the SIGN of the distance.
    // That is not enough here, because this world has a third kind of hit: a LEAF
    // CARD. The cutout structure is 99% of the triangles in the bubble, its cards
    // are two-sided (so "backface" is a coin flip on them — measured at 52% in the
    // first run, in a forest) and, more importantly, a probe INSIDE a crown is not
    // buried: it is in a shaded place that needs light like any other. Relocating
    // it would be the feature doing harm.
    //
    // So the hint is two bits in a channel of its own, and the distance stays a
    // plain distance — one meaning per value, and the distance blend is untouched.
    //   bit 0 = the ray stopped on SOLID geometry (the opaque structure)
    //   bit 1 = ...and hit it from behind, i.e. the ray began inside it
    // A miss and a leaf are both 0: neither is evidence of being buried.
    // ⭐The opaque/cutout split is a triangle-index comparison because both
    // structures were built over ONE soup, opaque first (vk_rt_scene.cpp).
    // reloc.w is 0 on the collision path, where there are no cutouts at all.
    const uint opaqueEnd = uint(max(ddgi.reloc.w, 0));
    const bool solid = h.hit && (ddgi.reloc.w <= 0 || h.prim < opaqueEnd);
    hintOut[gid] = solid ? (1u | (h.back ? 2u : 0u)) : 0u;
}

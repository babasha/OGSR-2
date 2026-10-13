#version 460
#extension GL_GOOGLE_include_directive : require
// ===========================================================================
//  xrRenderVulkan — DDGI PROBE RELOCATION: one thread per probe of the cascade
//  this frame updated, sliding that probe inside its own cell towards open air.
// ===========================================================================
//
//  WHY THIS EXISTS
//  ---------------
//  A lattice knows nothing about geometry. At 3 m spacing a 4 m room contains no
//  probe that is not inside masonry, so every one of its eight neighbours fails
//  the Chebyshev test and the interior lookup returns zero — the measured cause
//  of "GI does nothing indoors" (Dark Valley, 22-08). Outdoors the same lattice
//  drops probe planes just UNDER the terrain, which is why 2 m spacing printed
//  lattice-shaped blotches and why 3 m had to stay the default. Both defects are
//  the same defect: a probe standing in solid geometry measures nothing but
//  immediate hits, goes dark, and then gets weighted out of every lookup around
//  it, leaving the surfaces there with no answer at all.
//
//  Relocation gives each probe an offset from its lattice point — capped at
//  r_ddgi_reloc_max of the spacing so it stays inside its own cell — and moves it
//  towards whatever open space ITS OWN RAYS found. Nothing else in the pipeline
//  changes: the offset is stored in the strip at the bottom of the irradiance
//  atlas (see ddgi_common.glsl) and every consumer already has that atlas bound.
//
//  ⭐⭐⭐ TWO TESTS, BECAUSE THE TEXTBOOK ONE ALONE IS UNSAFE HERE
//  ------------------------------------------------------------------------
//  Majercik 2019 detects "probe inside geometry" by counting BACKFACE hits, and
//  moves the probe along the NEAREST one — that distance is literally the
//  thickness of what the probe is inside, so it is the shortest way out. That
//  test is kept (r_ddgi_reloc_back), with two changes forced by this world:
//
//   * ONLY NEARBY BACKFACES COUNT. Interior walls here are often one-sided, so a
//     probe in the middle of a room sees backfaces in every horizontal direction.
//     The plain count would call it buried and drive it INTO the masonry — the
//     exact opposite of the job. A backface 3 m away is a room; one 20 cm away is
//     the far side of the solid you are in.
//
//   * IT CAN BE SWITCHED OFF, because the sign is only as good as the winding.
//     rt_shade.glsl orients its normal against the ray precisely because collision
//     winding (r_rt_src 0) is not dependable, in a comment older than this pass.
//     With r_ddgi_reloc_back 0 the fallback test needs no winding at all: the
//     share of rays stopping within r_ddgi_reloc_near, and a move towards the
//     roomiest direction (every ray votes with the distance it survived; the
//     argmax ray breaks a tie that cancels).
//
//  ⭐⭐ MEASURED 23-08, AND IT DECIDES THE DEFAULT: on render geometry (r_rt_src 1)
//  the backface rate is 60% of all solid hits but 0.0% of DOWNWARD ones. Those two
//  numbers together are the whole answer. Random winding would put BOTH near 50%;
//  0.0% on the rays that land on terrain from above means the sign is honest. The
//  60% is the world being full of ONE-SIDED surfaces seen from their blind side —
//  the underside of a roof, the far face of a fence — which is exactly why the
//  nearby-only qualifier above is not optional. A distant backface is normal here
//  and means nothing; a close one still means "inside".
//
//  ⚠ THE FALLBACK IS STRICTLY WEAKER, in two ways. Under terrain it digs: terrain
//  is one-sided, so a probe below it has its UPWARD rays stop at the ground's
//  underside while its DOWNWARD rays miss entirely and come home at the ceiling
//  distance — "roomiest" then points further into the ground. And inside a thin
//  wall it does nothing at all: rays running ALONG the wall leave the shell and fly
//  far, so the near-share never reaches the threshold. Both cases are precisely
//  what the backface rule gets right, which is why it is the default.
//
//  ⭐ WHY THE FALLBACK STEPS INSTEAD OF JUMPING: it does not know how far out is
//  out, so it re-measures from the new spot each update — a fixed-point iteration,
//  stable only with a small step. The backface rule knows the distance and moves
//  once. Either way a probe that got out stops being buried and drifts home
//  slowly (r_ddgi_reloc_decay), with a margin so it cannot oscillate across the
//  threshold once per cascade round.
//
//  ⚠ ORDERING: this runs AFTER both blend passes, on the rays traced from where
//  the probe stood THIS update — so what the blends wrote matches where the probe
//  was. The move takes effect next update, and the "just moved" flag written into
//  w tells that update's blends to take their measurement whole instead of mixing
//  it into a history that describes the old position.

#define DDGI_SET      0
#define DDGI_B_CONSTS 0
#include "ddgi_common.glsl"

layout(std430, set = 0, binding = 1) readonly buffer DDGIRays  { vec4 rayIn[]; };
// Per-ray relocation hints written by the same trace: bit 0 = the ray stopped on
// SOLID geometry, bit 1 = it hit that geometry from behind. A miss and a leaf card
// are both 0 — see the write in ddgi_trace.comp.glsl for why a leaf is not a wall.
layout(std430, set = 0, binding = 4) readonly buffer DDGIHints { uint hintIn[]; };
// The irradiance atlas — written ONLY in the offset strip below the probe tiles.
layout(rgba16f, set = 0, binding = 2) uniform image2D irrImage;
// Self-audit counters. A pass whose whole job is invisible geometry has to be
// able to say what it did, or "relocation is on" and "relocation is broken" look
// identical in a log. Written only while the C++ side keeps the window open.
layout(std430, set = 0, binding = 3) buffer DDGIRelocStats { uint stats[]; };

layout(push_constant) uniform Push {
    float nearK;     // "a wall in my face" radius, in units of the cascade spacing
    float frac;      // buried when this share of rays is that close
    float stepK;     // travel per update, in units of the spacing
    float maxK;      // hard cap on |offset|, in units of the spacing
    float backFrac;  // > 0: also call it buried when this share of hits is backface
    float decay;     // pull back to the lattice per update when comfortable
    int   audit;     // 1 = accumulate the counters
    // ⭐THE CAMERA, AND WHY THE AUDIT NEEDS IT. A share taken over a whole cascade
    // cannot answer "is anything happening HERE": one room is two or three probes
    // out of 2048, so a working relocation and a dead one both print 0.0%. These
    // counters are the same questions asked of the probes within `nearR` metres of
    // the player, where the person looking at the screen actually is.
    // ⚠ SCALARS, NOT A vec3. In a std430 push block a vec3 aligns to 16 bytes, so
    // it would start at offset 32 while the C++ struct puts it at 28 — the exact
    // silent shift documented in the push-constant note. Four floats align to 4.
    float camX, camY, camZ;
    float nearR;
} pc;

layout(local_size_x = 64) in;

// ⚠ THE AUDIT REDUCES IN THE GROUP FIRST. Six atomics per probe means 64 threads
// queueing on the same six addresses of a HOST-visible buffer, which would make
// the pass measure its own instrumentation — the exact failure the profiler notes
// in this renderer keep recording. One atomic per counter per workgroup instead.
shared uint sSeen, sBuried, sMoved, sOffMM, sBack, sHits, sDownHits, sDownBack;
shared uint sLocal, sLocalBuried, sLocalMoved, sLocalOffMM;   // the same, within pc.nearR of the camera
shared uint sLocalHits;   // ...and how many of THEIR rays stopped on solid geometry
// PROBE CENSUS: mean luminance of each probe's own irradiance tile, in milli-units,
// summed for the probes near the camera and for the whole cascade. The pair is the
// "are the probes inside this house shining like the street outside" number — the
// question that until now could only be answered by eye from the dot view.
shared uint sLocalIrrMilli, sAllIrrMilli;
shared uint sInactive;   // classification: probes with no geometry in their cell box

void main()
{
    const int probe = int(gl_GlobalInvocationID.x);

    if (gl_LocalInvocationIndex == 0u) {
        sSeen = 0u; sBuried = 0u; sMoved = 0u; sOffMM = 0u; sBack = 0u; sHits = 0u;
        sDownHits = 0u; sDownBack = 0u;
        sLocal = 0u; sLocalBuried = 0u; sLocalMoved = 0u; sLocalOffMM = 0u; sLocalHits = 0u;
        sLocalIrrMilli = 0u; sAllIrrMilli = 0u;
        sInactive = 0u;
    }
    barrier();

    // ⚠ NOT an early return: the group-wide reduction below needs every thread to
    // reach its barriers, and a probe past the end simply contributes nothing.
    const bool live = probe < ddgi.casc.y;

    if (live) {
    const int   casc = ddgi.layout_.w;
    const int   gp   = ddgiGlobalProbe(casc, probe);
    // ⭐THE FIXED SET WHEN THERE IS ONE (r_ddgi_fixed_rays). Every count below is
    // a share of rays against a threshold; measured on the rotated set it is
    // re-sampled each update, a probe near the threshold flips, and each flip is a
    // "jumped" flag that throws its history away. The fixed set gives the same
    // answer for the same position every time. F = 0 = the old rotated rays.
    const int   rays = (ddgiFixedRays() > 0) ? ddgiFixedRays() : ddgi.dims.w;
    const float sp   = ddgiSpacing(casc);

    const ivec3 absC = ddgiSlotToAbs(ddgiSlotCoord(probe), ddgi.base[casc].xyz);
    const ivec2 tex  = ddgiOffsetTexel(gp);

    // A slot that scrolled in this update was traced from the bare lattice (see
    // ddgi_trace.comp.glsl), and the offset sitting in its texel belongs to the
    // previous occupant, at a different world position. Start it over.
    const vec3 cur = ddgiSlotIsNew(absC, casc) ? vec3(0.0) : imageLoad(irrImage, tex).xyz;

    const float nearDist = max(pc.nearK * sp, 0.01);

    int  near = 0, back = 0, backNear = 0, hits = 0;
    int  downHits = 0, downBack = 0;   // the terrain-winding probe, see below
    vec3 open  = vec3(0.0);
    float bestV = -1.0;
    vec3  bestDir = vec3(0.0, 1.0, 0.0);   // up: the answer for a probe under the terrain
    float backD   = 1e9;                   // nearest backface = the shortest way out
    vec3  backDir = vec3(0.0);

    // CLASSIFICATION (RTXGI): is there ANY geometry — solid or leaf — inside the
    // box of cells this probe takes part in, i.e. within one spacing along every
    // axis? If not, no surface point interpolates it and its rotated rays are
    // spent on nothing. A miss comes home at the ceiling, which is >= 4 spacings.
    const float ceilD = ddgiProbeDist(casc);
    bool nearGeom = false;

    for (int r = 0; r < rays; ++r) {
        const int   idx = probe * ddgi.dims.w + r;
        const float d   = rayIn[idx].a;
        const uint  hint = hintIn[idx];            // bit 0 = solid, bit 1 = backface
        const vec3  dir = ddgiProbeRayDir(r);

        if (d < ceilD * 0.999 && all(lessThan(abs(dir * d), vec3(sp)))) nearGeom = true;

        // ⚠ ONLY SOLID HITS COUNT AS GEOMETRY. A leaf card is neither a miss nor a
        // wall: a probe inside a crown is in a shaded place that needs light, not a
        // probe that has to escape, and the cutout structure is 99% of the triangles
        // in the bubble. The trace already made that distinction (bit 0).
        const bool solid = (hint & 1u) != 0u;
        if (solid) {
            ++hits;
            if ((hint & 2u) != 0u) {
                ++back;
                // ⚠⚠ ONLY A NEARBY BACKFACE MEANS "BURIED", and this qualifier is
                // what stops the feature from doing the exact opposite of its job.
                // Interior walls in this game are frequently one-sided, so a probe
                // standing in the MIDDLE OF A ROOM sees backfaces in every
                // horizontal direction — the plain Majercik count would call it
                // buried and drive it into the masonry it is trying to avoid. A
                // backface a few metres away is a wall seen from inside a room; a
                // backface 20 cm away is the far side of the thing you are inside.
                if (d < nearDist) { ++backNear; if (d < backD) { backD = d; backDir = dir; } }
            }
            if (d < nearDist)  ++near;
        }

        // ⭐DIAGNOSTIC, AND IT IS THE CONTROL FOR THE WHOLE BACKFACE RULE. A ray
        // going down out of a probe in open air lands on terrain from ABOVE, so
        // with honest winding this rate is ~0% while a random sign would sit at
        // ~50%. Measured 23-08: 0.0% down against 60% overall — the sign is real,
        // and the 60% is one-sided geometry seen from its blind side. Keep this
        // counter: it is the only thing that distinguishes "the world is like that"
        // from "the winding is noise", and the two demand opposite defaults.
        if (solid && dir.y < -0.5) {
            ++downHits;
            if ((hint & 2u) != 0u) ++downBack;
        }

        // Vote for the roomy direction. Capped, so one ray that escaped to the sky
        // does not out-vote the other 63 — what matters is which way is open, not
        // how open it is out there.
        const float v = min(d, nearDist * 4.0);
        open += dir * v;
        if (v > bestV) { bestV = v; bestDir = dir; }
    }

    const float nearF = float(near) / float(rays);

    // ⭐⭐⭐ TWO WAYS OUT, AND THE FIRST ONE IS THE ONLY ONE THAT WORKS UNDER TERRAIN
    //
    // "Move towards open space" is the obvious rule and it is WRONG for one-sided
    // geometry, which is most of this world: a probe half a metre under the
    // terrain sees its rays UPWARD stop at the ground's underside, while its rays
    // DOWNWARD miss entirely and come home at the ceiling distance — so the
    // roomiest direction points further into the ground, and the probe would dig.
    // The nearest BACKFACE is the honest answer: a hit taken from behind means the
    // solid ends there, so that distance IS the shortest way out. This is what
    // Majercik 2019 does and why the trace stores the sign at all.
    //
    // The open-space rule stays as the fallback for a world whose winding cannot be
    // trusted (r_rt_src 0, collision geometry — see rt_shade.glsl): there a probe
    // in a wall still escapes, just more slowly and only when it is properly
    // enclosed. r_ddgi_reloc_back is the switch between the two, and the self-audit
    // prints the measured backface rate so the choice is made on a number.
    const bool trustWinding = pc.backFrac > 0.0;
    const float backNearF   = float(backNear) / float(rays);
    const bool  buriedBack  = trustWinding && (backNearF > pc.backFrac) && (backD < 1e8);
    const bool buriedNear   = nearF > pc.frac;
    const bool buried       = buriedBack || buriedNear;

    vec3 want = cur;
    if (buriedBack) {
        // One move, not a crawl: the distance to the surface is known exactly, and
        // half the near radius past it is enough to be outside rather than on it.
        // The cap below is what keeps this inside the cell.
        want = cur + backDir * (backD + 0.5 * nearDist);
    } else if (buriedNear) {
        const float ol = length(open);
        const vec3  dir = (ol > 1e-3) ? (open / ol) : bestDir;
        want = cur + dir * (pc.stepK * sp);
    } else if (nearF < pc.frac * 0.5 && (!trustWinding || backNearF <= pc.backFrac * 0.5)) {
        // Comfortable WITH MARGIN. Without the margin a probe sitting exactly on
        // the threshold would step out, qualify, drift back in, and do it again
        // every update — a probe oscillating once per cascade round is a probe
        // whose history is permanently reset.
        want = cur * pc.decay;
    }

    // Stay inside the cell. Past ~0.45 of the spacing the probe crosses into a
    // neighbour's cell, where the trilinear weights (which stay on the lattice by
    // construction) would be describing a probe that is no longer between them.
    const float maxOff = pc.maxK * sp;
    const float m = length(want);
    if (m > maxOff) want *= maxOff / m;

    // The flag both blend passes read on the NEXT update of this cascade: the
    // history they hold was measured from `cur`, and if the probe has left that
    // place the honest blend is to take the new measurement whole.
    const float jumped = (distance(want, cur) > 0.05 * sp) ? 1.0 : 0.0;
    // ⭐w IS A FLAG FIELD, NOT A BOOL: bit 0 = moved (what the blends reset their
    // history on), bit 1 = buried (what the debug view paints red). One texel had
    // a spare channel and the state view needed a home; readers must mask the bit
    // they mean rather than test "> 0.5".
    // bit 2 = inactive (classification): only with the fixed set, because the
    // flag is what the NEXT update's trace and blends skip on and it must not
    // flicker with the rotation. A buried probe is never classified inactive —
    // it has to keep tracing to find its way out.
    const bool  inactive = (ddgi.classify.x != 0) && (ddgiFixedRays() > 0) && !nearGeom && !buried;
    const float flags  = jumped + (buried ? 2.0 : 0.0) + (inactive ? 4.0 : 0.0);

    imageStore(irrImage, tex, vec4(want, flags));

    if (pc.audit != 0) {
        atomicAdd(sSeen, 1u);
        if (inactive)      atomicAdd(sInactive, 1u);
        if (buried)        atomicAdd(sBuried, 1u);
        if (jumped > 0.5)  atomicAdd(sMoved, 1u);
        atomicAdd(sOffMM, uint(length(want) * 1000.0));   // mm, summed
        atomicAdd(sBack,  uint(back));
        atomicAdd(sHits,  uint(hits));
        atomicAdd(sDownHits, uint(downHits));
        atomicAdd(sDownBack, uint(downBack));

        // The census: the probe's own irradiance tile, averaged over its interior
        // texels (= mean irradiance over all directions). imageLoad, because this
        // pass binds the atlas as an image and the offset strip below the tiles is
        // out of reach of these coordinates by construction.
        float tileLum = 0.0;
        {
            const int   ppr  = ddgi.layout_.x;
            const ivec2 tile = ivec2(gp % ppr, gp / ppr) * (DDGI_IRR_N + 2) + 1;
            for (int ty = 0; ty < DDGI_IRR_N; ++ty)
                for (int tx = 0; tx < DDGI_IRR_N; ++tx) {
                    const vec3 c = imageLoad(irrImage, tile + ivec2(tx, ty)).rgb;
                    tileLum += dot(c, vec3(0.2126, 0.7152, 0.0722));
                }
            tileLum *= 1.0 / float(DDGI_IRR_N * DDGI_IRR_N);
        }
        atomicAdd(sAllIrrMilli, uint(min(tileLum, 64.0) * 1000.0));

        // ⭐AND THE SAME QUESTIONS ABOUT THE PROBES AROUND THE PLAYER. The cascade
        // average cannot see one room; this can. Measured from the LATTICE point,
        // so a probe that has already moved is still counted where it belongs.
        if (distance(ddgiLatticePos(absC, casc), vec3(pc.camX, pc.camY, pc.camZ)) <= pc.nearR) {
            atomicAdd(sLocal, 1u);
            if (buried)       atomicAdd(sLocalBuried, 1u);
            if (jumped > 0.5) atomicAdd(sLocalMoved, 1u);
            atomicAdd(sLocalOffMM, uint(length(want) * 1000.0));
            atomicAdd(sLocalIrrMilli, uint(min(tileLum, 64.0) * 1000.0));
            // ⭐The number that says whether the WALLS ARE THERE. A probe in a room
            // must stop most of its rays within a couple of metres; a few percent
            // means the geometry is missing from the RT world and every probe is
            // reading open sky through the walls — which looks exactly like "GI
            // does nothing indoors" while being no GI bug at all.
            atomicAdd(sLocalHits, uint(hits));
        }
    }
    }   // live

    barrier();
    if (pc.audit != 0 && gl_LocalInvocationIndex == 0u && sSeen != 0u) {
        atomicAdd(stats[0], sSeen);
        atomicAdd(stats[1], sBuried);
        atomicAdd(stats[2], sMoved);
        atomicAdd(stats[3], sOffMM);
        atomicAdd(stats[4], sBack);
        atomicAdd(stats[5], sHits);
        atomicAdd(stats[6], sDownHits);
        atomicAdd(stats[7], sDownBack);
        atomicAdd(stats[8],  sLocal);
        atomicAdd(stats[9],  sLocalBuried);
        atomicAdd(stats[10], sLocalMoved);
        atomicAdd(stats[11], sLocalOffMM);
        atomicAdd(stats[12], sLocalHits);
        atomicAdd(stats[13], sLocalIrrMilli);
        atomicAdd(stats[14], sAllIrrMilli);
        atomicAdd(stats[15], sInactive);
    }
}

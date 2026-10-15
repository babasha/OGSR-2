#version 460
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — DDGI irradiance blend: this frame's rays -> one probe's
// octahedral irradiance map, mixed into history.
//
// One workgroup per probe, one thread per texel of the (N+2)^2 tile. Each
// INTERIOR texel stands for a direction, and integrates every ray weighted by
// max(0, dot(texelDir, rayDir)) — the cosine lobe around that direction.
//
// The result stored is the cosine-weighted MEAN RADIANCE, not radiance times pi.
// That is deliberate: this engine's direct lighting is `albedo * sun_color * NdL`
// with no radiometric normalisation, so a probe that integrated the physical
// convention would come out consistently darker than every surface beside it.
// Matching the engine matters more here than matching radiometry — the same call
// rt_shade.glsl documents.
//
// The border texels are filled in the SAME dispatch, from shared memory rather
// than by reading the image back: the interior values are computed, published to
// shared, and only then does every thread (interior and border alike) write its
// texel. That avoids a second dispatch and an intra-dispatch image round-trip.

#define DDGI_SET      0
#define DDGI_B_CONSTS 0
#include "ddgi_common.glsl"

layout(std430, set = 0, binding = 1) readonly buffer DDGIRays { vec4 rayIn[]; };
layout(rgba16f, set = 0, binding = 2) uniform image2D irrImage;

const int N    = DDGI_IRR_N;      // 6
const int TILE = DDGI_IRR_N + 2;  // 8

layout(local_size_x = 8, local_size_y = 8) in;

const int kMaxRays = 64;
shared vec4 sRad[kMaxRays];   // rgb = radiance, a = distance (unused here)
shared vec3 sDir[kMaxRays];
shared vec3 sVal[N][N];       // NEW estimates first, then the blended interior (see the two barriers)
shared float sPrevL[N][N];    // history luminance — the probe-mean change test reads it

void main()
{
    const int casc  = ddgi.layout_.w;                    // the cascade this frame updates
    const int probe = int(gl_WorkGroupID.x);             // slot WITHIN that cascade
    if (probe >= ddgi.casc.y) return;
    const int gprobe = ddgiGlobalProbe(casc, probe);      // where it lives in the shared atlas

    // The rotated set only: rays [0, F) are the fixed relocation rays and carry no
    // radiance (ddgiProbeRayDir in ddgi_common.glsl). F = 0 is the old loop.
    const int fixedN = ddgiFixedRays();
    const int rays   = min(ddgiBlendRays(), kMaxRays);
    const int lane   = int(gl_LocalInvocationIndex);

    // The probe's strip texel: the "just moved" flag and the classification bit.
    // Read by every thread, before any barrier, because an inactive probe that is
    // not due for its refresh leaves the WHOLE WORKGROUP here — history untouched.
    const int flags = (ddgi.reloc.z != 0) ? int(round(imageLoad(irrImage, ddgiOffsetTexel(gprobe)).w)) : 0;
    const ivec3 absC = ddgiSlotToAbs(ddgiSlotCoord(probe), ddgi.base[casc].xyz);
    const bool  isNew = ddgiSlotIsNew(absC, casc);
    if (!isNew && ddgiSkipInactive(probe, flags)) return;

    // Cache the ray set once per probe: 64 threads each reading all 32 rays would
    // be 2048 loads per workgroup instead of 32.
    if (lane < rays) {
        sRad[lane] = rayIn[probe * ddgi.dims.w + fixedN + lane];
        sDir[lane] = ddgiProbeRayDir(fixedN + lane);
    }
    barrier();

    const ivec2 xy       = ivec2(gl_LocalInvocationID.xy);
    const bool  interior = (xy.x >= 1 && xy.x <= N && xy.y >= 1 && xy.y <= N);

    const int   ppr      = ddgi.layout_.x;
    const ivec2 tileBase = ivec2(gprobe % ppr, gprobe / ppr) * TILE;

    // Live across the barriers below: the estimate, the history texel and the
    // blend weight belong to THIS thread's texel, but the weight is only settled
    // after the whole probe's means are known.
    vec3  val  = vec3(0.0);
    vec3  prev = vec3(0.0);
    float hyst = 0.0;

    if (interior) {
        const ivec2 ic  = xy - ivec2(1);
        const vec3  dir = ddgiTexelDir(ic, N);

        vec3  sum = vec3(0.0);
        float wsum = 0.0;
        for (int r = 0; r < rays; ++r) {
            const float w = max(0.0, dot(dir, sDir[r]));
            if (w > 1e-4) { sum += sRad[r].rgb * w; wsum += w; }
        }
        val = (wsum > 1e-6) ? (sum / wsum) : vec3(0.0);

        // A slot that scrolled into the window this frame holds a stranger's
        // measurement; taking the new value whole is the only correct blend.
        //
        // ⭐ A PROBE THAT JUST MOVED IS THE SAME CASE. Relocation can slide a probe
        // most of a cell in one update, and the history in this tile was measured
        // from where it used to stand — with the hysteresis at 0.97 that stale
        // answer would outlive the move by ~30 updates and smear the very leak the
        // move was meant to fix. The relocation pass flags such a probe in w of its
        // offset texel; the flag is written AFTER this blend, so what we read here
        // is "did it move since the measurement this history is made of", which is
        // exactly the question.
        // `flags` is 0 while relocation is off — the strip is all zeroes then
        // anyway, and this keeps the blend bit-for-bit as it was.
        const bool  jumped = (flags & DDGI_FLAG_MOVED) != 0;
        // An inactive probe on its refresh carries `period` updates' worth of
        // change in one; ddgiRefreshHysteresis is plain grid.y otherwise.
        hyst = (isNew || jumped) ? 0.0 : ddgiRefreshHysteresis(ddgi.grid.y, flags);

        prev = imageLoad(irrImage, tileBase + xy).rgb;

        // Publish the RAW estimate first: the adaptive-hysteresis test below needs
        // the whole probe's new mean before any texel commits to a blend weight.
        sVal[ic.x][ic.y]   = val;
        sPrevL[ic.x][ic.y] = dot(prev, vec3(0.2126, 0.7152, 0.0722));
    }
    barrier();

    // ADAPTIVE HYSTERESIS (r_ddgi_fast, rides in ddgi.indoor.w) — a probe whose
    // fresh measurement DISAGREES with its history sheds that history fast.
    //
    // ⭐WHY: the fixed 0.95 is tuned for noise, but the bounce feedback multiplies
    // its settling time by ~1/(1-albedo_eff) — with the boosted albedo near the
    // 0.9 cap, a WRONG initial level (run into a house whose interior was until
    // now answered by a coarse cascade full of street-lit probes) takes 10-30
    // SECONDS to decay: the measured "забежал в дом — супер ярко, жди полминуты"
    // (27-08). The test is the PROBE-MEAN over the tile — 36 texels x 64 cosine-
    // weighted rays — whose frame noise is a few percent, far under the 0.25
    // threshold, so noise cannot trigger it; a x2 brightness step scores ~0.5
    // and drops the blend toward the floor, cutting the settle to ~1-2 s.
    if (interior && ddgi.indoor.w > 0.0 && hyst > 0.0) {
        float mNew = 0.0, mPrev = 0.0;
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                mNew  += dot(sVal[x][y], vec3(0.2126, 0.7152, 0.0722));
                mPrev += sPrevL[x][y];
            }
        const float rel = abs(mNew - mPrev) / max(max(mNew, mPrev), 1e-3);
        const float k   = smoothstep(0.25, 1.0, rel);
        hyst = mix(hyst, min(ddgi.indoor.w, hyst), k);
    }
    barrier();   // every thread past the mean loop before any texel overwrites sVal

    if (interior) {
        const ivec2 ic = xy - ivec2(1);
        val = mix(val, prev, hyst);
        sVal[ic.x][ic.y] = val;
    }
    barrier();

    // Border: an octahedral map wraps to the OPPOSITE edge, mirrored. Without
    // this a bilinear tap at a tile edge would read its neighbour probe's texels
    // and every probe would show a seam.
    ivec2 src = xy;
    if (xy.x == 0 && xy.y == 0)                 src = ivec2(N, N);
    else if (xy.x == 0        && xy.y == N + 1) src = ivec2(N, 1);
    else if (xy.x == N + 1    && xy.y == 0)     src = ivec2(1, N);
    else if (xy.x == N + 1    && xy.y == N + 1) src = ivec2(1, 1);
    else if (xy.x == 0)                         src = ivec2(1, N + 1 - xy.y);
    else if (xy.x == N + 1)                     src = ivec2(N, N + 1 - xy.y);
    else if (xy.y == 0)                         src = ivec2(N + 1 - xy.x, 1);
    else if (xy.y == N + 1)                     src = ivec2(N + 1 - xy.x, N);

    const vec3 outVal = sVal[src.x - 1][src.y - 1];
    imageStore(irrImage, tileBase + xy, vec4(outVal, 1.0));
}

#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — step 6b: RESAMPLE THE PROBE FIELD INTO THE VIEW FRUSTUM.
//
// One thread per froxel of a 128x72x64 grid laid over the same exponential-Z
// frustum the fog uses, filled with the two-band SH of the light arriving there.
// After this pass, "what light is in this bit of air" costs ONE trilinear tap for
// anything that is not a G-buffer surface: the volumetric fog, smoke sprites, and
// whatever translucent asks next.
//
// WHY THE GRID IS COARSER THAN THE FOG'S OWN (128x72x64 against 256x144x128)
// --------------------------------------------------------------------------
// Same call the smoke media grid already made, for the same reason and with the
// same result: the quantity is LOW FREQUENCY. Its source is a lattice of probes 1
// to 16 m apart, smoothed across eight of them — at 32 m a 128-wide grid already
// resolves half a metre laterally, far finer than anything the field can express.
// Consumers sample it at the SAME normalised frustum coordinates as their own
// grid, so the mismatch is invisible; what it buys is 8x fewer froxels to fill and
// 9.4 MB instead of 75.
//
// WHAT IS DROPPED FROM THE SURFACE LOOKUP, AND WHAT IS NOT
// --------------------------------------------------------
// ddgiSampleCascade weights each of the eight probes by three things: trilinear
// position, a wrap/backface term against the surface normal, and the Chebyshev
// visibility test. A point in AIR has no normal, so the second one is not
// simplified away here — it does not exist. The Chebyshev test does, and it is the
// one that matters: it is what stops a froxel inside a room from being lit by the
// probe standing in the street outside. Dropping it would build a beautiful,
// cheap, wall-ignoring fog.
//
// ⚠ ALSO DROPPED: the normal/view bias. On a surface it exists to push the lookup
// out of the geometry the sample sits on. In air there is nothing to push out of,
// and biasing along the view would have made the field slide as the camera turned.

#define DDGI_SET      0
#define DDGI_B_CONSTS 0
#define DDGI_B_IRR    1   // bound but unread here — DDGI_SAMPLE declares the atlas
#define DDGI_B_DEP    2   //   pair together with ddgiTileUV, which the test below needs
#define DDGI_SAMPLE
#include "ddgi_common.glsl"
#include "gi_sh.glsl"
#include "froxel.glsl"     // exp-Z slice <-> view Z, shared with vol_inject + the tonemap

layout(std430, set = 0, binding = 3) readonly buffer ProbeSH { GiProbeSH sh[]; };
layout(rgba16f, set = 0, binding = 4) uniform writeonly image3D uOutL0;   // rgb = c, a = field weight
layout(rgba16f, set = 0, binding = 5) uniform writeonly image3D uOutL1;   // rgb = d (luminance band 1)
// Self-audit counters (see ReportStats in vk_gi_froxel.cpp). Written only while the
// audit window is open; `dims.w` in the push block gates every atomic.
layout(std430, set = 0, binding = 6) buffer GiStats { uint gstat[]; };

layout(push_constant) uniform PC {
    vec4 camPosNear;    // xyz = camera world pos, w = grid near Z
    vec4 camDirLogFN;   // xyz = camera forward (unit), w = log2(far/near)
    vec4 camRightT;     // xyz = right * tan(fovX/2)
    vec4 camTopT;       // xyz = top   * tan(fovY/2)
    vec4 dims;          // xyz = grid dims, w = stats on
} pc;

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

shared float sLum[64];   // per-group audit reduction (one atomic per group, not per froxel)

// The eight-probe gather of ONE cascade, in SH. Mirrors ddgiSampleCascade's
// weighting exactly where it still applies — read them side by side.
bool giSampleCascade(int c, vec3 P, out vec3 co, out vec3 dd)
{
    const float s       = ddgiSpacing(c);
    const vec3  gf      = P / s;
    const ivec3 baseAbs = ivec3(floor(gf));
    const vec3  alpha   = clamp(gf - vec3(baseAbs), 0.0, 1.0);
    const vec2  depSize = vec2(textureSize(ddgiDepTex, 0));

    vec3  sumC = vec3(0.0), sumD = vec3(0.0);
    float sumW = 0.0;
    // Best Chebyshev visibility any consulted probe claims for this froxel —
    // the confidence gate below is the same one ddgiSampleCascade applies, for
    // the same reason (see the long note there).
    float vis  = 0.0;

    for (int i = 0; i < 8; ++i) {
        const ivec3 off  = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const ivec3 absC = baseAbs + off;

        // No probe exists outside the window; reading a wrapped-around stranger is
        // what the toroidal addressing exists to prevent.
        const ivec3 rel = absC - ddgi.base[c].xyz;
        if (any(lessThan(rel, ivec3(0))) || any(greaterThanEqual(rel, ddgi.dims.xyz))) continue;

        const int  slot = ddgiGlobalProbe(c, ddgiSlotIndex(ddgiAbsToSlot(absC)));
        // Relocation moves probes off the lattice; the moments below were measured
        // from where the probe actually stands, so the test has to use that too.
        const vec4 st   = ddgiProbeState(slot);
        // r_ddgi_skip_buried / r_ddgi_crush_tri — mirrored from ddgiSampleCascade,
        // so the fog and the surface behind it keep agreeing about the probes.
        if (ddgi.fix.w != 0 && (int(round(st.w)) & DDGI_FLAG_BURIED) != 0) continue;
        const vec3 pp   = ddgiLatticePos(absC, c) + st.xyz;

        const vec3 tri = mix(1.0 - alpha, alpha, vec3(off));
        const bool rtxgiOrder = ddgi.fix.y != 0;
        float w = rtxgiOrder ? 1.0 : tri.x * tri.y * tri.z;

        // Chebyshev / variance test against the probe's distance moments — the wall
        // test. Same sharpening and the same 0.05 floor as the surface path, so a
        // froxel and the surface behind it cannot disagree about which probes see
        // them.
        const vec3  bDir  = P - pp;
        const float bDist = length(bDir);
        float see = 1.0;   // what THIS probe claims: 1 unless the moments object
        if (bDist > 1e-6) {
            const vec2  md   = texture(ddgiDepTex, ddgiTileUV(slot, bDir, DDGI_DEP_N, depSize)).rg;
            const float mean = md.x;
            if (bDist > mean) {
                const float variance = abs(mean * mean - md.y);
                const float dlt      = bDist - mean;
                float cheb = variance / (variance + dlt * dlt);
                cheb = cheb * cheb * cheb;
                see  = cheb;
                w *= max(cheb, 0.05);
            }
        }
        vis = max(vis, see);

        // Crush the tail: near-zero weights carry no information and all of their
        // probe's noise. Eight of those summed is how a wall bleeds.
        const float crush = 0.2;
        if (w < crush) w *= (w * w) / (crush * crush);
        w = max(w, 1e-8);
        if (rtxgiOrder) {
            const vec3 t = max(tri, vec3(0.001));
            w *= t.x * t.y * t.z;
        }

        sumC += sh[slot].c0.rgb * w;
        sumD += sh[slot].d1.xyz * w;
        sumW += w;
    }

    if (sumW <= 1e-7) { co = vec3(0.0); dd = vec3(0.0); return false; }
    // The confidence gate (r_ddgi_conf, in ddgi.indoor.z) — same as the surface
    // path: when EVERY probe is rejected, the sumC/sumW normalisation rescales the
    // 0.05 floors back to full brightness and the froxel glows with whichever
    // reject was crushed the least (the open-air probe, i.e. the sky). Enclosed
    // air with no data goes dark instead; 0 = off, the old arithmetic.
    const float conf = (ddgi.indoor.z > 0.0) ? smoothstep(0.0, ddgi.indoor.z, vis) : 1.0;
    co = (sumC / sumW) * conf;
    dd = (sumD / sumW) * conf;
    return true;
}

void main()
{
    const ivec3 id   = ivec3(gl_GlobalInvocationID);
    const ivec3 dims = ivec3(pc.dims.xyz);
    const bool  live = all(lessThan(id, dims));

    float auditLum = -2.0;              // -2 = this thread is past the grid entirely
    if (live) {
        // The froxel CENTRE, unjittered. The fog jitters its own sample to
        // supersample density and shadows across frames; this field is already
        // temporally filtered upstream (the probes carry hysteresis), so jittering
        // it would only add motion to something meant to sit still.
        const vec2  uv    = (vec2(id.xy) + 0.5) / vec2(dims.xy);
        const vec2  ndc   = vec2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
        const float viewZ = Froxel_ViewZFromSlice((float(id.z) + 0.5) / float(dims.z),
                                                  max(pc.camPosNear.w, 1e-4),
                                                  max(pc.camDirLogFN.w, 1e-4));
        const vec3  P     = pc.camPosNear.xyz
                          + (pc.camDirLogFN.xyz + pc.camRightT.xyz * ndc.x + pc.camTopT.xyz * ndc.y) * viewZ;

        // Finest cascade that still covers this point, cross-faded into the next
        // where it runs out — a hard switch prints a ring on the ground centred on
        // the player, and in a VOLUME it would be a shell moving through the air.
        vec3 c = vec3(0.0), d = vec3(0.0);
        bool found = false;
        const int nc = ddgi.casc.x;
        for (int k = 0; k < nc; ++k) {
            const float f = ddgiCascadeFade(P, k);
            if (f <= 0.0) continue;
            vec3 cf, df;
            found = giSampleCascade(k, P, cf, df);
            c = cf; d = df;
            if (f < 0.999 && k + 1 < nc) {
                vec3 cc, dc;
                if (giSampleCascade(k + 1, P, cc, dc)) {
                    // r_ddgi_edge_clamp (edge.x) — the surface path's mirror: cap
                    // the coarse cascade's energy at k x the fine one's before the
                    // mix, so air inside a distant room does not glow with the 8 m
                    // lattice's street answer. See ddgiSampleIrradiance.
                    const float ek = ddgi.edge.x;
                    if (ek > 0.0) {
                        const vec3  lw   = vec3(0.2126, 0.7152, 0.0722);
                        const float cap  = ek * dot(cf, lw);
                        const float lumC = dot(cc, lw);
                        if (lumC > cap) cc *= cap / max(lumC, 1e-6);
                    }
                    c = mix(cc, cf, f); d = mix(dc, df, f); found = true;
                }
            }
            break;
        }

        // The weight is the OUTERMOST cascade's fade: past it the field has nothing
        // to say and every consumer must fall back to its own analytic fill.
        const float weight = found ? ddgiVolumeFade(P) : 0.0;
        imageStore(uOutL0, id, vec4(c, weight));
        imageStore(uOutL1, id, vec4(d, 0.0));
        auditLum = (weight > 0.0) ? dot(c, vec3(0.2126, 0.7152, 0.0722)) : -1.0;
    }

    // ---- self-audit ---------------------------------------------------------
    // Two numbers, printed by the module itself for its first ~900 frames: how much
    // of the frustum the field actually reaches, and how bright it says the air is.
    // The second one is the verdict — the constant it replaces is known on the CPU,
    // so "field 0.03 against flat 0.05" separates a feature that changes the frame
    // from one that reproduces, at 0.2 ms, the constant it displaced. A number that
    // needs a second console command is a number that does not exist (vk_wrc.cpp
    // learnt that the expensive way).
    if (pc.dims.w > 0.5) {
        const uint lane = gl_LocalInvocationIndex;
        sLum[lane] = auditLum;
        barrier();
        if (lane == 0) {
            float sum = 0.0; uint covered = 0u, total = 0u;
            for (uint i = 0u; i < 64u; ++i) {
                const float v = sLum[i];
                if (v < -1.5) continue;                 // outside the grid
                ++total;
                if (v < 0.0) continue;                  // live but the field does not reach
                sum += v; ++covered;
            }
            // Fixed point x64 per GROUP, not per froxel: 9216 groups of 64 froxels
            // cannot overflow a u32 at any plausible radiance, and it costs one
            // atomic per 64 threads instead of one per thread.
            atomicAdd(gstat[0], uint(min(sum * 64.0, 1.0e7)));
            atomicAdd(gstat[1], covered);
            atomicAdd(gstat[2], total);
        }
    }
}

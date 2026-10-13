#version 460
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — DDGI distance blend: mean and mean-of-squares of the ray
// lengths, per octahedral direction.
//
// This map is what separates DDGI from a plain irradiance volume. Interpolating
// eight probes with no idea where the walls are is exactly how light leaks
// through them; the two moments stored here let ddgiSampleIrradiance run a
// Chebyshev test — "given this probe's mean and variance of distance in your
// direction, how likely is it that you are even visible from it" — and weight the
// leaking probe out.
//
// Two differences from the irradiance blend, both deliberate:
//   * the lobe is SHARP (cos^k, k ~ 50, r_ddgi_depth_sharp). Irradiance wants a
//     wide cosine because that is the physical integral; a distance estimate
//     wants to hear from the rays that actually went that way, or a wall two
//     metres to the side smears its distance into this direction.
//   * distances are clamped to a ceiling (r_ddgi_probe_dist) BEFORE averaging, so
//     a miss that flew to the sky does not drag the mean to infinity and make
//     everything look visible. ⚠ That ceiling is PER CASCADE (ddgiProbeDist): one
//     shorter than the cascade's own cell diagonal makes the Chebyshev test reject
//     the far corner of every cell, which is what capped how sparse the ladder
//     could get.

#define DDGI_SET      0
#define DDGI_B_CONSTS 0
#include "ddgi_common.glsl"

layout(std430, set = 0, binding = 1) readonly buffer DDGIRays { vec4 rayIn[]; };
// RG32F since 23-09: the variance is E[d^2] - E[d]^2, a difference of two large
// close numbers, and in fp16 at the coarse cascades' 32 m+ ceilings the step of
// d^2 alone (1.0 at 1024) was the size of the variance being measured.
// DDGI_DEP_FP16 builds the fallback (ddgi_blend_dep_fp16.comp.spv) for a device
// that cannot linearly filter RG32F — vk_ddgi.cpp picks the atlas format to match.
#ifdef DDGI_DEP_FP16
layout(rg16f, set = 0, binding = 2) uniform image2D depImage;
#else
layout(rg32f, set = 0, binding = 2) uniform image2D depImage;
#endif
// The IRRADIANCE atlas, read-only, and only for the relocation strip in its last
// rows: that is where a probe's offset and its "just moved" flag live (see
// ddgi_common.glsl). The distance atlas has no spare texel to keep them in, and a
// probe that moved has to reset BOTH of its maps or the moments would keep
// describing the old position while the irradiance describes the new one.
layout(rgba16f, set = 0, binding = 3) readonly uniform image2D ddgiOffsImage;

const int N    = DDGI_DEP_N;      // 14
const int TILE = DDGI_DEP_N + 2;  // 16

layout(local_size_x = 16, local_size_y = 16) in;

const int kMaxRays = 64;
shared float sDist[kMaxRays];
shared vec3  sDir[kMaxRays];
shared vec2  sVal[N][N];          // blended interior moments, for the border copy

void main()
{
    const int casc  = ddgi.layout_.w;                    // the cascade this frame updates
    const int probe = int(gl_WorkGroupID.x);             // slot WITHIN that cascade
    if (probe >= ddgi.casc.y) return;
    const int gprobe = ddgiGlobalProbe(casc, probe);      // where it lives in the shared atlas

    // Rotated rays only, as in the irradiance twin: [0, F) are the fixed set.
    const int fixedN = ddgiFixedRays();
    const int rays   = min(ddgiBlendRays(), kMaxRays);
    const int lane   = int(gl_LocalInvocationIndex);

    // Strip flags, read before any barrier: an inactive probe not due for its
    // refresh leaves the whole workgroup here (see the irradiance twin).
    const int flags = (ddgi.reloc.z != 0) ? int(round(imageLoad(ddgiOffsImage, ddgiOffsetTexel(gprobe)).w)) : 0;
    const ivec3 absC = ddgiSlotToAbs(ddgiSlotCoord(probe), ddgi.base[casc].xyz);
    const bool  isNew = ddgiSlotIsNew(absC, casc);
    if (!isNew && ddgiSkipInactive(probe, flags)) return;

    if (lane < rays) {
        sDist[lane] = rayIn[probe * ddgi.dims.w + fixedN + lane].a;
        sDir[lane]  = ddgiProbeRayDir(fixedN + lane);
    }
    barrier();

    const ivec2 xy       = ivec2(gl_LocalInvocationID.xy);
    const bool  interior = (xy.x >= 1 && xy.x <= N && xy.y >= 1 && xy.y <= N);

    const int   ppr      = ddgi.layout_.x;
    const ivec2 tileBase = ivec2(gprobe % ppr, gprobe / ppr) * TILE;

    if (interior) {
        const ivec2 ic    = xy - ivec2(1);
        const vec3  dir   = ddgiTexelDir(ic, N);
        const float sharp = ddgi.grid.w;
        // ⚠ PER CASCADE. The trace already clamped to the same value; this second
        // clamp is what makes the two agree, and a ceiling shorter than the cell
        // diagonal is what made the coarse cascades reject themselves — see
        // ddgiProbeDist in ddgi_common.glsl.
        const float ceilD = ddgiProbeDist(casc);

        vec2  sum  = vec2(0.0);
        float wsum = 0.0;
        for (int r = 0; r < rays; ++r) {
            float w = max(0.0, dot(dir, sDir[r]));
            if (w > 1e-4) {
                w = pow(w, sharp);
                const float d = min(sDist[r], ceilD);
                sum  += vec2(d, d * d) * w;
                wsum += w;
            }
        }
        vec2 val = (wsum > 1e-6) ? (sum / wsum) : vec2(ceilD, ceilD * ceilD);

        // Same two resets as the irradiance blend, and for the same reasons: a slot
        // that scrolled in holds a stranger's history, and a probe that relocation
        // moved holds its own history from the wrong place. Moments are the half
        // that MUST be reset — a stale mean distance is precisely what makes the
        // Chebyshev test reject a probe that can now see the surface.
        // `flags` is 0 while relocation is off (see the irradiance twin).
        const bool  jumped = (flags & DDGI_FLAG_MOVED) != 0;
        const float hyst   = (isNew || jumped) ? 0.0 : ddgiRefreshHysteresis(ddgi.grid.y, flags);

        const vec2 prev = imageLoad(depImage, tileBase + xy).rg;
        val = mix(val, prev, hyst);

        sVal[ic.x][ic.y] = val;
    }
    barrier();

    ivec2 src = xy;
    if (xy.x == 0 && xy.y == 0)                 src = ivec2(N, N);
    else if (xy.x == 0        && xy.y == N + 1) src = ivec2(N, 1);
    else if (xy.x == N + 1    && xy.y == 0)     src = ivec2(1, N);
    else if (xy.x == N + 1    && xy.y == N + 1) src = ivec2(1, 1);
    else if (xy.x == 0)                         src = ivec2(1, N + 1 - xy.y);
    else if (xy.x == N + 1)                     src = ivec2(N, N + 1 - xy.y);
    else if (xy.y == 0)                         src = ivec2(N + 1 - xy.x, 1);
    else if (xy.y == N + 1)                     src = ivec2(N + 1 - xy.x, N);

    const vec2 outVal = sVal[src.x - 1][src.y - 1];
    imageStore(depImage, tileBase + xy, vec4(outVal, 0.0, 0.0));
}

#version 460
#extension GL_GOOGLE_include_directive : require
// ===========================================================================
//  xrRenderVulkan — GI PIXEL DEBUGGER (r_gi_probe): one thread, one dispatch,
//  a full numeric breakdown of the GI answer under the crosshair, read back to
//  the CPU and printed to the log.
// ===========================================================================
//
//  WHY THIS EXISTS. Every instrument this arc had was VISUAL (probe dots, debug
//  views, shade overlays) — usable only by the person at the screen. Every
//  question asked from the log ("is that wall lit by the field, the gather or
//  the sun? which probe is feeding it? did the Chebyshev test pass?") had to be
//  answered by describing pixels in words. This pass answers them in numbers:
//  aim at the suspicious surface, `r_gi_probe 1`, paste the [VK GIprobe] block.
//
//  WHAT IT CAPTURES for the surface at screen centre:
//   * world position, view depth, G-buffer normal, the cascade that serves it;
//   * the REAL ddgiSampleIrradiance answer (the exact function every receiver
//     calls — authoritative even if the capture loop below ever drifts);
//   * a per-probe breakdown of that lookup's finest cascade: trilinear, wrap
//     and Chebyshev weights, each probe's irradiance toward N, its position,
//     its offset-strip flags (buried/moved);
//   * the resolved GATHER at that pixel and the final HDR scene colour — so
//     "field vs gather vs everything-else(sun/direct)" separates in one read.
//
//  ⚠ THE CAPTURE LOOP IS A MIRROR of ddgiSampleCascade (the shared function
//  cannot return per-probe internals without touching every consumer). If the
//  weighting there changes, change it HERE TOO — a drifted instrument is worse
//  than none (see the gi_froxel mirror note; grep `cheb * cheb` finds all
//  copies). The authoritative `field` value above is computed by the REAL
//  function precisely so a drift shows up as capture-vs-field disagreement
//  instead of as a silent lie.

#define DDGI_SET      0
#define DDGI_B_CONSTS 0
#define DDGI_B_IRR    1
#define DDGI_B_DEP    2
#define DDGI_SAMPLE
#define DDGI_COMPUTE
#include "ddgi_common.glsl"

layout(set = 0, binding = 3) uniform sampler2D uDepth;    // full-res prepass depth (D32)
layout(set = 0, binding = 4) uniform sampler2D uNormal;   // normal G-buffer (rgb = N*0.5+0.5)
layout(set = 0, binding = 5) uniform sampler2D uGather;   // resolved final gather (GENERAL)
layout(set = 0, binding = 6) uniform sampler2D uScene;    // lit HDR scene, mip 0

layout(std430, set = 0, binding = 7) writeonly buffer GIProbeOut {
    vec4 oP;           // xyz = world position, w = view depth (m)
    vec4 oN;           // xyz = shading normal, w = device depth
    vec4 oField;       // rgb = ddgiSampleIrradiance (the REAL lookup), w = best cheb (vis) of the finest cascade
    vec4 oGather;      // rgb = resolved gather at this pixel, w = ddgiVolumeFade(P)
    vec4 oScene;       // rgb = final HDR scene colour at this pixel, w = gather-live flag echoed back
    vec4 oMisc;        // x = finest cascade index, y = its fade, z = capture-loop field luminance (drift check), w = flags (1 = sky pixel, 2 = outside volume)
    vec4 oProbeW[8];   // x = trilinear, y = wrap term, z = cheb (1 = untested/passed), w = final weight
    vec4 oProbeI[8];   // rgb = probe irradiance toward N, w = |probe - P| (m)
    vec4 oProbeP[8];   // xyz = probe world position (offset applied), w = flags: 1 = outside window (skipped), 2 = buried, 4 = |offset| > 1 cm
    // Appended at the TAIL (every offset above is unchanged): the WORLD RADIANCE
    // CACHE cell under the crosshair — the value the gather's rays actually read
    // for this patch, i.e. where r_wrc_field_clamp shows its work.
    vec4 oWrc;         // rgb = cached cell radiance at P, w = cache weight (0 = no shaded cell)
    vec4 oWrc2;        // x = cell LOD level (-1 = cache off), y = cell size (m), z = frames since its last shade (-1 = never), w = r_wrc_field_clamp echoed back
    // THE POST CHAIN (28-08). Born of a white blob the probe could not see: the
    // HDR pixel read dark while the SCREEN showed white, so the whiteness had to
    // be added AFTER sceneHDR — upscaler resolve, bloom add, fog composite — and
    // the instrument was blind to all three. These taps decompose the displayed
    // pixel: final ≈ tonemap((resolved ⊗ fog) + bloom×k). Tail append again.
    vec4 oPost1;       // rgb = RESOLVED base colour at this uv (DLSS/FSR output when upscaling, else scene mip0), w = resolved-live flag echoed back
    vec4 oPost2;       // rgb = raw bloom RT tap (pre-intensity), w = fog transmittance at this depth (1 = clear/off)
    vec4 oPost3;       // rgb = fog in-scatter at this depth, w = BAKED HEMI decoded from the normal tag (-1 = no bake under the crosshair)
} o;

// The WRC — read through wrcPeek ONLY (the read-only twin): a debugger that
// called wrcQuery would claim cells and overwrite their surface descriptors
// with a half-guessed albedo, corrupting the evidence it prints.
#define WRC_SET      0
#define WRC_B_CONSTS 8
#define WRC_B_CELLS  9
#define WRC_B_LIST   10
#define WRC_B_CTRL   11
#include "wrc_common.glsl"

// The post chain (see oPost1..3): what stands between sceneHDR and the display.
layout(set = 0, binding = 12) uniform sampler2D uBloom;    // blurred bright-pass, quarter res (tonemap's binding 1)
layout(set = 0, binding = 13) uniform sampler2D uResolved; // DLSS/FSR output when upscaling, else the scene stands in (tonemap's binding 6)
layout(set = 0, binding = 14) uniform sampler3D uVolume;   // integrated froxel fog: rgb = in-scatter, a = transmittance (tonemap's binding 4)

layout(push_constant) uniform PC {
    vec4 camDir;     // xyz = camera forward (unit)
    vec4 camRightT;  // xyz = right * tan(fovX/2)
    vec4 camTopT;    // xyz = top   * tan(fovY/2)
    vec4 zp;         // x = proj _33, y = proj _43, z = gather live (0/1), w = probe uv x
    vec4 camPos;     // xyz = camera world position, w = probe uv y
    vec4 res;        // zw = 1 / depth size (gbuffer_recon's derivative offsets)
    vec4 post;       // x = resolved live (0/1), y = fog grid nearZ (0 = fog off), z = farZ, w = log2(far/near)
} pc;

// The froxel slice math mirrors the tonemap's composite EXACTLY (same near/far/
// log from Vol::GetGridZ, single centre tap, no dither) — the instrument must
// read the same fog the pixel receives, not a rephrasing of it.
void samplePost(vec2 uv, float zview)
{
    o.oPost1 = vec4(textureLod(uResolved, uv, 0.0).rgb, pc.post.x);
    vec4 vol = vec4(0.0, 0.0, 0.0, 1.0);
    if (pc.post.y > 0.0) {
        const float zc = clamp(zview, pc.post.y, pc.post.z);
        const float vw = clamp(log2(zc / pc.post.y) / max(pc.post.w, 1e-4), 0.0, 1.0);
        vol = textureLod(uVolume, vec3(uv, vw), 0.0);
    }
    // The baked hemi under the crosshair — same decode the gather runs (the
    // normal tag carries 0.30 + hemi*0.45 for statics; see world_normal.frag).
    // No valid decode → store -(rawTag + 1) so the printout can show the RAW
    // alpha instead of a shrug (0 -> -1.0, 0.5 -> -1.5, 1.0 -> -2.0).
    const float gbTag = texture(uNormal, uv).a;
    const float hemi  = (gbTag > 0.27 && gbTag < 0.77)
                      ? clamp((gbTag - 0.30) / 0.45, 0.0, 1.0) : -(gbTag + 1.0);
    o.oPost2 = vec4(textureLod(uBloom, uv, 0.0).rgb, vol.a);
    o.oPost3 = vec4(vol.rgb, hemi);
}

// The SAME position/normal reconstruction the gather and RTAO start their rays
// from — including the depth-derivative fallback for surfaces that write no
// G-buffer normal (terrain, trees, grass). The first field dump naively decoded
// an empty normal texel into (-0.58,-0.58,-0.58) and would have mis-sampled
// every probe tile for such a surface.
#include "gbuffer_recon.glsl"

layout(local_size_x = 1) in;

void main()
{
    const vec2 uv = vec2(pc.zp.w, pc.camPos.w);

    const float zndc = texture(uDepth, uv).r;
    if (zndc >= 0.9999) {                                   // sky under the crosshair
        o.oP = vec4(0.0); o.oN = vec4(0.0, 0.0, 0.0, zndc);
        o.oField = vec4(0.0); o.oGather = vec4(0.0);
        o.oScene = vec4(textureLod(uScene, uv, 0.0).rgb, pc.zp.z);
        o.oMisc  = vec4(-1.0, 0.0, 0.0, 1.0);
        for (int i = 0; i < 8; ++i) { o.oProbeW[i] = vec4(0.0); o.oProbeI[i] = vec4(0.0); o.oProbeP[i] = vec4(0.0); }
        o.oWrc = vec4(0.0); o.oWrc2 = vec4(-1.0, 0.0, -1.0, wrc.params2.w);
        // Sky still receives the whole post chain (fog at the far slice, bloom,
        // the upscaler) — and a "white blob" CAN live on a sky pixel seen
        // through two windows, so the decomposition prints here too.
        samplePost(uv, pc.post.z);
        return;
    }

    const vec4  C     = fetchPos(uv);
    const float zview = C.w;
    const vec3  P     = pc.camPos.xyz + C.xyz;
    const vec3  V     = -normalize(C.xyz);                  // surface -> eye, the receivers' convention
    const vec3  N     = reconNormal(uv, C, V);

    o.oP = vec4(P, zview);
    o.oN = vec4(N, zndc);

    // The authoritative answer: the exact call every receiver makes.
    const vec3  field = ddgiSampleIrradiance(P, N, V);
    const float vfade = ddgiVolumeFade(P);

    // The WRC cell for this very patch — what a gather ray landing here reads.
    // BRIDGED across the LOD staircase exactly like probeFallbackCapped bridges
    // it (exact level, then +-1): the noise meter's MISS counter must count the
    // holes the PIXEL experiences, not the exact-level churn the bridge already
    // absorbs. oWrc2.x reports the level that actually answered.
    {
        vec3 wrad; int wage;
        int  wlvl = (wrc.params.z > 0.0) ? wrcLevel(P) : -1;
        int  used = wlvl;
        float ww = 0.0;
        if (wlvl >= 0) {
            ww = wrcPeekLvl(P, N, wlvl, wrad, wage);
            if (ww <= 0.0 && wlvl + 1 <= int(wrc.params.y)) { ww = wrcPeekLvl(P, N, wlvl + 1, wrad, wage); if (ww > 0.0) used = wlvl + 1; }
            if (ww <= 0.0 && wlvl - 1 >= 0)                 { ww = wrcPeekLvl(P, N, wlvl - 1, wrad, wage); if (ww > 0.0) used = wlvl - 1; }
        } else { wrad = vec3(0.0); wage = -1; }
        o.oWrc  = vec4(wrad, ww);
        o.oWrc2 = vec4(float(used), (used >= 0) ? wrcCellSize(used) : 0.0,
                       float(wage), wrc.params2.w);
    }

    o.oGather = vec4((pc.zp.z > 0.5) ? textureLod(uGather, uv, 0.0).rgb : vec3(0.0), vfade);
    o.oScene  = vec4(textureLod(uScene, uv, 0.0).rgb, pc.zp.z);
    samplePost(uv, zview);

    // Finest cascade covering P — the one the lookup starts from.
    int   cascade = -1;
    float cfade   = 0.0;
    for (int c = 0; c < ddgi.casc.x; ++c) {
        const float f = ddgiCascadeFade(P, c);
        if (f > 0.0) { cascade = c; cfade = f; break; }
    }
    if (cascade < 0) {                                       // outside the whole volume
        o.oField = vec4(field, 0.0);
        o.oMisc  = vec4(-1.0, 0.0, 0.0, 2.0);
        for (int i = 0; i < 8; ++i) { o.oProbeW[i] = vec4(0.0); o.oProbeI[i] = vec4(0.0); o.oProbeP[i] = vec4(0.0); }
        return;
    }

    // ---- per-probe capture: MIRROR of ddgiSampleCascade, see the header ⚠ ----
    const int   c  = cascade;
    const float s  = ddgiSpacing(c);
    const float bs = float(1 << c);
    const vec3  biased = P + N * (ddgi.grid.z * bs) + V * (ddgi.params.y * bs);

    const vec3  gf      = biased / s;
    const ivec3 baseAbs = ivec3(floor(gf));
    const vec3  alpha   = clamp(gf - vec3(baseAbs), 0.0, 1.0);

    const vec2 irrSize = vec2(textureSize(ddgiIrrTex, 0));
    const vec2 depSize = vec2(textureSize(ddgiDepTex, 0));

    vec3  sumIrr = vec3(0.0);
    float sumW   = 0.0;
    float vis    = 0.0;

    for (int i = 0; i < 8; ++i) {
        const ivec3 off  = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const ivec3 absC = baseAbs + off;

        const ivec3 rel = absC - ddgi.base[c].xyz;
        if (any(lessThan(rel, ivec3(0))) || any(greaterThanEqual(rel, ddgi.dims.xyz))) {
            o.oProbeW[i] = vec4(0.0);
            o.oProbeI[i] = vec4(0.0);
            o.oProbeP[i] = vec4(ddgiLatticePos(absC, c), 1.0);   // 1 = outside the window
            continue;
        }

        const int  slot = ddgiGlobalProbe(c, ddgiSlotIndex(ddgiAbsToSlot(absC)));
        const vec4 st   = ddgiProbeState(slot);
        const vec3 pp   = ddgiLatticePos(absC, c) + st.xyz;

        const vec3  toProbe = pp - P;
        const float distP   = length(toProbe);
        const vec3  dirP    = (distP > 1e-6) ? (toProbe / distP) : N;

        const vec3 tri = mix(1.0 - alpha, alpha, vec3(off));
        const float triW = tri.x * tri.y * tri.z;

        // r_ddgi_skip_buried, mirrored: the probe is reported (flags 2|8) but takes
        // no part in the sums, exactly as in ddgiSampleCascade.
        if (ddgi.fix.w != 0 && (int(round(st.w)) & DDGI_FLAG_BURIED) != 0) {
            o.oProbeW[i] = vec4(triW, 0.0, 0.0, 0.0);
            o.oProbeI[i] = vec4(0.0, 0.0, 0.0, distP);
            o.oProbeP[i] = vec4(pp, 2.0 + 8.0);
            continue;
        }

        // r_ddgi_crush_tri, mirrored: RTXGI order crushes wrap x Chebyshev and
        // applies the trilinear weight after.
        const bool rtxgiOrder = ddgi.fix.y != 0;
        float w = rtxgiOrder ? 1.0 : triW;

        const float wrap  = (dot(dirP, N) + 1.0) * 0.5;
        const float wrapT = (wrap * wrap) + 0.2;
        w *= wrapT;

        float cheb = 1.0;
        const vec3  bDir  = biased - pp;
        const float bDist = length(bDir);
        if (bDist > 1e-6) {
            const vec2  md   = DDGI_TEX(ddgiDepTex, ddgiTileUV(slot, bDir, DDGI_DEP_N, depSize)).rg;
            const float mean = md.x;
            if (bDist > mean) {
                const float variance = abs(mean * mean - md.y);
                const float d        = bDist - mean;
                cheb = variance / (variance + d * d);
                cheb = cheb * cheb * cheb;
                w *= max(cheb, 0.05);
            }
        }
        vis = max(vis, cheb);

        const float crush = 0.2;
        if (w < crush) w *= (w * w) / (crush * crush);
        w = max(w, 1e-8);
        if (rtxgiOrder) {
            const vec3 t = max(tri, vec3(0.001));
            w *= t.x * t.y * t.z;
        }

        const vec3 irr = DDGI_TEX(ddgiIrrTex, ddgiTileUV(slot, N, DDGI_IRR_N, irrSize)).rgb;
        sumIrr += irr * w;
        sumW   += w;

        // Offset-strip flags (bit 0 = just moved, bit 1 = buried) — only live with
        // relocation on; zeros otherwise.
        float flags = 0.0;
        if (ddgi.reloc.z != 0) {
            const vec4 ot = texelFetch(ddgiIrrTex, ddgiOffsetTexel(slot), 0);
            const int  fl = int(ot.w + 0.5);
            if ((fl & 2) != 0) flags += 2.0;
            if (length(ot.xyz) > 0.01) flags += 4.0;
            if ((fl & DDGI_FLAG_INACTIVE) != 0) flags += 16.0;
        }

        o.oProbeW[i] = vec4(triW, wrapT, cheb, w);
        o.oProbeI[i] = vec4(irr, distP);
        o.oProbeP[i] = vec4(pp, flags);
    }

    const vec3 capField = (sumW > 1e-7) ? (sumIrr / sumW) : vec3(0.0);

    o.oField = vec4(field, vis);
    o.oMisc  = vec4(float(cascade), cfade,
                    dot(capField, vec3(0.2126, 0.7152, 0.0722)), 0.0);
}

#version 450
#extension GL_GOOGLE_include_directive : require
// ===========================================================================
//  xrRenderVulkan — FINAL GATHER RESOLVE: spatial bilateral + temporal EMA.
// ===========================================================================
//  gather.comp deliberately produces a NOISY estimate (one or two rays per
//  pixel). This pass is the other half of it, and in idTech 8's own cost table
//  denoise + upscale are 1.4 ms of the 1.7 ms the whole GI system spends — the
//  tracing is the cheap part. So this is not "a blur added at the end": without
//  it the gather is not a feature, it is a noise generator.
//
//  Three weights, each killing a specific artefact:
//
//   * VIEW-DEPTH similarity — the same exp(-|dz|*16/z) gate ssao_blur uses, so
//     indirect light never bleeds across a silhouette (a bright wall's bounce
//     must not wash onto the sky-lit ground 10 m behind it).
//
//   * HIT-DISTANCE similarity — idTech 8 weights its denoiser by the delta of
//     the ray hit distance, and it matters most exactly where depth agrees: two
//     neighbouring pixels on the SAME flat floor, one whose ray hit a crate 30 cm
//     away and one whose ray escaped to the sky, are not measuring the same
//     quantity, and averaging them is what smears contact darkening away.
//
//   * TEMPORAL history through the motion vectors, with a RELAXED neighbourhood
//     clamp. The clamp bounds ghosting on disocclusion; the slack exists because
//     a tight min/max clamp drags the smooth history back into this frame's
//     narrow jittered range every frame and undoes the accumulation (the lesson
//     ssao_blur already paid for).
//
//  HOW LONG THE LIGHT TAKES TO ARRIVE IS THIS FILTER'S ALPHA, AND IT WAS FIXED
//  ---------------------------------------------------------------------------
//  ⭐⭐⭐ A CONSTANT alpha ANSWERS THE WRONG QUESTION ON THE FIRST FRAME. At 0.92 a
//  pixel needs ~12 frames per e-fold and ~30 to settle, so EVERY pixel the camera
//  newly reveals spends half a second climbing towards a value the very first
//  sample already estimated without bias. That is not denoising — it is a fade-in,
//  and it is what "turn around and wait for the light" actually is.
//
//  The fix is the one every progressive integrator uses: alpha is not a constant,
//  it is n/(n+1) where n is HOW MANY FRAMES THIS PIXEL HAS ACTUALLY ACCUMULATED.
//  Sample 1 is taken whole, sample 2 averages two, sample 3 averages three, and
//  once n passes 1/(1-alpha) the ramp hands over to the exponential window that
//  was always there. ⭐ Same steady state, same noise floor, same cost — the ONLY
//  thing that changes is that the first frames are an unbiased mean instead of 8%
//  of one. `n` lives in its own R32F ping-pong (uAge / outAge).
//
//  ⚠⚠ THE COUNTER IS ONLY HONEST IF IT IS RESET WHEN THE HISTORY IS A STRANGER,
//  and until now nothing checked. The only test was "is prevUV on screen": a pixel
//  disoccluded from BEHIND an object reprojects to a perfectly valid on-screen
//  texel belonging to the occluder, and the neighbourhood clamp below is
//  deliberately loose (three times the local spread), so that stranger was
//  accepted at 92% weight and dragged along — the smear when the player walks.
//
//  ⭐ THE TEST COSTS NOTHING BECAUSE THE DEPTH IS ALREADY THERE: this pass stores
//  the view depth of every texel in alpha (the receivers' upsample needs it), so
//  `texture(uHist, prevUV).a` IS the previous frame's depth at the reprojected
//  point. Compare it against where this pixel's surface WOULD have been last
//  frame — one scalar, `weights.w`, the camera's forward advance — and reject on
//  disagreement. Rotation is folded into the tolerance on the CPU rather than
//  reconstructed per pixel: the depth error a rotation of theta can induce is
//  bounded by tan(fovX/2) * theta RELATIVE to the depth, which is exactly the
//  shape of the tolerance already.
//
//  ⚠ THE HISTORY IS A PING-PONG, NOT A COPY. The previous frame's resolved image
//  is bound as `uHist` and this frame writes the OTHER one, so there is no blit
//  between them and no frame where a consumer could sample a half-written image.
//
//  THE SH RIDES ALONG ON THE SAME WEIGHTS (r_gather_sh)
//  ----------------------------------------------------
//  With the 2-band SH on, gather.comp also writes an L1 deviation vector per
//  colour channel (see its header). It is the NOISIEST part of the estimate — a
//  single ray always reports a direction, even where the light has none — so it
//  needs this filter more than the radiance does, not less.
//
//  ⚠⚠ IT NEEDS A HARDER FILTER THAN THE RADIANCE, AND THIS WAS MEASURED. The
//  first version shared the radiance's weights and its history weight, and
//  r_gather_debug 5 came back SATURATED: |d| / L0 stood at the limiter's ceiling
//  across the whole frame, which means the term was carrying noise, not
//  direction. The arithmetic agrees — one cosine-sampled ray reports
//  |d| = (8/3)|sin theta| L, about 1.6x the mean it sits next to, so the filter
//  has to remove an order of magnitude more from this channel than from that one.
//
//  Two changes, both from what the quantity IS rather than from taste:
//   * THE SPATIAL GATE IS DEPTH ALONE. The hit-distance gate exists to keep
//     contact darkening sharp in the RADIANCE, and at one ray per pixel it is
//     also what collapses the neighbourhood to nothing: adjacent rays hit a leaf
//     at 30 cm and the sky at 24 m. But two pixels on one wall are measuring the
//     same INCOMING DIRECTION whatever their rays happened to strike, so for this
//     channel that gate rejects agreement rather than protecting detail.
//   * ITS OWN, LONGER HISTORY (r_gather_sh_temporal, 0.98 against the radiance's
//     0.92). Where light comes FROM changes far more slowly than how much of it
//     arrives, so this channel can afford four times the accumulation — and it
//     needs it: 0.92 leaves ~24 effective samples, and 1.6 / sqrt(24) is still a
//     third of the value it is meant to modulate.
// ===========================================================================

layout(set = 0, binding = 0) uniform sampler2D uDepth;   // full-res prepass depth (D32)
layout(set = 0, binding = 1) uniform sampler2D uRaw;     // this frame's raw gather (rgb = irradiance, a = mean hit distance)
layout(set = 0, binding = 2) uniform sampler2D uHist;    // previous frame's RESOLVED gather
layout(set = 0, binding = 3) uniform sampler2D uMV;      // full-res motion vectors (prevUV - curUV)
layout(rgba16f, set = 0, binding = 4) uniform writeonly image2D outResolved;
// The 2-band SH's L1: three tiles stacked vertically (R, G, B channel), each
// texel xyz = that channel's deviation vector. Same ping-pong as the radiance.
layout(set = 0, binding = 5) uniform sampler2D uRawSH;    // this frame's raw L1
layout(set = 0, binding = 6) uniform sampler2D uHistSH;   // previous frame's RESOLVED L1
layout(rgba16f, set = 0, binding = 7) uniform writeonly image2D outSH;
// How many frames the pixel at this texel has accumulated, ping-ponged with the
// radiance. ⚠ NEAREST, unlike the radiance history it travels with: a count
// blended across a disocclusion edge would claim confidence the texel on the far
// side of that edge never earned.
layout(set = 0, binding = 8) uniform sampler2D uAge;
layout(r32f, set = 0, binding = 9) uniform writeonly image2D outAge;
// The normal G-buffer (rgb = worldN*0.5+0.5, a = source tag), read at the SAME uv
// the trace reconstructed its ray basis from — so "same surface" here means the
// same thing it meant when the sample was taken. NEAREST, like the trace reads it.
layout(set = 0, binding = 10) uniform sampler2D uNormal;

layout(push_constant) uniform PC {
    vec4 zp;         // x = proj _33, y = proj _43, z = SH channel live (r_gather_sh), w = the SH's own EMA alpha
    vec4 res;        // xy = gather target size, zw = 1 / size
    vec4 temporal;   // x = EMA alpha (0 = off), y = MV valid, z = history valid, w = spatial radius (texels)
    // THE TWO WEIGHT KNOBS, and why they are knobs and not a rewrite.
    // MEASURED 24.08: r_gather_radius 2 and 3 are indistinguishable while 0 and 1
    // are clearly worse. A filter whose result stops improving once the aperture
    // passes two texels is not aperture-limited - it is WEIGHT-limited, and any
    // wider kernel (a-trous included) would inherit the same ceiling. So the
    // aperture stays as it is and the weights become measurable instead.
    //   x = r_gather_hitgate: scales the hit-distance gate's exponent. 1 = as
    //       shipped, 0 = gate off. It is a one-sample quantity CORRELATED with the
    //       value being filtered (a ray that reached the sky is both far and
    //       bright), so at 1 spp it can reject neighbours for disagreeing rather
    //       than for being a different surface.
    //   y = r_gather_plane: how much the depth weight compares against the
    //       PLANE through the centre tap instead of against its depth alone.
    //       0 = as shipped. A floor seen at a grazing angle has neighbours at
    //       genuinely different depths, so the shipped test decays with ring
    //       number on the largest surface in the frame - which is exactly the
    //       saturation the measurement found.
    //   z = r_gather_disocc: RELATIVE depth tolerance of the reprojection test,
    //       with the frame's rotation already folded in on the CPU. 0 = OFF, i.e.
    //       exactly the shipped behaviour (accept any on-screen history).
    //   w = the camera's forward advance since the previous frame, in metres:
    //       dot(curEye - prevEye, curFwd). A static point that was z0 away this
    //       frame stood z0 + this far away last frame.
    vec4 weights;
    // x = r_gather_adapt: how much of the n/(n+1) ramp to apply. 1 = the full
    //     progressive mean, 0 = the shipped constant alpha (the A/B), and the
    //     values between trade response for stability continuously.
    // y = the count at which the ramp hands over to the exponential window,
    //     1/(1-alpha) - past it n/(n+1) is already above alpha and min() picks
    //     alpha. Computed on the CPU so the shader never divides by zero here.
    // z = r_gather_ffreject: k of the spatial sigma clip below (0 = off).
    // w reserved.
    vec4 hist;
} pc;

float viewDepth(vec2 uv)
{
    const float zndc = texture(uDepth, uv).r;
    return clamp(pc.zp.y / (zndc - pc.zp.x), 0.0, 10000.0);
}

// SPATIAL FIREFLY REJECTION (r_gather_ffreject) ------------------------------
//
// The filter below is a WEIGHTED MEAN, and a mean is not a robust estimator: one
// surviving outlier out of 25 taps still deposits ~1/25 of its brightness on this
// texel, and the receiver's 4-tap upsample then spreads that over a 4x4 block of
// screen pixels. That is why the artefact reads as a DOT rather than as noise, and
// why widening the aperture never helped — more taps dilute the outlier but also
// dilute the signal, so the dot only gets bigger and dimmer.
//
// The ceiling is mean + k*sigma of the 3x3 luminance window. ⚠ COMPUTED WITH ONE
// ROUND OF SIGMA CLIPPING, and that round is the whole point: a lone fireball in
// the window inflates BOTH the mean and the sigma it would be tested against, so
// the naive one-pass statistic quietly excuses the very sample it exists to catch
// (a 8.0 outlier among eight 0.15 neighbours lifts mean+2sigma to ~6, and passes).
// Pass one gets a first mean/sigma, pass two recomputes them from the taps within
// 2 sigma of it — cheap, because the nine luminances are already in registers.
//
// The clip is applied to every tap AND to the centre, before any weighting, so a
// pixel whose own ray came back a fireball is repaired by its neighbourhood rather
// than exported to it. Genuine contrast survives: a real edge inside a 3x3 window
// sits well within 2 sigma of a distribution that contains both of its sides.
float ffLuma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float ffCeiling(vec2 uv)
{
    if (pc.hist.z <= 0.0) return 1e30;

    float l[9];
    int   n = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            l[n++] = ffLuma(texture(uRaw, uv + vec2(x, y) * pc.res.zw).rgb);

    float s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < 9; ++i) { s1 += l[i]; s2 += l[i] * l[i]; }
    float mean = s1 / 9.0;
    float sd   = sqrt(max(s2 / 9.0 - mean * mean, 0.0));

    // Round two: the same two moments over the taps the first round did not flag.
    // The count cannot reach zero — the mean itself is always within 2 sigma — so
    // no guard is needed beyond the one the divide already has.
    const float lim = mean + 2.0 * sd;
    s1 = 0.0; s2 = 0.0; n = 0;
    for (int i = 0; i < 9; ++i)
        if (l[i] <= lim) { s1 += l[i]; s2 += l[i] * l[i]; ++n; }
    mean = s1 / float(n);
    sd   = sqrt(max(s2 / float(n) - mean * mean, 0.0));

    return mean + pc.hist.z * sd;
}

vec3 ffClip(vec3 c, float cap)
{
    const float l = ffLuma(c);
    return (l > cap) ? c * (cap / max(l, 1e-6)) : c;
}

// THE NORMAL WEIGHT (r_gather_nrmw) ------------------------------------------
//
// idTech 8's spatial denoise weights by normal, depth and hit-distance delta; ours
// had the last two. Depth alone cannot separate two surfaces meeting at an edge —
// a wall and the floor it stands on agree on depth right at the seam — so without
// this the filter either bleeds across the seam or has to buy its selectivity from
// the hit-distance gate, which at 1 spp rejects for BRIGHTNESS as much as for
// geometry. This is the honest half of that job.
//
// ⚠ Returns 1 where either side has no normal. Foliage, grass and terrain write no
// tag (see gbuffer_recon), and a missing normal must never veto a tap: those are
// the surfaces whose neighbourhoods are already hardest to filter.
vec3 nrmAt(vec2 uv, out bool ok)
{
    const vec4 n = texture(uNormal, uv);
    ok = n.a > 0.25 && dot(n.xyz - 0.5, n.xyz - 0.5) > 1e-6;
    return ok ? normalize(n.xyz * 2.0 - 1.0) : vec3(0.0);
}

float nrmWeight(vec3 N0, bool n0ok, vec2 uvT)
{
    if (pc.hist.w <= 0.0 || !n0ok) return 1.0;
    bool nOk;
    const vec3 Ni = nrmAt(uvT, nOk);
    if (!nOk) return 1.0;
    return pow(max(dot(N0, Ni), 0.0), pc.hist.w);
}

// A tile of the raw SH, addressed by TEXEL. texelFetch and not texture(): the
// neighbourhood taps land on texel centres anyway, and an integer address cannot
// wander into the next tile the way a filtered one can.
vec3 rawSH(ivec2 p, int c)
{
    const ivec2 hi = ivec2(int(pc.res.x) - 1, int(pc.res.y) - 1);
    const ivec2 q  = clamp(p, ivec2(0), hi);
    return texelFetch(uRawSH, ivec2(q.x, q.y + c * int(pc.res.y)), 0).xyz;
}

// The history tap, reprojected, and therefore filtered — it lands between texels.
// ⚠ HALF A TEXEL OF INSET, and it is not cosmetic: the three tiles share ONE
// image, so a bilinear footprint on a tile's first or last row would blend the
// RED channel's deviation into the GREEN one's. Clamping the sample point to
// [0.5, H-0.5] keeps all four taps inside the tile that was asked for.
vec3 histSH(vec2 uv, int c)
{
    const float H = pc.res.y;
    const float y = clamp(uv.y * H, 0.5, H - 0.5);
    return texture(uHistSH, vec2(uv.x, (float(c) * H + y) / (3.0 * H))).xyz;
}

layout(local_size_x = 8, local_size_y = 8) in;

void main()
{
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(pc.res.x) || px.y >= int(pc.res.y)) return;
    const vec2 uv = (vec2(px) + 0.5) * pc.res.zw;

    const vec4  c0 = texture(uRaw, uv);
    const float z0 = viewDepth(uv);
    const float d0 = c0.a;

    // ⚠ RADIANCE ONLY. c0.a is the hit distance the gate below weights by, and it
    // travels unclipped: a clipped SAMPLE is still an honest report of how far its
    // ray flew, and rescaling that would corrupt the very test that decides which
    // neighbours are the same surface.
    const float ffCap = ffCeiling(uv);

    bool       n0ok = false;
    const vec3 N0   = nrmAt(uv, n0ok);

    const int R = int(pc.temporal.w);

    // Screen-space depth slope, central differences, in HALF-RES TEXELS: what turns
    // "is my neighbour at MY DEPTH" into "is my neighbour ON MY PLANE".
    // ⚠ CLAMPED, and that cap is the whole safety of it: at a silhouette the
    // difference is not a slope at all, and an unclamped prediction would happily
    // reach across the edge and accept a tap from the surface behind. The cap is a
    // fraction of z0, so it scales with the same distance the tolerance below does.
    vec2 slope = vec2(0.0);
    if (pc.weights.y > 0.0) {
        const float zxp = viewDepth(uv + vec2(pc.res.z, 0.0)), zxm = viewDepth(uv - vec2(pc.res.z, 0.0));
        const float zyp = viewDepth(uv + vec2(0.0, pc.res.w)), zym = viewDepth(uv - vec2(0.0, pc.res.w));
        const float cap = max(z0, 0.1) * 0.05;
        slope = clamp(vec2(zxp - zxm, zyp - zym) * 0.5, vec2(-cap), vec2(cap)) * pc.weights.y;
    }

    const vec3 c0f = ffClip(c0.rgb, ffCap);

    vec3  sum  = c0f;
    float wsum = 1.0;
    vec3  mn   = c0f;
    vec3  mx   = c0f;

    // One column per colour channel, exactly as gather.comp accumulated them.
    const bool shOn = pc.zp.z > 0.5;
    mat3  shSum = mat3(0.0), shMn = mat3(0.0), shMx = mat3(0.0);
    float shWsum = 1.0;   // its own accumulator: the SH's gate is not the radiance's
    if (shOn)
        for (int c = 0; c < 3; ++c) { const vec3 v = rawSH(px, c); shSum[c] = v; shMn[c] = v; shMx[c] = v; }

    for (int y = -R; y <= R; ++y)
        for (int x = -R; x <= R; ++x) {
            if (x == 0 && y == 0) continue;
            const vec2 o  = vec2(x, y) * pc.res.zw;
            const vec4 t  = texture(uRaw, uv + o);
            const float zi = viewDepth(uv + o);

            // The depth this tap WOULD have if it lay on the centre's plane. With
            // r_gather_plane 0 the slope is zero and this is the shipped test.
            const float zRef = z0 + slope.x * float(x) + slope.y * float(y);
            float w = exp(-abs(zi - zRef) * 16.0 / max(z0, 0.1));
            // Geometry, judged by geometry — see nrmWeight. Applied to the radiance
            // AND to the SH's gate below it: a direction borrowed from a surface
            // facing elsewhere is wrong for exactly the same reason a radiance is.
            w *= nrmWeight(N0, n0ok, uv + o);
            // GEOMETRY agreement alone is the SH's gate — "is this the same
            // surface" (depth, plus the normal since r_gather_nrmw). See the header
            // for why the hit-distance gate below must not apply to it: for a
            // direction, two rays that struck different things are still two
            // samples of one answer.
            const float wSH = w;
            // Hit-distance gate, scaled by the distances themselves so it is
            // scale-free: 30 cm apart matters when both rays are short and does
            // not when both ran metres before hitting anything.
            w *= exp(-abs(t.a - d0) * 2.0 * pc.weights.x / max(max(d0, t.a), 0.25));

            // Clipped before it is weighted, and the min/max the temporal clamp
            // rides on are taken from the CLIPPED value too — otherwise a fireball
            // this filter just refused would still widen the window that decides
            // how far the history is allowed to move.
            const vec3 tf = ffClip(t.rgb, ffCap);
            sum  += tf * w;
            wsum += w;
            mn = min(mn, tf);
            mx = max(mx, tf);

            if (shOn) {
                for (int c = 0; c < 3; ++c) {
                    const vec3 v = rawSH(px + ivec2(x, y), c);
                    shSum[c] += v * wSH;
                    shMn[c]   = min(shMn[c], v);
                    shMx[c]   = max(shMx[c], v);
                }
                shWsum += wSH;
            }
        }

    vec3 val = sum / max(wsum, 1e-4);
    mat3 shVal = mat3(0.0);
    if (shOn) for (int c = 0; c < 3; ++c) shVal[c] = shSum[c] / max(shWsum, 1e-4);

    const float alpha = pc.temporal.x;
    // How many frames this pixel will have accumulated once this one is stored.
    // 1 means "only the sample computed above" — the value every rejected or
    // history-less pixel restarts from, so the NEXT frame averages two samples
    // instead of throwing the first one away at 8% weight.
    float ageOut = 1.0;
    if (alpha > 0.0 && pc.temporal.z > 0.5) {
        const vec2 mv     = (pc.temporal.y > 0.5) ? texture(uMV, uv).rg : vec2(0.0);
        const vec2 prevUV = uv + mv;
        if (all(greaterThanEqual(prevUV, vec2(0.0))) && all(lessThanEqual(prevUV, vec2(1.0)))) {
            // ---- is the history at prevUV THIS surface, or a stranger? --------
            // The alpha channel of the resolved image is the view depth that texel
            // was written with, so the test needs no extra target — only its own
            // tap. ⚠⚠ texelFetch, NOT texture(), and this is not a micro-optimisation:
            // uHist is bound LINEAR because the RADIANCE tap lands between texels,
            // and a bilinear depth beside a silhouette is a value halfway between
            // two surfaces — mid-air, matching neither. Testing against that would
            // reject a one-texel band around every edge in the frame, EVERY frame,
            // and leave a permanent sparkling fringe exactly where this pass is
            // supposed to be sharp. Fetching the nearest texel asks about a surface
            // that actually exists.
            const ivec2 hp      = clamp(ivec2(prevUV * pc.res.xy), ivec2(0), ivec2(pc.res.xy) - ivec2(1));
            const float zPrev   = texelFetch(uHist, hp, 0).a;
            const float zExpect = z0 + pc.weights.w;          // where this surface stood last frame
            const bool  zOK     = (pc.weights.z <= 0.0)
                               || (abs(zPrev - zExpect) <= pc.weights.z * max(zExpect, 1.0));

            if (zOK) {
                // n/(n+1) while the pixel is young, alpha once it has enough
                // samples for the exponential window to be the tighter of the two.
                // ⚠ min(), not a branch on the count: at the handover frame the two
                // are equal, so the weight is continuous and no pixel ever jumps.
                // Same texel the depth was tested at — a count and the surface it
                // describes must not come from two different places.
                const float n     = max(texelFetch(uAge, hp, 0).r, 1.0);
                // ⚠ A STRENGTH, NOT A SWITCH, and the reason is a real trade: at one
                // ray per pixel the gather's SHARPNESS came from the temporal
                // accumulation, so a young pixel taking its sample whole leaves the
                // 5x5 spatial blur carrying the whole denoise — flatter, and indoors
                // that costs LEVEL too, because the screen cache's bounce feeds on
                // this pass's own output and a truncated history truncates the
                // bounce with it. 1 = the full ramp, 0 = the shipped constant alpha.
                const float aEff  = mix(alpha, min(alpha, n / (n + 1.0)), pc.hist.x);

                const vec3 range = max(mx - mn, vec3(1e-3));
                const vec3 hist  = clamp(texture(uHist, prevUV).rgb, mn - range, mx + range);
                val = mix(val, hist, aEff);

                // Same clamp shape, per channel, but the SH's OWN alpha — see the
                // header. ⚠ The deviation is SIGNED and centred on zero, so a clamp
                // written against a "brightness" intuition (min 0, scale by the mean)
                // would quietly forbid half of its range; the only bound that means
                // anything here is the neighbourhood's own spread.
                const float alphaSH = mix(max(alpha, pc.zp.w),
                                          min(max(alpha, pc.zp.w), n / (n + 1.0)), pc.hist.x);
                if (shOn)
                    for (int c = 0; c < 3; ++c) {
                        const vec3 r = max(shMx[c] - shMn[c], vec3(1e-3));
                        const vec3 h = clamp(histSH(prevUV, c), shMn[c] - r, shMx[c] + r);
                        shVal[c] = mix(shVal[c], h, alphaSH);
                    }

                // Saturate at the handover point. Letting it run to thousands would
                // make no difference to aEff (min() already picks alpha) and would
                // only cost precision in the R32F it is stored in.
                ageOut = min(n + 1.0, max(pc.hist.y, 1.0));
            }
        }
    }

    // ⚠ALPHA IS THE VIEW DEPTH, not the hit distance any more. The receivers need
    // it to upsample this half-res image without smearing across silhouettes
    // (ddgi_receiver::gatherUpsample), and that is worth more than republishing a
    // number only the debug view read — which now takes the RAW hit distance
    // instead, straight from the buffer that produced it.
    imageStore(outResolved, px, vec4(val, z0));
    // ⚠ WRITTEN FOR EVERY TEXEL, EVERY FRAME, including the ones that took no
    // history. The image is a ping-pong, so a texel left unwritten would keep the
    // count from TWO frames ago and quietly claim confidence it does not have.
    imageStore(outAge, px, vec4(ageOut, 0.0, 0.0, 0.0));

    if (shOn) {
        const int H = int(pc.res.y);
        for (int c = 0; c < 3; ++c)
            imageStore(outSH, ivec2(px.x, px.y + c * H), vec4(shVal[c], 0.0));
    }
}

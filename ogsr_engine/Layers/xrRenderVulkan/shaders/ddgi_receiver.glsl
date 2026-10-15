#ifndef DDGI_RECEIVER_GLSL
#define DDGI_RECEIVER_GLSL
// xrRenderVulkan — the DDGI probe field as seen by a SURFACE. One place, so the
// three world receivers (lmap / vlit / terrain) cannot answer the same question
// three different ways.
//
// WHY THE FIELD REPLACES THE SKY FILL RATHER THAN ADDING TO IT
// ------------------------------------------------------------
// A probe's irradiance is the whole cosine integral of what arrives at that point
// — sky AND the light bounced off everything around it. The analytic term it sits
// next to, `skyAmbient(N) * bakeOcc`, is an estimate of the FIRST of those two.
// Adding the probe answer on top would therefore count the sky twice and blow the
// image out; the honest operation is a replacement, with r_ddgi_light as the blend
// so the two can be compared inside a single run (the only A/B this renderer
// accepts — weather and actor position drift between runs).
//
// The replacement is only calibrated because the probes integrate the SAME SH9 sky
// at the SAME scale (see ddgi_trace.comp.glsl). Without that, r_ddgi_light would be
// an exposure knob wearing a GI costume.
//
// WHAT THE BAKED LIGHTMAP STILL HAS TO SAY, AND WHERE IT NO LONGER DOES
// ---------------------------------------------------------------------
// It reaches this file as a SCALAR. world_frag_body collapses lm.rgb into one
// occlusion number (dot(lm.rgb, 1/3)) and throws the baked COLOUR away, so what
// can multiply the field here is baked SKY ACCESS and never baked light.
//
// The reason this multiplier used to be global was the tree crowns: with
// r_rt_src 0 the BVH is cut from level.cform, foliage carries no collision, and a
// probe under a canopy therefore saw open sky and reported a forest floor lit like
// a meadow. THAT REASON IS GONE. r_rt_src defaults to the render geometry and the
// probe rays trace with cull mask 0xFF, which includes the cutout structure that
// IS the forest, so the probes see the crowns themselves.
//
// It is gone in the WIRING too, and that is the part to know before reading the
// multiply below as live: vk_env_light zeroes ddgi_params.y outright whenever the
// world cache measures sky visibility (r_wrc > 0 && r_wrc_skyvis > 0, and both are
// the shipping defaults), because a measured occlusion and a baked one are the
// same quantity counted twice. So at r_gi 3 this term is ALREADY off, and
// r_ddgi_light_bake changes nothing unless the cache is down.
//
// NOTHING LEAKS WHEN IT IS OFF, and the reason is the mix at the end of
// ddgiAmbient rather than anything here: outside the volume k goes to zero and the
// answer falls back to skyFill, which world_frag_body built WITH bakeOcc already
// folded in. The baked scalar therefore still occludes exactly the term it should
// - the analytic sky standing in for the field - and stands down only where the
// field itself is the answer. The crossfade between those two is the k the mix
// already uses; it does not need a second one here.
//
// #include AFTER light_ubo.glsl (needs ENV_SET and L.ddgi_params).

// THE FINAL GATHER SITS IN THE SAME PLACE, AS A SOURCE AND NOT AS A LAYER
// ------------------------------------------------------------------------
// vk_pass_gather traces one ray per screen pixel and resolves the SAME quantity
// this file looks up from the lattice: the cosine-weighted mean radiance arriving
// at the surface. So it is a REPLACEMENT for the lookup, blended by
// L.ddgi_params.z, not something added on top — adding the two would count every
// bounce twice. What it buys is per-pixel visibility: contact darkening and colour
// bleeding that eight probes on a 3 m grid can never resolve, however good their
// values are.
//
// Screen-space, so it is sampled with the same UV rule as the half-res AO
// (gl_FragCoord * L.ao_params.xy) and bilinearly upsampled. ⚠ Every texel of it is
// a valid ambient answer — beyond the ray budget's reach the gather pass writes the
// plain probe lookup itself — so there is no validity mask to carry here.
#define DDGI_SET      ENV_SET
#define DDGI_B_CONSTS 34
#define DDGI_B_IRR    35
#define DDGI_B_DEP    36
#include "ddgi_field.glsl"   // ddgiFieldWeighted — the part a VERTEX shader can call too

layout(set = ENV_SET, binding = 37) uniform sampler2D uGather;   // resolved final gather (rgb + view depth in a), half res
// The gather's 2-BAND SH (r_gather_sh): three tiles stacked vertically (R, G, B
// channel), each texel xyz = that channel's L1 deviation vector. Same resolution
// and same texel grid as binding 37 — they are two halves of one estimate, and
// the receiver reads them with one set of weights.
layout(set = ENV_SET, binding = 38) uniform sampler2D uGatherSH;

// ---- BILATERAL UPSAMPLE OF THE HALF-RES GATHER -----------------------------
// idTech 8 upsamples its gather with four Poisson taps weighted by normal and
// depth. This renderer used ONE bilinear tap — the cheapest possible answer, and
// also the one that smears the gather straight across every depth discontinuity.
// At half resolution that turns per-pixel gather noise into soft two-pixel blobs
// and leaks light around silhouettes; indoors, where GI is the ONLY light in the
// frame, the result reads as dirt rather than as blur.
//
// What this does instead: the SAME four texels bilinear would have blended, each
// weighted by how well its view depth agrees with this pixel's. Where the depths
// agree the weights collapse back to the bilinear ones, so a flat wall comes out
// as it did before — the difference appears exactly at the edges, which is where
// the artefact lived.
//
// ⚠The agreement is read from the gather's ALPHA, which gather_resolve fills with
// the view depth of the texel. ⚠L.gather_params.x (r_gather_upsample) is 0 both
// when the user asks for the old path and whenever no gather image is bound.
vec3 gatherUpsample(vec2 fragXY, float zView)
{
    const vec2 uv = fragXY * L.ao_params.xy;
    if (L.gather_params.x < 0.5) return textureLod(uGather, uv, 0.0).rgb;

    const vec2  ts = vec2(textureSize(uGather, 0));
    const vec2  t  = uv * ts - 0.5;
    const vec2  f  = fract(t);
    const ivec2 b  = ivec2(floor(t));
    const ivec2 hi = ivec2(ts) - 1;

    vec3  sum  = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 4; ++i) {
        const ivec2 o  = ivec2(i & 1, i >> 1);
        const vec4  s  = texelFetch(uGather, clamp(b + o, ivec2(0), hi), 0);
        const float bw = (o.x == 0 ? 1.0 - f.x : f.x) * (o.y == 0 ? 1.0 - f.y : f.y);
        // Same kernel shape the resolve's own spatial filter uses, so the two
        // stages agree on what "the same surface" means.
        const float dw = exp(-abs(s.a - zView) * 16.0 / max(zView, 0.1));
        const float w  = bw * dw;
        sum  += s.rgb * w;
        wsum += w;
    }
    // Every tap disagreed — a thin silhouette with nothing behind it at this depth.
    // Fall back to plain bilinear rather than to black.
    return (wsum > 1e-4) ? sum / wsum : textureLod(uGather, uv, 0.0).rgb;
}

// ---- THE HALF-RES GATHER AS A PER-PIXEL IRRADIANCE PROBE (r_gather_sh) ------
// ⛔PARKED 2026-08-23, gather_params.y is 0 by default and this function is not
// reached — the directional term looked worse than its absence, because at one ray
// per pixel the L1 it reads is noise-dominated. Kept intact on purpose. The verdict
// and the way out are in the PARKED banner in vk_console_min.cpp.
// The upsample above answers with ONE colour, which is the answer for the flat
// surface the depth buffer holds. Every bump the normal map carves into that
// surface therefore receives identical ambient — and revealing exactly that kind
// of detail is what an ambient term is for. idTech 8 keeps a 2-band SH per pixel
// so the receiver can re-evaluate the gather for the normal it actually shades.
//
// gather.comp stores L1 as a DEVIATION vector per channel, built perpendicular to
// the normal it sampled with, so:
//   * dot(d, N_gather) == 0  ⇒  this returns L0 EXACTLY where the shading normal
//     equals the geometric one. Turning r_gather_sh on cannot change the mean of
//     the frame; it can only move light between normal-mapped facets. That is the
//     property to MEASURE when checking the wiring (a shift in the mean = a bug).
//   * everything above it — the bilateral weights, the fallback, the blend in
//     ddgiAmbient — is untouched, because L0 is still L0.
//
// ⚠ THE LIMITER IS NOT COSMETIC. |d| > L0 would reconstruct NEGATIVE irradiance on
// the far side of the pixel's own probe, and a max(0) there rectifies noise into
// energy (the mean of a clipped zero-mean signal is not zero) — the one way this
// term could brighten the frame. Scaling d down instead keeps L0 - |d| >= 0 by
// construction, so no clamp is needed on the result at all for k <= 1.
vec3 gatherUpsampleSH(vec2 fragXY, float zView, vec3 N)
{
    const vec2  uv = fragXY * L.ao_params.xy;
    const vec2  ts = vec2(textureSize(uGather, 0));
    const int   H  = int(ts.y);          // one tile = one colour channel
    const vec2  t  = uv * ts - 0.5;
    const vec2  f  = fract(t);
    const ivec2 b  = ivec2(floor(t));
    const ivec2 hi = ivec2(ts) - 1;

    vec3  sum  = vec3(0.0);
    float wsum = 0.0;
    mat3  dsum = mat3(0.0);              // column c = channel c's deviation, weighted
    for (int i = 0; i < 4; ++i) {
        const ivec2 o = ivec2(i & 1, i >> 1);
        const ivec2 p = clamp(b + o, ivec2(0), hi);
        const vec4  s = texelFetch(uGather, p, 0);
        const float bw = (o.x == 0 ? 1.0 - f.x : f.x) * (o.y == 0 ? 1.0 - f.y : f.y);
        // r_gather_upsample still decides whether the depth agreement is consulted;
        // with it off these collapse to the plain bilinear weights, for the SH too.
        const float dw = (L.gather_params.x < 0.5) ? 1.0
                                                   : exp(-abs(s.a - zView) * 16.0 / max(zView, 0.1));
        const float w  = bw * dw;
        sum  += s.rgb * w;
        wsum += w;
        for (int c = 0; c < 3; ++c)
            dsum[c] += texelFetch(uGatherSH, ivec2(p.x, p.y + c * H), 0).xyz * w;
    }
    if (wsum <= 1e-4) return textureLod(uGather, uv, 0.0).rgb;   // as above: bilinear, not black

    const vec3 L0 = sum / wsum;
    vec3 e;
    for (int c = 0; c < 3; ++c) {
        vec3 d = dsum[c] / wsum;
        const float len = length(d);
        if (len > L0[c]) d *= L0[c] / max(len, 1e-6);   // the limiter — see above
        e[c] = L0[c] + L.gather_params.z * dot(d, N);
    }
    return max(e, vec3(0.0));   // only reachable with the exaggeration knob past 1
}

// `skyFill`  — what the analytic path produced for this surface, sky-access scalar
//              and L.sky_params.y already folded in, AO not yet applied.
// `bakeSky`  — that same sky-access scalar on its own (baked lightmap x dynHemi).
// `V`        — unit vector from the surface TOWARDS the eye.
vec3 ddgiAmbient(vec3 skyFill, vec3 P, vec3 N, vec3 V, float bakeSky)
{
    float k = L.ddgi_params.x;
    if (k <= 0.0) return skyFill;
    // Outside the volume the lookup returns black by design, so the fade is what
    // keeps the world beyond ~96 m from going dark and the volume boundary from
    // sweeping across the frame as the player walks. It also buys the early-out
    // that keeps distant pixels from paying for 16 atlas taps.
    k *= ddgiVolumeFade(P);
    if (k <= 0.0) return skyFill;

    vec3 gi = ddgiSampleIrradiance(P, N, V);
    // The gather answers the same question per pixel; blend, never add.
    // ⚠ gl_FragCoord makes this fragment-only — every receiver of this file is a
    // fragment shader, and ddgi_params.z is 0 whenever the image does not exist.
    if (L.ddgi_params.z > 0.0) {
        const float zv = dot(P - L.eye_pos.xyz, L.cam_dir.xyz);
        // ⚠ N here is the shading normal the receiver hands in — normal-mapped and
        // POM-perturbed. That is the entire point of the SH path: the flat answer
        // is what the pixel already had.
        const vec3 g = (L.gather_params.y > 0.5) ? gatherUpsampleSH(gl_FragCoord.xy, zv, N)
                                                 : gatherUpsample(gl_FragCoord.xy, zv);
        gi = mix(gi, g, L.ddgi_params.z);
    }
    gi *= mix(1.0, bakeSky, L.ddgi_params.y);
    return mix(skyFill, gi, k);
}

// How much of ddgiAmbient()'s answer is the TRACED term (field/gather) rather than
// the analytic sky: the same k x fade the blend above uses. The receiver's AO is
// weighted by it — the traced term carries its own visibility (24 m gather rays,
// probe relocation), so multiplying it by the full screen/RT AO counts the same
// occlusion twice, and a corner under a per-pixel gather went black. What the AO
// still owes the traced term is the CONTACT range the rays cannot resolve, which
// is what r_ao_gi (gather_params.w) leaves in: 1 = the old full product, 0 = none.
float ddgiGiShare(vec3 P)
{
    float k = L.ddgi_params.x;
    if (k <= 0.0) return 0.0;
    return clamp(k * ddgiVolumeFade(P), 0.0, 1.0);
}
vec3 ddgiAmbientAO(vec3 occ, vec3 P)
{
    return mix(occ, mix(vec3(1.0), occ, L.gather_params.w), ddgiGiShare(P));
}

// The same answer for a surface whose FIELD LOOKUP ALREADY HAPPENED — in a vertex
// shader, via ddgiFieldWeighted (see ddgi_field.glsl for why foliage must do that).
// What is left here is only what is genuinely per-pixel: the gather blend (ONE tap,
// not sixteen) and the baked scalar.
//
// ⚠ `field` MUST be the interpolated ddgiFieldWeighted() output, w and all. Its w
// carries the volume fade, so a caller that drops it gets a hard volume boundary
// sweeping across the frame instead of the crossfade the lattice is built for.
//
// `zv` — this fragment's VIEW depth, dot(wpos - eye, camFwd). The callers all have
// a world position (vWPos), so the "no depth to weight by" that kept this variant
// on a raw bilinear tap was never true of the fragment, only of the vertex-side
// lookup it accompanies. It is the same quantity ddgiAmbient derives for the world.
vec3 ddgiAmbientFromField(vec3 skyFill, vec4 field, float bakeSky, float zv)
{
    if (field.w <= 0.0) return skyFill;
    vec3 gi = field.rgb;
    // THE BILATERAL, not the raw tap it used to be. Foliage is the receiver that
    // needs it MOST: a canopy is a screen full of depth discontinuities at quarter
    // resolution, so a plain bilinear tap reads gather texels belonging to whatever
    // stands metres behind the leaf — the sky, the ground, a wall — and that
    // mismatch changes every frame as the jitter and the wind move which texel wins.
    // ⚠ Costs nothing new when it cannot help: gatherUpsample falls back to exactly
    // this bilinear tap when every candidate disagrees on depth, and collapses to it
    // outright under r_gather_upsample 0 — which is therefore still the A/B.
    if (L.ddgi_params.z > 0.0)
        gi = mix(gi, gatherUpsample(gl_FragCoord.xy, zv), L.ddgi_params.z);
    gi *= mix(1.0, bakeSky, L.ddgi_params.y);
    return mix(skyFill, gi, field.w);
}

#endif // DDGI_RECEIVER_GLSL

#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require   // the RT diffuse array is indexed per invocation
#extension GL_GOOGLE_include_directive : require
// ===========================================================================
//  xrRenderVulkan — FINAL GATHER (step 4 of the RT arc), after idTech 8.
// ===========================================================================
//  A probe grid answers "what light arrives around here" on a 3 m lattice. That
//  is enough to make a room glow and nowhere near enough to make it look SHARP:
//  the wall next to a red crate gets the same low-frequency answer as the wall
//  two metres further along, and every contact between surfaces is missing.
//
//  idTech 8's answer, and this pass, is to give EVERY PIXEL its own gather ray
//  and to keep the probe field as the FALLBACK the ray reads at its hit point.
//  Two properties make that affordable, and both are the opposite of what a
//  naive "trace GI per pixel" would do:
//
//   1. THE RAY DOES NOT SHADE ANYTHING. No sun ray, no light loop, no shadow
//      map — the hit only LOOKS UP radiance that something else already
//      computed. Here that is the DDGI field (ddgiSampleIrradiance), which the
//      probe pass filled with sun + sky + dynamic lights + the previous bounce.
//      idTech 8 queries a hashed world radiance cache first and the volumes only
//      as the last resort; we have no such cache yet, so the volume IS the cache.
//      A gather ray therefore costs traversal + one texture lookup, not a second
//      lighting pass.
//
//   2. THE RESULT IS LOW-FREQUENCY BUT THE VISIBILITY IS NOT. What the ray adds
//      over a plain probe lookup is not a better radiance value — it is the fact
//      that THIS pixel's hemisphere was tested from THIS point. Occlusion,
//      contact darkening and colour bleeding come out sharp even though every
//      radiance value in the sum is a smooth probe answer.
//
//  ONE RAY PER PIXEL IS NOISE, BY CONSTRUCTION. This pass produces a noisy
//  estimate on purpose and hands it to gather_resolve.comp (spatial bilateral +
//  motion-vector temporal). Judging the raw output on its own is judging half a
//  pass — that is why r_gather_debug shows the RESOLVED image by default.
//
//  THE UNITS ARE THE PROBE FIELD'S UNITS. ddgi_blend_irr stores the COSINE-
//  WEIGHTED MEAN RADIANCE (no 1/pi — this engine's direct lighting carries no
//  radiometric normalisation either), so a cosine-weighted MC estimator's plain
//  mean is that same quantity. That is what lets ddgiAmbient swap one for the
//  other with a blend factor instead of an exposure knob.
//
//  ONE COLOUR PER PIXEL IS STILL A FLAT ANSWER (r_gather_sh) — ⛔PARKED, OFF BY
//  DEFAULT. At one ray per pixel this channel carries more noise than direction
//  (|d| / L0 median 0.65) and looked worse than not having it. Kept whole: the
//  estimator is correct, the fix is the filter. See the PARKED banner in
//  vk_console_min.cpp.
//  --------------------------------------------------------
//  A single RGB value answers "how much light arrives on THE SURFACE THE DEPTH
//  BUFFER SHOWS", and that surface is flat. Every bump the normal map carves
//  into it then receives exactly the same ambient — the detail an ambient term
//  exists to reveal is the detail this representation cannot hold. idTech 8
//  stores its gather as a 2-BAND SH per pixel (3 x RGBA16F) so that each pixel
//  becomes its own little irradiance probe and the receiver can re-evaluate it
//  for the normal it actually shades with.
//
//  The same quantity, in two parts:
//    L0 — the cosine-weighted mean radiance, unchanged, in outGather.rgb;
//    L1 — a DEVIATION VECTOR per colour channel, in outSH (three tiles stacked
//         vertically: R, G, B), which the receiver adds as dot(d, n).
//
//  ⭐⭐ THE DEVIATION IS PERPENDICULAR TO N BY CONSTRUCTION, and that is the
//  safety property of the whole representation: evaluated with the gather's OWN
//  normal it contributes exactly zero, so L0 comes back untouched. Turning
//  r_gather_sh on therefore CANNOT change the mean of the frame — it can only
//  redistribute light across normal-mapped detail. A measured shift in the mean
//  means the wiring is wrong, which makes this a falsifiable claim instead of an
//  impression.
//
//  The estimator, with directions drawn from the cosine lobe about N:
//      a0 = mean(L)            <- this is L0, and E[a0] is the answer for N
//      a1 = mean(L * omega)
//  For a linear (2-band) radiance field L = p + q.omega the perpendicular part
//  of a1 recovers the gradient EXACTLY at d = (8/3)(a1 - (a1.N) N), giving back
//  R(n) = L0 + dot(d, n). (a1's component ALONG N is dropped on purpose: the
//  cosine lobe barely separates p from q.N there, and recovering it takes a
//  factor of 18 — an amplifier for noise and nothing else.)
//  ⚠ For an ISOTROPIC field d is zero only IN EXPECTATION: a single ray always
//  reports some direction. So this channel is pure noise wherever the light is
//  flat, and it leans on the same denoiser and the same temporal filter the
//  radiance does.
// ===========================================================================

#define RT_B_TLAS   0
#define RT_B_VERTS  1
#define RT_B_MATS   2
#define RT_B_ALBEDO 3
#define RT_TEX_SET  1     // vk_rt_world: the world diffuse array (per-texel albedo + cutout alpha)
// r_gather_atest: this pass owns the ray-cost profiler, so it is the one that
// compiles the alpha-test modes. The probe update and the world cache include the
// same file and stay bit-identical until a mode is worth shipping to them too.
#define RT_ATEST_DYN
#include "rt_shade.glsl"

#define DDGI_SET      0
#define DDGI_B_CONSTS 4
#define DDGI_B_IRR    5
#define DDGI_B_DEP    6
#define DDGI_SAMPLE
#include "ddgi_common.glsl"
#define DDGI_B_SKYSH   7
#define DDGI_B_SKYCUBE 8
#include "ddgi_sky.glsl"   // the SAME sky the probes integrate — see the header there

layout(set = 0, binding = 9)  uniform sampler2D uDepth;    // full-res prepass depth (D32), NEAREST
layout(set = 0, binding = 10) uniform sampler2D uNormal;   // normal G-buffer (rgb = N*0.5+0.5, a = source tag)
layout(rgba16f, set = 0, binding = 11) uniform writeonly image2D outGather;

// ---- SCREEN-SPACE RADIANCE CACHE (r_gather_scache) -------------------------
// Cache #1 in idTech 8's lookup chain, and the one this pass did not have: before
// falling back to the probe lattice, ASK THE PREVIOUS FRAME. If the hit point was
// on screen last frame and nothing moved in front of it, that pixel already holds
// the fully lit outgoing radiance of exactly this surface — sun, its shadow, the
// dynamic lights, wetness and all — at PIXEL resolution instead of the lattice's
// 3 m. It costs one reprojection and two taps, and it is what turns "the crate is
// somewhere in a lit region" into "the crate is lit HERE, by this much".
layout(set = 0, binding = 12) uniform sampler2D uPrevColor;   // half-res lit HDR of the PREVIOUS frame (the SSIL history buffer)
layout(set = 0, binding = 13) uniform sampler2D uPrevDepth;   // half-res linear view Z of that SAME frame, written by this pass below
layout(r32f, set = 0, binding = 14) uniform writeonly image2D outDepth;   // this frame's linear view Z -> next frame's uPrevDepth

// ---- WORLD RADIANCE CACHE (r_wrc) — cache #2 of the chain -------------------
// The middle link, and the one that was missing: a hash of 25 cm world cells
// holding the SHADED radiance of the patch. Twelve times finer than the probe
// lattice it stands in front of, and — unlike the screen cache above — valid
// behind the camera, inside a room the player is not looking at, and everywhere
// else a gather ray goes that the frame does not show.
#define WRC_SET      0
#define WRC_B_CONSTS 15
#define WRC_B_CELLS  16
#define WRC_B_LIST   17
#define WRC_B_CTRL   18
#include "wrc_common.glsl"

// The 2-band SH's L1, three tiles stacked vertically (R, G, B), each texel xyz =
// the deviation vector of that colour channel. ⭐ ONE image and one binding
// instead of three: a plain 2D target also keeps every placeholder view in the
// engine usable while r_gather_sh is off, which an array view would not be.
layout(rgba16f, set = 0, binding = 19) uniform writeonly image2D outSH;

// ---- BLUE NOISE (r_gather_bn) ----------------------------------------------
// 64x64 two-channel void-and-cluster tile: R drives the azimuth, G the elevation.
// Read with texelFetch and an explicit mask rather than a filtered sample, so no
// sampler state can affect it and the tile wraps exactly at its own period.
layout(set = 0, binding = 20) uniform sampler2D uBlueNoise;

layout(push_constant) uniform PC {
    vec4 camDir;     // xyz = camera forward (unit); w = gather ray length (m)
    vec4 camRightT;  // xyz = right * tan(fovX/2), w = tan(fovX/2)
    vec4 camTopT;    // xyz = top   * tan(fovY/2), w = tan(fovY/2)
    vec4 zp;         // x = proj _33, y = proj _43, z = rays per pixel, w = normal-offset bias (m)
    vec4 res;        // xy = gather target size, zw = 1 / gather target size
    vec4 camPos;     // xyz = camera world position, w = view-distance cutoff (m)
    vec4 params;     // x = per-frame jitter phase [0,1), y = TLAS cull mask, z = screen-cache weight, w = its relative depth tolerance
    mat4 prevVP;     // the PREVIOUS frame's view-proj (row-major Fmatrix, GLSL reads it as the transpose) — the matrix that rendered uPrevColor/uPrevDepth
    vec4 sh;         // x = write the 2-band SH (r_gather_sh); y = blue noise on (r_gather_bn); z = suppress the screen cache by enclosedness (r_gather_scache_indoor); w = k of the world-cache ceiling on the probe fallback (r_gather_field_clamp, 0 = off)
    vec4 atest;      // the cutout alpha-test mode — goes straight into rtAtest, see rt_shade.glsl
} pc;

// EVERY exit from main() writes the SH, including the ones that write a plain
// probe lookup: an isotropic answer has NO deviation, and leaving last frame's
// there would print a directional term on sky and out-of-bubble texels that the
// radiance sitting next to it does not agree with.
void shStore(ivec2 px, vec3 dR, vec3 dG, vec3 dB)
{
    if (pc.sh.x < 0.5) return;
    const int H = int(pc.res.y);
    imageStore(outSH, ivec2(px.x, px.y),         vec4(dR, 0.0));
    imageStore(outSH, ivec2(px.x, px.y +     H), vec4(dG, 0.0));
    imageStore(outSH, ivec2(px.x, px.y + 2 * H), vec4(dB, 0.0));
}

#include "gbuffer_recon.glsl"   // fetchPos / reconNormal, shared with rtao.frag

const float PI = 3.14159265;

// FALLBACK PAIR (r_gather_bn 0), kept as the A/B against the blue-noise tile
// above. Interleaved-gradient noise (Jimenez) for the azimuth, a plain hash for the
// elevation. ONE noise source driving BOTH dimensions locks them together and
// prints IGN's diagonal gradient straight into the result — the corduroy
// hatching RTAO showed at 2 rays, and it is worse here because the signal is
// coloured. See rtao.frag for the same pair and the same reason.
float ign(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// van der Corput / radical inverse base 2 — stratifies the elevation across the
// ray budget so 2 rays cover two different elevations instead of one twice.
float radicalInverse(int i)
{
    uint b = uint(i);
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return float(b) * 2.3283064365386963e-10;
}

// The ambient answer WITHOUT tracing: the probe lookup this pass replaces. It is
// the fallback everywhere a ray cannot be trusted — beyond the BLAS bubble, and
// on the sky texels a bilinear tap at a silhouette will inevitably touch.
//
// WRITING IT INTO THE TEXTURE rather than signalling "invalid" is what keeps the
// receiver simple: every texel of this image is a usable ambient answer, so
// ddgiAmbient blends ONE value with one weight instead of carrying a validity
// mask through the denoiser, the upsample and three receivers.
vec3 probeFallback(vec3 P, vec3 N, vec3 V)
{
    const float f   = ddgiVolumeFade(P);
    const vec3  sky = ddgiSkyIrradiance(N) * ddgi.skyColor.w;
    if (f <= 0.0) return sky;
    return mix(sky, ddgiSampleIrradiance(P, N, V), f);
}

// r_gather_field_clamp (sh.w): the BAKED-HEMI energy ceiling on the fallback
// ambient, at the two call sites where it prints STRAIGHT to the pixel — past
// the trace cutoff (the bubble keeps it at 5-30 m, much nearer than
// r_gather_dist suggests) and the bubble-edge mix below.
//
// WHY THE BAKE. The field cannot represent enclosure past the fine cascades —
// a 4-8 m lattice has no probe inside a room, so distant interiors read
// "street" (measured 28-08: the same wall 0.081 from inside, 0.889 from 25 m).
// One full day of receiver-side repairs reconstructed "how enclosed is this
// surface" from runtime side-channels — radiance-cache cells (pos lottery,
// staircase holes, shot noise), cover rays, cascade disagreement — and each
// patch uncovered the next blind spot (the arc lives in the session notes:
// crossfade → cells → EMA → cross → median → no-cell up-ray). The level
// compiler already measured that number per-texel, offline, exactly: the baked
// hemi, which the statics normal prepass carries in the tag's alpha
// (world_normal.frag; r_ssao_static_normals must be on — it is the default).
//
//   cap = k × hemi × (full-sky irradiance),  k = r_gather_field_clamp (≈3:
//   the multi-bounce gain of an enclosure at the boosted albedo, the same
//   constant the WRC budget clamp derived)
//
// Deep interior wall (hemi ~0.05, close-up truth 0.014): cap ≈ 0.12 — dark at
// ANY distance, nothing breathes because the bake is static. Street wall
// (hemi 0.6+): cap ≥ 1.4 — never fires. A ceiling indoors (hemi ~0.03): dark —
// the case no runtime cache ever covered (rays do not land on distant
// down-faces; MISS 60/60 measured). ⚠ ON THE FINAL VALUE, SKY FILL INCLUDED:
// capping only the field moved the white windows from 50 m to 70 m — past the
// outermost cascade the sky fill was the leak.
//
// No bake under the pixel (dynamics, trees/grass/terrain write no normal tag):
// plain fallback. The runtime-cell ceilings that once stood here for that case
// were removed with the rest of the tower — their reach never justified their
// failure modes, and the surfaces without a bake are the ones that rarely read
// as "a lit room" anyway. 0 = off, the uncapped path bit-exact.
vec3 probeFallbackCapped(vec3 P, vec3 N, vec3 V, float bakedHemi)
{
    vec3 amb = probeFallback(P, N, V);
    if (pc.sh.w > 0.0 && bakedHemi >= 0.0) {
        const vec3  lw    = vec3(0.2126, 0.7152, 0.0722);
        const float skyUp = dot(ddgiSkyIrradiance(vec3(0.0, 1.0, 0.0)) * ddgi.skyColor.w, lw);
        const float cap   = pc.sh.w * max(bakedHemi, 0.02) * skyUp;
        const float lumA  = dot(amb, lw);
        if (lumA > cap) amb *= cap / max(lumA, 1e-6);
    }
    return amb;
}

// Reproject a hit into the PREVIOUS frame and read the lit colour it left there,
// if that pixel really shows this surface. Returns a CONFIDENCE (0 = miss).
//
// ⚠ THE DEPTH TEST IS THE WHOLE THING. Without it a hit behind a wall would
// cheerfully read whatever that wall painted last frame — the classic
// screen-space leak, and it looks exactly like light passing through geometry.
// `pclip.w` IS the previous camera's view Z (a D3D-style projection puts it
// there), so it compares directly against the linear Z this pass stored.
//
// ⚠ THE CONFIDENCE FADES AT THE SCREEN EDGE rather than switching off. A hard cut
// there crawls across the frame as the camera turns, and the temporal filter then
// smears that switch over a dozen frames instead of hiding it.
float scacheLookup(vec3 hitPos, out vec3 rad)
{
    rad = vec3(0.0);
    if (pc.params.z <= 0.0) return 0.0;

    const vec4 pclip = pc.prevVP * vec4(hitPos, 1.0);
    if (pclip.w <= 0.05) return 0.0;                                // behind the previous eye
    const vec2 pndc = pclip.xy / pclip.w;
    if (any(greaterThan(abs(pndc), vec2(1.0)))) return 0.0;         // off screen last frame

    const vec2  uvp   = vec2(pndc.x * 0.5 + 0.5, 0.5 - 0.5 * pndc.y);
    const float zPrev = texture(uPrevDepth, uvp).r;
    // RELATIVE tolerance: a fixed metric one is either too tight at 60 m or too
    // loose at 2 m, and this test runs at both ends of the bubble in one frame.
    if (abs(zPrev - pclip.w) > pc.params.w * pclip.w) return 0.0;   // occluded, or disoccluded

    // PREFILTERED (r_gather_scache_blur, ddgi.edge.z). A rendered pixel carries
    // specular, sun glints and texture detail; a DIFFUSE bounce has no use for any
    // of it, and at one ray per pixel that detail arrives as variance instead —
    // two neighbouring rays landing on a lit and a shaded leaf disagree by tens of
    // times. Lumen prefilters its screen-space radiance cache for this same reason.
    // s_prevColor is already a half-res blit; this is the second filtering step.
    // ⚠ THE DEPTH TEST ABOVE STAYS ON THE CENTRE TAP. Widening it would let the
    // cache answer for a surface it never validated — the taps blur the RADIANCE,
    // they do not extend the cache's claim about what is visible.
    if (ddgi.edge.z > 0.0) {
        const vec2 t = vec2(1.0) / vec2(textureSize(uPrevColor, 0));
        rad = texture(uPrevColor, uvp).rgb * 0.5
            + (texture(uPrevColor, uvp + vec2( t.x,  t.y)).rgb
            +  texture(uPrevColor, uvp + vec2(-t.x,  t.y)).rgb
            +  texture(uPrevColor, uvp + vec2( t.x, -t.y)).rgb
            +  texture(uPrevColor, uvp + vec2(-t.x, -t.y)).rgb) * 0.125;
    } else {
        rad = texture(uPrevColor, uvp).rgb;
    }
    const vec2 edge = min(uvp, vec2(1.0) - uvp);
    return smoothstep(0.0, 0.06, min(edge.x, edge.y)) * pc.params.z;
}

layout(local_size_x = 8, local_size_y = 8) in;

void main()
{
    // GI-only reflectance scale (r_rt_albedo), shipped as an int x256 in the slot
    // that used to be reserved. Set before any trace: rtTraceClosest reads it.
    rtAlbedoBoost = float(ddgi.casc.w) * (1.0 / 256.0);
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(pc.res.x) || px.y >= int(pc.res.y)) return;
    const vec2 uv = (vec2(px) + 0.5) * pc.res.zw;

    // Sky / no prepass coverage. Nothing shades from this texel directly (the sky
    // is its own pass), but the bilinear upsample in the receiver WILL touch it
    // along every silhouette, so it has to hold a plausible ambient rather than
    // black — black there prints a dark rim around every roof edge.
    const float zndc = texture(uDepth, uv).r;
    if (zndc >= 0.9999) {
        // ⚠ THE SKY MUST STILL WRITE A Z. The next frame's cache test reads this
        // image everywhere, and a texel left over from two frames ago is the one
        // thing that turns a visibility guard into a lie. "Infinitely far" fails
        // every tolerance test, which is exactly the answer for a sky texel.
        imageStore(outDepth, px, vec4(1.0e6, 0.0, 0.0, 0.0));
        imageStore(outGather, px, vec4(ddgiSkyIrradiance(vec3(0.0, 1.0, 0.0)) * ddgi.skyColor.w, pc.camDir.w));   // "nothing was hit", not "hit at zero" — the alpha is a HIT DISTANCE and both the denoiser weight and the debug ramp read it as one
        shStore(px, vec3(0.0), vec3(0.0), vec3(0.0));
        return;
    }

    const vec4  C     = fetchPos(uv);
    const vec3  cPos  = C.xyz;
    const float zview = C.w;
    const vec3  viewV = -normalize(cPos);
    const vec3  N     = reconNormal(uv, C, viewV);

    // THIS frame's linear Z, for the NEXT frame's cache test — written HERE, before
    // every early-out below, so the image is always complete and always this frame's.
    imageStore(outDepth, px, vec4(zview, 0.0, 0.0, 0.0));

    // World-space ray origin, lifted off the surface: the reconstructed position
    // carries depth quantisation, and with r_rt_src 0 the BVH is collision
    // geometry that does not sit exactly on the rendered surface at all.
    const vec3 P = pc.camPos.xyz + cPos + N * (pc.zp.w + zview * 0.002);

    // The baked hemi under this pixel, decoded from the normal tag's alpha
    // (0.30 + hemi*0.45 for statics — see world_normal.frag). -1 = no bake
    // (skinned at 1.0, no-normal at 0.0): the fallback keeps its runtime chain.
    const float gbTag     = texture(uNormal, uv).a;
    const float bakedHemi = (gbTag > 0.27 && gbTag < 0.77)
                          ? clamp((gbTag - 0.30) / 0.45, 0.0, 1.0) : -1.0;

    // Past the cutoff the bubble no longer covers the geometry, so a miss would
    // mean "nothing built" and every ray would report open sky. Fall back to the
    // probe field, which is what this pixel would have used anyway.
    const float cutoff = pc.camPos.w;
    const float fade   = 1.0 - smoothstep(cutoff * 0.75, cutoff, zview);
    if (fade <= 0.001) {
        imageStore(outGather, px, vec4(probeFallbackCapped(P, N, viewV, bakedHemi), pc.camDir.w));
        shStore(px, vec3(0.0), vec3(0.0), vec3(0.0));   // the lattice answer has no direction of its own
        return;
    }

    // Tangent frame for the cosine-weighted hemisphere.
    const vec3 up = abs(N.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    const vec3 T  = normalize(cross(up, N));
    const vec3 B  = cross(N, T);

    // ADAPTIVE RAY COUNT (r_gather_rays_edge, ddgi.edge.w) --------------------
    //
    // Variance in a one-sample estimate is not spread evenly over the frame. It is
    // concentrated where the hemisphere is CUT BY THIN GEOMETRY — a wall seen past
    // a bush, a window frame, foliage against sky — because there the single ray is
    // a coin flip between "leaf, one metre" and "open sky", and no amount of
    // filtering downstream can invent the sample that was not taken. Everywhere
    // else one ray is genuinely enough, which is why paying for more rays GLOBALLY
    // (measured 28-08: +0.85 ms for rays 1 -> 3, fps 91 -> 83) buys quality almost
    // entirely in places that did not need it.
    //
    // ⭐ THE TEST IS THE SECOND DERIVATIVE OF DEPTH, NOT THE FIRST. A first
    // difference flags every SLOPED surface — a floor at a grazing angle has
    // neighbours at genuinely different depths, and that is the exact mistake the
    // resolve's own plane-aware weight exists to avoid. Curvature is ~0 on any
    // plane no matter how it is tilted, and large only where the surface actually
    // breaks. Four depth taps, from the image the pass already has bound.
    int rays = int(pc.zp.z);
    if (ddgi.edge.w > 0.0 && zview > 0.0) {
        const float zL = fetchPos(uv - vec2(pc.res.z, 0.0)).w;
        const float zR = fetchPos(uv + vec2(pc.res.z, 0.0)).w;
        const float zU = fetchPos(uv - vec2(0.0, pc.res.w)).w;
        const float zD = fetchPos(uv + vec2(0.0, pc.res.w)).w;
        const float curve = max(abs(zL + zR - 2.0 * zview), abs(zU + zD - 2.0 * zview)) / zview;
        if (curve > 0.02) rays = min(rays + int(ddgi.edge.w), 8);
    }
    const float tmax = pc.camDir.w;
    const uint  mask = uint(pc.params.y);
    rtAtest = pc.atest;   // r_gather_atest — set before the first trace of the pixel

    // Two independent offsets, one per sampling dimension, both advancing with the
    // per-frame phase so the temporal filter averages a genuinely new ray set.
    //
    // r_gather_bn: BOTH dimensions come from the blue-noise tile instead of IGN +
    // a hash. What changes is not accuracy - one ray is one ray - but WHERE the
    // residual error sits in frequency, and that is the whole game: every filter
    // downstream (the 5x5 bilateral in gather_resolve, the temporal EMA, TAA/DLSS)
    // removes HIGH frequencies. The old hash on ELEVATION was the worst possible
    // place for white noise, because elevation is the axis that decides whether the
    // ray escapes to sky or strikes geometry - nearly all of a 1-spp estimate's
    // variance - so its low-frequency clumps were simultaneously the largest error
    // and the one shape nothing later could remove.
    //
    // ⚠ THE PER-FRAME ADVANCE IS UNCHANGED, AND IT IS STILL THE RIGHT ONE. Adding a
    // constant mod 1 leaves the tile's spatial spectrum intact while making every
    // frame a fresh realisation - Wolfe, "Animating Noise For Integration Over
    // Time". Two different multipliers keep the dimensions from advancing in step.
    //
    // ⚠ 63 IS THE TILE SIZE MINUS ONE. A mask only because the generator emits a
    // power of two; it has to stay in step with kGatherBlueNoiseDim in the header
    // that produced the tile.
    const vec2  bn  = texelFetch(uBlueNoise, px & ivec2(63), 0).rg;
    const bool  bnOn = pc.sh.y > 0.5;
    const float rot = fract((bnOn ? bn.x : ign(vec2(px)))    + pc.params.x);
    const float rad = fract((bnOn ? bn.y : hash12(vec2(px))) + pc.params.x * 0.7548776662);

    // INDOOR REFLECTANCE COMPENSATION (r_gi_indoor) - see rtIndoorUp in rt_shade.
    // The gather owns the LAST bounce (its hit albedo multiplies the field it reads),
    // so leaving it out would correct every bounce but the one the eye actually sees.
    // One opaque ray per pixel, jittered by the same per-pixel phase the gather ray
    // uses, so the half-res estimate is spatially decorrelated and the denoiser eats
    // what is left exactly as it does the radiance.
    // The enclosedness is kept: it also gates the screen cache below. Computed
    // when either consumer wants it, so r_gi_indoor 1 with the cache gate on
    // still measures it.
    // edge2.z = r_gather_indoor_ray: this is a SECOND trace per gather pixel, and
    // the profiler needs to be able to price it. 0 = skip it and read indoorF as 0
    // (⚠ that CHANGES THE PICTURE — indoor boost and the screen-cache gate both
    // feed on it — so it is a measurement switch, not a quality knob).
    float indoorF = 0.0;
    if ((ddgi.indoor.x > 1.001 || pc.sh.z > 0.0) && ddgi.edge2.z > 0.0) {
        const vec2 j = vec2(fract(rot + 0.3), fract(rad + 0.7));
        indoorF = rtIndoorUp(P, ddgi.indoor.y, j);
        if (ddgi.indoor.x > 1.001) rtAlbedoBoost *= mix(1.0, ddgi.indoor.x, indoorF);
    }

    vec3  acc     = vec3(0.0);
    // The L1 moment, one column per colour channel: column c accumulates
    // radiance_c * omega. outerProduct(dir, rad3)[c] IS dir * rad3[c], so all
    // three channels ride in one matrix and cost one madd each.
    mat3  accD    = mat3(0.0);
    float hitDist = 0.0;

    for (int i = 0; i < rays; ++i) {
        const float phi = 2.0 * PI * ((float(i) + rot) / float(rays));
        const float u   = fract(radicalInverse(i) + rad);
        const float r   = sqrt(u);
        const vec3  dir = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - u)));

        const RTHit h = rtTraceClosestMasked(P, dir, 0.01, tmax, mask);

        // Hoisted out of the hit branch so the debug view below can paint a MISS
        // black. Leaving that to the branch is what made r_wrc_debug 2 unreadable
        // the first time: at one ray per pixel, half the frame was still sky
        // radiance, so "only the cache" came out as salt and pepper over the sky.
        vec3  wr   = vec3(0.0);
        uint  wkey = 0u;
        float ww   = 0.0;

        vec3 rad3;
        if (h.hit) {
            // ZERO SHADING — still the whole point. Neither source re-lights the
            // hit: one is a pixel something already shaded, the other is a field
            // the probe pass already filled with sun + sky + lights + last bounce.
            // THE CACHE CHAIN, cheapest and sharpest first: last frame's screen,
            // then the probe lattice. Both answer the SAME question — the radiance
            // leaving this hit towards us — so the confidence BLENDS them rather
            // than picking one, and a cache miss degrades to exactly the previous
            // behaviour instead of to a hole.
            // ⚠ uPrevColor is already albedo x lighting (it is a rendered pixel).
            // Multiplying it by h.albedo again would darken every cached hit by a
            // second albedo — the mistake this blend is written out to prevent.
            //
            // ⭐⭐ CACHE #1 IS ASKED FIRST, AND THAT IS NOT A STYLE CHOICE. It wins
            // wherever it answers with full confidence, so asking the world hash
            // there would walk a probe chain only to have the result thrown away —
            // and, far worse, would CLAIM A CELL that nothing will ever read and
            // that the shade pass will then pay to fill.
            //
            // Measured 22-08, first version: ~1M queries a frame against 512k
            // slots, 10-15% of them lost to full chains and the shade queue
            // overflowing every frame. Most of that table was surface the screen
            // already had. This cache's real job is the rest of the hemisphere —
            // behind the camera, round a corner, under a canopy — and the
            // screenWins counter is what keeps that claim a number.
            vec3 sc;
            float scw = scacheLookup(h.pos, sc);
            // ⚠⚠ THE SCREEN IS THE ONE CACHE IN THIS CHAIN THAT BYPASSES THE
            // REFLECTANCE COMPENSATION. A rendered pixel is raw-albedo x lighting;
            // the lattice and the world cache both carry albedo x rtAlbedoBoost
            // (r_rt_albedo x the indoor boost) — indoors they disagree by ~3x, and
            // because the cache answers for most of an interior hemisphere it drags
            // the gather down to the RASTER's dark equilibrium instead of the
            // boosted one: dark walls -> dark screen -> the cache feeds the
            // darkness back. 📏Measured 27-08 with r_gi_probe on the wall by the
            // door: scache 0 -> gather == lattice (0.041); scache 1 -> gather
            // 0.014 and the wall followed at ambient x albedo. So the cache is
            // weighted out by the SAME enclosedness that turns the compensation on
            // (r_gather_scache_indoor rides in sh.z; 0 = the old behaviour) —
            // outdoors indoorF is 0 and nothing changes.
            scw *= 1.0 - pc.sh.z * indoorF;

            // edge2.y = r_gather_wrc: the world cache's own switch INSIDE the gather
            // (r_wrc turns the cache off everywhere, which is a different question).
            // It exists so the ray-cost profiler can price this link of the chain
            // alone — a hash walk with linear probing runs on every hit the screen
            // did not answer for.
            if (scw < 0.99 && ddgi.edge2.y > 0.0) ww = wrcQuery(h.pos, h.nrm, h.albedo, wr, wkey);
            else if (scw >= 0.99 && wrc.ctl2.y != 0u) atomicAdd(wrcCtrl.screenWins, 1u);

            // The chain is APPLIED cheapest-and-bluntest first, each better answer
            // blending over the last, so it reads in the opposite order from the
            // priority list: lattice, then world cache, then screen.
            //
            // ⭐⭐⭐ AND IT IS EVALUATED LAZILY, WHICH THE ORDER ABOVE MADE LOOK
            // IMPOSSIBLE. The final value is mix(mix(lattice, cell, ww), sc, scw),
            // so the lattice enters with weight (1-scw)*(1-ww) — and where the
            // screen answered with full confidence that weight is EXACTLY ZERO
            // while the cost was not: probeFallback is ddgiSampleIrradiance, i.e.
            // a cascade walk with eight probes, trilinear weights and a Chebyshev
            // visibility test — tens of texture fetches, thrown away. The comment
            // three blocks up already states the rule ("asking the world hash there
            // would walk a probe chain only to have the result thrown away"); it
            // was enforced for the WORLD cache and not for the lattice.
            //
            // ⚠ BIT-EXACT WHERE IT MATTERS: the skipped branch is taken only when
            // the lattice's own weight rounds to nothing, and both diagnostics
            // below need the lattice for their comparison — so with the stats or a
            // debug view on, the old unconditional path runs unchanged.
            const bool wantLattice = ddgi.edge2.x <= 0.0
                                   || ((1.0 - scw) * (1.0 - ww) > 1e-4)
                                   || wrc.ctl2.y != 0u || wrc.ctl2.z != 0u;
            vec3 lattice = vec3(0.0);
            // The cells hold the radiance LEAVING the patch, in the same unscaled
            // units ddgi_trace produces (hence the skyColor.w here and on the sky
            // line below), so this replaces the whole `albedo x irradiance` product
            // rather than one factor of it.
            if (wantLattice) {
                lattice = h.albedo * probeFallback(h.pos, h.nrm, -dir);
                rad3 = lattice;
                if (ww  > 0.0) rad3 = mix(rad3, wr * ddgi.skyColor.w, ww);
                if (scw > 0.0) rad3 = mix(rad3, sc, scw);
            } else {
                rad3 = mix(wr * ddgi.skyColor.w, sc, scw);
            }

            // ⭐⭐⭐ MEASURE THE DISAGREEMENT, DO NOT LOOK FOR IT. How far the cache's
            // answer is from the lattice answer it displaced, relative to the
            // lattice — the one number that says whether this cache is worth its
            // 0.3 ms, and the one an eye cannot supply. See the ctrl block.
            if (ww > 0.0 && wrc.ctl2.y != 0u) {
                const vec3  cached = wr * ddgi.skyColor.w;
                const float rel    = length(cached - lattice) / max(length(lattice), 1e-4);
                atomicAdd(wrcCtrl.deltaSum, uint(min(rel, 16.0) * 1024.0));
                atomicAdd(wrcCtrl.deltaCount, 1u);
            }

            hitDist += h.dist;
        } else {
            // RADIANCE, not irradiance, and scaled by the same energy the probe
            // rays apply — a miss here has to be worth exactly what a probe's miss
            // was worth, or the swap in ddgiAmbient reads as an exposure change.
            rad3 = ddgiSkyRadiance(dir) * ddgi.skyColor.w;
            hitDist += tmax;
        }

        // r_wrc_debug, applied to HIT AND MISS alike so the view shows the cache
        // and nothing else: 1 = a colour per cell (cell size, LOD rings and
        // collision churn visible at once), 2 = only what the cache contributed,
        // 3 = where it DISAGREES with the lattice (green = agrees, red = replaced
        // the answer outright) — the view that answers "is this cache doing
        // anything" without waiting for the stats line.
        if (wrc.ctl2.z != 0u) {
            if (ww <= 0.0) {
                rad3 = vec3(0.0);
            } else if (wrc.ctl2.z == 1u) {
                rad3 = wrcDebugColor(wkey);
            } else if (wrc.ctl2.z == 2u) {
                rad3 = wr * ddgi.skyColor.w;
            } else {
                const vec3  latt = h.albedo * probeFallback(h.pos, h.nrm, -dir);
                const float rel  = length(wr * ddgi.skyColor.w - latt) / max(length(latt), 1e-4);
                rad3 = vec3(min(rel, 1.0), 1.0 - min(rel, 1.0), 0.0);
            }
        }

        // Clamp BEFORE the mean, like the probe trace: one bright outlier out of
        // one or two rays would otherwise dominate the pixel and then live on in
        // the temporal history for dozens of frames.
        //
        // ⭐ THE CEILING IS THE GATHER'S OWN (r_gather_clamp, ddgi.edge.y), NOT the
        // shared params.z. The shared one is sized for a 64-ray probe update; this
        // loop runs ONE ray by default, and the same number that politely trims a
        // 64-sample mean lets a single sample BE the pixel. It applies to the
        // blended rad3, so the screen cache — a rendered HDR pixel, tens of units
        // on a sunlit leaf, and the heaviest tail in the chain — is cut here too.
        // 0 = inherit params.z: the old behaviour, bit-exact.
        const float clampTo = ddgi.edge.y > 0.0 ? ddgi.edge.y : ddgi.params.z;
        const float m = max(max(rad3.r, rad3.g), rad3.b);
        if (m > clampTo) rad3 *= clampTo / m;

        acc += rad3;
        // ⚠ THE SAME rad3 THE MEAN GETS, clamp and all. Feeding the moment the
        // unclamped value would let one fireball steer the direction of a pixel
        // whose brightness the clamp has already refused to believe.
        accD += outerProduct(dir, rad3);
    }

    // Cosine-weighted sampling makes the plain mean the cosine-weighted mean
    // radiance — the probe atlas's own convention (see the header).
    vec3 irr = acc / float(rays);

    // At the bubble's edge slide back to the probe answer over the same band the
    // cutoff test uses, so the boundary does not sweep a visible seam across the
    // frame as the player walks.
    if (fade < 0.999) irr = mix(probeFallbackCapped(P, N, viewV, bakedHemi), irr, fade);

    // L1: strip the component along N (see the header — it is the part the cosine
    // lobe cannot measure), scale by 8/3 so that a linear field is reproduced
    // exactly, and fade it on the SAME band the radiance uses. The fade is not
    // decoration: the probe answer this crossfades into is isotropic, so the
    // deviation has to arrive at the boundary already at zero, or the seam the
    // crossfade exists to hide would show up in the direction instead.
    if (pc.sh.x >= 0.5) {
        const float k = (8.0 / 3.0) * fade / float(rays);
        vec3 d[3];
        for (int c = 0; c < 3; ++c) {
            const vec3 a1 = accD[c];
            d[c] = (a1 - dot(a1, N) * N) * k;
        }
        shStore(px, d[0], d[1], d[2]);
    }

    imageStore(outGather, px, vec4(irr, hitDist / float(rays)));
}

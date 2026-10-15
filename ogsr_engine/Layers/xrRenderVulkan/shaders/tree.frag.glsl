#version 450
#extension GL_GOOGLE_include_directive : require
#define VSM_SET 2                  // EnvLight set is bound at set 2 for trees
#define ENV_SET 2                  // EnvLight UBO + samplers live at set 2
#include "vsm_sample.glsl"         // vsmSunShadow (set 2 b14)
#include "light_ubo.glsl"          // DynLight + Lighting UBO (set 2 b0) + samplers (set 2 b1..13)
#include "foliage_shadow.glsl"     // foliageLightShadow — foliage receives spot/point pool shadows
#include "surface_class.glsl"      // SC_* classification + snow (foliage)
#include "shore_wet.glsl"
#include "ao_common.glsl"       // coloredAO (shared with env_common.glsl)          // shoreWetness — trunks and snags lying in water
#include "common_math.glsl"     // EnvBRDFApprox — pure, shared with world/grass
#include "ao_sampled.glsl"      // gtaoVisK / ssilBoost — after light_ubo.glsl (uAO/uIL/L)
#include "shadow_math.glsl"     // cascTap — set-agnostic bilinear PCF tap
// ⭐CROWNS READ THE PROBE FIELD (set 2, b34..37 — ENV_SET is 2 here, and
// ddgi_receiver derives DDGI_SET from it, so the same include lands on the right
// set with no edit). In a forest the canopy IS most of the frame: we already trace
// 1.27 M of the bubble's 1.28 M triangles THROUGH these leaves, while the leaves
// themselves were lit by a weather constant. Grass came first and moved the frame
// mean from 0.004 to 2.9 of 255; this is the bigger half of the same omission.
#include "ddgi_receiver.glsl"   // ddgiAmbient — the same replacement the world does

// xrRenderVulkan - tree forward fragment shader. set 1/binding 0 = per-group
// diffuse; trees are alpha-tested (punch-out leaves). set 2 = the shared per-frame
// EnvLight, now via light_ubo.glsl (was a hand-rolled partial copy). The sun term
// is rebuilt here from the push constant and the per-tree hemi factor, then given a
// single shadow tap; foliage noise hides the hard edge.
layout(set = 1, binding = 0) uniform sampler2D uDiffuse;

// GTAO - trees ARE in the prepass depth, so crowns/trunks get their own AO. HALF
// the strength exponent (alpha-tested leaf depth is noisy at half-res) — that is
// the gtaoVisK(0.5) at the shading site below. gtaoVisK/ssilBoost live in
// ao_sampled.glsl, cascTap in shadow_math.glsl (both included above).

// One cascade lookup; -1.0 when outside (1% UV inset). Single tap - foliage.
float cascSample1(sampler2D smap, mat4 vp, vec3 wp, float bias_)
{
    vec3 n = (vp * vec4(wp, 1.0)).xyz;          // ortho -> already NDC (w == 1)
    vec2 uv = n.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.01 || uv.x > 0.99 || uv.y < 0.01 || uv.y > 0.99
        || n.z <= 0.0 || n.z >= 1.0)
        return -1.0;
    return cascTap(smap, uv, n.z - bias_);
}

// Sky ambient (crowns sample straight up — no per-leaf normal) now comes from the
// shared header, so the canopy gets the SH9 irradiance the world does instead of a
// private point-sampled copy.
#include "sky_ambient.glsl"

// Sky specular sheen for the canopy (r_ibl). Leaves have no normal, so a real
// reflection is meaningless — instead give the crown a soft view-dependent sky
// sheen (up-reflected, high roughness = blurry), gated by per-tree sky openness
// (vHemi) and boosted when wet. Karis EnvBRDFApprox → common_math.glsl.
// ⚠wetF is passed IN, not looked up here. This function used to call shoreWet(wp)
// itself while main() called it again eight lines later — two independent lookups
// of the same answer at the same position, and shoreWet is up to five texture taps
// (one level-water + four hand-filtered tile taps). On a fill-bound pass that is
// the cheapest kind of waste there is: pure duplication, no picture attached.
vec3 canopySkySheen(vec3 wp, float openness, float wetF)
{
    if (L.ibl_params.x < 0.004) return vec3(0.0);
    const vec3 up = vec3(0.0, 1.0, 0.0);
    vec3  V   = normalize(L.eye_pos.xyz - wp);
    float NoV = clamp(dot(up, V), 0.0, 1.0);
    vec3  R   = reflect(-V, up);
    float rough = mix(0.7, 0.5, wetF);
    vec3  pref  = textureLod(uSkySpec, R, rough * L.ibl_params.z).rgb;
    // Subtle when dry, glistens when wet; × canopy openness so inner/shaded leaves stay matte.
    return pref * EnvBRDFApprox(vec3(0.04), rough, NoV)
         * (L.ibl_params.x * L.ibl_params.y * openness * (0.12 + 0.88 * wetF));
}

// dynLightsFoliage → foliage_shadow.glsl (shared with the other foliage pass).

// ⭐ THE WHOLE BLOCK, from the shared header — not a hand-copied PREFIX of it.
// This used to redeclare the first five fields locally, which is the exact shape
// of the push-constant bug this renderer has already paid for once (a vec2 in the
// middle of a block silently shifting every offset after it). The FS now reads
// pc.vSunColor too (the sun term, rebuilt per fragment instead of interpolated), so
// the copy would have had to grow — and a growing hand-copy of a std430 block is a
// mine with a timer on it.
#include "tree_gfx_push.glsl"   // TreeGfxPush (160 B) — matches VK::TreeGfxPush

// DISTANCE TIER for the subtle per-pixel terms — see r_tree_shade_dist in
// vk_console_min.cpp for why the FRAGMENT shader is the lever on this pass.
// Returns 1 near, fading to 0 over the last quarter of the band. The fade matters:
// a hard radius draws a ring through the forest that slides with the camera,
// whereas a fade only ever removes a term that was already almost gone.
float shadeTier(float dist)
{
    if (pc.shadeDist <= 0.0) return 1.0;                       // r_tree_shade_dist 0 = tier disabled
    return 1.0 - smoothstep(pc.shadeDist * 0.75, pc.shadeDist, dist);
}


// ---------------------------------------------------------------------------
// THE CROWN UBERSHADER IS SPLIT BY SPECIALIZATION, NOT BY COPYING THE FILE.
//
// r_tree_flat answered "is this pass shading-bound?" — 4.7 -> 1.4 ms on the 780M,
// so 3.4 ms IS shading work. r_tree_terms then priced the terms one at a time by
// folding a mask at PIPELINE-creation time. Both instruments agreed on the shape
// of the answer, so the mask stopped being a probe and became the shipping path:
// every frame the CPU asks the world what can possibly contribute (no snow, no
// rain, no dynamic light in reach, no debug view open) and binds the crown
// pipeline compiled with exactly those terms in it.
//
// ⭐ A spec constant, NOT a uniform branch, and that is the whole design. This
// pass costs per WAVE (a wave with one surviving lane executes the entire shader
// — see the tree ledger), so a runtime `if` that most lanes skip prices nothing
// and would report every term as free. A spec constant is folded at pipeline
// creation and the term is dead-code-eliminated for real: texture taps, ALU and
// the registers it held — and, because the driver compiles VS and FS as a pair,
// the vertex work that fed a varying the FS no longer reads goes with it.
//
// ⚠ SELF-CHECK, still valid and still the first thing to run: mask 4095 must land
// on the shipping number and mask 0 must land near r_tree_flat 1. If mask 0 does
// NOT collapse the zone, the driver did not fold the constant and NOTHING
// measured through this mask means anything.
//
// TERMQ(bit, expr, off) writes the term and its neutral value side by side, so a
// later edit to one cannot silently leave the other behind. Bits, the constant
// and the macros live in tree_variants.glsl (shared with any twin of this file).
// ---------------------------------------------------------------------------
#include "tree_variants.glsl"
// ---- INTERPOLANTS: 10 components, and every one of them earns its place -------
// This list used to be 16, and six of those were work the pass did on every
// fragment for nothing. On a canopy — the most overdrawn surface in the frame,
// costing per WAVE — an interpolant is not free: it is a VS output, a parameter
// slot, per-fragment interpolation, and a register held for the whole shader.
// The three that went, and why they could not have been right:
//   * vLight was a vec3 carrying ONE useful number in .x with .yz hard-coded to 0;
//   * vSunLit was pc.vSunColor.rgb * hemi * 0.88 — a push constant times that same
//     number, i.e. a value the fragment can rebuild in two multiplies;
//   * vTreeIdx fed the visible-tree bitset ONLY, and that instrument is gone (it
//     cost the pass its early-Z; see the tree ledger).
// ⭐ `flat` on the hemi factor is not a rounding shortcut: hemiFac comes from the
// INSTANCE record, so every vertex of a tree carries the identical value and
// interpolating it computed a weighted average of three copies of one number.
layout(location = 0) in vec2 vUV;
layout(location = 1) flat in float vHemi;   // per-tree baked sky openness (c_scale+c_bias)
layout(location = 2) in vec3 vWPos;
// The probe field, sampled per VERTEX (tree.vert) and interpolated here. See
// ddgi_field.glsl: per-fragment it cost this pass 2.2 -> 11.9 ms.
layout(location = 3) in vec4 vGI;
layout(location = 4) flat in float vLodFade;   // R1 crossfade: the billboard's share of this crown (tree.vert)
layout(location = 0) out vec4 outColor;

#include "lod_dither.glsl"

// 1-tap sun shadow (cascades first, then the far map) - see world_lmap.frag.
float sunShadow1(vec3 worldPos)
{
    float s = cascSample1(uShadowNear, L.sun_near_vp, worldPos, 0.0004);
    if (s >= 0.0) return s;
    s = cascSample1(uShadowC1, L.sun_c1_vp, worldPos, 0.0006);
    if (s >= 0.0) return s;

    vec4 c = L.sun_vp * vec4(worldPos, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 ndc = c.xyz / c.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    return (ndc.z - 0.0015 <= texture(uShadow, uv).r) ? 1.0 : 0.0;
}

void main()
{
    vec4 diff = texture(uDiffuse, vUV, L.spot_flash.z);   // DLSS mip bias (0 native)
    // Alpha stays UNBIASED: the sharper (biased) alpha mip shrinks the cutout
    // coverage at the alphaRef threshold → black holes onto the dark crown
    // interior at distance («чёрные пятна на листве» under DLSS upscaling).
    if (L.spot_flash.z != 0.0) diff.a = texture(uDiffuse, vUV).a;
    // (The r_tree_flat 2 twin — the flat probe plus one extra unbiased fetch — used
    // to sit here. It answered its question with a number: one crown texture fetch
    // over the whole pass is 0.00-0.01 ms, which closed the entire "make the fetch
    // cheaper" family. A probe that has already answered is just a shader variant
    // nobody binds.)
    if (diff.a < pc.alphaRef)
        discard;
    if (vLodFade > 0.0 && lodDither() < vLodFade)   // R1 crossfade: these pixels are the billboard's
        discard;

#ifdef TREE_FLAT
    // r_tree_flat probe (tree_flat.frag.spv, compile_all.bat :onedef). Everything
    // above this line — the diffuse fetch, the DLSS alpha refetch, the discard —
    // is identical to the shipping shader, and everything below it is gone. The
    // question this exists to answer: Trees/Mesh sits at ~4.7 ms and does not
    // respond to zwrite, alphaRef, or a 17% cut in surviving fragments — so is
    // the cost in THIS shader's shading work (then deferred texturing / VRS have
    // something to win), or is it the raster + alpha fetch floor (then no shading
    // architecture can touch it)? Flat green so a live probe cannot be mistaken
    // for a real frame.
    outColor = vec4(0.05, 0.30, 0.08, 1.0);
    return;
#endif

    // ⚠⚠⚠ A VISIBLE-TREE BITSET USED TO BE MARKED HERE WITH AN atomicOr, AND IT IS
    // GONE ON PURPOSE — read this before reinstating anything like it. A storage-buffer
    // side effect in a fragment shader is the classic way to lose EARLY DEPTH TESTING,
    // and a runtime gate does not save it: early-Z is decided when the PIPELINE is
    // compiled, and at that point the driver only sees that the shader CAN write.
    // Measured, same scene, same 890 k surviving samples: with the atomic present the
    // colour pass ran 20.1 M fragment invocations at 2.81 ms; without it, 6.2 M at 1.25.
    // The instrument cost more than most things it was used to hunt, so it was #ifdef'd
    // out — and then nothing wrote the buffer while the CPU kept copying it back and
    // popcounting it every profiled frame to print a zero. Both halves are deleted now.
    // ⭐ If the count is ever wanted again, the variant system this shader now has makes
    // it a ONE-BIT job (a TS_SEEN term built only when the diagnostic asks) — which is
    // exactly the "separate pipeline, provably no side effect in the shipping one"
    // the old comment demanded and a #define could never give.

    // ⚠ EVERY debug early-out below sits behind SPEC_TDEBUG, which is off in normal
    // play — a new one is DEAD CODE until its UBO field is listed in the CPU-side
    // TreeSpecMask(), and it will look like a broken cvar rather than an unlisted
    // one. Add the field with the view. (Same rule, same wording, as the world FS:
    // that shader has already been through this exact mistake.)
    // r_ssao_debug 1: trees show the APPLIED factor — half strength, as they shade.
    if (SPEC_TDEBUG && L.ao_params.w > 0.5) {
        outColor = vec4(vec3(gtaoVisK(0.5)), 1.0);
        return;
    }
    // r_sf_debug 5: surface classification - foliage (yellow).
    if (SPEC_TDEBUG && int(L.sf_params.y + 0.5) == 5) {
        outColor = vec4(SC_DebugColor(SC_Refine(SC_FOLIAGE, vec3(0.0, 1.0, 0.0))), 1.0);
        return;
    }

    // SNOW frosting on the crown (Surface Field consumer, r_snow). Foliage has no
    // per-leaf normal -> treat as up; the canopy is sky-exposed.
    float snow = TERMQ(T_SNOW, SC_SnowAmount(SC_Refine(SC_FOLIAGE, vec3(0.0, 1.0, 0.0)), vec3(0.0, 1.0, 0.0), 1.0) * clamp(L.sf_params.w, 0.0, 1.0), 0.0);
    diff.rgb = mix(diff.rgb, vec3(0.90, 0.93, 0.97), snow);

    // Rebuilt here instead of interpolated: the vertex shader used to emit exactly
    // this product as vSunLit, and both factors are constant across the draw
    // (pc.vSunColor is a push constant, vHemi comes from the instance record). Two
    // multiplies per fragment against three interpolants held for the whole shader.
    // ⚠ Keep in step with tree.vert's ×0.88 — that factor is the tuned crown level
    // (foliage ran ×1.1 against the world's ×1.25), not an arbitrary constant.
    vec3 sunPart = pc.vSunColor.rgb * vHemi * 0.88;
    if (dot(sunPart, sunPart) > 0.0) {
        // VSM screen-space mask (r_vsm) vs the 1-tap cascade - gated by shadow_params.w.
        // puddle_geo.w > 0.5: no sun shadow maps this frame (r_sun_shadows 0 only clears them) -> lit, without the read.
        float sunSh = TERMQ(T_SUN, (L.puddle_geo.w > 0.5) ? 1.0
                                 : (L.shadow_params.w > 0.5) ? vsmSunShadow(gl_FragCoord.xy * L.ao_params.xy)
                                                            : sunShadow1(vWPos), 1.0);
        sunPart *= sunSh;
    }

    // ---- Distance tier ----
    // camDist is needed by the fog below anyway, so the tier costs one compare.
    // `near` is a UNIFORM branch across a distant crown (every fragment of one tree
    // is at roughly one distance), so the skipped work is genuinely skipped rather
    // than executed under a mask by half the warp.
    float camDist = distance(vWPos, L.eye_pos.xyz);
    float tier    = shadeTier(camDist);
    bool  near_   = tier > 0.0031;   // below ~1/320 the term cannot move an 8-bit channel

    // Soaked is DARK — see detail.frag. Wet bark is markedly darker than dry, and
    // for a trunk lying in the water that boundary IS the effect. ONE lookup now,
    // shared with the sheen below; the debug view forces it at any range so
    // r_wtr_wet still shows the map rather than the tier.
    float sw = TERMQ(T_WET, (near_ || (SPEC_TDEBUG && shoreWetDebug())) ? shoreWet(vWPos) : 0.0, 0.0);
    if (SPEC_TDEBUG && shoreWetDebug()) { outColor = vec4(sw, sw * 0.3, 0.0, 1.0); return; }   // red = wet
    sw *= tier;

    // Ambient = sky-cube light x per-tree openness (vHemi). x0.75 keeps the old
    // grass:tree ambient ratio. L.ambient = real env ambient (carries r_ambient_floor).
    // SSIL is a screen-space BOUNCE, i.e. a multiplier that trends to 1 — so the
    // tier fades it toward 1, not toward 0.
    vec3 ssil    = TERMQ(T_SSIL, near_ ? mix(vec3(1.0), ssilBoost(), tier) : vec3(1.0), vec3(1.0));
    // ⚠ THE 0.75 MUST SCALE THE FIELD TOO, OR r_ddgi_light BECOMES AN EXPOSURE KNOB
    // FOR THE CANOPY. That factor is this shader's historical grass:tree ambient
    // ratio, and it was baked into the analytic term the field REPLACES — so it is
    // applied to the replacement, outside the call, rather than to only one of the
    // two things ddgiAmbient blends. Same rule as everywhere in this arc: the swap
    // must not move the exposure, only the distribution.
    // ⚠ A crown card has no usable normal either (see detail.frag): sampled UP, the
    // direction every other ambient term in this shader already answers for.
    // `vHemi` is the per-tree baked openness — the canopy's bakeSky.
    const float kTreeAmb = 0.75;
#ifdef TREE_SKY_PUSH
    // nfs18 (Switch): the up-facing sky irradiance is ONE value per frame -- the CPU evaluates the same SH9 closed form
    // (EnvLight::SkyAmbientUpCPU) and pushes it in pc.vHemiColor; before the first projection it pushes the env hemi
    // colour (R1's L_hemi_color). nfs17 priced the per-pixel skyAmbientUp() at ~1.2 ms of the crown FS.
    const vec3  skyFillT = TERMQ(T_SKY, pc.vHemiColor.rgb, vec3(1.0)) * (vHemi * L.sky_params.y);
#else
    const vec3  skyFillT = TERMQ(T_SKY, skyAmbientUp(), vec3(1.0)) * (vHemi * L.sky_params.y);
#endif
#ifdef TREE_NO_GATHER
    // r_tree_gather 0 (tree_nogather.frag.spv) — ddgiAmbientFromField WITHOUT its
    // screen-space gather blend, i.e. the canopy is lit by the probe field it
    // already looked up per vertex. Byte-for-byte the same arithmetic as the call
    // below minus the one textureLod, and identical in form to what the cube tier
    // already does (tree_vox.vert): "the gather is a screen-space GI buffer
    // resolved at prepass depth, so for this tier it is wrong… dropping it is a
    // correction, not a shortcut". The crown has no normal in the G-buffer either,
    // so that tap traces from a normal reconstructed off alpha-tested leaf noise.
    //
    // ⚠ THIS EXISTS AS A SEPARATE .spv AND NOT AS A UNIFORM, AND THAT IS THE WHOLE
    // POINT. Measured 26-08: r_gather_light 0 already skips this tap at runtime and
    // Trees/Mesh does not move (1.42 vs 1.44) — while compiling the term out drops
    // it to 0.89. What costs is the tap's PRESENCE in the pipeline, not its
    // execution, so a cvar multiplier inside the function would fix the hiss and
    // return none of the milliseconds.
    const vec3  ambFillT = TERMQ(T_GI, (vGI.w <= 0.0) ? skyFillT
                                     : mix(skyFillT, vGI.rgb * mix(1.0, vHemi, L.ddgi_params.y), vGI.w),
                                 skyFillT) * kTreeAmb;
#else
    // The view depth handed to the gather's bilateral — dot, not distance(): the
    // gather's own .a is a VIEW depth, and camDist below is Euclidean, so reusing
    // it would make the weight fail off-axis where the two diverge most.
    const vec3  ambFillT = TERMQ(T_GI, ddgiAmbientFromField(skyFillT, vGI, vHemi,
                                           dot(vWPos - L.eye_pos.xyz, L.cam_dir.xyz)),
                                 skyFillT) * kTreeAmb;
#endif
    // r_gi_flat in step with the ground and the grass — one factor for all three,
    // or the forest floor darkens away from what grows out of it.
    // ⚠ Per-pixel, for the reason spelled out in world_frag_body: the field
    // answers per pixel and stops at the volume edge, so a frame-uniform
    // displacement strips the flat fill off everything beyond it. Trees reach
    // furthest of the four receivers, so that showed on the horizon first.
    // ⭐ THE FADE IS TAKEN FROM vGI.w, NOT RECOMPUTED. ddgiAmbientFromField above
    // crossfades by exactly that number, so reading it back costs nothing and cannot
    // drift from the lookup - whereas calling ddgiVolumeFade here would emit
    // ddgiSpacing/ddgiCascadeFade into a shader that has no other use for them
    // (measured: +3 function bodies in the SPIR-V) and evaluate at a different rate
    // than the vertex-side lookup it is meant to match. vGI.w is giWeight x fade and
    // ddgi_params.w is giWeight x r_gi_flat, so the ratio below is r_gi_flat alone.
    // ⭐ Gated on T_GI as well, and this is the line that lets the VERTEX shader go
    // with the term: vGI.w is the LAST thing the fragment reads out of that varying.
    // Leave it ungated and the FS still consumes vGI, so the driver keeps
    // ddgiFieldWeighted alive in tree.vert (or the buffer read in the per-tree twin)
    // to produce a value that provably cannot change the result — with the field off
    // ddgiFieldWeighted returns vec4(0), so `1.0 - flatShare * 0` IS 1.0. Exact, not
    // an approximation: the bit is cleared from the very lane the function tests.
    const float flatShare = L.ddgi_params.w / max(L.ddgi_params.x, 1e-4);
    vec3 ambient = (ambFillT + L.ambient.rgb * TERMQ(T_GI, (1.0 - flatShare * vGI.w), 1.0))
                 * coloredAO(TERMQ(T_AO, gtaoVisK(0.5), 1.0), diff.rgb) * ssil;

    // Rain OR standing water — a fallen trunk lying in a river is soaked where it lies.
    float wetF  = max(clamp(L.rain_params.y, 0.0, 1.0), sw);
    vec3  sheen = TERMQ(T_SHEEN, near_ ? canopySkySheen(vWPos, vHemi, wetF) * tier : vec3(0.0), vec3(0.0));   // sky specular sheen (r_ibl)
    vec3  dynl  = TERMQ(T_DYN,   near_ ? dynLightsFoliage(vWPos) * tier : vec3(0.0),   vec3(0.0));               // point/spot walk + 1-tap shadows
    vec3  col   = diff.rgb * (ambient + sunPart + dynl) + sheen;
    col *= 1.0 - 0.65 * sw;

    // Distance fog (R4).
    float fog = TERMQ(T_FOG, clamp(camDist * L.fog_params.w + L.fog_params.x, 0.0, 1.0), 0.0);
    col = mix(col, L.fog_color.rgb, fog);

    if (SPEC_TDEBUG && L.ibl_params.w > 0.5) { outColor = vec4(sheen, 1.0); return; }   // r_ibl_debug
    // ⚠ This debug lookup USED to be called unconditionally on the shipping path —
    // "a debug view nobody enabled" is exactly the kind of term that hides in a
    // per-wave cost, which is why it got a bit of its own before the split existed.
    vec4 ptdbg = TERMQ(T_PTDBG, foliagePointDebug(vWPos), vec4(0.0));
    if (ptdbg.a > 0.5) { outColor = vec4(ptdbg.rgb, 1.0); return; }
    outColor = vec4(col, 1.0);
}

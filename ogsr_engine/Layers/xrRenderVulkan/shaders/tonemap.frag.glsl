#version 450
#extension GL_GOOGLE_include_directive : require
#include "wet_common.glsl"   // vHash/vNoise/puddlesMaskProc/rippleLayer/rainRipples
#include "froxel.glsl"        // exp-Z slice <-> view-Z mapping (shared with vol_inject + particle probe)
// Tonemap / auto-exposure / bloom / color-grading composite — the final step
// mapping the HDR scene to the display. Ports the R4 (Enhanced Shaders) chain:
//   1. auto-exposure (bloom_luminance_3.ps): exposure = middlegray/(avgLum+low),
//      avgLum from the HDR target's top mip, clamped.
//   2. bloom add (blend_soft, common_functions.h): R4 inverse-tonemaps the LDR,
//      adds the blurred bloom, re-tonemaps — mathematically "add bloom to the
//      linear HDR before tonemapping", which is exactly what we do here.
//   3. CDL color grading in log space (ACES_Color_Grading.h + ACES_settings.h):
//      Slope = r2_img_exposure, Power = 2*(1 - r2_img_cg), Sat = r2_img_saturation,
//      applied on ACEScc-encoded values (USE_LOG_GRADING). Neutral by default.
//      (R4's Contrast_Reduction 0.7 × Contrast_Boost 1.42857 cancel exactly — skipped.)
//   4. Reinhard x/(x+1) normalized by white = 11.2 (tonemap_srgb.h) — soft
//      filmic highlights instead of a hard clip; bloom supplies the glow.
//   5. gamma (img_corrections.h): pow(c, 1/r2_img_gamma).

layout(set = 0, binding = 0) uniform sampler2D uHDR;     // scene, full mip chain (avg-luminance only)
layout(set = 0, binding = 1) uniform sampler2D uBloom;   // blurred bright-pass (quarter res)
layout(set = 0, binding = 2) uniform sampler2D uDistort; // particle heat-haze offsets (rg, neutral 0.5)
layout(set = 0, binding = 3) uniform sampler2D uDepth;   // scene depth (SSR puddles)
layout(set = 0, binding = 4) uniform sampler3D uVolume;  // integrated volumetrics (rgb=in-scatter, a=transmittance)
layout(set = 0, binding = 5) uniform sampler2D uILScene;      // SSIL — half-res one-bounce indirect light (HDR, pre-exposure)
layout(set = 0, binding = 6) uniform sampler2D uResolved;// RESOLVED base colour at DISPLAY res (DLSS output when upscaling, else == uHDR mip0)

// Shared per-frame environment set (same UBO/set the world shaders read at
// set 1) — the SSR puddles need the rain mask/VP, the wetness factor and the
// camera frustum terms. Layout MUST match vk_env_light.h LightUBO.
// Shared per-frame environment set (the SAME UBO + samplers the world shaders
// read at set 1 — vk_pass_tonemap binds EnvLight::GetSetLayout() there). The SSR
// puddles need the rain mask/VP, the wetness factor and the camera frustum terms.
//
// This used to be a hand-copied 29-field PREFIX of that UBO. A prefix only stays
// correct while every new field is appended at the TAIL; one insertion in the
// middle of light_ubo.glsl would have shifted everything here with no diagnostic.
#include "light_ubo.glsl"     // Lighting UBO (set 1 b0) + the set-1 samplers
layout(set = 1, binding = 14) uniform sampler2D uVsmMask; // VSM screen mask (B = dyn-atlas occlusion, r_vsm_debug_dyn overlay)

layout(push_constant) uniform PC {
    vec4 p0;   // x=whitePoint, y=topMipLOD, z=middleGray, w=lowLum
    vec4 p1;   // x=expMin, y=expMax, z=expComp, w=bloomIntensity
    vec4 p2;   // x=cdlSlope, y=cdlSaturation, z=invGamma, w=distortAmount (0 = off)
    vec4 p3;   // xyz=cdlPower (2*(1-cg)), w=r_dither (output-dither amplitude in 8-bit LSBs, 0=off)
    vec4 p4;   // x=vol mode (0 off / 1 composite / 2 debug), y=near, z=far, w=log2(far/near)
    vec4 p5;   // SSIL: x=strength, y=debug (show only bounce), z=enable (0 = skip); w = VSM dyn-shadow debug (r_vsm_debug_dyn: red overlay)
    vec4 p6;   // sun-beam ground splash: x=strength (0=off, r_sun_beam_splash), y=in-scatter luminance threshold (r_sun_beam_splash_thr); z=DLSS CAS sharpen (r_dlss_sharp, 0=off); w=DLSS debug mode ±1..3 (r_dlss_debug; sign: + = DLSS output resolved this frame, − = plain scene)
    vec4 p7;   // x=r_vol_upsample (volume reconstruction filter, 0=old single tap), y=frame counter for the per-frame pattern rotation (0 = frozen: no temporal upscaler this frame), zw reserved
} pc;

layout(location = 0) out vec4 outColor;

const vec3 LUM = vec3(0.2126, 0.7152, 0.0722);

// puddleMask/rippleLayer/rainRipples → wet_common.glsl (shared; the SSR ripples
// now match the world-pass ripples exactly — they used to be a separate old copy).

// Water depth (metres) from the flow sim, sampled at the column via rain_vp.
float waterDepthAt(vec3 wp)
{
    vec3 n = (L.rain_vp * vec4(wp, 1.0)).xyz;
    vec2 uvr = n.xy * 0.5 + 0.5; uvr.y = 1.0 - uvr.y;
    if (uvr.x < 0.0 || uvr.x > 1.0 || uvr.y < 0.0 || uvr.y > 1.0) return 0.0;
    // Blurred (5-tap) — spreads the sim's crease/seam line-pooling into smooth
    // area puddles instead of thin lines along mesh folds.
    vec2 px = 5.0 / vec2(textureSize(uWater, 0));
    return textureLod(uWater, uvr, 0.0).r * 0.4
         + (textureLod(uWater, uvr + vec2(px.x, 0.0), 0.0).r
          + textureLod(uWater, uvr - vec2(px.x, 0.0), 0.0).r
          + textureLod(uWater, uvr + vec2(0.0, px.y), 0.0).r
          + textureLod(uWater, uvr - vec2(0.0, px.y), 0.0).r) * 0.15;
}

// vHash/vNoise/puddlesMaskProc → wet_common.glsl (shared, #included above).

// Puddle coverage — SAME procedural blob (+ wetness-driven coverage) as the world
// pass (sssPuddle), so the SSR scene-mirror appears exactly where the world pass
// paints puddles. Slope/upface gating is done by the caller's `upface`.
float puddleCoverage(vec3 wp)
{
    float wet = clamp(L.rain_params.y, 0.0, 1.0);
    return puddlesMaskProc(wp.xz, clamp(wet * L.pom_params7.y * 1.5, 0.0, 1.0), L.pom_params7.w);
}

// Binary rain-map visibility (single tap — only gates the puddle mirror).
float rainVisB(vec3 wp)
{
    vec3 n = (L.rain_vp * vec4(wp, 1.0)).xyz;
    vec2 uvr = n.xy * 0.5 + 0.5;
    uvr.y = 1.0 - uvr.y;
    if (uvr.x < 0.0 || uvr.x > 1.0 || uvr.y < 0.0 || uvr.y > 1.0 || n.z <= 0.0 || n.z >= 1.0)
        return 1.0;
    return (n.z - 0.0015 <= textureLod(uRainMap, uvr, 0.0).r) ? 1.0 : 0.0;
}

void main()
{
    // 1. Auto-exposure. The CPU passes a TEMPORALLY-SMOOTHED exposure in p5.x
    //    (eye adaptation — eases toward the metered target so the image doesn't
    //    darken/brighten instantly as the camera tilts sky↔ground). p5.x <= 0 on
    //    the first frames (before a readback is valid) → fall back to the
    //    instantaneous whole-frame-average estimate from the HDR top mip.
    vec3  avg    = textureLod(uHDR, vec2(0.5), pc.p0.y).rgb;
    // NaN FIREWALL on the average: the mip chain is LINEAR blits, so ONE
    // non-finite scene pixel reaches the top mip and this tap — and clamp/max
    // against NaN is undefined in SPIR-V, so without the select the exposure
    // (and everything scaled by it) rides on driver luck. Same guard as
    // bloom_build.frag (which eats the same chain).
    avg = mix(clamp(avg, vec3(0.0), vec3(65504.0)), vec3(0.0), isnan(avg));
    float avgLum = max(dot(avg, LUM), 1e-4);   // also a scene-brightness proxy for water murk below
    float exposure = (pc.p5.x > 0.0)
                   ? pc.p5.x                                                        // CPU-smoothed (eye adaptation)
                   : clamp(pc.p0.z / (avgLum + pc.p0.w), pc.p1.x, pc.p1.y) * pc.p1.z; // fallback: instantaneous

    // Particle heat haze (R2/R4 combine_2.ps): offset the scene UV by the
    // distortion buffer (rg around neutral 0.5). At zero offset the linear
    // sample at the pixel centre equals the old texelFetch exactly.
    // UV from the DISPLAY-res resolved target (== render res when not upscaling), so
    // the fullscreen composite maps [0,1] correctly at the swapchain resolution.
    // p7.zw = 1/display size when the scene is rendered BELOW display resolution without an upscaler (r_render_scale < 1):
    // uResolved is then the small render-res image, and dividing the display-res fragment coordinate by ITS size put the
    // picture in the top-left corner with the edge stretched over the rest (Switch 540p, 30-09). 0 = old behaviour.
    vec2 uv = (pc.p7.z > 0.0) ? gl_FragCoord.xy * pc.p7.zw
                              : gl_FragCoord.xy / vec2(textureSize(uResolved, 0));
    vec2 sceneUV = uv;
    if (pc.p2.w > 0.0)
        sceneUV += (texture(uDistort, uv).rg - 0.5) * pc.p2.w;
    vec3 c = textureLod(uResolved, sceneUV, 0.0).rgb;
    // NaN FIREWALL on the composite input: a non-finite scene/DLSS pixel would
    // otherwise reach the display as a black dot. This guards the DISPLAY only
    // — the SSIL-history and water-refraction snapshots are blits of the scene
    // image taken UPSTREAM of this pass, so their feedback loops carry their
    // own guards (ssilBoost in ao_sampled.glsl). The firewall does not hide
    // the diagnosis either: r_dlss_debug 4 still classifies the raw scene
    // (magenta = NaN) from the untouched uHDR chain.
    c = mix(clamp(c, vec3(0.0), vec3(65504.0)), vec3(0.0), isnan(c)) * exposure;
    vec3 casBase = c;   // raw exposed centre tap — the CAS sharpen (post-tonemap) works on this

    // ---- DLSS debug (r_dlss_debug, p6.w = ±mode; sign + = DLSS resolved this frame) ----
    //   1 = CAS delta heatmap (forces a floor sharpen so it shows even at r_dlss_sharp 0)
    //   2 = split screen: LEFT = render-res scene bilinear-upscaled (no DLSS), RIGHT = the
    //       real composite (DLSS output when on) — green seam at the split
    //   3 = gate flag: green tint = tonemap is compositing the DLSS output, red = plain scene
    //   4 = input sanitizer: classify the RENDER-RES scene (uHDR = what DLSS eats):
    //       magenta = NaN/Inf, red = negative channel, cyan = luminance > 1000 —
    //       any of those poisons the DLSS convolution into black smears.
    float dbgMode = abs(pc.p6.w);
    if (dbgMode > 1.5 && dbgMode < 2.5 && uv.x < 0.5) {
        c = textureLod(uHDR, sceneUV, 0.0).rgb * exposure;   // raw render-res scene, bilinear
        casBase = c;
    }
    if (dbgMode > 3.5) {
        vec3 s = textureLod(uHDR, uv, 0.0).rgb;   // pre-exposure raw scene
        vec3 col;
        if (any(isnan(s)) || any(isinf(s)))            col = vec3(1.0, 0.0, 1.0);
        else if (any(lessThan(s, vec3(0.0))))          col = vec3(1.0, 0.0, 0.0);
        else if (any(greaterThan(s, vec3(1000.0))))    col = vec3(0.0, 1.0, 1.0);
        else col = vec3(dot(s * exposure, LUM) * 0.15);   // dim grey scene for context
        outColor = vec4(col, 1.0);
        return;
    }

    // (CAS sharpen r_dlss_sharp moved POST-tonemap — sharpening here in HDR was a
    // near no-op: Reinhard's 1/(1+x)^2 slope crushed the unsharp delta before it
    // reached the screen. See the display-referred block after gamma below.)

    // ---- Water: volumetric depth (murk + refraction) + SSR surface mirror ----
    // The flow sim gives a per-column water DEPTH. The deeper the water the more
    // it absorbs/scatters (Beer-Lambert) → a murky bottom that reads as DEEP
    // ("knee-deep, can't see the floor"), the bottom refracted by the surface
    // ripples; the SSR march then mirrors the scene on the surface. Falls back
    // to the procedural puddle mask when the sim is off. (rain_params.z<0 = the
    // wet-debug flag → skip.)
    float wet   = L.rain_params.y;
    bool  simOn = L.pom_params6.x > 0.5;
    if ((wet > 0.01 || simOn) && L.rain_params.z >= 0.0) {
        float zndc = textureLod(uDepth, uv, 0.0).r;
        if (zndc < 0.9999) {
            // Depth → world via the frustum-ray basis (SSAO scheme, verified).
            float zview = clamp(L.cam_rightT.w / (zndc - L.cam_dir.w), 0.0, 10000.0);
            vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
            vec3 ray = L.cam_dir.xyz + L.cam_rightT.xyz * ndc.x + L.cam_topT.xyz * ndc.y;
            vec3 wp  = L.eye_pos.xyz + ray * zview;

            vec3 Ng = normalize(cross(dFdx(wp), dFdy(wp)));
            // Floor puddles are BELOW the camera (we look DOWN at them). The
            // derivative normal's sign is ambiguous (don't flip it up — that put
            // water on ceilings), so gate by the VIEW ray: only surfaces we look
            // down at get water; ceilings (we look up) never do.
            float viewDownY = normalize(wp - L.eye_pos.xyz).y;   // < 0 = looking down
            float upface = smoothstep(0.72, 0.85, abs(Ng.y)) * step(viewDownY, 0.0);

            float wd  = simOn ? waterDepthAt(wp) : 0.0;
            // Real-dip placement (not the old procedural sine mask) → the SSR mirror
            // only fires in actual depressions, so flat wet ground is NOT a lake.
            float pud = simOn ? smoothstep(0.04, 0.12, wd) : puddleCoverage(wp);

            float ripFade = smoothstep(18.0, 8.0, zview)
                          * clamp(L.rain_params.x * 1.5 + 0.1, 0.0, 1.0);
            vec2 rip = rainRipples(wp.xz, L.sky_params.w * 0.6) * (0.3 * ripFade);
            vec3 Nw  = normalize(vec3(rip.x, 1.0, rip.y));
            vec3 V   = normalize(L.eye_pos.xyz - wp);

            // Volumetric absorption: murk the bottom by the water the view ray
            // crosses (depth / vertical view component). Deep water → can't see
            // the floor; the bottom is refracted by the surface ripples.
            if (simOn && wd > 0.04 && upface > 0.0) {
                float cosV    = max(V.y, 0.06);
                float pathLen = min(wd / cosV, 8.0);
                float murk    = 1.0 - exp(-pathLen * max(L.pom_params6.y, 0.01));
                vec2  refr    = rip * (L.pom_params6.z * (0.5 + wd));
                vec3  bottom  = textureLod(uResolved, sceneUV + refr, 0.0).rgb * exposure;
                vec3  murkCol = vec3(0.05, 0.10, 0.09) * (0.3 + 0.7 * avgLum * exposure);
                c = mix(c, mix(bottom, murkCol, murk), upface);
            }

            // SSR surface mirror — buildings/trees reflect in the water surface.
            float k0 = (simOn ? max(wet, 0.5) : wet) * pud * upface;
            if (k0 > 0.02) {
                k0 *= rainVisB(wp);
                vec3 R  = reflect(-V, Nw);
                float fres = pow(1.0 - clamp(dot(V, Nw), 0.0, 1.0), 2.0);
                float k = k0 * (0.25 + 0.75 * fres) * clamp(L.rain_params.w, 0.0, 1.5);
                // SSR is the rain perf hog (a depth march per water pixel — cost
                // scales with puddle coverage). Cull it to near the camera; far
                // water keeps the painted sky reflection from the world pass.
                if (k > 0.02 && R.y > 0.01 && zview < 35.0) {
                    vec3  p   = wp + R * 0.15;
                    float stp = 0.3;
                    vec2  hitUV = vec2(-1.0);
                    for (int i = 0; i < 16; ++i) {
                        p   += R * stp;
                        stp *= 1.22;
                        vec4 cp = L.scene_vp * vec4(p, 1.0);
                        if (cp.w <= 0.0) break;
                        vec3 pn = cp.xyz / cp.w;
                        vec2 puv = pn.xy * 0.5 + 0.5;
                        puv.y = 1.0 - puv.y;
                        if (puv.x < 0.0 || puv.x > 1.0 || puv.y < 0.0 || puv.y > 1.0 || pn.z >= 1.0)
                            break;
                        float d = textureLod(uDepth, puv, 0.0).r;
                        if (d < pn.z - 0.0003) {
                            // Thickness test in view depth: don't let rays
                            // passing far BEHIND a thin silhouette "hit" it.
                            float zHit = L.cam_rightT.w / (d    - L.cam_dir.w);
                            float zRay = L.cam_rightT.w / (pn.z - L.cam_dir.w);
                            if (zRay - zHit < 4.0) hitUV = puv;
                            break;
                        }
                    }
                    if (hitUV.x >= 0.0) {
                        vec3 refl = textureLod(uResolved, hitUV, 0.0).rgb * exposure;
                        vec2 ef = min(hitUV, 1.0 - hitUV);
                        float edge = clamp(min(ef.x, ef.y) * 8.0, 0.0, 1.0);
                        c = mix(c, refl, k * edge);
                    }
                }
            }
        }
    }

    // Lens raindrops: screen-space droplets "on the camera", each a tiny lens that
    // REFRACTS the scene (magnify toward the drop centre) + a bright rim, sliding
    // DOWN over time. Density/size scale with rain intensity. (SSFX hud_raindrops.)
    float rainInt = clamp(L.rain_params.x, 0.0, 1.0);
    if (rainInt > 0.04 && L.rain_params.z >= 0.0) {
        vec2  res    = vec2(textureSize(uResolved, 0));
        float aspect = res.x / max(res.y, 1.0);
        float tt     = L.sky_params.w;
        for (int li = 0; li < 2; ++li) {
            float sc  = (li == 0) ? 13.0 : 20.0;
            float spd = (li == 0) ? 0.5  : 0.8;
            vec2 g = vec2(uv.x * aspect, uv.y) * sc;
            g.y -= tt * spd;                                   // run DOWN the screen (uv.y grows downward)
            vec2 cell = floor(g);
            if (vHash(cell + float(li) * 37.0) < 0.72) continue;  // only ~28% of cells carry a drop
            vec2 jit = vec2(vHash(cell + 5.0), vHash(cell + 9.0)) - 0.5;
            vec2 f   = fract(g) - 0.5 - jit * 0.6;
            float d  = length(f);
            float r  = (0.09 + 0.09 * vHash(cell + 3.0)) * (0.55 + 0.45 * rainInt);
            if (d > r) continue;
            float inside = smoothstep(r, r * 0.4, d) * 0.7;    // subtler
            vec2  refr   = (f / r) * inside * 0.03;            // gentle lens refraction
            vec3  drop   = textureLod(uResolved, sceneUV - refr, 0.0).rgb * exposure;
            c = mix(c, drop, inside);
            c += vec3(0.025) * smoothstep(r * 0.7, r, d) * inside;  // faint rim
        }
    }

    // ---- Volumetric fog / god-rays (r_vol) ----
    // Sample the integrated froxel volume at this pixel's depth (inverse exp-Z,
    // EXACT inverse of vol_inject's forward map) and composite in HDR before the
    // tonemap: scene*transmittance + in-scatter. Sky/no-geo → far slice (aerial
    // perspective). In-scatter is exposure-scaled to match the lit scene.
    if (pc.p4.x > 0.5) {
        float zndc  = textureLod(uDepth, uv, 0.0).r;
        float near_ = pc.p4.y, far_ = pc.p4.z, logFN = pc.p4.w;
        float zview = (zndc >= 0.9999) ? far_
                    : clamp(L.cam_rightT.w / (zndc - L.cam_dir.w), near_, far_);
        float vw  = clamp(Froxel_SliceFromViewZ(zview, near_, logFN), 0.0, 1.0);
        // Sub-froxel dither (interleaved-gradient noise) breaks the residual grid
        // banding into fine grain. Kept SUBTLE (±0.25 froxel) — the higher-res grid
        // already smooths most of it, so a light dither avoids visible noise. (Full
        // convergence is the P4 temporal-accumulation job.)
        vec2 ign = vec2(
            fract(52.9829189 * fract(dot(gl_FragCoord.xy,             vec2(0.06711056, 0.00583715)))),
            fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238,  vec2(0.06711056, 0.00583715))))) - 0.5;
        ign *= 0.5;   // ±0.25 froxel
        vec3 vtex = vec3(textureSize(uVolume, 0));
        vec4 vol;
        if (pc.p7.x > 0.5) {
            // RECONSTRUCTION (r_vol_upsample). The volume is ~7-8 screen pixels per
            // froxel, so a single tap resolves it at its own coarse resolution — that
            // is the "PS1 pixels" visible with r_vol_ta 0, and what the temporal pass
            // was really covering up. Four taps on a rotated grid inside ±0.5 froxel
            // form a tent over ~2 froxels: the blockiness (information the volume can
            // never carry anyway) turns into a smooth gradient. The Z offset keeps the
            // old per-pixel dither so slice banding stays broken up.
            //
            // The grid ROTATES per frame, but only while a temporal upscaler resolves
            // (p7.y, else 0). That inverts a trap: a screen-STATIC pattern is the worst
            // possible input to DLSS — it looks like stable detail, so DLSS preserves
            // the grain instead of averaging it. Rotating turns the same taps into free
            // supersampling of the volume. With no upscaler the pattern stays frozen,
            // since an animated one would just crawl.
            float ang = 2.39996323 * pc.p7.y + 6.2831853 * ign.x;   // golden angle/frame + per-pixel decorrelation
            vec2  cs  = vec2(cos(ang), sin(ang));
            mat2  rot = mat2(cs.x, -cs.y, cs.y, cs.x);
            const vec2 kTap[4] = vec2[4](vec2( 0.35,  0.35), vec2(-0.35,  0.35),
                                         vec2( 0.35, -0.35), vec2(-0.35, -0.35));
            float zoff = (ign.x + ign.y) * 0.5 / vtex.z;
            vol = vec4(0.0);
            for (int i = 0; i < 4; ++i)
                vol += textureLod(uVolume, vec3(uv + (rot * kTap[i]) / vtex.xy, vw + zoff), 0.0);
            vol *= 0.25;
        } else {
            vec3 vuvw = vec3(uv + ign * (1.0 / vtex.xy), vw + (ign.x + ign.y) * 0.5 / vtex.z);
            vol = textureLod(uVolume, vuvw, 0.0);
        }
        if (pc.p4.x > 1.5)
            c = vol.rgb * exposure * 8.0;             // r_vol_debug: raw in-scatter pattern
        else {
            // Spectral extinction (aerial perspective): the froxel stores a SCALAR
            // transmittance, but distant surfaces should also lose their warm colours
            // (long-wavelength light survives the haze least in the perceived aerial
            // look) — so reconstruct the optical depth and attenuate RED more than
            // BLUE. Distant geometry desaturates toward the blue in-scatter veil.
            float tau    = -log(clamp(vol.a, 1e-4, 1.0));
            vec3  Tsp    = exp(-tau * vec3(1.22, 1.05, 0.85));   // red fades first → blue distance
            vec3  cScene = c * Tsp;
            vec3  inscat = vol.rgb * exposure;
            c = cScene + inscat;
            // [PARKED 2026-07-06 — r_sun_beam_splash default 0, pc.p6.x = 0 → block skipped.
            //  Read as air haze, not ground light; superseded then also parked. Kept as
            //  scaffolding. See the PARKED note in vk_console_min.cpp.]
            // GOD-RAY GROUND SPLASH (r_sun_beam_splash): a shaft LANDING on shadowed
            // ground reads as bright in-scatter over a dark surface — the view ray
            // gathered the lit shaft through the crown gap, but at the ground it just
            // looks like thin haze and "dissolves" (the sun is geometrically occluded
            // there, so there is no direct pool to recover — proven via r_terrain_debug
            // 7). Relight the SURFACE itself (not the air): where the beam outshines the
            // ground it lands on, scale that ground colour UP, warm-tinted by the shaft,
            // so the terrain brightens WITH its own texture — a sun pool — instead of
            // just thickening the haze (adding in-scatter only brightened the fog). Bloom
            // flares the bright pool. Surfaces only (skip sky); uniform fog over already-
            // lit terrain barely triggers (the beam has to out-shine the surface).
            if (pc.p6.x > 0.0 && zndc < 0.9999) {
                float bl   = dot(inscat, vec3(0.299, 0.587, 0.114));
                float sl   = dot(cScene, vec3(0.299, 0.587, 0.114));
                float gate = smoothstep(pc.p6.y, pc.p6.y * 2.0, bl) * clamp(bl - sl, 0.0, 1.0);
                vec3  tint = inscat / max(bl, 1e-4);   // normalized warm shaft colour
                c += cScene * tint * (gate * pc.p6.x);
            }
        }
    }

    // ---- SSIL debug view (r_ssil_debug) ----
    // SSIL is now applied in the FORWARD shaders (ssilBoost() multiplies the ambient
    // term, SSFX-style — see light_ubo.glsl), not here. The tonemap only offers a
    // visualization of the raw bounce buffer the receivers consume.
    if (pc.p5.y > 0.5) {
        vec3 il = textureLod(uILScene, uv, 0.0).rgb;
        il = il / (1.0 + il);   // SSFX compression → [0,1) so HDR bounce stays visible
        outColor = vec4(pow(clamp(il, 0.0, 1.0), vec3(pc.p2.z)), 1.0);
        return;
    }

    // 2. Bloom (built exposure-scaled at quarter res, gaussian-blurred) added in
    //    linear HDR — the blend_soft equivalence.
    c += texture(uBloom, uv).rgb * pc.p1.w;

    // 3. CDL color grading in LINEAR space. R4's user build runs the USE_ACES
    //    path where the saved mod grading (e.g. Gunslinger's cg 0.515-green +
    //    saturation 1.3 in user.ltx) reads subtle; grading those same numbers
    //    in ACEScc LOG space (the non-ACES fallback) multiplies their leverage
    //    ~4× and tinted the whole world green. Linear CDL matches the ACES-path
    //    magnitude. Neutral at default console vars.
    c *= pc.p2.x;                                        // Slope (ssfx_exposure)
    c = pow(max(c, vec3(0.0)), pc.p3.xyz);               // Power (2*(1 - ssfx_color_grading))
    float luma = dot(c, LUM);
    c = luma + pc.p2.y * (c - luma);                     // Saturation (ssfx_saturation)
    c = max(c, vec3(0.0));

    // 4. Reinhard normalized to white = 11.2 (R4 tonemap_sRGB).
    float W = pc.p0.x;
    c = (c / (c + 1.0)) / (W / (W + 1.0));

    // 5. Gamma (img_corrections).
    c = pow(max(c, vec3(0.0)), vec3(pc.p2.z));

    // ---- CAS sharpen on the DLSS-resolved colour (r_dlss_sharp, p6.z; 0 = off —
    // only set when DLSS ran this frame). NGX dropped built-in sharpening, so the
    // temporal resolve reads slightly soft. Runs DISPLAY-REFERRED (post Reinhard +
    // gamma, where AMD CAS lives): each raw tap goes through the same
    // exposure→Reinhard→gamma curve, the unsharp delta is built in that space and
    // ADDED to the graded pixel — sharpening in HDR was crushed by the tonemap's
    // 1/(1+x)^2 slope. Low-frequency terms (fog/bloom/CDL) cancel out of the delta.
    // Clamped to the tap neighbourhood min/max so edges sharpen without ringing.
    float sharpAmt = pc.p6.z;
    bool  casHeat  = (dbgMode > 0.5 && dbgMode < 1.5);   // r_dlss_debug 1
    if (casHeat) sharpAmt = max(sharpAmt, 0.3);          // heatmap shows even at sharp 0
    if (sharpAmt > 0.0) {
        float Wn = W / (W + 1.0);
        vec2 ts = 1.0 / vec2(textureSize(uResolved, 0));
        #define CAS_TM(x) pow(max((x) / ((x) + 1.0) / Wn, vec3(0.0)), vec3(pc.p2.z))
        vec3 t0 = CAS_TM(casBase);
        vec3 tN = CAS_TM(textureLod(uResolved, sceneUV + vec2(0.0, -ts.y), 0.0).rgb * exposure);
        vec3 tS = CAS_TM(textureLod(uResolved, sceneUV + vec2(0.0,  ts.y), 0.0).rgb * exposure);
        vec3 tW = CAS_TM(textureLod(uResolved, sceneUV + vec2(-ts.x, 0.0), 0.0).rgb * exposure);
        vec3 tE = CAS_TM(textureLod(uResolved, sceneUV + vec2( ts.x, 0.0), 0.0).rgb * exposure);
        #undef CAS_TM
        vec3 mn = min(t0, min(min(tN, tS), min(tW, tE)));
        vec3 mx = max(t0, max(max(tN, tS), max(tW, tE)));
        vec3 blur = (tN + tS + tW + tE) * 0.25;
        vec3 sharpened = clamp(t0 + (t0 - blur) * (sharpAmt * 2.0), mn, mx);
        vec3 delta = sharpened - t0;
        // Dark-halo guard: the DARKENING half of the unsharp mask reads as a black
        // fringe on thin high-contrast edges (foliage vs bright sky) — keep only a
        // third of the negative push; brightening keeps full strength.
        delta = max(delta, delta * 0.33);
        c = max(c + delta, vec3(0.0));
        // Heatmap: |delta| ×20 — grey speckle on detail = sharpen alive; pure black
        // = the unsharp mask finds nothing (or the gate is off — check the sign tint).
        if (casHeat) c = vec3(min(length(delta) * 20.0, 1.0));
    }

    // ---- VSM dyn-shadow debug (r_vsm_debug_dyn): pixels shadowed by the DYNAMIC
    // atlas (NPC/grass casters) tint RED. mask.B is written raw by vsm_resolve,
    // UNGATED by r_vsm_dyn_gate — it shows the dyn atlas's actual content, so
    // "red blob under the NPC" = casters render+bin fine (any shadow loss is the
    // resolve gate), "no red at all" = casters never reach the dyn atlas.
    if (pc.p5.w > 0.5) {
        float dyn = textureLod(uVsmMask, uv, 0.0).b;
        c = mix(c, vec3(1.0, 0.03, 0.03), 0.7 * clamp(dyn, 0.0, 1.0));
    }

    // 6. Output dither (r_dither). The swapchain is 8-bit UNORM, so any SLOW
    // gradient — AO-modulated ambient on flat asphalt, sky-ambient on distant
    // slopes, dusk sky — quantizes into visible contour STRIPES ("полосы"). The
    // AO-side data is smooth (offline-verified, _parked/gtao_band_repro.js); the
    // bands are born HERE, at the 8-bit write. This is why every geometric AO
    // bias was a no-op while forcing AO to a CONSTANT (grazing fade, strength 0,
    // debug 4) "fixed" them: a constant has no gradient to contour. TPDF dither
    // (difference of two decorrelated IGNs → triangular ±1 LSB) linearizes the
    // quantizer: steps become imperceptible per-pixel grain, mean is unchanged.
    // Monochrome (same offset per channel) to avoid chroma noise. Static pattern
    // — no temporal shimmer, and it must NOT depend on scene state.
    if (pc.p3.w > 0.0) {
        float dA = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        float dB = fract(52.9829189 * fract(dot(gl_FragCoord.xy + 23.14069, vec2(0.06711056, 0.00583715))));
        c += vec3((dA - dB) * (pc.p3.w / 255.0));
    }

    // ---- DLSS debug overlays (see the block after casBase above) ----
    if (dbgMode > 1.5 && dbgMode < 2.5) {
        // Split seam: 1px green line at the half-screen boundary.
        if (abs(uv.x - 0.5) * float(textureSize(uResolved, 0).x) < 1.0) c = vec3(0.0, 1.0, 0.0);
    } else if (dbgMode > 2.5) {
        // Gate flag: green = the composite reads the DLSS output this frame, red = plain scene.
        c = mix(c, (pc.p6.w > 0.0) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), 0.2);
    }

    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}

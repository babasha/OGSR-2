#version 450
#extension GL_GOOGLE_include_directive : require

// Two-cubemap blended sky, mirroring R4 dxEnvironmentRender::RenderSky, plus an
// animated cloud layer that mirrors R4 dxEnvironmentRender::RenderClouds.
//
// R4 renders a half-cube (`hbox_verts` in dxEnvironmentRender.cpp): top is a
// regular unit cube, the lower hemisphere is squashed flat at y_vis=-0.01,
// and the side face's mid-ring uses tc.y=-1 while the top uses tc.y=1. Net
// effect: the cubemap's V axis gets stretched across the visible upper
// hemisphere, the horizon line lands at tc.y ≈ -0.98 (bottom edge of the
// cubemap), and the lower hemisphere is essentially invisible. X-Ray sky
// cubemaps are authored against this mapping — the sky gradient occupies
// almost the full V range, and the horizon line is at the very bottom.
//
// To get the R4 look from a fullscreen-triangle pass, we remap the per-pixel
// world direction to the same tc the half-cube geometry would have produced
// for that view ray, then sample the cubemap with that.
//
// CLOUDS: R4 draws a SEPARATE pass (RenderClouds) — a flattened cloud dome
// (unit hemisphere scaled (10, 0.4, 10), rotated by sky_rotation) whose UVs
// scroll with global time (clouds.vs): tc = p.xz*CLOUD_TILE + wind*t*CLOUD_SPEED.
// Two cloud textures at different tile/speed are summed and tinted by
// clouds_color (clouds.ps). We reproduce that here per-pixel: project the view
// ray onto the same dome (inverse of the (10,0.4,10) scale), scroll two 2D
// cloud textures by fTimeGlobal, sum + tint, and additively composite over the
// sky. This is what makes the clouds MOVE — the cubemap sky itself is static.

layout(set = 0, binding = 0) uniform samplerCube uSky0;
layout(set = 0, binding = 1) uniform samplerCube uSky1;
layout(set = 0, binding = 2) uniform sampler2D   uClouds0;  // scrolling cloud layer 0
layout(set = 0, binding = 3) uniform sampler2D   uClouds1;  // scrolling cloud layer 1

// Volumetric cloud noise volumes, baked once at init by vk_clouds.
layout(set = 0, binding = 5) uniform sampler3D uCloudShape;    // 128^3: base form + Worley FBM
layout(set = 0, binding = 6) uniform sampler3D uCloudDetail;   // 32^3: edge erosion
layout(set = 0, binding = 7) uniform sampler2D uCloudWeather;  // 512^2: coverage / type / band

// Cloud coverage counters (r_clouds_debug). Written only when debug is on; the CPU
// reads them back and LOGS the result, because "look at the screen and describe it"
// is a diagnostic the log cannot carry — and the log is how this project is debugged.
layout(std430, set = 0, binding = 8) buffer CloudDebug {
    uint skyPixels;     // pixels where the sky pass ran with the deck enabled
    uint cloudPixels;   // ...of those, how many got any cloud at all
    uint maxAlphaM;     // max coverage x1000
    uint sumAlphaM;     // sum of coverage x1000 (mean over skyPixels)
} dbg;

// The buffer above is a side effect, and a FS with side effects runs its depth test
// late: every pixel of the fullscreen triangle paid the shader, the ones behind
// houses and terrain too (07-10: a flat ~1 ms on the Switch wherever you stood).
// Nothing here writes depth or discards, so the early test changes no result.
layout(early_fragment_tests) in;

#include "sky_ubo.glsl"

layout(location = 0) in  vec3 vWorldDir;
layout(location = 0) out vec4 outColor;

// R4 cloudconfig.h — two layers, different tile/speed, fixed wind headings.
const float CLOUD_TILE0  = 0.7;
const float CLOUD_SPEED0 = 0.1;   // 2 * 0.05
const float CLOUD_TILE1  = 2.8;
const float CLOUD_SPEED1 = 0.05;  // 2 * 0.025
// R4 RenderClouds: wd0 heading = 45°, wd1 heading = 67.5° (fixed, not weather wind).
const vec2  WIND0 = vec2(0.70710678, 0.70710678);  // (sin45, cos45)
const vec2  WIND1 = vec2(0.92387953, 0.38268343);  // (sin67.5, cos67.5)

// The half-cube mapping + sky_rotation now live in sky_halfcube.glsl — SHARED
// with ibl_prefilter.comp, which must reproduce this exact mapping to build a
// world-space light probe (see the header for why).
#include "sky_halfcube.glsl"
#include "atmosphere.glsl"     // procedural Rayleigh+Mie sky (r_sky_proc)
#include "noise_common.glsl"   // nc_remap for the cloud model
#include "clouds.glsl"         // volumetric cloud deck + cirrus (r_clouds_vol)

void main()
{
    vec3 dir = normalize(vWorldDir);

    // R4 rotates the skybox geometry by sky_rotation; sampling at R(-θ)*d
    // lands on the same texel that geometry would expose to the view ray.
    float skyRot = S.camRightTan_rot.w;
    vec3 dirRot = SkyUnrotate(dir, skyRot);

    vec3 sampleDir = SampleDirHalfCube(dirRot);

    // Compared in UNROTATED world space (skyRot is a cube-authoring thing).
    vec3  toSun = normalize(-S.sunDir_pad.xyz);
    float cosA  = dot(dir, toSun);

    vec3 outc;
    if (S.atmoParams.x > 0.5) {
        // ── PROCEDURAL SKY (r_sky_proc) ──────────────────────────────────────
        // Radiance from geometry + physics, NOT from the weather config's colours.
        // That is the whole point: at dusk the config hands us neutral grey and a
        // zeroed sun, so anything derived from it is grey. This is derived from the
        // sun's ELEVATION, which the config does get right.
        outc = AtmosphereRadiance(dir, toSun, S.atmoParams.y, S.atmoParams.w,
                                  S.atmoParams.z, 0.0);

        // The sun's own disk, coloured by how much atmosphere its light crossed —
        // this is what turns it red as it sets, with no authored colour involved.
        vec3  sunCol = SunTransmittance(toSun, S.atmoParams.z, 0.0);
        float disk   = smoothstep(0.9993, 0.9997, cosA);
        outc += sunCol * (disk * 12.0 * S.atmoParams.y);
        // No separate halo term here: the Mie lobe in AtmosphereRadiance already
        // produces the aureole, physically, and adding the old ad-hoc one on top
        // would double it.
    } else {
        // ── LEGACY CUBEMAP SKY (A/B path) ────────────────────────────────────
        vec4 c0 = texture(uSky0, sampleDir);
        vec4 c1 = texture(uSky1, sampleDir);
        vec3 col = mix(c0.rgb, c1.rgb, clamp(S.camUpTan_weight.w, 0.0, 1.0));

        // skybox.vs in R4 pre-scales the per-vertex tint by 1.7 ("pre-scale by
        // tonemap"). We do the same here so brightness matches R4 output.
        vec3 tint = S.skyColor_pad.xyz * 1.7;

        // Sun disk + aureole — additive, gated above the horizon. sunColor is the
        // time-of-day env colour (which measures ~0 at dusk, hence this whole arc).
        float above = smoothstep(-0.08, 0.02, toSun.y);
        float disk  = smoothstep(0.9993, 0.9997, cosA);          // sharp core (~1° radius)
        float halo  = pow(max(cosA, 0.0), 350.0) * 0.35
                    + pow(max(cosA, 0.0),  40.0) * 0.05;         // aureole
        outc = col * tint + S.sunColor_pad.xyz * ((disk * 12.0 + halo) * above);
    }

    // --- Animated clouds (R4 RenderClouds port) -----------------------------
    // Gate: r_clouds cvar (cloudParams.y) AND clouds_color.w > 0 (R4 skips the
    // pass when the weather's cloud weight is ~0 — e.g. clear night).
    // Skipped entirely when the volumetric deck is on — ONE kind of cloud, never two
    // (the decision that was settled when this was first planned). The painted clouds
    // in the weather cube need no suppression here: the procedural sky never samples
    // that cube at all.
    if (S.shape.x <= 0.001
        && S.cloudParams.y > 0.5 && S.cloudsColor.w > 0.001 && dirRot.y > 0.0) {
        // Project the view ray onto R4's flattened cloud dome. The dome is a
        // unit hemisphere scaled (10, 0.4, 10); clouds.vs tiles by the OBJECT-
        // space position p.xz and fades by pow(p.y, 25). Undo the scale to get
        // p from the world ray: p = normalize(S^-1 * dirRot), S^-1 = (0.1,2.5,0.1).
        vec3 p = normalize(vec3(dirRot.x * 0.1, dirRot.y * 2.5, dirRot.z * 0.1));

        float t = S.cloudParams.x;                              // fTimeGlobal/10 * speed
        vec2 tc0 = p.xz * CLOUD_TILE0 + WIND0 * t * CLOUD_SPEED0;
        vec2 tc1 = p.xz * CLOUD_TILE1 + WIND1 * t * CLOUD_SPEED1;

        vec4 s0 = texture(uClouds0, tc0);
        vec4 s1 = texture(uClouds1, tc1);

        // R4 clouds.ps: mix = I.color * (s0 + s1). clouds.s blends the result
        // with (srcalpha, invsrcalpha) — a normal alpha blend, so the cloud
        // SHAPE lives in the texture ALPHA, not the colour. I.color.w =
        // clouds_color.w * pow(p.y, 25) → fade to the horizon. Reproduce that
        // alpha composite over the sky (additive-of-rgb was wrong → invisible
        // clouds; the visible ones were baked into the day cubemap).
        float fade   = pow(clamp(p.y, 0.0, 1.0), 25.0);
        vec3  cloudRGB = S.cloudsColor.rgb * (s0.rgb + s1.rgb);
        float cloudA   = clamp(S.cloudsColor.w * fade * (s0.a + s1.a)
                             * S.cloudParams.z, 0.0, 1.0);
        outc = mix(outc, cloudRGB, cloudA);
    }

    // --- Volumetric clouds (r_clouds_vol) -----------------------------------
    // Composited far-to-near: cirrus sits above the deck, so the deck goes over the
    // sky first and the cirrus over both.
    if (S.shape.x > 0.001) {
        // Per-pixel march offset. A fixed start plane prints concentric rings across
        // the whole sky (the classic raymarch banding); an interleaved-gradient
        // dither turns those rings into noise the eye reads as cloud grain.
        float dith = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

        vec4 cir = CloudsCirrus(dir, toSun, dith);
        vec4 dek = CloudsRaymarch(dir, toSun, S.camForward_alt.w, dith);

        // r_clouds_debug: show the marched COVERAGE directly, unlit. This answers the
        // only question that matters when the sky looks unchanged — is there any
        // density at all, or is the model producing nothing? 1 = raw alpha,
        // 2 = alpha x20 (catches a deck that exists but is far too thin to see).
        if (S.shape.w > 0.5) {
            // Sample SPARSELY (1 pixel in 64). Four atomics per pixel from ~4M
            // invocations all target the same four addresses — that is total
            // serialisation on one cache line and was itself a large part of the
            // slowdown. An 8x8 stride keeps the statistics just as meaningful.
            ivec2 px = ivec2(gl_FragCoord.xy);
            if (((px.x | px.y) & 7) == 0) {
                float a = clamp(dek.a, 0.0, 1.0);
                atomicAdd(dbg.skyPixels, 1u);
                if (a > 0.01) atomicAdd(dbg.cloudPixels, 1u);
                atomicMax(dbg.maxAlphaM, uint(a * 1000.0));
                atomicAdd(dbg.sumAlphaM, uint(a * 1000.0));
            }
            float a = (S.shape.w > 1.5) ? min(dek.a * 20.0, 1.0) : dek.a;
            outColor = vec4(vec3(a), 1.0);
            return;
        }

        outc = mix(outc, dek.rgb / max(dek.a, 1e-4), dek.a);
        outc = mix(outc, cir.rgb, cir.a * (1.0 - dek.a * 0.85));
    }

    // 10-10 the horizon in the fog colour (r_sky_fog_horizon, see vk_pass_sky.cpp): pure fog at and below the horizon,
    // the sky back by the band's elevation -- the far edge of the world, clipped already fully fogged, then meets a sky
    // of its own colour instead of popping against it as grey silhouettes.
    if (S.fogColor.w > 0.0)
        outc = mix(outc, S.fogColor.rgb, 1.0 - smoothstep(0.0, S.fogColor.w, dir.y));

    outColor = vec4(outc, 1.0);
}

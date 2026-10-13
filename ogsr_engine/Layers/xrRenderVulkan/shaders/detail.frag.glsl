#version 450
#extension GL_GOOGLE_include_directive : require
#define VSM_GRASS_DIRECT           // grass samples the VSM static atlas at its OWN 3D pos (b15/16/23)
#include "vsm_sample.glsl"         // vsmSunShadow (set 1 b14) + vsmSunShadowGrassDirect
#include "light_ubo.glsl"          // DynLight + Lighting UBO (set 1 b0) + samplers (set 1 b1..13)
#include "foliage_shadow.glsl"     // foliageLightShadow — grass receives spot/point pool shadows
#include "surface_class.glsl"      // SC_* classification + snow (grass)
#include "shore_wet.glsl"       // shoreWet/shoreFoam — reeds standing in water
#include "ao_common.glsl"       // coloredAO (shared with env_common.glsl)
#include "common_math.glsl"     // EnvBRDFApprox — pure, shared with world/tree
#include "ao_sampled.glsl"      // gtaoVis / ssilBoost — after light_ubo.glsl (uAO/uIL/L)
// ⭐GRASS READS THE PROBE FIELD TOO (set 1, b34..37). Measured 22-08: with only the
// world and terrain receivers sampling it, turning GI up left every blade, bush and
// tuft lit by the flat weather constant — light patches standing on ground that had
// just been re-lit. In a forest or a field that vegetation IS most of the frame, so
// the feature was invisible for the most ordinary of reasons: it was not applied to
// what the camera actually sees.
#include "ddgi_receiver.glsl"   // ddgiAmbient — the same replacement the world does

// xrRenderVulkan - detail (grass) fragment shader. set 0/binding 0 = per-type
// diffuse (alpha-cutoff 0.5, punch-out). set 1 = the shared EnvLight, now via
// light_ubo.glsl (was a hand-rolled partial copy). Sun gets a single shadow tap;
// grass overdraw noise hides the hard edge.
layout(set = 0, binding = 0) uniform sampler2D uDiffuse;

// Rain visibility - is this blade open to the sky? Single tap (grass overdraw is noisy).
float rainVisGrass(vec3 wp)
{
    vec3 n = (L.rain_vp * vec4(wp, 1.0)).xyz;
    vec2 uv = n.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || n.z <= 0.0 || n.z >= 1.0)
        return 1.0;
    return (n.z - 0.0015 <= texture(uRainMap, uv).r) ? 1.0 : 0.0;
}

// GTAO at this pixel - grass isn't in the prepass, so this is the AO of the GROUND
// behind the blade (grass in a dark corner sits in the same ambient as the dirt).
// gtaoVis / ssilBoost come from ao_sampled.glsl (they read light_ubo's uAO/uIL/L).

// Sky ambient (grass samples straight up — no real normal, same light as the
// terrain beneath it) now comes from the shared header, so grass gets the SH9
// irradiance the world does instead of a private point-sampled copy.
#include "sky_ambient.glsl"

// Sky specular sheen for grass (r_ibl). Blades have no normal → up-reflected blurry
// sky, gated by baked openness + WET (dry grass ≈ matte; the payoff is wet grass
// glistening). Karis EnvBRDFApprox → common_math.glsl. `openness` = vHemi baked hemi.
// ⚠wetF is passed IN, not looked up here — the exact duplication the crown's
// canopySkySheen already paid for once: this function used to call rainVisGrass
// AND shoreWet itself while main() called both again lines later, two
// independent lookups of the same answer at the same position (shoreWet is up
// to five taps). On the most overdrawn pass in the frame that is pure waste.
vec3 grassSkySheen(vec3 wp, float openness, float wetF)
{
    if (L.ibl_params.x < 0.004) return vec3(0.0);
    if (wetF < 0.02 && openness < 0.01) return vec3(0.0);
    const vec3 up = vec3(0.0, 1.0, 0.0);
    vec3  V   = normalize(L.eye_pos.xyz - wp);
    float NoV = clamp(dot(up, V), 0.0, 1.0);
    vec3  R   = reflect(-V, up);
    float rough = mix(0.75, 0.55, wetF);
    vec3  pref  = textureLod(uSkySpec, R, rough * L.ibl_params.z).rgb;
    // Very subtle dry, glistens wet — grass is near-ground and matte.
    return pref * EnvBRDFApprox(vec3(0.04), rough, NoV)
         * (L.ibl_params.x * L.ibl_params.y * openness * (0.05 + 0.95 * wetF));
}

// dynLightsFoliage → foliage_shadow.glsl (shared with the other foliage pass).

// ⭐ THE WHOLE BLOCK, from the shared header — not a hand-copied twin of
// detail.vert's (the two copies' field comments had already diverged; the
// offsets were next). Same cure as tree_gfx_push.glsl.
#include "detail_gfx_push.glsl"   // DetailConstants (224 B) — matches DetailGfxPushConstants

// ---- INTERPOLANTS: 8 components (was 13) — see detail.vert for the diet.
// vHemi/vSunOcc are `flat` (per-slot values from the instance record); the sun
// COLOUR is rebuilt here from the push constant instead of interpolated.
layout(location = 0) in vec2  vUV;
layout(location = 1) flat in float vHemi;    // baked per-slot hemi occlusion
layout(location = 2) in float vHeight;
layout(location = 3) flat in float vSunOcc;  // baked per-slot sun occlusion
layout(location = 4) in vec3  vWPos;
#ifdef GRASS_GI_INST
// r_grass_gi_inst 1 twin (detail_giinst.frag.spv): the probe field arrives
// PRECOMPUTED — one rgb9e5 sample per tuft from detail_generate.comp — instead
// of this shader walking ~16 atlas taps per fragment on the most overdrawn pass
// in the frame. Same cure, same reasons as the crown's r_tree_gi_inst (the
// per-fragment walk measured Trees 2.2 → 11.9 ms). The per-pixel twin stays
// compiled from this same source WITHOUT the define, so the A/B is two
// pipelines over identical resources, flipped live by the cvar.
#include "rgb9e5.glsl"
layout(location = 5) flat in uint vGI9;
#endif
layout(location = 0) out vec4 outColor;

// 1-tap sun shadow - see world_lmap.frag.
float sunShadow1(vec3 worldPos)
{
    vec4 c = L.sun_vp * vec4(worldPos, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 ndc = c.xyz / c.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    return (ndc.z - 0.0015 <= texture(uShadow, uv).r) ? 1.0 : 0.0;
}

// r_grass_debug 1..11 isolation views — a BODY include (needs the varyings, pc,
// L and sunShadow1 above). Debug-only; first against the wall when the grass
// grows its world-state variant mask.
#include "detail_debug.glsl"      // grassDebugColor

void main()
{
    vec4 diff = texture(uDiffuse, vUV, L.spot_flash.z);   // DLSS mip bias (0 native)
    // Alpha UNBIASED — the biased alpha mip shrinks blade coverage at the cutoff
    // (same black-holes physics as the tree crowns under DLSS upscaling).
    if (L.spot_flash.z != 0.0) diff.a = texture(uDiffuse, vUV).a;
    // Mip-fade compensation (r_grass_asharp, pc.vConsts.y), DISTANCE-driven: alpha
    // MIPs average the blade against its transparent background, so far grass
    // dissolves see-through. NOT lod-driven — textureQueryLod is high on thin
    // blades even right at the camera (minification is geometric, not distance),
    // which striped/fringed near grass in-game. Distance ramp: untouched inside
    // 15 m, full boost (1 + 4·asharp) by 60 m.
    // camDist: ONE length for the mip-fade, the AO parallax fade and the fog —
    // this used to be computed three times from the same two vectors.
    float camDist = length(vWPos - L.eye_pos.xyz);
    diff.a *= 1.0 + smoothstep(15.0, 60.0, camDist) * 4.0 * pc.vConsts.y;
    // Alpha-test cutoff is live-tunable via r_grass_aref (pc.vConsts.x). Lower =
    // fatter/denser blades (kills the "see-through" look); fall back to 0.5 if
    // the push slot is ever zero.
    float aref = pc.vConsts.x > 0.001 ? pc.vConsts.x : 0.5;
    if (diff.a < aref) discard;

#ifdef DETAIL_R1
    // nfs22 (Switch): R1 detail lighting -- the tuft's baked sun occlusion and sky openness times the environment's sun
    // and hemi colours (x1.5, the sky boost the crowns use), plus the flat ambient; fogged. No shadows, SH, GI, AO,
    // SSIL, dynamic lights, wetness, sheen or foam: the grass a handheld screen can afford, and what R1 drew.
    {
        vec3 r1 = diff.rgb * (pc.vSunColor.rgb * vSunOcc + L.hemi_color.rgb * (1.5 * vHemi * L.sky_params.y) + L.ambient.rgb);
        const float fogR1 = clamp(camDist * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
        outColor = vec4(mix(r1, L.fog_color.rgb, fogR1), 1.0);
        return;
    }
#endif

    // r_grass_debug: lighting-component isolation views (see grassDebugColor).
    int gdbg = int(L.spot_params.z + 0.5);
    if (gdbg > 0) {
        outColor = vec4(grassDebugColor(gdbg, diff.rgb), 1.0);
        return;
    }

    // r_ssao_debug 1: grass shows the APPLIED AO factor too (see world_frag_body).
    if (L.ao_params.w > 0.5) {
        outColor = vec4(vec3(gtaoVis()), 1.0);
        return;
    }
    // r_sf_debug 5: surface classification - grass (cyan).
    if (int(L.sf_params.y + 0.5) == 5) {
        outColor = vec4(SC_DebugColor(SC_Refine(SC_GRASS, vec3(0.0, 1.0, 0.0))), 1.0);
        return;
    }

    // rainVis: ONE tap of the rain map for the snow gate, the wet darken and
    // the sheen's wetness — this used to be three independent lookups of the
    // same answer at the same position (one of them buried inside the sheen).
    float rainVis = rainVisGrass(vWPos);

    // SNOW cover on the grass (Surface Field consumer, r_snow), gated by sky
    // exposure so grass under cover stays green.
    float snow = SC_SnowAmount(SC_Refine(SC_GRASS, vec3(0.0, 1.0, 0.0)), vec3(0.0, 1.0, 0.0), rainVis) * clamp(L.sf_params.w, 0.0, 1.0);
    diff.rgb = mix(diff.rgb, vec3(0.92, 0.94, 0.98), snow);

    // Rebuilt here instead of interpolated — same move as the crown: both
    // factors are constant across the draw (push constant × flat per-slot
    // scalar), so this is one multiply against three interpolants held for the
    // whole shader. Sun colour arrives pre-boosted (r_sun_boost).
    vec3 sunPart = pc.vSunColor.rgb * vSunOcc;
    if (dot(sunPart, sunPart) > 0.0) {
        // VSM (r_vsm) vs the 1-tap cascade - gated by shadow_params.w. Grass does NOT
        // read the screen-space mask (it belongs to the surface BEHIND the blade —
        // terrain shadows 20 m away painted themselves onto the bush): it samples
        // BOTH VSM atlases directly at the blade's own 3D position (static = trees/
        // buildings; dyn = grass/NPC with the r_grass_self_bias anti-acne slack).
        // Zero parallax by construction — same principle as the cascade path.
        float sunSh = (L.shadow_params.w > 0.5)
            ? vsmSunShadowGrassDirect(vWPos, L.spot_params.w)
            : sunShadow1(vWPos);
        sunPart *= sunSh;
    }

    // Ambient = sky-cube light x baked per-slot hemi occlusion (vColor.r). L.ambient
    // = real env ambient (carries r_ambient_floor). GTAO gates it (sun/dyn untouched).
    // GTAO/SSIL are SCREEN-SPACE, resolved on the opaque surface BEHIND the blade
    // (grass skips the prepass). Right next to that surface this is the correct
    // ambient (dark-corner case the gtaoVis comment describes); when the background
    // sits METERS behind the blade, its occlusion band (fence-base corner AO, a
    // building's shadowed wall) painted itself onto the lit foreground tuft — the
    // "shadow seen through the grass". Same parallax principle as the direct VSM
    // sun sampling above: fade both toward neutral by how far behind the blade the
    // mask's recorded surface sits (mask G = resolved surface view distance; VSM
    // sessions only — without the mask the distance source doesn't exist).
    float aoPar = 0.0;
    if (L.shadow_params.w > 0.5) {
        float mg = texture(uVsmMask, gl_FragCoord.xy * L.ao_params.xy).g;
        aoPar = smoothstep(0.15, 0.6, mg - camDist);
    }
    // The analytic sky fill this blade used to live on, kept as its own name because
    // the probe field REPLACES exactly this quantity — same rule, same reason as the
    // world receivers (adding would count the sky twice; see ddgi_receiver.glsl).
    //
    // ⚠ A BLADE HAS NO NORMAL. It is a crossed billboard, and every ambient term here
    // already answers "what arrives from straight up" (skyAmbientUp, the up-reflected
    // sheen). The field is sampled the same way for the same reason — asking it along
    // a fake per-vertex normal would make two blades of one tuft disagree about the
    // light in a way nothing in the scene justifies.
    // `vHemi` is the BAKED openness this shader already trusts for sky access, so
    // it plays the part bakeOcc x dynHemi plays in the world sum.
    const vec3 gUp   = vec3(0.0, 1.0, 0.0);
    const vec3 skyFillG = skyAmbientUp() * (vHemi * L.sky_params.y);
#ifdef GRASS_GI_INST
    // The field sample is per-TUFT (vGI9); the WEIGHT is rebuilt here at the
    // blade's own position — k = r_ddgi_light × volume fade, exactly what the
    // per-pixel path folded inside ddgiAmbient — so the volume-edge crossfade
    // stays per-pixel smooth. The fade is then REUSED for the flat-ambient gate
    // below (the tree does the same through vGI.w: taken once, never recomputed).
    // ddgiAmbientFromField keeps the genuinely per-pixel parts: the single
    // gather tap and the baked-sky multiply.
    const float giFadeG  = ddgiVolumeFade(vWPos);
    const vec3  ambFillG = ddgiAmbientFromField(skyFillG,
                               vec4(unpackRGB9E5(vGI9), L.ddgi_params.x * giFadeG), vHemi,
                               dot(vWPos - L.eye_pos.xyz, L.cam_dir.xyz));
    vec3 ambient = (ambFillG + L.ambient.rgb * (1.0 - L.ddgi_params.w * giFadeG))
#else
    const vec3 ambFillG = ddgiAmbient(skyFillG, vWPos, gUp,
                                      normalize(L.eye_pos.xyz - vWPos), vHemi);
    // r_gi_flat, in step with the world receivers: whatever share of the fill the
    // field is answering, the flat weather constant gives up. Grass and ground have
    // to fade it by the SAME factor or the ground darkens away from what stands on it.
    // ⚠ ddgiVolumeFade for the same reason as the world receivers (see
    // world_frag_body): frame-uniform, it strips the fill past the volume edge.
    vec3 ambient = (ambFillG + L.ambient.rgb * (1.0 - L.ddgi_params.w * ddgiVolumeFade(vWPos)))
#endif
                 * coloredAO(mix(gtaoVis(), 1.0, aoPar), diff.rgb)
                 * mix(ssilBoost(), vec3(1.0), aoPar);   // + SSIL bounce (ambient only, same parallax fade)

    vec3 col = diff.rgb * (ambient + sunPart + dynLightsFoliage(vWPos));

    // Wet grass: blades open to the rain darken slightly (+ a touch cooler).
    float wet = clamp(L.rain_params.y, 0.0, 1.0) * max(L.rain_params.z, 0.0);
    if (wet > 0.005) {
        float wv = wet * rainVis;
        col *= 1.0 - 0.5 * wv;
        col = mix(col, col * vec3(0.85, 0.92, 1.0), 0.5 * wv);
    }

    // Rain OR standing water — reeds grow out of a river and were bone dry to
    // the waterline, because every wet surface in this renderer was wet from
    // the sky. ONE shoreWet lookup, shared by the sheen, the soak darken and
    // the debug view below.
    float sw   = shoreWet(vWPos);
    float wetF = max(clamp(L.rain_params.y, 0.0, 1.0) * rainVis, sw);

    // Sky specular sheen (r_ibl) — additive, after the wet darken (reflected sky, not absorbed).
    vec3 sheen = grassSkySheen(vWPos, vHemi, wetF);
    col += sheen;

    // SOAKED IS DARK. The sheen alone does not read as wet — it is a grazing-angle
    // highlight, and a reed standing in a river is read by TONE first. Applied
    // after the lighting so it darkens the lit result, which is what water in the
    // blade does: it fills the surface pores and stops them scattering back.
    if (shoreWetDebug()) { outColor = vec4(sw, sw * 0.3, 0.0, 1.0); return; }   // red = wet
    // Wet vegetation is markedly darker than dry — more so than soil, because a
    // film of water on a blade kills the diffuse scatter that makes it look pale.
    col *= 1.0 - 0.65 * sw;
    // ...and then the foam the surge left ON it. Grass at a waterline is where the
    // scum line actually catches, so leaving this to the terrain alone would put the
    // foam under the very blades it hangs off. Lightening the LIT colour rather than
    // an albedo, because that is where this shader's wetness already works.
    float sf = shoreFoam(vWPos);
    if (sf > 0.003) col = mix(col, vec3(0.80, 0.81, 0.78) * (0.35 + 0.65 * vHemi), sf * 0.8);

    // Distance fog (R4).
    float fog = clamp(camDist * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
    col = mix(col, L.fog_color.rgb, fog);

    if (L.ibl_params.w > 0.5) { outColor = vec4(sheen, 1.0); return; }   // r_ibl_debug
    vec4 ptdbg = foliagePointDebug(vWPos);
    if (ptdbg.a > 0.5) { outColor = vec4(ptdbg.rgb, 1.0); return; }
    outColor = vec4(col, 1.0);
}

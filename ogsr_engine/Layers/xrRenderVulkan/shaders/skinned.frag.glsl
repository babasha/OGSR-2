#version 450
#extension GL_GOOGLE_include_directive : require
// Skinned binds the SHARED EnvLight descriptor set (vk_env_light.h, the very same
// VkDescriptorSetLayout the world uses) — only at set 2 instead of set 1, because
// set 1 carries the leaf's WorldMaterial. So every shared header works verbatim
// once the set index is pointed at 2; nothing here needs a private copy.
#define ENV_SET     2
#define VSM_SET     2
#define CLUSTER_SET 2
#include "light_ubo.glsl"         // DynLight + Lighting UBO (set 2 b0) + set-2 samplers
#include "vsm_sample.glsl"        // vsmSunShadow — screen-space VSM mask (set 2, binding 14)
#include "cluster_lights.glsl"    // clustered forward (set 2 bindings 17-19, r_clustered)
#include "shadow_common.glsl"     // spotShadowF/pointShadowF/cascTap/cascSample/sunShadow
#include "env_common.glsl"        // gtaoVis*/coloredAO/ssilBoost/skyAmbient/ibl* (pulls sky_ambient)
#include "nx_fog.glsl"            // 10-10: the fog fades into the sky behind (r_fog_sky)
#include "world_variants.glsl"    // SPEC_IBL/DEBUG/LAMPSH/RRGUIDE -- the world's frame variants (before light_shade)
#include "light_shade.glsl"       // shadeDynLight/dynLights
#include "rr_guides.glsl"         // DLSS Ray Reconstruction guide outputs (locations 1..3)
// skinned.frag - forward skinned shading with the diffuse texture.
// The leaf's WorldMaterial descriptor set (base/detail/lmap) is bound as set 1;
// we sample the base diffuse (binding 0) and light it with the REAL environment
// sun + hemi + ambient (set 2), so dynamics track time-of-day like the sky does.
// vertHW UVs are raw floats (no SHORT2 scale), so v_uv is used directly.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_nrm;
layout(location = 2) in vec3 v_wpos;   // world-space position (shadow lookup)
layout(location = 3) in float v_hudSun;  // 10-10 r_hud_vsun: the sun visibility of the HUD, per vertex (skinned.vert)

layout(location = 0) out vec4 o_color;

layout(set = 1, binding = 0) uniform sampler2D uTexDiffuse;

// Shared with skinned.vert; the fragment only reads hudMode (offset 76).
layout(push_constant) uniform PC {
    mat4  mvp;
    uint  skinMode;
    uint  baseBone;
    uint  boneCount;
    float hudMode;    // 1 = first-person HUD (hands/weapon)
    float hemi;       // sky-ambient gate (ray-traced sky visibility, 0..1)
} pc;

// Which half of this shader the pipeline runs (vk_pass_skinned bakes it): 1 = the world
// (NPCs, kinematics props), 2 = the first-person HUD (hands/weapon). The two share almost
// nothing -- the HUD has no world position, so no shadow map, AO, lamps, IBL or fog -- and
// a pipeline carrying both paid the registers of both. 0 (unspecialized) = decide per draw
// by pc.hudMode, the shader as it was.
layout(constant_id = 9) const int SPEC_SKIN_PATH = 0;

// Per-frame environment lighting (CEnvDescriptorMixer, filled in Pass_Skinned)
// now comes from light_ubo.glsl. sun_dir.xyz is X-Ray's direction the light
// TRAVELS (points downward, .y<0); toward the sun for N·L is therefore -sun_dir.
//
// This file used to hand-mirror that UBO: 50 fields, 24 of them `_pad_*` whose
// only job was to hold the offsets of the ~15 fields skinned actually reads. It
// was in sync — but nothing enforced that. std140 offsets are positional, so a
// field inserted anywhere in the middle of the real UBO silently shifts every
// later field here, with no compile error and no validation warning: the shader
// just reads a neighbouring vec4. One shared declaration cannot drift.

// GTAO visibility for SKINNED receivers. Deliberately NOT env_common.glsl's
// gtaoVis(): NPCs use HALF the strength exponent. AO normals come from half-res
// depth derivatives, which are noisy on small curvy skinned geometry (limbs,
// folds) - full strength reads as dark "dirt" speckle on characters. They still
// CAST full-strength AO onto the world.
float gtaoVisSkinned()
{
    float ao = textureLod(uAO, gl_FragCoord.xy * L.ao_params.xy, 0.0).r;
    return pow(clamp(ao, 0.0, 1.0), L.ao_params.z * 0.5);
}


// spotTileOf/pointCubeOf (light_ubo.glsl), spotShadowF/pointShadowF/cascTap/
// cascSample/sunShadow (shadow_common.glsl) and shadeDynLight/dynLights
// (light_shade.glsl) used to be copied in here, because those headers name the
// env descriptor set and skinned binds it at 2. ENV_SET settles that — and the
// copies had ALREADY drifted from the originals: see the header comment above.

void main()
{
    bool hud = (SPEC_SKIN_PATH == 2) || (SPEC_SKIN_PATH == 0 && pc.hudMode > 0.5);
    vec4 base = texture(uTexDiffuse, v_uv, L.spot_flash.z);   // DLSS mip bias (0 native)
    // Alpha UNBIASED — biased alpha mips shrink the cutout coverage (straps/hair
    // edges pixelate under DLSS upscaling; same physics as the tree-crown holes).
    if (L.spot_flash.z != 0.0) base.a = texture(uTexDiffuse, v_uv).a;

    // Collimator/red-dot sight marks (R4 hud_reddotsight: additive, unlit):
    // output the texture as-is — the additive pipeline ADDS it over the sight
    // glass; lighting/fog/alpha-test don't apply. ×2 keeps the mark vivid
    // through the HDR exposure+tonemap.
    if ((pc.skinMode & 16u) != 0u) {
        // Angle fade for light-beam planes (weapon torches, lamp selflight) — an
        // edge-on plane vanishes instead of showing as a hard line; collimator
        // marks are face-on when aiming, so they keep full brightness. The HUD's
        // v_wpos is camera-relative → the view vector is just -v_wpos there.
        vec3  Vv   = (hud && dot(v_wpos, v_wpos) < 9.0)
                   ? -normalize(v_wpos)
                   : normalize(L.eye_pos.xyz - v_wpos);
        float face = abs(dot(normalize(v_nrm), Vv));
        float w    = face * face;
        o_color = vec4(base.rgb * 2.0 * w, base.a * w);
        return;
    }

    // GLASS pane (skinMode bit 32 — kinematics furniture/door panes, blended
    // pipeline): keep the lit shading but skip the cutout test (semi-transparent
    // glass texels would all be discarded) and cap the blend alpha at the end.
    bool isGlass = (pc.skinMode & 32u) != 0u;
    if (!isGlass && base.a < 0.25)   // alpha-tested skinned parts (straps, hair, foliage)
        discard;

    // r_skin_flat_probe (SPEC_SKIN_PATH 3, 10-09 A/B): texture x const, the same alpha test -- what the NPC colour
    // pass costs without its per-pixel lighting. A specialization, so the rest compiles out of this pipeline.
    if (SPEC_SKIN_PATH == 3) {
        o_color = vec4(base.rgb * 0.5, 1.0);
        return;
    }

    // r_ssao_debug 1: NPCs draw the APPLIED factor — half strength, as they shade (never the HUD hands).
    if (SPEC_DEBUG && L.ao_params.w > 0.5 && !hud) {
        o_color = vec4(vec3(gtaoVisSkinned()), 1.0);
        return;
    }

    vec3  N     = normalize(v_nrm);
    vec3  toSun = normalize(-L.sun_dir.xyz);          // direction toward the sun
    float ndl   = max(dot(N, toSun), 0.0);            // directional (sun) term
    float hemiF = 0.6 + 0.4 * N.y;                    // up-facing surfaces see more sky

    // Sun shadow: only worth the PCF taps when the sun term is non-zero, and never
    // for the HUD (its bones are view-relative - v_wpos is not a world position).
    // The NPC is its OWN caster in the map - sample offset along the normal so
    // thin limbs don't self-shadow (acne would eat the whole sun term).
    if (ndl > 0.0 && !hud) {
        // VSM screen-space mask (r_vsm) vs cascade. NOTE: the VSM atlas has no skinned
        // casters yet → NPC self-shadow returns with that work; static-cast shadows
        // (building on NPC) are correct now and match the VSM-shadowed world.
        float sunSh = (L.puddle_geo.w > 0.5) ? pc.hemi      // no shadow maps: the object's own sky visibility stands in
                    : (L.shadow_params.w > 0.5) ? vsmSunShadow(gl_FragCoord.xy * L.ao_params.xy)
                                                : sunShadow(v_wpos + N * 0.05);
        ndl *= sunSh;
    }

    // Full env sun colour - the world shaders use it untinted (dynamic R4-style
    // N-L sun), so NPCs must match or they read "standing in the dark" next to
    // a warm-lit wall. (The old 60%-desaturate hack predates env-lit statics.)
    // sun_color/ambient arrive final from vk_env_light (r_sun_boost /
    // r_ambient_floor) - same values as the world shaders.
    vec3 sun = L.sun_color.rgb;

    // Hemisphere sky fill (R4 hmodel) for the body; the HUD keeps the flat hemi
    // term because its bones are view-relative (N would rotate with the camera,
    // so a sky-cube sample in N is meaningless there).
    // GTAO gates ambient/hemi like the world shaders - never for the HUD
    // (hands would inherit the AO of whatever wall is behind them on screen).
    vec3 occ = !hud ? coloredAO(gtaoVisSkinned(), base.rgb) * ssilBoost() : vec3(1.0);   // HUD path = vec3(1) → no IL on hands
    vec3 skyFill = !hud ? skyAmbient(N) * (L.sky_params.y * 0.7 * pc.hemi)
                                      : vec3(0.0);   // HUD: light fully replaced below
    vec3 light = L.ambient.rgb * occ
               + skyFill * occ
               + sun * ndl;

    // Dynamic point/spot lights - skip for the HUD (v_wpos is not world-space
    // there; the flat hudLight floor below keeps hands readable instead).
    if (!hud)
        light += dynLights(v_wpos, N);

    // First-person HUD (hands/weapon): the bones live in a view-relative space, so
    // dotting their normals against the world-space sun gives an unreliable (often
    // near-zero) N-L - hands look too dark. Floor the HUD with a flat, sun-direction-
    // independent term so they're consistently lit regardless of camera/sun angle.
    // The SUN part is gated by the shadow sampled at the CAMERA position: standing
    // in a light gap - warm sun-lit hands (the R4 look where hands carry orange
    // morning light), under a crown / indoors - hands drop to ambient. Whole-hand
    // modulation, not per-pixel dapples (HUD has no world positions), but it sells
    // "light through the foliage" where it matters most.
    if (hud) {
        // Per-pixel dapples: the HUD renders "camera at origin" (X-Ray shifts
        // the hud space by -camera to keep float precision), so v_wpos is the
        // WORLD-ORIENTED position RELATIVE to the camera - the true world
        // point is simply v_wpos + eye. Detect which convention the data is in
        // (near origin - camera-relative; near eye - already world) and
        // reconstruct; the shadow map then paints real light shafts through
        // the foliage onto the hands, like R4.
        vec3 hudWp;
        if (dot(v_wpos, v_wpos) < 9.0)                       // |v_wpos| < 3 - camera-relative
            hudWp = v_wpos + L.eye_pos.xyz;
        else if (distance(v_wpos, L.eye_pos.xyz) < 3.0)      // already world
            hudWp = v_wpos;
        else
            hudWp = L.eye_pos.xyz + vec3(0.0, 0.1, 0.0);     // unknown - camera point
        // 10-10 r_hud_vsun (hudMode 2): per vertex from skinned.vert -- 4 cascade taps on every HUD pixel were the fixed
        // cost of the first-person weapon (w.skinned 0.12 holstered / ~0.85 with a 15k-triangle gun drawn).
        float hudSun = (pc.hudMode > 1.5) ? v_hudSun
                     : (L.puddle_geo.w > 0.5) ? (L.puddle_geo.w - 1.0) : sunShadow(hudWp);   // no shadow maps: the camera's sun visibility
        // REPLACE the body-path light (it adds the sun UNSHADOWED for the HUD -
        // max() with it was erasing the dapples). ndl is approximate on HUD
        // normals, so the sun term keeps a floor instead of full N-L shaping.
        // 0.44 = old 0.55 - 1.25 - sun_color now arrives pre-boosted
        // (r_sun_boost), so the tuned HUD level is unchanged.
        light = L.ambient.rgb
              + L.hemi_color.rgb * hemiF * 1.4
              + sun * (0.44 + 0.44 * ndl) * hudSun;
    }

    vec3 col = base.rgb * light;
    float outA = 1.0;

    // Sky specular IBL (r_ibl) — NPC gear/skin reflects the sky + a sun glint.
    // Reflection needs only directions; the BODY's N/Vv are world-space (correct),
    // gated by ray-traced sky visibility (pc.hemi). HUD bones are view-relative, so
    // a directional reflection would swing with the camera → HUD gets a flat sky-tint
    // Fresnel rim instead (weapon/hands catch the sky colour at grazing angles).
    // BODY NPCs only. HUD hands/weapon are SKIPPED: their bones are view-relative,
    // so a sky reflection is meaningless there — the earlier flat Fresnel rim just
    // painted a pale "waxy" edge on grazing hand/bolt surfaces. Body N/Vview are
    // world-space (correct), gated by ray-traced sky visibility (pc.hemi).
    vec3  specIBL = vec3(0.0);
    float specE   = 0.0;
    if (SPEC_IBL && !hud) {
        vec3 Vview = normalize(L.eye_pos.xyz - v_wpos);
        specIBL  = iblSpecular(N, Vview, 0.6, vec3(0.04)) * pc.hemi;
        specIBL += sun * sunSpec(N, Vview, normalize(-L.sun_dir.xyz), 0.6, vec3(0.04)) * max(ndl, 0.0);
        specE    = iblSpecWeight(N, Vview, 0.6, vec3(0.04)) * pc.hemi;
    }
    // Energy: what the gear mirrors away it does not also transmit. NPCs keep their
    // sky glint — it just stops being free light added on top of full diffuse.
    col = col * (1.0 - specE) + specIBL;

    // GLASS pane (kinematics furniture/doors/vehicle windows, NPC glasses) —
    // R4 model_env_lq.ps: colour = light × lerp(ENV REFLECTION, texture, a),
    // blend alpha = the texture's own alpha. Clean glass ≈ invisible + sheen.
    // (Lit-blend lightplanes leaves are never drawn — Pass_LightCones replaces
    // them with real volumetric beams; DrawSkinnedList skips the draw.)
    if (isGlass) {
        // GLASS: R4 base + fresnel + sun glint — see world_lmap.frag for the note.
        float aG   = min(base.a, L.pom_params5.y);
        vec3  V    = normalize(v_wpos - L.eye_pos.xyz);
        vec3  Rv   = reflect(V, N);
        float NoV  = clamp(dot(N, -V), 0.0, 1.0);
        float fres = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);
        float xf   = clamp(L.sky_params.x, 0.0, 1.0);
        vec3  env  = textureLod(uSky0, Rv, 1.0).rgb;
        if (xf > 0.01) env = mix(env, textureLod(uSky1, Rv, 1.0).rgb, xf);
        vec3 glint   = sun * (pow(max(dot(Rv, toSun), 0.0), 128.0) * 2.0 * clamp(ndl * 3.0, 0.0, 1.0));
        // Reflection modulated by the LOCAL lighting; fresnel coverage weighted by
        // reflection brightness (no dark "tint" film indoors) — see world_lmap.frag.
        vec3 reflCol = env * light * (0.5 + fres) + glint;
        float fCov = fres * clamp(dot(reflCol, vec3(0.6)), 0.0, 1.0);
        float aOut = clamp(aG + fCov * (1.0 - aG), 1e-4, 1.0);
        col  = (base.rgb * light * aG + reflCol * fCov * (1.0 - aG)) / aOut;
        outA = aOut;
    }

    // Distance fog (R4) - see world_lmap.frag. NEVER on the HUD: its v_wpos is
    // view-relative, not a world position, so the distance would be garbage.
    if (!hud) {
        float fog = clamp(length(v_wpos - L.eye_pos.xyz) * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
        if (fog > 0.0) col = mix(col, nxFogColor(v_wpos, fog), fog);   // 10-10 r_fog_sky: into the sky behind
        if (isGlass) outA *= (1.0 - fog) * (1.0 - fog);   // R4: alpha fades with fog²
    }

    // ---- RR GUIDE G-BUFFER ------------------------------------------------
    // ⭐This is the class that matters INDOORS. It is not only NPCs: every
    // kinematics prop — doors, lamps, wires, handles, screens, furniture — renders
    // through this shader, and a room is made of them. While they wrote no guides
    // they read to RR as "flat grey material with no surface", which is exactly the
    // signal that makes a denoiser produce blotches.
    // ⚠GLASS excluded: it is blended over what is behind it, so the guide must keep
    // describing the opaque surface underneath (same rule as world_frag_body).
    // ⚠ROUGHNESS 0.6 — the same constant this shader's own IBL/sun-spec uses above.
    // Skinned material carries no gloss map here; inventing a second number would
    // put the guide at odds with the specular the frame actually drew.
    // ⚠HUD (hands/weapon): its bones are view-attached, so N swings with the camera
    // — but it swings the way a held weapon really does, and v_wpos is documented as
    // WORLD-ORIENTED (camera-relative by translation only). So the vector is in the
    // buffer's space, and writing it beats leaving the centre of every frame blank.
    if (SPEC_RRGUIDE && !isGlass) {
        vec3 Vg = (hud && dot(v_wpos, v_wpos) < 9.0)
                ? -normalize(v_wpos)
                : normalize(L.eye_pos.xyz - v_wpos);
        rrWriteGuides(N, 0.6, base.rgb, vec3(0.04), Vg);
    }

    if (SPEC_DEBUG && L.ibl_params.w > 0.5) { o_color = vec4(specIBL, 1.0); return; }   // r_ibl_debug
    o_color = vec4(col, outA);
}

#version 450
// xrRenderVulkan — tree forward vertex shader (Session B).
//
// GPU-driven indirect draw: the cull compute wrote firstInstance = global
// tree index into each draw command, so gl_InstanceIndex selects this tree's
// world transform from the transforms SSBO (set 0). Vertex format matches the
// level VB pool: FLOAT3 position @ 0, SHORT2 SSCALED UV @ tcOffset (24 or 28,
// picked by the pipeline variant).
//
// Matrix convention: raw X-Ray (row-major) Fmatrices loaded into column-major
// GLSL mat4 transpose, so `M * v` == X-Ray `v * F`. Chaining
// `mViewProj * (xform * pos)` composes local→world→clip correctly (same as the
// monolith gbuffer_tree_indirect.vert).

layout(location = 0) in vec3 aPos;   // local-space position
layout(location = 1) in vec2 aUV;    // SHORT2 SSCALED raw int16 → float

#include "tree_instance.glsl"   // TreeInstance + XformBuf (set 0 b0) — matches VK::GpuTreeInstance

#ifdef TREE_GI_INST
// r_tree_gi_inst 1 (tree_giinst.vert.spv) — the probe field arrives PRECOMPUTED,
// two samples per tree from tree_gi.comp, instead of ~16 atlas taps per vertex.
// See that shader for the measurement that produced this: the driver reported the
// crown VS at 56 registers / 20224 bytes with the lookup and 32 / 5376 without it,
// and that difference is the 0.45 ms the per-term sweep had been chasing.
// Layout: [3i+0] rgb=field at crown bottom, w=weight; [3i+1] rgb=field at top;
//         [3i+2] x=bottom world Y, y=1/(top-bottom).
layout(set = 0, binding = 5, std430) readonly buffer TreeGIBuf { vec4 treeGI[]; };   // 3/4 are the cube tier
#endif

// ⭐THE PROBE FIELD IS SAMPLED HERE, NOT IN THE FRAGMENT. 📏Per-fragment it cost the
// Trees pass 2.2 -> 11.9 ms (overdraw x 16 atlas taps); per-vertex it is nothing, and
// the interpolator gives the crown the vertical gradient that reads as depth.
#define ENV_SET 2                // EnvLight is bound at set 2 for trees (same as tree.frag)
#include "light_ubo.glsl"        // Lighting UBO (set 2 b0) — L.ddgi_params + eye_pos
#include "ddgi_field.glsl"       // ddgiFieldWeighted (set 2 b34..36, vertex-callable)

#include "tree_gfx_push.glsl"   // TreeGfxPush (160 B) — matches VK::TreeGfxPush

// SSFX tree wind (trunk sway + crown flutter). Shared with the shadow caster
// (tree_depth.vert) so the bent geometry and its shadow stay in sync.
#include "ssfx_tree_wind.glsl"

// ---- INTERPOLANTS: 10 components. Must match tree.frag exactly ----------------
// Three outputs were removed here because the fragment shader could not use them:
// vLight was a vec3 with .yz nailed to 0, vSunLit was a push constant times the
// one number vLight actually carried, and vTreeIdx fed a diagnostic that no longer
// exists. On the most overdrawn surface in the frame each of those was a VS store,
// a parameter slot and a live register for the whole fragment shader.
layout(location = 0) out vec2 vUV;
#ifdef TREE_R1
#include "shadow_math.glsl"     // nfs43: cascTap
#include "nx_sun_shadow.glsl"   // nfs43: the live sun shadow, per vertex
#define NX_WET_VERTEX
#include "nx_wet.glsl"           // v317: the wet crown, per vertex
#include "nx_fog.glsl"           // 10-10: the fog fades into the sky behind (r_fog_sky), per vertex
// nfs31 R1 crown (tree_r1.vert.spv, Switch): the whole light of the crown and its fog, per vertex -- see main().
layout(location = 1) out vec3  vLight;
layout(location = 2) out vec3  vFogC;   // 10-10: the fog's share of the colour (nxFogColor x fog); vLight carries (1 - fog)
#else
layout(location = 1) flat out float vHemi;   // per-tree baked sky openness — FLAT: it comes from the instance
layout(location = 2) out vec3 vWPos;         // world-space position (shadow lookup)
// rgb = the probe field at this vertex, w = how much of the ambient it owns (0 =
// outside the volume / feature off → the fragment keeps its analytic sky fill).
layout(location = 3) out vec4 vGI;
#endif

// R1 crossfade with the FLOD billboard (pc.lodBand > 1, the Switch preset): how much of this crown its group's
// billboard has already taken over, 0..1, per TREE -- the fragment dithers that share of the crown OUT with the same
// noise the billboard dithers IN with (lod_imposter.frag), so a pixel shows one of the two, never both. Same group
// centre and quantised k (w, 0 = no billboard group) the cull cuts the mesh by and the billboard fades by.
layout(set = 0, binding = 2, std430) readonly buffer TreeLodC { vec4 treeLodC[]; };
layout(location = 4) flat out float vLodFade;

void main()
{
    TreeInstance t = inst[gl_InstanceIndex];

    vec4 worldPos = t.xform * vec4(aPos, 1.0);
    // SSFX tree wind (trunk sway + crown flutter). baseY = tree origin world Y
    // (xform translation) = per-tree phase; H = vertex height above base; tc_y =
    // the scaled UV (fades flutter toward the trunk base).
    float baseY = t.xform[3].y;
    float H     = worldPos.y - baseY;
    float tc_y = aUV.y * pc.uvScale;
    worldPos.xyz += ssfxTreeWindWorld(worldPos.xyz, H, tc_y, t._p0, baseY,
                                      pc.wind_params, pc.wsetup_trees, pc.wind_anim);
    gl_Position   = pc.mViewProj * worldPos;

    vUV = aUV * pc.uvScale;
    // ⚠ AFTER the wind displacement, so the sample follows the vertex that will
    // actually be shaded — a crown bending a metre in the wind would otherwise read
    // the light of the place it is not.
    // ⚠ Sampled UP, like every other ambient term on foliage: a crown card has no
    // usable normal (see detail.frag for the same call and the same reason).
#ifdef TREE_R1
    // nfs31: what tree.frag (TREE_SKY_PUSH, variant sun+sky+fog, sun shadow gated off) computes after its alpha test,
    // moved here term for term: sky fill * 0.75 + ambient + sun * 0.88, all scaled by the per-tree openness, and the
    // fog factor. coloredAO(1, albedo) is 1 (+-1e-4) and drops out. Only the fog varies inside a tree, and linearly
    // enough to interpolate. The C++ side binds this only while that variant is the one the world state asks for.
    {
        const float hemi = clamp(t.c_scale_hemi + t.c_bias_hemi, 0.3, 1.5);
        float sunVisV = 1.0;   // nfs43: per-vertex live sun shadow (puddle_geo.w > 0.5 = no maps this frame)
        if (L.puddle_geo.w < 0.5) { float cov; sunVisV = nxSunShadowCov(worldPos.xyz, cov); }
        vLight = pc.vHemiColor.rgb * (hemi * L.sky_params.y) * 0.75 + L.ambient.rgb + pc.vSunColor.rgb * (hemi * 0.88 * sunVisV);
        const float camDist = distance(worldPos.xyz, L.eye_pos.xyz);
        const float fog = clamp(camDist * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
        // 10-09 the dynamic lights (the flashlight at night), per vertex like the rest of this crown: tree.frag's
        // dynLightsFoliage walk and its distance tier, without the 1-tap shadow (texture() is fragment-only). Before,
        // any light in reach sent EVERY crown to the full tree.frag: Trees 1.2 -> 6-8 ms with the torch on (v279).
        const float tier = (pc.shadeDist <= 0.0) ? 1.0 : 1.0 - smoothstep(pc.shadeDist * 0.75, pc.shadeDist, camDist);
        const int   nDyn = int(L.counts.x + 0.5);
        for (int i = 0; i < nDyn && tier > 0.0031; ++i) {
            const vec3  dv = L.lights[i].pos.xyz - worldPos.xyz;
            const float r  = L.lights[i].pos.w;
            const float d2 = dot(dv, dv);
            if (d2 >= r * r) continue;
            const float d = sqrt(max(d2, 1e-6));
            float att;
            if (L.lights[i].color.w > 0.5 && L.lights[i].dir.w > 0.87) { att = 1.0 - d2 / (r * r); att *= att; }
            else                                                          { att = 1.0 - d / r;       att *= att; }
            if (L.lights[i].color.w > 0.5) {
                const float ca = dot(-dv / d, L.lights[i].dir.xyz);
                const float ci = L.lights[i].dir.w;
                if (ci > 0.87) {
                    const float co = 2.0 * ci * ci - 1.0;
                    att *= clamp((ca - co) / max(ci - co, 1e-3), 0.0, 1.0);
                } else
                    att *= clamp((ca - ci) / max(1.0 - ci, 1e-3), 0.0, 1.0);
            }
            vLight += L.lights[i].color.rgb * (att * 0.7 * tier);
        }
        vLight *= nxWetFoliageLight(worldPos.xyz, hemi);   // v317: a wet crown is a darker albedo -- under every light
        // 10-10 the fog as mix(diff * light, fogColor, fog) split across the stages: the FS adds vFogC to diff * vLight
        vFogC  = (fog > 0.0) ? nxFogColor(worldPos.xyz, fog) * fog : vec3(0.0);
        vLight *= 1.0 - fog;
    }
#elif defined(TREE_GI_INST)
    // The gradient the comment above is about is PRESERVED, just carried by two
    // per-tree samples instead of one lookup per vertex: lerp bottom->top by this
    // vertex's height. Everything past this line is unchanged, so the A/B against
    // the per-vertex path compares two ways of answering, not two pictures.
    {
        const vec4  giLo  = treeGI[3u * uint(gl_InstanceIndex) + 0u];
        const vec4  giHi  = treeGI[3u * uint(gl_InstanceIndex) + 1u];
        const vec4  giMap = treeGI[3u * uint(gl_InstanceIndex) + 2u];
        const float h     = clamp((worldPos.y - giMap.x) * giMap.y, 0.0, 1.0);
        vGI = vec4(mix(giLo.rgb, giHi.rgb, h), giLo.w);
    }
#else
    vGI = ddgiFieldWeighted(worldPos.xyz, vec3(0.0, 1.0, 0.0),
                            normalize(L.eye_pos.xyz - worldPos.xyz));
#endif
    // c_scale.hemi defaults to ~1.0 (lit), c_bias.hemi ~0 → per-tree openness factor.
    // The openness scalar goes to the fragment as vHemi — it lights the crown with
    // the SAME sky-cube ambient the world ground uses (see tree.frag), so
    // trees/bushes track the upgraded world lighting instead of the old flat hemi
    // (which read dark next to the sky-lit terrain).
    // The sun part is NOT emitted any more: it was pc.vSunColor.rgb * hemiFac * 0.88,
    // and the fragment rebuilds it from this same number in two multiplies (that ×0.88
    // is the tuned crown level — foliage ran ×1.1 against the world's ×1.25 — so it
    // still lives, in tree.frag, next to the shadow tap that attenuates it).
#ifndef TREE_R1
    vHemi = clamp(t.c_scale_hemi + t.c_bias_hemi, 0.3, 1.5);
    vWPos = worldPos.xyz;
#endif
    // Billboard's opacity at this crown's group, exactly as lod_imposter*.vert computes it: 0 at D_A = base * k,
    // 1 at D_A * band (where tree_cull stops drawing the mesh).
    vLodFade = 0.0;
    if (pc.lodBand > 1.0001) {
        const vec4 g = treeLodC[gl_InstanceIndex];
        if (g.w > 0.0) {
            const float dA = pc.vSunColor.w * g.w, dB = dA * pc.lodBand;
            vLodFade = clamp((distance(g.xyz, L.eye_pos.xyz) - dA) / max(dB - dA, 1e-3), 0.0, 1.0);
        }
    }
}

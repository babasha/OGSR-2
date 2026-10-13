#version 450
// xrRenderVulkan — detail (grass) vertex shader.
// Per-vertex (binding 0): pos + uv + height
// Per-instance (binding 1): 3 transform rows + color (sun, reserved, objId, hemi)
//
// Character interaction is LIVE: the CPU feeds vInteractors[0] = player and
// [1..3] = the nearest NPCs every frame (vk_DetailManager_Render.cpp); slots
// with radius 0 are skipped. ⚠ The MV pass (detail_motion.vert) does NOT
// replicate the bend — blades pressed aside by an actor lose their sway MV
// there (depth no longer bit-matches). Known gap, fix belongs to the MV pass.

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec2  aUV;
layout(location = 2) in float aHeight;
layout(location = 3) in vec4  aInstRow0;
layout(location = 4) in vec4  aInstRow1;
layout(location = 5) in vec4  aInstRow2;
layout(location = 6) in vec4  aInstColor;   // (sun, giPacked, objId, hemi)
// The per-tuft probe-field sample detail_generate packed into color.g — declared
// as a SECOND attribute over the same bytes (R32_UINT @ offset 52, binding 1) so
// the rgb9e5 payload is fetched as raw bits. Reading it through aInstColor.g +
// floatBitsToUint would push the bits through a FLOAT attribute fetch, where a
// payload that happens to look like NaN/denormal has no bit-preservation
// guarantee — the same class of mine as the push-constant offsets.
layout(location = 7) in uint  aGIField;

// ⭐ THE WHOLE BLOCK, from the shared header — not a hand-copied twin of
// detail.frag's. Same cure as tree_gfx_push.glsl.
#include "detail_gfx_push.glsl"   // DetailConstants (224 B) — matches DetailGfxPushConstants
#ifdef DETAIL_R1V
#include "light_ubo.glsl"         // nfs32 R1V: L.hemi_color / ambient / sky_params / fog / eye_pos (set 1 b0)
#include "shadow_math.glsl"       // nfs43: cascTap
#include "nx_sun_shadow.glsl"     // nfs43: the live sun shadow, per vertex
#define NX_WET_VERTEX
#include "nx_wet.glsl"             // v317: the wet grass, per vertex
#include "nx_fog.glsl"             // 10-10: the fog fades into the sky behind (r_fog_sky), per vertex
#endif

#include "ssfx_wind.glsl"   // SSFX flow-map wind (s_waves @ set0 binding1)

// ---- INTERPOLANTS: 8 components (was 13) — the same diet the crown went
// through. The two baked per-slot occluders come from the INSTANCE record, so
// they are `flat`: every vertex of a tuft carries the identical value, and
// interpolating one was a weighted average of copies of one number. The old
// vColor was a vec4 carrying ONE number in .r with .gba hard-coded; the old
// vSunLit (vec3) was pc.vSunColor.rgb * that scalar — a push constant times a
// flat float, which the fragment now rebuilds with one multiply instead of
// carrying three interpolated components for the whole shader.
layout(location = 0) out vec2  vUV;
#ifdef DETAIL_R1V
// nfs32 R1V grass (detail_r1v.vert.spv, Switch): the tuft light, the alpha mip-fade factor and the fog, per vertex.
layout(location = 1) out vec3  vLight;
layout(location = 2) out float vFade;
layout(location = 3) out vec3  vFogC;   // 10-10: the fog's share of the colour (nxFogColor x fog); vLight carries (1 - fog)
#else
layout(location = 1) flat out float vHemi;    // baked per-slot hemi occlusion (aInstColor.a)
layout(location = 2) out float vHeight;
layout(location = 3) flat out float vSunOcc;  // baked per-slot sun occlusion (aInstColor.r)
layout(location = 4) out vec3  vWPos;         // world-space position (shadow lookup)
// rgb9e5 probe-field sample, once per tuft (detail_generate). Consumed only by
// the GRASS_GI_INST fragment twin; the per-pixel twin simply leaves the output
// unread, which the interface rules allow — ONE vertex shader serves both.
layout(location = 5) flat out uint  vGI9;
#endif

vec3 ApplyInteraction(vec3 pos, float h)
{
    if (h < 0.01) return pos;
    for (int i = 0; i < 4; ++i) {
        float r = pc.vInteractors[i].w;
        if (r < 0.01) continue;
        vec2 d = pos.xz - pc.vInteractors[i].xz;
        float dist = length(d);
        if (dist < r && dist > 0.01) {
            float t = 1.0 - dist / r;
            float s = t * t * h * 0.7;
            pos.xz += (d / dist) * s;
            pos.y  -= s * 0.25;
        }
    }
    return pos;
}

void main()
{
    // Reconstruct 3×4 instance transform (row-major rows in a column-major
    // mat4x3: 3 column-vectors + a translation column).
    mat4x3 inst = mat4x3(
        aInstRow0.xyz, aInstRow1.xyz, aInstRow2.xyz,
        vec3(aInstRow0.w, aInstRow1.w, aInstRow2.w));

    vec3 worldPos = inst * vec4(aPos, 1.0);
    // SSFX flow-map wind (replaces the old sine ApplyWind). Sample at the base
    // world position, drift by Environment.wind_anim.
    WindSetup W = ssfx_wind_setup(pc.wind_params, pc.wsetup_grass, pc.wind_anim.w);
    // wind_params.w = per-detail-type wind scale, pushed each draw: 0 for models
    // flagged DO_NO_WAVING (the tiny "asphalt" plants R4 keeps static), 1 for
    // normal grass. Without this the SSFX wind stretched those micro-meshes.
    worldPos     += ssfx_wind_grass(worldPos, aHeight, W, pc.wind_anim.xy) * pc.wind_params.w;
    worldPos      = ApplyInteraction(worldPos, aHeight);

    // (A trail press-down used to read aInstColor.g here. It was a PROVEN no-op:
    // the TrailMap feeding it is a 1×1 black placeholder and a writer for it
    // never existed — the gen shader multiplied a tap of constant black into
    // every instance. Deleted 26-08 with the tap; aInstColor.g is reserved 0.
    // Re-adding the feature starts with building the trail-map WRITER.)

    gl_Position = pc.mViewProj * vec4(worldPos, 1.0);
    vUV         = aUV;

    // Baked per-slot scalars for the fragment: hemi occlusion is lit there with
    // the SAME sky-cube ambient the world ground uses (skyAmbientUp), so grass
    // doesn't read dark against the terrain it grows on; the sun side is
    // rebuilt in the fragment as pc.vSunColor.rgb * vSunOcc and shadowed there.
#ifdef DETAIL_R1V
    // nfs32: what detail.frag -DDETAIL_R1 computes per pixel, term for term (sun * baked sun occlusion + hemi * 1.5 *
    // openness * sky scale + flat ambient; the distance-driven alpha mip compensation; the fog).
    {
        const float camDist = length(worldPos - L.eye_pos.xyz);
        float sunVisV = 1.0;   // nfs43: live sun shadow at the blade (puddle_geo.w > 0.5 = no maps this frame)
        if (L.puddle_geo.w < 0.5) { float cov; sunVisV = mix(1.0, nxSunShadowCov(worldPos, cov), cov); }
        vLight = pc.vSunColor.rgb * (aInstColor.r * sunVisV) + L.hemi_color.rgb * (1.5 * aInstColor.a * L.sky_params.y) + L.ambient.rgb;
        vLight *= nxWetFoliageLight(worldPos, aInstColor.a);   // v317: wet grass is a darker albedo
        vFade  = 1.0 + smoothstep(15.0, 60.0, camDist) * 4.0 * pc.vConsts.y;
        // 10-10 the fog as mix(diff * light, fogColor, fog) split across the stages: the FS adds vFogC to diff * vLight
        const float fog = clamp(camDist * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
        vFogC  = (fog > 0.0) ? nxFogColor(worldPos, fog) * fog : vec3(0.0);
        vLight *= 1.0 - fog;
    }
#else
    vHemi   = aInstColor.a;
    vSunOcc = aInstColor.r;
    vWPos   = worldPos;
    vHeight = aHeight;
    vGI9    = aGIField;
#endif
}

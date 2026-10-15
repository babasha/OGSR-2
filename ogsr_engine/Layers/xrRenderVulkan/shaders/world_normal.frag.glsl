#version 450
// xrRenderVulkan — NORMAL G-buffer fragment for STATIC level geometry.
//
// Output: world-space normal *0.5+0.5 in rgb; ALPHA IS A SOURCE TAG, not just
// a valid flag:
//   1.0 = skinned  (shadow_skinned_normal.frag) — GTAO bends it toward the viewer
//   0.5 = static   (this shader)                — GTAO takes it as-is
//   0.0 = no normal (the clear)                 — GTAO falls back to depth recon
//
// The tag earns its keep: the grazing bend (kMinNdV in ssao.frag) exists because
// half-res depth horizons collapse the visibility integral on NPC silhouettes.
// On a flat floor the opposite is true — an unoccluded plane integrates to
// exactly 1.0 only while N agrees with its own horizons, so bending N there
// RE-CREATES the self-occlusion that real normals are here to remove (that
// self-occlusion is what r_ssao_bias has been fading away).

layout(location = 0) in vec3 vNormal;
layout(location = 2) in float vHemi;   // baked sky access (NORMAL.a) — see the vert
#ifdef NORMAL_AT
layout(location = 1) in vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2D uDiffuse;   // WorldMaterial set, binding 0

// Only the AT twin declares a push block: the solid pipeline's layout exposes
// the range to the VERTEX stage alone.
layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 mi;
    vec4 mj;
    vec4 misc;   // .x = alphaRef
} pc;
#endif

layout(location = 0) out vec4 outNormal;

void main()
{
#ifdef NORMAL_AT
    if (texture(uDiffuse, vUV).a < pc.misc.x)
        discard;
#endif
    // The static tag now CARRIES the baked hemi: a = 0.30 + hemi * 0.45, range
    // [0.30 .. 0.75]. Every reader of the tag tests thresholds, not equality
    // (verified 28-08: ssao.frag and gbuffer_recon use `> 0.25` for "has a real
    // normal" and `> 0.75` for "skinned") — both still hold: statics stay above
    // 0.25 at hemi 0 and top out at 191/255 = 0.749 < 0.75 at hemi 1. The
    // gather decodes the hemi back and bounds its distant-ambient fallback by
    // it — the OFFLINE answer to "how enclosed is this texel", any distance.
    outNormal = vec4(normalize(vNormal) * 0.5 + 0.5, 0.30 + clamp(vHemi, 0.0, 1.0) * 0.45);
}

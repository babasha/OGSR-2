// xrRenderVulkan — GPU-skinned depth caster VS, shared by three variants that
// form a strict ladder. The skinning matrix S is the SAME maths in all of them
// (and in skinned.vert): the color pass re-rasterizes these exact positions with
// LEQUAL, so the depths must match BIT-FOR-BIT — which is precisely why this must
// be one source and not three copies that can drift apart independently.
//
//   (no define)      sun shadow caster, depth only.
//   CASTER_UV        depth PREPASS caster, alpha-tested: passes UV so the FS can
//                    discard at the SAME threshold as skinned.frag (a < 0.25).
//                    Without it, hair/strap cutouts leave opaque depth and the
//                    early-Z'd color pass punches fog-coloured holes around them.
//   CASTER_NORMAL    (implies CASTER_UV) NORMAL G-buffer caster for NPCs: also
//                    skins the vertex normal and outputs it in WORLD space, so
//                    GTAO gets real per-pixel normals instead of the noisy
//                    depth-derivative ones (kills the "dirt"/speckle on NPCs).
//
// Bones are uploaded PRE-MULTIPLIED by the object's world matrix, so S*pos is
// already world-space and pc.mvp is the plain view·proj (light's for the sun
// caster, camera's for the prepass/normal variants).
#ifndef SHADOW_SKINNED_BODY_GLSL
#define SHADOW_SKINNED_BODY_GLSL

#ifdef CASTER_NORMAL
#define CASTER_UV
#endif

layout(location = 0) in vec4 a_Position;     // FLOAT4 xyz=pos
layout(location = 1) in vec4 a_Normal;       // u8x4 unorm: xyz=normal (CASTER_NORMAL), a=boneIdx(1W)/w0(2W+)
layout(location = 2) in vec4 a_TexCoordExt;  // FLOAT2 (36/40) or FLOAT4 (44): zw=idx (44)
layout(location = 3) in vec4 a_Tangent;      // u8x4 unorm: a=w1 (3W/4W)
layout(location = 4) in vec4 a_Binormal;     // u8x4 unorm: a=idx2 (3W) / w2 (4W)
layout(location = 5) in vec4 a_BoneIndices;  // u8x4 unorm: 4 idx (4W only)

#ifdef CASTER_UV
layout(location = 0) out vec2 vUV;
#endif
#ifdef CASTER_NORMAL
layout(location = 1) out vec3 vNormal;       // world-space
#endif

layout(push_constant) uniform PC {
    mat4  mvp;       // view·proj (bones are already world-space)
    uint  skinMode;  // 1=1W,2=2W,3=3W,4=4W
    uint  baseBone;
    uint  boneCount;
    float hudMode;   // unused here
} pc;

#include "skin_bones.glsl"     // bones[]: SSBO, or the dynamic-UBO window under SKIN_BONES_UBO

#include "skin_matrix.glsl"   // dN/dF/clampB + skinMatrix() — shared by all skinned passes

void main()
{
    vec3 pos = a_Position.xyz;

    mat4 S = skinMatrix();

#ifdef CASTER_NORMAL
    vec3 nrm = a_Normal.xyz * 2.0 - 1.0;     // object-space unit normal (same decode as skinned.vert)
    vNormal  = mat3(S) * nrm;                // bone-skinned → world space (normalized in the FS)
#endif
#ifdef CASTER_UV
    vUV      = a_TexCoordExt.xy;
#endif
    gl_Position = pc.mvp * (S * vec4(pos, 1.0));
}

#endif // SHADOW_SKINNED_BODY_GLSL

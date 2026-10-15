#version 450
// ============================================================================
// skinned.vert — FORWARD GPU-skinning vertex shader for OGSR Vulkan (Step B/2).
// Bone math + vertHW attribute layout ported from the monolith's
// gbuffer_skinned.vert (monolith is deferred; this is the forward adaptation:
// no motion vectors, single MVP push, flat output for skinned.frag).
//
// vertHW formats (vk_Visual.h), per skinMode:
//   1W (stride 36): P FLOAT4 | N_I u8x4 (a=boneIdx) | T u8x4 | B u8x4 | tc FLOAT2
//   2W (stride 44): P | N(a=w0) | T | B | tc_i FLOAT4 (zw = idx0,idx1 as float)
//   3W (stride 44): P | N(a=w0) | T(a=w1) | B(a=idx2) | tc_i FLOAT4 (zw = idx0,idx1)
//   4W (stride 40): P | N(a=w0) | T(a=w1) | B(a=w2) | tc FLOAT2 | indices u8x4 (rgba=idx)
// Bone index stored RAW (0..BoneCount-1) -> direct index into bones[].
// bones[] holds the Fmatrix render transforms written row-major; GLSL reads them
// column-major, which transposes, so `bones[i] * v` == X-Ray's `v * Mbone`.
// Bones are PRE-MULTIPLIED by the object's world matrix at upload, so S*pos is
// WORLD-space: pc.mvp is the plain viewProj, v_wpos feeds the shadow lookup,
// and normals carry the object rotation.
// ============================================================================

layout(location = 0) in vec4 a_Position;     // FLOAT4 xyz=pos
layout(location = 1) in vec4 a_Normal;       // u8x4 unorm: xyz=normal, a=boneIdx(1W)/w0(2W+)
layout(location = 2) in vec4 a_TexCoordExt;  // FLOAT2 (36/40) or FLOAT4 (44): xy=uv, zw=idx (44)
layout(location = 3) in vec4 a_Tangent;      // u8x4 unorm: a=w1 (3W/4W)
layout(location = 4) in vec4 a_Binormal;     // u8x4 unorm: a=idx2*3 (3W) / w2 (4W)
layout(location = 5) in vec4 a_BoneIndices;  // u8x4 unorm: 4 idx*3 (4W only)

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_nrm;
layout(location = 2) out vec3 v_wpos;   // world-space position (shadow lookup)

layout(push_constant) uniform PC {
    mat4  mvp;       // offset 0:  world->clip (bones are pre-multiplied to world)
    uint  skinMode;  // offset 64: 1=1W,2=2W,3=3W,4=4W
    uint  baseBone;  // offset 68: this skeleton's first bone slot in bones[]
    uint  boneCount; // offset 72: this skeleton's bone count (for index clamp)
    float hudMode;   // offset 76: 1 = first-person HUD (read by the fragment shader)
} pc;

// All skeletons' bone matrices concatenated; this skeleton's start = pc.baseBone
// (SSBO, or a 1024-bone window of it as a dynamic UBO under SKIN_BONES_UBO).
#include "skin_bones.glsl"

// Bone index decode. Indices are now stored RAW (0..BoneCount-1) — direct SSBO
// index, no legacy "*3" matrix-row stride (which overflowed the u8 channels for
// bones >85). dN: from u8-normalized alpha; dF: from a float field.
#include "skin_matrix.glsl"   // dN/dF/clampB + skinMatrix() — shared by all skinned passes

// 10-10 r_hud_vsun (hudMode 2): the first-person hands/weapon take the sun shadow PER VERTEX -- the FS sampled the cascades
// (4 x cascTap) on every HUD pixel, and the hands and the gun cover a good part of the screen. The env set (set 2) exposes
// its bindings 0/1/4/5 to the vertex stage for the R1 crowns/grass already (nx_sun_shadow.glsl).
#define ENV_SET 2
#include "light_ubo.glsl"
#include "shadow_math.glsl"
#include "nx_sun_shadow.glsl"
layout(location = 3) out float v_hudSun;   // the sun visibility of a HUD vertex (1 when unused)

void main()
{
    vec3 pos = a_Position.xyz;
    vec3 nrm = a_Normal.xyz * 2.0 - 1.0;


    mat4 S = skinMatrix();

    vec4 sp = S * vec4(pos, 1.0);    // world-space (bones pre-multiplied)
    gl_Position = pc.mvp * sp;
    v_uv   = a_TexCoordExt.xy;
    v_nrm  = normalize(mat3(S) * nrm);
    v_wpos = sp.xyz;
    v_hudSun = 1.0;
    if (pc.hudMode > 1.5) {
        // the HUD renders "camera at origin": v_wpos is camera-relative there (skinned.frag's hudWp, same three cases)
        vec3 wp = (dot(sp.xyz, sp.xyz) < 9.0) ? sp.xyz + L.eye_pos.xyz
                : (distance(sp.xyz, L.eye_pos.xyz) < 3.0) ? sp.xyz : L.eye_pos.xyz + vec3(0.0, 0.1, 0.0);
        if (L.puddle_geo.w > 0.5) v_hudSun = L.puddle_geo.w - 1.0;   // no shadow maps: the camera's sun visibility
        else { float cov; v_hudSun = nxSunShadowCov(wp, cov); }
    }
}

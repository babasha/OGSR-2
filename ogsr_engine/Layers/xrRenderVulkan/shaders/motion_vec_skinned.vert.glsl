#version 450
// xrRenderVulkan — skinned NPC motion vectors (MV Phase 2a).
//
// Re-draws the animated skinned leaves into the motion-vector target, OVER the
// camera/static field the fullscreen pass laid down, so each NPC pixel carries
// its TRUE motion (camera + skeletal animation + the body moving through the
// world), not just camera reprojection.
//
// Skins the vertex TWICE: with THIS frame's bones (baseBone) and the PREVIOUS
// frame's bones (prevBase) — both regions live simultaneously in the shared bone
// SSBO (kMaxBones × FRAMES_IN_FLIGHT; last frame's slot isn't overwritten this
// frame). Bones are pre-multiplied to world space (see skinned.vert), so S*pos is
// world; we project cur with curVP and prev with prevVP. The fragment turns the
// two clip positions into screen-space motion.
//
// The skinning decode comes from skin_matrix.glsl — the SAME include the colour,
// caster and VSM passes use. It used to be a private copy here, which is how a pass
// silently falls behind the shared one; skinMatrixAt(bb) exists precisely so this
// pass can pick the pose base instead of forking the decode.

layout(location = 0) in vec4 a_Position;
layout(location = 1) in vec4 a_Normal;       // a = boneIdx(1W) / w0(2W+)
layout(location = 2) in vec4 a_TexCoordExt;  // zw = idx0,idx1 (44)
layout(location = 3) in vec4 a_Tangent;      // a = w1 (3W/4W)
layout(location = 4) in vec4 a_Binormal;     // a = idx2(3W) / w2(4W)
layout(location = 5) in vec4 a_BoneIndices;  // rgba = 4 idx (4W)

layout(location = 0) out vec4 vCurClip;
layout(location = 1) out vec4 vPrevClip;
layout(location = 2) out vec2 vUV;       // for the fragment alpha-test (cutout holes)

layout(push_constant) uniform PC {
    mat4 curVP;     // 0   this frame's view-proj, UNJITTERED (jitter-free MV)
    mat4 prevVP;    // 64  previous frame's view-proj, UNJITTERED
    uint skinMode;  // 128 1=1W,2=2W,3=3W,4=4W
    uint baseBone;  // 132 this frame's first bone slot (skin_matrix.glsl contract name)
    uint prevBase;  // 136 previous frame's first bone slot (== baseBone if the skeleton is new)
    uint boneCount; // 140 bone count (index clamp)
    vec2 jitter;    // 144 this frame's sub-pixel jitter (D3D-NDC) re-applied to gl_Position
} pc;

layout(std430, set = 0, binding = 0) readonly buffer Bones { mat4 bones[]; };

#include "skin_matrix.glsl"   // dN/dF/clampB + skinMatrix()/skinMatrixAt() — shared by all skinned passes

void main()
{
    vec3 pos = a_Position.xyz;
    vec4 wpCur  = skinMatrix()               * vec4(pos, 1.0);   // world pos, this frame's pose
    vec4 wpPrev = skinMatrixAt(pc.prevBase)  * vec4(pos, 1.0);   // world pos, previous pose
    vCurClip  = pc.curVP  * wpCur;    // unjittered → clean MV in the fragment
    vPrevClip = pc.prevVP * wpPrev;
    vUV         = a_TexCoordExt.xy;
    // Rasterize at the JITTERED position so depth bit-matches the forward/prepass
    // (which jittered the combined matrix). Re-applying the clip translation here
    // (clip.xy += jitter*w) reproduces that exactly while keeping MV jitter-free.
    gl_Position = vCurClip;
    gl_Position.xy += pc.jitter * gl_Position.w;
}

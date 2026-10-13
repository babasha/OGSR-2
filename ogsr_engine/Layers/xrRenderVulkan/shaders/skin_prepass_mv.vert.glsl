#version 450
// xrRenderVulkan — PRE-SKINNED NPC motion vectors written IN THE DEPTH PREPASS
// (r_mv_prepass). Pairs with motion_vec_skinned.frag (same varyings, same
// alpha-test), and replaces the separate MV-only re-raster of every skeleton.
//
// The prepass draws NPCs from the compute pre-skinning pool (preskin.comp), whose
// vertices are ALREADY world-space for this frame's pose. This shader therefore
// does no skinning at all: it takes the current world position from that pool and
// the previous frame's world position from the PARALLEL pool preskin.comp now
// writes alongside it (binding 1, same vertex order, so one vertexOffset indexes
// both). Projecting the two with the unjittered cur/prev view-proj gives the true
// screen motion — camera + skeletal pose + the body travelling through the world.
//
// ⚠⚠ WHY NOT RE-SKIN HERE. The colour pass draws these same leaves from the same
// pool, and its depth test is LEQUAL. Recomputing the current position by any other
// route — even the identical blend in a vertex shader — lands one ULP away, and the
// colour pass would then drop NPC fragments at random. The current position must be
// READ, not recomputed. That is also why the previous pose is produced in compute.

layout(location = 0) in vec4 a_Position;   // pre-skinned pool: world pos, w = 1
layout(location = 2) in vec2 a_TexCoord;   // pool dwords 7,8 — the leaf UV, passed through verbatim
layout(location = 6) in vec3 a_PrevPos;    // parallel pool: the SAME vertex, previous frame's pose

layout(location = 0) out vec4 vCurClip;
layout(location = 1) out vec4 vPrevClip;
layout(location = 2) out vec2 vUV;

// Byte-identical to motion_vec_skinned.vert's block (MVSkinPush, 152 B) so both
// pipelines take the same push from the same call site. The skinning fields are
// unused here — the pool already did that work.
layout(push_constant) uniform PC {
    mat4 curVP;     // 0   this frame's view-proj, UNJITTERED (jitter-free MV)
    mat4 prevVP;    // 64  previous frame's view-proj, UNJITTERED
    uint skinMode;  // 128 (unused: pre-skinned)
    uint baseBone;  // 132 (unused)
    uint prevBase;  // 136 (unused: the previous pose arrives as a vertex stream)
    uint boneCount; // 140 (unused)
    vec2 jitter;    // 144 this frame's sub-pixel jitter (D3D-NDC), re-applied to gl_Position
} pc;

void main()
{
    vCurClip  = pc.curVP  * vec4(a_Position.xyz, 1.0);
    vPrevClip = pc.prevVP * vec4(a_PrevPos,      1.0);
    vUV       = a_TexCoord;
    // Rasterize at the JITTERED position so this depth bit-matches the colour pass
    // (which jitters the combined matrix). The translation touches clip.xy only, so
    // z/w — the values LEQUAL compares — are untouched, and the MV stays jitter-free.
    gl_Position = vCurClip;
    gl_Position.xy += pc.jitter * gl_Position.w;
}

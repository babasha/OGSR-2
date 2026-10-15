#version 450
#extension GL_EXT_buffer_reference : require
// ============================================================================
// preskin.comp — COMPUTE PRE-SKINNING for OGSR Vulkan.
//
// WHY: every consumer used to re-run the 1-4 bone blend in its own vertex
// shader, so a single NPC vertex was skinned 6-15x per frame — depth prepass,
// SSAO normal prepass, forward colour, sun shadow + cascades, point cube (up to
// 6 faces), spot tiles, VSM pages, glass distort. This shader skins each
// visible leaf ONCE per frame into a shared pool; the consumers then draw plain
// pre-transformed geometry.
//
// OUTPUT FORMAT: vertHW_1W (stride 36, see vk_Visual.h) with bone index 0.
// That is deliberate — it is byte-identical to what the existing skinned
// pipelines already accept, so the whole consumer side needs NO new pipelines
// and NO shader edits: they bind this pool instead of the mesh VB and push
// skinMode=1 / baseBone=<identity slot> / boneCount=1. `S` in skinned.vert then
// resolves to the IDENTITY matrix and `S * pos` passes the world position
// straight through, while `mat3(S) * nrm` passes the world normal through.
//
// The bone math below is a line-for-line copy of skinned.vert's — the two MUST
// stay in sync (weights, the 2W lerp order, the RAW bone-index decode).
// Positions/normals come out WORLD-space because Skinned_UploadBones stores the
// bones pre-multiplied by the object transform.
// ============================================================================

layout(local_size_x = 64) in;

// set 0: the SAME bone SSBO the graphics pipelines bind (world-space matrices,
// written row-major and read column-major here, which transposes — so
// `bones[i] * v` == X-Ray's `v * Mbone`), plus the shared output pool.
layout(std430, set = 0, binding = 0) readonly  buffer Bones   { mat4 bones[]; };
layout(std430, set = 0, binding = 1) writeonly buffer Dst     { uint dst[];   };
// PREVIOUS-FRAME world position, same vertex order, 3 dwords per vertex
// (r_mv_prepass). The depth prepass binds it as a SECOND vertex stream and can
// then state each NPC pixel's true motion in the raster that lays its depth,
// instead of a second MV-only re-raster of every skeleton afterwards.
// ⚠ It has to be produced HERE. The prepass draws PRE-SKINNED vertices; a vertex
// shader that re-skinned the current pose to get a matching origin would compute
// the same position by a different route and land one ULP off the colour pass's
// depth — whose LEQUAL would then reject NPC fragments at random.
layout(std430, set = 0, binding = 2) writeonly buffer DstPrev { uint dstPrev[]; };

// The source mesh differs per leaf, so it arrives as a device address in the
// push block instead of a descriptor — that keeps this to ONE descriptor set
// for the whole frame (hundreds of leaves, zero descriptor churn).
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer SrcRef { uint v[]; };

layout(push_constant) uniform PC {
    SrcRef src;          // 0  : leaf's vertHW_* vertex buffer
    uint   vertCount;    // 8
    uint   srcStrideDW;  // 12 : source stride in dwords (36/40/44 -> 9/10/11)
    uint   dstFirst;     // 16 : first output vertex in the shared pool
    uint   skinMode;     // 20 : 1=1W, 2=2W, 3=3W, 4=4W
    uint   baseBone;     // 24
    uint   boneCount;    // 28
    uint   prevBase;     // 32 : this skeleton's bone region LAST frame. Equal to
                         //      baseBone for one first seen this frame, which makes
                         //      its previous position its current one — zero motion,
                         //      the right answer for a spawn.
} pc;

// Bone index decode — identical to skinned.vert. Indices are stored RAW
// (0..BoneCount-1); dN reads a u8-normalized alpha, dF a float field.
uint dN(float a) { return uint(round(a * 255.0)); }
uint dF(float v) { return uint(round(abs(v)));    }
uint clampB(uint i) { return (pc.boneCount == 0u) ? 0u : min(i, pc.boneCount - 1u); }

// The 1..4-bone blend, line-for-line skinned.vert's — factored out of main only so
// that the CURRENT and the PREVIOUS pose (r_mv_prepass) go through the SAME code
// with a different bone base `bb`. `s` is the source vertex's first dword; N/T/B
// are the already-unpacked normal/tangent/binormal whose alphas carry the weights.
mat4 skinAt(uint bb, uint mode, uint s, vec4 N, vec4 T, vec4 B)
{
    if (mode == 1u) {
        return bones[bb + clampB(dN(N.a))];
    } else if (mode == 2u) {
        // R4 (FSkinned vertHW_2W::get_pos_bones): matrix0 gets (1-w), matrix1 gets w.
        const float w0 = N.a;
        return bones[bb + clampB(dF(uintBitsToFloat(pc.src.v[s + 9])))]  * (1.0 - w0)
             + bones[bb + clampB(dF(uintBitsToFloat(pc.src.v[s + 10])))] * w0;
    } else if (mode == 3u) {
        const float w0 = N.a, w1 = T.a;
        return bones[bb + clampB(dF(uintBitsToFloat(pc.src.v[s + 9])))]  * w0
             + bones[bb + clampB(dF(uintBitsToFloat(pc.src.v[s + 10])))] * w1
             + bones[bb + clampB(dN(B.a))]                               * (1.0 - w0 - w1);
    } else {
        const vec4  I  = unpackUnorm4x8(pc.src.v[s + 9]);
        const float w0 = N.a, w1 = T.a, w2 = B.a;
        return bones[bb + clampB(dN(I.r))] * w0
             + bones[bb + clampB(dN(I.g))] * w1
             + bones[bb + clampB(dN(I.b))] * w2
             + bones[bb + clampB(dN(I.a))] * (1.0 - w0 - w1 - w2);
    }
}

void main()
{
    const uint gid = gl_GlobalInvocationID.x;
    if (gid >= pc.vertCount) return;

    // vertHW_* dword map (all three strides share the first 9 dwords):
    //   0..3 P FLOAT4 | 4 N(a=boneIdx/w0) | 5 T(a=w1) | 6 B(a=idx2/w2) | 7,8 tc
    //   44: 9,10 = bone idx0/idx1 as floats   |   40: 9 = 4 bone indices u8x4
    const uint s = gid * pc.srcStrideDW;

    const vec3 pos = vec3(uintBitsToFloat(pc.src.v[s + 0]),
                          uintBitsToFloat(pc.src.v[s + 1]),
                          uintBitsToFloat(pc.src.v[s + 2]));
    const vec4 N = unpackUnorm4x8(pc.src.v[s + 4]);
    const vec4 T = unpackUnorm4x8(pc.src.v[s + 5]);
    const vec4 B = unpackUnorm4x8(pc.src.v[s + 6]);

    const uint mode = pc.skinMode & 15u;   // bits 4/5 are fragment-side flags

    const mat4 S = skinAt(pc.baseBone, mode, s, N, T, B);

    // PREVIOUS pose — position only; a motion vector needs nothing else. Same
    // blend, same weights, same vertex: only the bone REGION differs, and both
    // regions are live in the SSBO at once (kMaxBones × FRAMES_IN_FLIGHT — last
    // frame's slot is not overwritten this frame), which is what makes the
    // previous pose reachable from here at all.
    {
        const vec3 wprev = (skinAt(pc.prevBase, mode, s, N, T, B) * vec4(pos, 1.0)).xyz;
        const uint dp = (pc.dstFirst + gid) * 3u;   // 12 bytes = 3 dwords
        dstPrev[dp + 0] = floatBitsToUint(wprev.x);
        dstPrev[dp + 1] = floatBitsToUint(wprev.y);
        dstPrev[dp + 2] = floatBitsToUint(wprev.z);
    }

    const vec3 wpos = (S * vec4(pos, 1.0)).xyz;
    const mat3 R    = mat3(S);

    // Re-encode into the same u8 unorm channels the consumers decode with
    // `xyz * 2 - 1`. packUnorm4x8 is the exact inverse of the unpackUnorm4x8
    // above, so this round-trips through the identical byte order.
    const vec3 wn = normalize(R * (N.xyz * 2.0 - 1.0));
    vec3 wt = R * (T.xyz * 2.0 - 1.0);
    vec3 wb = R * (B.xyz * 2.0 - 1.0);
    wt = (dot(wt, wt) > 1e-12) ? normalize(wt) : vec3(1.0, 0.0, 0.0);
    wb = (dot(wb, wb) > 1e-12) ? normalize(wb) : vec3(0.0, 1.0, 0.0);

    const uint d = (pc.dstFirst + gid) * 9u;   // 36 bytes = 9 dwords
    dst[d + 0] = floatBitsToUint(wpos.x);
    dst[d + 1] = floatBitsToUint(wpos.y);
    dst[d + 2] = floatBitsToUint(wpos.z);
    dst[d + 3] = floatBitsToUint(1.0);
    dst[d + 4] = packUnorm4x8(vec4(wn * 0.5 + 0.5, 0.0));   // a = bone index 0 (identity slot)
    dst[d + 5] = packUnorm4x8(vec4(wt * 0.5 + 0.5, 0.0));
    dst[d + 6] = packUnorm4x8(vec4(wb * 0.5 + 0.5, 0.0));
    dst[d + 7] = pc.src.v[s + 7];                           // uv passes through verbatim
    dst[d + 8] = pc.src.v[s + 8];
}

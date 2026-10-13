// xrRenderVulkan — GPU vertex skinning: decode the packed bone indices/weights and
// build the blended bone matrix S. Shared by EVERY pass that skins the same mesh:
//
//   skinned.vert              colour pass
//   shadow_skinned_body       sun caster / depth prepass / normal G-buffer
//   vsm_skinned_page.vert     VSM atlas page caster
//   motion_vec_skinned.vert   motion vectors (skins TWICE — see skinMatrixAt below)
//
// These MUST agree bit-for-bit. The colour pass re-rasterizes the prepass positions
// with LEQUAL: any difference — even one that only shows up for a particular skinMode
// — turns into z-fighting or a wholly rejected NPC. It was copied into all three.
//
// CONTRACT: the including shader declares the vertex attributes a_Normal /
// a_TexCoordExt / a_Tangent / a_Binormal / a_BoneIndices, the `bones[]` SSBO, and a
// push-constant block `pc` with uint skinMode / baseBone / boneCount.
#ifndef SKIN_MATRIX_GLSL
#define SKIN_MATRIX_GLSL

uint dN(float a) { return uint(round(a * 255.0)); }
uint dF(float v) { return uint(round(abs(v)));    }

// Clamp a decoded local bone index to [0, boneCount). An out-of-range index would
// read an unwritten (zero) SSBO slot -> S becomes a zero matrix -> w=0 -> the vertex
// shoots to infinity (thin stray spike). Clamping pins it to a valid bone instead.
uint clampB(uint i) { return (pc.boneCount == 0u) ? 0u : min(i, pc.boneCount - 1u); }

// Blended bone matrix rooted at an EXPLICIT bone-slot base. Motion vectors need this:
// they skin the same vertex twice, once against this frame's pose and once against the
// previous frame's, and both poses live simultaneously in the shared bone SSBO. Every
// other pass wants pc.baseBone and calls skinMatrix() below.
mat4 skinMatrixAt(uint bb)
{
    // Low 4 bits = skinning mode. The HIGH bits are fragment-side flags the colour
    // pass ORs in (16 = emissive/collimator, 32 = glass, vk_pass_skinned.cpp) and
    // must be masked off before the compare. Only the colour pass sets them today,
    // but the caster passes share this push block — an unmasked compare would fall
    // through to the 4-weight branch the moment one appeared, skinning the SAME mesh
    // differently in the depth and colour passes.
    uint mode = pc.skinMode & 15u;

    mat4 S;
    if (mode == 1u) {
        S = bones[bb + clampB(dN(a_Normal.a))];
    } else if (mode == 2u) {
        // R4 (FSkinned vertHW_2W::get_pos_bones): lerp(boneA, boneB, w) =
        // boneA*(1-w) + boneB*w, with w stored in N.a and A=matrix0, B=matrix1.
        // So matrix0 gets (1-w), matrix1 gets w — NOT the other way round.
        float w0 = a_Normal.a;
        S = bones[bb + clampB(dF(a_TexCoordExt.z))] * (1.0 - w0) + bones[bb + clampB(dF(a_TexCoordExt.w))] * w0;
    } else if (mode == 3u) {
        float w0 = a_Normal.a, w1 = a_Tangent.a;
        S = bones[bb + clampB(dF(a_TexCoordExt.z))] * w0
          + bones[bb + clampB(dF(a_TexCoordExt.w))] * w1
          + bones[bb + clampB(dN(a_Binormal.a))]    * (1.0 - w0 - w1);
    } else {
        float w0 = a_Normal.a, w1 = a_Tangent.a, w2 = a_Binormal.a;
        S = bones[bb + clampB(dN(a_BoneIndices.r))] * w0
          + bones[bb + clampB(dN(a_BoneIndices.g))] * w1
          + bones[bb + clampB(dN(a_BoneIndices.b))] * w2
          + bones[bb + clampB(dN(a_BoneIndices.a))] * (1.0 - w0 - w1 - w2);
    }
    return S;
}

mat4 skinMatrix() { return skinMatrixAt(pc.baseBone); }

#endif // SKIN_MATRIX_GLSL

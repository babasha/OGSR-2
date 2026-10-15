#ifndef FROXEL_GLSL_INCLUDED
#define FROXEL_GLSL_INCLUDED

// ============================================================================
//  froxel.glsl — single source of truth for the volumetric froxel exp-Z mapping
// ============================================================================
//  The volumetric fog grid (vk_volumetrics) packs the view frustum into a 3D
//  texture with an EXPONENTIAL Z distribution (denser near the camera). The
//  forward map (normalized slice -> view-space Z) is used by the injection
//  compute; the inverse (view-space Z -> slice) is used by every consumer that
//  samples the grid:
//    - the tonemap composite                  (tonemap.frag)
//    - the Stage-0 smoke light probe           (particle.vert)
//    - the temporal reprojection in injection  (vol_inject.comp)
//
//  The forward and inverse MUST stay exact inverses. A mismatch makes a consumer
//  read the WRONG Z slice — that once cost a full day of chasing "trembling
//  shafts" (the temporal reprojection sampled a slightly different slice than
//  injection wrote). Keeping the math in ONE place is the whole point of this
//  file: edit it here and every copy stays in lockstep.
//
//  Conventions:
//    nearZ  = grid near plane (linear view Z of slice 0's front face)
//    logFN  = log2(far / near)
//    slice  = NORMALIZED depth coord in [0,1] (the texture's W); for froxel
//             index z it is (z + 0.5) / dimZ.
//  Callers own their clamping (clamp the slice to [0,1], guard viewZ >= nearZ
//  and logFN > 0 as their context needs) — kept out of here so the two maps
//  stay branch-free exact inverses.
// ============================================================================

// Normalized slice [0,1] -> linear view-space Z.
float Froxel_ViewZFromSlice(float slice, float nearZ, float logFN)
{
    return nearZ * exp2(logFN * slice);
}

// Linear view-space Z -> normalized slice [0,1] (UNCLAMPED). Exact inverse of
// Froxel_ViewZFromSlice.
float Froxel_SliceFromViewZ(float viewZ, float nearZ, float logFN)
{
    return log2(viewZ / nearZ) / logFN;
}

#endif // FROXEL_GLSL_INCLUDED

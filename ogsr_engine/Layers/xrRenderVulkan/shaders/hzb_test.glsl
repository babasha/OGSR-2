#ifndef HZB_TEST_GLSL
#define HZB_TEST_GLSL
// xrRenderVulkan — shared Hi-Z (HZB) occlusion test for bounding spheres.
//
// ONE copy of this test, used by every consumer of the pyramid CDetailManager
// builds (hzb_build.comp.glsl): world_cull_hzb.comp, tree_cull.comp,
// lod_cull.comp. It used to be copy-pasted into each of them, and every fix
// below had to be re-applied by hand — twice it was not, and the stale copy
// kept false-culling. Fix it HERE.
//
// WHY IT IS SAFE TO CULL ONLY THE COLOUR SET (the argument every caller relies on):
// the pyramid is a MAX reduction of THIS frame's finished depth PREPASS, and the
// geometry being tested is part of that prepass. Anything visible in the prepass
// therefore has its own depth inside its own footprint, so hzbDepth >= its near
// face and this test can NEVER cull it. Whatever this test does drop contributed
// nothing to the depth buffer — so nothing is left written-but-unshaded (the
// black-silhouette failure). The prepass must keep drawing the full frustum set:
// it IS the pyramid's source.
//
// CONVENTIONS ASSUMED (all true for the X-Ray/VK camera path):
//   * standard Z: NDC z in [0,1], depth cleared to 1.0, LEQUAL — so MAX reduction
//     is the conservative direction and sky (1.0) never occludes anything;
//   * viewProj such that viewProj * vec4(p,1) = clip and clip.w = view-space Z;
//   * the colour/depth viewport uses NEGATIVE height => HZB Y is flipped;
//   * SQUARE PIXELS (P00/P11 == height/width, see Fmatrix::build_projection with
//     fASPECT = H/W). That is what makes the projected sphere's footprint the SAME
//     number of texels on both axes, which the texel-space maths below relies on.
//
// THREE THINGS THAT ARE EASY TO GET WRONG (each one cost a bug):
//  1. UV from the sphere CENTRE, compared depth from the sphere's NEAR FACE.
//     Pulling the sample UV camera-ward too (as the grass path does — fine for a
//     0.5 m tuft) offsets the tap to a DIFFERENT screen location than the object
//     once radii are tens of metres => view-dependent false culls (whole terrain
//     tiles and window frames vanishing).
//  2. The footprint needs the projection's FOCAL scale: diameter in texels =
//     r * P11 * hzbHeight / clip.w. Dropping P11 understates it ~1.5x => too fine
//     a mip => the block missed part of the footprint => false culls.
//  3. The block must be built in TEXEL space, not by offsetting UVs by a single
//     radius: UV is anisotropic (x is compressed by the aspect ratio) while the
//     texel footprint is not, so one shared UV radius stretched the x span to
//     ~1.8 texels and the 4 corner taps then STEPPED OVER the middle column —
//     a MAX with a hole in it, i.e. false culls again. Working in mip-0 texels
//     and shifting by the mip keeps the span <= 1 texel per axis, so the 2x2
//     block provably covers the footprint. It also removes the non-power-of-two
//     skew: mip dims are floor-halved and hzb_build folds the odd remainder into
//     the edge texel, which makes texel(mipK) == clamp(texel(mip0) >> K) exact.
//
// Returns TRUE when the sphere is entirely behind the farthest surface in its
// screen footprint (safe to skip). Any doubt — off screen, behind the camera,
// camera inside the sphere, footprint larger than the pyramid — returns FALSE
// (keep), because over-culling is a visible hole and under-culling is only cost.
bool HZB_SphereOccluded(sampler2D hzb, mat4 viewProj, vec3 centre, float radius,
                        vec3 eye, float focal /* 1/tan(fovY/2) = P11 */)
{
    vec4 clipC = viewProj * vec4(centre, 1.0);
    if (clipC.w <= 0.0) return false;                     // centre behind the camera

    vec3  toCam   = eye - centre;
    float camDist = length(toCam);
    if (camDist <= 1e-3) return false;

    vec2 ndc = clipC.xy / clipC.w;
    vec2 uv  = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);   // Y flip (negative-height viewport)
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return false;

    // Near face = closest point of the sphere to the camera = the most
    // conservative depth to compare. w <= 0 means the camera is inside the
    // sphere — nothing to occlude it with.
    vec4 clipN = viewProj * vec4(centre + (toCam / camDist) * radius, 1.0);
    if (clipN.w <= 0.0) return false;
    float sphereDepth = clipN.z / clipN.w;                // NDC z in [0,1] = depth-buffer space

    // Mip where the footprint covers ~1 texel. NOT clamping this used to be
    // undefined behaviour: texelFetch/textureSize with lod >= levels is UB, and
    // r/dist > ~0.55 (terrain tiles, big walls, a tree next to the camera) walks
    // straight past the last mip. A footprint that large is never occluded in
    // practice, so bail out instead of guessing.
    ivec2 hzbSize      = textureSize(hzb, 0);
    float screenTexels = radius * focal * float(hzbSize.y) / clipC.w;
    int   mi           = int(ceil(log2(max(1.0, screenTexels))));
    if (mi > textureQueryLevels(hzb) - 1) return false;

    // Footprint rect in MIP-0 texels, then shifted into the chosen mip. The span
    // is screenTexels <= 2^mi mip-0 texels wide, so after the shift the two
    // corners are at most 1 texel apart on each axis => the 2x2 block below
    // provably covers every texel the sphere touches (see note 3 above).
    vec2  pc0  = uv * vec2(hzbSize);
    float halfT = 0.5 * screenTexels;
    ivec2 tmax = textureSize(hzb, mi) - 1;
    ivec2 t0 = clamp(ivec2(floor(pc0 - halfT)) >> mi, ivec2(0), tmax);
    ivec2 t1 = clamp(ivec2(floor(pc0 + halfT)) >> mi, ivec2(0), tmax);

    float d0 = texelFetch(hzb, ivec2(t0.x, t0.y), mi).r;
    float d1 = texelFetch(hzb, ivec2(t1.x, t0.y), mi).r;
    float d2 = texelFetch(hzb, ivec2(t0.x, t1.y), mi).r;
    float d3 = texelFetch(hzb, ivec2(t1.x, t1.y), mi).r;
    float hzbDepth = max(max(d0, d1), max(d2, d3));

    // hzbDepth == 0 = "no pyramid content here" (never written / bound dummy) —
    // treat as visible rather than culling everything against a zero.
    return sphereDepth > hzbDepth && hzbDepth > 0.0;
}

#endif   // HZB_TEST_GLSL

// xrRenderVulkan — crown voxel-BRICK caster VS BODY (r_vsm_tree_hull + _vox). Shared
// by two wrappers picking the target atlas via TV_* macros (same pattern as
// tree_vsm_hull_body.glsl):
//   tree_vsm_vox.vert.glsl    DYNAMIC atlas — near tier, live wind push
//   tree_vsm_vox_s.vert.glsl  STATIC atlas  — far tier (r_vsm_tree_hull 2), rigid
//
// UE Nanite voxel-brick model (RasterizeBricks.usf / NaniteRasterizer.usf): the
// shadow representation is 4×4×4-cell BRICKS carrying 64-bit occupancy masks — NOT
// per-voxel geometry. This VS emits ONE quad per brick (6 verts) covering the brick's
// exact light-space AABB (abs-of-axes trick — the VSM view is orthographic along the
// sun) at the brick's nearest light depth; the FS DDA-raycasts the mask along the sun
// and writes conservative depth. Per-voxel cost collapses from 36 HW verts (the first
// cube version measured 26–92 ms of VSM/DynTrees) to a few mask tests per pixel.
// Wind runs ONCE per vertex at the brick center — the brick sways rigidly.
//
// CROSSFADE BAND (fade16 in choice.w < 0xFFFF, the approach to r_vsm_tree_hull_dist):
//   stage 1 (fade 1 → ~0.55): bricks swap full→core mask one by one (staggered by
//     bake hash) — low-coverage "extras" drop, cubes remain on the leaf clumps;
//   stage 2: whole bricks dissolve out coverage-ordered (soft per-brick window, IGN
//     in the FS) while the real alpha-tested crown casts below fade 0.5 (vsm_hull_cmd).
//
// NON-indexed instanced-over-pages draw (vsm_hull_cmd.comp builds the commands):
//   gl_VertexIndex = 6·brickIdx + corner (firstVertex = 6·brickFirst),
//   gl_InstanceIndex → casterPages → atlas slot (IDENTICAL to the crown/hull routing).
#include "vsm_common.glsl"
#include "ssfx_tree_wind.glsl"

#include "tree_instance.glsl"   // TreeInstance + XformBuf (set 0 b0) — matches VK::GpuTreeInstance

layout(set = 2, binding = 0) readonly buffer PageList    { uvec4 pageList[]; };
layout(set = 2, binding = 1) readonly buffer CasterPages { uint  casterPages[]; };
#define VSM_PARAMS_SET     2
#define VSM_PARAMS_BINDING 2
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)

// Matches GpuTreeBrick (32 B): p = brick MIN corner (mesh-local), meta = avgCov4 |
// hash8<<4, masks = (fullLo, fullHi, coreLo, coreHi).
struct GpuBrick { vec3 p; uint meta; uvec4 masks; };
layout(set = 3, binding = 0, std430) readonly buffer BrickBuf { GpuBrick brick[]; };
// Per-tree: x = brickFirst, y = brickCount, z = floatBits(cellEdge mesh-local),
// w = fade16<<16 | amp16. amp = brick wind amplitude (1−fade): coherent sway next to
// the swaying crown at the band's inner edge, 0 for fully-voxel and static-tier trees
// — at amp 0 the whole wind block is skipped (UE disables WPO at voxel distances).
layout(set = 3, binding = 1, std430) readonly buffer Choice { uvec4 choice[]; };
// Stage-2 remap (r_vsm_tree_hull_vox_cull, pc.pad != 0): vsm_vox_cull.comp compacted
// the (tree,page) draws to only the bricks overlapping each page; the command's
// firstVertex = 6·listBase, so gl_VertexIndex/6 indexes this list directly and it
// yields the ORIGINAL global brick index (page routing and the position-ordered band
// dissolve are unchanged).
layout(set = 3, binding = 2, std430) readonly buffer BrickRemap { uint blist[]; };

#include "tree_vsm_push.glsl"   // VSM caster push — shared by the four caster bodies

layout(location = 0) out flat float vFade;
layout(location = 1) out flat uvec2 vMask;
layout(location = 2) out flat vec3  vMx;      // ∂brick/∂lp.x (cell units)
layout(location = 3) out flat vec3  vMy;      // ∂brick/∂lp.y
layout(location = 4) out flat vec3  vDir;     // ∂brick/∂lp.z = ray dir per light-z meter
layout(location = 5) out flat vec3  vB0;      // brick coords at (lpRef.xy, minLz)
layout(location = 6) out flat vec2  vLpRef;   // light-space reference point (brick origin xy)
layout(location = 7) out noperspective vec2 vLpXY;   // this fragment's light-space xy (ortho → linear)
layout(location = 8) out flat vec2  vNz;      // x = normalized depth at minLz, y = depth per light-z meter

out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; };

const vec2 kQuad[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void cullVert()
{
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    gl_ClipDistance[0] = gl_ClipDistance[1] = gl_ClipDistance[2] = gl_ClipDistance[3] = -1.0;
    vFade = 1.0; vMask = uvec2(0u); vMx = vMy = vDir = vB0 = vec3(0.0);
    vLpRef = vLpXY = vNz = vec2(0.0);
}

void main()
{
    uint entry = casterPages[gl_InstanceIndex];   // arena entry = (treeIdx<<13)|slot (vsm_tree_bin)
    uint slot  = entry & 0x1FFFu;
    if (slot >= uint(TV_MAX_PHYS)) { cullVert(); return; }
    uint treeIdx = entry >> 13u;

    uvec4 ch       = choice[treeIdx];
    uint  brickIdx = uint(gl_VertexIndex) / 6u;
    uint  corner   = uint(gl_VertexIndex) % 6u;
    if (pc.pad != 0u) brickIdx = blist[brickIdx];   // stage-2 per-page compacted draw

    GpuBrick bk = brick[brickIdx];
    float fade  = float(ch.w >> 16) / 65535.0;
    uvec2 m     = bk.masks.xy;
    float vf    = 1.0;
    if (fade < 0.999) {
        float hash = float((bk.meta >> 4) & 0xFFu) / 255.0;
        // Stage 1: staggered full→core swap — "extras removed", leaf clumps stay.
        if (fade < 0.55 + 0.35 * hash) m = bk.masks.zw;
        // Stage 2: whole-brick dissolve by NORMALIZED SLICE POSITION. The bake sorts
        // each slice by descending rank (0.5·cov + 0.5·hash — sparse card edges pop
        // first, leaf clumps last), so the survivors are exactly the first fade·count
        // bricks and vsm_hull_cmd shrinks the draw to that prefix — a band tree at
        // fade 0.2 costs 20% of its bricks instead of a full draw with VS kills.
        float idx01 = (float(brickIdx - ch.x) + 0.5) / float(ch.y);
        vf = clamp((fade - idx01) / 0.08, 0.0, 1.0);
    }
    if (vf <= 0.0 || (m.x | m.y) == 0u) { cullVert(); return; }
    vFade = vf;
    vMask = m;

    // Brick box in world: min corner + three axis vectors (4 cells each). Instance
    // scale rides in the xform columns; cell edge comes mesh-local via the choice.
    mat4  X    = inst[treeIdx].xform;
    float cell = uintBitsToFloat(ch.z);
    vec3  axX  = X[0].xyz * (4.0 * cell);
    vec3  axY  = X[1].xyz * (4.0 * cell);
    vec3  axZ  = X[2].xyz * (4.0 * cell);
    vec3  wmin = (X * vec4(bk.p, 1.0)).xyz;
    // Wind only while the brick coexists with the swaying crown (band, amp = 1−fade);
    // amp 0 = the common case (fully-voxel dyn + every static-tier tree) skips every
    // sine — UE turns WPO off at voxel distances, this is our analog. The branch is
    // uniform per tree, so warps stay coherent.
    float amp = float(ch.w & 0xFFFFu) / 65535.0;
    if (amp > 0.0) {
        vec3  wc    = wmin + 0.5 * (axX + axY + axZ);
        float baseY = X[3].y;
        float H     = wc.y - baseY;
        float r     = -pc.wind_params.x + 1.57079;
        vec2  wdir  = vec2(cos(r), sin(r));
        float spd   = max(pc.wsetup_trees.w, clamp(pc.wind_params.y * 0.001, 0.0, 1.0));
        wmin += amp * ssfxTreeWind(inst[treeIdx]._p0, wc, H, 0.35, wdir, spd, baseY,
                                   pc.wind_anim.xyz, pc.wsetup_trees.x, pc.wsetup_trees.y,
                                   pc.wsetup_trees.z, pc.wind_anim.w, pc.wind_params.w);
    }

    // Light space: exact AABB of the (rotated) brick box via per-axis abs sums.
    mat3 V3 = mat3(vsm.view);
    vec3 Lo = (vsm.view * vec4(wmin, 1.0)).xyz;
    vec3 Lx = V3 * axX, Ly = V3 * axY, Lz = V3 * axZ;
    vec3 lmin = Lo + min(Lx, vec3(0.0)) + min(Ly, vec3(0.0)) + min(Lz, vec3(0.0));
    vec3 lmax = Lo + max(Lx, vec3(0.0)) + max(Ly, vec3(0.0)) + max(Lz, vec3(0.0));
    vec2 lxy  = mix(lmin.xy, lmax.xy, kQuad[corner]);

    // Inverse mapping light → brick-cell coords ([0,4]³): lp = Lo + M·b, b = M⁻¹(lp-Lo).
    mat3 M    = mat3(Lx * 0.25, Ly * 0.25, Lz * 0.25);
    mat3 Minv = inverse(M);
    vMx    = Minv[0];
    vMy    = Minv[1];
    vDir   = Minv[2];
    vB0    = Minv * vec3(0.0, 0.0, lmin.z - Lo.z);
    vLpRef = Lo.xy;
    vLpXY  = lxy;
    vNz    = vec2((lmin.z - vsm.zparams.x) * vsm.zparams.y, vsm.zparams.y);

    uvec4 pg   = pageList[slot];
    int   L    = int(pg.x);
    ivec2 page = ivec2(pg.yz);
    vec2  origin = vsm.level[L].xy;
    float pw     = vsm.level[L].z / float(VSM_PAGES_AXIS);
    vec2  pmin   = origin + vec2(page) * pw;
    vec2  pmax   = pmin + vec2(pw);
    vec2  nxy    = (lxy - pmin) / pw * 2.0 - 1.0;

    gl_ClipDistance[0] = lxy.x - pmin.x;
    gl_ClipDistance[1] = pmax.x - lxy.x;
    gl_ClipDistance[2] = lxy.y - pmin.y;
    gl_ClipDistance[3] = pmax.y - lxy.y;

    uint  ax = slot % uint(TV_ATLAS_W);
    uint  ay = slot / uint(TV_ATLAS_W);
    float hX = 1.0 / float(TV_ATLAS_W);
    float hY = 1.0 / float(TV_ATLAS_H);
    float cx = (float(ax) + 0.5) * 2.0 * hX - 1.0;
    float cy = (float(ay) + 0.5) * 2.0 * hY - 1.0;
    gl_Position = vec4(cx + nxy.x * hX, cy + nxy.y * hY, vNz.x, 1.0);
}

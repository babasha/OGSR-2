#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — tree frustum-cull compute (Session B).
//
// One dispatch per TreeIndirectGroup. Each invocation tests one tree's
// bounding sphere against the view frustum; survivors atomically append a
// VkDrawIndexedIndirectCommand into the group's section of the indirect
// buffer and bump the group's draw-count. DrawTrees then issues one
// vkCmdDrawIndexedIndirectCount per group.
//
// Layout mirrors VK::GpuTreeMeta (32 B, vk_TreeManager.h). The C++ side
// packs sphere_P(vec3)+sphere_R(float) into 16 B, then 4×u32.

#include "hzb_test.glsl"   // HZB_SphereOccluded — shared with world_cull_hzb / lod_cull

layout(local_size_x = 256) in;

// binding 0: per-tree metadata (read-only). std430 packs vec3+float into 16 B.
struct TreeMeta {
    vec3 sphere_P;     // world-space center
    float sphere_R;    // radius
    uint index_count;  // num_tris * 3
    uint ib_first;     // first index (element offset into IB pool)
    uint first_vertex; // base vertex (element offset into VB pool)
    uint flags;        // bit 0 = TREE_FLOD_BACKED (vk_TreeManager.h)
};
#define TREE_FLOD_BACKED 1u
#define TREE_VOX_BACKED  2u
// Bits 8..23 of `flags` are the crown's row in the per-species voxel LOD table; 24..31 = the FLOD group's k (nfs21).
#define TREE_VOXLOD_SHIFT 8u
#define TREE_VOXLOD_MASK  0xFFFFu
layout(set = 0, binding = 0, std430) readonly buffer MetaBuf {
    TreeMeta meshes[];
};

// binding 1 was a single-buffered frustum UBO — REMOVED. Written by the CPU each
// frame into shared mapped memory, it raced the in-flight frames: while rotating,
// the next frame's CPU write clobbered this frame's planes before the GPU cull read
// them, so the colour cull tested a DIFFERENT (rotated) frustum than the depth
// prepass → edge trees landed in the prepass depth but were culled from colour →
// black tree silhouettes at the screen edge (only on the iGPU, by frame pacing).
// The frustum now travels in PUSH CONSTANTS (recorded per dispatch → no aliasing).
// The descriptor still declares binding 1 (unused) to avoid touching the layout.

// binding 2: VkDrawIndexedIndirectCommand array (20 B each), write-only.
struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};
layout(set = 0, binding = 2, std430) writeonly buffer IndirectBuf {
    DrawCommand commands[];
} drawCmds;

// binding 3: per-group visible draw count, read-write (atomic append cursor).
layout(set = 0, binding = 3, std430) buffer CountBuf {
    uint counts[];
} drawCount;

layout(push_constant) uniform PC {
    vec4 planes[6];     // view frustum (6 normalized planes) — per-frame, race-free
    uint  mesh_count;   // trees in this group
    float cut_dist;     // r_tree_dist: drop FLOD-backed trees past this (<=0 = off)
    uint mesh_offset;   // start index in metadata buffer for this group
    uint output_base;   // base index into indirect command buffer (group * maxGroupMesh)
    uint count_index;   // index into draw-count buffer (= group)
    // 128 B EXACTLY = guaranteed push limit. eye_* are SCALARS on purpose: a vec3
    // here aligns to 16 B and would overflow. Mirrors VK::TreeCullPush field for
    // field (vk_TreeManager.h) — keep both in step.
    float eye_x;        // camera position — reference for the distance cut
    float eye_y;
    float eye_z;
    mat4  viewProj;     // @128 (mat4 is 16-B aligned; 128 is) — sphere -> clip for the HZB
    uint  hzb_on;       // 1 = run the Hi-Z occlusion test
    float hzb_focal;    // 1/tan(fovY/2) — true screen footprint of the sphere
    float vox_cut;      // r_tree_vox_dist: hand VOX-backed crowns to the cube cloud
    float vox_k0;       // cube edge(d) = d·(vox_k0 + vox_k1·d) — see VK::TreeCullPush
    float vox_k1;
    float vox_fade;     // level crossfade band, fraction of the handover cell size
    uint  vox_all;      // 1 = cube every backed crown at any range (r_vsm_tree_hull_debug)
    uint  vox_count;    // slot of the cube draw counter inside drawCount.counts[]; +1 = cube total
    float scr_h;        // render height in pixels — turns a sphere radius into a SCREEN size
    uint  stats_on;     // r_tree_stats: 1 = fill the cull ledger at binding 8 (0 = untouched)
    // 0 = COLOUR cull, 1 = camera DEPTH PREPASS cull (r_tree_depth_gpu).
    // ⭐⭐⭐ THE TWO MODES LIVE IN ONE SHADER ON PURPOSE, AND THE DIFFERENCE BETWEEN
    // THEM IS THE WHOLE SAFETY ARGUMENT. Colour must stay a SUPERSET of the prepass:
    // a tree that writes prepass depth but is culled from colour is a BLACK SILHOUETTE.
    // Colour therefore measures its distance cuts from the sphere's NEAR side
    // (dist - R, keeps more), the prepass from the CENTRE (keeps strictly fewer). That
    // asymmetry used to be split across this shader and a CPU loop in RenderDepth,
    // where the two could drift apart without anything noticing. Here they are four
    // lines apart and share every other test.
    uint  mode;
    // SHADOW ONLY (both are 0 in the two camera modes). Camera-distance band that
    // splits a sun cascade into the CACHED far layer and the PER-FRAME near layer whose
    // crowns sway with the wind. Measured from the sphere CENTRE with no radius slack,
    // because the CPU walk this replaces measured it that way and the two layers have to
    // stay COMPLEMENTARY: a crown leaving the near band must enter the far one on the
    // same comparison, or its shadow blinks out for a frame at the handover.
    float min_dist;
    float max_dist;
    float size_lod;     // nfs21/23: 0 = old near-side metric; else |value| = band, cut by the GROUP centre at cut_dist * k * band (k = q/64 if > 0, 1 if < 0)
    float glod_start;   // nfs21: R4 r_ssaGLOD_start for the sliding windows (0 = window 0 always)
    float glod_end;
} pc;
// nfs21: sliding windows of progressive crowns -- per tree (base | count << 24) and the (first, count) table.
layout(set = 0, binding = 9, std430) readonly buffer TreeAuxBuf { uvec4 treeAux[]; };   // nfs23: xyz group centre bits, w swi
layout(set = 0, binding = 10, std430) readonly buffer SwiWinBuf { uvec2 swiWin[]; };
#define CULL_COLOUR  0u
#define CULL_PREPASS 1u
// 2 = a SHADOW target (r_tree_shadow_gpu): the sun far map, one sun-cascade layer, or
// the rain map. `planes` then carry the LIGHT's box/cone instead of the camera frustum,
// while eye_* / min_dist / max_dist stay the CAMERA's — the layer split is a camera
// distance even though the projection belongs to the light.
#define CULL_SHADOW  2u

// ---------------------------------------------------------------------------
// CROWN CUBE TIER (r_tree_vox_dist) — bindings 5..7.
// ---------------------------------------------------------------------------
// The middle rung of the tree LOD ladder lives in THIS shader rather than one of
// its own, and that is the point: the crown that leaves the mesh here is the same
// crown that arrives as cubes, tested against the same frustum, the same Hi-Z and
// the same distance in the same invocation. A second cull pass would have had to
// reproduce all three and could only ever drift from them.
#define VOX_LODS     5u      // == CTreeManager::kHullLods
#define VOX_DRAW_CAP 32768u  // == CTreeManager::kVoxDrawCap

// Per-SPECIES ladder, VOX_LODS rows per unique mesh, finest → coarsest. Mirrors
// VK::TreeVoxLod (16 B). `size` is the grid cell edge in mesh-local metres.
// ⚠⚠ THE `_pad` IS LOad-BEARING. VK::TreeVoxLod carries a trailing u32; drop it here
// and std430 gives this array a stride of 12 while the upload writes 16, so row 0
// reads correctly and EVERY row after it is off by four bytes per step. That shipped
// once (24-08) and read as a working tier: `count` came back as garbage, most crowns
// silently emitted nothing and a handful emitted hundreds of millions of cubes —
// 873 ms in the Trees zone with only 23 draws to show for it.
// ⭐ The lesson is narrower than "check the layout": spirv-dis DID say ArrayStride 12,
// and that was read as confirmation. A stride is only ever confirmation when BOTH
// sides of it were looked at in the same breath.
struct VoxLod { uint first; uint count; float size; uint _pad; };
layout(set = 0, binding = 5, std430) readonly buffer VoxLodBuf { VoxLod voxLod[]; };
// VkDrawIndirectCommand[] for the cube draw (16 B: vertex/instance/firstVertex/firstInstance).
struct DrawCmd { uint vertexCount; uint instanceCount; uint firstVertex; uint firstInstance; };
layout(set = 0, binding = 6, std430) writeonly buffer VoxCmdBuf { DrawCmd voxCmd[]; };
// Mirrors VK::GpuVoxDraw (16 B) — read by tree_vox.vert through gl_InstanceIndex.
struct VoxDraw { uint tree; float cubeSize; float density; float fade; };
layout(set = 0, binding = 7, std430) writeonly buffer VoxDrawBuf { VoxDraw voxDraw[]; };

// ---------------------------------------------------------------------------
// CULL LEDGER (binding 8, r_tree_stats) — a flat u32 array, indices mirrored in
// vk_TreeManager.h (TreeCullStat).
// ---------------------------------------------------------------------------
// This shader is the ONE place in the frame that looks at every tree on the level
// with a camera in hand, so it is the only place that can answer the questions an
// optimisation needs answered BEFORE it is written:
//   • which test is actually doing the culling (a knob that removes nothing is not
//     a knob — r_tree_hzb spent a session at "-18%" with no ms attached);
//   • how the surviving TRIANGLES are distributed by screen size. Instance counts
//     mislead here by an order of magnitude: one crown is 500 tris and another is
//     40 000, so "1227 trees" says nothing about the raster load. The px histogram
//     is the one that decides whether a LOD tier can pay — if most triangles sit in
//     crowns under 8 px across, a cheaper representation has something to win;
//     if they sit in the 64 px+ bins, no LOD ladder will ever help and the answer
//     is fragment cost (which is where r_tree_shade_dist already landed).
// ⚠ Every invocation accumulates into SHARED memory first and the workgroup flushes
// once. 193 471 invocations hammering ~48 global addresses would serialise on the
// atomic unit and measure the instrument instead of the frame.
#define ST_TESTED      0u   // invocations with a live mesh_count
#define ST_DEGEN       1u   // sphere_R <= 0 (no geometry)
#define ST_CUT_FLOD    2u   // dropped by r_tree_dist (billboard takes over)
#define ST_CUT_FRUSTUM 3u
#define ST_CUT_HZB     4u   // dropped by r_tree_hzb (occluded)
#define ST_TO_VOX      5u   // crown handed to the cube tier
#define ST_DRAWN       6u   // reached the indirect buffer
#define ST_OVERFLOW    7u   // group section full — a DROPPED tree, i.e. a visible bug
#define ST_TRI_DRAWN   8u   // triangles behind ST_DRAWN
#define ST_TRI_FLOD    9u   // triangles the FLOD cut saved
#define ST_TRI_HZB    10u   // triangles the Hi-Z test saved
#define ST_TRI_VOX    11u   // triangles the cube tier took over
#define ST_TRI_FRUS   12u   // triangles the frustum saved
#define ST_PX_SUM     13u   // sum of drawn screen diameters x16 (fixed point -> mean)
#define ST_PX_MAX     14u   // largest drawn screen diameter, px (atomicMax)
#define ST_TRI_MAX    15u   // heaviest single drawn tree, triangles (atomicMax)
#define ST_DIST_N     16u   // [16..23] drawn instances per distance octave
#define ST_DIST_TRI   24u   // [24..31] their triangles
#define ST_PX_N       32u   // [32..39] drawn instances per screen-size octave
#define ST_PX_TRI     40u   // [40..47] their triangles
#define ST_COUNT      48u
layout(set = 0, binding = 8, std430) buffer StatsBuf { uint stat[]; };
shared uint s_stat[ST_COUNT];

// Local accumulate. `stats_on` is uniform across the dispatch, so these calls sit in
// uniform control flow even though the culling around them does not.
void stAdd(uint slot, uint v) { if (pc.stats_on != 0u) atomicAdd(s_stat[slot], v); }
void stMax(uint slot, uint v) { if (pc.stats_on != 0u) atomicMax(s_stat[slot], v); }

// One cube draw. 18 vertices per cube, not 36: tree_vox.vert emits only the three
// camera-facing faces, which cover the cube's silhouette exactly.
//   firstVertex  = 18·lod.first  → gl_VertexIndex/18 IS the global voxel index
//   firstInstance = the draw's own slot → gl_InstanceIndex indexes voxDraw[]
// (the same "address the payload through firstInstance" trick the VSM voxel caster
// uses for its page routing).
void voxEmit(VoxLod lod, uint tree, float fade, float cubeSize)
{
    if (lod.count == 0u) return;
    // Density thinning: once the rendered cube outgrows this grid's cell, cull the
    // matching fraction of instances in the VS — (cell/cube)² keeps the crown's
    // covered area constant, so the cube COUNT stays continuous across a grid swap.
    float r = lod.size / max(cubeSize, 1e-4);
    uint slot = atomicAdd(drawCount.counts[pc.vox_count], 1u);
    if (slot >= VOX_DRAW_CAP) { atomicAdd(drawCount.counts[pc.vox_count], -1); return; }
    // field by field: a whole-struct store into an SSBO made Adreno 750 fail the pipeline (Android, 14-10: no trees)
    voxCmd[slot].vertexCount   = 18u * lod.count;
    voxCmd[slot].instanceCount = 1u;
    voxCmd[slot].firstVertex   = 18u * lod.first;
    voxCmd[slot].firstInstance = slot;
    voxDraw[slot].tree     = tree;
    voxDraw[slot].cubeSize = cubeSize;
    voxDraw[slot].density  = min(1.0, r * r);
    voxDraw[slot].fade     = fade;
    // Telemetry, and not a luxury: a draw carries anything from a dozen cubes to a whole
    // level-0 grid, so the draw count alone cannot tell a cheap tier from a ruinous one.
    atomicAdd(drawCount.counts[pc.vox_count + 1u], lod.count);
}

// Pick the level and the continuous cube size for one crown, then emit 1 draw (or 2
// while crossfading). Ported from the CPU viewmode loop this replaced — the comments
// there explained the geometry, so they travel with it:
//   • the rendered cube edge is CONTINUOUS in distance (no size steps at all); the
//     baked grids only supply POSITIONS, so a grid swap changes where cubes sit but
//     never how big they are, and the dither hides the swap itself;
//   • growth is capped near the crown extent so a far tree converges to one block
//     instead of a comically oversized cube;
//   • the crossfade is computed in SIZE space (T vs the next cell S) so the band
//     ENDS exactly where the grids swap, for any px-vs-distance curve.
void voxSelect(uint tree, uint species, float dist)
{
    uint row = species * VOX_LODS;
    float d  = max(dist, 1.0);
    float T  = d * (pc.vox_k0 + pc.vox_k1 * d);
    float sLast = voxLod[row + VOX_LODS - 1u].count != 0u ? voxLod[row + VOX_LODS - 1u].size
                                                          : voxLod[row].size;
    T = min(T, 4.0 * sLast);
    uint lvl = 0u;
    while (lvl + 1u < VOX_LODS && voxLod[row + lvl + 1u].size <= T
           && voxLod[row + lvl + 1u].count != 0u) ++lvl;
    float cube = max(T, voxLod[row].size);
    float f = 0.0;
    if (pc.vox_fade > 0.001 && lvl + 1u < VOX_LODS && voxLod[row + lvl + 1u].count != 0u) {
        float S = voxLod[row + lvl + 1u].size;
        f = clamp((T - S * (1.0 - pc.vox_fade)) / (S * pc.vox_fade), 0.0, 1.0);
    }
    if (f > 0.004) {
        voxEmit(voxLod[row + lvl],      tree,  f, cube);   // fine grid dissolves OUT
        voxEmit(voxLod[row + lvl + 1u], tree, -f, cube);   // coarse dissolves IN, same cube size
    } else {
        voxEmit(voxLod[row + lvl], tree, 0.0, cube);
    }
}

// Hi-Z pyramid (MAX depth, GENERAL layout), shared with grass/world. Bound to a
// 1x1 dummy when occlusion is off, so the descriptor is always valid.
layout(set = 0, binding = 4) uniform sampler2D u_HZB;

bool FrustumTest(vec3 center, float radius)
{
    // This GPU colour cull MUST be a SUPERSET of the depth-prepass cull, otherwise a
    // tree at the frustum boundary can land in the prepass depth (occluding the world
    // → black clear) yet be culled from the colour pass → black tree silhouettes at
    // the screen edge while rotating. The boundary decision diverges between GPUs at
    // FP precision (the 780M iGPU showed it; the dGPU did not). Two changes keep us a
    // superset of the prepass (vk_pass_world.cpp builds CFrustum with LRTB|FAR):
    //   1. SKIP the near plane (index 4) — the prepass has no near plane.
    //   2. Add a metre of slack — the CPU CFrustum and this GPU extraction are
    //      different code paths, so absorb any plane-equation difference.
    // Planes are normalized (vk_cull.h), so dot+w is metres → the margin is metres.
    // Plane order: 0=L 1=R 2=B 3=T 4=N 5=F.
    //
    // ⭐ THE PREPASS TAKES NO MARGIN, AND THAT IS THE SAME ARGUMENT INVERTED. The slack
    // exists so the COLOUR set can never fall short of the prepass set at the frustum
    // boundary; handing it to the prepass as well only widens the prepass, and matches
    // what the CPU walk did before (CFrustum, no slack). Colour keeps the metre, the
    // prepass does not, so colour stays the superset by construction.
    // ⚠ Worth ~1.5% of the prepass fragment count here, not more — measured INSIDE one
    // run (GPU 40.1-40.7 M vs CPU walk 39.4 M). An earlier reading of "34 -> 41 M" was
    // a comparison BETWEEN runs and meant nothing: the scene differs between them. That
    // trap is old and documented; it caught this edit too.
    //
    // ⭐ A SHADOW TARGET IS NEITHER, AND ITS RULE COMES FROM THE CPU WALK IT REPLACES.
    // ShadowMap::SphereVisible / CascadeSphereVisible / RainSphereVisible all test the
    // sphere against SIX slabs of the light box — the near one included — with exactly
    // `+radius` and no slack. So shadows take all six planes and margin 0. The
    // superset argument that shaped the two camera modes does not apply here at all: a
    // shadow map has no colour twin to stay ahead of, and widening the set would only
    // raster casters the light cannot see.
    // ⚠ The planes come from the LIGHT's VP, so for an ortho cascade they ARE the box
    // faces and the near plane is a real cut (a caster behind it is behind the light).
    // Skipping it the way the camera modes do would keep the entire level.
    const bool  shadow = (pc.mode == CULL_SHADOW);
    const float margin = (pc.mode == CULL_COLOUR) ? 1.0 : 0.0;
    for (int i = 0; i < 6; ++i)
    {
        if (i == 4 && !shadow) continue;   // near plane — excluded to match the prepass set
        if (dot(pc.planes[i].xyz, center) + pc.planes[i].w < -radius - margin)
            return false;
    }
    return true;
}

// Screen DIAMETER of a bounding sphere, in pixels. focal = 1/tan(fovY/2), so
// R/dist·focal is the NDC half-extent and ·scr_h/2 turns that into pixels; ×2 for the
// diameter. dist is floored at the radius so a camera inside a crown reports its size
// instead of dividing by ~0. This is the same footprint metric the Hi-Z test uses —
// deliberately, so the histogram bins and the occlusion test speak one language.
float screenPx(vec3 c, float r, vec3 eye)
{
    float d = max(distance(c, eye), max(r, 0.01));
    return r / d * pc.hzb_focal * pc.scr_h;
}

// Octave bin, 0..7. Both histograms are log2 because that is how both quantities
// actually behave: a tree at 30 m and one at 35 m are the same problem, a tree at
// 30 m and one at 300 m are not.
uint octave(float v, float base)
{
    return uint(clamp(floor(log2(max(v, base) / base)), 0.0, 7.0));
}

void cullOne(uint local)
{
    if (local >= pc.mesh_count)
        return;
    stAdd(ST_TESTED, 1u);

    TreeMeta m = meshes[pc.mesh_offset + local];
    if (m.sphere_R <= 0.0) {
        stAdd(ST_DEGEN, 1u);
        return;
    }
    const uint tris = m.index_count / 3u;

    // Distance cut (r_tree_dist) — ONLY for trees CLODManager replaces with a
    // billboard past kImposterMinDist. Without this the full mesh AND the imposter
    // both render out there; trees lacking the flag have no stand-in and are kept
    // at any range. Measured from the sphere's NEAR side (dist - R), which keeps
    // strictly more trees than the prepass twin in RenderDepth (it compares centre
    // distance) — that direction is mandatory: colour must be a SUPERSET of the
    // prepass or edge trees land in depth with nothing shading them (black
    // silhouettes, the same failure the frustum margin below guards against).
    // Distance reference: near side of the sphere for COLOUR, centre for the PREPASS.
    // See the `mode` note in the push block — this one line is what keeps the colour
    // set a superset of the prepass set.
    const vec3  eyePos = vec3(pc.eye_x, pc.eye_y, pc.eye_z);
    const float dCentre = distance(m.sphere_P, eyePos);
    const float dCut    = (pc.mode == CULL_COLOUR) ? (dCentre - m.sphere_R) : dCentre;

    // ---- SHADOW LAYER BAND (r_tree_shadow_gpu) ------------------------------
    // The wind hybrid: near crowns re-raster every frame at their current sway into
    // the cascade's dynamic overlay, the far forest sits frozen in the cached static
    // map. One camera-distance comparison decides which map a tree belongs to, and it
    // is the FIRST test here on purpose — it is the cheapest and, for the per-frame
    // dynamic layer, by far the most selective (a 60 m band out of a 2 km level).
    // ⚠ Counted before the box test like the FLOD cut above, so ST_* would read as
    // "whole level", not "in the light box". The shadow modes push stats_on = 0 — the
    // ledger describes the COLOUR cull and mixing five more dispatches into the same
    // 48 counters would make every one of its numbers a sum of unrelated passes.
    if (pc.mode == CULL_SHADOW
        && (dCentre < pc.min_dist || dCentre >= pc.max_dist))
        return;

    // nfs21: the R1-style size-scaled cut (the group's billboard starts at the same distance, lod_cull_v2)
    // nfs23: under the size LOD the cut is by the GROUP centre (the billboard's own metric) and lasts the crossfade band
    // The GROUP-centre metric is the fix for "billboard in front, the detailed crown still swaying behind it" (and
    // the opposite hole: a tree on the far side of its group cut by its own near edge while the group, measured at
    // its centre, has no billboard yet) -- so it is on whenever |size_lod| > 0.5, the size LOD or not.
    // size_lod > 0: size LOD on -> k = q/64, band = size_lod. size_lod < 0: size LOD off -> k = 1, band = -size_lod.
    float cutDistT = dCut, cutLim = pc.cut_dist;
    // (VOX-backed crowns too: k has its own bits since nfs21 -- otherwise a vox crown and its trunk would be cut by
    // different metrics and the crown would double-draw over its group's billboard.)
    if (abs(pc.size_lod) > 0.5) {
        const uint q = (m.flags >> 24u) & 255u;
        if (q != 0u) {
            const vec3 gc = uintBitsToFloat(treeAux[pc.mesh_offset + local].xyz);
            cutDistT = distance(gc, eyePos);
            cutLim   = pc.cut_dist * ((pc.size_lod > 0.0) ? float(q) / 64.0 : 1.0) * abs(pc.size_lod);
        }
    }
    if (pc.cut_dist > 0.0 && (m.flags & TREE_FLOD_BACKED) != 0u) {
        if (cutDistT > cutLim) {
            // ⚠ Counted BEFORE the frustum test, matching the code order — so this
            // number covers the whole level, not the visible cone. Read it as "work
            // the billboard swap removed", never as "trees that were on screen".
            stAdd(ST_CUT_FLOD, 1u); stAdd(ST_TRI_FLOD, tris);
            return;
        }
    }

    // Voxel cut (r_tree_vox_dist) — the middle rung of the ladder, one step nearer and
    // built the same way: crowns with a baked cube cloud on the GPU hand the mesh over
    // to it; crowns without the flag (trunks, unbaked species) keep the mesh at any
    // range. Near side of the sphere again, so the CPU prepass twin in RenderDepth —
    // which compares CENTRE distance — stays a subset and colour ⊇ prepass still holds.
    //
    // ⚠ Unlike the FLOD cut above, this crown does not simply disappear: the SAME
    // invocation emits its replacement below, after the frustum and Hi-Z tests. That is
    // why the decision is only recorded here and acted on twice.
    const float dCen = dCentre;
    bool voxBacked = (m.flags & TREE_VOX_BACKED) != 0u;
    bool meshCut   = pc.vox_cut > 0.0 && voxBacked && (dCut > pc.vox_cut);
    // vox_all (r_vsm_tree_hull_debug) cubes every backed crown at any range WITHOUT
    // cutting the mesh, i.e. the old overlay viewmode — which is also how the tier's
    // cost is measured against the mesh it is meant to replace.
    // ⚠ The PREPASS never emits cubes: the cube tier draws into the COLOUR pass and
    // writes its own depth there. Emitting them here would produce a second, duplicate
    // set of cube draws — and the prepass has no pipeline bound to draw them with.
    bool cubes     = (pc.mode == CULL_COLOUR) && voxBacked && (pc.vox_all != 0u || meshCut);

    if (!FrustumTest(m.sphere_P, m.sphere_R)) {
        stAdd(ST_CUT_FRUSTUM, 1u); stAdd(ST_TRI_FRUS, tris);
        return;
    }

    // ---- Hi-Z occlusion (r_tree_hzb): drop trees fully behind nearer geometry --
    // Safe against the black-silhouette failure mode WITHOUT a matching test in
    // the depth prepass, and the reason is worth stating: the pyramid is a MAX
    // reduction of THIS frame's finished prepass depth, which the trees are part
    // of. A tree visible in the prepass has its own depth inside its footprint, so
    // hzbDepth >= its near face and the test can never cull it. A tree culled here
    // therefore contributed NOTHING to the depth buffer — nothing is left unshaded.
    // (Same argument as world_cull_hzb.comp; alpha-tested gaps only ever raise the
    // max toward the background, i.e. they err toward keeping.)
    // The maths lives in hzb_test.glsl, shared with world_cull_hzb / lod_cull.
    // It used to be copied into each of them, and BOTH fixes it carries (the
    // focal-scale footprint, then the mip clamp + texel-space block) had to be
    // re-applied by hand per copy — which is exactly how this shader spent a
    // while false-culling against a mip that did not exist.
    if (pc.hzb_on != 0u
        && HZB_SphereOccluded(u_HZB, pc.viewProj, m.sphere_P, m.sphere_R,
                              eyePos, pc.hzb_focal)) {
        // ⭐ THE honest measure of r_tree_hzb: triangles, not instances. The test was
        // signed off on "-18% of the instance count" with no ms behind it, and an
        // instance count cannot distinguish 289 saplings from 289 full canopies.
        stAdd(ST_CUT_HZB, 1u); stAdd(ST_TRI_HZB, tris);
        return;   // fully behind the farthest surface in its footprint
    }

    // Cube tier. The Hi-Z verdict above is REUSED rather than re-derived, and it is
    // sound for cubes too: the pyramid is a MAX reduction of the prepass, a cut crown
    // contributed nothing to it, so what the test sees in that footprint is whatever
    // stands BEHIND the crown — the test can only fire when something else genuinely
    // occludes it. (For the mesh the argument runs the other way; see above.)
    if (cubes)
        voxSelect(pc.mesh_offset + local, (m.flags >> TREE_VOXLOD_SHIFT) & TREE_VOXLOD_MASK, dCen);
    if (meshCut) {
        stAdd(ST_TO_VOX, 1u); stAdd(ST_TRI_VOX, tris);
        return;   // the cubes ARE this crown now
    }

    uint out_idx = atomicAdd(drawCount.counts[pc.count_index], 1);
    if (out_idx >= pc.mesh_count)   // never overruns the group section
    {
        atomicAdd(drawCount.counts[pc.count_index], -1);
        stAdd(ST_OVERFLOW, 1u);     // a tree that vanished from the frame — never expected
        return;
    }

    // ---- Survivor. Everything below is the ledger of what we are about to raster.
    if (pc.stats_on != 0u) {
        const float px = screenPx(m.sphere_P, m.sphere_R, eyePos) * 2.0;
        stAdd(ST_DRAWN, 1u);
        stAdd(ST_TRI_DRAWN, tris);
        stAdd(ST_PX_SUM, uint(min(px, 262144.0) * 16.0));
        stMax(ST_PX_MAX, uint(min(px, 65535.0)));
        stMax(ST_TRI_MAX, tris);
        // 6.25 m and 1 px chosen so the first bin is a catch-all for "right here" and
        // "sub-pixel" rather than a bin nothing ever lands in.
        const uint db = octave(dCen, 6.25);
        stAdd(ST_DIST_N + db, 1u); stAdd(ST_DIST_TRI + db, tris);
        const uint pb = octave(px, 1.0);
        stAdd(ST_PX_N + pb, 1u);   stAdd(ST_PX_TRI + pb, tris);
    }

    // nfs21: R4 FTreeVisual_PM::select_lod_id -- the sliding window from the crown's screen share.
    uint drawCount_ = m.index_count, drawFirst_ = m.ib_first;
    if (pc.glod_start > 0.0) {
        const uint si = treeAux[pc.mesh_offset + local].w;
        const uint nw = si >> 24u;
        if (nw > 1u) {
            const float ssa = m.sphere_R / max(dCentre * dCentre, 1e-4);
            const float lod = sqrt(clamp((ssa - pc.glod_end) / (pc.glod_start - pc.glod_end), 0.01, 1.0));
            const uint  id  = min(uint(floor((1.0 - lod) * float(nw - 1u) + 0.5)), nw - 1u);
            const uvec2 w   = swiWin[(si & 0xFFFFFFu) + id];
            drawFirst_ = w.x;
            drawCount_ = w.y;
        }
    }
    uint ci = pc.output_base + out_idx;
    drawCmds.commands[ci].indexCount    = drawCount_;
    drawCmds.commands[ci].instanceCount = 1;
    drawCmds.commands[ci].firstIndex    = drawFirst_;
    drawCmds.commands[ci].vertexOffset  = int(m.first_vertex);
    drawCmds.commands[ci].firstInstance = pc.mesh_offset + local; // → gl_InstanceIndex in tree.vert
}

void main()
{
    // ⚠ The cull proper lives in cullOne() and NOT here, for one structural reason:
    // it is full of early `return`s, and a barrier() reached by only some invocations
    // of a workgroup is undefined behaviour. Returning from a callee skips no barrier
    // in the caller, so the ledger's zero/flush pair stays in uniform control flow
    // whatever the culling decides. `pc.stats_on` is dispatch-uniform, so gating the
    // barriers on it is legal too.
    if (pc.stats_on != 0u) {
        if (gl_LocalInvocationID.x < ST_COUNT) s_stat[gl_LocalInvocationID.x] = 0u;
        barrier();
    }

    cullOne(gl_GlobalInvocationID.x);

    if (pc.stats_on != 0u) {
        barrier();
        const uint i = gl_LocalInvocationID.x;
        if (i < ST_COUNT) {
            const uint v = s_stat[i];
            // The two extrema are maxima, not sums — folding them with atomicAdd would
            // read as "the heaviest tree got heavier the more workgroups ran".
            if (i == ST_PX_MAX || i == ST_TRI_MAX) { if (v != 0u) atomicMax(stat[i], v); }
            else if (v != 0u)                        atomicAdd(stat[i], v);
        }
    }
}

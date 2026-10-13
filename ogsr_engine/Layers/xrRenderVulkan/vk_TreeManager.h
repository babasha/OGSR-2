// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - GPU-driven tree manager.
//
// Trees (MT_TREE_ST / MT_TREE_PM) extracted from Visuals[] at level load,
// uploaded as metadata + transforms SSBOs, then drawn each frame via a
// compute cull → indirect draw pipeline (Session B). Architecture mirrors
// the GPU-driven grass path (CDetailManager); replaces R4's DSGraph
// FloraVertData streaming.
//
// SESSION A SCOPE (this file): extraction + SSBO upload + per-texture
// descriptor sets + indirect/draw-count buffer allocation. NO compute or
// graphics pipeline yet — that's Session B.

#pragma once
#include "vk_core.h"
#include "vk_buffer.h"

// Visual classes live in the global namespace (vk_Visual.h).
class vkRender_Visual;
class vkFTreeVisual;
class vkFHierrarhyVisual;
class CFrustum;   // camera-frustum cull for the depth-prepass path

namespace VK
{

class CVulkanTexture;
struct FrameContext;

// Tree graphics push constants (160 B, VS+FS). Matches tree.vert/frag.
struct TreeGfxPush
{
    Fmatrix  mViewProj;    // 64 B  world → clip
    float    uvScale;      //  4 B  1/2048 (FTreeVisual_quant = 32768/16)
    float    alphaRef;     //  4 B  fragment alpha cutoff
    float    lodBand;      //  4 B  R1 crossfade band D_B/D_A (> 1 = the crown dithers OUT where its billboard dithers in;
                           //       0 = off -- casters, prepass on). Base D = vSunColor.w. Was statsOn (the bitset is gone).
    float    shadeDist;    //  4 B  r_tree_shade_dist — metres past which tree.frag drops its subtle terms
                           //       (vec4 below needs 16-byte alignment → offset 80, so these two are free anyway)
    Fvector4 vSunColor;    // 16 B  env sun colour (rgb); w = the crossfade's base D (ImposterMinDist), see lodBand
    Fvector4 vHemiColor;   // 16 B  env hemi colour (rgb); w unused
    Fvector4 wind_params;  // 16 B  SSFX (wind_direction, wind_velocity, _, _)
    Fvector4 wsetup_trees; // 16 B  SSFX (branchSpeed, trunkSpeed, bend, minWindSpeed)
    Fvector4 wind_anim;    // 16 B  Environment.wind_anim drift (xyz); w unused
};
static_assert(sizeof(TreeGfxPush) == 160, "TreeGfxPush must be 160 B");

// Tree wind-sway motion-vector push (VS+FS) — pairs with tree_motion.{vert,frag}.
// Projects the cur AND prev wind pose so swaying trees carry their true screen
// motion; the fullscreen MV pass only reconstructs camera motion from depth and
// misses the sway. wsetup/wind_params are frame-stable-ish; wind_anim (drift) is the
// dominant per-frame delta. 240 B (device max push 256).
struct TreeMotionPush
{
    Fmatrix  curVP;             // 0    this frame's view-proj, UNJITTERED (jitter-free MV)
    Fmatrix  prevVP;            // 64   previous frame's view-proj, UNJITTERED
    float    uvScale;           // 128  1/2048
    float    alphaRef;          // 132  leaf cutout (fragment)
    float    jitterX;           // 136  this frame's sub-pixel jitter (D3D-NDC), re-applied to gl_Position
    float    jitterY;           // 140
    Fvector4 wind_params;       // 144  cur
    Fvector4 wsetup_trees;      // 160  cur
    Fvector4 wind_anim;         // 176  cur
    Fvector4 wind_params_prev;  // 192
    Fvector4 wsetup_trees_prev; // 208
    Fvector4 wind_anim_prev;    // 224
};
static_assert(sizeof(TreeMotionPush) == 240, "TreeMotionPush must be 240 B");

// View frustum UBO (binding 1 of the cull set). 6 normalized planes, std140.
// LEGACY: kept only so the cull descriptor still has a valid buffer bound at
// binding 1 — the planes now travel via TreeCullPush (see below) to avoid the
// single-buffered cross-frame race that caused black tree silhouettes at the
// screen edge during rotation.
struct TreeFrustumUBO
{
    Fvector4 planes[6];  // 96 B
};

// Cull compute push constants (208 B). The frustum planes ride in push constants
// (recorded per dispatch) instead of the racy single-buffered UBO. planes FIRST
// (offset 0) keeps the trailing scalars 4-aligned. Mirrors tree_cull.comp's
// push_constant block.
// ⚠ eye_* are three SCALARS, not an Fvector/vec3: a vec3 in the shader's push
// block aligns to 16 B, which would shift every following offset.
// ⚠ 208 B EXCEEDS the 128 B guaranteed minimum — legal here because this renderer
// already ships TreeMotionPush at 240 B, i.e. it targets the 256 B every desktop
// GPU provides. CreateCullPipeline checks Caps.maxPushConstantsSize and refuses
// to build rather than letting vkCreatePipelineLayout fail obscurely.
// tree_gi.comp push block. Four scalars, so std430 and C++ agree trivially — the
// mine in [[vulkan-push-constant-std430-mismatch]] is vec2/vec3 alignment, and this
// block deliberately has none. The pads exist to keep the block 16 B aligned rather
// than to carry anything.
struct TreeGIPush
{
    u32   count;      // trees in the level
    float maxDist;    // skip trees further than this from the eye (draw distance + margin)
    float pad0 = 0.f;
    float pad1 = 0.f;
};
static_assert(sizeof(TreeGIPush) == 16, "TreeGIPush must stay 16 B (matches tree_gi.comp)");

struct TreeCullPush
{
    Fvector4 planes[6];   // 96 B  view frustum (normalized; 0=L 1=R 2=B 3=T 4=N 5=F)
    u32      mesh_count;   //  4 B  trees in this group
    float    cut_dist;     //  4 B  r_tree_dist: drop FLOD-backed trees past this (0 = off)
    u32      mesh_offset;  //  4 B  start index in the metadata buffer
    u32      output_base;  //  4 B  base index into the indirect command buffer
    u32      count_index;  //  4 B  index into the draw-count buffer (= group)
    float    eye_x;        //  4 B  camera position — distance cut + HZB near face
    float    eye_y;        //  4 B
    float    eye_z;        //  4 B
    Fmatrix  viewProj;     // 64 B  @128 (mat4 needs 16-B alignment — 128 is aligned)
    u32      hzb_on;       //  4 B  1 = run the Hi-Z occlusion test
    float    hzb_focal;    //  4 B  1/tan(fovY/2) — true screen footprint of the sphere
    float    vox_cut;      //  4 B  r_tree_vox_dist: drop VOX-backed crowns past this (0 = off)
    // Cube-size curve, PRE-FOLDED on the CPU so the shader carries two numbers instead
    // of five: edge(d) = d·(vox_k0 + vox_k1·d), which is the old
    // dist × pxAt1m × (vox_px + vox_px_far·dist/100) × 2^lodBias written out. Folding
    // it here also keeps FOV/resolution/bias out of the shader entirely.
    float    vox_k0;       //  4 B  pxAt1m · vox_px · 2^bias
    float    vox_k1;       //  4 B  pxAt1m · vox_px_far · 0.01 · 2^bias
    float    vox_fade;     //  4 B  r_vsm_tree_hull_vox_fade — level crossfade band (0 = hard swap)
    u32      vox_all;      //  4 B  1 = r_vsm_tree_hull_debug: cube EVERY backed crown, ignore vox_cut
    u32      vox_count;    //  4 B  slot of the cube draw counter in the shared count buffer (= group count)
    float    scr_h;        //  4 B  render height in px — with hzb_focal this turns a sphere into a SCREEN SIZE
    u32      stats_on;     //  4 B  r_tree_stats: 1 = fill the cull ledger (binding 8)
    u32      mode;         //  4 B  0 = colour, 1 = camera depth prepass, 2 = shadow target
    // SHADOW targets only (0 / 1e9f elsewhere): the camera-distance band that splits a
    // cascade into its cached far layer and its per-frame near (wind) layer. Centre
    // distance, no radius slack — the same comparison the CPU walk used, so the two
    // layers stay complementary and a crown cannot fall out of both.
    float    min_dist;     //  4 B
    float    max_dist;     //  4 B
    float    size_lod;     //  4 B  nfs21: 1 = cut each FLOD-backed tree at cut_dist * k(group) (flags bits 24..31)
    float    glod_start;   //  4 B  nfs21: R4 r_ssaGLOD_start for the sliding windows (0 = always window 0)
    float    glod_end;     //  4 B  nfs21: R4 r_ssaGLOD_end
};
static_assert(sizeof(TreeCullPush) == 256, "TreeCullPush must be 256 B");
// Mirrors CULL_COLOUR / CULL_PREPASS / CULL_SHADOW in tree_cull.comp.glsl.
enum TreeCullMode : u32 { TREE_CULL_COLOUR = 0, TREE_CULL_PREPASS = 1, TREE_CULL_SHADOW = 2 };

// ============================================================================
// GPU-CULLED SHADOW TARGETS (r_tree_shadow_gpu)
// ============================================================================
// One slot per shadow map the trees are rastered into. Each slot owns a section of
// the shared indirect buffer and of the shared count buffer, exactly the way
// InstanceGPU/ShadowGPU lay theirs out — so a frame can cull every target it is about
// to draw in one batch of dispatches, before any render pass opens.
//
// ⚠ THE TWO CASCADE LAYERS ARE SEPARATE SLOTS, NOT ONE. On a cache-miss frame BOTH
// run: the static map re-rasters the far forest while the dynamic overlay redraws the
// near crowns at this frame's wind pose. They hold disjoint tree sets and are drawn
// into different images, so sharing a slot would give one of them the other's list.
//
// ⚠ The SPOT tiles are deliberately absent. They are perspective cones, they refresh
// on light movement rather than per frame, and each extra slot costs a full
// m_TotalCount command section — the walk they do is real but it is not on the
// per-frame path, which is the whole reason this exists. They keep the CPU walk.
enum TreeShadowTarget : u32
{
    TREE_SH_CASC0_STATIC = 0,   // cascade 0, cached far layer   (redraw only)
    TREE_SH_CASC1_STATIC,       // cascade 1, cached far layer   (redraw only)
    TREE_SH_CASC0_DYN,          // cascade 0, per-frame near/wind layer
    TREE_SH_CASC1_DYN,          // cascade 1, per-frame near/wind layer
    TREE_SH_FAR,                // the far static sun map        (redraw only)
    TREE_SH_RAIN,               // the rain/wetness map          (while raining)
    TREE_SH_COUNT
};

// One target to cull in a DispatchShadowCull batch. vp is the LIGHT's view-projection
// — its six extracted planes are the ortho box (or the cone) the caster is tested
// against; minDist/maxDist are the CAMERA-distance band described above.
struct TreeShadowJob
{
    u32     target;    // TreeShadowTarget
    Fmatrix vp;
    float   minDist = 0.0f;
    float   maxDist = 1e9f;
    // nfs47: centre of the [minDist, maxDist) band. The static and live layers of one cascade must cut around the
    // SAME point (the camera of the last static redraw) or a tree crossing the band edge between redraws is in
    // neither layer / in both. useEye = false -> the current camera.
    bool    useEye  = false;
    Fvector eye{};
    // nfs48: > 0 = R4 sliding-window LOD (tree_cull glod_*) with the colour thresholds scaled by this (bigger =
    // coarser). 0 = full mesh (the shadow default before nfs48).
    float   lodScale = 0.0f;
};

// ============================================================================
// TREE LEDGER (r_tree_stats) — the instrument layer for the tree arc.
// ============================================================================
// Trees are the most expensive system in this renderer and the least measured:
// the frame profiler's `Trees` zone covers ONLY the forward colour pass, while the
// same canopy geometry is also rastered into the depth prepass, the motion vectors,
// the shadow cascades and the VSM atlas. Every optimisation attempted on it so far
// was steered by an instance count, and instance counts have repeatedly pointed the
// wrong way (see vulkan-tree-forward-no-lod-double-draw / tree-voxel-forward-tier).
//
// Three layers, all off at r_tree_stats 0:
//   1. CULL LEDGER — a u32 array the cull compute fills: which test culled what, in
//      instances AND triangles, plus histograms of the survivors by distance and by
//      SCREEN SIZE. The screen-size histogram is the one that answers "is a LOD tier
//      worth building", which is the question two closed arcs answered by building
//      one first.
//   2. HARDWARE COUNTERS — pipeline statistics + a precise occlusion query per pass.
//   3. GPU ZONES + CPU PROBES — the passes the `Trees` zone never covered.
// ----------------------------------------------------------------------------

// Indices into the cull ledger. ⚠ MIRRORED in tree_cull.comp.glsl (ST_*) — the
// shader writes by number, so a field inserted here without a matching edit there
// silently retitles every field below it.
enum TreeCullStat : u32
{
    TS_TESTED = 0, TS_DEGEN, TS_CUT_FLOD, TS_CUT_FRUSTUM, TS_CUT_HZB,
    TS_TO_VOX, TS_DRAWN, TS_OVERFLOW,
    TS_TRI_DRAWN, TS_TRI_FLOD, TS_TRI_HZB, TS_TRI_VOX, TS_TRI_FRUS,
    TS_PX_SUM, TS_PX_MAX, TS_TRI_MAX,
    TS_DIST_N = 16, TS_DIST_TRI = 24, TS_PX_N = 32, TS_PX_TRI = 40,
    kTreeStatCount = 48
};
constexpr u32 kTreeHistBins = 8;
// First bin edges; bin i covers [base<<i, base<<(i+1)). Mirrored as the `base`
// argument of octave() in tree_cull.comp.glsl.
constexpr float kTreeDistBase = 6.25f;   // metres
constexpr float kTreePxBase   = 1.0f;    // screen pixels (diameter)

// Passes trees are rastered into. One pipeline-statistics query slot each, so the
// question "what does the canopy cost in total" has an answer that is not an estimate.
enum TreePassId : u32
{
    TP_DEPTH = 0,   // camera depth prepass (vk_pass_world)
    TP_COLOR,       // forward colour mesh (CTreeManager::Render)
    TP_VOX,         // forward crown cube tier
    TP_MOTION,      // wind-sway motion vectors (vk_motionvec)
    TP_SHADOW,      // every shadow caster draw of the frame, summed
    kTreePass
};
// Statistic bits requested, IN BIT ORDER — vkGetQueryPoolResults returns one u64 per
// enabled bit, ordered by the bit's value, so this list and the flags below must agree.
enum TreeStatField : u32
{
    TF_IA_VERTS = 0,   // INPUT_ASSEMBLY_VERTICES     — index buffer traffic
    TF_IA_PRIMS,       // INPUT_ASSEMBLY_PRIMITIVES   — triangles submitted
    TF_VS_INV,         // VERTEX_SHADER_INVOCATIONS   — post-cache vertex work
    TF_CLIP_PRIMS,     // CLIPPING_PRIMITIVES         — triangles that reached the raster
    TF_FS_INV,         // FRAGMENT_SHADER_INVOCATIONS — the fill bill, helpers included
    kTreeStatFields
};

// ============================================================================
// Per-instance data uploaded to GPU. The vertex shader reads
// transforms[gl_InstanceIndex] (firstInstance encodes the global tree index
// in the indirect draw command). Layout matches a column-major mat4 plus
// padding so std430 alignment is automatic.
// ============================================================================
struct GpuTreeInstance
{
    Fmatrix xform;          // 64 B  Per-instance world transform
    float   c_scale_hemi;   //  4 B
    float   c_bias_hemi;    //  4 B
    u32     _pad0;          //  4 B
    u32     _pad1;          //  4 B
};
static_assert(sizeof(GpuTreeInstance) == 80, "GpuTreeInstance must be 80 B");

// ============================================================================
// Per-mesh metadata SSBO entry. Cull compute reads sphere_P/R for frustum
// test; on hit it writes a VkDrawIndexedIndirectCommand built from
// index_count + ib_first + first_vertex + global tree index.
// ============================================================================
struct GpuTreeMeta
{
    Fvector  sphere_P;      // 12 B  World-space center
    float    sphere_R;      //  4 B  Radius
    u32      index_count;   //  4 B  num_tris * 3
    u32      ib_first;      //  4 B  First index (element offset into IB)
    u32      first_vertex;  //  4 B  Base vertex (element offset into VB)
    u32      flags;         //  4 B  was _pad. bit 0 = TREE_FLOD_BACKED (see below)
};
// This tree lives inside an FLOD container that CLODManager will draw as a
// billboard past kImposterMinDist — so past that range the full mesh is pure
// duplicate work and may be dropped. Trees WITHOUT this bit have no billboard
// replacement and must never be distance-culled (they would just vanish).
// Mirrored in tree_cull.comp.glsl; the VSM bin shaders read the field as `pad`
// and ignore it, so setting it costs them nothing.
static constexpr u32 TREE_FLOD_BACKED = 1u << 0;

// This tree is a CROWN with a baked voxel cloud (m_VoxLodInfoCPU[i*kHullLods].count > 0)
// that is present on the GPU, so past r_tree_vox_dist the leaf-card mesh may be
// dropped and the cube cloud draws instead. Only foliage (windClass 2) is baked, so
// trunks never carry this bit and always keep their mesh. Set AFTER the crown bake
// (BuildMeshlets) — the flag must describe cubes that actually exist, or the tree is
// cut from the mesh path and nothing replaces it. Mirrored in tree_cull.comp.glsl.
static constexpr u32 TREE_VOX_BACKED = 1u << 1;
// Bits 8..31 of `flags` carry the tree's index into the per-SPECIES voxel LOD table
// (m_VoxLodTable), valid only with TREE_VOX_BACKED set. It rides here instead of a
// per-tree buffer of its own because `flags` had 30 spare bits and a parallel u32
// array over 193 k instances would have been 0.8 MB of VRAM to say what these bits
// already can. Only the low 8 bits are flags — keep new ones inside that byte.
// nfs21 split the high bits: 8..23 = that species row (16 bits, TREE_VOXLOD_MASK), 24..31 = the FLOD group's
// size-LOD k in 1/64 steps (0 = no group) -- for EVERY FLOD-backed tree, VOX-backed or not.
static constexpr u32 TREE_FLAG_BITS     = 8;
static constexpr u32 TREE_VOXLOD_SHIFT  = TREE_FLAG_BITS;
static constexpr u32 TREE_VOXLOD_MASK   = 0xFFFFu;

// One forward cube draw, produced by tree_cull.comp and consumed by tree_vox.vert
// through gl_InstanceIndex (firstInstance = the draw's slot). The per-draw payload
// that used to travel in push constants when this was a CPU loop.
struct GpuVoxDraw
{
    u32   tree;      //  4 B  global tree index → transforms[] (wind + xform)
    float cubeSize;  //  4 B  rendered cube edge, mesh-local metres (continuous in distance)
    float density;   //  4 B  (cellSize/cubeSize)^2 — VS thins instances to keep area constant
    float fade;      //  4 B  screen-door: >0 dissolve OUT, <0 dissolve IN, 0 solid
};
static_assert(sizeof(GpuVoxDraw) == 16, "GpuVoxDraw must be 16 B — mirrored in tree_vox.vert.glsl");

// r_tree_dist, resolved (0 = cut disabled). Both the colour cull and the camera
// depth prepass MUST read the distance through this one accessor — see the note
// on its definition in vk_TreeManager_Render.cpp.
float TreeFlodCutDist();

// r_tree_vox_dist, resolved (0 = the voxel tier is off). Clamped ABOVE by the FLOD
// cut: past that range the billboard already replaces the tree, so cubes there would
// be a third copy of the same crown. Same one-accessor rule as TreeFlodCutDist — the
// colour cull and the camera depth prepass MUST read the range through this.
float TreeVoxCutDist();

// r_tree_alpharef, resolved. `shipped` is the threshold the pass would use on its own
// (200/255 everywhere but the impostor atlas); the cvar overrides it when >= 0.
//
// ⭐⭐⭐ THIS IS A PROBE, NOT A QUALITY KNOB, AND IT ANSWERS ONE EXPENSIVE QUESTION.
// The canopy costs ~half the frame and the ledger says 96% of its shaded fragments are
// thrown away by the alpha test. Every published fix for that (Simplygon tight-fitting
// impostors, Ghost of Tsushima's opaque blades, Nanite's voxels) amounts to the same
// move: shrink the card until it is OPAQUE, trading triangles — of which there are
// 1.7 M out of a 145 M budget here — for fragments.
// Building that bake is weeks. `r_tree_alpharef 0` makes every card opaque IMMEDIATELY,
// with the wrong silhouette but the RIGHT fragment behaviour: overdraw collapses to the
// true depth complexity and early-Z finally works. Whatever the frame does then is the
// CEILING of the entire direction — measured before a line of the bake exists.
// ⚠ The picture is wrong in this mode (leaf cards become solid rectangles). It is for
// reading [VK TreeFill] and Trees/Mesh, never for playing.
// ⚠ The load-time silhouette/impostor BAKES deliberately do NOT read this: they write
// data that outlives the probe frame, and baking a solid rectangle would corrupt it.
float TreeAlphaRef(float shipped);
static_assert(sizeof(GpuTreeMeta) == 32, "GpuTreeMeta must be 32 B");

// ============================================================================
// Meshlet cluster (Phase A — per-page VSM meshlet culling, r_vsm_meshlet).
// One cluster ≈ kMeshletTris triangles of a UNIQUE tree mesh (shared across all
// instances of that species). center/radius are LOCAL (pre-xform) mesh space —
// the bin transforms them per instance. first_index/index_count point into the
// dedicated meshlet index buffer (m_MeshletIndexBuffer), index values still
// relative to the mesh's vBase so vertexOffset = first_vertex works unchanged.
// ============================================================================
struct GpuMeshlet
{
    Fvector center;       // 12 B  local mesh-space sphere center
    float   radius;       //  4 B  local sphere radius
    u32     first_index;  //  4 B  offset into m_MeshletIndexBuffer
    u32     index_count;  //  4 B  tris * 3
    u32     _pad0;        //  4 B
    u32     _pad1;        //  4 B  std430 (vec3+float+4u = 32 B)
};
static_assert(sizeof(GpuMeshlet) == 32, "GpuMeshlet must be 32 B");

// Per-tree meshlet slice into GpuMeshlet[] (instances of one mesh share a slice).
struct GpuTreeMeshletRange
{
    u32 base;    // first meshlet index in m_MeshletBuffer
    u32 count;   // meshlet count
};
static_assert(sizeof(GpuTreeMeshletRange) == 8, "GpuTreeMeshletRange must be 8 B");

// ============================================================================
// Per-tree crown-HULL range (r_vsm_tree_hull) — the AC-Shadows-style shadow-caster
// LOD. Built per UNIQUE mesh in BuildMeshlets (k-means vertex clusters → ellipsoid
// icosphere lobes, local mesh space), instances share the range. ib indices are
// relative to vb_first (vertexOffset of the indirect draw). count 0 = no hull.
// Matches uvec4 in vsm_hull_cmd.comp.
// ============================================================================
struct GpuTreeHullInfo
{
    u32 ib_first;   // first index in m_HullIB
    u32 ib_count;   // index count (0 = hull unavailable → tree keeps its crown mesh)
    u32 vb_first;   // base vertex in m_HullVB (→ indirect vertexOffset)
    u32 _pad;
};
static_assert(sizeof(GpuTreeHullInfo) == 16, "GpuTreeHullInfo must be 16 B");

// ============================================================================
// One crown VOXEL (r_vsm_tree_hull_vox debug viewmode, UE Nanite-foliage style):
// an individual colored cube, NOT a merged shell face. Species-local center +
// baked RGBA8 (rgb = per-voxel foliage tint with AO/height grading; a = size
// jitter seed in the HIGH nibble + leaf coverage 1-15 in the LOW nibble — the
// caster dissolves cubes in coverage order across the crossfade band). Fed to
// the voxel-debug pipeline as a PER-INSTANCE vertex stream (stride 16); the
// cube's 36 corners are generated from gl_VertexIndex.
// ============================================================================
struct GpuTreeVoxel
{
    Fvector p;      // voxel center, species-local mesh space (same space as m_HullVB)
    u32     rgba;   // packed R|G<<8|B<<16|A<<24
};
static_assert(sizeof(GpuTreeVoxel) == 16, "GpuTreeVoxel must be 16 B");

// ============================================================================
// Voxel BRICK — the shadow-caster unit, straight from UE Nanite's voxel bricks
// (RasterizeBricks.usf): 4×4×4 cells of a crown voxel grid packed into one
// 64-bit occupancy mask. The caster draws ONE light-facing quad per brick
// (6 verts) and the FS DDA-raycasts the mask along the sun direction, writing
// conservative depth — per-voxel cost collapses from 36 HW verts to a few
// mask tests per covered pixel. Two masks: `full` = every occupied cell,
// `core` = only cells whose leaf coverage nibble ≥ 6 — the crossfade band
// swaps full→core per brick (staggered by hash) so approaching a tree first
// drops the low-coverage "extras", leaving cubes on the leaf clumps.
// meta: bits 0-3 = average coverage (rank for the band dissolve),
//       bits 4-11 = bake hash (stagger seed). Bit index inside a mask:
//       bit = x | y<<2 | z<<4 (x fastest), matching the FS DDA.
// ============================================================================
struct GpuTreeBrick
{
    Fvector p;        // brick MIN corner, species-local mesh space
    u32     meta;     // avgCov4 | hash8<<4
    u32     fullLo, fullHi;
    u32     coreLo, coreHi;
};
static_assert(sizeof(GpuTreeBrick) == 32, "GpuTreeBrick must be 32 B (std430 mirror)");

// Per-tree, per-LOD slice of the voxel-cloud buffer (CPU-side; debug draws are
// CPU-recorded). `size` is the voxel edge length in mesh-local meters — the
// renderer picks the finest level whose projected size still covers the target
// pixel count (constant screen-size voxels, the Nanite LOD-cut metric).
struct TreeVoxLod
{
    u32   first;    // first voxel (instance) in m_VoxVB
    u32   count;    // voxel count (0 = level unavailable)
    float size;     // voxel edge, mesh-local meters
    u32   _pad;
};
// ⚠ MIRRORED IN tree_cull.comp.glsl (`struct VoxLod`), and the trailing pad is part of
// the contract: std430 gives a 3-scalar struct a stride of 12, so a shader that drops
// the pad reads row 0 correctly and every row after it four bytes further off. That
// shipped once and did not look like a layout bug — it looked like a working tier that
// cost 873 ms. Change the size here and the shader MUST change with it.
static_assert(sizeof(TreeVoxLod) == 16, "TreeVoxLod must be 16 B — mirrored in tree_cull.comp.glsl");

// ============================================================================
// One contiguous (vb, ib, tcOffset, descSetIdx) span in m_TreeMetadataBuffer.
// CullCompute writes its indirect-cmds into m_TreeIndirectBuffer at offset
// `groupIndex * m_MaxGroupMeshCount * sizeof(VkDrawIndexedIndirectCommand)`,
// and DrawTrees reads `vkCmdDrawIndexedIndirectCount` from there. Per-group
// pipeline variant chosen by tcOffset (24 vs 28).
// ============================================================================
struct TreeIndirectGroup
{
    VkBuffer vb;
    VkBuffer ib;
    u32      stride;
    u32      tcOffset;       // 24 or 28 → pipeline variant
    u32      descSetIdx;     // index into m_TexDescSets
    u32      meshOffset;     // start in m_TreeMetadataBuffer
    u32      meshCount;
    // ⭐ ENCLOSING SPHERE OF EVERY TREE IN THE GROUP, and the reason it exists is a
    // measurement. A GPU-culled pass issues one vkCmdDrawIndexedIndirectCount PER GROUP
    // whether or not the cull left anything in that group's section, because the count
    // lives on the device. For the camera prepass that is fine — 1 700 survivors spread
    // over ~50 of 161 groups. For a SHADOW target it is not: ~100 survivors, so most of
    // the 161 indirect draws fetch a zero and cost a command-processor round trip for
    // nothing. Three targets measured +1.6 ms of gpu_total against 0.19 ms of actual
    // cull — the draws, not the compute.
    // This bound turns that back into a 161-test CPU loop (vs the 193 471-instance walk
    // the GPU cull removed) that can skip a whole group before binding anything. It is
    // an ENCLOSING sphere, so rejecting it proves no tree inside could have survived —
    // the skip can never drop a caster the GPU kept.
    Fvector  bsP{};          // centre, world space
    float    bsR = 0.0f;     // radius covering every member tree's own sphere; 0 = untested
};

// ============================================================================
// CTreeManager — Session A only (extraction + upload).
// Sessions B/C add: cull compute pipeline + graphics pipeline + render path.
// ============================================================================
class CTreeManager
{
public:
    CTreeManager();
    ~CTreeManager();

    // Walks RImplementation.Visuals[] for MT_TREE_ST/PM, builds groups,
    // uploads metadata + transforms SSBOs, allocates indirect/draw-count
    // buffers, creates per-texture descriptor sets. No-op on subsequent
    // calls or empty levels.
    void Build();
    void Destroy();

    // Session B per-frame entry point — registered as a scene pass. Runs the
    // frustum-cull compute then issues per-group indirect draws. No-op until
    // pipelines are built and at least one tree group exists.
    void Render(VK::FrameContext& ctx);

    // Shadow caster path: depth-only, alpha-tested draws of the trees whose
    // sphere intersects the sun ortho box, CPU-recorded per tree (no compute
    // cull). cascade < 0: the far static map (ShadowMap::SphereVisible, runs
    // only on sun static redraws). cascade >= 0: the per-frame near cascade
    // with that index (ShadowMap::CascadeSphereVisible). frustum != null
    // overrides both culls: camera depth-PREPASS path — trees join the scene
    // depth so GTAO sees them and the foliage color passes get early-Z
    // (same VP + alpha-ref as the LEQUAL color pass → re-raster matches).
    // Caller owns render begin/end, viewport and bias.
    // minDist/maxDist: cull trees by camera distance to split the shadow into a
    // cached STATIC layer (far: minDist=wind_shadow_dist) and a per-frame DYNAMIC
    // layer (near: maxDist=wind_shadow_dist) so near trees sway in the wind without
    // re-rasterizing the whole forest. Defaults = all trees (no split).
    // flodCut > 0: additionally drop TREE_FLOD_BACKED trees farther than flodCut —
    // the camera-prepass twin of the colour cull's r_tree_dist. ⚠ Pass it ONLY on
    // the prepass path: the colour cull MUST stay a superset of the prepass (see
    // tree_cull.comp.glsl), and shadow casters have no billboard stand-in at all.
    // sortBuckets (r_tree_depth_sort, CAMERA prepass only): >1 draws the set in
    // near→far depth slabs instead of texture-group order, so the prepass gets its own
    // early-Z. It only PERMUTES draws — depth is written with LEQUAL, so the resulting
    // buffer is the same in any order. Shadow call sites leave it 0: "front to back"
    // means nothing measured from a camera the light does not share.
    // gpuShadowTarget >= 0 (a TreeShadowTarget): draw this shadow map from the slot
    // DispatchShadowCull filled earlier in the frame instead of walking m_MetaCPU.
    // Falls back to the walk when the slot holds nothing for this frame, so the
    // caller can pass it unconditionally and let the cvar decide.
    // mvTarget (r_mv_prepass): this is the CAMERA depth prepass and its scope carries
    // the RG16F motion attachment — draw with the tree_motion pair instead of
    // tree_depth, so the crowns write their true wind-sway motion in the same raster
    // that lays their depth. lightVP is then IGNORED for the projection: the motion
    // shaders take the UNJITTERED cur/prev view-proj from MotionVec and re-apply the
    // jitter to gl_Position only (which leaves clip z/w bit-identical, so the forward
    // tree pass's LEQUAL still passes over this depth).
    void RenderDepth(VkCommandBuffer cmd, const Fmatrix& lightVP, s32 cascade = -1,
                     const CFrustum* frustum = nullptr,
                     float minDist = 0.0f, float maxDist = 1e9f,
                     float flodCut = 0.0f, float voxCut = 0.0f, u32 sortBuckets = 0,
                     bool gpuCulled = false, s32 gpuShadowTarget = -1, bool mvTarget = false,
                     const Fvector* bandEye = nullptr, int windPose = 0);
    // nfs47 RenderDepth tail: bandEye = centre of the min/max distance band (nullptr = camera); windPose for shadow
    // casters: 0 = full wind, 1 = REST (no bend, no flutter -- cached layers: a redraw moves nothing), 2 = bend only.

    // Wind-sway motion-vector overlay. Called by VK::MotionVec::ExecuteDynamic
    // INSIDE its already-begun MV render pass (MV target + scene depth bound,
    // negative-height full-depth viewport). Re-draws THIS frame's visible trees
    // (same indirect buffers the forward pass filled), reprojecting the cur+prev
    // wind pose. curVP/prevVP are UNJITTERED (jitter-free MV); jitterNdcX/Y (D3D-NDC,
    // 0 when DLSS off) is re-applied to gl_Position so tree depth still bit-matches the
    // jittered forward draw. No-op until trees are loaded.
    void RenderMotion(const VK::FrameContext& ctx, const Fmatrix& curVP, const Fmatrix& prevVP,
                      float jitterNdcX, float jitterNdcY);

    // VSM caster path. vk_vsm provides the page buffers (its own pageTable/pageList +
    // this frame's clipmap UBO). VsmBin/VsmBinDyn run the per-tree page binning (compute,
    // before the atlas pass); VsmRender/VsmRenderDyn rasterize the trees into the bound
    // atlas pages (call INSIDE vk_vsm's RenderAtlas static/dynamic pass respectively).
    // Near/far wind hybrid: VsmUpdateNearSet refreshes the CPU near set (call BEFORE the
    // residency dispatch, with this frame's world->light sun view — the near metric is the
    // LIGHT-SPACE lateral distance to the tree's shadow column, not trunk distance);
    // VsmPopTransitions hands out boundary-crossing tree spheres for the residency
    // invalidation circles (≤maxOut per call, rest stay queued).
    void VsmUpdateNearSet(const Fmatrix& sunView);
    u32  VsmPopTransitions(Fvector4* out, u32 maxOut);
    // pageList (slot -> level,page) is finalized by the residency/alloc passes BEFORE the
    // bin runs; the meshlet-cull stage 2 (r_vsm_meshlet) needs it, so it's threaded through
    // VsmBin/VsmBinDyn even though the per-tree stage 1 ignores it.
    // pageMax = shadow-HZB per-static-slot occluder max (r_vsm_hzb); occlusion is applied only in the
    // STATIC pass (far trees vs cached walls/terrain). VsmBinDyn binds it for layout parity but never culls.
    void VsmBin(VkCommandBuffer cmd, VkBuffer pageTable, VkBuffer slotDirty, VkBuffer dynUsed, VkBuffer clipmapUBO, VkBuffer pageList, VkBuffer pageMax);
    // staticPageTable (Option A): lets the DYNAMIC near-tree bin look up the STATIC occluder for each
    // world page (virtual page -> static slot -> pageMax) → cull near-tree pages hidden behind walls.
    void VsmBinDyn(VkCommandBuffer cmd, VkBuffer dynPageTable, VkBuffer dynUsed, VkBuffer clipmapUBO, VkBuffer dynPageList, VkBuffer pageMax, VkBuffer staticPageTable);
    // De-serialized bin phasing (see vk_vsm MarkPages): VsmBinClears batches this pass's
    // counter/stat fills into the frame-start fill block (NO barriers of its own);
    // VsmBinMeshlets runs the stage-2 meshlet refinement for both atlases AFTER the
    // caller's single stage-1→stage-2 barrier (stage 2 reads stage-1 casterPages/indirect).
    void VsmBinClears(VkCommandBuffer cmd);
    void VsmBinMeshlets(VkCommandBuffer cmd, bool dyn, VkBuffer pageListStatic, VkBuffer pageListDyn, VkBuffer clipmapUBO,
                        VkBuffer pageMaxBlk = VK_NULL_HANDLE, VkBuffer staticPT = VK_NULL_HANDLE);
    void VsmRender(VkCommandBuffer cmd, VkBuffer pageList, VkBuffer clipmapUBO);
    void VsmRenderDyn(VkCommandBuffer cmd, VkBuffer dynPageList, VkBuffer clipmapUBO);

    // ========================================================================
    //  RAY-TRACED WORLD (vk_rt_world)
    // ========================================================================
    // Trees are the one thing the collision model never had and the static
    // render set does not either: a forest is TENS OF THOUSANDS of instances of
    // a handful of meshes, drawn by this manager alone. Without them a probe ray
    // in a forest sees terrain and sky, which is most of the light in the frame
    // arriving from the wrong place.
    //
    // The RT gather needs the same three things the cull needs — where each tree
    // is, which bytes its mesh occupies, and what it is painted with — so this
    // exposes them rather than duplicating the extraction:
    //   HostMeta()   per-tree world sphere + ib_first/index_count/first_vertex
    //                (index-aligned with the transforms SSBO)
    //   RTGroup()    the (vb, ib, stride, tcOffset, view) span each tree belongs to
    //   Meta/XformBuffer() the SSBOs the gather shader reads
    // ⭐ The tree textures are frozen non-streamable at Build (see the texFloor
    // pass), so a view handed out here never moves under the RT descriptor array.
    const xr_vector<GpuTreeMeta>& RTHostMeta() const { return m_MetaCPU; }
    u32  RTGroupCount() const { return (u32)m_Groups.size(); }
    bool RTGroup(u32 g, VkBuffer& vb, VkBuffer& ib, u32& stride, u32& tcOffset,
                 u32& meshOffset, u32& meshCount, VkImageView& view) const;
    VkBuffer RTMetaBuffer() const;
    VkBuffer RTXformBuffer() const;

    bool IsBuilt() const { return m_bBuilt; }
    u32  VsmNearCount() const { return m_VsmNearCount; }   // this frame's near-set size (diag)
    bool IsReady() const;
    // The forward crown cube tier can actually draw: species LOD table baked and
    // uploaded, both GPU buffers alive, pipeline built. VK::TreeVoxCutDist() asks this
    // so the mesh is never cut for a replacement that cannot be drawn.
    bool VoxTierReady() const;
    u32  GetTotalCount() const { return m_TotalCount; }
    u32  GetGroupCount() const { return (u32)m_Groups.size(); }

    // ----- TREE LEDGER (r_tree_stats) ---------------------------------------
    // Called by the passes that raster trees but do not live in this class, so the
    // canopy's bill is collected in ONE place instead of being estimated from the
    // one zone that happens to be named after it.
    // ⚠ The Begin/End pair must bracket ONLY tree draws — a query around a whole
    // render pass would bill the world's fragments to the canopy. Nesting is refused
    // (one open slot at a time), which is why TP_SHADOW sums its many call sites into
    // a single slot rather than opening one per cascade.
    // GPU cull for the CAMERA depth prepass (r_tree_depth_gpu). Must be recorded
    // BEFORE the prepass render pass begins — it is a compute dispatch, and
    // vkCmdDispatch is illegal inside a render pass. RenderDepth then draws from its
    // output instead of walking the CPU list. Returns false when the path is
    // unavailable, in which case RenderDepth falls back to the CPU walk.
    bool DispatchDepthCull(VkCommandBuffer cmd, const Fmatrix& viewProj);
    bool DepthCullReady() const { return m_DepthCullValid; }

    // GPU cull for SHADOW targets (r_tree_shadow_gpu) — the same shader, the same
    // indirect draw, pointed at the sun/rain maps. One call culls a whole BATCH so the
    // frame pays one clear + one barrier pair for every target instead of per target.
    // ⚠ Like DispatchDepthCull this is a compute dispatch: record it OUTSIDE any
    // render pass, before the passes that will draw from it. Returns the number of
    // targets that actually got a slot (0 = the path is unavailable and every caller
    // will fall back to the CPU walk).
    u32  DispatchShadowCull(VkCommandBuffer cmd, const TreeShadowJob* jobs, u32 n);
    bool ShadowCullReady(u32 target) const
    { return target < TREE_SH_COUNT && (m_ShadowCullValid & (1u << target)) != 0; }

    static bool StatsEnabled();                       // r_tree_stats > 0 (and the level is up)
    void StatsFrameReset(VkCommandBuffer cmd, u32 frameIndex);   // harvest + reset, OUTSIDE a render pass
    void StatsBeginPass(VkCommandBuffer cmd, u32 pass);
    void StatsEndPass(VkCommandBuffer cmd, u32 pass);
    // CPU side: RenderDepth walks every instance on the level, once per shadow target.
    // Nothing else reports that, and it hides inside CPU_RECORD looking like recording.
    void StatsNoteDepthWalk(u32 seen, u32 drawn, u32 binds);
    void StatsRequestMark() { m_StatMark = true; }    // `vk_trees` — print on the next frame
    // `vk_pipestats` — ask the driver what it compiled each crown pipeline into
    // (registers, occupancy, spill). Needs r_pipe_stats set BEFORE they were built.
    void DumpPipelineStats();
    void StatsReport();                               // the [VK Tree*] block

private:
    // Builds a second descriptor set on the SHARED cull layout whose bindings 2/3 point
    // at `indirect`/`count` instead of the colour buffers. Everything else (metadata,
    // the HZB dummy, the cube-tier slots, the ledger) is aliased onto the colour set's
    // resources — the shader must never see an unbound descriptor even in a mode that
    // cannot read it. Used by the prepass and by the shadow arena.
    bool CreateAuxCullSet(VkBuffer indirect, VkBuffer count,
                          VkDescriptorPool& pool, VkDescriptorSet& set, const char* name);
    // Prefix sums of per-group meshCount, computed once. Both aux paths address their
    // command sections with it.
    void EnsureGroupCmdBase();

    // Recursive helper: descends MT_HIERRARHY / MT_LOD into children
    // until it reaches MT_TREE_ST/PM leaves.
    // outFlodBacked collects (with duplicates) every leaf reached through an FLOD
    // container that has a VALID billboard — i.e. the trees CLODManager will draw
    // as imposters past kImposterMinDist. Build() turns that into TREE_FLOD_BACKED.
    // The flag must be carried through the recursion, not inferred from `source`:
    // an FLOD child can itself be an MT_HIERRARHY whose leaves are the trees.
    void ExtractFromVisual(::vkRender_Visual* vis,
                           xr_vector<::vkFTreeVisual*>& outTrees,
                           xr_vector<::vkFTreeVisual*>& outFlodBacked,
                           u32 source = 0,              // 0=top-level 1=hierarchy 2=LOD
                           bool underValidFlod = false);

    void UploadMetadata(const xr_vector<GpuTreeMeta>& meta);
    void UploadTransforms(const xr_vector<GpuTreeInstance>& xforms);

    // Phase A: build per-mesh meshlet clusters for the VSM meshlet-cull path.
    // GPU-reads back each unique tree mesh's positions + indices (pools carry
    // TRANSFER_SRC), Morton-slices them into GpuMeshlet clusters, and uploads the
    // dedicated meshlet IB + GpuMeshlet[] + per-tree range SSBOs. Best-effort:
    // on any failure the meshlet path just stays unavailable (old path unaffected).
    // meta is index-aligned with trees (holds the real drawn ib_first/index_count/
    // first_vertex, incl. the MT_TREE_PM sw[0] window).
    // meta is NON-const: the crown bake stamps TREE_VOX_BACKED into it, and the caller
    // uploads the metadata AFTER this returns so the GPU sees the finished flags.
    void BuildMeshlets(const xr_vector<::vkFTreeVisual*>& trees,
                       xr_vector<GpuTreeMeta>& meta);
    void CreateIndirectBuffers();
    void CreateTextureDescriptors(const xr_vector<VkImageView>& uniqueViews);

    // ----- Session B: pipelines + per-frame -----
    void CreateFrustumUBO();
    void CreateCullPipeline();        // compute cull (tree_cull.comp)
    void CreateXformDescriptor();     // set 0 of the graphics pipeline (transforms SSBO)
    void CreateGfxPipelines();        // tree.vert/frag — tcOffset 24 + 28 variants
    void CreateMotionPipelines();     // tree_motion.vert/frag — lazy MV overlay (24/28)
    void DestroyMotionPipelines();
    void DestroySessionB();

private:
    bool m_bBuilt           = false;
    u32  m_TotalCount       = 0;
    u32  m_FlodBackedCount  = 0;   // trees carrying TREE_FLOD_BACKED (diagnostic)
    u32  m_VoxBackedCount   = 0;   // trees carrying TREE_VOX_BACKED  (diagnostic)

    // ---- Forward CROWN VOXEL TIER (r_tree_vox_dist) -------------------------
    // The cube cloud stopped being a viewmode and became the middle rung of the tree
    // LOD ladder: mesh → cubes → FLOD billboard. Selection runs inside tree_cull.comp
    // (the same dispatch that culls the mesh, so both read one frustum, one HZB and
    // one distance) and appends a VkDrawIndirectCommand per surviving crown; the draw
    // is a single vkCmdDrawIndirectCount. Nothing here is per-frame CPU work — a CPU
    // loop would have had to touch all 193 k instances every frame just to test them.
    static constexpr u32 kVoxDrawCap = 32768;   // crowns x up to 2 (level crossfade); 32 B/slot = 1 MB
    CVulkanBuffer* m_VoxDrawCmds   = nullptr;   // VkDrawIndirectCommand[kVoxDrawCap]
    CVulkanBuffer* m_VoxDrawParams = nullptr;   // GpuVoxDraw[kVoxDrawCap]
    CVulkanBuffer* m_VoxLodTable   = nullptr;   // TreeVoxLod[species × kHullLods] (finest→coarsest)
    CVulkanBuffer* m_VoxCountRB    = nullptr;   // host readback of the draw count (telemetry)
    u32*           m_VoxCountPtr   = nullptr;
    u32            m_VoxLodSpecies = 0;         // rows in m_VoxLodTable (0 = tier unavailable)
    u32            m_VoxFwdDraws   = 0;         // cube draws last frame (readback, diagnostic)
    u32            m_VoxFwdCubes   = 0;         // CUBES those draws carry — draws alone say nothing
    VkImageView m_CullHzbView = VK_NULL_HANDLE;   // last HZB view written to binding 4 (rewrite ONLY on change)
    u32         m_CullHzbGen  = 0xFFFFFFFFu;      // its generation — handles can be recycled, generations cannot
    u32  m_MaxGroupMeshCount = 0;

    xr_vector<TreeIndirectGroup> m_Groups;

    // GPU resources — owned by manager, freed in Destroy().
    CVulkanBuffer* m_TreeMetadataBuffer  = nullptr;
    CVulkanBuffer* m_TreeTransformsBuffer = nullptr;
    CVulkanBuffer* m_SwiInstBuffer = nullptr;   // nfs21/23: per tree uvec4 (group centre bits xyz, window base | count << 24)
    xr_vector<Fvector> m_TreeGroupC;            // nfs23: CPU copy of the group centres (prepass twin)
    CVulkanBuffer* m_TreeLodCBuffer = nullptr;  // per tree vec4: FLOD group centre xyz, w = the group's k (q/64; 0 = no
                                                // billboard group) -- the crown's crossfade (set 0 binding 2, tree.vert)
    CVulkanBuffer* m_SwiWinBuffer  = nullptr;   // nfs21: (first index, index count) per window
    CVulkanBuffer* m_TreeIndirectBuffer   = nullptr;  // size = numGroups * maxGroupMesh * sizeof(VkDrawIndexedIndirectCommand)
    CVulkanBuffer* m_TreeDrawCountBuffer  = nullptr;  // size = numGroups * sizeof(u32)

    // Per-texture descriptor sets (one COMBINED_IMAGE_SAMPLER each).
    // Layout is created in CreateTextureDescriptors() and reused by Session B's
    // graphics pipeline.
    VkDescriptorSetLayout      m_TexDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool           m_TexDescPool   = VK_NULL_HANDLE;
    VkSampler                  m_TexSampler    = VK_NULL_HANDLE;
    VK::CVulkanTexture*        m_WaveTex       = nullptr;          // SSFX wind flow map (s_waves)
    VkSampler                  m_WaveSampler   = VK_NULL_HANDLE;   // linear/repeat for s_waves
    xr_vector<VkDescriptorSet> m_TexDescSets;
    // The views behind those sets, in descSetIdx order — the RT gather needs the
    // IMAGE, not a set built for the tree pipeline's layout.
    xr_vector<VkImageView>     m_TexViews;

    // ----- Session B GPU resources -----
    // View-frustum UBO (host-visible, rewritten each frame in Render).
    CVulkanBuffer* m_FrustumUBO = nullptr;

    // Cull compute: descriptor set {meta, frustum, indirect, count}.
    VkDescriptorSetLayout m_CullDescLayout    = VK_NULL_HANDLE;
    VkDescriptorPool      m_CullDescPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_CullDescSet       = VK_NULL_HANDLE;
    VkPipelineLayout      m_CullPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_CullPipeline       = VK_NULL_HANDLE;

    // Graphics set 0 = transforms SSBO (set 1 = m_TexDescSets, reused).
    VkDescriptorSetLayout m_XformDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_XformDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_XformDescSet    = VK_NULL_HANDLE;

    // Graphics pipeline — two variants by UV byte offset (24 / 28).
    VkPipelineLayout m_GfxPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_GfxPipeline24     = VK_NULL_HANDLE;
    // r_tree_zwrite 0 twins — depth test, NO depth write, so the leaf `discard` stops
    // disabling early-Z. Built alongside the normal pair so the A/B is live.
    VkPipeline       m_GfxPipeline24NoZW = VK_NULL_HANDLE;
    VkPipeline       m_GfxPipeline28NoZW = VK_NULL_HANDLE;
    VkPipeline       m_GfxPipeline28     = VK_NULL_HANDLE;
    // r_tree_flat probe twins — same state as the default pair, FS is tree_flat.frag
    // (alpha test + constant colour). Built alongside so the A/B is live; the probe
    // decides whether Trees/Mesh is bound by shading work or by the raster floor.
    VkPipeline       m_GfxPipeline24Flat = VK_NULL_HANDLE;
    VkPipeline       m_GfxPipeline28Flat = VK_NULL_HANDLE;
    // nfs31 R1 crown (Switch): tree_r1.vert/frag -- light + fog per vertex, FS = fetch + alpha + mad + fog. Bound only
    // while the world state asks for nothing beyond sun + sky + fog and the sun maps are empty (see Render).
    VkPipeline       m_GfxPipeline24R1 = VK_NULL_HANDLE;
    VkPipeline       m_GfxPipeline28R1 = VK_NULL_HANDLE;
    // (The r_tree_flat 2 twins — flat plus one extra diffuse fetch — are gone. They
    // measured one crown texture fetch across the whole pass at 0.00-0.01 ms, which
    // closed the "cheaper fetch" family for good. Two pipelines per level load is
    // not what a finished measurement is worth.)

    // r_tree_gather 0 twins — the crown FS with the screen-space gather tap
    // compiled OUT of ddgiAmbientFromField (probe field only). A rendering option,
    // not a probe: it changes the canopy's GI, and it is the only shape that can
    // return the 0.5 ms, because that tap costs by being present rather than by
    // executing. Built at load next to the default pair; zwrite-on state, so
    // r_tree_zwrite is ignored while this is on.
    VkPipeline       m_GfxPipeline24NoGather = VK_NULL_HANDLE;
    VkPipeline       m_GfxPipeline28NoGather = VK_NULL_HANDLE;

    // ----- r_tree_gi_inst: the probe field sampled ONCE PER TREE -----------------
    // Built because vk_pipestats put the crown's GI cost in the VERTEX shader, not
    // the fragment one: with the FS no longer consuming vGI.rgb the driver deleted
    // ddgiFieldWeighted from tree.vert and its register count went 56 -> 32 (binary
    // 20224 -> 5376 B). ddgiSampleIrradiance is ~16 atlas taps and a canopy draws
    // ~10^6 vertices; the field is metres-scale, a crown is metres. So: one compute
    // invocation per tree (two samples, crown bottom and top), the vertex shader
    // lerps between them by height and keeps the gradient it had.
    CVulkanBuffer*        m_TreeGIBuffer   = nullptr;   // vec4[3 * m_TotalCount]
    VkDescriptorSetLayout m_GIDescLayout   = VK_NULL_HANDLE;   // set 0 = {meta, giOut}
    VkDescriptorPool      m_GIDescPool     = VK_NULL_HANDLE;
    VkDescriptorSet       m_GIDescSet      = VK_NULL_HANDLE;
    VkPipelineLayout      m_GIPipelineLayout = VK_NULL_HANDLE; // set 0 = ours, set 1 = EnvLight
    VkPipeline            m_GIPipeline       = VK_NULL_HANDLE;
    // The crown pair whose VS reads that buffer instead of sampling the field.
    VkPipeline            m_GfxPipeline24GIInst = VK_NULL_HANDLE;
    VkPipeline            m_GfxPipeline28GIInst = VK_NULL_HANDLE;
    void CreateGIPass();          // buffer + set + compute pipeline (called from Build's Session B)
    void DispatchTreeGI(VkCommandBuffer cmd);   // per frame, before the crown draws

    // ===== CROWN VARIANTS: one pipeline per (world state x VS/FS twin) ===========
    // The crown ubershader is split by SPECIALIZATION, not by copying the file —
    // tree_variants.glsl carries the whole argument, and TreeSpec below is the C++
    // half of that bit layout. ⚠ The two MUST stay in step; the live value comes
    // from VK::EnvLight::TreeSpecMask(), which reads the same UBO fields the
    // shader's branches test.
    enum TreeSpec : u32 {
        TS_SNOW  = 1u << 0,   // sf_params.w > 0
        TS_SUN   = 1u << 1,   // sun colour non-black (night compiles the shadow tap out)
        TS_WET   = 1u << 2,   // rain wetness OR live shore water OR the wet debug view
        TS_SSIL  = 1u << 3,   // r_ssil and a real IL view bound
        TS_SKY   = 1u << 4,   // always set — SH9 sky ambient measured at ~0
        TS_GI    = 1u << 5,   // r_ddgi_light: the expensive term (0.45 ms of 0.78 on a 5070)
        TS_AO    = 1u << 6,   // ao_params.z > 0
        TS_SHEEN = 1u << 7,   // r_ibl
        TS_DYN   = 1u << 8,   // dynamic light count > 0
        TS_FOG   = 1u << 9,   // always set — one mad
        TS_PTDBG = 1u << 10,  // r_point_debug overlay
        TS_DEBUG = 1u << 11,  // any other crown debug view
        TS_ALL   = 0xFFFu,    // every term present == the ubershader before the split
    };
    // One pipeline PAIR per (mask, VS, FS). Built the first frame that combination
    // is asked for and kept until the level ends: destroying a pipeline a submitted
    // frame may still reference, to save a few MB, is not a trade this gets to make.
    // The VS/FS handles are part of the key because r_tree_gi_inst and
    // r_tree_gather pick different modules — a cache that ignored them would hand
    // back a pipeline built from the shader the cvar just switched away from, and
    // the switch would read as "the change did nothing".
    // The four crown shader modules, resolved ONCE at pipeline creation. The variant
    // selector picks between them every frame, and asking the shader manager by name
    // per frame would be a string lookup for an answer that cannot change until the
    // level does.
    VkShaderModule       m_CrownVS       = VK_NULL_HANDLE;   // tree.vert.spv
    VkShaderModule       m_CrownVSGI     = VK_NULL_HANDLE;   // tree_giinst.vert.spv (r_tree_gi_inst 1)
    VkShaderModule       m_CrownFS       = VK_NULL_HANDLE;   // tree.frag.spv
    VkShaderModule       m_CrownFSNoGath = VK_NULL_HANDLE;   // tree_nogather.frag.spv (r_tree_gather 0)

    struct PermPipes { u32 mask; VkShaderModule vs, fs; VkPipeline p24, p28; };
    xr_vector<PermPipes> m_PermPipes;
    // Masks wanted but not built yet — drained ONE pair per frame so a weather
    // change costs a frame of latency instead of a visible hitch (the world pass
    // solves the same problem the same way: see s_prewarmTerrainMask).
    xr_vector<u32>       m_PermPending;
    u32                  m_PermLastWant = 0xFFFFFFFFu;   // last mask asked for
    // ⚠ HOW MANY FRAMES THAT MASK HAS HELD STILL, and it is not bookkeeping — it is
    // the fix for a real misfire. While a level settles, AO strength ramps, the probe
    // volume goes live and lights arrive, so the mask walks through four or five
    // states in the first second. Building on every step (and dragging four prewarm
    // flips along with each) put FIFTEEN variants in the cache in 1.5 s — half of the
    // 32-entry ceiling spent on states the level was only passing through, and past
    // that ceiling the crown silently falls back to the full shader. Waiting costs
    // nothing: an unbuilt mask draws the full crown, which is merely the shader that
    // shipped before the split.
    u32                  m_PermStable    = 0;
    bool                 m_PermPrewarmed = false;         // flips for THIS mask already queued
    // Returns the pair for `mask` if it exists, else null — and queues it. Call
    // BEFORE recording the colour pass; the caller falls back to the full-mask
    // pipeline for the frames the build has not landed yet.
    const PermPipes*     FindPermPipes(u32 mask, VkShaderModule vs, VkShaderModule fs) const;
    bool                 BuildPermPipes(u32 mask, VkShaderModule vs, VkShaderModule fs);
    // Names what a mask leaves OUT, for the one log line that says which crown is
    // actually bound. A silent fallback reads in a sweep as "the change did
    // nothing", which is the one answer an A/B must never be allowed to fake.
    static void          DescribePermMask(u32 mask, char* out, size_t cap);

    // Shadow caster: depth-only alpha-tested variants (tree_depth.{vert,frag},
    // same layout) + a CPU copy of the per-mesh metadata for CPU-side culling.
    VkPipeline       m_DepthPipeline24   = VK_NULL_HANDLE;
    VkPipeline       m_DepthPipeline28   = VK_NULL_HANDLE;

    // Wind-sway MV overlay: reprojects the cur+prev wind pose into the MV target.
    // Own layout (set0 = xform+waves, set1 = diffuse; NO env set) + push; lazy-built.
    // LEQUAL no-write, no bias → depth bit-matches the forward tree draw.
    VkPipelineLayout m_MotionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_MotionPipeline24     = VK_NULL_HANDLE;
    VkPipeline       m_MotionPipeline28     = VK_NULL_HANDLE;
    // r_mv_prepass twins of the above: the SAME tree_motion.{vert,frag} pair, but
    // depth WRITE on and the prepass's motion attachment — so the canopy lays its
    // depth and states its motion in ONE raster. That deletes the overlay above,
    // which was the canopy's THIRD full raster of the frame (1.35 ms measured).
    VkPipeline       m_MvDepthPipeline24    = VK_NULL_HANDLE;
    VkPipeline       m_MvDepthPipeline28    = VK_NULL_HANDLE;
    // Wind history for the MV pass (cur = this frame, prev = last frame). Rolled ONCE
    // per Device.dwFrame by RollMvWind — whichever pass gets there first. Under
    // r_mv_prepass that is the depth prepass, which runs long before Render(); a
    // second roll in the same frame would set prev := cur and flatten the sway to zero.
    Fvector4 m_MvWindParams{}, m_MvWsetup{}, m_MvWindAnim{};
    Fvector4 m_MvWindParamsPrev{}, m_MvWsetupPrev{}, m_MvWindAnimPrev{};
    bool     m_MvWindValid = false;
    u32      m_MvWindFrame = u32(-1);
    void     RollMvWind(const Fvector4& windParams, const Fvector4& wsetup, const Fvector4& windAnim);
    xr_vector<GpuTreeMeta> m_MetaCPU;

    // ----- Phase A meshlet clusters (r_vsm_meshlet) — built in Build(), consumed by
    // the two-stage VSM bin (Phase B). Device-local, owned; freed in Destroy().
    bool           m_MeshletsReady    = false;
    u32            m_MeshletTotal     = 0;   // GpuMeshlet[] entries
    u32            m_MeshletIndexTotal = 0;  // u16 indices in the dedicated meshlet IB
    CVulkanBuffer* m_MeshletBuffer      = nullptr;   // GpuMeshlet[]        (m_MeshletTotal)
    CVulkanBuffer* m_MeshletIndexBuffer = nullptr;   // u16 dedicated IB    (m_MeshletIndexTotal)
    CVulkanBuffer* m_TreeMeshletRangeBuffer = nullptr;   // GpuTreeMeshletRange[] (m_TotalCount)

    // ----- VSM caster path (lazy; built on first VsmBin once Session B is up) -----
    // Near/far WIND HYBRID (r_vsm_tree_wind): NEAR trees (< r_vsm_tree_wind_dist, CPU set
    // with hysteresis) cast into the DYNAMIC atlas every frame WITH wind → smooth coherent
    // sway; FAR trees stay in the toroidal STATIC cache, rigid. Boundary crossings emit
    // invalidation spheres (VsmPopTransitions) that the residency pass turns into dirty
    // static pages — the rigid shadow is added/removed the same frame (no ghosts).
    void CreateVsmResources();
    void DestroyVsm();
    // Phase B (r_vsm_meshlet): build the stage-2 meshlet-bin pipeline + meshlet page
    // pipelines + command/group buffers. Called from CreateVsmResources once meshlets exist.
    void CreateMeshletVsmResources();
    // Stage 2 dispatch (mode 0 = static / 1 = dynamic). pageList = slot->page for that atlas.
    void DispatchMeshletBin(VkCommandBuffer cmd, u32 mode, VkBuffer pageList, VkBuffer clipmapUBO);
    // Meshlet render for one atlas (mode 0 = static / 1 = dynamic). Reuses set2 (m_Vsm[Dyn]PageSet).
    void DrawMeshlets(VkCommandBuffer cmd, u32 mode, VkDescriptorSet pageSet, bool wind);
    bool IsMeshletMode() const;   // r_vsm_meshlet on + resources ready

    bool                  m_VsmReady      = false;
    VkPipeline            m_VsmBinPipe    = VK_NULL_HANDLE;   // vsm_tree_bin.comp (mode 0 = static+dirty filter, mode 1 = dynamic+dynUsed)
    VkPipelineLayout      m_VsmBinLayout  = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_VsmBinSetL    = VK_NULL_HANDLE;
    VkDescriptorPool      m_VsmDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_VsmBinSet[VK_FRAMES_IN_FLIGHT]    = {};   // static pass
    VkDescriptorSet       m_VsmDynBinSet[VK_FRAMES_IN_FLIGHT] = {};   // dynamic pass
    VkDescriptorSetLayout m_VsmPageSetL   = VK_NULL_HANDLE;   // set 2: pageList + casterPages + clipmap UBO
    VkDescriptorSet       m_VsmPageSet[VK_FRAMES_IN_FLIGHT]    = {};
    VkDescriptorSet       m_VsmDynPageSet[VK_FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      m_VsmPageLayout = VK_NULL_HANDLE;
    VkPipeline            m_VsmPagePipe24 = VK_NULL_HANDLE;      // STATIC atlas variant (rigid)
    VkPipeline            m_VsmPagePipe28 = VK_NULL_HANDLE;
    VkPipeline            m_VsmPageDynPipe24 = VK_NULL_HANDLE;   // DYNAMIC atlas variant (wind)
    VkPipeline            m_VsmPageDynPipe28 = VK_NULL_HANDLE;
    CVulkanBuffer* m_VsmCasterPages = nullptr;   // shared ARENA, kVsmTreeArena u32 of (treeIdx<<13)|slot (was trees×cap slicing = 396MB on 193k-tree maps)
    CVulkanBuffer* m_VsmIndirect    = nullptr;   // m_TotalCount cmds — STATIC pass (near trees carry instanceCount 0)
    CVulkanBuffer* m_VsmDynIndirect = nullptr;   // m_TotalCount cmds — DYNAMIC pass (far trees carry instanceCount 0)

    // ----- Phase B meshlet-cull path (r_vsm_meshlet). Stage 2 refines the per-tree page
    // list into per-(meshlet,page) draws pooled into each tree's GROUP command section.
    bool                  m_MeshletVsmReady = false;
    VkPipeline            m_MeshletBinPipe   = VK_NULL_HANDLE;   // vsm_tree_meshlet_bin.comp
    VkPipelineLayout      m_MeshletBinLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_MeshletBinSetL   = VK_NULL_HANDLE;
    VkDescriptorPool      m_MeshletDescPool  = VK_NULL_HANDLE;
    VkDescriptorSet       m_MeshletBinSet[VK_FRAMES_IN_FLIGHT]    = {};   // static stage 2
    VkDescriptorSet       m_MeshletDynBinSet[VK_FRAMES_IN_FLIGHT] = {};   // dynamic stage 2
    VkPipeline            m_MeshletPagePipe24    = VK_NULL_HANDLE;   // STATIC atlas meshlet VS
    VkPipeline            m_MeshletPagePipe28    = VK_NULL_HANDLE;
    VkPipeline            m_MeshletPageDynPipe24 = VK_NULL_HANDLE;   // DYNAMIC atlas meshlet VS
    VkPipeline            m_MeshletPageDynPipe28 = VK_NULL_HANDLE;
    CVulkanBuffer* m_VsmMeshletCmd        = nullptr;   // m_TotalCount*kCmdPerTree cmds (STATIC)
    CVulkanBuffer* m_VsmMeshletCmdDyn     = nullptr;   // m_TotalCount*kCmdPerTree cmds (DYNAMIC)
    CVulkanBuffer* m_VsmMeshletGroupCount = nullptr;   // per-group append counter / draw count (STATIC)
    CVulkanBuffer* m_VsmMeshletGroupCountDyn = nullptr;// (DYNAMIC)
    CVulkanBuffer* m_TreeGroupBuffer      = nullptr;   // u32/tree -> group index
    CVulkanBuffer* m_GroupInfoBuffer      = nullptr;   // uvec2/group -> (cmdBase, cmdCap)
    xr_vector<u32> m_MeshletGroupBase;                 // CPU copy for the render (cmd offset per group)
    xr_vector<u32> m_MeshletGroupCap;                  // CPU copy for the render (maxDrawCount per group)
    CVulkanBuffer* m_MeshletStats         = nullptr;   // [0]=commands [1]=overflow (both passes accumulate)
    CVulkanBuffer* m_MeshletStatsRB       = nullptr;
    u32*           m_MeshletStatsPtr      = nullptr;

    CVulkanBuffer* m_VsmStats       = nullptr;
    CVulkanBuffer* m_VsmStatsRB     = nullptr;   // host readback (r_vsm_debug diagnostics)
    u32*           m_VsmStatsPtr    = nullptr;
    // Forward drawn-count readback (r_profiler): the main-view cull writes the
    // per-group instance counts GPU-side only — this mirrors them to the host
    // (a frame or two stale, no fence — diagnostic) so "how many trees do we
    // actually draw" exists as a number. Lazy-created on first profiled frame.
    CVulkanBuffer* m_FwdCountRB     = nullptr;
    u32*           m_FwdCountPtr    = nullptr;
    u32            m_FwdLastLog     = 0;
    // ----- Cull ledger (r_tree_stats) — see TreeCullStat / tree_cull.comp.glsl.
    // Device-side u32[kTreeStatCount], zeroed each frame by the same fill that clears
    // the draw counts and mirrored to the host right after the dispatch. Read a frame
    // or two stale on purpose: no fence, no stall, and the numbers are steady-state.
    CVulkanBuffer* m_CullStats      = nullptr;
    CVulkanBuffer* m_CullStatsRB    = nullptr;
    u32*           m_CullStatsPtr   = nullptr;
    // ----- Per-pass HARDWARE counters (r_tree_stats): pipeline statistics around each
    // pass trees appear in, plus one precise occlusion query around the colour pass.
    // ⭐ THE PAIR IS THE POINT. FS invocations count every fragment shader launch —
    // helper lanes, `discard`ed leaf pixels, everything. Samples-passed counts only
    // fragments that survived the depth test AND were not discarded. Their ratio is
    // therefore the alpha-test waste of the canopy, measured rather than assumed, and
    // it is the number that decides whether cheaper leaf cards would pay. Nothing here
    // needs an atomic in tree.frag: the hardware already keeps both tallies.
    VkQueryPool    m_StatPool       = VK_NULL_HANDLE;   // PIPELINE_STATISTICS, kTreePass slots per frame
    VkQueryPool    m_OccPool        = VK_NULL_HANDLE;   // OCCLUSION (precise), 1 slot per frame
    bool           m_StatUsed[VK_FRAMES_IN_FLIGHT] = {};
    int            m_StatOpen       = -1;               // pass index currently inside vkCmdBeginQuery
    bool           m_OccOpen        = false;
    u64            m_StatVal[kTreePass][kTreeStatFields] = {};   // last harvested, per pass
    u64            m_OccSamples     = 0;
    // CPU side of the ledger: RenderDepth is a CPU walk over every instance on the
    // level, and it runs once per shadow target per frame. Counting the walks and the
    // instances they touch is the only way that cost shows up at all — it lives in
    // CPU_RECORD, where it looks like "recording".
    u32            m_CpuDepthWalks  = 0;    // RenderDepth calls this frame
    u32            m_CpuDepthSeen   = 0;    // instances examined by them
    u32            m_CpuDepthDrawn  = 0;    // draws they issued
    u32            m_CpuDepthBinds  = 0;    // state changes — the price front-to-back sorting pays
    // Sort scratch for the front-to-back prepass order (r_tree_depth_sort). A member
    // so the per-frame path never allocates; key = slab | group | global tree index.
    xr_vector<u64> m_DepthSortKeys;

    // ----- GPU-CULLED CAMERA DEPTH PREPASS (r_tree_depth_gpu) -----------------
    // The prepass used to be a CPU walk over every instance on the level, once per
    // shadow target, ending in one vkCmdDrawIndexed per surviving tree: 193 471
    // instances scanned to issue 1 706 draws (99.1% of the walk a rejection), and the
    // resulting zone measured DRAW-CALL bound. This is the colour path's machinery
    // pointed at the prepass — same tree_cull.comp, same indirect draw — collapsing
    // both the walk and the 1 706 draws into one dispatch and one draw per group.
    //
    // ⚠ SEPARATE BUFFERS, not the colour ones: both sets exist in the same frame and
    // hold DIFFERENT trees (the prepass cuts by centre distance, colour by the near
    // side of the sphere), and the prepass records earlier in the frame than the
    // colour cull runs.
    // ⭐ Sized by the PREFIX SUM of group mesh counts (= m_TotalCount commands, ~3.9 MB)
    // rather than groups × maxGroupMesh the way the colour buffer is — that shape costs
    // 98 MB for the same content. Fixing the colour buffer the same way is a separate,
    // obvious win noted for later.
    CVulkanBuffer* m_DepthIndirect   = nullptr;   // VkDrawIndexedIndirectCommand[m_TotalCount]
    CVulkanBuffer* m_DepthCount      = nullptr;   // u32 per group
    VkDescriptorSet m_DepthCullSet   = VK_NULL_HANDLE;   // same layout as the colour cull set
    VkDescriptorPool m_DepthCullPool = VK_NULL_HANDLE;
    xr_vector<u32> m_GroupCmdBase;                // prefix sum of meshCount, per group
    bool           m_DepthCullValid  = false;     // a dispatch ran for THIS frame

    // ----- GPU-CULLED SHADOW TARGETS (r_tree_shadow_gpu) ---------------------
    // The prepass was one CPU walk of the level per frame; the SHADOW maps are five
    // more on every frame that misses a shadow cache — 967 k to 1.2 M instances tested
    // to issue ~10 000 draws, all inside CPU_RECORD where it reads as "recording".
    // Same machinery as the prepass, one slot per target.
    //
    // ⭐ ONE ARENA, NOT SIX BUFFERS. Slot t owns commands [t*m_TotalCount, +m_TotalCount)
    // and counts [t*groups, +groups), so the whole thing is a single allocation and a
    // single descriptor set; the draw side just adds a base offset. Sized by the prefix
    // sum like the prepass (~3.9 MB a slot at 193 k trees), NOT groups x maxGroupMesh.
    // ⚠ Allocated LAZILY on the first shadow dispatch: a level whose shadow path never
    // runs (VSM owning the sun, night) must not pay ~23 MB for slots nothing fills.
    CVulkanBuffer*  m_ShadowIndirect  = nullptr;  // [TREE_SH_COUNT][m_TotalCount] cmds
    CVulkanBuffer*  m_ShadowCount     = nullptr;  // [TREE_SH_COUNT][groups] u32
    VkDescriptorSet m_ShadowCullSet   = VK_NULL_HANDLE;   // same layout as the colour set
    VkDescriptorPool m_ShadowCullPool = VK_NULL_HANDLE;
    // Bit t = slot t was filled THIS frame. A bitmask rather than a bool because the
    // targets are independent: a frame can refresh the rain map and one cascade layer
    // and leave the other four slots holding last frame's commands. Cleared in
    // StatsFrameReset for exactly the reason m_DepthCullValid is.
    u32            m_ShadowCullValid = 0;
    bool           m_ShadowCullFailed = false;    // allocation refused — do not retry every frame
    u32            m_StatLastLog    = 0;
    bool           m_StatMark       = false;   // `vk_trees` console command → print next frame
    // (The visible-tree bitset that used to live here is gone. tree.frag stopped
    // marking it when the atomic was found to cost the canopy its early-Z — 20.1 M
    // fragment invocations vs 6.2 M — and after that the buffers, the per-frame
    // readback copy and the popcount stayed behind, computing a number that was
    // always zero. See the block where the atomicOr was, in tree.frag.glsl.)
    // VSM bin diag bitsets (binding 12 of the bin layout): [0..W) distinct trees
    // binned to the DYN atlas per frame, [W..2W) trees ever binned to the cached
    // STATIC atlas (accumulated).
    CVulkanBuffer* m_VsmTreeBits    = nullptr;
    CVulkanBuffer* m_VsmTreeBitsRB  = nullptr;
    u32*           m_VsmTreeBitsPtr = nullptr;
    CVulkanBuffer* m_VsmNearFlags[VK_FRAMES_IN_FLIGHT] = {};   // host-visible u32/tree: bit0 = near (dynamic wind), bit1 = hull tier (r_vsm_tree_hull)
    void*          m_VsmNearPtr[VK_FRAMES_IN_FLIGHT]   = {};   // legacy (device-local flags are not mapped anymore)
    xr_vector<u8>       m_VsmNearCPU;        // current near set (CPU truth, hysteresis)
    xr_vector<u32>      m_VsmNearU32;        // widened upload scratch (shader reads u32 per tree)
    xr_vector<Fvector4> m_VsmPendingInval;   // world spheres of boundary-crossing trees (→ residency)
    u32            m_VsmNearCount   = 0;     // CPU near-set size this frame (diagnostic)
    u32            m_VsmHullCount   = 0;     // hull-tier subset of the near set (diagnostic)
    u32            m_VsmHullFarCount = 0;    // far/static hull tier size (r_vsm_tree_hull 2, diagnostic)
    u32            m_VsmLastLog     = 0;
    u32            m_VsmSlot        = 0;          // frame-in-flight ring for the descriptor sets

    // ----- Impostor crown shadows (r_vsm_tree_impostor) — near-tree DYNAMIC pass only.
    // Bakes a per-UNIQUE-MESH silhouette atlas at load (8 azimuth facets/row, alpha-tested
    // from the crown diffuse), then replaces the near crown MESH raster with ONE sun-facing
    // billboard per tree (2 tris) routed through the same VSM page mechanism. Kills the
    // geometry/raster cost of DynTrees under wind. See tree_vsm_impostor.* / BuildImpostorAtlas.
    void BuildImpostorAtlas(const xr_vector<::vkFTreeVisual*>& trees,
                            const xr_vector<GpuTreeMeta>& meta,
                            const xr_vector<u32>& treeTexIdx);
    void CreateImpostorResources();   // billboard pipeline + sets + indirect (from CreateVsmResources)
    void DestroyImpostor();
    bool IsImpostorMode() const;      // r_vsm_tree_impostor on + atlas + resources ready
    // Fills m_VsmImpostorDynIndirect from m_VsmDynIndirect (indexCount→6, non-indexed);
    // records the copy compute + a compute→compute barrier. Call at the end of VsmBinDyn.
    void DispatchImpostorCmd(VkCommandBuffer cmd);

    // ----- Crown-HULL shadow casters (r_vsm_tree_hull) — near-tree DYNAMIC pass tiering.
    // The AC-Shadows-style caster LOD: near trees WITHIN r_vsm_tree_hull_dist keep the full
    // alpha-tested crown mesh (dappling stays perfect where the player looks); near trees
    // BEYOND it cast from a baked low-poly OPAQUE hull (ellipsoid lobes over k-means vertex
    // clusters, per unique mesh) — no texture fetch / no discard / early-Z, attacking the
    // proven alpha-test-fill bound of VSM/DynTrees. Hull geometry is built unconditionally
    // in BuildMeshlets (shares the mesh readback, ~KBs) so the cvar toggles LIVE; the swap
    // happens per frame in vsm_hull_cmd.comp (crown cmd zeroed → plain AND meshlet paths
    // skip the tree automatically). Far/static cache is untouched.
    void CreateHullResources();       // cmd compute + opaque hull pipeline (from CreateVsmResources)
    void DestroyHull();
    bool IsHullMode() const;          // cvar on + geometry + pipelines ready (excludes impostor mode)
    // Swaps crown→hull indirect cmds for hull-tier trees in BOTH bins (zeroes the crown
    // cmds; static tier only at r_vsm_tree_hull 2). One compute→compute barrier covers
    // the stage-1 writes. Call at the end of VsmBinDyn (both bins recorded by then).
    void DispatchHullCmd(VkCommandBuffer cmd);
    // ONE multi-draw of the tier's hull trees (staticPass: rigid wind + static atlas).
    void DrawHulls(VkCommandBuffer cmd, u32 slot, bool staticPass);
    void DrawVoxCasters(VkCommandBuffer cmd, u32 slot, bool staticPass);
    // Stage-2 per-page BRICK cull (r_vsm_tree_hull_vox_cull, vsm_vox_cull.comp) — the
    // UE-parity piece: only bricks overlapping a page rasterize into it, instead of
    // the whole slice sweeping the VS for every page of the tree's shadow column.
    // Compacts (tree,page) pairs into one draw each + a surviving-brick remap list;
    // DrawVoxCasters then uses vkCmdDrawIndirectCount over the compacted stream.
    // Called from VsmBinMeshlets (after the caller's stage-1→stage-2 barrier, which
    // orders vsm_hull_cmd's writes before this pass's reads).
    // pageMaxBlk/staticPT (shadow-HZB, r_vsm_hzb): per-STATIC-slot 8×8 block occluder
    // maxes + the static page table — bricks fully behind the cached canopy/terrain are
    // culled per (brick,page). NULL → occlusion test off (XY-only compaction as before).
    void DispatchVoxCull(VkCommandBuffer cmd, VkBuffer pageListDyn, VkBuffer pageListStatic, VkBuffer clipmapUBO,
                         VkBuffer pageMaxBlk, VkBuffer staticPT);

    bool           m_HullDataReady   = false;     // geometry baked + uploaded (Build time)
    bool           m_HullReady       = false;     // pipelines/sets built (lazy, CreateVsmResources)
    u32            m_HullLobeTotal   = 0;         // diag: ellipsoid lobes baked
    CVulkanBuffer* m_HullVB          = nullptr;   // tightly packed vec3 positions (stride 12)
    CVulkanBuffer* m_HullIB          = nullptr;   // u16, per-species ranges (relative to vb_first)
    CVulkanBuffer* m_HullInfoBuffer  = nullptr;   // GpuTreeHullInfo[m_TotalCount]
    CVulkanBuffer* m_VsmHullIndirect  = nullptr;  // VkDrawIndexedIndirectCommand/tree (DYN hull tier)
    CVulkanBuffer* m_VsmHullIndirectS = nullptr;  // VkDrawIndexedIndirectCommand/tree (STATIC far tier, mode 2)
    VkDescriptorSetLayout m_HullCmdSetL   = VK_NULL_HANDLE;
    VkDescriptorPool      m_HullCmdPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_HullCmdSet[VK_FRAMES_IN_FLIGHT] = {};   // ring (nearFlags is per-frame)
    VkPipelineLayout      m_HullCmdLayout = VK_NULL_HANDLE;
    VkPipeline            m_HullCmdPipe   = VK_NULL_HANDLE;
    VkPipeline            m_VsmHullPipe   = VK_NULL_HANDLE;   // opaque depth-only hull raster, DYN atlas (reuses m_VsmPageLayout)
    VkPipeline            m_VsmHullPipeS  = VK_NULL_HANDLE;   // STATIC atlas variant (tree_vsm_hull_s.vert, rigid)
    // Voxel-cloud SHADOW caster (r_vsm_tree_hull_vox > 0 supersedes the merged shell):
    // the shadow casts from the SAME cubes the viewmode shows — camera-distance LOD +
    // continuous density from the per-tree CHOICE ring (CPU-computed in VsmUpdateNearSet),
    // and a dithered CROSSFADE band below r_vsm_tree_hull_dist where the real crown
    // still casts while the cubes dissolve in (approach = shadow morphs to true leaves).
    CVulkanBuffer*        m_VsmVoxIndirect  = nullptr;        // VkDrawIndirectCommand/tree (DYN)
    CVulkanBuffer*        m_VsmVoxIndirectS = nullptr;        // VkDrawIndirectCommand/tree (STATIC, mode 2)
    CVulkanBuffer*        m_VoxChoiceBuf[VK_FRAMES_IN_FLIGHT] = {};   // host-visible ring: uvec4/tree
    void*                 m_VoxChoicePtr[VK_FRAMES_IN_FLIGHT] = {};
    xr_vector<u32>        m_VoxChoiceCPU;                     // per tree ×4: brickFirst, brickCount, cellBits, fade16<<16|amp16 (amp = band wind amplitude, 0 = rigid)
    // STATIC-tier voxel LOD ratchet: cube size at the tree's last page invalidation.
    // Cached static pages never re-render on a choice change alone, so a far tree's
    // shadow keeps the cube size from whenever its pages last drew; when the current
    // cube leaves ±1.5× of this anchor, VsmUpdateNearSet queues an invalidation
    // circle and re-anchors (0 = not tracked / dyn tier).
    xr_vector<float>      m_VoxLodSizeCPU;
    VkDescriptorSetLayout m_VoxCasterSetL   = VK_NULL_HANDLE; // set3: voxel SSBO + choice
    VkDescriptorPool      m_VoxCasterPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_VoxCasterSet[VK_FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      m_VoxCasterLayout = VK_NULL_HANDLE; // sets 0-2 = m_VsmPageLayout's, +set3
    VkPipeline            m_VsmVoxPipe  = VK_NULL_HANDLE;     // DYN atlas (tree_vsm_vox.vert)
    VkPipeline            m_VsmVoxPipeS = VK_NULL_HANDLE;     // STATIC atlas (tree_vsm_vox_s.vert)
    u32                   m_VoxCastCount = 0;                 // diag: trees casting voxels this frame
    u32                   m_VoxCastSlice = 0;                 // diag: summed caster slice (bricks; VS cost ∝ survivors × 6)
    u32                   m_VoxBandCount = 0;                 // diag: trees in the crown↔voxel crossfade band
    // Stage-2 brick cull resources (see DispatchVoxCull). The remap list is created
    // whenever bricks exist (set3 binding 2 must stay valid even if the cull pipeline
    // fails and the un-culled path draws).
    static constexpr u32  kVoxCullCmdCap  = 32768;            // compacted (tree,page) draws per tier
    static constexpr u32  kVoxCullListCap = 1u << 20;         // surviving brick indices (shared by both tiers)
    bool                  m_VoxCullReady  = false;
    bool                  m_VoxCullActive = false;            // dispatched this frame → draws use the compacted stream
    VkDescriptorSetLayout m_VoxCullSetL   = VK_NULL_HANDLE;
    VkDescriptorPool      m_VoxCullPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_VoxCullSet[VK_FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout      m_VoxCullLayout = VK_NULL_HANDLE;
    VkPipeline            m_VoxCullPipe   = VK_NULL_HANDLE;
    CVulkanBuffer*        m_VoxCullCmdD   = nullptr;          // VkDrawIndirectCommand[kVoxCullCmdCap] (dyn)
    CVulkanBuffer*        m_VoxCullCmdS   = nullptr;          // (static)
    CVulkanBuffer*        m_VoxCullList   = nullptr;          // u32[kVoxCullListCap] global brick indices
    CVulkanBuffer*        m_VoxCullStats  = nullptr;          // [0]=cmdD [1]=cmdS [2]=list [3]=overflow [4]=hzbCulled — words 0/1 double as draw counts
    CVulkanBuffer*        m_VoxCullStatsRB = nullptr;         // host readback of the above (r_vsm_debug telemetry)
    u32*                  m_VoxCullStatsPtr = nullptr;
    // Compact dispatch: the cull runs one workgroup per tree WITH a live brick choice
    // (CPU list from VsmUpdateNearSet), not per tree in the level — the fixed overhead
    // of ~2600 mostly-empty workgroups ×2 modes was a measurable slice of VSM/Bins.
    xr_vector<u32>        m_VoxCastListCPU;                                 // tree indices with choice.count > 0 this frame
    CVulkanBuffer*        m_VoxCullTreeList[VK_FRAMES_IN_FLIGHT] = {};      // host-visible ring: the list, uploaded in VsmBin
    void*                 m_VoxCullTreeListPtr[VK_FRAMES_IN_FLIGHT] = {};
    u32                   m_VoxCullTreeN = 0;                               // entries in this frame's slot
    VkPipeline            m_HullDebugPipe = VK_NULL_HANDLE;   // r_vsm_tree_hull_debug: shaded hull/lobe fallback overlay
    xr_vector<GpuTreeHullInfo> m_HullInfoCPU;                 // per-tree hull ranges (shadow shell; debug fallback draws)
    // Crown VOXEL CLOUD (UE Nanite-foliage style, r_vsm_tree_hull_vox): per species,
    // kHullLods levels of individual colored cubes baked from the leaf-card triangles;
    // voxel GRID cell doubles per level (finest = level 0 = maxExt / r_vsm_tree_hull_vox).
    // The RENDERED cube edge is CONTINUOUS — dist × r_vsm_tree_hull_vox_px angular size,
    // so voxels grow smoothly with every meter (constant projected px, no size steps);
    // the baked levels only supply positions/density, picked as the largest cell ≤ the
    // current cube size (grid swaps hidden by the dither crossfade).
    static constexpr u32       kHullLods = 5;
    CVulkanBuffer*             m_VoxVB = nullptr;             // GpuTreeVoxel[], per-instance vertex stream
    CVulkanBuffer*             m_BrickVB = nullptr;           // GpuTreeBrick[] SSBO — the shadow caster's unit
    xr_vector<TreeVoxLod>      m_BrickLodInfoCPU;             // per-tree × kHullLods brick slices (finest→coarsest)
    u32                        m_BrickTotal = 0;              // diag: bricks baked (all species, all LODs)
    VkPipeline                 m_VoxFwdPipe = VK_NULL_HANDLE;   // forward crown cube tier (r_tree_vox_dist)
    xr_vector<TreeVoxLod>      m_VoxLodInfoCPU;               // per-tree × kHullLods (finest→coarsest)
    u32                        m_VoxTotal = 0;                // diag: voxels baked (all species, all LODs)
    xr_vector<u8>              m_WindClassCPU;                // per-tree wind class (2=foliage 1=bark 0=rigid) — hull tier is CROWNS-ONLY

    // Baked silhouette atlas (R8 coverage; FACETS cols × m_ImpostorMeshCount rows).
    VkImage        m_ImpostorAtlas      = VK_NULL_HANDLE;
    VmaAllocation  m_ImpostorAtlasAlloc = VK_NULL_HANDLE;
    VkImageView    m_ImpostorAtlasView  = VK_NULL_HANDLE;
    VkSampler      m_ImpostorSampler    = VK_NULL_HANDLE;
    u32            m_ImpostorMeshCount  = 0;   // atlas rows (unique meshes)
    CVulkanBuffer* m_ImpostorSilIdx     = nullptr;   // u32/tree → atlas row
    CVulkanBuffer* m_VsmImpostorDynIndirect = nullptr;   // VkDrawIndirectCommand/tree (dyn near set)

    bool                  m_ImpostorReady    = false;
    VkDescriptorSetLayout m_ImpostorSetL     = VK_NULL_HANDLE;   // set1: meta + silIdx + atlas
    VkDescriptorPool      m_ImpostorPool     = VK_NULL_HANDLE;
    VkDescriptorSet       m_ImpostorSet      = VK_NULL_HANDLE;
    VkPipelineLayout      m_ImpostorLayout   = VK_NULL_HANDLE;
    VkPipeline            m_ImpostorPipe     = VK_NULL_HANDLE;
    // Indirect builder (VkDrawIndexedIndirectCommand → VkDrawIndirectCommand, indexCount→6).
    VkDescriptorSetLayout m_ImpostorCmdSetL  = VK_NULL_HANDLE;
    VkDescriptorSet       m_ImpostorCmdSet   = VK_NULL_HANDLE;
    VkPipelineLayout      m_ImpostorCmdLayout = VK_NULL_HANDLE;
    VkPipeline            m_ImpostorCmdPipe  = VK_NULL_HANDLE;
};

}  // namespace VK

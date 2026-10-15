// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  GPU-DRIVEN WORLD FORWARD PASS (vk_world_gpu)
// ============================================================================
//  The forward World pass is CPU-bound: every frame the CPU walks all level
//  visuals, frustum-tests each, Submit()s the survivors, sorts, and issues one
//  draw per mesh with full state tracking. That cost scales with OBJECT COUNT —
//  so detail-rich, interior-heavy mod levels hit a CPU draw-call wall and the
//  fps collapses even though the GPU is idle.
//
//  This moves the static-world submission to the GPU: extract the opaque static
//  meshes once at level load (shared VB/IB pools, world-space → no per-instance
//  transform), grouped by (material, pipeline key, vb, ib); per frame a compute
//  shader frustum-culls them into per-group VkDrawIndexedIndirectCommand lists,
//  and the draw loop issues ONE vkCmdDrawIndexedIndirectCount per group. CPU
//  cost becomes ~O(material groups) instead of O(meshes) → object count no
//  longer drives the CPU, so modders get headroom for dense interiors.
//
//  Mirrors vk_shadow_gpu (same cull/indirect machinery) but the draw is the
//  rich forward path: per-group material set + env set + mvp/tail push, deriving
//  the pipeline exactly like RenderQueue::Flush.
//
//  v1: OPAQUE + alpha-tested STATIC level meshes on the standard lmap/vlit +
//  terrain pipelines. Tessellated + wmark materials and DYNAMIC visuals stay on
//  the CPU RenderQueue (added on top). Phase 2 adds HZB occlusion culling.
// ============================================================================
#pragma once
#include "HW_Vulkan.h"
#include "vk_pass_context.h"   // VK::FrameContext

class vkRender_Visual;
class vkFVisual;
namespace VK { class RenderQueue; struct WorldMaterial; }   // WorldMaterial: vk_world_material.h

namespace VK { namespace WorldGPU {

// ============================================================================
//  Shared leaf index (load-time only)
// ============================================================================
// Every load-time pass that needs the level's static leaves used to walk
// RImplementation.Visuals itself, recursing MT_HIERRARHY/MT_LOD and deduping
// on its own. Measured on pripyat_full (449 864 visuals, 18-08): WorldGPU
// extract 327 ms + CompactPools collect 679 + TerrainMask collect 504 +
// ShadowGPU casters — well over a second of a 19.5 s load spent walking the
// SAME tree four times. This walks it ONCE and hands the result to all of
// them; each keeps its own filter, which is where they actually differ.
//
// Order is the old recursive one (roots in Visuals[] order, children in
// declaration order, first occurrence wins) — TerrainMask draws its regions in
// list order, so preserving it keeps that bake bit-identical.
// Built on first call, valid for this level; ReleaseLeafVisuals() drops it at
// the end of the render load phase (dynamic spawns add visuals after that, so
// nothing may read a stale list later).
const xr_vector<vkFVisual*>& LeafVisuals();
void ReleaseLeafVisuals();

// One cullable ENTRY: either a whole mesh (material fragment) or one cluster
// of its LOD DAG (r_cluster). Shared with vk_cluster_stream (page assembly /
// residency) — the layout mirrors the `Meta` struct in world_cull.comp (80 B,
// std430). _pad1 carries the entry's streaming PAGE id (0 = identity page:
// non-clustered meshes whose ib_first stays absolute in their own pool IB).
struct GpuMeshMeta {
    Fvector sphere_P; float sphere_R;    // cull sphere (frustum/HZB)
    Fvector4 lodSelf;                    // xyz birth-group centre, w radius (self LOD test)
    Fvector4 lodParent;                  // xyz parent-group centre, w radius (parent LOD test)
    u32 index_count; u32 ib_first; u32 first_vertex; u32 group;
    float selfError; float parentError;
    u32 flags;   // bit0 = hard cut (alpha-tested material: its depth path can't dither)
    u32 _pad1;   // streaming page id (vk_cluster_stream); 0 = identity page
};
constexpr float kErrInf = 1e30f;   // "no parent" sentinel (roots + plain meshes)

void Build();    // extract static world meshes at level load (idempotent)
bool Built();
void Destroy();

// ============================================================================
//  Pool compaction (Stage B increment (б) — free the cluster-repacked slices)
// ============================================================================
// After Build, the vertex/index data of fully-repacked clustered meshes exists
// TWICE: in the ClusterStream page pools (what every GPU path actually draws)
// and in the original level VB/IB pools (nVB/nIB). CompactPools rebuilds each
// pool buffer with only the slices still needed by CPU-side consumers
// (non-clustered meshes, alpha-tested casters, terrain, trees, CPU leaves),
// swaps it in place (CVulkanBuffer::AdoptFrom — every visual's m_mesh pointer
// heals automatically), patches the offsets (m_mesh vBase/iBase, identity
// metas, groups) and zeroes freed visuals' m_mesh counts so any CPU submit of
// them is a clean no-op. Freed meshes can then only be drawn by the GPU-driven
// paths — the callers of PoolsCompacted() force those paths on and route the
// always-CPU static consumers (rain/ground map, spot/point light shadows)
// through the cluster shadow targets kTargetRain/kTargetDyn.
// Requires Built() + ShadowReady() + r_pool_compact; call from level_Load
// AFTER Build and BEFORE Trees->Build / ShadowGPU::Build (they snapshot pool
// handles/offsets). No-op otherwise.
void CompactPools();
bool PoolsCompacted();   // true = freed slices are gone; CPU fallbacks must stay off

// Extra CullShadow/DrawShadow target slots (beyond ShadowGPU::TGT_FAR/C0/C1):
constexpr u32 kTargetRain = 3;   // rain/ground top-down occlusion map
constexpr u32 kTargetDyn  = 4;   // shared slot for spot tiles / point-cube faces
                                 // (cull→draw pairs serialize in the frame cmd)

// True if this visual is in the GPU-driven set (drawn by Draw*; the CPU queue
// must exclude it to avoid double-draw). Cheap binary search; safe before Build.
bool InSet(vkRender_Visual* v);

// Count of meshes in the GPU set (diagnostics).
u32  SetSize();

// Per-frame CPU path: Submit ONLY the pre-built non-GPU static leaves (wmark/tess/
// no-diffuse), frustum-culled (same 6-plane test as the GPU cull). Replaces the
// O(all-visuals) walk → the CPU static cost no longer scales with object count.
// Returns the number submitted (visible). The GPU set is drawn by Draw*, not here.
u32  SubmitCpuMeshes(RenderQueue& q, const Fmatrix& viewProj, bool doCull);

// Compute-cull the static set against the camera frustum (planes from viewProj)
// → per-group indirect/count regions. MUST run OUTSIDE a dynamic-rendering scope.
void Cull(VkCommandBuffer cmd, const Fmatrix& viewProj);
// Stage D compose: same cull with an explicit camera (chunk-LOCAL position/dir).
// A chunk at world offset T culls with viewProj·T and camPos − T so the LOD cut
// and frustum are evaluated in the chunk's local (= dataset) space.
void Cull(VkCommandBuffer cmd, const Fmatrix& viewProj, const Fvector& camPos, const Fvector& viewDir);

// True if the Hi-Z occlusion cull pipeline built OK (world_cull_hzb.comp + the
// 2nd indirect/count buffers). When false, CullColor no-ops and DrawColor must
// fall back to the frustum set (DrawSet::A).
bool OcclusionReady();

// Which of the two indirect/count buffer pairs a Draw* call consumes. The pairs
// carry different things depending on which cull path ran this frame:
//
//            r_hzb_prepass 0 (single phase)      r_hzb_prepass 1 (two phase)
//   A        Cull()      -> frustum + LOD cut    CullEarly() -> EARLY set
//   B        CullColor() -> that set, Hi-Z'd     CullLate()  -> LATE set
//   AB       (not used)                          the whole prepass set
//
// Both pairs use the SAME per-group region layout (groupBase prefix sums), which
// is what lets AB be two indirect draws at identical offsets rather than a merge.
// An enum class, not the old `bool useOcclusion`: the compiler then finds every
// call site instead of silently accepting a stale `false`.
enum class DrawSet : u32 { A = 0, B = 1, AB = 2 };

// Phase A occlusion cull: frustum + Hi-Z test the static set against `hzbView`
// (the depth pyramid built from THIS frame's prepass depth) → a SEPARATE indirect/
// count region (cmds2/counts2) drawn only by DrawColor(useOcclusion=true). The
// depth prepass keeps the full frustum set (Cull), so the pyramid is complete and
// this never over-culls. MUST run OUTSIDE a render pass, AFTER the HZB build.
// `hzbGen` = CDetailManager::HZBGeneration() — the descriptor for the pyramid is
// (re)written only when it changes, because s_set2 is ONE set shared by all frames
// in flight and rewriting it every frame updates a set an executing command buffer
// still references. A raw handle comparison is not enough: after Destroy+Create the
// driver can hand back the same VkImageView value.
void CullColor(VkCommandBuffer cmd, const Fmatrix& viewProj, const Fvector& cameraPos,
               VkImageView hzbView, VkSampler hzbSampler, u32 hzbGen);

// ---- Two-phase occlusion (r_hzb_prepass) -----------------------------------
// True if world_cull_two.comp + the late list built OK. False ⇒ the caller must
// keep the single-phase Cull/CullColor pair.
bool TwoPhaseReady();

// PHASE 1. Replaces Cull() for this frame: same cut, same streaming requests and
// LRU feedback (it is the only full scan now, so it has to keep feeding the
// streamer), but entries the PREVIOUS frame's pyramid says are hidden go to a
// compact late list instead of into set A. `prevHzbUsable` false (first frame,
// resize, a frame where nothing built the pyramid) ⇒ the test is skipped entirely
// and set A comes out identical to Cull()'s — the safe degenerate case.
// The previous frame's matrix and eye are remembered internally, so the caller
// always passes THIS frame's camera. MUST run OUTSIDE a render pass.
void CullEarly(VkCommandBuffer cmd, const Fmatrix& viewProj, const Fvector& camPos,
               const Fvector& viewDir, VkImageView hzbView, VkSampler hzbSampler,
               u32 hzbGen, bool prevHzbUsable);

// PHASE 2. Re-tests ONLY the late list against the pyramid rebuilt from phase 1's
// depth, into set B, via vkCmdDispatchIndirect over the group count phase 1
// accumulated — the CPU never learns how many entries were deferred. Run after
// the phase-1 depth is down and the pyramid has been rebuilt; MUST run OUTSIDE a
// render pass. After it, the prepass set is A ∪ B and so is the colour set.
// `hzbFresh` = "the pyramid really was rebuilt from phase-1 depth THIS frame"
// (CDetailManager::HZBBuiltThisFrame after the forced rebuild). False makes the
// pass emit the whole late list untested — the deferred entries are already out
// of set A, so a stale pyramid must never be allowed to drop them a second time.
void CullLate(VkCommandBuffer cmd, const Fmatrix& viewProj, const Fvector& cameraPos,
              VkImageView hzbView, VkSampler hzbSampler, u32 hzbGen, bool hzbFresh);

// Depth prepass: indirect depth-only draw of the culled statics (solid + AT
// variants), reusing the shared depth pipelines. INSIDE the prepass BeginRendering.
// mvTarget: the camera prepass scope carries the RG16F motion attachment
// (r_mv_prepass) — bind the pipeline twins that declare it. See RenderQueue::FlushDepth.
void DrawDepth(VkCommandBuffer cmd, const Fmatrix& viewProj, bool displaceTerrain = false,
               DrawSet set = DrawSet::A, bool mvTarget = false);

// NORMAL G-buffer: the same culled statics again, into the SSAO normal RT with
// depth TEST-only, so GTAO gets real vertex normals instead of depth derivatives.
// Terrain groups are skipped (their prepass depth is tessellated + displaced —
// a flat re-raster would fail LEQUAL). INSIDE the AOnormal BeginRendering.
void DrawNormals(VkCommandBuffer cmd, const Fmatrix& viewProj, DrawSet set = DrawSet::A);

// Color pass: indirect forward draw of the culled statics. Binds per group the
// pipeline (derived like Flush) + material set + env set, pushes mvp(=viewProj)
// + per-material tail. INSIDE the color BeginRendering. `envSet` = EnvLight set.
// `set` picks which culled set to draw: A = frustum (single-phase, no occlusion),
// B = CullColor's Hi-Z set, AB = the two-phase prepass set (EARLY then LATE, one
// indirect draw each per group — equality with the prepass, so nothing is written
// but unshaded).
// atEqual: AT groups draw with the depth-EQUAL/no-write pipeline variant
// (r_at_equal) — pass true only when THIS view's depth prepass drew the AT set
// (home pass); compose clones and other callers without a matching AT prepass
// must pass false or their AT geometry early-Z's away entirely.
// prepassZ: opaque groups drop the redundant depth write (r_z_prepass) so the
// discard-bearing uber-FS keeps early-Z; same caller contract as atEqual.
// fsFrame >= 0 arms the r_fsinv_split terrain/mesh attribution: query 1 brackets
// the terrain groups (they sort first), query 2 the mesh groups; both always
// begin+end so the frame's result batch stays readable.
void DrawColor(VkCommandBuffer cmd, const Fmatrix& viewProj, VkDescriptorSet envSet,
               DrawSet set = DrawSet::A, bool atEqual = false, bool prepassZ = false,
               int fsFrame = -1);

// Cluster-LOD debug overlay (r_cluster_debug): redraw the culled set colored by
// cluster id — mode 1 = flat fill (UE-style cluster view), 2 = wireframe over
// the scene. INSIDE the color BeginRendering, after DrawColor. `set` must match
// the DrawColor call so the overlay shows exactly what was drawn.
void DrawDebug(VkCommandBuffer cmd, const Fmatrix& viewProj, int mode, DrawSet set = DrawSet::A);

// ============================================================================
//  Phase 3 — cluster-LOD SHADOW casters (sun cascades / far map + VSM)
// ============================================================================
// The same meta SSBO + cluster IBs drive the shadow-caster cut: for an ORTHO
// light the DAG test degenerates to a constant world-error budget (selfError <=
// texelWorld·k < parentError) — camera-independent, so cached shadow content
// (VSM pages, cascade static maps) never changes from camera motion.

// Raw data exports (the VSM bin builds its own descriptor sets over these).
VkBuffer MetaBuffer();        // GpuMeshMeta[] SSBO (80 B entries)
VkBuffer GroupBaseBuffer();   // per-group cmd-region base (= entryOffset)
u32      EntryCount();        // total cullable entries (dispatch size)
u32      GroupCount();        // draw groups (material × pipeline × vb/ib)
// Host copy of the entry array (kept for the CPU cut diagnostics) + a build
// counter — the VSM candidate table is derived from these and rebuilt lazily
// when the stamp moves (level reload) or its error budget changes.
const xr_vector<GpuMeshMeta>& HostMeta();
u32      BuildStamp();
// Per-group depth-draw info. Returns false past the end. `alphaTested` groups
// draw through the AT discard pipelines when ShadowATActive() (legacy: skipped).
// matOut/tcOffsetOut (optional): the group's material + UV byte offset — the
// VSM AT page path needs them to bind the diffuse for its discard.
bool GetGroupShadow(u32 g, VkBuffer& vb, VkBuffer& ib, u32& stride, VkIndexType& iType,
                    u32& entryOffset, u32& entryCount, bool& alphaTested,
                    const WorldMaterial** matOut = nullptr, u32* tcOffsetOut = nullptr);

// Sun cascade / far-map path (r_shadow_cluster): compute-cull the cluster set
// into per-(target, group) indirect regions with the ortho LOD cut, then
// depth-draw a target's regions. Targets index a 3-slot buffer (mirrors
// ShadowGPU::Target: 0 = far, 1/2 = cascades). texelWorld[i] = the target's
// texel size in metres (errBudget = texelWorld × r_vsm_cluster_lod).
bool ShadowReady();
// r_gpu_shadows_at && the AT depth layout exists: the shadow cull emits the
// alpha-tested entries too and DrawShadow draws them with the discard pipeline
// + the material's diffuse — the CPU FlushDepth cutout queues become redundant.
bool ShadowATActive();
void CullShadow(VkCommandBuffer cmd, const u32* targets, const Fmatrix* viewProjs,
                const float* texelWorld, u32 n);
// skipTerrain: don't issue the terrain groups' draws — the spot/point light
// maps exclude terrain by design (kSpotCasterMaxR; a lamp under the ground is kept
// off the terrain by sector, vk_light.cpp), so their kTargetDyn draws must too.
void DrawShadow(VkCommandBuffer cmd, u32 target, const Fmatrix& lightVP, bool skipTerrain = false);

}} // namespace VK::WorldGPU

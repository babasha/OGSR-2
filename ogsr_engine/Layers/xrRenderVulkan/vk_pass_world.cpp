// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_rendering.h"
#include "vk_rr_guides.h"          // VK::RRGuide — guide G-buffer on the opaque scope
#include "vk_pass_world.h"
#include "vk_pipeline_cache.h"
#include "vk_render_queue.h"   // g_RenderQueue (phase 3)
#include "vk_world_gpu.h"      // VK::WorldGPU (GPU-driven static forward path)
#include <algorithm>
#include "vk_swapchain.h"      // Swapchain.m_Images / m_ImageViews
#include "CRender_Vulkan.h"    // RImplementation, vkRender_Visual ref
#include "vk_Visual.h"         // vkRender_Visual::Submit
#include "vk_pass_skinned.h"   // Pass_Skinned (skinned dynamic leaves)
#include "vk_env_light.h"      // EnvLight::Update — per-frame sun/hemi/ambient UBO (set 1)
#include "vk_instance_gpu.h"   // InstanceGPU — GPU-driven instanced host scene (editor)
#include "vk_command_buffer.h" // CommandManager.GetCurrentFrame() — in-flight slot
#include "vk_barriers.h"       // SceneAttachmentBarrier + ImageState — prepass → color ordering
#include "vk_motionvec.h"     // MV written by the depth prepass (r_mv_prepass) + the resolve before SSAO
#include "vk_framegraph.h"     // VK::g_FrameGraph — frame-wide depth/color state (depth-thrash coalescing)
#include "vk_pass_ssao.h"      // GTAO from the prepass depth (before the color pass)
#include "vk_pass_gather.h"    // final gather (step 4): per-pixel indirect, same prepass inputs
#include "vk_TreeManager.h"    // trees join the prepass depth (GTAO occluders + early-Z)
#include "vk_profiler.h"       // VK::Prof sub-zones (Depth/SSAO/Color/Statics/Skinned breakdown)
#include "vk_pass_registry.h"  // VK_REC_SUB -- подстатьи записи в циклах
#include "vk_vrs.h"            // VK::VRS — variable rate shading (depth-driven SRI)
#include "vk_pass_shadow.h"    // nfs46: VK::SunMask
#include "vk_vsm.h"            // VK::VSM — virtual shadow maps page marking (WIP, r_vsm)
#include "vk_clustered.h"      // VK::Clustered — clustered forward light cull (r_clustered)
#include "vk_volumetrics.h"    // VK::Vol — froxel volumetric inject/integrate (r_vol)
#include "vk_gi_froxel.h"      // VK::GiFroxel — the GI field over that same froxel grid
#include "vk_async.h"          // VK::Async — records the fog inject onto the compute queue
#include "vk_terrain_cache.h"  // VK::TerrainCache — composite ground cache bake (r_terra_cache)
#include "vk_pass_water.h"     // VK::Water_CaptureLidStatics — statics for the ripple lid map
#include "vk_pass_particles.h" // VK::CollectSmokeParticles — Stage-1 smoke media inject
#include "vk_DetailManager.h"  // RImplementation.Details — shared Hi-Z pyramid (r_hzb_cull)
#include "vk_hzb_readback.h"   // VK::HzbReadback — one pyramid mip to the CPU for NxOcc (r_dyn_occ 2)

extern float ps_r_vol_smoke_inject;   // gate the per-frame smoke collect (0 = skip)
extern float ps_r_gi_froxel;          // only to name the value in the "asked for, not built" line
#include "HW_Vulkan.h"         // VulkanHW.m_bVRSSupported
#include "../../xr_3da/device.h" // RV().dwTimeGlobal (cull diag throttle)
#ifdef __SWITCH__
void NxGpuMark(VkCommandBuffer cmd, const char* name);   // CRender_Vulkan.cpp: [nx gpu slow], GPU time up to here
#define NX_GPU_MARK(cmd, name) NxGpuMark(cmd, name)
#else
#define NX_GPU_MARK(cmd, name) ((void)0)
#endif

// Global scope (not in namespace VK → avoid VK::ps_r_cull mangling), like the rain externs.
extern int ps_r_cull;
extern int ps_r_gpu_world;   // GPU-driven static world forward path (A/B with 0)
extern int ps_r_clustered;   // clustered forward light cull (A/B with 0)
extern int ps_r_clustered_debug; // clustered froxel light-count heatmap (also activates the cull)
extern int ps_r_hzb_cull;    // Hi-Z occlusion cull of the GPU-driven static color pass (A/B with 0)
extern int ps_r_tree_hzb;    // Hi-Z occlusion cull of the forward TREE set — the pyramid's other consumer
extern int ps_r_dyn_occ;     // r_dyn_occ 2: NxOcc reads the pyramid back (VK::HzbReadback) -- the third consumer
extern int ps_r_profiler;    // 2+ opens the Depth/Trees sub-zone (the canopy's share of the prepass)
extern int ps_r_tree_depth_sort; // camera prepass in near-to-far depth slabs (0 = texture-group order)
extern int ps_r_tree_depth_gpu;  // GPU-cull + indirect the camera depth prepass (0 = CPU walk)
extern int ps_r_hzb_prepass; // two-phase occlusion: cull the DEPTH PREPASS too (supersedes r_hzb_cull)
extern int ps_r_world_geom_stats; // 1 = zone-level HW counters, 2 = split the DEPTH PREPASS by who draws
extern int ps_r_cluster_debug; // cluster-LOD debug overlay: 1 = fill colors, 2 = wireframe (live)
extern int ps_r_fg_coalesce; // framegraph: coalesce SSAO/VRS/VSM depth-read round-trips (A/B with 0)
extern int ps_r_ssao_npc_normals;   // NPC normal G-buffer for GTAO (global scope: block-scope extern in namespace VK would mangle → LNK2001)
extern int ps_r_ssao_static_normals; // statics normal G-buffer for GTAO (same global-scope reason as above)
extern int ps_r_compose;     // Stage D world composition: total chunk count (home + clones), 0/1 = off
extern int ps_r_vrs;         // VRS level (0/1/2) — here only to gate the FS-invocation stats query
extern int ps_r_vrs_force;   // diag: force pipeline-rate NxN on the world color pass, ignoring the SRI
extern int ps_r_at_equal;    // AT statics: depth-EQUAL color path (needs the prepass) — live A/B
extern int ps_r_fsinv_split; // diag: 4-way FS-invocation attribution in World/Color (replaces the single counter)
extern int ps_r_tree_prepass;  // r_tree_prepass 0: skip only the trees in the depth prepass
// -nx_bisect2 sub-experiments inside World (Switch, 01-10): set by vk_pass_registry for ONE frame.
// 1 = frame without the depth prepass (as r_depth_prepass 0: colour writes its own depth), 2 = no statics colour,
// 3 = no rigid dynamics, 4 = no skinned. 0 = normal.
extern "C" int g_xr_nx_wskip;
int g_xr_nx_wskip = 0;
#ifdef __SWITCH__
extern "C" int g_xr_nx_diag;   // device.cpp, nx_diag.txt window mode (1 = NPC colour through a 1x1 scissor)
VkRect2D g_xr_nx_world_scissor{};   // the colour scope's scissor, for vk_world_gpu's nx_statab 28 to restore
#endif
extern int ps_r_grass;          // nfs41 vk_console_min.cpp: r_grass (file scope -- inside namespace VK it would be VK::ps_r_grass)
extern int ps_r_depth_prepass; // r_depth_prepass 0: keep the prepass scope (it clears depth) but draw nothing into it (R1 had no prepass)
extern int ps_r_z_prepass;   // statics color: no z-write → early-Z with discard (live A/B)

namespace VK {

// Dynamic (spawned) visuals collected by CRender::add_Visual this frame, drained
// below after the level statics. Declared in vk_pass_world.h (one set per frame slot).
RT::PerSlot<VisualLists> g_VisualLists;

void BeginVisualExtract()
{
    VisualLists& l = g_VisualLists.cur();
    l.dyn.clear();
    l.hud.clear();
    l.shadowOnly.clear();
}

void ClearAllVisualLists()
{
    for (u32 i = 0; i < RT::kSlots; ++i)
    {
        VisualLists& l = g_VisualLists.at(i);
        l.dyn.clear();
        l.hud.clear();
        l.shadowOnly.clear();
    }
}

// ---------------------------------------------------------------------------
// Stage D world composition (r_compose N): replicate the loaded level's GPU
// static set as N-1 clone chunks tiled on a grid around the home footprint.
// A chunk is {the ONE resident dataset, world offset T}: it culls with
// viewProj·T (frustum planes land in chunk-local space for free) and draws
// with mvp = viewProj·T — zero shader changes, zero extra geometry VRAM.
// vWorldPos in the FS stays in HOME coordinates (fog/VSM/dyn-light sampling
// behaves "as the original tile") — known slice-1 tradeoff.
// Clones are render-only statics: no collision/AI; trees/grass/LODs/CPU
// leaves stay home-only in this slice.
// ---------------------------------------------------------------------------
namespace Compose {
    static xr_vector<Fvector> s_Offsets;    // clone offsets (home excluded)
    static size_t             s_vis = 0;    // Visuals.size() the layout was built from
    static Fvector            s_center{};   // home footprint center (world)
    static float              s_radius = 0.f; // bounding-sphere radius of the footprint

    static void EnsureLayout()
    {
        const size_t nVis = RImplementation.Visuals.size();
        if (!s_Offsets.empty() && s_vis == nVis) return;
        s_Offsets.clear();
        s_vis = nVis;
        s_radius = 0.f;
        if (!nVis) return;
        // Chunk pitch MUST be >= the level's full GEOMETRY envelope (sphere P±R),
        // not just the playable footprint. The first attempt tiled by the 2-98%
        // playable width (~3.2 km) while the actual geometry — the fake backdrop
        // hills + terrain skirt X-Ray adds to hide the map edge — reaches R≈2.3 km
        // from centre. A clone at +3.2 km then had geometry back at 3.2-2.3≈0.9 km,
        // overlapping home (which reaches 2.3 km): the overlap wrote nearer depth
        // over home walls (early-Z ate them) and its backdrop sliced through the
        // scene as a grey plane. Envelope pitch makes tiles ABUT with no overlap.
        // (Copies of one level still seam backdrop-to-backdrop — inherent to
        // cloning; truly seamless needs authored-abutting levels, a later slice.)
        Fvector mn{ 1e9f, 1e9f, 1e9f }, mx{ -1e9f, -1e9f, -1e9f };
        u32 nUsed = 0;
        for (IRenderVisual* iv : RImplementation.Visuals) {
            if (!iv) continue;
            const Fsphere& bs = static_cast<vkRender_Visual*>(iv)->vis.sphere;
            if (bs.R <= 0.f || bs.R > 2000.f) continue;   // skip sky domes / fog volumes
            mn.x = _min(mn.x, bs.P.x - bs.R); mx.x = _max(mx.x, bs.P.x + bs.R);
            mn.y = _min(mn.y, bs.P.y - bs.R); mx.y = _max(mx.y, bs.P.y + bs.R);
            mn.z = _min(mn.z, bs.P.z - bs.R); mx.z = _max(mx.z, bs.P.z + bs.R);
            ++nUsed;
        }
        if (nUsed < 2 || mx.x <= mn.x || mx.z <= mn.z) return;
        // 8 m seam gap so abutting tiles don't z-fight at the shared edge.
        const float pitchX = (mx.x - mn.x) + 8.f;
        const float pitchZ = (mx.z - mn.z) + 8.f;
        s_center.set((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
        // Bounding sphere of one chunk envelope (for the per-chunk frustum
        // pre-check in the hooks — chunks fully out of view never dispatch).
        const float ex = (mx.x - mn.x) * 0.5f, ey = _max(1.f, (mx.y - mn.y) * 0.5f), ez = (mx.z - mn.z) * 0.5f;
        s_radius = _sqrt(ex * ex + ey * ey + ez * ez);
        // Ring order around home: 8 first-ring neighbours, then the 16 of ring 2
        // → supports up to r_compose 25 (5×5 world).
        static const int ring[24][2] = {
            { 1,0},{-1,0},{0, 1},{0,-1},{ 1, 1},{-1, 1},{ 1,-1},{-1,-1},
            { 2,0},{-2,0},{0, 2},{0,-2},{ 2, 1},{ 2,-1},{-2, 1},{-2,-1},
            { 1,2},{-1,2},{ 1,-2},{-1,-2},{ 2, 2},{-2, 2},{ 2,-2},{-2,-2} };
        for (const auto& rc : ring) {
            Fvector o; o.set(rc[0] * pitchX, 0.f, rc[1] * pitchZ);
            s_Offsets.push_back(o);
        }
        Msg("[VK Compose] layout: envelope %.0fx%.0fm (%u visuals), pitch %.0fx%.0fm, chunk R=%.0fm, %zu clone slots",
            mx.x - mn.x, mx.z - mn.z, nUsed, pitchX, pitchZ, s_radius, s_Offsets.size());
    }

    // Number of clone chunks to draw this frame (0 = composition off).
    static u32 CloneCount()
    {
        if (ps_r_compose <= 1) return 0;
        EnsureLayout();
        return (u32)_min((size_t)(ps_r_compose - 1), s_Offsets.size());
    }

    // viewProj·T and chunk-local camera for clone c.
    static void ChunkView(u32 c, const Fmatrix& viewProj, Fmatrix& outVP, Fvector& outCam)
    {
        Fmatrix T; T.translate(s_Offsets[c]);
        outVP.mul(viewProj, T);                      // same composition as Flush's mvp = viewProj·xform
        outCam.sub(RV().vCameraPosition, s_Offsets[c]);
    }

    // CPU pre-check: is clone c's footprint sphere in the camera frustum at all?
    // Chunks beyond the far plane / behind the camera skip their cull dispatch
    // AND their render pass entirely — with 24 slots and a ~1.5 km far plane
    // only the adjacent visible chunks cost anything (measured: 24 blind culls
    // of the full entry set burned ~10 ms/frame of pure compute).
    static bool ChunkInView(u32 c, CFrustum& f)
    {
        if (s_radius <= 0.f) return true;
        Fvector p; p.add(s_center, s_Offsets[c]);
        return !!f.testSphere_dirty(p, s_radius);
    }
} // namespace Compose

// Layout is managed centrally now (single-layout convention): the swapchain
// image is in COLOR_ATTACHMENT_OPTIMAL for the whole frame and depth in
// DEPTH_ATTACHMENT_OPTIMAL — see CRender::Begin/End + ExecutePasses. This pass
// only records draws; it never transitions the targets.

// Push constants must match world.{vert,frag}.glsl PushConstants block.
// alphaRef (FS) + detailScale (VS) are the per-material tail at offset 72
// (8 bytes), patched together by RenderQueue::Flush when a new material
// binds. Pushing both at once requires the range stageFlags to include
// both VS and FS — the layout already does (PipelineCache::Init).
struct WorldPush
{
    Fmatrix mvp;
    float   uvScale[2];
    float   alphaRef;       // FS — discard threshold (<0 disables)
    float   detailScale;    // VS — UV scale for detail-texture sampling (.thm detail_scale)
};
static_assert(sizeof(WorldPush) == 80, "WorldPush must match GLSL PushConstants block (80 bytes)");

void Pass_World(FrameContext& ctx)
{
    if (ctx.cmd == VK_NULL_HANDLE)         return;
    if (!RImplementation.b_loaded)         return;        // No level loaded yet
    if (RImplementation.Visuals.empty())   return;
    if (PipelineCache::GetLayout()    == VK_NULL_HANDLE) return;
    if (PipelineCache::WorldLmapVS() == VK_NULL_HANDLE) return;
    NX_GPU_MARK(ctx.cmd, "w.begin");   // [nx gpu slow]: everything between the previous pass and the World pass
    if (PipelineCache::WorldVlitVS() == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = ctx.cmd;

    const int nxWSkip = g_xr_nx_wskip;
    struct NxDepthPreRestore
    {
        int saved;
        bool active;
        ~NxDepthPreRestore() { if (active) ps_r_depth_prepass = saved; }
    } nxDepthPre{ps_r_depth_prepass, nxWSkip == 1};
    if (nxWSkip == 1)
        ps_r_depth_prepass = 0;

    // ⭐⭐CPU probes that TILE this function. The 31-07 hitch hunt got exactly
    // this far and stopped: the frames a player calls a stutter spend 40 ms of
    // CPU in the World zone while World/Depth+Color+Statics sum to 6 ms and the
    // GPU is idle-normal. The 34 ms lives in the gap BETWEEN the measured
    // children — a gap no existing meter brackets. So probe every block and
    // print W/all too: `W/all - (the named ones)` is then a number, not a
    // shrug, and the next run cannot come back with "it is in the gap again".
    VK_CPU_PROBE("W/all");
    VK_REC_SUB("W/<whole body>");

    // (The late-glass and water lists are reset in CRender::Calculate now -- this
    // pass returns early without a level, and that left the lists alive into the
    // main menu pointing at deleted visuals. See the comment there.)

    // (The shared depth pipelines are baked for D32 — if the driver fell back to
    // another depth format, skip the prepass rather than mismatch formats.)
    const bool prepass = PipelineCache::GetDepthLayout() != VK_NULL_HANDLE
                      && Swapchain.m_DepthFormat == VK_FORMAT_D32_SFLOAT;

    // SSAO targets must exist BEFORE EnvLight::Update binds the AO view at
    // binding 8 — a resize re-creates them here, not mid-frame after the bind.
    // Only when the prepass will run: without it Execute never transitions the
    // fresh image out of UNDEFINED, and binding it would be invalid to sample.
    if (prepass)
    {
        VK_CPU_PROBE("W/pro/ssaotgt");
        SSAOPass::EnsureTargets(ctx.extent);
        // Same rule, same reason: EnvLight::Update binds the gather result at
        // binding 37 below, so the target it will sample has to be settled now —
        // and this call is also where the pass advances its history ping-pong, so
        // the view bound and the view written this frame are the same one.
        GatherPass::EnsureTargets(ctx.extent);
    }

    // VSM (r_vsm): compute this frame's clipmap params + ensure the screen-space mask
    // target BEFORE EnvLight binds it on the receiver set (the actual mark/alloc/render/
    // resolve happen in the prepass below; this only sets up params + the mask image).
    if (VK::VSM::Wanted())
    {
        VK_CPU_PROBE("W/pro/vsm");
        VK::VSM::BeginFrame(RV().vCameraPosition, ctx.extent);
    }

    // Terrain composite cache (r_terra_cache): probe the terrain uv affine /
    // re-bake the height+weights clipmap when the camera left the window.
    // MUST run BEFORE EnvLight::Update — the UBO's tcache transform is derived
    // from the BAKED window origin; baking after the UBO write made a re-bake
    // frame sample the NEW image with the OLD transform (one-frame relief pop
    // every few metres of running). Compute, outside any render pass, before
    // the prepass — both terrain FS passes sample the frame-static result.
    { VK_CPU_PROBE("W/pro/terra"); VK::TerrainCache::Update(cmd); }
    NX_GPU_MARK(cmd, "w.terrainCache");

    // Refresh this frame's env-lighting UBO (sun/hemi/ambient) once, before any
    // draw. RenderQueue::Flush binds the resulting set at set 1; Pass_Skinned
    // reuses the same set (at set 2). Fence-guarded slot → no in-flight write hazard.
    // `cmd` here is what lets the sky-probe refresh ride this frame's command buffer
    // instead of a blocking immediate submit: this point is outside any render pass and
    // ahead of every sampler of bindings 26/31. See vk_ibl.h.
    { VK_CPU_PROBE("W/pro/env"); EnvLight::Update(CommandManager.GetCurrentFrame(), cmd); }
    NX_GPU_MARK(cmd, "w.envLight");

    // Collect the static queue up front — the depth prepass and the color pass
    // below both consume it. With r_gpu_world the GPU draws the eligible static set
    // (Cull below) and the CPU touches ONLY the pre-built non-GPU static leaves
    // (wmark/tess/no-diffuse) — no per-frame walk over all level visuals, no
    // hierarchy double-submit. This DECOUPLES per-frame static CPU from total
    // object count (the thing that walls detail-heavy levels). Without it (A/B),
    // the old full walk runs.
    // Pool compaction freed the GPU-set meshes' CPU slices — the old full-walk
    // path can no longer draw them, so the GPU path stays forced on. For a true
    // r_gpu_world A/B set r_pool_compact 0 and reload the level.
    const bool gpuWorld = (ps_r_gpu_world || WorldGPU::PoolsCompacted()) && WorldGPU::Built();
    if (!ps_r_gpu_world && gpuWorld) {
        static u32 s_warned = 0;
        if (RV().dwTimeGlobal > s_warned + 5000) { s_warned = RV().dwTimeGlobal;
            Msg("![VK WorldGPU] r_gpu_world 0 ignored: pools compacted (r_pool_compact 0 + reload for A/B)"); }
    }

    // CPU-cost diag: time the static collect vs the whole-frame cpu — shows how
    // much CPU the collection costs (and that gpuWorld decouples it from objects).
    static double s_qpcToMs = 1000.0 / double(CPU::QPCFreq());
    const u64 t_collA = CPU::QPC();

    u32 cullTotal = 0, cullSkipped = 0;
    {
    VK_CPU_PROBE("W/pro/collect");
    Fmatrix identity;
    identity.identity();
    g_RenderQueue.Clear();
    const bool doCull = (ps_r_cull != 0);
    if (gpuWorld) {
        // Only the small pre-built non-GPU static set (frustum-culled). The GPU set
        // is compute-culled + drawn by WorldGPU::Draw* (no CPU walk for it at all).
        cullTotal = WorldGPU::SubmitCpuMeshes(g_RenderQueue, *ctx.viewProj, doCull);
    } else {
        // FRUSTUM CULLING (old path): walk ALL level visuals, frustum-test, Submit.
        // Lossless (off-frustum geometry produces no pixels). Toggle: r_cull.
        CFrustum camFrustum;
        if (doCull) { Fmatrix vp = *ctx.viewProj; camFrustum.CreateFromMatrix(vp, FRUSTUM_P_LRTB | FRUSTUM_P_FAR); }
        for (IRenderVisual* iv : RImplementation.Visuals) {
            if (!iv) continue;
            auto* rv = static_cast<vkRender_Visual*>(iv);
            ++cullTotal;
            if (doCull) {
                const Fsphere& bs = rv->vis.sphere;
                if (bs.R > 0.f && !camFrustum.testSphere_dirty(bs.P, bs.R)) { ++cullSkipped; continue; }
            }
            rv->Submit(g_RenderQueue, identity, 0.0f);
        }
    }
    g_RenderQueue.SortByKey();
    }
    const u64 t_collB = CPU::QPC();
    const double cpuCollectMs = double(t_collB - t_collA) * s_qpcToMs;
    { static u32 s_log = 0; if (RV().dwTimeGlobal > s_log + 3000) { s_log = RV().dwTimeGlobal;
        if (gpuWorld) Msg("[VK WorldGPU] CPU set drawn=%u (GPU set %u on GPU), material groups=%u", cullTotal, WorldGPU::SetSize(), WorldGPU::GroupCount());
        else          Msg("[VK Cull] world statics: %u/%u drawn (%u culled)", cullTotal - cullSkipped, cullTotal, cullSkipped); } }

    // ---- Two-phase occlusion for the DEPTH PREPASS (r_hzb_prepass) ----------
    // The single-phase path can only cull the COLOUR pass, because its pyramid is
    // a reduction of the FINISHED prepass depth — the prepass has to draw
    // everything to produce it. Two-phase breaks that circle with LAST frame's
    // pyramid, and the prepass then rasters roughly what is visible instead of
    // the whole frustum set (measured 5069 -> 2755 here, i.e. 45% waste).
    //
    // It also SUPERSEDES r_hzb_cull rather than stacking with it: the prepass set
    // ends up as EARLY u LATE, which IS the colour set, so CullColor's own scan of
    // all 830k entries (66 MB read for ~5k draws) is not run at all. That deleted
    // scan is the larger half of what this buys.
    //
    // ⚠ Off when Stage-D compose clones exist. The clone loop re-culls into set A
    // with each clone's matrix and leaves the LAST clone's commands there, which
    // would destroy the EARLY set the colour pass still needs. r_compose defaults
    // to 0; when it is not, we say so rather than let the cvar look dead — a
    // silently inert switch is exactly how r_tree_hzb wasted an A/B once.
    const bool twoPhase = gpuWorld && prepass && ps_r_depth_prepass && ps_r_hzb_prepass
                       && WorldGPU::TwoPhaseReady()
                       && RImplementation.Details && RImplementation.Details->HZBReady()
                       && Compose::CloneCount() == 0;
    if (gpuWorld && prepass && ps_r_hzb_prepass && !twoPhase) {
        static u32 s_warnTwo = 0;
        if (RV().dwTimeGlobal > s_warnTwo + 5000) { s_warnTwo = RV().dwTimeGlobal;
            Msg("![VK WorldGPU] r_hzb_prepass ignored: %s",
                !WorldGPU::TwoPhaseReady() ? "two-phase pipeline unavailable"
                : (!RImplementation.Details || !RImplementation.Details->HZBReady()) ? "no Hi-Z pyramid"
                : "r_compose clones are active"); }
    }

    // The set that reproduces the DEPTH PREPASS exactly: two-phase drew A then B,
    // single-phase drew A. The AO normal re-raster must use THIS and not the
    // colour set — it depth-tests against the prepass depth, so a mesh missing
    // from it leaves GTAO reading a depth derivative where a real normal belongs.
    const WorldGPU::DrawSet prepassSet = twoPhase ? WorldGPU::DrawSet::AB : WorldGPU::DrawSet::A;

    // nfs41: the grass generation here, with the other frame-start compute and before any raster, instead of between
    // the Trees and LODs passes (a graphics -> compute -> graphics switch in the middle of the frame). Same work, same
    // result, earlier; GenerateEarly is a no-op unless the lean2 generator is built (the HZB paths need the depth and
    // stay in place). nx_diag 220 = the old in-place generation.
    {
#ifdef __SWITCH__
        const bool nxInPlace = (g_xr_nx_diag == 220);
#else
        const bool nxInPlace = false;
#endif
        NX_GPU_MARK(cmd, "w.preGrass");   // [nx gpu slow]: the World pass up to the grass generation
        if (ps_r_grass && !nxInPlace && RImplementation.Details && RImplementation.Details->IsLoaded())
            RImplementation.Details->GenerateEarly(ctx);
        NX_GPU_MARK(cmd, "w.grassGen");
    }
    // GPU-driven static world: compute-cull the GPU static set (outside any render
    // pass); DrawDepth/DrawColor below consume its indirect buffer in both passes.
    if (gpuWorld)
    {
        VK_CPU_PROBE("W/pro/wgpucull");
        // Zone was missing entirely until 24-08: this dispatch reads the same
        // 66 MB meta array as World/HZBcull, which has been caught at 6.6 ms when
        // VRAM ran short — and ~8 ms of `World` was unaccounted for in the forest
        // profile. Measure it before believing anything about where the frame goes.
        const int zCull = VK::Prof::ZoneBegin(cmd, twoPhase ? "World/CullEarly" : "World/Cull");
        if (twoPhase)
            WorldGPU::CullEarly(cmd, *ctx.viewProj, RV().vCameraPosition, RV().vCameraDirection,
                                RImplementation.Details->HZBView(), RImplementation.Details->HZBSampler(),
                                RImplementation.Details->HZBGeneration(),
                                RImplementation.Details->HZBPrevUsable());
        else
            WorldGPU::Cull(cmd, *ctx.viewProj);
        VK::Prof::ZoneEnd(cmd, zCull);
        NX_GPU_MARK(cmd, "w.cull");
    }

    // Clustered forward: bin this frame's dynamic lights into the froxel grid
    // (compute, outside the render pass). EnvLight::Update above already uploaded
    // the light list to this slot's SSBO; the world/skinned color fragments read
    // the resulting per-cluster lists. (Init is lazy via EnvLight::Update.)
    if ((ps_r_clustered || ps_r_clustered_debug) && VK::Clustered::Ready()) {
        VK_CPU_PROBE("W/pro/clustered");
        const VK::ProjTerms cpt = VK::DeriveProjTerms(*ctx.viewProj);
        VK::Clustered::Cull(cmd, cpt, RV().vCameraPosition, ctx.extent,
                            CommandManager.GetCurrentFrame(),
                            VK::Lights::CollectFrame(RV().vCameraPosition).count);
    }

    // Froxel volumetrics (r_vol): inject + integrate the view-frustum volume
    // (compute, OUTSIDE the render pass). Needs NO scene depth — the froxel world
    // pos is analytic — so it runs here, before the prepass. EnvLight::Update +
    // the earlier Pass_SunShadow already produced the sun cascade maps it samples.
    // The composite is folded into the tonemap pass (gated on r_vol).
    if (VK::Vol::Wanted() && VK::Vol::Ready()) {
        VK_CPU_PROBE("W/pro/vol");
        const VK::ProjTerms vpt = VK::DeriveProjTerms(*ctx.viewProj);
        // Stage-1 VMS: gather this frame's alpha-smoke particles to inject as media.
        static xr_vector<VK::Vol::SmokeParticle> s_smoke;
        s_smoke.clear();
        if (ps_r_vol_smoke_inject > 0.0f)
        {
            VK_CPU_PROBE("W/pro/vol_smoke");
            s_smoke = VK::SmokeParticlesFrame();   // gathered in the extract phase (Particles_Extract)
        }
        // ASYNC COMPUTE (r_async): record the whole pass into the compute queue's own
        // command buffer instead of this frame's graphics one, and submit it right
        // away. It then runs CONCURRENTLY with everything the graphics queue does for
        // the rest of this frame (prepass, shadows, colour) — that overlap IS the win.
        // Its shadow source is the snapshot taken last frame (SnapshotShadows), and
        // depth rejection is off there: the scene depth is being rewritten by this
        // frame's prepass on the other queue, so there is no stable copy to read.
        // r_async 0 → Begin() returns null and everything below is the old path.
        // Graphics-only prerequisites (terrain-height render + canopy upload) FIRST,
        // on the graphics buffer — they used to live inside Execute, which put a render
        // pass onto the compute queue under async and killed the device.
        VK::Vol::RecordGraphicsBakes(cmd);

        VkCommandBuffer volCmd  = cmd;
        bool            onAsync = false;
        if (VK::Vol::AsyncInjectReady())
            if (VkCommandBuffer ac = VK::Async::Begin()) { volCmd = ac; onAsync = true; }

        VK::Vol::Execute(volCmd, vpt, CommandManager.GetCurrentFrame(),
                         s_smoke.empty() ? nullptr : s_smoke.data(), (u32)s_smoke.size(),
                         onAsync ? VK_NULL_HANDLE : ctx.depthView,   // prev-frame depth → froxel depth rejection
                         onAsync);

        if (onAsync) VK::Async::Submit();
    }
    // The froxel GI field is built inside that block, because it shares the fog's
    // grid and its queue decision. So r_vol 0 means the field is not built either —
    // and a feature that is silently inert because of ANOTHER feature's switch is
    // the exact shape of the two-gate trap this renderer has walked into four times.
    // Say it once, out loud, instead of leaving it to be rediscovered.
    else if (VK::GiFroxel::Wanted()) {
        static bool s_saidIt = false;
        if (!s_saidIt) {
            s_saidIt = true;
            Msg("![VK GiFroxel] r_gi_froxel is %.2f but r_vol is 0 — the field rides the fog grid, "
                "so nothing builds it and the media keep their flat fill. `r_vol 1` to see it.",
                ps_r_gi_froxel);
        }
    }

    // Throttled average: static-collect CPU vs the whole-frame cpu — confirms the
    // collect cost (now decoupled from object count when gpuWorld).
    {
        static double s_accColl = 0; static u32 s_accN = 0, s_accLast = 0;
        s_accColl += cpuCollectMs; ++s_accN;
        if (RV().dwTimeGlobal > s_accLast + 3000 && s_accN) {
            Msg("[VK WorldGPU] CPU diag: collect=%.2fms (avg/%u fr, gpuWorld=%d) | frame cpu=%.2fms",
                s_accColl / s_accN, s_accN, gpuWorld ? 1 : 0, RV().fTimeDeltaRealMS);
            s_accColl = 0; s_accN = 0; s_accLast = RV().dwTimeGlobal;
        }
    }

    // --- DEPTH PREPASS: statics into the scene depth (CLEAR). The color pass
    // then LOADs depth and early-Z rejects every occluded pixel BEFORE the
    // (heavy) forward fragment shader runs — kills overdraw cost. Alpha-tested
    // items go through the AT depth variant (same discard threshold as the
    // color pass → identical coverage, no holes).
    // r_world_geom_stats: the pool reset must sit outside every rendering scope and
    // BEFORE the first query of the frame — the prepass is the first, so it lands here
    // rather than next to SubStatsReset (which runs later, at the colour pass).
    VK::VRS::GeomStatsReset(cmd, CommandManager.GetCurrentFrame());
    if (prepass)
    {
        VK_CPU_PROBE("W/depth");
        // r_depth_prepass 0 (the Switch's R1 mode): the scope below still opens -- it is what CLEARS the scene depth -- but
        // nothing is drawn into it; the colour passes write depth themselves. The original R1 renderer had no prepass.
        const bool noPreDraw = (ps_r_depth_prepass == 0);
        // ⚠ BEFORE the render pass opens: r_tree_depth_gpu culls the prepass's trees
        // with a COMPUTE dispatch, and vkCmdDispatch inside a render pass is illegal.
        // The barrier to DRAW_INDIRECT is inside the call, so by the time the prepass
        // draws below, the indirect buffer is ready.
        if (!noPreDraw && ps_r_tree_prepass && ps_r_tree_depth_gpu && RImplementation.Trees && RImplementation.Trees->IsReady())
            RImplementation.Trees->DispatchDepthCull(cmd, *ctx.viewProj);

        // MOTION VECTORS IN THE PREPASS (r_mv_prepass). The scope gains an RG16F
        // attachment cleared to a sentinel; geometry that already runs a fragment
        // shader here (tree crowns, for the leaf alpha-test) writes its true screen
        // motion in the same raster that lays its depth, statics leave the sentinel
        // standing, and ResolveEarly below fills those by reconstruction — so the
        // motion field is complete BEFORE SSAO reads it, and the canopy's separate
        // MV overlay raster disappears.
        // ⚠⚠ mvPre IS THE ONE FLAG. Dynamic rendering requires a pipeline's colour-
        // attachment COUNT to equal the scope's, so whether the attachment is here and
        // which pipeline twins the draws below bind is ONE decision, not two — every
        // draw call in this scope is handed the same `mvPre`. (A null-view attachment
        // would keep the count at 1 and let the toggle be free of pipeline variants,
        // but it would also keep every shadow caster's pipeline out of step: these are
        // literally the same VkPipelines.)
        const VkImageView mvPreView = MotionVec::BeginPrepassTarget(cmd, ctx.extent);
        const bool        mvPre     = (mvPreView != VK_NULL_HANDLE);

        const int zDepth = VK::Prof::ZoneBegin(cmd, "World/Depth");
        if (ps_r_world_geom_stats < 2) VK::VRS::GeomStatsBegin(cmd, CommandManager.GetCurrentFrame(), 0);   // mode 1: whole zone
        {
            VK::RenderingBuilder rb(ctx.extent);
            if (mvPre) rb.ColorClear(mvPreView, MotionVec::PrepassClearValue());
            rb.Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_CLEAR).Begin(cmd);
        }

        VkViewport pvp{ 0.f, (float)ctx.extent.height, (float)ctx.extent.width, -(float)ctx.extent.height, 0.f, 1.f };
        vkCmdSetViewport(cmd, 0, 1, &pvp);
        VkRect2D psc{ {}, ctx.extent };
        vkCmdSetScissor(cmd, 0, 1, &psc);
        vkCmdSetDepthBias(cmd, 0.f, 0.f, 0.f);   // depth pipelines have dynamic bias — none here

        if (ps_r_world_geom_stats >= 2) VK::VRS::GeomStatsBegin(cmd, CommandManager.GetCurrentFrame(), 2);   // mode 2: statics only
        if (!noPreDraw)
            g_RenderQueue.FlushDepth(cmd, *ctx.viewProj, false /*include alpha-tested*/, false /*alphaTestedOnly*/, true /*displaceTerrain: snow volume matches color*/,
                                     nullptr /*cull*/, mvPre);
        if (gpuWorld && !noPreDraw) { VK_REC_SUB("W/DrawDepth A"); WorldGPU::DrawDepth(cmd, *ctx.viewProj, true /*displaceTerrain*/, WorldGPU::DrawSet::A, mvPre); }   // GPU static set into the prepass depth
        if (ps_r_world_geom_stats >= 2) VK::VRS::GeomStatsEnd(cmd, CommandManager.GetCurrentFrame(), 2);

        // Trees into the prepass depth too: GTAO sees trunks/canopies (R4's
        // gbuffer includes trees — most wilderness SSAO comes from them) and
        // the foliage color passes get early-Z. Same VP + alpha-ref as the
        // tree color pass (LEQUAL, no vertex animation) → re-raster matches.
        if (RImplementation.Trees && !noPreDraw && ps_r_tree_prepass) {
            // ⭐ The canopy's SECOND raster of the frame, and until the tree ledger it
            // had no name of its own — it was billed to World/Depth, so the `Trees`
            // zone read as the whole tree cost while a full re-raster of the same
            // geometry sat one zone over. Sub-zone at r_profiler 2 + its own hardware
            // counters, so "what do trees cost" can be answered by addition.
            if (ps_r_world_geom_stats >= 2) VK::VRS::GeomStatsBegin(cmd, CommandManager.GetCurrentFrame(), 3);   // mode 2: trees only
            const int zTD = (ps_r_profiler >= 2) ? VK::Prof::ZoneBegin(cmd, "Depth/Trees") : -1;
            RImplementation.Trees->StatsBeginPass(cmd, VK::TP_DEPTH);
            CFrustum f;
            Fmatrix fullT = *ctx.viewProj;   // CreateFromMatrix takes a non-const ref
            f.CreateFromMatrix(fullT, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
            // flodCut = r_tree_dist: skip trees the FLOD billboard already draws
            // out there. voxCut = r_tree_vox_dist: skip crowns the baked cube cloud
            // takes over (CTreeManager::Render draws those cubes into the colour pass,
            // and they write this depth themselves). Only this call site passes either
            // — shadow casters have neither stand-in, so their sets are untouched.
            // r_tree_depth_sort: THE call site that gets it. This is the camera
            // prepass — the only tree draw whose order can matter, because it is the
            // one building the depth everything else tests against.
            // gpuCulled: THIS call site, and only this one, owns the dispatch made
            // above — see the note on useGpuCull in RenderDepth for why the argument
            // list cannot be trusted to identify it.
            // mvPre: THIS call site is the one that gets it — the camera prepass is
            // the only tree raster whose scope carries the motion attachment, and the
            // canopy is the geometry the whole path exists to stop drawing twice.
            RImplementation.Trees->RenderDepth(cmd, *ctx.viewProj, -1, &f,
                                               0.0f, 1e9f, VK::TreeFlodCutDist(),
                                               VK::TreeVoxCutDist(),
                                               (u32)_max(0, ps_r_tree_depth_sort),
                                               ps_r_tree_depth_gpu != 0, -1 /*gpuShadowTarget*/,
                                               mvPre);
            RImplementation.Trees->StatsEndPass(cmd, VK::TP_DEPTH);
            if (zTD >= 0) VK::Prof::ZoneEnd(cmd, zTD);
        }
        if (ps_r_world_geom_stats >= 2) VK::VRS::GeomStatsEnd(cmd, CommandManager.GetCurrentFrame(), 3);

        // NPCs too (alpha-tested) — GTAO contact darkening under characters +
        // early-Z behind them. Identical skinning math → depths match the
        // color pass exactly (LEQUAL).
        if (ps_r_world_geom_stats >= 2) VK::VRS::GeomStatsBegin(cmd, CommandManager.GetCurrentFrame(), 4);   // mode 2: skinned only
        if (!noPreDraw) Skinned_RenderDepthPrepass(cmd, *ctx.viewProj, mvPre);
        // r_hud_prepass: the hands/weapon too, at their own near range -- the world under them then skips shading.
        // Not into a motion-attachment scope: the plain caster pipeline declares no colour target.
        if (!noPreDraw && !mvPre) Skinned_RenderHudDepthPrepass(cmd, ctx.extent);
        if (ps_r_world_geom_stats >= 2) VK::VRS::GeomStatsEnd(cmd, CommandManager.GetCurrentFrame(), 4);

        vkCmdEndRendering(cmd);

        // ---- PHASE 2 (r_hzb_prepass) --------------------------------------
        // Everything above is phase 1: the CPU statics, the GPU set that last
        // frame's pyramid kept, ALL the trees and ALL the NPCs. Only the GPU
        // static set is two-phased, and deliberately: it is the one with a GPU
        // cull to re-run, and the trees/NPCs it now sits behind make phase 1's
        // depth a much better occluder than the statics alone would be.
        //
        // Rebuild the pyramid from that partial depth, re-test only what phase 1
        // deferred (a dispatchIndirect over a few thousand entries, not another
        // scan of 830k), and draw the survivors into the same depth buffer with
        // LOAD. After this the prepass depth is complete and IDENTICAL to what
        // the single-phase path produces — which is why SSAO, VSM marking and the
        // gather below need to know nothing about any of it.
        if (twoPhase)
        {
            VK_CPU_PROBE("W/depth2");
            // The build transitions depth to SHADER_READ and back to
            // DEPTH_ATTACHMENT itself, and its own barrier orders the phase-1
            // writes before the reduce — no SceneAttachmentBarrier needed here.
            const int zMid = VK::Prof::ZoneBegin(cmd, "World/HZBmid");
            RImplementation.Details->BuildHZBForFrame(ctx, true /*force: build twice this frame*/);
            VK::Prof::ZoneEnd(cmd, zMid);
            NX_GPU_MARK(cmd, "w.hzbMid");

            const int zLate = VK::Prof::ZoneBegin(cmd, "World/CullLate");
            WorldGPU::CullLate(cmd, *ctx.viewProj, RV().vCameraPosition,
                               RImplementation.Details->HZBView(), RImplementation.Details->HZBSampler(),
                               RImplementation.Details->HZBGeneration(),
                               RImplementation.Details->HZBBuiltThisFrame());
            VK::Prof::ZoneEnd(cmd, zLate);
            NX_GPU_MARK(cmd, "w.cullLate");

            {
                // LOAD the motion attachment too: phase 1 already cleared it to the
                // sentinel and wrote the canopy into it — a second CLEAR here would
                // erase that and send every tree pixel back to reconstruction.
                // The LOAD is a READ of what phase 1 wrote, and the HZB build between
                // them only barriered the DEPTH image, so order the colour writes too.
                if (mvPre) SceneAttachmentBarrier(cmd);
                VK::RenderingBuilder rb(ctx.extent);
                if (mvPre) rb.Color(mvPreView, VK_ATTACHMENT_LOAD_OP_LOAD);
                rb.Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_LOAD).Begin(cmd);
            }
            vkCmdSetViewport(cmd, 0, 1, &pvp);
            vkCmdSetScissor(cmd, 0, 1, &psc);
            vkCmdSetDepthBias(cmd, 0.f, 0.f, 0.f);
            { VK_REC_SUB("W/DrawDepth B"); WorldGPU::DrawDepth(cmd, *ctx.viewProj, true /*displaceTerrain*/, WorldGPU::DrawSet::B, mvPre); }
            vkCmdEndRendering(cmd);
        }

        SceneAttachmentBarrier(cmd);   // order prepass depth writes before the color pass
        // The motion field is finished being written; hand it to the resolve as a
        // sampled image. (The resolve itself waits for the depth read window below.)
        MotionVec::EndPrepassTarget(cmd);
        if (ps_r_world_geom_stats < 2) VK::VRS::GeomStatsEnd(cmd, CommandManager.GetCurrentFrame(), 0);
        VK::Prof::ZoneEnd(cmd, zDepth);
        NX_GPU_MARK(cmd, "w.depth");

        // NOTE: Stage D clone depth is DELIBERATELY NOT drawn here. Clones are
        // distant backdrop; letting their (6+ km) depth into the scene buffer
        // before SSAO / VSM MarkPages / Hi-Z would pollute those home-only
        // screen-space passes (VSM would mark pages 6 km away and starve the home
        // pages → home surfaces fall back to "fully lit" = bright white patches).
        // The clone depth+color is drawn AFTER those consumers, just before the
        // home color pass — see the Compose block below.

        // NPC NORMAL G-buffer for GTAO: render the near-camera skinned casters into
        // the SSAO normal RT, depth-tested against the prepass depth (no depth write).
        // Gives GTAO real per-pixel normals where NPCs are visible → no depth-
        // derivative speckle on characters; statics/trees fall back to depth recon.
        // The RT is always CLEARed (defined for the GTAO sample) even if the draw is
        // gated off. ps_r_ssao_npc_normals (vk_console_min) — A/B toggle.
        // ⚠ ResourcesWanted, not Enabled: GTAO was this buffer's first reader but is
        // no longer its only one — the final gather takes its ray origins and its
        // cosine lobe from the same image. Gated on the AO switch, turning AO off
        // left the gather with no normals and killed GLOBAL ILLUMINATION silently.
        // See the note on SSAOPass::ResourcesWanted.
        if (SSAOPass::ResourcesWanted() && SSAOPass::GetNormalView() != VK_NULL_HANDLE)
        {
            const int zNrm = VK::Prof::ZoneBegin(cmd, "World/AOnormal");
            ImageBarrier(cmd, SSAOPass::GetNormalImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            // Colour CLEAR → a=0 everywhere = the GTAO fallback; depth LOADed to
            // test the prepass z (the pipeline has depthWrite off, hence DONT_CARE).
            VK::RenderingBuilder(ctx.extent)
                .ColorClear(SSAOPass::GetNormalView(), VkClearColorValue{ { 0.f, 0.f, 0.f, 0.f } })
                .Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE)
                .Begin(cmd);

            VkViewport nvp{ 0.f, (float)ctx.extent.height, (float)ctx.extent.width, -(float)ctx.extent.height, 0.f, 1.f };
            vkCmdSetViewport(cmd, 0, 1, &nvp);
            VkRect2D nsc{ {}, ctx.extent };
            vkCmdSetScissor(cmd, 0, 1, &nsc);

            // STATICS normals (r_ssao_static_normals): the level geometry the
            // prepass just drew, re-rastered with the vertex normal. Both halves
            // of the static set — the CPU queue AND the GPU-driven indirect path
            // — so the normal coverage matches the depth it tests against.
            // Terrain is skipped inside both (its prepass depth is tessellated
            // and snow-displaced; a flat re-raster would fail LEQUAL).
            if (ps_r_ssao_static_normals) {
                g_RenderQueue.FlushNormals(cmd, *ctx.viewProj);
                if (gpuWorld) { VK_REC_SUB("W/DrawNormals"); WorldGPU::DrawNormals(cmd, *ctx.viewProj, prepassSet); }
            }

            if (ps_r_ssao_npc_normals)
                Skinned_RenderNormalPrepass(cmd, *ctx.viewProj);

            vkCmdEndRendering(cmd);
            ImageBarrier(cmd, SSAOPass::GetNormalImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            VK::Prof::ZoneEnd(cmd, zNrm);
            NX_GPU_MARK(cmd, "w.aoNormal");
        }

        // --- SCENE-DEPTH READ WINDOW (framegraph seam) ---------------------
        // SSAO, VRS, VSM-mark and VSM-resolve all sample THIS prepass depth. Each
        // historically did its own ATTACHMENT->SHADER_READ->ATTACHMENT round-trip
        // (4 round-trips = 8 depth barriers that serialize the GPU). The ImageState
        // tracker coalesces them: with r_fg_coalesce the depth flips to SHADER_READ
        // once, every consumer runs, then it flips back once before the color pass.
        // VSM::RenderAtlas rasters into its OWN atlas depth (never scene depth), so
        // it rides happily inside the read window. r_fg_coalesce 0 = the exact old
        // per-consumer round-trips (A/B fallback). The read barrier now includes
        // COMPUTE (SSAO/VRS/VSM are compute dispatches) — stricter than the old auto
        // ImageBarrier's FRAGMENT-only derivation, closing a latent sync hole [#3].
        // The depth state is the frame-wide g_FrameGraph.Depth() (seeded to
        // DEPTH_ATTACHMENT in CRender::Begin; the prepass just wrote it in that same
        // layout) — routing through the graph so any later pass sees the true state.
        VK::ImageState& depthState = VK::g_FrameGraph.Depth();
        const bool fgCoalesce = (ps_r_fg_coalesce != 0);
        // The two states are defined once in vk_framegraph (the same pair the water,
        // light-cone and motion-vector passes now use); these keep the local names
        // the window below reads with. COMPUTE is in the read scope because SSAO/VRS/
        // VSM-mark are compute dispatches — the FRAGMENT-only default is for the
        // fullscreen/forward consumers outside this pass.
        auto depthToRead = [&]() {
            VK::SceneDepthToSampled(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                       | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        };
        auto depthToAttach = [&]() { VK::SceneDepthToAttachment(cmd); };

        // MOTION VECTOR RESOLVE (r_mv_prepass) — FIRST consumer of the depth read
        // window, and deliberately ahead of SSAO: the AO temporal history reprojects
        // through this field, and its whole defect was reading a field produced after
        // the world pass, i.e. one frame old. Sentinel texels (everything statics laid)
        // are reconstructed from this depth; texels the canopy wrote in the prepass keep
        // their own motion. No-op when the path is off.
        if (mvPre) {
            const int zMV = VK::Prof::ZoneBegin(cmd, "World/MVresolve");
            depthToRead();
            MotionVec::ResolveEarly(cmd, ctx.extent);
            if (!fgCoalesce) depthToAttach();
            VK::Prof::ZoneEnd(cmd, zMV);
            NX_GPU_MARK(cmd, "w.mv");
        }

        // GTAO from the prepass depth (R4 SSAO analog): render half-res AO + blur.
        // The color pass below samples the result via EnvLight binding 8 (ambient/hemi).
        const int zSSAO = VK::Prof::ZoneBegin(cmd, "World/SSAO");
        if (SSAOPass::Enabled()) {
            depthToRead();
            SSAOPass::Execute(cmd, ctx.extent);
            if (!fgCoalesce) depthToAttach();
        }
        VK::Prof::ZoneEnd(cmd, zSSAO);
        NX_GPU_MARK(cmd, "w.ssao");

        // FINAL GATHER (r_gather): one traced ray per half-res pixel, resolving the
        // ambient this pixel actually receives instead of interpolating it off the
        // probe lattice. It reads the SAME prepass depth and the SAME normal
        // G-buffer GTAO just used — hence this slot, right after SSAO — and the
        // colour pass below samples the result via EnvLight binding 37.
        // ⚠⚠ NO GATE OF ITS OWN HERE, AND THAT IS THE POINT. ReportGate asks every
        // condition the pass depends on — user intent, the BVH bubble, the probe
        // volume, the normal G-buffer that happens to belong to the SSAO pass — and
        // NAMES the one that is closed. Testing any of them out here instead would
        // skip the pass without entering it, so the pass could not report its own
        // failure: precisely how a dead final gather spent a whole session looking
        // like a merely weak one. It still returns before the two depth layout
        // transitions, so a switched-off feature costs nothing.
        if (!GatherPass::ReportGate(ctx.depthView, SSAOPass::GetNormalView())) {
            depthToRead();
            GatherPass::Execute(cmd, ctx.extent, ctx.depthView, SSAOPass::GetNormalView(),
                                VK::DeriveProjTerms(*ctx.viewProj), *ctx.viewProj);
            if (!fgCoalesce) depthToAttach();
        }
        // Motion vectors moved OUT of the World pass: the static depth-reconstruction
        // now runs in MotionVec::ExecuteDynamic (registered after LODs), where the
        // depth holds ALL opaque geometry — so trees (rigid → exact) and grass get
        // correct camera motion for free, not the background's. See vk_motionvec.cpp.

        // VRS: build this frame's depth-driven shading-rate image (compute reads the
        // prepass depth → coarser rate with distance). Needs depth SHADER_READ.
        if (VK::VRS::Wanted()) {
            const int zVRS = VK::Prof::ZoneBegin(cmd, "World/VRSbuild");
            depthToRead();
            const VK::ProjTerms pt = VK::DeriveProjTerms(*ctx.viewProj);
            VK::VRS::BuildFromDepth(cmd, CommandManager.GetCurrentFrame(), ctx.depthView,
                                    ctx.extent, pt.p43, pt.p33);
            if (!fgCoalesce) depthToAttach();
            VK::Prof::ZoneEnd(cmd, zVRS);
            NX_GPU_MARK(cmd, "w.vrs");
        }

        // VSM page marking (WIP, r_vsm): which sun-clipmap pages do visible pixels
        // need? Reads the prepass depth in SHADER_READ. Gated on Wanted() (the cvar,
        // no Init dependency); MarkPages lazily Inits + no-ops if not yet Ready.
        // NIGHT FREEZE: with the sun below the horizon it lights nothing, so the whole
        // sun-shadow update (mark + atlas raster + resolve, ~1.7 ms) is pure waste. Skip
        // it and keep the last daylit mask — receivers multiply it by sun_color≈0, so
        // there is no visible change. Resumes automatically at dawn (NightFrozen() clears).
        // nfs46 (r_sun_mask): the half-res sun mask for the R1 receivers (reads the prepass depth).
        if (VK::SunMask::Active()) {
            const int zSM = VK::Prof::ZoneBegin(cmd, "World/SunMask");
            depthToRead();
            VK::SunMask::Resolve(cmd, ctx.depthView, ctx.extent, *ctx.viewProj);
            if (!fgCoalesce) depthToAttach();
            VK::Prof::ZoneEnd(cmd, zSM);
            NX_GPU_MARK(cmd, "w.sunMask");
        }
        if (VK::VSM::Wanted() && !VK::VSM::NightFrozen() && !VK::VSM::LoadScreenFrozen()) {
            const int zVSM = VK::Prof::ZoneBegin(cmd, "World/VSMmark");
            depthToRead();
            VK::VSM::MarkPages(cmd, ctx.depthView, ctx.extent, *ctx.viewProj);
            if (!fgCoalesce) depthToAttach();
            VK::Prof::ZoneEnd(cmd, zVSM);
            // Rasterize the casters into the VSM atlas (own render pass, own atlas
            // depth — never touches scene depth, so it stays inside the read window).
            const int zVSMr = VK::Prof::ZoneBegin(cmd, "World/VSMrender");
            VK::VSM::RenderAtlas(cmd);
            VK::Prof::ZoneEnd(cmd, zVSMr);
            // Temporal resolve: sample the atlas per screen pixel + reproject history →
            // the screen-space sun-shadow mask the receivers read. Also reads the
            // prepass depth in SHADER_READ.
            const int zVSMres = VK::Prof::ZoneBegin(cmd, "World/VSMresolve");
            depthToRead();
            VK::VSM::ResolveMask(cmd, ctx.depthView, ctx.extent, *ctx.viewProj);
            if (!fgCoalesce) depthToAttach();
            VK::Prof::ZoneEnd(cmd, zVSMres);
        }

        // Close the depth-read window: if any consumer left scene depth in
        // SHADER_READ (the coalesced path defers the flip-back to here), restore
        // DEPTH_ATTACHMENT once for the Hi-Z build + color pass — its src waits on
        // the union of every reader above. Skipped when depth is already attachment
        // (non-coalesced path restored it per consumer; all-disabled never opened
        // the window) → default r_fg_coalesce 0 emits the SAME barriers as the
        // original hand-placed code, no extras.
        if (depthState.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            depthToAttach();
    }

    // r_hzb_cull (Phase A): build the Hi-Z pyramid from THIS frame's PREPASS depth,
    // then occlusion-cull the GPU static set into a 2nd indirect buffer drawn only
    // by the color pass below — meshes fully behind nearer geometry skip the heavy
    // forward shading. Outside any render pass; depth is DEPTH_ATTACHMENT here
    // (SSAO/VRS/VSM restored it), which BuildHZBForFrame expects. We SHARE the grass
    // pyramid: grass Render runs after Pass_World, so its BuildHZB no-ops (same-frame
    // stamp) and reuses this prepass-depth build. The depth prepass already drew the
    // full frustum set (it is the pyramid source), so this never over-culls.
    // ⚠ The BUILD must not hide behind the world's own cvar. The Trees pass runs
    // after this one and deliberately does NOT build the pyramid itself (it would
    // only ever have LAST frame's = a stale camera, so r_tree_hzb self-disables on
    // HZBBuiltThisFrame). With the build gated on r_hzb_cull, setting r_hzb_cull 0
    // to A/B the static cull silently switched the TREE occlusion off as well —
    // two features measured as one, the exact class of lying instrument this
    // renderer has been bitten by before. Build when EITHER consumer wants it;
    // cull the static colour set only under the static gate.
    // ⚠ With r_hzb_prepass on, the static colour cull is NOT run here: the colour
    // set is already the prepass set (EARLY u LATE), so a third full scan of the
    // meta array would buy a marginally tighter cut for the price this whole
    // change exists to remove. r_hzb_cull therefore has no effect while
    // r_hzb_prepass is on — stated here and in the console help rather than left
    // for someone to discover through a flat A/B.
    // The pyramid still gets rebuilt: it was last reduced from PHASE-1 depth
    // mid-prepass, and the consumers that run after this point (tree cull, grass
    // gen, lod_cull) want the finished scene.
    bool worldOccluded = false;
    const bool hzbWorld = gpuWorld && !twoPhase && ps_r_hzb_cull && WorldGPU::OcclusionReady();
    const bool hzbTrees = ps_r_tree_hzb && RImplementation.Trees && RImplementation.Trees->IsReady();
    const bool hzbSkel  = ps_r_dyn_occ >= 2;   // NxOcc's veto (CRender_Vulkan.cpp): an NPC seen through a fence stays drawn
    if (prepass && (hzbWorld || hzbTrees || twoPhase || hzbSkel)
        && RImplementation.Details && RImplementation.Details->HZBReady())
    {
        VK_CPU_PROBE("W/hzb");
        // Separate zones: the pyramid build is paid by whoever asked for it, the
        // cull only by the static set. Lumped into one number there was no way to
        // read the build's own cost — the first thing any optimisation of it needs.
        const int zBuild = VK::Prof::ZoneBegin(cmd, "World/HZBbuild");
        RImplementation.Details->BuildHZBForFrame(ctx, twoPhase /*force: already built mid-prepass*/);
        if (hzbSkel)
        {
            const CDetailManager& d = *RImplementation.Details;
            VK::HzbReadback::Capture(cmd, *ctx.viewProj, d.HZBImage(), d.HZBWidth(), d.HZBHeight(), d.HZBMipCount());
        }
        VK::Prof::ZoneEnd(cmd, zBuild);
        NX_GPU_MARK(cmd, "w.hzbBuild");

        if (hzbWorld) {
            const int zHZB = VK::Prof::ZoneBegin(cmd, "World/HZBcull");
            WorldGPU::CullColor(cmd, *ctx.viewProj, RV().vCameraPosition,
                                RImplementation.Details->HZBView(), RImplementation.Details->HZBSampler(),
                                RImplementation.Details->HZBGeneration());
            worldOccluded = true;
            VK::Prof::ZoneEnd(cmd, zHZB);
            NX_GPU_MARK(cmd, "w.hzbCull");
        }
    }

    // Which culled set the COLOUR pass draws. With two-phase it is the prepass set
    // itself — equality, so nothing can be written-but-unshaded (the black-
    // silhouette failure) and nothing shaded that has no depth (geometry punching
    // through). It is a slightly looser cut than CullColor's would be, because an
    // entry that last frame's pyramid kept is drawn even if this frame's would
    // drop it; with a depth prepass in front, occlusion buys the colour pass only
    // vertex work anyway (measured 20-06: World/Color unchanged with and without
    // it, early-Z had already killed the fragments), so that trade is the right
    // way round.
    const WorldGPU::DrawSet colorSet = twoPhase    ? WorldGPU::DrawSet::AB
                                     : worldOccluded ? WorldGPU::DrawSet::B
                                                     : WorldGPU::DrawSet::A;

    // Targets travel in FrameContext; both are already in their attachment layout
    // (Begin set them, ExecutePasses ordered prior passes). No transition here.

    VkRenderingFragmentShadingRateAttachmentInfoKHR sriAtt{ VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR };

    // Colour LOAD preserves the background clear. Depth: the prepass already laid
    // it down — LOAD it so early-Z can reject; without a prepass keep the old
    // CLEAR behaviour. STORE either way so Pass_Sky can depth-test against
    // world's z and only paint cleared pixels (z == 1.0).
    VK::RenderingBuilder rb(ctx.extent);
    rb.Color(ctx.colorView)
      .Depth(ctx.depthView, prepass ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR);
    // RAY RECONSTRUCTION GUIDES. Attached to the OPAQUE scope only — this is where
    // terrain, statics and NPCs are shaded, which is where the material data RR needs
    // actually exists. LOAD, never CLEAR: CRender::Begin already cleared them to the
    // RR defaults, and the overlay passes append to the same targets later.
    // No-op unless RRGuide::Ready(), so with r_dlss_rr 0 this scope is byte-identical
    // to what it was.
    VK::RRGuide::Attach(rb);
    // Attach the shading-rate image so distant tiles coarse-shade (built above).
    if (VK::VRS::Wanted() && VK::VRS::GetView() != VK_NULL_HANDLE) {
        sriAtt.imageView                      = VK::VRS::GetView();
        sriAtt.imageLayout                    = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        sriAtt.shadingRateAttachmentTexelSize = VK::VRS::TexelSize();
        rb.Next(&sriAtt);
    }
    // Stage D compose: clone chunks' DEPTH — drawn HERE (after SSAO/VSM/Hi-Z have
    // consumed the home-only depth), so clones never pollute those home passes.
    // All visible clones' depth is laid first so the color loop below gets correct
    // inter-clone occlusion (a near clone occludes a far clone). Clones are far, so
    // their depth never overwrites a nearer home wall (LEQUAL) — home is untouched.
    // Zone opened every frame (profiler matches zones by open order).
    {
        const int zCompZ = VK::Prof::ZoneBegin(cmd, "World/ComposeZ");
        const u32 nClones = (gpuWorld && prepass) ? Compose::CloneCount() : 0;
        CFrustum composeFr;
        if (nClones) { Fmatrix fm = *ctx.viewProj; composeFr.CreateFromMatrix(fm, FRUSTUM_P_LRTB | FRUSTUM_P_FAR); }
        u32 nDrawn = 0;
        for (u32 c = 0; c < nClones; ++c) {
            if (!Compose::ChunkInView(c, composeFr)) continue;
            ++nDrawn;
            Fmatrix cvp; Fvector camL;
            Compose::ChunkView(c, *ctx.viewProj, cvp, camL);
            WorldGPU::Cull(cmd, cvp, camL, RV().vCameraDirection);

            // depth LOAD — append to the home depth.
            VK::RenderingBuilder(ctx.extent).Depth(ctx.depthView).Begin(cmd);
            VkViewport cvpv{ 0.f, (float)ctx.extent.height, (float)ctx.extent.width, -(float)ctx.extent.height, 0.f, 1.f };
            vkCmdSetViewport(cmd, 0, 1, &cvpv);
            VkRect2D csc{ {}, ctx.extent };
            vkCmdSetScissor(cmd, 0, 1, &csc);
            vkCmdSetDepthBias(cmd, 0.f, 0.f, 0.f);
            { VK_REC_SUB("W/ComposeZ depth"); WorldGPU::DrawDepth(cmd, cvp, true /*displaceTerrain*/); }
            vkCmdEndRendering(cmd);
        }
        if (nDrawn) SceneAttachmentBarrier(cmd);   // order clone depth writes before the color pass reads
        if (nClones) {
            static u32 s_lastLog = 0;
            if (RV().dwTimeGlobal > s_lastLog + 3000) { s_lastLog = RV().dwTimeGlobal;
                Msg("[VK Compose] %u/%u clones in view", nDrawn, nClones); }
        }
        VK::Prof::ZoneEnd(cmd, zCompZ);
        NX_GPU_MARK(cmd, "w.composeZ");
    }

    // The clone depth loop above left the LAST clone's set in the frustum indirect
    // buffer (cmds1). When the color pass draws the frustum set (no Hi-Z cull this
    // frame), re-cull HOME first or its color would draw the clone's indirect set
    // with the home matrix. With worldOccluded the home color reads the untouched
    // Hi-Z set (cmds2) — no re-cull needed.
    if (prepass && gpuWorld && !worldOccluded && Compose::CloneCount())
        WorldGPU::Cull(cmd, *ctx.viewProj);

    VK_CPU_PROBE("W/color");   // runs to the end of the pass — closes the tiling against W/all
    const int zColor = VK::Prof::ZoneBegin(cmd, "World/Color");
    // VRS diag: FS-invocation count for THIS pass ([VK VRS] world-color FS
    // invocations). r_vrs_force 1 = stats only (rate stays 1x1) for a baseline.
    // r_fsinv_split REPLACES the single bracket with 4 sequential sub-queries
    // (only one pipeline-stats query may be active at a time).
    const bool fsSplit = ps_r_fsinv_split > 0;
    if (fsSplit)
        VK::VRS::SubStatsReset(cmd, CommandManager.GetCurrentFrame());   // reset must stay outside the rendering scope
    else if (VulkanHW.m_bVRSSupported && (ps_r_vrs > 0 || ps_r_vrs_force > 0))
        VK::VRS::StatsBegin(cmd, CommandManager.GetCurrentFrame());
    rb.Begin(cmd);
    // VRS: no vkCmdSetFragmentShadingRateKHR here — world pipelines carry a STATIC
    // {1x1, KEEP, REPLACE} FSR state (SRI attachment wins when bound). A dynamic
    // rate proved unusable: any bind of a non-FSR-dynamic pipeline invalidates it
    // (validation 18-07-2026), which is why r_vrs looked like a no-op for so long.

    // X-Ray builds D3D-style projection (Y-up clip space). Vulkan clip-space Y
    // is down → negate viewport height to flip the image. (VK_KHR_maintenance1
    // / Vulkan 1.1+ allows negative height.)
    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = (float)ctx.extent.height;
    vp.width    = (float)ctx.extent.width;
    vp.height   = -(float)ctx.extent.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ {}, ctx.extent };
    vkCmdSetScissor(cmd, 0, 1, &sc);
#ifdef __SWITCH__
    g_xr_nx_world_scissor = sc;
#endif

    // MVP + uvScale are constant across the whole pass: world-space verts
    // (model = identity), and every level vertex format we render uses
    // SHORT2 SSCALED TCs. Push once before the loop. (The tess block at
    // offsets 84..112 is owned by RenderQueue::Flush.) Stage flags must
    // match the layout's push range exactly — GetPushStages, not VS|FS.
    WorldPush push{};
    push.mvp        = *ctx.viewProj;
    push.uvScale[0] = 1.0f / 1024.0f;
    push.uvScale[1] = 1.0f / 1024.0f;
    push.alphaRef    = -1.0f;  // Flush patches FS-tail (alphaRef + detailScale) per material
    push.detailScale = 0.0f;
    vkCmdPushConstants(cmd, PipelineCache::GetLayout(), PipelineCache::GetPushStages(),
                       0, sizeof(WorldPush), &push);

    // Per-pass DIAG: counts visuals by type and stride. The 'unsupported'
    // bucket is what's still rendered as a dummy — once it's zero on a given
    // level we're done porting visual classes.
    static bool s_diag_done = false;
    if (!s_diag_done) {
        u32 cntTotal=0, cntFV=0, cntHier=0, cntProg=0, cntSkel=0, cntTree=0, cntLOD=0, cntPart=0, cntOtherType=0;
        u32 cntNoVB=0, cntStride12=0, cnt32=0, cnt36=0, cnt40=0, cnt44=0, cntOtherStride=0;
        for (IRenderVisual* iv : RImplementation.Visuals) {
            if (!iv) continue;
            cntTotal++;
            auto* rv = static_cast<vkRender_Visual*>(iv);
            switch (rv->Type) {
                case MT_HIERRARHY:           cntHier++; continue;
                case MT_SKELETON_ANIM:
                case MT_SKELETON_RIGID:      cntSkel++;  continue;
                case MT_TREE_ST:
                case MT_TREE_PM:             cntTree++;  continue;
                case MT_LOD:                 cntLOD++;   continue;
                case MT_PARTICLE_EFFECT:
                case MT_PARTICLE_GROUP:      cntPart++;  continue;
            }
            if (rv->Type == MT_NORMAL || rv->Type == MT_PROGRESSIVE) {
                if (rv->Type == MT_NORMAL)      cntFV++;
                else                            cntProg++;
                auto* fv = static_cast<vkFVisual*>(rv);
                if (!fv->m_mesh.p_rm_Vertices) { cntNoVB++; continue; }
                switch (fv->m_mesh.vStride) {
                    case 12: cntStride12++;   break;
                    case 32: cnt32++;         break;
                    case 36: cnt36++;         break;
                    case 40: cnt40++;         break;
                    case 44: cnt44++;         break;
                    default: cntOtherStride++;break;
                }
            } else cntOtherType++;
        }
        Msg("[VK World] DIAG types: total=%u FV=%u Prog=%u Hier=%u | Skel=%u Tree=%u LOD=%u Part=%u other=%u",
            cntTotal, cntFV, cntProg, cntHier, cntSkel, cntTree, cntLOD, cntPart, cntOtherType);
        Msg("[VK World] DIAG mesh:  noVB=%u s12=%u s32=%u s36=%u s40=%u s44=%u sOther=%u",
            cntNoVB, cntStride12, cnt32, cnt36, cnt40, cnt44, cntOtherStride);
        s_diag_done = true;
    }

    // Phase 3: Flush the statics queue collected (and sorted) above, before the
    // prepass. With the prepass depth already in place, early-Z rejects every
    // occluded fragment before the forward shader runs.
    const int zStatics = VK::Prof::ZoneBegin(cmd, "World/Statics");
    if (!fsSplit) VK::VRS::GeomStatsBegin(cmd, CommandManager.GetCurrentFrame(), 1);   // r_world_geom_stats slot 1
    // r_at_equal: AT statics ride the depth-EQUAL/no-write variant — valid only
    // because the prepass above (FlushDepth + DrawDepth) wrote their final depth.
    const bool atEq = prepass && ps_r_depth_prepass && ps_r_at_equal > 0;
    const bool zPre = prepass && ps_r_depth_prepass && ps_r_z_prepass > 0;
    const u32  fsFrame = CommandManager.GetCurrentFrame();
    g_RenderQueue.SetATEqual(atEq);
    g_RenderQueue.SetPrepassZ(zPre);
    if (fsSplit) VK::VRS::SubStatsBegin(cmd, fsFrame, 0);
    NX_GPU_MARK(cmd, "w.colorBegin");   // [nx gpu avg]: the colour scope's begin (clears/loads) and everything since the last mark
    if (nxWSkip != 2) { VK_CPU_PROBE("W/stat/flush");   // 18-09: Statics = 0.22-0.29 мс при ТРЁХ draw call — делим очередь CPU и косвенную отрисовку
      g_RenderQueue.Flush(ctx); }
    NX_GPU_MARK(cmd, "w.cpuFlush");     // the CPU statics queue (non-GPU leaves)
    if (fsSplit) VK::VRS::SubStatsEnd(cmd, fsFrame, 0);
    g_RenderQueue.SetATEqual(false);   // dynamics flush below is NOT in the prepass
    g_RenderQueue.SetPrepassZ(false);
    // GPU static set (drawn after the CPU flush so DrawColor's self-contained
    // pushes don't disturb Flush's push state). EnvLight set = set 1. worldOccluded
    // → draw the Hi-Z-culled set (CullColor ran above); else the frustum set.
    if (fsSplit) VK::VRS::SubOccBegin(cmd, fsFrame);   // samples-passed over the GPU statics draw
    if (gpuWorld && nxWSkip != 2) { VK_CPU_PROBE("W/stat/gpudraw"); VK_REC_SUB("W/DrawColor");
        WorldGPU::DrawColor(cmd, *ctx.viewProj, EnvLight::GetCurrentSet(), colorSet, atEq, zPre,
                                      fsSplit ? (int)fsFrame : -1);
    }
    else if (fsSplit) {   // keep all split queries begun+ended so the batch stays readable
        VK::VRS::SubStatsBegin(cmd, fsFrame, 1); VK::VRS::SubStatsEnd(cmd, fsFrame, 1);
        VK::VRS::SubStatsBegin(cmd, fsFrame, 2); VK::VRS::SubStatsEnd(cmd, fsFrame, 2);
    }
    if (fsSplit) VK::VRS::SubOccEnd(cmd, fsFrame);
    // Cluster-LOD debug overlay (r_cluster_debug): the DAG cut made visible —
    // clusters as flat colors (1) or wireframe (2), same indirect set as above.
    if (gpuWorld && ps_r_cluster_debug) WorldGPU::DrawDebug(cmd, *ctx.viewProj, ps_r_cluster_debug, colorSet);
    if (!fsSplit) VK::VRS::GeomStatsEnd(cmd, CommandManager.GetCurrentFrame(), 1);
    VK::Prof::ZoneEnd(cmd, zStatics);
    NX_GPU_MARK(cmd, "w.statics");

    // Dynamic (spawned) visuals — collected by CRender::add_Visual this frame.
    // Same render pass / depth as the statics; each carries its own world matrix,
    // so Flush pushes a per-item MVP (= xform * viewProj). vkFHierrarhyVisual /
    // CKinematics recurse into children inside Submit. Skinned leaves with no VB
    // (index-only stopgap) self-skip in Submit until the skinned path is ported.
    if (fsSplit) VK::VRS::SubStatsBegin(cmd, fsFrame, 3);
    // The water pass runs much later and needs the STATICS to build its lid map
    // (which surface lies directly over each patch of water — the one thing that
    // tells a puddle from the same sheet running under the floor). Grab them here,
    // while the queue still holds them and before the dynamic pass reuses it.
    VK::Water_CaptureLidStatics();
    if (!DynamicVisuals().empty() && nxWSkip != 3) {
        g_RenderQueue.Clear();
        for (const DynVisual& d : DynamicVisuals()) {
            if (d.vis) {
                g_RenderQueue.SetSubmitHemi(d.hemi);   // per-object sky-ambient gate
                d.vis->Submit(g_RenderQueue, d.xform, 0.0f);
            }
        }
        g_RenderQueue.SetSubmitHemi(1.0f);             // reset for the next (static) flush
        g_RenderQueue.SortByKey();
        // No tessellation for dynamics: the world VS emits MODEL-space
        // vWorldPos, so the TES camera-distance factor would be garbage
        // under a non-identity model xform.
        g_RenderQueue.SetAllowTess(false);
        g_RenderQueue.Flush(ctx);
        g_RenderQueue.SetAllowTess(true);
    }
    if (fsSplit) VK::VRS::SubStatsEnd(cmd, fsFrame, 3);

    // Skinned dynamic leaves (NPCs / weapons / hands): GPU skinning, own pipeline +
    // bone SSBO. Same render pass (color + depth) as the statics above.
    const int zSkin = VK::Prof::ZoneBegin(cmd, "World/Skinned");
    if (fsSplit) VK::VRS::SubStatsBegin(cmd, fsFrame, 4);
#ifdef __SWITCH__
    // nx_diag.txt mode 1 (nfs2): every NPC/hands draw is still sent, but through a 1x1 scissor -- the GPU frame drops by
    // their fragment cost; what stays is their vertex/setup floor. (Nothing after this pass in the frame needs their pixels.)
    const bool nxDiagScissor = (g_xr_nx_diag == 1);
    if (nxDiagScissor) { const VkRect2D one{ {0, 0}, {1u, 1u} }; vkCmdSetScissor(cmd, 0, 1, &one); }
#endif
#ifdef __SWITCH__
    if (nxWSkip != 4 && g_xr_nx_diag != 4 && g_xr_nx_diag != 6) { VK_REC_SUB("W/Pass_Skinned"); Pass_Skinned(ctx); }   // nfs3: diag 4 = no skinned
#else
    if (nxWSkip != 4) { VK_REC_SUB("W/Pass_Skinned"); Pass_Skinned(ctx); }
#endif
#ifdef __SWITCH__
    if (nxDiagScissor) vkCmdSetScissor(cmd, 0, 1, &sc);
#endif
    if (fsSplit) VK::VRS::SubStatsEnd(cmd, fsFrame, 4);
    VK::Prof::ZoneEnd(cmd, zSkin);
    NX_GPU_MARK(cmd, "w.skinned");

    vkCmdEndRendering(cmd);
    VK::VRS::StatsEnd(cmd, CommandManager.GetCurrentFrame());   // no-op if StatsBegin didn't run
    VK::Prof::ZoneEnd(cmd, zColor);
    NX_GPU_MARK(cmd, "w.color");

    // Inc 1: a few frames after load — once the dry/summer world + terrain uber-FS
    // variants have been lazily built — pre-create their WET/SNOW weather flips so the
    // first rain/snowfall doesn't stall compiling pipelines mid-gameplay. Spread ONE
    // create per frame (cold uber-FS compile is ~300ms — doing all 21 at once froze a
    // whole frame for ~7.8s on the first run).
    {
        static u32  s_prewarmCd   = 8;
        static bool s_prewarmDone = false;
        if (!s_prewarmDone) {
            if (s_prewarmCd) --s_prewarmCd;                                          // let the dry set build first
            else if (!PipelineCache::PrewarmWeatherVariants(1)) s_prewarmDone = true; // 1 pipeline/frame
        }
        // r_prewarm_async: the worker hands its pipelines back here.
        PipelineCache::PrewarmDrain();
    }

    // Stage D compose: clone chunks' COLOR. Own cull → LOAD color+depth pass per
    // clone (cull can't run inside a rendering scope; the single indirect buffer
    // is reused sequentially — home already consumed its Hi-Z set above). Opaque
    // draws are z-buffered so ordering vs sky/trees/grass is irrelevant; the sky
    // pass only paints z==1 pixels and the clones' depth landed in the prepass.
    // Frustum-only cull (no Hi-Z: the pyramid slot was consumed by home's
    // CullColor; clone over-draw is acceptable in this slice). Zone opened every
    // frame (profiler matches zones by open order).
    {
        const int zCompC = VK::Prof::ZoneBegin(cmd, "World/ComposeC");
        const u32 nClones = (gpuWorld && prepass) ? Compose::CloneCount() : 0;
        CFrustum composeFr;
        if (nClones) {
            Fmatrix fm = *ctx.viewProj;
            composeFr.CreateFromMatrix(fm, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
        }
        for (u32 c = 0; c < nClones; ++c) {
            if (!Compose::ChunkInView(c, composeFr)) continue;
            Fmatrix cvp; Fvector camL;
            Compose::ChunkView(c, *ctx.viewProj, cvp, camL);
            WorldGPU::Cull(cmd, cvp, camL, RV().vCameraDirection);

            rb.SetDepthLoad(VK_ATTACHMENT_LOAD_OP_LOAD);  // never re-clear over the home scene
            rb.Begin(cmd);                                // same attachments (colour LOAD / depth LOAD / VRS static state)
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            { VK_REC_SUB("W/ComposeC color"); WorldGPU::DrawColor(cmd, cvp, EnvLight::GetCurrentSet(), WorldGPU::DrawSet::A /*frustum set*/); }
            vkCmdEndRendering(cmd);
        }
        VK::Prof::ZoneEnd(cmd, zCompC);
        NX_GPU_MARK(cmd, "w.composeC");
    }
    // No exit transition: the image stays in COLOR_ATTACHMENT for the next pass.
    // ExecutePasses inserts the inter-pass barrier; End brings it to PRESENT.
}

// PHASE B (editor-on-Vulkan, chunk 2): draw the add_Visual'd model(s) with no level.
// Pass_World owns the dynamics draw normally, but returns early with no level
// (!b_loaded), so nothing add_Visual'd this frame is rendered. Replicate JUST the
// color-pass dynamics block here (rigid RenderQueue + Pass_Skinned), lit by the
// editor-driven environment. Runs into the HDR SceneColor that EditorClear seeded
// (grey + depth=1.0), so the lit model composites on the grey backdrop through the
// normal tonemap. Registered only in -vk_editor (guarded by VKEditor::Active()).
void Pass_EditorDynamics(FrameContext& ctx)
{
    if (ctx.cmd == VK_NULL_HANDLE || !ctx.viewProj)   return;
    if (DynamicVisuals().empty() && HudVisuals().empty()) return;
    if (PipelineCache::GetLayout() == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = ctx.cmd;

    // Depth prepass + GTAO. Both live inside Pass_World, which returns on its first line
    // without a level, so the editor had no ambient occlusion at all — nothing darkened
    // contacts, and objects read as pasted onto surfaces rather than resting on them.
    // Everything the two need — extent, depth target, geometry — is already here.
    const bool prepass = PipelineCache::GetDepthLayout() != VK_NULL_HANDLE
                      && Swapchain.m_DepthFormat == VK_FORMAT_D32_SFLOAT;

    // Ahead of EnvLight::Update, which binds the AO view at binding 8: a target created
    // after that bind would be sampled while still UNDEFINED.
    if (prepass)
        SSAOPass::EnsureTargets(ctx.extent);

    // Per-frame sun/hemi/ambient UBO (set 1). Pass_World normally calls this; it is
    // skipped with no level, so drive it here. Runs BEFORE the render pass begins —
    // it may record uploads/barriers, which must be outside dynamic rendering — so it
    // can carry the sky-probe refresh too.
    EnvLight::Update(CommandManager.GetCurrentFrame(), cmd);

    // Volumetric fog (froxel inject + integrate). Its only other call site is inside
    // Pass_World, so with no level the volume was never integrated and held
    // transmittance 0 — which is why the tonemap composite had to be force-disabled
    // in editor. Driving it here restores aerial perspective / distance haze, the
    // single biggest "why doesn't this look like the game" term outdoors. Compute,
    // needs no scene depth (froxel world pos is analytic), so it belongs here,
    // before the prepass and outside any render pass — same placement as Pass_World.
    if (VK::Vol::Wanted() && VK::Vol::Ready()) {
        const VK::ProjTerms vpt = VK::DeriveProjTerms(*ctx.viewProj);
        // Editor path stays fully on the graphics queue; the bakes it needs are the
        // same ones Pass_World records (they moved out of Execute — see there).
        VK::Vol::RecordGraphicsBakes(cmd);
        // No smoke particles in an editor scene — the host pushes static models only.
        VK::Vol::Execute(cmd, vpt, CommandManager.GetCurrentFrame(), nullptr, 0);
    }

    // Pass_World owns this at the top of every frame, and it is unreachable here, so
    // the glass list was never reset in the editor: panes from deleted or replaced
    // host objects kept drawing until some other visual displaced them.
    g_RenderQueue.ClearGlass();
    g_RenderQueue.ClearWater();

    // Instanced host scene (vk_instance_gpu): compute-cull against the camera into
    // the TGT_COLOR region. MUST precede BeginRendering — it is compute. Also does
    // the lazy (re)build, so ColorReady() below reflects THIS frame.
    InstanceGPU::CullColor(cmd, *ctx.viewProj);
    // Whatever the instanced path owns is skipped here — that CPU walk (a Submit
    // per object plus a sort, every frame) was the dominant cost of this pass, and
    // leaving an owned visual in would draw it twice. Ownership is whole-visual, so
    // the handful carrying late-translucent leaves still comes through and reaches
    // the blended pass intact. Skeletons go to Pass_Skinned, which self-collects.
    const bool instColor = InstanceGPU::ColorReady();

    // Collect once — the same queue feeds the depth prepass and the colour pass.
    g_RenderQueue.Clear();
    if (!DynamicVisuals().empty()) {
        for (const DynVisual& d : DynamicVisuals()) {
            if (d.vis && !(instColor && InstanceGPU::OwnsVisual(d.vis))) {
                g_RenderQueue.SetSubmitHemi(d.hemi);
                d.vis->Submit(g_RenderQueue, d.xform, 0.0f);
            }
        }
        g_RenderQueue.SetSubmitHemi(1.0f);
        g_RenderQueue.SortByKey();
    }

    if (prepass && (g_RenderQueue.Size() || instColor))
    {
        const int zDepth = VK::Prof::ZoneBegin(cmd, "EditorDepth");

        // CLEAR + STORE — the colour pass LOADs it.
        VK::RenderingBuilder(ctx.extent).Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_CLEAR).Begin(cmd);

        VkViewport pvp{ 0.f, (float)ctx.extent.height, (float)ctx.extent.width, -(float)ctx.extent.height, 0.f, 1.f };
        vkCmdSetViewport(cmd, 0, 1, &pvp);
        VkRect2D psc{ {}, ctx.extent };
        vkCmdSetScissor(cmd, 0, 1, &psc);
        vkCmdSetDepthBias(cmd, 0.f, 0.f, 0.f);

        g_RenderQueue.FlushDepth(cmd, *ctx.viewProj);
        InstanceGPU::DrawColorDepth(cmd, *ctx.viewProj);   // indirect, opaque host instances
        Skinned_RenderDepthPrepass(cmd, *ctx.viewProj);

        vkCmdEndRendering(cmd);
        VK::Prof::ZoneEnd(cmd, zDepth);
        NX_GPU_MARK(cmd, "w.depth");

        if (SSAOPass::Enabled())
        {
            VK::SceneDepthToSampled(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                       | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
            SSAOPass::Execute(cmd, ctx.extent);
            VK::SceneDepthToAttachment(cmd);
        }
    }

    // No own profiler zone — ExecutePasses already wraps this pass in a zone named
    // "EditorDynamics" (a nested same-name zone printed a confusing double entry).
    BeginOverlayRendering(cmd, ctx, VK_ATTACHMENT_STORE_OP_STORE);

    // Constant across the pass: MVP = viewProj (per-item model xform applied by
    // Flush), unit UV scale, no global alpha ref. Same push Pass_World sets.
    WorldPush push{};
    push.mvp         = *ctx.viewProj;
    push.uvScale[0]  = 1.0f / 1024.0f;
    push.uvScale[1]  = 1.0f / 1024.0f;
    push.alphaRef    = -1.0f;
    push.detailScale = 0.0f;
    vkCmdPushConstants(cmd, PipelineCache::GetLayout(), PipelineCache::GetPushStages(),
                       0, sizeof(WorldPush), &push);

    // Rigid dynamics — the queue was collected above, before the depth prepass.
    if (g_RenderQueue.Size()) {
        g_RenderQueue.SetAllowTess(false);   // dynamics use model-space vWorldPos → no tess
        g_RenderQueue.Flush(ctx);
        g_RenderQueue.SetAllowTess(true);
    }

    // Instanced host scene: one indirect draw per material group instead of one
    // per object. Alpha-tested groups ride along — the world FS applies alphaRef
    // from the material tail, so colour needs no opaque/AT split.
    InstanceGPU::DrawColor(cmd, *ctx.viewProj, EnvLight::GetCurrentSet());

    // Skinned dynamics (skeleton devices / NPCs) — self-collects from DynamicVisuals().
    Pass_Skinned(ctx);

    vkCmdEndRendering(cmd);
}

// Late translucent pass: GLASS panes (WorldMaterial::isGlass) accumulated by
// RenderQueue::Push across the statics/dynamics flushes draw HERE — after the
// whole opaque world (GPU statics, trees, grass, LODs, sky). They blend without
// z-write, so drawing them inside the normal flush let everything rendered
// after them overwrite the blended pixels: level windows looked missing, prop
// panes opaque. Registered between "Shafts" and "Wallmarks" (CRender_Vulkan).
#ifdef __SWITCH__
extern "C" int g_xr_nx_skip_glass;
#endif
void Pass_WorldGlass(FrameContext& ctx)
{
    if (ctx.cmd == VK_NULL_HANDLE) return;
#ifdef __SWITCH__
    if (g_xr_nx_skip_glass) return;
#endif
    if (!g_RenderQueue.HasGlass()) return;
    VkCommandBuffer cmd = ctx.cmd;
    const int z = VK::Prof::ZoneBegin(cmd, "World/Glass");
    // LOAD colour+depth, depth test vs the finished opaque scene, no depth write
    // (the glass pipelines are the wmark variants — no z-write by construction).
    BeginOverlayRendering(cmd, ctx, VK_ATTACHMENT_STORE_OP_DONT_CARE);
    g_RenderQueue.FlushGlass(ctx);
    vkCmdEndRendering(cmd);
    VK::Prof::ZoneEnd(cmd, z);
}

}  // namespace VK

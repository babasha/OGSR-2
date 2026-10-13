// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — CTreeManager Session B: GPU-driven tree render.
//
// Per-frame flow (mirrors the monolith CShadowManager g-buffer tree path,
// adapted to the OGSR forward single-pass renderer):
//   1. Fill the view-frustum UBO from ctx.viewProj.
//   2. Clear per-group draw counts; barrier transfer→compute.
//   3. Per group: dispatch tree_cull.comp — frustum-tests each tree sphere and
//      appends a VkDrawIndexedIndirectCommand (firstInstance = global tree idx)
//      into the group's section, bumping the group's draw count.
//   4. Barrier compute→draw-indirect.
//   5. Begin dynamic-rendering pass (color + depth LOAD, single-layout
//      convention — no swapchain transitions here).
//   6. Per group: bind pipeline variant (tcOffset 24/28) + diffuse + VB/IB,
//      vkCmdDrawIndexedIndirectCount over the group's section.
//
// Build() (Session A) already uploaded m_TreeMetadataBuffer / m_TreeTransforms
// and allocated m_TreeIndirectBuffer / m_TreeDrawCountBuffer + per-texture
// descriptor sets. This file only adds the pipelines + the per-frame path.

#include "stdafx.h"
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_TreeManager.h"
#include "vk_color_space.h"   // ColorSpace::LinearizeRGB — env colours are authored sRGB
#include "vk_Visual.h"           // vkFTreeVisual (m_mesh / vis.box) — impostor silhouette bake
#include "vk_pass_context.h"
#include "vk_swapchain.h"
#include "vk_scene_color.h"       // HDR scene target format
#include "vk_motionvec.h"         // VK::MotionVec::Format — RG16F MV target (tree wind-sway MV)
#include "vk_texture.h"
#include "vk_shaders.h"          // g_ShaderManager
#include "vk_pipeline_cache.h"   // PipelineCache::GetCacheObject
#include "vk_compute_util.h"     // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_gfx_pipeline.h"     // GfxPipelineBuilder
#include "vk_descriptors.h"      // VK::DescriptorWriter
#include "vk_vsm.h"              // VSM::GetRMaskHandle — receiver mask for the dyn bins (r_vsm_rmask)
#include "vk_profiler.h"         // VK::Prof::ZoneBegin — Bins/Meshlet + Bins/VoxCull sub-zones (r_profiler 2)
#include "vk_barriers.h"         // VK::ImageBarrier / BufferBarrier — impostor bake + indirect
#include "vk_env_light.h"        // VK::EnvLight — set 2 (sun_vp + sun shadow map)
#include "vk_shadow.h"           // ShadowMap::SphereVisible — caster culling (RenderDepth)
#include "vk_LODManager.h"       // VK::kImposterMinDist — floor for the r_tree_dist cut
#include "vk_DetailManager.h"    // shared Hi-Z pyramid (HZBView/HZBSampler/HZBBuiltThisFrame)
#include "CRender_Vulkan.h"      // RImplementation.Details — owner of that pyramid
#include "vk_cull.h"             // VK::ExtractFrustumPlanes (shared with DetailManager)
#include "HW_Vulkan.h"
#include "vk_vrs.h"            // VK::VRS::CmdSetPipelineRate — 2x2 coarse crown-shadow shading
#include "../../xr_3da/IGame_Persistent.h" // ES()
#include <bit>                    // std::popcount — diag tree-bitset readbacks
#include "../../xr_3da/Environment.h"      // CEnvDescriptorMixer (sun_color/hemi_color)

// GLOBAL scope (an extern inside namespace VK would mangle as VK::* → LNK2001).
extern float ps_r_sun_boost;   // r_sun_boost — global sun multiplier (see vk_env_light)
extern float ps_r_wind_tree_bend;   // r_wind_tree_bend  — trunk sway intensity
extern float ps_r_wind_tree_anim;   // r_wind_tree_anim  — branch flutter speed
extern float ps_r_wind_tree_trunk;  // r_wind_tree_trunk — trunk anim speed
extern float ps_r_wind_tree_flutter;// r_wind_tree_flutter — crown flutter amplitude
extern float ps_r_wind_tree_crown;  // r_wind_tree_crown — flutter fade-in height
extern int   ps_r_vsm_tree_wind;      // r_vsm_tree_wind — near/far hybrid: near trees cast into the DYNAMIC atlas with live wind
extern float ps_r_vsm_tree_wind_dist; // r_vsm_tree_wind_dist — near set radius (m)
extern int   ps_r_vsm_tree_vrs;       // r_vsm_tree_vrs — 2x2 coarse shading on the dyn crown-shadow raster (A/B)
extern int   ps_r_vsm_debug;          // r_vsm_debug — hybrid stats log
extern int   ps_r_profiler;           // r_profiler — 2+ enables the Bins/* per-dispatch sub-zones
extern int   ps_r_vsm_meshlet;        // r_vsm_meshlet — per-page meshlet cull of tree casters (Phase B)
extern int   ps_r_vsm_hzb;            // r_vsm_hzb — shadow-HZB occlusion cull of tree casters (static pass)
extern float ps_r_vsm_hzb_margin;     // r_vsm_hzb_margin — occluder depth slack (stale-sun safety)
extern int   ps_r_vsm_rmask;          // r_vsm_rmask — receiver mask: dyn bins sub-page-cull vs sampled 8×8 cells
extern int   ps_r_vsm_tree_impostor;       // r_vsm_tree_impostor — near crown mesh → baked sun-facing billboard in the dyn atlas
extern float ps_r_vsm_tree_impostor_scale; // r_vsm_tree_impostor_scale — billboard size vs crown sphere (shadow footprint tuning)
extern int   ps_r_vsm_tree_hull;           // r_vsm_tree_hull — caster LOD: near trees beyond _dist cast from a baked opaque crown hull
extern float ps_r_vsm_tree_hull_dist;      // r_vsm_tree_hull_dist — crown-mesh tier radius (m, light-space lateral, same metric as the near set)
extern int   ps_r_vsm_tree_hull_debug;     // r_vsm_tree_hull_debug — pink lobe overlay over hull-tier trees (forward view)
extern int   ps_r_vsm_tree_hull_lod;       // r_vsm_tree_hull_lod — LIVE coarseness bias for the graded voxel view
extern float ps_r_vsm_tree_hull_vox_px;    // r_vsm_tree_hull_vox_px — target PROJECTED voxel size (pixels) for the LOD cut
extern float ps_r_vsm_tree_hull_vox_fade;  // r_vsm_tree_hull_vox_fade — dithered LOD crossfade band (fraction of handover dist)
extern float ps_r_vsm_tree_hull_vox_far;   // r_vsm_tree_hull_vox_far — extra target px per 100 m (far cubes read chunkier)
extern float ps_r_vsm_tree_hull_vox_stex;   // r_vsm_tree_hull_vox_stex — SHADOW voxel LOD: page texels per voxel edge (UE shadow-view metric)
extern float ps_r_vsm_tree_hull_vox_sfloor; // r_vsm_tree_hull_vox_sfloor — SHADOW-only safety floor on the voxel edge (m)
extern int   ps_r_vsm_tree_hull_vox_cap;    // r_vsm_tree_hull_vox_cap — SHADOW-only max bricks per tree slice
extern int   ps_r_vsm_tree_hull_vox_wpo;    // r_vsm_tree_hull_vox_wpo — UE WPODisableDistance analog: rigid voxel-tier trees live in the static cache
extern int   ps_r_vsm_tree_hull_vox_static; // r_vsm_tree_hull_vox_static — static-tier proxy: 0 = merged shell (default), 1 = bricks
extern int   ps_r_vsm_tree_hull_vox_cull;   // r_vsm_tree_hull_vox_cull — stage-2 per-page brick compaction (vsm_vox_cull.comp)
extern float ps_r_vsm_base;                 // r_vsm_base — VSM clipmap level-0 extent (m); texel(L) = base·2^L / 4096
extern int   ps_r_vsm_tree_hull_vox;       // r_vsm_tree_hull_vox — voxel bake resolution (0 = shell/lobes only)
extern float ps_r_vsm_tree_hull_band;      // r_vsm_tree_hull_band — crown↔voxel shadow crossfade band (fraction of _dist)
extern float ps_r_tree_dist;               // r_tree_dist — forward draw distance for billboard-backed trees (0 = off)
extern int   ps_r_tree_hzb;                // r_tree_hzb — Hi-Z occlusion cull of the forward tree set
extern float ps_r_tree_shade_dist;         // r_tree_shade_dist — range past which tree.frag drops its subtle shading terms
extern float ps_r_tree_vox_dist;           // r_tree_vox_dist — range past which the baked cube cloud replaces the crown mesh
extern float ps_r_tree_alpharef;           // r_tree_alpharef — opaque-card CEILING probe (<0 = each pass keeps its shipped 200/255)
extern int   ps_r_tree_zwrite;             // r_tree_zwrite — 0 = colour pass tests depth but does not write it (early-Z vs the leaf discard)
extern float ps_r_tree_sky_boost;         // nfs19 r_tree_sky_boost
extern int   ps_r_tree_prepass;           // r_tree_prepass -- the crown crossfade needs it off (see the colour push)
extern int   ps_r_tree_swi;               // nfs21
extern float ps_r_tree_swi_lod;           // nfs21
extern float ps_r__GLOD_ssa_start, ps_r__GLOD_ssa_end;   // vk_console_min.cpp (r__ssa_glod_start/end)
extern int   ps_r_tree_flat;               // r_tree_flat — 1 = flat-FS probe (alpha test + constant colour, no shading)
extern int   ps_r_tree_perm;               // r_tree_perm — 1 = crown compiled per world state (lean variants); 0 = A/B the full ubershader
extern int   ps_r_tree_terms;              // r_tree_terms — force ONE variant mask (-1 = off, let the world state decide)
extern int   ps_r_tree_gi_inst;            // r_tree_gi_inst — 1 = probe field sampled once per TREE (compute) instead of per vertex
extern int   ps_r_tree_gather;             // r_tree_gather — 0 = crown FS without the screen-space gather tap (compiled out)
extern int   ps_r_tree_stats;              // r_tree_stats — tree ledger: 0 off, 1 cull+hardware counters, 2 + per-pass GPU zones

#ifdef __SWITCH__
extern "C" int g_xr_nx_diag;    // nfs45 device.cpp (also declared lower down)
#endif
extern int ps_r_tree_r1;        // r_tree_r1 (vk_console_min.cpp): 1 = R1 crown (tree_r1.vert/frag, light per vertex) when the world state allows
extern int ps_r_tree_r1_dyn;    // r_tree_r1_dyn: the R1 crown lights the dynamic lights per vertex (else they force tree.frag)
extern int ps_r_tree_r1_wet;    // r_tree_r1_wet: the R1 crown also in the rain (darkened per vertex; else the wet state forces tree.frag)
extern int ps_r_tree_sky_push;  // r_tree_sky_push (vk_console_min.cpp): 1 = crown FS tree_skypush.frag.spv (sky term from the push), read at pipeline build

namespace VK
{

// Single source of truth for the cut distance, shared by the colour cull and the
// camera depth prepass. They MUST agree: if the prepass keeps a tree the colour
// pass drops, that tree writes depth with nothing shading it — black silhouettes
// (the bug the frustum margin in tree_cull.comp already guards against). Reading
// the cvar from two places invites exactly that drift, so both go through here.
// nfs21: the R1-style LOD fields of the cull push -- the size-scaled cut and R4's sliding-window thresholds
// (g_fSCREEN = W*H*(90/fov)^2*(EPS_S + r_tree_swi_lod), r_ssaGLOD_x = (r__ssa_glod_x / 3)^2 / g_fSCREEN).
static void TreeLodPush(TreeCullPush& p)
{
    // nfs23: the GROUP-centre cut is the double-draw/hole fix and is always on; the sign says whether the size LOD
    // scales it (> 0: k = flags bits 24..31, |value| = band) or not (< 0: k = 1, band 1 -- the billboard's own start).
    p.size_lod = TreeLodSizeOn() ? TreeLodBand() : -1.f;
    p.glod_start = p.glod_end = 0.f;
    if (ps_r_tree_swi)
    {
        const float fov = (RV().fFOV > 1.f) ? RV().fFOV : 67.f;
        const float scr = float(_max(RV().dwWidth, 1u)) * float(_max(RV().dwHeight, 1u)) * _sqr(90.f / fov) * (EPS_S + ps_r_tree_swi_lod);
        p.glod_start = _sqr(ps_r__GLOD_ssa_start / 3.f) / scr;
        p.glod_end   = _sqr(ps_r__GLOD_ssa_end / 3.f) / scr;
    }
}

// nfs48: the sliding-window thresholds for a SHADOW cull, independent of r_tree_swi (the colour switch) -- k scales
// them (the crown counts as k times smaller). Nothing else of TreeLodPush: size_lod would delete casters.
static void TreeShadowLodPush(TreeCullPush& p, float k)
{
    const float fov = (RV().fFOV > 1.f) ? RV().fFOV : 67.f;
    const float scr = float(_max(RV().dwWidth, 1u)) * float(_max(RV().dwHeight, 1u)) * _sqr(90.f / fov) * (EPS_S + ps_r_tree_swi_lod);
    p.glod_start = _sqr(ps_r__GLOD_ssa_start / 3.f) / scr * k;
    p.glod_end   = _sqr(ps_r__GLOD_ssa_end / 3.f) / scr * k;
}

float TreeFlodCutDist()
{
    if (ps_r_tree_dist <= 0.0f) return 0.0f;            // cut disabled
    // Never cut CLOSER than where the imposter starts drawing, whatever the cvar
    // says — inside that range the mesh is the ONLY thing representing the tree,
    // so an over-eager r_tree_dist would delete trees outright instead of
    // de-duplicating them. Cutting later than the imposter is merely less saving.
    return _max(ps_r_tree_dist, ImposterMinDist());
}

// Same single-source-of-truth contract for the VOXEL rung (r_tree_vox_dist): the
// colour cull (tree_cull.comp), the camera depth prepass (RenderDepth) and the cube
// draw that has to COVER what those two dropped all read the range through here.
//
// Clamped ABOVE by the FLOD cut, not below: past that range CLODManager already draws
// a billboard for the whole container, so cubes out there would be a third copy of the
// same crown rather than a cheaper one. With the FLOD cut off (r_tree_dist 0) the mesh
// runs to infinity and so may the cubes — nothing further out is replacing them.
float TreeVoxCutDist()
{
    if (ps_r_tree_vox_dist <= 0.0f) return 0.0f;        // tier disabled
    // Nothing to hand over TO — keep the mesh. Asking the manager rather than
    // trusting the cvar is what makes the prepass, the colour cull and the cube draw
    // agree on a level whose cloud never got baked or uploaded.
    if (!RImplementation.Trees || !RImplementation.Trees->VoxTierReady()) return 0.0f;
    const float flod = TreeFlodCutDist();
    return (flod > 0.0f) ? _min(ps_r_tree_vox_dist, flod) : ps_r_tree_vox_dist;
}

// r_tree_alpharef — the opaque-card CEILING probe. See the note on the declaration in
// vk_TreeManager.h for what it is for and why the picture is wrong while it is on.
// One accessor for the same reason the two cut distances have one: the probe is only a
// ceiling if EVERY raster pass that draws a leaf card moves together. A pass left on
// 200/255 keeps its own overdraw and quietly caps the measurement.
float TreeAlphaRef(float shipped)
{
    return (ps_r_tree_alpharef >= 0.0f) ? ps_r_tree_alpharef : shipped;
}

// Shorthands for the set-layout type lists handed to VK::MakeSetLayout — this
// file declares ten sets and the lists read as tables of bindings.
namespace {
    constexpr VkDescriptorType kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    constexpr VkDescriptorType kUBO  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constexpr VkDescriptorType kTex  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

bool CTreeManager::IsReady() const
{
    return m_bBuilt
        && m_CullPipeline != VK_NULL_HANDLE
        && (m_GfxPipeline24 != VK_NULL_HANDLE || m_GfxPipeline28 != VK_NULL_HANDLE)
        && m_TreeIndirectBuffer != nullptr
        && m_TreeDrawCountBuffer != nullptr
        && m_XformDescSet != VK_NULL_HANDLE
        && !m_Groups.empty();
}

// Everything the crown cube tier needs, in ONE place. The cut and the replacement must
// live or die together: a frame that drops crowns from the mesh but cannot draw their
// cubes renders a forest with holes in it, so every consumer of r_tree_vox_dist — the
// colour cull, the camera depth prepass, the draw — routes through here.
bool CTreeManager::VoxTierReady() const
{
    return m_VoxLodSpecies != 0
        && m_VoxFwdPipe != VK_NULL_HANDLE
        && m_VoxVB         && m_VoxVB->GetHandle()         != VK_NULL_HANDLE
        && m_VoxDrawCmds   && m_VoxDrawCmds->GetHandle()   != VK_NULL_HANDLE
        && m_VoxDrawParams && m_VoxDrawParams->GetHandle() != VK_NULL_HANDLE;
}

// ============================================================================
// Frustum UBO — host-visible, rewritten each frame in Render().
// ============================================================================
void CTreeManager::CreateFrustumUBO()
{
    m_FrustumUBO = xr_new<CVulkanBuffer>();
    m_FrustumUBO->Create(sizeof(TreeFrustumUBO),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
}

// ============================================================================
// Cull compute pipeline — descriptor {meta SSBO, frustum UBO, indirect SSBO,
// count SSBO}, 20 B push. Loads tree_cull.comp.spv.
// ============================================================================
void CTreeManager::CreateCullPipeline()
{
    if (!g_ShaderManager) { Msg("![VK Trees] g_ShaderManager null — cull pipeline disabled"); return; }
    if (!m_TreeMetadataBuffer || !m_FrustumUBO || !m_TreeIndirectBuffer || !m_TreeDrawCountBuffer) return;

    // 0=meta(SSBO) 1=frustum(UBO, legacy) 2=indirect(SSBO) 3=count(SSBO) 4=HZB(tex)
    // 5=voxel LOD table 6=cube draw cmds 7=cube draw params — the forward crown cube
    // tier is selected by THIS shader on purpose: it then shares one frustum, one HZB
    // pyramid and one distance with the mesh cull it is handing over from, so the two
    // sets can never disagree about which crown belongs to which representation.
    // 8 = the cull ledger (r_tree_stats). Declared unconditionally: a set layout is
    // baked at load and the cvar is live, so making the binding conditional would mean
    // rebuilding the pipeline to turn an instrument on. It costs 192 bytes and the
    // shader never touches it while stats_on is 0.
    // nfs21: + 9 = per-tree sliding-window table base, 10 = the windows (tree_cull_v2)
    m_CullDescLayout = VK::MakeSetLayout({ kSSBO, kUBO, kSSBO, kSSBO, kTex, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                         VK_SHADER_STAGE_COMPUTE_BIT, "Trees.Cull");
    if (m_CullDescLayout == VK_NULL_HANDLE) return;

    m_CullDescPool = VK::MakeDescriptorPool({ kSSBO, kUBO, kSSBO, kSSBO, kTex, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                            1, "Trees.Cull");
    if (m_CullDescPool == VK_NULL_HANDLE) return;
    if (!VK::AllocSets(m_CullDescPool, m_CullDescLayout, 1, &m_CullDescSet, "Trees.Cull")) return;

    // ⚠ Bindings 5-7 must point at a VALID buffer even when the tier is off: a
    // declared-but-unbound descriptor is UB the moment the shader is dispatched, no
    // matter that the branch reading it is never taken (the same rule the HZB dummy
    // below follows). With no cube buffers, alias them onto the metadata buffer —
    // the shader's vox_cut/vox_all gate keeps it from ever reading them.
    VkBuffer inert = m_TreeMetadataBuffer->GetHandle();
    VkBuffer lodT  = (m_VoxLodTable   && m_VoxLodTable->GetHandle())   ? m_VoxLodTable->GetHandle()   : inert;
    VkBuffer cmdB  = (m_VoxDrawCmds   && m_VoxDrawCmds->GetHandle())   ? m_VoxDrawCmds->GetHandle()   : inert;
    VkBuffer parB  = (m_VoxDrawParams && m_VoxDrawParams->GetHandle()) ? m_VoxDrawParams->GetHandle() : inert;

    // Cull ledger (binding 8) + its host mirror. gpuOnly for the device side: the
    // shader hammers it with atomics, and a HOST_VISIBLE allocation would put those
    // atomics across PCIe — an instrument that changes what it measures.
    {
        const VkDeviceSize sz = (VkDeviceSize)kTreeStatCount * sizeof(u32);
        m_CullStats = xr_new<CVulkanBuffer>();
        m_CullStats->Create(sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                              | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true /*gpuOnly*/);
        m_CullStatsRB = xr_new<CVulkanBuffer>();
        m_CullStatsRB->Create(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        m_CullStatsPtr = (u32*)m_CullStatsRB->Map();
        if (m_CullStatsPtr) memset(m_CullStatsPtr, 0, (size_t)sz);
    }
    VkBuffer statB = (m_CullStats && m_CullStats->GetHandle()) ? m_CullStats->GetHandle() : inert;

    VK::DescriptorWriter(m_CullDescSet)
        .StorageBuffer(0, m_TreeMetadataBuffer->GetHandle())
        .UniformBuffer(1, m_FrustumUBO->GetHandle())
        .StorageBuffer(2, m_TreeIndirectBuffer->GetHandle())
        .StorageBuffer(3, m_TreeDrawCountBuffer->GetHandle())
        .StorageBuffer(5, lodT)
        .StorageBuffer(6, cmdB)
        .StorageBuffer(7, parB)
        .StorageBuffer(8, statB)
        .StorageBuffer(9, (m_SwiInstBuffer && m_SwiInstBuffer->GetHandle()) ? m_SwiInstBuffer->GetHandle() : inert)
        .StorageBuffer(10, (m_SwiWinBuffer && m_SwiWinBuffer->GetHandle()) ? m_SwiWinBuffer->GetHandle() : inert)
        .Flush();
    // Binding 4 (HZB) is written per frame in Render() — the pyramid is created
    // and can be RECREATED on resize by CDetailManager, so a write here would go
    // stale. Until the first write it stays unbound, hence hzb_on gates on the
    // view being non-null rather than on the cvar alone.

    // Pipeline layout: 1 set + push = frustum planes (6×vec4 = 96 B) + 5 u32 (20 B).
    // The frustum used to live in a single-buffered UBO (binding 1) but that raced
    // the in-flight frames during rotation (the next frame's CPU write clobbered this
    // frame's planes before the GPU cull read them) → the colour cull tested a rotated
    // frustum vs the depth prepass → black tree silhouettes at screen edges. Push
    // constants are recorded per dispatch, so they can't alias across frames.
    // TreeCullPush = 208 B: over the 128 B guaranteed minimum, under the 256 B
    // every desktop GPU exposes (this renderer already ships a 240 B graphics
    // push). Check it rather than let vkCreatePipelineLayout fail obscurely.
    if (sizeof(TreeCullPush) > VulkanHW.Caps.maxPushConstantsSize) {
        Msg("![VK Trees] TreeCullPush %u B exceeds device maxPushConstantsSize %u B — cull pipeline disabled",
            (u32)sizeof(TreeCullPush), VulkanHW.Caps.maxPushConstantsSize);
        return;
    }
    m_CullPipelineLayout = VK::MakePipelineLayout({ m_CullDescLayout }, sizeof(TreeCullPush));
    if (m_CullPipelineLayout == VK_NULL_HANDLE) return;

    // nfs21/23: + group-centre cut, size LOD and sliding windows (bindings 9/10, 256 B push) -- one canonical .spv.
    m_CullPipeline = VK::CreateComputePipeline("tree_cull.comp.spv", m_CullPipelineLayout, "Trees.Cull");
    if (m_CullPipeline == VK_NULL_HANDLE) return;
    Msg("[VK Trees] Cull pipeline OK");
}

// ============================================================================
// r_tree_gi_inst — THE PROBE FIELD, ONCE PER TREE INSTEAD OF ONCE PER VERTEX.
//
// vk_pipestats is what pointed here. Printing the crown pipelines side by side:
//   crown SHIPPING              VS Register Count=56  Binary Size=20224  FS inputs=16
//   crown TERMS 0x7DF (gi off)  VS Register Count=32  Binary Size= 5376  FS inputs=13
// Clearing the gi bit does not merely drop a fragment term — the driver sees the
// FS stop consuming vGI.rgb and deletes ddgiFieldWeighted from the VERTEX shader,
// four fifths of that shader's code. THAT is the 0.45 ms the per-term sweep found,
// and it explains why removing the screen-space gather tap (r_tree_gather 0) moved
// the zone by 0.03 ms: the tap was never the cost.
//
// The vertex is simply the wrong RATE. ddgiSampleIrradiance walks eight probes at
// two atlas taps each per cascade; a canopy draws on the order of a million
// vertices. Probe spacing is metres and a crown is metres, so the answer barely
// varies across one tree. This pass samples it twice per tree (crown bottom and
// top, so tree.vert keeps its vertical gradient) and only for trees inside the
// draw distance — ~10^3 lookups instead of ~10^6.
//
// Set 1 is EnvLight's own set, the same one the graphics path binds at set 2; the
// shader defines ENV_SET 1 so the shared includes land on it. That is also why
// EnvLight's DDGI bindings gained COMPUTE visibility (vk_env_light.cpp).
// ============================================================================
void CTreeManager::CreateGIPass()
{
    if (!m_TreeMetadataBuffer || !m_TreeGIBuffer || !m_TreeGIBuffer->GetHandle()) return;
    VkDescriptorSetLayout envLayout = VK::EnvLight::GetSetLayout();
    if (envLayout == VK_NULL_HANDLE) { Msg("![VK TreeGI] EnvLight layout not ready — r_tree_gi_inst disabled"); return; }

    constexpr VkDescriptorType kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    m_GIDescLayout = VK::MakeSetLayout({ kSSBO, kSSBO }, VK_SHADER_STAGE_COMPUTE_BIT, "Trees.GI");
    m_GIDescPool   = VK::MakeDescriptorPool({ kSSBO, kSSBO }, 1, "Trees.GI");
    if (!m_GIDescLayout || !m_GIDescPool) return;
    if (!VK::AllocSets(m_GIDescPool, m_GIDescLayout, 1, &m_GIDescSet, "Trees.GI")) return;

    VK::DescriptorWriter(m_GIDescSet)
        .StorageBuffer(0, m_TreeMetadataBuffer->GetHandle())
        .StorageBuffer(1, m_TreeGIBuffer->GetHandle())
        .Flush();

    m_GIPipelineLayout = VK::MakePipelineLayout({ m_GIDescLayout, envLayout }, sizeof(TreeGIPush));
    if (m_GIPipelineLayout == VK_NULL_HANDLE) return;
    m_GIPipeline = VK::CreateComputePipeline("tree_gi.comp.spv", m_GIPipelineLayout, "Trees.GI");
    if (m_GIPipeline == VK_NULL_HANDLE) { Msg("![VK TreeGI] tree_gi.comp.spv missing — r_tree_gi_inst disabled"); return; }
    Msg("[VK TreeGI] per-tree field pass OK");
}

// Runs once per frame, before the crown draws. Skipped entirely when the cvar is
// off — the shipping path samples the field in the vertex shader and this buffer
// is then dead weight, not stale data anyone reads.
void CTreeManager::DispatchTreeGI(VkCommandBuffer cmd)
{
    if (ps_r_tree_gi_inst == 0 || m_GIPipeline == VK_NULL_HANDLE || m_TotalCount == 0) return;
    VkDescriptorSet envSet = VK::EnvLight::GetCurrentSet();
    if (envSet == VK_NULL_HANDLE) return;

    const int zGI = (ps_r_profiler >= 2) ? VK::Prof::ZoneBegin(cmd, "Trees/GI") : -1;

    TreeGIPush push{};
    push.count = m_TotalCount;
    // Same window the cull uses, plus a margin: a tree whose field is missing draws
    // BLACK, so the set that gets a field must be a superset of the set that draws.
    push.maxDist = (ps_r_tree_dist > 0.0f ? ps_r_tree_dist : 250.0f) + 32.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_GIPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_GIPipelineLayout, 0, 1, &m_GIDescSet, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_GIPipelineLayout, 1, 1, &envSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_GIPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (m_TotalCount + 63u) / 64u, 1, 1);

    // COMPUTE writes -> VERTEX reads. Without this the crown reads last frame's
    // field at best and garbage at worst, and the failure mode (slightly wrong
    // ambient) is exactly the kind that looks like "the approximation is bad"
    // rather than like a missing barrier.
    VkMemoryBarrier mb{};
    mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);

    if (zGI >= 0) VK::Prof::ZoneEnd(cmd, zGI);
}

// ============================================================================
// Graphics set 0 — transforms SSBO (read by tree.vert).
// ============================================================================
void CTreeManager::CreateXformDescriptor()
{
    VK::Vram::Scope _vram_scope("Trees");
    if (!m_TreeTransformsBuffer) return;

    // SSFX wind flow map (s_waves) — sampled in tree.vert for the crown flutter.
    // Load it + a linear/repeat sampler here so the wind branch has its texture.
    if (m_WaveSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        vkCreateSampler(VulkanHW.m_Device, &sci, nullptr, &m_WaveSampler);
    }
    if (m_WaveTex == nullptr) {
        string_path full;
        FS.update_path(full, "$game_textures$", "fx\\wind_wave.dds");
        if (FS.exist(full)) {
            auto* tex = xr_new<VK::CVulkanTexture>();
            if (tex->LoadDDS(full, /*applyBCSwizzle*/ false)) { m_WaveTex = tex; Msg("[VK Tree] SSFX wind flow map loaded"); }
            else { Msg("![VK Tree] LoadDDS failed for fx\\wind_wave.dds — tree flutter flat"); xr_delete(tex); }
        } else {
            Msg("![VK Tree] fx\\wind_wave.dds not found — tree flutter flat");
        }
    }

    // Set 0: binding 0 = transforms SSBO (vertex), binding 1 = s_waves (vertex).
    VkDescriptorSetLayoutBinding b[6]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    // 2 = per-tree crossfade input (group centre + k, m_TreeLodCBuffer), read by tree.vert. Once the visible-tree
    // bitset of tree.frag (its atomic cost the canopy early-Z), hence FRAGMENT too.
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // Forward crown cube tier (tree_vox.vert): 3 = the baked voxel cloud, 4 = the
    // per-draw payload tree_cull.comp produced for it. Both ride set 0 so the tier
    // needs no pipeline layout of its own — it draws inside the tree colour pass with
    // the same TreeGfxPush (viewProj, sun/hemi, wind) the crown mesh uses.
    b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    // 5 = the per-tree probe field (r_tree_gi_inst). Declared unconditionally: a
    // binding no shader reads costs nothing, and making the LAYOUT depend on a cvar
    // would mean the pipeline layout differs between runs of the same build.
    b[5].binding = 5; b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 6; lci.pBindings = b;
    vkCreateDescriptorSetLayout(VulkanHW.m_Device, &lci, nullptr, &m_XformDescLayout);

    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
    vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &m_XformDescPool);

    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = m_XformDescPool; dai.descriptorSetCount = 1;
    dai.pSetLayouts = &m_XformDescLayout;
    vkAllocateDescriptorSets(VulkanHW.m_Device, &dai, &m_XformDescSet);

    VK::DescriptorWriter dw(m_XformDescSet);
    dw.StorageBuffer(0, m_TreeTransformsBuffer->GetHandle())
      // Binding 2: the crown crossfade input (group centre + k per tree). Falls back to the transforms SSBO if the
      // upload failed -- a declared descriptor must resolve to a real buffer; the push then keeps lodBand at 0.
      .StorageBuffer(2, (m_TreeLodCBuffer && m_TreeLodCBuffer->GetHandle()) ? m_TreeLodCBuffer->GetHandle()
                                                                              : m_TreeTransformsBuffer->GetHandle());
    // Cube tier (bindings 3/4). Same rule as the cull set: a declared descriptor must
    // always resolve to a real buffer, so with the tier off they alias the transforms
    // SSBO — the tier's pipeline is never bound in that case, and nothing reads them.
    {
        VkBuffer inert = m_TreeTransformsBuffer->GetHandle();
        dw.StorageBuffer(3, (m_VoxVB && m_VoxVB->GetHandle()) ? m_VoxVB->GetHandle() : inert)
          .StorageBuffer(4, (m_VoxDrawParams && m_VoxDrawParams->GetHandle())
                            ? m_VoxDrawParams->GetHandle() : inert);
    }
    // s_waves view — falling back to the SSBO-only case is unsafe (the shader
    // samples it), so bind the flow map; if it failed to load the tree still
    // draws (flat wind) and we simply skip the write rather than bind a null view.
    if (m_WaveTex)
        dw.ImageSampler(1, m_WaveTex->GetView(), m_WaveSampler);
    // Per-tree probe field (binding 5). Created here rather than in CreateGIPass so
    // this one descriptor write covers it: a declared binding must always resolve to
    // a real buffer even when r_tree_gi_inst is 0 and nothing reads it.
    {
        VK::Vram::Scope _vram_gi("Trees");
        const VkDeviceSize bytes = (VkDeviceSize)m_TotalCount * 3 * 16;   // 3 x vec4 per tree
        m_TreeGIBuffer = xr_new<CVulkanBuffer>();
        m_TreeGIBuffer->Create(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);   // Create() returns void
        if (m_TreeGIBuffer->GetHandle() != VK_NULL_HANDLE) {
            // ZEROED AT BIRTH. w == 0 is the documented "the field has nothing to
            // say here" value, and every reader already falls back to the analytic
            // sky fill on it — so a tree whose entry was never written comes out
            // lit like the field is off, not lit by whatever was in that memory.
            xr_vector<u8> zero((size_t)bytes, 0u);
            m_TreeGIBuffer->Upload(zero.data(), bytes);
        }
        if (m_TreeGIBuffer->GetHandle() == VK_NULL_HANDLE) {
            xr_delete(m_TreeGIBuffer);
            Msg("![VK TreeGI] per-tree field buffer (%.1f MB) failed — r_tree_gi_inst unavailable",
                bytes / (1024.0 * 1024.0));
        } else {
            Msg("[VK TreeGI] per-tree field buffer: %.1f MB for %u trees", bytes / (1024.0 * 1024.0), m_TotalCount);
        }
    }
    dw.StorageBuffer(5, m_TreeGIBuffer && m_TreeGIBuffer->GetHandle()
                        ? m_TreeGIBuffer->GetHandle() : m_TreeTransformsBuffer->GetHandle());
    dw.Flush();
}

// ============================================================================
// Graphics pipelines — tree.vert/frag, two variants by UV byte offset.
// Set 0 = transforms (m_XformDescLayout), Set 1 = diffuse (m_TexDescLayout).
// ============================================================================
void CTreeManager::CreateGfxPipelines()
{
    if (!g_ShaderManager) { Msg("![VK Trees] g_ShaderManager null — gfx pipeline disabled"); return; }
    if (m_XformDescLayout == VK_NULL_HANDLE || m_TexDescLayout == VK_NULL_HANDLE) return;

    // set0 = per-tree transforms (SSBO), set1 = per-group diffuse,
    // set2 = shared env lighting (sun_vp + sun shadow map — vk_env_light).
    VkDescriptorSetLayout setLayouts[3] = { m_XformDescLayout, m_TexDescLayout, VK::EnvLight::GetSetLayout() };
    if (setLayouts[2] == VK_NULL_HANDLE) { Msg("![VK Trees] EnvLight layout not ready"); return; }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0; pcr.size = sizeof(TreeGfxPush);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 3; plci.pSetLayouts = setLayouts;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &m_GfxPipelineLayout);

    VkShaderModule vs = g_ShaderManager->Load("tree.vert.spv");
    // nfs18: r_tree_sky_push 1 (Switch preset) = tree_skypush.frag.spv (tree.frag -DTREE_SKY_PUSH: the up-facing sky
    // term is one CPU value per frame in pc.vHemiColor instead of skyAmbientUp() per pixel). Read here, at level load:
    // the per-world-state crown variants are all built from this one module. Missing twin = the full tree.frag.
    VkShaderModule fs = ps_r_tree_sky_push ? g_ShaderManager->Load("tree_skypush.frag.spv") : VK_NULL_HANDLE;
    if (fs == VK_NULL_HANDLE) fs = g_ShaderManager->Load("tree.frag.spv");
    m_CrownVS = vs; m_CrownFS = fs;   // the variant selector picks modules, never names
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        Msg("![VK Trees] tree.{vert,frag}.spv load failed"); return;
    }

    // Vertex input for every tree pipeline below: stride=32, FLOAT3 pos @ 0,
    // SHORT2 SSCALED UV @ tcOffset.
    auto createVariant = [&](u32 tcOffset, VkPipeline& out) -> bool
    {
        out = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
            .Vert(vs).Frag(fs)
            .Binding(0, 32)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
            .Cull(VK_CULL_MODE_NONE)          // trees: double-sided leaves
            .Depth(true, true)
            .Color(VK::SceneColor::Format())
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Trees gfx (tcOff=%u)", tcOffset);
        return out != VK_NULL_HANDLE;
    };

    bool ok24 = createVariant(24, m_GfxPipeline24);
    bool ok28 = createVariant(28, m_GfxPipeline28);

    // ---- r_tree_zwrite 0 twins: depth TEST but no depth WRITE ------------------
    // ⭐⭐⭐ THE SAME SHAPE THE STATICS PATH ALREADY PAID FOR. From r_z_prepass in
    // vk_console_min.cpp: "the world uber-FS statically contains `discard`, and
    // discard + z-write ON disables early-Z entirely — the whole frustum's depth
    // complexity was being shaded (10.8M FS inv vs ~3.5M visible)". tree.frag has a
    // `discard` (the leaf alpha test) and this pipeline writes depth, so the tree pass
    // has been in exactly that configuration all along — and it is the biggest zone in
    // the frame (7.9 of 18.2 ms, ~45% counting its shadow/MV twins). A canopy is the
    // worst possible case for it: layered alpha-tested cards, i.e. high depth complexity
    // with almost nothing opaque to stop the shading.
    //
    // Writing depth here is redundant anyway: trees are drawn into the camera depth
    // PREPASS (vk_pass_world.cpp), with the same VP, alpha ref and wind, so the depth
    // is already final before this pass begins.
    // ⚠ One asymmetry to keep in mind: the colour set is deliberately a SUPERSET of the
    // prepass set (frustum margin, skipped near plane, radius-vs-centre in the distance
    // cut). Those few edge trees have no prepass depth, so with writes off they no
    // longer lay any down for LATER passes (grass, water, particles) to test against.
    // That is the trade this cvar exists to measure before it is taken.
    auto createVariantNoZW = [&](u32 tcOffset, VkPipeline& out) -> bool
    {
        out = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
            .Vert(vs).Frag(fs)
            .Binding(0, 32)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
            .Cull(VK_CULL_MODE_NONE)
            .Depth(true, false)               // test LEQUAL, NO write -> early-Z survives the discard
            .Color(VK::SceneColor::Format())
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Trees gfx no-zwrite (tcOff=%u)", tcOffset);
        return out != VK_NULL_HANDLE;
    };
    createVariantNoZW(24, m_GfxPipeline24NoZW);
    createVariantNoZW(28, m_GfxPipeline28NoZW);

    // ---- r_tree_flat probe twins: the SAME state as the default pair, with the FS
    // swapped for tree_flat.frag.spv (identical alpha test, constant colour, nothing
    // else). Exists to answer the one question the 26-08 sweeps left open: Trees/Mesh
    // does not respond to zwrite, alphaRef, or fragment count — so is the cost the
    // ubershader's shading work, or the raster+alpha floor no shading architecture
    // can touch? Missing .spv just disables the probe, never the trees.
    if (VkShaderModule ffs = g_ShaderManager->Load("tree_flat.frag.spv")) {
        auto createVariantFlat = [&](u32 tcOffset, VkPipeline& out)
        {
            out = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
                .Vert(vs).Frag(ffs)
                .Binding(0, 32)
                .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
                .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
                .Cull(VK_CULL_MODE_NONE)
                .Depth(true, true)
                .Color(VK::SceneColor::Format())
                .DepthTarget(Swapchain.m_DepthFormat)
                .Build("Trees gfx flat probe (tcOff=%u)", tcOffset);
        };
        createVariantFlat(24, m_GfxPipeline24Flat);
        createVariantFlat(28, m_GfxPipeline28Flat);
    } else {
        Msg("![VK Trees] tree_flat.frag.spv missing — r_tree_flat probe disabled");
    }
    // nfs31 R1 crown pair (r_tree_r1): same state as the default pair, VS/FS from tree.vert -DTREE_R1 (tree_r1.vert.spv:
    // + nfs43 per-vertex live sun shadow, + nfs47 cascade z cut) / tree_r1.frag. Built only when r_tree_r1 is on at
    // level load (a PC never pays the two pipelines); missing .spv = the full crown shader stays.
    if (ps_r_tree_r1) {
        VkShaderModule rvs = g_ShaderManager->Load("tree_r1.vert.spv");
        VkShaderModule rfs = rvs ? g_ShaderManager->Load("tree_r1.frag.spv") : VK_NULL_HANDLE;
        if (rvs && rfs) {
            auto createVariantR1 = [&](u32 tcOffset, VkPipeline& out)
            {
                out = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
                    .Vert(rvs).Frag(rfs)
                    .Binding(0, 32)
                    .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
                    .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
                    .Cull(VK_CULL_MODE_NONE)
                    .Depth(true, true)
                    .Color(VK::SceneColor::Format())
                    .DepthTarget(Swapchain.m_DepthFormat)
                    .Build("Trees gfx R1 crown (tcOff=%u)", tcOffset);
            };
            createVariantR1(24, m_GfxPipeline24R1);
            createVariantR1(28, m_GfxPipeline28R1);
        }
        Msg("[VK Trees] R1 crown pipelines: %s", (m_GfxPipeline24R1 && m_GfxPipeline28R1) ? "OK" : "missing (full crown shader)");
    }

    // ---- r_tree_gather 0 twins: the crown FS with the screen-space gather tap
    // compiled out of ddgiAmbientFromField. Built eagerly (unlike the term masks)
    // because this is a rendering option someone may want to leave on, not a
    // measurement mode — and two pipelines at load is not a load-time cost worth
    // deferring behind a first-use hitch.
    if (VkShaderModule ngf = g_ShaderManager->Load("tree_nogather.frag.spv")) {
        m_CrownFSNoGath = ngf;
        auto createVariantNoGather = [&](u32 tcOffset, VkPipeline& out)
        {
            out = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
                .Vert(vs).Frag(ngf)
                .Binding(0, 32)
                .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
                .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
                .Cull(VK_CULL_MODE_NONE)
                .Depth(true, true)
                .Color(VK::SceneColor::Format())
                .DepthTarget(Swapchain.m_DepthFormat)
                .Build("Trees gfx no-gather (tcOff=%u)", tcOffset);
        };
        createVariantNoGather(24, m_GfxPipeline24NoGather);
        createVariantNoGather(28, m_GfxPipeline28NoGather);
        Msg("[VK Trees] no-gather crown pipelines: 24=%s 28=%s",
            m_GfxPipeline24NoGather ? "ok" : "FAIL", m_GfxPipeline28NoGather ? "ok" : "FAIL");
    } else {
        Msg("![VK Trees] tree_nogather.frag.spv missing — r_tree_gather 0 falls back to the shipping crown");
    }

    // ---- r_tree_gi_inst 1 twins: same FS, VERTEX shader swapped for the one that
    // reads the per-tree field buffer instead of walking the probe atlases. The VS
    // is what this whole step is about, so the pair differs in nothing else.
    if (VkShaderModule givs = g_ShaderManager->Load("tree_giinst.vert.spv")) {
        m_CrownVSGI = givs;
        auto createVariantGIInst = [&](u32 tcOffset, VkPipeline& out)
        {
            out = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
                .Vert(givs).Frag(fs)
                .Binding(0, 32)
                .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
                .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
                .Cull(VK_CULL_MODE_NONE)
                .Depth(true, true)
                .Color(VK::SceneColor::Format())
                .DepthTarget(Swapchain.m_DepthFormat)
                .Build("Trees gfx per-tree GI (tcOff=%u)", tcOffset);
        };
        createVariantGIInst(24, m_GfxPipeline24GIInst);
        createVariantGIInst(28, m_GfxPipeline28GIInst);
        Msg("[VK TreeGI] per-tree-GI crown pipelines: 24=%s 28=%s",
            m_GfxPipeline24GIInst ? "ok" : "FAIL", m_GfxPipeline28GIInst ? "ok" : "FAIL");
    } else {
        Msg("![VK TreeGI] tree_giinst.vert.spv missing — r_tree_gi_inst 1 falls back to the shipping crown");
    }

    // The world-state VARIANTS are not built here: BuildPermPipes drains one pair per
    // frame instead. Enumerating them at load would mean guessing weather and lighting
    // the level has not reported yet, and paying a dozen pipeline compiles out of the
    // load time this project has spent months shortening — for masks most of which
    // that level will never be in.
    Msg("[VK Trees] Gfx pipeline (stride=32, tcOff 24=%s 28=%s)",
        ok24 ? "ok" : "FAIL", ok28 ? "ok" : "FAIL");

    // FORWARD CROWN CUBE TIER (tree_vox.{vert,frag}, r_tree_vox_dist) — the middle rung
    // of the LOD ladder, so it belongs HERE next to the mesh it replaces, not in the
    // VSM caster's resource block where the cloud's first consumer happened to live.
    // ⚠ That placement was a real trap, not tidiness: CreateHullResources bails when the
    // VSM caster path is not up, and the pipeline going missing while the CULL still cut
    // crowns at r_tree_vox_dist would have deleted every distant crown under r_vsm 0.
    //
    // NO VERTEX INPUT: the cloud is read as an SSBO (set 0 b3) indexed by
    // gl_VertexIndex/18 and the per-draw payload from set 0 b4 by gl_InstanceIndex —
    // which is what lets the whole tier be ONE vkCmdDrawIndirectCount. A per-instance
    // vertex stream would have pinned each draw to a CPU-known range per tree.
    // OPAQUE with depth-write: the cubes ARE the crown out there and they are not in
    // the depth prepass, so this pass is where their depth comes from.
    if (m_VoxLodSpecies) {
        VkShaderModule vvs = g_ShaderManager->Load("tree_vox.vert.spv");
        VkShaderModule vfs = g_ShaderManager->Load("tree_vox.frag.spv");
        if (vvs && vfs) {
            m_VoxFwdPipe = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
                .Vert(vvs).Frag(vfs)
                .Cull(VK_CULL_MODE_NONE)
                .Depth(true, true)
                .Color(VK::SceneColor::Format())
                .DepthTarget(Swapchain.m_DepthFormat)
                .Build("Trees crown cube tier");
        } else {
            Msg("![VK Trees] tree_vox.{vert,frag}.spv missing — crown cube tier disabled");
        }
    }

    // Shadow caster variants: tree_depth.{vert,frag} (alpha-tested, depth-only
    // into the sun map's D32), same pipeline layout (push offset 0..72 used).
    VkShaderModule dvs = g_ShaderManager->Load("tree_depth.vert.spv");
    VkShaderModule dfs = g_ShaderManager->Load("tree_depth.frag.spv");
    if (dvs == VK_NULL_HANDLE || dfs == VK_NULL_HANDLE) {
        Msg("![VK Trees] tree_depth.{vert,frag}.spv missing — tree shadow casting disabled");
        return;
    }
    auto createDepthVariant = [&](u32 tcOffset, VkPipeline& out)
    {
        out = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
            .Vert(dvs).Frag(dfs)
            .Binding(0, 32)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
            .Cull(VK_CULL_MODE_NONE)          // double-sided leaves
            .DynamicDepthBias()               // caller sets the sun bias
            .Depth(true, true)
            .DepthTarget(VK_FORMAT_D32_SFLOAT)   // sun shadow map, no color attachments
            .Build("Trees depth (tcOff=%u)", tcOffset);
    };
    createDepthVariant(24, m_DepthPipeline24);
    createDepthVariant(28, m_DepthPipeline28);
}

// ============================================================================
// CROWN VARIANTS — the ubershader split by specialization (r_tree_perm).
//
// r_tree_flat priced the ubershader as a block (3.4 of 4.8 ms on the 780M) but
// named no term. r_tree_terms named them, by folding a bit-per-term mask at
// PIPELINE-creation time — so a term is dead-code-eliminated for real: taps,
// ALU, registers, and (because the driver compiles VS and FS as a pair) the
// vertex work behind any varying the FS stops reading.
//
// ⭐ A spec constant and not a uniform branch, deliberately: this pass costs per
// WAVE (one surviving lane makes the whole wave execute the whole shader), so a
// runtime `if` most lanes skip would price every term at zero. Same reason
// r_tree_alpharef and a 17% cut in survivors moved this zone by nothing, and the
// same reason `r_gather_light 0` returned 0.02 ms where compiling the identical
// term out returned 0.45.
//
// So the probe became the shipping path: every frame the crown binds the
// pipeline compiled with exactly the terms the world can still produce. In plain
// dry summer daylight with no lamp in reach and no debug view open, that is
// snow, wet, ssil, dyn, ptdbg and debug gone — six of twelve.
//
// Pipelines are built on first use and never destroyed until the level ends —
// see the PermPipes comment in the header for why not the other way round.
// ============================================================================
const CTreeManager::PermPipes* CTreeManager::FindPermPipes(u32 mask, VkShaderModule vs, VkShaderModule fs) const
{
    for (const PermPipes& p : m_PermPipes)
        if (p.mask == mask && p.vs == vs && p.fs == fs) return &p;
    return nullptr;
}

// Names what the mask leaves OUT — "mask 0x7DF" tells nobody anything, "OFF:
// snow,wet,dyn" is the state of the world in three words.
void CTreeManager::DescribePermMask(u32 mask, char* out, size_t cap)
{
    static const char* kNames[12] = { "snow","sun","wet","ssil","sky","gi","ao","sheen","dyn","fog","ptdbg","debug" };
    out[0] = 0;
    for (int b = 0; b < 12; ++b)
        if ((mask & (1u << b)) == 0) {
            if (out[0]) xr_strcat(out, cap, ",");
            xr_strcat(out, cap, kNames[b]);
        }
    if (!out[0]) xr_strcpy(out, cap, "nothing");
}

bool CTreeManager::BuildPermPipes(u32 mask, VkShaderModule vs, VkShaderModule fs)
{
    if (m_GfxPipelineLayout == VK_NULL_HANDLE || vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) return false;
    if (FindPermPipes(mask, vs, fs)) return true;
    // The world only has so many states, and a sweep uses one mask per step. A
    // hundred would mean something is flipping the mask per frame, and 200 leaked
    // pipelines is worse than a lean crown.
    if (m_PermPipes.size() >= 32) {
        static bool warned = false;
        if (!warned) { warned = true; Msg("![VK TreePerm] 32 variant pipelines built — refusing more (restart the level)"); }
        return false;
    }

    const int32_t maskVal = (int32_t)mask;
    VkSpecializationMapEntry me{};
    me.constantID = 0; me.offset = 0; me.size = sizeof(int32_t);
    VkSpecializationInfo si{};
    si.mapEntryCount = 1; si.pMapEntries = &me;
    si.dataSize = sizeof(int32_t); si.pData = &maskVal;

    // Same state as the default colour pair — the variant must differ in the
    // specialization constant and in NOTHING else, or a delta is not the term.
    auto build = [&](u32 tcOffset) -> VkPipeline {
        return VK::GfxPipelineBuilder(m_GfxPipelineLayout)
            .Vert(vs).Frag(fs, &si)
            .Binding(0, 32)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
            .Cull(VK_CULL_MODE_NONE)
            .Depth(true, true)
            .Color(VK::SceneColor::Format())
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Trees gfx variant 0x%03X (tcOff=%u)", mask, tcOffset);
    };
    PermPipes pp{};
    pp.mask = mask; pp.vs = vs; pp.fs = fs;
    pp.p24  = build(24);
    pp.p28  = build(28);
    if (pp.p24 == VK_NULL_HANDLE || pp.p28 == VK_NULL_HANDLE) {
        if (pp.p24) vkDestroyPipeline(VulkanHW.m_Device, pp.p24, nullptr);
        if (pp.p28) vkDestroyPipeline(VulkanHW.m_Device, pp.p28, nullptr);
        Msg("![VK TreePerm] pipeline build failed for mask 0x%03X", mask);
        return false;
    }
    m_PermPipes.push_back(pp);

    string256 off; DescribePermMask(mask, off, sizeof(off));
    Msg("[VK TreePerm] built crown variant 0x%03X — OFF: %s (%u cached)", mask, off, (u32)m_PermPipes.size());
    return true;
}

// ============================================================================
// GPU cull for the CAMERA depth prepass (r_tree_depth_gpu).
// ============================================================================
// Why this exists, in the numbers that produced it: the prepass was a CPU walk of
// every instance on the level (193 471 scanned for 1 706 draws, 99.1% rejections)
// ending in one vkCmdDrawIndexed per tree, and r_tree_depth_sort proved the zone is
// DRAW-CALL bound rather than fill bound — cutting its fragments by 26% moved it by
// nothing. So the fix is fewer draws and no walk, which is exactly what the colour
// path already does. This reuses it: same tree_cull.comp (mode = CULL_PREPASS), one
// dispatch per group, one indirect draw per group.
//
// ⚠ RECORDED OUTSIDE THE RENDER PASS. vkCmdDispatch inside one is illegal, so the
// call site sits in Pass_World before the prepass begins.
// Prefix sum of per-group mesh counts. Group g's commands live at
// [m_GroupCmdBase[g], +meshCount) inside a slot, so a slot is m_TotalCount commands
// (~3.9 MB) rather than groups x maxGroupMesh (98 MB, which is what the COLOUR buffer
// still costs — fixing that the same way is a separate, obvious win).
void CTreeManager::EnsureGroupCmdBase()
{
    const u32 numGroups = (u32)m_Groups.size();
    if ((u32)m_GroupCmdBase.size() == numGroups || !numGroups) return;
    m_GroupCmdBase.resize(numGroups);
    u32 base = 0;
    for (u32 g = 0; g < numGroups; ++g) { m_GroupCmdBase[g] = base; base += m_Groups[g].meshCount; }
    // The groups PARTITION [0, m_TotalCount) into consecutive runs (Build() seeds one and
    // extends it while the vb/ib/stride/tcOffset/descSet tuple holds), so this sum is
    // m_TotalCount by construction — which is what lets both aux arenas size a slot by
    // m_TotalCount instead of carrying the sum around. Assert it rather than trust it:
    // an undersized indirect buffer would not crash, it would silently drop the last
    // group's trees.
    VERIFY(base == m_TotalCount);
}

// A second set on the SHARED cull layout: bindings 2/3 point at this path's own
// indirect/count buffers, every other binding is aliased onto the colour set's
// resources. Aliasing is not laziness — a declared-but-unbound descriptor is UB the
// moment the shader is dispatched, however unreachable the branch that would read it.
bool CTreeManager::CreateAuxCullSet(VkBuffer indirect, VkBuffer count,
                                    VkDescriptorPool& pool, VkDescriptorSet& set, const char* name)
{
    if (m_CullDescLayout == VK_NULL_HANDLE || !m_TreeMetadataBuffer || !m_FrustumUBO) return false;

    constexpr VkDescriptorType kS = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    constexpr VkDescriptorType kU = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constexpr VkDescriptorType kT = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool = VK::MakeDescriptorPool({ kS, kU, kS, kS, kT, kS, kS, kS, kS, kS, kS }, 1, name);   // nfs21: + 9, 10
    if (pool == VK_NULL_HANDLE) return false;
    if (!VK::AllocSets(pool, m_CullDescLayout, 1, &set, name)) return false;

    VkBuffer inert = m_TreeMetadataBuffer->GetHandle();
    VkBuffer lodT  = (m_VoxLodTable   && m_VoxLodTable->GetHandle())   ? m_VoxLodTable->GetHandle()   : inert;
    VkBuffer cmdB  = (m_VoxDrawCmds   && m_VoxDrawCmds->GetHandle())   ? m_VoxDrawCmds->GetHandle()   : inert;
    VkBuffer parB  = (m_VoxDrawParams && m_VoxDrawParams->GetHandle()) ? m_VoxDrawParams->GetHandle() : inert;
    VkBuffer statB = (m_CullStats     && m_CullStats->GetHandle())     ? m_CullStats->GetHandle()     : inert;
    VK::DescriptorWriter(set)
        .StorageBuffer(0, inert)
        .UniformBuffer(1, m_FrustumUBO->GetHandle())
        .StorageBuffer(2, indirect)
        .StorageBuffer(3, count)
        .StorageBuffer(5, lodT).StorageBuffer(6, cmdB).StorageBuffer(7, parB)
        .StorageBuffer(8, statB)
        .StorageBuffer(9, (m_SwiInstBuffer && m_SwiInstBuffer->GetHandle()) ? m_SwiInstBuffer->GetHandle() : inert)
        .StorageBuffer(10, (m_SwiWinBuffer && m_SwiWinBuffer->GetHandle()) ? m_SwiWinBuffer->GetHandle() : inert)
        .Flush();

    // Binding 4 (HZB) — written once to the 1x1 dummy: neither aux path runs the
    // occlusion test (the prepass IS the pyramid's source; a shadow map has no pyramid
    // at all), but the descriptor still has to resolve to a real image or the dispatch
    // is UB.
    if (RImplementation.Details) {
        if (VkImageView dummy = RImplementation.Details->DummyHZBView())
            VK::DescriptorWriter(set)
                // ⚠ SHADER_READ_ONLY_OPTIMAL, not GENERAL. The real Hi-Z pyramid lives
                // in GENERAL (it is compute-written), the 1x1 dummy does not — it is
                // created and left in SHADER_READ_ONLY_OPTIMAL, the way CDetailManager
                // binds it. Writing the wrong layout here is not theoretical:
                // validation flagged it 244 times in one run (VUID-vkCmdDraw-None-09600),
                // because the layout a descriptor was written with must match the
                // image's actual layout whenever the command could access it — and
                // "the shader does not read it in this mode" is not an exemption.
                .ImageSampler(4, dummy, RImplementation.Details->HZBSampler(),
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                .Flush();
    }
    return true;
}

bool CTreeManager::DispatchDepthCull(VkCommandBuffer cmd, const Fmatrix& viewProj)
{
    m_DepthCullValid = false;
    if (!IsReady() || cmd == VK_NULL_HANDLE) return false;
    if (m_CullPipeline == VK_NULL_HANDLE || m_CullPipelineLayout == VK_NULL_HANDLE) return false;

    const u32 numGroups = (u32)m_Groups.size();
    if (!numGroups) return false;

    // ---- Lazy allocation. Prefix-summed sections: group g owns m_GroupCmdBase[g]
    // .. +meshCount, so the whole buffer is m_TotalCount commands (~3.9 MB) instead of
    // groups × maxGroupMesh (98 MB, which is what the colour buffer costs today).
    if (!m_DepthIndirect)
    {
        EnsureGroupCmdBase();
        if (m_GroupCmdBase.empty()) return false;
        const u32 base = m_TotalCount;

        m_DepthIndirect = xr_new<CVulkanBuffer>();
        m_DepthIndirect->Create((VkDeviceSize)base * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true /*gpuOnly*/);
        m_DepthCount = xr_new<CVulkanBuffer>();
        m_DepthCount->Create((VkDeviceSize)numGroups * sizeof(u32),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true /*gpuOnly*/);
        if (m_DepthIndirect->GetHandle() == VK_NULL_HANDLE || m_DepthCount->GetHandle() == VK_NULL_HANDLE) {
            Msg("![VK Trees] no VRAM for the prepass indirect buffers (%u KB) — r_tree_depth_gpu inert",
                (u32)((base * sizeof(VkDrawIndexedIndirectCommand) + numGroups * 4) >> 10));
            return false;
        }

        if (!CreateAuxCullSet(m_DepthIndirect->GetHandle(), m_DepthCount->GetHandle(),
                              m_DepthCullPool, m_DepthCullSet, "Trees.DepthCull"))
            return false;
        Msg("[VK Trees] prepass GPU cull ready: %u commands (%u KB), %u groups",
            base, (u32)((base * sizeof(VkDrawIndexedIndirectCommand)) >> 10), numGroups);
    }
    if (m_DepthCullSet == VK_NULL_HANDLE) return false;

    // ---- Push. Deliberately the same block the colour cull fills, with mode flipped:
    // one struct, one shader, so the two sets cannot drift in anything except the
    // asymmetry that keeps colour a superset.
    TreeCullPush pc{};
    VK::ExtractFrustumPlanes(viewProj, pc.planes);
    pc.cut_dist = TreeFlodCutDist();
    TreeLodPush(pc);   // nfs21
    pc.vox_cut  = TreeVoxCutDist();
    pc.eye_x = RV().vCameraPosition.x;
    pc.eye_y = RV().vCameraPosition.y;
    pc.eye_z = RV().vCameraPosition.z;
    pc.viewProj = viewProj;
    pc.hzb_on   = 0;                       // the prepass IS the pyramid's source
    pc.hzb_focal = 1.0f / _max(0.05f, tanf(deg2rad(RV().fFOV) * 0.5f));
    pc.scr_h    = (float)VK::LodScreenHeight();   // r_lod_render_res: this cull feeds the CAMERA prepass
    pc.stats_on = 0;                       // the ledger counts the COLOUR cull, not this one
    pc.mode     = TREE_CULL_PREPASS;
    pc.vox_count = numGroups;              // unused in this mode (no cubes emitted)

    // ---- Clear counts, barrier, dispatch. Same cross-frame WAR shape as the colour
    // path: these buffers are single-buffered while frames are in flight, so the
    // previous frame's indirect fetch must be ordered before this frame's fill.
    {
        VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, 0,
                           VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    vkCmdFillBuffer(cmd, m_DepthCount->GetHandle(), 0, VK_WHOLE_SIZE, 0u);
    {
        VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }

    const bool zoneSub = ps_r_profiler >= 2;
    const int  zDC = zoneSub ? VK::Prof::ZoneBegin(cmd, "Depth/TreeCull") : -1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CullPipelineLayout,
                            0, 1, &m_DepthCullSet, 0, nullptr);
    for (u32 g = 0; g < numGroups; ++g) {
        const TreeIndirectGroup& grp = m_Groups[g];
        pc.mesh_count  = grp.meshCount;
        pc.mesh_offset = grp.meshOffset;
        pc.output_base = m_GroupCmdBase[g];
        pc.count_index = g;
        vkCmdPushConstants(cmd, m_CullPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (grp.meshCount + 255) / 256, 1, 1);
    }
    if (zDC >= 0) VK::Prof::ZoneEnd(cmd, zDC);

    {
        VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_INDIRECT_COMMAND_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    }

    m_DepthCullValid = true;
    return true;
}

// ============================================================================
// GPU cull for SHADOW targets (r_tree_shadow_gpu).
// ============================================================================
// The prepass walk this mirrors was ONE scan of the level per frame. The shadow maps
// are five more on any frame that misses a shadow cache: 967 k - 1.2 M instances
// tested to issue ~10 000 vkCmdDrawIndexed, on the main thread, inside CPU_RECORD
// where it reads as "recording". Per unit of work it is the same win the prepass got,
// five times over — with one honest caveat, measured BEFORE this was written and worth
// keeping next to the code: with a still camera and the shadow caches holding, those
// five walks fire on roughly one sampled frame in ten, not on every frame. What this
// removes is a recurring SPIKE (and the reason to keep those caches lazy), not a
// steady per-frame tax. `[VK TreeCPU]` is where that ratio is read off.
//
// The predicate differs from BOTH camera modes and comes straight from the CPU test it
// replaces (ShadowMap::SphereVisible / CascadeSphereVisible / RainSphereVisible): six
// planes of the light's box, +radius, no slack, plus the camera-distance band that
// splits a cascade's cached far layer from its per-frame near one. See the CULL_SHADOW
// notes in tree_cull.comp.glsl.
//
// ⚠ RECORDED OUTSIDE ANY RENDER PASS — vkCmdDispatch inside one is illegal. Callers
// batch every target the frame is about to draw into ONE call, so the frame pays one
// clear and one barrier pair in total rather than per target.
u32 CTreeManager::DispatchShadowCull(VkCommandBuffer cmd, const TreeShadowJob* jobs, u32 n)
{
    if (!jobs || !n || cmd == VK_NULL_HANDLE) return 0;
    if (!IsReady() || m_ShadowCullFailed) return 0;
    if (m_CullPipeline == VK_NULL_HANDLE || m_CullPipelineLayout == VK_NULL_HANDLE) return 0;

    const u32 numGroups = (u32)m_Groups.size();
    if (!numGroups || !m_TotalCount) return 0;

    // ---- Lazy allocation of the whole arena, on the first shadow dispatch. A level
    // where the shadow path never runs (VSM owning the sun, or night) must not pay for
    // slots nothing fills, and this is not a rounding error: TREE_SH_COUNT sections of
    // m_TotalCount commands is ~23 MB at 193 k trees. One allocation and one descriptor
    // set, because a slot is nothing but an OFFSET into both buffers.
    if (!m_ShadowIndirect)
    {
        EnsureGroupCmdBase();
        if (m_GroupCmdBase.empty()) { m_ShadowCullFailed = true; return 0; }

        const VkDeviceSize cmds = (VkDeviceSize)TREE_SH_COUNT * m_TotalCount;
        m_ShadowIndirect = xr_new<CVulkanBuffer>();
        m_ShadowIndirect->Create(cmds * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true /*gpuOnly*/);
        m_ShadowCount = xr_new<CVulkanBuffer>();
        m_ShadowCount->Create((VkDeviceSize)TREE_SH_COUNT * numGroups * sizeof(u32),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true /*gpuOnly*/);
        if (m_ShadowIndirect->GetHandle() == VK_NULL_HANDLE || m_ShadowCount->GetHandle() == VK_NULL_HANDLE
            || !CreateAuxCullSet(m_ShadowIndirect->GetHandle(), m_ShadowCount->GetHandle(),
                                 m_ShadowCullPool, m_ShadowCullSet, "Trees.ShadowCull"))
        {
            // ⚠ Latched, not retried. Without the latch a VRAM-starved level would
            // attempt a 23 MB allocation again on every shadow frame, forever.
            Msg("![VK Trees] no VRAM for the shadow indirect arena (%u MB) — r_tree_shadow_gpu inert",
                (u32)((cmds * sizeof(VkDrawIndexedIndirectCommand)) >> 20));
            m_ShadowCullFailed = true;
            return 0;
        }
        Msg("[VK Trees] shadow GPU cull ready: %u slots x %u commands = %u MB, %u groups",
            (u32)TREE_SH_COUNT, m_TotalCount,
            (u32)((cmds * sizeof(VkDrawIndexedIndirectCommand)) >> 20), numGroups);
    }
    if (m_ShadowCullSet == VK_NULL_HANDLE) return 0;

    // ---- Clear only the slots in THIS batch. A slot left out keeps last frame's
    // commands, which is harmless — its validity bit stays clear and its call site
    // falls back to the CPU walk — but wiping it would cost a fill for nothing. The
    // leading barrier is the same cross-frame WAR the prepass needs: these buffers are
    // single-buffered while frames are in flight, so the previous frame's indirect
    // FETCH has to be ordered before this frame's fill.
    {
        VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, 0,
                           VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    u32 wanted = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 t = jobs[i].target;
        if (t >= TREE_SH_COUNT || (wanted & (1u << t))) continue;   // ignore junk + duplicates
        wanted |= (1u << t);
        vkCmdFillBuffer(cmd, m_ShadowCount->GetHandle(),
                        (VkDeviceSize)t * numGroups * sizeof(u32),
                        (VkDeviceSize)numGroups * sizeof(u32), 0u);
    }
    if (!wanted) return 0;
    {
        VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }

    const bool zoneSub = ps_r_profiler >= 2;
    const int  zSC = zoneSub ? VK::Prof::ZoneBegin(cmd, "Shadow/TreeCull") : -1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CullPipelineLayout,
                            0, 1, &m_ShadowCullSet, 0, nullptr);

    u32 done = 0;
    for (u32 i = 0; i < n; ++i)
    {
        const TreeShadowJob& job = jobs[i];
        if (job.target >= TREE_SH_COUNT || (m_ShadowCullValid & (1u << job.target))) continue;

        TreeCullPush pc{};
        VK::ExtractFrustumPlanes(job.vp, pc.planes);
        pc.viewProj = job.vp;
        // ⚠ The EYE stays the CAMERA even though the projection is the light's: the
        // layer band is a camera distance (near crowns sway, far ones sit cached), and
        // that is exactly what the CPU walk compared.
        const Fvector& bandEye = job.useEye ? job.eye : RV().vCameraPosition;   // nfs47: the layer's band centre
        pc.eye_x = bandEye.x;
        pc.eye_y = bandEye.y;
        pc.eye_z = bandEye.z;
        pc.min_dist = job.minDist;
        pc.max_dist = job.maxDist;
        if (job.lodScale > 0.0f) TreeShadowLodPush(pc, job.lodScale);   // nfs48: cached layers
        // Everything the camera modes do and a shadow map must NOT:
        //  - cut_dist / vox_cut = 0. A caster has no billboard or cube stand-in inside a
        //    shadow map, so dropping it past a distance simply deletes its shadow. The
        //    CPU shadow call sites passed 0 for that same reason.
        //  - hzb_on = 0. The Hi-Z pyramid is the CAMERA's depth; testing a caster the
        //    LIGHT sees against occluders the CAMERA sees answers a different question.
        //  - stats_on = 0. The ledger describes the colour cull; folding five more
        //    dispatches into its 48 counters would make every field a sum of unrelated
        //    passes.
        pc.cut_dist  = 0.0f;
        pc.vox_cut   = 0.0f;
        pc.hzb_on    = 0;
        pc.hzb_focal = 1.0f / _max(0.05f, tanf(deg2rad(RV().fFOV) * 0.5f));
        pc.scr_h     = (float)RV().dwHeight;
        pc.stats_on  = 0;
        pc.mode      = TREE_CULL_SHADOW;
        pc.vox_count = numGroups;          // unused in this mode (no cubes emitted)

        const u32 cmdSlot = job.target * m_TotalCount;
        const u32 cntSlot = job.target * numGroups;
        for (u32 g = 0; g < numGroups; ++g) {
            const TreeIndirectGroup& grp = m_Groups[g];
            pc.mesh_count  = grp.meshCount;
            pc.mesh_offset = grp.meshOffset;
            pc.output_base = cmdSlot + m_GroupCmdBase[g];
            pc.count_index = cntSlot + g;
            vkCmdPushConstants(cmd, m_CullPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (grp.meshCount + 255) / 256, 1, 1);
        }
        m_ShadowCullValid |= (1u << job.target);
        ++done;
    }
    if (zSC >= 0) VK::Prof::ZoneEnd(cmd, zSC);

    {
        VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_INDIRECT_COMMAND_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    return done;
}

// ============================================================================
// Shadow caster path — depth-only alpha-tested draws of trees inside the sun
// ortho box. GPU-culled when the call site claimed a slot (r_tree_shadow_gpu),
// otherwise a CPU walk over every instance on the level, once per target.
// ============================================================================
void CTreeManager::RenderDepth(VkCommandBuffer cmd, const Fmatrix& lightVP, s32 cascade,
                               const CFrustum* frustum, float minDist, float maxDist,
                               float flodCut, float voxCut, u32 sortBuckets, bool gpuCulled,
                               s32 gpuShadowTarget, bool mvTarget, const Fvector* bandEye, int windPose)
{
    const Fvector& bandP = bandEye ? *bandEye : RV().vCameraPosition;   // nfs47: centre of the distance band
    if (!m_bBuilt || m_MetaCPU.empty()) return;
    if (m_XformDescSet == VK_NULL_HANDLE || m_GfxPipelineLayout == VK_NULL_HANDLE) return;
    if (m_DepthPipeline24 == VK_NULL_HANDLE && m_DepthPipeline28 == VK_NULL_HANDLE) return;

    // ---- r_mv_prepass: draw with the MOTION pair instead of the depth pair --------
    // Same geometry, same alpha-test, same clip z/w — but the fragment also states
    // this crown's screen motion, which is what removes the separate overlay raster.
    // Built lazily like the overlay's (needs the xform/tex layouts from Build).
    if (mvTarget) CreateMotionPipelines();
    const bool mvOn = mvTarget && m_MotionPipelineLayout != VK_NULL_HANDLE
                   && m_MvDepthPipeline24 != VK_NULL_HANDLE && m_MvDepthPipeline28 != VK_NULL_HANDLE;
    // ⚠ The scope's attachment list is decided by the CALLER (Pass_World hangs the
    // motion target on the prepass only when MotionVec handed it a view). If the
    // twins failed to build we must NOT silently fall back to the depth pair — its
    // pipelines declare no colour attachment and the scope has one. Pass_World gates
    // mvTarget on the same MotionVec state, so this can only diverge on a build
    // failure; say so once rather than drawing into a mismatched scope.
    if (mvTarget && !mvOn) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; Msg("![VK Trees] MV prepass pipelines unavailable — canopy skipped in the motion-carrying prepass"); }
        return;
    }
    VkPipelineLayout drawLayout = mvOn ? m_MotionPipelineLayout : m_GfxPipelineLayout;
    VkPipeline       pipe24 = mvOn ? m_MvDepthPipeline24 : m_DepthPipeline24;
    VkPipeline       pipe28 = mvOn ? m_MvDepthPipeline28 : m_DepthPipeline28;

    // Wind, built the same way in both branches — the shadow caster must bend with
    // the trees (a rigid shadow on a bent trunk shows a black self-shadow band), and
    // the motion pass needs the same pose to reproject.
    Fvector4 windParams{}, wsetup{}, windAnim{};
    wsetup.set(ps_r_wind_tree_anim, ps_r_wind_tree_trunk, ps_r_wind_tree_bend, 0.1f);
    if (g_pGamePersistent) {
        const Fvector3 wa = ES().wind_anim;
        windAnim.set(wa.x, wa.y, wa.z, ps_r_wind_tree_flutter);
        if (auto* E = ES().CurrentEnv)
            windParams.set(E->wind_direction, E->wind_velocity, 0.0f, 0.0f);
    }
    windParams.w = ps_r_wind_tree_crown;   // flutter height gate — must match the forward pass
    // nfs47: caster pose chosen by the caller. 1 = REST: no trunk bend (wsetup.z) and no flutter (windAnim.w) -> zero
    // displacement, so a CACHED layer looks the same whenever it is redrawn. 2 = bend only (a live layer rebuilt at a
    // cadence > 1: the flutter is too fast for it and jumps -- nfs45).
    if (!mvOn && windPose == 1) { wsetup.z = 0.0f; windAnim.w = 0.0f; }
    if (!mvOn && windPose == 2) windAnim.w = 0.0f;

    if (mvOn) {
        // The camera prepass is now the frame's FIRST tree raster, so it is where the
        // wind history rolls (idempotent — the forward pass finds it stamped).
        RollMvWind(windParams, wsetup, windAnim);
        float jx = 0.0f, jy = 0.0f;
        VK::MotionVec::FrameJitter(jx, jy);
        TreeMotionPush mp{};
        mp.curVP             = VK::MotionVec::CurVP();    // UNJITTERED — jitter-free MV
        mp.prevVP            = VK::MotionVec::PrevVP();
        mp.uvScale           = 1.0f / 2048.0f;
        mp.alphaRef          = TreeAlphaRef(200.0f / 255.0f);
        mp.jitterX           = jx;                        // re-applied to gl_Position only
        mp.jitterY           = jy;
        mp.wind_params       = m_MvWindParams;
        mp.wsetup_trees      = m_MvWsetup;
        mp.wind_anim         = m_MvWindAnim;
        mp.wind_params_prev  = m_MvWindParamsPrev;
        mp.wsetup_trees_prev = m_MvWsetupPrev;
        mp.wind_anim_prev    = m_MvWindAnimPrev;
        vkCmdPushConstants(cmd, drawLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(mp), &mp);
    } else {
        // Push: light view·proj + tree UV quant + leaf alpha cutoff (first 72 B of
        // the TreeGfxPush range; sun/hemi tail unused by the depth shaders).
        TreeGfxPush pc{};
        pc.mViewProj    = lightVP;
        pc.uvScale      = 1.0f / 2048.0f;
        pc.alphaRef     = TreeAlphaRef(200.0f / 255.0f);
        pc.wsetup_trees = wsetup;
        pc.wind_anim    = windAnim;
        pc.wind_params  = windParams;
        vkCmdPushConstants(cmd, drawLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout,
                            0, 1, &m_XformDescSet, 0, nullptr);

    VkPipeline      lastPipe = VK_NULL_HANDLE;
    VkDescriptorSet lastTex  = VK_NULL_HANDLE;
    u32 nDraw = 0;
    u32 nSeen = 0;
    u32 nBind = 0;   // ledger: state changes, the price this sort pays
    u32 nSkip = 0;   // groups the bound rejected before any bind (GPU-culled paths)

    // ⚠⚠ THE CALLER SAYS SO, EXPLICITLY, AND IT MUST. My first version inferred "this
    // is the camera prepass" from the arguments — frustum set, cascade < 0, no wind
    // distance band — and the FLASHLIGHT SPOT ATLAS matches every one of those
    // (vk_pass_shadow.cpp passes a CFrustum with cascade -1). It would have drawn the
    // spot tiles from the camera's indirect buffer: the wrong tree set, culled against
    // the wrong frustum, into a shadow map. Nothing about the argument list can
    // distinguish those two call sites, so the call site declares itself.
    // m_DepthCullValid additionally proves the dispatch ran THIS frame — without it a
    // toggled cvar would leave the last frame's commands to be drawn again.
    const bool useGpuCull = gpuCulled && m_DepthCullValid && m_DepthIndirect && m_DepthCount
                         && !m_GroupCmdBase.empty();

    // Same declaration-by-the-call-site rule for shadow targets, and the slot number
    // carries it: DispatchShadowCull culled THIS target against THIS light's box, so
    // there is no way to end up drawing one map's tree list into another's. The
    // validity BIT (not a bool) proves the dispatch ran this frame for this slot —
    // toggling r_tree_shadow_gpu off skips the dispatch, and a flag left standing would
    // redraw the previous frame's commands into a map that moved.
    const bool useGpuShadow = gpuShadowTarget >= 0
                           && ShadowCullReady((u32)gpuShadowTarget)
                           && m_ShadowIndirect && m_ShadowCount && !m_GroupCmdBase.empty();

    // ---- Where the indirect draw below reads from. The two GPU-culled paths differ
    // only in the buffer pair and a slot offset; the loop that consumes them is one
    // piece of code, which is the point — the prepass and five shadow maps cannot
    // drift apart in how they bind, batch or count.
    VkBuffer     giCmds = VK_NULL_HANDLE, giCounts = VK_NULL_HANDLE;
    VkDeviceSize giCmdBase = 0, giCntBase = 0;
    if (useGpuShadow) {
        giCmds    = m_ShadowIndirect->GetHandle();
        giCounts  = m_ShadowCount->GetHandle();
        giCmdBase = (VkDeviceSize)(u32)gpuShadowTarget * m_TotalCount;
        giCntBase = (VkDeviceSize)(u32)gpuShadowTarget * (u32)m_Groups.size();
    } else if (useGpuCull) {
        giCmds   = m_DepthIndirect->GetHandle();
        giCounts = m_DepthCount->GetHandle();
    }

    // ---- The per-tree tests, in ONE place so both draw orders below apply exactly
    // the same set. The SET must not depend on the ORDER: the whole safety argument
    // for sorting is that it only permutes draws, so the colour-superset-of-prepass
    // invariant and the final depth buffer are untouched.
    auto passes = [&](const GpuTreeMeta& m) -> bool
    {
        // Distance split (wind shadows): keep near trees in the per-frame
        // dynamic layer and far trees in the cached static layer.
        if (minDist > 0.0f || maxDist < 1e9f) {
            const float d = bandP.distance_to(m.sphere_P);
            if (d < minDist || d >= maxDist) return false;
        }
        // r_tree_dist twin of the colour cull (camera prepass only — callers
        // pass flodCut == 0 everywhere else). CENTRE distance, no radius slack,
        // so this set is strictly SMALLER than what tree_cull.comp keeps
        // (it subtracts sphere_R): colour ⊇ prepass holds by construction, and
        // FP differences between the two code paths can't invert it.
        if (flodCut > 0.0f && (m.flags & TREE_FLOD_BACKED) != 0u) {
            // nfs21: the same size-scaled cut as tree_cull (centre distance here, so still a subset)
            float k = 1.f;
            Fvector ref = m.sphere_P;
            // nfs23: the group-centre metric is always on (tree_cull: size_lod < 0 = k 1); the size LOD only scales it.
            if ((m.flags >> 24) & 255u) {   // VOX-backed too: k has its own bits (vk_TreeManager.h)
                const float kq = TreeLodSizeOn() ? float((m.flags >> 24) & 255u) / 64.f : 1.f;
                k = kq * TreeLodBand() * 0.98f;   // a hair inside colour
                const size_t ti = size_t(&m - m_MetaCPU.data());
                if (ti < m_TreeGroupC.size()) ref = m_TreeGroupC[ti];
            }
            if (RV().vCameraPosition.distance_to(ref) > flodCut * k) return false;
        }
        // r_tree_vox_dist twin, identical construction: CENTRE distance here, near
        // side of the sphere in tree_cull.comp, so this crown set stays strictly
        // SMALLER than the colour one. Only crowns with cubes on the GPU qualify;
        // trunks (never voxelized) and unbaked species fall through untouched.
        if (voxCut > 0.0f && (m.flags & TREE_VOX_BACKED) != 0u) {
            if (RV().vCameraPosition.distance_to(m.sphere_P) > voxCut) return false;
        }
        if (frustum) return frustum->testSphere_dirty(m.sphere_P, m.sphere_R);
        return cascade >= 0 ? ShadowMap::CascadeSphereVisible((u32)cascade, m.sphere_P, m.sphere_R)
                            : ShadowMap::SphereVisible(m.sphere_P, m.sphere_R);
    };

    // ========================================================================
    // GPU-CULLED PATH — one indirect draw per GROUP.
    // ========================================================================
    // 1 706 vkCmdDrawIndexed become ~92 vkCmdDrawIndexedIndirectCount, and the CPU
    // walk over 193 471 instances disappears entirely: the cull dispatch already
    // decided the set on the GPU, using the same shader as the colour cull.
    // ⭐ ONE LOOP FOR THE CAMERA PREPASS (r_tree_depth_gpu) AND FOR EVERY SHADOW MAP
    // (r_tree_shadow_gpu). Which buffers it reads was settled above; the batching,
    // the state binds and the ledger are shared, so six call sites cannot drift into
    // six slightly different draw loops the way the CPU walk's copies were starting to.
    // ⚠ The SPOT tiles still fall through to the CPU walk: perspective cones that
    // refresh on light movement, not per frame, and each would cost another full
    // command section. They are a separate, later question.
    if (useGpuCull || useGpuShadow)
    {
        // ---- Group-level reject, the counterpart to the per-instance walk the GPU cull
        // removed. An indirect draw is issued per group because the survivor count lives
        // on the device — so a target with ~100 survivors was paying 161 zero-count draws
        // plus their vertex/index binds, and that measured LARGER than the cull it was
        // feeding (+1.6 ms of gpu_total against 0.19 ms of compute, three targets).
        // ⚠ CONSERVATIVE BY CONSTRUCTION, and it has to be: grp.bsR encloses every member
        // tree's own sphere, so a group this rejects cannot contain a tree the GPU kept.
        // Each clause below is the group-radius-widened form of the per-tree test in
        // passes() — widened OUTWARD, never inward.
        auto groupVisible = [&](const TreeIndirectGroup& grp) -> bool
        {
            if (grp.bsR <= 0.f) return true;             // no bound computed → never skip
            const float d = RV().vCameraPosition.distance_to(grp.bsP);
            // Layer band: keep the group if ANY point of it can fall inside [min, max).
            const float dB = bandP.distance_to(grp.bsP);   // nfs47: band around bandEye
            if (minDist > 0.0f && dB + grp.bsR < minDist) return false;
            if (maxDist < 1e9f && dB - grp.bsR >= maxDist) return false;
            // The prepass distance cuts, measured to the group's NEAR side.
            if (flodCut > 0.0f && d - grp.bsR > flodCut) return false;
            if (voxCut  > 0.0f && d - grp.bsR > voxCut)  return false;
            // ⚠ A METRE OF SLACK ON THE BOX TESTS, and it is not superstition. The GPU
            // culls against the planes of the SNAPPED cascade VP (ComputeCascadeVP shifts
            // the projection onto the texel lattice), while CascadeSphereVisible measures
            // the UNSNAPPED kCascadeBox. The two boxes differ by up to one texel — about
            // 3 cm at 60 m over 2048 — and on that sliver this reject could in principle
            // drop a group the GPU kept. A metre buys the whole discrepancy back at the
            // cost of occasionally issuing a draw that turns out empty, which is the
            // direction this test is allowed to be wrong in.
            constexpr float kSlack = 1.0f;
            if (frustum) return frustum->testSphere_dirty(grp.bsP, grp.bsR + kSlack);
            return cascade >= 0 ? ShadowMap::CascadeSphereVisible((u32)cascade, grp.bsP, grp.bsR, kSlack)
                                : ShadowMap::SphereVisible(grp.bsP, grp.bsR + kSlack);
        };
        // ⚠ The FLOD/vox clauses above only fire for the camera prepass (every other
        // caller passes 0), which is correct: a shadow caster has no billboard stand-in.
        // ⚠ The rain target reaches the `cascade < 0` branch, i.e. the FAR SUN box, not
        // the rain box — same mismatch the old CPU walk had. Here it is harmless in the
        // safe direction only if the sun box contains the rain one; it does not in
        // general, so rain skips this reject entirely (see the guard below).
        const bool groupReject = !(useGpuShadow && gpuShadowTarget == (s32)TREE_SH_RAIN);

        for (u32 g = 0; g < (u32)m_Groups.size(); ++g)
        {
            const TreeIndirectGroup& grp = m_Groups[g];
            VkPipeline pipe = (grp.tcOffset == 24) ? pipe24 : pipe28;
            if (pipe == VK_NULL_HANDLE) continue;
            if (groupReject && !groupVisible(grp)) { ++nSkip; continue; }
            if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; ++nBind; }
            if (grp.descSetIdx < m_TexDescSets.size() && m_TexDescSets[grp.descSetIdx] != lastTex) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout,
                                        1, 1, &m_TexDescSets[grp.descSetIdx], 0, nullptr);
                lastTex = m_TexDescSets[grp.descSetIdx];
                ++nBind;
            }
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &vbOff);
            vkCmdBindIndexBuffer(cmd, grp.ib, 0, VK_INDEX_TYPE_UINT16);
            nBind += 2;

            vkCmdDrawIndexedIndirectCount(cmd,
                giCmds,
                (giCmdBase + m_GroupCmdBase[g]) * sizeof(VkDrawIndexedIndirectCommand),
                giCounts, (giCntBase + g) * sizeof(u32),
                grp.meshCount, sizeof(VkDrawIndexedIndirectCommand));
            ++nDraw;
        }
        // seen = 0: there IS no walk any more, and that is the point of the line.
        StatsNoteDepthWalk(0, nDraw, nBind);
        // Two latches, not one: the prepass and the shadow targets take this loop with
        // very different group-skip ratios, and a shared latch would print whichever ran
        // first and hide the other — which is the half the bound was added for.
        static bool s_diagGpu[2] = { false, false };
        const u32 which = useGpuShadow ? 1u : 0u;
        if (!s_diagGpu[which]) { s_diagGpu[which] = true;
            Msg("[VK Trees] %s drawn GPU-culled: %u indirect draws, %u of %u groups skipped by bound",
                useGpuShadow ? "shadow target" : "prepass", nDraw, nSkip, (u32)m_Groups.size()); }
        return;
    }

    // ========================================================================
    // FRONT-TO-BACK ORDER (r_tree_depth_sort) — the prepass's own early-Z.
    // ========================================================================
    // ⭐ WHY THIS PASS AND NOT THE COLOUR ONE. The colour pass already draws onto
    // finished depth, so its order cannot matter. The PREPASS is the one that
    // BUILDS that depth: nothing shields it, and the tree ledger measured it
    // running 34 M fragment shader invocations to cover ~3.7 M pixels. It draws
    // grouped by TEXTURE, which is an arbitrary order in depth — so a near crown
    // frequently arrives after the far ones it would have occluded, and their
    // alpha-tested fragments are shaded for nothing.
    //
    // Sorting strictly by distance would maximise the rejection and destroy the
    // batching: every draw could then need its own vertex/index buffer and
    // descriptor bind. So the order is BUCKETED — depth slabs near→far, and
    // within a slab the original group order, which keeps whole groups batched.
    // A handful of slabs captures most of the occlusion; the cvar is the slab
    // count so the trade can be swept rather than guessed.
    //
    // ⚠ SAFETY: this only permutes draws. Depth is written with LEQUAL, so the
    // resulting buffer is the per-pixel minimum either way — bit-identical, in
    // any order. What changes is how many fragments are shaded on the way there.
    // ⚠ Applies to the CAMERA prepass only: "front to back" is measured from the
    // camera, which is meaningless for a shadow caster (there it would have to be
    // distance along the light). Shadow call sites pass sortBuckets = 0.
    if (sortBuckets > 1)
    {
        m_DepthSortKeys.clear();
        if (m_DepthSortKeys.capacity() < 4096) m_DepthSortKeys.reserve(4096);
        // Slab edges are linear in distance out to the cut — equal depth slabs are
        // what the occlusion argument wants. flodCut is the natural far edge (past
        // it the mesh is not drawn at all); without it, fall back to a generous range.
        const float maxD = (flodCut > 0.0f) ? flodCut : 1000.0f;
        const u32   nB   = _min(sortBuckets, 64u);
        const float kB   = (float)nB / _max(1.0f, maxD);

        for (u32 g = 0; g < (u32)m_Groups.size(); ++g)
        {
            const TreeIndirectGroup& grp = m_Groups[g];
            if (((grp.tcOffset == 24) ? pipe24 : pipe28) == VK_NULL_HANDLE) continue;
            nSeen += grp.meshCount;
            for (u32 i = 0; i < grp.meshCount; ++i)
            {
                const u32 gi = grp.meshOffset + i;
                const GpuTreeMeta& m = m_MetaCPU[gi];
                if (!passes(m)) continue;
                const float d = RV().vCameraPosition.distance_to(m.sphere_P);
                u32 b = (u32)_max(0.0f, d * kB);
                if (b >= nB) b = nB - 1;
                // Sort key: slab (near first) | group (keeps a slab's draws batched)
                // | global tree index. 18 bits of index covers 262 144 trees, which is
                // the same ceiling the VSM caster pack already imposes.
                m_DepthSortKeys.push_back(((u64)b << 48) | ((u64)g << 32) | (u64)gi);
            }
        }
        std::sort(m_DepthSortKeys.begin(), m_DepthSortKeys.end());

        u32 curGroup = ~0u;
        for (const u64 key : m_DepthSortKeys)
        {
            const u32 g  = (u32)((key >> 32) & 0xFFFFu);
            const u32 gi = (u32)(key & 0xFFFFFFFFu);
            const TreeIndirectGroup& grp = m_Groups[g];
            if (g != curGroup) {
                curGroup = g;
                VkPipeline pipe = (grp.tcOffset == 24) ? pipe24 : pipe28;
                if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; ++nBind; }
                if (grp.descSetIdx < m_TexDescSets.size() && m_TexDescSets[grp.descSetIdx] != lastTex) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout,
                                            1, 1, &m_TexDescSets[grp.descSetIdx], 0, nullptr);
                    lastTex = m_TexDescSets[grp.descSetIdx];
                    ++nBind;
                }
                VkDeviceSize vbOff = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &vbOff);
                vkCmdBindIndexBuffer(cmd, grp.ib, 0, VK_INDEX_TYPE_UINT16);
                nBind += 2;
            }
            const GpuTreeMeta& m = m_MetaCPU[gi];
            vkCmdDrawIndexed(cmd, m.index_count, 1, m.ib_first, (s32)m.first_vertex, gi);
            ++nDraw;
        }

        StatsNoteDepthWalk(nSeen, nDraw, nBind);
        static bool s_diagSort = false;
        if (!s_diagSort && nDraw) {
            s_diagSort = true;
            Msg("[VK Trees] depth prepass sorted front-to-back: %u slabs, %u draws, %u state binds", nB, nDraw, nBind);
        }
        return;
    }

    for (const TreeIndirectGroup& grp : m_Groups)
    {
        VkPipeline pipe = (grp.tcOffset == 24) ? pipe24 : pipe28;
        if (pipe == VK_NULL_HANDLE) continue;

        bool boundGroup = false;
        nSeen += grp.meshCount;   // ledger: this walk is O(instances on the level)
        for (u32 i = 0; i < grp.meshCount; ++i)
        {
            const GpuTreeMeta& m = m_MetaCPU[grp.meshOffset + i];
            if (!passes(m)) continue;

            if (!boundGroup) {
                if (pipe != lastPipe) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                    lastPipe = pipe;
                    ++nBind;
                }
                if (grp.descSetIdx < m_TexDescSets.size() && m_TexDescSets[grp.descSetIdx] != lastTex) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout,
                                            1, 1, &m_TexDescSets[grp.descSetIdx], 0, nullptr);
                    lastTex = m_TexDescSets[grp.descSetIdx];
                    ++nBind;
                }
                VkDeviceSize vbOff = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &vbOff);
                vkCmdBindIndexBuffer(cmd, grp.ib, 0, VK_INDEX_TYPE_UINT16);
                nBind += 2;
                boundGroup = true;
            }
            // firstInstance = global tree index → gl_InstanceIndex picks the xform.
            vkCmdDrawIndexed(cmd, m.index_count, 1, m.ib_first, (s32)m.first_vertex, grp.meshOffset + i);
            ++nDraw;
        }
    }

    // ⚠ THE COST THIS RECORDS IS CPU, NOT GPU, AND NOTHING ELSE BILLS IT. This is the
    // fallback walk: it scans m_MetaCPU end to end, once per target that did not get a
    // GPU slot. At ~193 k instances it was over a million distance tests per frame
    // across the camera prepass, two cascade layers, rain and each spot tile, all of it
    // inside CPU_RECORD where it reads as "recording". r_tree_depth_gpu took the prepass
    // off it and r_tree_shadow_gpu the sun/rain maps; what is left here is the SPOT
    // tiles, plus whatever either cvar is switched off. A climbing number now means one
    // of those two, and `walks` says which.
    StatsNoteDepthWalk(nSeen, nDraw, nBind);

    static bool s_diag = false;
    if (!s_diag && nDraw) { s_diag = true; Msg("[VK Trees] first shadow render: %u trees in the sun box", nDraw); }
}

// ============================================================================
// Per-frame render.
// ============================================================================
#ifdef __SWITCH__
extern "C" int g_xr_nx_diag;   // nfs30 device.cpp: 131 = tree colour draws through a 1x1 scissor
#endif
void CTreeManager::Render(VK::FrameContext& ctx)
{
    VK::Vram::Scope _vram_scope("Trees");
    if (!IsReady()) return;
    if (ctx.cmd == VK_NULL_HANDLE || ctx.viewProj == nullptr) return;

    const VkCommandBuffer cmd = ctx.cmd;
    const Fmatrix& vp = *ctx.viewProj;
    const u32 numGroups = (u32)m_Groups.size();

    // ----- 1) Extract this frame's frustum planes (Gribb/Hartmann; same as grass).
    // These go into the per-dispatch PUSH below — NOT the shared UBO — so a rotating
    // camera's next-frame planes can't clobber this frame's cull (the old single-
    // buffered UBO did, producing black tree silhouettes at the screen edge).
    TreeCullPush cullPush{};
    VK::ExtractFrustumPlanes(vp, cullPush.planes);
    // Everything the crown cube tier needs, in one place: the per-species LOD table
    // (baked + uploaded), both GPU buffers, and the pipeline. See the gate below.
    const bool voxReady = VoxTierReady();
    // Distance cut for billboard-backed trees (r_tree_dist). The prepass applies
    // the SAME value via RenderDepth(..., flodCut) but on centre distance, so this
    // one — measured from the sphere's near side — always keeps a superset. See
    // tree_cull.comp.glsl.
    cullPush.cut_dist = TreeFlodCutDist();
    TreeLodPush(cullPush);   // nfs21
    // r_tree_vox_dist — the voxel rung. Same near-side-of-the-sphere measurement as
    // cut_dist, and the prepass twin in RenderDepth uses centre distance, so the
    // colour set stays a superset of the prepass set here too.
    cullPush.vox_cut  = TreeVoxCutDist();
    // Cube-size curve for that rung, pre-folded into two coefficients (see TreeCullPush):
    // FOV, render height and the live LOD bias enter HERE and nowhere else, so neither
    // the cull shader nor the cube VS has to know what a pixel is. The curve itself is
    // Nanite's screen-error cut — a cube's projected size stays near r_..._vox_px, with
    // _vox_far letting far crowns read deliberately chunkier.
    {
        const float pxAt1m = 2.f * tanf(deg2rad(RV().fFOV) * 0.5f) / (float)_max(1u, RV().dwHeight);
        const float bias   = powf(2.f, (float)ps_r_vsm_tree_hull_lod);
        cullPush.vox_k0    = pxAt1m * _max(1.f, ps_r_vsm_tree_hull_vox_px) * bias;
        cullPush.vox_k1    = pxAt1m * ps_r_vsm_tree_hull_vox_far * 0.01f * bias;
        cullPush.vox_fade  = ps_r_vsm_tree_hull_vox_fade;
        // r_vsm_tree_hull_debug keeps its old job: cube EVERY backed crown at any range,
        // which with r_tree_vox_dist 0 draws them OVER the mesh — that is how the tier's
        // cost was measured before it replaced anything, and it stays available.
        cullPush.vox_all   = (ps_r_vsm_tree_hull_debug != 0) ? 1u : 0u;
        cullPush.vox_count = numGroups;   // the spare slot appended in CreateIndirectBuffers
        // ⚠ ONE gate, checked BEFORE the cut reaches the shader. The cut and the
        // replacement must live or die together: a frame that cuts crowns from the mesh
        // and cannot draw their cubes renders a forest with holes in it. Anything that
        // can be missing — the species table, the buffers, the pipeline — zeroes the cut.
        if (!voxReady) { cullPush.vox_cut = 0.f; cullPush.vox_all = 0u; }
    }
    const bool voxActive = voxReady && (cullPush.vox_cut > 0.f || cullPush.vox_all != 0u);
    cullPush.eye_x = RV().vCameraPosition.x;
    cullPush.eye_y = RV().vCameraPosition.y;
    cullPush.eye_z = RV().vCameraPosition.z;
    // Tree ledger (r_tree_stats). scr_h + the hzb_focal already in the push are what
    // turn a bounding sphere into a SCREEN SIZE, which is the axis the LOD question
    // is actually asked on — distance only stands in for it at a fixed FOV.
    const bool statsOn = StatsEnabled() && m_CullStats != nullptr;
    cullPush.scr_h    = (float)VK::LodScreenHeight();   // r_lod_render_res: colour cull rasterises into the scene extent
    cullPush.stats_on = statsOn ? 1u : 0u;

    // ----- 1b) Hi-Z occlusion (r_tree_hzb). We deliberately do NOT build the
    // pyramid ourselves: Pass_World builds it from the finished prepass depth
    // (which trees are part of) and runs BEFORE this pass. If it did not build
    // this frame, the only pyramid available is last frame's — culling against a
    // stale camera would pop trees in and out, so we simply skip the test.
    // ⚠ The prepass keeps drawing the full frustum set on purpose: it IS the
    // pyramid's source. See the argument in tree_cull.comp.glsl for why culling
    // only the colour set cannot leave unshaded pixels here.
    cullPush.viewProj  = vp;
    cullPush.hzb_focal = 1.0f / _max(0.05f, tanf(deg2rad(RV().fFOV) * 0.5f));
    cullPush.hzb_on    = 0;
    VkImageView hzbView = VK_NULL_HANDLE;
    if (ps_r_tree_hzb && RImplementation.Details
        && RImplementation.Details->HZBReady() && RImplementation.Details->HZBBuiltThisFrame())
    {
        hzbView = RImplementation.Details->HZBView();
        if (hzbView != VK_NULL_HANDLE) cullPush.hzb_on = 1;
    }
    // The descriptor must ALWAYS point at something valid, even with the test off
    // (an unbound combined-image-sampler is UB the moment the shader is dispatched,
    // regardless of whether the branch reading it is taken).
    if (hzbView == VK_NULL_HANDLE && RImplementation.Details)
        hzbView = RImplementation.Details->DummyHZBView();
    // Key the cache on view + GENERATION: after DestroyHZB+CreateHZB the driver can
    // hand back the same numeric VkImageView, and a handle-only check would then keep
    // a descriptor bound to the destroyed object.
    const u32 hzbGen = RImplementation.Details ? RImplementation.Details->HZBGeneration() : 0u;
    if (hzbView == VK_NULL_HANDLE) {
        cullPush.hzb_on = 0;                       // no pyramid, no dummy — never sample
    } else if (hzbView != m_CullHzbView || hzbGen != m_CullHzbGen) {
        // ⚠ ONLY on change. m_CullDescSet is a SINGLE set while kFramesInFlight
        // frames are in flight, so rewriting it every frame would update a set
        // that an executing command buffer still references (UB — the same trap
        // the VSM cascade rebind documents). The view changes only when
        // CDetailManager recreates the pyramid (resize), and the device is idle
        // across that, so a write here is safe.
        // ⚠ The layout depends on WHICH image this is. The real pyramid is compute-
        // written and sits in GENERAL; the 1x1 dummy bound when the test is off sits in
        // SHADER_READ_ONLY_OPTIMAL. This used to write GENERAL unconditionally, which
        // was correct only because the dummy path had never been run under validation
        // — see the same fix in the prepass cull set (VUID-vkCmdDraw-None-09600).
        const bool isDummy = RImplementation.Details && hzbView == RImplementation.Details->DummyHZBView();
        VK::DescriptorWriter(m_CullDescSet)
            .ImageSampler(4, hzbView, RImplementation.Details->HZBSampler(),
                          isDummy ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL)
            .Flush();
        m_CullHzbView = hzbView;
        m_CullHzbGen  = hzbGen;
    }

    // ----- 2) Clear per-group draw counts; barrier transfer→compute. ----------
    // Same cross-frame WAR hazard as grass: m_TreeIndirectBuffer and
    // m_TreeDrawCountBuffer are single-buffered while 3 frames are in flight.
    // Order the previous frame's indirect fetches before this frame's
    // fill/compute rewrites (execution dependency is enough for WAR).
    {
        VkMemoryBarrier b{};
        b.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &b, 0, nullptr, 0, nullptr);
    }
    // ⚠ A per-frame readback of the visible-tree bitset used to sit here: two
    // pipeline barriers, a buffer copy and a fill, every frame the profiler was on —
    // to snapshot a buffer nothing had written since the marking atomic was taken
    // out of tree.frag. It paid GPU work in exactly the mode the numbers come from.
#ifdef __SWITCH__
    // nfs36 nx_diag 190: the cull frozen -- no count reset and no per-group dispatches; the indirect commands of the
    // last culled frame are drawn again (camera still = the same trees). Prices the 86 dispatches + their work.
    const bool nxCullFrozen = (g_xr_nx_diag == 190);
#else
    const bool nxCullFrozen = false;
#endif
    if (!nxCullFrozen)
        vkCmdFillBuffer(cmd, m_TreeDrawCountBuffer->GetHandle(), 0, VK_WHOLE_SIZE, 0u);
    // The cull ledger is a PER-FRAME tally, so it is zeroed here and read after the
    // dispatch — the same transfer→compute barrier below covers both.
    if (statsOn)
        vkCmdFillBuffer(cmd, m_CullStats->GetHandle(), 0, VK_WHOLE_SIZE, 0u);
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }

    // ----- 3) Cull dispatch per group. ----------------------------------------
    // Own zone at r_profiler 2: the `Trees` zone has always billed the cull compute
    // and the raster as one number, and they answer to completely different knobs.
    const bool zoneSub = ps_r_profiler >= 2;
    const int  zCull   = zoneSub ? VK::Prof::ZoneBegin(cmd, "Trees/Cull") : -1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CullPipelineLayout,
                            0, 1, &m_CullDescSet, 0, nullptr);
    for (u32 g = 0; g < (nxCullFrozen ? 0u : numGroups); ++g)
    {
        const TreeIndirectGroup& grp = m_Groups[g];
        // planes, cut_dist and eye_* stay as set above (frame-constant); only the
        // per-group indices change. This slot used to be `_unused` and was zeroed
        // here every group — it now carries cut_dist, so leave it alone.
        cullPush.mesh_count  = grp.meshCount;
        cullPush.mesh_offset = grp.meshOffset;
        cullPush.output_base = g * m_MaxGroupMeshCount;
        cullPush.count_index = g;
        vkCmdPushConstants(cmd, m_CullPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(cullPush), &cullPush);
        vkCmdDispatch(cmd, (grp.meshCount + 255) / 256, 1, 1);
    }
    if (zCull >= 0) VK::Prof::ZoneEnd(cmd, zCull);

    // ----- 3b) Per-tree probe field (r_tree_gi_inst). Recorded here because this is
    // already outside the render pass and already the compute part of the frame; it
    // reads only the metadata buffer, so it is independent of the cull above and the
    // two can overlap. Its own barrier (COMPUTE -> VERTEX) is inside DispatchTreeGI.
    DispatchTreeGI(cmd);

    // ----- 4) Barrier compute→draw-indirect (+transfer for the count readback).
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        // SHADER_READ|VERTEX too: the cube tier's per-draw payload (m_VoxDrawParams)
        // is written by this same dispatch and read in tree_vox.vert, so the indirect
        // fetch is not the only consumer that has to be ordered after it.
        b.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT
                        | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                             | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }

    // ----- 4a) Cull ledger readback (r_tree_stats). Same "no fence, read it stale"
    // contract as the draw counts below: this is steady-state telemetry, and stalling
    // the frame to make it current would change the thing being measured.
    if (statsOn && m_CullStatsRB) {
        VkBufferCopy sc{ 0, 0, (VkDeviceSize)kTreeStatCount * sizeof(u32) };
        vkCmdCopyBuffer(cmd, m_CullStats->GetHandle(), m_CullStatsRB->GetHandle(), 1, &sc);
    }

    // ----- 4b) Forward drawn-count readback (r_profiler): mirror this frame's
    // per-group counts to the host (read a frame or two stale — diagnostic) and
    // log the total every 2 s: the "how many trees do we actually draw" number
    // the VSM-epic backlog asked for. Only the main-view cull above writes these
    // counts (the shadow paths draw per-mesh on the CPU), so this is main-view.
    if (ps_r_profiler > 0) {
        // numGroups + 1: the last slot is the cube tier's draw count, so the A/B of
        // r_tree_vox_dist reads BOTH halves of the handover off one line — how many
        // crowns left the mesh and how many arrived as cubes.
        if (!m_FwdCountRB) {
            m_FwdCountRB = xr_new<CVulkanBuffer>();
            m_FwdCountRB->Create((VkDeviceSize)(numGroups + 2) * sizeof(u32),
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            m_FwdCountPtr = (u32*)m_FwdCountRB->Map();
            if (m_FwdCountPtr) memset(m_FwdCountPtr, 0, (size_t)(numGroups + 2) * sizeof(u32));
        }
        if (m_FwdCountPtr) {
            VkBufferCopy rc{ 0, 0, (VkDeviceSize)(numGroups + 2) * sizeof(u32) };
            vkCmdCopyBuffer(cmd, m_TreeDrawCountBuffer->GetHandle(), m_FwdCountRB->GetHandle(), 1, &rc);
            // ⚠ dwTimeContinual, NOT dwTimeGlobal. Every sweep this instrument exists to
            // serve runs with `time_factor 0.001` (the scene must not animate between
            // steps), and the GAME clock is frozen there — this line simply stopped
            // printing at the exact moment it was needed. Same fix, same reason, as the
            // one [VK Perf] already had to make.
            if (RV().dwTimeContinual > m_FwdLastLog + 2000) {
                m_FwdLastLog = RV().dwTimeContinual;
                u64 drawn = 0; u32 nonEmpty = 0, maxGrp = 0;
                for (u32 g = 0; g < numGroups; ++g) {
                    const u32 c = m_FwdCountPtr[g];
                    drawn += c;
                    if (c) { ++nonEmpty; maxGrp = _max(maxGrp, c); }
                }
                // cut= is stamped into the line on purpose: an A/B of r_tree_dist
                // compares two logs, and "which mode was this run in" must be IN
                // the measurement, not remembered alongside it.
                m_VoxFwdDraws = m_FwdCountPtr[numGroups];
                m_VoxFwdCubes = m_FwdCountPtr[numGroups + 1];
                // ⚠ There is no `visible` count here any more, and that is deliberate:
                // it came from a per-tree bitset tree.frag marked with an atomicOr, and
                // a storage write in a fragment shader costs the pass its EARLY DEPTH
                // TEST — decided at pipeline compile time, so no runtime gate saves it.
                // Measured, same scene, same 890 k surviving samples: 20.1 M fragment
                // invocations at 2.81 ms with the atomic, 6.2 M at 1.25 ms without.
                // The instrument cost more than most things it was used to hunt. The
                // honest replacement is the hardware counters in `vk_trees`.
                Msg("[VK Trees] forward drawn: %llu of %u instances | %u/%u groups, max group %u | cut=%.0fm flod-backed=%u shade=%.0fm hzb=%d | CUBES vox=%.0fm draws=%u cubes=%u backed=%u",
                    (unsigned long long)drawn, m_TotalCount, nonEmpty, numGroups, maxGrp,
                    TreeFlodCutDist(), m_FlodBackedCount, ps_r_tree_shade_dist, ps_r_tree_hzb,
                    TreeVoxCutDist(), m_VoxFwdDraws, m_VoxFwdCubes, m_VoxBackedCount);
            }
        }
    }

    // ----- 5) Begin dynamic-rendering pass (LOAD color + depth from world). ----
    VK::BeginOverlayRendering(cmd, ctx);   // shared overlay begin — see vk_pass_context.h
#ifdef __SWITCH__
    // nfs30 nx_diag 131: every tree draw still sent, through a 1x1 scissor -- the frame drops by their fragment cost;
    // the cull dispatches, the vertex work and the per-group indirect draws stay.
    if (g_xr_nx_diag == 131) { const VkRect2D one{ { 0, 0 }, { 1u, 1u } }; vkCmdSetScissor(cmd, 0, 1, &one); }
#endif

    // Push constants + transforms (set 0) once — shared across both variants.
    TreeGfxPush pc{};
    pc.mViewProj = vp;
    // Tree UVs are quantized with FTreeVisual_quant = 32768/16 = 2048
    // (FTreeVisual.cpp tree_data consts.xy), NOT the static-geometry 1/1024.
    pc.uvScale   = 1.0f / 2048.0f;
    pc.alphaRef  = TreeAlphaRef(200.0f / 255.0f);
    // R1 crossfade of the crown (was the reserved statsOn slot -- same offset, so the depth caster and the hull debug
    // view that share this block are untouched; they zero it). Set below, after the sun boost scales vSunColor.
    pc.lodBand   = 0.0f;
    // r_tree_shade_dist — ONLY the forward colour pass gets this. The depth caster
    // and the hull debug view share this push block but have no shading to drop, and
    // the shadow passes must not thin out by camera distance at all.
    pc.shadeDist = ps_r_tree_shade_dist;
    // Env lighting (colorize the baked per-tree hemi factor + open-sky sun term so
    // foliage tracks time-of-day like the world). Neutral fallback if env not up.
    pc.vSunColor.set(0.6f, 0.6f, 0.6f, 0.0f);
    pc.vHemiColor.set(0.45f, 0.45f, 0.45f, 0.0f);
    // SSFX trunk wind (screenspace_wind.h): direction/velocity drive the sway,
    // drift from the shared Environment.wind_anim. Defaults match SSFX "10 - Wind"
    // (branches 11.0, trunk 0.15, bend 0.5, min wind speed 0.1).
    pc.wsetup_trees.set(ps_r_wind_tree_anim, ps_r_wind_tree_trunk, ps_r_wind_tree_bend, 0.1f);
    if (g_pGamePersistent) {
        auto& envMgr = ES();
        const Fvector3 wa = envMgr.wind_anim;
        pc.wind_anim.set(wa.x, wa.y, wa.z, ps_r_wind_tree_flutter);   // w = crown flutter amplitude
        if (auto* E = envMgr.CurrentEnv) {
            pc.vSunColor.set(E->sun_color.x, E->sun_color.y, E->sun_color.z, 0.0f);
            pc.vHemiColor.set(E->hemi_color.x, E->hemi_color.y, E->hemi_color.z, 0.0f);
            pc.wind_params.set(E->wind_direction, E->wind_velocity, 0.0f, 0.0f);
        }
    }
    pc.wind_params.w = ps_r_wind_tree_crown;   // leaf-flutter fade-in height (gate the low trunk)
    // Linearise before the boost (see vk_env_light / vk_DetailManager_Render): trees,
    // like grass, take their env colours by push constant and never see the LightUBO.
    VK::ColorSpace::LinearizeRGB(pc.vSunColor);
    VK::ColorSpace::LinearizeRGB(pc.vHemiColor);
    // nfs18: pc.vHemiColor is read by no tree shader except tree_skypush.frag (TREE_SKY_PUSH) and tree_r1.vert
    // (TREE_R1), which take its .rgb as the up-facing sky irradiance: the CPU-evaluated SH value (already linear) once
    // it exists, the linearised env hemi colour -- R1's L_hemi_color -- before that. The full tree.frag/tree.vert
    // never read it, so on the default path this block changes nothing on screen.
    {
        Fvector up;
        if (VK::EnvLight::SkyAmbientUpCPU(up)) pc.vHemiColor.set(up.x, up.y, up.z, 1.f);
        pc.vHemiColor.x *= ps_r_tree_sky_boost; pc.vHemiColor.y *= ps_r_tree_sky_boost; pc.vHemiColor.z *= ps_r_tree_sky_boost;
#ifdef __SWITCH__
        static u32 s_skyLog = 0;   // nfs19: the scale of the term, to calibrate r_tree_sky_boost against what the eye prefers
        if ((++s_skyLog % 600) == 1)
            Msg("[nx tree] crown sky fill (%.3f %.3f %.3f) %s x%.2f | mesh cut %.0f m, billboards from %.0f m", pc.vHemiColor.x,
                pc.vHemiColor.y, pc.vHemiColor.z, pc.vHemiColor.w > 0.5f ? "SH-up" : "env-hemi", ps_r_tree_sky_boost,
                TreeFlodCutDist(), ImposterMinDist());
#endif
    }
    // Same global sun boost as the world (vk_env_light premultiplies it into
    // the LightUBO; the tree sun colour travels via push constants).
    pc.vSunColor.mul(ps_r_sun_boost);

    // R1 crossfade (r_tree_lod_size 1 + r_tree_lod_band > 1, the Switch preset). Over [D_A, D_A*band] of its group
    // (D_A = r_tree_mesh_dist * k, the GROUP centre) the billboard dithers IN (lod_imposter*.frag) while tree_cull
    // still draws the mesh to D_A*band -- so without this the swaying crown stood fully drawn inside an almost
    // opaque static billboard. The crown now dithers OUT with the same noise: each pixel shows one of the two.
    // ⚠Off with the tree depth prepass: its depth would still hold the discarded pixels and hide the billboard.
    if (TreeLodSizeOn() && TreeLodBand() > 1.0001f && TreeFlodCutDist() > 0.f && !ps_r_tree_prepass
        && m_TreeLodCBuffer && m_TreeLodCBuffer->GetHandle() != VK_NULL_HANDLE)
    {
        pc.lodBand     = TreeLodBand();
        pc.vSunColor.w = ImposterMinDist();   // the billboard's own D_A base, not r_tree_dist
    }

    // Roll wind history for the MV pass: this frame's wind → cur, last frame's cur →
    // prev (the sway delta). First frame → prev == cur (zero sway motion). Idempotent
    // per frame — under r_mv_prepass the prepass already rolled it (see RollMvWind).
    RollMvWind(pc.wind_params, pc.wsetup_trees, pc.wind_anim);

    vkCmdPushConstants(cmd, m_GfxPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GfxPipelineLayout,
                            0, 1, &m_XformDescSet, 0, nullptr);
    // set 2 = shared env lighting (sun_vp + sun shadow map) — updated by Pass_World.
    if (VkDescriptorSet envSet = VK::EnvLight::GetCurrentSet())
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GfxPipelineLayout,
                                2, 1, &envSet, 0, nullptr);

    // ----- 6) Per-group indirect draw. ----------------------------------------
    // The ledger brackets the MESH draws only — the cube tier gets its own slot below.
    // A query around the whole pass would merge two representations whose entire
    // reason for existing is that they cost differently.
    // ---- CROWN VARIANT (r_tree_perm) -----------------------------------------
    // Pick the crown compiled with exactly the terms this frame's world can still
    // produce. Decided ONCE, before the group loop: vkCreateGraphicsPipelines
    // during recording is legal, but a build belongs at one point in the frame,
    // not once per group.
    //
    // ⚠ The cache is keyed on the VS/FS modules as well as the mask, because
    // r_tree_gi_inst and r_tree_gather choose different ones. Keyed on the mask
    // alone, flipping either cvar would hand back the variant built from the
    // shader it just switched AWAY from — and in a sweep that reads as "the
    // change did nothing", the one answer an A/B must never be allowed to fake.
    const bool giInst = (ps_r_tree_gi_inst != 0 && m_GfxPipeline24GIInst != VK_NULL_HANDLE
                         && m_GIPipeline != VK_NULL_HANDLE);
    const bool noGath = (ps_r_tree_gather == 0 && m_GfxPipeline24NoGather != VK_NULL_HANDLE);
    VkShaderModule permVS = giInst ? m_CrownVSGI     : m_CrownVS;   // resolved at load, not by name per frame
    VkShaderModule permFS = noGath ? m_CrownFSNoGath : m_CrownFS;

    u32 wantMask = TS_ALL;
    if (ps_r_tree_terms >= 0) {
        wantMask = (u32)ps_r_tree_terms & TS_ALL;                                 // probe forces a mask
    } else if (ps_r_tree_perm != 0) {
        wantMask = VK::EnvLight::TreeSpecMask() & TS_ALL;
        // T_SUN is decided HERE and not in the UBO, because the crown's sun is
        // pc.vSunColor — a push constant this pass filled in a few lines ago — and
        // not L.sun_color. The two disagree the moment r_sky_sun_from_atmo replaces
        // the UBO's sun with atmospheric transmittance. The FS gate is
        // `dot(vSunLit, vSunLit) > 0` and vSunLit is this colour times a hemi factor
        // clamped to [0.3, 1.5], so a black push constant is exactly the case where
        // the shadow tap can never change a pixel.
        if (pc.vSunColor.x + pc.vSunColor.y + pc.vSunColor.z > 0.001f) wantMask |= TS_SUN;
        else                                                          wantMask &= ~(u32)TS_SUN;
    }

    VkPipeline permP24 = VK_NULL_HANDLE, permP28 = VK_NULL_HANDLE;
    if (wantMask != TS_ALL) {
        if (const PermPipes* pp = FindPermPipes(wantMask, permVS, permFS)) {
            permP24 = pp->p24; permP28 = pp->p28;
        } else if (ps_r_tree_terms >= 0) {
            // A probe step may cost ONE hitch — a sweep that measured the frame
            // the variant was still building would measure the build.
            if (BuildPermPipes(wantMask, permVS, permFS))
                if (const PermPipes* np = FindPermPipes(wantMask, permVS, permFS)) {
                    permP24 = np->p24; permP28 = np->p28;
                }
        }
    }
    // Queue what is missing and drain ONE pair per frame. Nothing here stalls: a
    // mask that is not built yet draws the FULL crown for a frame or two, which is
    // the shader that shipped before the split — slower, never wrong.
    if (ps_r_tree_perm != 0 && ps_r_tree_terms < 0 && permVS && permFS) {
        auto queue = [&](u32 m) {
            if (m == TS_ALL || FindPermPipes(m, permVS, permFS)) return;
            for (u32 q : m_PermPending) if (q == m) return;
            m_PermPending.push_back(m);
        };
        // ⚠ BUILD ON A MASK THAT HAS HELD STILL, NOT ON EVERY CHANGE. Measured
        // 26-08: a level settling walks the mask through 0x212 -> 0x216 -> 0x213 ->
        // ... -> 0x37E in about a second and a half as AO ramps, the probe volume
        // goes live and lights arrive. Building each step, with four prewarm flips
        // apiece, filled 15 of the 32 cache slots with states nobody would be in
        // again — and the slot after the last one is where the crown silently
        // reverts to the full shader for the rest of the level.
        constexpr u32 kPermSettle  = 30;    // ~0.5 s: build the live mask
        constexpr u32 kPermPrewarm = 120;   // ~2 s:   then the flips it may take next
        if (wantMask != m_PermLastWant) {
            m_PermLastWant  = wantMask;
            m_PermStable    = 0;
            m_PermPrewarmed = false;
            // Anything queued for the state we just left is work for a mask nobody
            // is in. The one we ARE in gets queued below, on its own schedule.
            m_PermPending.clear();
        } else if (++m_PermStable == kPermSettle) {
            queue(wantMask);
        } else if (m_PermStable == kPermPrewarm && !m_PermPrewarmed) {
            m_PermPrewarmed = true;
            // PREWARM the bits that flip DURING play, so "it started raining" and
            // "a lamp came into reach" cost a pipeline build that already happened.
            // The world pass prewarms exactly the same weather flips for terrain.
            // Combinations are deliberately NOT enumerated: 2^4 masks to save the
            // rarer transitions a frame of full crown is not a trade worth 32
            // pipelines. Single flips cover what actually happens minute to minute.
            queue(wantMask ^ TS_WET);
            queue(wantMask ^ TS_SNOW);
            queue(wantMask ^ TS_DYN);
            queue(wantMask ^ TS_SUN);
        }
        // FRONT, not back: the mask actually wanted was queued first and must be
        // built first — prewarming the flips ahead of the live one would leave the
        // crown on the full shader while the cache filled with states nobody is in.
        if (!m_PermPending.empty()) {
            const u32 m = m_PermPending.front();
            m_PermPending.erase(m_PermPending.begin());
            BuildPermPipes(m, permVS, permFS);
        }
    }

    // ⚠ WHICH CROWN FS IS ACTUALLY BOUND — printed once per change, never per frame.
    // Every twin here falls back to the shipping pipeline when its .spv is missing or
    // its build failed, and a silent fallback reads in a sweep as "the change did
    // nothing" — which is the one answer an A/B must never be allowed to fake. This
    // line is what tells a null delta from an unbound pipeline.
    {
        static int s_lastFS = -999;
        int fs = 0;                                                    // 0 = shipping
        if (giInst)                                                    fs = 2;
        if (noGath)                                                    fs = 1;
        if (permP24 != VK_NULL_HANDLE)                                 fs = 100 + (int)wantMask;
        if (ps_r_tree_flat)                                            fs = 10 + ps_r_tree_flat;
        if (fs != s_lastFS) {
            s_lastFS = fs;
            if      (fs == 0)   Msg("[VK Trees] crown FS: FULL (tree.frag, every term)");
            else if (fs == 1)   Msg("[VK Trees] crown FS: NO-GATHER (r_tree_gather 0)");
            else if (fs == 2)   Msg("[VK Trees] crown VS: PER-TREE GI (r_tree_gi_inst 1)");
            else if (fs >= 100) {
                string256 off; DescribePermMask(wantMask, off, sizeof(off));
                Msg("[VK Trees] crown FS: VARIANT 0x%03X — OFF: %s%s", wantMask, off,
                    ps_r_tree_terms >= 0 ? " (forced by r_tree_terms)" : "");
            }
            else                Msg("[VK Trees] crown FS: FLAT probe %d", fs - 10);
        }
        // A variant that was asked for and could not be bound is a louder problem
        // than a wrong number: the pass silently keeps the full crown, and the frame
        // time then says "the split is worth nothing" when it says nothing at all.
        if (ps_r_tree_terms >= 0 && permP24 == VK_NULL_HANDLE && wantMask != TS_ALL) {
            static bool warnedT = false;
            if (!warnedT) { warnedT = true; Msg("![VK Trees] r_tree_terms 0x%03X asked for, but its variant did not build", wantMask); }
        }
        // A request that could not be honoured is a louder problem than a wrong number.
        if (ps_r_tree_gather == 0 && m_GfxPipeline24NoGather == VK_NULL_HANDLE) {
            static bool warned = false;
            if (!warned) { warned = true; Msg("![VK Trees] r_tree_gather 0 asked for, but the no-gather pipeline is not built"); }
        }
    }

    // nfs31 (r_tree_r1, Switch preset): the R1 crown reproduces the sun + sky + fog variant, light per vertex (nfs43:
    // it samples the cascades/far map itself). Anything else the world asks for (wet, snow, dyn lights, ...) or a VSM
    // mask -> the full crown shader. Off by default: the PC keeps the per-pixel crown.
    // 10-09: + the dynamic lights (tree_r1.vert, per vertex, unshadowed) -- the torch used to send every crown to the
    // full shader. r_tree_r1_dyn 0 = the old fallback, for the A/B.
    // 10-10: + the wet state (r_tree_r1_wet): rain sent every crown to the full shader, 0.45 -> 1.5-3.6 units.
    const u32  r1Terms = (u32)(TS_SUN | TS_SKY | TS_FOG) | (ps_r_tree_r1_dyn ? (u32)TS_DYN : 0u) | (ps_r_tree_r1_wet ? (u32)TS_WET : 0u);
    bool r1Crown = ps_r_tree_r1 != 0 && m_GfxPipeline24R1 != VK_NULL_HANDLE && m_GfxPipeline28R1 != VK_NULL_HANDLE
        && ps_r_tree_perm != 0 && ps_r_tree_terms < 0 && ps_r_tree_zwrite != 0
        && (wantMask & ~r1Terms) == 0 && (wantMask & TS_SKY) && (wantMask & TS_FOG)
        && !VK::VSM::MaskReady();
#ifdef __SWITCH__
    if (g_xr_nx_diag == 140) r1Crown = false;   // nx window: the full crown for the A/B
    {
        static int s_lastR1 = -1;
        if (int(r1Crown) != s_lastR1) {
            s_lastR1 = int(r1Crown);
            Msg("[nx tree] crown shader: %s (mask 0x%03X)", r1Crown ? "R1 (light per vertex)" : "full tree.frag", wantMask);
        }
    }
#endif
    const int zMesh = zoneSub ? VK::Prof::ZoneBegin(cmd, "Trees/Mesh") : -1;
    StatsBeginPass(cmd, TP_COLOR);
    VkPipeline      lastPipe = VK_NULL_HANDLE;
    VkDescriptorSet lastTex  = VK_NULL_HANDLE;
    for (u32 g = 0; g < numGroups; ++g)
    {
        const TreeIndirectGroup& grp = m_Groups[g];
        // r_tree_zwrite 0 selects the no-depth-write twin, live (both are built at load).
        const bool zw = ps_r_tree_zwrite != 0;
        VkPipeline pipe = (grp.tcOffset == 24) ? (zw ? m_GfxPipeline24 : m_GfxPipeline24NoZW)
                                               : (zw ? m_GfxPipeline28 : m_GfxPipeline28NoZW);
        // r_tree_gi_inst 1: the crown whose VS reads the precomputed per-tree field.
        // Applied before r_tree_gather so that, if both are set, the no-gather twin
        // wins and the log line below says which one is actually bound.
        if (ps_r_tree_gi_inst != 0 && m_GfxPipeline24GIInst != VK_NULL_HANDLE
            && m_GIPipeline != VK_NULL_HANDLE)   // no feeder -> do not bind the reader
            pipe = (grp.tcOffset == 24) ? m_GfxPipeline24GIInst : m_GfxPipeline28GIInst;
        // r_tree_gather 0: the no-gather crown. A rendering CHOICE, so it is applied
        // to the base pipeline first and both probes below still override it.
        if (ps_r_tree_gather == 0 && m_GfxPipeline24NoGather != VK_NULL_HANDLE)
            pipe = (grp.tcOffset == 24) ? m_GfxPipeline24NoGather : m_GfxPipeline28NoGather;
        // The crown VARIANT for this frame's world state (r_tree_perm), or the mask
        // r_tree_terms forced. Same state as the pair it replaces — it differs in
        // the specialization constant and nothing else.
        // ⚠ zwrite-on state only: r_tree_zwrite 0 is a probe of the depth-write
        // trade, and a probe must not silently also change which terms are compiled.
        // Checked BEFORE r_tree_flat so that flat, the cruder hammer, still wins
        // when both are on — two probes at once is a mistake, not a mode.
        if (permP24 != VK_NULL_HANDLE && zw)
            pipe = (grp.tcOffset == 24) ? permP24 : permP28;
        // r_tree_flat probe wins over the zwrite twin (its state is the zwrite-on
        // default) — it is a measurement mode, not a rendering option.
        if (r1Crown) pipe = (grp.tcOffset == 24) ? m_GfxPipeline24R1 : m_GfxPipeline28R1;
        if (ps_r_tree_flat) {
            VkPipeline flat = (grp.tcOffset == 24) ? m_GfxPipeline24Flat : m_GfxPipeline28Flat;
            if (flat != VK_NULL_HANDLE) pipe = flat;
        }
        if (pipe == VK_NULL_HANDLE) pipe = (grp.tcOffset == 24) ? m_GfxPipeline24 : m_GfxPipeline28;
        if (pipe == VK_NULL_HANDLE) continue;

        if (pipe != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            lastPipe = pipe;
        }
        if (grp.descSetIdx < m_TexDescSets.size()) {
            VkDescriptorSet tex = m_TexDescSets[grp.descSetIdx];
            if (tex != lastTex) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GfxPipelineLayout,
                                        1, 1, &tex, 0, nullptr);
                lastTex = tex;
            }
        }

        VkDeviceSize vbOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &vbOff);
        vkCmdBindIndexBuffer(cmd, grp.ib, 0, VK_INDEX_TYPE_UINT16);

        const VkDeviceSize cmdOff = (VkDeviceSize)g * m_MaxGroupMeshCount * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)g * sizeof(u32);
        vkCmdDrawIndexedIndirectCount(cmd,
            m_TreeIndirectBuffer->GetHandle(), cmdOff,
            m_TreeDrawCountBuffer->GetHandle(), cntOff,
            m_MaxGroupMeshCount, sizeof(VkDrawIndexedIndirectCommand));
    }
    StatsEndPass(cmd, TP_COLOR);
    if (zMesh >= 0) VK::Prof::ZoneEnd(cmd, zMesh);

    // ----- 7) CROWN VOXEL TIER (r_tree_vox_dist) — the middle rung of the tree LOD
    // ladder: mesh → baked cube cloud → FLOD billboard. tree_cull.comp already picked
    // the crowns and their LOD level in the same dispatch that culled the mesh, so this
    // is one indirect draw over its output, with the count sharing the mesh count
    // buffer's spare last slot. The cubes are OPAQUE and sample nothing — that, not the
    // triangle count, is where the saving comes from on a fill-bound pass.
    //
    // The cube VS/FS ride this pass's own pipeline layout and TreeGfxPush (viewProj,
    // sun/hemi, wind), so no state beyond the pipeline changes here.
    //
    // ⚠ This draw MUST cover exactly what the mesh cull dropped. Both sides read
    // VK::TreeVoxCutDist() and both measure from the sphere's near side, in the same
    // shader — see the vox_cut block in tree_cull.comp.glsl.
    if (voxActive) {
        const int zVox = zoneSub ? VK::Prof::ZoneBegin(cmd, "Trees/Vox") : -1;
        StatsBeginPass(cmd, TP_VOX);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VoxFwdPipe);
        vkCmdDrawIndirectCount(cmd, m_VoxDrawCmds->GetHandle(), 0,
                               m_TreeDrawCountBuffer->GetHandle(),
                               (VkDeviceSize)numGroups * sizeof(u32),
                               kVoxDrawCap, sizeof(VkDrawIndirectCommand));
        StatsEndPass(cmd, TP_VOX);
        if (zVox >= 0) VK::Prof::ZoneEnd(cmd, zVox);
    }

    // Hull/lobe overlay — the r_vsm_tree_hull_debug view for levels baked WITHOUT a
    // voxel cloud (r_vsm_tree_hull_vox 0). With a cloud the cube tier above already
    // shows the crowns, so this only fires when there is nothing else to show.
    if (ps_r_vsm_tree_hull_debug && !voxActive && m_HullDebugPipe != VK_NULL_HANDLE
        && m_HullVB && m_HullIB && m_HullInfoCPU.size() == m_TotalCount
        && m_VsmNearCPU.size() == m_TotalCount)
    {
        const bool allHulls = ps_r_vsm_tree_hull_debug >= 2;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_HullDebugPipe);
        VkDeviceSize vbOff = 0;
        VkBuffer hvb = m_HullVB->GetHandle();
        vkCmdBindVertexBuffers(cmd, 0, 1, &hvb, &vbOff);
        vkCmdBindIndexBuffer(cmd, m_HullIB->GetHandle(), 0, VK_INDEX_TYPE_UINT16);
        for (u32 i = 0; i < m_TotalCount; ++i) {
            if (!allHulls && (m_VsmNearCPU[i] & 6) == 0) continue;
            const GpuTreeHullInfo& h = m_HullInfoCPU[i];
            if (h.ib_count == 0) continue;
            vkCmdDrawIndexed(cmd, h.ib_count, 1, h.ib_first, (s32)h.vb_first, i);
        }
    }

    vkCmdEndRendering(cmd);

    static bool s_diag = false;
    if (!s_diag) {
        Msg("[VK Trees] First Render: %u trees in %u groups (maxGroupMesh=%u)",
            m_TotalCount, numGroups, m_MaxGroupMeshCount);
        s_diag = true;
    }

    // ----- 8) The ledger (r_tree_stats / `vk_trees`). Printed from here because this
    // is the one place per frame that is guaranteed to run after every tree pass has
    // recorded — the shadow casters, the prepass and the MV overlay all report into
    // counters this call then reads.
    // ⚠ dwTimeContinual, NOT dwTimeGlobal: every sweep this exists to serve runs with
    // `time_factor 0.001`, and the game clock is frozen there — the instrument would
    // go silent at exactly the moment it is needed. Same fix [VK Perf] already made.
    if (StatsEnabled() && (m_StatMark || RV().dwTimeContinual > m_StatLastLog + 5000)) {
        m_StatLastLog = RV().dwTimeContinual;
        m_StatMark    = false;
        StatsReport();
    }
}

// ============================================================================
// Wind-sway motion-vector overlay (tree_motion.{vert,frag}).
// ============================================================================
// Own pipeline layout: set0 = xform SSBO + s_waves (as forward), set1 = diffuse
// (alpha test), NO env set. MV color (RG16F), depth LEQUAL WITHOUT write, and NO
// depth bias — same as the forward tree draw (which uses no bias) so the cur pose's
// clip depth bit-matches → LEQUAL passes over the tree depth already in the buffer.
void CTreeManager::CreateMotionPipelines()
{
    if (m_MotionPipeline24 != VK_NULL_HANDLE || m_MotionPipeline28 != VK_NULL_HANDLE) return;
    if (!g_ShaderManager) return;
    if (m_XformDescLayout == VK_NULL_HANDLE || m_TexDescLayout == VK_NULL_HANDLE) return;

    VkShaderModule vs = g_ShaderManager->Load("tree_motion.vert.spv");
    VkShaderModule fs = g_ShaderManager->Load("tree_motion.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        Msg("![VK Trees] tree_motion.{vert,frag}.spv load failed — tree MV disabled");
        return;
    }

    VkDescriptorSetLayout setLayouts[2] = { m_XformDescLayout, m_TexDescLayout };
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0; pcr.size = sizeof(TreeMotionPush);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 2; plci.pSetLayouts = setLayouts;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &m_MotionPipelineLayout) != VK_SUCCESS) {
        Msg("![VK Trees] MV pipeline layout create failed"); return;
    }

    auto createVariant = [&](u32 tcOffset, VkPipeline& out)
    {
        out = VK::GfxPipelineBuilder(m_MotionPipelineLayout)
            .Vert(vs).Frag(fs)
            .Binding(0, 32)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
            .Cull(VK_CULL_MODE_NONE)             // double-sided leaves (matches forward)
            .Depth(true, false)                  // scene depth owns the surface
            .Color(VK::MotionVec::Format(), VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT)   // RG16F motion
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Trees MV (tcOff=%u)", tcOffset);
    };
    createVariant(24, m_MotionPipeline24);
    createVariant(28, m_MotionPipeline28);

    // r_mv_prepass twins: SAME shaders, same layout, same cull — depth WRITE on and
    // no bias, because here the pass is not an overlay onto finished depth, it IS the
    // depth prepass. The crowns' clip z/w is bit-identical to what tree_depth.vert
    // produced (the jitter re-applied to gl_Position only translates clip.xy), so the
    // forward tree pass's LEQUAL and everything else that tests this depth is unmoved.
    auto createDepthVariant = [&](u32 tcOffset, VkPipeline& out)
    {
        out = VK::GfxPipelineBuilder(m_MotionPipelineLayout)
            .Vert(vs).Frag(fs)
            .Binding(0, 32)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
            .Cull(VK_CULL_MODE_NONE)
            .Depth(true, true)                   // THIS raster lays the canopy depth
            .Color(VK::MotionVec::Format(), VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT)
            .DepthTarget(Swapchain.m_DepthFormat)
            .Build("Trees MV prepass (tcOff=%u)", tcOffset);
    };
    createDepthVariant(24, m_MvDepthPipeline24);
    createDepthVariant(28, m_MvDepthPipeline28);
    Msg("[VK Trees] MV pipelines OK (overlay + prepass twins: 24=%s 28=%s)",
        m_MvDepthPipeline24 ? "ok" : "FAIL", m_MvDepthPipeline28 ? "ok" : "FAIL");
}

// Roll the wind history ONCE per frame. Under r_mv_prepass the depth prepass asks
// first (it now needs the prev pose to write motion); the forward pass then finds
// the frame already stamped and reuses what was rolled, instead of rolling again
// and setting prev := cur — which would zero the sway motion silently.
void CTreeManager::RollMvWind(const Fvector4& windParams, const Fvector4& wsetup, const Fvector4& windAnim)
{
    if (m_MvWindFrame == RV().frameIndex) return;
    m_MvWindParamsPrev = m_MvWindValid ? m_MvWindParams : windParams;
    m_MvWsetupPrev     = m_MvWindValid ? m_MvWsetup     : wsetup;
    m_MvWindAnimPrev   = m_MvWindValid ? m_MvWindAnim   : windAnim;
    m_MvWindParams = windParams;
    m_MvWsetup     = wsetup;
    m_MvWindAnim   = windAnim;
    m_MvWindValid  = true;
    m_MvWindFrame  = RV().frameIndex;
}

void CTreeManager::DestroyMotionPipelines()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    VkDevice dev = VulkanHW.m_Device;
    if (m_MotionPipeline24)     { vkDestroyPipeline(dev, m_MotionPipeline24, nullptr); m_MotionPipeline24 = VK_NULL_HANDLE; }
    if (m_MotionPipeline28)     { vkDestroyPipeline(dev, m_MotionPipeline28, nullptr); m_MotionPipeline28 = VK_NULL_HANDLE; }
    if (m_MvDepthPipeline24)    { vkDestroyPipeline(dev, m_MvDepthPipeline24, nullptr); m_MvDepthPipeline24 = VK_NULL_HANDLE; }
    if (m_MvDepthPipeline28)    { vkDestroyPipeline(dev, m_MvDepthPipeline28, nullptr); m_MvDepthPipeline28 = VK_NULL_HANDLE; }
    if (m_MotionPipelineLayout) { vkDestroyPipelineLayout(dev, m_MotionPipelineLayout, nullptr); m_MotionPipelineLayout = VK_NULL_HANDLE; }
}

// Re-draws THIS frame's visible trees (same indirect buffers the forward pass
// filled) into the MV target, reprojecting the cur AND prev wind pose. Runs INSIDE
// MotionVec::ExecuteDynamic's render pass (MV target + scene depth bound, viewport
// set). Same VP + wind + no bias → cur depth bit-matches → LEQUAL passes. No cull
// re-dispatch: the forward Render already filled the indirect/count buffers.
void CTreeManager::RenderMotion(const VK::FrameContext& ctx, const Fmatrix& curVP, const Fmatrix& prevVP,
                                float jitterNdcX, float jitterNdcY)
{
    if (!m_MvWindValid) return;                     // trees didn't render this frame
    if (ctx.cmd == VK_NULL_HANDLE) return;
    if (m_TreeIndirectBuffer == nullptr || m_TreeDrawCountBuffer == nullptr) return;

    CreateMotionPipelines();                        // lazy — needs xform + tex layouts (built by Build)
    if (m_MotionPipeline24 == VK_NULL_HANDLE && m_MotionPipeline28 == VK_NULL_HANDLE) return;

    const VkCommandBuffer cmd = ctx.cmd;
    const u32 numGroups = (u32)m_Groups.size();
    if (numGroups == 0) return;

    TreeMotionPush pc{};
    pc.curVP             = curVP;
    pc.prevVP            = prevVP;
    pc.uvScale           = 1.0f / 2048.0f;          // matches the forward tree pass
    pc.alphaRef          = TreeAlphaRef(200.0f / 255.0f);
    pc.jitterX           = jitterNdcX;              // re-applied to gl_Position (jitter-free MV)
    pc.jitterY           = jitterNdcY;
    pc.wind_params       = m_MvWindParams;
    pc.wsetup_trees      = m_MvWsetup;
    pc.wind_anim         = m_MvWindAnim;
    pc.wind_params_prev  = m_MvWindParamsPrev;
    pc.wsetup_trees_prev = m_MvWsetupPrev;
    pc.wind_anim_prev    = m_MvWindAnimPrev;
    vkCmdPushConstants(cmd, m_MotionPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_MotionPipelineLayout,
                            0, 1, &m_XformDescSet, 0, nullptr);

    VkPipeline      lastPipe = VK_NULL_HANDLE;
    VkDescriptorSet lastTex  = VK_NULL_HANDLE;
    for (u32 g = 0; g < numGroups; ++g)
    {
        const TreeIndirectGroup& grp = m_Groups[g];
        VkPipeline pipe = (grp.tcOffset == 24) ? m_MotionPipeline24 : m_MotionPipeline28;
        if (pipe == VK_NULL_HANDLE) continue;
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; }

        if (grp.descSetIdx < m_TexDescSets.size()) {
            VkDescriptorSet tex = m_TexDescSets[grp.descSetIdx];
            if (tex != lastTex) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_MotionPipelineLayout,
                                        1, 1, &tex, 0, nullptr);
                lastTex = tex;
            }
        }

        VkDeviceSize vbOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &vbOff);
        vkCmdBindIndexBuffer(cmd, grp.ib, 0, VK_INDEX_TYPE_UINT16);

        const VkDeviceSize cmdOff = (VkDeviceSize)g * m_MaxGroupMeshCount * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)g * sizeof(u32);
        vkCmdDrawIndexedIndirectCount(cmd,
            m_TreeIndirectBuffer->GetHandle(), cmdOff,
            m_TreeDrawCountBuffer->GetHandle(), cntOff,
            m_MaxGroupMeshCount, sizeof(VkDrawIndexedIndirectCommand));
    }

    static bool s_diagMv = false;
    if (!s_diagMv) { s_diagMv = true; Msg("[VK Trees] first wind-sway MV render: groups=%u", numGroups); }
}

// ============================================================================
// Session B teardown — called from Destroy() before the buffers go.
// ============================================================================
void CTreeManager::DestroySessionB()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    VkDevice dev = VulkanHW.m_Device;

    DestroyMotionPipelines();   // MV overlay — built on the xform/tex layouts, free first
    if (m_GfxPipeline24)     { vkDestroyPipeline(dev, m_GfxPipeline24, nullptr); m_GfxPipeline24 = VK_NULL_HANDLE; }
    if (m_GfxPipeline24NoZW) { vkDestroyPipeline(dev, m_GfxPipeline24NoZW, nullptr); m_GfxPipeline24NoZW = VK_NULL_HANDLE; }
    if (m_GfxPipeline28NoZW) { vkDestroyPipeline(dev, m_GfxPipeline28NoZW, nullptr); m_GfxPipeline28NoZW = VK_NULL_HANDLE; }
    if (m_GfxPipeline28)     { vkDestroyPipeline(dev, m_GfxPipeline28, nullptr); m_GfxPipeline28 = VK_NULL_HANDLE; }
    if (m_GfxPipeline24Flat) { vkDestroyPipeline(dev, m_GfxPipeline24Flat, nullptr); m_GfxPipeline24Flat = VK_NULL_HANDLE; }
    if (m_GfxPipeline28Flat) { vkDestroyPipeline(dev, m_GfxPipeline28Flat, nullptr); m_GfxPipeline28Flat = VK_NULL_HANDLE; }
    if (m_GfxPipeline24R1)   { vkDestroyPipeline(dev, m_GfxPipeline24R1, nullptr);   m_GfxPipeline24R1 = VK_NULL_HANDLE; }
    if (m_GfxPipeline28R1)   { vkDestroyPipeline(dev, m_GfxPipeline28R1, nullptr);   m_GfxPipeline28R1 = VK_NULL_HANDLE; }
    if (m_GfxPipeline24NoGather) { vkDestroyPipeline(dev, m_GfxPipeline24NoGather, nullptr); m_GfxPipeline24NoGather = VK_NULL_HANDLE; }
    if (m_GfxPipeline28NoGather) { vkDestroyPipeline(dev, m_GfxPipeline28NoGather, nullptr); m_GfxPipeline28NoGather = VK_NULL_HANDLE; }
    if (m_GfxPipeline24GIInst) { vkDestroyPipeline(dev, m_GfxPipeline24GIInst, nullptr); m_GfxPipeline24GIInst = VK_NULL_HANDLE; }
    if (m_GfxPipeline28GIInst) { vkDestroyPipeline(dev, m_GfxPipeline28GIInst, nullptr); m_GfxPipeline28GIInst = VK_NULL_HANDLE; }
    if (m_GIPipeline)       { vkDestroyPipeline(dev, m_GIPipeline, nullptr);       m_GIPipeline = VK_NULL_HANDLE; }
    if (m_GIPipelineLayout) { vkDestroyPipelineLayout(dev, m_GIPipelineLayout, nullptr); m_GIPipelineLayout = VK_NULL_HANDLE; }
    if (m_GIDescPool)       { vkDestroyDescriptorPool(dev, m_GIDescPool, nullptr); m_GIDescPool = VK_NULL_HANDLE; }
    m_GIDescSet = VK_NULL_HANDLE;   // the pool owned it
    if (m_GIDescLayout)     { vkDestroyDescriptorSetLayout(dev, m_GIDescLayout, nullptr); m_GIDescLayout = VK_NULL_HANDLE; }
    for (PermPipes& t : m_PermPipes) {          // crown world-state variants
        if (t.p24) vkDestroyPipeline(dev, t.p24, nullptr);
        if (t.p28) vkDestroyPipeline(dev, t.p28, nullptr);
    }
    m_PermPipes.clear();                // the VS/FS modules belong to the shader manager
    m_PermPending.clear();
    m_PermLastWant  = 0xFFFFFFFFu;      // a new level re-queues from scratch
    m_PermStable    = 0;
    m_PermPrewarmed = false;
    if (m_DepthPipeline24)   { vkDestroyPipeline(dev, m_DepthPipeline24, nullptr); m_DepthPipeline24 = VK_NULL_HANDLE; }
    if (m_DepthPipeline28)   { vkDestroyPipeline(dev, m_DepthPipeline28, nullptr); m_DepthPipeline28 = VK_NULL_HANDLE; }
    m_MetaCPU.clear();
    if (m_GfxPipelineLayout) { vkDestroyPipelineLayout(dev, m_GfxPipelineLayout, nullptr); m_GfxPipelineLayout = VK_NULL_HANDLE; }

    if (m_XformDescPool)   { vkDestroyDescriptorPool(dev, m_XformDescPool, nullptr); m_XformDescPool = VK_NULL_HANDLE; m_XformDescSet = VK_NULL_HANDLE; }
    if (m_XformDescLayout) { vkDestroyDescriptorSetLayout(dev, m_XformDescLayout, nullptr); m_XformDescLayout = VK_NULL_HANDLE; }
    if (m_WaveTex)     { m_WaveTex->Destroy(); xr_delete(m_WaveTex); m_WaveTex = nullptr; }
    if (m_WaveSampler) { vkDestroySampler(dev, m_WaveSampler, nullptr); m_WaveSampler = VK_NULL_HANDLE; }

    if (m_CullPipeline)       { vkDestroyPipeline(dev, m_CullPipeline, nullptr); m_CullPipeline = VK_NULL_HANDLE; }
    if (m_CullPipelineLayout) { vkDestroyPipelineLayout(dev, m_CullPipelineLayout, nullptr); m_CullPipelineLayout = VK_NULL_HANDLE; }
    if (m_CullDescPool)       { vkDestroyDescriptorPool(dev, m_CullDescPool, nullptr); m_CullDescPool = VK_NULL_HANDLE; m_CullDescSet = VK_NULL_HANDLE; }
    if (m_CullDescLayout)     { vkDestroyDescriptorSetLayout(dev, m_CullDescLayout, nullptr); m_CullDescLayout = VK_NULL_HANDLE; }

    if (m_FrustumUBO) { m_FrustumUBO->Destroy(); xr_delete(m_FrustumUBO); }

    DestroyVsm();
}

// ============================================================================
// VSM caster path — trees into the virtual shadow atlas (static → temporal-safe).
// Mirrors RenderDepth, but each tree is instanced over the atlas pages it overlaps
// (binned by vsm_skinned_bin.comp — GpuTreeMeta shares SkinMeta's 32 B layout) and
// rasterized with tree_vsm_page.{vert,frag} which routes per page + alpha-tests leaves.
// ============================================================================
// Max atlas pages a tree bins into. 512 was enough for the STATIC path (dirty-filtered:
// a tree touches only a handful of pages per frame) but the DYNAMIC wind pass bins a NEAR
// tree into ALL its resident pages every frame — a 10m crown a few metres away covers
// hundreds of L0 pages alone (L0 page = base/32 ≈ 0.75 m) and overflowed 512 → truncated
// page lists → near tree shadows visibly fell apart / vanished. 1024 covers it (watch
// "maxPages" in the r_vsm_debug hybrid log; the dyn pool ceiling is VSM_MAX_PHYS=2048).
// 512 (was 1024): the buffer is m_TotalCount × cap × 4B — 792 MB on a 193k-tree map, the
// single biggest VRAM line item, paid for every tree while only near crowns bin many pages
// (observed peak 349). The bin stat line prints [TRUNCATED!] if a caster ever hits the cap.
// casterPages is a shared ARENA of (treeIdx<<13)|slot entries (vsm_tree_bin two-pass:
// count → atomic reserve → write; firstInstance = the tree's run). The old per-tree
// 512-slot slicing cost trees × cap × 4 B = 396 MB on a 193k-tree map — the single
// biggest VRAM line item, big enough to overcommit VRAM and demote tree buffers to
// host memory (25-40 ms Bins/Tree* on Pripyat). A frame really bins ~10-40k pairs
// (near crowns × pages + dirty-page far trees); 1M slots = 4 MB with 25× headroom;
// the [ARENA FULL!] diag in the hybrid log fires if a map ever saturates it.
// kVsmTreeCap remains ONLY as the legacy push value for VS layouts (unused there now).
namespace { constexpr u32 kVsmTreeCap = 512; constexpr u32 kVsmTreeArena = 1u << 20; }

// Impostor crown shadows (r_vsm_tree_impostor): baked silhouette atlas grid = kImpFacets
// azimuth columns × (unique mesh) rows, kImpCell px each. See BuildImpostorAtlas.
namespace { constexpr u32 kImpFacets = 8; constexpr u32 kImpCell = 128; }

// Phase B: meshlet-cull command budget. Commands are POOLED per group; cap[g] =
// max(min(meshCount, kMeshletNearTreesCap)*kCmdPerTree, kGroupFloor). The per-tree term
// scales big species groups; the FLOOR covers small groups of a few large near trees (a
// single near crown bins ~maxPages×fewMeshlets ≈ up to ~430 draws — a 2-tree rare-species
// group would starve on meshCount*k alone → that was the steady-state [TRUNCATED] in the
// first test). Bases are a prefix sum of cap[] (not meshOffset*k). Both static + dynamic
// buffers share this layout.
// kMeshletNearTreesCap: only a fraction of a group's trees emit meshlet commands at once
// (near set + the static-cache warm-up burst), yet cap[g] used to reserve slots for EVERY
// tree of the group — 2×495 MB of VRAM on a 193k-tree map, which alone shoved textures
// into mip-demotion ("всё мыло"). 2048 covers the static-bake burst with margin (128 was
// too tight: overflow=24k [TRUNCATED] during warm-up); overruns show up as "overflow" in
// the r_vsm_debug meshlet stat line and truncate gracefully — bump if it ever ticks.
namespace { constexpr u32 kCmdPerTree = 128; constexpr u32 kGroupFloor = 8192; constexpr u32 kMeshletNearTreesCap = 2048; }

void CTreeManager::CreateVsmResources()
{
    VK::Vram::Scope _vram_scope("Trees");
    if (m_VsmReady) return;
    if (m_XformDescLayout == VK_NULL_HANDLE || m_TexDescLayout == VK_NULL_HANDLE) return;   // Session B not up
    if (m_XformDescSet == VK_NULL_HANDLE) return;
    if (!m_TreeMetadataBuffer || !m_TreeTransformsBuffer || m_TotalCount == 0) return;
    if (!g_ShaderManager) return;

    VkShaderModule binCS = g_ShaderManager->Load("vsm_tree_bin.comp.spv");   // near/far hybrid bin (mode 0 static / 1 dynamic)
    VkShaderModule pvs   = g_ShaderManager->Load("tree_vsm_page.vert.spv");        // STATIC atlas (rigid)
    VkShaderModule pvsD  = g_ShaderManager->Load("tree_vsm_page_dyn.vert.spv");    // DYNAMIC atlas (wind)
    VkShaderModule pfs   = g_ShaderManager->Load("tree_vsm_page.frag.spv");
    if (!binCS || !pvs || !pvsD || !pfs) { Msg("![VK Trees] VSM caster shaders missing — tree VSM shadows disabled"); return; }

    VkDevice dev = VulkanHW.m_Device;
    const u32 N = VK_FRAMES_IN_FLIGHT;

    // Buffers.
    // TRANSFER_DST everywhere below: these are GPU-written append/command buffers, and
    // the ONE-TIME zero-fill in VsmBin's lazy-init branch is load-bearing — a recreated
    // TreeManager (level transition) gets RECYCLED VRAM full of garbage, and a garbage
    // VkDrawIndexedIndirectCommand is a billion-instance draw = GPU hang / DEVICE_LOST.
    // (A fresh process gets OS-zeroed pages, which is why only transitions crashed.)
    // gpuOnly=true everywhere below: these are read/written by EVERY thread of the
    // 193k-thread bin dispatches — the default storage-buffer host-access flag would
    // put them in BAR/system memory (PCIe per warp = the 20-30ms Bins/Tree* zones).
    m_VsmCasterPages = xr_new<CVulkanBuffer>();
    m_VsmCasterPages->Create((VkDeviceSize)kVsmTreeArena * sizeof(u32),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_VsmIndirect = xr_new<CVulkanBuffer>();
    m_VsmIndirect->Create((VkDeviceSize)m_TotalCount * sizeof(VkDrawIndexedIndirectCommand),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_VsmDynIndirect = xr_new<CVulkanBuffer>();
    m_VsmDynIndirect->Create((VkDeviceSize)m_TotalCount * sizeof(VkDrawIndexedIndirectCommand),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_VsmStats = xr_new<CVulkanBuffer>();
    m_VsmStats->Create(8 * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);   // [4]=shadow-HZB culled
    m_VsmStatsRB = xr_new<CVulkanBuffer>();
    m_VsmStatsRB->Create(8 * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    m_VsmStatsPtr = (u32*)m_VsmStatsRB->Map();
    if (m_VsmStatsPtr) memset(m_VsmStatsPtr, 0, 8 * sizeof(u32));
    // Diag bitsets (binding 12): [0..W) = distinct trees binned to the DYN atlas this
    // frame (cleared per frame in VsmBinClears), [W..2W) = trees that ever entered the
    // cached STATIC atlas (accumulated — static bins touch dirty pages only).
    {
        const u32 W = (m_TotalCount + 31u) / 32u;
        m_VsmTreeBits = xr_new<CVulkanBuffer>();
        m_VsmTreeBits->Create((VkDeviceSize)2 * W * sizeof(u32),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
        m_VsmTreeBitsRB = xr_new<CVulkanBuffer>();
        m_VsmTreeBitsRB->Create((VkDeviceSize)2 * W * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        m_VsmTreeBitsPtr = (u32*)m_VsmTreeBitsRB->Map();
        if (m_VsmTreeBitsPtr) memset(m_VsmTreeBitsPtr, 0, (size_t)2 * W * sizeof(u32));
    }
    // Per-frame CPU near-set flags (ring). DEVICE-local + staged upload — NOT host-visible:
    // the bin dispatches read nearFlags[c] from EVERY one of the 193k threads, and a
    // host-memory buffer turned that into per-warp PCIe round-trips = the 17-30 ms
    // Bins/TreeS + Bins/TreeD zones on Pripyat (invisible on few-tree maps). The CPU
    // side uploads m_VsmNearCPU via the async transfer queue instead (774 KB/frame).
    {
        xr_vector<u32> zero(m_TotalCount, 0u);
        for (u32 i = 0; i < N; ++i) {
            m_VsmNearFlags[i] = xr_new<CVulkanBuffer>();
            m_VsmNearFlags[i]->Create((VkDeviceSize)m_TotalCount * sizeof(u32),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
            m_VsmNearFlags[i]->Upload(zero.data(), (VkDeviceSize)m_TotalCount * sizeof(u32));   // all-far until the first near-set update
            m_VsmNearPtr[i] = nullptr;
        }
    }
    // One-shot placement audit: any of these landing outside DEVICE_LOCAL means the
    // bins ride PCIe again — the log line is the tripwire.
    {
        auto place = [](const char* n, CVulkanBuffer* b) {
            if (!b || !b->GetAllocation()) return;
            VkMemoryPropertyFlags f = 0;
            vmaGetAllocationMemoryProperties(VulkanHW.m_Allocator, b->GetAllocation(), &f);
            Msg("[VK Trees] vsm buf %-12s: %s%s", n,
                (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE" : "HOST(!)",
                (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "+host-visible" : "");
        };
        place("casterPages", m_VsmCasterPages);
        place("indirect",    m_VsmIndirect);
        place("dynIndirect", m_VsmDynIndirect);
        place("stats",       m_VsmStats);
        place("nearFlags0",  m_VsmNearFlags[0]);
        place("meta",        m_TreeMetadataBuffer);
        place("xforms",      m_TreeTransformsBuffer);
    }

    // Pool: bin (12 SSBO + 1 UBO) ×2N (static + dynamic) + page (2 SSBO + 1 UBO) ×2N.
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, N * 32 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, N * 4 } };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = N * 4; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &m_VsmDescPool) != VK_SUCCESS) return;

    // Bin set layout (13 bindings: +7 dynUsed, +8 nearFlags, +9 pageMax, +10 staticPageTable=Option A,
    // +11 receiver mask r_vsm_rmask, +12 diag tree bitsets) + N static + N dyn sets.
    {
        m_VsmBinSetL = VK::MakeSetLayout({ kSSBO, kUBO,  kSSBO, kSSBO, kSSBO, kSSBO, kSSBO,
                                           kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                         VK_SHADER_STAGE_COMPUTE_BIT, "Trees.VsmBin");
        if (m_VsmBinSetL == VK_NULL_HANDLE) return;
        if (!VK::AllocSets(m_VsmDescPool, m_VsmBinSetL, N, m_VsmBinSet,    "Trees.VsmBin.static")) return;
        if (!VK::AllocSets(m_VsmDescPool, m_VsmBinSetL, N, m_VsmDynBinSet, "Trees.VsmBin.dyn")) return;
        // push: count, cap, mode, hzbOn, margin(float), rmaskOn, bitBase
        m_VsmBinLayout = VK::MakePipelineLayout({ m_VsmBinSetL }, 7 * sizeof(u32));
        if (m_VsmBinLayout == VK_NULL_HANDLE) return;
        m_VsmBinPipe = VK::CreateComputePipeline(binCS, m_VsmBinLayout, "Trees.VsmBin");
        if (m_VsmBinPipe == VK_NULL_HANDLE) return;
    }

    // Page set layout (3 bindings: pageList, casterPages, clipmap UBO) + N static + N dynamic sets.
    {
        m_VsmPageSetL = VK::MakeSetLayout({ kSSBO, kSSBO, kUBO },
                                          VK_SHADER_STAGE_VERTEX_BIT, "Trees.VsmPage");
        if (m_VsmPageSetL == VK_NULL_HANDLE) return;
        if (!VK::AllocSets(m_VsmDescPool, m_VsmPageSetL, N, m_VsmPageSet,    "Trees.VsmPage.static")) return;
        if (!VK::AllocSets(m_VsmDescPool, m_VsmPageSetL, N, m_VsmDynPageSet, "Trees.VsmPage.dyn")) return;
    }

    // Page pipeline layout: set0 = transforms (reuse), set1 = diffuse (reuse), set2 = page data.
    // Push: 16 B base (uvScale, alphaRef, cap, pad) + 48 B TEST wind (wind_params,
    // wsetup_trees, wind_anim) for r_vsm_tree_wind. See tree_vsm_page.vert.
    m_VsmPageLayout = VK::MakePipelineLayout({ m_XformDescLayout, m_TexDescLayout, m_VsmPageSetL }, 64,
                                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    if (m_VsmPageLayout == VK_NULL_HANDLE) return;

    // Page pipelines (tcOffset 24 / 28 × static/dynamic VS) — depth-only into the D16 atlas, alpha-test FS.
    auto createPageVariant = [&](VkShaderModule vs, u32 tcOffset, VkPipeline& out, bool wantFsr) {
        VK::GfxPipelineBuilder b(m_VsmPageLayout);
        b.Vert(vs).Frag(pfs)
         .Binding(0, 32)
         .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
         .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
         .Cull(VK_CULL_MODE_NONE)
         .DynamicDepthBias()
         .Depth(true, true)
         .DepthTarget(VK_FORMAT_D16_UNORM);   // VSM atlas is D16 (see vk_vsm CreateRenderResources)
        // Dynamic crown pipelines add a FRAGMENT_SHADING_RATE dynamic state so VsmRenderDyn
        // can 2x2-coarse the crown-shadow fill at runtime (r_vsm_tree_vrs). Static pipelines
        // never set a rate.
        if (wantFsr && VulkanHW.m_bVRSPipelineSupported)
            b.Dynamic(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR);
        out = b.Build("Trees VSM page (tcOff=%u)", tcOffset);
    };
    createPageVariant(pvs,  24, m_VsmPagePipe24,    false);
    createPageVariant(pvs,  28, m_VsmPagePipe28,    false);
    createPageVariant(pvsD, 24, m_VsmPageDynPipe24, true);   // dyn crowns → VRS-capable
    createPageVariant(pvsD, 28, m_VsmPageDynPipe28, true);

    m_VsmReady = true;
    Msg("[VK Trees] VSM caster path ready (%u trees, arena %u pairs, wind hybrid %s dist %.0fm)",
        m_TotalCount, kVsmTreeArena, ps_r_vsm_tree_wind ? "ON" : "off", ps_r_vsm_tree_wind_dist);
    if (m_TotalCount > 0x40000u)   // (treeIdx<<13) pack = 18-bit tree ids; trees past 262144 never bin (warn, don't corrupt)
        Msg("![VK Trees] %u trees exceed the 18-bit caster-pack ceiling (262144) - the excess cast no VSM shadow", m_TotalCount);

    CreateImpostorResources();     // r_vsm_tree_impostor: billboard crown shadows (near dyn pass)
    // Phase B meshlet-cull (r_vsm_meshlet): resources are LAZY — the two pooled command
    // buffers alone are ~235 MB ×2 on a 193k-tree map, dead VRAM while the mode is off
    // (its own header says "no net perf gain on dGPU"). A live 0→1 flip creates them in
    // VsmBinMeshlets (with an immediate zero — recycled-VRAM garbage in an indirect
    // command buffer is the billion-instance-draw GPU hang of 2026-07-09).
    if (ps_r_vsm_meshlet) CreateMeshletVsmResources();
    CreateHullResources();         // r_vsm_tree_hull: opaque crown-hull caster LOD (near dyn pass)
}

bool CTreeManager::IsMeshletMode() const
{
    return ps_r_vsm_meshlet && m_MeshletVsmReady && m_MeshletsReady;
}

// ============================================================================
// Phase B — stage-2 meshlet-cull resources: the meshlet-bin compute pipeline, the
// meshlet page pipelines (reuse m_VsmPageLayout / set2), and the command/group buffers.
// Best-effort: any failure leaves m_MeshletVsmReady=false → the old per-tree path runs.
// ⛔ NO NET PERF GAIN on dGPU (2026-07-02) — kept but DISABLED (r_vsm_meshlet=0). Reached only when
//    the cvar is on; inert by default. Don't re-chase — see vk_console_min.cpp cvar block + memory.
// ============================================================================
void CTreeManager::CreateMeshletVsmResources()
{
    VK::Vram::Scope _vram_scope("Trees");
    if (m_MeshletVsmReady) return;
    if (!m_MeshletsReady || !m_MeshletBuffer || !m_MeshletIndexBuffer || !m_TreeMeshletRangeBuffer) return;
    if (m_VsmPageLayout == VK_NULL_HANDLE || m_TotalCount == 0) return;
    if (!g_ShaderManager) return;

    VkDevice   dev = VulkanHW.m_Device;
    const u32  N   = VK_FRAMES_IN_FLIGHT;

    VkShaderModule binCS = g_ShaderManager->Load("vsm_tree_meshlet_bin.comp.spv");
    VkShaderModule pvs   = g_ShaderManager->Load("tree_vsm_meshlet_page.vert.spv");
    VkShaderModule pvsD  = g_ShaderManager->Load("tree_vsm_meshlet_page_dyn.vert.spv");
    VkShaderModule pfs   = g_ShaderManager->Load("tree_vsm_page.frag.spv");   // reuse the alpha-test FS
    if (!binCS || !pvs || !pvsD || !pfs) { Msg("![VK Trees] meshlet shaders missing — r_vsm_meshlet disabled"); return; }

    // ----- CPU: per-tree group index + per-group (cmdBase, cmdCap). Groups are contiguous
    // tree ranges, so the pooled section [meshOffset*kCmdPerTree, +meshCount*kCmdPerTree) is
    // contiguous and renderable per group with one DrawIndexedIndirectCount.
    const u32 groupN = (u32)m_Groups.size();
    xr_vector<u32>  treeGroup(m_TotalCount, 0u);
    xr_vector<u32>  groupInfo(groupN * 2u, 0u);   // uvec2 (cmdBase, cmdCap) per group
    m_MeshletGroupBase.assign(groupN, 0u);
    m_MeshletGroupCap.assign(groupN, 0u);
    u32 cursor = 0;   // running command-slot cursor -> per-group base (prefix sum of caps)
    for (u32 g = 0; g < groupN; ++g) {
        const TreeIndirectGroup& grp = m_Groups[g];
        u32 cap = std::min(grp.meshCount, kMeshletNearTreesCap) * kCmdPerTree;
        if (cap < kGroupFloor) cap = kGroupFloor;
        groupInfo[g * 2 + 0] = cursor;
        groupInfo[g * 2 + 1] = cap;
        m_MeshletGroupBase[g] = cursor;
        m_MeshletGroupCap[g]  = cap;
        cursor += cap;
        for (u32 i = 0; i < grp.meshCount; ++i)
            if (grp.meshOffset + i < m_TotalCount) treeGroup[grp.meshOffset + i] = g;
    }
    const VkDeviceSize cmdBytes = (VkDeviceSize)cursor * sizeof(VkDrawIndexedIndirectCommand);

    auto mkBuf = [&](CVulkanBuffer*& b, VkDeviceSize sz, VkBufferUsageFlags usage, VmaMemoryUsage mem) {
        b = xr_new<CVulkanBuffer>(); b->Create(sz, usage, mem, true);   // gpuOnly: GPU-written command/count buffers, no CPU mapping
    };
    // TRANSFER_DST: zero-filled once in VsmBin's lazy-init (recycled-VRAM garbage command
    // = billion-instance indirect draw = the level-transition GPU hang; see CreateVsmResources).
    mkBuf(m_VsmMeshletCmd,        cmdBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    mkBuf(m_VsmMeshletCmdDyn,     cmdBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    mkBuf(m_VsmMeshletGroupCount,    (VkDeviceSize)groupN * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    mkBuf(m_VsmMeshletGroupCountDyn, (VkDeviceSize)groupN * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    mkBuf(m_TreeGroupBuffer, (VkDeviceSize)m_TotalCount * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    mkBuf(m_GroupInfoBuffer, (VkDeviceSize)groupN * 2 * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    m_TreeGroupBuffer->Upload(treeGroup.data(), treeGroup.size() * sizeof(u32));
    m_GroupInfoBuffer->Upload(groupInfo.data(), groupInfo.size() * sizeof(u32));
    mkBuf(m_MeshletStats, 4 * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    mkBuf(m_MeshletStatsRB, 4 * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    m_MeshletStatsPtr = (u32*)m_MeshletStatsRB->Map();
    if (m_MeshletStatsPtr) memset(m_MeshletStatsPtr, 0, 4 * sizeof(u32));

    // ----- Descriptor pool + layout (13 bindings, all SSBO except binding 1 = clipmap UBO).
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, N * 2 * 12 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, N * 2 } };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = N * 2; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &m_MeshletDescPool) != VK_SUCCESS) return;

    {
        m_MeshletBinSetL = VK::MakeSetLayout({ kSSBO, kUBO,  kSSBO, kSSBO, kSSBO, kSSBO, kSSBO,   // 1 = VsmParams
                                               kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                             VK_SHADER_STAGE_COMPUTE_BIT, "Trees.MeshletBin");
        if (m_MeshletBinSetL == VK_NULL_HANDLE) return;
        if (!VK::AllocSets(m_MeshletDescPool, m_MeshletBinSetL, N, m_MeshletBinSet,    "Trees.MeshletBin.static")) return;
        if (!VK::AllocSets(m_MeshletDescPool, m_MeshletBinSetL, N, m_MeshletDynBinSet, "Trees.MeshletBin.dyn")) return;
        // push: casterCount, cap, slop(float), pad
        m_MeshletBinLayout = VK::MakePipelineLayout({ m_MeshletBinSetL }, 4 * sizeof(u32));
        if (m_MeshletBinLayout == VK_NULL_HANDLE) return;
        m_MeshletBinPipe = VK::CreateComputePipeline(binCS, m_MeshletBinLayout, "Trees.MeshletBin");
        if (m_MeshletBinPipe == VK_NULL_HANDLE) return;
    }

    // ----- Meshlet page pipelines (reuse m_VsmPageLayout: set0 xform, set1 tex, set2 page).
    auto createPageVariant = [&](VkShaderModule vs, u32 tcOffset, VkPipeline& out) {
        out = VK::GfxPipelineBuilder(m_VsmPageLayout)
            .Vert(vs).Frag(pfs)
            .Binding(0, 32)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
            .Cull(VK_CULL_MODE_NONE)
            .DynamicDepthBias()
            .Depth(true, true)
            .DepthTarget(VK_FORMAT_D16_UNORM)   // VSM atlas is D16 (see vk_vsm CreateRenderResources)
            .Build("Trees meshlet page (tcOff=%u)", tcOffset);
    };
    createPageVariant(pvs,  24, m_MeshletPagePipe24);
    createPageVariant(pvs,  28, m_MeshletPagePipe28);
    createPageVariant(pvsD, 24, m_MeshletPageDynPipe24);
    createPageVariant(pvsD, 28, m_MeshletPageDynPipe28);

    // Immediate one-off zero of the GPU-written buffers: on the LOAD path VsmBin's
    // lazy-init fill covers them, but a live r_vsm_meshlet 0→1 flip creates them
    // mid-session — recycled-VRAM garbage in an indirect command buffer would be a
    // billion-instance draw (the level-transition GPU hang of 2026-07-09).
    if (VkCommandBuffer zc = VulkanHW.BeginSingleTimeCommands()) {
        auto z = [&](CVulkanBuffer* b) { if (b) vkCmdFillBuffer(zc, b->GetHandle(), 0, VK_WHOLE_SIZE, 0u); };
        z(m_VsmMeshletCmd); z(m_VsmMeshletCmdDyn);
        z(m_VsmMeshletGroupCount); z(m_VsmMeshletGroupCountDyn); z(m_MeshletStats);
        VulkanHW.EndSingleTimeCommands(zc);
    }

    m_MeshletVsmReady = true;
    Msg("[VK Trees] Meshlet VSM path ready (%u clusters, %u cmd/tree, %u KB cmd x2)",
        m_MeshletTotal, kCmdPerTree, (u32)(cmdBytes / 1024));
}

// Stage 2: refine the per-tree page list into per-(meshlet,page) draws. mode 0 = static
// (m_VsmIndirect / m_VsmMeshletCmd), mode 1 = dynamic (m_VsmDynIndirect / m_VsmMeshletCmdDyn).
// pageList is that atlas's slot->page map. Runs right after the matching stage-1 dispatch.
void CTreeManager::DispatchMeshletBin(VkCommandBuffer cmd, u32 mode, VkBuffer pageList, VkBuffer clipmapUBO)
{
    if (!m_MeshletVsmReady) return;
    const u32 slot = m_VsmSlot;
    const bool  isDyn = (mode == 1u);
    VkDescriptorSet set   = isDyn ? m_MeshletDynBinSet[slot] : m_MeshletBinSet[slot];
    VkBuffer indirect     = isDyn ? m_VsmDynIndirect->GetHandle()          : m_VsmIndirect->GetHandle();
    VkBuffer outCmd       = isDyn ? m_VsmMeshletCmdDyn->GetHandle()        : m_VsmMeshletCmd->GetHandle();
    VkBuffer groupCnt     = isDyn ? m_VsmMeshletGroupCountDyn->GetHandle() : m_VsmMeshletGroupCount->GetHandle();

    // No fills/barriers here: the group append-counters + stats were cleared by
    // VsmBinClears in the frame-start fill block, and the caller (vk_vsm MarkPages)
    // issued the single stage-1→stage-2 barrier ordering the tree bins' casterPages/
    // indirect writes before this refinement's reads.
    VK::DescriptorWriter(set)
        .StorageBuffer(0,  m_TreeMetadataBuffer->GetHandle())      // meta
        .UniformBuffer(1,  clipmapUBO)                             // VsmParams
        .StorageBuffer(2,  m_TreeTransformsBuffer->GetHandle())    // xform
        .StorageBuffer(3,  m_MeshletBuffer->GetHandle())           // meshlets
        .StorageBuffer(4,  m_TreeMeshletRangeBuffer->GetHandle())  // tree range
        .StorageBuffer(5,  m_VsmCasterPages->GetHandle())          // caster pages (stage 1)
        .StorageBuffer(6,  pageList)                               // slot -> page
        .StorageBuffer(7,  indirect)                               // stage-1 indirect (pageCount)
        .StorageBuffer(8,  outCmd)                                 // out commands
        .StorageBuffer(9,  groupCnt)                               // group counter
        .StorageBuffer(10, m_TreeGroupBuffer->GetHandle())         // tree -> group
        .StorageBuffer(11, m_GroupInfoBuffer->GetHandle())         // group (base,cap)
        .StorageBuffer(12, m_MeshletStats->GetHandle())            // stats
        .Flush();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_MeshletBinPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_MeshletBinLayout, 0, 1, &set, 0, nullptr);
    const u32 castN = _min(m_TotalCount, 0x40000u);   // 18-bit caster-pack ceiling (matches the stage-1 bins)
    struct { u32 casterCount, cap; float slop; u32 pad; } push{ castN, kVsmTreeCap, isDyn ? 0.5f : 0.0f, 0u };
    vkCmdPushConstants(cmd, m_MeshletBinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, castN, 1, 1);   // one workgroup per tree
}

// Meshlet render for one atlas. Per group: bind meshlet VS + diffuse + group VB + the shared
// meshlet IB, then DrawIndexedIndirectCount over the group's pooled command section.
void CTreeManager::DrawMeshlets(VkCommandBuffer cmd, u32 mode, VkDescriptorSet pageSet, bool wind)
{
    const bool isDyn = (mode == 1u);
    VkPipeline p24 = isDyn ? m_MeshletPageDynPipe24 : m_MeshletPagePipe24;
    VkPipeline p28 = isDyn ? m_MeshletPageDynPipe28 : m_MeshletPagePipe28;
    if (p24 == VK_NULL_HANDLE && p28 == VK_NULL_HANDLE) return;
    VkBuffer cmdBuf   = isDyn ? m_VsmMeshletCmdDyn->GetHandle()        : m_VsmMeshletCmd->GetHandle();
    VkBuffer countBuf = isDyn ? m_VsmMeshletGroupCountDyn->GetHandle() : m_VsmMeshletGroupCount->GetHandle();

    struct VsmPagePush {
        float    uvScale, alphaRef; u32 cap, pad;
        Fvector4 wind_params, wsetup_trees, wind_anim;
    } pc{};
    pc.uvScale = 1.0f / 2048.0f; pc.alphaRef = TreeAlphaRef(200.0f / 255.0f); pc.cap = kVsmTreeCap;
    if (wind) {
        pc.wsetup_trees.set(ps_r_wind_tree_anim, ps_r_wind_tree_trunk, ps_r_wind_tree_bend, 0.1f);
        if (g_pGamePersistent) {
            const Fvector3 wa = ES().wind_anim;
            pc.wind_anim.set(wa.x, wa.y, wa.z, ps_r_wind_tree_flutter);
            if (auto* E = ES().CurrentEnv)
                pc.wind_params.set(E->wind_direction, E->wind_velocity, 0.0f, 0.0f);
        }
        pc.wind_params.w = ps_r_wind_tree_crown;   // flutter height gate — must match the forward pass
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 0, 1, &m_XformDescSet, 0, nullptr);   // set0 transforms
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 2, 1, &pageSet, 0, nullptr);          // set2 page data
    vkCmdPushConstants(cmd, m_VsmPageLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    VkPipeline lastPipe = VK_NULL_HANDLE; VkDescriptorSet lastTex = VK_NULL_HANDLE;
    for (u32 g = 0; g < (u32)m_Groups.size(); ++g) {
        const TreeIndirectGroup& grp = m_Groups[g];
        VkPipeline pipe = (grp.tcOffset == 24) ? p24 : p28;
        if (pipe == VK_NULL_HANDLE || grp.descSetIdx >= m_TexDescSets.size()) continue;
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; lastTex = VK_NULL_HANDLE; }
        VkDescriptorSet tex = m_TexDescSets[grp.descSetIdx];
        if (tex != lastTex) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 1, 1, &tex, 0, nullptr); lastTex = tex; }
        VkDeviceSize vbOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &vbOff);
        vkCmdBindIndexBuffer(cmd, m_MeshletIndexBuffer->GetHandle(), 0, VK_INDEX_TYPE_UINT16);   // shared meshlet IB
        const VkDeviceSize cmdOff = (VkDeviceSize)m_MeshletGroupBase[g] * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)g * sizeof(u32);
        vkCmdDrawIndexedIndirectCount(cmd, cmdBuf, cmdOff, countBuf, cntOff,
                                      m_MeshletGroupCap[g], sizeof(VkDrawIndexedIndirectCommand));
    }
}

// CPU near/far set for the wind hybrid. THE METRIC IS LIGHT-SPACE, NOT WORLD DISTANCE:
// with a low sun a tree's shadow lands tens of metres DOWN-SUN of its trunk, so "trunk
// near the camera" misses exactly the shadows the player is looking at (a house shaded
// by a tree 60 m up-sun stayed rigid). A tree goes DYNAMIC when its shadow COLUMN passes
// within r_vsm_tree_wind_dist of the camera — i.e. the light-space-XY distance between
// the tree and the camera (both projected along the sun) is under the threshold. A world
// -distance bound (3×dist) caps the up-sun corridor cost. Hysteresis (×1.15 on both)
// stops boundary flapping; every crossing queues an invalidation sphere so the residency
// pass re-renders the tree's STATIC pages the same frame (no ghost doubling). Call
// BEFORE the residency dispatch (vk_vsm) with this frame's sun view.
void CTreeManager::VsmUpdateNearSet(const Fmatrix& sunView)
{
    if (m_TotalCount == 0 || m_MetaCPU.size() < m_TotalCount) return;
    VK_CPU_PROBE("TreeNearSet");   // O(total trees) walk, every non-skip frame
    if (m_VsmNearCPU.size() != m_TotalCount) m_VsmNearCPU.assign(m_TotalCount, 0);
    const float nd = ps_r_vsm_tree_wind ? ps_r_vsm_tree_wind_dist : 0.f;
    // World-distance corridor bound: 2× the lateral radius (80 m at default 40). Trees
    // further up-sun still shade the player, but their sway detail no longer reads and
    // every corridor tree re-rasters its crown into the dyn atlas EVERY frame — measured
    // 3× (near 330 trees, instDyn 5-12k, VSMrender to 2-3.5ms) → the main perf knob.
    const float wd = nd * 2.f;
    const float ndIn2  = nd * nd,                    ndOut2 = (nd * 1.15f) * (nd * 1.15f);
    const float wdIn2  = wd * wd,                    wdOut2 = (wd * 1.15f) * (wd * 1.15f);
    // Crown-hull tier (r_vsm_tree_hull): near trees beyond hd cast from the baked opaque
    // hull, only the closest ones keep the full alpha-tested crown mesh. Same light-space
    // lateral metric as the near set (the tier decides the detail of the shadow NEAR THE
    // PLAYER). Hysteresis avoids per-frame tier flicker at the boundary; crown↔hull
    // crossings need NO invalidation circles — both tiers live in the per-frame dyn atlas.
    const bool  hullOn  = ps_r_vsm_tree_hull && m_HullDataReady;
    const bool  farHullOn = hullOn && ps_r_vsm_tree_hull >= 2;   // mode 2: far/static foliage too
    const float hd     = hullOn ? ps_r_vsm_tree_hull_dist : 0.f;
    const float hdIn2  = hd * hd,                    hdOut2 = (hd * 1.1f) * (hd * 1.1f);
    const Fvector cam = RV().vCameraPosition;
    Fvector camL; sunView.transform_tiny(camL, cam);  // camera in light space
    u32 nearCount = 0, hullCount = 0, farHullCount = 0;
    // Voxel-cloud caster CHOICE (r_vsm_tree_hull_vox): per tree, the camera-distance LOD
    // (same continuous cube-size/density math as the visual viewmode — shadow and view
    // agree on the cubes) plus the CROSSFADE fade: 1 = fully voxel, (0,1) = the band
    // just inside r_vsm_tree_hull_dist where the real crown still casts and the cubes
    // dissolve in — approaching a tree smoothly morphs its shadow into true leaves.
    const bool voxOn = hullOn && ps_r_vsm_tree_hull_vox > 0 && m_BrickTotal &&
                       m_BrickLodInfoCPU.size() == (size_t)m_TotalCount * kHullLods;
    // UE WPODisableDistance analog (needs mode 2 — the demoted trees land in bit2).
    const bool voxWpo = voxOn && farHullOn && ps_r_vsm_tree_hull_vox_wpo;
    if (m_VoxChoiceCPU.size() != (size_t)m_TotalCount * 4) m_VoxChoiceCPU.assign((size_t)m_TotalCount * 4, 0u);
    if (m_VoxLodSizeCPU.size() != m_TotalCount) m_VoxLodSizeCPU.assign(m_TotalCount, 0.f);
    const float band    = (std::min)(0.9f, (std::max)(0.02f, ps_r_vsm_tree_hull_band));
    const float bandIn  = hd * (1.f - band);   // fade 0 at this lateral dist, 1 at hd
    const float biasMul = powf(2.f, (float)ps_r_vsm_tree_hull_lod);
    const float stex    = ps_r_vsm_tree_hull_vox_stex;
    u32 voxCastCount = 0, voxBandCount = 0, voxCastSlice = 0;
    m_VoxCastListCPU.clear();   // compact vox-cull dispatch list (trees with a live brick choice)
    // Small visuals (bushes/shrubs, R < 3 m) never go dynamic: their shadows are small
    // ground blobs where sway is unreadable, but they dominate the near set by COUNT
    // (the manager holds every trees\* visual incl. bushes — user saw "20 trees", the
    // set held 330). They keep casting rigidly from the static cache.
    constexpr float kWindMinRadius = 3.0f;
    for (u32 i = 0; i < m_TotalCount; ++i) {
        const GpuTreeMeta& m = m_MetaCPU[i];
        Fvector tL; sunView.transform_tiny(tL, m.sphere_P);
        const float lx = tL.x - camL.x, ly = tL.y - camL.y;
        const float dxy2 = lx * lx + ly * ly;                 // lateral dist to the shadow column
        const float dw2  = cam.distance_to_sqr(m.sphere_P);   // world dist (cost bound)
        const bool wasNear    = (m_VsmNearCPU[i] & 1) != 0;
        const bool wasHull    = (m_VsmNearCPU[i] & 2) != 0;
        const bool wasFarHull = (m_VsmNearCPU[i] & 4) != 0;
        // Hull tiers (both use the same lateral shadow-column metric + hysteresis).
        // CROWNS-ONLY (windClass 2): trunks are opaque bark — no alpha test, not the
        // bottleneck, and their exact shadow anchors the silhouette; non-foliage statics
        // (vehicles/props also carried by this path) must never turn into lobes.
        //   bit1 = NEAR hull: dyn near set beyond hd (re-rendered per frame, wind).
        //   bit2 = FAR hull (mode 2): STATIC foliage beyond hd — makes the shadow LOD
        //          MONOTONIC (a 60 m tree no longer casts a MORE detailed shadow than a
        //          25 m one) and cuts the full-crown re-raster on dirty cache refreshes.
        //          Bushes/trees whose shadow column passes NEAR the player stay real.
        const bool crown     = i < m_WindClassCPU.size() && m_WindClassCPU[i] == 2;
        const bool hullSide  = hd > 0.f && (wasHull || wasFarHull ? (dxy2 > hdIn2) : (dxy2 > hdOut2));
        // UE WPODisableDistance: past the hull boundary the brick shadow is RIGID (its
        // wind amp ramps to 0 across the crossfade band, see amp16 below), and a rigid
        // caster buys nothing from the per-frame dyn atlas — demote it straight to the
        // cached STATIC tier (bit2). The dyn near set shrinks to the real-crown ring +
        // the band; hd crossings ride the same invalidation circles as every other
        // static-representation flip. Trees without baked bricks keep the old dyn
        // shell path (the shell has no per-brick wind story).
        const bool voxRigid  = voxWpo && crown && hullSide &&
                               m_BrickLodInfoCPU[(size_t)i * kHullLods].count != 0;
        const bool isNear  = !voxRigid && (nd > 0.f) && (m.sphere_R >= kWindMinRadius)
                           && (wasNear ? (dxy2 < ndOut2 && dw2 < wdOut2)
                                       : (dxy2 < ndIn2  && dw2 < wdIn2));
        const bool isHull    = isNear && crown && hullSide;
        const bool isFarHull = !isNear && farHullOn && crown && hullSide;
        if (isNear) ++nearCount;
        if (isHull) ++hullCount;
        if (isFarHull) ++farHullCount;
        // Per-tree voxel-caster choice — refreshed EVERY frame (camera moves even when
        // the tier flags don't), so it lives before the early-out below.
        if (voxOn) {
            u32* ch = &m_VoxChoiceCPU[(size_t)i * 4];
            ch[0] = ch[1] = ch[2] = ch[3] = 0u;
            // Leaving the far-hull tier drops the LOD ratchet anchor: the bit2 flip
            // below already invalidates the pages, and a stale anchor would fire a
            // duplicate circle on re-entry.
            if (!isFarHull) m_VoxLodSizeCPU[i] = 0.f;
            if (crown && (isNear || isHull || isFarHull)) {
                const float dxy = sqrtf(dxy2);
                float fade = 1.f;
                if (!isHull && !isFarHull)   // near-set band: dissolve in toward hd
                    fade = (dxy - bandIn) / (std::max)(hd - bandIn, 0.01f);
                // BRICK slices (UE-style shadow representation): the caster draws one
                // light-facing quad per 4×4×4-cell brick and DDA-raycasts its 64-bit
                // mask in the FS — the slice count here is BRICKS, ~×14-50 fewer units
                // than cubes, each costing 6 verts.
                const TreeVoxLod* lods = &m_BrickLodInfoCPU[(size_t)i * kHullLods];
                if (fade > 0.004f && lods[0].count) {
                    fade = (std::min)(fade, 1.f);
                    // Voxel LOD from the SHADOW VIEW, not the camera (UE: the same
                    // LOD metric serves color and every shadow view; for a VSM page
                    // the resolution is its clipmap texel). The tree's clipmap level
                    // comes from the same lateral window metric the residency uses
                    // (level L window spans base·2^L centred on the camera), so
                    // T = stex page texels at that level: near rings pick the finest
                    // bake (real leaf gaps close up), far rings coarsen in lockstep
                    // with the pages — the old camera-px curve + 0.35 m floor drove
                    // far trees to the 1-brick level = literal solid AABB shadows.
                    float lvlExt = ps_r_vsm_base;
                    while (lvlExt < 2.f * dxy && lvlExt < 32.f * ps_r_vsm_base) lvlExt *= 2.f;
                    float T = (lvlExt / 4096.f) * stex * biasMul;
                    T = (std::max)(T, ps_r_vsm_tree_hull_vox_sfloor);
                    const float sLast = lods[kHullLods - 1].count ? lods[kHullLods - 1].size : lods[0].size;
                    T = (std::min)(T, 4.f * sLast);
                    int lvl = 0;
                    while (lvl + 1 < (int)kHullLods && lods[lvl + 1].size <= T && lods[lvl + 1].count) ++lvl;
                    // Hard cap on the caster slice (bricks/tree) — bounds the worst-case
                    // draw regardless of species density.
                    const u32 voxCap = (u32)ps_r_vsm_tree_hull_vox_cap;
                    while (lvl + 1 < (int)kHullLods && lods[lvl].count > voxCap && lods[lvl + 1].count) ++lvl;
                    // Rendered voxel = the level's CELL exactly (bricks are a fixed grid;
                    // the continuous cube growth of the viewmode doesn't apply — level
                    // steps in the shadow hide behind PCF/EMA at these distances).
                    const float cube = lods[lvl].size;
                    ch[0] = lods[lvl].first;
                    ch[1] = lods[lvl].count;
                    memcpy(&ch[2], &cube, sizeof(float));
                    // Low halfword = brick wind AMPLITUDE (1−fade): full coherent sway
                    // next to the swaying crown at the band's inner edge, exactly 0 by
                    // the hd handover — so the demoted tree enters the rigid static
                    // cache without a snap, and every fully-voxel/static tree carries
                    // amp 0, letting the VS skip the whole ssfxTreeWind block (UE:
                    // WPO disabled at voxel distances, zero wind ALU).
                    const u32 amp16 = (u32)((1.f - fade) * 65535.f + 0.5f);
                    ch[3] = ((u32)(fade * 65535.f + 0.5f) << 16) | amp16;
                    ++voxCastCount;
                    m_VoxCastListCPU.push_back(i);
                    voxCastSlice += ch[1];
                    if (fade < 0.999f) ++voxBandCount;
                    // STATIC-tier LOD ratchet: far-hull voxels live in CACHED pages —
                    // a choice change alone never redraws them, so approaching a far
                    // tree (or any bush: R<3 m never joins the near set) left its
                    // shadow at the cube size from whenever the page last rendered,
                    // then "snapped" at the tier flip. Re-anchor + queue the same
                    // invalidation circle a tier flip uses whenever the cube leaves
                    // ±1.5× of the last rendered size (ratchet = built-in hysteresis;
                    // ≤4 circles/frame FIFO absorbs bursts). Dyn tier re-renders per
                    // frame and needs none of this.
                    // Only when the static tier actually casts bricks — the default far
                    // SHELL never changes with the cube size, so ratchet circles would
                    // be pure wasted cache invalidation traffic.
                    if (isFarHull && ps_r_vsm_tree_hull_vox_static) {
                        float& anchor = m_VoxLodSizeCPU[i];
                        if (anchor <= 0.f) anchor = cube;
                        else if (cube > anchor * 1.5f || cube * 1.5f < anchor) {
                            anchor = cube;
                            m_VsmPendingInval.push_back({ m.sphere_P.x, m.sphere_P.y, m.sphere_P.z, m.sphere_R * 1.4143f });
                        }
                    }
                }
            }
        }
        const u8 v = u8((isNear ? 1 : 0) | (isHull ? 2 : 0) | (isFarHull ? 4 : 0));
        if (v == m_VsmNearCPU[i]) continue;
        m_VsmNearCPU[i] = v;
        // Static pages must redraw when the tree's STATIC representation changes:
        // near↔far moves (existing) AND far real-mesh↔hull flips (bit 2). Near-set
        // crown↔hull (bit 1) is dyn-only — the dyn atlas re-renders every frame anyway.
        if (isNear == wasNear && isFarHull == wasFarHull) continue;
        // sqrt(2) inflation: the bin covers the sphere's bounding SQUARE in light XY —
        // the invalidation circle must reach its corners.
        m_VsmPendingInval.push_back({ m.sphere_P.x, m.sphere_P.y, m.sphere_P.z, m.sphere_R * 1.4143f });
    }
    m_VsmNearCount = nearCount;
    m_VsmHullCount = hullCount;
    m_VsmHullFarCount = farHullCount;
    m_VoxCastCount = voxCastCount;
    m_VoxBandCount = voxBandCount;
    m_VoxCastSlice = voxCastSlice;

    // Burst guard: a representation flip of half the forest (r_vsm_tree_hull 2 at
    // level load — every far tree's bit2 goes 0→1 on the FIRST update — or a live
    // toggle) queues tens of thousands of circles; drained at ≤4/frame that is an
    // HOUR-long permanent redraw storm (seen 17-07: pendingInval=88k, fps 11 on the
    // live flip). Past a threshold one full atlas invalidation is strictly cheaper:
    // a single clean re-render, then the cache is coherent again.
    if (m_VsmPendingInval.size() > 4096) {
        Msg("[VK Trees] VSM inval burst (%zu circles) -> full atlas invalidation instead",
            m_VsmPendingInval.size());
        m_VsmPendingInval.clear();
        VK::VSM::InvalidateCache();
    }
}

u32 CTreeManager::VsmPopTransitions(Fvector4* out, u32 maxOut)
{
    // FIFO: the resid pass consumes ≤4 circles/frame, so a burst of crossings (sprint
    // through a treeline) queues up — LIFO kept serving the NEWEST and starved the
    // oldest, leaving a crossed tree's stale rigid shadow doubled with its dyn one
    // until the queue drained. Oldest-first bounds the ghost to queue-length frames.
    const u32 n = _min(maxOut, (u32)m_VsmPendingInval.size());
    for (u32 i = 0; i < n; ++i) out[i] = m_VsmPendingInval[i];
    m_VsmPendingInval.erase(m_VsmPendingInval.begin(), m_VsmPendingInval.begin() + n);
    return n;
}

void CTreeManager::VsmBin(VkCommandBuffer cmd, VkBuffer pageTable, VkBuffer slotDirty, VkBuffer dynUsed, VkBuffer clipmapUBO, VkBuffer pageList, VkBuffer pageMax)
{
    if (!m_VsmReady) {
        CreateVsmResources();
        if (m_VsmReady) {
            // FIRST FRAME after lazy create: the frame-start VsmBinClears already ran
            // and skipped (m_VsmReady was false), so every GPU-written buffer still
            // holds its allocation garbage. On a level TRANSITION that garbage is
            // recycled VRAM from the destroyed managers — a junk indirect command
            // (indexCount/instanceCount in the billions) hangs the GPU → the
            // 'VSM/DynTrees' DEVICE_LOST. Zero everything NOW, before this frame's
            // bins/draws consume it. One-off cost, once per level.
            auto fill0 = [&](CVulkanBuffer* b) { if (b) vkCmdFillBuffer(cmd, b->GetHandle(), 0, VK_WHOLE_SIZE, 0u); };
            fill0(m_VsmStats);
            fill0(m_VsmTreeBits);
            fill0(m_VsmCasterPages);
            fill0(m_VsmIndirect);
            fill0(m_VsmDynIndirect);
            fill0(m_VsmHullIndirect);
            fill0(m_VsmHullIndirectS);
            fill0(m_VsmVoxIndirect);
            fill0(m_VsmVoxIndirectS);
            fill0(m_VoxCullCmdD);
            fill0(m_VoxCullCmdS);
            fill0(m_VoxCullList);
            fill0(m_VoxCullStats);
            fill0(m_VsmMeshletCmd);
            fill0(m_VsmMeshletCmdDyn);
            fill0(m_VsmMeshletGroupCount);
            fill0(m_VsmMeshletGroupCountDyn);
            fill0(m_MeshletStats);
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                 0, 1, &mb, 0, nullptr, 0, nullptr);
        }
    }
    if (!m_VsmReady || m_TotalCount == 0 || m_VsmBinPipe == VK_NULL_HANDLE) return;
    if (pageTable == VK_NULL_HANDLE || slotDirty == VK_NULL_HANDLE || dynUsed == VK_NULL_HANDLE || clipmapUBO == VK_NULL_HANDLE) return;
    if (pageMax == VK_NULL_HANDLE) pageMax = m_VsmCasterPages->GetHandle();   // HZB off: bind any valid buffer for layout parity

    m_VsmSlot = (m_VsmSlot + 1) % VK_FRAMES_IN_FLIGHT;
    const u32 slot = m_VsmSlot;

    // Upload this frame's CPU near-set flags (computed by VsmUpdateNearSet) into the ring
    // slot — async staged copy to the DEVICE-local buffer (the graphics submit waits the
    // upload timeline). Empty/mismatched CPU set → keep the slot's previous flags (it was
    // zero-filled at create; sizes only mismatch across a rebuild).
    if (m_VsmNearFlags[slot] && m_VsmNearCPU.size() == m_TotalCount) {
        if (m_VsmNearU32.size() != m_TotalCount) m_VsmNearU32.resize(m_TotalCount);
        for (u32 i = 0; i < m_TotalCount; ++i) m_VsmNearU32[i] = m_VsmNearCPU[i];   // u8 truth -> the shader's u32 flags
        m_VsmNearFlags[slot]->Upload(m_VsmNearU32.data(), (VkDeviceSize)m_TotalCount * sizeof(u32));
    }
    // Voxel-caster choice ring (same cadence: vsm_hull_cmd consumes it this VSM frame).
    if (u32* chp = (u32*)m_VoxChoicePtr[slot]) {
        if (m_VoxChoiceCPU.size() == (size_t)m_TotalCount * 4)
            memcpy(chp, m_VoxChoiceCPU.data(), (size_t)m_TotalCount * 4 * sizeof(u32));
        else
            memset(chp, 0, (size_t)m_TotalCount * 4 * sizeof(u32));
    }
    // Compact vox-cull tree list (same ring cadence): the stage-2 brick cull dispatches
    // one workgroup per LISTED tree, not one per tree in the level.
    m_VoxCullTreeN = 0;
    if (u32* tl = (u32*)m_VoxCullTreeListPtr[slot]) {
        const u32 n = _min((u32)m_VoxCastListCPU.size(), m_TotalCount);
        if (n) memcpy(tl, m_VoxCastListCPU.data(), n * sizeof(u32));
        m_VoxCullTreeN = n;
    }

    // No stats fill/barrier here — VsmBinClears batched it into the frame-start fill
    // block, and the caller's single residency→bins barrier orders it before this read.

    // mode 0 = STATIC pass: FAR trees into dirty static pages. dynUsed is bound for layout
    // completeness only (the shader's write is mode-gated).
    VkBuffer rmask = VK::VSM::GetRMaskHandle();
    if (rmask == VK_NULL_HANDLE) rmask = pageTable;   // layout parity (mode 0 never reads it)
    VK::DescriptorWriter(m_VsmBinSet[slot])
        .StorageBuffer(0,  m_TreeMetadataBuffer->GetHandle())
        .UniformBuffer(1,  clipmapUBO)
        .StorageBuffer(2,  pageTable)
        .StorageBuffer(3,  m_VsmCasterPages->GetHandle())
        .StorageBuffer(4,  m_VsmIndirect->GetHandle())
        .StorageBuffer(5,  m_VsmStats->GetHandle())
        .StorageBuffer(6,  slotDirty)                        // STATIC-atlas dirty set (cache filter)
        .StorageBuffer(7,  dynUsed)                          // unused in mode 0
        .StorageBuffer(8,  m_VsmNearFlags[slot]->GetHandle())// near set
        .StorageBuffer(9,  pageMax)                          // shadow-HZB occluder max (r_vsm_hzb)
        .StorageBuffer(10, pageTable)                        // staticPageTable — here pageTable IS static (mode 0 ignores it)
        .StorageBuffer(11, rmask)                            // receiver mask (mode 1 only — static must not partial-render)
        .StorageBuffer(12, m_VsmTreeBits->GetHandle())       // diag tree bitsets (dyn | static-accum)
        .Flush();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_VsmBinPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_VsmBinLayout, 0, 1, &m_VsmBinSet[slot], 0, nullptr);
    const u32 castN = _min(m_TotalCount, 0x40000u);   // 18-bit caster-pack ceiling
    struct { u32 count, arenaSlots, mode, hzbOn; float margin; u32 rmaskOn, bitBase; } push{
        castN, kVsmTreeArena, 0u, (u32)(ps_r_vsm_hzb ? 1 : 0), ps_r_vsm_hzb_margin, 0u,
        (m_TotalCount + 31u) / 32u };   // mode 0 = static + shadow-HZB (rmask off); bitBase = static-accum half
    vkCmdPushConstants(cmd, m_VsmBinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (castN + 63) / 64, 1, 1);
    // Stage-2 meshlet refinement moved to VsmBinMeshlets (after the caller's barrier).
}

// mode 1 = DYNAMIC pass: NEAR trees into all resident dyn pages (re-rendered with wind
// every frame) + dynUsed flags for the resolve gate. Call right after VsmBin (shares the
// frame slot + the stats clear/barrier; the two dispatches write disjoint buffers/slices).
void CTreeManager::VsmBinDyn(VkCommandBuffer cmd, VkBuffer dynPageTable, VkBuffer dynUsed, VkBuffer clipmapUBO, VkBuffer dynPageList, VkBuffer pageMax, VkBuffer staticPageTable)
{
    if (!m_VsmReady || m_TotalCount == 0 || m_VsmBinPipe == VK_NULL_HANDLE) return;
    if (dynPageTable == VK_NULL_HANDLE || dynUsed == VK_NULL_HANDLE || clipmapUBO == VK_NULL_HANDLE) return;
    const bool hzb = ps_r_vsm_hzb && pageMax != VK_NULL_HANDLE && staticPageTable != VK_NULL_HANDLE;
    if (pageMax == VK_NULL_HANDLE)         pageMax = m_VsmCasterPages->GetHandle();          // layout parity when HZB off
    if (staticPageTable == VK_NULL_HANDLE) staticPageTable = dynPageTable;                    // layout parity when HZB off
    const u32 slot = m_VsmSlot;

    VkBuffer rmask = VK::VSM::GetRMaskHandle();
    const bool rmaskOn = ps_r_vsm_rmask && rmask != VK_NULL_HANDLE;
    if (rmask == VK_NULL_HANDLE) rmask = dynPageTable;   // layout parity when off
    VK::DescriptorWriter(m_VsmDynBinSet[slot])
        .StorageBuffer(0,  m_TreeMetadataBuffer->GetHandle())
        .UniformBuffer(1,  clipmapUBO)
        .StorageBuffer(2,  dynPageTable)
        .StorageBuffer(3,  m_VsmCasterPages->GetHandle())
        .StorageBuffer(4,  m_VsmDynIndirect->GetHandle())
        .StorageBuffer(5,  m_VsmStats->GetHandle())
        .StorageBuffer(6,  dynUsed)                          // slotDirty slot — unused in mode 1, any valid buffer
        .StorageBuffer(7,  dynUsed)                          // dynUsed flags (resolve gate)
        .StorageBuffer(8,  m_VsmNearFlags[slot]->GetHandle())// near set
        .StorageBuffer(9,  pageMax)                          // STATIC occluder max (Option A)
        .StorageBuffer(10, staticPageTable)                  // virtual -> STATIC slot (Option A)
        .StorageBuffer(11, rmask)                            // receiver mask (r_vsm_rmask sub-page cull)
        .StorageBuffer(12, m_VsmTreeBits->GetHandle())       // diag tree bitsets (dyn | static-accum)
        .Flush();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_VsmBinPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_VsmBinLayout, 0, 1, &m_VsmDynBinSet[slot], 0, nullptr);
    // Option A: near trees (dyn) cull ONLY pages hidden behind STATIC occluders → visible near shadow unchanged.
    const u32 castN = _min(m_TotalCount, 0x40000u);   // 18-bit caster-pack ceiling
    struct { u32 count, arenaSlots, mode, hzbOn; float margin; u32 rmaskOn, bitBase; } push{
        castN, kVsmTreeArena, 1u, (u32)(hzb ? 1 : 0), ps_r_vsm_hzb_margin, (u32)(rmaskOn ? 1 : 0),
        (m_TotalCount + 31u) / 32u };
    vkCmdPushConstants(cmd, m_VsmBinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (castN + 63) / 64, 1, 1);

    // Crown-hull path (r_vsm_tree_hull): swap hull-tier trees' crown cmds for hull cmds and
    // zero the crown ones (the meshlet stage-2, which reads instanceCount, then skips them too —
    // it runs after the caller's stage-1→stage-2 barrier, which orders these writes).
    DispatchHullCmd(cmd);
    // Impostor path (r_vsm_tree_impostor): translate the crown page-bin's indirect into a
    // billboard-quad indirect (indexCount→6). Outside the atlas render pass; the draw-indirect
    // hazard is covered by vk_vsm's global barrier after this call.
    DispatchImpostorCmd(cmd);
    // Stage-2 meshlet refinement + diagnostics moved to VsmBinMeshlets (after the caller's barrier).
}

// Frame-start clears for the tree VSM bins — recorded inside vk_vsm's MarkPages fill
// block (before its transfer→compute barrier), so the bin dispatches themselves need
// no per-bin fills or barriers. Lazy resources may not exist on the very first frame
// (VsmBin creates them) — skip here; VsmBin's lazy-init branch then records a one-off
// zero of EVERY GPU-written buffer. That zero is NOT optional: recycled-VRAM garbage
// in an indirect command buffer = billion-instance draw = GPU hang (the level-transition
// DEVICE_LOST of 2026-07-09/10).
void CTreeManager::VsmBinClears(VkCommandBuffer cmd)
{
    if (!m_VsmReady || m_TotalCount == 0) return;
    if (m_VsmStats) vkCmdFillBuffer(cmd, m_VsmStats->GetHandle(), 0, VK_WHOLE_SIZE, 0u);
    // Diag bitsets: clear the per-frame DYN half only — the static half accumulates
    // (the static bin touches dirty pages only, per-frame counts would be noise).
    if (m_VsmTreeBits) vkCmdFillBuffer(cmd, m_VsmTreeBits->GetHandle(), 0,
                                       (VkDeviceSize)((m_TotalCount + 31u) / 32u) * sizeof(u32), 0u);
    if (m_VoxCullStats) vkCmdFillBuffer(cmd, m_VoxCullStats->GetHandle(), 0, VK_WHOLE_SIZE, 0u);   // per-frame append counters / draw counts
    if (m_MeshletVsmReady) {
        if (m_VsmMeshletGroupCount)    vkCmdFillBuffer(cmd, m_VsmMeshletGroupCount->GetHandle(),    0, VK_WHOLE_SIZE, 0u);
        if (m_VsmMeshletGroupCountDyn) vkCmdFillBuffer(cmd, m_VsmMeshletGroupCountDyn->GetHandle(), 0, VK_WHOLE_SIZE, 0u);
        if (m_MeshletStats)            vkCmdFillBuffer(cmd, m_MeshletStats->GetHandle(),            0, VK_WHOLE_SIZE, 0u);
    }
}

// Stage-2 meshlet refinement for both atlases. Call AFTER a barrier ordering the tree
// stage-1 bins' writes (casterPages / indirect) before compute reads — the refinement
// is the only cross-dispatch dependency in the whole bin group. Also hosts the hybrid
// diagnostics (moved from VsmBinDyn so the meshlet stats it copies are this frame's).
void CTreeManager::VsmBinMeshlets(VkCommandBuffer cmd, bool dyn, VkBuffer pageListStatic, VkBuffer pageListDyn, VkBuffer clipmapUBO,
                                  VkBuffer pageMaxBlk, VkBuffer staticPT)
{
    if (!m_VsmReady || m_TotalCount == 0) return;
    // Lazy resource build for a live r_vsm_meshlet 0→1 flip (the load path skips the
    // ~470 MB of pooled command buffers while the mode is off).
    if (ps_r_vsm_meshlet && !m_MeshletVsmReady) CreateMeshletVsmResources();
    const bool binSub = ps_r_profiler > 1;   // per-dispatch attribution (see vk_vsm's Bins/* zones)
    if (IsMeshletMode()) {
        const int zbMesh = binSub ? VK::Prof::ZoneBegin(cmd, "Bins/Meshlet") : -1;
        DispatchMeshletBin(cmd, 0u, pageListStatic, clipmapUBO);
        if (dyn) DispatchMeshletBin(cmd, 1u, pageListDyn, clipmapUBO);
        if (zbMesh >= 0) VK::Prof::ZoneEnd(cmd, zbMesh);
    }
    // Stage-2 brick cull rides the same barrier slot (reads vsm_hull_cmd's outputs).
    {
        const int zbVox = binSub ? VK::Prof::ZoneBegin(cmd, "Bins/VoxCull") : -1;
        DispatchVoxCull(cmd, pageListDyn, pageListStatic, clipmapUBO, pageMaxBlk, staticPT);
        if (zbVox >= 0) VK::Prof::ZoneEnd(cmd, zbVox);
    }

    // ---- Hybrid diagnostics (r_vsm_debug): stats readback + 2s log. stats[3] = instances
    // the DYN pass binned (0 while near trees exist = the GPU never saw the near flags).
    if ((ps_r_vsm_debug || ps_r_profiler > 0) && m_VsmStatsRB) {   // profiler sessions get the tree caster telemetry too
        VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        VkBufferCopy rc{ 0, 0, 8 * sizeof(u32) };
        vkCmdCopyBuffer(cmd, m_VsmStats->GetHandle(), m_VsmStatsRB->GetHandle(), 1, &rc);
        if (m_VsmTreeBits && m_VsmTreeBitsPtr) {
            VkBufferCopy bc{ 0, 0, m_VsmTreeBits->GetSize() };
            vkCmdCopyBuffer(cmd, m_VsmTreeBits->GetHandle(), m_VsmTreeBitsRB->GetHandle(), 1, &bc);
        }
        if (m_VsmStatsPtr && RV().dwTimeGlobal > m_VsmLastLog + 2000) {
            m_VsmLastLog = RV().dwTimeGlobal;
            if (ps_r_vsm_hzb)
                Msg("[VK Trees] VSM shadow-HZB: culled (tree,page) pairs = %u", m_VsmStatsPtr[4]);
            // instStatic/instDyn: what the STATIC (cached, dirty-pages-only) vs DYNAMIC
            // (near, every-frame wind) passes actually rasterize — the "are we promoting
            // too much to dyn / redrawing too much static" watch numbers. arena = pairs
            // reserved this frame of the shared casterPages arena (drops must stay 0).
            Msg("[VK Trees] VSM hybrid: near=%u/%u hull=%u hullFar=%u pendingInval=%zu | GPU draws=%u instStatic=%u instDyn=%u maxPages=%u | arena=%u/%u drops=%u%s | rmaskCulled=%u",
                m_VsmNearCount, m_TotalCount, IsHullMode() ? m_VsmHullCount : 0u,
                IsHullMode() ? m_VsmHullFarCount : 0u, m_VsmPendingInval.size(),
                m_VsmStatsPtr[0], m_VsmStatsPtr[1] - m_VsmStatsPtr[3], m_VsmStatsPtr[3],
                m_VsmStatsPtr[2],
                m_VsmStatsPtr[6], kVsmTreeArena, m_VsmStatsPtr[7], m_VsmStatsPtr[7] ? " [ARENA FULL!]" : "",
                m_VsmStatsPtr[5]);   // receiver-mask culled (tree,page) pairs, dyn bin (r_vsm_rmask)
            // Distinct-tree attribution (diag bitsets, a frame or two stale): how many
            // trees shadow through the ACTIVE dyn atlas this frame vs how many live in
            // the CACHED static atlas (accumulated since it last fully invalidated).
            if (m_VsmTreeBitsPtr) {
                const u32 W = (m_TotalCount + 31u) / 32u;
                u32 dynN = 0, cachedN = 0;
                for (u32 i = 0; i < W; ++i) { dynN += (u32)std::popcount(m_VsmTreeBitsPtr[i]); cachedN += (u32)std::popcount(m_VsmTreeBitsPtr[W + i]); }
                Msg("[VK Trees] shadow trees: dyn(active)=%u/frame, cached(static atlas)=%u accum of %u",
                    dynN, cachedN, m_TotalCount);
            }
        }
        // Stage-2 brick cull: compacted draw counts + list usage + shadow-HZB culled bricks.
        if (m_VoxCullActive && m_VoxCullStatsRB && m_VoxCullStatsPtr) {
            VkBufferCopy vc{ 0, 0, 8 * sizeof(u32) };
            vkCmdCopyBuffer(cmd, m_VoxCullStats->GetHandle(), m_VoxCullStatsRB->GetHandle(), 1, &vc);
            if (RV().dwTimeGlobal == m_VsmLastLog)   // logged this tick above (stale by a frame — fine)
                Msg("[VK Trees] vox-cull: trees=%u cmdD=%u cmdS=%u list=%u/%u overflow=%u hzbCulled=%u rmaskCulled=%u",
                    m_VoxCullTreeN,
                    m_VoxCullStatsPtr[0], m_VoxCullStatsPtr[1], m_VoxCullStatsPtr[2], kVoxCullListCap,
                    m_VoxCullStatsPtr[3], m_VoxCullStatsPtr[4], m_VoxCullStatsPtr[5]);
        }
        // Meshlet path (r_vsm_meshlet): commands emitted + group overflow (truncation).
        if (IsMeshletMode() && m_MeshletStatsRB && m_MeshletStatsPtr) {
            VkBufferCopy mc{ 0, 0, 4 * sizeof(u32) };
            vkCmdCopyBuffer(cmd, m_MeshletStats->GetHandle(), m_MeshletStatsRB->GetHandle(), 1, &mc);
            if (RV().dwTimeGlobal == m_VsmLastLog)   // logged this tick above
                Msg("[VK Trees] VSM meshlet: cmds=%u overflow=%u (clusters=%u, %u cmd/tree)%s",
                    m_MeshletStatsPtr[0], m_MeshletStatsPtr[1], m_MeshletTotal, kCmdPerTree,
                    m_MeshletStatsPtr[1] ? " [TRUNCATED!]" : "");
        }
    }
}

void CTreeManager::VsmRender(VkCommandBuffer cmd, VkBuffer pageList, VkBuffer clipmapUBO)
{
    if (!m_VsmReady || m_TotalCount == 0 || m_XformDescSet == VK_NULL_HANDLE) return;
    if (m_VsmPagePipe24 == VK_NULL_HANDLE && m_VsmPagePipe28 == VK_NULL_HANDLE) return;
    if (pageList == VK_NULL_HANDLE || clipmapUBO == VK_NULL_HANDLE) return;
    const u32 slot = m_VsmSlot;

    VK::DescriptorWriter(m_VsmPageSet[slot])
        .StorageBuffer(0, pageList)
        .StorageBuffer(1, m_VsmCasterPages->GetHandle())
        .UniformBuffer(2, clipmapUBO)
        .Flush();

    // FAR crown-hull tier (r_vsm_tree_hull 2): distant foliage casts rigid opaque lobes
    // into the dirty static pages (its crown cmds were zeroed by vsm_hull_cmd) — the
    // shadow LOD stays monotonic beyond the near set and dirty refreshes stop paying the
    // full alpha-tested crown raster.
    if (IsHullMode() && ps_r_vsm_tree_hull >= 2) {
        DrawHulls(cmd, slot, /*staticPass*/true);
        DrawVoxCasters(cmd, slot, /*staticPass*/true);   // voxel-cloud far tier (vsm_hull_cmd routed the pages)
    }

    // Phase B: meshlet-cull path draws only the clusters that survived stage 2 (STATIC = rigid).
    if (IsMeshletMode()) { DrawMeshlets(cmd, 0u, m_VsmPageSet[slot], /*wind*/false); return; }

    // STATIC pass push: NO wind, ever — the toroidal cache needs frame-invariant page
    // content (per-page wind was the "jelly": staggered refreshes froze each page at a
    // different wind phase). Near trees sway via VsmRenderDyn instead.
    struct VsmPagePush {
        float    uvScale, alphaRef; u32 cap, pad;
        Fvector4 wind_params, wsetup_trees, wind_anim;
    } pc{};
    pc.uvScale = 1.0f / 2048.0f; pc.alphaRef = TreeAlphaRef(200.0f / 255.0f); pc.cap = kVsmTreeCap;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 0, 1, &m_XformDescSet, 0, nullptr);       // set0 transforms
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 2, 1, &m_VsmPageSet[slot], 0, nullptr);   // set2 page data
    vkCmdPushConstants(cmd, m_VsmPageLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    VkPipeline lastPipe = VK_NULL_HANDLE; VkDescriptorSet lastTex = VK_NULL_HANDLE;
    for (const TreeIndirectGroup& grp : m_Groups) {
        VkPipeline pipe = (grp.tcOffset == 24) ? m_VsmPagePipe24 : m_VsmPagePipe28;
        if (pipe == VK_NULL_HANDLE || grp.descSetIdx >= m_TexDescSets.size()) continue;
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; lastTex = VK_NULL_HANDLE; }
        VkDescriptorSet tex = m_TexDescSets[grp.descSetIdx];
        if (tex != lastTex) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 1, 1, &tex, 0, nullptr); lastTex = tex; }
        VkDeviceSize vbOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &vbOff);
        vkCmdBindIndexBuffer(cmd, grp.ib, 0, VK_INDEX_TYPE_UINT16);
        // [#7] ONE multi-draw indirect for the whole group — the group's trees are contiguous
        // in m_VsmIndirect (tree-index order), so a single call replaces meshCount individual
        // draws. Cuts ~6288 draw calls/frame to one per group (~tens). Trees with no dirty pages
        // carry instanceCount=0 from the bin → ~free on the GPU. Needs multiDrawIndirect +
        // drawIndirectFirstInstance (both enabled in HW_Vulkan).
        vkCmdDrawIndexedIndirect(cmd, m_VsmIndirect->GetHandle(),
                                 (VkDeviceSize)grp.meshOffset * sizeof(VkDrawIndexedIndirectCommand),
                                 grp.meshCount, sizeof(VkDrawIndexedIndirectCommand));
    }
}

// DYNAMIC pass: near trees into the dyn atlas with LIVE wind (one time slice per frame →
// smooth coherent sway, the r_vsm_tree_wind feature done right). Call inside vk_vsm's
// RenderAtlas DYNAMIC pass, after the skinned/grass casters.
void CTreeManager::VsmRenderDyn(VkCommandBuffer cmd, VkBuffer dynPageList, VkBuffer clipmapUBO)
{
    if (!ps_r_vsm_tree_wind) return;   // hybrid off → all trees are in the static pass
    if (!m_VsmReady || m_TotalCount == 0 || m_XformDescSet == VK_NULL_HANDLE) return;
    if (m_VsmPageDynPipe24 == VK_NULL_HANDLE && m_VsmPageDynPipe28 == VK_NULL_HANDLE) return;
    if (dynPageList == VK_NULL_HANDLE || clipmapUBO == VK_NULL_HANDLE) return;
    const u32 slot = m_VsmSlot;

    VK::DescriptorWriter(m_VsmDynPageSet[slot])
        .StorageBuffer(0, dynPageList)
        .StorageBuffer(1, m_VsmCasterPages->GetHandle())
        .UniformBuffer(2, clipmapUBO)
        .Flush();

    // Crown-hull tier (r_vsm_tree_hull): draw the hull-tier trees FIRST (opaque low-poly
    // lobes, one multi-draw, no texture) — their crown cmds were zeroed by vsm_hull_cmd,
    // so the plain/meshlet crown draws below cover only the crown tier. Depth-only pass →
    // draw order irrelevant.
    if (IsHullMode()) {
        DrawHulls(cmd, slot, /*staticPass*/false);
        DrawVoxCasters(cmd, slot, /*staticPass*/false);   // voxel-cloud near tier + crown crossfade band
    }

    // Impostor path (r_vsm_tree_impostor): ONE sun-facing billboard per near tree instead of
    // the crown mesh — 2 tris vs thousands of crown verts every frame. Same page routing (set2);
    // the quad samples the baked silhouette atlas. Supersedes the crown mesh + meshlet paths.
    if (IsImpostorMode()) {
        struct ImpPush {
            Fvector4 sunDir, wind_params, wsetup_trees, wind_anim;
            u32 cap, numMeshes; float alphaRef; u32 facetCount;
        } pc{};
        Fvector sd; sd.set(0.f, -1.f, 0.f);
        if (g_pGamePersistent) if (auto* E = ES().CurrentEnv) sd = E->sun_dir;
        pc.sunDir.set(sd.x, sd.y, sd.z, ps_r_vsm_tree_impostor_scale);
        pc.wsetup_trees.set(ps_r_wind_tree_anim, ps_r_wind_tree_trunk, ps_r_wind_tree_bend, 0.1f);
        if (g_pGamePersistent) {
            const Fvector3 wa = ES().wind_anim;
            pc.wind_anim.set(wa.x, wa.y, wa.z, ps_r_wind_tree_flutter);
            if (auto* E = ES().CurrentEnv)
                pc.wind_params.set(E->wind_direction, E->wind_velocity, 0.0f, 0.0f);
        }
        pc.wind_params.w = ps_r_wind_tree_crown;   // flutter height gate — must match the crown pass
        pc.cap = kVsmTreeCap; pc.numMeshes = m_ImpostorMeshCount; pc.alphaRef = 0.5f; pc.facetCount = kImpFacets;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorLayout, 0, 1, &m_XformDescSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorLayout, 1, 1, &m_ImpostorSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorLayout, 2, 1, &m_VsmDynPageSet[slot], 0, nullptr);
        vkCmdPushConstants(cmd, m_ImpostorLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        // ONE multi-draw over all trees: far/idle trees carry instanceCount 0 from the copy → free.
        vkCmdDrawIndirect(cmd, m_VsmImpostorDynIndirect->GetHandle(), 0, m_TotalCount, sizeof(VkDrawIndirectCommand));
        return;
    }

    // Phase B: meshlet-cull path draws only surviving clusters, with LIVE wind (near trees).
    if (IsMeshletMode()) { DrawMeshlets(cmd, 1u, m_VsmDynPageSet[slot], /*wind*/true); return; }

    // Live wind push — same params as the forward tree pass (ssfx_tree_wind).
    struct VsmPagePush {
        float    uvScale, alphaRef; u32 cap, pad;
        Fvector4 wind_params, wsetup_trees, wind_anim;
    } pc{};
    pc.uvScale = 1.0f / 2048.0f; pc.alphaRef = TreeAlphaRef(200.0f / 255.0f); pc.cap = kVsmTreeCap;
    pc.wsetup_trees.set(ps_r_wind_tree_anim, ps_r_wind_tree_trunk, ps_r_wind_tree_bend, 0.1f);
    if (g_pGamePersistent) {
        const Fvector3 wa = ES().wind_anim;
        pc.wind_anim.set(wa.x, wa.y, wa.z, ps_r_wind_tree_flutter);
        if (auto* E = ES().CurrentEnv)
            pc.wind_params.set(E->wind_direction, E->wind_velocity, 0.0f, 0.0f);
    }
    pc.wind_params.w = ps_r_wind_tree_crown;   // flutter height gate — must match the forward pass
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 0, 1, &m_XformDescSet, 0, nullptr);          // set0 transforms
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 2, 1, &m_VsmDynPageSet[slot], 0, nullptr);   // set2 page data (dyn)
    vkCmdPushConstants(cmd, m_VsmPageLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    // VRS: 2x2 coarse shading of the crown-shadow fill (A/B via r_vsm_tree_vrs). The dyn
    // pipelines carry a FRAGMENT_SHADING_RATE dynamic state (HW pipeline-rate), so the rate
    // MUST be set before the draws; reset to 1x1 after so nothing downstream inherits it.
    // No-op when HW lacks pipeline-rate VRS. Attacks the fill-bound cost that survives WIND.
    const u32 tvrs = (ps_r_vsm_tree_vrs && VulkanHW.m_bVRSPipelineSupported) ? 2u : 1u;
    VK::VRS::CmdSetPipelineRate(cmd, tvrs, tvrs);

    VkPipeline lastPipe = VK_NULL_HANDLE; VkDescriptorSet lastTex = VK_NULL_HANDLE;
    for (const TreeIndirectGroup& grp : m_Groups) {
        VkPipeline pipe = (grp.tcOffset == 24) ? m_VsmPageDynPipe24 : m_VsmPageDynPipe28;
        if (pipe == VK_NULL_HANDLE || grp.descSetIdx >= m_TexDescSets.size()) continue;
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; lastTex = VK_NULL_HANDLE; }
        VkDescriptorSet tex = m_TexDescSets[grp.descSetIdx];
        if (tex != lastTex) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 1, 1, &tex, 0, nullptr); lastTex = tex; }
        VkDeviceSize vbOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &vbOff);
        vkCmdBindIndexBuffer(cmd, grp.ib, 0, VK_INDEX_TYPE_UINT16);
        // Far trees carry instanceCount=0 in m_VsmDynIndirect → ~free on the GPU.
        vkCmdDrawIndexedIndirect(cmd, m_VsmDynIndirect->GetHandle(),
                                 (VkDeviceSize)grp.meshOffset * sizeof(VkDrawIndexedIndirectCommand),
                                 grp.meshCount, sizeof(VkDrawIndexedIndirectCommand));
    }
    VK::VRS::CmdSetPipelineRate(cmd, 1u, 1u);   // reset coarse rate (dyn crowns only)
}

// ============================================================================
// Impostor crown shadows (r_vsm_tree_impostor). Bakes a per-UNIQUE-MESH silhouette
// atlas at load (8 azimuth facets/row, alpha-tested from the crown diffuse), then in
// the DYNAMIC near-tree pass replaces the crown MESH raster with ONE sun-facing
// billboard per tree (2 tris) routed through the same VSM page mechanism — killing
// the geometry/raster cost that survives WIND. See tree_vsm_impostor.* + the plan
// memory [vulkan-vsm-tree-impostor-plan]. Best-effort: any failure leaves the atlas
// unbuilt → IsImpostorMode() false → the crown mesh path runs unchanged.
// ============================================================================
bool CTreeManager::IsImpostorMode() const
{
    return ps_r_vsm_tree_impostor && ps_r_vsm_tree_wind && m_ImpostorReady;
}

void CTreeManager::BuildImpostorAtlas(const xr_vector<vkFTreeVisual*>& trees,
                                      const xr_vector<GpuTreeMeta>& meta,
                                      const xr_vector<u32>& treeTexIdx)
{
    VK::Vram::Scope _vram_scope("Trees");
    if (m_TotalCount == 0 || !g_ShaderManager) return;
    if (m_TexDescSets.empty() || trees.size() != m_TotalCount) return;

    VkDevice dev = VulkanHW.m_Device;

    // ----- Dedup unique meshes by DRAWN window (vb, ib, first_vertex, ib_first, index_count).
    struct MeshKey { VkBuffer vb, ib; u32 fv, ibf, ic, tc, desc; };
    xr_vector<MeshKey> uniq; uniq.reserve(64);
    xr_vector<u32> treeSil(m_TotalCount, 0u);
    for (u32 i = 0; i < m_TotalCount; ++i) {
        vkFTreeVisual* t = trees[i];
        MeshKey k{ t->m_mesh.p_rm_Vertices->GetHandle(), t->m_mesh.p_rm_Indices->GetHandle(),
                   meta[i].first_vertex, meta[i].ib_first, meta[i].index_count,
                   t->m_mesh.tcOffset, treeTexIdx[i] };
        u32 slot = ~0u;
        for (u32 j = 0; j < uniq.size(); ++j) {
            const MeshKey& u = uniq[j];
            if (u.vb == k.vb && u.ib == k.ib && u.fv == k.fv && u.ibf == k.ibf && u.ic == k.ic) { slot = j; break; }
        }
        if (slot == ~0u) { slot = (u32)uniq.size(); uniq.push_back(k); }
        treeSil[i] = slot;
    }
    m_ImpostorMeshCount = (u32)uniq.size();
    if (m_ImpostorMeshCount == 0) return;

    // Per-unique-mesh local framing (box center + world sphere radius from the first tree using it).
    struct MeshFrame { Fvector center; float radius; };
    xr_vector<MeshFrame> frame(m_ImpostorMeshCount, MeshFrame{ Fvector{}, 1.0f });
    xr_vector<u8> haveFrame(m_ImpostorMeshCount, 0);
    for (u32 i = 0; i < m_TotalCount; ++i) {
        u32 s = treeSil[i]; if (haveFrame[s]) continue;
        haveFrame[s] = 1;
        const Fbox& bb = trees[i]->vis.box;   // LOCAL AABB (bake renders local verts)
        Fvector c; c.add(bb.min, bb.max); c.mul(0.5f);
        frame[s].center = c;
        frame[s].radius = (meta[i].sphere_R > 1e-3f) ? meta[i].sphere_R : 1.0f;
    }

    // ----- Silhouette atlas (R8 coverage): FACETS cols × meshCount rows, kImpCell each.
    const u32 W = kImpFacets * kImpCell;
    const u32 H = m_ImpostorMeshCount * kImpCell;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.extent = { W, H, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.format = VK_FORMAT_R8_UNORM;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    aci.priority = 1.0f;   // tree impostor atlas — hot render target, never evict before streamable textures
    if (VK::Vram::CreateImage(VulkanHW.m_Allocator, &ici, &aci, &m_ImpostorAtlas, &m_ImpostorAtlasAlloc, nullptr) != VK_SUCCESS) {
        Msg("![VK Trees] impostor atlas image create failed"); m_ImpostorMeshCount = 0; return;
    }
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = m_ImpostorAtlas; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = ici.format;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(dev, &vci, nullptr, &m_ImpostorAtlasView);
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev, &sci, nullptr, &m_ImpostorSampler);

    // Per-tree silhouette-row SSBO (host-visible, read in the impostor VS).
    m_ImpostorSilIdx = xr_new<CVulkanBuffer>();
    m_ImpostorSilIdx->Create((VkDeviceSize)m_TotalCount * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    if (void* p = m_ImpostorSilIdx->Map()) memcpy(p, treeSil.data(), (size_t)m_TotalCount * sizeof(u32));

    // ----- Transient bake pipeline (tree_bake_silhouette VS/FS, tcOffset 24 + 28).
    VkShaderModule bvs = g_ShaderManager->Load("tree_bake_silhouette.vert.spv");
    VkShaderModule bfs = g_ShaderManager->Load("tree_bake_silhouette.frag.spv");
    if (!bvs || !bfs) { Msg("![VK Trees] impostor bake shaders missing"); m_ImpostorMeshCount = 0; return; }
    struct BakePush { Fmatrix mvp; float uvScale, alphaRef; };
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BakePush) };
    VkPipelineLayout bakeLayout = VK_NULL_HANDLE;
    { VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
      plci.setLayoutCount = 1; plci.pSetLayouts = &m_TexDescLayout; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
      vkCreatePipelineLayout(dev, &plci, nullptr, &bakeLayout); }
    VkPipeline bakePipe24 = VK_NULL_HANDLE, bakePipe28 = VK_NULL_HANDLE;
    auto makeBake = [&](u32 tcOffset, VkPipeline& out) {   // colour-only — no depth state at all
        out = VK::GfxPipelineBuilder(bakeLayout)
            .Vert(bvs).Frag(bfs)
            .Binding(0, 32)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Attr(1, 0, VK_FORMAT_R16G16_SSCALED,   tcOffset)
            .Cull(VK_CULL_MODE_NONE)
            .Color(VK_FORMAT_R8_UNORM, VK_COLOR_COMPONENT_R_BIT)
            .Build("Trees impostor bake (tcOff=%u)", tcOffset);
    };
    makeBake(24, bakePipe24); makeBake(28, bakePipe28);

    // ----- Bake: one-off command buffer, all cells in one dynamic-rendering pass.
    VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
    VK::ImageBarrier(cmd, m_ImpostorAtlas, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VK::RenderingBuilder(W, H)
        .ColorClear(m_ImpostorAtlasView, VkClearColorValue{ { 0.f, 0.f, 0.f, 0.f } })
        .Begin(cmd);
    for (u32 s = 0; s < m_ImpostorMeshCount; ++s) {
        const MeshKey& k = uniq[s];
        VkPipeline pipe = (k.tc == 24) ? bakePipe24 : bakePipe28;
        if (pipe == VK_NULL_HANDLE || k.desc >= m_TexDescSets.size()) continue;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bakeLayout, 0, 1, &m_TexDescSets[k.desc], 0, nullptr);
        VkDeviceSize vbOff = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &k.vb, &vbOff);
        vkCmdBindIndexBuffer(cmd, k.ib, 0, VK_INDEX_TYPE_UINT16);
        const Fvector C = frame[s].center; const float R = frame[s].radius;
        for (u32 f = 0; f < kImpFacets; ++f) {
            const float ang = float(f) * (6.28318530718f / float(kImpFacets));
            Fvector dir; dir.set(_cos(ang), 0.f, _sin(ang));
            Fvector eye; eye.mad(C, dir, 2.0f * R);      // eye = C + dir*2R (on the sun-facing side)
            Fvector look; look.invert(dir);              // look toward C along -dir
            Fvector up; up.set(0.f, 1.f, 0.f);
            Fmatrix view; view.build_camera_dir(eye, look, up);
            Fmatrix proj; proj.build_projection_ortho(2.0f * R, 2.0f * R, 0.05f, 4.0f * R);
            // ⚠ RAW 200/255, NOT TreeAlphaRef(): this bake writes an atlas that outlives the
            // frame. Baking it while r_tree_alpharef 0 is up would store solid rectangles as
            // the crown silhouette and every later impostor would inherit them — a probe
            // corrupting persistent data is exactly the failure the accessor exists to avoid.
            BakePush bp{}; bp.mvp.mul(proj, view); bp.uvScale = 1.0f / 2048.0f; bp.alphaRef = 200.0f / 255.0f;
            VkViewport vpc{ float(f * kImpCell), float(s * kImpCell), float(kImpCell), float(kImpCell), 0.f, 1.f };
            VkRect2D scc{ { int(f * kImpCell), int(s * kImpCell) }, { kImpCell, kImpCell } };
            vkCmdSetViewport(cmd, 0, 1, &vpc); vkCmdSetScissor(cmd, 0, 1, &scc);
            vkCmdPushConstants(cmd, bakeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bp), &bp);
            vkCmdDrawIndexed(cmd, k.ic, 1, k.ibf, (s32)k.fv, 0);
        }
    }
    vkCmdEndRendering(cmd);
    VK::ImageBarrier(cmd, m_ImpostorAtlas, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VulkanHW.EndSingleTimeCommands(cmd);

    if (bakePipe24) vkDestroyPipeline(dev, bakePipe24, nullptr);
    if (bakePipe28) vkDestroyPipeline(dev, bakePipe28, nullptr);
    if (bakeLayout) vkDestroyPipelineLayout(dev, bakeLayout, nullptr);

    Msg("[VK Trees] impostor silhouette atlas baked: %u meshes × %u facets (%ux%u R8)", m_ImpostorMeshCount, kImpFacets, W, H);
}

void CTreeManager::CreateImpostorResources()
{
    if (m_ImpostorReady) return;
    if (m_ImpostorAtlasView == VK_NULL_HANDLE || !m_ImpostorSilIdx || m_ImpostorMeshCount == 0) return;
    if (m_XformDescLayout == VK_NULL_HANDLE || m_VsmPageSetL == VK_NULL_HANDLE || !m_TreeMetadataBuffer || !m_VsmDynIndirect) return;
    if (!g_ShaderManager) return;
    VkShaderModule vs = g_ShaderManager->Load("tree_vsm_impostor.vert.spv");
    VkShaderModule fs = g_ShaderManager->Load("tree_vsm_impostor.frag.spv");
    VkShaderModule cs = g_ShaderManager->Load("vsm_impostor_cmd.comp.spv");
    if (!vs || !fs || !cs) { Msg("![VK Trees] impostor render shaders missing — impostor disabled"); return; }
    VkDevice dev = VulkanHW.m_Device;

    m_VsmImpostorDynIndirect = xr_new<CVulkanBuffer>();
    m_VsmImpostorDynIndirect->Create((VkDeviceSize)m_TotalCount * sizeof(VkDrawIndirectCommand),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &m_ImpostorPool) != VK_SUCCESS) return;

    // set1: meta SSBO (VS) + silIdx SSBO (VS) + silhouette sampler (FS).
    { VkDescriptorSetLayoutBinding b[3]{};
      b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
      b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
      b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; lci.bindingCount = 3; lci.pBindings = b;
      if (vkCreateDescriptorSetLayout(dev, &lci, nullptr, &m_ImpostorSetL) != VK_SUCCESS) return;
      VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; dai.descriptorPool = m_ImpostorPool; dai.descriptorSetCount = 1; dai.pSetLayouts = &m_ImpostorSetL;
      if (vkAllocateDescriptorSets(dev, &dai, &m_ImpostorSet) != VK_SUCCESS) return;
      VK::DescriptorWriter(m_ImpostorSet)
          .StorageBuffer(0, m_TreeMetadataBuffer->GetHandle())
          .StorageBuffer(1, m_ImpostorSilIdx->GetHandle())
          .ImageSampler (2, m_ImpostorAtlasView, m_ImpostorSampler)
          .Flush();
    }

    // Graphics pipeline: {set0 xform, set1 impostor, set2 page}, no vertex input (gl_VertexIndex quad).
    m_ImpostorLayout = VK::MakePipelineLayout({ m_XformDescLayout, m_ImpostorSetL, m_VsmPageSetL }, 80,
                                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    if (m_ImpostorLayout == VK_NULL_HANDLE) return;
    m_ImpostorPipe = VK::GfxPipelineBuilder(m_ImpostorLayout)   // no vertex input — gl_VertexIndex quad
        .Vert(vs).Frag(fs)
        .Cull(VK_CULL_MODE_NONE)
        .DynamicDepthBias()
        .Depth(true, true)
        .DepthTarget(VK_FORMAT_D16_UNORM)
        .Build("Trees impostor");
    if (m_ImpostorPipe == VK_NULL_HANDLE) return;

    // Indirect builder compute: set {0 src (dyn indirect), 1 dst (impostor indirect)}.
    { m_ImpostorCmdSetL = VK::MakeSetLayout({ kSSBO, kSSBO }, VK_SHADER_STAGE_COMPUTE_BIT, "Trees.ImpostorCmd");
      if (m_ImpostorCmdSetL == VK_NULL_HANDLE) return;
      if (!VK::AllocSets(m_ImpostorPool, m_ImpostorCmdSetL, 1, &m_ImpostorCmdSet, "Trees.ImpostorCmd")) return;
      VK::DescriptorWriter(m_ImpostorCmdSet)
          .StorageBuffer(0, m_VsmDynIndirect->GetHandle())
          .StorageBuffer(1, m_VsmImpostorDynIndirect->GetHandle())
          .Flush();
      m_ImpostorCmdLayout = VK::MakePipelineLayout({ m_ImpostorCmdSetL }, sizeof(u32));
      if (m_ImpostorCmdLayout == VK_NULL_HANDLE) return;
      m_ImpostorCmdPipe = VK::CreateComputePipeline(cs, m_ImpostorCmdLayout, "Trees.ImpostorCmd");
      if (m_ImpostorCmdPipe == VK_NULL_HANDLE) return; }

    m_ImpostorReady = true;
    Msg("[VK Trees] impostor crown shadows ready (%u meshes, %u facets, scale %.2f)", m_ImpostorMeshCount, kImpFacets, ps_r_vsm_tree_impostor_scale);
}

// Build m_VsmImpostorDynIndirect from the DYN crown bin's m_VsmDynIndirect (indexCount→6,
// non-indexed). Records a compute→compute barrier on the source; the draw-indirect
// hazard on the DST is covered by vk_vsm's global SHADER_WRITE→INDIRECT barrier after
// VsmBinDyn. Call at the end of VsmBinDyn (outside the atlas render pass).
void CTreeManager::DispatchImpostorCmd(VkCommandBuffer cmd)
{
    if (!IsImpostorMode() || m_ImpostorCmdPipe == VK_NULL_HANDLE) return;
    VK::BufferBarrier(cmd, m_VsmDynIndirect->GetHandle(),
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ImpostorCmdPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ImpostorCmdLayout, 0, 1, &m_ImpostorCmdSet, 0, nullptr);
    u32 count = m_TotalCount;
    vkCmdPushConstants(cmd, m_ImpostorCmdLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32), &count);
    vkCmdDispatch(cmd, (m_TotalCount + 63) / 64, 1, 1);
}

void CTreeManager::DestroyImpostor()
{
    VkDevice dev = VulkanHW.m_Device;
    if (dev == VK_NULL_HANDLE) return;
    if (m_ImpostorCmdPipe)   { vkDestroyPipeline(dev, m_ImpostorCmdPipe, nullptr); m_ImpostorCmdPipe = VK_NULL_HANDLE; }
    if (m_ImpostorCmdLayout) { vkDestroyPipelineLayout(dev, m_ImpostorCmdLayout, nullptr); m_ImpostorCmdLayout = VK_NULL_HANDLE; }
    if (m_ImpostorCmdSetL)   { vkDestroyDescriptorSetLayout(dev, m_ImpostorCmdSetL, nullptr); m_ImpostorCmdSetL = VK_NULL_HANDLE; }
    if (m_ImpostorPipe)      { vkDestroyPipeline(dev, m_ImpostorPipe, nullptr); m_ImpostorPipe = VK_NULL_HANDLE; }
    if (m_ImpostorLayout)    { vkDestroyPipelineLayout(dev, m_ImpostorLayout, nullptr); m_ImpostorLayout = VK_NULL_HANDLE; }
    if (m_ImpostorSetL)      { vkDestroyDescriptorSetLayout(dev, m_ImpostorSetL, nullptr); m_ImpostorSetL = VK_NULL_HANDLE; }
    if (m_ImpostorPool)      { vkDestroyDescriptorPool(dev, m_ImpostorPool, nullptr); m_ImpostorPool = VK_NULL_HANDLE; m_ImpostorSet = VK_NULL_HANDLE; m_ImpostorCmdSet = VK_NULL_HANDLE; }
    if (m_VsmImpostorDynIndirect) { m_VsmImpostorDynIndirect->Destroy(); xr_delete(m_VsmImpostorDynIndirect); }
    if (m_ImpostorSilIdx)    { m_ImpostorSilIdx->Destroy(); xr_delete(m_ImpostorSilIdx); }
    if (m_ImpostorSampler)   { vkDestroySampler(dev, m_ImpostorSampler, nullptr); m_ImpostorSampler = VK_NULL_HANDLE; }
    if (m_ImpostorAtlasView) { vkDestroyImageView(dev, m_ImpostorAtlasView, nullptr); m_ImpostorAtlasView = VK_NULL_HANDLE; }
    if (m_ImpostorAtlas)     { VK::Vram::DestroyImage(VulkanHW.m_Allocator, m_ImpostorAtlas, m_ImpostorAtlasAlloc); m_ImpostorAtlas = VK_NULL_HANDLE; m_ImpostorAtlasAlloc = VK_NULL_HANDLE; }
    m_ImpostorMeshCount = 0;
    m_ImpostorReady = false;
}

// ============================================================================
// Crown-HULL shadow casters (r_vsm_tree_hull) — the AC-Shadows-style caster LOD.
// Near trees beyond r_vsm_tree_hull_dist swap the alpha-tested crown MESH for a baked
// low-poly OPAQUE hull (ellipsoid lobes, BuildMeshlets bake): no texture fetch, no
// discard, early-Z — the first lever aimed at the PROVEN alpha-test-fill bound of
// VSM/DynTrees (meshlet/HZB/D16/VRS all missed it). The closest trees keep the full
// crown (dappling perfect where the player looks). Geometry always baked → live A/B.
// ============================================================================
bool CTreeManager::IsHullMode() const
{
    return ps_r_vsm_tree_hull && ps_r_vsm_tree_wind && m_HullReady && m_HullDataReady && !IsImpostorMode();
}

void CTreeManager::CreateHullResources()
{
    if (m_HullReady) return;
    if (!m_HullDataReady || !m_HullVB || !m_HullIB || !m_HullInfoBuffer) return;
    if (m_VsmPageLayout == VK_NULL_HANDLE || !m_VsmDynIndirect || m_TotalCount == 0) return;
    if (!g_ShaderManager) return;
    VkShaderModule vs  = g_ShaderManager->Load("tree_vsm_hull.vert.spv");
    VkShaderModule vsS = g_ShaderManager->Load("tree_vsm_hull_s.vert.spv");
    VkShaderModule fs  = g_ShaderManager->Load("tree_vsm_hull.frag.spv");
    VkShaderModule cs  = g_ShaderManager->Load("vsm_hull_cmd.comp.spv");
    if (!vs || !vsS || !fs || !cs) { Msg("![VK Trees] crown-hull shaders missing — r_vsm_tree_hull disabled"); return; }
    const u32 N = VK_FRAMES_IN_FLIGHT;

    // TRANSFER_DST: joins the lazy-init zero fill (recycled-VRAM garbage in an indirect
    // buffer = billion-instance draw = DEVICE_LOST — the level-transition lesson).
    auto makeIndirect = [&](CVulkanBuffer*& b) {
        b = xr_new<CVulkanBuffer>();
        b->Create((VkDeviceSize)m_TotalCount * sizeof(VkDrawIndexedIndirectCommand),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    };
    makeIndirect(m_VsmHullIndirect);    // DYN hull tier
    makeIndirect(m_VsmHullIndirectS);   // STATIC far tier (r_vsm_tree_hull 2)

    // Voxel-caster buffers: NON-indexed indirect cmds (4 u32/tree) + the per-frame
    // choice ring (host-visible, CPU fills camera-distance LOD/fade/density per tree).
    auto makeIndirect4 = [&](CVulkanBuffer*& b) {
        b = xr_new<CVulkanBuffer>();
        b->Create((VkDeviceSize)m_TotalCount * sizeof(u32) * 4,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    };
    makeIndirect4(m_VsmVoxIndirect);
    makeIndirect4(m_VsmVoxIndirectS);
    for (u32 i = 0; i < N; ++i) {
        m_VoxChoiceBuf[i] = xr_new<CVulkanBuffer>();
        m_VoxChoiceBuf[i]->Create((VkDeviceSize)m_TotalCount * sizeof(u32) * 4,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        m_VoxChoicePtr[i] = m_VoxChoiceBuf[i]->Map();
    }

    // Own pool (never resize the shared VSM pool — the regression-prone class of change).
    // Cmd-swap compute: {0 dyn crown RW, 1 hullInfo, 2 nearFlags (per-frame ring),
    //                    3 dyn hull W, 4 STATIC crown RW, 5 static hull W,
    //                    6 vox choice (ring), 7 dyn vox W, 8 static vox W}.
    {
        if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                    N, m_HullCmdSetL, m_HullCmdPool, m_HullCmdSet,
                                    VK_SHADER_STAGE_COMPUTE_BIT, "Trees.HullCmd"))
            return;
        m_HullCmdLayout = VK::MakePipelineLayout({ m_HullCmdSetL }, 3 * sizeof(u32));
        if (m_HullCmdLayout == VK_NULL_HANDLE) return;
        m_HullCmdPipe = VK::CreateComputePipeline(cs, m_HullCmdLayout, "Trees.HullCmd");
        if (m_HullCmdPipe == VK_NULL_HANDLE) return;
    }

    // Opaque depth-only hull raster ×2 (dyn/static atlas VS variants). Reuses
    // m_VsmPageLayout (set1 diffuse unused by the shader — stays bound from the crown
    // draws, layout-compatible). Tight vec3 VB.
    auto createHullPipe = [&](VkShaderModule hvs, VkPipeline& out) {
        out = VK::GfxPipelineBuilder(m_VsmPageLayout)
            .Vert(hvs).Frag(fs)
            .Binding(0, 12)
            .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
            .Cull(VK_CULL_MODE_NONE)
            .DynamicDepthBias()
            .Depth(true, true)
            .DepthTarget(VK_FORMAT_D16_UNORM)   // VSM atlas format
            .Build("Trees crown-hull");
    };
    createHullPipe(vs,  m_VsmHullPipe);    // dyn atlas (wind push)
    createHullPipe(vsS, m_VsmHullPipeS);   // static atlas (rigid)
    if (m_VsmHullPipe == VK_NULL_HANDLE || m_VsmHullPipeS == VK_NULL_HANDLE) return;

    // ---- Voxel-BRICK caster (r_vsm_tree_hull_vox): set3 = brick SSBO + choice ring,
    // sets 0-2 identical to m_VsmPageLayout's → binding compatibility with the crown
    // draws is preserved. Pipelines mirror the hull pipe but with NO vertex input (one
    // light-facing quad per brick from gl_VertexIndex, bricks from the SSBO) and the
    // DDA-raycast FS (UE RasterizeBricks model). Failure here only disables the brick
    // caster — the shell path above still works.
    if (m_BrickVB && m_BrickTotal) do {
        VkShaderModule vvs  = g_ShaderManager->Load("tree_vsm_vox.vert.spv");
        VkShaderModule vvsS = g_ShaderManager->Load("tree_vsm_vox_s.vert.spv");
        VkShaderModule vfs  = g_ShaderManager->Load("tree_vsm_vox.frag.spv");
        if (!vvs || !vvsS || !vfs) { Msg("![VK Trees] voxel caster shaders missing — shell fallback"); break; }
        // Stage-2 cull outputs first — the remap list is set3 binding 2, so it must
        // exist before the descriptor writes even if the cull pipeline itself fails.
        {
            auto mkBuf = [&](CVulkanBuffer*& b, VkDeviceSize sz, VkBufferUsageFlags extra) {
                b = xr_new<CVulkanBuffer>();
                b->Create(sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra,
                          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);   // gpuOnly: GPU-written, no CPU mapping
            };
            mkBuf(m_VoxCullCmdD,  (VkDeviceSize)kVoxCullCmdCap * sizeof(u32) * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
            mkBuf(m_VoxCullCmdS,  (VkDeviceSize)kVoxCullCmdCap * sizeof(u32) * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
            mkBuf(m_VoxCullList,  (VkDeviceSize)kVoxCullListCap * sizeof(u32), 0);
            mkBuf(m_VoxCullStats, (VkDeviceSize)8 * sizeof(u32), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);   // words 0/1 double as draw counts; [4]=hzbCulled
            m_VoxCullStatsRB = xr_new<CVulkanBuffer>();
            m_VoxCullStatsRB->Create((VkDeviceSize)8 * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            m_VoxCullStatsPtr = (u32*)m_VoxCullStatsRB->Map();
            if (m_VoxCullStatsPtr) memset(m_VoxCullStatsPtr, 0, 8 * sizeof(u32));
            for (u32 i = 0; i < N; ++i) {   // compact dispatch list ring (host-visible, CPU fills in VsmBin)
                m_VoxCullTreeList[i] = xr_new<CVulkanBuffer>();
                m_VoxCullTreeList[i]->Create((VkDeviceSize)m_TotalCount * sizeof(u32),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
                m_VoxCullTreeListPtr[i] = m_VoxCullTreeList[i]->Map();
            }
        }
        {
            if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO }, N,
                                        m_VoxCasterSetL, m_VoxCasterPool, m_VoxCasterSet,
                                        VK_SHADER_STAGE_VERTEX_BIT, "Trees.VoxCaster"))
                break;
            for (u32 i = 0; i < N; ++i)     // handles are fixed → write once
                VK::DescriptorWriter(m_VoxCasterSet[i])
                    .StorageBuffer(0, m_BrickVB->GetHandle())
                    .StorageBuffer(1, m_VoxChoiceBuf[i]->GetHandle())
                    .StorageBuffer(2, m_VoxCullList->GetHandle())
                    .Flush();
            m_VoxCasterLayout = VK::MakePipelineLayout(
                { m_XformDescLayout, m_TexDescLayout, m_VsmPageSetL, m_VoxCasterSetL }, 64,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
            if (m_VoxCasterLayout == VK_NULL_HANDLE) break;
        }
        auto createVoxPipe = [&](VkShaderModule mvs, VkPipeline& out) {
            out = VK::GfxPipelineBuilder(m_VoxCasterLayout)   // no vertex input
                .Vert(mvs).Frag(vfs)
                .Cull(VK_CULL_MODE_NONE)
                .DynamicDepthBias()
                .Depth(true, true)
                .DepthTarget(VK_FORMAT_D16_UNORM)   // VSM atlas format
                .Build("Trees voxel caster");
        };
        createVoxPipe(vvs,  m_VsmVoxPipe);
        createVoxPipe(vvsS, m_VsmVoxPipeS);

        // ---- Stage-2 per-page brick cull (vsm_vox_cull.comp): 12 storage + 1 UBO.
        // Failure only disables the compaction — the un-culled instanced path draws.
        do {
            VkShaderModule cs = g_ShaderManager->Load("vsm_vox_cull.comp.spv");
            if (!cs) { Msg("![VK Trees] vsm_vox_cull.comp.spv missing — un-culled brick path"); break; }
            if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO,
                                          kUBO,                                        // 8 = clipmap UBO
                                          kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                        N, m_VoxCullSetL, m_VoxCullPool, m_VoxCullSet,
                                        VK_SHADER_STAGE_COMPUTE_BIT, "Trees.VoxCull"))
                break;
            // push (32 B): count, mode, cmdCap, listCap, slop, hzbOn, margin, rmaskOn
            m_VoxCullLayout = VK::MakePipelineLayout({ m_VoxCullSetL }, 32);
            if (m_VoxCullLayout == VK_NULL_HANDLE) break;
            m_VoxCullPipe = VK::CreateComputePipeline(cs, m_VoxCullLayout, "Trees.VoxCull");
            if (m_VoxCullPipe == VK_NULL_HANDLE) {
                Msg("![VK Trees] vox-cull pipeline create failed — un-culled brick path"); break;
            }
            m_VoxCullReady = true;
        } while (false);
    } while (false);

    // Debug overlay (r_vsm_tree_hull_debug): translucent PINK lobes over hull-tier trees
    // in the forward view — makes the r_vsm_tree_hull_dist boundary visible in-world.
    // Rides the forward tree pass: same layout (TreeGfxPush + set0 xform/waves already
    // bound), scene color + depth formats, alpha blend, LEQUAL test, no depth write.
    if (m_GfxPipelineLayout != VK_NULL_HANDLE) {
        VkShaderModule dvs = g_ShaderManager->Load("tree_hull_debug.vert.spv");
        VkShaderModule dfs = g_ShaderManager->Load("tree_hull_debug.frag.spv");
        if (dvs && dfs) {
            m_HullDebugPipe = VK::GfxPipelineBuilder(m_GfxPipelineLayout)
                .Vert(dvs).Frag(dfs)
                .Binding(0, 12)
                .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
                .Cull(VK_CULL_MODE_NONE)
                .Depth(true, false)
                .Color(VK::SceneColor::Format()).BlendAlpha()
                .DepthTarget(Swapchain.m_DepthFormat)
                .Build("Trees crown-hull debug");
        }
    }

    m_HullReady = true;
    Msg("[VK Trees] crown-hull caster LOD ready (%u hull tris, %u voxels, dist %.0fm, %s)",
        m_HullLobeTotal, m_VoxTotal, ps_r_vsm_tree_hull_dist, ps_r_vsm_tree_hull ? "ON" : "off");
}

// Swap crown→hull cmds for hull-tier trees. Called at the end of VsmBinDyn: the barrier
// covers stage-1-bin-write → this read/rewrite; the later stage-1→stage-2 barrier orders
// the zeroed counts before the meshlet refinement, and vk_vsm's global barrier before
// RenderAtlas covers the DRAW_INDIRECT reads of both buffers.
void CTreeManager::DispatchHullCmd(VkCommandBuffer cmd)
{
    if (!IsHullMode() || m_HullCmdPipe == VK_NULL_HANDLE || !m_VsmHullIndirect || !m_VsmHullIndirectS) return;
    const u32 slot = m_VsmSlot;
    // One global compute→compute barrier: orders BOTH stage-1 tree bins' indirect writes
    // (static + dyn — both already recorded; this runs at the end of VsmBinDyn) before
    // this dispatch's read-and-rewrite of them.
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    VK::DescriptorWriter(m_HullCmdSet[slot])
        .StorageBuffer(0, m_VsmDynIndirect->GetHandle())
        .StorageBuffer(1, m_HullInfoBuffer->GetHandle())
        .StorageBuffer(2, m_VsmNearFlags[slot]->GetHandle())
        .StorageBuffer(3, m_VsmHullIndirect->GetHandle())
        .StorageBuffer(4, m_VsmIndirect->GetHandle())          // STATIC crown cmds (far tier)
        .StorageBuffer(5, m_VsmHullIndirectS->GetHandle())     // static hull cmds
        .StorageBuffer(6, m_VoxChoiceBuf[slot]->GetHandle())   // voxel choice (ring)
        .StorageBuffer(7, m_VsmVoxIndirect->GetHandle())       // dyn voxel cmds
        .StorageBuffer(8, m_VsmVoxIndirectS->GetHandle())      // static voxel cmds
        .Flush();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HullCmdPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HullCmdLayout, 0, 1, &m_HullCmdSet[slot], 0, nullptr);
    const bool voxCast = ps_r_vsm_tree_hull_vox > 0 && m_VsmVoxPipe != VK_NULL_HANDLE && m_BrickTotal > 0;
    // voxOn bits: 1 = dyn tier uses bricks, 2 = STATIC tier too (default off — the far
    // shell is per-page-cull-free and measured cheaper on living-sun cache refreshes;
    // bricks lack the meshlet-style stage-2 the crowns have).
    struct { u32 count, farOn, voxOn; } push{ m_TotalCount, (u32)(ps_r_vsm_tree_hull >= 2 ? 1 : 0),
        (u32)(voxCast ? (1u | (ps_r_vsm_tree_hull_vox_static ? 2u : 0u)) : 0u) };
    vkCmdPushConstants(cmd, m_HullCmdLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (m_TotalCount + 63) / 64, 1, 1);
}

// Stage-2 per-page brick cull (see vk_TreeManager.h / vsm_vox_cull.comp.glsl). Runs
// once per frame from VsmBinMeshlets — the caller's stage-1→stage-2 barrier already
// orders vsm_hull_cmd's brick-cmd writes before this pass's reads; the cull's own
// writes (compacted cmds + remap list) are ordered before the atlas draws by vk_vsm's
// global pre-RenderAtlas barrier, same as the meshlet stage-2 outputs.
void CTreeManager::DispatchVoxCull(VkCommandBuffer cmd, VkBuffer pageListDyn, VkBuffer pageListStatic, VkBuffer clipmapUBO,
                                   VkBuffer pageMaxBlk, VkBuffer staticPT)
{
    m_VoxCullActive = false;
    if (!m_VoxCullReady || !ps_r_vsm_tree_hull_vox_cull || ps_r_vsm_tree_hull_vox <= 0) return;
    if (!IsHullMode() || !m_BrickTotal || !m_VsmVoxIndirect || !m_VsmVoxIndirectS) return;
    if (pageListDyn == VK_NULL_HANDLE || clipmapUBO == VK_NULL_HANDLE) return;
    // Compact dispatch: no tree carries a live brick choice → every brick cmd is zero,
    // nothing to compact — skip both dispatches (DrawVoxCasters skips its draw too).
    if (m_VoxCullTreeN == 0 || !m_VoxCullTreeList[m_VsmSlot]) return;
    if (pageListStatic == VK_NULL_HANDLE) pageListStatic = pageListDyn;   // layout parity (static cmds empty then anyway)
    // shadow-HZB occlusion (r_vsm_hzb): both buffers or nothing; layout parity when off.
    const bool hzb = pageMaxBlk != VK_NULL_HANDLE && staticPT != VK_NULL_HANDLE;
    if (pageMaxBlk == VK_NULL_HANDLE) pageMaxBlk = m_VoxCullStats->GetHandle();
    if (staticPT == VK_NULL_HANDLE)   staticPT   = m_VoxCullStats->GetHandle();
    const u32 slot = m_VsmSlot;
    VkBuffer rmask = VK::VSM::GetRMaskHandle();
    const bool rmaskOn = ps_r_vsm_rmask && rmask != VK_NULL_HANDLE;
    if (rmask == VK_NULL_HANDLE) rmask = m_VoxCullStats->GetHandle();   // layout parity when off
    VK::DescriptorWriter(m_VoxCullSet[slot])
        .StorageBuffer(0,  m_VsmVoxIndirect->GetHandle())
        .StorageBuffer(1,  m_VsmVoxIndirectS->GetHandle())
        .StorageBuffer(2,  m_VoxChoiceBuf[slot]->GetHandle())
        .StorageBuffer(3,  m_BrickVB->GetHandle())
        .StorageBuffer(4,  m_TreeTransformsBuffer->GetHandle())
        .StorageBuffer(5,  m_VsmCasterPages->GetHandle())
        .StorageBuffer(6,  pageListDyn)
        .StorageBuffer(7,  pageListStatic)
        .UniformBuffer(8,  clipmapUBO)
        .StorageBuffer(9,  m_VoxCullCmdD->GetHandle())
        .StorageBuffer(10, m_VoxCullCmdS->GetHandle())
        .StorageBuffer(11, m_VoxCullList->GetHandle())
        .StorageBuffer(12, m_VoxCullStats->GetHandle())
        .StorageBuffer(13, pageMaxBlk)                            // shadow-HZB block maxes
        .StorageBuffer(14, staticPT)                              // virtual page -> static slot
        .StorageBuffer(15, rmask)                                 // receiver mask (r_vsm_rmask, dyn tier)
        .StorageBuffer(16, m_VoxCullTreeList[slot]->GetHandle())  // compact tree list (this frame's ring slot)
        .Flush();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_VoxCullPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_VoxCullLayout, 0, 1, &m_VoxCullSet[slot], 0, nullptr);
    // count = LIST length (one workgroup per listed tree), not the level's tree count.
    struct { u32 count, mode, cmdCap, listCap; float slop; u32 hzbOn; float margin; u32 rmaskOn; } push{
        m_VoxCullTreeN, 0u, kVoxCullCmdCap, kVoxCullListCap, 0.75f, (u32)(hzb ? 1 : 0), ps_r_vsm_hzb_margin, (u32)(rmaskOn ? 1 : 0) };
    vkCmdPushConstants(cmd, m_VoxCullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, m_VoxCullTreeN, 1, 1);
    if (ps_r_vsm_tree_hull_vox_static) {   // static tier casts bricks → compact it too (rigid: no wind slop)
        push.mode = 1u; push.slop = 0.f;
        vkCmdPushConstants(cmd, m_VoxCullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, m_VoxCullTreeN, 1, 1);
    }
    m_VoxCullActive = true;
}

// ONE multi-draw of the tier's trees over its hull indirect (non-hull trees carry
// instanceCount 0 → free). DYN pass: live-wind push (both tiers sway coherently),
// set2 = dyn page data. STATIC pass (r_vsm_tree_hull 2): zero wind → rigid, cache-
// stable pages, set2 = static page data. set1 (diffuse) is layout-required but unused
// by the hull shaders — bind any valid set.
void CTreeManager::DrawHulls(VkCommandBuffer cmd, u32 slot, bool staticPass)
{
    VkPipeline pipe = staticPass ? m_VsmHullPipeS : m_VsmHullPipe;
    CVulkanBuffer* ind = staticPass ? m_VsmHullIndirectS : m_VsmHullIndirect;
    if (pipe == VK_NULL_HANDLE || !ind || !m_HullVB || !m_HullIB) return;
    struct VsmPagePush {
        float    uvScale, alphaRef; u32 cap, pad;
        Fvector4 wind_params, wsetup_trees, wind_anim;
    } pc{};
    pc.uvScale = 1.0f / 2048.0f; pc.alphaRef = TreeAlphaRef(200.0f / 255.0f); pc.cap = kVsmTreeCap;
    if (!staticPass) {
        pc.wsetup_trees.set(ps_r_wind_tree_anim, ps_r_wind_tree_trunk, ps_r_wind_tree_bend, 0.1f);
        if (g_pGamePersistent) {
            const Fvector3 wa = ES().wind_anim;
            pc.wind_anim.set(wa.x, wa.y, wa.z, ps_r_wind_tree_flutter);
            if (auto* E = ES().CurrentEnv)
                pc.wind_params.set(E->wind_direction, E->wind_velocity, 0.0f, 0.0f);
        }
        pc.wind_params.w = ps_r_wind_tree_crown;   // flutter height gate — match the crown pass
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 0, 1, &m_XformDescSet, 0, nullptr);
    if (!m_TexDescSets.empty())
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 1, 1, &m_TexDescSets[0], 0, nullptr);
    VkDescriptorSet pageSet = staticPass ? m_VsmPageSet[slot] : m_VsmDynPageSet[slot];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VsmPageLayout, 2, 1, &pageSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_VsmPageLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    VkDeviceSize vbOff = 0;
    VkBuffer hvb = m_HullVB->GetHandle();
    vkCmdBindVertexBuffers(cmd, 0, 1, &hvb, &vbOff);
    vkCmdBindIndexBuffer(cmd, m_HullIB->GetHandle(), 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexedIndirect(cmd, ind->GetHandle(), 0, m_TotalCount, sizeof(VkDrawIndexedIndirectCommand));
}

// Voxel-cloud caster multi-draw — same shape as DrawHulls but NON-indexed (cube corners
// from gl_VertexIndex, voxels/choice from set3). Trees not chosen by vsm_hull_cmd carry
// instanceCount 0 → free. Sets 0-2 are re-bound with the 4-set layout (identical layout
// objects → the crown draws that follow keep their own bindings valid).
void CTreeManager::DrawVoxCasters(VkCommandBuffer cmd, u32 slot, bool staticPass)
{
    if (ps_r_vsm_tree_hull_vox <= 0) return;
    // No tree carries a live brick choice → vsm_hull_cmd wrote every vox cmd with
    // instanceCount 0 this frame — skip the whole multi-draw (binds + N zero-draws).
    if (m_VoxCastCount == 0) return;
    VkPipeline pipe = staticPass ? m_VsmVoxPipeS : m_VsmVoxPipe;
    CVulkanBuffer* ind = staticPass ? m_VsmVoxIndirectS : m_VsmVoxIndirect;
    if (pipe == VK_NULL_HANDLE || !ind || m_VoxCasterLayout == VK_NULL_HANDLE) return;
    // Stage-2 compacted stream (pad=1 → VS fetches bricks through the remap list).
    const bool cull = m_VoxCullActive && m_VoxCullCmdD && m_VoxCullStats;
    struct VsmPagePush {
        float    uvScale, alphaRef; u32 cap, pad;
        Fvector4 wind_params, wsetup_trees, wind_anim;
    } pc{};
    pc.uvScale = 1.0f / 2048.0f; pc.alphaRef = TreeAlphaRef(200.0f / 255.0f); pc.cap = kVsmTreeCap;
    pc.pad = cull ? 1u : 0u;
    if (!staticPass) {
        pc.wsetup_trees.set(ps_r_wind_tree_anim, ps_r_wind_tree_trunk, ps_r_wind_tree_bend, 0.1f);
        if (g_pGamePersistent) {
            const Fvector3 wa = ES().wind_anim;
            pc.wind_anim.set(wa.x, wa.y, wa.z, ps_r_wind_tree_flutter);
            if (auto* E = ES().CurrentEnv)
                pc.wind_params.set(E->wind_direction, E->wind_velocity, 0.0f, 0.0f);
        }
        pc.wind_params.w = ps_r_wind_tree_crown;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VoxCasterLayout, 0, 1, &m_XformDescSet, 0, nullptr);
    if (!m_TexDescSets.empty())
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VoxCasterLayout, 1, 1, &m_TexDescSets[0], 0, nullptr);
    VkDescriptorSet pageSet = staticPass ? m_VsmPageSet[slot] : m_VsmDynPageSet[slot];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VoxCasterLayout, 2, 1, &pageSet, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VoxCasterLayout, 3, 1, &m_VoxCasterSet[slot], 0, nullptr);
    vkCmdPushConstants(cmd, m_VoxCasterLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    if (cull) {
        CVulkanBuffer* cc = staticPass ? m_VoxCullCmdS : m_VoxCullCmdD;
        vkCmdDrawIndirectCount(cmd, cc->GetHandle(), 0, m_VoxCullStats->GetHandle(),
                               staticPass ? sizeof(u32) : 0, kVoxCullCmdCap, sizeof(u32) * 4);
    } else
        vkCmdDrawIndirect(cmd, ind->GetHandle(), 0, m_TotalCount, sizeof(u32) * 4);
}

void CTreeManager::DestroyHull()
{
    VkDevice dev = VulkanHW.m_Device;
    if (dev == VK_NULL_HANDLE) return;
    if (m_VsmHullPipe)   { vkDestroyPipeline(dev, m_VsmHullPipe, nullptr); m_VsmHullPipe = VK_NULL_HANDLE; }
    if (m_VsmHullPipeS)  { vkDestroyPipeline(dev, m_VsmHullPipeS, nullptr); m_VsmHullPipeS = VK_NULL_HANDLE; }
    if (m_HullDebugPipe) { vkDestroyPipeline(dev, m_HullDebugPipe, nullptr); m_HullDebugPipe = VK_NULL_HANDLE; }
    if (m_VoxFwdPipe)  { vkDestroyPipeline(dev, m_VoxFwdPipe,  nullptr); m_VoxFwdPipe  = VK_NULL_HANDLE; }
    if (m_HullCmdPipe)   { vkDestroyPipeline(dev, m_HullCmdPipe, nullptr); m_HullCmdPipe = VK_NULL_HANDLE; }
    if (m_HullCmdLayout) { vkDestroyPipelineLayout(dev, m_HullCmdLayout, nullptr); m_HullCmdLayout = VK_NULL_HANDLE; }
    if (m_HullCmdSetL)   { vkDestroyDescriptorSetLayout(dev, m_HullCmdSetL, nullptr); m_HullCmdSetL = VK_NULL_HANDLE; }
    if (m_HullCmdPool)   { vkDestroyDescriptorPool(dev, m_HullCmdPool, nullptr); m_HullCmdPool = VK_NULL_HANDLE;
                           for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) m_HullCmdSet[i] = VK_NULL_HANDLE; }
    if (m_VsmHullIndirect)  { m_VsmHullIndirect->Destroy(); xr_delete(m_VsmHullIndirect); }
    if (m_VsmHullIndirectS) { m_VsmHullIndirectS->Destroy(); xr_delete(m_VsmHullIndirectS); }
    // ----- Voxel-cloud caster resources.
    if (m_VsmVoxPipe)      { vkDestroyPipeline(dev, m_VsmVoxPipe, nullptr); m_VsmVoxPipe = VK_NULL_HANDLE; }
    if (m_VsmVoxPipeS)     { vkDestroyPipeline(dev, m_VsmVoxPipeS, nullptr); m_VsmVoxPipeS = VK_NULL_HANDLE; }
    if (m_VoxCasterLayout) { vkDestroyPipelineLayout(dev, m_VoxCasterLayout, nullptr); m_VoxCasterLayout = VK_NULL_HANDLE; }
    if (m_VoxCasterPool)   { vkDestroyDescriptorPool(dev, m_VoxCasterPool, nullptr); m_VoxCasterPool = VK_NULL_HANDLE;
                             for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) m_VoxCasterSet[i] = VK_NULL_HANDLE; }
    if (m_VoxCasterSetL)   { vkDestroyDescriptorSetLayout(dev, m_VoxCasterSetL, nullptr); m_VoxCasterSetL = VK_NULL_HANDLE; }
    if (m_VsmVoxIndirect)  { m_VsmVoxIndirect->Destroy(); xr_delete(m_VsmVoxIndirect); }
    if (m_VsmVoxIndirectS) { m_VsmVoxIndirectS->Destroy(); xr_delete(m_VsmVoxIndirectS); }
    // ----- Stage-2 brick-cull resources.
    m_VoxCullReady = m_VoxCullActive = false;
    if (m_VoxCullPipe)   { vkDestroyPipeline(dev, m_VoxCullPipe, nullptr); m_VoxCullPipe = VK_NULL_HANDLE; }
    if (m_VoxCullLayout) { vkDestroyPipelineLayout(dev, m_VoxCullLayout, nullptr); m_VoxCullLayout = VK_NULL_HANDLE; }
    if (m_VoxCullPool)   { vkDestroyDescriptorPool(dev, m_VoxCullPool, nullptr); m_VoxCullPool = VK_NULL_HANDLE;
                           for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) m_VoxCullSet[i] = VK_NULL_HANDLE; }
    if (m_VoxCullSetL)   { vkDestroyDescriptorSetLayout(dev, m_VoxCullSetL, nullptr); m_VoxCullSetL = VK_NULL_HANDLE; }
    if (m_VoxCullCmdD)   { m_VoxCullCmdD->Destroy();  xr_delete(m_VoxCullCmdD); }
    if (m_VoxCullCmdS)   { m_VoxCullCmdS->Destroy();  xr_delete(m_VoxCullCmdS); }
    if (m_VoxCullList)   { m_VoxCullList->Destroy();  xr_delete(m_VoxCullList); }
    if (m_VoxCullStats)  { m_VoxCullStats->Destroy(); xr_delete(m_VoxCullStats); }
    if (m_VoxCullStatsRB) { m_VoxCullStatsPtr = nullptr; m_VoxCullStatsRB->Destroy(); xr_delete(m_VoxCullStatsRB); }
    for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) {
        if (m_VoxCullTreeList[i]) { m_VoxCullTreeList[i]->Destroy(); xr_delete(m_VoxCullTreeList[i]); }
        m_VoxCullTreeListPtr[i] = nullptr;
    }
    m_VoxCullTreeN = 0;
    m_VoxCastListCPU.clear();
    for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) {
        if (m_VoxChoiceBuf[i]) { m_VoxChoiceBuf[i]->Destroy(); xr_delete(m_VoxChoiceBuf[i]); }
        m_VoxChoicePtr[i] = nullptr;
    }
    m_VoxChoiceCPU.clear();
    m_VoxLodSizeCPU.clear();
    // m_HullVB/IB/InfoBuffer are Build()-owned (bake data) — freed in CTreeManager::Destroy.
    m_HullReady = false;
}

void CTreeManager::DestroyVsm()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    VkDevice dev = VulkanHW.m_Device;
    DestroyImpostor();   // impostor pipelines reference m_VsmPageSetL — tear down before it
    DestroyHull();       // hull pipelines reference m_VsmPageLayout — same ordering rule
    if (m_VsmPagePipe24) { vkDestroyPipeline(dev, m_VsmPagePipe24, nullptr); m_VsmPagePipe24 = VK_NULL_HANDLE; }
    if (m_VsmPagePipe28) { vkDestroyPipeline(dev, m_VsmPagePipe28, nullptr); m_VsmPagePipe28 = VK_NULL_HANDLE; }
    if (m_VsmPageDynPipe24) { vkDestroyPipeline(dev, m_VsmPageDynPipe24, nullptr); m_VsmPageDynPipe24 = VK_NULL_HANDLE; }
    if (m_VsmPageDynPipe28) { vkDestroyPipeline(dev, m_VsmPageDynPipe28, nullptr); m_VsmPageDynPipe28 = VK_NULL_HANDLE; }
    if (m_VsmPageLayout) { vkDestroyPipelineLayout(dev, m_VsmPageLayout, nullptr); m_VsmPageLayout = VK_NULL_HANDLE; }
    if (m_VsmBinPipe)    { vkDestroyPipeline(dev, m_VsmBinPipe, nullptr); m_VsmBinPipe = VK_NULL_HANDLE; }
    if (m_VsmBinLayout)  { vkDestroyPipelineLayout(dev, m_VsmBinLayout, nullptr); m_VsmBinLayout = VK_NULL_HANDLE; }
    if (m_VsmDescPool)   { vkDestroyDescriptorPool(dev, m_VsmDescPool, nullptr); m_VsmDescPool = VK_NULL_HANDLE;
                           for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) { m_VsmBinSet[i] = VK_NULL_HANDLE; m_VsmDynBinSet[i] = VK_NULL_HANDLE; m_VsmPageSet[i] = VK_NULL_HANDLE; m_VsmDynPageSet[i] = VK_NULL_HANDLE; } }
    if (m_VsmBinSetL)    { vkDestroyDescriptorSetLayout(dev, m_VsmBinSetL, nullptr); m_VsmBinSetL = VK_NULL_HANDLE; }
    if (m_VsmPageSetL)   { vkDestroyDescriptorSetLayout(dev, m_VsmPageSetL, nullptr); m_VsmPageSetL = VK_NULL_HANDLE; }
    if (m_VsmCasterPages) { m_VsmCasterPages->Destroy(); xr_delete(m_VsmCasterPages); }
    if (m_VsmIndirect)    { m_VsmIndirect->Destroy(); xr_delete(m_VsmIndirect); }
    if (m_VsmDynIndirect) { m_VsmDynIndirect->Destroy(); xr_delete(m_VsmDynIndirect); }
    if (m_VsmStats)       { m_VsmStats->Destroy(); xr_delete(m_VsmStats); }
    if (m_VsmStatsRB)     { m_VsmStatsRB->Destroy(); xr_delete(m_VsmStatsRB); }
    m_VsmStatsPtr = nullptr;
    if (m_VsmTreeBits)    { m_VsmTreeBits->Destroy(); xr_delete(m_VsmTreeBits); }
    if (m_VsmTreeBitsRB)  { m_VsmTreeBitsRB->Destroy(); xr_delete(m_VsmTreeBitsRB); }
    m_VsmTreeBitsPtr = nullptr;
    for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) {
        if (m_VsmNearFlags[i]) { m_VsmNearFlags[i]->Destroy(); xr_delete(m_VsmNearFlags[i]); }
        m_VsmNearPtr[i] = nullptr;
    }
    m_VsmNearCPU.clear(); m_VsmPendingInval.clear();

    // ----- Phase B meshlet-cull resources.
    if (m_MeshletBinPipe)   { vkDestroyPipeline(dev, m_MeshletBinPipe, nullptr); m_MeshletBinPipe = VK_NULL_HANDLE; }
    if (m_MeshletBinLayout) { vkDestroyPipelineLayout(dev, m_MeshletBinLayout, nullptr); m_MeshletBinLayout = VK_NULL_HANDLE; }
    if (m_MeshletPagePipe24)    { vkDestroyPipeline(dev, m_MeshletPagePipe24, nullptr); m_MeshletPagePipe24 = VK_NULL_HANDLE; }
    if (m_MeshletPagePipe28)    { vkDestroyPipeline(dev, m_MeshletPagePipe28, nullptr); m_MeshletPagePipe28 = VK_NULL_HANDLE; }
    if (m_MeshletPageDynPipe24) { vkDestroyPipeline(dev, m_MeshletPageDynPipe24, nullptr); m_MeshletPageDynPipe24 = VK_NULL_HANDLE; }
    if (m_MeshletPageDynPipe28) { vkDestroyPipeline(dev, m_MeshletPageDynPipe28, nullptr); m_MeshletPageDynPipe28 = VK_NULL_HANDLE; }
    if (m_MeshletDescPool)  { vkDestroyDescriptorPool(dev, m_MeshletDescPool, nullptr); m_MeshletDescPool = VK_NULL_HANDLE;
                              for (u32 i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) { m_MeshletBinSet[i] = VK_NULL_HANDLE; m_MeshletDynBinSet[i] = VK_NULL_HANDLE; } }
    if (m_MeshletBinSetL)   { vkDestroyDescriptorSetLayout(dev, m_MeshletBinSetL, nullptr); m_MeshletBinSetL = VK_NULL_HANDLE; }
    auto delBuf = [](CVulkanBuffer*& b) { if (b) { b->Destroy(); xr_delete(b); } };
    delBuf(m_VsmMeshletCmd); delBuf(m_VsmMeshletCmdDyn);
    delBuf(m_VsmMeshletGroupCount); delBuf(m_VsmMeshletGroupCountDyn);
    delBuf(m_TreeGroupBuffer); delBuf(m_GroupInfoBuffer);
    delBuf(m_MeshletStats); delBuf(m_MeshletStatsRB);
    m_MeshletStatsPtr = nullptr;
    m_MeshletGroupBase.clear(); m_MeshletGroupCap.clear();
    m_MeshletVsmReady = false;

    m_VsmReady = false; m_VsmSlot = 0;
}

}  // namespace VK

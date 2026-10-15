// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// GPU-driven shadow casters — see vk_shadow_gpu.h. Mirrors vk_TreeManager.

#include "stdafx.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_shadow_gpu.h"
#include "CRender_Vulkan.h"     // RImplementation.Visuals
#include "vk_Visual.h"          // vkFVisual, m_mesh, m_pWorldMaterial
#include "vk_world_material.h"  // WorldMaterial (alphaRef, isWmark)
#include "vk_buffer.h"          // CVulkanBuffer
#include "vk_pipeline_cache.h"  // GetDepthPipeline / GetDepthLayout
#include "vk_compute_util.h"    // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_shaders.h"         // g_ShaderManager
#include "vk_world_gpu.h"       // WorldGPU::InSet (cluster-shadow coverage diag)
#include "vk_profiler.h"        // Prof::CmdBeginLabel
#include <algorithm>

namespace VK { namespace ShadowGPU {

namespace {

struct GpuCasterMeta {     // std430, 48 B — matches shadow_cull.comp `Meta`
    Fvector sphere_P; float sphere_R;
    u32 index_count; u32 ib_first; u32 first_vertex; u32 group;   // full draw range + (vb,ib,stride) group
    u32 lod_first; u32 lod_count; u32 _pad0; u32 _pad1;           // coarse LOD slice (caster-LOD)
};
struct Group { VkBuffer vb, ib; u32 stride; VkIndexType iType; u32 meshOffset, meshCount; };

// One dispatch per target routes every group's output, so the push carries the
// target + region geometry. camLod = (cameraPos.xyz, LOD distance) for caster-LOD.
// 128 B total — stays within the guaranteed minimum maxPushConstantsSize.
struct CullPush { Fvector4 planes[6]; u32 target, numGroups, maxGroupMesh, total; Fvector4 camLod; };

bool s_built = false;
u32  s_total = 0;
u32  s_maxGroupMesh = 0;
xr_vector<Group> s_groups;

CVulkanBuffer* s_meta     = nullptr;
CVulkanBuffer* s_indirect = nullptr;   // TGT_COUNT * numGroups * maxGroupMesh cmds
CVulkanBuffer* s_count    = nullptr;   // TGT_COUNT * numGroups u32

VkDescriptorSetLayout s_setL  = VK_NULL_HANDLE;
VkDescriptorPool      s_pool  = VK_NULL_HANDLE;
VkDescriptorSet       s_set   = VK_NULL_HANDLE;
VkPipelineLayout      s_cullLayout = VK_NULL_HANDLE;
VkPipeline            s_cullPipe   = VK_NULL_HANDLE;

inline u32 Region(Target tgt, u32 g) { return ((u32)tgt * (u32)s_groups.size()) + g; }

void MemBarrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst,
                VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = src; b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 1, &b, 0, nullptr, 0, nullptr);
}

// Recursively collect OPAQUE static casters. Mirrors vkFHierrarhyVisual::Submit
// and the tree ExtractFromVisual: descend MT_HIERRARHY / MT_LOD into children so
// opaque casters nested in composite props (buildings/props built as a hierarchy)
// aren't missed — a top-level-only walk drops their shadows vs the CPU FlushDepth
// path. Trees (MT_TREE_*) and skeletons are handled elsewhere / not opaque static
// casters; alpha-tested + wmark leaves stay on the CPU AT path.
// Caster filter over the shared leaf index (WorldGPU::LeafVisuals): the
// container recursion + dedup this used to repeat now happen once per load.
void ExtractCasters(vkFVisual* fv, xr_vector<vkFVisual*>& out, xr_vector<vkFVisual*>* atOut)
{
    const u32 t = fv->Type;
    if (t == MT_NORMAL || t == MT_PROGRESSIVE) {
        if (!fv->m_mesh.IsValid() || !fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) return;
        WorldMaterial* mat = fv->m_pWorldMaterial;
        if (mat && mat->isWmark) return;
        if (mat && mat->alphaRef >= 0.f) {   // alpha-tested → CPU AT path / GPU cluster AT (coverage diag)
            if (atOut) atOut->push_back(fv);
            return;
        }
        out.push_back(fv);
    }
}

bool CreateCullPipeline()
{
    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule cs = g_ShaderManager->Load("shadow_cull.comp.spv");
    if (!cs) { Msg("![VK ShadowGPU] shadow_cull.comp.spv load failed"); return false; }

    // 0 = meta, 1 = indirect, 2 = count.
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO }, 1, s_setL, s_pool, &s_set,
                                VK_SHADER_STAGE_COMPUTE_BIT, "ShadowGPU.Cull"))
        return false;

    VK::DescriptorWriter(s_set)
        .StorageBuffer(0, s_meta->GetHandle())
        .StorageBuffer(1, s_indirect->GetHandle())
        .StorageBuffer(2, s_count->GetHandle())
        .Flush();

    s_cullLayout = VK::MakePipelineLayout({ s_setL }, sizeof(CullPush));
    if (s_cullLayout == VK_NULL_HANDLE) return false;

    s_cullPipe = VK::CreateComputePipeline(cs, s_cullLayout, "ShadowGPU.Cull");
    if (s_cullPipe == VK_NULL_HANDLE) return false;
    return true;
}

} // anonymous namespace

void Build()
{
    VK::Vram::Scope _vram_scope("ShadowGPU");
    if (s_built) return;
    s_built = true;   // one attempt; stays "built" (possibly empty) so we don't retry every frame

    // Sub-timers: this is 44 ms of the post-visual budget reported as ONE number,
    // and it mixes an extract over the leaf set, two sorts, a meta pack, buffer
    // creation, a pipeline, and three diagnostic walks over the caster set. Which
    // of those owns the time decides whether there is anything here worth moving.
    CTimer _s;
    float msExtract = 0, msMeta = 0, msGroups = 0, msBufs = 0, msPipe = 0, msDiag = 0;

    // Collect OPAQUE static casters, recursing into hierarchy/LOD containers
    // (alpha-tested stay on the CPU AT path; wmarks never cast).
    _s.Start();
    xr_vector<vkFVisual*> casters; casters.reserve(8192);
    xr_vector<vkFVisual*> atCasters; atCasters.reserve(8192);   // coverage diag only (GPU AT draws via WorldGPU)
    for (vkFVisual* fv : VK::WorldGPU::LeafVisuals())
        ExtractCasters(fv, casters, &atCasters);
    const u32 nPreDedup = (u32)casters.size();   // diag: hierarchy double-collection vs nesting recovered
    std::sort(casters.begin(), casters.end());
    casters.erase(std::unique(casters.begin(), casters.end()), casters.end());
    std::sort(atCasters.begin(), atCasters.end());
    atCasters.erase(std::unique(atCasters.begin(), atCasters.end()), atCasters.end());
    msExtract = _s.GetElapsed_ms_total();

    if (casters.empty()) { Msg("[VK ShadowGPU] no opaque static casters"); return; }

    // Group consecutive by (vb, ib, stride) — opaque depth pipeline keys only on stride.
    _s.Start();
    std::sort(casters.begin(), casters.end(), [](vkFVisual* a, vkFVisual* b) {
        const VkBuffer av = a->m_mesh.p_rm_Vertices->GetHandle(), bv = b->m_mesh.p_rm_Vertices->GetHandle();
        if (av != bv) return av < bv;
        const VkBuffer ai = a->m_mesh.p_rm_Indices->GetHandle(), biw = b->m_mesh.p_rm_Indices->GetHandle();
        if (ai != biw) return ai < biw;
        return a->m_mesh.vStride < b->m_mesh.vStride;
    });

    s_total = (u32)casters.size();
    xr_vector<GpuCasterMeta> meta(s_total);
    for (u32 i = 0; i < s_total; ++i) {
        vkFVisual* fv = casters[i];
        GpuCasterMeta& m = meta[i];
        m.sphere_P     = fv->vis.sphere.P;
        m.sphere_R     = fv->vis.sphere.R;

        // Full draw range mirrors the CPU path: progressive casters draw their
        // finest sliding-window slice (iBase + sw_offsets[0]/sw_counts[0], =
        // vkFProgressive::Submit), NOT the whole IB span (which over-draws). The
        // coarse range is the coarsest LOD slice for distance caster-LOD. MT_NORMAL
        // has one full range → coarse = full.
        u32 fullFirst = fv->m_mesh.iBase, fullCount = fv->m_mesh.iCount;
        u32 lodFirst  = fullFirst,        lodCount  = fullCount;
        if (fv->Type == MT_PROGRESSIVE) {
            auto* pg = static_cast<vkFProgressive*>(fv);
            if (pg->sw_count > 0 && pg->sw_offsets && pg->sw_counts) {
                fullFirst = fv->m_mesh.iBase + pg->sw_offsets[0];
                fullCount = pg->sw_counts[0];
                const u32 cl = pg->sw_count - 1;   // coarsest slice
                lodFirst  = fv->m_mesh.iBase + pg->sw_offsets[cl];
                lodCount  = pg->sw_counts[cl];
            }
        }
        m.index_count  = fullCount;
        m.ib_first     = fullFirst;
        m.first_vertex = fv->m_mesh.vBase;
        m.group        = 0;   // assigned after grouping below
        m.lod_first    = lodFirst;
        m.lod_count    = lodCount;
        m._pad0 = 0; m._pad1 = 0;
    }
    msMeta = _s.GetElapsed_ms_total();

    _s.Start();
    s_groups.clear(); s_maxGroupMesh = 0;
    Group cur{};
    auto seed = [&](u32 i) {
        cur.vb = casters[i]->m_mesh.p_rm_Vertices->GetHandle();
        cur.ib = casters[i]->m_mesh.p_rm_Indices->GetHandle();
        cur.stride = casters[i]->m_mesh.vStride;
        cur.iType  = casters[i]->m_mesh.iType;
        cur.meshOffset = i; cur.meshCount = 1;
    };
    seed(0);
    for (u32 i = 1; i < s_total; ++i) {
        const VkBuffer vb = casters[i]->m_mesh.p_rm_Vertices->GetHandle();
        const VkBuffer ib = casters[i]->m_mesh.p_rm_Indices->GetHandle();
        const u32 st = casters[i]->m_mesh.vStride;
        if (vb == cur.vb && ib == cur.ib && st == cur.stride) cur.meshCount++;
        else { s_maxGroupMesh = _max(s_maxGroupMesh, cur.meshCount); s_groups.push_back(cur); seed(i); }
    }
    s_maxGroupMesh = _max(s_maxGroupMesh, cur.meshCount);
    s_groups.push_back(cur);

    const u32 nGroups = (u32)s_groups.size();

    // Stamp each caster's group index into its meta — the single-dispatch cull
    // routes output to (target,group) regions from this.
    for (u32 g = 0; g < nGroups; ++g)
        for (u32 i = s_groups[g].meshOffset; i < s_groups[g].meshOffset + s_groups[g].meshCount; ++i)
            meta[i].group = g;
    msGroups = _s.GetElapsed_ms_total();

    // meta SSBO (device-local)
    _s.Start();
    s_meta = xr_new<CVulkanBuffer>();
    s_meta->Create(sizeof(GpuCasterMeta) * s_total, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_meta->Upload(meta.data(), sizeof(GpuCasterMeta) * s_total);

    // indirect + count (device-local, GPU-written) — per (target, group) region.
    const VkBufferUsageFlags iu = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    s_indirect = xr_new<CVulkanBuffer>();
    s_indirect->Create((VkDeviceSize)TGT_COUNT * nGroups * s_maxGroupMesh * sizeof(VkDrawIndexedIndirectCommand), iu,
                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_count = xr_new<CVulkanBuffer>();
    s_count->Create((VkDeviceSize)TGT_COUNT * nGroups * sizeof(u32), iu, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    msBufs = _s.GetElapsed_ms_total();

    _s.Start();
    if (!CreateCullPipeline()) { Msg("![VK ShadowGPU] cull pipeline failed — disabled"); s_groups.clear(); return; }
    msPipe = _s.GetElapsed_ms_total();

    _s.Start();
    // ---- DIAG: caster-LOD viability — type split, how many progressives carry
    // real LOD slices (sw_count>1; single-slice = degraded full-only), and the
    // SHARE of shadow triangles that is LOD-able (= the ceiling of caster-LOD).
    {
        u32 nNormal = 0, nProg = 0, nProgLOD = 0;
        u64 trisRaw = 0, trisFull = 0, trisLODfull = 0, trisLODcoarse = 0;
        for (vkFVisual* fv : casters) {
            const u64 rawTris = fv->m_mesh.iCount / 3;   // whole IB span (the old GPU draw)
            u64 fullTris = rawTris, coarseTris = rawTris; bool hasLOD = false;
            if (fv->Type == MT_PROGRESSIVE) {
                ++nProg;
                auto* pg = static_cast<vkFProgressive*>(fv);
                if (pg->sw_count > 0 && pg->sw_counts) {
                    fullTris = pg->sw_counts[0] / 3;     // corrected full = LOD 0 (what we draw now)
                    if (pg->sw_count > 1) { hasLOD = true; coarseTris = pg->sw_counts[pg->sw_count - 1] / 3; }
                }
            } else { ++nNormal; }
            trisRaw += rawTris; trisFull += fullTris;
            if (hasLOD) { ++nProgLOD; trisLODfull += fullTris; trisLODcoarse += coarseTris; }
        }
        Msg("[VK ShadowGPU] DIAG types: MT_NORMAL=%u MT_PROGRESSIVE=%u (with LOD slices=%u)",
            nNormal, nProg, nProgLOD);
        Msg("[VK ShadowGPU] DIAG tris: drawn(full)=%llu rawIB=%llu (prog over-draw fix -%llu) | "
            "LOD-able full=%llu coarsest=%llu | best-case caster-LOD cut=%.1f%% of drawn",
            (unsigned long long)trisFull, (unsigned long long)trisRaw, (unsigned long long)(trisRaw - trisFull),
            (unsigned long long)trisLODfull, (unsigned long long)trisLODcoarse,
            trisFull ? 100.0 * double(trisLODfull - trisLODcoarse) / double(trisFull) : 0.0);
    }

    Msg("[VK ShadowGPU] built: %u opaque casters (%u pre-dedup), %u groups, maxGroup=%u",
        s_total, nPreDedup, nGroups, s_maxGroupMesh);

    // Phase 3 coverage diag: casters NOT in the WorldGPU cluster set lose their
    // shadow when the cluster shadow paths replace this per-mesh set (they are
    // usually null-material / no-diffuse leaves; tessellated ones only appear
    // with r_tess on). A big number here = fall back (r_shadow_cluster 0).
    {
        u32 uncovered = 0;
        for (vkFVisual* fv : casters)
            if (!WorldGPU::InSet(fv)) ++uncovered;
        if (uncovered)
            Msg("![VK ShadowGPU] cluster-shadow coverage: %u/%u casters outside the WorldGPU set (their shadows drop under r_shadow_cluster/r_vsm_cluster)",
                uncovered, s_total);
        // Same coverage question for the ALPHA-TESTED casters now that
        // r_gpu_shadows_at routes them through the cluster shadow slices: an AT
        // leaf outside the WorldGPU set loses its shadow (the CPU cutout queues
        // are idle under gpuAT). A big number = flip r_gpu_shadows_at 0.
        u32 uncoveredAT = 0;
        for (vkFVisual* fv : atCasters)
            if (!WorldGPU::InSet(fv)) ++uncoveredAT;
        Msg("%s[VK ShadowGPU] cluster-shadow AT coverage: %u/%u AT casters outside the WorldGPU set%s",
            uncoveredAT ? "!" : "", uncoveredAT, (u32)atCasters.size(),
            uncoveredAT ? " (their shadows drop under r_gpu_shadows_at)" : "");
    }
    msDiag = _s.GetElapsed_ms_total();

    Msg("[load step]   ShadowGPU::Build: extract+dedup %.0f | meta %.0f | groups %.0f | bufs %.0f | pipeline %.0f | diag walks %.0f ms",
        msExtract, msMeta, msGroups, msBufs, msPipe, msDiag);
}

bool Built() { return s_built && !s_groups.empty() && s_cullPipe != VK_NULL_HANDLE; }

VkBuffer GetMetaBuffer() { return s_meta ? s_meta->GetHandle() : VK_NULL_HANDLE; }
u32      CasterCount()   { return s_total; }
u32      MetaStride()    { return (u32)sizeof(GpuCasterMeta); }
u32      GroupCount()    { return (u32)s_groups.size(); }
u32      MaxGroupMesh()  { return s_maxGroupMesh; }

bool GetGroupBind(u32 g, VkBuffer& vb, VkBuffer& ib, u32& stride, VkIndexType& iType)
{
    if (g >= s_groups.size()) return false;
    const Group& grp = s_groups[g];
    vb = grp.vb; ib = grp.ib; stride = grp.stride; iType = grp.iType;
    return true;
}

void Cull(VkCommandBuffer cmd, const Target* tgts, const Fvector4* planes, u32 n,
          const Fvector& camPos, float lodDist)
{
    if (!Built() || n == 0) return;
    const u32 nGroups = (u32)s_groups.size();
    const u32 wg = (s_total + 255) / 256;   // one dispatch covers ALL casters per target

    // WAR-guard vs the previous frame's indirect reads (once for the whole batch).
    MemBarrier(cmd, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // Clear each target's count region.
    for (u32 i = 0; i < n; ++i)
        vkCmdFillBuffer(cmd, s_count->GetHandle(), (VkDeviceSize)Region(tgts[i], 0) * sizeof(u32),
                        (VkDeviceSize)nGroups * sizeof(u32), 0u);
    MemBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullLayout, 0, 1, &s_set, 0, nullptr);
    for (u32 i = 0; i < n; ++i) {
        CullPush pc{};
        for (int p = 0; p < 6; ++p) pc.planes[p] = planes[i * 6 + p];
        pc.target       = (u32)tgts[i];
        pc.numGroups    = nGroups;
        pc.maxGroupMesh = s_maxGroupMesh;
        pc.total        = s_total;
        pc.camLod.x = camPos.x; pc.camLod.y = camPos.y; pc.camLod.z = camPos.z; pc.camLod.w = lodDist;
        vkCmdPushConstants(cmd, s_cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, wg, 1, 1);
    }
    // compute write → indirect read (once for the whole batch)
    MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
}

void Draw(VkCommandBuffer cmd, Target tgt, const Fmatrix& lightVP)
{
    if (!Built()) return;
    VkPipelineLayout layout = PipelineCache::GetDepthLayout();
    if (layout == VK_NULL_HANDLE) return;
    const u32 nGroups = (u32)s_groups.size();

    // Opaque depth push = lightVP (statics are world-space; xform is identity).
    Fmatrix vp = lightVP;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Fmatrix), &vp);

    VkPipeline  lastPipe = VK_NULL_HANDLE;
    VkBuffer    lastVB   = VK_NULL_HANDLE;
    VkBuffer    lastIB   = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;
    for (u32 g = 0; g < nGroups; ++g) {
        const Group& grp = s_groups[g];
        VkPipeline pipe = PipelineCache::GetDepthPipeline(grp.stride);
        if (pipe == VK_NULL_HANDLE) continue;
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; }
        if (grp.vb != lastVB) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &z); lastVB = grp.vb; }
        if (grp.ib != lastIB || grp.iType != lastIType) { vkCmdBindIndexBuffer(cmd, grp.ib, 0, grp.iType); lastIB = grp.ib; lastIType = grp.iType; }
        const VkDeviceSize cmdOff = (VkDeviceSize)Region(tgt, g) * s_maxGroupMesh * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)Region(tgt, g) * sizeof(u32);
        vkCmdDrawIndexedIndirectCount(cmd, s_indirect->GetHandle(), cmdOff, s_count->GetHandle(), cntOff,
                                      s_maxGroupMesh, sizeof(VkDrawIndexedIndirectCommand));
    }
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_cullPipe)   { vkDestroyPipeline(VulkanHW.m_Device, s_cullPipe, nullptr); s_cullPipe = VK_NULL_HANDLE; }
    if (s_cullLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_cullLayout, nullptr); s_cullLayout = VK_NULL_HANDLE; }
    if (s_pool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setL)       { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setL, nullptr); s_setL = VK_NULL_HANDLE; }
    auto del = [](CVulkanBuffer*& b) { if (b) { xr_delete(b); b = nullptr; } };
    del(s_meta); del(s_indirect); del(s_count);
    s_groups.clear(); s_total = 0; s_maxGroupMesh = 0; s_set = VK_NULL_HANDLE; s_built = false;
}

}} // namespace VK::ShadowGPU

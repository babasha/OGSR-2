// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// GPU-driven instanced rigid casters — see vk_instance_gpu.h for the why.
// Mirrors vk_shadow_gpu (cull/indirect machinery) and vk_TreeManager (the
// per-instance transform SSBO + firstInstance indexing).

#include "stdafx.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_instance_gpu.h"
#include "CRender_Vulkan.h"     // RImplementation
#include "vk_Visual.h"          // vkFVisual, vkFHierrarhyVisual, m_mesh
#include "vk_world_material.h"  // WorldMaterial (alphaRef, isWmark)
#include "vk_buffer.h"          // CVulkanBuffer
#include "vk_pipeline_cache.h"  // world pipelines (Key::instanced) + push offsets
#include "vk_cull.h"            // VK::ExtractFrustumPlanes (camera frustum → cull planes)
#include "vk_compute_util.h"    // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_gfx_pipeline.h"    // VK::GfxPipelineBuilder
#include "vk_shaders.h"         // g_ShaderManager
#include "vk_pass_world.h"      // DynamicVisuals(), DynVisual
#include "vk_pass_skinned.h"    // Skinned_HandlesVisual
#include <algorithm>

namespace VK { namespace InstanceGPU {

namespace {

struct Group {
    VkBuffer       vb, ib;
    u32            stride;
    u32            tcOffset;      // 24 = lmap sub-layout, else vertex-lit
    VkIndexType    iType;
    WorldMaterial* mat;           // colour pass: descriptor set + push tail
    bool           alphaTested;   // shadow cull drops these (depth-only can't discard)
    u32            entryOffset;   // prefix sum into meta/indirect
    u32            entryCount;
};

// Alpha-testedness is a GROUP property (one mesh = one material), so it rides the
// high bit of groupBase rather than costing every meta entry 16 bytes of padding.
// Mirrored in instance_cull.comp.glsl.
constexpr u32 kGroupATBit    = 0x80000000u;
constexpr u32 kGroupBaseMask = 0x7FFFFFFFu;

// 112 B — within the guaranteed minimum maxPushConstantsSize (128).
struct CullPush { Fvector4 planes[6]; u32 target, total, numGroups, skipAlphaTested; };

bool s_structDirty = true;    // meta/groups/buffers must be rebuilt
bool s_xformDirty  = true;    // transform SSBO must be re-uploaded
bool s_ready       = false;
bool s_pipeFailed  = false;   // shader/pipeline creation failed — never retry

u32 s_total = 0;
xr_vector<Group>       s_groups;
xr_vector<GpuInstMeta> s_metaCPU;
xr_vector<Fmatrix>     s_xformCPU;

CVulkanBuffer* s_meta      = nullptr;   // total * 32 B
CVulkanBuffer* s_xform     = nullptr;   // total * 64 B
CVulkanBuffer* s_groupBase = nullptr;   // numGroups * u32
CVulkanBuffer* s_indirect  = nullptr;   // TGT_COUNT * total cmds
CVulkanBuffer* s_count     = nullptr;   // TGT_COUNT * numGroups u32

VkDescriptorSetLayout s_cullSetL = VK_NULL_HANDLE;
VkDescriptorPool      s_pool     = VK_NULL_HANDLE;
VkDescriptorSet       s_cullSet  = VK_NULL_HANDLE;
VkPipelineLayout      s_cullLayout = VK_NULL_HANDLE;
VkPipeline            s_cullPipe   = VK_NULL_HANDLE;

VkDescriptorSetLayout s_drawSetL   = VK_NULL_HANDLE;
VkDescriptorSet       s_drawSet    = VK_NULL_HANDLE;
VkPipelineLayout      s_drawLayout = VK_NULL_HANDLE;
xr_map<u32, VkPipeline> s_drawPipes;   // keyed by vertex stride

void MemBarrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst,
                VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = src; b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 1, &b, 0, nullptr, 0, nullptr);
}

// One flattened instance before grouping.
struct RawInst { vkFVisual* fv; Fmatrix xform; };

// Leaves this path deliberately does NOT own, and therefore cannot let the CPU
// queue be skipped over: wmark decals (never write depth) and the late-translucent
// classes isGlass / isEmisAdd / isLitBlend, which vk_Visual routes to a separate
// blended pass (vk_Visual.cpp:432). Drawing those as ordinary opaques would be
// wrong, and dropping the CPU queue while any exist would lose them entirely — so
// a non-zero count DISABLES the instanced colour path instead of silently
// corrupting the picture. Shadows are unaffected (none of these cast).
u32 s_excludedLeaves  = 0;
u32 s_excludedVisuals = 0;
xr_vector<vkRender_Visual*> s_owned;   // sorted; the CPU queue must skip exactly these

// Recursively flatten a pushed visual into rigid leaves, carrying the instance
// transform down. Mirrors ShadowGPU::ExtractCasters (vk_shadow_gpu.cpp:70) and
// vkFHierrarhyVisual::Submit, which forwards the SAME xform to every child — a
// top-level-only walk would silently drop composite props (buildings built as a
// hierarchy).
// ALPHA-TESTED LEAVES ARE INCLUDED. For the COLOUR pass there is no opaque/AT
// split at all: the world FS applies alphaRef from the material tail, one pipeline
// family. Only the depth-only SHADOW pipelines need the split (they cannot
// discard), and the cull drops AT groups for those targets via kGroupATBit.
// Wmark (baked decals) stay out — they never write depth and the CPU queue owns
// them; counted so a scene that actually has some is visible in the log rather
// than silently missing geometry.
void Flatten(vkRender_Visual* rv, const Fmatrix& xf, xr_vector<RawInst>& out, bool& excluded)
{
    if (!rv) return;
    const u32 t = rv->Type;
    if (t == MT_NORMAL || t == MT_PROGRESSIVE) {
        auto* fv = static_cast<vkFVisual*>(rv);
        if (!fv->m_mesh.IsValid() || !fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) return;
        WorldMaterial* mat = fv->m_pWorldMaterial;
        // TERRAIN is excluded for a different reason than the translucent classes:
        // it needs its OWN pipeline and its 15-binding terrain descriptor set (base,
        // splat mask, 4 detail, lmap, 4 detail-normals, 4 detail-heights), which
        // RenderQueue::Flush binds and this path does not. Drawing it here with the
        // ordinary world pipeline + ordinary material set silently dropped every
        // detail layer and rendered the ground as a smooth blur. Terrain is a handful
        // of meshes, so leaving it on the CPU queue costs nothing; an instanced
        // terrain variant would need an INSTANCED world_terrain.vert first.
        if (mat && (mat->isWmark || mat->isGlass || mat->isEmisAdd || mat->isLitBlend
                    || mat->isWater || mat->isTerrain)) {
            excluded = true;
            return;
        }
        out.push_back({ fv, xf });
        return;
    }
    if (t == MT_HIERRARHY || t == MT_LOD) {
        auto* hv = dynamic_cast<vkFHierrarhyVisual*>(rv);
        if (!hv) return;
        for (auto* child : hv->children)
            Flatten(child, xf, out, excluded);
    }
}

// Walk DynamicVisuals() and take ownership at WHOLE-VISUAL granularity: a visual
// with even one late-translucent leaf is left entirely to the CPU queue. Per-leaf
// ownership would be finer but unusable — the CPU submit works per visual, so
// splitting one visual across both paths would double-draw its ordinary leaves.
// On Cordon this costs ~85 leaves out of 22k (0.4%), which is the right trade.
// Ownership is the SAME for the colour and shadow paths, and both `Rebuild` and
// `RefreshTransforms` must derive it identically or the transform slots misalign.
void CollectOwned(xr_vector<RawInst>& out, xr_vector<vkRender_Visual*>* ownedOut)
{
    xr_vector<RawInst> perVisual;
    for (const DynVisual& d : DynamicVisuals()) {
        if (!d.vis) continue;
        if (Skinned_HandlesVisual(d.vis)) continue;   // skeletons: Skinned_RenderShadow
        perVisual.clear();
        bool excluded = false;
        Flatten(d.vis, d.xform, perVisual, excluded);
        if (excluded) { s_excludedLeaves += (u32)perVisual.size() + 1; ++s_excludedVisuals; continue; }
        if (perVisual.empty()) continue;
        out.insert(out.end(), perVisual.begin(), perVisual.end());
        if (ownedOut) ownedOut->push_back(d.vis);
    }
}

bool CreateCullPipeline()
{
    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule cs = g_ShaderManager->Load("instance_cull.comp.spv");
    if (!cs) { Msg("![VK InstanceGPU] instance_cull.comp.spv load failed"); return false; }

    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    s_cullSetL = VK::MakeSetLayout({ kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                   VK_SHADER_STAGE_COMPUTE_BIT, "InstanceGPU.Cull");
    // Draw side: set 0 = the same transform SSBO, read by the VERTEX stage.
    s_drawSetL = VK::MakeSetLayout({ kSSBO }, VK_SHADER_STAGE_VERTEX_BIT, "InstanceGPU.Draw");
    if (!s_cullSetL || !s_drawSetL) return false;

    // One pool for both sets: 5 SSBOs for the cull set + 1 for the draw set.
    VkDescriptorPoolSize ps{ kSSBO, 6 };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 2; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_pool) != VK_SUCCESS) return false;

    if (!VK::AllocSets(s_pool, s_cullSetL, 1, &s_cullSet, "InstanceGPU.Cull")) return false;
    if (!VK::AllocSets(s_pool, s_drawSetL, 1, &s_drawSet, "InstanceGPU.Draw")) return false;

    s_cullLayout = VK::MakePipelineLayout({ s_cullSetL }, sizeof(CullPush));
    if (s_cullLayout == VK_NULL_HANDLE) return false;
    s_cullPipe = VK::CreateComputePipeline(cs, s_cullLayout, "InstanceGPU.Cull");
    if (s_cullPipe == VK_NULL_HANDLE) return false;

    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, (u32)sizeof(Fmatrix) };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &s_drawSetL;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_drawLayout) != VK_SUCCESS) return false;

    return true;
}

// Depth-only instanced caster pipeline. State copied from
// PipelineCache::GetDepthPipeline (vk_pipeline_cache.cpp:379) so this path
// rasterises identically to the CPU one it replaces — same D32 target, same
// LESS_OR_EQUAL, same dynamic bias, cull NONE. Only the VS and layout differ.
VkPipeline GetDrawPipeline(u32 stride)
{
    auto it = s_drawPipes.find(stride);
    if (it != s_drawPipes.end()) return it->second;

    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule vs = g_ShaderManager->Load("instance_depth.vert.spv");
    if (!vs) {
        Msg("![VK InstanceGPU] instance_depth.vert.spv load failed");
        s_drawPipes.emplace(stride, VK_NULL_HANDLE);
        return VK_NULL_HANDLE;
    }

    VkPipeline h = VK::GfxPipelineBuilder(s_drawLayout)
        .Vert(vs)
        .Binding(0, stride)
        .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
        .DynamicDepthBias()
        .Depth(true, true)
        .DepthTarget(VK_FORMAT_D32_SFLOAT)   // matches the shadow map
        .Build("InstanceGPU instanced depth stride=%u", stride);
    if (h != VK_NULL_HANDLE)
        Msg("[VK InstanceGPU] instanced depth pipeline stride=%u", stride);
    s_drawPipes.emplace(stride, h);
    return h;
}

void ReleaseBuffers()
{
    auto del = [](CVulkanBuffer*& b) { if (b) { xr_delete(b); b = nullptr; } };
    del(s_meta); del(s_xform); del(s_groupBase); del(s_indirect); del(s_count);
}

// Rebuild meta + groups + buffers from the current DynamicVisuals() contents.
// Structural only: called when objects are added/removed, NOT when they move.
void Rebuild()
{
    s_structDirty    = false;
    s_ready          = false;
    s_excludedLeaves = 0;
    s_groups.clear(); s_metaCPU.clear(); s_xformCPU.clear(); s_total = 0;

    if (s_pipeFailed) return;
    if (DynamicVisuals().empty()) { ReleaseBuffers(); return; }

    xr_vector<RawInst> raw;
    raw.reserve(DynamicVisuals().size());
    s_owned.clear();
    s_owned.reserve(DynamicVisuals().size());
    CollectOwned(raw, &s_owned);
    std::sort(s_owned.begin(), s_owned.end());   // OwnsVisual is a binary search
    if (raw.empty()) { ReleaseBuffers(); return; }

    // Group by (vb, ib, stride, iType) — one bind + one indirect draw per group.
    // Each host .ogf owns its own buffers, so this lands at ~one group per
    // distinct model (434 on Cordon) regardless of the instance count.
    std::stable_sort(raw.begin(), raw.end(), [](const RawInst& a, const RawInst& b) {
        VkBuffer av = a.fv->m_mesh.p_rm_Vertices->GetHandle(), bv = b.fv->m_mesh.p_rm_Vertices->GetHandle();
        if (av != bv) return av < bv;
        VkBuffer ai = a.fv->m_mesh.p_rm_Indices->GetHandle(),  bi = b.fv->m_mesh.p_rm_Indices->GetHandle();
        if (ai != bi) return ai < bi;
        if (a.fv->m_mesh.vStride != b.fv->m_mesh.vStride) return a.fv->m_mesh.vStride < b.fv->m_mesh.vStride;
        return (u32)a.fv->m_mesh.iType < (u32)b.fv->m_mesh.iType;
    });

    s_total = (u32)raw.size();
    s_metaCPU.resize(s_total);
    s_xformCPU.resize(s_total);

    auto sameGroup = [](const RawInst& a, const RawInst& b) {
        return a.fv->m_mesh.p_rm_Vertices->GetHandle() == b.fv->m_mesh.p_rm_Vertices->GetHandle()
            && a.fv->m_mesh.p_rm_Indices->GetHandle()  == b.fv->m_mesh.p_rm_Indices->GetHandle()
            && a.fv->m_mesh.vStride == b.fv->m_mesh.vStride
            && a.fv->m_mesh.iType   == b.fv->m_mesh.iType;
    };

    for (u32 i = 0; i < s_total; ++i) {
        const RawInst& r = raw[i];
        if (s_groups.empty() || !sameGroup(raw[s_groups.back().entryOffset], r)) {
            Group g{};
            g.vb = r.fv->m_mesh.p_rm_Vertices->GetHandle();
            g.ib = r.fv->m_mesh.p_rm_Indices->GetHandle();
            g.stride   = r.fv->m_mesh.vStride;
            g.tcOffset = r.fv->m_mesh.tcOffset;
            g.iType    = r.fv->m_mesh.iType;
            // One mesh has one material, and groups are keyed per (vb,ib) — i.e.
            // per mesh — so the material and its alpha-testedness are group-wide.
            g.mat = r.fv->m_pWorldMaterial ? r.fv->m_pWorldMaterial : WorldMaterialCache::GetDefault();
            g.alphaTested = g.mat && g.mat->alphaRef >= 0.f;
            g.entryOffset = i; g.entryCount = 0;
            s_groups.push_back(g);
        }
        Group& g = s_groups.back();
        ++g.entryCount;

        GpuInstMeta& m = s_metaCPU[i];
        m.sphere_P    = r.fv->vis.sphere.P;      // MODEL space — cull transforms it
        m.sphere_R    = r.fv->vis.sphere.R;
        m.index_count = r.fv->m_mesh.iCount;
        m.ib_first    = r.fv->m_mesh.iBase;
        m.first_vertex= r.fv->m_mesh.vBase;
        m.group       = (u32)s_groups.size() - 1;

        s_xformCPU[i] = r.xform;
    }

    const u32 nGroups = (u32)s_groups.size();

    // A structural rebuild frees buffers that earlier frames may still be reading.
    // Those rebuilds are rare (scene load / add / delete), so a full idle is the
    // honest price; a moved object never reaches here (see TouchTransforms).
    VK_DeviceWaitIdleLocked();
    ReleaseBuffers();

    // High bit = alpha-tested, read by the shadow cull to drop the group.
    xr_vector<u32> bases(nGroups);
    for (u32 g = 0; g < nGroups; ++g)
        bases[g] = s_groups[g].entryOffset | (s_groups[g].alphaTested ? kGroupATBit : 0u);

    auto make = [](CVulkanBuffer*& b, VkDeviceSize size, VkBufferUsageFlags extra) {
        b = xr_new<CVulkanBuffer>();
        b->Create(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra,
                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    };
    make(s_meta,      (VkDeviceSize)s_total * sizeof(GpuInstMeta), 0);
    make(s_xform,     (VkDeviceSize)s_total * sizeof(Fmatrix),     0);
    make(s_groupBase, (VkDeviceSize)nGroups * sizeof(u32),         0);
    make(s_indirect,  (VkDeviceSize)TGT_COUNT * s_total * sizeof(VkDrawIndexedIndirectCommand),
         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    make(s_count,     (VkDeviceSize)TGT_COUNT * nGroups * sizeof(u32),
         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);

    s_meta->Upload(s_metaCPU.data(), (VkDeviceSize)s_total * sizeof(GpuInstMeta));
    s_groupBase->Upload(bases.data(), (VkDeviceSize)nGroups * sizeof(u32));
    s_xform->Upload(s_xformCPU.data(), (VkDeviceSize)s_total * sizeof(Fmatrix));
    s_xformDirty = false;

    VK::DescriptorWriter(s_cullSet)
        .StorageBuffer(0, s_meta->GetHandle())
        .StorageBuffer(1, s_xform->GetHandle())
        .StorageBuffer(2, s_indirect->GetHandle())
        .StorageBuffer(3, s_count->GetHandle())
        .StorageBuffer(4, s_groupBase->GetHandle())
        .Flush();
    // The draw set only needs the transforms (the VS pulls its instance row from them).
    VK::DescriptorWriter(s_drawSet).StorageBuffer(0, s_xform->GetHandle()).Flush();

    u32 atGroups = 0, atInstances = 0;
    for (const Group& g : s_groups) if (g.alphaTested) { ++atGroups; atInstances += g.entryCount; }
    s_ready = true;
    Msg("[VK InstanceGPU] built: %u instance(s) in %u group(s) from %zu dynamic visual(s) "
        "| alpha-tested: %u instance(s) in %u group(s) (colour: all; shadows: opaque only)",
        s_total, nGroups, DynamicVisuals().size(), atInstances, atGroups);
    if (s_excludedVisuals)
        Msg("[VK InstanceGPU] %u visual(s) left to the CPU queue — they carry wmark/glass/"
            "emissive/lit-blend leaves that belong to the late translucent pass (whole-visual "
            "granularity: the CPU submit works per visual, so a split would double-draw)",
            s_excludedVisuals);
}

// Re-read every instance's matrix and push it. No realloc, no device wait — this
// is the path a gizmo drag takes, and it is why the meta keeps model-space bounds.
void RefreshTransforms()
{
    s_xformDirty = false;
    if (!s_xform || s_total == 0) return;

    xr_vector<RawInst> raw;
    raw.reserve(DynamicVisuals().size());
    CollectOwned(raw, nullptr);
    // The flatten order is stable, but the set is grouped — a changed COUNT means
    // the structure moved under us, so fall back to a full rebuild rather than
    // writing matrices against stale slots.
    if (raw.size() != s_total) { s_structDirty = true; return; }

    // Same stable sort as Rebuild, so slot i still belongs to the same leaf.
    std::stable_sort(raw.begin(), raw.end(), [](const RawInst& a, const RawInst& b) {
        VkBuffer av = a.fv->m_mesh.p_rm_Vertices->GetHandle(), bv = b.fv->m_mesh.p_rm_Vertices->GetHandle();
        if (av != bv) return av < bv;
        VkBuffer ai = a.fv->m_mesh.p_rm_Indices->GetHandle(),  bi = b.fv->m_mesh.p_rm_Indices->GetHandle();
        if (ai != bi) return ai < bi;
        if (a.fv->m_mesh.vStride != b.fv->m_mesh.vStride) return a.fv->m_mesh.vStride < b.fv->m_mesh.vStride;
        return (u32)a.fv->m_mesh.iType < (u32)b.fv->m_mesh.iType;
    });
    for (u32 i = 0; i < s_total; ++i) s_xformCPU[i] = raw[i].xform;
    s_xform->Upload(s_xformCPU.data(), (VkDeviceSize)s_total * sizeof(Fmatrix));
}

void EnsureCurrent()
{
    if (s_pipeFailed) return;
    if (s_cullPipe == VK_NULL_HANDLE) {
        if (!CreateCullPipeline()) {
            s_pipeFailed = true;
            Msg("![VK InstanceGPU] disabled — pipeline creation failed; CPU rigid path stays in charge");
            return;
        }
    }
    if (s_structDirty) Rebuild();
    if (s_xformDirty)  RefreshTransforms();
}

} // anonymous namespace

// Clearing s_ready HERE, not at the next Rebuild, is load-bearing: HostClearScene
// deletes the visuals and only then invalidates, so between those two points the
// cached group vb/ib handles refer to freed buffers. DrawShadow does not rebuild
// (it must stay callable inside a render pass), so it has to see "not ready"
// immediately or it would submit a draw against deleted geometry.
void Invalidate()      { s_structDirty = true; s_ready = false; }
void TouchTransforms() { s_xformDirty  = true; }
bool Ready()           { return s_ready && s_total > 0; }

// Cheap binary search over the sorted owned list. Both the colour submit and the
// shadow submit key off this, so the CPU and GPU halves can never overlap or gap.
bool OwnsVisual(vkRender_Visual* v)
{
    if (!s_ready || !v) return false;
    return std::binary_search(s_owned.begin(), s_owned.end(), v);
}
u32  InstanceCount()   { return s_total; }
u32  GroupCount()      { return (u32)s_groups.size(); }

// Shared cull body. `skipAT` drops alpha-tested groups (depth-only shadow
// pipelines cannot discard); the colour target keeps everything.
static void CullBatch(VkCommandBuffer cmd, const u32* tgts, const Fvector4* planes, u32 n, u32 skipAT)
{
    const u32 nGroups = (u32)s_groups.size();

    // Zero only the regions this batch writes (the others keep last frame's counts,
    // which nothing reads because their draws are gated on the same target list).
    for (u32 i = 0; i < n; ++i)
        vkCmdFillBuffer(cmd, s_count->GetHandle(),
                        (VkDeviceSize)((u32)tgts[i] * nGroups) * sizeof(u32),
                        (VkDeviceSize)nGroups * sizeof(u32), 0u);
    MemBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullLayout, 0, 1, &s_cullSet, 0, nullptr);
    const u32 wg = (s_total + 63) / 64;
    for (u32 i = 0; i < n; ++i) {
        CullPush pc{};
        for (int p = 0; p < 6; ++p) pc.planes[p] = planes[i * 6 + p];
        pc.target = tgts[i]; pc.total = s_total; pc.numGroups = nGroups;
        pc.skipAlphaTested = skipAT;
        vkCmdPushConstants(cmd, s_cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, wg, 1, 1);
    }
    MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
}

void CullShadow(VkCommandBuffer cmd, const Target* tgts, const Fvector4* planes, u32 n)
{
    EnsureCurrent();
    if (!Ready() || n == 0) return;
    u32 ids[TGT_COUNT];
    const u32 cnt = (n < TGT_COUNT) ? n : (u32)TGT_COUNT;
    for (u32 i = 0; i < cnt; ++i) ids[i] = (u32)tgts[i];
    CullBatch(cmd, ids, planes, cnt, 1u);   // shadows: opaque only
}

void CullColor(VkCommandBuffer cmd, const Fmatrix& viewProj)
{
    EnsureCurrent();
    if (!Ready()) return;
    Fvector4 planes[12];
    VK::ExtractFrustumPlanes(viewProj, planes);
    for (int p = 0; p < 6; ++p) planes[6 + p] = planes[p];   // same frustum, both regions

    // Two regions from one frustum: the colour pass takes everything, the depth
    // prepass only the opaque half (its pipeline cannot discard). Two dispatches
    // over 8k instances is a rounding error next to the draw it replaces.
    const u32 tgtAll = (u32)TGT_COLOR;
    CullBatch(cmd, &tgtAll, planes, 1, 0u);
    const u32 tgtOpaque = (u32)TGT_COLOR_OPAQUE;
    CullBatch(cmd, &tgtOpaque, planes, 1, 1u);
}

void DrawShadow(VkCommandBuffer cmd, Target tgt, const Fmatrix& lightVP)
{
    if (!Ready() || s_drawLayout == VK_NULL_HANDLE) return;
    const u32 nGroups = (u32)s_groups.size();

    Fmatrix vp = lightVP;
    vkCmdPushConstants(cmd, s_drawLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Fmatrix), &vp);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_drawLayout, 0, 1, &s_drawSet, 0, nullptr);

    VkPipeline  lastPipe = VK_NULL_HANDLE;
    VkBuffer    lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;
    for (u32 g = 0; g < nGroups; ++g) {
        const Group& grp = s_groups[g];
        VkPipeline pipe = GetDrawPipeline(grp.stride);
        if (pipe == VK_NULL_HANDLE) continue;
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; }
        if (grp.vb != lastVB) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &z); lastVB = grp.vb; }
        if (grp.ib != lastIB || grp.iType != lastIType) {
            vkCmdBindIndexBuffer(cmd, grp.ib, 0, grp.iType); lastIB = grp.ib; lastIType = grp.iType;
        }
        const VkDeviceSize cmdOff = (VkDeviceSize)((u32)tgt * s_total + grp.entryOffset)
                                  * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)((u32)tgt * nGroups + g) * sizeof(u32);
        vkCmdDrawIndexedIndirectCount(cmd, s_indirect->GetHandle(), cmdOff, s_count->GetHandle(), cntOff,
                                      grp.entryCount, sizeof(VkDrawIndexedIndirectCommand));
    }
}

// ============================================================================
//  CAMERA VIEW — depth prepass + colour pass
// ============================================================================

bool ColorReady()
{
    return Ready()
        && PipelineCache::WorldLmapInstVS() != VK_NULL_HANDLE
        && PipelineCache::WorldVlitInstVS() != VK_NULL_HANDLE
        && PipelineCache::GetLayout()       != VK_NULL_HANDLE;
}

// The prepass depth pipeline is the same D32 depth-only one the shadow targets
// use — Pass_EditorDynamics only enables the prepass when the scene depth IS
// D32_SFLOAT (vk_pass_world.cpp:832), so the formats agree by construction.
// Draws the OPAQUE region: alpha-tested leaves are absent from the prepass (they
// simply miss out on early-Z and GTAO), which is correct — punching solid depth
// for a leaf card would occlude everything behind the foliage.
// Gated on ColorReady(), not Ready(): when the colour path stands down the CPU
// queue draws the prepass, and adding this would just redraw the same depth.
void DrawColorDepth(VkCommandBuffer cmd, const Fmatrix& viewProj)
{
    if (!ColorReady()) return;
    DrawShadow(cmd, TGT_COLOR_OPAQUE, viewProj);
}

void DrawColor(VkCommandBuffer cmd, const Fmatrix& viewProj, VkDescriptorSet envSet)
{
    if (!ColorReady()) return;
    const u32 nGroups = (u32)s_groups.size();
    const VkShaderStageFlags kStages = PipelineCache::GetPushStages();

    // Push layout mirrors RenderQueue::Flush exactly — the shaders are the same.
    // mvp holds the plain viewProj here: the INSTANCED vertex body applies the
    // model matrix from the instance-rate attributes instead of push constants.
    struct MvpUV { Fmatrix mvp; float uvScale[2]; };
    MvpUV base{}; base.mvp = viewProj; base.uvScale[0] = 1.0f/1024.0f; base.uvScale[1] = 1.0f/1024.0f;
    struct TessBlock { float tessMax, tessNear, tessFar; float eyeHeight[4]; float pnScale; };
    const TessBlock tessZero{ 0.f, 0.f, 1.f, {0,0,0,0}, 0.f };
    constexpr u32 kTailOffset     = sizeof(Fmatrix) + 2 * sizeof(float);    // 72
    constexpr u32 kTessOffset     = sizeof(Fmatrix) + 5 * sizeof(float);    // 84
    constexpr u32 kStreamIDOffset = sizeof(Fmatrix) + 13 * sizeof(float);   // 116

    VkPipelineLayout layout = PipelineCache::GetLayout();
    vkCmdPushConstants(cmd, layout, kStages, 0, sizeof(base), &base);
    vkCmdPushConstants(cmd, layout, kStages, kTessOffset, sizeof(tessZero), &tessZero);
    if (envSet != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &envSet, 0, nullptr);

    // Instance-rate binding 1 = the transform SSBO, read via firstInstance.
    const VkBuffer xfBuf = s_xform->GetHandle();

    VkPipeline  lastPipe = VK_NULL_HANDLE;
    VkDescriptorSet lastMatSet = VK_NULL_HANDLE;
    VkBuffer    lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;

    for (u32 g = 0; g < nGroups; ++g) {
        const Group& grp = s_groups[g];
        WorldMaterial* mat = grp.mat;
        if (!mat || mat->set == VK_NULL_HANDLE) continue;

        PipelineCache::Key k{};
        k.stride = grp.stride; k.tcOffset = grp.tcOffset;
        const bool lmap = (grp.tcOffset == 24);
        k.vs = lmap ? PipelineCache::WorldLmapInstVS() : PipelineCache::WorldVlitInstVS();
        k.fs = lmap ? PipelineCache::WorldLmapFS()     : PipelineCache::WorldVlitFS();
        k.depthTest = true; k.wmark = false; k.tess = false;
        k.instanced = true;
        k.specMask  = mat->tessellated ? (u8)PipelineCache::WS_POM : (u8)0;
        VkPipeline pipe = PipelineCache::Get(k);
        if (pipe == VK_NULL_HANDLE) continue;

        if (pipe != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            lastPipe = pipe; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; lastMatSet = VK_NULL_HANDLE;
        }
        if (mat->set != lastMatSet) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &mat->set, 0, nullptr);
            lastMatSet = mat->set;
        }
        // hemi is sign-ENCODED as -(1+hemi) — the FS decodes -x-1. The host scene
        // uses hemi 1.0 (add_Visual leaves it at 1.0 for a null root, and
        // Pass_EditorDynamics pushes exactly that), so -2.0 reproduces the CPU
        // path's sky ambient bit for bit. The negative sign also told the
        // non-instanced VS "model rows follow"; the INSTANCED body ignores it.
        const float tail[3] = { mat->alphaRef, mat->detailScale, -2.0f };
        vkCmdPushConstants(cmd, layout, kStages, kTailOffset, sizeof(tail), tail);
        vkCmdPushConstants(cmd, layout, kStages, kStreamIDOffset, sizeof(u32), &mat->streamID);

        if (grp.vb != lastVB) {
            const VkBuffer bufs[2] = { grp.vb, xfBuf };
            const VkDeviceSize offs[2] = { 0, 0 };
            vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
            lastVB = grp.vb;
        }
        if (grp.ib != lastIB || grp.iType != lastIType) {
            vkCmdBindIndexBuffer(cmd, grp.ib, 0, grp.iType); lastIB = grp.ib; lastIType = grp.iType;
        }
        const VkDeviceSize cmdOff = (VkDeviceSize)((u32)TGT_COLOR * s_total + grp.entryOffset)
                                  * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)((u32)TGT_COLOR * nGroups + g) * sizeof(u32);
        vkCmdDrawIndexedIndirectCount(cmd, s_indirect->GetHandle(), cmdOff, s_count->GetHandle(), cntOff,
                                      grp.entryCount, sizeof(VkDrawIndexedIndirectCommand));
    }
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    for (auto& kv : s_drawPipes) if (kv.second) vkDestroyPipeline(VulkanHW.m_Device, kv.second, nullptr);
    s_drawPipes.clear();
    if (s_drawLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_drawLayout, nullptr); s_drawLayout = VK_NULL_HANDLE; }
    if (s_cullPipe)   { vkDestroyPipeline(VulkanHW.m_Device, s_cullPipe, nullptr); s_cullPipe = VK_NULL_HANDLE; }
    if (s_cullLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_cullLayout, nullptr); s_cullLayout = VK_NULL_HANDLE; }
    if (s_pool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_cullSetL)   { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_cullSetL, nullptr); s_cullSetL = VK_NULL_HANDLE; }
    if (s_drawSetL)   { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_drawSetL, nullptr); s_drawSetL = VK_NULL_HANDLE; }
    ReleaseBuffers();
    s_groups.clear(); s_metaCPU.clear(); s_xformCPU.clear();
    s_total = 0; s_ready = false; s_structDirty = true; s_xformDirty = true;
    s_cullSet = VK_NULL_HANDLE; s_drawSet = VK_NULL_HANDLE;
}

}} // namespace VK::InstanceGPU

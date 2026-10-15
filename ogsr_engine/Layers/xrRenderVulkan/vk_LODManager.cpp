// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — LOD imposter manager implementation.
// See vk_LODManager.h. v1: nearest-facet billboard, distance-gated.

#include "stdafx.h"
extern float ps_r_tree_mesh_dist;   // r_tree_mesh_dist (vk_console_min.cpp) -- global, so declared outside the VK namespaces (after the PCH include: MSVC /Yu drops anything above it)
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_LODManager.h"
#include "vk_world_gpu.h"       // nfs12 census: WorldGPU::LeafVisuals
#include "vk_Visual.h"            // vkFLOD
#include "vk_world_material.h"    // fallback white
#include "vk_texture.h"           // CVulkanTexture (level_lods atlas)
#include "vk_buffer.h"
#include "vk_pass_context.h"
#include "vk_swapchain.h"
#include "vk_scene_color.h"       // HDR scene target format
#include "vk_shaders.h"           // g_ShaderManager
#include "vk_pipeline_cache.h"    // PipelineCache::GetCacheObject
#include "vk_command_buffer.h"    // CommandManager
#include "vk_compute_util.h"      // MakePipelineLayout / CreateComputePipeline
#include "vk_gfx_pipeline.h"      // GfxPipelineBuilder
#include "vk_descriptors.h"       // VK::DescriptorWriter
#include "vk_cull.h"              // ExtractFrustumPlanes (GPU cull push)
#include "vk_DetailManager.h"     // shared HZB pyramid (grass/world)
#include "vk_profiler.h"          // GPU zones for the cull dispatch
#include "vk_env_light.h"         // 10-10: the env set (set 2) -- the imposter fog fades into the sky (nx_fog.glsl)
#include "HW_Vulkan.h"
#include "CRender_Vulkan.h"
#include "../../xr_3da/IGame_Persistent.h" // ES()
#include "../../xr_3da/Environment.h"      // CEnvDescriptorMixer (sun_color/hemi_color)

// kImposterMinDist moved to vk_LODManager.h — CTreeManager's r_tree_dist cut has
// to floor itself at the same value (drop the mesh only where the billboard is
// already drawing), and two copies of that number would drift.

// GPU path (compute cull + vertex pulling) — instant A/B against the CPU walk.
extern int ps_r_lods_gpu;
extern int ps_r_tree_lod_size;   // nfs21 r_tree_lod_size (vk_console_min.cpp)
extern float ps_r_tree_lod_band;  // nfs23
extern float ps_r_fog_dist;       // r_fog_dist — forward distance-fog range scale (0 = off)

extern int ps_r_hzb_cull, ps_r_tree_hzb, ps_r_hzb_prepass;   // vk_console_min.cpp

namespace VK
{
float ImposterMinDist() { return ps_r_tree_mesh_dist; }   // see vk_LODManager.h
float g_TreeLodRref = 0.f;
bool TreeLodSizeOn() { return ps_r_tree_lod_size != 0 && g_TreeLodRref > 0.f; }
float TreeLodBand() { return TreeLodSizeOn() ? _max(ps_r_tree_lod_band, 1.f) : 1.f; }
float TreeLodKRaw(float groupR)
{
    if (g_TreeLodRref <= 0.f) return 1.f;
    float k = _sqrt(_max(groupR, 0.f) / g_TreeLodRref);
    clamp(k, kTreeLodKMin, kTreeLodKMax);
    return floorf(k * 64.f + 0.5f) / 64.f;   // lod_cull.comp: floor(k * 64.0 + 0.5) / 64.0
}

// Draw push block — shared by the CPU and GPU imposter pipelines (128 B, the
// guaranteed push-constant minimum; layout mirrors lod_imposter*.vert/frag PC).
struct LodPush
{
    Fmatrix  mvp;
    Fvector4 tint;
    Fvector4 fog_color;
    Fvector4 fog_params;
    Fvector4 eye_pos;
};

// Env tint for the pre-lit imposter atlas: clamp(hemi + sun*0.5, 0, 1). ≈1 at
// midday (atlas unchanged), warm & dim at sunset, dark at night — tracks the
// near trees without knowing the atlas bake's weather. Neutral fallback if env down.
static void FillLodPush(LodPush& lp, const VK::FrameContext& ctx)
{
    lp.mvp = *ctx.viewProj;
    lp.tint.set(0.9f, 0.9f, 0.9f, 0.0f);
    lp.fog_params.set(0.f, 1e6f, 1e6f, 0.f);   // no fog until env is up
    if (g_pGamePersistent) {
        if (auto* E = ES().CurrentEnv) {
            lp.tint.x = clampr(E->hemi_color.x + E->sun_color.x * 0.5f, 0.0f, 1.0f);
            lp.tint.y = clampr(E->hemi_color.y + E->sun_color.y * 0.5f, 0.0f, 1.0f);
            lp.tint.z = clampr(E->hemi_color.z + E->sun_color.z * 0.5f, 0.0f, 1.0f);
            // Distance fog (R4) — same params the world shaders use (vk_env_light):
            // scaled by r_fog_dist, 0 = off. The raw weather range fogged the
            // billboards alone wherever the world had no fog (Switch preset r_fog_dist 0).
            const float fogScale = ps_r_fog_dist;
            const float fn = E->fog_near * fogScale, ff = E->fog_far * fogScale;
            const float r  = (fogScale > 0.f && ff > fn + 1e-3f) ? 1.f / (ff - fn) : 0.f;
            if (r > 0.f) lp.fog_params.set(-fn * r, fn, ff, r);   // else keep the "no fog" defaults above
            lp.fog_color.set(E->fog_color.x, E->fog_color.y, E->fog_color.z, 0.f);
        }
    }
    lp.eye_pos.set(RV().vCameraPosition.x, RV().vCameraPosition.y, RV().vCameraPosition.z, VK::ImposterMinDist());
    // nfs23: the billboard fades in across the R1 band [D_A, D_A*band], D_A = base * k(group sphere)
    lp.tint.w      = VK::TreeLodSizeOn() ? VK::TreeLodBand() : 0.f;
    lp.fog_color.w = VK::TreeLodSizeOn() ? VK::g_TreeLodRref : 0.f;
}

// Cull-dispatch push (208 B) — must match lod_cull.comp PC.
struct LodCullPush
{
    Fmatrix  viewProj;
    Fvector4 planes[6];
    Fvector4 cameraPos;   // xyz eye, w = kImposterMinDist
    Fvector4 hzbParams;   // x = focal scale 1/tan(fovY/2)
    u32 total; u32 hzbOn; u32 _p0; u32 _p1;
};
static_assert(sizeof(LodCullPush) == 208, "LodCullPush must match lod_cull.comp");

CLODManager::CLODManager() {}
CLODManager::~CLODManager() { Destroy(); }

bool CLODManager::IsReady() const
{
    return m_bBuilt && m_Pipeline != VK_NULL_HANDLE && m_DescSet != VK_NULL_HANDLE && !m_Lods.empty();
}

// ============================================================================
void CLODManager::Build()
{
    if (m_bBuilt) return;
    m_bBuilt = true;

    CTimer _t; _t.Start();
    float msCollect = 0, msVB = 0, msAtlas = 0, msPipe = 0, msGpu = 0;

    // ----- Collect FLODs with valid billboard facets.
    m_Lods.reserve(2048);
    // MT_LOD is set by exactly one class: vkVisual_Create builds a vkFLOD for it
    // and nothing else, so the type tag IS the cast. The dynamic_cast this used
    // to do cost ~0.4 us each over the 92k MT_LOD visuals — half of this pass —
    // and RTTI here is not free insurance either: it was the 18-08 crash site
    // (a corrupted vtable faulted INSIDE _RTDynamicCast). That corruption is
    // fixed, and VisualGuard::Check now watches the vtables between phases.
    for (IRenderVisual* iv : RImplementation.Visuals)
    {
        auto* rv = static_cast<vkRender_Visual*>(iv);
        if (!rv || rv->Type != MT_LOD) continue;
        auto* lod = static_cast<vkFLOD*>(rv);
        if (lod->facetsValid && lod->vis.sphere.R > 0.01f)
            m_Lods.push_back(lod);
    }
    msCollect = _t.GetElapsed_ms_total();

    // nfs12 census: what the FLOD groups hold. R1/R4 (r__dsgraph_build add_leafs_static MT_LOD) draw the children only
    // while ssa > r_ssaLOD_B and the billboard while ssa < r_ssaLOD_A; we drop only TREE meshes past r_tree_mesh_dist,
    // every other child (houses, fences, ...) goes to WorldGPU and is drawn at any range next to its billboard.
    // Answered on Kordon (0 groups with non-tree children); kept as a Switch console diagnostic only.
#ifdef __SWITCH__
    {
        u32 groups = 0, groupsNonTree = 0, kidsTree = 0, kidsNonTree = 0, kidsOther = 0;
        u64 triTree = 0, triNonTree = 0, triAllStatic = 0;
        float rMin = 1e9f, rMax = 0.f; double rSum = 0.0;
        for (IRenderVisual* iv : RImplementation.Visuals)
        {
            auto* rv = static_cast<vkRender_Visual*>(iv);
            if (!rv || rv->Type != MT_LOD) continue;
            ++groups;
            bool nonTree = false;
            for (vkRender_Visual* ch : static_cast<vkFLOD*>(rv)->children)
            {
                if (!ch) continue;
                if (ch->Type == MT_TREE_ST || ch->Type == MT_TREE_PM)
                {
                    ++kidsTree;
                    if (auto* fv = static_cast<vkFVisual*>(ch)) triTree += fv->m_mesh.iCount / 3;
                }
                else if (ch->Type == MT_NORMAL || ch->Type == MT_PROGRESSIVE)
                {
                    ++kidsNonTree; nonTree = true;
                    if (auto* fv = static_cast<vkFVisual*>(ch)) triNonTree += fv->m_mesh.iCount / 3;
                }
                else
                    ++kidsOther;
            }
            if (nonTree)
            {
                ++groupsNonTree;
                const float R = rv->vis.sphere.R;
                rMin = _min(rMin, R); rMax = _max(rMax, R); rSum += R;
            }
        }
        for (vkFVisual* fv : WorldGPU::LeafVisuals())
            if (fv && fv->Type != MT_TREE_ST && fv->Type != MT_TREE_PM) triAllStatic += fv->m_mesh.iCount / 3;
        Msg("[VK LOD] census: %u FLOD groups, %u hold non-tree children | children: tree %u (%llu tris), non-tree %u (%llu tris), other %u"
            " | non-tree group radius min %.1f avg %.1f max %.1f m | all non-tree static leaves %llu tris",
            groups, groupsNonTree, kidsTree, (unsigned long long)triTree, kidsNonTree, (unsigned long long)triNonTree, kidsOther,
            groupsNonTree ? rMin : 0.f, groupsNonTree ? float(rSum / groupsNonTree) : 0.f, rMax, (unsigned long long)triAllStatic);
    }
#endif

    if (m_Lods.empty())
    {
        Msg("[VK LOD] No FLOD imposters on this level");
        return;
    }

    // ----- Triple-buffered dynamic VB: 6 verts (2 tris) per FLOD quad.
    _t.Start();
    m_MaxVerts = (u32)m_Lods.size() * 6;
    const VkDeviceSize vbSize = (VkDeviceSize)m_MaxVerts * sizeof(LodImposterVertex);
    for (u32 f = 0; f < LOD_FRAMES; ++f)
    {
        m_DynVB[f] = xr_new<CVulkanBuffer>();
        m_DynVB[f]->Create(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        m_DynVB[f]->Map();   // persistent map — VERTEX buffers aren't auto-mapped (only UBO/SSBO are)
    }
    msVB = _t.GetElapsed_ms_total();

    _t.Start(); CreateAtlasDescriptor(); msAtlas = _t.GetElapsed_ms_total();
    _t.Start(); CreatePipeline();        msPipe  = _t.GetElapsed_ms_total();
    _t.Start(); BuildGpuPath();          msGpu   = _t.GetElapsed_ms_total();

    Msg("[VK LOD] Built: %u FLOD imposters (VB %u KB ×%u)",
        (u32)m_Lods.size(), (u32)(vbSize / 1024), LOD_FRAMES);
    Msg("[load step]   LODs::Build: collect %.0f (%u of %u visuals) | dynVB %.0f | atlas %.0f | pipeline %.0f | gpuPath %.0f ms",
        msCollect, (u32)m_Lods.size(), (u32)RImplementation.Visuals.size(), msVB, msAtlas, msPipe, msGpu);
}

// ============================================================================
// Atlas descriptor — one COMBINED_IMAGE_SAMPLER pointing at level_lods.
// ============================================================================
void CLODManager::CreateAtlasDescriptor()
{
    // level_lods.dds lives in the level's own dir ($level$), packed in the level
    // .db — WorldMaterialCache only searches $game_textures$, so load it directly.
    VkImageView view = VK_NULL_HANDLE;
    {
        string_path full;
        FS.update_path(full, "$level$", "level_lods.dds");
        if (!FS.exist(full))
            FS.update_path(full, "$game_textures$", "level_lods.dds");

        m_AtlasTex = xr_new<CVulkanTexture>();
        // Imposter atlas holds baked albedo of distant statics — Colour, so the far
        // LODs shade in the same space as the near geometry they hand over to.
        if (FS.exist(full) && m_AtlasTex->LoadDDS(full, /*applyBCSwizzle*/ false,
                                                  TexStreamClass::UI, TexColorSpace::Color))
        {
            view = m_AtlasTex->GetView();
        }
        else
        {
            xr_delete(m_AtlasTex);
            view = WorldMaterialCache::GetDefault()->view;   // white fallback
        }
    }

    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   // atlas — clamp
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod       = VK_LOD_CLAMP_NONE;
    vkCreateSampler(VulkanHW.m_Device, &sci, nullptr, &m_Sampler);

    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, 1,
                                m_DescLayout, m_DescPool, &m_DescSet,
                                VK_SHADER_STAGE_FRAGMENT_BIT, "LOD.Atlas"))
        return;

    VK::DescriptorWriter(m_DescSet).ImageSampler(0, view, m_Sampler).Flush();

    Msg("[VK LOD] Atlas 'level_lods' %s", m_AtlasTex ? "loaded from $level$" : "MISSING (white fallback)");
}

// ============================================================================
// Shared graphics-pipeline builder for both imposter draw paths. They differ
// only in the vertex stage: the CPU path streams LodImposterVertex through the
// fixed-function input, the GPU path (vertexInput=false) pulls everything from
// SSBOs by gl_VertexIndex. Fragment stage / state is identical.
static VkPipeline CreateImposterPipeline(const char* vsName, bool vertexInput, VkPipelineLayout layout)
{
    VkShaderModule vs = g_ShaderManager->Load(vsName);
    VkShaderModule fs = g_ShaderManager->Load("lod_imposter.frag.spv");   // nfs23: + crossfade dither (vColor.a < 1)
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        Msg("![VK LOD] %s / lod_imposter.frag.spv load failed", vsName); return VK_NULL_HANDLE;
    }

    VK::GfxPipelineBuilder b(layout);
    b.Vert(vs).Frag(fs)
     .Cull(VK_CULL_MODE_NONE)             // billboards double-sided
     .Depth(true, true)
     .Color(VK::SceneColor::Format())
     .DepthTarget(Swapchain.m_DepthFormat);
    if (vertexInput) {   // else: empty input state — the GPU path pulls verts from SSBOs
        b.Binding(0, (u32)sizeof(LodImposterVertex))
         .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)    // pos
         .Attr(1, 0, VK_FORMAT_R32G32_SFLOAT,    12)   // uv
         .Attr(2, 0, VK_FORMAT_R8G8B8A8_UNORM,   20);  // color
    }
    return b.Build("LOD %s", vsName);
}

// ============================================================================
void CLODManager::CreatePipeline()
{
    if (!g_ShaderManager) { Msg("![VK LOD] g_ShaderManager null"); return; }

    // Push: mat4 viewProj + vec4 tint + vec4 fog_color + vec4 fog_params +
    // vec4 eye_pos = 128 bytes (the guaranteed push-constant minimum).
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0; pcr.size = sizeof(LodPush);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    // 10-10: set 2 = the env set (L + the sky cubes) for the shared FS's fog into the sky; set 1 is the GPU path's vertex
    // buffers, unused by this VS -- the atlas layout stands in so the env set keeps the same index in both layouts.
    VkDescriptorSetLayout cpuSets[3] = { m_DescLayout, m_DescLayout, VK::EnvLight::GetSetLayout() };
    plci.setLayoutCount = 3; plci.pSetLayouts = cpuSets;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &m_PipelineLayout);

    m_Pipeline = CreateImposterPipeline("lod_imposter.vert.spv", true, m_PipelineLayout);
    if (m_Pipeline != VK_NULL_HANDLE)
        Msg("[VK LOD] Pipeline OK");
}

// ============================================================================
// GPU path (r_lods_gpu): static facet SSBO + cull compute + pulling pipeline.
// All buffers are DEVICE_LOCAL (gpuOnly — see the vk_buffer BAR lesson); the
// only CPU work left per frame is a 16 B vkCmdUpdateBuffer and one dispatch.
// ============================================================================
void CLODManager::BuildGpuPath()
{
    if (!g_ShaderManager || m_DescLayout == VK_NULL_HANDLE) return;
    VkDevice dev = VulkanHW.m_Device;
    const u32 N = (u32)m_Lods.size();

    // ----- Pack the static facet data (must match LodEntry in lod_cull.comp).
    struct GpuLodFacet { Fvector4 n; Fvector4 c[4]; Fvector4 vs; };
    struct GpuLodEntry { Fvector4 sphere; GpuLodFacet f[8]; };
    static_assert(sizeof(GpuLodEntry) == 784, "GpuLodEntry must match lod_cull.comp (std430)");

    xr_vector<GpuLodEntry> data(N);
    for (u32 i = 0; i < N; ++i)
    {
        const vkFLOD* lod = m_Lods[i];
        GpuLodEntry& e = data[i];
        e.sphere.set(lod->vis.sphere.P.x, lod->vis.sphere.P.y, lod->vis.sphere.P.z, lod->vis.sphere.R);
        for (u32 s = 0; s < 8; ++s)
        {
            const vkFLOD::LodFacet& F = lod->facets[s];
            e.f[s].n.set(F.N.x, F.N.y, F.N.z, 0.f);
            for (u32 k = 0; k < 4; ++k)
                e.f[s].c[k].set(F.v[k].v.x, F.v[k].v.y, F.v[k].v.z, F.v[k].t.x);
            e.f[s].vs.set(F.v[0].t.y, F.v[1].t.y, F.v[2].t.y, F.v[3].t.y);
        }
    }

    m_GpuLods = xr_new<CVulkanBuffer>();
    m_GpuLods->Create((VkDeviceSize)N * sizeof(GpuLodEntry),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_GpuLods->Upload(data.data(), (VkDeviceSize)N * sizeof(GpuLodEntry));

    m_GpuInsts = xr_new<CVulkanBuffer>();
    m_GpuInsts->Create((VkDeviceSize)N * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    m_GpuIndirect = xr_new<CVulkanBuffer>();
    m_GpuIndirect->Create(sizeof(VkDrawIndirectCommand),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    // ----- Descriptor layouts: cull (3 SSBO + HZB sampler), pulling VS (2 SSBO).
    {
        constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        m_CullDescLayout = VK::MakeSetLayout({ kSSBO, kSSBO, kSSBO,
                                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },   // 3 = HZB
                                             VK_SHADER_STAGE_COMPUTE_BIT, "LOD.Cull");
        m_VtxDescLayout  = VK::MakeSetLayout({ kSSBO, kSSBO }, VK_SHADER_STAGE_VERTEX_BIT, "LOD.Vtx");
        if (!m_CullDescLayout || !m_VtxDescLayout) return;
    }

    // ----- Pool + sets. Cull sets are per-frame-in-flight so the HZB binding can
    // be rewritten each frame without touching a set the GPU still reads.
    {
        VkDescriptorPoolSize ps[2] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         3 * LOD_FRAMES + 2 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, LOD_FRAMES },
        };
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = LOD_FRAMES + 1; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        vkCreateDescriptorPool(dev, &pci, nullptr, &m_GpuDescPool);

        if (!VK::AllocSets(m_GpuDescPool, m_CullDescLayout, LOD_FRAMES, m_CullSets, "LOD.Cull")) return;
        if (!VK::AllocSets(m_GpuDescPool, m_VtxDescLayout,  1,          &m_VtxSet,  "LOD.Vtx"))  return;

        // Static buffer bindings (b0..b2 cull, b0..b1 vtx). HZB (b3) is per frame.
        for (u32 f = 0; f < LOD_FRAMES; ++f)
            VK::DescriptorWriter(m_CullSets[f])
                .StorageBuffer(0, m_GpuLods->GetHandle())
                .StorageBuffer(1, m_GpuInsts->GetHandle())
                .StorageBuffer(2, m_GpuIndirect->GetHandle())
                .Flush();
        VK::DescriptorWriter(m_VtxSet)
            .StorageBuffer(0, m_GpuLods->GetHandle())
            .StorageBuffer(1, m_GpuInsts->GetHandle())
            .Flush();
    }

    // ----- Pipelines: cull compute + pulling graphics (set 0 = atlas, set 1 = geo).
    VkShaderModule cs = g_ShaderManager->Load("lod_cull.comp.spv");   // nfs21: + size-scaled handover (hzbParams.y)
    if (cs != VK_NULL_HANDLE) {
        m_CullPipelineLayout = MakePipelineLayout({ m_CullDescLayout }, sizeof(LodCullPush));
        m_CullPipeline = CreateComputePipeline(cs, m_CullPipelineLayout, "lod_cull");
    }

    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0; pcr.size = sizeof(LodPush);
        VkDescriptorSetLayout sets[3] = { m_DescLayout, m_VtxDescLayout, VK::EnvLight::GetSetLayout() };   // 10-10: + env (set 2)
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 3; plci.pSetLayouts = sets;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(dev, &plci, nullptr, &m_PipelineLayoutGPU);
        m_PipelineGPU = CreateImposterPipeline("lod_imposter_gpu.vert.spv", false, m_PipelineLayoutGPU);   // nfs23: + crossfade
    }

    const bool ok = m_CullPipeline != VK_NULL_HANDLE && m_PipelineGPU != VK_NULL_HANDLE;
    Msg("[VK LOD] GPU path %s: %u entries, SSBO %u KB", ok ? "ready" : "FAILED (CPU fallback)",
        N, (u32)((N * sizeof(GpuLodEntry)) / 1024));
}

// ============================================================================
void CLODManager::Render(VK::FrameContext& ctx)
{
    if (!IsReady() || ctx.cmd == VK_NULL_HANDLE || ctx.viewProj == nullptr) return;

    if (ps_r_lods_gpu && m_CullPipeline != VK_NULL_HANDLE && m_PipelineGPU != VK_NULL_HANDLE)
    {
        RenderGpu(ctx);
        return;
    }

    const u32 frame = CommandManager.GetCurrentFrame() % LOD_FRAMES;
    CVulkanBuffer* vb = m_DynVB[frame];
    if (!vb || !vb->IsMapped()) return;

    const Fvector eye = RV().vCameraPosition;
    auto* out = (LodImposterVertex*)vb->m_Mapped;
    u32 nVerts = 0;

    // Build best-facet quads for distant FLODs.
    for (vkFLOD* lod : m_Lods)
    {
        const Fvector& C = lod->vis.sphere.P;
        const float R = lod->vis.sphere.R;
        Fvector d; d.sub(C, eye);
        const float dist = d.magnitude();
        // nfs21: the group's handover scales with its size (same k as its trees' mesh cut in tree_cull)
        if (dist < ImposterMinDist() * (TreeLodSizeOn() ? TreeLodKRaw(R) : 1.f)) continue;     // close → full mesh handles it

        Fvector Ldir = d; Ldir.div(dist > 0.001f ? dist : 1.0f);   // camera→object
        Fvector shift; shift.mul(Ldir, -0.5f * R);                 // pull toward camera
        // nfs23: R1 crossfade, same as lod_imposter_gpu_v2.vert
        u32 fadeA = 255u;
        if (TreeLodSizeOn() && TreeLodBand() > 1.0001f) {
            const float dA = ImposterMinDist() * TreeLodKRaw(R), dB = dA * TreeLodBand();
            fadeA = (u32)iFloor(clampr((dist - dA) / _max(dB - dA, 1e-3f), 0.f, 1.f) * 255.f + 0.5f);
        }

        // Pick the facet whose normal best faces the camera.
        u32 best = 0; float bestDot = -1e9f;
        for (u32 s = 0; s < 8; ++s) {
            const float dt = Ldir.dotproduct(lod->facets[s].N);
            if (dt > bestDot) { bestDot = dt; best = s; }
        }
        const vkFLOD::LodFacet& F = lod->facets[best];

        // Emit quad v0,v1,v2 + v0,v2,v3 (cull NONE → winding irrelevant).
        const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; ++k) {
            const vkFLOD::LodVertex& sv = F.v[tri[k]];
            LodImposterVertex& dv = out[nVerts++];
            dv.pos.add(sv.v, shift);
            dv.uv = sv.t;
            dv.color = 0x00FFFFFFu | (fadeA << 24);   // nfs23: alpha = crossfade (atlas carries the colour)
        }
    }

    if (nVerts == 0) return;
    vb->Flush();

    // ----- Draw into the live color+depth pass (single-layout convention). ----
    VK::BeginOverlayRendering(ctx.cmd, ctx);   // shared overlay begin — see vk_pass_context.h

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout,
                            0, 1, &m_DescSet, 0, nullptr);
    if (VkDescriptorSet envSet = VK::EnvLight::GetCurrentSet())   // 10-10: set 2, the fog into the sky
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 2, 1, &envSet, 0, nullptr);
    LodPush lp{};
    FillLodPush(lp, ctx);
    vkCmdPushConstants(ctx.cmd, m_PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(lp), &lp);
    VkBuffer h = vb->GetHandle(); VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &h, &off);
    vkCmdDraw(ctx.cmd, nVerts, 1, 0, 0);

    vkCmdEndRendering(ctx.cmd);

    static bool s_diag = false;
    if (!s_diag) { Msg("[VK LOD] First Render: %u imposter verts (%u quads)", nVerts, nVerts / 6); s_diag = true; }
}

// ============================================================================
// GPU path — the whole per-frame CPU cost is: rewrite one HZB descriptor, a
// 16 B indirect reset, one dispatch and one indirect draw. The FLOD walk,
// facet picks and vertex building all moved to lod_cull.comp / the pulling VS.
// ============================================================================
void CLODManager::RenderGpu(VK::FrameContext& ctx)
{
    VkCommandBuffer cmd = ctx.cmd;
    const u32 N = (u32)m_Lods.size();

    // Hi-Z pyramid: Pass_World (r_hzb_cull) or grass already built it this frame;
    // the guarded call covers configs where neither ran (build-once-per-frame
    // stamp makes it a no-op otherwise). Runs outside a render pass — we are too.
    bool hzbOn = false;
    // No occlusion consumer on (lean/Switch preset) -> no Hi-Z pyramid at all; the LOD cull runs without it.
    if ((ps_r_hzb_cull || ps_r_tree_hzb || ps_r_hzb_prepass) && RImplementation.Details && RImplementation.Details->HZBReady())
    {
        RImplementation.Details->BuildHZBForFrame(ctx);
        hzbOn = true;
    }

    // Rewrite the HZB binding of THIS frame slot's cull set (its fence passed, the
    // GPU no longer reads it). Without a pyramid bind the atlas as an inert dummy
    // (never sampled — the shader gates on hzbOn) so validation sees a live view.
    const u32 frame = CommandManager.GetCurrentFrame() % LOD_FRAMES;
    {
        VkDescriptorImageInfo ii{};
        if (hzbOn) {
            ii.sampler     = RImplementation.Details->HZBSampler();
            ii.imageView   = RImplementation.Details->HZBView();
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;   // HZB lives in GENERAL
        } else {
            ii.sampler     = m_Sampler;
            ii.imageView   = m_AtlasTex ? m_AtlasTex->GetView() : WorldMaterialCache::GetDefault()->view;
            ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VK::DescriptorWriter(m_CullSets[frame])
            .Image(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ii)
            .Flush();
    }

    const int zc = Prof::ZoneBegin(cmd, "LODs/cull");

    // Reset the indirect draw {vertexCount=0, instanceCount=1}. The barrier also
    // orders the PREVIOUS frame's indirect/VS reads of these single-buffered
    // streams before this frame's transfer + compute writes (WAR, execution-only).
    const VkDrawIndirectCommand rst{ 0, 1, 0, 0 };
    vkCmdUpdateBuffer(cmd, m_GpuIndirect->GetHandle(), 0, sizeof(rst), &rst);
    {
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CullPipelineLayout,
                            0, 1, &m_CullSets[frame], 0, nullptr);

    LodCullPush cp{};
    cp.viewProj = *ctx.viewProj;
    VK::ExtractFrustumPlanes(*ctx.viewProj, cp.planes);
    const Fvector eye = RV().vCameraPosition;
    cp.cameraPos.set(eye.x, eye.y, eye.z, ImposterMinDist());
    // Focal scale for the HZB footprint (same formula as WorldGPU::LodParams .w).
    cp.hzbParams.set(1.f / _max(0.05f, tanf(deg2rad(RV().fFOV) * 0.5f)), TreeLodSizeOn() ? g_TreeLodRref : 0.f,
                     kTreeLodKMin, kTreeLodKMax);   // nfs21: .y = R_mean (0 = off), .zw = k clamp
    cp.total = N;
    cp.hzbOn = hzbOn ? 1u : 0u;
    vkCmdPushConstants(cmd, m_CullPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cp), &cp);
    vkCmdDispatch(cmd, (N + 255) / 256, 1, 1);

    // Cull results → indirect arg + VS-pulled instance stream.
    {
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    Prof::ZoneEnd(cmd, zc);

    // ----- Indirect draw into the live color+depth pass (same as the CPU path).
    VK::BeginOverlayRendering(cmd, ctx);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineGPU);
    VkDescriptorSet sets[2] = { m_DescSet, m_VtxSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayoutGPU,
                            0, 2, sets, 0, nullptr);
    if (VkDescriptorSet envSet = VK::EnvLight::GetCurrentSet())   // 10-10: set 2, the fog into the sky
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayoutGPU, 2, 1, &envSet, 0, nullptr);
    LodPush lp{};
    FillLodPush(lp, ctx);
    vkCmdPushConstants(cmd, m_PipelineLayoutGPU, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(lp), &lp);
    vkCmdDrawIndirect(cmd, m_GpuIndirect->GetHandle(), 0, 1, sizeof(VkDrawIndirectCommand));

    vkCmdEndRendering(cmd);

    static bool s_diagGpu = false;
    if (!s_diagGpu) { Msg("[VK LOD] GPU path first frame: %u entries, hzb=%u", N, cp.hzbOn); s_diagGpu = true; }
}

// ============================================================================
void CLODManager::Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) { m_bBuilt = false; m_Lods.clear(); return; }
    VkDevice dev = VulkanHW.m_Device;

    if (m_Pipeline)       { vkDestroyPipeline(dev, m_Pipeline, nullptr); m_Pipeline = VK_NULL_HANDLE; }
    if (m_PipelineLayout) { vkDestroyPipelineLayout(dev, m_PipelineLayout, nullptr); m_PipelineLayout = VK_NULL_HANDLE; }

    // GPU path (r_lods_gpu)
    if (m_CullPipeline)       { vkDestroyPipeline(dev, m_CullPipeline, nullptr); m_CullPipeline = VK_NULL_HANDLE; }
    if (m_CullPipelineLayout) { vkDestroyPipelineLayout(dev, m_CullPipelineLayout, nullptr); m_CullPipelineLayout = VK_NULL_HANDLE; }
    if (m_PipelineGPU)        { vkDestroyPipeline(dev, m_PipelineGPU, nullptr); m_PipelineGPU = VK_NULL_HANDLE; }
    if (m_PipelineLayoutGPU)  { vkDestroyPipelineLayout(dev, m_PipelineLayoutGPU, nullptr); m_PipelineLayoutGPU = VK_NULL_HANDLE; }
    if (m_GpuDescPool)        { vkDestroyDescriptorPool(dev, m_GpuDescPool, nullptr); m_GpuDescPool = VK_NULL_HANDLE;
                                for (u32 f = 0; f < LOD_FRAMES; ++f) m_CullSets[f] = VK_NULL_HANDLE;
                                m_VtxSet = VK_NULL_HANDLE; }
    if (m_CullDescLayout)     { vkDestroyDescriptorSetLayout(dev, m_CullDescLayout, nullptr); m_CullDescLayout = VK_NULL_HANDLE; }
    if (m_VtxDescLayout)      { vkDestroyDescriptorSetLayout(dev, m_VtxDescLayout, nullptr); m_VtxDescLayout = VK_NULL_HANDLE; }
    if (m_GpuLods)            { m_GpuLods->Destroy(); xr_delete(m_GpuLods); }
    if (m_GpuInsts)           { m_GpuInsts->Destroy(); xr_delete(m_GpuInsts); }
    if (m_GpuIndirect)        { m_GpuIndirect->Destroy(); xr_delete(m_GpuIndirect); }
    if (m_DescPool)       { vkDestroyDescriptorPool(dev, m_DescPool, nullptr); m_DescPool = VK_NULL_HANDLE; m_DescSet = VK_NULL_HANDLE; }
    if (m_DescLayout)     { vkDestroyDescriptorSetLayout(dev, m_DescLayout, nullptr); m_DescLayout = VK_NULL_HANDLE; }
    if (m_Sampler)        { vkDestroySampler(dev, m_Sampler, nullptr); m_Sampler = VK_NULL_HANDLE; }
    if (m_AtlasTex)       { m_AtlasTex->Destroy(); xr_delete(m_AtlasTex); }
    for (u32 f = 0; f < LOD_FRAMES; ++f)
        if (m_DynVB[f]) { m_DynVB[f]->Destroy(); xr_delete(m_DynVB[f]); }

    m_Lods.clear();
    m_MaxVerts = 0;
    m_bBuilt = false;
}

}  // namespace VK

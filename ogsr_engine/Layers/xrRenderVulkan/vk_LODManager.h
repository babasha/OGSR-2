// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — LOD imposter manager (MT_LOD / FLOD billboards).
//
// Collects every vkFLOD at level load and, each frame, draws the single
// best-facing billboard facet of each (distant) FLOD into the level_lods atlas.
// This restores the far-forest silhouette density R4 gets from
// r_dsgraph_render_lods(). v1: nearest-facet only (no 2-facet cross-fade),
// distance-gated so up-close trees still use their full mesh.

#pragma once
#include "vk_core.h"

class vkFLOD;

namespace VK
{
class CVulkanBuffer;
class CVulkanTexture;
struct FrameContext;

// Range at which an FLOD container starts drawing its billboard instead of
// relying on the full mesh. Shared, NOT file-local, because CTreeManager's
// r_tree_dist cut keys off it: dropping a tree's mesh BEFORE its imposter kicks
// in makes the tree vanish, so TreeFlodCutDist() floors itself at this value.
// Far enough that the flatness reads as distant haze, not a low-poly pop-in.
// Where the full tree mesh hands over to the FLOD billboard. A RUNTIME value now (console r_tree_mesh_dist) so the
// handover can be A/B'd on the console; the defaults are the old constants: 250 m on PC; the Switch preset sets 40 m (was 80 m in v71).
float ImposterMinDist();
// nfs21: R1-style size-scaled handover (see nfs21 in vk_TreeManager.cpp). k(group) = clamp(sqrt(R/R_mean), kMin, kMax),
// quantised to 1/64 so tree_cull (k stored per tree) and lod_cull (k recomputed from the group sphere) agree exactly.
constexpr float kTreeLodKMin = 0.6f, kTreeLodKMax = 2.0f;
extern float g_TreeLodRref;          // mean FLOD group radius, set by CTreeManager::Build (0 = no size LOD)
bool  TreeLodSizeOn();               // r_tree_lod_size && a mean radius exists
float TreeLodKRaw(float groupR);     // the quantised k, regardless of the cvar
float TreeLodBand();                 // nfs23: r_tree_lod_band (>= 1) when the size LOD is on, else 1

// Per-vertex imposter quad data (host-built each frame). 24 B.
struct LodImposterVertex
{
    Fvector  pos;     // world-space position (facet vertex + camera-ward shift)
    Fvector2 uv;      // UV into level_lods atlas
    u32      color;   // RGBA8 tint (hemi lighting; white for v1)
};
static_assert(sizeof(LodImposterVertex) == 24, "LodImposterVertex must be 24 B");

inline constexpr u32 LOD_FRAMES = VK_FRAMES_IN_FLIGHT;   // == CommandManager FRAMES_IN_FLIGHT

class CLODManager
{
public:
    CLODManager();
    ~CLODManager();

    void Build();                       // collect FLODs + atlas + pipeline
    void Destroy();
    void Render(VK::FrameContext& ctx); // per-frame best-facet billboard draw
    bool IsReady() const;

private:
    void CreatePipeline();
    void CreateAtlasDescriptor();
    void BuildGpuPath();                 // r_lods_gpu: facet SSBO + cull pipeline + pulling pipeline
    void RenderGpu(VK::FrameContext& ctx);

private:
    bool m_bBuilt = false;
    xr_vector<vkFLOD*> m_Lods;

    // Triple-buffered host-visible vertex stream (6 verts per FLOD quad).
    CVulkanBuffer* m_DynVB[LOD_FRAMES] = {};
    u32            m_MaxVerts = 0;

    // ----- GPU path (r_lods_gpu): compute cull + vertex pulling ------------
    // Static per-FLOD facet SSBO (784 B each), a visible-instance stream and a
    // single VkDrawIndirectCommand — all GPU-only; the per-frame CPU cost is one
    // vkCmdUpdateBuffer + one dispatch. CPU path above stays as instant A/B.
    CVulkanBuffer* m_GpuLods     = nullptr;   // LodEntry[] (matches lod_cull.comp)
    CVulkanBuffer* m_GpuInsts    = nullptr;   // u32 per visible quad: (lod<<3)|facet
    CVulkanBuffer* m_GpuIndirect = nullptr;   // VkDrawIndirectCommand, reset each frame

    VkDescriptorSetLayout m_CullDescLayout = VK_NULL_HANDLE;
    VkDescriptorSet       m_CullSets[LOD_FRAMES] = {};   // per-frame: HZB binding rewritten
    VkPipelineLayout      m_CullPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_CullPipeline       = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_VtxDescLayout = VK_NULL_HANDLE;   // set 1 of the pulling draw
    VkDescriptorSet       m_VtxSet        = VK_NULL_HANDLE;
    VkDescriptorPool      m_GpuDescPool   = VK_NULL_HANDLE;
    VkPipelineLayout      m_PipelineLayoutGPU = VK_NULL_HANDLE;
    VkPipeline            m_PipelineGPU       = VK_NULL_HANDLE;

    // Atlas (level_lods) descriptor. The atlas lives in $level$, not
    // $game_textures$, so it's loaded directly (not via WorldMaterialCache).
    CVulkanTexture*       m_AtlasTex   = nullptr;
    VkDescriptorSetLayout m_DescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_DescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_DescSet    = VK_NULL_HANDLE;
    VkSampler             m_Sampler    = VK_NULL_HANDLE;

    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_Pipeline       = VK_NULL_HANDLE;
};

}  // namespace VK

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — RT world built from the RENDER geometry. See vk_rt_world.h.
#include "stdafx.h"
#include "vk_rt_world.h"
#include "vk_world_gpu.h"
#include "vk_world_material.h"
#include "vk_TreeManager.h"     // the forest: instanced geometry no other RT source has
#include "CRender_Vulkan.h"     // RImplementation.Trees
#include "vk_cluster_stream.h"
#include "vk_buffer.h"
#include "vk_barriers.h"
#include "vk_descriptors.h"
#include "vk_compute_util.h"
#include "vk_profiler.h"
#include "vk_command_buffer.h"
#include "vk_texture.h"
#include "HW_Vulkan.h"

#include <unordered_map>

extern float ps_r_rt_lod;      // RT LOD error scale — the cut's "how fine, how far"
extern float ps_r_rt_tree_range;   // foliage-only bubble half-extent (m); 0 = the full bubble

namespace VK { namespace RTWorld {

namespace {

constexpr u32 kSets     = CVulkanCommandManager::FRAMES_IN_FLIGHT + 1;
// Descriptor array size for the diffuse table. Fixed for the process, NOT per
// level: the consumers bake this set layout into their pipeline layouts at Init,
// and a per-level count would make every one of them incompatible on the second
// level. Materials past the cap fall back to the constant grey.
// ⚠ Must equal RT_TEX_COUNT in shaders/rt_shade.glsl. pripyat_full alone uses
// 1936 world materials, so 2048 was one busy level away from silently dropping
// surfaces to grey.
constexpr u32 kMaxMaterials = 4096;

// ---------------------------------------------------------------------------
//  Per-group table — the bridge from "which entry" to "which bytes".
//  Mirrors `RtGroup` in rt_gather.comp.glsl (std430, 32 B). The two addresses are
//  stored as PAIRS OF u32 rather than u64 so the shader can build its
//  buffer_reference from a uvec2 without the int64 arithmetic extension.
// ---------------------------------------------------------------------------
struct RtGroup {
    u32   vbAddr[2];
    u32   ibAddr[2];
    u32   stride;      // vertex stride in BYTES (position is always at offset 0)
    u32   tcOffset;    // byte offset of TEXCOORD0 (SHORT2) in the vertex
    u32   flags;       // bit0 = 32-bit index pool, bit1 = alpha-tested material
    u32   mat;         // RT material slot (index into the diffuse array + albedo table)
    float uvScale;     // SHORT2 -> UV quantisation. Level statics use 1/1024 and
                       // trees 1/2048, so the gather applies it and writes the
                       // attribute as a half2 — the hit shader then needs to know
                       // nothing about where the triangle came from.
    u32   _pad[3];
};
static_assert(sizeof(RtGroup) == 48, "RtGroup must match the std430 layout in rt_gather.comp.glsl");

constexpr u32 kGrpIB32 = 1u << 0;
constexpr u32 kGrpAT   = 1u << 1;

struct GatherPush { u32 jobCount; u32 triCap; u32 flags; u32 _pad; };
constexpr u32 kPushHaveBits = 1u << 0;   // the residency bits bound are the real ones

bool s_built  = false;
bool s_failed = false;
u32  s_groupCount = 0;
u32  s_matCount   = 0;

// Tree groups live in the same table, after the world's. A span says which run
// of TREE INSTANCES (indices into CTreeManager's meta/transform SSBOs) draws with
// which group — the tree metas carry no group field of their own.
struct TreeSpan { u32 meshOffset, meshCount, group, _pad; };
xr_vector<TreeSpan> s_treeSpan;
u32 s_treeGroupBase  = 0;
u32 s_treeGroupCount = 0;

CVulkanBuffer s_groupBuf;    // RtGroup[] (host-visible, written once per level)
CVulkanBuffer s_albedoBuf;   // vec4[] material slot -> (fallback rgb, texture slot)
CVulkanBuffer s_jobBuf;      // Job[] (host-visible, rewritten per rebuild)
CVulkanBuffer s_zeroBuf;     // 2 u32 of zeroes — the pageSlotBase stand-in when
                             // cluster streaming is inactive (identity page only)

// The diffuse array, one entry per RT material slot. `mat` is kept so a
// published view can be compared against the live one — texture streaming swaps
// the image behind a material without telling anybody.
struct MatSlot { const WorldMaterial* mat; VkImageView view; VkSampler sampler; };
xr_vector<MatSlot> s_slots;

VkDescriptorSetLayout s_texLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_texPool   = VK_NULL_HANDLE;
VkDescriptorSet       s_texSets[kSets] = {};
u32                   s_texGen[kSets]  = {};   // which publication each set holds
u32                   s_texCur  = 0;
u32                   s_texGenCur = 1;         // bumped when a view moves under us

VkDescriptorSetLayout s_gatherLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_gatherPool   = VK_NULL_HANDLE;
VkDescriptorSet       s_gatherSets[kSets] = {};
VkPipelineLayout      s_gatherPipeLayout = VK_NULL_HANDLE;
VkPipeline            s_gatherPipe = VK_NULL_HANDLE;
u32                   s_gatherCur  = 0;

// The tree gather is a second pipeline rather than a branch in the first: its
// input is a different meta layout and every vertex needs an instance transform,
// and neither belongs in the inner loop of the world's.
VkDescriptorSetLayout s_treeLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_treePool   = VK_NULL_HANDLE;
VkDescriptorSet       s_treeSets[kSets] = {};
VkPipelineLayout      s_treePipeLayout = VK_NULL_HANDLE;
VkPipeline            s_treePipe   = VK_NULL_HANDLE;
u32                   s_treeCur    = 0;

// One selected STATIC entry: which cull meta to gather, and the first triangle
// of the soup it owns. Mirrors uvec2 in rt_gather.comp.
struct Job { u32 entry, outBase; };
// One tree INSTANCE to gather: which instance, where its triangles land, and the
// group (mesh + material) it belongs to. Mirrors uvec4 in rt_gather_tree.comp.
struct TreeJob { u32 tree, outBase, group, _pad; };
xr_vector<Job>     s_worldJobs;     // opaque first, then the alpha-tested ones
xr_vector<Job>     s_worldJobsAT;   // staging for the second pass (see Select)
xr_vector<TreeJob> s_treeJobs;
CVulkanBuffer      s_treeJobBuf;
u32                s_opaqueTris = 0;   // the split point in the soup

VkDeviceAddress BufAddr(VkBuffer b)
{
    if (b == VK_NULL_HANDLE) return 0;
    VkBufferDeviceAddressInfo i = {};
    i.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    i.buffer = b;
    return vkGetBufferDeviceAddress(VulkanHW.m_Device, &i);
}

void PutAddr(u32* dst, VkDeviceAddress a)
{
    dst[0] = u32(a & 0xFFFFFFFFull);
    dst[1] = u32(a >> 32);
}

bool EnsureHostBuffer(CVulkanBuffer& b, VkDeviceSize need, VkBufferUsageFlags usage)
{
    if (b.IsValid() && b.GetSize() >= need) return true;
    b.Destroy();
    b.Create(need, usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, false);
    return b.IsValid();
}

bool WriteHost(CVulkanBuffer& b, const void* src, size_t bytes)
{
    void* dst = b.Map();
    if (!dst) return false;
    memcpy(dst, src, bytes);
    b.Flush();
    b.Unmap();
    return true;
}

// ---------------------------------------------------------------------------
//  The diffuse descriptor array.
//
//  Every slot is written at creation (with the cache's 1x1 white) and rewritten
//  whenever a material's view moves — which happens on its own schedule, because
//  the texture streamer swaps images as mip residency changes. A set that an
//  in-flight frame might still be reading must never be rewritten, hence the ring:
//  FrameTick publishes into the NEXT slot and hands that one out.
// ---------------------------------------------------------------------------
bool CreateTexSet()
{
    if (s_texLayout != VK_NULL_HANDLE) return true;
    // The array index is per-invocation (two rays from the same probe land on
    // different materials), so this needs non-uniform indexing — not merely the
    // dynamically-uniform kind core Vulkan gives for free.
    if (!VulkanHW.m_bBindlessSupported) {
        static bool warned = false;
        if (!warned) { warned = true; Msg("![VK RTWorld] no non-uniform descriptor indexing — RT hit shading unavailable"); }
        return false;
    }

    VkDescriptorSetLayoutBinding b = {};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = kMaxMaterials;
    b.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lci = {};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(VulkanHW.m_Device, &lci, nullptr, &s_texLayout) != VK_SUCCESS) {
        Msg("![VK RTWorld] diffuse array layout failed");
        s_texLayout = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxMaterials * kSets };
    VkDescriptorPoolCreateInfo pci = {};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets       = kSets;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_texPool) != VK_SUCCESS) {
        Msg("![VK RTWorld] diffuse array pool failed");
        return false;
    }

    VkDescriptorSetLayout layouts[kSets];
    for (u32 i = 0; i < kSets; ++i) layouts[i] = s_texLayout;
    VkDescriptorSetAllocateInfo ai = {};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = s_texPool;
    ai.descriptorSetCount = kSets;
    ai.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(VulkanHW.m_Device, &ai, s_texSets) != VK_SUCCESS) {
        Msg("![VK RTWorld] diffuse array sets failed");
        return false;
    }
    for (u32 i = 0; i < kSets; ++i) s_texGen[i] = 0;
    return true;
}

// Publish the current material views into set `slot`. Fills the WHOLE array —
// unused tail included — so no descriptor is ever undefined.
void PublishTexSet(u32 slot)
{
    const WorldMaterial* def = WorldMaterialCache::GetDefault();
    const VkImageView defView = def ? def->view : VK_NULL_HANDLE;
    const VkSampler   defSamp = WorldMaterialCache::GetSampler();
    if (defView == VK_NULL_HANDLE || defSamp == VK_NULL_HANDLE) return;

    xr_vector<VkDescriptorImageInfo> infos(kMaxMaterials);
    for (u32 i = 0; i < kMaxMaterials; ++i) {
        // Re-read the LIVE view here rather than the one recorded at build time:
        // this publication is exactly the point where a streamed swap is picked up.
        if (i < s_slots.size() && s_slots[i].mat) s_slots[i].view = s_slots[i].mat->view;
        const bool have = (i < s_slots.size()) && s_slots[i].view != VK_NULL_HANDLE;
        infos[i].sampler     = have && s_slots[i].sampler ? s_slots[i].sampler : defSamp;
        infos[i].imageView   = have ? s_slots[i].view : defView;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkWriteDescriptorSet w = {};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = s_texSets[slot];
    w.dstBinding      = 0;
    w.dstArrayElement = 0;
    w.descriptorCount = kMaxMaterials;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = infos.data();
    vkUpdateDescriptorSets(VulkanHW.m_Device, 1, &w, 0, nullptr);
    s_texGen[slot] = s_texGenCur;
}

bool CreateGatherPipeline()
{
    if (s_gatherPipe != VK_NULL_HANDLE) return true;

    // 0 metas, 1 page slot bases, 2 stream bits, 3 groups, 4 jobs, 5 out positions,
    // 6 out attributes.
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                kSets, s_gatherLayout, s_gatherPool, s_gatherSets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "RTWorld.Gather"))
        return false;

    s_gatherPipeLayout = VK::MakePipelineLayout({ s_gatherLayout }, sizeof(GatherPush));
    if (!s_gatherPipeLayout) return false;
    s_gatherPipe = VK::CreateComputePipeline("rt_gather.comp.spv", s_gatherPipeLayout, "RTWorld.Gather");
    return s_gatherPipe != VK_NULL_HANDLE;
}

// Best-effort: a level with no trees, or a missing shader, must not take the
// static world's RT down with it.
void CreateTreePipeline()
{
    if (s_treePipe != VK_NULL_HANDLE) return;
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // 0 tree metas, 1 instance transforms, 2 groups, 3 jobs, 4 out positions,
    // 5 out attributes.
    if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                kSets, s_treeLayout, s_treePool, s_treeSets,
                                VK_SHADER_STAGE_COMPUTE_BIT, "RTWorld.TreeGather"))
        return;
    s_treePipeLayout = VK::MakePipelineLayout({ s_treeLayout }, sizeof(GatherPush));
    if (!s_treePipeLayout) return;
    s_treePipe = VK::CreateComputePipeline("rt_gather_tree.comp.spv", s_treePipeLayout, "RTWorld.TreeGather");
    if (s_treePipe == VK_NULL_HANDLE)
        Msg("![VK RTWorld] rt_gather_tree.comp.spv missing — the forest stays out of the BVH");
}

}   // anon namespace

// ---------------------------------------------------------------------------
//  Build — resolve the group table and the material table for this level.
// ---------------------------------------------------------------------------
bool Build()
{
    if (s_built)  return true;
    if (s_failed) return false;
    if (!WorldGPU::Built()) return false;      // level still loading; try again next frame

    const u32 nGroups = WorldGPU::GroupCount();
    if (nGroups == 0) return false;

    if (!CreateTexSet() || !CreateGatherPipeline()) { s_failed = true; return false; }
    CreateTreePipeline();

    xr_vector<RtGroup>  groups(nGroups);
    xr_vector<Fvector4> albedo;
    std::unordered_map<const WorldMaterial*, u32> matSlot;
    s_slots.clear();
    s_treeSpan.clear();

    const WorldMaterial* def = WorldMaterialCache::GetDefault();
    const VkImageView    defView = def ? def->view : VK_NULL_HANDLE;
    const VkSampler      defSamp = WorldMaterialCache::GetSampler();

    u32 noAddr = 0, overflow = 0, textured = 0;
    for (u32 g = 0; g < nGroups; ++g) {
        VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
        u32 stride = 0, entryOffset = 0, entryCount = 0, tcOffset = 24;
        VkIndexType iType = VK_INDEX_TYPE_UINT16;
        bool at = false;
        const WorldMaterial* mat = nullptr;
        RtGroup& r = groups[g];
        memset(&r, 0, sizeof(r));
        if (!WorldGPU::GetGroupShadow(g, vb, ib, stride, iType, entryOffset, entryCount, at, &mat, &tcOffset))
            continue;

        const VkDeviceAddress va = BufAddr(vb), ia = BufAddr(ib);
        if (!va || !ia || stride < 16) { ++noAddr; continue; }   // r.mat stays 0 and the entry is skipped
        PutAddr(r.vbAddr, va);
        PutAddr(r.ibAddr, ia);
        r.stride   = stride;
        r.tcOffset = tcOffset;
        r.uvScale  = 1.0f / 1024.0f;   // level statics' SHORT2 quantisation
        r.flags    = (iType == VK_INDEX_TYPE_UINT32 ? kGrpIB32 : 0u) | (at ? kGrpAT : 0u);

        // Material slot: one per distinct WorldMaterial, shared by every group
        // that draws with it.
        u32 slot = 0;
        auto it = matSlot.find(mat);
        if (it != matSlot.end()) slot = it->second;
        else {
            slot = (u32)s_slots.size();
            if (slot >= kMaxMaterials) { ++overflow; slot = 0; }
            else {
                MatSlot ms{};
                ms.mat     = mat;
                ms.view    = mat ? mat->view : VK_NULL_HANDLE;
                ms.sampler = mat ? mat->sampler : VK_NULL_HANDLE;
                s_slots.push_back(ms);
                // ⚠ TWO MEANINGS, ONE vec4, chosen by w:
                //   w <  0 : no texture — rgb IS the albedo (the whole cform path).
                //   w >= 0 : w is the texture slot, and the rgb is free, so x
                //            carries the material's ALPHA REFERENCE (< 0 = opaque).
                // The alternative was a second table read on every hit for a number
                // only cutouts use.
                const bool haveTex = ms.view != VK_NULL_HANDLE && ms.view != defView;
                const float aref = (mat && mat->alphaRef >= 0.f) ? mat->alphaRef : -1.f;
                albedo.push_back(haveTex ? Fvector4().set(aref, 0.f, 0.f, float(slot))
                                         : Fvector4().set(0.30f, 0.29f, 0.28f, -1.f));
                if (haveTex) ++textured;
                matSlot.insert({ mat, slot });
            }
        }
        r.mat = slot;
    }

    // ---- TREES ------------------------------------------------------------
    // A forest is tens of thousands of INSTANCES of a handful of meshes, drawn
    // by CTreeManager and by nothing else: neither the collision model (crowns
    // have none) nor the static render set contains a single leaf. Their groups
    // are appended after the world's, so one table and one material numbering
    // serve both gathers — only the gather SHADER differs, because a tree's
    // triangles have to be transformed by their instance on the way out.
    s_treeGroupBase  = (u32)groups.size();
    s_treeGroupCount = 0;
    CTreeManager* tm = RImplementation.Trees;
    if (tm && tm->IsReady()) {
        const u32 nTree = tm->RTGroupCount();
        std::unordered_map<VkImageView, u32> viewSlot;
        for (u32 g = 0; g < nTree; ++g) {
            VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
            u32 stride = 0, tcOffset = 24, meshOffset = 0, meshCount = 0;
            VkImageView view = VK_NULL_HANDLE;
            if (!tm->RTGroup(g, vb, ib, stride, tcOffset, meshOffset, meshCount, view)) continue;
            const VkDeviceAddress va = BufAddr(vb), ia = BufAddr(ib);
            if (!va || !ia || stride < 16 || meshCount == 0) { ++noAddr; continue; }

            RtGroup r{};
            PutAddr(r.vbAddr, va);
            PutAddr(r.ibAddr, ia);
            r.stride   = stride;
            r.tcOffset = tcOffset;
            r.uvScale  = 1.0f / 2048.0f;   // FTreeVisual quantisation (32768/16), NOT the statics' 1/1024
            r.flags    = kGrpAT;           // every leaf card is an alpha cutout
            u32 slot = 0;
            auto vit = viewSlot.find(view);
            if (vit != viewSlot.end()) slot = vit->second;
            else {
                slot = (u32)s_slots.size();
                if (slot >= kMaxMaterials) { ++overflow; slot = 0; }
                else {
                    // No WorldMaterial back-pointer on purpose: tree textures are
                    // frozen non-streamable at Build (the texFloor pass), so this
                    // view never moves and must not be re-read from a material.
                    MatSlot ms{}; ms.mat = nullptr; ms.view = view; ms.sampler = defSamp;
                    s_slots.push_back(ms);
                    // 200/255 is the cutoff the tree pipelines use (TreeGfxPush);
                    // the RT copy has to punch the leaves out at the same place or
                    // its crowns are a different shape from the ones on screen.
                    albedo.push_back(view != VK_NULL_HANDLE
                                     ? Fvector4().set(200.f / 255.f, 0.f, 0.f, float(slot))
                                     : Fvector4().set(0.30f, 0.29f, 0.28f, -1.f));
                    if (view != VK_NULL_HANDLE) ++textured;
                    viewSlot.insert({ view, slot });
                }
            }
            r.mat = slot;
            groups.push_back(r);
            s_treeSpan.push_back({ meshOffset, meshCount, (u32)groups.size() - 1, 0 });
            ++s_treeGroupCount;
        }
    }

    if (albedo.empty()) albedo.push_back(Fvector4().set(0.30f, 0.29f, 0.28f, -1.f));

    const size_t gBytes = groups.size() * sizeof(RtGroup);
    const size_t aBytes = albedo.size() * sizeof(Fvector4);
    const u32    zero[2] = { 0, 0 };
    if (!EnsureHostBuffer(s_groupBuf,  gBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !EnsureHostBuffer(s_albedoBuf, aBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !EnsureHostBuffer(s_zeroBuf,   sizeof(zero), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !WriteHost(s_groupBuf,  groups.data(), gBytes) ||
        !WriteHost(s_albedoBuf, albedo.data(), aBytes) ||
        !WriteHost(s_zeroBuf,   zero, sizeof(zero))) {
        Msg("![VK RTWorld] table upload failed (%u groups) — render-geometry RT off", nGroups);
        s_failed = true;
        return false;
    }

    s_groupCount = (u32)groups.size();
    s_matCount   = (u32)s_slots.size();
    s_built      = true;
    ++s_texGenCur;
    for (u32 i = 0; i < kSets; ++i) s_texGen[i] = 0;   // every set is now stale

    Msg("[VK RTWorld] render-geometry tables: %u world groups + %u tree groups, %u materials (%u with a real diffuse)",
        nGroups, s_treeGroupCount, s_matCount, textured);
    if (noAddr)   Msg("![VK RTWorld] %u groups have no buffer device address — their geometry stays out of the BVH", noAddr);
    if (overflow) Msg("![VK RTWorld] material cap %u exceeded by %u — those surfaces fall back to grey", kMaxMaterials, overflow);
    return true;
}

bool Ready() { return s_built && !s_failed; }

void OnLevelUnload()
{
    // The pool buffers are about to be destroyed with the level, and every
    // address in the group table points into them.
    s_built = false;
    s_failed = false;
    s_groupCount = 0;
    s_matCount = 0;
    s_slots.clear();
    s_treeSpan.clear();
    s_treeGroupCount = 0;
    s_worldJobs.clear();
    s_treeJobs.clear();
    ++s_texGenCur;
    for (u32 i = 0; i < kSets; ++i) s_texGen[i] = 0;
}

void Destroy()
{
    OnLevelUnload();
    s_groupBuf.Destroy(); s_albedoBuf.Destroy(); s_jobBuf.Destroy(); s_zeroBuf.Destroy();
    s_treeJobBuf.Destroy();
    if (s_gatherPipe)       { vkDestroyPipeline(VulkanHW.m_Device, s_gatherPipe, nullptr); s_gatherPipe = VK_NULL_HANDLE; }
    if (s_gatherPipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_gatherPipeLayout, nullptr); s_gatherPipeLayout = VK_NULL_HANDLE; }
    if (s_gatherPool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_gatherPool, nullptr); s_gatherPool = VK_NULL_HANDLE; }
    if (s_gatherLayout)     { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_gatherLayout, nullptr); s_gatherLayout = VK_NULL_HANDLE; }
    if (s_treePipe)         { vkDestroyPipeline(VulkanHW.m_Device, s_treePipe, nullptr); s_treePipe = VK_NULL_HANDLE; }
    if (s_treePipeLayout)   { vkDestroyPipelineLayout(VulkanHW.m_Device, s_treePipeLayout, nullptr); s_treePipeLayout = VK_NULL_HANDLE; }
    if (s_treePool)         { vkDestroyDescriptorPool(VulkanHW.m_Device, s_treePool, nullptr); s_treePool = VK_NULL_HANDLE; }
    if (s_treeLayout)       { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_treeLayout, nullptr); s_treeLayout = VK_NULL_HANDLE; }
    if (s_texPool)          { vkDestroyDescriptorPool(VulkanHW.m_Device, s_texPool, nullptr); s_texPool = VK_NULL_HANDLE; }
    if (s_texLayout)        { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_texLayout, nullptr); s_texLayout = VK_NULL_HANDLE; }
    for (u32 i = 0; i < kSets; ++i) { s_texSets[i] = VK_NULL_HANDLE; s_gatherSets[i] = VK_NULL_HANDLE; s_treeSets[i] = VK_NULL_HANDLE; }
}

VkDescriptorSetLayout TexSetLayout() { CreateTexSet(); return s_texLayout; }
VkDescriptorSet       TexSet()
{
    if (s_texLayout == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    // A set that has never been written cannot be in use by anything, so filling
    // it here is safe — and it saves every consumer from ordering itself after
    // the first FrameTick.
    if (s_texGen[s_texCur] == 0) PublishTexSet(s_texCur);
    // Still zero = the material cache had no default texture to fill the array
    // with. Handing out a set whose descriptors were never written is worse than
    // handing out nothing: the shader statically samples it.
    return s_texGen[s_texCur] ? s_texSets[s_texCur] : VK_NULL_HANDLE;
}
VkBuffer              AlbedoBuffer() { return s_albedoBuf.IsValid() ? s_albedoBuf.GetHandle() : VK_NULL_HANDLE; }
u32                   AlbedoCount()  { return s_matCount; }

void FrameTick()
{
    if (s_texLayout == VK_NULL_HANDLE) return;

    // A streamed mip promotion swaps the material's image (and view) without
    // telling anyone here, so compare what we published against what the cache
    // holds now. A pointer compare per material per frame is nothing next to the
    // alternative of rewriting 2048 descriptors every frame.
    for (const MatSlot& ms : s_slots) {
        if (ms.mat && ms.mat->view != ms.view) { ++s_texGenCur; break; }
    }

    const u32 next = (s_texCur + 1) % kSets;
    if (s_texGen[next] != s_texGenCur) PublishTexSet(next);
    s_texCur = next;
}

// ---------------------------------------------------------------------------
//  SELECT — the RT cut of the cluster-LOD DAG (worker thread)
//
//  Same shape as world_cull.comp, with two deliberate differences:
//
//   * DISTANCE, NOT VIEW-Z. The raster projects the error along the view axis
//     because that is what perspective divides by. A probe ray is not looking
//     anywhere in particular — it leaves the bubble in every direction at once —
//     so the RT cut measures plain Euclidean distance from the bubble centre.
//   * HARD CUT, NO CROSSFADE. The raster's dithered band exists to hide a LOD pop
//     on screen; two overlapping LOD levels in a BVH are just double geometry.
// ---------------------------------------------------------------------------
SelectStats s_stats = {};
const SelectStats& LastSelect() { return s_stats; }

u32 Select(const Fvector& center, float half, u32 maxTris)
{
    // ⭐ THE SOUP IS SORTED BY OPACITY, and that is what makes an alpha test
    // affordable. Ray flags are per GEOMETRY, not per triangle, so a soup with
    // leaf cards mixed into it would have to be declared non-opaque WHOLE — every
    // hit anywhere in the world would then leave the traversal to ask a shader
    // whether it counts. Opaque triangles first, cutouts after, and the two halves
    // become two acceleration structures: one that never calls back, one that does.
    xr_vector<Job>& out = s_worldJobs;
    out.clear();
    s_worldJobsAT.clear();
    s_treeJobs.clear();
    s_stats = SelectStats{};
    s_opaqueTris = 0;
    if (!s_built) return 0;

    const xr_vector<WorldGPU::GpuMeshMeta>& meta = WorldGPU::HostMeta();
    const u32 n = (u32)meta.size();

    const u32* bits = ClusterStream::HostBits();

    // The bubble is a BOX in the collision path; a sphere that contains it keeps
    // the test to one dot product and never drops geometry the box would have kept.
    const float reach = half * 1.7320508f;
    const float scale = _max(1.f, ps_r_rt_lod);
    const float minD  = 1.f;

    u32 tris = 0;
    out.reserve(4096);
    for (u32 e = 0; e < n; ++e) {
        const WorldGPU::GpuMeshMeta& m = meta[e];
        const float d = center.distance_to(m.sphere_P);
        if (d - m.sphere_R > reach) continue;
        ++s_stats.inRange;

        bool leaf = false;
        if (bits) {
            const u32 sb = (bits[e >> 4u] >> ((e & 15u) * 2u)) & 3u;
            if ((sb & 1u) == 0u) { ++s_stats.noPage; continue; }   // not resident — an ancestor covers it
            leaf = (sb & 2u) != 0u;
        }

        // projErr, Euclidean: error <= distance / scale is "fine enough here".
        const float dSelf = _max(minD, center.distance_to(*(const Fvector*)&m.lodSelf)   - m.lodSelf.w);
        const float dPar  = _max(minD, center.distance_to(*(const Fvector*)&m.lodParent) - m.lodParent.w);
        const float sp = m.selfError   * scale / dSelf;
        const float pp = m.parentError * scale / dPar;
        if (pp <= 1.f) { ++s_stats.lodCut; continue; }   // the parent is fine here — it draws instead
        if (sp > 1.f && !leaf) { ++s_stats.lodCut; continue; }   // too coarse: a child level covers this

        const u32 t = m.index_count / 3u;
        if (t == 0) continue;
        if (tris + t > maxTris) { ++s_stats.capped; continue; }  // over budget: keep taking cheap ones
        // outBase is the TRIANGLE COUNT for now — the second pass turns the two
        // classes into two contiguous runs.
        if (m.flags & 1u) s_worldJobsAT.push_back({ e, t });   // bit0 = alpha-tested material
        else              out.push_back({ e, t });
        tris += t;
    }
    s_stats.picked = (u32)(out.size() + s_worldJobsAT.size());

    // ---- TREES ------------------------------------------------------------
    // No DAG and no residency here: a tree is one mesh, always resident, and the
    // manager's own LOD story is an imposter billboard that a ray has no use for.
    // Just every instance whose sphere reaches the bubble, until the budget ends.
    CTreeManager* tm = RImplementation.Trees;
    if (tm && tm->IsReady() && !s_treeSpan.empty()) {
        // ⭐ FOLIAGE HAS ITS OWN, SHORTER REACH (r_rt_tree_range). A tree enters
        // whole or not at all, and a forest inside the full bubble IS the
        // structure — 99.5% of it, measured. The statics around it cost a
        // rounding error, so cutting BOTH classes at one radius (which is what a
        // smaller r_rtao_range does) pays for the forest by deleting the town.
        // See the note over ps_r_rt_tree_range for why 32 m is the number.
        const float treeReach = (ps_r_rt_tree_range > 0.f)
                              ? _min(reach, ps_r_rt_tree_range) : reach;
        const xr_vector<GpuTreeMeta>& tmeta = tm->RTHostMeta();
        for (const TreeSpan& sp : s_treeSpan) {
            const u32 end = _min((u32)tmeta.size(), sp.meshOffset + sp.meshCount);
            for (u32 i = sp.meshOffset; i < end; ++i) {
                const GpuTreeMeta& m = tmeta[i];
                if (center.distance_to(m.sphere_P) - m.sphere_R > reach) continue;
                ++s_stats.treesInRange;
                if (center.distance_to(m.sphere_P) - m.sphere_R > treeReach) { ++s_stats.treesCut; continue; }
                const u32 t = m.index_count / 3u;
                if (t == 0) continue;
                if (tris + t > maxTris) { ++s_stats.capped; continue; }
                s_treeJobs.push_back({ i, t, sp.group, 0u });   // y = tri count for now
                tris += t;
            }
        }
    }
    s_stats.trees = (u32)s_treeJobs.size();

    // Second pass: opaque run first, then every cutout (static AT materials and
    // every tree, because a leaf card is nothing but a cutout).
    u32 base = 0;
    for (Job& j : out)            { const u32 t = j.outBase; j.outBase = base; base += t; }
    s_opaqueTris = base;
    for (Job& j : s_worldJobsAT)  { const u32 t = j.outBase; j.outBase = base; base += t; }
    for (TreeJob& j : s_treeJobs) { const u32 t = j.outBase; j.outBase = base; base += t; }
    // One dispatch covers both static classes; the split lives in the outBases.
    out.insert(out.end(), s_worldJobsAT.begin(), s_worldJobsAT.end());
    return base;
}

u32 OpaqueTris() { return s_opaqueTris; }

// ---------------------------------------------------------------------------
//  RECORD GATHER — copy the selected triangles into the RT soup (render thread)
// ---------------------------------------------------------------------------
bool RecordGather(VkCommandBuffer cmd, u32 tris, VkBuffer outPos, VkBuffer outAttr)
{
    const xr_vector<Job>& jobs = s_worldJobs;
    if (!s_built || tris == 0) return false;
    if (jobs.empty() && s_treeJobs.empty()) return false;
    if (outPos == VK_NULL_HANDLE || outAttr == VK_NULL_HANDLE) return false;

    const size_t jBytes = _max<size_t>(jobs.size() * sizeof(Job), 16);
    if (!EnsureHostBuffer(s_jobBuf, jBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        (!jobs.empty() && !WriteHost(s_jobBuf, jobs.data(), jobs.size() * sizeof(Job)))) {
        Msg("![VK RTWorld] job buffer upload failed (%u entries)", (u32)jobs.size());
        return false;
    }
    const size_t tBytes = _max<size_t>(s_treeJobs.size() * sizeof(TreeJob), 16);
    if (!EnsureHostBuffer(s_treeJobBuf, tBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        (!s_treeJobs.empty() && !WriteHost(s_treeJobBuf, s_treeJobs.data(), s_treeJobs.size() * sizeof(TreeJob)))) {
        Msg("![VK RTWorld] tree job upload failed (%u instances)", (u32)s_treeJobs.size());
        return false;
    }

    VkBuffer slotBase = ClusterStream::SlotBaseBuffer();
    VkBuffer bitsBuf  = ClusterStream::BitsBuffer();
    const u32 pushFlags = (bitsBuf != VK_NULL_HANDLE) ? kPushHaveBits : 0u;
    if (slotBase == VK_NULL_HANDLE) slotBase = s_zeroBuf.GetHandle();
    if (bitsBuf  == VK_NULL_HANDLE) bitsBuf  = s_zeroBuf.GetHandle();
    const VkBuffer metaBuf = WorldGPU::MetaBuffer();
    if (metaBuf == VK_NULL_HANDLE) return false;

    s_gatherCur = (s_gatherCur + 1) % kSets;
    VK::DescriptorWriter(s_gatherSets[s_gatherCur])
        .StorageBuffer(0, metaBuf)
        .StorageBuffer(1, slotBase)
        .StorageBuffer(2, bitsBuf)
        .StorageBuffer(3, s_groupBuf.GetHandle())
        .StorageBuffer(4, s_jobBuf.GetHandle())
        .StorageBuffer(5, outPos)
        .StorageBuffer(6, outAttr)
        .Flush();

    // The page installs of THIS frame were recorded (as transfer copies into the
    // very pools this pass is about to read by address) before anything traces,
    // and a device address carries no automatic dependency at all.
    VK::MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,       VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    const int z = VK::Prof::ZoneBegin(cmd, "RT/Gather");
    if (!jobs.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_gatherPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_gatherPipeLayout, 0, 1,
                                &s_gatherSets[s_gatherCur], 0, nullptr);
        GatherPush push{ (u32)jobs.size(), tris, pushFlags, 0u };
        vkCmdPushConstants(cmd, s_gatherPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (u32)jobs.size(), 1, 1);
    }
    // The trees write into DIFFERENT triangles of the same soup (their outBase
    // continues past the world's), so the two dispatches never touch the same
    // bytes and need no barrier between them.
    if (!s_treeJobs.empty() && s_treePipe != VK_NULL_HANDLE) {
        CTreeManager* tm = RImplementation.Trees;
        const VkBuffer tMeta  = tm ? tm->RTMetaBuffer()  : VK_NULL_HANDLE;
        const VkBuffer tXform = tm ? tm->RTXformBuffer() : VK_NULL_HANDLE;
        if (tMeta && tXform) {
            s_treeCur = (s_treeCur + 1) % kSets;
            VK::DescriptorWriter(s_treeSets[s_treeCur])
                .StorageBuffer(0, tMeta)
                .StorageBuffer(1, tXform)
                .StorageBuffer(2, s_groupBuf.GetHandle())
                .StorageBuffer(3, s_treeJobBuf.GetHandle())
                .StorageBuffer(4, outPos)
                .StorageBuffer(5, outAttr)
                .Flush();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_treePipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_treePipeLayout, 0, 1,
                                    &s_treeSets[s_treeCur], 0, nullptr);
            GatherPush push{ (u32)s_treeJobs.size(), tris, 0u, 0u };
            vkCmdPushConstants(cmd, s_treePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (u32)s_treeJobs.size(), 1, 1);
        }
    }
    VK::Prof::ZoneEnd(cmd, z);

    // The soup is the BLAS build input and the shading source both — and the two
    // read it under DIFFERENT access types (a build's vertex fetch is
    // ACCELERATION_STRUCTURE_READ, not SHADER_READ), so the mask has to name both
    // or sync validation is right to call the build unsynchronised.
    VK::MemoryBarrier(cmd,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    return true;
}

}}  // namespace VK::RTWorld

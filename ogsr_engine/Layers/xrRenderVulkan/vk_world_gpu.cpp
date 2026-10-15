// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// GPU-driven world forward pass — see vk_world_gpu.h. Cull mirrors vk_shadow_gpu;
// the draw mirrors RenderQueue::Flush / FlushDepth.

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_profiler.h"   // TEMP VUID-hunt: VK::Prof::NameSet
#include "vk_world_gpu.h"
#include "vk_rr_guides.h"       // VK::RRGuide — world-scope attachment list
#include "vk_cluster_stream.h"  // Stage B: cluster-IB page streaming (pools + residency bits)
#include "CRender_Vulkan.h"     // RImplementation.Visuals
#include "vk_Visual.h"          // vkFVisual, vkFHierrarhyVisual, m_mesh, m_pWorldMaterial
#include "vk_world_material.h"  // WorldMaterial / WorldMaterialCache
#include "vk_render_queue.h"    // VK::RenderQueue (SubmitCpuMeshes)
#include "vk_terrain_cache.h"   // TerrainCache capture (composite ground cache)
#include "vk_buffer.h"          // CVulkanBuffer
#include "vk_pipeline_cache.h"  // PipelineCache pipelines/layouts
#include "vk_compute_util.h"    // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_gfx_pipeline.h"    // VK::GfxPipelineBuilder
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_parallel.h"        // VK::ParallelChunks — per-mesh resolves run on the idle cores
#include "vk_env_light.h"       // EnvLight::GetCurrentSet (set 1, terrain snow-depth)
#include "vk_vrs.h"             // SubStats* — r_fsinv_split terrain/mesh attribution
#include "../../xrCDB/Frustum.h"   // CFrustum - the merge-ceiling diag replicates the shader frustum test
#include "vk_pass_context.h"   // VK::LodScreenHeight - the extent the LOD cut is measured in
#include "vk_scene_color.h"     // HDR scene target format (cluster debug overlay)
#include "vk_swapchain.h"       // Swapchain.m_DepthFormat (cluster debug overlay)
#include "vk_shaders.h"         // g_ShaderManager
#include "vk_cull.h"            // VK::ExtractFrustumPlanes
#include "vk_command_buffer.h"
#include "../../xr_3da/device.h" // RV().dwTimeGlobal (occlusion-stats throttle)
#include "../../../3rd_party/Src/meshoptimizer/src/meshoptimizer.h"  // cluster split (vk_meshopt.cpp)
#include "../../../3rd_party/Src/meshoptimizer/demo/clusterlod.h"    // clodBuild — Nanite-style DAG (impl in vk_meshopt.cpp)
#include <algorithm>
#include <unordered_map>
#include <unordered_set>   // LeafVisuals: dedup for the XROS_LEAFWALK_FULL cross-check
#include <cstdlib>         // std::getenv — XROS_LEAFWALK_FULL
#include <atomic>
#include <thread>

extern int ps_r_cluster;        // cluster-granularity cull (Phase 1 of cluster-LOD) — read at Build
extern int ps_r_cluster_tris;   // min triangles before a mesh is split into clusters
extern int ps_r_cluster_merge;  // Phase 2.5: merge touching solid fragments into one component DAG — read at Build
#ifdef __SWITCH__
void NxGpuMark(VkCommandBuffer cmd, const char* name);   // CRender_Vulkan.cpp: [nx gpu avg/slow], GPU time up to here
#define NX_GPU_MARK(cmd, name) NxGpuMark(cmd, name)
#else
#define NX_GPU_MARK(cmd, name) ((void)0)
#endif
extern int ps_r_cluster_cache;  // 0 = ignore the on-disk DAG cache and always rebake (repro aid)

extern float ps_r_cluster_lod;  // DAG-cut px error threshold (Phase 2) — live, read per frame
extern float ps_r_vsm_cluster_lod;   // Phase 3 shadow LOD quality k (error budget in target texels) — live
extern float ps_r_cluster_fade; // crossfade band as a fraction of the threshold (0 = hard cut) — live
extern int ps_r_cluster_debug;  // 1 = cluster fill colors, 2 = wireframe (live)
extern int ps_r_world_bindless;     // r_world_bindless: opaque statics in material-agnostic groups on the bindless twins (latched at Build)
extern int ps_r_world_push_hoist; // A/B: viewProj pushed once per layout flip (1) vs once per group (0)
extern int ps_r_profiler;       // 2 = verbose: enables the throttled occlusion/cpu-cut stats log
extern int ps_r_ssao_debug;     // r_ssao_debug — gates the statics-normal coverage log in DrawNormals
extern float ps_r_tess;         // global tessellation toggle — decides tessellated-material routing at Build
extern int ps_r_pool_compact;   // Stage B (б): free cluster-repacked slices from the level VB/IB pools — read at load
extern int ps_r_gpu_shadows_at; // AT casters through the cluster shadow cull/draw (live A/B; cull+draw read it in the same record)
extern float ps_r_ssa_px;       // SSA cull of plain whole meshes: projected-diameter threshold in px (0 = off) — live
extern int ps_r_fsinv_split;    // FS-invocation attribution diag — also arms the [VK Cut] view-cut composition log

namespace VK { namespace WorldGPU {

namespace {

// r_cluster_fade as the cull AND both draws see it this frame. 0 = a hard LOD cut -- no dithered band in the cull, and
// the plain (no-FS) depth pipeline + the non-fade colour twins instead of the discard ones. While the band is on, the
// prepass takes the fade pipeline for EVERY solid group, and discard + z-write is late Z.
float ClusterFade()
{
    return ps_r_cluster_fade;
}

// GpuMeshMeta (one cullable ENTRY — a whole mesh or one LOD-DAG cluster) and
// kErrInf moved to vk_world_gpu.h: vk_cluster_stream shares the layout for the
// page assembly / residency tables. The cull shader picks the DAG cut per
// frame: draw iff projected selfError <= threshold < projected parentError;
// siblings share the exact values their parents test as self → the whole group
// flips atomically (crack-free). Plain meshes: selfError=0 / parentError=INF.
// Cluster ib_first is PAGE-LOCAL since Stage B; the cull adds the page's slot
// base (pageSlotBase SSBO), so eviction/install never touches this meta.

// One indirect-draw batch: a run of meshes sharing material + pipeline key + VB/IB,
// so the draw binds those once and issues a single vkCmdDrawIndexedIndirectCount.
// entryOffset/entryCount = this group's slice of the meta/indirect arrays (exact
// prefix-sum capacity — clusters skew group sizes far too much for the old uniform
// maxGroupMesh regions).
struct Group {
    WorldMaterial* mat;
    bool        terrain;
    bool        bindless;   // r_world_bindless: a material-agnostic group (grp.mat = its first mesh's, for the flags only)
    u32         stride, tcOffset;
    VkBuffer    vb, ib;
    VkIndexType iType;
    u32         meshCount;
    u32         entryOffset, entryCount;
    // Union of the group entries cull spheres, in world space. Groups are keyed
    // by MATERIAL, not by place, so this is only tight when a material happens to
    // be local (one building, one prop). r_world_group_cull tests it on the CPU to
    // drop the groups indirect packet exactly (no stale counts) — how much of the
    // list that actually catches is what the diag below reports.
    Fvector     bsP; float bsR;
};

// lodParams: x = viewportH/(2·tan(fovY/2))/thresholdPx (projected-error scale),
// y = min camera distance clamp; z,w spare.
struct CullPush { Fvector4 planes[6]; Fvector4 cameraPos; Fvector4 viewDir; Fvector4 lodParams; u32 numGroups; float ssaCull /*2·pxScale/r_ssa_px, 0=off (was unused0)*/; u32 total, _pad; };
// Hi-Z occlusion cull push (240 B) — matches world_cull_hzb.comp `PC`, same shape
// as the proven detail_generate.comp push (viewProj + planes + cameraPos).
struct CullColorPush { Fmatrix viewProj; Fvector4 planes[6]; Fvector4 cameraPos; Fvector4 viewDir; Fvector4 lodParams; u32 numGroups; float ssaCull /*2·pxScale/r_ssa_px, 0=off (was unused0)*/; u32 total, _pad; };

bool s_built = false;
u32  s_total = 0;          // total cullable ENTRIES (meshes + clusters) = dispatch size
xr_vector<Group> s_groups;

// ---- r_world_bindless (10-09) ----------------------------------------------------------------------------------------
// One group per MATERIAL was ~600 indirect draws per pass; on the Switch (NVK, Maxwell) each is a descriptor bind plus
// pushbuffer segments the command processor fetches one by one -- the statics colour pass cost 0.80 of 2.10 timestamp
// units with EMPTY commands (v288). Opaque statics now group by VB/IB/stride/POM only and draw on the *_bl world twins:
// set 0 = one bindless set (uWorldTex[5 * material + k], the material records, cull entry -> material; see
// world_bindless.glsl). Latched at Build with the groups. Terrain, alpha-tested, glass/emissive/decal/water stay one
// group per material (few, and their depth/shadow/TES shaders read the material set).
constexpr u32 kBlFrames = VK_FRAMES_IN_FLIGHT;
struct BlMatRec { float detailScale; u32 streamID; u32 _p0, _p1; };   // world_bindless.glsl WorldMatRec
bool                              s_bindless = false;
xr_vector<WorldMaterial*>         s_blMats;                    // material index -> material
xr_map<const WorldMaterial*, u32> s_blIndex;                   // material -> index (Build only)
CVulkanBuffer*                    s_blEntryMat = nullptr;      // u32 per cull entry
CVulkanBuffer*                    s_blMatRec[kBlFrames] = {};  // per frame in flight, host-mapped, rewritten per frame
BlMatRec*                         s_blMatPtr[kBlFrames] = {};
VkDescriptorPool                  s_blPool = VK_NULL_HANDLE;
VkDescriptorSet                   s_blSet[kBlFrames] = {};
xr_vector<VkImageView>            s_blBound[kBlFrames];        // what each set's texture slots hold right now
u32                               s_blUpdatedFrame = ~0u;

bool BlEligible(const WorldMaterial* m)
{
    return m && !m->isTerrain && m->alphaRef < -0.5f && m->alphaRef > -1.5f && m->set != VK_NULL_HANDLE
        && !m->isWmark && !m->isGlass && !m->isEmisAdd && !m->isLitBlend && !m->isWater;
}

void BlDestroy()
{
    if (s_blPool) { vkDestroyDescriptorPool(VulkanHW.m_Device, s_blPool, nullptr); s_blPool = VK_NULL_HANDLE; }
    for (u32 f = 0; f < kBlFrames; ++f) {
        s_blSet[f] = VK_NULL_HANDLE;
        if (s_blMatRec[f]) { s_blMatRec[f]->Unmap(); xr_delete(s_blMatRec[f]); }
        s_blMatPtr[f] = nullptr;
        s_blBound[f].clear();
    }
    if (s_blEntryMat) xr_delete(s_blEntryMat);
    s_blUpdatedFrame = ~0u;
}

// The per-frame sets + material records for s_blMats (Build, before the grouping depends on them).
bool BlCreate()
{
    const u32 nMat = (u32)s_blMats.size();
    const u32 nTex = PipelineCache::WorldBindlessMaxTextures();
    const VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nTex * kBlFrames },
                                         { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u * kBlFrames } };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = kBlFrames; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_blPool) != VK_SUCCESS) { s_blPool = VK_NULL_HANDLE; return false; }
    VkDescriptorSetLayout sl[kBlFrames];
    for (u32 f = 0; f < kBlFrames; ++f) sl[f] = PipelineCache::GetWorldBindlessSetLayout();
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = s_blPool; ai.descriptorSetCount = kBlFrames; ai.pSetLayouts = sl;
    if (vkAllocateDescriptorSets(VulkanHW.m_Device, &ai, s_blSet) != VK_SUCCESS) { BlDestroy(); return false; }
    for (u32 f = 0; f < kBlFrames; ++f) {
        s_blMatRec[f] = xr_new<CVulkanBuffer>();
        s_blMatRec[f]->Create(sizeof(BlMatRec) * nMat, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        s_blMatPtr[f] = (BlMatRec*)s_blMatRec[f]->Map();
        if (!s_blMatPtr[f]) { BlDestroy(); return false; }
        s_blBound[f].assign(nMat * 5u, VK_NULL_HANDLE);
        VK::DescriptorWriter(s_blSet[f]).StorageBuffer(1, s_blMatRec[f]->GetHandle()).Flush();
    }
    return true;
}

// Once per frame, before the first bindless draw: this frame slot's set gets the views that CHANGED since it was last
// used (texture streaming swaps a material's base/bump view at runtime -- WorldMaterialCache rewrites its own set the
// same moment), and the material records are rewritten whole (~16 B each; streamID may be reset after Build).
// Safe without UPDATE_AFTER_BIND: the slot's previous frame is fence-complete when this frame records.
void BlUpdate()
{
    if (!s_bindless || s_blUpdatedFrame == RV().frameIndex) return;
    s_blUpdatedFrame = RV().frameIndex;
    const u32 f = CommandManager.GetCurrentFrame() % kBlFrames;
    if (!s_blSet[f] || !s_blMatPtr[f]) return;
    const VkSampler smp = WorldMaterialCache::GetSampler();
    static xr_vector<VkDescriptorImageInfo> s_info;
    static xr_vector<VkWriteDescriptorSet>  s_write;
    s_info.clear(); s_write.clear();
    const u32 nMat = (u32)s_blMats.size();
    s_info.reserve(nMat * 5u);
    for (u32 i = 0; i < nMat; ++i) {
        const WorldMaterial* m = s_blMats[i];
        VkImageView v[5];
        WorldMaterialCache::SetViews(m, v);
        for (u32 k = 0; k < 5; ++k) {
            VkImageView& bound = s_blBound[f][i * 5u + k];
            if (v[k] == VK_NULL_HANDLE || v[k] == bound) continue;
            bound = v[k];
            s_info.push_back({ smp, v[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = s_blSet[f]; w.dstBinding = 0; w.dstArrayElement = i * 5u + k;
            w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            s_write.push_back(w);
        }
        s_blMatPtr[f][i] = { m->detailScale, m->streamID, 0u, 0u };
    }
    for (u32 j = 0; j < (u32)s_write.size(); ++j) s_write[j].pImageInfo = &s_info[j];
    if (!s_write.empty())
        vkUpdateDescriptorSets(VulkanHW.m_Device, (u32)s_write.size(), s_write.data(), 0, nullptr);
}
xr_vector<vkRender_Visual*> s_meshSet;   // pointer-sorted, for InSet() (CPU-queue exclusion)
xr_vector<vkFVisual*> s_cpuMeshes;       // non-GPU static leaves (wmark/tess/no-diffuse) — CPU draws these

// Pool-compaction bookkeeping (CompactPools): per GPU-set mesh, its meta run +
// how its entries reference the original pools. Filled during the grouping walk,
// consumed once by CompactPools, cleared on Destroy.
struct CompactRef {
    vkFVisual* fv;
    u32 metaFirst, metaCount;
    u8  clustered;   // entries live in the ClusterStream IB pools
    u8  ib32;        // u32 pool-global component DAG (page indices address the pool VB directly)
    u8  repacked;    // vertices live in the ClusterStream VB pool (original VB slice unused)
};
xr_vector<CompactRef> s_compactRefs;
bool s_poolsCompacted = false;

CVulkanBuffer* s_meta      = nullptr;
xr_vector<GpuMeshMeta> s_metaCPU;   // host copy for the throttled cut simulation (diag)
u32 s_buildStamp = 0;               // bumped each Build (VSM candidate-table freshness)
CVulkanBuffer* s_indirect  = nullptr;   // s_total cmds, per-group prefix-sum regions (frustum set)
CVulkanBuffer* s_count     = nullptr;   // numGroups u32 (frustum set)
CVulkanBuffer* s_groupBase = nullptr;   // numGroups u32 — each group's entryOffset (cmd region base)
// Cluster index data lives in the ClusterStream page pools since Stage B
// (PoolIB16 = u16 per-mesh DAGs rel. vBase, PoolIB32 = u32 pool-global
// component DAGs). Page directory + streaming source, filled by BuildClusters:
xr_vector<ClusterStream::PageRec> s_pageDir;
string_path s_cachePath = "";
u64 s_blobOff16 = 0, s_blobOff32 = 0, s_blobOffVb = 0;

VkDescriptorSetLayout s_setL  = VK_NULL_HANDLE;
VkDescriptorPool      s_pool  = VK_NULL_HANDLE;
VkDescriptorSet       s_set   = VK_NULL_HANDLE;
VkPipelineLayout      s_cullLayout = VK_NULL_HANDLE;
VkPipeline            s_cullPipe   = VK_NULL_HANDLE;

// ---- Phase 3: cluster-LOD shadow casters (sun far map + 2 cascades) ----
// One indirect/count slice per shadow target; the ortho cull writes exact
// prefix-sum regions (groupBase) inside each target's slice. Best-effort like
// the occlusion set — failure leaves s_shadowReady=false and vk_pass_shadow
// falls back to the per-mesh ShadowGPU path.
constexpr u32 kShadowTargets = 5;   // ShadowGPU::TGT_COUNT (far, casc0, casc1) + kTargetRain + kTargetDyn
bool s_shadowReady = false;
CVulkanBuffer* s_indirectSh = nullptr;   // kShadowTargets × s_total cmds
CVulkanBuffer* s_countSh    = nullptr;   // kShadowTargets × numGroups u32
VkDescriptorSetLayout s_setL3  = VK_NULL_HANDLE;
VkDescriptorPool      s_pool3  = VK_NULL_HANDLE;
VkDescriptorSet       s_set3   = VK_NULL_HANDLE;
VkPipelineLayout      s_cullLayout3 = VK_NULL_HANDLE;
VkPipeline            s_cullPipe3   = VK_NULL_HANDLE;
// Matches world_cull_shadow.comp PC (116 B).
struct CullShadowPush { Fvector4 planes[6]; float errBudget; u32 target, numGroups, total, atOn; };

// ---- Phase A: Hi-Z occlusion cull (separate set, drawn only in the color pass) ----
bool s_occlReady = false;
CVulkanBuffer* s_indirect2 = nullptr;  // numGroups * maxGroupMesh cmds (occlusion set)
CVulkanBuffer* s_count2    = nullptr;  // numGroups u32 (occlusion set)
VkDescriptorSetLayout s_setL2  = VK_NULL_HANDLE;
VkDescriptorPool      s_pool2  = VK_NULL_HANDLE;
VkDescriptorSet       s_set2   = VK_NULL_HANDLE;
// What binding 3 (the Hi-Z pyramid) currently holds — s_set2 is single while
// several frames are in flight, so CullColor rewrites it only when this changes.
VkImageView s_hzbBoundView    = VK_NULL_HANDLE;
VkSampler   s_hzbBoundSampler = VK_NULL_HANDLE;
u32         s_hzbBoundGen     = 0xFFFFFFFFu;
VkPipelineLayout      s_cullLayout2 = VK_NULL_HANDLE;
VkPipeline            s_cullPipe2   = VK_NULL_HANDLE;

// ---- Two-phase occlusion for the depth prepass (r_hzb_prepass) -------------
// ONE shader (world_cull_two.comp), TWO dispatches, TWO descriptor sets with an
// IDENTICAL layout — [0] writes set A (EARLY), [1] writes set B (LATE). The only
// difference between the sets is which indirect/count pair bindings 1/2 point at,
// so the shader needs no output switch and the cut cannot diverge between phases.
//
// When this path runs it REPLACES BOTH Cull() and CullColor(). That is where the
// win is: those two dispatches each read the whole 830k-entry meta array (80 B
// each = 66 MB) to produce ~5k draws, and World/HZBcull has already been caught
// at 6.6 ms when VRAM ran short and the array spilled over PCIe. Two-phase leaves
// ONE full scan plus a dispatchIndirect over a few thousand deferred entries.
bool s_twoReady = false;
CVulkanBuffer* s_late = nullptr;   // {dispatchX,Y,Z, count, items[kLateCap]}
VkDescriptorSetLayout s_setLTwo = VK_NULL_HANDLE;
VkDescriptorPool      s_poolTwo = VK_NULL_HANDLE;
VkDescriptorSet       s_setTwo[2]{ VK_NULL_HANDLE, VK_NULL_HANDLE };   // 0 = EARLY→A, 1 = LATE→B
VkPipelineLayout      s_cullLayoutTwo = VK_NULL_HANDLE;
VkPipeline            s_cullPipeTwo   = VK_NULL_HANDLE;
// Binding 8 (the pyramid) per set — same single-set/frames-in-flight trap as
// s_set2's binding 3: rewrite ONLY on change, keyed on generation as well as the
// handle (Destroy+Create can hand back the same VkImageView value).
VkImageView s_twoHzbView[2]{ VK_NULL_HANDLE, VK_NULL_HANDLE };
VkSampler   s_twoHzbSmp [2]{ VK_NULL_HANDLE, VK_NULL_HANDLE };
u32         s_twoHzbGen [2]{ 0xFFFFFFFFu, 0xFFFFFFFFu };
// The camera the EARLY pass tests against: the one the pyramid it reads was built
// from, i.e. LAST frame's. Captured here rather than borrowed from MotionVec's
// PrevVP() so it is exactly the (jittered) matrix THIS module was called with.
// Getting it wrong would cost culls, never correctness — phase 2 re-tests every
// early reject — but a silent efficiency loss is the kind nobody finds later.
Fmatrix s_prevVP;
Fvector s_prevCam{};
u32     s_prevVPFrame = 0xFFFFFFFFu;   // RV().frameIndex s_prevVP belongs to

// Late-list capacity. MUST stay a multiple of 256: world_cull_two.comp's LATE
// pass relies on ceil(cap/256)*256 == cap to stay inside the array when the list
// overflows (see the two-bounds note in mainLate). 256k entries = 1 MB; the
// forest defers ~2.3k, so the overflow path is a formality, not a design point —
// and even then it degrades to "draw it in phase 1", never to a hole.
constexpr u32 kLateCap = 256u * 1024u;
// Byte offsets inside s_late (mirrors the shader's Late block).
constexpr VkDeviceSize kLateHeaderBytes = 4u * sizeof(u32);   // dispatchXYZ + count

// Matches world_cull_two.comp `PC` — 244 B, offsets verified with spirv-dis
// (0/64/160/176/192/208/212/216/220/224/228/232/236/240). hzbEye is three
// SCALARS on purpose: a vec3 pads to 16 B and takes the block to 256.
struct CullTwoPush {
    Fmatrix  viewProj;          // EARLY: previous frame's; LATE: this frame's
    Fvector4 planes[6];         // this frame's frustum, always
    Fvector4 cameraPos;         // this frame's camera (LOD/SSA view-Z metric)
    Fvector4 viewDir;
    Fvector4 lodParams;
    u32   numGroups;
    float ssaCull;
    u32   total;
    u32   mode;                 // 0 = EARLY, 1 = LATE
    u32   lateCap;
    u32   hzbValid;
    float hzbEyeX, hzbEyeY, hzbEyeZ;   // eye matching viewProj (near-face pull)
};
static_assert(sizeof(CullTwoPush) == 244, "CullTwoPush must match world_cull_two.comp PC (spirv-dis offsets)");

// Occlusion-stats readback (host-visible, mapped): per-frame we copy both count
// sets here [0..nGroups)=frustum, [nGroups..2n)=occlusion; CPU sums them throttled
// to log "culled K of N". Read is a frame or two stale (no fence wait) — fine for a
// diagnostic. Created raw via VMA (HOST_ACCESS_RANDOM = readable; the CVulkanBuffer
// helper only offers SEQUENTIAL_WRITE for storage buffers).
VkBuffer      s_countReadback      = VK_NULL_HANDLE;
VmaAllocation s_countReadbackAlloc = VK_NULL_HANDLE;
u32*          s_countReadbackPtr   = nullptr;
u32           s_nGroups            = 0;   // cached group count (readback stride)

// ---- Cluster debug view (r_cluster_debug): the LOD cut made visible, UE-style.
// Redraws the SAME per-group indirect regions the color pass consumed, with a
// tiny pipeline that colors by cluster id (the cull shader stores the entry id
// in firstInstance → gl_InstanceIndex, no extra bindings). mode 1 = flat fill,
// mode 2 = wireframe (LINE polygon) over the lit scene. Lazy-built.
VkPipelineLayout        s_dbgLayout = VK_NULL_HANDLE;
xr_map<u64, VkPipeline> s_dbgPipes;   // key = stride*2 + (line ? 1 : 0)
bool                    s_dbgFailed = false;   // don't retry every frame after a failure
// Debug set 0 = the meta SSBO (mode 4 LOD-health view reads parentError per entry).
VkDescriptorSetLayout   s_dbgSetL = VK_NULL_HANDLE;
VkDescriptorPool        s_dbgPool = VK_NULL_HANDLE;
VkDescriptorSet         s_dbgSet  = VK_NULL_HANDLE;

// DAG-cut projection scale for the cull shaders (recomputed per frame — the px
// threshold r_cluster_lod is a live knob). worldError · x / dist = error/threshold;
// the shader draws the first cut whose value stays <= 1.
Fvector4 LodParams()
{
    const float fovR    = deg2rad(RV().fFOV);
    const float pxScale = float(VK::LodScreenHeight()) / (2.f * tanf(fovR * 0.5f));   // r_lod_render_res
    // .z = crossfade band (0 = hard cut). Gated on the fade shader variants
    // being present — the cull must not widen the cut if the draw side can't
    // dither (that would double-draw the whole transition band).
    const bool canFade = PipelineCache::WorldLmapFadeVS() != VK_NULL_HANDLE
                      && PipelineCache::GetDepthFadePipeline(32) != VK_NULL_HANDLE;
    // .w = vertical focal scale P11 = 1/tan(fovY/2). The HZB occlusion cull
    // needs it for the sphere's true screen footprint: ndcHeight = 2·r·P11/w.
    // The old footprint (2·r/w, no focal term) UNDERSTATED the size ~1.5× →
    // too fine a mip → the 2×2 block missed part of the footprint → false
    // culls (the reason r_hzb_cull stayed situational).
    const float lodPx = ps_r_cluster_lod, band = ClusterFade();
    Fvector4 p; p.set(pxScale / _max(0.05f, lodPx), 0.01f,
                      canFade ? _max(0.f, band) : 0.f,
                      1.f / _max(0.05f, tanf(fovR * 0.5f)));
    return p;
}

// SSA cull factor (r_ssa_px): shader culls a plain mesh when
// r · ssaCull < viewZ  ⟺  projected sphere DIAMETER < r_ssa_px pixels.
static float SsaCull()
{
    if (ps_r_ssa_px <= 0.f) return 0.f;
    const float pxScale = float(VK::LodScreenHeight()) / (2.f * tanf(deg2rad(RV().fFOV) * 0.5f));   // r_lod_render_res
    return 2.f * pxScale / ps_r_ssa_px;
}

// Issue one group's indirect draw for a DrawSet. Both buffer pairs share the
// groupBase prefix-sum region layout, so DrawSet::AB is literally the same
// offsets twice — no merge pass and no second region table. Ordering A-then-B
// matches the prepass (EARLY drew first), which keeps LEQUAL/EQUAL depth
// comparisons seeing the same history they saw in the prepass.
void EmitIndirect(VkCommandBuffer cmd, DrawSet set, VkDeviceSize cmdOff,
                  VkDeviceSize cntOff, u32 maxDraws)
{
    const VkBuffer cmdA = s_indirect->GetHandle();
    const VkBuffer cmdB = (set != DrawSet::A) ? s_indirect2->GetHandle() : VK_NULL_HANDLE;
    if (set != DrawSet::B)
        vkCmdDrawIndexedIndirectCount(cmd, cmdA, cmdOff, s_count->GetHandle(),
                                      cntOff, maxDraws, sizeof(VkDrawIndexedIndirectCommand));
    if (set != DrawSet::A)
        vkCmdDrawIndexedIndirectCount(cmd, cmdB, cmdOff, s_count2->GetHandle(),
                                      cntOff, maxDraws, sizeof(VkDrawIndexedIndirectCommand));
}

// Every Draw* below walks ALL groups and issues one vkCmdDrawIndexedIndirectCount
// per group whether or not the cull left that group anything. ⛔Skipping the empty
// ones was MEASURED 26-08-2026 AND REFUTED (r_world_empty_skip, since removed): on a
// 780M at pripyat_full ~1818 of 5428 packets per pass were dropped and not one zone
// moved. Empty indirect packets are free here; the REAL draws are not (World/Depth
// fits 4.77 ms + 95 ns x draws at ~48k draws).

void MemBarrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst,
                VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = src; b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 1, &b, 0, nullptr, 0, nullptr);
}

// The level's leaf visuals, walked ONCE for every load-time consumer — see
// LeafVisuals() in the header for why. `s_leaves` is first-seen ordered and
// deduped; the recursion below is the only place that touches the tree.
xr_vector<vkFVisual*> s_leaves;
bool                  s_leavesReady = false;

void CollectLeaves(vkRender_Visual* rv, std::unordered_set<void*>& seen)
{
    if (!rv) return;

    // Type-dispatch instead of dynamic_cast. vkVisual_Create is the single place
    // that maps MT_* to a class, and the mapping is exact — so RTTI here was
    // ~700k __RTDynamicCast calls answering a question the Type field already
    // answers. The default arm keeps RTTI for anything outside that mapping.
    vkFVisual* fv = nullptr;
    switch (rv->Type) {
    case MT_HIERRARHY:
    case MT_LOD:
        // vkFHierrarhyVisual and vkFLOD (derived) — the only container classes.
        // Their children are getVisual() REFERENCES into Visuals[], so the same
        // leaf arrives both directly and through its container: hence the
        // seen-set, which every one of the old separate walks kept for itself.
        for (auto* child : static_cast<vkFHierrarhyVisual*>(rv)->children)
            CollectLeaves(child, seen);
        return;
    case MT_NORMAL:
    case MT_PROGRESSIVE:
    case MT_TREE_ST:
    case MT_TREE_PM:
        fv = static_cast<vkFVisual*>(rv);   // vkFVisual / vkFProgressive / vkFTreeVisual_*
        break;
    default:
        // Skeletons, particles, the dummy: no level seen so far puts them in
        // Visuals[] (the MT_* tally accounts for every entry), but ask rather
        // than assume — this arm is cold by construction.
        fv = dynamic_cast<vkFVisual*>(rv);
        break;
    }
    if (!fv) return;
    if (!seen.insert(fv).second) return;
    s_leaves.push_back(fv);
}

// Partition the renderable STATIC world-mesh leaves into:
//   gpuOut — opaque/AT meshes the GPU forward path can draw (have a material set +
//            diffuse, not wmark/tess) → compute-culled + indirect-drawn.
//   cpuOut — everything else still renderable (wmark decals, tessellated, no diffuse
//            / no material) → drawn by the CPU RenderQueue (Flush handles them).
// gpuOut ∪ cpuOut = exactly what the old full CPU walk drew (deduped), so nothing
// is lost. Called per entry of LeafVisuals() (which did the recursion + dedup).
// Trees/skeletons/particles are other passes → skipped entirely.
u32 s_cpuWhyNoMat = 0, s_cpuWhyWmark = 0, s_cpuWhyWater = 0, s_cpuWhyTess = 0, s_cpuWhyNoView = 0;

// Input validation for clodBuild — the one call in the load that can turn bad
// data into HEAP CORRUPTION rather than a wrong picture. meshopt's
// buildTriangleAdjacency does `counts[indices[i]]++` over an array sized by
// vertex_count, with the bounds check compiled out in release: a single index
// past the end writes into somebody else's allocation, and the crash surfaces
// later somewhere unrelated (a visual's vtable, in the case that started this).
// Cheap next to a 27 s bake, and it names the mesh instead of leaving a dump.
bool ValidateClodInput(const char* path, const clodMesh& cm, size_t attrFloats, size_t lockBytes,
                       const char* who)
{
    const char* bad = nullptr;
    if (cm.vertex_count == 0)                     bad = "vertex_count == 0";
    else if (cm.index_count == 0)                 bad = "index_count == 0";
    else if (cm.index_count % 3)                  bad = "index_count not a multiple of 3";
    else if (attrFloats < (size_t)cm.vertex_count * 7) bad = "attribute array shorter than vertex_count";
    else if (lockBytes  < (size_t)cm.vertex_count)     bad = "vertex_lock shorter than vertex_count";
    if (!bad) {
        for (size_t i = 0; i < cm.index_count; ++i)
            if (cm.indices[i] >= cm.vertex_count) { bad = "index >= vertex_count"; break; }
    }
    if (!bad) return true;
    Msg("!![VK Cluster] %s unit REJECTED (%s): verts=%u idx=%u attrs=%u lock=%u %s",
        path, bad, (u32)cm.vertex_count, (u32)cm.index_count, (u32)attrFloats, (u32)lockBytes,
        who ? who : "");
    return false;
}

void ExtractMeshes(vkFVisual* fv, xr_vector<vkFVisual*>& gpuOut, xr_vector<vkFVisual*>& cpuOut)

{
    const u32 t = fv->Type;
    if (t == MT_NORMAL || t == MT_PROGRESSIVE) {
        if (!fv->m_mesh.IsValid() || !fv->m_mesh.p_rm_Vertices || !fv->m_mesh.p_rm_Indices) return;  // not renderable
        WorldMaterial* mat = fv->m_pWorldMaterial;
        // `tessellated` = the material HAS a height texture (every bump-mapped
        // wall!), but the CPU queue only tessellates when r_tess is globally on
        // (RenderQueue::Flush: tessAvail) — otherwise those meshes draw on the
        // very same flat lmap/vlit pipelines the GPU path binds. So when tess
        // is inactive at Build time, claim them for the GPU set: that's most
        // of the building geometry (and the cluster-LOD coverage with it).
        // Flipping r_tess later only takes effect on statics after level reload.
        const bool tessActive = PipelineCache::TessAvailable() && ps_r_tess > 0.5f;
        // isWmark already covers water (it shares the marker), but state it
        // explicitly: water has its OWN pipeline and shader in Pass_Water, and a
        // silent fallthrough here would draw it a second time as opaque geometry.
        const bool gpuOk = mat && !mat->isWmark && !mat->isWater && !(mat->tessellated && tessActive)
                        && (mat->view != VK_NULL_HANDLE || mat->isTerrain);
        if (gpuOk) gpuOut.push_back(fv);
        else {
            // Which clause rejected it. The set is NOT stable across loads of the
            // same level (see the cluster-cache superset note) — these counters are
            // what name the drifting clause instead of leaving it to guesswork.
            if (!mat)                              ++s_cpuWhyNoMat;
            else if (mat->isWater)                 ++s_cpuWhyWater;
            else if (mat->isWmark)                 ++s_cpuWhyWmark;
            else if (mat->tessellated && tessActive) ++s_cpuWhyTess;
            else                                   ++s_cpuWhyNoView;
            cpuOut.push_back(fv);
        }

    }
    // Other leaf types (trees, skeletons, particles) belong to other passes.
}

// ============================================================================
// Cluster split (r_cluster) — Phase 1 of the cluster-LOD system.
// Big static meshes are split at load into ~128-tri meshlets (meshoptimizer)
// so the compute cull works at cluster granularity: a whole building wall no
// longer draws because one corner peeks into the frustum, and the Hi-Z test
// gets small spheres it can actually reject (fragment-sized spheres almost
// never pass). Cluster indices are u16 RELATIVE TO vBase exactly like the
// source IB (level indices are u16-per-slice already), so the draw side needs
// nothing new: same vertex fetch, same first_vertex, just another VkBuffer.
// NOTE: no normal-cone backface culling — every world pipeline rasterizes
// with CULL_MODE_NONE (two-sided), a cone test would drop visible geometry.
// ============================================================================
constexpr u32 kClusterMaxVerts = 64;
constexpr u32 kClusterMaxTris  = 128;   // multiple of 4 (meshopt requirement)

// GPU-readback of a device-local buffer into a host vector — one-shot staging
// copy (pools carry TRANSFER_SRC specifically for this, see rvk_loader). Returns false on any failure.
bool ReadbackBuffer(VkBuffer src, VkDeviceSize size, xr_vector<u8>& out)
{
    if (src == VK_NULL_HANDLE || size == 0) return false;
    CVulkanBuffer stg;
    stg.Create(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
               false, false, /*hostRead*/ true);   // cached: this mapping is READ
    VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) { stg.Destroy(); return false; }
    VkBufferCopy cp{ 0, 0, size };
    vkCmdCopyBuffer(cmd, src, stg.GetHandle(), 1, &cp);
    VulkanHW.EndSingleTimeCommands(cmd);   // waits (one-shot)
    stg.Invalidate();
    void* p = stg.Map();
    if (!p) { stg.Destroy(); return false; }
    out.resize((size_t)size);
    memcpy(out.data(), p, (size_t)size);
    stg.Unmap();
    stg.Destroy();
    return true;
}

// The drawn IB slice = exactly what the CPU path draws: PROGRESSIVE meshes use
// their finest sliding-window slice, not the whole span (the coarse slices
// poke through the fine mesh — same rule as the meta fill / vk_shadow_gpu).
void DrawnSlice(vkFVisual* fv, u32& first, u32& count)
{
    first = fv->m_mesh.iBase; count = fv->m_mesh.iCount;
    if (fv->Type == MT_PROGRESSIVE) {
        auto* pg = static_cast<vkFProgressive*>(fv);
        if (pg->sw_count > 0 && pg->sw_offsets && pg->sw_counts) {
            first = fv->m_mesh.iBase + pg->sw_offsets[0];
            count = pg->sw_counts[0];
        }
    }
}

// Which cluster-meta entries belong to a mesh. Per-mesh DAGs own a contiguous
// run; component DAGs (Phase 2.5) scatter a mesh's entries across clusters
// shared with its neighbors, so it's an explicit index list either way. ib32
// picks the IB the entries reference (u16 rel-vBase vs u32 pool-global).
struct MeshEntries { bool ib32; xr_vector<u32> entries; };

u32 s_statSimplified = 0, s_statStalled = 0;   // DAG diag (reset per BuildClusters)
u32 s_statCompUnits = 0, s_statCompMeshes = 0; // Phase 2.5 diag: component units / meshes merged into them

// ============================================================================
// Cluster DAG disk cache — the clodBuild result depends only on the level
// geometry + build params, so it's baked once to $app_data_root$ and reloaded
// on subsequent loads of the same level (~0.5s parallel bake → ~0). Pattern:
// vk_pipeline_cache. One file per level ("vk_cluster_<crc(levelpath)>.bin"),
// stamped with level.geom size^CRC so edited geometry rebuilds automatically.
// ============================================================================
constexpr u32 kClusterCacheMagic   = 0x4B4C4356u;   // 'VCLK'
constexpr u32 kClusterCacheVersion = 13;            // bump on ANY builder change (v11: Stage B page-ordered blobs; v12: slice 2 vertex repack blob; v13: bake hygiene — self-loop stall entries dropped)

#pragma pack(push, 1)
// v12: pageCount + absolute file offsets of the idx/vertex blobs (the blobs
// are read lazily — the cache file doubles as the page-streaming source).
struct CacheHeader { u32 magic, params, stamp, meshCount, totalMeta, totalIdx16, totalIdx32, pageCount, totalVbBytes; u64 blobOff16, blobOff32, blobOffVb; };
// Stable mesh identity. vbSize/ibSize discriminate the owning pools: vBase/ibFirst are
// offsets INTO a pool, and two different pools can host meshes at equal offsets.
struct CacheMeshKey { u32 vBase, ibFirst, idxCount, vCount, stride, tcOffset, vbSize, ibSize; };
struct CacheMeshRec { CacheMeshKey key; u32 ib32, entryCount; };  // + entryCount inline u32 meta indices follow
#pragma pack(pop)

u32 ClusterParamsHash()
{
    u32 h = kClusterCacheVersion * 2166136261u;
    h = (h ^ (u32)kClusterMaxTris)     * 16777619u;
    h = (h ^ (u32)ps_r_cluster_tris)   * 16777619u;
    h = (h ^ (ps_r_cluster_merge ? 1u : 0u)) * 16777619u;
    return h;
}

CacheMeshKey MakeMeshKey(vkFVisual* fv)
{
    u32 first, count; DrawnSlice(fv, first, count);
    return { fv->m_mesh.vBase, first, count, fv->m_mesh.vCount, fv->m_mesh.vStride, fv->m_mesh.tcOffset,
             (u32)fv->m_mesh.p_rm_Vertices->GetSize(), (u32)fv->m_mesh.p_rm_Indices->GetSize() };
}

void ClusterCachePath(string_path& fn, u32& stamp)
{
    string_path lp{};
    FS.update_path(lp, "$level$", "");
    string64 name;
    xr_sprintf(name, "vk_cluster_%08x.bin", crc32(lp, (u32)xr_strlen(lp)));
    FS.update_path(fn, "$app_data_root$", name);

    stamp = 0;
    if (IReader* r = FS.r_open("$level$", "level.geom")) {
        stamp = (u32)r->length() ^ crc32(r->pointer(), (u32)_min<size_t>(65536, (size_t)r->length()));
        FS.r_close(r);
    }
}

// Cache hit = every eligible mesh has a record with a bit-identical key.
// v11: read with plain stdio (NOT FS.r_open — that pulls the whole 400 MB file
// into RAM; we only parse header/records/pages/meta and leave the page-ordered
// idx blobs ON DISK: the same file is the runtime page-streaming source).
// ib_first in the stored meta is page-local; the page directory + blob offsets
// land in s_pageDir / s_cachePath / s_blobOff*.
bool LoadClusterCache(const xr_vector<vkFVisual*>& eligMeshes,
                      xr_vector<GpuMeshMeta>& outMeta, xr_vector<u16>& outIdx16, xr_vector<u32>& outIdx32,
                      xr_map<vkFVisual*, MeshEntries>& outRanges)
{
    (void)outIdx16; (void)outIdx32;   // blobs stay on disk (v11)
    string_path fn; u32 stamp;
    ClusterCachePath(fn, stamp);
    FILE* f = fopen(XR_OSPATH(fn), "rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const u64 fileSize = (u64)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    auto rd = [&](void* dst, size_t bytes) { return fread(dst, 1, bytes, f) == bytes; };

    bool ok = false;
    const char* missWhy = nullptr;   // every non-hit path names itself — misses must be diagnosable from the log
    do {
        CacheHeader h;
        if (!rd(&h, sizeof(h))) { missWhy = "truncated header"; break; }
        if (h.magic != kClusterCacheMagic) { missWhy = "bad magic"; break; }
        if (h.params != ClusterParamsHash()) { missWhy = "params/version changed"; break; }
        if (h.stamp != stamp) { missWhy = "level.geom stamp changed"; break; }
        // SUPERSET is a hit, not a miss. The eligible set is not a property of the
        // level alone: WorldMaterial flags (isWmark/isWater/glass/emissive) are
        // upgraded in place and the material cache outlives a level change, so
        // re-entering a level classifies a handful of meshes differently than the
        // first visit did — measured: 84 of 100896 move from the GPU set to the CPU
        // set, every time. Records are matched by KEY below and duplicates are
        // already legal, so a cache holding MORE meshes than we ask for serves all
        // of them correctly; the per-key loop is what actually proves the hit.
        // Insisting on equality here meant a 27 s rebake on every repeat load, and
        // that rebake then SAVED the smaller set — poisoning the cache for the next
        // fresh process, which then rebaked too.
        if (h.meshCount < (u32)eligMeshes.size()) { missWhy = "cache has fewer meshes than the level needs"; break; }


        // Records carry inline entry lists (variable length) — read sequentially.
        struct Rec { CacheMeshRec r; xr_vector<u32> entries; };
        xr_vector<Rec> recs(h.meshCount);
        bool match = true;
        for (u32 i = 0; i < h.meshCount && match; ++i) {
            if (!rd(&recs[i].r, sizeof(CacheMeshRec))) { match = false; break; }
            const u32 n = recs[i].r.entryCount;
            if (n > h.totalMeta) { match = false; break; }
            recs[i].entries.resize(n);
            if (n && !rd(recs[i].entries.data(), n * sizeof(u32))) { match = false; break; }
            for (u32 e : recs[i].entries)
                if (e >= h.totalMeta) { match = false; break; }
        }
        if (!match) { missWhy = "corrupt records"; break; }
        if (h.pageCount == 0 || h.pageCount > 1u << 20) { missWhy = "bad page table"; break; }
        if (h.blobOff16 + (u64)h.totalIdx16 * sizeof(u16) > fileSize
         || h.blobOff32 + (u64)h.totalIdx32 * sizeof(u32) > fileSize
         || h.blobOffVb + (u64)h.totalVbBytes > fileSize) { missWhy = "truncated blobs"; break; }
        s_pageDir.resize(h.pageCount);
        if (!rd(s_pageDir.data(), h.pageCount * sizeof(ClusterStream::PageRec))) { missWhy = "truncated page table"; break; }

        // Mesh extraction order is pointer-sort — NOT stable across runs — so
        // records match by KEY, not by position. Identical keys are legal: two
        // visuals can reference the SAME geometry slice (same pool, offsets,
        // counts ⇒ same triangles ⇒ same DAG), and one record serves them all —
        // Pripyat-sized levels have thousands of such shares, and treating them
        // as a miss meant an 18s rebake every single load.
        auto keyHash = [](const CacheMeshKey& k) -> u64 {
            u64 hh = 1469598103934665603ull;
            const u8* b = (const u8*)&k;
            for (size_t i = 0; i < sizeof(k); ++i) { hh ^= b[i]; hh *= 1099511628211ull; }
            return hh;
        };
        std::unordered_map<u64, u32> byKey;
        byKey.reserve(h.meshCount);
        for (u32 i = 0; i < h.meshCount && match; ++i) {
            auto ins = byKey.emplace(keyHash(recs[i].r.key), i);
            if (!ins.second && memcmp(&recs[i].r.key, &recs[ins.first->second].r.key, sizeof(CacheMeshKey)) != 0)
                match = false;   // true 64-bit hash collision of DIFFERENT keys — bail, don't mis-serve
        }
        if (!match) { missWhy = "key hash collision"; break; }
        xr_vector<u32> recOf(eligMeshes.size());
        for (u32 i = 0; i < (u32)eligMeshes.size(); ++i) {
            const CacheMeshKey k = MakeMeshKey(eligMeshes[i]);
            auto it = byKey.find(keyHash(k));
            if (it == byKey.end() || memcmp(&k, &recs[it->second].r.key, sizeof(k)) != 0) { match = false; break; }
            recOf[i] = it->second;
        }
        if (!match) { missWhy = "mesh key not in cache"; break; }

        outMeta.resize(h.totalMeta);
        if (!rd(outMeta.data(), (size_t)h.totalMeta * sizeof(GpuMeshMeta))) { missWhy = "truncated meta"; break; }
        for (u32 i = 0; i < (u32)eligMeshes.size(); ++i) {
            Rec& rec = recs[recOf[i]];
            if (rec.r.entryCount)   // COPY entries — a shared-slice record can serve several visuals
                outRanges.emplace(eligMeshes[i], MeshEntries{ rec.r.ib32 != 0, rec.entries });
        }
        xr_strcpy(s_cachePath, fn);
        s_blobOff16 = h.blobOff16;
        s_blobOff32 = h.blobOff32;
        s_blobOffVb = h.blobOffVb;
        ok = true;
    } while (false);

    fclose(f);
    if (!ok) {
        s_pageDir.clear();
        Msg("![VK Cluster] cache MISS (%s) — full rebake", missWhy ? missWhy : "?");
    }
    return ok;
}

// v12 layout: header | records+entry lists | page table | meta | idx16 | idx32
// | vertex blob. The blob offsets are precomputed and stored in the header —
// the cache file is also the RUNTIME page-streaming source, so pages must be
// seekable directly. On success s_cachePath / s_blobOff* point the streamer at
// the fresh file.
void SaveClusterCache(const xr_vector<vkFVisual*>& eligMeshes,
                      const xr_vector<GpuMeshMeta>& meta, const xr_vector<u16>& idx16, const xr_vector<u32>& idx32,
                      const xr_vector<u8>& vbBlob,
                      const xr_map<vkFVisual*, MeshEntries>& ranges,
                      const xr_vector<ClusterStream::PageRec>& pages)
{
    string_path fn; u32 stamp;
    ClusterCachePath(fn, stamp);
    s_cachePath[0] = 0;   // set only on a successful write
    IWriter* w = FS.w_open(fn);
    if (!w) { Msg("![VK Cluster] cache write failed (%s)", fn); return; }

    size_t listBytes = 0;
    for (vkFVisual* fv : eligMeshes) {
        auto it = ranges.find(fv);
        if (it != ranges.end()) listBytes += it->second.entries.size() * sizeof(u32);
    }
    const u64 metaOff = sizeof(CacheHeader) + eligMeshes.size() * sizeof(CacheMeshRec) + listBytes
                      + pages.size() * sizeof(ClusterStream::PageRec);
    const u64 off16 = metaOff + meta.size() * sizeof(GpuMeshMeta);
    const u64 off32 = off16 + idx16.size() * sizeof(u16);
    const u64 offVb = off32 + idx32.size() * sizeof(u32);

    CacheHeader h{ kClusterCacheMagic, ClusterParamsHash(), stamp,
                   (u32)eligMeshes.size(), (u32)meta.size(), (u32)idx16.size(), (u32)idx32.size(),
                   (u32)pages.size(), (u32)vbBlob.size(), off16, off32, offVb };
    w->w(&h, sizeof(h));
    for (vkFVisual* fv : eligMeshes) {
        CacheMeshRec rec{ MakeMeshKey(fv), 0, 0 };
        auto it = ranges.find(fv);
        if (it != ranges.end()) { rec.ib32 = it->second.ib32 ? 1u : 0u; rec.entryCount = (u32)it->second.entries.size(); }
        w->w(&rec, sizeof(rec));
        if (rec.entryCount) w->w(it->second.entries.data(), rec.entryCount * sizeof(u32));
    }
    if (!pages.empty()) w->w(pages.data(), (u32)(pages.size() * sizeof(ClusterStream::PageRec)));
    if (!meta.empty())  w->w(meta.data(),  (u32)(meta.size()  * sizeof(GpuMeshMeta)));
    if (!idx16.empty()) w->w(idx16.data(), (u32)(idx16.size() * sizeof(u16)));
    if (!idx32.empty()) w->w(idx32.data(), (u32)(idx32.size() * sizeof(u32)));
    if (!vbBlob.empty()) w->w(vbBlob.data(), (u32)vbBlob.size());
    FS.w_close(w);
    xr_strcpy(s_cachePath, fn);
    s_blobOff16 = off16;
    s_blobOff32 = off32;
    s_blobOffVb = offVb;
    Msg("[VK Cluster] cache saved: %u meshes, %u pages, %.1f MB (%.1f MB vertex blob)", (u32)eligMeshes.size(), (u32)pages.size() - 1,
        double(offVb + vbBlob.size()) / (1024.0 * 1024.0), double(vbBlob.size()) / (1024.0 * 1024.0));
}

// Split eligible meshes into meshlets with a LOD DAG. Two build paths:
//  - per-mesh (Phase 2): one clodBuild per mesh; u16 indices rel. to vBase.
//  - component (Phase 2.5, r_cluster_merge): solid fragments whose dilated
//    AABBs touch within one VB pool merge into ONE clodBuild — the simplifier
//    sees the building as a single surface, so a low-poly door DISSOLVES into
//    its facade at distance instead of collapsing onto itself. Each cluster's
//    triangles are binned back to their source mesh (by v0) → one entry per
//    (cluster × mesh); entries share the cluster's LOD spheres/errors, so the
//    cluster still flips atomically (crack-free). Cross-mesh triangles can't
//    fit a u16 window over one vBase → component clusters use pool-global u32
//    indices with first_vertex = 0 (the cull/draw need nothing new for that).
// Fills outMeta (per-entry data, group patched later), outIdx16/outIdx32 (the
// shared cluster IBs) and outRanges (mesh → its entry list). Terrain is
// excluded: tiles are already grid-partitioned, and the prepass TES displaces
// them (spheres would lie).
void BuildClusters(const xr_vector<vkFVisual*>& meshes,
                   xr_vector<GpuMeshMeta>& outMeta, xr_vector<u16>& outIdx16, xr_vector<u32>& outIdx32,
                   xr_vector<u8>& outVbBlob,
                   xr_map<vkFVisual*, MeshEntries>& outRanges)
{
    const u32 minTris = (u32)_max(ps_r_cluster_tris, (int)kClusterMaxTris);
    s_statSimplified = 0; s_statStalled = 0; s_statCompUnits = 0; s_statCompMeshes = 0;
    s_pageDir.clear(); s_cachePath[0] = 0; s_blobOff16 = s_blobOff32 = s_blobOffVb = 0;

    // Eligibility (no GPU readback needed) — the exact predicate the cache was
    // saved under; buffer-content guards run only on the build path below.
    // With component merge on, SOLID meshes of ANY size are candidates (a
    // 100-tri door must JOIN the building's DAG to dissolve into it); AT
    // meshes and merge-off solids keep the size filter — alone, a small mesh
    // gains nothing from clustering below ~2 meshlets.
    xr_vector<vkFVisual*> eligMeshes;
    for (vkFVisual* fv : meshes) {
        if (fv->m_pWorldMaterial->isTerrain) continue;
        if (fv->m_mesh.vStride < 12 || fv->m_mesh.vCount == 0) continue;
        u32 first, count; DrawnSlice(fv, first, count);
        if (count < 3) continue;
        const bool mergeCand = ps_r_cluster_merge && fv->m_pWorldMaterial->alphaRef < 0.f;
        if (!mergeCand && count < minTris * 3) continue;
        eligMeshes.push_back(fv);
    }
    if (eligMeshes.empty()) return;

    // Disk cache: on a hit the whole readback + clodBuild is skipped.
    if (ps_r_cluster_cache && LoadClusterCache(eligMeshes, outMeta, outIdx16, outIdx32, outRanges)) {

        Msg("[VK Cluster] DAG loaded from cache: %u meshes, %u entries", (u32)outRanges.size(), (u32)outMeta.size());
        return;
    }

    // Read back each unique VB/IB pool ONCE (host copies), then slice in memory —
    // per-mesh readback would mean thousands of queue round-trips.
    xr_map<VkBuffer, xr_vector<u8>> vbData, ibData;
    for (vkFVisual* fv : eligMeshes) {
        vbData.emplace(fv->m_mesh.p_rm_Vertices->GetHandle(), xr_vector<u8>());
        ibData.emplace(fv->m_mesh.p_rm_Indices->GetHandle(),  xr_vector<u8>());
    }
    for (vkFVisual* fv : eligMeshes) {   // fill (buffer sizes come from the owning CVulkanBuffer)
        auto vit = vbData.find(fv->m_mesh.p_rm_Vertices->GetHandle());
        if (vit != vbData.end() && vit->second.empty())
            if (!ReadbackBuffer(vit->first, fv->m_mesh.p_rm_Vertices->GetSize(), vit->second))
                { Msg("![VK Cluster] VB readback failed — mesh(es) stay unclustered"); vbData.erase(vit); }
        auto iit = ibData.find(fv->m_mesh.p_rm_Indices->GetHandle());
        if (iit != ibData.end() && iit->second.empty())
            if (!ReadbackBuffer(iit->first, fv->m_mesh.p_rm_Indices->GetSize(), iit->second))
                { Msg("![VK Cluster] IB readback failed — mesh(es) stay unclustered"); ibData.erase(iit); }
    }

    // Full guards (buffer contents) → the build list with resolved pointers.
    struct Elig { vkFVisual* fv; const float* pos; const u16* idx; u32 idxCount; };
    xr_vector<Elig> elig;
    for (vkFVisual* fv : eligMeshes) {
        const auto& mesh = fv->m_mesh;
        u32 ibFirst, idxCount; DrawnSlice(fv, ibFirst, idxCount);

        auto vit = vbData.find(mesh.p_rm_Vertices->GetHandle());
        auto iit = ibData.find(mesh.p_rm_Indices->GetHandle());
        if (vit == vbData.end() || iit == ibData.end()) continue;
        if ((size_t)(ibFirst + idxCount) * sizeof(u16) > iit->second.size()) continue;
        if ((size_t)(mesh.vBase + mesh.vCount) * mesh.vStride > vit->second.size()) continue;

        const float* pos = (const float*)(vit->second.data() + (size_t)mesh.vBase * mesh.vStride);
        const u16*   idx = (const u16*)iit->second.data() + ibFirst;

        // Corrupt-data guard: indices must stay inside the mesh's vertex slice.
        bool ok = true;
        for (u32 i = 0; i < idxCount; ++i) if (idx[i] >= mesh.vCount) { ok = false; break; }
        if (!ok) { Msg("![VK Cluster] mesh has out-of-slice indices — skipped"); continue; }

        elig.push_back({ fv, pos, idx, idxCount });
    }
    if (elig.empty()) return;

    // ---- Build units --------------------------------------------------------
    // A unit = one clodBuild call. With r_cluster_merge, solid fragments whose
    // dilated AABBs overlap within one VB pool (same vb/stride/tcOffset — the
    // draw binds one VB per group and the attr layout must be uniform) union
    // into COMPONENT units; oversized components split along their longest
    // axis at a triangle cap (keeps the bake parallel and clod memory bounded).
    // Everything else (AT meshes, isolated solids) stays per-mesh.
    struct Unit { xr_vector<u32> members; bool comp; u32 tris; };
    xr_vector<Unit> units;
    constexpr u32 kNoUnit = 0xFFFFFFFFu;
    xr_vector<u32> unitOf(elig.size(), kNoUnit);

    if (ps_r_cluster_merge) {
        xr_vector<u32> uf(elig.size());
        for (u32 i = 0; i < (u32)uf.size(); ++i) uf[i] = i;
        auto find = [&](u32 x) { while (uf[x] != x) { uf[x] = uf[uf[x]]; x = uf[x]; } return x; };

        xr_map<std::tuple<VkBuffer, u32, u32>, xr_vector<u32>> poolsOf;
        for (u32 i = 0; i < (u32)elig.size(); ++i) {
            vkFVisual* fv = elig[i].fv;
            if (fv->m_pWorldMaterial->alphaRef >= 0.f) continue;   // AT: per-mesh path
            poolsOf[std::make_tuple(fv->m_mesh.p_rm_Vertices->GetHandle(),
                                    (u32)fv->m_mesh.vStride, (u32)fv->m_mesh.tcOffset)].push_back(i);
        }

        constexpr float kDilate = 0.5f;   // fragments of one building adjoin exactly; dilation absorbs authoring slack
        for (auto& kv : poolsOf) {
            const xr_vector<u32>& list = kv.second;
            for (size_t a = 0; a < list.size(); ++a) {
                const Fbox& ba = elig[list[a]].fv->vis.box;
                for (size_t b = a + 1; b < list.size(); ++b) {
                    const u32 ra = find(list[a]), rb = find(list[b]);
                    if (ra == rb) continue;
                    const Fbox& bb = elig[list[b]].fv->vis.box;
                    if (ba.min.x - kDilate <= bb.max.x && bb.min.x - kDilate <= ba.max.x &&
                        ba.min.y - kDilate <= bb.max.y && bb.min.y - kDilate <= ba.max.y &&
                        ba.min.z - kDilate <= bb.max.z && bb.min.z - kDilate <= ba.max.z)
                        uf[rb] = ra;
                }
            }
        }

        xr_map<u32, xr_vector<u32>> comps;
        for (auto& kv : poolsOf)
            for (u32 i : kv.second)
                comps[find(i)].push_back(i);

        // Recursive spatial split: chop every component along its longest axis
        // until BOTH the tri cap and the EXTENT cap hold. The extent cap is
        // what keeps the DAG usable: monotonic containment (Karis) inflates
        // deep-level group spheres toward the unit's bounding sphere, and the
        // parent test needs the camera OUTSIDE that sphere (dist - r). AABB
        // chaining (fence -> shed -> house...) builds map-sized components —
        // measured parent spheres avg 187m / max 1197m on Кордон = the cut
        // froze at full detail everywhere. ~48m keeps a building whole while
        // deep spheres stay escapable a few dozen meters out.
        constexpr u32   kCompMaxTris   = 200000;
        constexpr float kCompMaxExtent = 48.f;
        for (auto& kv : comps) {
            xr_vector<xr_vector<u32>> stack;
            stack.push_back(std::move(kv.second));
            while (!stack.empty()) {
                xr_vector<u32> cur = std::move(stack.back());
                stack.pop_back();
                if (cur.size() < 2) continue;   // singleton → per-mesh path below

                u32 tris = 0;
                Fbox all; all.invalidate();
                for (u32 i : cur) { tris += elig[i].idxCount / 3; all.merge(elig[i].fv->vis.box); }
                Fvector ext; all.getsize(ext);
                const float maxExt = _max(ext.x, _max(ext.y, ext.z));

                if (tris <= kCompMaxTris && maxExt <= kCompMaxExtent) {
                    Unit u{}; u.comp = true; u.tris = tris;
                    u.members = std::move(cur);
                    for (u32 m : u.members) unitOf[m] = (u32)units.size();
                    units.push_back(std::move(u));
                    continue;
                }

                const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
                std::sort(cur.begin(), cur.end(), [&](u32 a, u32 b) {
                    const Fbox& A = elig[a].fv->vis.box; const Fbox& B = elig[b].fv->vis.box;
                    return (&A.min.x)[axis] + (&A.max.x)[axis] < (&B.min.x)[axis] + (&B.max.x)[axis];
                });
                // Prefer cutting at the largest spatial GAP along the axis — a gap
                // wider than the AABB dilate means nothing is welded across it, so
                // the cut adds no cross-chunk locks and buildings stay whole. Fall
                // back to the tri-count median when the run is contiguous. Both
                // halves are non-empty by construction, so the recursion ends.
                u32 half = 0;
                {
                    float bestGap = 0.75f;   // > kDilate: separable by construction
                    float runMax = -1e30f;
                    for (u32 k = 0; k < (u32)cur.size(); ++k) {
                        const Fbox& B = elig[cur[k]].fv->vis.box;
                        if (k && (&B.min.x)[axis] - runMax > bestGap) { bestGap = (&B.min.x)[axis] - runMax; half = k; }
                        runMax = _max(runMax, (&B.max.x)[axis]);
                    }
                }
                if (half == 0) {
                    u32 acc = 0;
                    for (u32 k = 0; k < (u32)cur.size() - 1 && acc * 2 < tris; ++k) { acc += elig[cur[k]].idxCount / 3; half = k + 1; }
                    if (half == 0) half = 1;
                }
                xr_vector<u32> right(cur.begin() + half, cur.end());
                cur.resize(half);
                stack.push_back(std::move(cur));
                stack.push_back(std::move(right));
            }
        }
        s_statCompUnits = (u32)units.size();
        for (const Unit& u : units) s_statCompMeshes += (u32)u.members.size();
    }

    // Not merged: per-mesh unit if big enough, else PLAIN (stays a whole-mesh
    // entry — but its vertices still pin shared seams below: it never moves).
    for (u32 i = 0; i < (u32)elig.size(); ++i) {
        if (unitOf[i] != kNoUnit) continue;
        if (elig[i].idxCount < minTris * 3) continue;
        Unit u{}; u.comp = false; u.tris = elig[i].idxCount / 3; u.members.push_back(i);
        unitOf[i] = (u32)units.size();
        units.push_back(std::move(u));
    }

    // ---- Attach pass ---------------------------------------------------------
    // The extent split can orphan a small mesh from its host surface (a window
    // frame separated from its wall when the wall's chunk got cut) — and an
    // orphan below the per-mesh size gate becomes a PLAIN entry: full detail
    // forever, unable to dissolve into anything (this was exactly the "frames
    // never dissolve at any distance" report: ~1500 orphans, all parentError
    // INF). Reattach every leftover solid merge candidate to the TOUCHING unit
    // whose bounds grow the least (its wall — not the 500m rail line that also
    // happens to touch), converting single-mesh hosts into components. Hosts
    // must be all-solid and share the orphan's pool (vb/stride/tcOffset).
    if (ps_r_cluster_merge) {
        struct HostInfo { Fbox box; bool solid; VkBuffer vb; u32 stride, tc; };
        xr_vector<HostInfo> hosts(units.size());
        for (u32 u = 0; u < (u32)units.size(); ++u) {
            HostInfo& h = hosts[u];
            h.box.invalidate(); h.solid = true;
            vkFVisual* f0 = elig[units[u].members[0]].fv;
            h.vb = f0->m_mesh.p_rm_Vertices->GetHandle(); h.stride = f0->m_mesh.vStride; h.tc = f0->m_mesh.tcOffset;
            for (u32 m : units[u].members) {
                h.box.merge(elig[m].fv->vis.box);
                if (elig[m].fv->m_pWorldMaterial->alphaRef >= 0.f) h.solid = false;
            }
        }
        constexpr float kAttachDilate = 0.5f;
        auto overlaps = [&](const Fbox& a, const Fbox& b) {
            return a.min.x - kAttachDilate <= b.max.x && b.min.x - kAttachDilate <= a.max.x
                && a.min.y - kAttachDilate <= b.max.y && b.min.y - kAttachDilate <= a.max.y
                && a.min.z - kAttachDilate <= b.max.z && b.min.z - kAttachDilate <= a.max.z;
        };
        u32 attached = 0;
        for (u32 i = 0; i < (u32)elig.size(); ++i) {
            if (unitOf[i] != kNoUnit) continue;
            vkFVisual* fv = elig[i].fv;
            if (fv->m_pWorldMaterial->alphaRef >= 0.f) continue;   // AT never joins components
            const Fbox& bi = fv->vis.box;
            u32 best = kNoUnit; float bestExt = 1e30f;
            for (u32 u = 0; u < (u32)units.size(); ++u) {
                const HostInfo& h = hosts[u];
                if (!h.solid || h.vb != fv->m_mesh.p_rm_Vertices->GetHandle()
                    || h.stride != fv->m_mesh.vStride || h.tc != fv->m_mesh.tcOffset) continue;
                if (!overlaps(bi, h.box)) continue;   // cheap unit-bounds reject
                bool touch = false;
                for (u32 m : units[u].members)
                    if (overlaps(bi, elig[m].fv->vis.box)) { touch = true; break; }
                if (!touch) continue;
                Fbox mb = h.box; mb.merge(bi);
                Fvector me; mb.getsize(me);
                const float ext = _max(me.x, _max(me.y, me.z));
                if (ext < bestExt) { bestExt = ext; best = u; }
            }
            if (best == kNoUnit) continue;   // genuinely isolated → plain
            unitOf[i] = best;
            units[best].members.push_back(i);
            units[best].tris += elig[i].idxCount / 3;
            units[best].comp = units[best].members.size() >= 2;
            hosts[best].box.merge(bi);
            ++attached;
        }
        s_statCompMeshes += attached;
    }
    if (units.empty()) {   // nothing clusterable — save the (empty) result so reloads skip the readback
        s_pageDir.clear();
        s_pageDir.push_back({ ClusterStream::kPagePinned, 0, 0, 0, 0 });   // identity page only
        if (ps_r_cluster_cache)
            SaveClusterCache(eligMeshes, outMeta, outIdx16, outIdx32, outVbBlob, outRanges, s_pageDir);
        return;
    }

    // ---- Cross-UNIT seam pins -----------------------------------------------
    // Vertices whose POSITION is shared between two COMPONENT units must not
    // move: those seams are welded surfaces the extent-cap split cut in half.
    // Only component↔component positions pin — pinning against AT/plain meshes
    // (Phase 2 behavior) hard-locked the ring around every window (glass pins
    // frame pins wall...) and froze 16% of component verts → stalled groups →
    // the DAG could never ascend to the levels where prune removes the frames.
    // Drift against unpinned neighbors is bounded by the error metric, exactly
    // like every other collapse (Nanite does no cross-object welding at all).
    constexpr u32 kShared = 0xFFFFFFFFu;
    auto posKey = [](const float* p) -> u64 {
        u64 h = 1469598103934665603ull;
        const u8* b = (const u8*)p;
        for (int i = 0; i < 12; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        return h;
    };
    std::unordered_map<u64, u32> posOwner;
    posOwner.reserve(1u << 20);
    for (u32 m = 0; m < (u32)elig.size(); ++m) {
        if (unitOf[m] == kNoUnit || !units[unitOf[m]].comp) continue;   // only component units pin
        const Elig& e = elig[m];
        const u8* vb = (const u8*)e.pos;
        const u32 stride = e.fv->m_mesh.vStride;
        const u32 owner = unitOf[m];
        for (u32 v = 0; v < e.fv->m_mesh.vCount; ++v) {
            auto ins = posOwner.emplace(posKey((const float*)(vb + (size_t)v * stride)), owner);
            if (!ins.second && ins.first->second != owner) ins.first->second = kShared;
        }
    }

    // ---- clodBuild per unit ---------------------------------------------------
    // The hierarchy, boundary locking (position-welded — lmap-UV seam duplicates
    // count as one vertex), permissive simplification with UV-seam protect bits
    // and monotonic error propagation all come from clusterlod (the reference
    // Nanite-style builder shipped with meshopt). Units are independent, so the
    // build fans out over a thread pool, biggest units first (components dwarf
    // single meshes — the pool drains them while small units backfill).
    xr_vector<u32> buildOrder(units.size());
    for (u32 i = 0; i < (u32)buildOrder.size(); ++i) buildOrder[i] = i;
    std::sort(buildOrder.begin(), buildOrder.end(), [&](u32 a, u32 b) { return units[a].tris > units[b].tris; });

    // Per-thread outputs, merged (with offset fixups) after the join so the
    // final meta/IB layout is identical to a serial build.
    struct ThreadOut {
        xr_vector<GpuMeshMeta> meta;
        xr_vector<u16>         idx16;
        xr_vector<u32>         idx32;
        xr_vector<std::pair<vkFVisual*, MeshEntries>> ranges;
        u32 simplified = 0, stalled = 0;
        // Component diagnostics ("why doesn't the building LOD?"):
        u32 compBuilt = 0, compFlat = 0;          // units built / units whose DAG never left level 0
        u32 l0Groups = 0, l0Stalled = 0;          // level-0 groups: total / terminal (stuck at full detail)
        u64 compVerts = 0, lockVerts = 0, protVerts = 0;   // component verts: total / hard-locked / UV-protected
        u32 flatWorstTris = 0, flatWorstMeshes = 0;        // biggest flat unit (for the log)
        xr_vector<std::pair<u32, Fvector>> flatUnits;      // (tris, world centre) of every flat unit
    };
    const u32 nThreads = _min(8u, _max(1u, (u32)std::thread::hardware_concurrency()));
    xr_vector<ThreadOut> touts(nThreads);
    std::atomic<u32> cursor{ 0 };

    auto worker = [&](u32 t) {
        ThreadOut& out = touts[t];
        xr_vector<unsigned int>  idxIn;
        xr_vector<float>         attrs;    // 7 floats/vert: nx ny nz u0 v0 u1 v1
        xr_vector<unsigned char> vlock;
        xr_vector<float>         cpos;     // component: compact float3 positions
        xr_vector<u32>           meshOfV, globalOf;   // component: compact vert → member idx / pool-global id
        xr_vector<u64>           pkeys;    // component: compact vert → position hash
        xr_vector<std::pair<u32, u32>> bins;          // component emit scratch: (member, tri)
        std::unordered_map<u64, u32> canon;           // component: position hash → first compact vert
        xr_vector<clodBounds> groups;
        const float attrWeights[3] = { 0.5f, 0.5f, 0.5f };   // normals in the error metric; UVs protect-only

        // Attributes are only compared/weighted — absolute scale is irrelevant,
        // so raw SHORT2 UVs and unorm-byte normals go in undecoded-but-consistent.
        auto vertexAttrs = [](const u8* vp, u32 stride, u32 tcOffset, float* a) {
            a[0] = vp[12] / 255.f; a[1] = vp[13] / 255.f; a[2] = vp[14] / 255.f;   // packed normal
            const s16* uv0 = (const s16*)(vp + tcOffset);
            a[3] = (float)uv0[0]; a[4] = (float)uv0[1];
            if (tcOffset == 24 && stride >= 32) {   // lmap layout: 2nd UV @28
                const s16* uv1 = (const s16*)(vp + 28);
                a[5] = (float)uv1[0]; a[6] = (float)uv1[1];
            } else { a[5] = a[6] = 0.f; }
        };

        auto baseConfig = []() {
            clodConfig cfg = clodDefaultConfig(kClusterMaxTris);
            cfg.optimize_bounds = true;            // tight per-cluster cull spheres
            // NO sloppy fallback: vertex-clustering "simplification" is what folds
            // low-poly doors into triangles while under-reporting the error. Groups
            // it would have rescued now stay roots — full detail forever, no LOD,
            // but also no visible breakage. X-Ray content is low-poly; quality wins.
            cfg.simplify_fallback_sloppy = false;
            // PRUNE is what actually dissolves window frames/sills/trims: they are
            // separate SHELLS dropped into wall openings, not welded to the wall —
            // edge collapses can never merge disconnected shells, so those groups
            // stalled at full detail forever (health view: red frames everywhere).
            // Pruning removes a small shell outright once its size fits the error
            // budget, and that size lands in the reported error → the cut swaps to
            // the pruned parent exactly when the frame is ~subpixel.
            cfg.simplify_prune = true;
            return cfg;
        };

        // Shared cluster meta prototype. Parent test = the group this cluster is
        // a MEMBER of (drawn only while that group's simplified error projects
        // over the threshold); self test = the group whose simplification
        // PRODUCED this cluster.
        auto protoMeta = [&groups](const clodGroup& g, const clodCluster& c) {
            GpuMeshMeta e{};
            e.sphere_P.set(c.bounds.center[0], c.bounds.center[1], c.bounds.center[2]);
            e.sphere_R = c.bounds.radius;
            e.lodParent.set(g.simplified.center[0], g.simplified.center[1], g.simplified.center[2], g.simplified.radius);
            e.parentError = _min(g.simplified.error, kErrInf);
            if (c.refined >= 0) {
                const clodBounds& rb = groups[c.refined];
                e.lodSelf.set(rb.center[0], rb.center[1], rb.center[2], rb.radius);
                e.selfError = _min(rb.error, kErrInf);
            } else {
                e.lodSelf.set(c.bounds.center[0], c.bounds.center[1], c.bounds.center[2], c.bounds.radius);
                e.selfError = 0.f;   // original geometry
            }
            return e;
        };

        for (;;) {
            const u32 uIdx = cursor.fetch_add(1);
            if (uIdx >= (u32)buildOrder.size()) break;
            const Unit& un = units[buildOrder[uIdx]];
            groups.clear();

            if (!un.comp) {
                // ---- per-mesh DAG (Phase 2 path, u16 rel-vBase) ----
                const Elig& e = elig[un.members[0]];
                const auto& mesh = e.fv->m_mesh;

                idxIn.resize(e.idxCount);
                for (u32 i = 0; i < e.idxCount; ++i) idxIn[i] = e.idx[i];

                attrs.resize((size_t)mesh.vCount * 7);
                vlock.assign(mesh.vCount, 0);
                const u8* vb = (const u8*)e.pos;
                for (u32 v = 0; v < mesh.vCount; ++v) {
                    const u8* vp = vb + (size_t)v * mesh.vStride;
                    vertexAttrs(vp, mesh.vStride, mesh.tcOffset, &attrs[(size_t)v * 7]);
                    auto sh = posOwner.find(posKey((const float*)vp));   // read-only after the seam pass
                    if (sh != posOwner.end() && sh->second == kShared) vlock[v] = meshopt_SimplifyVertex_Lock;
                }

                clodConfig cfg = baseConfig();
                clodMesh cm{};
                cm.indices                 = idxIn.data();
                cm.index_count             = e.idxCount;
                cm.vertex_count            = mesh.vCount;
                cm.vertex_positions        = e.pos;
                cm.vertex_positions_stride = mesh.vStride;
                cm.vertex_attributes       = attrs.data();
                cm.vertex_attributes_stride = 7 * sizeof(float);
                cm.vertex_lock             = vlock.data();
                cm.attribute_weights       = attrWeights;
                cm.attribute_count         = 3;
                cm.attribute_protect_mask  = (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6);   // both UV sets

                if (!ValidateClodInput("per-mesh", cm, attrs.size(), vlock.size(),
                                       e.fv->dbg_name.c_str() ? e.fv->dbg_name.c_str() : "?"))
                    continue;

                const u32    metaFirst = (u32)out.meta.size();

                const size_t idxFirst  = out.idx16.size();
                u32 sim = 0, stall = 0;
                clodBuild(cfg, cm, [&](clodGroup g, const clodCluster* cs, size_t n) -> int {
                    const int id = (int)groups.size();
                    groups.push_back(g.simplified);
                    if (g.simplified.error >= FLT_MAX) ++stall; else ++sim;
                    for (size_t i = 0; i < n; ++i) {
                        const clodCluster& c = cs[i];
                        GpuMeshMeta e2 = protoMeta(g, c);
                        e2.index_count  = (u32)c.index_count;
                        e2.ib_first     = (u32)out.idx16.size();
                        e2.first_vertex = mesh.vBase;
                        for (size_t j = 0; j < c.index_count; ++j) out.idx16.push_back((u16)c.indices[j]);
                        out.meta.push_back(e2);
                    }
                    return id;
                });
                const u32 emitted = (u32)out.meta.size() - metaFirst;
                if (emitted < 2) {   // degenerate (single cluster) — the plain path is equal & cheaper
                    out.meta.resize(metaFirst);
                    out.idx16.resize(idxFirst);
                    continue;
                }
                out.simplified += sim; out.stalled += stall;
                MeshEntries me{ false, {} };
                me.entries.reserve(emitted);
                for (u32 k = 0; k < emitted; ++k) me.entries.push_back(metaFirst + k);
                out.ranges.emplace_back(e.fv, std::move(me));
                continue;
            }

            // ---- component DAG (Phase 2.5, u32 pool-global) ----
            // Gather a COMPACT vertex/index set: clodBuild runs O(vertex_count)
            // passes per level, so feeding it whole-pool arrays would scale with
            // the pool, not the component. Compact verts remap back to
            // pool-global ids (globalOf) at emit time.
            const u32 nMembers = (u32)un.members.size();
            u32 totalV = 0, totalI = 0;
            for (u32 m : un.members) { totalV += elig[m].fv->m_mesh.vCount; totalI += elig[m].idxCount; }

            cpos.resize((size_t)totalV * 3);
            attrs.resize((size_t)totalV * 7);
            vlock.assign(totalV, 0);
            meshOfV.resize(totalV); globalOf.resize(totalV); pkeys.resize(totalV);
            idxIn.clear(); idxIn.reserve(totalI);

            u32 vCursor = 0;
            for (u32 mi = 0; mi < nMembers; ++mi) {
                const Elig& e = elig[un.members[mi]];
                const auto& mesh = e.fv->m_mesh;
                const u8* vb = (const u8*)e.pos;
                const u32 base = vCursor;
                for (u32 v = 0; v < mesh.vCount; ++v, ++vCursor) {
                    const u8* vp = vb + (size_t)v * mesh.vStride;
                    memcpy(&cpos[(size_t)vCursor * 3], vp, 12);
                    vertexAttrs(vp, mesh.vStride, mesh.tcOffset, &attrs[(size_t)vCursor * 7]);
                    meshOfV[vCursor]  = mi;
                    globalOf[vCursor] = mesh.vBase + v;
                    pkeys[vCursor]    = posKey((const float*)vp);
                }
                for (u32 i = 0; i < e.idxCount; ++i) idxIn.push_back(base + e.idx[i]);
            }

            // Locks & protects. Lock: position shared with another unit (the
            // neighbor won't move). Protect: LMAP-UV discontinuity against the
            // position-canonical vertex WITHIN THE SAME MESH — light seams are
            // the ones that flash visibly when collapsed. Diffuse-UV seams get
            // NO protect (unlike the per-mesh path): low-poly content has a UV
            // seam on nearly every corner, and protecting them froze 40% of
            // component verts → stalled DAGs. Their stretch is subpixel at the
            // distances where a coarse cut is chosen. Cross-mesh discontinuities
            // get no protect either — those are the material boundaries a door
            // must collapse across (the Nanite answer to material boundaries).
            canon.clear();
            canon.reserve(totalV);
            for (u32 v = 0; v < totalV; ++v) canon.emplace(pkeys[v], v);
            for (u32 v = 0; v < totalV; ++v) {
                auto sh = posOwner.find(pkeys[v]);
                if (sh != posOwner.end() && sh->second == kShared) vlock[v] |= meshopt_SimplifyVertex_Lock;
                const u32 c = canon.find(pkeys[v])->second;
                if (c != v && meshOfV[c] == meshOfV[v]) {
                    const float* av = &attrs[(size_t)v * 7];
                    const float* ac = &attrs[(size_t)c * 7];
                    if (av[5] != ac[5] || av[6] != ac[6]) vlock[v] |= meshopt_SimplifyVertex_Protect;   // lmap UV only
                }
            }
            for (u32 v = 0; v < totalV; ++v) {   // diag: how much of the component is frozen
                if (vlock[v] & meshopt_SimplifyVertex_Lock)    ++out.lockVerts;
                if (vlock[v] & meshopt_SimplifyVertex_Protect) ++out.protVerts;
            }
            out.compVerts += totalV;

            clodConfig cfg = baseConfig();
            clodMesh cm{};
            cm.indices                  = idxIn.data();
            cm.index_count              = idxIn.size();
            cm.vertex_count             = totalV;
            cm.vertex_positions         = cpos.data();
            cm.vertex_positions_stride  = 3 * sizeof(float);
            cm.vertex_attributes        = attrs.data();
            cm.vertex_attributes_stride = 7 * sizeof(float);
            cm.vertex_lock              = vlock.data();
            cm.attribute_weights        = attrWeights;
            cm.attribute_count          = 3;
            cm.attribute_protect_mask   = 0;   // protect bits self-computed above (intra-mesh only)

            {
                string64 who;
                xr_sprintf(who, "component of %u meshes", nMembers);
                if (!ValidateClodInput("component", cm, attrs.size(), vlock.size(), who))
                    continue;
            }

            const u32    metaFirst = (u32)out.meta.size();

            const size_t idxFirst  = out.idx32.size();
            xr_vector<xr_vector<u32>> memberEntries(nMembers);
            u32 clustersEmitted = 0;
            u32 sim = 0, stall = 0;
            int maxDepth = 0; u32 l0 = 0, l0s = 0;   // diag: did the DAG ever leave level 0?
            clodBuild(cfg, cm, [&](clodGroup g, const clodCluster* cs, size_t n) -> int {
                const int id = (int)groups.size();
                groups.push_back(g.simplified);
                if (g.simplified.error >= FLT_MAX) ++stall; else ++sim;
                maxDepth = _max(maxDepth, g.depth);
                if (g.depth == 0) { ++l0; if (g.simplified.error >= FLT_MAX) ++l0s; }
                for (size_t i = 0; i < n; ++i) {
                    const clodCluster& c = cs[i];
                    ++clustersEmitted;
                    const GpuMeshMeta proto = protoMeta(g, c);
                    // Bin the cluster's triangles by their v0's source mesh →
                    // one entry per (cluster × mesh), sharing the cluster's lod
                    // spheres/errors so the cull flips them as one.
                    bins.clear();
                    const size_t triCount = c.index_count / 3;
                    for (size_t t2 = 0; t2 < triCount; ++t2)
                        bins.emplace_back(meshOfV[c.indices[t2 * 3]], (u32)t2);
                    std::stable_sort(bins.begin(), bins.end(),
                                     [](const std::pair<u32, u32>& a, const std::pair<u32, u32>& b) { return a.first < b.first; });
                    for (size_t a2 = 0; a2 < bins.size();) {
                        size_t b2 = a2;
                        while (b2 < bins.size() && bins[b2].first == bins[a2].first) ++b2;
                        GpuMeshMeta e2 = proto;
                        e2.index_count  = (u32)((b2 - a2) * 3);
                        e2.ib_first     = (u32)out.idx32.size();
                        e2.first_vertex = 0;   // pool-global u32 indices
                        for (size_t k = a2; k < b2; ++k) {
                            const u32 t3 = bins[k].second;
                            out.idx32.push_back(globalOf[c.indices[t3 * 3 + 0]]);
                            out.idx32.push_back(globalOf[c.indices[t3 * 3 + 1]]);
                            out.idx32.push_back(globalOf[c.indices[t3 * 3 + 2]]);
                        }
                        memberEntries[bins[a2].first].push_back((u32)out.meta.size());
                        out.meta.push_back(e2);
                        a2 = b2;
                    }
                }
                return id;
            });
            if (clustersEmitted < 2) {   // whole component fits one cluster — the plain path is equal & cheaper
                out.meta.resize(metaFirst);
                out.idx32.resize(idxFirst);
                continue;
            }
            ++out.compBuilt;
            out.l0Groups += l0; out.l0Stalled += l0s;
            if (maxDepth == 0) {   // DAG never simplified anything — the unit stays full-detail forever
                ++out.compFlat;
                if (un.tris > out.flatWorstTris) { out.flatWorstTris = un.tris; out.flatWorstMeshes = nMembers; }
                Fbox fb; fb.invalidate();
                for (u32 m : un.members) fb.merge(elig[m].fv->vis.box);
                Fvector fc; fb.getcenter(fc);
                out.flatUnits.emplace_back(un.tris, fc);
            }
            out.simplified += sim; out.stalled += stall;
            for (u32 mi = 0; mi < nMembers; ++mi)
                if (!memberEntries[mi].empty())
                    out.ranges.emplace_back(elig[un.members[mi]].fv, MeshEntries{ true, std::move(memberEntries[mi]) });
        }
    };

    xr_vector<std::thread> pool;
    for (u32 t = 1; t < nThreads; ++t) pool.emplace_back(worker, t);
    worker(0);
    for (auto& th : pool) th.join();

    // Merge thread outputs: shift each thread's meta/IB offsets — the result is
    // layout-identical to a serial run. Every meta entry belongs to exactly one
    // mesh's entry list, so the list's ib32 flag routes its ib_first fixup.
    for (ThreadOut& out : touts) {
        const u32 metaBase  = (u32)outMeta.size();
        const u32 idx16Base = (u32)outIdx16.size();
        const u32 idx32Base = (u32)outIdx32.size();
        for (auto& r : out.ranges) {
            for (u32& e : r.second.entries) {
                out.meta[e].ib_first += r.second.ib32 ? idx32Base : idx16Base;
                e += metaBase;
            }
            outRanges.emplace(r.first, std::move(r.second));
        }
        outMeta.insert(outMeta.end(), out.meta.begin(), out.meta.end());
        outIdx16.insert(outIdx16.end(), out.idx16.begin(), out.idx16.end());
        outIdx32.insert(outIdx32.end(), out.idx32.begin(), out.idx32.end());
        s_statSimplified += out.simplified; s_statStalled += out.stalled;
    }

    {   // component diag: the answer to "why doesn't this building LOD?"
        u32 built = 0, flat = 0, l0g = 0, l0s = 0, worstT = 0, worstM = 0;
        u64 cv = 0, lv = 0, pv = 0;
        for (const ThreadOut& out : touts) {
            built += out.compBuilt; flat += out.compFlat;
            l0g += out.l0Groups; l0s += out.l0Stalled;
            cv += out.compVerts; lv += out.lockVerts; pv += out.protVerts;
            if (out.flatWorstTris > worstT) { worstT = out.flatWorstTris; worstM = out.flatWorstMeshes; }
        }
        if (built)
            Msg("[VK Cluster] comp diag: %u units (%u FLAT = never simplified; worst flat %u tris/%u meshes) | L0 groups %u, %u stalled (%.0f%%) | verts: %.1f%% locked, %.1f%% protected",
                built, flat, worstT, worstM, l0g, l0s, l0g ? 100.0 * l0s / l0g : 0.0,
                cv ? 100.0 * lv / cv : 0.0, cv ? 100.0 * pv / cv : 0.0);
        xr_vector<std::pair<u32, Fvector>> flats;
        for (const ThreadOut& out : touts) flats.insert(flats.end(), out.flatUnits.begin(), out.flatUnits.end());
        std::sort(flats.begin(), flats.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (u32 i = 0; i < _min<u32>(8, (u32)flats.size()); ++i)
            Msg("[VK Cluster]   flat unit %u: %u tris @ (%.0f, %.0f, %.0f)", i, flats[i].first,
                flats[i].second.x, flats[i].second.y, flats[i].second.z);
    }

    // ⭐ Bake hygiene (17-07): drop SELF-LOOP stall entries — clusters that
    // declare THEMSELVES their own parent ((lodSelf,selfError) bitwise equal to
    // (lodParent,parentError); clusterlod stall chains emit them). They can
    // never produce pixels: the hard cut needs sp<=1<pp (impossible with
    // sp==pp) and in the fade band their two dither masks cancel exactly. They
    // only cost meta slots, page bytes, cull invocations and VSM candidates —
    // and their severed child links were what left stall groups with no
    // refiner (the orphan-cohort/white-polygon saga; direct demand now covers
    // those regardless). The runtime self-loop cut in ClusterStream stays as a
    // safety net and should report 0 for caches baked with this filter.
    {
        const u32 n0 = (u32)outMeta.size();
        xr_vector<u8> drop(n0, 0);
        u32 nDrop = 0;
        for (u32 i = 0; i < n0; ++i) {
            const GpuMeshMeta& m = outMeta[i];
            if (m.selfError > 0.f && m.parentError < kErrInf
                && memcmp(&m.lodSelf, &m.lodParent, sizeof(Fvector4)) == 0
                && memcmp(&m.selfError, &m.parentError, sizeof(float)) == 0)
                { drop[i] = 1; ++nDrop; }
        }
        if (nDrop) {
            // Never empty a whole mesh (would silently unclusterize it): if every
            // entry of a mesh is a self-loop — shouldn't happen, its level-0
            // leaves carry distinct values — keep that mesh untouched.
            for (auto& r : outRanges) {
                u32 live = 0;
                for (u32 e : r.second.entries) if (e < n0 && !drop[e]) ++live;
                if (!live)
                    for (u32 e : r.second.entries) if (e < n0 && drop[e]) { drop[e] = 0; --nDrop; }
            }
        }
        if (nDrop) {
            xr_vector<u32> remap(n0, 0xFFFFFFFFu);
            u32 keep = 0;
            for (u32 i = 0; i < n0; ++i)
                if (!drop[i]) { remap[i] = keep; if (keep != i) outMeta[keep] = outMeta[i]; ++keep; }
            outMeta.resize(keep);
            for (auto& r : outRanges) {
                auto& es = r.second.entries;
                size_t w = 0;
                for (u32 e : es) if (e < n0 && remap[e] != 0xFFFFFFFFu) es[w++] = remap[e];
                es.resize(w);
            }
            Msg("[VK Cluster] bake hygiene: dropped %u self-loop stall entries (%u -> %u)", nDrop, n0, keep);
        }
    }

    {   // Stage B: pack the DAG into streaming pages (rewrites ib_first to
        // page-local + fills meta._pad1 with page ids, reorders the blobs).
        // Slice 2: hand the assembler each entry's vertex source (the host VB
        // pool copies read back above) so repackable meshes (stride ==
        // kVbStride) get their vertices repacked into per-page blocks too —
        // outVbBlob becomes the vertex-pool image (cached + uploaded).
        xr_vector<u8>  entryIb32(outMeta.size(), 0);
        xr_vector<u32> entryMesh(outMeta.size(), 0);
        xr_vector<ClusterStream::MeshVbInfo> meshInfo;
        xr_vector<ClusterStream::VbSource>   sources;
        xr_map<std::pair<VkBuffer, u32>, u32> srcOf;   // (pool VB, stride) -> source id
        for (const auto& kv : outRanges) {
            const auto& mesh = kv.first->m_mesh;
            const VkBuffer vbh = mesh.p_rm_Vertices->GetHandle();
            auto vit = vbData.find(vbh);
            u32 srcId = 0; u8 repack = 0;
            if (vit != vbData.end() && !vit->second.empty()) {
                auto ins = srcOf.emplace(std::make_pair(vbh, (u32)mesh.vStride), (u32)sources.size());
                if (ins.second) sources.push_back({ vit->second.data(), (u64)vit->second.size(), (u32)mesh.vStride });
                srcId = ins.first->second;
                repack = mesh.vStride == ClusterStream::kVbStride ? 1 : 0;
            }
            const u32 mid = (u32)meshInfo.size();
            meshInfo.push_back({ srcId, repack });
            for (u32 e : kv.second.entries) {
                if (kv.second.ib32) entryIb32[e] = 1;
                entryMesh[e] = mid;
            }
        }
        ClusterStream::AssemblePages(outMeta, entryIb32, entryMesh, meshInfo, sources,
                                     outIdx16, outIdx32, outVbBlob, s_pageDir);
    }
    // r_cluster_cache 0 is a REPRO switch, not a policy: it must not also overwrite the
    // on-disk cache with whatever this particular load happened to classify.
    if (ps_r_cluster_cache)
        SaveClusterCache(eligMeshes, outMeta, outIdx16, outIdx32, outVbBlob, outRanges, s_pageDir);
}


bool CreateCullPipeline()
{
    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule cs = g_ShaderManager->Load("world_cull.comp.spv");
    if (!cs) { Msg("![VK WorldGPU] world_cull.comp.spv load failed"); return false; }

    // 0 meta, 1 cmds, 2 counts, 3 groupBase, + Stage B: 4 stream bits,
    // 5 page slot bases, 6 page requests (feedback), 7 touched-page bitset.
    constexpr u32 kB = 8;
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                1, s_setL, s_pool, &s_set,
                                VK_SHADER_STAGE_COMPUTE_BIT, "WorldGPU.Cull"))
        return false;
    VK::Prof::NameSet(s_set, "WorldGPU.Set");   // TEMP diag: VUID hunt

    const VkBuffer bufs[kB] = {
        s_meta->GetHandle(),             s_indirect->GetHandle(),
        s_count->GetHandle(),            s_groupBase->GetHandle(),
        ClusterStream::BitsBuffer(),     ClusterStream::SlotBaseBuffer(),
        ClusterStream::RequestBuffer(),  ClusterStream::TouchedBuffer(),
    };
    VK::DescriptorWriter dw(s_set);
    for (u32 i = 0; i < kB; ++i) dw.StorageBuffer(i, bufs[i]);
    dw.Flush();

    s_cullLayout = VK::MakePipelineLayout({ s_setL }, sizeof(CullPush));
    if (!s_cullLayout) return false;

    s_cullPipe = VK::CreateComputePipeline(cs, s_cullLayout, "WorldGPU.Cull");
    return s_cullPipe != VK_NULL_HANDLE;
}

// Shadow cull pipeline (world_cull_shadow.comp): 4 SSBOs — meta (0), the
// per-target shadow indirect (1), per-target counts (2), groupBase (3).
bool CreateShadowCullPipeline()
{
    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule cs = g_ShaderManager->Load("world_cull_shadow.comp.spv");
    if (!cs) { Msg("![VK WorldGPU] world_cull_shadow.comp.spv load failed"); return false; }

    // 0 meta, 1 per-target indirect, 2 per-target counts, 3 groupBase,
    // + Stage B: 4 stream bits, 5 page slot bases (no requests — main view asks).
    constexpr u32 kB3 = 6;
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO },
                                1, s_setL3, s_pool3, &s_set3,
                                VK_SHADER_STAGE_COMPUTE_BIT, "WorldGPU.CullShadow"))
        return false;

    const VkBuffer bufs[kB3] = {
        s_meta->GetHandle(),         s_indirectSh->GetHandle(),
        s_countSh->GetHandle(),      s_groupBase->GetHandle(),
        ClusterStream::BitsBuffer(), ClusterStream::SlotBaseBuffer(),
    };
    VK::DescriptorWriter dw(s_set3);
    for (u32 i = 0; i < kB3; ++i) dw.StorageBuffer(i, bufs[i]);
    dw.Flush();

    s_cullLayout3 = VK::MakePipelineLayout({ s_setL3 }, sizeof(CullShadowPush));
    if (!s_cullLayout3) return false;

    s_cullPipe3 = VK::CreateComputePipeline(cs, s_cullLayout3, "WorldGPU.CullShadow");
    return s_cullPipe3 != VK_NULL_HANDLE;
}

// Hi-Z occlusion cull pipeline (world_cull_hzb.comp). 4 bindings: meta (0),
// cmds2 (1), count2 (2) — written once here; HZB combined-image-sampler (3) —
// (re)written per-frame in CullColor since the depth pyramid view/sampler can
// change on resize. Best-effort: failure leaves s_occlReady=false (the base
// frustum path keeps working; the color pass just won't occlusion-cull).
bool CreateCullColorPipeline()
{
    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule cs = g_ShaderManager->Load("world_cull_hzb.comp.spv");
    if (!cs) { Msg("![VK WorldGPU] world_cull_hzb.comp.spv load failed — occlusion cull disabled"); return false; }

    // 0 meta, 1 cmds2, 2 count2, 3 HZB (per-frame), 4 groupBase,
    // + Stage B: 5 stream bits, 6 page slot bases.
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 3 = HZB
                                  kSSBO, kSSBO, kSSBO },
                                1, s_setL2, s_pool2, &s_set2,
                                VK_SHADER_STAGE_COMPUTE_BIT, "WorldGPU.CullColor"))
        return false;
    VK::Prof::NameSet(s_set2, "WorldGPU.Set2");   // TEMP diag: VUID hunt
    // Fresh set → binding 3 is unwritten. Invalidate the cache so the first
    // CullColor writes it (a level reload lands here with stale statics).
    s_hzbBoundView = VK_NULL_HANDLE; s_hzbBoundSampler = VK_NULL_HANDLE; s_hzbBoundGen = 0xFFFFFFFFu;

    // binding 3 = HZB, written on change (see CullColor)
    VK::DescriptorWriter(s_set2)
        .StorageBuffer(0, s_meta->GetHandle())
        .StorageBuffer(1, s_indirect2->GetHandle())
        .StorageBuffer(2, s_count2->GetHandle())
        .StorageBuffer(4, s_groupBase->GetHandle())
        .StorageBuffer(5, ClusterStream::BitsBuffer())
        .StorageBuffer(6, ClusterStream::SlotBaseBuffer())
        .Flush();

    s_cullLayout2 = VK::MakePipelineLayout({ s_setL2 }, sizeof(CullColorPush));
    if (!s_cullLayout2) return false;

    s_cullPipe2 = VK::CreateComputePipeline(cs, s_cullLayout2, "WorldGPU.CullColor");
    return s_cullPipe2 != VK_NULL_HANDLE;
}

// Two-phase cull pipeline (world_cull_two.comp) — one pipeline, TWO sets of the
// SAME layout differing only in which indirect/count pair bindings 1/2 name:
//   s_setTwo[0] -> s_indirect /s_count   (EARLY, phase 1)
//   s_setTwo[1] -> s_indirect2/s_count2  (LATE,  phase 2)
// Bindings otherwise mirror world_cull.comp (it is the same cut, streaming
// requests included) plus 8 = the pyramid and 9 = the late list. Best-effort:
// failure leaves s_twoReady false and the single-phase pair keeps working.
bool CreateCullTwoPipeline()
{
    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule cs = g_ShaderManager->Load("world_cull_two.comp.spv");
    if (!cs) { Msg("![VK WorldGPU] world_cull_two.comp.spv load failed — r_hzb_prepass disabled"); return false; }

    // 244 B is above the 128 B every Vulkan device guarantees. The renderer
    // already ships a 240 B range (TreeMotionPush), so this is legal on the GPUs
    // it runs on — but check rather than let vkCreatePipelineLayout fail opaquely.
    if (sizeof(CullTwoPush) > VulkanHW.Caps.maxPushConstantsSize) {
        Msg("![VK WorldGPU] CullTwoPush %u B exceeds device maxPushConstantsSize %u B — r_hzb_prepass disabled",
            (u32)sizeof(CullTwoPush), VulkanHW.Caps.maxPushConstantsSize);
        return false;
    }

    // 0 meta, 1 cmds, 2 counts, 3 groupBase, 4 stream bits, 5 page slot bases,
    // 6 page requests, 7 touched-page bitset, 8 HZB (per-frame), 9 late list.
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO, kSSBO,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 8 = HZB
                                  kSSBO },
                                2, s_setLTwo, s_poolTwo, s_setTwo,
                                VK_SHADER_STAGE_COMPUTE_BIT, "WorldGPU.CullTwo"))
        return false;
    VK::Prof::NameSet(s_setTwo[0], "WorldGPU.SetTwoEarly");
    VK::Prof::NameSet(s_setTwo[1], "WorldGPU.SetTwoLate");
    // Fresh sets → binding 8 unwritten; invalidate both caches so the first
    // dispatch of each writes it (a level reload lands here with stale statics).
    for (u32 i = 0; i < 2; ++i) {
        s_twoHzbView[i] = VK_NULL_HANDLE; s_twoHzbSmp[i] = VK_NULL_HANDLE; s_twoHzbGen[i] = 0xFFFFFFFFu;
    }

    for (u32 i = 0; i < 2; ++i) {
        VK::DescriptorWriter(s_setTwo[i])
            .StorageBuffer(0, s_meta->GetHandle())
            .StorageBuffer(1, (i == 0 ? s_indirect : s_indirect2)->GetHandle())
            .StorageBuffer(2, (i == 0 ? s_count    : s_count2   )->GetHandle())
            .StorageBuffer(3, s_groupBase->GetHandle())
            .StorageBuffer(4, ClusterStream::BitsBuffer())
            .StorageBuffer(5, ClusterStream::SlotBaseBuffer())
            .StorageBuffer(6, ClusterStream::RequestBuffer())
            .StorageBuffer(7, ClusterStream::TouchedBuffer())
            .StorageBuffer(9, s_late->GetHandle())
            .Flush();
    }

    s_cullLayoutTwo = VK::MakePipelineLayout({ s_setLTwo }, sizeof(CullTwoPush));
    if (!s_cullLayoutTwo) return false;

    s_cullPipeTwo = VK::CreateComputePipeline(cs, s_cullLayoutTwo, "WorldGPU.CullTwo");
    return s_cullPipeTwo != VK_NULL_HANDLE;
}

} // anonymous namespace

const xr_vector<vkFVisual*>& LeafVisuals()
{
    if (s_leavesReady) return s_leaves;
    s_leavesReady = true;

    CTimer t; t.Start();
    s_leaves.clear();
    s_leaves.reserve(RImplementation.Visuals.size());

    // FLAT pass, no recursion and no dedup set. A container's children are
    // getVisual(id) results — i.e. ELEMENTS OF Visuals[] — so every leaf a
    // recursion could reach is already a top-level entry, and Visuals[] holds each
    // object once. That makes the recursive walk a much more expensive way to
    // compute the same set: on pripyat_full both produce exactly 295 773 leaves
    // (= 95248 MT_NORMAL + 7054 MT_PROGRESSIVE + 87848 MT_TREE_ST + 105623
    // MT_TREE_PM, which is every leaf-typed entry), and the flat pass avoids
    // ~700k hash inserts plus the recursion itself.
    // ⚠If a future level ever wraps a visual that is NOT in Visuals[], this would
    // miss it — set XROS_LEAFWALK_FULL=1 to run the old recursive walk instead and
    // compare the two counts in the log.
    static const bool s_full = [] {
        const char* e = std::getenv("XROS_LEAFWALK_FULL");
        return e && e[0] && e[0] != '0';
    }();

    if (s_full) {
        std::unordered_set<void*> seen;
        seen.reserve(RImplementation.Visuals.size() * 2);
        for (IRenderVisual* iv : RImplementation.Visuals)
            CollectLeaves(static_cast<vkRender_Visual*>(iv), seen);
    } else {
        for (IRenderVisual* iv : RImplementation.Visuals) {
            auto* rv = static_cast<vkRender_Visual*>(iv);
            if (!rv) continue;
            switch (rv->Type) {
            case MT_NORMAL: case MT_PROGRESSIVE: case MT_TREE_ST: case MT_TREE_PM:
                s_leaves.push_back(static_cast<vkFVisual*>(rv));
                break;
            default: break;   // containers (recursed into nothing new) and non-mesh types
            }
        }
    }

    Msg("[load step]   LeafVisuals: %u roots -> %u unique leaves in %.0f ms (%s; shared by Build/CompactPools/ShadowGPU/TerrainMask)",
        (u32)RImplementation.Visuals.size(), (u32)s_leaves.size(), t.GetElapsed_ms_total(),
        s_full ? "recursive walk, XROS_LEAFWALK_FULL" : "flat pass");
    return s_leaves;
}

void ReleaseLeafVisuals()
{
    // 450k pointers = ~3.6 MB, and it must not survive into the spawn phase:
    // dynamic objects register their own visuals after this point.
    xr_vector<vkFVisual*>().swap(s_leaves);
    s_leavesReady = false;
}

void Build()
{
    VK::Vram::Scope _vram_scope("WorldGPU");
    if (s_built) return;
    s_built = true;   // one attempt; stays "built" (possibly empty) so we don't retry every frame
    ++s_buildStamp;   // consumers holding derived tables (VSM candidates) rebuild on stamp change
    s_compactRefs.clear();
    s_poolsCompacted = false;

    // Sub-timers (`[load step]`, one line at the end). Build measured 1248 ms of a
    // 19.5 s load as ONE bucket; three of the blocks below are pure diagnostics, so
    // the split has to exist before anything here is called expensive.
    CTimer _b; _b.Start();
    float msExtract = 0, msPools = 0, msSort = 0, msSwi = 0, msGroup = 0,
          msValid = 0, msCut = 0, msMetaCopy = 0, msState = 0, msBufs = 0;

    xr_vector<vkFVisual*> meshes; meshes.reserve(16384);
    xr_vector<vkFVisual*> cpu;    cpu.reserve(8192);
    s_cpuWhyNoMat = s_cpuWhyWmark = s_cpuWhyWater = s_cpuWhyTess = s_cpuWhyNoView = 0;
    for (vkFVisual* fv : LeafVisuals())
        ExtractMeshes(fv, meshes, cpu);
    Msg("[VK WorldGPU] cpu-set reasons: no-material=%u water=%u wmark=%u tess=%u no-view=%u",
        s_cpuWhyNoMat, s_cpuWhyWater, s_cpuWhyWmark, s_cpuWhyTess, s_cpuWhyNoView);

    // LeafVisuals() is already deduped; the sorts stay because s_meshSet/InSet
    // need pointer order (unique() is now a no-op that costs one pass).
    std::sort(meshes.begin(), meshes.end());
    meshes.erase(std::unique(meshes.begin(), meshes.end()), meshes.end());
    // CPU (non-GPU) static leaves — pre-built + deduped so the per-frame path walks
    // only these (small) instead of all level visuals + hierarchy recursion.
    std::sort(cpu.begin(), cpu.end());
    cpu.erase(std::unique(cpu.begin(), cpu.end()), cpu.end());
    s_cpuMeshes.swap(cpu);

    if (meshes.empty()) { Msg("[VK WorldGPU] no eligible static meshes (cpu-set=%u)", (u32)s_cpuMeshes.size()); return; }

    // Pointer-sorted membership set (meshes is currently sorted by pointer +
    // deduped) for InSet() — the CPU queue excludes these to avoid double-draw.
    s_meshSet.assign(meshes.begin(), meshes.end());
    msExtract = _b.GetElapsed_ms_total();

    // Cluster split (r_cluster): build the per-cluster meta pool + shared
    // cluster IBs BEFORE grouping — clustered meshes bind s_clusterIB (u16,
    // per-mesh DAGs) or s_clusterIB32 (u32 pool-global, component DAGs) instead
    // of their pool IB, so the grouping key must see the effective IB.
    xr_vector<GpuMeshMeta> clusterMeta;
    xr_vector<u16>         clusterIdx16;
    xr_vector<u32>         clusterIdx32;
    xr_vector<u8>          clusterVbBlob;
    xr_map<vkFVisual*, MeshEntries> clustered;
    u64 clusterMs = 0;
    if (ps_r_cluster) {
        const u64 t0 = CPU::QPC();
        BuildClusters(meshes, clusterMeta, clusterIdx16, clusterIdx32, clusterVbBlob, clustered);
        const u64 t1 = CPU::QPC();
        clusterMs = (t1 - t0) * 1000 / CPU::QPCFreq();
    }
    // Stage B pools (phase 1): the cluster index data now lives in ClusterStream
    // page pools — pinned pages install here; the rest streams by GPU feedback
    // (or everything, when r_clpage 0 / no cache file = pre-streaming parity).
    // Slice 2: repacked cluster vertices live in the ClusterStream vertex pool.
    _b.Start();
    if (!ClusterStream::CreatePools(s_pageDir, s_cachePath, s_blobOff16, s_blobOff32, s_blobOffVb,
                                    &clusterIdx16, &clusterIdx32, &clusterVbBlob)) {
        Msg("![VK Cluster] page pools failed — clusters disabled (plain meshes only)");
        clustered.clear(); clusterMeta.clear(); s_pageDir.clear();
        ClusterStream::CreatePools(s_pageDir, nullptr, 0, 0, 0, nullptr, nullptr, nullptr);   // identity-only state
    }
    {   // A referenced pool must exist — otherwise nothing may reference it.
        bool need16 = false, need32 = false;
        for (const auto& kv : clustered) (kv.second.ib32 ? need32 : need16) = true;
        if ((need16 && ClusterStream::PoolIB16() == VK_NULL_HANDLE)
         || (need32 && ClusterStream::PoolIB32() == VK_NULL_HANDLE)) {
            Msg("![VK Cluster] pool missing for referenced index type — clusters disabled");
            clustered.clear();
        }
    }
    msPools = _b.GetElapsed_ms_total();

    // Every mesh asks `clustered` (an xr_map = red-black tree over 50k meshes)
    // the same question up to seven times: twice for the sort key, once per
    // effectiveVB/IB in the grouping walk, once more in appendEntries. Resolve
    // it ONCE per mesh, on the idle cores (find is read-only), and carry the
    // answer alongside the mesh from here on.
    xr_vector<const MeshEntries*> clOf(meshes.size(), nullptr);
    ParallelChunks((int)meshes.size(), [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            auto it = clustered.find(meshes[i]);
            clOf[i] = (it == clustered.end()) ? nullptr : &it->second;
        }
    });

    auto effectiveIB = [&](const MeshEntries* cl, vkFVisual* a) -> VkBuffer {
        if (!cl) return a->m_mesh.p_rm_Indices->GetHandle();
        return cl->ib32 ? ClusterStream::PoolIB32() : ClusterStream::PoolIB16();
    };
    // Slice 2: a clustered mesh whose entries were vertex-repacked (its first
    // entry's page carries a VB payload — the flag is uniform per mesh by
    // construction) draws from the ClusterStream vertex pool; everything else
    // keeps its original pool VB. Same layout/stride — only the binding moves.
    auto effectiveVB = [&](const MeshEntries* cl, vkFVisual* a) -> VkBuffer {
        if (cl && !cl->entries.empty()) {
            const u32 pg = clusterMeta[cl->entries[0]]._pad1;
            if (pg < (u32)s_pageDir.size() && (s_pageDir[pg].flags & ClusterStream::kPageHasVB)
                && ClusterStream::PoolVB() != VK_NULL_HANDLE)
                return ClusterStream::PoolVB();
        }
        return a->m_mesh.p_rm_Vertices->GetHandle();
    };

    // Sort into draw batches: terrain first-class, then by (material, stride,
    // tcOffset, effective vb, effective ib) so a run shares pipeline + descriptor
    // set + buffer binds. Clustered meshes separate naturally (their ib/vb =
    // the ClusterStream pools).
    // Keys are MATERIALIZED first, then sorted. effectiveVB/effectiveIB each do an
    // xr_map (= std::map) lookup, and computing the key inside the comparator meant
    // ~4 tree lookups per comparison — measured 280 ms of the 18-08 load. Now it is
    // two lookups per mesh, total. The visual* is the last key component so the
    // order is total (ties used to resolve arbitrarily, which decided WHICH terrain
    // mesh seed() hands to the TerrainCache probe — now that pick is reproducible).
    // r_world_bindless: latched here with the material indices -- the sort and the grouping below key on it. Off when
    // the twins are missing or the level's materials outgrow the descriptor capacity (5 slots each).
    s_bindless = false; s_blMats.clear(); s_blIndex.clear();
    if (ps_r_world_bindless && PipelineCache::WorldBindlessReady()) {
        for (vkFVisual* fv : meshes) {
            WorldMaterial* m = fv->m_pWorldMaterial;
            if (BlEligible(m) && s_blIndex.find(m) == s_blIndex.end()) { s_blIndex[m] = (u32)s_blMats.size(); s_blMats.push_back(m); }
        }
        s_bindless = !s_blMats.empty() && (u32)s_blMats.size() * 5u <= PipelineCache::WorldBindlessMaxTextures();
        if (!s_bindless && !s_blMats.empty())
            Msg("![VK WorldGPU] r_world_bindless: %u materials > %u descriptor slots / 5 -- one group per material",
                (u32)s_blMats.size(), PipelineCache::WorldBindlessMaxTextures());
        if (s_bindless && !BlCreate()) {
            Msg("![VK WorldGPU] r_world_bindless: descriptor pool / material buffers failed -- one group per material");
            s_bindless = false;
        }
        if (!s_bindless) { s_blMats.clear(); s_blIndex.clear(); }
    }

    _b.Start();
    // `mat` = the material, or for a bindless-eligible mesh only its pipeline class (POM or not) -- the group then
    // spans materials; `mat2` keeps each material's meshes together inside it (texture-cache locality).
    struct SortKey {
        u32 terrain, stride, tcOffset;
        const void *mat, *vb, *ib, *mat2;
        vkFVisual*  fv;
        const MeshEntries* cl;   // rides along so the grouping walk keeps it
        bool operator<(const SortKey& o) const {
            return std::tie(  terrain,   mat,   stride,   tcOffset,   vb,   ib,   mat2,   fv)
                 < std::tie(o.terrain, o.mat, o.stride, o.tcOffset, o.vb, o.ib, o.mat2, o.fv);
        }
    };
    xr_vector<SortKey> order; order.resize(meshes.size());
    ParallelChunks((int)meshes.size(), [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            vkFVisual* a = meshes[i];
            const WorldMaterial* m = a->m_pWorldMaterial;
            const bool bl = s_bindless && BlEligible(m);
            order[i] = { m->isTerrain ? 0u : 1u,
                         a->m_mesh.vStride, a->m_mesh.tcOffset,
                         bl ? (const void*)(uintptr_t)(m->tessellated ? 2u : 1u) : (const void*)m,
                         (const void*)effectiveVB(clOf[i], a), (const void*)effectiveIB(clOf[i], a),
                         (const void*)m, a, clOf[i] };
        }
    });
    std::sort(order.begin(), order.end());
    for (size_t i = 0; i < order.size(); ++i) { meshes[i] = order[i].fv; clOf[i] = order[i].cl; }
    msSort = _b.GetElapsed_ms_total();

    const u32 nMeshes = (u32)meshes.size();
    _b.Start();

    // DIAG (one-shot): SWI LOD-level distribution of the GPU-set progressive meshes.
    // Tells us if there are INTERMEDIATE LODs (sw_count>2) to grade through (smooth,
    // no popping) or only fine+coarse, and the finest->coarsest triangle reduction
    // (= the LOD savings headroom). Drives whether main-view LOD is graduated or binary.
    {
        u32 progN = 0, multiN = 0, swMin = 0xFFFFFFFFu, swMax = 0; u64 swSum = 0;
        u64 fineTris = 0, coarseTris = 0; u32 hist[9] = { 0 };   // buckets sw_count = 1,2,..,8,9+
        for (vkFVisual* fv : meshes) {
            if (fv->Type != MT_PROGRESSIVE) continue;
            auto* pg = static_cast<vkFProgressive*>(fv);
            if (!pg->sw_counts || pg->sw_count == 0) continue;
            ++progN;
            const u32 c = pg->sw_count;
            swMin = _min(swMin, c); swMax = _max(swMax, c); swSum += c;
            hist[_min(c, 9u) - 1]++;
            if (c > 1) { ++multiN; fineTris += pg->sw_counts[0] / 3; coarseTris += pg->sw_counts[c - 1] / 3; }
        }
        if (progN) {
            Msg("[VK WorldGPU] SWI-LOD: %u progressive (%u multi-level), sw_count min/avg/max = %u/%.1f/%u",
                progN, multiN, swMin, double(swSum) / progN, swMax);
            Msg("[VK WorldGPU] SWI-LOD hist[1..9+] = %u %u %u %u %u %u %u %u %u",
                hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8]);
            if (multiN) Msg("[VK WorldGPU] SWI-LOD tris: finest avg=%llu coarsest avg=%llu (%.0f%% fewer)",
                (unsigned long long)(fineTris / multiN), (unsigned long long)(coarseTris / multiN),
                100.0 * (1.0 - double(coarseTris) / double(_max((u64)1, fineTris))));
        } else {
            Msg("[VK WorldGPU] SWI-LOD: no progressive meshes in the GPU set");
        }
    }
    msSwi = _b.GetElapsed_ms_total();

    // Grouping walk: one pass over the sorted meshes builds the groups AND the
    // meta array in entry order — an entry is either the whole mesh (drawn slice
    // = finest SWI window for PROGRESSIVE, see DrawnSlice: the coarse slices poke
    // through the fine mesh) or, for clustered meshes, each prebuilt cluster.
    xr_vector<GpuMeshMeta> meta;
    meta.reserve(nMeshes + clusterMeta.size());
    // r_world_bindless: cull entry -> material index, parallel to meta (the *_bl VS reads it through firstInstance).
    xr_vector<u32> entryMat;
    if (s_bindless) entryMat.reserve(nMeshes + clusterMeta.size());
    auto blEntryMat = [&](vkFVisual* fv) {
        if (!s_bindless) return;
        const auto it = s_blIndex.find(fv->m_pWorldMaterial);
        entryMat.resize(meta.size(), it != s_blIndex.end() ? it->second : 0u);
    };

    auto appendEntries = [&](vkFVisual* fv, const MeshEntries* cl) {
        // Hard-cut flag: alpha-tested materials draw the prepass through the AT
        // pipeline, which has no dither — their transitions stay instant. Set
        // here (not at cluster build) so the disk cache stays material-agnostic.
        const u32 flags = (fv->m_pWorldMaterial->alphaRef >= 0.f) ? 1u : 0u;
        // Pool-compaction bookkeeping: remember this mesh's meta run + how its
        // entries reference the original pools (CompactPools patches by these).
        CompactRef ref{};
        ref.fv        = fv;
        ref.metaFirst = (u32)meta.size();
        ref.clustered = cl ? 1 : 0;
        ref.ib32      = (cl && cl->ib32) ? 1 : 0;
        ref.repacked  = (cl && effectiveVB(cl, fv) == ClusterStream::PoolVB()
                         && ClusterStream::PoolVB() != VK_NULL_HANDLE) ? 1 : 0;
        if (cl) {
            for (u32 ei : cl->entries) {
                GpuMeshMeta e = clusterMeta[ei];
                e.flags = flags;
                meta.push_back(e);
            }
            ref.metaCount = (u32)meta.size() - ref.metaFirst;
            blEntryMat(fv);
            s_compactRefs.push_back(ref);
            return;
        }
        GpuMeshMeta m{};
        m.sphere_P = fv->vis.sphere.P;
        m.sphere_R = fv->vis.sphere.R;
        DrawnSlice(fv, m.ib_first, m.index_count);
        m.first_vertex = fv->m_mesh.vBase;
        m.group        = 0;   // assigned after grouping
        // bit1 = plain WHOLE mesh → SSA-cullable (r_ssa_px). Only set here:
        // clusters (even parentError=INF orphans) must never SSA-cull — a large
        // surface's small clusters would evaporate chunk by chunk at range.
        m.flags        = flags | 2u;
        m.lodSelf.set(m.sphere_P.x, m.sphere_P.y, m.sphere_P.z, m.sphere_R);
        m.selfError    = 0.f;        // plain mesh: exact geometry ...
        m.parentError  = kErrInf;    // ... with no coarser parent → always drawn
        meta.push_back(m);
        ref.metaCount = 1;
        blEntryMat(fv);
        s_compactRefs.push_back(ref);
    };

    _b.Start();
    s_groups.clear();
    Group cur{};
    auto seed = [&](u32 i) {
        vkFVisual* fv = meshes[i];
        cur.mat      = fv->m_pWorldMaterial;
        cur.terrain  = fv->m_pWorldMaterial->isTerrain;
        cur.bindless = s_bindless && BlEligible(fv->m_pWorldMaterial);
        cur.stride   = fv->m_mesh.vStride;
        cur.tcOffset = fv->m_mesh.tcOffset;
        const MeshEntries* cl = clOf[i];
        cur.vb       = effectiveVB(cl, fv);
        cur.ib       = effectiveIB(cl, fv);
        cur.iType    = !cl ? fv->m_mesh.iType
                     : (cl->ib32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        cur.meshCount   = 1;
        cur.entryOffset = (u32)meta.size();
        cur.entryCount  = 0;   // finalized when the group closes
        // Composite cache (r_terra_cache): capture the terrain material + mesh
        // for the bake (GPU-driven path bypasses the CPU render queue). The
        // probe needs a CONTIGUOUS vertex run at vBase — that only exists in
        // the ORIGINAL pool VB (cur.vb is the page-repacked ClusterStream pool
        // for clustered terrain, where vBase addresses someone else's pages).
        if (cur.terrain && cur.mat->terrainSet != VK_NULL_HANDLE) {
            TerrainCache::OnTerrainMaterial(cur.mat->terrainSet, cur.mat->detailScale);
            TerrainCache::OnTerrainMesh(fv->m_mesh.p_rm_Vertices->GetHandle(),
                                        fv->m_mesh.vBase, cur.stride, cur.tcOffset);
        }
    };
    // 10-09 Switch: vkCmdDrawIndexedIndirectCount on Maxwell is our mesa-switch patch (Fermi MME), and it draws at most
    // one push of commands per call -- min(count, ~1637 at 20 B) -- dropping the rest SILENTLY. The ~600 per-material
    // groups mostly fit; the ~55 bindless ones hold thousands of entries, so whole walls/roofs went missing from the
    // prepass AND the colour pass, worse the finer the cut (scope, r_cluster_lod). Cap a group's entries under that.
#ifdef __SWITCH__
    constexpr u32 kMaxGroupEntries = 1536;
#else
    constexpr u32 kMaxGroupEntries = 0;   // desktop drivers loop the count themselves
#endif
    seed(0); appendEntries(meshes[0], clOf[0]);
    for (u32 i = 1; i < nMeshes; ++i) {
        vkFVisual* fv = meshes[i];
        // r_world_bindless: a bindless group spans materials of one pipeline class (POM or not); every other group
        // stays one material.
        const WorldMaterial* fm = fv->m_pWorldMaterial;
        const bool bl = s_bindless && BlEligible(fm);
        const bool sameMat = bl ? (cur.bindless && fm->tessellated == cur.mat->tessellated)
                                : (!cur.bindless && fm == cur.mat);
        const u32  addEntries = clOf[i] ? (u32)clOf[i]->entries.size() : 1u;
        const bool fits = !kMaxGroupEntries || (u32)meta.size() - cur.entryOffset + addEntries <= kMaxGroupEntries;
        const bool same = sameMat && fits
            && fv->m_mesh.vStride == cur.stride && fv->m_mesh.tcOffset == cur.tcOffset
            && effectiveVB(clOf[i], fv) == cur.vb
            && effectiveIB(clOf[i], fv) == cur.ib;
        if (same) cur.meshCount++;
        else { cur.entryCount = (u32)meta.size() - cur.entryOffset; s_groups.push_back(cur); seed(i); }
        appendEntries(fv, clOf[i]);
    }
    cur.entryCount = (u32)meta.size() - cur.entryOffset;
    s_groups.push_back(cur);
    if (kMaxGroupEntries) {
        u32 over = 0, worst = 0;
        for (const Group& g : s_groups)
            if (g.entryCount > kMaxGroupEntries) { ++over; worst = _max(worst, g.entryCount); }
        if (over)
            Msg("![VK WorldGPU] %u groups exceed %u entries (largest %u): one mesh's clusters alone -- the indirect-count "
                "draw drops the tail on Switch", over, kMaxGroupEntries, worst);
    }

    // 📏ЧТО ДРОБИТ ГРУППЫ (18-09-2026). Замерено: groups=639, различных
    // МАТЕРИАЛОВ 482 при 19 VB и 10 IB ⇒ дробят материалы, не геометрия.
    // 16% групп содержат один меш. Цена — ~0.34 мкс записи команд на группу в
    // КАЖДОМ пассе, рисующем статику (половина пасса World при ТРЁХ реальных
    // вызовах отрисовки). ⛔Мемо ключа конвейера проверено A/B в одном прогоне —
    // выигрыша НЕТ, значит цена в самих vkCmd*, а не в поиске конвейера.
    // ▶Настоящий рычаг — сократить 482 материала (bindless/атлас).
    {
        static bool s_gdiag = false;
        if (!s_gdiag) {
            s_gdiag = true;
            xr_vector<const void*> mats, vbs, ibs;
            u32 single = 0, terr = 0, maxC = 0;
            for (const Group& g : s_groups) {
                if (std::find(mats.begin(), mats.end(), (const void*)g.mat) == mats.end()) mats.push_back((const void*)g.mat);
                if (std::find(vbs.begin(), vbs.end(), (const void*)g.vb) == vbs.end()) vbs.push_back((const void*)g.vb);
                if (std::find(ibs.begin(), ibs.end(), (const void*)g.ib) == ibs.end()) ibs.push_back((const void*)g.ib);
                if (g.entryCount <= 1) ++single;
                if (g.terrain) ++terr;
                if (g.entryCount > maxC) maxC = g.entryCount;
            }
            Msg("[VK WorldGPU] GDIAG groups=%u | distinct: materials=%u VB=%u IB=%u | terrain groups=%u",
                (u32)s_groups.size(), (u32)mats.size(), (u32)vbs.size(), (u32)ibs.size(), terr);
            Msg("[VK WorldGPU] GDIAG entries/group: max=%u | groups with <=1 entry=%u (%.0f%%)",
                maxC, single, s_groups.size() ? 100.0 * single / s_groups.size() : 0.0);
        }
    }

    // Per-group world bounds: union of the entry cull spheres, computed once here
    // (an AABB over centre+-radius, then its bounding sphere — conservative, which
    // is the only safe direction for a cull). Feeds r_world_group_cull.
    for (Group& g : s_groups) {
        if (!g.entryCount) { g.bsP.set(0, 0, 0); g.bsR = -1.f; continue; }
        Fvector lo, hi;
        lo.set(flt_max, flt_max, flt_max);
        hi.set(-flt_max, -flt_max, -flt_max);
        for (u32 e = g.entryOffset; e < g.entryOffset + g.entryCount; ++e) {
            const GpuMeshMeta& m = meta[e];
            const float r = m.sphere_R;
            lo.x = _min(lo.x, m.sphere_P.x - r); hi.x = _max(hi.x, m.sphere_P.x + r);
            lo.y = _min(lo.y, m.sphere_P.y - r); hi.y = _max(hi.y, m.sphere_P.y + r);
            lo.z = _min(lo.z, m.sphere_P.z - r); hi.z = _max(hi.z, m.sphere_P.z + r);
        }
        g.bsP.set((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
        Fvector d; d.sub(hi, lo);
        g.bsR = d.magnitude() * 0.5f;
    }

    s_total = (u32)meta.size();   // dispatch size = all cullable entries
    const u32 nGroups = (u32)s_groups.size();
    for (u32 g = 0; g < nGroups; ++g)
        for (u32 e = s_groups[g].entryOffset; e < s_groups[g].entryOffset + s_groups[g].entryCount; ++e)
            meta[e].group = g;
    msGroup = _b.GetElapsed_ms_total();
    _b.Start();

    {   // Slice-2 VB-BINDING VALIDATOR (bisected bug: occasional white stretched
        // polygons / missing walls). The cull adds the page VB base to first_vertex
        // unconditionally, and the group binds ONE VB. So every entry's page MUST
        // agree with its group's VB: a kPageHasVB page ⟺ the group binds PoolVB
        // (repacked, page-local first_vertex). A mismatch means the entry fetches
        // from the WRONG buffer → garbage vertices. effectiveVB decides per-mesh
        // from entries[0] only; this catches any mesh whose entries disagree.
        const VkBuffer poolVB = ClusterStream::PoolVB();
        u32 nMis = 0, nFv = 0, shown = 0;
        for (u32 g = 0; g < nGroups; ++g) {
            const bool bindsPool = (poolVB != VK_NULL_HANDLE) && (s_groups[g].vb == poolVB);
            for (u32 e = s_groups[g].entryOffset; e < s_groups[g].entryOffset + s_groups[g].entryCount; ++e) {
                const u32 pg = meta[e]._pad1;
                const bool hasVB = pg < (u32)s_pageDir.size()
                                && (s_pageDir[pg].flags & ClusterStream::kPageHasVB) != 0;
                // (A) group VB binding must match the entry's page repack class.
                if (hasVB != bindsPool) {
                    ++nMis;
                    if (shown < 12) { ++shown;
                        Msg("![VK ClVB-MISMATCH] entry %u group %u page %u: pageHasVB=%d groupBindsPool=%d groupMeshes=%u first_vertex=%u mat=%s",
                            e, g, pg, hasVB ? 1 : 0, bindsPool ? 1 : 0,
                            s_groups[g].meshCount, meta[e].first_vertex,
                            (s_groups[g].mat && s_groups[g].mat->name.c_str()) ? s_groups[g].mat->name.c_str() : "?");
                    }
                }
                // (B) a repacked entry (its page carries VB) MUST have first_vertex 0
                //     (page-local; the cull adds the page VB base). Non-zero = the
                //     assembler left an absolute offset → base+abs overshoots.
                if (hasVB && meta[e].first_vertex != 0u) {
                    ++nFv;
                    if (shown < 12) { ++shown;
                        Msg("![VK ClVB-FIRSTVERT] entry %u group %u page %u: kPageHasVB but first_vertex=%u (should be 0)",
                            e, g, pg, meta[e].first_vertex);
                    }
                }
            }
        }
        Msg("[VK ClVB] validator: %u entries, %u groups, PoolVB=%s | mismatches: binding=%u firstVertex=%u",
            s_total, nGroups, poolVB != VK_NULL_HANDLE ? "yes" : "no", nMis, nFv);
        if (nMis || nFv) Msg("![VK ClVB] ^^ THIS is the white-polygon / missing-wall bug");
    }
    msValid = _b.GetElapsed_ms_total();
    _b.Start();

    // LOD-cut sanity + the cut invariant: two full passes over 830k entries that
    // READ meta and nothing else. BuildState below also only reads it, so the
    // diagnostic rides along on its own thread and is joined afterwards — it
    // costs wall time only if it outlives the state build. Msg() stays on this
    // thread: the worker fills the counters, the print happens after the join.
    u32 dgInf = 0, dgH100 = 0, dgH10 = 0, dgH1 = 0, dgH01 = 0, dgSm = 0, dgLevel0 = 0, dgHoles = 0, dgOrphans = 0;
    float dgRmax = 0.f; double dgRsum = 0.0;
    float msCutThread = 0.f;
    std::thread cutDiagThread([&] {
        CTimer _d; _d.Start();
        {   // LOD-cut data sanity: parentError histogram (meters) + parent sphere radii.
        // An entry can only ever be REPLACED by its parent if parentError is finite
        // and the camera can get beyond the parent sphere (dist - r > 0).
        u32 hInf = 0, h100 = 0, h10 = 0, h1 = 0, h01 = 0, hSm = 0, level0 = 0;
        float rMax = 0.f; double rSum = 0.0;
        for (const GpuMeshMeta& m : meta) {
            if (m.selfError == 0.f) ++level0;
            if      (m.parentError >= kErrInf) ++hInf;
            else if (m.parentError > 100.f)    ++h100;
            else if (m.parentError > 10.f)     ++h10;
            else if (m.parentError > 1.f)      ++h1;
            else if (m.parentError > 0.1f)     ++h01;
            else                               ++hSm;
            if (m.parentError < kErrInf) { rMax = _max(rMax, m.lodParent.w); rSum += m.lodParent.w; }
        }
        dgInf = hInf; dgH100 = h100; dgH10 = h10; dgH1 = h1; dgH01 = h01; dgSm = hSm;
        dgLevel0 = level0; dgRmax = rMax; dgRsum = rSum;

        // Cut COMPLEMENTARITY invariant (Nanite: "parents are all dependent"): a
        // child hides exactly when its (lodParent, parentError) projects small,
        // trusting that some entry with a BITWISE-equal (lodSelf, selfError)
        // appears at that same threshold. If no such entry exists (emit bug,
        // cache corruption, future refactor), the child vanishes into a hole.
        // Note: a parent entry existing for ANOTHER mesh of the same cluster is
        // fine (that's how pruned shells dissolve) — the check is per-group, not
        // per-mesh. Runs on both the fresh-build and cache-load paths.
        {
            auto key = [](const Fvector4& s, float err) {
                u64 h = 1469598103934665603ull;
                auto mix = [&h](u32 v) { h = (h ^ v) * 1099511628211ull; };
                mix(*(const u32*)&s.x); mix(*(const u32*)&s.y); mix(*(const u32*)&s.z); mix(*(const u32*)&s.w);
                mix(*(const u32*)&err);
                return h;
            };
            // Sorted vector, not xr_set: this runs over EVERY cull entry (830k on
            // pripyat_full) and a red-black tree spent ~40 ms of load doing it. Same
            // membership test, same result  one sort plus binary searches.
            xr_vector<u64> selfKeys;
            selfKeys.reserve(meta.size());
            for (const GpuMeshMeta& m : meta)
                if (m.selfError > 0.f) selfKeys.push_back(key(m.lodSelf, m.selfError));
            std::sort(selfKeys.begin(), selfKeys.end());
            selfKeys.erase(std::unique(selfKeys.begin(), selfKeys.end()), selfKeys.end());
            u32 holes = 0;
            for (const GpuMeshMeta& m : meta)
                if (m.parentError < kErrInf
                    && !std::binary_search(selfKeys.begin(), selfKeys.end(), key(m.lodParent, m.parentError))) ++holes;
            dgHoles = holes;
            // 10-09 the converse (Switch: slate roofs/walls vanish in the scope): a COARSE entry hides once its own
            // (lodSelf, selfError) projects too big, trusting children with a BITWISE-equal (lodParent, parentError)
            // to appear. None -> the zoomed / closer cut leaves a hole where it was.
            xr_vector<u64> parentKeys;
            parentKeys.reserve(meta.size());
            for (const GpuMeshMeta& m : meta)
                if (m.parentError < kErrInf) parentKeys.push_back(key(m.lodParent, m.parentError));
            std::sort(parentKeys.begin(), parentKeys.end());
            parentKeys.erase(std::unique(parentKeys.begin(), parentKeys.end()), parentKeys.end());
            u32 orphans = 0;
            for (const GpuMeshMeta& m : meta)
                if (m.selfError > 0.f
                    && !std::binary_search(parentKeys.begin(), parentKeys.end(), key(m.lodSelf, m.selfError))) ++orphans;
            dgOrphans = orphans;
        }
        }
        msCutThread = _d.GetElapsed_ms_total();
    });

    msCut = _b.GetElapsed_ms_total();   // the fork itself; the wait is measured at the join
    _b.Start();
    s_metaCPU = meta;   // host copy: the stats tick simulates the cut CPU-side (diag)
    msMetaCopy = _b.GetElapsed_ms_total();
    _b.Start();

    // Stage B state (phase 2): group/liveness tables + residency bits + feedback
    // buffers over the FINAL entry array. The cull shaders bind these — failure
    // means the new cull pipelines can't run, so the whole GPU path steps aside
    // (CPU queue draws everything, like a cull-pipeline failure).
    const bool stateOk = ClusterStream::BuildState(meta);
    msState = _b.GetElapsed_ms_total();

    // Join BEFORE anything can return: the worker reads `meta`, a local.
    _b.Start();
    cutDiagThread.join();
    msCut += _b.GetElapsed_ms_total();
    {
        const u32 fin = s_total - dgInf;
        Msg("[VK Cluster] cut diag: %u entries (%u level-0) | parentError: INF=%u >100m=%u 10-100=%u 1-10=%u 0.1-1=%u <0.1=%u | parent sphere r avg=%.1f max=%.1f",
            s_total, dgLevel0, dgInf, dgH100, dgH10, dgH1, dgH01, dgSm,
            fin ? dgRsum / fin : 0.0, dgRmax);
        if (dgHoles)
            // Legal source: a group fully pruned to nothing (lone-shell units
            // dissolve into nothing by design) — expect a SMALL, stable count.
            // A jump after a builder/cache change = bitwise mismatch = popping
            // holes at the swap distance; bisect the change.
            Msg("[VK Cluster] cut invariant: %u/%u entries dissolve to nothing (no parent-level entry)", dgHoles, s_total);
        if (dgOrphans)
            Msg("[VK Cluster] cut invariant: %u/%u coarse entries have no children (hole when the cut refines past them)", dgOrphans, s_total);
    }

    if (!stateOk) {
        Msg("![VK WorldGPU] cluster-stream state failed — GPU world path disabled");
        s_groups.clear();
        return;
    }

    _b.Start();

    // meta SSBO (device-local)
    s_meta = xr_new<CVulkanBuffer>();
    s_meta->Create(sizeof(GpuMeshMeta) * s_total, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_meta->Upload(meta.data(), sizeof(GpuMeshMeta) * s_total);

    // r_world_bindless: the cull entry -> material table (binding 2 of every frame's set).
    if (s_bindless) {
        entryMat.resize(s_total, 0u);
        s_blEntryMat = xr_new<CVulkanBuffer>();
        s_blEntryMat->Create(sizeof(u32) * s_total, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
        s_blEntryMat->Upload(entryMat.data(), sizeof(u32) * s_total);
        for (u32 f = 0; f < kBlFrames; ++f)
            VK::DescriptorWriter(s_blSet[f]).StorageBuffer(2, s_blEntryMat->GetHandle()).Flush();
        u32 nBl = 0;
        for (const Group& g : s_groups) nBl += g.bindless ? 1u : 0u;
        Msg("[VK WorldGPU] r_world_bindless: %u materials in %u groups (+%u per-material groups) | %u texture slots",
            (u32)s_blMats.size(), nBl, (u32)s_groups.size() - nBl, (u32)s_blMats.size() * 5u);
    }

    // Per-group cmd-region bases = entryOffset (exact prefix-sum layout: the
    // indirect buffer holds exactly s_total cmds; region g = [base[g], +entryCount)).
    {
        xr_vector<u32> bases(nGroups);
        for (u32 g = 0; g < nGroups; ++g) bases[g] = s_groups[g].entryOffset;
        s_groupBase = xr_new<CVulkanBuffer>();
        s_groupBase->Create(sizeof(u32) * nGroups, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
        s_groupBase->Upload(bases.data(), sizeof(u32) * nGroups);
    }

    // TRANSFER_SRC: the draw-command audit copies cmds+counts back for verification.
    const VkBufferUsageFlags iu = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    s_indirect = xr_new<CVulkanBuffer>();
    s_indirect->Create((VkDeviceSize)s_total * sizeof(VkDrawIndexedIndirectCommand), iu,
                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_count = xr_new<CVulkanBuffer>();
    s_count->Create((VkDeviceSize)nGroups * sizeof(u32), iu, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    if (!CreateCullPipeline()) { Msg("![VK WorldGPU] cull pipeline failed — disabled"); s_groups.clear(); return; }

    // Phase A: a SECOND indirect/count set + Hi-Z cull pipeline for r_hzb_cull.
    // Best-effort — if it fails, the base frustum path is unaffected (s_occlReady
    // stays false → CullColor no-ops, DrawColor uses the frustum set).
    s_indirect2 = xr_new<CVulkanBuffer>();
    s_indirect2->Create((VkDeviceSize)s_total * sizeof(VkDrawIndexedIndirectCommand), iu,
                        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_count2 = xr_new<CVulkanBuffer>();
    s_count2->Create((VkDeviceSize)nGroups * sizeof(u32), iu, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_occlReady = CreateCullColorPipeline();
    if (!s_occlReady) Msg("![VK WorldGPU] Hi-Z occlusion cull unavailable (r_hzb_cull will be a no-op)");

    // Two-phase (r_hzb_prepass): the late list + its pipeline. INDIRECT_BUFFER
    // because its first three words are the vkCmdDispatchIndirect arguments for
    // phase 2 — the count never travels to the CPU. Reuses s_indirect2/s_count2
    // as the phase-2 output, so this costs 1 MB, not another 16 MB command buffer.
    s_late = xr_new<CVulkanBuffer>();
    s_late->Create(kLateHeaderBytes + (VkDeviceSize)kLateCap * sizeof(u32),
                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                       | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_twoReady = CreateCullTwoPipeline();   // independent of s_occlReady: it needs the BUFFERS, not that pipeline
    if (!s_twoReady) Msg("![VK WorldGPU] two-phase prepass cull unavailable (r_hzb_prepass will be a no-op)");

    // Phase 3: shadow-caster indirect/count slices (far + 2 cascades) + the
    // ortho-cut cull pipeline. Best-effort — failure keeps the per-mesh
    // ShadowGPU fallback (vk_pass_shadow gates on ShadowReady()).
    s_indirectSh = xr_new<CVulkanBuffer>();
    s_indirectSh->Create((VkDeviceSize)kShadowTargets * s_total * sizeof(VkDrawIndexedIndirectCommand), iu,
                         VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_countSh = xr_new<CVulkanBuffer>();
    s_countSh->Create((VkDeviceSize)kShadowTargets * nGroups * sizeof(u32), iu, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    {   // zero-init: a draw can hit a target slice before its first cull (live
        // r_shadow_cluster toggle, VSM-active fog reuse) — count 0 draws nothing.
        xr_vector<u32> zeros(kShadowTargets * nGroups, 0u);
        s_countSh->Upload(zeros.data(), zeros.size() * sizeof(u32));
    }
    s_shadowReady = CreateShadowCullPipeline();
    if (!s_shadowReady) Msg("![VK WorldGPU] shadow cluster cull unavailable (r_shadow_cluster will be a no-op)");

    // Host-visible readback for the occlusion-stats log (frustum vs occlusion counts).
    s_nGroups = nGroups;
    if (s_occlReady) {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = (VkDeviceSize)2 * nGroups * sizeof(u32);
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo ai{};
        if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &s_countReadback, &s_countReadbackAlloc, &ai) == VK_SUCCESS)
            s_countReadbackPtr = (u32*)ai.pMappedData;
    }

    msBufs = _b.GetElapsed_ms_total();
    Msg("[VK WorldGPU] built: %u GPU meshes -> %u cull entries, %u groups | cpu-set=%u (non-GPU static leaves) | occlusion=%d",
        nMeshes, s_total, nGroups, (u32)s_cpuMeshes.size(), s_occlReady ? 1 : 0);
    // ---- What the group list would collapse to, per pass ---------------------
    // Every Draw* walks s_groups and issues one indirect-draw packet per group,
    // so the group count IS the packet count of a pass. The DEPTH prepass does not
    // need a material for opaque geometry (position only), which is exactly the
    // observation vk_vsm.cpp already exploits: its caster loop keys regions by
    // BUFFER COMBO (vb/ib/stride/iType) instead of by material and "stays a handful
    // of draws even at 5k+ material groups". Print both counts here so the win of
    // doing the same in DrawDepth is a number before it is a refactor.
    {
        struct ComboKey { VkBuffer vb, ib; u32 stride, tcOffset; u32 iType; u32 at; };
        xr_vector<ComboKey> combos, combosAT;
        auto add = [](xr_vector<ComboKey>& v, const ComboKey& k) {
            for (const ComboKey& e : v)
                if (e.vb == k.vb && e.ib == k.ib && e.stride == k.stride
                    && e.iType == k.iType && e.tcOffset == k.tcOffset && e.at == k.at) return;
            v.push_back(k);
        };
        u32 nAT = 0, nTerr = 0;
        for (const Group& g : s_groups) {
            const bool at = g.mat && g.mat->alphaRef >= 0.f;
            if (at) ++nAT;
            if (g.terrain) ++nTerr;
            // Opaque depth needs no material at all -> tcOffset/AT are irrelevant.
            ComboKey ko{ g.vb, g.ib, g.stride, 0u, (u32)g.iType, 0u };
            // The AT depth variant still needs the material's diffuse bound, so
            // AT groups can only collapse per (combo x material) -> count them apart.
            ComboKey ka{ g.vb, g.ib, g.stride, g.tcOffset, (u32)g.iType, 1u };
            if (at) add(combosAT, ka); else add(combos, ko);
        }
        Msg("[VK WorldGPU] packets/pass: %u material groups -> depth could be %u opaque buffer-combos + %u AT combos (AT groups %u, terrain groups %u)",
            nGroups, (u32)combos.size(), (u32)combosAT.size(), nAT, nTerr);
    }
    Msg("[load step]   WorldGPU::Build: extract %.0f | clusters %llu | pools %.0f | sort %.0f | swiDiag %.0f | group %.0f | vbDiag %.0f | cutDiagWait %.0f (thread %.0f) | metaCopy %.0f | state %.0f | bufs+pipes %.0f ms",
        msExtract, (unsigned long long)clusterMs, msPools, msSort, msSwi, msGroup,
        msValid, msCut, msCutThread, msMetaCopy, msState, msBufs);
    if (!clustered.empty()) {
        u32 mergedMeshes = 0;
        for (const auto& kv : clustered) if (kv.second.ib32) ++mergedMeshes;
        u64 ibBytes16 = 0, ibBytes32 = 0;   // from the page dir (blobs may live on disk)
        for (const auto& pr : s_pageDir) ((pr.flags & ClusterStream::kPageIB32) ? ibBytes32 : ibBytes16) += pr.byteSize;
        Msg("[VK Cluster] %u meshes -> %u entries (IB %.1f MB u16 + %.1f MB u32 in %u pages) in %llu ms | DAG groups: %u simplified, %u stalled",
            (u32)clustered.size(), (u32)clusterMeta.size(),
            double(ibBytes16) / (1024.0 * 1024.0),
            double(ibBytes32) / (1024.0 * 1024.0),
            (u32)_max<size_t>(1, s_pageDir.size()) - 1, (unsigned long long)clusterMs,
            s_statSimplified, s_statStalled);
        if (mergedMeshes)
            Msg("[VK Cluster] merge: %u meshes in component DAGs%s", mergedMeshes,
                s_statCompUnits ? "" : " (from cache)");
        if (s_statCompUnits)
            Msg("[VK Cluster] merge: %u components baked from %u meshes", s_statCompUnits, s_statCompMeshes);
    }
    else if (ps_r_cluster)
        Msg("[VK Cluster] no meshes eligible for cluster split (r_cluster_tris=%d)", ps_r_cluster_tris);
}

u32 SubmitCpuMeshes(VK::RenderQueue& q, const Fmatrix& viewProj, bool doCull)
{
    if (s_cpuMeshes.empty()) return 0;
    Fvector4 planes[6];
    if (doCull) VK::ExtractFrustumPlanes(viewProj, planes);   // same 6-plane test as the GPU cull
    Fmatrix identity; identity.identity();
    u32 n = 0;
    for (vkFVisual* fv : s_cpuMeshes) {
        if (doCull) {
            const Fvector& c = fv->vis.sphere.P; const float r = fv->vis.sphere.R;
            if (r > 0.f) {
                bool outside = false;
                for (int i = 0; i < 6; ++i)
                    if (planes[i].x*c.x + planes[i].y*c.y + planes[i].z*c.z + planes[i].w < -r) { outside = true; break; }
                if (outside) continue;
            }
        }
        fv->Submit(q, identity, 0.0f);   // leaf → one DrawItem (no hierarchy recursion, no dups)
        ++n;
    }
    return n;
}

bool Built() { return s_built && !s_groups.empty() && s_cullPipe != VK_NULL_HANDLE; }

bool InSet(vkRender_Visual* v) { return std::binary_search(s_meshSet.begin(), s_meshSet.end(), v); }
u32  SetSize() { return (u32)s_meshSet.size(); }

void Cull(VkCommandBuffer cmd, const Fmatrix& viewProj)
{
    Cull(cmd, viewProj, RV().vCameraPosition, RV().vCameraDirection);
}

void Cull(VkCommandBuffer cmd, const Fmatrix& viewProj, const Fvector& camPos, const Fvector& viewDir)
{
    if (!Built()) return;
    const u32 nGroups = (u32)s_groups.size();

    Fvector4 planes[6];
    VK::ExtractFrustumPlanes(viewProj, planes);

    // WAR-guard vs the previous frame's indirect reads (TRANSFER src stage: the
    // count-stats copy also reads these buffers).
    MemBarrier(cmd, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdFillBuffer(cmd, s_count->GetHandle(), 0, (VkDeviceSize)nGroups * sizeof(u32), 0u);
    MemBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullLayout, 0, 1, &s_set, 0, nullptr);
    CullPush pc{};
    for (int p = 0; p < 6; ++p) pc.planes[p] = planes[p];
    pc.cameraPos.set(camPos.x, camPos.y, camPos.z, 0.f);
    pc.viewDir.set(viewDir.x, viewDir.y, viewDir.z, 0.f);
    pc.lodParams = LodParams();
    pc.numGroups = nGroups; pc.ssaCull = SsaCull(); pc.total = s_total;
    vkCmdPushConstants(cmd, s_cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (s_total + 255) / 256, 1, 1);

    // r_fsinv_split companion — [VK Cut]: THIS view's drawn-cut composition with
    // TRIANGLE sums, CPU-recomputed (frustum + SSA + residency + DAG cut mirror
    // the shader). Attributes the FS-invocation load to entry classes — healthy
    // cut levels vs level-0 clusters with an INF/absent parent (those never
    // coarsen at range → sub-pixel triangle soup → quad-helper cost).
    if (ps_r_fsinv_split > 0 && !s_metaCPU.empty()) {
        static u32 s_lastCut = 0;
        if (RV().dwTimeGlobal > s_lastCut + 3000) {
            s_lastCut = RV().dwTimeGlobal;
            const Fvector4 lp   = LodParams();
            const float    ssa  = SsaCull();
            const u32*     sbits = ClusterStream::HostBits();
            u32 nAll = 0, nL0 = 0, nInf = 0, nPlain = 0;
            u64 tAll = 0, tL0 = 0, tInf = 0, tPlain = 0;
            for (u32 ei = 0; ei < (u32)s_metaCPU.size(); ++ei) {
                const GpuMeshMeta& m = s_metaCPU[ei];
                bool out = false;
                for (int p = 0; p < 6 && !out; ++p)
                    out = planes[p].x * m.sphere_P.x + planes[p].y * m.sphere_P.y
                        + planes[p].z * m.sphere_P.z + planes[p].w < -m.sphere_R;
                if (out) continue;
                const bool plain = (m.flags & 2u) != 0u;
                if (ssa > 0.f && plain) {
                    const float dz = _max(lp.y, viewDir.dotproduct(Fvector{ m.sphere_P.x - camPos.x, m.sphere_P.y - camPos.y, m.sphere_P.z - camPos.z }) - m.sphere_R);
                    if (m.sphere_R * ssa < dz) continue;
                }
                bool leaf = false;
                if (sbits) {
                    const u32 sb = (sbits[ei >> 4u] >> ((ei & 15u) * 2u)) & 3u;
                    if (!(sb & 1u)) continue;
                    leaf = (sb & 2u) != 0u;
                }
                const float dsd = _max(lp.y, viewDir.dotproduct(Fvector{ m.lodSelf.x - camPos.x, m.lodSelf.y - camPos.y, m.lodSelf.z - camPos.z }) - m.lodSelf.w);
                const float dpd = _max(lp.y, viewDir.dotproduct(Fvector{ m.lodParent.x - camPos.x, m.lodParent.y - camPos.y, m.lodParent.z - camPos.z }) - m.lodParent.w);
                const float sp = m.selfError * lp.x / dsd;
                const float pp = _min(m.parentError, kErrInf) * lp.x / dpd;
                if (!((sp <= 1.f || leaf) && pp > 1.f)) continue;
                const u64 tris = m.index_count / 3;
                ++nAll; tAll += tris;
                if (plain)                         { ++nPlain; tPlain += tris; }
                else if (m.parentError >= kErrInf) { ++nInf;   tInf   += tris; }
                else if (m.selfError == 0.f)       { ++nL0;    tL0    += tris; }
            }
            Msg("[VK Cut] view cut: entries=%u tris=%.2fM | L0(healthy)=%u/%.2fM infParent=%u/%.2fM plain=%u/%.2fM | lodx=%.0f ssa=%.1f",
                nAll, double(tAll) / 1e6, nL0, double(tL0) / 1e6, nInf, double(tInf) / 1e6,
                nPlain, double(tPlain) / 1e6, lp.x, ps_r_ssa_px);
        }
    }

    MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
}

bool OcclusionReady() { return s_occlReady; }
bool TwoPhaseReady()  { return s_twoReady; }

namespace {

// Point a two-phase set's binding 8 at this frame's pyramid — ONLY on change.
// s_setTwo[i] is a single set with several frames in flight, so an unconditional
// write updates a set that executing command buffers still reference (the
// VUID-vkUpdateDescriptorSets-None-03047 trap that CullColor and the tree cull
// both document). Keyed on the generation too: Destroy+Create can return the
// same VkImageView value, and a handle-only check would keep a dead object bound.
void BindTwoHZB(u32 i, VkImageView view, VkSampler smp, u32 gen)
{
    if (view == s_twoHzbView[i] && smp == s_twoHzbSmp[i] && gen == s_twoHzbGen[i]) return;
    VK::DescriptorWriter(s_setTwo[i]).ImageSampler(8, view, smp, VK_IMAGE_LAYOUT_GENERAL).Flush();
    s_twoHzbView[i] = view; s_twoHzbSmp[i] = smp; s_twoHzbGen[i] = gen;
}

// The half of the push both phases fill identically: the CUT is decided with THIS
// frame's camera in both. Only viewProj/hzbEye/mode/hzbValid differ.
void FillTwoPushCommon(CullTwoPush& pc, const Fmatrix& viewProj, const Fvector& camPos,
                       const Fvector& viewDir)
{
    VK::ExtractFrustumPlanes(viewProj, pc.planes);
    pc.cameraPos.set(camPos.x, camPos.y, camPos.z, 0.f);
    pc.viewDir.set(viewDir.x, viewDir.y, viewDir.z, 0.f);
    pc.lodParams = LodParams();
    pc.numGroups = (u32)s_groups.size();
    pc.ssaCull   = SsaCull();
    pc.total     = s_total;
    pc.lateCap   = kLateCap;
}

} // anonymous namespace

void CullEarly(VkCommandBuffer cmd, const Fmatrix& viewProj, const Fvector& camPos,
               const Fvector& viewDir, VkImageView hzbView, VkSampler hzbSampler,
               u32 hzbGen, bool prevHzbUsable)
{
    if (!Built() || !s_twoReady) return;
    const u32 nGroups = (u32)s_groups.size();

    // Verify whatever the audit recorded a FIF cycle ago, exactly as Cull() does.
    // ⚠ The [VK Cut] composition log lives in Cull(), which two-phase replaces; it
    // is a tool from the closed white-polygon hunt and not load-bearing, so it is
    // left where it is rather than duplicated into a second scan — r_hzb_prepass 1
    // silences it.

    // The pyramid is only meaningful together with the frame it was built from.
    // Both must be one frame old; either one missing ⇒ run the cut with no
    // occlusion at all, which reproduces Cull() exactly.
    const bool usePrev = prevHzbUsable && s_prevVPFrame != 0xFFFFFFFFu
                      && s_prevVPFrame + 1 == RV().frameIndex
                      && hzbView != VK_NULL_HANDLE && hzbSampler != VK_NULL_HANDLE;

    // Binding 8 must name a live image even when the test is skipped: an unbound
    // combined-image-sampler is UB on dispatch whether or not the read executes.
    if (hzbView != VK_NULL_HANDLE && hzbSampler != VK_NULL_HANDLE)
        BindTwoHZB(0, hzbView, hzbSampler, hzbGen);

    // WAR-guard vs the previous frame's indirect reads of set A, and vs last
    // frame's late list (the phase-2 dispatch read it as an indirect argument).
    MemBarrier(cmd, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdFillBuffer(cmd, s_count->GetHandle(), 0, (VkDeviceSize)nGroups * sizeof(u32), 0u);
    // Late-list header: dispatch (0,1,1) + count 0. vkCmdUpdateBuffer, not
    // FillBuffer — the three words differ, and 16 B inline is free.
    const u32 hdr[4] = { 0u, 1u, 1u, 0u };
    vkCmdUpdateBuffer(cmd, s_late->GetHandle(), 0, sizeof(hdr), hdr);
    MemBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullPipeTwo);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullLayoutTwo, 0, 1, &s_setTwo[0], 0, nullptr);
    CullTwoPush pc{};
    FillTwoPushCommon(pc, viewProj, camPos, viewDir);
    pc.mode     = 0u;                       // EARLY
    pc.hzbValid = usePrev ? 1u : 0u;
    pc.viewProj = usePrev ? s_prevVP : viewProj;   // the matrix the pyramid was built under
    const Fvector eye = usePrev ? s_prevCam : camPos;
    pc.hzbEyeX = eye.x; pc.hzbEyeY = eye.y; pc.hzbEyeZ = eye.z;
    vkCmdPushConstants(cmd, s_cullLayoutTwo, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (s_total + 255) / 256, 1, 1);

    // Set A feeds the prepass draw; the late list feeds phase 2 both as storage
    // (the items) and as an indirect argument (the group count).
    MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT,
               VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // Remember this frame's camera for the NEXT frame's phase 1 — the pyramid it
    // will read is the one built from THIS frame's depth.
    s_prevVP      = viewProj;
    s_prevCam     = camPos;
    s_prevVPFrame = RV().frameIndex;
}

void CullLate(VkCommandBuffer cmd, const Fmatrix& viewProj, const Fvector& cameraPos,
              VkImageView hzbView, VkSampler hzbSampler, u32 hzbGen, bool hzbFresh)
{
    if (!Built() || !s_twoReady) return;
    // ⚠ THE ONE WAY THIS DESIGN COULD PUNCH A HOLE, AND ITS SEAL. Phase 1 has
    // already REMOVED the deferred entries from set A on the promise that this
    // pass re-tests them. If the mid-prepass rebuild silently did not happen
    // (BuildHZB early-outs when its descriptors don't match the mip count), the
    // pyramid here still holds LAST frame's depth and testing against it would
    // drop entries that are visible now — into neither set. So a stale pyramid
    // does not mean "skip the test", it means "keep everything": hzbValid 0, the
    // whole late list is emitted, and the prepass set is the full frustum set
    // again. Same for a missing view — but that one cannot be sampled at all, so
    // we bind whatever is already there and simply do not read it.
    if (hzbView == VK_NULL_HANDLE || hzbSampler == VK_NULL_HANDLE) hzbFresh = false;
    const u32 nGroups = (u32)s_groups.size();

    if (hzbView != VK_NULL_HANDLE && hzbSampler != VK_NULL_HANDLE)
        BindTwoHZB(1, hzbView, hzbSampler, hzbGen);

    // WAR-guard vs the previous frame's indirect reads of set B (the colour pass
    // consumed it) before we zero its counts.
    MemBarrier(cmd, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdFillBuffer(cmd, s_count2->GetHandle(), 0, (VkDeviceSize)nGroups * sizeof(u32), 0u);
    MemBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullPipeTwo);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullLayoutTwo, 0, 1, &s_setTwo[1], 0, nullptr);
    CullTwoPush pc{};
    FillTwoPushCommon(pc, viewProj, cameraPos, RV().vCameraDirection);
    pc.mode     = 1u;                      // LATE
    pc.hzbValid = hzbFresh ? 1u : 0u;      // 1 = the pyramid IS this frame's phase-1 depth
    pc.viewProj = viewProj;
    pc.hzbEyeX = cameraPos.x; pc.hzbEyeY = cameraPos.y; pc.hzbEyeZ = cameraPos.z;
    vkCmdPushConstants(cmd, s_cullLayoutTwo, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    // The group count came out of phase 1 and never touched the CPU. Zero groups
    // is legal and does nothing — exactly what an empty late list should cost.
    vkCmdDispatchIndirect(cmd, s_late->GetHandle(), 0);

    // Snapshot both count sets → host readback, exactly as CullColor does (it does
    // not run on this path, so nothing else is writing that buffer). Read a frame
    // or two stale, which is fine for a stat. This is the number that says whether
    // two-phase is doing anything: `early` should land near the single-phase
    // `visible`, `late` should be small (disocclusions only), and early+late is
    // what the prepass now rasters instead of the whole frustum set.
    if (s_countReadback) {
        MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferCopy cA{ 0, 0, (VkDeviceSize)nGroups * sizeof(u32) };
        vkCmdCopyBuffer(cmd, s_count->GetHandle(),  s_countReadback, 1, &cA);
        VkBufferCopy cB{ 0, (VkDeviceSize)nGroups * sizeof(u32), (VkDeviceSize)nGroups * sizeof(u32) };
        vkCmdCopyBuffer(cmd, s_count2->GetHandle(), s_countReadback, 1, &cB);
    }
    if (s_countReadbackPtr && (ps_r_profiler > 1 || ps_r_cluster_debug > 0)) {
        static u32 s_statLastTwo = 0;
        if (RV().dwTimeContinual > s_statLastTwo + 3000) {
            s_statLastTwo = RV().dwTimeContinual;
            u32 early = 0, late = 0;
            for (u32 g = 0; g < s_nGroups; ++g) { early += s_countReadbackPtr[g]; late += s_countReadbackPtr[s_nGroups + g]; }
            Msg("[VK WorldGPU] two-phase: early=%u + late=%u = %u drawn (%u total entries)%s",
                early, late, early + late, s_total, hzbFresh ? "" : " [pyramid stale: late untested]");
            {
                u32 gA = 0, gB = 0;
                for (u32 g = 0; g < s_nGroups; ++g) { if (s_countReadbackPtr[g]) ++gA; if (s_countReadbackPtr[s_nGroups + g]) ++gB; }
                Msg("[VK WorldGPU] groups with work: A=%u B=%u of %u (empty A=%u B=%u -> wasted indirect packets/pass)",
                    gA, gB, s_nGroups, s_nGroups - gA, s_nGroups - gB);
            }
        }
    }

    MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
}

void CullColor(VkCommandBuffer cmd, const Fmatrix& viewProj, const Fvector& cameraPos,
               VkImageView hzbView, VkSampler hzbSampler, u32 hzbGen)
{
    if (!Built() || !s_occlReady) return;
    if (hzbView == VK_NULL_HANDLE || hzbSampler == VK_NULL_HANDLE) return;
    const u32 nGroups = (u32)s_groups.size();

    // Occlusion-stats log (throttled): sum the readback copied a frame or two ago.
    // frustum = sum(s_count) = frustum-visible; occl = sum(s_count2) = also Hi-Z
    // visible; the difference is what occlusion removed from the heavy color pass.
    // Verbose-only (r_profiler 2 / cluster debug view on) — it's 2 lines every 3s.
    if (s_countReadbackPtr && (ps_r_profiler > 1 || ps_r_cluster_debug > 0)) {
        static u32 s_statLast = 0;
        if (RV().dwTimeContinual > s_statLast + 3000) {
            s_statLast = RV().dwTimeContinual;
            u32 frust = 0, occl = 0;
            for (u32 g = 0; g < s_nGroups; ++g) { frust += s_countReadbackPtr[g]; occl += s_countReadbackPtr[s_nGroups + g]; }
            Msg("[VK WorldGPU] occlusion: frustum=%u -> visible=%u (culled %u | %u total meshes)",
                frust, occl, frust >= occl ? frust - occl : 0u, s_total);
            {   // how many of the groups actually had work: every OTHER group still
                // costs one indirect draw packet in EVERY pass that walks the list.
                u32 gA = 0, gB = 0;
                for (u32 g = 0; g < s_nGroups; ++g) { if (s_countReadbackPtr[g]) ++gA; if (s_countReadbackPtr[s_nGroups + g]) ++gB; }
                Msg("[VK WorldGPU] groups with work: A=%u B=%u of %u (empty A=%u B=%u -> wasted indirect packets/pass)",
                    gA, gB, s_nGroups, s_nGroups - gA, s_nGroups - gB);
            }

            // CPU replica of the shader's DAG cut (no frustum) — diverging from
            // the GPU counts pins a bug to the runtime side; matching counts pin
            // it to the DAG data. Also splits what the cut keeps at this camera.
            if (!s_metaCPU.empty()) {
                const Fvector4 lp = LodParams();
                const Fvector cam = RV().vCameraPosition;
                const Fvector dir = RV().vCameraDirection;   // view-Z metric — must match the shaders
                const u32* sbits = ClusterStream::HostBits();  // Stage B residency (must match the GPU test)
                u32 drawn = 0, drawnComp = 0, drawnInf = 0, drawnL0 = 0, drawnLeaf = 0;
                for (u32 ei = 0; ei < (u32)s_metaCPU.size(); ++ei) {
                    const GpuMeshMeta& m = s_metaCPU[ei];
                    bool leaf = false;
                    if (sbits) {
                        const u32 sb = (sbits[ei >> 4u] >> ((ei & 15u) * 2u)) & 3u;
                        if (!(sb & 1u)) continue;   // page/cohort not drawable
                        leaf = (sb & 2u) != 0u;
                    }
                    const float dsd = _max(lp.y, dir.dotproduct(Fvector{ m.lodSelf.x - cam.x, m.lodSelf.y - cam.y, m.lodSelf.z - cam.z }) - m.lodSelf.w);
                    const float dpd = _max(lp.y, dir.dotproduct(Fvector{ m.lodParent.x - cam.x, m.lodParent.y - cam.y, m.lodParent.z - cam.z }) - m.lodParent.w);
                    const float sp = m.selfError * lp.x / dsd;
                    const float pp = _min(m.parentError, kErrInf) * lp.x / dpd;
                    if ((sp <= 1.f || leaf) && pp > 1.f) {
                        ++drawn;
                        if (sp > 1.f) ++drawnLeaf;
                        if (m._pad1 != 0) ++drawnComp;   // paged (clustered) entries — first_vertex went page-local in slice 2
                        if (m.parentError >= kErrInf) ++drawnInf;
                        if (m.selfError == 0.f) ++drawnL0;
                    }
                }
                Msg("[VK Cluster] cpu-cut: drawn=%u (paged=%u, level0=%u, infParent=%u, streamLeaf=%u) lodx=%.1f cam=(%.0f,%.0f,%.0f)",
                    drawn, drawnComp, drawnL0, drawnInf, drawnLeaf, lp.x, cam.x, cam.y, cam.z);
            }

            // ---- MERGE CEILING: what merging contiguous clusters could pay ----
            // A draw was priced at ~95 ns in the depth prepass and ~82 ns in the
            // colour pass (six-point sweep, 26-08). So the question that decides
            // whether merging is worth a cull-shader rewrite is one number: how
            // many of the visible entries sit NEXT TO EACH OTHER in the index
            // buffer? Entries of one mesh are contiguous in s_metaCPU, so an
            // in-order walk answers it exactly — no readback, no GPU pass.
            //
            // Two commands are one command iff they land in the same group (same
            // pipeline/material/buffers), the same streaming page (page bases
            // differ), the same vertexOffset, and the second's firstIndex is where
            // the first's range ends.
            //
            // ⚠ This replica culls by the DAG cut + FRUSTUM and NOT by Hi-Z, which
            // makes it the DEPTH PREPASS's set — the pass with 50% per-draw cost,
            // and the one worth merging. Sanity gate: `visible` here must land near
            // `frustum=` in the occlusion line above. If it does not, the replica
            // is wrong and so is the ceiling.
            if (!s_metaCPU.empty()) {
                const Fvector4 lp = LodParams();
                const Fvector  cam = RV().vCameraPosition;
                const Fvector  dir = RV().vCameraDirection;
                const u32*     sbits = ClusterStream::HostBits();
                Fmatrix fm = viewProj;
                CFrustum fr; fr.CreateFromMatrix(fm, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);

                u32 vis = 0, runs = 0, longest = 0, cur = 0;
                u32 prevEnd = 0, prevGroup = 0xFFFFFFFFu, prevPage = 0xFFFFFFFFu, prevVtx = 0xFFFFFFFFu;
                bool have = false;
                for (u32 ei = 0; ei < (u32)s_metaCPU.size(); ++ei) {
                    const GpuMeshMeta& m = s_metaCPU[ei];
                    bool leaf = false;
                    if (sbits) {
                        const u32 sb = (sbits[ei >> 4u] >> ((ei & 15u) * 2u)) & 3u;
                        if (!(sb & 1u)) continue;
                        leaf = (sb & 2u) != 0u;
                    }
                    const float dsd = _max(lp.y, dir.dotproduct(Fvector{ m.lodSelf.x - cam.x, m.lodSelf.y - cam.y, m.lodSelf.z - cam.z }) - m.lodSelf.w);
                    const float dpd = _max(lp.y, dir.dotproduct(Fvector{ m.lodParent.x - cam.x, m.lodParent.y - cam.y, m.lodParent.z - cam.z }) - m.lodParent.w);
                    const float sp  = m.selfError * lp.x / dsd;
                    const float pp  = _min(m.parentError, kErrInf) * lp.x / dpd;
                    if (!((sp <= 1.f || leaf) && pp > 1.f)) continue;
                    if (!fr.testSphere_dirty(m.sphere_P, m.sphere_R)) continue;

                    ++vis;
                    const bool joins = have && m.group == prevGroup && m._pad1 == prevPage
                                    && m.first_vertex == prevVtx && m.ib_first == prevEnd;
                    if (joins) ++cur;
                    else { if (cur > longest) longest = cur; ++runs; cur = 1; }
                    have = true;
                    prevGroup = m.group; prevPage = m._pad1; prevVtx = m.first_vertex;
                    prevEnd   = m.ib_first + m.index_count;
                }
                if (cur > longest) longest = cur;
                Msg("[VK WorldGPU] merge ceiling: %u visible -> %u draws if contiguous runs merge (%.2fx fewer, longest run %u) | at 95ns/draw that is %.2f ms",
                    vis, runs, runs ? double(vis) / double(runs) : 0.0, longest,
                    double(vis > runs ? vis - runs : 0u) * 95e-6);
            }
        }
    }

    // Point binding 3 at this frame's HZB (view/sampler can change on resize).
    // HZB lives in GENERAL (see CreateHZB).
    // ⚠ ONLY ON CHANGE. s_set2 is a SINGLE descriptor set while VK_FRAMES_IN_FLIGHT
    // frames are in flight: rewriting it every frame updates a set that command
    // buffers still executing on the GPU reference — UB, and the source of the
    // live-descriptor validation errors (VUID-vkUpdateDescriptorSets-None-03047,
    // dstBinding 3) that were being written off as another subsystem's noise. The
    // tree cull's binding-4 write documents the same trap. Keyed on the GENERATION
    // as well as the handle: Destroy+Create can return the same VkImageView value,
    // and a handle-only check would then keep a descriptor bound to a dead object.
    if (hzbView != s_hzbBoundView || hzbSampler != s_hzbBoundSampler || hzbGen != s_hzbBoundGen) {
        VK::DescriptorWriter(s_set2)
            .ImageSampler(3, hzbView, hzbSampler, VK_IMAGE_LAYOUT_GENERAL)
            .Flush();
        s_hzbBoundView    = hzbView;
        s_hzbBoundSampler = hzbSampler;
        s_hzbBoundGen     = hzbGen;
    }

    // WAR-guard vs the previous frame's indirect reads of cmds2/count2
    // (TRANSFER src stage: the count-stats copy also reads them).
    MemBarrier(cmd, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdFillBuffer(cmd, s_count2->GetHandle(), 0, (VkDeviceSize)nGroups * sizeof(u32), 0u);
    MemBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullPipe2);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullLayout2, 0, 1, &s_set2, 0, nullptr);
    CullColorPush pc{};
    pc.viewProj = viewProj;
    VK::ExtractFrustumPlanes(viewProj, pc.planes);   // same normalized planes as the frustum cull
    pc.cameraPos.set(cameraPos.x, cameraPos.y, cameraPos.z, 0.f);
    pc.viewDir.set(RV().vCameraDirection.x, RV().vCameraDirection.y, RV().vCameraDirection.z, 0.f);
    pc.lodParams = LodParams();
    pc.numGroups = nGroups; pc.ssaCull = SsaCull(); pc.total = s_total;
    vkCmdPushConstants(cmd, s_cullLayout2, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (s_total + 255) / 256, 1, 1);

    // Snapshot both count sets → host readback (for the throttled stats log above,
    // read next frame). Global compute-write→transfer-read barrier covers s_count
    // (frustum, written earlier) and s_count2 (just dispatched).
    if (s_countReadback) {
        MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferCopy cA{ 0, 0, (VkDeviceSize)nGroups * sizeof(u32) };
        vkCmdCopyBuffer(cmd, s_count->GetHandle(),  s_countReadback, 1, &cA);
        VkBufferCopy cB{ 0, (VkDeviceSize)nGroups * sizeof(u32), (VkDeviceSize)nGroups * sizeof(u32) };
        vkCmdCopyBuffer(cmd, s_count2->GetHandle(), s_countReadback, 1, &cB);
    }

    MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
}

void DrawDepth(VkCommandBuffer cmd, const Fmatrix& viewProj, bool displaceTerrain, DrawSet set, bool mvTarget)
{
    if (!Built()) return;
    VkPipelineLayout layoutSolid = PipelineCache::GetDepthLayout();
    VkPipelineLayout layoutAT    = PipelineCache::GetDepthATLayout();
    if (layoutSolid == VK_NULL_HANDLE) return;
    const u32 nGroups = (u32)s_groups.size();

    struct ATPush { Fmatrix mvp; float uvScale[2]; float aref; float _pad; };   // matches FlushDepth
    Fmatrix vp = viewProj;

    VkPipeline       lastPipe   = VK_NULL_HANDLE;
    VkPipelineLayout lastLayout = VK_NULL_HANDLE;
    VkDescriptorSet  lastMatSet = VK_NULL_HANDLE;
    VkBuffer    lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;
    // ⭐r_world_push_hoist: layout, для которого голова пуш-блока (mvp + uvScale)
    // уже записана. Пуш-константы сохраняются в буфере до привязки конвейера с
    // НЕСОВМЕСТИМЫМ по ним layout, поэтому ключ -- именно layout, и он сбрасывается
    // ровно там же, где сбрасывается lastLayout. DrawColor так делает с самого
    // начала; здесь матрица пушилась каждой из 639 групп.
    VkPipelineLayout vpPushedFor = VK_NULL_HANDLE;
    const bool hoist = (ps_r_world_push_hoist != 0);
    constexpr u32 kAtArefOffset = sizeof(Fmatrix) + 2 * sizeof(float);   // 72 -- aref в ATPush

    for (u32 g = 0; g < nGroups; ++g) {
        const Group& grp = s_groups[g];
        WorldMaterial* mat = grp.mat;

        // Terrain in the DEPTH PREPASS: snow-displaced terrain depth pipeline (same
        // world_terrain.vert as color -> matching displacement -> no z-fight). Set 1
        // = EnvLight (sf_params.w). Only in the prepass (displaceTerrain); shadows
        // pass false. Terrain groups sort first (group key isTerrain?0:1).
        if (displaceTerrain && mat->isTerrain && mat->terrainSet != VK_NULL_HANDLE) {
            VkPipeline       tpipe = PipelineCache::GetTerrainDepthPipeline(mvTarget);
            VkPipelineLayout tlay  = PipelineCache::GetTerrainLayout();
            VkDescriptorSet  eset  = EnvLight::GetCurrentSet();
            if (tpipe == VK_NULL_HANDLE || tlay == VK_NULL_HANDLE || eset == VK_NULL_HANDLE) continue;
            if (tlay != lastLayout) { lastLayout = tlay; lastMatSet = VK_NULL_HANDLE; lastPipe = VK_NULL_HANDLE; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; vpPushedFor = VK_NULL_HANDLE; }
            if (tpipe != lastPipe)  { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tpipe); lastPipe = tpipe; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; }
            if (eset != lastMatSet) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tlay, 1, 1, &eset, 0, nullptr); lastMatSet = eset; }   // set 1 = EnvLight
            // Set 0 = terrain material: the tessellation eval samples uMask (soil
            // softness for the mud carve) — unbound set 0 here was a device-lost.
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tlay, 0, 1, &mat->terrainSet, 0, nullptr);
            struct TPush { Fmatrix mvp; float uv[2]; float aref; float ds; } tp{}; tp.mvp = vp;
            // uvScale must match the color pass (1/1024): the TES samples uMask at vUV
            // for the mud carve — zero uvScale = different carve = prepass z-fight.
            tp.uv[0] = tp.uv[1] = 1.0f / 1024.0f;
            tp.ds = mat->detailScale;
            vkCmdPushConstants(cmd, tlay, PipelineCache::GetPushStages(), 0, sizeof(TPush), &tp);
            if (grp.vb != lastVB) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &z); lastVB = grp.vb; }
            if (grp.ib != lastIB || grp.iType != lastIType) { vkCmdBindIndexBuffer(cmd, grp.ib, 0, grp.iType); lastIB = grp.ib; lastIType = grp.iType; }
            const VkDeviceSize cmdOffT = (VkDeviceSize)grp.entryOffset * sizeof(VkDrawIndexedIndirectCommand);
            const VkDeviceSize cntOffT = (VkDeviceSize)g * sizeof(u32);
            EmitIndirect(cmd, set, cmdOffT, cntOffT, grp.entryCount);
            continue;
        }

        const bool at = mat->alphaRef >= 0.f;
        if (at && (layoutAT == VK_NULL_HANDLE || mat->set == VK_NULL_HANDLE)) continue;  // mirror FlushDepth skip

        // Crossfade (r_cluster_fade): solid statics swap to the dithering depth
        // variant so prepass coverage matches the color pass exactly. AT keeps
        // its own pipeline — AT entries are hard-cut in the cull (meta flag).
        const bool fade = ClusterFade() > 0.f;
        VkPipeline pipe = at ? PipelineCache::GetDepthATPipeline(grp.stride, grp.tcOffset, mvTarget)
                             : (fade ? PipelineCache::GetDepthFadePipeline(grp.stride, mvTarget) : VK_NULL_HANDLE);
        if (!at && pipe == VK_NULL_HANDLE) pipe = PipelineCache::GetDepthPipeline(grp.stride, mvTarget);
        if (pipe == VK_NULL_HANDLE) continue;
        VkPipelineLayout layout = at ? layoutAT : layoutSolid;

        if (layout != lastLayout) { lastLayout = layout; lastMatSet = VK_NULL_HANDLE; lastPipe = VK_NULL_HANDLE; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; vpPushedFor = VK_NULL_HANDLE; }
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; }

        if (at) {
            if (mat->set != lastMatSet) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &mat->set, 0, nullptr); lastMatSet = mat->set; }
            if (hoist) {
                // Голова блока (mvp + uvScale) одна на весь цикл; по группам меняется
                // только aref -- 4 байта вместо 80.
                if (vpPushedFor != layout) {
                    ATPush head{}; head.mvp = vp; head.uvScale[0] = 1.0f/1024.0f; head.uvScale[1] = 1.0f/1024.0f;
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, kAtArefOffset, &head);
                    vpPushedFor = layout;
                }
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   kAtArefOffset, sizeof(float), &mat->alphaRef);
            } else {
                ATPush push{}; push.mvp = vp; push.uvScale[0] = 1.0f/1024.0f; push.uvScale[1] = 1.0f/1024.0f; push.aref = mat->alphaRef;
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            }
        } else if (!hoist || vpPushedFor != layout) {
            // Солидным группам в этом цикле нужна ТОЛЬКО матрица -- значит при выносе
            // вызов на группу не нужен вообще.
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Fmatrix), &vp);
            vpPushedFor = layout;
        }

        if (grp.vb != lastVB) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &z); lastVB = grp.vb; }
        if (grp.ib != lastIB || grp.iType != lastIType) { vkCmdBindIndexBuffer(cmd, grp.ib, 0, grp.iType); lastIB = grp.ib; lastIType = grp.iType; }
        const VkDeviceSize cmdOff = (VkDeviceSize)grp.entryOffset * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)g * sizeof(u32);
        EmitIndirect(cmd, set, cmdOff, cntOff, grp.entryCount);
    }
}


// NORMAL G-buffer pass for the GPU-driven statics (GTAO real normals). Same
// culled set and the SAME indirect/count buffers the prepass just consumed, so
// the coverage matches exactly; only the pipeline differs (writes the vertex
// normal to a colour attachment, depth TEST-only). Terrain is skipped — its
// prepass depth is tessellated + snow-displaced, and a flat re-raster would sit
// behind it and fail LEQUAL.
void DrawNormals(VkCommandBuffer cmd, const Fmatrix& viewProj, DrawSet set)
{
    if (!Built()) return;
    VkPipelineLayout layoutSolid = PipelineCache::GetNormalLayout();
    VkPipelineLayout layoutAT    = PipelineCache::GetNormalATLayout();
    if (layoutSolid == VK_NULL_HANDLE) return;
    const u32 nGroups = (u32)s_groups.size();
    u32 nDrawn = 0, nTerrain = 0, nNoPipe = 0;   // coverage diag (see the tail)

    Fmatrix vp = viewProj;

    VkPipeline       lastPipe   = VK_NULL_HANDLE;
    VkPipelineLayout lastLayout = VK_NULL_HANDLE;
    VkDescriptorSet  lastMatSet = VK_NULL_HANDLE;
    VkBuffer    lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;
    VkPipelineLayout vpPushedFor = VK_NULL_HANDLE;   // r_world_push_hoist, как в DrawDepth
    const bool hoist = (ps_r_world_push_hoist != 0);
    constexpr u32 kNormalMiscOffset = offsetof(PipelineCache::NormalPush, misc);   // 96

    for (u32 g = 0; g < nGroups; ++g) {
        const Group& grp = s_groups[g];
        WorldMaterial* mat = grp.mat;
        if (mat && mat->isTerrain) { ++nTerrain; continue; }

        const bool at = mat && mat->alphaRef >= 0.f;
        if (at && (layoutAT == VK_NULL_HANDLE || mat->set == VK_NULL_HANDLE)) continue;

        VkPipeline pipe = at ? PipelineCache::GetNormalATPipeline(grp.stride, grp.tcOffset)
                             : PipelineCache::GetNormalPipeline(grp.stride);
        if (pipe == VK_NULL_HANDLE) { ++nNoPipe; continue; }
        VkPipelineLayout layout = at ? layoutAT : layoutSolid;

        if (layout != lastLayout) { lastLayout = layout; lastMatSet = VK_NULL_HANDLE; lastPipe = VK_NULL_HANDLE; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; vpPushedFor = VK_NULL_HANDLE; }
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; }
        if (at && mat->set != lastMatSet) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &mat->set, 0, nullptr);
            lastMatSet = mat->set;
        }

        // The GPU set is baked in world space: identity model, so the rows stay
        // identity and misc.y = 0 tells the VS to skip the rotation entirely.
        const VkShaderStageFlags stg = at ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                                          : VK_SHADER_STAGE_VERTEX_BIT;
        if (hoist) {
            // Всё, кроме misc.x (=aref), в этом цикле постоянно: mvp + единичные
            // строки модели + uvScale. Значит 96 байт -- один раз на layout,
            // 16 байт (misc) -- на группу.
            if (vpPushedFor != layout) {
                PipelineCache::NormalPush head{};
                head.mvp = vp;
                head.mi[0] = 1.f; head.mj[1] = 1.f;
                head.mi[3] = 1.0f / 1024.0f;
                head.mj[3] = 1.0f / 1024.0f;
                vkCmdPushConstants(cmd, layout, stg, 0, kNormalMiscOffset, &head);
                vpPushedFor = layout;
            }
            const float misc[4] = { at ? mat->alphaRef : -1.f, 0.f, 0.f, 0.f };
            vkCmdPushConstants(cmd, layout, stg, kNormalMiscOffset, sizeof(misc), misc);
        } else {
            PipelineCache::NormalPush push{};
            push.mvp = vp;
            push.mi[0] = 1.f; push.mj[1] = 1.f;
            push.mi[3] = 1.0f / 1024.0f;
            push.mj[3] = 1.0f / 1024.0f;
            push.misc[0] = at ? mat->alphaRef : -1.f;
            push.misc[1] = 0.f;
            vkCmdPushConstants(cmd, layout, stg, 0, sizeof(push), &push);
        }

        if (grp.vb != lastVB) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &z); lastVB = grp.vb; }
        if (grp.ib != lastIB || grp.iType != lastIType) { vkCmdBindIndexBuffer(cmd, grp.ib, 0, grp.iType); lastIB = grp.ib; lastIType = grp.iType; }
        const VkDeviceSize cmdOff = (VkDeviceSize)grp.entryOffset * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)g * sizeof(u32);
        EmitIndirect(cmd, set, cmdOff, cntOff, grp.entryCount);
        ++nDrawn;
    }
    // Coverage diag on the r_ssao_debug readback cadence. THIS is the branch
    // that carries the statics (the CPU queue reports draws=0 in a GPU-driven
    // level), so without this number the log could not say whether the pass
    // stamped anything at all — which is exactly what made the first in-game
    // A/B unreadable. Pairs with r_ssao_debug 5 (the on-screen source map).
    static u32 s_diagDN = 0;
    if (ps_r_ssao_debug && (s_diagDN++ % 300) == 0)
        Msg("[VK SSAO] WorldGPU::DrawNormals: groups=%u drawn=%u terrain-skipped=%u no-pipeline=%u",
            nGroups, nDrawn, nTerrain, nNoPipe);
}
// ---- Phase 3: cluster-LOD shadow casters ----------------------------------

VkBuffer MetaBuffer()      { return s_meta      ? s_meta->GetHandle()      : VK_NULL_HANDLE; }
VkBuffer GroupBaseBuffer() { return s_groupBase ? s_groupBase->GetHandle() : VK_NULL_HANDLE; }
u32      EntryCount()      { return s_total; }
u32      GroupCount()      { return (u32)s_groups.size(); }
const xr_vector<GpuMeshMeta>& HostMeta() { return s_metaCPU; }
u32      BuildStamp()      { return s_buildStamp; }

bool GetGroupShadow(u32 g, VkBuffer& vb, VkBuffer& ib, u32& stride, VkIndexType& iType,
                    u32& entryOffset, u32& entryCount, bool& alphaTested,
                    const WorldMaterial** matOut, u32* tcOffsetOut)
{
    if (g >= s_groups.size()) return false;
    const Group& grp = s_groups[g];
    vb = grp.vb; ib = grp.ib; stride = grp.stride; iType = grp.iType;
    entryOffset = grp.entryOffset; entryCount = grp.entryCount;
    alphaTested = grp.mat && grp.mat->alphaRef >= 0.f;
    if (matOut)      *matOut      = grp.mat;
    if (tcOffsetOut) *tcOffsetOut = grp.tcOffset;
    return true;
}

bool ShadowReady() { return Built() && s_shadowReady; }

// GPU AT casters: live cvar + the AT depth layout must exist. CullShadow and
// DrawShadow both read this inside one frame's command record (console vars
// apply between frames), so the culled set and the drawn groups always agree.
bool ShadowATActive() { return ps_r_gpu_shadows_at && PipelineCache::GetDepthATLayout() != VK_NULL_HANDLE; }

void CullShadow(VkCommandBuffer cmd, const u32* targets, const Fmatrix* viewProjs,
                const float* texelWorld, u32 n)
{
    if (!ShadowReady() || n == 0) return;
    const u32 nGroups = (u32)s_groups.size();
    const float k = _max(0.05f, ps_r_vsm_cluster_lod);

    // Zone INSIDE the function, not at the call sites. Shadow/CascCull wraps the
    // branch, so its 0.00 ms in the very frames where World/HZBcull read 7-8 ms
    // proves nothing: the zone opens whether or not this path was the one taken.
    // Measured here, a zero means "n dispatches over 830k entries are free" — and
    // that would rule out the meta array as the explanation for World/HZBcull,
    // because this reads the same array up to three times a frame.
    const int zSh = VK::Prof::ZoneBegin(cmd, "Shadow/CullWorld");

    // WAR-guard vs the previous frame's indirect reads of the shadow slices.
    MemBarrier(cmd, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    for (u32 i = 0; i < n; ++i)
        vkCmdFillBuffer(cmd, s_countSh->GetHandle(), (VkDeviceSize)targets[i] * nGroups * sizeof(u32),
                        (VkDeviceSize)nGroups * sizeof(u32), 0u);
    MemBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullPipe3);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cullLayout3, 0, 1, &s_set3, 0, nullptr);
    for (u32 i = 0; i < n; ++i) {
        CullShadowPush pc{};
        VK::ExtractFrustumPlanes(viewProjs[i], pc.planes);
        pc.errBudget = texelWorld[i] * k;
        pc.target    = targets[i];
        pc.numGroups = nGroups;
        pc.total     = s_total;
        pc.atOn      = ShadowATActive() ? 1u : 0u;
        vkCmdPushConstants(cmd, s_cullLayout3, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (s_total + 255) / 256, 1, 1);
    }
    MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
    VK::Prof::ZoneEnd(cmd, zSh);
}

void DrawShadow(VkCommandBuffer cmd, u32 target, const Fmatrix& lightVP, bool skipTerrain)
{
    if (!ShadowReady()) return;
    VkPipelineLayout layoutSolid = PipelineCache::GetDepthLayout();
    VkPipelineLayout layoutAT    = PipelineCache::GetDepthATLayout();
    if (layoutSolid == VK_NULL_HANDLE) return;
    const bool atOn = ShadowATActive();
    const u32 nGroups = (u32)s_groups.size();

    // Depth-only, statics are world-space → the same lightVP for every group.
    // AT groups draw with the discard pipeline + their material's diffuse
    // bound (mirrors DrawDepth); with atOn off their regions hold zero
    // commands (the cull skips flags bit0) — skip their binds. Terrain draws
    // through the plain per-stride depth pipeline (no displacement in shadow
    // targets, same as the CPU/ShadowGPU paths).
    struct ATPush { Fmatrix mvp; float uvScale[2]; float aref; float _pad; };   // matches FlushDepth
    Fmatrix vp = lightVP;

    VkPipeline       lastPipe   = VK_NULL_HANDLE;
    VkPipelineLayout lastLayout = VK_NULL_HANDLE;
    VkDescriptorSet  lastMatSet = VK_NULL_HANDLE;
    VkBuffer    lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;
    // r_world_push_hoist -- то же, что в DrawDepth, и здесь это дороже: цикл
    // прокручивается на дальнюю карту, оба каскада и каждую плитку спотов/куб.
    VkPipelineLayout vpPushedFor = VK_NULL_HANDLE;
    const bool hoist = (ps_r_world_push_hoist != 0);
    constexpr u32 kAtArefOffset = sizeof(Fmatrix) + 2 * sizeof(float);   // 72
    for (u32 g = 0; g < nGroups; ++g) {
        const Group& grp = s_groups[g];
        const bool at = grp.mat && grp.mat->alphaRef >= 0.f;
        if (at && (!atOn || grp.mat->set == VK_NULL_HANDLE)) continue;   // legacy split: AT → CPU cutout path
        if (skipTerrain && grp.terrain) continue;            // spot/point maps exclude terrain
        VkPipeline pipe = at ? PipelineCache::GetDepthATPipeline(grp.stride, grp.tcOffset)
                             : PipelineCache::GetDepthPipeline(grp.stride);
        if (pipe == VK_NULL_HANDLE) continue;
        VkPipelineLayout layout = at ? layoutAT : layoutSolid;
        if (layout != lastLayout) { lastLayout = layout; lastMatSet = VK_NULL_HANDLE; vpPushedFor = VK_NULL_HANDLE; }
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; }
        if (at) {
            if (grp.mat->set != lastMatSet) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &grp.mat->set, 0, nullptr); lastMatSet = grp.mat->set; }
            if (hoist) {
                if (vpPushedFor != layout) {
                    ATPush head{}; head.mvp = vp; head.uvScale[0] = 1.0f/1024.0f; head.uvScale[1] = 1.0f/1024.0f;
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, kAtArefOffset, &head);
                    vpPushedFor = layout;
                }
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   kAtArefOffset, sizeof(float), &grp.mat->alphaRef);
            } else {
                ATPush push{}; push.mvp = vp; push.uvScale[0] = 1.0f/1024.0f; push.uvScale[1] = 1.0f/1024.0f; push.aref = grp.mat->alphaRef;
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            }
        } else if (!hoist || vpPushedFor != layout) {
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Fmatrix), &vp);
            vpPushedFor = layout;
        }
        if (grp.vb != lastVB) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &z); lastVB = grp.vb; }
        if (grp.ib != lastIB || grp.iType != lastIType) { vkCmdBindIndexBuffer(cmd, grp.ib, 0, grp.iType); lastIB = grp.ib; lastIType = grp.iType; }
        const VkDeviceSize cmdOff = ((VkDeviceSize)target * s_total + grp.entryOffset) * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = ((VkDeviceSize)target * nGroups + g) * sizeof(u32);
        vkCmdDrawIndexedIndirectCount(cmd, s_indirectSh->GetHandle(), cmdOff, s_countSh->GetHandle(), cntOff,
                                      grp.entryCount, sizeof(VkDrawIndexedIndirectCommand));
    }
}

void DrawColor(VkCommandBuffer cmd, const Fmatrix& viewProj, VkDescriptorSet envSet, DrawSet drawSet, bool atEqual, bool prepassZ, int fsFrame)
{
    if (!Built()) return;
    const VkShaderStageFlags kStages = PipelineCache::GetPushStages();
    const u32 nGroups = (u32)s_groups.size();
    BlUpdate();   // r_world_bindless: this frame slot's texture slots + material records (once per frame)
    const VkDescriptorSet blSet = s_bindless ? s_blSet[CommandManager.GetCurrentFrame() % kBlFrames] : VK_NULL_HANDLE;
    // r_fsinv_split: terrain groups sort first in s_groups — query 1 brackets
    // them, query 2 the rest (see vk_vrs.h).
    const bool fsSplit = fsFrame >= 0;
    bool fsInTerrain = true;
    if (fsSplit) VK::VRS::SubStatsBegin(cmd, (u32)fsFrame, 1);
    // Set B only exists if a pass wrote it this frame: r_hzb_cull's CullColor or
    // two-phase's CullLate. Neither ready ⇒ fall back to A (the frustum set),
    // which is always written by Cull()/CullEarly().
    // (named drawSet, not set: the per-group loop below has its own VkDescriptorSet set)
    if (drawSet != DrawSet::A && !s_occlReady && !s_twoReady) drawSet = DrawSet::A;

    // mvp + uvScale at offset 0 (constant: world-space verts, SHORT2 SSCALED TCs) —
    // re-pushed per layout flip. Per-material tail {aref, detailScale, hemi} at 72.
    // Tess block at 84 = zero (we only bind flat, non-tess pipelines).
    struct MvpUV { Fmatrix mvp; float uvScale[2]; };
    MvpUV base{}; base.mvp = viewProj; base.uvScale[0] = 1.0f/1024.0f; base.uvScale[1] = 1.0f/1024.0f;
    struct TessBlock { float tessMax, tessNear, tessFar; float eyeHeight[4]; float pnScale; };
    const TessBlock tessZero{ 0.f, 0.f, 1.f, {0,0,0,0}, 0.f };
    constexpr u32 kTailOffset = sizeof(Fmatrix) + 2 * sizeof(float);   // 72
    constexpr u32 kTessOffset = sizeof(Fmatrix) + 5 * sizeof(float);   // 84

    VkPipeline       lastPipe   = VK_NULL_HANDLE;
    VkPipelineLayout lastLayout = VK_NULL_HANDLE;
    VkDescriptorSet  lastMatSet = VK_NULL_HANDLE;
    VkBuffer    lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;
    // r_world_push_hoist. Матрица здесь и так пушилась по смене layout; экономить
    // можно на ХВОСТЕ: {aref, detailScale, hemi} и streamID зависят ТОЛЬКО от
    // материала, а 639 групп дают 482 различных материала ⇒ примерно каждая
    // четвёртая группа продолжает материал предыдущей и обе записи ей не нужны.
    // Сбрасывается вместе с layout: пуш-константы там становятся неопределёнными.
    const WorldMaterial* lastMatPushed = nullptr;
    const bool hoist = (ps_r_world_push_hoist != 0);
    int markStage = 0;   // [nx gpu avg] w.stTerrain / w.stBindless (see the loop)

#ifdef __SWITCH__
    {
        static u32 s_terrLog = 0;   // what the terrain is: how many groups, cull entries (= max indirect commands), meshes
        if ((s_terrLog++ % 1800) == 0) {
            u32 tg = 0, te = 0, tm = 0;
            for (u32 g = 0; g < nGroups; ++g)
                if (s_groups[g].terrain) { ++tg; te += s_groups[g].entryCount; tm += s_groups[g].meshCount; }
            Msg("[nx terrain] groups %u, cull entries %u, meshes %u (of %u groups)", tg, te, tm, nGroups);
        }
    }
#endif
    for (u32 g = 0; g < nGroups; ++g) {
        const Group& grp = s_groups[g];
        WorldMaterial* mat = grp.mat;
        // Terrain→mesh transition (r_fsinv_split): flip query 1 → 2 once.
        if (fsSplit && fsInTerrain && !grp.terrain) {
            VK::VRS::SubStatsEnd(cmd, (u32)fsFrame, 1);
            VK::VRS::SubStatsBegin(cmd, (u32)fsFrame, 2);
            fsInTerrain = false;
        }

        // [nx gpu avg]: the groups sort terrain, then the bindless statics, then the per-material rest -- mark each run.
        if (markStage == 0 && !grp.terrain)                    { NX_GPU_MARK(cmd, "w.stTerrain");  markStage = 1; }
        if (markStage == 1 && !grp.terrain && !grp.bindless)   { NX_GPU_MARK(cmd, "w.stBindless"); markStage = 2; }

        VkPipeline       pipe;
        VkPipelineLayout layout;
        VkDescriptorSet  set;
        const bool terrain = grp.terrain && mat->terrainSet != VK_NULL_HANDLE
                          && PipelineCache::GetTerrainPipeline() != VK_NULL_HANDLE;
        if (terrain) {
            pipe = PipelineCache::GetTerrainPipeline(); layout = PipelineCache::GetTerrainLayout(); set = mat->terrainSet;
        } else {
            PipelineCache::Key k{};
            k.stride = grp.stride; k.tcOffset = grp.tcOffset;
            const bool lmap = (grp.tcOffset == 24);
            // Crossfade (r_cluster_fade): the fade shader variants decode the
            // cull-packed fades and screen-door the LOD transitions. Separate
            // modules → separate cached pipelines; zero cost when fade is off.
            const bool fade = ClusterFade() > 0.f && PipelineCache::WorldLmapFadeVS() != VK_NULL_HANDLE;
            if (fade) {
                k.vs = lmap ? PipelineCache::WorldLmapFadeVS() : PipelineCache::WorldVlitFadeVS();
                k.fs = lmap ? PipelineCache::WorldLmapFadeFS() : PipelineCache::WorldVlitFadeFS();
            } else {
                k.vs = lmap ? PipelineCache::WorldLmapVS() : PipelineCache::WorldVlitVS();
                k.fs = lmap ? PipelineCache::WorldLmapFS() : PipelineCache::WorldVlitFS();
            }
            k.depthTest = true; k.wmark = false; k.tess = false;
            // Uber-FS variant (Inc 1): POM bit per-material; frame bits stamped in Get().
            k.specMask = mat->tessellated ? (u8)PipelineCache::WS_POM : (u8)0;
            // AT groups (r_at_equal): DrawDepth drew them into the prepass with the
            // same aref discard → EQUAL + no write collapses their FS invocations
            // to exactly the visible opaque texels. Opaque groups (r_z_prepass):
            // LEQUAL + no write re-enables early-Z (FS discard + z-write = late-Z).
            k.atEqual  = atEqual && mat->alphaRef >= 0.0f;
            k.noZWrite = prepassZ;
            // r_world_bindless: the group spans materials -- the *_bl twins on the bindless layout, whose set 0 (bound
            // once below) carries every material; no per-group material set.
            k.bindless = grp.bindless;
            pipe = PipelineCache::Get(k);
            layout = grp.bindless ? PipelineCache::GetLayoutBindless() : PipelineCache::GetLayout();
            set    = grp.bindless ? blSet : mat->set;
        }
        if (pipe == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) continue;

        if (layout != lastLayout) {
            lastLayout = layout; lastPipe = VK_NULL_HANDLE; lastMatSet = VK_NULL_HANDLE; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE;
            lastMatPushed = nullptr;
            if (envSet != VK_NULL_HANDLE)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &envSet, 0, nullptr);
            vkCmdPushConstants(cmd, layout, kStages, 0, sizeof(base), &base);
            vkCmdPushConstants(cmd, layout, kStages, kTessOffset, sizeof(tessZero), &tessZero);
        }
        if (pipe != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            lastPipe = pipe; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; lastMatSet = VK_NULL_HANDLE;
        }
        if (set != VK_NULL_HANDLE && set != lastMatSet) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
            lastMatSet = set;
        }
        // GPU-feedback stream id (offset 116) — the statics color pass is where most
        // world diffuse gets sampled, so this is the main feedback producer.
        constexpr u32 kStreamIDOffset = sizeof(Fmatrix) + 13 * sizeof(float);   // 116
        if (!hoist || mat != lastMatPushed) {
            float tail[3] = { mat->alphaRef, mat->detailScale, 1.0f };   // hemi=1.0 (statics = open sky)
            vkCmdPushConstants(cmd, layout, kStages, kTailOffset, sizeof(tail), tail);
            vkCmdPushConstants(cmd, layout, kStages, kStreamIDOffset, sizeof(u32), &mat->streamID);
            lastMatPushed = mat;
        }

        if (grp.vb != lastVB) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &z); lastVB = grp.vb; }
        if (grp.ib != lastIB || grp.iType != lastIType) { vkCmdBindIndexBuffer(cmd, grp.ib, 0, grp.iType); lastIB = grp.ib; lastIType = grp.iType; }
        const VkDeviceSize cmdOff = (VkDeviceSize)grp.entryOffset * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)g * sizeof(u32);
        EmitIndirect(cmd, drawSet, cmdOff, cntOff, grp.entryCount);
    }
    // Close the split queries — both must have begun+ended for the frame's
    // result batch to be readable (all-terrain or empty group list included).
    if (fsSplit) {
        if (fsInTerrain) {
            VK::VRS::SubStatsEnd(cmd, (u32)fsFrame, 1);
            VK::VRS::SubStatsBegin(cmd, (u32)fsFrame, 2);
        }
        VK::VRS::SubStatsEnd(cmd, (u32)fsFrame, 2);
    }
}

// Lazily build one debug pipeline per (vertex stride, fill/line). Mirrors the
// tree crown-hull debug overlay (vk_TreeManager_Render): dynamic rendering on
// SceneColor + swapchain depth, LEQUAL test, no depth write, no descriptors —
// just an mvp push in the VS and the cluster id via gl_InstanceIndex.
VkPipeline GetDebugPipeline(u32 stride, bool line)
{
    // Bit 32 = RR guide attachments: this overlay draws inside the world colour
    // scope, whose attachment count changes with r_dlss_rr.
    const u64 key = (u64)stride * 2 + (line ? 1 : 0) + (VK::RRGuide::Ready() ? (1ull << 32) : 0ull);
    auto it = s_dbgPipes.find(key);
    if (it != s_dbgPipes.end()) return it->second;
    if (s_dbgFailed) return VK_NULL_HANDLE;

    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule vs = g_ShaderManager->Load("world_cluster_debug.vert.spv");
    VkShaderModule fs = g_ShaderManager->Load("world_cluster_debug.frag.spv");
    if (!vs || !fs) { Msg("![VK Cluster] debug shaders missing — r_cluster_debug disabled"); s_dbgFailed = true; return VK_NULL_HANDLE; }

    if (s_dbgLayout == VK_NULL_HANDLE) {
        // Set 0 = meta SSBO (VS reads parentError for the mode-4 health view).
        if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER }, 1,
                                    s_dbgSetL, s_dbgPool, &s_dbgSet,
                                    VK_SHADER_STAGE_VERTEX_BIT, "Cluster.Debug")) {
            s_dbgFailed = true; return VK_NULL_HANDLE;
        }
        VK::DescriptorWriter(s_dbgSet).StorageBuffer(0, s_meta->GetHandle()).Flush();

        // mvp (64) + tint vec4 (16): tint.a 1 = mode 3 path color, 2 = mode 4 health.
        s_dbgLayout = VK::MakePipelineLayout({ s_dbgSetL }, sizeof(Fmatrix) + sizeof(Fvector4),
                                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        if (s_dbgLayout == VK_NULL_HANDLE) { s_dbgFailed = true; return VK_NULL_HANDLE; }
    }

    VK::GfxPipelineBuilder b(s_dbgLayout);
    b.Vert(vs).Frag(fs)
     .Binding(0, stride)
     .Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)   // position only
     .Polygon(line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL)
     .Depth(true, false)
     .Color(VK::SceneColor::Format());
    // Declare-only (mask 0): a wireframe cluster overlay has no material to guide with.
    VK::RRGuide::PipelineTargets(b, false);
    b.DepthTarget(Swapchain.m_DepthFormat);
    // Bound inside the world-color pass (SRI may be attached) — needs the create
    // flag; no dynamic FSR state, so the overlay stays full-rate (static 1x1).
    if (VulkanHW.m_bVRSSupported)
        b.CreateFlags(VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);
    VkPipeline pipe = b.Build("Cluster debug stride=%u line=%d", stride, line ? 1 : 0);
    if (pipe == VK_NULL_HANDLE) { s_dbgFailed = true; return VK_NULL_HANDLE; }
    s_dbgPipes.emplace(key, pipe);
    return pipe;
}

void DrawDebug(VkCommandBuffer cmd, const Fmatrix& viewProj, int mode, DrawSet set)
{
    if (!Built() || mode <= 0) return;
    const bool line   = (mode == 2);
    const bool path   = (mode == 3);   // path view: which LOD system draws each mesh
    const bool health = (mode == 4);   // health view: can this entry ever coarsen?
    if (set != DrawSet::A && !s_occlReady && !s_twoReady) set = DrawSet::A;   // mirrors DrawColor
    const VkShaderStageFlags kDbgStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkBuffer ib16 = ClusterStream::PoolIB16();
    const VkBuffer ib32 = ClusterStream::PoolIB32();

    const u32 nGroups = (u32)s_groups.size();
    VkPipeline  lastPipe = VK_NULL_HANDLE;
    VkBuffer    lastVB = VK_NULL_HANDLE, lastIB = VK_NULL_HANDLE;
    VkIndexType lastIType = VK_INDEX_TYPE_MAX_ENUM;
    bool pushed = false;

    for (u32 g = 0; g < nGroups; ++g) {
        const Group& grp = s_groups[g];
        VkPipeline pipe = GetDebugPipeline(grp.stride, line);
        if (pipe == VK_NULL_HANDLE) return;   // shaders/pipeline unavailable
        if (!pushed) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_dbgLayout, 0, 1, &s_dbgSet, 0, nullptr);
            vkCmdPushConstants(cmd, s_dbgLayout, kDbgStages, 0, sizeof(Fmatrix), &viewProj);
            Fvector4 off; off.set(0.f, 0.f, 0.f, health ? 2.f : 0.f);   // a: 0 = hash colors, 2 = health view
            vkCmdPushConstants(cmd, s_dbgLayout, kDbgStages, sizeof(Fmatrix), sizeof(Fvector4), &off);
            pushed = true;
        }
        if (path) {   // red = plain (no LOD), green = per-mesh DAG, blue = component DAG
            Fvector4 tint;
            if      (grp.ib == ib32) tint.set(0.15f, 0.35f, 1.0f, 1.f);
            else if (grp.ib == ib16) tint.set(0.15f, 1.0f, 0.25f, 1.f);
            else                     tint.set(1.0f, 0.2f, 0.15f, 1.f);
            vkCmdPushConstants(cmd, s_dbgLayout, kDbgStages, sizeof(Fmatrix), sizeof(Fvector4), &tint);
        }
        if (pipe != lastPipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); lastPipe = pipe; lastVB = VK_NULL_HANDLE; lastIB = VK_NULL_HANDLE; }
        if (grp.vb != lastVB) { VkDeviceSize z = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &grp.vb, &z); lastVB = grp.vb; }
        if (grp.ib != lastIB || grp.iType != lastIType) { vkCmdBindIndexBuffer(cmd, grp.ib, 0, grp.iType); lastIB = grp.ib; lastIType = grp.iType; }
        const VkDeviceSize cmdOff = (VkDeviceSize)grp.entryOffset * sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize cntOff = (VkDeviceSize)g * sizeof(u32);
        EmitIndirect(cmd, set, cmdOff, cntOff, grp.entryCount);
    }
}

// ============================================================================
//  Pool compaction (Stage B increment (б)) — see vk_world_gpu.h.
//  Runs once from level_Load, AFTER Build (needs the cluster/repack refs) and
//  BEFORE Trees->Build / ShadowGPU::Build (they snapshot pool handles/offsets
//  from m_mesh — building them after means they see the compacted state).
//  Caller must drain async uploads first (FlushUploadsAndWait).
// ============================================================================
bool PoolsCompacted() { return s_poolsCompacted; }

void CompactPools()
{
    // One-shot: the refs are only valid right after this level's Build.
    xr_vector<CompactRef> refs;
    refs.swap(s_compactRefs);

    if (!ps_r_pool_compact) { Msg("[VK PoolCompact] off (r_pool_compact 0)"); return; }
    if (!Built() || refs.empty()) return;
    // Freed meshes can only be drawn by the cluster shadow targets — without
    // the shadow-cull pipeline the rain/spot/point maps would lose them.
    if (!s_shadowReady) { Msg("![VK PoolCompact] skipped: shadow cluster cull unavailable"); return; }

    VK::Vram::Scope _vram_scope("Geom/Pools");

    // Sub-timers (`[load step]`): 817 ms of a 19.5 s load, split unknown between the
    // 450k-visual walk, the interval bookkeeping and the actual GPU pool rebuild.
    CTimer _c; _c.Start();
    float msPrep = 0, msCollect = 0, msKeep = 0, msMerge = 0, msCopy = 0, msPatch = 0;

    // ---- Pool lookup: CVulkanBuffer* -> pool table slot --------------------
    struct Pool {
        CVulkanBuffer* buf = nullptr;
        bool     isVB = false;
        bool     bail = false;      // u32 pool-global page indices point into this VB — can't move it
        xr_vector<std::pair<u64, u64>> keep;   // byte intervals, merged later
        // After merge: prefix-summed new offsets (newOff[i] for keep[i]).
        xr_vector<u64> newOff;
        u64      keepBytes = 0;
        bool     compact = false;   // this buffer actually gets rebuilt
        VkBuffer oldHandle = VK_NULL_HANDLE;
    };
    xr_vector<Pool> pools;
    xr_map<CVulkanBuffer*, u32> poolOf;
    auto addPools = [&](xr_vector<CVulkanBuffer*>& v, bool isVB) {
        for (CVulkanBuffer* b : v) {
            if (!b || !b->IsValid()) continue;
            if (poolOf.find(b) != poolOf.end()) continue;
            poolOf[b] = (u32)pools.size();
            Pool p; p.buf = b; p.isVB = isVB; p.oldHandle = b->GetHandle();
            pools.push_back(p);
        }
    };
    addPools(RImplementation.nVB, true);
    addPools(RImplementation.nIB, false);
    if (pools.empty()) return;

    // 938 pools x ~300k pool-backed visuals, and the three passes below ask
    // "which pool is this buffer?" six times per visual. As an xr_map that is a
    // red-black tree walk each time; as a sorted array it is the same ten
    // comparisons inside 15 KB that never leaves L2.
    constexpr u32 kNoPool = 0xFFFFFFFFu;
    xr_vector<std::pair<CVulkanBuffer*, u32>> poolIdx;
    poolIdx.reserve(poolOf.size());
    for (const auto& kv : poolOf) poolIdx.emplace_back(kv.first, kv.second);   // xr_map: already ordered
    auto poolFind = [&](CVulkanBuffer* b) -> u32 {
        auto it = std::lower_bound(poolIdx.begin(), poolIdx.end(), b,
            [](const std::pair<CVulkanBuffer*, u32>& e, CVulkanBuffer* k) { return e.first < k; });
        return (it != poolIdx.end() && it->first == b) ? it->second : kNoPool;
    };

    // ---- Freed predicate ----------------------------------------------------
    // A mesh's pool slices can go iff every GPU path draws it from the
    // ClusterStream pools (clustered + vertex-repacked) and no always-CPU path
    // needs it: alpha-tested casters draw on the CPU cutout path in every
    // shadow target, and the terrain slice feeds the TerrainCache probe copy.
    // Same story as poolIdx: one sorted (visual -> ref) array instead of a
    // 50k-node tree that the keep and patch passes each query per visual. The
    // grouping walk emits exactly one ref per mesh, so there are no duplicate
    // keys to disambiguate.
    constexpr u32 kNoRef = 0xFFFFFFFFu;
    xr_vector<std::pair<vkFVisual*, u32>> refIdx;
    refIdx.reserve(refs.size());
    for (u32 i = 0; i < (u32)refs.size(); ++i) refIdx.emplace_back(refs[i].fv, i);
    std::sort(refIdx.begin(), refIdx.end());
    auto refFind = [&](vkFVisual* fv) -> u32 {
        auto it = std::lower_bound(refIdx.begin(), refIdx.end(), fv,
            [](const std::pair<vkFVisual*, u32>& e, vkFVisual* k) { return e.first < k; });
        return (it != refIdx.end() && it->first == fv) ? it->second : kNoRef;
    };

    // u32 pool-global component pages address their pool VB by ABSOLUTE vertex
    // index baked into the page blobs — that VB must not move. (Repacked pages
    // don't reference the pool at all, so only the raw-u32 combination bails.)
    for (const CompactRef& r : refs)
        if (r.clustered && r.ib32 && !r.repacked && r.fv->m_mesh.p_rm_Vertices) {
            const u32 pi = poolFind(r.fv->m_mesh.p_rm_Vertices);
            if (pi != kNoPool) pools[pi].bail = true;
        }

    msPrep = _c.GetElapsed_ms_total();
    _c.Start();

    // ---- Every pool-backed visual (shared walk; this pass only filters) -----
    xr_vector<vkFVisual*> all;
    all.reserve(RImplementation.Visuals.size());
    for (vkFVisual* fv : LeafVisuals()) {
        if (!fv->m_mesh.IsValid()) continue;
        if (poolFind(fv->m_mesh.p_rm_Vertices) == kNoPool
         && poolFind(fv->m_mesh.p_rm_Indices)  == kNoPool) continue;   // owns its buffers
        all.push_back(fv);
    }
    // No sort/unique any more: the shared walk deduped, and `all`'s order is not
    // observable here — the keep intervals it feeds get sorted below, and the
    // patch loop treats each mesh independently.
    //
    // Resolve each visual's ref ONCE, on the idle cores, instead of three times
    // serially (keep: freed-test + attribution, patch: freed-test).
    xr_vector<u32> refFor(all.size(), kNoRef);
    ParallelChunks((int)all.size(), [&](int lo, int hi) {
        for (int k = lo; k < hi; ++k) refFor[k] = refFind(all[k]);
    });
    auto isFreedAt = [&](size_t k) -> bool {
        const u32 ri = refFor[k];
        if (ri == kNoRef) return false;
        const CompactRef& r = refs[ri];
        if (!r.clustered || !r.repacked) return false;
        WorldMaterial* m = all[k]->m_pWorldMaterial;
        if (!m || m->alphaRef >= 0.f || m->isTerrain) return false;
        return true;
    };
    msCollect = _c.GetElapsed_ms_total();
    _c.Start();

    // ---- Keep intervals + why-kept accounting (VB bytes) --------------------
    u64 accFreed = 0, accAT = 0, accTerrain = 0, accRaw = 0, accPlainGpu = 0, accCpu = 0;
    auto ibSize = [](const VK_Render_Mesh& m) -> u64 { return m.iType == VK_INDEX_TYPE_UINT32 ? 4 : 2; };
    // Progressive SWI windows live inside [iBase, iBase+iCount) by construction,
    // but guard the keep range with the actual window extents anyway.
    auto ibEnd = [&](vkFVisual* fv) -> u64 {
        u64 end = (u64)fv->m_mesh.iBase + fv->m_mesh.iCount;
        if (fv->Type == MT_PROGRESSIVE) {
            auto* pg = static_cast<vkFProgressive*>(fv);
            if (pg->sw_offsets && pg->sw_counts)
                for (u32 j = 0; j < pg->sw_count; ++j)
                    end = _max(end, (u64)fv->m_mesh.iBase + pg->sw_offsets[j] + pg->sw_counts[j]);
        }
        return end;
    };
    for (size_t k = 0; k < all.size(); ++k) {
        vkFVisual* fv = all[k];
        const VK_Render_Mesh& m = fv->m_mesh;
        const u64 vbBytes = (u64)m.vCount * m.vStride;
        if (isFreedAt(k)) { accFreed += vbBytes; continue; }
        const u32 pV = poolFind(m.p_rm_Vertices);
        if (pV != kNoPool && m.vCount) {
            Pool& p = pools[pV];
            p.keep.emplace_back((u64)m.vBase * m.vStride, ((u64)m.vBase + m.vCount) * m.vStride);
        }
        const u32 pI = poolFind(m.p_rm_Indices);
        if (pI != kNoPool && m.iCount) {
            Pool& p = pools[pI];
            p.keep.emplace_back((u64)m.iBase * ibSize(m), ibEnd(fv) * ibSize(m));
        }
        // Attribution (diagnostic): why does this mesh keep its VB slice?
        const u32 ri = refFor[k];
        WorldMaterial* mat = fv->m_pWorldMaterial;
        if      (ri == kNoRef)                    accCpu     += vbBytes;   // CPU leaf / tree / non-GPU-set
        else if (mat && mat->isTerrain)           accTerrain += vbBytes;
        else if (mat && mat->alphaRef >= 0.f)     accAT      += vbBytes;
        else if (refs[ri].clustered)              accRaw     += vbBytes;   // clustered but not repacked
        else                                      accPlainGpu+= vbBytes;   // plain identity mesh
    }

    msKeep = _c.GetElapsed_ms_total();
    _c.Start();

    // ---- Merge intervals, decide per buffer, build remap tables -------------
    u64 oldVbTotal = 0, newVbTotal = 0, oldIbTotal = 0, newIbTotal = 0;
    u32 nCompact = 0, nBail = 0, nSkip = 0;
    for (Pool& p : pools) {
        std::sort(p.keep.begin(), p.keep.end());
        xr_vector<std::pair<u64, u64>> merged;
        for (const auto& iv : p.keep) {
            if (iv.second <= iv.first) continue;
            if (!merged.empty() && iv.first <= merged.back().second)
                merged.back().second = _max(merged.back().second, iv.second);
            else
                merged.push_back(iv);
        }
        p.keep.swap(merged);
        p.newOff.resize(p.keep.size());
        u64 off = 0;
        for (size_t i = 0; i < p.keep.size(); ++i) {
            p.newOff[i] = off;
            off += p.keep[i].second - p.keep[i].first;
        }
        p.keepBytes = off;
        const u64 sz = p.buf->GetSize();
        (p.isVB ? oldVbTotal : oldIbTotal) += sz;
        // Not worth the churn if nearly everything stays; bailed VBs stay whole.
        p.compact = !p.bail && p.keepBytes + (sz / 32) < sz;   // > ~3% savings
        if (p.bail) ++nBail;
        else if (!p.compact) ++nSkip;
        else ++nCompact;
        (p.isVB ? newVbTotal : newIbTotal) += p.compact ? p.keepBytes : sz;
    }

    // Byte remap inside a compacted pool (interval binary search). Offsets fed
    // to it always come from keep-interval owners, so a miss is a logic bug.
    auto remap = [](const Pool& p, u64 oldByte) -> u64 {
        size_t lo = 0, hi = p.keep.size();
        while (lo + 1 < hi) {
            const size_t mid = (lo + hi) / 2;
            if (p.keep[mid].first <= oldByte) lo = mid; else hi = mid;
        }
        R_ASSERT(!p.keep.empty() && p.keep[lo].first <= oldByte && oldByte <= p.keep[lo].second);
        return p.newOff[lo] + (oldByte - p.keep[lo].first);
    };

    msMerge = _c.GetElapsed_ms_total();
    _c.Start();

    // ---- Patch visuals (m_mesh offsets; freed ones become empty) ------------
    // Runs on a worker DURING the pool copy below: it rewrites CPU-side offsets
    // (m_mesh, s_metaCPU) and touches no Vulkan object, while the copy is a queue
    // round trip the loader would otherwise sit out. The handle swap and the
    // group/meta patch that depend on it stay on this thread, after the join.
    //
    // new vBase - old vBase per REF (raw-u16 cluster meta patch). A vector
    // indexed by ref, not a map keyed by visual: only refs are ever asked, and
    // "no entry" and "delta 0" mean the same thing to the consumer below.
    xr_vector<s64> refVbDelta(refs.size(), 0);
    float msPatchThread = 0.f;
    auto patchCpu = [&] {
        CTimer _pt; _pt.Start();
        for (size_t k = 0; k < all.size(); ++k) {
            vkFVisual* fv = all[k];
            VK_Render_Mesh& m = fv->m_mesh;
            if (isFreedAt(k)) {
                // Every CPU path gates on IsValid() (vCount>0) / draws iCount — this
                // makes any submit of the mesh a clean no-op. GPU paths draw it from
                // the ClusterStream pools via the meta, which never reads m_mesh.
                m.vCount = 0; m.iCount = 0; m.dwPrimitives = 0;
                continue;
            }
            const u32 pV = poolFind(m.p_rm_Vertices);
            if (pV != kNoPool && pools[pV].compact && m.vCount) {
                const Pool& p = pools[pV];
                const u32 nb = (u32)(remap(p, (u64)m.vBase * m.vStride) / m.vStride);
                if (nb != m.vBase && refFor[k] != kNoRef)
                    refVbDelta[refFor[k]] = (s64)nb - (s64)m.vBase;
                m.vBase = nb;
            }
            const u32 pI = poolFind(m.p_rm_Indices);
            if (pI != kNoPool && pools[pI].compact && m.iCount) {
                const Pool& p = pools[pI];
                m.iBase = (u32)(remap(p, (u64)m.iBase * ibSize(m)) / ibSize(m));
            }
        }
        // Identity metas take their mesh offsets straight from the patched m_mesh,
        // so this half belongs with the loop above, not after the join.
        for (u32 ri = 0; ri < (u32)refs.size(); ++ri) {
            const CompactRef& r = refs[ri];
            if (r.metaFirst + r.metaCount > (u32)s_metaCPU.size()) continue;
            if (!r.clustered) {
                GpuMeshMeta& m = s_metaCPU[r.metaFirst];
                m.first_vertex = r.fv->m_mesh.vBase;
                DrawnSlice(r.fv, m.ib_first, m.index_count);
            } else if (!r.repacked && !r.ib32) {
                const s64 d = refVbDelta[ri];
                if (!d) continue;
                for (u32 e = r.metaFirst; e < r.metaFirst + r.metaCount; ++e)
                    s_metaCPU[e].first_vertex = (u32)((s64)s_metaCPU[e].first_vertex + d);
            }
        }
        msPatchThread = _pt.GetElapsed_ms_total();
    };
    std::thread patchThread;

    // ---- Rebuild the compacted buffers (one submit, waits on completion) ----
    xr_vector<CVulkanBuffer> fresh(pools.size());
    {
        VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
        if (cmd == VK_NULL_HANDLE) { Msg("![VK PoolCompact] no upload cmd — skipped"); return; }
        for (size_t i = 0; i < pools.size(); ++i) {
            Pool& p = pools[i];
            if (!p.compact || p.keepBytes == 0) continue;
            const VkBufferUsageFlags usage = (p.isVB
                ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
                | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                | VK_BUFFER_USAGE_TRANSFER_SRC_BIT   // tree meshlet readback + future rebuilds
                | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;  // RT world gather reads the pool by address
            fresh[i].Create(p.keepBytes, usage, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
            if (!fresh[i].IsValid()) { p.compact = false; ++nSkip; --nCompact; continue; }
            xr_vector<VkBufferCopy> regions(p.keep.size());
            for (size_t k = 0; k < p.keep.size(); ++k)
                regions[k] = { p.keep[k].first, p.newOff[k], p.keep[k].second - p.keep[k].first };
            vkCmdCopyBuffer(cmd, p.buf->GetHandle(), fresh[i].GetHandle(),
                            (u32)regions.size(), regions.data());
        }
        // The mesh/meta patch below is pure CPU (offsets in m_mesh and s_metaCPU,
        // no Vulkan object touched) and does not depend on the copy having run —
        // only the HANDLE swap does. So it rides the queue round trip instead of
        // waiting its turn behind it.
        patchThread = std::thread(patchCpu);
        VulkanHW.EndSingleTimeCommands(cmd);   // waits — old buffers are idle after this
    }

    msCopy = _c.GetElapsed_ms_total();
    _c.Start();

    // The offsets the group patch and the meta upload read must be final by now.
    if (patchThread.joinable()) patchThread.join();

    // ---- Swap in place + raw-handle map -------------------------------------
    xr_map<VkBuffer, VkBuffer> handleMap;
    for (size_t i = 0; i < pools.size(); ++i) {
        Pool& p = pools[i];
        if (!p.compact) continue;
        if (p.keepBytes == 0) { p.buf->Destroy(); continue; }   // fully freed pool: no one binds it
        p.buf->AdoptFrom(fresh[i]);
        handleMap[p.oldHandle] = p.buf->GetHandle();
    }

    // ---- Patch the GPU-set state --------------------------------------------
    // Groups snapshot raw pool handles — this half needs the swap above, so it
    // is the only patch left on this thread.
    for (Group& g : s_groups) {
        auto it = handleMap.find(g.vb); if (it != handleMap.end()) g.vb = it->second;
        it = handleMap.find(g.ib);      if (it != handleMap.end()) g.ib = it->second;
    }
    if (s_meta) s_meta->Upload(s_metaCPU.data(), sizeof(GpuMeshMeta) * s_metaCPU.size());
    ++s_buildStamp;   // derived tables (VSM candidates) refresh from the patched meta

    // Terrain composite cache captured (original pool VB handle, vBase) during
    // grouping — re-point it at the compacted slice (same rule as seed(): the
    // FIRST terrain capture wins, so re-capture from the first terrain mesh).
    for (const CompactRef& r : refs) {
        if (r.metaFirst >= (u32)s_metaCPU.size()) continue;
        const u32 g = s_metaCPU[r.metaFirst].group;
        if (g >= (u32)s_groups.size()) continue;
        const Group& grp = s_groups[g];
        if (!grp.terrain || !grp.mat || grp.mat->terrainSet == VK_NULL_HANDLE) continue;
        if (r.metaFirst != grp.entryOffset) continue;   // first mesh of the group only
        if (!r.fv->m_mesh.p_rm_Vertices) continue;
        TerrainCache::RecaptureMesh(r.fv->m_mesh.p_rm_Vertices->GetHandle(), r.fv->m_mesh.vBase);
        break;   // OnTerrainMesh is first-capture-wins — mirror that
    }

    s_poolsCompacted = true;
    msPatch = _c.GetElapsed_ms_total();
    Msg("[load step]   WorldGPU::CompactPools: prep %.0f | collect %.0f (%u visuals -> %u pool-backed) | keep %.0f | merge %.0f | gpuCopy %.0f (patch %.0f rides along) | swap+meta %.0f ms",
        msPrep, msCollect, (u32)RImplementation.Visuals.size(), (u32)all.size(),
        msKeep, msMerge, msCopy, msPatchThread, msPatch);
    const double MB = 1.0 / (1024.0 * 1024.0);
    Msg("[VK PoolCompact] VB %.1f -> %.1f MB, IB %.1f -> %.1f MB (%u compacted, %u skipped, %u bailed of %u pools)",
        oldVbTotal * MB, newVbTotal * MB, oldIbTotal * MB, newIbTotal * MB,
        nCompact, nSkip, nBail, (u32)pools.size());
    Msg("[VK PoolCompact] VB freed %.1f MB | kept: cpu/trees %.1f, terrain %.1f, alpha-test %.1f, cluster-raw %.1f, plain %.1f MB",
        accFreed * MB, accCpu * MB, accTerrain * MB, accAT * MB, accRaw * MB, accPlainGpu * MB);
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    BlDestroy(); s_bindless = false; s_blMats.clear(); s_blIndex.clear();   // r_world_bindless
    if (s_cullPipe)   { vkDestroyPipeline(VulkanHW.m_Device, s_cullPipe, nullptr); s_cullPipe = VK_NULL_HANDLE; }
    if (s_cullLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_cullLayout, nullptr); s_cullLayout = VK_NULL_HANDLE; }
    if (s_pool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setL)       { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setL, nullptr); s_setL = VK_NULL_HANDLE; }
    for (auto& kv : s_dbgPipes) if (kv.second) vkDestroyPipeline(VulkanHW.m_Device, kv.second, nullptr);
    s_dbgPipes.clear(); s_dbgFailed = false;
    if (s_dbgLayout)   { vkDestroyPipelineLayout(VulkanHW.m_Device, s_dbgLayout, nullptr); s_dbgLayout = VK_NULL_HANDLE; }
    if (s_dbgPool)     { vkDestroyDescriptorPool(VulkanHW.m_Device, s_dbgPool, nullptr); s_dbgPool = VK_NULL_HANDLE; }
    if (s_dbgSetL)     { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_dbgSetL, nullptr); s_dbgSetL = VK_NULL_HANDLE; }
    s_dbgSet = VK_NULL_HANDLE;
    if (s_cullPipe2)   { vkDestroyPipeline(VulkanHW.m_Device, s_cullPipe2, nullptr); s_cullPipe2 = VK_NULL_HANDLE; }
    if (s_cullLayout2) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_cullLayout2, nullptr); s_cullLayout2 = VK_NULL_HANDLE; }
    if (s_pool2)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool2, nullptr); s_pool2 = VK_NULL_HANDLE; }
    if (s_setL2)       { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setL2, nullptr); s_setL2 = VK_NULL_HANDLE; }
    if (s_cullPipeTwo)   { vkDestroyPipeline(VulkanHW.m_Device, s_cullPipeTwo, nullptr); s_cullPipeTwo = VK_NULL_HANDLE; }
    if (s_cullLayoutTwo) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_cullLayoutTwo, nullptr); s_cullLayoutTwo = VK_NULL_HANDLE; }
    if (s_poolTwo)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_poolTwo, nullptr); s_poolTwo = VK_NULL_HANDLE; }
    if (s_setLTwo)       { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLTwo, nullptr); s_setLTwo = VK_NULL_HANDLE; }
    if (s_cullPipe3)   { vkDestroyPipeline(VulkanHW.m_Device, s_cullPipe3, nullptr); s_cullPipe3 = VK_NULL_HANDLE; }
    if (s_cullLayout3) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_cullLayout3, nullptr); s_cullLayout3 = VK_NULL_HANDLE; }
    if (s_pool3)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool3, nullptr); s_pool3 = VK_NULL_HANDLE; }
    if (s_setL3)       { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setL3, nullptr); s_setL3 = VK_NULL_HANDLE; }
    auto del = [](CVulkanBuffer*& b) { if (b) { xr_delete(b); b = nullptr; } };
    del(s_meta); del(s_indirect); del(s_count); del(s_indirect2); del(s_count2);
    del(s_late);
    del(s_indirectSh); del(s_countSh);
    del(s_groupBase);
    ClusterStream::Destroy();   // pools, residency state, IO thread
    s_pageDir.clear(); s_cachePath[0] = 0; s_blobOff16 = s_blobOff32 = s_blobOffVb = 0;
    if (s_countReadback) { VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, s_countReadback, s_countReadbackAlloc);
                           s_countReadback = VK_NULL_HANDLE; s_countReadbackAlloc = VK_NULL_HANDLE; s_countReadbackPtr = nullptr; }
    s_groups.clear(); s_meshSet.clear(); s_cpuMeshes.clear(); s_metaCPU.clear(); s_total = 0; s_nGroups = 0;
    ReleaseLeafVisuals();   // also covers an unload that never reached the loader's release
    s_compactRefs.clear(); s_poolsCompacted = false;
    s_set = VK_NULL_HANDLE; s_set2 = VK_NULL_HANDLE; s_set3 = VK_NULL_HANDLE;
    s_setTwo[0] = s_setTwo[1] = VK_NULL_HANDLE;
    // The next level's first frame has no previous pyramid to trust — a stale
    // s_prevVPFrame would let phase 1 test this world against the last one's depth.
    s_prevVPFrame = 0xFFFFFFFFu;
    for (u32 i = 0; i < 2; ++i) { s_twoHzbView[i] = VK_NULL_HANDLE; s_twoHzbSmp[i] = VK_NULL_HANDLE; s_twoHzbGen[i] = 0xFFFFFFFFu; }
    s_twoReady = false;
    s_occlReady = false; s_shadowReady = false; s_built = false;
}

}} // namespace VK::WorldGPU

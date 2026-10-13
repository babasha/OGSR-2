// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// Variable Rate Shading — depth-driven SRI built by compute. See vk_vrs.h.

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_vrs.h"
#include "HW_Vulkan.h"
#include "vk_image.h"      // VK::CreateImage / CreateImageView
#include "vk_compute_util.h" // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_shaders.h"    // g_ShaderManager (vrs_build.comp.spv)
#include "vk_profiler.h"   // Prof::NameImage
#include "vk_buffer.h"     // CVulkanBuffer (tile-histogram readback)
#include "../../xr_3da/device.h" // RV().dwTimeGlobal (histogram log throttle)

extern int   ps_r_vrs;        // 0 off / 1 mild / 2 aggressive
extern float ps_r_vrs_near;   // metres: below this stays 1x1 (level 1 threshold)
extern float ps_r_vrs_far;    // metres: above this goes coarsest (level 1 threshold)
extern int   ps_r_world_geom_stats; // HW counters on World/Depth + World/Statics (see GeomStats*)
extern int   ps_r_tree_stats;       // its per-pass HW counters use the SAME query type - one at a time

namespace VK { namespace VRS {

namespace {

constexpr u32 N = VK_FRAMES_IN_FLIGHT;

PFN_vkCmdSetFragmentShadingRateKHR pfnSetRate = nullptr;
bool s_inited = false;
bool s_dead   = false;   // unsupported / init failed → permanent no-op

// compute pipeline
VkPipeline            s_pipe   = VK_NULL_HANDLE;
VkPipelineLayout      s_layout = VK_NULL_HANDLE;
VkDescriptorSetLayout s_setL   = VK_NULL_HANDLE;
VkDescriptorPool      s_pool   = VK_NULL_HANDLE;
VkDescriptorSet       s_set[N] = {};
VkSampler             s_depthSampler = VK_NULL_HANDLE;

// per-frame shading-rate images (written by compute, read as FSR attachment)
VkImage       s_image[N] = {};
VmaAllocation s_alloc[N] = {};
VkImageView   s_view[N]  = {};
VkExtent2D    s_screen   = { 0, 0 };
VkExtent2D    s_tiles    = { 0, 0 };
int           s_cur      = -1;

// Diagnostics: per-frame tile histogram (1x1/2x2/4x4) the compute fills via
// atomics. Host-visible; the frame fence makes slot `cur` (written N frames
// ago) safe to read+zero on the CPU right before re-recording it.
CVulkanBuffer s_hist[N];
u32*          s_histPtr[N] = {};

// Diagnostics: fragment-shader-invocation counter around the world color pass
// (pipeline statistics query). THE ground truth for "is VRS shading coarser":
// force 4x4 must cut this ~16x, SRI mode by roughly the coarse-tile fraction.
VkQueryPool s_statsPool = VK_NULL_HANDLE;
bool        s_statsUsed[N] = {};   // slot has an unread result
bool        s_statsOpen    = false;

struct Push {
    float   invScreen[2];
    float   nearFar[2];
    float   proj[2];     // A (p43), B (p33)
    int32_t tiles[2];
    int32_t texel[2];
    int32_t level;
};

void FreeImages()
{
    for (u32 i = 0; i < N; ++i) {
        if (s_view[i])  { vkDestroyImageView(VulkanHW.m_Device, s_view[i], nullptr);  s_view[i]  = VK_NULL_HANDLE; }
        if (s_image[i]) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_image[i], s_alloc[i]); s_image[i] = VK_NULL_HANDLE; s_alloc[i] = VK_NULL_HANDLE; }
    }
    s_screen = { 0, 0 }; s_tiles = { 0, 0 }; s_cur = -1;
}

bool CreatePipeline()
{
    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule cs = g_ShaderManager->Load("vrs_build.comp.spv");
    if (!cs) { Msg("![VK VRS] vrs_build.comp.spv load failed"); return false; }

    // 0 = depth (in), 1 = VRS image (out), 2 = stats.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
                                N, s_setL, s_pool, s_set, VK_SHADER_STAGE_COMPUTE_BIT, "VRS.Build"))
        return false;

    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_depthSampler) != VK_SUCCESS) return false;

    for (u32 i = 0; i < N; ++i) {
        s_hist[i].Create(3 * sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, false /*host-visible*/);
        s_histPtr[i] = (u32*)s_hist[i].Map();
        if (s_histPtr[i]) { s_histPtr[i][0] = s_histPtr[i][1] = s_histPtr[i][2] = 0; }
    }

    s_layout = VK::MakePipelineLayout({ s_setL }, sizeof(Push));
    if (s_layout == VK_NULL_HANDLE) return false;

    s_pipe = VK::CreateComputePipeline(cs, s_layout, "VRS.Build");
    if (s_pipe == VK_NULL_HANDLE) return false;
    return true;
}

void EnsureSize(VkExtent2D screen)
{
    if (s_image[0] && screen.width == s_screen.width && screen.height == s_screen.height) return;
    FreeImages();

    const VkExtent2D tex = VulkanHW.m_VRSTexelSize;
    const u32 tw = (screen.width  + tex.width  - 1) / tex.width;
    const u32 th = (screen.height + tex.height - 1) / tex.height;
    if (tw == 0 || th == 0) return;

    for (u32 i = 0; i < N; ++i) {
        VK::ImageDesc d;
        d.format   = VK_FORMAT_R8_UINT;
        d.extent   = { tw, th, 1 };
        d.usage    = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
        d.memUsage = VMA_MEMORY_USAGE_AUTO;
        d.name     = "VRS.ShadingRateImage";
        if (!VK::CreateImage(d, s_image[i], s_alloc[i])) { FreeImages(); return; }
        s_view[i] = VK::CreateImageView(s_image[i], VK_FORMAT_R8_UINT);
        if (s_view[i] == VK_NULL_HANDLE) { FreeImages(); return; }
    }
    s_screen = screen; s_tiles = { tw, th };
    Msg("[VK VRS] depth-driven SRI %ux%u tiles x%u (tile %ux%u)", tw, th, N, tex.width, tex.height);
}

} // anonymous namespace

void Init()
{
    if (s_inited) return;
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    s_inited = true;

    if (!VulkanHW.m_bVRSSupported) { s_dead = true; return; }

    pfnSetRate = (PFN_vkCmdSetFragmentShadingRateKHR)
                 vkGetDeviceProcAddr(VulkanHW.m_Device, "vkCmdSetFragmentShadingRateKHR");

    // R8_UINT must be storage-image capable (compute writes the SRI directly).
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(VulkanHW.m_PhysicalDevice, VK_FORMAT_R8_UINT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        Msg("![VK VRS] R8_UINT storage images unsupported — VRS disabled"); s_dead = true; return;
    }
    if (!pfnSetRate || !CreatePipeline()) {
        Msg("![VK VRS] compute init failed — VRS disabled"); s_dead = true; return;
    }
    Msg("[VK VRS] depth-driven init OK");
}

bool Wanted() { return VulkanHW.m_bVRSSupported && ps_r_vrs > 0; }

void BuildFromDepth(VkCommandBuffer cmd, u32 frameIndex, VkImageView depthView, VkExtent2D screen, float A, float B)
{
    Init();
    if (s_dead || s_pipe == VK_NULL_HANDLE) return;
    EnsureSize(screen);
    if (!s_image[0]) return;

    const u32 cur = frameIndex % N;
    s_cur = (int)cur;

    // Histogram readback: slot `cur` was written N frames ago and its fence has
    // been waited — read what the compute counted, log throttled, zero for reuse.
    if (s_histPtr[cur]) {
        const u32 c1 = s_histPtr[cur][0], c2 = s_histPtr[cur][1], c4 = s_histPtr[cur][2];
        static u32 s_lastLog = 0;
        if (c1 + c2 + c4 && RV().dwTimeGlobal > s_lastLog + 3000) {
            s_lastLog = RV().dwTimeGlobal;
            const u32 total = c1 + c2 + c4;
            Msg("[VK VRS] tiles 1x1=%u 2x2=%u 4x4=%u (%u%% coarse of %u)",
                c1, c2, c4, (c2 + c4) * 100u / total, total);
        }
        s_histPtr[cur][0] = s_histPtr[cur][1] = s_histPtr[cur][2] = 0;
    }

    // descriptor: depth (read) + this slot's SRI (write) + histogram SSBO
    VK::DescriptorWriter(s_set[cur])
        .ImageSampler (0, depthView, s_depthSampler)
        .StorageImage (1, s_view[cur])
        .StorageBuffer(2, s_hist[cur].GetHandle())
        .Flush();

    auto imgBarrier = [&](VkImageLayout oldL, VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA,
                          VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier ib{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        ib.oldLayout = oldL; ib.newLayout = newL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = s_image[cur]; ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        ib.srcAccessMask = srcA; ib.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &ib);
    };

    // UNDEFINED (discard prev) → GENERAL for the compute store.
    imgBarrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
               0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_layout, 0, 1, &s_set[cur], 0, nullptr);

    const VkExtent2D tex = VulkanHW.m_VRSTexelSize;
    const int level = ps_r_vrs;
    Push pc{};
    pc.invScreen[0] = 1.0f / float(screen.width);
    pc.invScreen[1] = 1.0f / float(screen.height);
    // distance thresholds (metres, live via r_vrs_near/r_vrs_far). Level 1 uses
    // them as-is; level 2 (aggressive) pulls them ~closer for more coarse area.
    const float kn = ps_r_vrs_near, kf = ps_r_vrs_far;
    pc.nearFar[0] = (level >= 2) ? _max(2.0f,            kn - 10.0f) : kn;
    pc.nearFar[1] = (level >= 2) ? _max(pc.nearFar[0] + 5.0f, kf - 25.0f) : kf;
    pc.proj[0] = A; pc.proj[1] = B;
    pc.tiles[0] = (int)s_tiles.width; pc.tiles[1] = (int)s_tiles.height;
    pc.texel[0] = (int)tex.width;     pc.texel[1] = (int)tex.height;
    pc.level = level;
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &pc);

    vkCmdDispatch(cmd, (s_tiles.width + 7) / 8, (s_tiles.height + 7) / 8, 1);

    // GENERAL (compute write) → FSR attachment (read by the world color pass).
    imgBarrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);
}

VkImageView GetView()   { return (s_cur >= 0 && s_image[s_cur]) ? s_view[s_cur] : VK_NULL_HANDLE; }
VkExtent2D  TexelSize() { return VulkanHW.m_VRSTexelSize; }

void StatsBegin(VkCommandBuffer cmd, u32 frameIndex)
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_statsPool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qci.queryType  = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        qci.queryCount = N;
        qci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (vkCreateQueryPool(VulkanHW.m_Device, &qci, nullptr, &s_statsPool) != VK_SUCCESS) {
            s_statsPool = VK_NULL_HANDLE; return;
        }
    }
    const u32 slot = frameIndex % N;
    // Slot was recorded N frames ago; its fence has been waited — harvest.
    if (s_statsUsed[slot]) {
        u64 inv = 0;
        if (vkGetQueryPoolResults(VulkanHW.m_Device, s_statsPool, slot, 1, sizeof(inv), &inv,
                                  sizeof(inv), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            static u32 s_lastLog = 0;
            if (RV().dwTimeGlobal > s_lastLog + 3000) {
                s_lastLog = RV().dwTimeGlobal;
                Msg("[VK VRS] world-color FS invocations = %.2fM", double(inv) / 1e6);
            }
        }
        s_statsUsed[slot] = false;
    }
    vkCmdResetQueryPool(cmd, s_statsPool, slot, 1);
    vkCmdBeginQuery(cmd, s_statsPool, slot, 0);
    s_statsUsed[slot] = true;
    s_statsOpen = true;
}

void StatsEnd(VkCommandBuffer cmd, u32 frameIndex)
{
    if (!s_statsOpen) return;
    vkCmdEndQuery(cmd, s_statsPool, frameIndex % N);
    s_statsOpen = false;
}

// ---- r_fsinv_split: FS-invocation attribution (see vk_vrs.h) ----------------
// 0=CPU statics flush, 1=GPU terrain groups, 2=GPU mesh groups, 3=dynamics,
// 4=skinned. Plus an OCCLUSION query (SAMPLES_PASSED, precise) around the GPU
// statics draw: samples exclude helper lanes and discarded/failed fragments, so
// invocations >> samples ⇒ quad-helper inflation / late-Z waste, while
// samples ≈ invocations ⇒ genuinely multi-shaded pixels (duplicate draws).
static constexpr u32 kSub = 5;
VkQueryPool s_subPool    = VK_NULL_HANDLE;
VkQueryPool s_occPool    = VK_NULL_HANDLE;
bool        s_subUsed[N] = {};
int         s_subOpen    = -1;
bool        s_occOpen    = false;

void SubStatsReset(VkCommandBuffer cmd, u32 frameIndex)
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_subPool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qci.queryType          = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        qci.queryCount         = N * kSub;
        qci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (vkCreateQueryPool(VulkanHW.m_Device, &qci, nullptr, &s_subPool) != VK_SUCCESS) {
            s_subPool = VK_NULL_HANDLE; return;
        }
        VkQueryPoolCreateInfo oci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        oci.queryType  = VK_QUERY_TYPE_OCCLUSION;
        oci.queryCount = N;
        if (vkCreateQueryPool(VulkanHW.m_Device, &oci, nullptr, &s_occPool) != VK_SUCCESS)
            s_occPool = VK_NULL_HANDLE;   // split still works without samples
    }
    const u32 slot = frameIndex % N;
    // Harvest the slot recorded N frames ago (its fence has been waited).
    if (s_subUsed[slot]) {
        u64 inv[kSub] = {};
        if (vkGetQueryPoolResults(VulkanHW.m_Device, s_subPool, slot * kSub, kSub, sizeof(inv), inv,
                                  sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            u64 smp = 0;
            if (s_occPool != VK_NULL_HANDLE)
                vkGetQueryPoolResults(VulkanHW.m_Device, s_occPool, slot, 1, sizeof(smp), &smp,
                                      sizeof(smp), VK_QUERY_RESULT_64_BIT);
            static u32 s_lastLog = 0;
            if (RV().dwTimeGlobal > s_lastLog + 3000) {
                s_lastLog = RV().dwTimeGlobal;
                Msg("[VK FSinv] cpuFlush=%.2fM gpuTerrain=%.2fM gpuMesh=%.2fM dyn=%.2fM skin=%.2fM | total=%.2fM | gpuSamplesPassed=%.2fM",
                    double(inv[0]) / 1e6, double(inv[1]) / 1e6, double(inv[2]) / 1e6, double(inv[3]) / 1e6,
                    double(inv[4]) / 1e6,
                    double(inv[0] + inv[1] + inv[2] + inv[3] + inv[4]) / 1e6, double(smp) / 1e6);
            }
        }
        s_subUsed[slot] = false;
    }
    vkCmdResetQueryPool(cmd, s_subPool, slot * kSub, kSub);   // outside the render pass
    if (s_occPool != VK_NULL_HANDLE) vkCmdResetQueryPool(cmd, s_occPool, slot, 1);
    s_subOpen = -1;
    s_occOpen = false;
}

// ---- r_world_geom_stats: hardware counters on World/Depth and World/Statics --
// See the header note. Seven statistics in BIT ORDER (that is the order Vulkan
// writes them, not the order they are OR-ed): IA vertices, IA primitives, VS
// invocations, clipping primitives, FS invocations, tess-control patches,
// tess-eval invocations.
// r_world_geom_stats 1: slots 0-1 bracket the two ZONES.
// r_world_geom_stats 2: slots 2-4 split the DEPTH PREPASS by who draws - statics,
//   trees, skinned. The modes are exclusive because Vulkan allows ONE active
//   pipeline-statistics query per command buffer, so a sub-bracket cannot nest
//   inside slot 0.
// Built 26-08 because "who owns the 29.6M fragment invocations in a DEPTH-ONLY
// pass" could not be answered by inference: two-phase removed 26% of the draws and
// 17% of the triangles and moved neither the fragments nor the time, and
// r_tree_dist cannot test the tree half at all (floored at 250 m in TreeFlodCutDist).
static constexpr u32 kGeomSlots = 5;    // 0 = World/Depth, 1 = World/Statics, 2 = Depth/statics, 3 = Depth/trees, 4 = Depth/skinned
static constexpr u32 kGeomVals  = 7;
VkQueryPool s_geomPool    = VK_NULL_HANDLE;
bool        s_geomUsed[N] = {};
int         s_geomOpen    = -1;

void GeomStatsReset(VkCommandBuffer cmd, u32 frameIndex)
{
    if (!ps_r_world_geom_stats || VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_geomPool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qci.queryType  = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        qci.queryCount = N * kGeomSlots;
        qci.pipelineStatistics =
              VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT
            | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT
            | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
            | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT
            | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT
            | VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT
            | VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT;
        if (vkCreateQueryPool(VulkanHW.m_Device, &qci, nullptr, &s_geomPool) != VK_SUCCESS) {
            s_geomPool = VK_NULL_HANDLE; return;
        }
    }
    const u32 slot = frameIndex % N;
    if (s_geomUsed[slot]) {
        // ONE SLOT AT A TIME, deliberately. A batched read of all kGeomSlots fails
        // as a whole (VK_NOT_READY) the moment ONE query in the range was reset but
        // never begun — and that is the normal case here, because the two modes use
        // disjoint slots. Batching it made the instrument print NOTHING at all,
        // which reads exactly like "the counters are zero".
        u64 v[kGeomSlots * kGeomVals] = {};
        bool anyGeom = false;
        for (u32 s = 0; s < kGeomSlots; ++s)
            if (vkGetQueryPoolResults(VulkanHW.m_Device, s_geomPool, slot * kGeomSlots + s, 1,
                                      sizeof(u64) * kGeomVals, v + s * kGeomVals,
                                      sizeof(u64) * kGeomVals, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                anyGeom = true;
        if (anyGeom) {

            // dwTimeContinual, NOT dwTimeGlobal: a sweep freezes the game clock
            // (time_factor 0.001) and a throttle on dwTimeGlobal then prints ONCE
            // and goes silent for the rest of the run. That trap already cost this
            // session one misread of the group-occupancy diag.
            static u32 s_lastGeom = 0;
            if (RV().dwTimeContinual > s_lastGeom + 3000) {
                s_lastGeom = RV().dwTimeContinual;
                static const char* kName[kGeomSlots] = { "World/Depth  ", "World/Statics", "Depth/statics", "Depth/trees  ", "Depth/skinned" };
                for (u32 s = 0; s < kGeomSlots; ++s) {
                    const u64* q = v + s * kGeomVals;
                    if (!q[0] && !q[4]) continue;   // slot belongs to the other mode
                    Msg("[VK Geom] %s IAvert=%.2fM IAprim=%.2fM VSinv=%.2fM clipPrim=%.2fM FSinv=%.2fM | tessPatch=%.2fM tessEval=%.2fM",
                        kName[s], double(q[0]) / 1e6, double(q[1]) / 1e6, double(q[2]) / 1e6,
                        double(q[3]) / 1e6, double(q[4]) / 1e6, double(q[5]) / 1e6, double(q[6]) / 1e6);
                }
            }
        }
        s_geomUsed[slot] = false;
    }
    vkCmdResetQueryPool(cmd, s_geomPool, slot * kGeomSlots, kGeomSlots);
    s_geomOpen = -1;
}

void GeomStatsBegin(VkCommandBuffer cmd, u32 frameIndex, u32 idx)
{
    // Vulkan allows ONE active pipeline-statistics query per command buffer, and the
    // tree ledger brackets its own passes inside World/Depth. Yield to it rather than
    // produce an invalid command buffer; r_tree_stats and r_world_geom_stats are
    // deliberately mutually exclusive (so is r_fsinv_split, gated at the call site).
    if (ps_r_tree_stats) return;
    if (!ps_r_world_geom_stats || s_geomPool == VK_NULL_HANDLE || idx >= kGeomSlots) return;
    if (s_geomOpen >= 0) return;   // one active pipeline-stats query at a time
    vkCmdBeginQuery(cmd, s_geomPool, (frameIndex % N) * kGeomSlots + idx, 0);
    s_geomOpen = (int)idx;
}

void GeomStatsEnd(VkCommandBuffer cmd, u32 frameIndex, u32 idx)
{
    if (!ps_r_world_geom_stats || s_geomPool == VK_NULL_HANDLE || idx >= kGeomSlots) return;
    if (s_geomOpen != (int)idx) return;
    vkCmdEndQuery(cmd, s_geomPool, (frameIndex % N) * kGeomSlots + idx);
    s_geomOpen = -1;
    s_geomUsed[frameIndex % N] = true;
}


void SubOccBegin(VkCommandBuffer cmd, u32 frameIndex)
{
    if (s_occPool == VK_NULL_HANDLE || s_occOpen) return;
    // ⚠ PRECISE is only legal when the device enabled occlusionQueryPrecise. This call
    // passed it unconditionally since it was written and was out of spec the whole time
    // (VUID-vkCmdBeginQuery-queryType-00800, surfaced 25-08) — a driver was free to
    // answer "non-zero" instead of a true sample count, which is exactly the number
    // this split is read for. The feature is now requested in HW_Vulkan.cpp.
    vkCmdBeginQuery(cmd, s_occPool, frameIndex % N,
                    VulkanHW.m_bOcclusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    s_occOpen = true;
}

void SubOccEnd(VkCommandBuffer cmd, u32 frameIndex)
{
    if (s_occPool == VK_NULL_HANDLE || !s_occOpen) return;
    vkCmdEndQuery(cmd, s_occPool, frameIndex % N);
    s_occOpen = false;
}

void SubStatsBegin(VkCommandBuffer cmd, u32 frameIndex, u32 idx)
{
    if (s_subPool == VK_NULL_HANDLE || idx >= kSub || s_subOpen >= 0) return;
    vkCmdBeginQuery(cmd, s_subPool, (frameIndex % N) * kSub + idx, 0);
    s_subOpen = (int)idx;
}

void SubStatsEnd(VkCommandBuffer cmd, u32 frameIndex, u32 idx)
{
    if (s_subPool == VK_NULL_HANDLE || s_subOpen != (int)idx) return;
    vkCmdEndQuery(cmd, s_subPool, (frameIndex % N) * kSub + idx);
    s_subOpen = -1;
    if (idx == kSub - 1) s_subUsed[frameIndex % N] = true;
}

void CmdSetRate(VkCommandBuffer cmd)
{
    // The world-color pipelines carry a FRAGMENT_SHADING_RATE dynamic state whenever
    // the HW supports VRS (m_bVRSSupported), independent of whether the VRS module
    // was inited (r_vrs). So a rate MUST be settable here even when VRS is off — else
    // those draws hit VUID-09238 ("dynamic state never set"). Lazily resolve the fn.
    if (!pfnSetRate) {
        if (!VulkanHW.m_bVRSSupported || VulkanHW.m_Device == VK_NULL_HANDLE) return;
        pfnSetRate = (PFN_vkCmdSetFragmentShadingRateKHR)
                     vkGetDeviceProcAddr(VulkanHW.m_Device, "vkCmdSetFragmentShadingRateKHR");
        if (!pfnSetRate) return;
    }
    const VkExtent2D rate = { 1, 1 };
    const VkFragmentShadingRateCombinerOpKHR ops[2] = {
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR,
    };
    pfnSetRate(cmd, &rate, ops);
    static bool s_logged = false;
    if (!s_logged) { s_logged = true; Msg("[VK VRS] attachment-rate combiner set (pfn=%p)", (void*)pfnSetRate); }
}

void CmdSetPipelineRate(VkCommandBuffer cmd, u32 w, u32 h)
{
    if (!VulkanHW.m_bVRSPipelineSupported) return;
    if (!pfnSetRate) {   // lazy-resolve like CmdSetRate — this path must work with r_vrs 0 (never Init'ed)
        if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
        pfnSetRate = (PFN_vkCmdSetFragmentShadingRateKHR)
                     vkGetDeviceProcAddr(VulkanHW.m_Device, "vkCmdSetFragmentShadingRateKHR");
        if (!pfnSetRate) return;
    }
    const VkExtent2D rate = { w, h };
    const VkFragmentShadingRateCombinerOpKHR ops[2] = {
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,   // ignore primitive rate
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,   // ignore attachment/SRI — use the pipeline rate
    };
    pfnSetRate(cmd, &rate, ops);
    static u32 s_seenMask = 0;   // one log per distinct rate (cones 2x2 + force NxN alternate per frame)
    const u32 lw = (w >= 4u) ? 2u : (w >= 2u) ? 1u : 0u, lh = (h >= 4u) ? 2u : (h >= 2u) ? 1u : 0u;
    const u32 bit = 1u << (lw * 3u + lh);
    if (!(s_seenMask & bit)) {
        s_seenMask |= bit;
        // Whose function are we actually calling? (SL interposer vs the NV driver.)
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)pfnSetRate, &hm) && hm)
            GetModuleFileNameA(hm, mod, sizeof(mod));
        Msg("[VK VRS] pipeline-rate %ux%u set (pfn=%p in %s)", w, h, (void*)pfnSetRate, mod);
    }
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    FreeImages();
    for (u32 i = 0; i < N; ++i) {
        if (s_histPtr[i]) { s_hist[i].Unmap(); s_histPtr[i] = nullptr; }
        s_hist[i].Destroy();
    }
    if (s_statsPool) { vkDestroyQueryPool(VulkanHW.m_Device, s_statsPool, nullptr); s_statsPool = VK_NULL_HANDLE; }
    for (u32 i = 0; i < N; ++i) s_statsUsed[i] = false;
    s_statsOpen = false;
    if (s_subPool) { vkDestroyQueryPool(VulkanHW.m_Device, s_subPool, nullptr); s_subPool = VK_NULL_HANDLE; }
    if (s_occPool) { vkDestroyQueryPool(VulkanHW.m_Device, s_occPool, nullptr); s_occPool = VK_NULL_HANDLE; }
    if (s_geomPool) { vkDestroyQueryPool(VulkanHW.m_Device, s_geomPool, nullptr); s_geomPool = VK_NULL_HANDLE; }
    for (u32 i = 0; i < N; ++i) s_geomUsed[i] = false;
    s_geomOpen = -1;
    for (u32 i = 0; i < N; ++i) s_subUsed[i] = false;
    s_subOpen = -1;
    s_occOpen = false;
    if (s_pipe)          { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_layout)        { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_pool)          { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setL)          { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setL, nullptr); s_setL = VK_NULL_HANDLE; }
    if (s_depthSampler)  { vkDestroySampler(VulkanHW.m_Device, s_depthSampler, nullptr); s_depthSampler = VK_NULL_HANDLE; }
    for (u32 i = 0; i < N; ++i) s_set[i] = VK_NULL_HANDLE;
    s_inited = false; s_dead = false; pfnSetRate = nullptr;
}

}} // namespace VK::VRS

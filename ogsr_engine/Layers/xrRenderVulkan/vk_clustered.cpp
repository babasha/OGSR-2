// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// Clustered forward light culling — see vk_clustered.h. The compute dispatch +
// single-shared-buffer + WAR-barrier discipline mirrors vk_world_gpu.

#include "stdafx.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_clustered.h"
#include "vk_buffer.h"           // CVulkanBuffer
#include "vk_command_buffer.h"   // CVulkanCommandManager::FRAMES_IN_FLIGHT
#include "vk_shaders.h"          // g_ShaderManager (SPIR-V loader)
#include "vk_compute_util.h"     // VK::MakePipelineLayout / CreateComputePipeline
#include <cmath>

extern int ps_r_clustered_debug;   // gate the per-frame cull diag behind the debug cvar

namespace VK { namespace Clustered {

namespace {
    constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
    constexpr VkDeviceSize kLightStride =
        (sizeof(Lights::GpuLight) * Lights::kMaxClusterLights + 255) & ~VkDeviceSize(255);

    bool s_inited = false, s_failed = false, s_ready = false;

    CVulkanBuffer s_lights;     // host-visible, kFramesInFlight regions (Light[256])
    CVulkanBuffer s_grid;       // device-local, uint[kClusters]
    CVulkanBuffer s_indices;    // device-local, uint[kClusters * kMaxPerCluster]
    u8*           s_lightsMapped = nullptr;

    VkDescriptorSetLayout s_setL = VK_NULL_HANDLE;
    VkDescriptorPool      s_pool = VK_NULL_HANDLE;
    VkDescriptorSet       s_set[kFramesInFlight] = {};
    VkPipelineLayout      s_layout = VK_NULL_HANDLE;
    VkPipeline            s_pipe   = VK_NULL_HANDLE;

    // Must match light_cluster.comp.glsl `Push` (7 vec4 = 112 B).
    struct CullPush {
        float eye[4];     // xyz camera world pos
        float fwd[4];     // xyz camera forward (unit)
        float right[4];   // xyz camera right (unit)
        float up[4];      // xyz camera up/top (unit)
        float grid[4];    // GX, GY, GZ, kMaxPerCluster
        float proj[4];    // tanX, tanY, near, far
        float misc[4];    // numLights, logFarNear, 0, 0
    };

    void MemBarrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds)
    {
        VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        b.srcAccessMask = src; b.dstAccessMask = dst;
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
}

GridZ DeriveGridZ(const ProjTerms& pt)
{
    // zview = p43 / (zndc - p33): near at zndc=0, far at zndc=1 (D3D [0,1] depth).
    float nearZ = pt.p43 / (0.0f - pt.p33);   // = -p43/p33
    float farZ  = pt.p43 / (1.0f - pt.p33);
    clamp(nearZ, 0.05f, 10.0f);
    clamp(farZ,  nearZ * 4.0f, 5000.0f);

    GridZ g{};
    g.nearZ      = nearZ;
    g.farZ       = farZ;
    g.logFarNear = std::log2(farZ / nearZ);
    g.sliceScale = float(kGridZ) / g.logFarNear;
    g.sliceBias  = -(float(kGridZ) * std::log2(nearZ) / g.logFarNear);
    return g;
}

bool Init()
{
    if (s_inited) return s_ready;
    s_inited = true;

    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule cs = g_ShaderManager->Load("light_cluster.comp.spv");
    if (!cs) { Msg("![VK Clustered] light_cluster.comp.spv load failed — disabled"); s_failed = true; return false; }

    // Host-visible light list (double-buffered: CPU writes off the GPU timeline).
    s_lights.Create(kLightStride * kFramesInFlight,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    s_lightsMapped = static_cast<u8*>(s_lights.Map());
    if (!s_lightsMapped) { Msg("![VK Clustered] light buffer map failed"); s_failed = true; return false; }

    // Device-local grid + index list (written by compute, read by the fragments).
    s_grid.Create((VkDeviceSize)kClusters * sizeof(u32),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    s_indices.Create((VkDeviceSize)kClusters * kMaxPerCluster * sizeof(u32),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);

    // Compute set layout: 0 = lights (in), 1 = grid (out), 2 = indices (out).
    constexpr auto kSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!VK::MakeDescriptorSets({ kSSBO, kSSBO, kSSBO }, kFramesInFlight, s_setL, s_pool, s_set,
                                VK_SHADER_STAGE_COMPUTE_BIT, "Clustered.Cull")) {
        s_failed = true; return false;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        VK::DescriptorWriter(s_set[i])
            .StorageBuffer(0, s_lights.GetHandle(),
                              sizeof(Lights::GpuLight) * Lights::kMaxClusterLights, kLightStride * i)
            .StorageBuffer(1, s_grid.GetHandle())
            .StorageBuffer(2, s_indices.GetHandle())
            .Flush();
    }

    s_layout = VK::MakePipelineLayout({ s_setL }, sizeof(CullPush));
    if (s_layout == VK_NULL_HANDLE) { s_failed = true; return false; }

    s_pipe = VK::CreateComputePipeline(cs, s_layout, "Clustered.Cull");
    if (s_pipe == VK_NULL_HANDLE) { s_failed = true; return false; }

    s_ready = true;
    Msg("[VK Clustered] init OK — grid %ux%ux%u (%u clusters), %u lights/cluster, %u max lights",
        kGridX, kGridY, kGridZ, kClusters, kMaxPerCluster, Lights::kMaxClusterLights);
    return true;
}

bool Ready() { return s_ready && !s_failed; }

void UploadLights(const Lights::FrameLights& fl, u32 slot)
{
    if (!s_inited) Init();
    if (!s_ready || !s_lightsMapped) return;
    if (slot >= kFramesInFlight) slot = 0;
    const u32 n = (fl.count < Lights::kMaxClusterLights) ? fl.count : Lights::kMaxClusterLights;
    if (n) memcpy(s_lightsMapped + size_t(slot) * kLightStride, fl.gpu, sizeof(Lights::GpuLight) * n);
}

void Cull(VkCommandBuffer cmd, const ProjTerms& pt, const Fvector& eye,
          VkExtent2D extent, u32 slot, u32 numLights)
{
    if (!s_ready) return;
    if (slot >= kFramesInFlight) slot = 0;
    (void)extent;   // grid is resolution-independent; the fragment maps gl_FragCoord → tile
    const GridZ gz = DeriveGridZ(pt);

    // WAR guard: this frame's compute writes wait for the PREVIOUS frame's
    // fragment reads of the shared grid/index buffers (single queue, ordered).
    MemBarrier(cmd, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_layout, 0, 1, &s_set[slot], 0, nullptr);

    CullPush pc{};
    pc.eye[0] = eye.x; pc.eye[1] = eye.y; pc.eye[2] = eye.z;
    pc.fwd[0] = pt.dir.x;   pc.fwd[1] = pt.dir.y;   pc.fwd[2] = pt.dir.z;
    pc.right[0] = pt.right.x; pc.right[1] = pt.right.y; pc.right[2] = pt.right.z;
    pc.up[0] = pt.top.x;    pc.up[1] = pt.top.y;    pc.up[2] = pt.top.z;
    pc.grid[0] = float(kGridX); pc.grid[1] = float(kGridY); pc.grid[2] = float(kGridZ); pc.grid[3] = float(kMaxPerCluster);
    pc.proj[0] = pt.tanX; pc.proj[1] = pt.tanY; pc.proj[2] = gz.nearZ; pc.proj[3] = gz.farZ;
    pc.misc[0] = float(numLights < Lights::kMaxClusterLights ? numLights : Lights::kMaxClusterLights);
    pc.misc[1] = gz.logFarNear;
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    vkCmdDispatch(cmd, (kClusters + 63) / 64, 1, 1);

    // Compute writes → fragment reads (color pass) of grid/indices.
    MemBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // Throttled confirmation the cull runs — only with r_clustered_debug (keeps the
    // log clean in normal use). First dispatch + every ~3 s @ 60 fps.
    if (ps_r_clustered_debug) {
        static u32 s_diagCd = 0;
        if (s_diagCd == 0) {
            s_diagCd = 180;
            Msg("[VK Clustered] cull active: %u lights -> %u froxels (near %.2f far %.2f)",
                (u32)pc.misc[0], kClusters, gz.nearZ, gz.farZ);
        } else --s_diagCd;
    }
}

VkBuffer     GetLightsHandle(u32 slot) { return s_lights.GetHandle(); (void)slot; }
VkDeviceSize GetLightsOffset(u32 slot) { return kLightStride * (slot < kFramesInFlight ? slot : 0); }
VkDeviceSize GetLightsRange()          { return sizeof(Lights::GpuLight) * Lights::kMaxClusterLights; }
VkBuffer     GetGridHandle()           { return s_grid.GetHandle(); }
VkBuffer     GetIndicesHandle()        { return s_indices.GetHandle(); }

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipe)   { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_layout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_pool)   { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setL)   { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setL, nullptr); s_setL = VK_NULL_HANDLE; }
    s_lights.Destroy(); s_grid.Destroy(); s_indices.Destroy();
    s_lightsMapped = nullptr;
    for (u32 i = 0; i < kFramesInFlight; ++i) s_set[i] = VK_NULL_HANDLE;
    s_inited = false; s_failed = false; s_ready = false;
}

}} // namespace VK::Clustered

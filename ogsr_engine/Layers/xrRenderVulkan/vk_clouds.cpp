// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// Volumetric cloud noise bake — see vk_clouds.h.

#include "stdafx.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_clouds.h"
#include "vk_image.h"            // VK::CreateImage / CreateImageView
#include "vk_compute_util.h"     // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_command_buffer.h"   // CommandManager (BeginImmediate / EndAndSubmitImmediate)
#include "vk_shaders.h"          // g_ShaderManager

namespace VK { namespace Clouds {

namespace {
    constexpr u32 kShapeRes   = 128;   // RGBA8 128^3 = 8 MB
    constexpr u32 kDetailRes  = 32;    // RGBA8  32^3 = 128 KB
    constexpr u32 kWeatherRes = 512;   // RGBA8 512^2 = 1 MB

    bool s_inited = false, s_failed = false, s_ready = false;

    struct Vol {
        VkImage       img   = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView   sample = VK_NULL_HANDLE;   // read by the sky shader
        VkImageView   store  = VK_NULL_HANDLE;   // written by the bake compute
    };
    Vol s_shape, s_detail, s_weather;

    VkSampler s_sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout s_setL3 = VK_NULL_HANDLE, s_setL2 = VK_NULL_HANDLE;
    VkDescriptorPool      s_pool  = VK_NULL_HANDLE;
    VkPipelineLayout      s_lay3  = VK_NULL_HANDLE, s_lay2 = VK_NULL_HANDLE;
    VkPipeline            s_pipe3 = VK_NULL_HANDLE, s_pipe2 = VK_NULL_HANDLE;
    VkDescriptorSet       s_setShape = VK_NULL_HANDLE, s_setDetail = VK_NULL_HANDLE, s_setWeather = VK_NULL_HANDLE;

    struct Push { float p[4]; };

    bool MakeVol(Vol& v, u32 res, u32 depth, const char* name)
    {
        VK::ImageDesc d;
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.extent = { res, res, depth };
        d.mips   = 1;
        d.layers = 1;
        d.type   = (depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        d.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        d.name   = name;
        if (!VK::CreateImage(d, v.img, v.alloc)) return false;

        const VkImageViewType vt = (depth > 1) ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        v.sample = VK::CreateImageView(v.img, VK_FORMAT_R8G8B8A8_UNORM, vt,
                                       VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        v.store  = v.sample;   // same view works for both storage and sampled here
        return v.sample != VK_NULL_HANDLE;
    }

    void Barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout oldL, VkImageLayout newL,
                 VkAccessFlags srcA, VkAccessFlags dstA,
                 VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    VkDescriptorSetLayout MakeStorageSetLayout(VkDescriptorType type)
    {
        return VK::MakeSetLayout({ type }, VK_SHADER_STAGE_COMPUTE_BIT, "Clouds");
    }

    void WriteStorage(VkDescriptorSet set, VkImageView view)
    {
        VK::DescriptorWriter(set).StorageImage(0, view).Flush();
    }
}

// Ready() = the noise was actually BAKED. The views below are valid as soon as the
// images exist, baked or not, because the sky pass declares sampler3D bindings for
// them and there is no 2D fallback that can stand in for a 3D sampler — an unbound
// or wrongly-typed descriptor there is undefined behaviour, not a missing effect.
// If the bake never ran the volumes read as zero, so clouds simply render nothing.
bool Ready()              { return s_ready; }
VkImageView ShapeView()   { return s_shape.sample;   }
VkImageView DetailView()  { return s_detail.sample;  }
VkImageView WeatherView() { return s_weather.sample; }
VkSampler   Sampler()     { return s_sampler; }

bool Init()
{
    if (s_inited) return s_ready;
    s_inited = true;

    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule cs3 = g_ShaderManager->Load("cloud_noise3d.comp.spv");
    VkShaderModule cs2 = g_ShaderManager->Load("cloud_weather.comp.spv");
    const bool canBake = (cs3 != VK_NULL_HANDLE && cs2 != VK_NULL_HANDLE);
    if (!canBake) Msg("![VK Clouds] bake shaders missing — volumes stay empty (clouds render nothing)");

    if (!MakeVol(s_shape,   kShapeRes,   kShapeRes,  "Clouds.Shape")   ||
        !MakeVol(s_detail,  kDetailRes,  kDetailRes, "Clouds.Detail")  ||
        !MakeVol(s_weather, kWeatherRes, 1,          "Clouds.Weather")) {
        Msg("![VK Clouds] volume create failed"); s_failed = true; return false;
    }

    // REPEAT on every axis — the fields are baked tileable and the raymarch wraps
    // them across kilometres of sky. CLAMP here would stretch the edge texel into a
    // visible streak across the whole cloudscape.
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxAnisotropy = 1.f; sci.minLod = 0.f; sci.maxLod = 1.f;
    if (vkCreateSampler(VulkanHW.m_Device, &sci, nullptr, &s_sampler) != VK_SUCCESS) {
        Msg("![VK Clouds] sampler create failed"); s_failed = true; return false;
    }

    if (!canBake) {
        // Still transition the (zeroed) volumes to SHADER_READ so the sky pass can
        // sample them legally. Leaving them UNDEFINED would be a validation error on
        // every frame, and on some drivers a hang.
        VkCommandBuffer c = CommandManager.BeginImmediate();
        if (c != VK_NULL_HANDLE) {
            for (VkImage im : { s_shape.img, s_detail.img, s_weather.img })
                Barrier(c, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        0, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            CommandManager.EndAndSubmitImmediate(c);
        }
        return false;
    }

    s_setL3 = MakeStorageSetLayout(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    s_setL2 = MakeStorageSetLayout(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    if (!s_setL3 || !s_setL2) { s_failed = true; return false; }

    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 3; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_pool) != VK_SUCCESS) { s_failed = true; return false; }

    VkDescriptorSetLayout ls[3] = { s_setL3, s_setL3, s_setL2 };
    VkDescriptorSet       sets[3]{};
    VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = s_pool; dai.descriptorSetCount = 3; dai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(VulkanHW.m_Device, &dai, sets) != VK_SUCCESS) { s_failed = true; return false; }
    s_setShape = sets[0]; s_setDetail = sets[1]; s_setWeather = sets[2];

    WriteStorage(s_setShape,   s_shape.store);
    WriteStorage(s_setDetail,  s_detail.store);
    WriteStorage(s_setWeather, s_weather.store);

    s_lay3 = VK::MakePipelineLayout({ s_setL3 }, sizeof(Push));
    s_lay2 = VK::MakePipelineLayout({ s_setL2 }, sizeof(Push));
    if (!s_lay3 || !s_lay2) { s_failed = true; return false; }
    s_pipe3 = VK::CreateComputePipeline(cs3, s_lay3, "Clouds.Noise3D");
    s_pipe2 = VK::CreateComputePipeline(cs2, s_lay2, "Clouds.Weather");
    if (!s_pipe3 || !s_pipe2) { s_failed = true; return false; }

    // ── Bake, once ───────────────────────────────────────────────────────────
    VkCommandBuffer cmd = CommandManager.BeginImmediate();
    if (cmd == VK_NULL_HANDLE) { Msg("![VK Clouds] BeginImmediate failed"); s_failed = true; return false; }

    Barrier(cmd, s_shape.img,   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    Barrier(cmd, s_detail.img,  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    Barrier(cmd, s_weather.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe3);
    {
        Push p{}; p.p[0] = float(kShapeRes); p.p[1] = 0.f;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_lay3, 0, 1, &s_setShape, 0, nullptr);
        vkCmdPushConstants(cmd, s_lay3, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        const u32 g = (kShapeRes + 3) / 4;
        vkCmdDispatch(cmd, g, g, g);
    }
    {
        Push p{}; p.p[0] = float(kDetailRes); p.p[1] = 1.f;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_lay3, 0, 1, &s_setDetail, 0, nullptr);
        vkCmdPushConstants(cmd, s_lay3, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        const u32 g = (kDetailRes + 3) / 4;
        vkCmdDispatch(cmd, g, g, g);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe2);
    {
        Push p{}; p.p[0] = float(kWeatherRes);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_lay2, 0, 1, &s_setWeather, 0, nullptr);
        vkCmdPushConstants(cmd, s_lay2, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        const u32 g = (kWeatherRes + 7) / 8;
        vkCmdDispatch(cmd, g, g, 1);
    }

    Barrier(cmd, s_shape.img,   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    Barrier(cmd, s_detail.img,  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    Barrier(cmd, s_weather.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    CommandManager.EndAndSubmitImmediate(cmd);   // fence-waited

    s_ready = true;
    Msg("[VK Clouds] noise bake OK — shape %u^3, detail %u^3, weather %u^2 (~%.1f MB)",
        kShapeRes, kDetailRes, kWeatherRes,
        (float(kShapeRes) * kShapeRes * kShapeRes * 4 +
         float(kDetailRes) * kDetailRes * kDetailRes * 4 +
         float(kWeatherRes) * kWeatherRes * 4) / (1024.f * 1024.f));
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipe3) { vkDestroyPipeline(VulkanHW.m_Device, s_pipe3, nullptr); s_pipe3 = VK_NULL_HANDLE; }
    if (s_pipe2) { vkDestroyPipeline(VulkanHW.m_Device, s_pipe2, nullptr); s_pipe2 = VK_NULL_HANDLE; }
    if (s_lay3)  { vkDestroyPipelineLayout(VulkanHW.m_Device, s_lay3, nullptr); s_lay3 = VK_NULL_HANDLE; }
    if (s_lay2)  { vkDestroyPipelineLayout(VulkanHW.m_Device, s_lay2, nullptr); s_lay2 = VK_NULL_HANDLE; }
    if (s_pool)  { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setL3) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setL3, nullptr); s_setL3 = VK_NULL_HANDLE; }
    if (s_setL2) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setL2, nullptr); s_setL2 = VK_NULL_HANDLE; }
    if (s_sampler) { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }

    auto killVol = [](Vol& v) {
        if (v.sample) { vkDestroyImageView(VulkanHW.m_Device, v.sample, nullptr); v.sample = VK_NULL_HANDLE; v.store = VK_NULL_HANDLE; }
        if (v.img)    { VK::Vram::DestroyImage(VulkanHW.m_Allocator, v.img, v.alloc); v.img = VK_NULL_HANDLE; v.alloc = VK_NULL_HANDLE; }
    };
    killVol(s_shape); killVol(s_detail); killVol(s_weather);

    s_setShape = s_setDetail = s_setWeather = VK_NULL_HANDLE;
    s_inited = s_failed = s_ready = false;
}

}}  // namespace VK::Clouds

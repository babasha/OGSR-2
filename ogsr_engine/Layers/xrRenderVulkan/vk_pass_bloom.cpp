// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — bloom pass. See vk_pass_bloom.h.
#include "stdafx.h"
#include "vk_profiler.h"   // TEMP VUID-hunt: VK::Prof::NameImage
#include "vk_pass_bloom.h"
#include "vk_scene_color.h"
#include "vk_descriptors.h" // VK::DescriptorWriter
#include "vk_shaders.h"            // g_ShaderManager
#include "vk_pipeline_cache.h"     // PipelineCache::GetCacheObject
#include "vk_barriers.h"           // ImageBarrier
#include "vk_image.h"              // VK::CreateImage2D / CreateImageView
#include "vk_exposure.h"           // VK::Exposure — shared auto-exposure constants (also used by tonemap)
#include "vk_fullscreen.h"         // VK::Fullscreen — shared fullscreen pipeline + draw
#include "HW_Vulkan.h"

namespace VK { namespace BloomPass {

namespace {
    constexpr u32 kMaxImages = 8;   // mirror SceneColor

    // R4 knobs: threshold is the post-exposure luminance where glow starts
    // (b_params.x analog), knee softens the cutoff.
    constexpr float kThreshold = 0.85f;
    constexpr float kKnee      = 0.35f;
    // Auto-exposure params — shared with vk_pass_tonemap.cpp via vk_exposure.h.
    using Exposure::MiddleGray;   // per color pipeline (see vk_exposure.h) — MUST match the tonemap
    using Exposure::kLowLum;
    using Exposure::ExpMin;
    using Exposure::ExpMax;

    bool                  s_inited = false;
    VkPipeline            s_PipeBuild = VK_NULL_HANDLE;
    VkPipeline            s_PipeBlur  = VK_NULL_HANDLE;
    VkPipelineLayout      s_Layout    = VK_NULL_HANDLE;   // shared: 1 sampler set + push
    VkDescriptorSetLayout s_SetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      s_Pool      = VK_NULL_HANDLE;
    VkDescriptorSet       s_SetScene[kMaxImages] = {};    // build: scene sample view per image idx
    VkDescriptorSet       s_SetA = VK_NULL_HANDLE;        // blur H reads A
    VkDescriptorSet       s_SetB = VK_NULL_HANDLE;        // blur V reads B
    VkSampler             s_Sampler = VK_NULL_HANDLE;
    u32                   s_boundSceneGen = 0;

    // Quarter-res ping-pong RTs (A holds the final result).
    VkImage       s_img[2] = {};
    VmaAllocation s_alloc[2] = {};
    VkImageView   s_view[2] = {};
    VkExtent2D    s_extent = {};
    u32           s_generation = 0;
    bool          s_first[2] = { true, true };   // images start UNDEFINED

    struct BuildPush { float p0[4]; float p1[4]; float p2[4]; };   // p2.x = smoothed exposure (0 = in-shader estimate)
    struct BlurPush  { float dir[4]; };
    static_assert(sizeof(BuildPush) >= sizeof(BlurPush), "shared push range sized by BuildPush");

    void DestroyRTs()
    {
        if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
        for (u32 i = 0; i < 2; ++i) {
            if (s_view[i]) { vkDestroyImageView(VulkanHW.m_Device, s_view[i], nullptr); s_view[i] = VK_NULL_HANDLE; }
            if (s_img[i])  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_img[i], s_alloc[i]); s_img[i] = VK_NULL_HANDLE; s_alloc[i] = VK_NULL_HANDLE; }
            s_first[i] = true;
        }
        s_extent = {};
    }

    bool EnsureRTs(VkExtent2D sceneExtent)
    {
        VkExtent2D want{ sceneExtent.width / 4 ? sceneExtent.width / 4 : 1,
                         sceneExtent.height / 4 ? sceneExtent.height / 4 : 1 };
        if (s_img[0] && want.width == s_extent.width && want.height == s_extent.height)
            return true;
        DestroyRTs();
        s_extent = want;
        for (u32 i = 0; i < 2; ++i) {
            if (!VK::CreateImage2D(VK_FORMAT_R16G16B16A16_SFLOAT, want,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    s_img[i], s_alloc[i], "Bloom"))
                return false;
            s_view[i] = VK::CreateImageView(s_img[i], VK_FORMAT_R16G16B16A16_SFLOAT);
            if (s_view[i] == VK_NULL_HANDLE) return false;
        }
        // Blur sets point at the (new) ping-pong views.
        VK::DescriptorWriter(s_SetA).ImageSampler(0, s_view[0], s_Sampler).Flush();
        VK::DescriptorWriter(s_SetB).ImageSampler(0, s_view[1], s_Sampler).Flush();
        ++s_generation;
        Msg("[VK Bloom] RTs ready (%ux%u, gen %u)", want.width, want.height, s_generation);
        return true;
    }

    // Bloom RTs are R16G16B16A16_SFLOAT, RGBA write, no blend (full-screen overwrite).
    VkPipeline CreatePipe(VkShaderModule vs, VkShaderModule fs)
    {
        return Fullscreen::CreatePipeline(vs, fs, VK_FORMAT_R16G16B16A16_SFLOAT, s_Layout,
                                          Fullscreen::OpaqueAttachment(), "Bloom");
    }

    // One fullscreen pass: render into dst view with the given pipeline/set/push.
    void Draw(VkCommandBuffer cmd, VkImageView dst, VkPipeline pipe, VkDescriptorSet set,
              const void* push, u32 pushSize)
    {
        Fullscreen::DrawSimple(cmd, dst, s_extent, pipe, s_Layout, set, push, pushSize);
    }
}

VkImageView GetResultView() { return s_view[0]; }
u32         Generation()    { return s_generation; }

bool Init()
{
    if (s_inited) return s_PipeBuild != VK_NULL_HANDLE;
    s_inited = true;

    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule vs    = g_ShaderManager->Load("tonemap.vert.spv");   // shared fullscreen triangle
    VkShaderModule build = g_ShaderManager->Load("bloom_build.frag.spv");
    VkShaderModule blur  = g_ShaderManager->Load("bloom_blur.frag.spv");
    if (vs == VK_NULL_HANDLE || build == VK_NULL_HANDLE || blur == VK_NULL_HANDLE) {
        Msg("![VK Bloom] shader load failed"); return false;
    }

    const u32 nSets = kMaxImages + 2;
    {
        VkDescriptorSet sets[kMaxImages + 2]{};
        if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, nSets,
                                    s_SetLayout, s_Pool, sets,
                                    VK_SHADER_STAGE_FRAGMENT_BIT, "Bloom")) {
            return false;
        }
        for (u32 i = 0; i < kMaxImages; ++i) s_SetScene[i] = sets[i];
        s_SetA = sets[kMaxImages];
        s_SetB = sets[kMaxImages + 1];
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f; si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_Sampler) != VK_SUCCESS) {
        Msg("![VK Bloom] sampler failed"); return false;
    }

    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, (u32)sizeof(BuildPush) };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &s_SetLayout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_Layout) != VK_SUCCESS) {
        Msg("![VK Bloom] pipeline layout failed"); return false;
    }

    s_PipeBuild = CreatePipe(vs, build);
    s_PipeBlur  = CreatePipe(vs, blur);
    if (s_PipeBuild == VK_NULL_HANDLE || s_PipeBlur == VK_NULL_HANDLE) return false;

    Msg("[VK Bloom] Init OK (threshold %.2f knee %.2f, quarter-res 9-tap gaussian)", kThreshold, kKnee);
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    DestroyRTs();
    if (s_PipeBuild) { vkDestroyPipeline(VulkanHW.m_Device, s_PipeBuild, nullptr); s_PipeBuild = VK_NULL_HANDLE; }
    if (s_PipeBlur)  { vkDestroyPipeline(VulkanHW.m_Device, s_PipeBlur, nullptr);  s_PipeBlur = VK_NULL_HANDLE; }
    if (s_Layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_Layout, nullptr); s_Layout = VK_NULL_HANDLE; }
    if (s_Sampler)   { vkDestroySampler(VulkanHW.m_Device, s_Sampler, nullptr); s_Sampler = VK_NULL_HANDLE; }
    if (s_Pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_Pool, nullptr); s_Pool = VK_NULL_HANDLE; }
    if (s_SetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_SetLayout, nullptr); s_SetLayout = VK_NULL_HANDLE; }
    s_inited = false; s_boundSceneGen = 0; s_generation = 0;
    for (auto& s : s_SetScene) s = VK_NULL_HANDLE;
    s_SetA = s_SetB = VK_NULL_HANDLE;
}

void Execute(VkCommandBuffer cmd, u32 imageIndex, VkExtent2D sceneExtent, u32 sceneGen)
{
    if (s_PipeBuild == VK_NULL_HANDLE) return;
    if (!EnsureRTs(sceneExtent)) return;

    // (Re)bind the scene sample views when SceneColor was recreated.
    if (sceneGen != s_boundSceneGen) {
        for (u32 i = 0; i < SceneColor::Count() && i < kMaxImages; ++i)
            VK::DescriptorWriter(s_SetScene[i])
                .ImageSampler(0, SceneColor::GetSampleView(i), s_Sampler).Flush();
        s_boundSceneGen = sceneGen;
    }

    auto toColor = [&](u32 i) {
        ImageBarrier(cmd, s_img[i],
                     s_first[i] ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        s_first[i] = false;
    };
    auto toRead = [&](u32 i) {
        ImageBarrier(cmd, s_img[i],
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };

    // Pick the mip whose size is closest to quarter res (mip 2 of the scene).
    const float quarterLod = 2.0f;
    const float topLod     = float(SceneColor::MipLevels() - 1);

    // 1) Bright pass: scene (mips, SHADER_READ after GenerateMips) → A.
    toColor(0);
    BuildPush bp{};
    bp.p0[0] = quarterLod; bp.p0[1] = topLod; bp.p0[2] = MiddleGray(); bp.p0[3] = kLowLum;
    bp.p1[0] = ExpMin();   bp.p1[1] = ExpMax(); bp.p1[2] = kThreshold; bp.p1[3] = kKnee;
    // The exposure the composite ACTUALLY applies (robust meter + temporal ease
    // + user compensation). The in-shader estimate below it is instantaneous AND
    // arithmetic, so with r_expo_meter on the two would threshold different
    // brightnesses — a window would bloom against an exposure the viewer never
    // sees. 0 for the first frames = the shader keeps its own estimate.
    bp.p2[0] = Exposure::Smoothed();
    Draw(cmd, s_view[0], s_PipeBuild, s_SetScene[imageIndex < kMaxImages ? imageIndex : 0], &bp, sizeof(bp));
    toRead(0);

    // 2) Blur H: A → B.
    toColor(1);
    BlurPush hp{}; hp.dir[0] = 1.f; hp.dir[1] = 0.f;
    Draw(cmd, s_view[1], s_PipeBlur, s_SetA, &hp, sizeof(hp));
    toRead(1);

    // 3) Blur V: B → A (final result in A).
    toColor(0);
    BlurPush vp2{}; vp2.dir[0] = 0.f; vp2.dir[1] = 1.f;
    Draw(cmd, s_view[0], s_PipeBlur, s_SetB, &vp2, sizeof(vp2));
    toRead(0);
}

}}  // namespace VK::BloomPass

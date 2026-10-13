// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// DLSS Ray Reconstruction guide G-buffer — see vk_rr_guides.h for the why.
#include "stdafx.h"
#include "vk_rr_guides.h"
#include "HW_Vulkan.h"
#include "vk_image.h"        // VK::CreateImage2D / CreateImageView
#include "vk_barriers.h"     // ImageBarrier
#include "vk_rendering.h"    // VK::RenderingBuilder
#include "vk_gfx_pipeline.h" // VK::GfxPipelineBuilder
#include "vk_vram_stats.h"   // VK::Vram::DestroyImage
#include "vk_shaders.h"       // g_ShaderManager — the debug-view fragment shader
#include "vk_descriptors.h"   // VK::MakeDescriptorSets / DescriptorWriter
#include "vk_fullscreen.h"    // VK::Fullscreen — shared fullscreen-triangle pipeline + draw
#include "vk_swapchain.h"     // Swapchain.m_Format — the debug view draws post-tonemap

extern int ps_r_dlss_rr;        // r_dlss_rr — Ray Reconstruction master toggle (vk_console_min.cpp)
extern int ps_r_dlss_rr_debug;  // r_dlss_rr_debug — guide view: 1 diff, 2 spec, 3 normal, 4 roughness

namespace VK { namespace RRGuide {

namespace {
    // ⚠NOT sRGB for the albedos: "at this time sRGB formats are not supported"
    // (DLSS-RR Integration Guide 3.4.1 / 3.4.2). The value RR wants is the linear
    // reflectance the shading multiplied by, which is what our shaders hold anyway.
    const VkFormat kNormalFmt = VK_FORMAT_R16G16B16A16_SFLOAT;  // normal.xyz + roughness.w
    const VkFormat kAlbedoFmt = VK_FORMAT_R8G8B8A8_UNORM;

    struct Target {
        VkImage       img   = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView   view  = VK_NULL_HANDLE;
    };

    Target     s_nrm, s_dif, s_spc;
    VkExtent2D s_extent   = { 0, 0 };
    u32        s_gen      = 0;
    bool       s_ready    = false;

    void DestroyTarget(Target& t)
    {
        if (t.view) { vkDestroyImageView(VulkanHW.m_Device, t.view, nullptr); t.view = VK_NULL_HANDLE; }
        if (t.img)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, t.img, t.alloc); t.img = VK_NULL_HANDLE; t.alloc = VK_NULL_HANDLE; }
    }

    bool MakeTarget(Target& t, VkFormat fmt, VkExtent2D e, const char* name)
    {
        // TRANSFER_DST is for OUR per-frame clear (BeginFrame), SAMPLED is for the
        // debug view and for NGX's formatted reads of the guide.
        if (!VK::CreateImage2D(fmt, e,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                t.img, t.alloc, name))
            return false;
        t.view = VK::CreateImageView(t.img, fmt);
        if (t.view == VK_NULL_HANDLE) { DestroyTarget(t); return false; }
        return true;
    }

    VkImageSubresourceRange FullColor()
    {
        VkImageSubresourceRange r{};
        r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.levelCount = 1; r.layerCount = 1;
        return r;
    }
}

bool Wanted() { return ps_r_dlss_rr != 0; }
bool Ready()  { return s_ready; }

VkExtent2D Extent()     { return s_extent; }
u32        Generation() { return s_gen; }

VkImage     NormalImage()  { return s_nrm.img;  }
VkImageView NormalView()   { return s_nrm.view; }
VkFormat    NormalFormat() { return kNormalFmt; }
VkImage     DiffAlbImage() { return s_dif.img;  }
VkImageView DiffAlbView()  { return s_dif.view; }
VkFormat    DiffAlbFormat(){ return kAlbedoFmt; }
VkImage     SpecAlbImage() { return s_spc.img;  }
VkImageView SpecAlbView()  { return s_spc.view; }
VkFormat    SpecAlbFormat(){ return kAlbedoFmt; }

void Destroy()
{
    DestroyTarget(s_nrm); DestroyTarget(s_dif); DestroyTarget(s_spc);
    s_extent = { 0, 0 };
    s_ready  = false;
}


void EnsureSize(VkExtent2D extent)
{
    // ⚠IDLE BEFORE ANY DESTROY. These are colour ATTACHMENTS of command buffers that
    // may still be in flight — r_dlss_rr is toggled from the console mid-frame-loop,
    // so freeing them on the spot is a use-after-free the GPU discovers, not the
    // validation layer. The toggle is rare and user-initiated; an idle is the cheap
    // half of this trade. (CRender::Begin already idles on a render-extent change,
    // which is why the resize path below never hit this.)
    // RR off → the targets are pure cost. Release them so toggling the feature off
    // gives the VRAM back (RGBA16F + 2x RGBA8 at 1440p is ~45 MB).
    if (!Wanted()) {
        if (s_ready) {
            VK_DeviceWaitIdleLocked();
            Destroy();
            Msg("[VK RR] guide targets released (r_dlss_rr 0)");
        }
        return;
    }
    if (s_ready && extent.width == s_extent.width && extent.height == s_extent.height) return;

    if (s_ready) VK_DeviceWaitIdleLocked();
    Destroy();
    if (extent.width == 0 || extent.height == 0) return;

    if (!MakeTarget(s_nrm, kNormalFmt, extent, "RR.NormalRough") ||
        !MakeTarget(s_dif, kAlbedoFmt, extent, "RR.DiffAlbedo")  ||
        !MakeTarget(s_spc, kAlbedoFmt, extent, "RR.SpecAlbedo"))
    {
        Msg("![VK RR] guide target allocation failed — Ray Reconstruction stays off");
        Destroy();
        return;
    }

    s_extent = extent;
    s_ready  = true;
    ++s_gen;
    Msg("[VK RR] guide G-buffer ready (%ux%u: RGBA16F normal+rough, 2x RGBA8 albedo, gen %u)",
        extent.width, extent.height, s_gen);
}

void BeginFrame(VkCommandBuffer cmd)
{
    if (!s_ready) return;

    // ⚠0.5, not 0. An unwritten guide texel must read as "ordinary grey material",
    // which is the SDK's own recommendation for sky; black would tell RR the pixel
    // reflects nothing and it would hand the whole texel's radiance to the
    // denoised-lighting term. The normal is cleared to zero length + roughness 1:
    // "no surface here", which is what sky and any not-yet-converted class are.
    VkClearColorValue cNrm{}; cNrm.float32[0] = 0.f; cNrm.float32[1] = 0.f; cNrm.float32[2] = 0.f; cNrm.float32[3] = 1.f;
    VkClearColorValue cAlb{}; cAlb.float32[0] = 0.5f; cAlb.float32[1] = 0.5f; cAlb.float32[2] = 0.5f; cAlb.float32[3] = 1.f;

    const VkImage      imgs[3] = { s_nrm.img, s_dif.img, s_spc.img };
    VkImageSubresourceRange rr = FullColor();

    // UNDEFINED source: every texel is overwritten by the clear, so the previous
    // frame's contents are worth nothing and discarding them skips a decompress.
    ImageBarriers(cmd, 3, imgs, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(cmd, s_nrm.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cNrm, 1, &rr);
    vkCmdClearColorImage(cmd, s_dif.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cAlb, 1, &rr);
    vkCmdClearColorImage(cmd, s_spc.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cAlb, 1, &rr);
    ImageBarriers(cmd, 3, imgs, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void Attach(RenderingBuilder& rb)
{
    if (!s_ready) return;
    // LOAD: the frame-start clear (and every earlier opaque scope) is the history
    // these scopes append to — an overlay scope that CLEARed would erase the world.
    rb.Color(s_nrm.view, VK_ATTACHMENT_LOAD_OP_LOAD)
      .Color(s_dif.view, VK_ATTACHMENT_LOAD_OP_LOAD)
      .Color(s_spc.view, VK_ATTACHMENT_LOAD_OP_LOAD);
}

void PipelineTargets(GfxPipelineBuilder& pb, bool write)
{
    if (!s_ready) return;
    const VkColorComponentFlags mask = write
        ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
        : 0;
    pb.Color(kNormalFmt, mask).Color(kAlbedoFmt, mask).Color(kAlbedoFmt, mask);
}

void ToGeneral(VkCommandBuffer cmd)
{
    if (!s_ready) return;
    const VkImage imgs[3] = { s_nrm.img, s_dif.img, s_spc.img };
    ImageBarriers(cmd, 3, imgs, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

void ToAttachment(VkCommandBuffer cmd)
{
    if (!s_ready) return;
    const VkImage imgs[3] = { s_nrm.img, s_dif.img, s_spc.img };
    ImageBarriers(cmd, 3, imgs, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// ---------------------------------------------------------------------------
//  r_dlss_rr_debug — the guide viewer. Lazily built the first time the cvar is
//  turned on, so a renderer running with RR off never allocates it.
// ---------------------------------------------------------------------------
namespace {
    bool                  s_dbgTried  = false;
    bool                  s_dbgFailed = false;
    VkDescriptorSetLayout s_dbgSetL   = VK_NULL_HANDLE;
    VkDescriptorPool      s_dbgPool   = VK_NULL_HANDLE;
    VkDescriptorSet       s_dbgSet    = VK_NULL_HANDLE;
    VkPipelineLayout      s_dbgLayout = VK_NULL_HANDLE;
    VkPipeline            s_dbgPipe   = VK_NULL_HANDLE;
    VkSampler             s_dbgSamp   = VK_NULL_HANDLE;
    struct DbgPush { float p[4]; };

    bool EnsureDebug()
    {
        if (s_dbgTried) return !s_dbgFailed;
        s_dbgTried = true;
        if (!g_ShaderManager) { s_dbgFailed = true; return false; }
        VkShaderModule vs = g_ShaderManager->Load("tonemap.vert.spv");        // shared fullscreen triangle
        VkShaderModule fs = g_ShaderManager->Load("rr_guide_debug.frag.spv");
        if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
            Msg("![VK RR] rr_guide_debug.frag.spv missing — r_dlss_rr_debug unavailable");
            s_dbgFailed = true; return false;
        }
        if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, 1,
                                    s_dbgSetL, s_dbgPool, &s_dbgSet,
                                    VK_SHADER_STAGE_FRAGMENT_BIT, "RRGuideDebug")) {
            s_dbgFailed = true; return false;
        }
        // NEAREST: the point is to read the stored value, not a filtered opinion of it.
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_dbgSamp) != VK_SUCCESS) {
            Msg("![VK RR] debug sampler create failed"); s_dbgFailed = true; return false;
        }
        VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, (u32)sizeof(DbgPush) };
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s_dbgSetL;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_dbgLayout) != VK_SUCCESS) {
            s_dbgFailed = true; return false;
        }
        s_dbgPipe = VK::Fullscreen::CreatePipeline(vs, fs, Swapchain.m_Format, s_dbgLayout,
                                                   VK::Fullscreen::OpaqueAttachment(), "RRGuideDebug");
        if (s_dbgPipe == VK_NULL_HANDLE) { s_dbgFailed = true; return false; }
        return true;
    }
}

void DrawDebugOverlay(VkCommandBuffer cmd, VkImageView dstView, VkExtent2D extent)
{
    if (ps_r_dlss_rr_debug == 0 || !s_ready || dstView == VK_NULL_HANDLE) return;
    if (!EnsureDebug()) return;

    // The guides live in COLOR_ATTACHMENT all frame (single-layout discipline); the
    // viewer is the only thing that samples them, so it owns the round trip.
    const VkImage imgs[3] = { s_nrm.img, s_dif.img, s_spc.img };
    ImageBarriers(cmd, 3, imgs, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VK::DescriptorWriter(s_dbgSet)
        .ImageSampler(0, s_nrm.view, s_dbgSamp)
        .ImageSampler(1, s_dif.view, s_dbgSamp)
        .ImageSampler(2, s_spc.view, s_dbgSamp)
        .Flush();

    DbgPush dp{};
    dp.p[0] = float(ps_r_dlss_rr_debug);
    dp.p[2] = extent.width  ? 1.0f / float(extent.width)  : 0.0f;
    dp.p[3] = extent.height ? 1.0f / float(extent.height) : 0.0f;
    VK::Fullscreen::DrawSimple(cmd, dstView, extent, s_dbgPipe, s_dbgLayout, s_dbgSet,
                               &dp, (u32)sizeof(dp));

    ImageBarriers(cmd, 3, imgs, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void Shutdown()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    Destroy();
    if (s_dbgPipe)   { vkDestroyPipeline(VulkanHW.m_Device, s_dbgPipe, nullptr); s_dbgPipe = VK_NULL_HANDLE; }
    if (s_dbgLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_dbgLayout, nullptr); s_dbgLayout = VK_NULL_HANDLE; }
    if (s_dbgSamp)   { vkDestroySampler(VulkanHW.m_Device, s_dbgSamp, nullptr); s_dbgSamp = VK_NULL_HANDLE; }
    if (s_dbgPool)   { vkDestroyDescriptorPool(VulkanHW.m_Device, s_dbgPool, nullptr); s_dbgPool = VK_NULL_HANDLE; }
    if (s_dbgSetL)   { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_dbgSetL, nullptr); s_dbgSetL = VK_NULL_HANDLE; }
    s_dbgSet = VK_NULL_HANDLE;
    s_dbgTried = false; s_dbgFailed = false;
}

}}  // namespace VK::RRGuide

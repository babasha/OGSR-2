// xrRenderVulkan - Water flow simulation (compute) — velocity/momentum model.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"       // VK::DescriptorWriter
#include "vk_profiler.h"   // TEMP VUID-hunt: VK::Prof::NameImage
#include "vk_water_sim.h"
#include "vk_image.h"      // VK::CreateImage2D / CreateImageView
#include "vk_barriers.h"   // VK::ImageBarrier (explicit stage/access overload)
#include "vk_compute_util.h" // VK::MakePipelineLayout / CreateComputePipeline
#include "vk_shadow.h"          // rain ortho VP / views / size / sampler (the grid)
#include "vk_shaders.h"         // g_ShaderManager
#include "vk_pipeline_cache.h"  // shared pipeline cache object

// Console knobs (global scope — block-scope extern inside namespace VK mangles).
extern int   ps_r_water_sim;    // r_water_sim  — master enable
extern float ps_r_water_rain;   // r_water_rain — rain input rate (depth/s at density 1)
extern float ps_r_water_evap;   // r_water_evap — exponential leak rate
extern float ps_r_water_flow;   // r_water_flow — downhill ACCEL (gravity) — drives flow speed/streams
extern int   ps_r_water_iters;  // r_water_iters — sim steps per frame (1 = no leak compounding)

namespace VK { namespace WaterSim {

namespace {
    bool          s_inited = false, s_failed = false, s_first = true;
    u32           s_size = 0;
    Fmatrix       s_prevVP;

    // Two ping-pong pairs: water DEPTH (R16F, sampled by receivers) and VELOCITY
    // (RG16F). Each: a sampled "state" + a storage "scratch" copied back to state.
    struct Buf { VkImage img = VK_NULL_HANDLE; VmaAllocation alloc = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
    Buf s_depth, s_depthScr, s_vel, s_velScr;

    VkSampler             s_sampler   = VK_NULL_HANDLE;
    VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
    VkDescriptorSet       s_set       = VK_NULL_HANDLE;
    VkPipelineLayout      s_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            s_pipe       = VK_NULL_HANDLE;

    struct PushConstants {
        Fmatrix curInvVP;
        Fmatrix prevVP;
        float   p0[4];   // N, rainAdd·dt, leak·dt, accel
        float   p1[4];   // rainDensity, zNear, zRange, hasPrev
        float   p2[4];   // damping, dt, maxVel, 0
    };

    bool createImage(VkFormat fmt, VkImageUsageFlags usage, Buf& out)
    {
        if (!VK::CreateImage2D(fmt, { s_size, s_size }, usage, out.img, out.alloc, "WaterSim"))
            return false;
        out.view = VK::CreateImageView(out.img, fmt);
        return out.view != VK_NULL_HANDLE;
    }

    void destroyBuf(Buf& b)
    {
        if (b.view) { vkDestroyImageView(VulkanHW.m_Device, b.view, nullptr); b.view = VK_NULL_HANDLE; }
        if (b.img)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, b.img, b.alloc); b.img = VK_NULL_HANDLE; }
    }

    // compute write (GENERAL) -> copy to state -> back to working layouts.
    void copyBack(VkCommandBuffer cmd, Buf& scr, Buf& state)
    {
        VK::ImageBarrier(cmd, scr.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VK::ImageBarrier(cmd, state.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy cp{};
        cp.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.extent = { s_size, s_size, 1 };
        vkCmdCopyImage(cmd, scr.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        VK::ImageBarrier(cmd, state.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        VK::ImageBarrier(cmd, scr.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    }
}

bool Ready() { return s_inited && !s_failed; }
VkImageView GetStateView() { return s_depth.view; }   // water depth, for EnvLight binding 11
VkImageView GetVelView()   { return s_vel.view; }     // water velocity, for EnvLight binding 12
VkSampler   GetSampler()   { return s_sampler; }

bool Init()
{
    if (s_inited) return true;
    if (s_failed) return false;
    if (ShadowMap::GetRainView() == VK_NULL_HANDLE || ShadowMap::GetGroundView() == VK_NULL_HANDLE) return false;
    s_size = ShadowMap::RainSize();
    if (s_size == 0) return false;
    s_prevVP.identity();

    const VkImageUsageFlags stateUse = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags scrUse   = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!createImage(VK_FORMAT_R16_SFLOAT,    stateUse, s_depth)    ||
        !createImage(VK_FORMAT_R16_SFLOAT,    scrUse,   s_depthScr) ||
        !createImage(VK_FORMAT_R16G16_SFLOAT, stateUse, s_vel)      ||
        !createImage(VK_FORMAT_R16G16_SFLOAT, scrUse,   s_velScr)) {
        Msg("![VK Water] image create failed"); s_failed = true; return false;
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.25f;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) {
        Msg("![VK Water] sampler create failed"); s_failed = true; return false;
    }

    if (!g_ShaderManager) { Msg("![VK Water] g_ShaderManager null"); s_failed = true; return false; }

    // 0..3 = sampled inputs, 4/5 = storage outputs.
    constexpr auto kTex = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    constexpr auto kImg = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (!VK::MakeDescriptorSets({ kTex, kTex, kTex, kTex, kImg, kImg }, 1,
                                s_setLayout, s_pool, &s_set,
                                VK_SHADER_STAGE_COMPUTE_BIT, "Water.Sim")) {
        s_failed = true; return false;
    }

    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo ii[6] = {
        { ShadowMap::GetSampler(), ShadowMap::GetGroundView(), RO },   // 0 terrain (no trees)
        { ShadowMap::GetSampler(), ShadowMap::GetRainView(),   RO },   // 1 rain occ (exposure)
        { s_sampler, s_depth.view, RO },                              // 2 prev depth
        { s_sampler, s_vel.view,   RO },                              // 3 prev velocity
        { VK_NULL_HANDLE, s_depthScr.view, VK_IMAGE_LAYOUT_GENERAL }, // 4 out depth
        { VK_NULL_HANDLE, s_velScr.view,   VK_IMAGE_LAYOUT_GENERAL }, // 5 out velocity
    };
    VK::DescriptorWriter w(s_set);
    for (u32 i = 0; i < 6; ++i)
        w.Image(i, (i < 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                           : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ii[i]);
    w.Flush();

    s_pipeLayout = VK::MakePipelineLayout({ s_setLayout }, sizeof(PushConstants));

    VkShaderModule cs = g_ShaderManager->Load("water_sim.comp.spv");
    if (cs == VK_NULL_HANDLE) { Msg("![VK Water] water_sim.comp.spv load failed"); s_failed = true; return false; }
    s_pipe = VK::CreateComputePipeline(cs, s_pipeLayout, "Water.Sim");
    if (s_pipe == VK_NULL_HANDLE) { s_failed = true; return false; }

    s_inited = true; s_first = true;
    Msg("[VK Water] velocity sim init OK (%ux%u, depth R16F + vel RG16F)", s_size, s_size);
    return true;
}

void Dispatch(VkCommandBuffer cmd, float rainDensity01)
{
    if (!ps_r_water_sim) return;
    if (!s_inited && !Init()) return;

    if (s_first) {
        for (Buf* st : { &s_depth, &s_vel })
            VK::ImageBarrier(cmd, st->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        for (Buf* sc : { &s_depthScr, &s_velScr })
            VK::ImageBarrier(cmd, sc->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    const Fmatrix& curVP = ShadowMap::GetRainVP();
    Fmatrix curInv; curInv.invert(curVP);
    const float dt = _min(RV().fTimeDelta, 0.066f);

    PushConstants pc{};
    pc.curInvVP = curInv;
    pc.prevVP   = s_prevVP;
    pc.p0[0] = float(s_size);
    pc.p0[1] = ps_r_water_rain * dt;
    pc.p0[2] = ps_r_water_evap * dt;          // exponential leak
    pc.p0[3] = ps_r_water_flow;               // downhill accel (gravity)
    pc.p1[0] = clampr(rainDensity01, 0.f, 1.f);
    pc.p1[1] = 1.f;                           // rain ortho zNear
    pc.p1[2] = 349.f;                         // rain ortho zRange
    pc.p1[3] = s_first ? 0.f : 1.f;
    pc.p2[0] = 0.90f;                         // velocity damping (friction)
    pc.p2[1] = dt;
    pc.p2[2] = 0.05f;                         // max speed (uv/sec)
    pc.p2[3] = 0.f;

    const u32 groups = (s_size + 7u) / 8u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeLayout, 0, 1, &s_set, 0, nullptr);
    vkCmdPushConstants(cmd, s_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groups, groups, 1);

    copyBack(cmd, s_depthScr, s_depth);
    copyBack(cmd, s_velScr,   s_vel);

    s_prevVP = curVP;
    s_first  = false;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipe)       { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_pipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_pipeLayout, nullptr); s_pipeLayout = VK_NULL_HANDLE; }
    if (s_pool)       { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout)  { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    if (s_sampler)    { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    destroyBuf(s_depth); destroyBuf(s_depthScr); destroyBuf(s_vel); destroyBuf(s_velScr);
    s_set = VK_NULL_HANDLE;
    s_inited = false; s_failed = false; s_first = true;
}

}}  // namespace VK::WaterSim

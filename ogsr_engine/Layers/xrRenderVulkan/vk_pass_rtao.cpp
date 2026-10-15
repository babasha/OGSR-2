// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — RTAO overlay pass. See vk_pass_rtao.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_pass_rtao.h"
#include "vk_rt_dyn.h"             // RTDyn::TLAS — static instances + this frame's skeletons
#include "vk_rt_scene.h"
#include "vk_descriptors.h"     // VK::MakeDescriptorSets
#include "vk_shaders.h"         // g_ShaderManager
#include "vk_fullscreen.h"      // shared fullscreen pipeline skeleton
#include "vk_rendering.h"       // VK::RenderingBuilder (this draw LOADs, unlike Fullscreen::DrawMRT)
#include "vk_command_buffer.h"  // CommandManager.GetCurrentFrame()
#include "vk_profiler.h"
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"   // RV().vCameraPosition — the ray origins are world-space

extern int   ps_r_rtao;
extern int   ps_r_rtao_rays;
extern float ps_r_rtao_radius;
extern float ps_r_rtao_dist;
extern float ps_r_rtao_bias;
extern float ps_r_rtao_power;
extern int   ps_r_rtao_debug;
extern float ps_r_ssao_strength;   // the consumer's exponent the shader pre-compensates for

namespace VK { namespace RTAOPass {

namespace {

constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;

// MUST match SSAOPass's raw targets — this pass draws into them.
constexpr VkFormat kAOFormat = VK_FORMAT_R16_SFLOAT;
constexpr VkFormat kILFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

bool s_inited = false;
bool s_failed = false;

VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
VkDescriptorSet       s_sets[kFramesInFlight] = {};
VkPipelineLayout      s_layout    = VK_NULL_HANDLE;
VkPipeline            s_pipeMin   = VK_NULL_HANDLE;   // min-blend over GTAO (normal path)
VkPipeline            s_pipeOver  = VK_NULL_HANDLE;   // plain overwrite (r_rtao_debug 1)

// The AS descriptor each set currently holds. Sets live across frames, so a
// rebuild that swapped the TLAS has to rewrite them — but only then: rewriting a
// set an executing command buffer still references is exactly the hazard the
// WorldGPU HZB comment warns about.
VkAccelerationStructureKHR s_boundTLAS[kFramesInFlight] = {};
VkImageView                s_boundDepth[kFramesInFlight] = {};
VkImageView                s_boundNormal[kFramesInFlight] = {};

struct RTAOPush {
    float camDir[4];     // xyz = forward, w = ray length (m)
    float camRightT[4];
    float camTopT[4];
    float zp[4];         // x = proj _33, y = proj _43, z = rays, w = normal-offset bias
    float res[4];
    float camPos[4];     // xyz = camera world position, w = cutoff distance
    float params[4];     // x = jitter phase, y = AO power, z/w = reserved
    float shape[4];      // x = falloff exponent, y = 1 / consumer strength, z = backface-is-miss, w = unused
};
static_assert(sizeof(RTAOPush) == 128, "must match rtao.frag's PC block (128 = the guaranteed push-constant minimum)");

}   // anon namespace

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;

    if (!RTScene::Init()) { s_failed = true; return false; }   // no ray query on this device
    if (!g_ShaderManager) { s_failed = true; return false; }

    VkShaderModule vs = g_ShaderManager->Load("tonemap.vert.spv");   // shared fullscreen triangle
    VkShaderModule fs = g_ShaderManager->Load("rtao.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        Msg("![VK RTAO] rtao.frag.spv missing — RTAO disabled");
        s_failed = true; return false;
    }

    // 0 = scene depth, 1 = normal G-buffer, 2 = TLAS.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR },
                                kFramesInFlight, s_setLayout, s_pool, s_sets,
                                VK_SHADER_STAGE_FRAGMENT_BIT, "RTAO")) {
        s_failed = true; return false;
    }

    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, (u32)sizeof(RTAOPush) };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &s_setLayout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_layout) != VK_SUCCESS) {
        s_failed = true; return false;
    }

    // Target 0: MIN blend — the darker of (GTAO already in the attachment, RTAO
    // from this draw) survives, which is the compose rule stated in the header
    // and costs nothing (fixed-function). Target 1 (SSIL): write mask 0, the
    // attachment exists only because the layout is shared with the GTAO pipeline.
    VkPipelineColorBlendAttachmentState minBlend{};
    minBlend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT;
    minBlend.blendEnable         = VK_TRUE;
    minBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    minBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    minBlend.colorBlendOp        = VK_BLEND_OP_MIN;
    minBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    minBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    minBlend.alphaBlendOp        = VK_BLEND_OP_MIN;

    const VkFormat fmts[2] = { kAOFormat, kILFormat };
    {
        const VkPipelineColorBlendAttachmentState blends[2] = { minBlend, Fullscreen::OpaqueAttachment(0) };
        s_pipeMin = Fullscreen::CreatePipelineMRT(vs, fs, fmts, 2, s_layout, blends, "RTAO");
    }
    {
        const VkPipelineColorBlendAttachmentState blends[2] = {
            Fullscreen::OpaqueAttachment(VK_COLOR_COMPONENT_R_BIT), Fullscreen::OpaqueAttachment(0) };
        s_pipeOver = Fullscreen::CreatePipelineMRT(vs, fs, fmts, 2, s_layout, blends, "RTAOdbg");
    }
    if (s_pipeMin == VK_NULL_HANDLE || s_pipeOver == VK_NULL_HANDLE) { s_failed = true; return false; }

    Msg("[VK RTAO] init OK (ray query, half-res overlay on GTAO)");
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    RTDyn::Destroy();   // its structures reference RTScene's, which SSAOPass frees right after this
    if (s_pipeMin)   { vkDestroyPipeline(VulkanHW.m_Device, s_pipeMin, nullptr);   s_pipeMin = VK_NULL_HANDLE; }
    if (s_pipeOver)  { vkDestroyPipeline(VulkanHW.m_Device, s_pipeOver, nullptr);  s_pipeOver = VK_NULL_HANDLE; }
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        s_sets[i] = VK_NULL_HANDLE;
        s_boundTLAS[i] = VK_NULL_HANDLE;
        s_boundDepth[i] = VK_NULL_HANDLE;
        s_boundNormal[i] = VK_NULL_HANDLE;
    }
    s_inited = false; s_failed = false;
}

bool Enabled()
{
    return ps_r_rtao != 0 && s_inited && !s_failed && RTScene::Ready();
}

void Overlay(VkCommandBuffer cmd, VkImageView aoView, VkImageView ilView, VkExtent2D extent,
             VkImageView depthView, VkImageView normalView, VkSampler sampNear,
             const ProjTerms& pt, float jitterPhase)
{
    if (!Enabled()) return;
    // The dynamic module's TLAS carries the static instances too, so it is a
    // strict superset whenever it exists this frame (r_rt_dyn, skeletons on screen).
    const bool dyn = RTDyn::Ready();
    const VkAccelerationStructureKHR tlas = dyn ? RTDyn::TLAS() : RTScene::TLAS();
    if (tlas == VK_NULL_HANDLE) return;

    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;

    // Rewrite only on an actual change (TLAS swap after a rebuild, or a
    // swapchain/RT recreate moving the depth/normal views).
    if (s_boundTLAS[slot] != tlas || s_boundDepth[slot] != depthView || s_boundNormal[slot] != normalView) {
        VkDescriptorImageInfo depthI{ sampNear, depthView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo normI { sampNear, normalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
        asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asInfo.accelerationStructureCount = 1;
        asInfo.pAccelerationStructures    = &tlas;

        VkWriteDescriptorSet w[3]{};
        for (u32 i = 0; i < 3; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s_sets[slot];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &depthI;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &normI;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w[2].pNext = &asInfo;
        vkUpdateDescriptorSets(VulkanHW.m_Device, 3, w, 0, nullptr);

        s_boundTLAS[slot]   = tlas;
        s_boundDepth[slot]  = depthView;
        s_boundNormal[slot] = normalView;
    }

    // The cutoff can never exceed what the bubble actually covers: a ray that
    // leaves the BLAS misses, and a miss reads as "open sky" — bright, wrong, and
    // exactly at the distance where the eye is looking for the far scenery. Keep
    // it inside the built radius minus the drift since the anchor.
    const Fvector& anchor = RTScene::Anchor();
    const float drift  = RV().vCameraPosition.distance_to(anchor);
    const float safe   = _max(0.f, RTScene::Coverage() * 0.85f - drift - ps_r_rtao_radius);
    const float cutoff = _min(ps_r_rtao_dist, safe);
    if (cutoff <= 1.f) return;   // nothing usable this frame (just after a level change)

    RTAOPush push{};
    push.camDir[0] = pt.dir.x; push.camDir[1] = pt.dir.y; push.camDir[2] = pt.dir.z;
    push.camDir[3] = _max(0.05f, ps_r_rtao_radius);
    push.camRightT[0] = pt.right.x * pt.tanX; push.camRightT[1] = pt.right.y * pt.tanX;
    push.camRightT[2] = pt.right.z * pt.tanX; push.camRightT[3] = pt.tanX;
    push.camTopT[0]   = pt.top.x * pt.tanY;   push.camTopT[1]   = pt.top.y * pt.tanY;
    push.camTopT[2]   = pt.top.z * pt.tanY;   push.camTopT[3]   = pt.tanY;
    push.zp[0] = pt.p33;
    push.zp[1] = pt.p43;
    push.zp[2] = float(_max(1, _min(16, ps_r_rtao_rays)));
    push.zp[3] = ps_r_rtao_bias;
    push.res[0] = float(extent.width);
    push.res[1] = float(extent.height);
    push.res[2] = 1.f / float(extent.width);
    push.res[3] = 1.f / float(extent.height);
    push.camPos[0] = RV().vCameraPosition.x;
    push.camPos[1] = RV().vCameraPosition.y;
    push.camPos[2] = RV().vCameraPosition.z;
    push.camPos[3] = cutoff;
    push.params[0] = jitterPhase;
    push.params[1] = ps_r_rtao_power;
    push.shape[0]  = 1.f;   // (1 - t/R)^k hit weight; 0 would be binary
    // The consumer applies pow(ao, SSAOPass::Strength() * r_ssao_strength) — the
    // shader writes pow(ao, 1/that) so the traced term arrives linear. Strength 0
    // means AO is off downstream; nothing to compensate.
    const float strength = SSAOPass::Strength() * ps_r_ssao_strength;
    push.shape[1]  = (strength > 0.01f) ? 1.f / strength : 1.f;
    // Back-face rejection needs a dependable winding: the render soup has one, the
    // collision soup does not (rt_shade.glsl h.back).
    push.shape[2]  = RTScene::UsingRenderGeometry() ? 1.f : 0.f;
    push.shape[3]  = dyn ? 1.f : 0.f;   // add instance mask 0x04 (the skeletons) to the ray

    const int zone = VK::Prof::ZoneBegin(cmd, "World/RTAO");
    const VkImageView views[2] = { aoView, ilView };
    RenderingBuilder rb(extent);
    for (VkImageView v : views) rb.Color(v, VK_ATTACHMENT_LOAD_OP_LOAD);
    rb.BeginPlain(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      ps_r_rtao_debug ? s_pipeOver : s_pipeMin);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &s_sets[slot], 0, nullptr);
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    VK::Prof::ZoneEnd(cmd, zone);
}

}}  // namespace VK::RTAOPass

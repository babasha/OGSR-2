// xrRenderVulkan - Snow MESH pass (VHM-style dense snow surface).
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_pass_snow.h"
#include "vk_scene_color.h"       // HDR scene target format
#include "vk_swapchain.h"         // depth format
#include "vk_shaders.h"           // g_ShaderManager
#include "vk_gfx_pipeline.h"      // VK::GfxPipelineBuilder
#include "vk_env_light.h"         // EnvLight::GetSetLayout / GetCurrentSet (lighting set)
#include "vk_deform.h"            // Deform::Ready (the press field this samples)
#include "vk_shadow.h"            // ShadowMap::RainEyeY (exact base-height reconstruction)
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"  // RV().vCameraPosition

extern int   ps_r_snow_mesh;          // r_snow_mesh — dense snow surface enable
extern int   ps_r_snow_deform;        // r_snow_deform — footprint deformation enable
extern int   ps_r_snow_deform_tex;    // r_snow_deform_tex — deform texture (required for the mesh)
extern float ps_r_snow;               // r_snow — TARGET snow coverage
extern float ps_r_snow_deform_depth;  // r_snow_deform_depth — dent depth (m)
extern float ps_r_snow_ripple;        // r_snow_ripple — wind-ripple relief strength

namespace VK {

namespace {
    VkPipeline       s_pipe       = VK_NULL_HANDLE;
    VkPipelineLayout s_pipeLayout = VK_NULL_HANDLE;
    VkShaderModule   s_vs = VK_NULL_HANDLE, s_fs = VK_NULL_HANDLE;
    bool             s_failed = false;

    constexpr float kHalf = 20.f;   // ±20 m dense snow ring around the player
    constexpr int   kN    = 256;    // grid dimension -> 0.157 m quads (footprint = ~3 quads)

    struct SnowPush {
        Fmatrix mvp;
        float   p0[4];   // center.x, center.z, half, N
        float   p1[4];   // maxDepth, eyeY, cellWorld, debug
        float   p2[4];   // ripple strength, unused...
    };
}

bool SnowMesh_Init()
{
    if (s_pipe) return true;
    if (s_failed) return false;

    if (!g_ShaderManager) { s_failed = true; return false; }
    s_vs = g_ShaderManager->Load("snow_mesh.vert.spv");
    s_fs = g_ShaderManager->Load("snow_mesh.frag.spv");
    if (s_vs == VK_NULL_HANDLE || s_fs == VK_NULL_HANDLE) {
        Msg("![VK SnowMesh] shader load failed"); s_failed = true; return false;
    }

    VkDescriptorSetLayout envLayout = EnvLight::GetSetLayout();
    if (envLayout == VK_NULL_HANDLE) { Msg("![VK SnowMesh] EnvLight layout not ready"); s_failed = true; return false; }

    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SnowPush) };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &envLayout;       // set 0 = EnvLight (ENV_SET 0 in the shaders)
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_pipeLayout) != VK_SUCCESS) {
        Msg("![VK SnowMesh] pipeline layout failed"); s_failed = true; return false;
    }

    // Procedural grid — no vertex input, the VS builds it from gl_VertexIndex.
    s_pipe = GfxPipelineBuilder(s_pipeLayout)
        .Vert(s_vs).Frag(s_fs)
        .Depth(true, true)
        .Color(VK::SceneColor::Format())
        .DepthTarget(Swapchain.m_DepthFormat)
        .Build("SnowMesh");
    if (s_pipe == VK_NULL_HANDLE) { s_failed = true; return false; }

    Msg("[VK SnowMesh] init OK (grid %dx%d, +-%.0f m, %.1f cm quads)", kN, kN, kHalf, 200.f * kHalf / float(kN - 1));
    return true;
}

void Pass_SnowMesh(FrameContext& ctx)
{
    if (!ps_r_snow_mesh || !ps_r_snow_deform || !ps_r_snow_deform_tex) return;
    if (ps_r_snow <= 0.f) return;
    if (!Deform::Ready()) return;
    if (!s_pipe && !SnowMesh_Init()) return;
    if (ctx.cmd == VK_NULL_HANDLE) return;
    VkDescriptorSet envSet = EnvLight::GetCurrentSet();
    if (envSet == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = ctx.cmd;

    // Snap the grid centre to a cell so vertices map to stable world XZ (no swimming
    // as the player walks; matches the deform texture's texel snap).
    const float cell = (2.f * kHalf) / float(kN - 1);
    const Fvector& cam = RV().vCameraPosition;
    SnowPush push{};
    push.mvp     = *ctx.viewProj;
    push.p0[0]   = floorf(cam.x / cell) * cell;
    push.p0[1]   = floorf(cam.z / cell) * cell;
    push.p0[2]   = kHalf;
    push.p0[3]   = float(kN);
    push.p1[0]   = ps_r_snow_deform_depth;
    push.p1[1]   = ShadowMap::RainEyeY();               // rain ortho eye-Y (exact terrain height)
    push.p1[2]   = cell;
    push.p1[3]   = (ps_r_snow_mesh >= 2) ? 1.f : 0.f;   // debug: flat magenta sheet
    push.p2[0]   = ps_r_snow_ripple;                    // wind-ripple relief strength
    push.p2[1]   = 0.f; push.p2[2] = 0.f; push.p2[3] = 0.f;

    BeginOverlayRendering(cmd, ctx, VK_ATTACHMENT_STORE_OP_STORE);   // LOAD color+depth, write depth
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeLayout, 0, 1, &envSet, 0, nullptr);
    vkCmdPushConstants(cmd, s_pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    const u32 verts = u32((kN - 1) * (kN - 1) * 6);
    vkCmdDraw(cmd, verts, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void SnowMesh_Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipe)       { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_pipeLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_pipeLayout, nullptr); s_pipeLayout = VK_NULL_HANDLE; }
    s_vs = s_fs = VK_NULL_HANDLE;
    s_failed = false;
}

}  // namespace VK

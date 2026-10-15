// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — RT indirect light, step 0 (the shadeable hit). See vk_pass_rtgi.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_pass_rtgi.h"
#include "vk_rt_scene.h"
#include "vk_rt_world.h"        // set 1 = the RT diffuse array (rt_shade.glsl samples it)
#include "vk_ddgi.h"            // probe atlases (modes 5/6) + the shared sun/sky
#include "vk_descriptors.h"     // VK::MakeDescriptorSets
#include "vk_shaders.h"         // g_ShaderManager
#include "vk_fullscreen.h"      // shared fullscreen pipeline skeleton
#include "vk_rendering.h"       // RenderingBuilder (LOAD, this overwrites the scene)
#include "vk_scene_color.h"     // SceneColor::Format — the HDR target this draws into
#include "vk_command_buffer.h"  // CommandManager.GetCurrentFrame()
#include "vk_pass_ssao.h"       // DeriveProjTerms — same camera basis GTAO/RTAO use
#include "vk_profiler.h"
#include "HW_Vulkan.h"

#include "../../xr_3da/device.h"

extern int   ps_r_rtgi_debug;        // 0 off, 1 shaded, 2 albedo, 3 normal, 4 distance
extern float ps_r_rtgi_debug_dist;   // primary-ray range (m), clamped to the bubble

namespace VK { namespace RTGIPass {

namespace {

constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;

bool s_inited = false;
bool s_failed = false;

VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
VkDescriptorSet       s_sets[kFramesInFlight] = {};
VkPipelineLayout      s_layout    = VK_NULL_HANDLE;
VkPipeline            s_pipe      = VK_NULL_HANDLE;

// What each set currently points at. A bubble rebuild swaps the TLAS AND both
// geometry buffers at once, and a set an in-flight command buffer still
// references must not be rewritten under it — so rewrite only on a real change
// (same rule as the RTAO pass; the slot retire stamps in vk_rt_scene are what
// keep the OLD buffers alive long enough for that to be safe).
struct Bound {
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkBuffer    verts  = VK_NULL_HANDLE;
    VkBuffer    mats   = VK_NULL_HANDLE;
    VkBuffer    alb    = VK_NULL_HANDLE;
    VkBuffer    consts = VK_NULL_HANDLE;
    VkImageView irr    = VK_NULL_HANDLE;
    VkImageView dep    = VK_NULL_HANDLE;
};
Bound s_bound[kFramesInFlight];

// ⚠ Mirrors the PC block in rtgi_debug.frag.glsl EXACTLY. 128 bytes is the
// guaranteed push-constant minimum and this uses all of it — one more field means
// moving to a UBO, not silently overflowing (std430 vs C++ packing has cost this
// renderer a session before).
struct RTGIDebugPush {
    float camDir[4];      // xyz = forward (unit), w = primary ray range (m)
    float camRightT[4];   // xyz = right * tan(fovX/2), w = tan(fovX/2)
    float camTopT[4];     // xyz = top   * tan(fovY/2), w = tan(fovY/2)
    float camPos[4];      // xyz = camera world position, w = shadow ray range (m)
    float res[4];         // xy = target size, zw = 1 / size
    float sunDir[4];      // xyz = direction TO the sun (unit)
    float sunColor[4];    // rgb, linear
    float skyColor[4];    // rgb linear, w = debug mode
};
static_assert(sizeof(RTGIDebugPush) == 128, "must match rtgi_debug.frag's PC block");

}   // anon namespace

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;

    if (!RTScene::Init())  { s_failed = true; return false; }   // no ray query on this device
    if (!g_ShaderManager)  { s_failed = true; return false; }

    VkShaderModule vs = g_ShaderManager->Load("tonemap.vert.spv");     // shared fullscreen triangle
    VkShaderModule fs = g_ShaderManager->Load("rtgi_debug.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        Msg("![VK RTGI] rtgi_debug.frag.spv missing — RT preview disabled");
        s_failed = true; return false;
    }

    // 0 = TLAS, 1 = vertex soup, 2 = per-tri material, 3 = albedo table,
    // 4 = DDGI constants, 5 = irradiance atlas, 6 = distance atlas.
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
                                kFramesInFlight, s_setLayout, s_pool, s_sets,
                                VK_SHADER_STAGE_FRAGMENT_BIT, "RTGI")) {
        s_failed = true; return false;
    }

    // Set 1 is the world's diffuse array. It belongs to vk_rt_world rather than to
    // this pass because the probe trace binds the SAME table — one publication of
    // 2048 descriptors serves both, and the two can never disagree about a slot.
    const VkDescriptorSetLayout texLayout = RTWorld::TexSetLayout();
    if (texLayout == VK_NULL_HANDLE) {
        Msg("![VK RTGI] no RT diffuse array (descriptor indexing?) — RT preview disabled");
        s_failed = true; return false;
    }
    const VkDescriptorSetLayout setLayouts[2] = { s_setLayout, texLayout };

    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, (u32)sizeof(RTGIDebugPush) };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 2; plci.pSetLayouts = setLayouts;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_layout) != VK_SUCCESS) {
        s_failed = true; return false;
    }

    s_pipe = Fullscreen::CreatePipeline(vs, fs, VK::SceneColor::Format(), s_layout,
                                        Fullscreen::OpaqueAttachment(), "RTGIdbg");
    if (s_pipe == VK_NULL_HANDLE) { s_failed = true; return false; }

    Msg("[VK RTGI] init OK (RT preview; r_rtgi_debug 1..4)");
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipe)      { vkDestroyPipeline(VulkanHW.m_Device, s_pipe, nullptr); s_pipe = VK_NULL_HANDLE; }
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    for (u32 i = 0; i < kFramesInFlight; ++i) { s_sets[i] = VK_NULL_HANDLE; s_bound[i] = Bound{}; }
    s_inited = false; s_failed = false;
}

void Execute(FrameContext& ctx)
{
    // Step the camera bubble. Deliberately BEFORE the r_rtgi_debug test and
    // outside any rendering scope: SSAO owns this call on a normal frame, but
    // SSAO can be off entirely, and then nothing else would ever build the
    // structure. RTScene guards itself against the double step by frame number.
    RTScene::Update(ctx.cmd, RV().vCameraPosition);

    if (ps_r_rtgi_debug <= 0) return;
    if (!Init()) return;

    const VkAccelerationStructureKHR tlas = RTScene::TLAS();
    const VkBuffer verts = RTScene::VertexBuffer();
    const VkBuffer mats  = RTScene::MaterialBuffer();
    const VkBuffer alb   = RTScene::AlbedoBuffer();
    // The shader statically uses the probe atlases in every mode, so they have to
    // be bound even when only modes 1..4 are asked for. DDGI keeps them alive
    // (and zeroed) for exactly that reason whenever either gate is on.
    const VkBuffer ddgiC   = DDGI::ConstsBuffer();
    const VkImageView irrV = DDGI::IrradianceView();
    const VkImageView depV = DDGI::DistanceView();
    const VkSampler   samp = DDGI::GetSampler();
    // The fragment shader samples the diffuse array unconditionally (the branch is
    // on the table's w, not on a specialization), so an unpublished set is fatal
    // to this draw, not a downgrade.
    const VkDescriptorSet texSet = RTWorld::TexSet();
    if (tlas == VK_NULL_HANDLE || verts == VK_NULL_HANDLE ||
        mats == VK_NULL_HANDLE || alb == VK_NULL_HANDLE || texSet == VK_NULL_HANDLE ||
        ddgiC == VK_NULL_HANDLE || irrV == VK_NULL_HANDLE ||
        depV == VK_NULL_HANDLE || samp == VK_NULL_HANDLE) {
        // Silence would read as "the shader is broken". Name the missing piece
        // once — each has a different cause (no bubble built yet / the material
        // upload failed / the material library was empty when the table was made).
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Msg("![VK RTGI] preview idle: tlas=%s verts=%s mats=%s albedo=%s tex=%s ddgi=%s (%u materials)",
                tlas ? "ok" : "MISSING", verts ? "ok" : "MISSING",
                mats ? "ok" : "MISSING", alb ? "ok" : "MISSING", texSet ? "ok" : "MISSING",
                (ddgiC && irrV && depV && samp) ? "ok" : "MISSING", RTScene::AlbedoCount());
        }
        return;
    }

    // A primary ray must not outlive the bubble: past its boundary a miss means
    // "nothing built", not "nothing there", and would paint sky over solid world.
    // For a box of half-extent h centred on the anchor, (h - drift) is a valid
    // lower bound on how far any interior ray can travel before leaving it.
    const float drift = RV().vCameraPosition.distance_to(RTScene::Anchor());
    const float range = _min(ps_r_rtgi_debug_dist, _max(0.f, RTScene::Coverage() - drift));
    if (range < 1.f) return;   // just after a level change / rebuild

    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;
    Bound want{ tlas, verts, mats, alb, ddgiC, irrV, depV };
    if (memcmp(&s_bound[slot], &want, sizeof(Bound)) != 0) {
        VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
        asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asInfo.accelerationStructureCount = 1;
        asInfo.pAccelerationStructures    = &tlas;

        const VkDescriptorBufferInfo bufs[4] = {
            { verts, 0, VK_WHOLE_SIZE }, { mats, 0, VK_WHOLE_SIZE }, { alb, 0, VK_WHOLE_SIZE },
            { ddgiC, 0, VK_WHOLE_SIZE } };
        const VkDescriptorType bufTypes[4] = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER };
        // The atlases live in GENERAL permanently (storage write + sampled read),
        // so that is the layout the descriptor must declare.
        const VkDescriptorImageInfo imgs[2] = {
            { samp, irrV, VK_IMAGE_LAYOUT_GENERAL }, { samp, depV, VK_IMAGE_LAYOUT_GENERAL } };

        VkWriteDescriptorSet w[7]{};
        for (u32 i = 0; i < 7; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s_sets[slot];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w[0].pNext = &asInfo;
        for (u32 i = 0; i < 4; ++i) {
            w[i + 1].descriptorType = bufTypes[i];
            w[i + 1].pBufferInfo    = &bufs[i];
        }
        for (u32 i = 0; i < 2; ++i) {
            w[i + 5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i + 5].pImageInfo     = &imgs[i];
        }
        vkUpdateDescriptorSets(VulkanHW.m_Device, 7, w, 0, nullptr);
        s_bound[slot] = want;
    }

    // Sun and sky from the ONE place that owns them for ray tracing. Sharing it
    // with the probe volume is the point: a preview lit differently from the field
    // it previews would send every mismatch hunting in the wrong file.
    Fvector toSun; Fvector3 sunC, skyC;
    DDGI::SunSky(toSun, sunC, skyC);

    const ProjTerms pt = DeriveProjTerms(RV().mFullTransform);

    RTGIDebugPush push{};
    push.camDir[0] = pt.dir.x; push.camDir[1] = pt.dir.y; push.camDir[2] = pt.dir.z;
    push.camDir[3] = range;
    push.camRightT[0] = pt.right.x * pt.tanX; push.camRightT[1] = pt.right.y * pt.tanX;
    push.camRightT[2] = pt.right.z * pt.tanX; push.camRightT[3] = pt.tanX;
    push.camTopT[0]   = pt.top.x * pt.tanY;   push.camTopT[1]   = pt.top.y * pt.tanY;
    push.camTopT[2]   = pt.top.z * pt.tanY;   push.camTopT[3]   = pt.tanY;
    push.camPos[0] = RV().vCameraPosition.x;
    push.camPos[1] = RV().vCameraPosition.y;
    push.camPos[2] = RV().vCameraPosition.z;
    push.camPos[3] = range;                       // shadow rays share the bubble's reach
    push.res[0] = float(ctx.extent.width);
    push.res[1] = float(ctx.extent.height);
    push.res[2] = 1.f / float(ctx.extent.width);
    push.res[3] = 1.f / float(ctx.extent.height);
    push.sunDir[0] = toSun.x; push.sunDir[1] = toSun.y; push.sunDir[2] = toSun.z;
    push.sunColor[0] = sunC.x; push.sunColor[1] = sunC.y; push.sunColor[2] = sunC.z;
    push.skyColor[0] = skyC.x; push.skyColor[1] = skyC.y; push.skyColor[2] = skyC.z;
    push.skyColor[3] = float(ps_r_rtgi_debug);

    const int zone = VK::Prof::ZoneBegin(ctx.cmd, "World/RTGIdbg");
    RenderingBuilder rb(ctx.extent);
    rb.Color(ctx.colorView, VK_ATTACHMENT_LOAD_OP_LOAD);
    rb.BeginPlain(ctx.cmd);

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    const VkDescriptorSet sets[2] = { s_sets[slot], texSet };
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(ctx.cmd, s_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(ctx.cmd);
    VK::Prof::ZoneEnd(ctx.cmd, zone);
}

}}  // namespace VK::RTGIPass

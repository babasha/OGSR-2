// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — capsule sun shadows for characters. See vk_pass_capsule_shadow.h.
#include "stdafx.h"
#include "vk_env_snapshot.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter / MakeDescriptorSets

// CKinematics — same compat trick as the skeleton wrapper TUs (see vk_pass_skinned.cpp): keep dxRender_Visual mapped
// to vkRender_Visual, and include the compat header BEFORE the real skeleton headers.
#define FBasicVisualH
#include "vk_FBasicVisual.h"    // pulls vk_SkeletonCompat.h (dxRender_Visual->vkRender_Visual, vk_Visual.h)
#include "CRender_Vulkan.h"     // RImplementation.b_loaded
#include "../xrRender/SkeletonCustom.h" // CKinematics — only skeletons are characters
#include "vk_rendering.h"       // VK::RenderingBuilder
#include "vk_pass_capsule_shadow.h"
#include "vk_swapchain.h"       // Swapchain.m_DepthView
#include "vk_framegraph.h"      // SceneDepthToSampled / SceneDepthEndSample
#include "vk_scene_color.h"     // HDR scene target format
#include "vk_shaders.h"         // g_ShaderManager
#include "vk_shadow.h"          // ShadowMap::GetSampler (the depth sampler the cone pass uses)
#include "vk_pass_ssao.h"       // DeriveProjTerms
#include "vk_command_buffer.h"  // CommandManager.GetCurrentFrame()
#include "vk_fullscreen.h"      // VK::Fullscreen — shared fullscreen pipeline
#include "vk_pass_world.h"      // DynamicVisuals() / DynVisual
#include "vk_pass_skinned.h"    // Skinned_CharacterBounds
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"
#include "../../xr_3da/IGame_Level.h"            // g_pGameLevel->ObjectSpace — "is the sun blocked for this one"
#include "../../xr_3da/IGame_Persistent.h"       // ES()
#include "../../xr_3da/Environment.h"            // CEnvDescriptorMixer: sun_dir, sun_color, ambient
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

extern int   ps_r_shadow_lite;      // r_shadow_lite — master of the R1-hybrid option
extern float ps_r_capsule_shadow;   // r_capsule_shadow — strength 0..1

namespace VK {
namespace {
    constexpr u32   kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
    constexpr u32   kPerDraw        = 5;      // capsules per fullscreen draw (push-constant budget)
    constexpr u32   kMaxCaps        = 15;     // three draws at most
    constexpr float kMaxDist        = 35.f;   // beyond this a character casts nothing
    constexpr float kSunRayLen      = 30.f;   // "sun blocked" probe length (m)

    bool                  s_inited    = false;
    bool                  s_failed    = false;
    VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;   // set 0: binding 0 = scene depth
    VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
    VkDescriptorSet       s_set[kFramesInFlight] = {};
    VkPipelineLayout      s_layout    = VK_NULL_HANDLE;
    VkPipeline            s_pipeline  = VK_NULL_HANDLE;

    struct CapPush {
        float cap[10][4];     // [2i] = (A.xyz, r), [2i+1] = (B.xyz, strength)
        float camPos[4];      // xyz, w = proj _33
        float camDir[4];      // xyz, w = proj _43
        float camRightT[4];
        float camTopT[4];
        float sun[4];         // xyz = to the sun, w = darkening
    };
    static_assert(sizeof(CapPush) == 240, "must match capsule_shadow.frag PC block (device max 256)");

    // Per-character state: how much of the sun reaches it, smoothed (the ray probe runs every few frames, the fade every frame).
    struct Track { float sunReach = 1.f; float target = 1.f; u32 probeFrame = 0; u32 seenFrame = 0; };
    std::unordered_map<const void*, Track> s_track;

    bool Init()
    {
        if (s_inited) return !s_failed;
        s_inited = true;
        if (!g_ShaderManager) { s_failed = true; return false; }
        VkShaderModule vs = g_ShaderManager->Load("sunshafts.vert.spv");      // shared fullscreen triangle
        VkShaderModule fs = g_ShaderManager->Load("capsule_shadow.frag.spv");
        if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
            Msg("![VK CapShadow] capsule_shadow.frag.spv missing — capsule shadows disabled");
            s_failed = true; return false;
        }
        constexpr auto kTex = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (!VK::MakeDescriptorSets({ kTex }, kFramesInFlight, s_setLayout, s_pool, s_set,
                                    VK_SHADER_STAGE_FRAGMENT_BIT, "CapShadow")) {
            s_failed = true; return false;
        }
        VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CapPush) };
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s_setLayout;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_layout) != VK_SUCCESS) {
            s_failed = true; return false;
        }
        // out = dst * (1 - a): the shader writes black with the shadow amount in alpha; scene alpha is left alone.
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable         = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp        = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp        = VK_BLEND_OP_ADD;
        blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        s_pipeline = Fullscreen::CreatePipeline(vs, fs, VK::SceneColor::Format(), s_layout, blend, "CapShadow");
        if (s_pipeline == VK_NULL_HANDLE) { s_failed = true; return false; }
        Msg("[VK CapShadow] init OK");
        return true;
    }

    struct Cap { Fvector A, B; float r; float strength; float dist; };
    float smooth01(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
}  // anon namespace

// RENDER_THREAD_PLAN step 8: results of CapsuleShadows_Extract, consumed by Pass_CapsuleShadows.
// Two-slot pipeline: per frame slot.
struct CapSlot { xr_vector<Cap> caps; Fvector toSun{}; float overall = 0.f; u32 tag = u32(-1); };
static RT::PerSlot<CapSlot> s_capSlots;

void CapsuleShadows_Extract()
{
    s_capSlots.cur().tag = u32(-1);
    Skinned_UploadBones();   // idempotent per frame: the character bounds below come from it
    if (!RImplementation.b_loaded) { s_track.clear(); return; }
    if (!ps_r_shadow_lite || ps_r_capsule_shadow <= 0.001f) return;
    if (DynamicVisuals().empty() || !g_pGamePersistent || !g_pGameLevel) return;

    // The sun: needs to be up, and to carry a real share of the light (overcast / dusk = no shadow to draw).
    const EnvDescSnap* E = ES().CurrentEnv;
    if (!E) return;
    Fvector toSun = E->sun_dir; toSun.mul(-1.f);
    if (toSun.magnitude() < 1e-4f) return;
    toSun.normalize();
    const float elev = smooth01(0.06f, 0.30f, toSun.y);
    if (elev <= 0.f) return;
    const float sunLum = (E->sun_color.x + E->sun_color.y + E->sun_color.z) * (1.f / 3.f);
    const float ambLum = (E->ambient.x + E->ambient.y + E->ambient.z) * (1.f / 3.f);
    const float sunShare = std::clamp(sunLum / (sunLum + ambLum + 1e-4f) * 1.4f, 0.f, 1.f);
    const float overall = std::clamp(ps_r_capsule_shadow, 0.f, 1.f) * elev * sunShare;
    if (overall < 0.01f) return;

    // ---- gather characters: capsule from the bounding box (model space -> world through the visual's matrix) ----
    xr_vector<Cap>& caps = s_capSlots.cur().caps;
    caps.clear();
    const Fvector eye = RV().vCameraPosition;
    const u32 frame = RV().frameIndex;
    for (const CharacterBounds& d : Skinned_CharacterBounds()) {   // skeletons only, bounds from the extract side
        if (!d.vis || d.noShadow) continue;
        const Fbox& bb = d.box;
        const float w = bb.max.x - bb.min.x, dp = bb.max.z - bb.min.z;
        if (w <= 0.05f || dp <= 0.05f || bb.max.y - bb.min.y < 0.4f) continue;
        const float cx = 0.5f * (bb.min.x + bb.max.x), cz = 0.5f * (bb.min.z + bb.max.z);
        Fvector bot{ cx, bb.min.y, cz }, top{ cx, bb.max.y, cz };
        d.xform.transform_tiny(bot);
        d.xform.transform_tiny(top);
        Fvector axis = top; axis.sub(bot);
        const float height = axis.magnitude();
        if (height < 0.4f) continue;
        axis.mul(1.f / height);
        const float r = std::clamp(0.5f * std::min(w, dp) * 0.7f, 0.17f, 0.5f);
        if (height < 2.f * r + 0.05f) continue;

        Fvector mid = bot; mid.mad(axis, height * 0.5f);
        const float dist = mid.distance_to(eye);
        if (dist > kMaxDist) continue;

        // Is the sun blocked by level geometry for this character (roof, wall, building)? The probe runs every ~8
        // frames per character; the result is faded in/out every frame so nothing pops. R1's ROS did the same thing.
        Track& tr = s_track[d.vis];
        if (tr.seenFrame == 0 || frame - tr.probeFrame >= 8) {
            tr.probeFrame = frame;
            Fvector from = mid; from.mad(toSun, 0.4f);
            const BOOL hit = g_pGameLevel->ObjectSpace.RayTest(from, toSun, kSunRayLen, collide::rqtStatic, nullptr, nullptr);
            tr.target = hit ? 0.f : 1.f;
            if (tr.seenFrame == 0) tr.sunReach = tr.target;
        }
        tr.seenFrame = frame;
        tr.sunReach += (tr.target - tr.sunReach) * 0.12f;
        const float strength = tr.sunReach * (1.f - smooth01(20.f, kMaxDist, dist));
        if (strength < 0.02f) continue;

        Cap c;
        c.A = bot; c.A.mad(axis, r);
        c.B = top; c.B.mad(axis, -r);
        c.r = r; c.strength = strength; c.dist = dist;
        caps.push_back(c);
    }
    // forget characters that have not been seen for a while (despawned / out of range)
    if ((frame & 127u) == 0) {
        for (auto it = s_track.begin(); it != s_track.end();)
            it = (frame - it->second.seenFrame > 240) ? s_track.erase(it) : std::next(it);
    }
    if (caps.empty()) return;
    if (caps.size() > kMaxCaps) {
        std::partial_sort(caps.begin(), caps.begin() + kMaxCaps, caps.end(), [](const Cap& a, const Cap& b) { return a.dist < b.dist; });
        caps.resize(kMaxCaps);
    }
    s_capSlots.cur().toSun = toSun; s_capSlots.cur().overall = overall; s_capSlots.cur().tag = frame;
}

void Pass_CapsuleShadows(FrameContext& ctx)
{
    if (ctx.cmd == VK_NULL_HANDLE) return;
    // Built by CapsuleShadows_Extract() (PREPARE phase) for THIS frame -- the ground-truth probes
    // (RayTest toward the sun) and the character bounds are extract-side work.
    if (s_capSlots.cur().tag != RV().frameIndex || s_capSlots.cur().caps.empty()) return;
    const Fvector toSun = s_capSlots.cur().toSun;
    const float overall = s_capSlots.cur().overall;
    const Fvector eye = RV().vCameraPosition;
    xr_vector<Cap>& caps = s_capSlots.cur().caps;
    if (!Init()) return;

    VkCommandBuffer cmd = ctx.cmd;
    SceneDepthToSampled(cmd);       // the shader reads the scene depth

    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;
    VK::DescriptorWriter(s_set[slot]).ImageSampler(0, Swapchain.m_DepthView, ShadowMap::GetSampler()).Flush();

    VK::RenderingBuilder(ctx.extent).Color(ctx.colorView).BeginPlain(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &s_set[slot], 0, nullptr);

    const ProjTerms pt = DeriveProjTerms(RV().mFullTransform);
    CapPush push{};
    push.camPos[0] = eye.x; push.camPos[1] = eye.y; push.camPos[2] = eye.z; push.camPos[3] = pt.p33;
    push.camDir[0] = pt.dir.x; push.camDir[1] = pt.dir.y; push.camDir[2] = pt.dir.z; push.camDir[3] = pt.p43;
    push.camRightT[0] = pt.right.x * pt.tanX; push.camRightT[1] = pt.right.y * pt.tanX; push.camRightT[2] = pt.right.z * pt.tanX;
    push.camTopT[0] = pt.top.x * pt.tanY; push.camTopT[1] = pt.top.y * pt.tanY; push.camTopT[2] = pt.top.z * pt.tanY;
    push.sun[0] = toSun.x; push.sun[1] = toSun.y; push.sun[2] = toSun.z; push.sun[3] = overall;

    for (size_t base = 0; base < caps.size(); base += kPerDraw) {
        std::memset(push.cap, 0, sizeof(push.cap));
        const size_t n = std::min<size_t>(kPerDraw, caps.size() - base);
        for (size_t k = 0; k < n; ++k) {
            const Cap& c = caps[base + k];
            push.cap[2 * k][0] = c.A.x; push.cap[2 * k][1] = c.A.y; push.cap[2 * k][2] = c.A.z; push.cap[2 * k][3] = c.r;
            push.cap[2 * k + 1][0] = c.B.x; push.cap[2 * k + 1][1] = c.B.y; push.cap[2 * k + 1][2] = c.B.z; push.cap[2 * k + 1][3] = c.strength;
        }
        vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    SceneDepthEndSample(cmd);
}

void CapsuleShadows_Destroy()
{
    s_track.clear();
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    if (s_pipeline)  { vkDestroyPipeline(VulkanHW.m_Device, s_pipeline, nullptr); s_pipeline = VK_NULL_HANDLE; }
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    for (u32 i = 0; i < kFramesInFlight; ++i) s_set[i] = VK_NULL_HANDLE;
    s_inited = false; s_failed = false;
}

}  // namespace VK

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — screen-space motion vectors. See vk_motionvec.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_profiler.h"           // TEMP VUID-hunt: VK::Prof::NameImage
#include "vk_rendering.h"          // VK::RenderingBuilder
#include "vk_motionvec.h"
#include "vk_pass_ssao.h"          // VK::DeriveProjTerms / VK::ProjTerms (shared depth→world basis)
#include "vk_descriptors.h"        // VK::DescriptorWriter
#include "vk_pass_context.h"       // VK::FrameContext (dynamic MV pass)
#include "vk_pass_skinned.h"       // VK::Skinned_RenderMotion (NPC animation MV)
#include "CRender_Vulkan.h"        // RImplementation (grass detail manager access)
#include "vk_DetailManager.h"      // CDetailManager::RenderMotion (grass wind-sway MV)
#include "vk_TreeManager.h"        // CTreeManager::RenderMotion (tree wind-sway MV)
#include "vk_swapchain.h"          // Swapchain.m_DepthView / m_Format
#include "vk_shaders.h"            // g_ShaderManager
#include "vk_barriers.h"           // ImageBarrier
#include "vk_framegraph.h"         // SceneDepthToSampled/ToAttachment — tracked scene-depth invariant
#include "vk_image.h"              // VK::CreateImage2D / CreateImageView
#include "vk_command_buffer.h"     // CommandManager.GetCurrentFrame()
#include "vk_fullscreen.h"         // VK::Fullscreen — shared fullscreen pipeline + draw
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"   // RV().mFullTransform + vCameraPosition

extern int   ps_r_motion_vectors;  // r_motion_vectors — global MV pass on/off
extern int   ps_r_mv_prepass;      // r_mv_prepass     — MV written by the depth prepass (no overlay re-raster)
extern int   ps_r_mv_debug;        // r_mv_debug       — false-colour overlay on/off
extern float ps_r_mv_debug_scale;  // r_mv_debug_scale — overlay magnitude scale
extern int   ps_r_profiler;        // r_profiler       — 2+ opens the MV/Trees sub-zone

namespace VK { namespace MotionVec {

namespace {
    constexpr u32      kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
    constexpr VkFormat kFormat = VK_FORMAT_R16G16_SFLOAT;

    bool                  s_inited  = false;
    bool                  s_failed  = false;

    VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;   // binding 0 = sampler2D (depth for MV / MV for debug)
    VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
    VkDescriptorSet       s_setMV [kFramesInFlight] = {}; // binds scene depth
    VkDescriptorSet       s_setDbg[kFramesInFlight] = {}; // binds the MV target
    VkSampler             s_samp      = VK_NULL_HANDLE;    // nearest/clamp

    VkPipelineLayout      s_mvLayout  = VK_NULL_HANDLE;    // set0 + 128B push (matrices)
    VkPipelineLayout      s_dbgLayout = VK_NULL_HANDLE;    // set0 + 16B push (scale)
    VkPipeline            s_pipeMV    = VK_NULL_HANDLE;
    VkPipeline            s_pipeDbg   = VK_NULL_HANDLE;

    // ---- prepass-written field (r_mv_prepass) ------------------------------
    // The sentinel is a value no real motion vector can reach: a vector is a UV
    // delta, so even a whole-screen sweep in one frame stays inside ±1. −1000 is
    // exact in fp16 (integers are exact to 2048 there), which matters — the
    // resolve separates "written" from "cleared" with ONE compare and nothing
    // else, so the cleared value must survive the format bit-for-bit.
    constexpr float       kSentinel     = -1000.0f;
    constexpr float       kSentinelTest =  -100.0f;   // written iff mv.x > this

    VkImage       s_preImg   = VK_NULL_HANDLE;        // the prepass RG16F field
    VmaAllocation s_preAlloc = VK_NULL_HANDLE;
    VkImageView   s_preView  = VK_NULL_HANDLE;
    bool          s_preFirst = true;                  // first transition is from UNDEFINED
    // Was the field actually handed to a prepass scope THIS frame? EndPrepassTarget
    // must not emit its COLOR_ATTACHMENT→SHADER_READ barrier otherwise: with the cvar
    // toggled off mid-run the image is already SHADER_READ, and a barrier claiming
    // the wrong oldLayout is a validation error, not a no-op.
    u32           s_preBegunFrame = u32(-1);
    VkDescriptorSetLayout s_preSetLayout = VK_NULL_HANDLE;   // b0 = depth, b1 = prepass field
    VkDescriptorPool      s_prePool      = VK_NULL_HANDLE;
    VkDescriptorSet       s_setPre[kFramesInFlight] = {};
    VkPipelineLayout      s_preLayout    = VK_NULL_HANDLE;
    VkPipeline            s_pipePre      = VK_NULL_HANDLE;
    bool                  s_preFailed    = false;     // resolve shader/pipeline missing → path stays off
    u32                   s_preResolvedFrame = u32(-1);   // RV().frameIndex of the last ResolveEarly

    // The RG16F motion target (full-res, one image — written + consumed within a frame).
    VkImage       s_img   = VK_NULL_HANDLE;
    VmaAllocation s_alloc = VK_NULL_HANDLE;
    VkImageView   s_view  = VK_NULL_HANDLE;
    VkExtent2D    s_extent = {};
    u32           s_generation = 0;
    bool          s_first  = true;     // first transition is from UNDEFINED

    // View-proj history. Both MV passes this frame (static fullscreen + dynamic
    // overlay) must use the SAME previous-frame matrix, so we roll it ONCE per
    // RV().frameIndex (the static Execute used to cache at its tail, which the later
    // dynamic pass would then read as the CURRENT frame — a one-frame bug).
    Fmatrix s_prevVP;        // previous frame's view-proj
    Fmatrix s_curVP;         // this frame's view-proj (becomes prev next frame)
    // Same history for the HUD-FOV projection (RV().mFullTransform_hud) — the HUD
    // weapon MV overlay reprojects its cur/prev poses with the HUD matrices, not the
    // world view-proj (different FOV + camera-at-origin).
    Fmatrix s_prevHudVP;
    Fmatrix s_curHudVP;
    bool    s_vpValid = false;
    u32     s_vpFrame = u32(-1);

    // UNJITTERED view-proj for THIS frame + the jitter that was applied to the raster
    // matrix (Option A). CRender::Begin sets these before it jitters RV().mFullTransform.
    // When DLSS is off, SetFrameVP still runs with the plain matrices and zero jitter →
    // behaviour is identical to the pre-jitter path.
    Fmatrix s_frameWorldVP;
    Fmatrix s_frameHudVP;
    float   s_frameJitX  = 0.0f, s_frameJitY = 0.0f;
    bool    s_frameVPSet = false;

    void EnsurePrevVP()
    {
        if (s_vpFrame == RV().frameIndex) return;   // already rolled this frame
        // Roll history from the UNJITTERED matrices (jitter-free MV). Fall back to the
        // live Device matrices if Begin hasn't set them yet (defensive; Begin always runs first).
        const Fmatrix& curWorld = s_frameVPSet ? s_frameWorldVP : RV().mFullTransform;
        const Fmatrix& curHud   = s_frameVPSet ? s_frameHudVP   : RV().mFullTransform_hud;
        s_prevVP    = s_vpValid ? s_curVP    : curWorld;       // first frame → zero motion
        s_curVP     = curWorld;
        s_prevHudVP = s_vpValid ? s_curHudVP : curHud;
        s_curHudVP  = curHud;
        s_vpValid   = true;
        s_vpFrame   = RV().frameIndex;
    }

    struct MVPush {
        float camDir[4];     // xyz fwd, w eye.x
        float camRightT[4];  // xyz right*tanX, w eye.y
        float camTopT[4];    // xyz top*tanY, w eye.z
        float zp[4];         // _33, _43, 1/w, 1/h
        float prevVP[16];    // row-major Fmatrix copied verbatim (see motion_vec.frag) — UNJITTERED
        float jitter[4];     // xy = this frame's sub-pixel jitter in D3D-NDC (unjitter the ndc); zw pad
    };
    static_assert(sizeof(MVPush) == 144, "must match motion_vec.frag PC block");

    struct DbgPush { float p[4]; };   // x = display scale

    void DestroyTarget()
    {
        if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
        if (s_view)  { vkDestroyImageView(VulkanHW.m_Device, s_view, nullptr); s_view = VK_NULL_HANDLE; }
        if (s_img)   { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_img, s_alloc); s_img = VK_NULL_HANDLE; s_alloc = VK_NULL_HANDLE; }
        if (s_preView) { vkDestroyImageView(VulkanHW.m_Device, s_preView, nullptr); s_preView = VK_NULL_HANDLE; }
        if (s_preImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_preImg, s_preAlloc); s_preImg = VK_NULL_HANDLE; s_preAlloc = VK_NULL_HANDLE; }
        s_extent = {};
        s_first  = true;
        s_preFirst = true;
    }

    // Reconstruction basis from the UNJITTERED cur view-proj (s_curVP, rolled by
    // EnsurePrevVP from CRender::Begin's pre-jitter capture). The depth was rendered
    // JITTERED, but the shaders unjitter the ndc (subtract push.jitter) so the ray is
    // built in the same unjittered space as this basis. p33/p43 are unaffected by the
    // sub-pixel jitter (it only translates clip.xy), so the depth decode is exact.
    // Shared by the fullscreen reconstruction and the prepass resolve — one basis,
    // so the two cannot disagree about where a static pixel was.
    MVPush BuildPush(VkExtent2D extent)
    {
        const ProjTerms pt = DeriveProjTerms(s_curVP);
        const Fvector eye  = RV().vCameraPosition;
        MVPush push{};
        push.camDir[0]    = pt.dir.x;             push.camDir[1]    = pt.dir.y;             push.camDir[2]    = pt.dir.z;             push.camDir[3]    = eye.x;
        push.camRightT[0] = pt.right.x * pt.tanX; push.camRightT[1] = pt.right.y * pt.tanX; push.camRightT[2] = pt.right.z * pt.tanX; push.camRightT[3] = eye.y;
        push.camTopT[0]   = pt.top.x   * pt.tanY; push.camTopT[1]   = pt.top.y   * pt.tanY; push.camTopT[2]   = pt.top.z   * pt.tanY; push.camTopT[3]   = eye.z;
        push.zp[0] = pt.p33;
        push.zp[1] = pt.p43;
        push.zp[2] = (extent.width  > 0) ? 1.0f / float(extent.width)  : 0.0f;
        push.zp[3] = (extent.height > 0) ? 1.0f / float(extent.height) : 0.0f;
        std::memcpy(push.prevVP, &s_prevVP, sizeof(push.prevVP));   // UNJITTERED prev VP
        push.jitter[0] = s_frameJitX; push.jitter[1] = s_frameJitY;  // unjitter the ndc (0 when DLSS off)
        push.jitter[2] = kSentinelTest;   // resolve only: "this texel was written" test
        push.jitter[3] = 0.0f;
        return push;
    }

    // Lazily build the resolve pipeline. Separate from Init() because it can fail
    // on its own (the .spv is new): a missing resolve must degrade to the shipping
    // overlay path, not take the whole MV pass down with it.
    bool EnsureResolvePipeline()
    {
        if (s_pipePre != VK_NULL_HANDLE) return true;
        if (s_preFailed || !g_ShaderManager) return false;

        VkShaderModule vs = g_ShaderManager->Load("tonemap.vert.spv");
        VkShaderModule fs = g_ShaderManager->Load("motion_vec_pre.frag.spv");
        if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
            Msg("![VK MotionVec] motion_vec_pre.frag.spv missing — r_mv_prepass disabled");
            s_preFailed = true; return false;
        }
        {
            VkDescriptorSet sets[kFramesInFlight]{};
            if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, kFramesInFlight,
                                        s_preSetLayout, s_prePool, sets,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, "MotionVecPre")) {
                s_preFailed = true; return false;
            }
            for (u32 i = 0; i < kFramesInFlight; ++i) s_setPre[i] = sets[i];
        }
        VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, (u32)sizeof(MVPush) };
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s_preSetLayout;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_preLayout) != VK_SUCCESS) {
            Msg("![VK MotionVec] resolve layout failed"); s_preFailed = true; return false;
        }
        s_pipePre = Fullscreen::CreatePipeline(vs, fs, kFormat, s_preLayout,
                                               Fullscreen::OpaqueAttachment(), "MotionVecResolve");
        if (s_pipePre == VK_NULL_HANDLE) { s_preFailed = true; return false; }
        Msg("[VK MotionVec] prepass resolve ready (r_mv_prepass %d)", ps_r_mv_prepass);
        return true;
    }
}

bool        Enabled()       { return s_inited && !s_failed && ps_r_motion_vectors != 0 && s_img != VK_NULL_HANDLE; }
void SetFrameVP(const Fmatrix& worldVP, const Fmatrix& hudVP, float jitterNdcX, float jitterNdcY)
{
    s_frameWorldVP = worldVP;
    s_frameHudVP   = hudVP;
    s_frameJitX    = jitterNdcX;
    s_frameJitY    = jitterNdcY;
    s_frameVPSet   = true;
}

VkImageView GetResultView() { return Enabled() ? s_view : VK_NULL_HANDLE; }
VkImage     GetResultImage(){ return Enabled() ? s_img  : VK_NULL_HANDLE; }
VkSampler   GetSampler()    { return s_samp; }
VkFormat    Format()        { return kFormat; }
u32         Generation()    { return s_generation; }

void EnsureSize(VkExtent2D extent)
{
    if (!s_inited || s_failed) return;
    if (s_img && extent.width == s_extent.width && extent.height == s_extent.height) return;
    DestroyTarget();
    if (extent.width == 0 || extent.height == 0) return;
    s_extent = extent;

    if (!VK::CreateImage2D(kFormat, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            s_img, s_alloc, "MotionVec")) {
        return;
    }
    s_view = VK::CreateImageView(s_img, kFormat);
    if (s_view == VK_NULL_HANDLE) {
        DestroyTarget(); return;
    }
    // The prepass field (r_mv_prepass). Allocated alongside the resolved target
    // rather than on first use: it is an attachment of the DEPTH PREPASS, the
    // earliest raster of the frame, and a lazy create there would have to happen
    // inside an open rendering scope.
    if (!VK::CreateImage2D(kFormat, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            s_preImg, s_preAlloc, "MotionVecPre")) {
        DestroyTarget(); return;
    }
    s_preView = VK::CreateImageView(s_preImg, kFormat);
    if (s_preView == VK_NULL_HANDLE) {
        DestroyTarget(); return;
    }
    ++s_generation;
    s_vpValid = false;   // history is stale across a resize
    Msg("[VK MotionVec] target ready (%ux%u RG16F, gen %u)", extent.width, extent.height, s_generation);
}

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;

    if (!g_ShaderManager) { s_failed = true; return false; }
    VkShaderModule vs  = g_ShaderManager->Load("tonemap.vert.spv");        // shared fullscreen triangle
    VkShaderModule fs  = g_ShaderManager->Load("motion_vec.frag.spv");
    VkShaderModule fsD = g_ShaderManager->Load("motion_vec_debug.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE || fsD == VK_NULL_HANDLE) {
        Msg("![VK MotionVec] motion_vec.{frag,debug}.spv missing — MV disabled");
        s_failed = true; return false;
    }

    // One binding (sampler2D), reused: MV pass binds depth, debug binds the MV target.
    const u32 nSets = kFramesInFlight * 2;
    {
        VkDescriptorSet sets[kFramesInFlight * 2]{};
        if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, nSets,
                                    s_setLayout, s_pool, sets,
                                    VK_SHADER_STAGE_FRAGMENT_BIT, "MotionVec")) {
            s_failed = true; return false;
        }
        for (u32 i = 0; i < kFramesInFlight; ++i) {
            s_setMV[i]  = sets[i];
            s_setDbg[i] = sets[kFramesInFlight + i];
        }
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_samp) != VK_SUCCESS) {
        Msg("![VK MotionVec] sampler create failed"); s_failed = true; return false;
    }

    auto makeLayout = [&](u32 pushSize, VkPipelineLayout& out) {
        VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize };
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s_setLayout;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        return vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &out) == VK_SUCCESS;
    };
    if (!makeLayout((u32)sizeof(MVPush), s_mvLayout) || !makeLayout((u32)sizeof(DbgPush), s_dbgLayout)) {
        s_failed = true; return false;
    }

    s_pipeMV  = Fullscreen::CreatePipeline(vs, fs,  kFormat,           s_mvLayout,  Fullscreen::OpaqueAttachment(), "MotionVec");
    s_pipeDbg = Fullscreen::CreatePipeline(vs, fsD, Swapchain.m_Format, s_dbgLayout, Fullscreen::OpaqueAttachment(), "MotionVecDebug");
    if (s_pipeMV == VK_NULL_HANDLE || s_pipeDbg == VK_NULL_HANDLE) { s_failed = true; return false; }

    Msg("[VK MotionVec] init OK (RG16F, depth reconstruction, r_motion_vectors %d)", ps_r_motion_vectors);
    return true;
}

void Execute(VkCommandBuffer cmd, VkExtent2D extent)
{
    if (!s_inited || s_failed || ps_r_motion_vectors == 0) return;
    EnsureSize(extent);
    if (s_img == VK_NULL_HANDLE) return;

    EnsurePrevVP();

    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;

    // (Re)bind the scene depth (its view can change on swapchain recreate).
    {
        VK::DescriptorWriter(s_setMV[slot])
            .ImageSampler(0, Swapchain.m_DepthView, s_samp).Flush();
    }

    const MVPush push = BuildPush(extent);

    const VkImageLayout oldL = s_first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ImageBarrier(cmd, s_img, oldL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    s_first = false;
    Fullscreen::DrawSimple(cmd, s_view, extent, s_pipeMV, s_mvLayout, s_setMV[slot], &push, sizeof(push));
    ImageBarrier(cmd, s_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ===========================================================================
// PREPASS-WRITTEN MOTION (r_mv_prepass) — see the header for the argument.
// ===========================================================================
bool PrepassWanted() { return Enabled() && ps_r_mv_prepass != 0 && !s_preFailed; }
bool ResolvedEarly() { return s_preResolvedFrame == RV().frameIndex; }

VkClearColorValue PrepassClearValue()
{
    VkClearColorValue c{};
    c.float32[0] = kSentinel; c.float32[1] = kSentinel;
    return c;
}

VkImageView BeginPrepassTarget(VkCommandBuffer cmd, VkExtent2D extent)
{
    if (!s_inited || s_failed || ps_r_motion_vectors == 0 || ps_r_mv_prepass == 0) return VK_NULL_HANDLE;
    // Build the resolve BEFORE handing the attachment out: writing a field nothing
    // can read back would only cost bandwidth and leave the overlay path skipped.
    if (!EnsureResolvePipeline()) return VK_NULL_HANDLE;
    EnsureSize(extent);
    if (s_preImg == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    // ⚠ HERE, not in ResolveEarly: the prepass draws that write motion (the canopy)
    // read CurVP()/PrevVP() while this scope is open, i.e. BEFORE the resolve. Roll
    // the pair now and everything downstream in the frame sees the same two matrices.
    EnsurePrevVP();

    const VkImageLayout oldL = s_preFirst ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ImageBarrier(cmd, s_preImg, oldL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    s_preFirst      = false;
    s_preBegunFrame = RV().frameIndex;
    return s_preView;
}

void EndPrepassTarget(VkCommandBuffer cmd)
{
    if (s_preImg == VK_NULL_HANDLE || s_preBegunFrame != RV().frameIndex) return;
    ImageBarrier(cmd, s_preImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void ResolveEarly(VkCommandBuffer cmd, VkExtent2D extent)
{
    if (!PrepassWanted() || s_preImg == VK_NULL_HANDLE || s_pipePre == VK_NULL_HANDLE) return;
    if (s_img == VK_NULL_HANDLE) return;

    // The history roll normally happens in the MV passes at the END of the frame;
    // this one runs at the START, so it has to ask for it — otherwise prevVP is
    // still the matrix pair as the PREVIOUS frame left it (a two-frame-old camera).
    EnsurePrevVP();

    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;
    VK::DescriptorWriter(s_setPre[slot])
        .ImageSampler(0, Swapchain.m_DepthView, s_samp)
        .ImageSampler(1, s_preView,             s_samp)
        .Flush();

    const MVPush push = BuildPush(extent);

    const VkImageLayout oldL = s_first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ImageBarrier(cmd, s_img, oldL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    s_first = false;
    Fullscreen::DrawSimple(cmd, s_view, extent, s_pipePre, s_preLayout, s_setPre[slot], &push, sizeof(push));
    ImageBarrier(cmd, s_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    s_preResolvedFrame = RV().frameIndex;
}

void ExecuteDynamic(VkCommandBuffer cmd, const FrameContext& ctx)
{
    // The WHOLE motion pass, run after all opaque geometry (registered after LODs):
    //   1) fullscreen depth reconstruction → camera motion for EVERYTHING now in the
    //      depth (statics + TREES (rigid → exact) + GRASS (camera part) + NPCs) + sky.
    //   2) skinned NPC overlay → replaces the camera-only NPC motion with the true
    //      animated motion (camera + skeletal pose + body travel).
    // Doing (1) here (not in the World pass) is what gives trees/grass correct motion
    // instead of the background's — at World time they aren't in the depth yet.
    if (!Enabled() || s_img == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE || ctx.depthView == VK_NULL_HANDLE) return;
    EnsurePrevVP();

    // r_mv_prepass: the reconstruction ALREADY ran, right after the depth prepass, and
    // the field it produced is what SSAO consumed. Re-running it here would overwrite
    // the dynamics the prepass rasterised with the camera-only answer — the exact bug
    // the path exists to remove. What still has to be drawn are the overlays for
    // geometry that is NOT in the prepass depth: the HUD weapon (forward-only, near
    // depth range) and the grass (drawn after the world pass entirely).
    const bool early = ResolvedEarly();
    if (!early) {
        // 1) Camera/static/tree/grass MV from the complete opaque depth (needs SHADER_READ).
        // Routed through g_FrameGraph.Depth() rather than a hand-placed ImageBarrier:
        // the tracker's src scope is what actually touched the depth (the LODs pass
        // wrote it) instead of the old layout's canonical access, and the frame-wide
        // layout stays something the graph KNOWS rather than something this pass
        // promises. The restore below is what the overlay's own depth attachment
        // (and every pass after us) needs. See vk_framegraph.h.
        SceneDepthToSampled(cmd);   // fragment-only: DrawSimple samples it in the FS
        Execute(cmd, ctx.extent);   // fullscreen reconstruction (owns its MV-target barriers)
        SceneDepthToAttachment(cmd);
    }

    // 2) Skinned NPC animation overlay — depth-test (LEQUAL, no write) against the
    //    same complete depth so only visible NPC pixels overwrite the camera field.
    ImageBarrier(cmd, s_img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Colour LOAD keeps the static camera field; depth is the complete opaque
    // depth (post-LODs), LOADed for the test only — the pipeline has no write.
    // BeginFlipped's negative-height viewport MUST match the forward/prepass
    // raster so the NPC lands on the same pixels and its depth bit-matches
    // (LEQUAL passes on equal).
    RenderingBuilder(s_extent)
        .Color(s_view)
        .Depth(ctx.depthView, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE)
        .BeginFlipped(cmd);

    // Jitter-free MV (Option A): overlays reproject with the UNJITTERED cur/prev VP
    // (s_curVP / s_prevVP, rolled from CRender::Begin's pre-jitter capture) and
    // re-apply this frame's jitter only to gl_Position, so their depth still
    // bit-matches the jittered forward geometry while the MV carries no jitter wobble.
    // r_mv_prepass: the near NPCs already wrote their motion in the prepass, in the
    // raster that laid their depth. What is left is the ones the prepass never draws —
    // beyond its AO-occluder range they are not in that depth at all, so without this
    // half their pixels would carry the motion of whatever the resolve found behind them.
    const Fmatrix& curVP = s_curVP;
    Skinned_RenderMotion(cmd, curVP, s_prevVP, s_frameJitX, s_frameJitY,
                         early ? Skinned_PrepassRange() : 0.0f);


    // First-person HUD weapon: its own overlay with the HUD-FOV projection at the
    // near-depth range [0,0.02] (matches the forward HUD pass, drawn earlier in
    // Pass_World → its depth is already in ctx.depthView). Replaces the fullscreen
    // pass's bogus camera-reproject MV on the viewmodel so it doesn't ghost.
    Skinned_RenderMotionHud(cmd, s_extent, s_curHudVP, s_prevHudVP, s_frameJitX, s_frameJitY);

    // Grass wind-sway MV: re-project the cur/prev wind pose so blades swaying in the
    // wind carry their true screen motion (the fullscreen reconstruction above only
    // saw camera motion). Same open render pass; depth-tested against the grass depth.
    if (RImplementation.Details && RImplementation.Details->IsLoaded())
        RImplementation.Details->RenderMotion(ctx, curVP, s_prevVP, s_frameJitX, s_frameJitY);

    // Tree wind-sway MV: trees sway (SSFX trunk + crown flutter) → same overlay as
    // grass so their crowns don't ghost on wind. Same open render pass.
    // ⛔ Not under r_mv_prepass: the canopy already wrote this field in the prepass,
    // in the raster that laid its depth. THIS is the raster that path deletes.
    if (!early && RImplementation.Trees && RImplementation.Trees->IsReady()) {
        // The canopy's THIRD raster of the frame. Measured once (12-08) at 1.35 ms and
        // never given a zone — it lived inside MotionVecDyn with the skinned meshes and
        // the fullscreen reconstruction, so an A/B on trees could not see it move.
        // ⚠ ::ps_r_profiler, qualified. This file is inside namespace VK::MotionVec, and
        // a bare `extern int` here declares VK::MotionVec::ps_r_profiler — a symbol
        // nothing defines. It compiles clean and fails at LINK, which in this tree also
        // DELETES the previous exe, so the cost of the shortcut is a full rebuild.
        const int zMV = (::ps_r_profiler >= 2) ? VK::Prof::ZoneBegin(cmd, "MV/Trees") : -1;
        RImplementation.Trees->StatsBeginPass(cmd, VK::TP_MOTION);
        RImplementation.Trees->RenderMotion(ctx, curVP, s_prevVP, s_frameJitX, s_frameJitY);
        RImplementation.Trees->StatsEndPass(cmd, VK::TP_MOTION);
        if (zMV >= 0) VK::Prof::ZoneEnd(cmd, zMV);
    }

    vkCmdEndRendering(cmd);
    ImageBarrier(cmd, s_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

const Fmatrix& CurVP()  { return s_curVP;  }
const Fmatrix& PrevVP() { return s_prevVP; }
void EnsureHistory()    { EnsurePrevVP(); }
void FrameJitter(float& ndcX, float& ndcY) { ndcX = s_frameJitX; ndcY = s_frameJitY; }

void DrawDebugOverlay(VkCommandBuffer cmd, VkImageView dstView, VkExtent2D extent)
{
    if (!s_inited || s_failed || ps_r_mv_debug == 0 || s_img == VK_NULL_HANDLE || dstView == VK_NULL_HANDLE) return;

    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;
    {
        VK::DescriptorWriter(s_setDbg[slot]).ImageSampler(0, s_view, s_samp).Flush();
    }
    DbgPush dp{};
    dp.p[0] = (ps_r_mv_debug_scale > 0.0f) ? ps_r_mv_debug_scale : 30.0f;
    Fullscreen::DrawSimple(cmd, dstView, extent, s_pipeDbg, s_dbgLayout, s_setDbg[slot], &dp, sizeof(dp));
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    DestroyTarget();
    if (s_pipePre)      { vkDestroyPipeline(VulkanHW.m_Device, s_pipePre, nullptr); s_pipePre = VK_NULL_HANDLE; }
    if (s_preLayout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_preLayout, nullptr); s_preLayout = VK_NULL_HANDLE; }
    if (s_prePool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_prePool, nullptr); s_prePool = VK_NULL_HANDLE; }
    if (s_preSetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_preSetLayout, nullptr); s_preSetLayout = VK_NULL_HANDLE; }
    for (u32 i = 0; i < kFramesInFlight; ++i) s_setPre[i] = VK_NULL_HANDLE;
    s_preFailed = false; s_preResolvedFrame = u32(-1); s_preBegunFrame = u32(-1);
    if (s_pipeMV)    { vkDestroyPipeline(VulkanHW.m_Device, s_pipeMV, nullptr); s_pipeMV = VK_NULL_HANDLE; }
    if (s_pipeDbg)   { vkDestroyPipeline(VulkanHW.m_Device, s_pipeDbg, nullptr); s_pipeDbg = VK_NULL_HANDLE; }
    if (s_mvLayout)  { vkDestroyPipelineLayout(VulkanHW.m_Device, s_mvLayout, nullptr); s_mvLayout = VK_NULL_HANDLE; }
    if (s_dbgLayout) { vkDestroyPipelineLayout(VulkanHW.m_Device, s_dbgLayout, nullptr); s_dbgLayout = VK_NULL_HANDLE; }
    if (s_samp)      { vkDestroySampler(VulkanHW.m_Device, s_samp, nullptr); s_samp = VK_NULL_HANDLE; }
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    for (u32 i = 0; i < kFramesInFlight; ++i) { s_setMV[i] = VK_NULL_HANDLE; s_setDbg[i] = VK_NULL_HANDLE; }
    s_inited = false; s_failed = false; s_generation = 0; s_vpValid = false; s_vpFrame = u32(-1);
}

}}  // namespace VK::MotionVec

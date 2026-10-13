// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — GTAO pass. See vk_pass_ssao.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_descriptors.h"       // VK::DescriptorWriter
#include "vk_pass_ssao.h"
#include "vk_pass_gather.h"     // GatherPass::WantsPrevColor — SSIL is no longer the only reader of the history
#include "vk_swapchain.h"          // Swapchain.m_DepthView (prepass depth)
#include "vk_shaders.h"            // g_ShaderManager
#include "vk_pipeline_cache.h"     // PipelineCache::GetCacheObject
#include "vk_barriers.h"           // ImageBarrier
#include "vk_image.h"              // VK::CreateImage2D / CreateImageView
#include "vk_buffer.h"             // CVulkanBuffer (debug AO readback)
#include "vk_command_buffer.h"     // CommandManager.GetCurrentFrame()
#include "vk_fullscreen.h"         // VK::Fullscreen — shared fullscreen pipeline + draw
#include "vk_profiler.h"           // VK::Prof::NameImage — TEMP VUID-hunt instrumentation
#include "vk_motionvec.h"          // VK::MotionVec — temporal reprojection of the IL/AO history
#include "vk_pass_rtao.h"          // VK::RTAOPass — hardware-traced AO min-blended over the GTAO result
#include "vk_rt_dyn.h"             // VK::RTDyn::Update — per-frame skeleton BLAS/TLAS, built before RTAO
#include "vk_rt_scene.h"           // VK::RTScene — the camera-local BLAS it traces against
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"   // Device camera basis + mProject

extern u32 ps_r_ao_quality;        // r2_ssao token (0 off / 1 low / 2 med / 3 high / 4 ultra)
extern int ps_r_ssao_enable;       // r_ssao — global GTAO on/off (GLOBAL scope: block-scope extern inside the namespace would mangle as VK::SSAOPass::* → LNK2001)
extern int ps_r_ssao_debug;        // r_ssao_debug — also enables the readback stats below
extern int   ps_r_ssil_enable;     // r_ssil — fold-in SSIL on/off (gates the prev-colour taps in the horizon march)
extern float ps_r_ssil_strength;   // r_ssil_strength — baked into the IL output (forward receivers apply a fixed ssilBoost)
extern float ps_r_ssil_temporal;   // r_ssil_temporal — GTAO temporal accumulation α (0 = off; per-frame jitter + MV-reprojected EMA)
extern float ps_r_ssao_temporal;   // r_ssao_temporal — same temporal accumulation as a first-class AO control (works without r_ssil)
extern float ps_r_ssao_bias;       // r_ssao_bias — grazing-angle N·V fade of the AO result (flat grazing floor → fully open)
extern int   ps_r_ssao_reproj;     // r_ssao_reproj — 1 = history reprojected from depth+prevVP in the blur (this frame); 0 = legacy MV target (one frame stale)

namespace VK {

ProjTerms DeriveProjTerms(const Fmatrix& M)
{
    ProjTerms t{};
    // w column (M_i4, i=1..3) = view dir scaled — pick the largest component's
    // row for the _33 division so a near-axis-aligned camera stays stable.
    const float w[3] = { M._14, M._24, M._34 };
    const float z[3] = { M._13, M._23, M._33 };
    int bi = 0;
    if (std::fabs(w[1]) > std::fabs(w[bi])) bi = 1;
    if (std::fabs(w[2]) > std::fabs(w[bi])) bi = 2;
    t.p33 = (std::fabs(w[bi]) > 1e-9f) ? z[bi] / w[bi] : 1.f;
    t.p43 = M._43 - M._44 * t.p33;
    const float p11 = std::sqrt(M._11 * M._11 + M._21 * M._21 + M._31 * M._31);
    const float p22 = std::sqrt(M._12 * M._12 + M._22 * M._22 + M._32 * M._32);
    t.tanX = (p11 > 1e-9f) ? 1.f / p11 : 1.f;
    t.tanY = (p22 > 1e-9f) ? 1.f / p22 : 1.f;
    t.dir.set(w[0], w[1], w[2]);          t.dir.normalize_safe();
    t.right.set(M._11, M._21, M._31);     t.right.normalize_safe();
    t.top.set(M._12, M._22, M._32);       t.top.normalize_safe();
    return t;
}

namespace SSAOPass {

namespace {
    constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;

    // R4 gtao.h: GTAO_RADIUS 4 (falloff range; effective horizon-march radius
    // is half that). Strength multiplies the occlusion in the receivers.
    constexpr float kRadius   = 4.0f;
    constexpr float kStrength = 1.0f;

    bool                  s_inited    = false;
    bool                  s_failed    = false;
    VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;   // 0 = depth, 1 = raw AO (blur only)
    VkDescriptorPool      s_pool      = VK_NULL_HANDLE;
    VkDescriptorSet       s_setGtao[kFramesInFlight] = {};
    VkDescriptorSet       s_setBlur[kFramesInFlight] = {};
    VkPipelineLayout      s_layout    = VK_NULL_HANDLE;
    VkPipeline            s_pipeGtao  = VK_NULL_HANDLE;
    VkPipeline            s_pipeBlur  = VK_NULL_HANDLE;
    VkSampler             s_sampNear  = VK_NULL_HANDLE;   // nearest/clamp — depth + raw AO taps
    VkSampler             s_sampLin   = VK_NULL_HANDLE;   // linear/clamp — receivers upsample

    // Half-res ping-pong: [0] = final (blurred), [1] = raw GTAO.
    // [1] is R16F (AO only). [0] is RG16F: r = AO the receivers sample, g = the
    // pixel's VIEW DEPTH — it rides into the temporal history so next frame's
    // blur can tell "same surface, moved on screen" from "a different surface
    // is now here" (disocclusion) and drop the stale sample instead of blending it.
    constexpr VkFormat kAORawFormat   = VK_FORMAT_R16_SFLOAT;
    constexpr VkFormat kAOFinalFormat = VK_FORMAT_R16G16_SFLOAT;
    VkImage       s_img[2]   = {};
    VmaAllocation s_alloc[2] = {};
    VkImageView   s_view[2]  = {};

    // SSIL (folded into the GTAO horizon march, MRT target 1). Same half-res
    // ping-pong as AO ([0] = final/blurred IL, [1] = raw IL). RGBA16F (rgb = HDR
    // indirect radiance). Plus a persistent half-res PREV-frame colour buffer the
    // horizon gather samples (captured from the composited scene each frame).
    constexpr VkFormat kILFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr float    kILFireClamp = 6.0f;   // per-sample radiance clamp → no single specular pixel dominates
    VkImage       s_ilImg[2]   = {};
    VmaAllocation s_ilAlloc[2] = {};
    VkImageView   s_ilView[2]  = {};
    VkImage       s_prevColor      = VK_NULL_HANDLE;   // half-res linear-HDR history (prev frame's lit scene)
    VmaAllocation s_prevColorAlloc = VK_NULL_HANDLE;
    VkImageView   s_prevColorView  = VK_NULL_HANDLE;
    bool          s_prevColorCleared = false;          // false → first Execute clears it to black (valid SHADER_READ)

    // TEMPORAL (r_ssil_temporal): persistent half-res copies of the PREVIOUS frame's
    // FINAL AO and IL. The blur reprojects them through the motion vectors and EMA-
    // blends — the per-frame-jittered gather averages into a band-free result. AO
    // history is R16F (matches s_img[0]); IL history is kILFormat (s_ilImg[0]).
    VkImage       s_aoHist      = VK_NULL_HANDLE;
    VmaAllocation s_aoHistAlloc = VK_NULL_HANDLE;
    VkImageView   s_aoHistView  = VK_NULL_HANDLE;
    VkImage       s_ilHist      = VK_NULL_HANDLE;
    VmaAllocation s_ilHistAlloc = VK_NULL_HANDLE;
    VkImageView   s_ilHistView  = VK_NULL_HANDLE;
    bool          s_histCleared = false;   // false → first Execute clears both history buffers to a valid SHADER_READ black
    bool          s_histValid   = false;   // a fresh final result was copied in last frame → safe to reproject
    bool          s_temporalWasOn = false; // tracks the cvar edge so off→on starts clean (no stale-history blend)
    u32           s_frame       = 0;       // monotonic frame counter → per-frame jitter phase
    u32           s_mvWarm      = 0;       // frames MV has been enabled+present → only reproject once it has surely rendered (SHADER_READ)

    // Temporal reprojection constants (r_ssao_reproj 1): the blur rebuilds each
    // pixel's world position from the prepass depth and projects it with the
    // PREVIOUS frame's view-proj — the same arithmetic motion_vec.frag does, but
    // done here, THIS frame, for THIS depth. A 4×4 does not fit next to the 112-byte
    // push (128 is the guaranteed minimum), so it travels in a small per-slot UBO
    // (binding 8), persistently mapped, one 256-aligned slot per frame in flight.
    struct TemporalUBO {
        float prevVP[16];   // previous frame's UNJITTERED view-proj (row-major Fmatrix → GLSL reads the transpose, like motion_vec.frag)
        float eye[4];       // xyz = camera position; w unused
        float jitter[4];    // xy = this frame's sub-pixel jitter in D3D-NDC (unjitter the pixel before reconstructing); zw unused
    };
    static_assert(sizeof(TemporalUBO) == 96, "must match ssao_blur.frag TemporalUBO block");
    CVulkanBuffer s_tempUbo;
    u8*           s_tempUboMap    = nullptr;
    VkDeviceSize  s_tempUboStride = 0;

    // FULL-res NPC normal G-buffer (skinned pass writes worldN*0.5+0.5, a=valid;
    // GTAO uses it where a=1, else falls back to depth-derived normals). RGBA8.
    constexpr VkFormat kNormalFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage       s_normImg   = VK_NULL_HANDLE;
    VmaAllocation s_normAlloc  = VK_NULL_HANDLE;
    VkImageView   s_normView   = VK_NULL_HANDLE;
    VkExtent2D    s_normExtent  = {};
    VkExtent2D    s_extent   = {};
    u32           s_generation = 0;
    bool          s_first[2] = { true, true };

    // Debug readback (active while r_ssao_debug != 0): every ~300 frames the
    // final AO image is copied to this host buffer and min/max/avg logged —
    // verifies the GTAO output end-to-end without a GPU capture tool.
    CVulkanBuffer s_dbgBuf;
    u16*          s_dbgMap = nullptr;     // R16_SFLOAT texels (half-float)
    int           s_dbgCountdown = -1;   // frames until the mapped copy is safely written
    u32           s_dbgCooldown  = 0;

    // Decode an IEEE-754 half (one R16_SFLOAT AO texel) onto the legacy 0..255
    // scale so the debug log reads the same as it did under R8_UNORM.
    u8 AoHalfToU8(u16 h)
    {
        const u32 exp = (h >> 10) & 0x1F;
        const u32 man = h & 0x3FF;
        float v;
        if (exp == 0)         v = man * (1.0f / 16777216.0f);             // subnormal ~0
        else if (exp == 0x1F) v = 1.0f;                                   // inf/nan → clamp
        else                  v = ldexpf(1.0f + man / 1024.0f, int(exp) - 15);
        if (h & 0x8000) v = 0.0f;                                         // AO is never negative
        if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
        return u8(v * 255.0f + 0.5f);
    }

    struct SSAOPush {
        float camDir[4];
        float camRightT[4];   // xyz = right * tan(fovX/2), w = tan(fovX/2)
        float camTopT[4];     // xyz = top   * tan(fovY/2), w = tan(fovY/2)
        float zp[4];          // proj _33, proj _43, radius, samples/side
        float res[4];         // AO size xy, 1/AO size zw
        float dbg[4];         // x = r_ssao_debug mode (2 = depth, 3 = normal); y = SSIL on; z = SSIL firefly clamp; w = SSIL strength
        float temporal[4];    // x = EMA α (0 = off); y = per-frame jitter phase [0,1); z = MV valid; w = history valid
    };
    static_assert(sizeof(SSAOPush) == 112, "must match ssao.frag / ssao_blur.frag PC blocks");

    u32 SampleCount()
    {
        // r2_ssao token → GTAO_SAMPLE (R4: low 2 / medium 3 / high+ 4). The
        // user's carried-over user.ltx may have it off — the Vulkan renderer
        // defaults to medium then (set r2_ssao to override; st_opt_off is
        // honoured once any other value was chosen at least once... for now
        // 0 == "unset" → medium, since legacy configs predate this pass).
        // Unset (legacy user.ltx carries r2_ssao off = 0) now defaults to LOW = 2
        // samples (was medium = 3): with temporal accumulation default ON
        // (r_ssao_temporal) 2 samples is band-free + measured ~33% cheaper than 4
        // (RTX 5070: SSAO 2.04ms→1.36ms; the real win is the bandwidth-bound iGPU).
        // Explicit r2_ssao st_opt_med/high still forces 3/4 for users who want it.
        const u32 q = ps_r_ao_quality ? ps_r_ao_quality : 1;
        return q <= 1 ? 2 : (q == 2 ? 3 : 4);
    }

    void DestroyRTs()
    {
        if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
        for (u32 i = 0; i < 2; ++i) {
            if (s_view[i]) { vkDestroyImageView(VulkanHW.m_Device, s_view[i], nullptr); s_view[i] = VK_NULL_HANDLE; }
            if (s_img[i])  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_img[i], s_alloc[i]); s_img[i] = VK_NULL_HANDLE; s_alloc[i] = VK_NULL_HANDLE; }
            s_first[i] = true;
        }
        if (s_normView) { vkDestroyImageView(VulkanHW.m_Device, s_normView, nullptr); s_normView = VK_NULL_HANDLE; }
        if (s_normImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_normImg, s_normAlloc); s_normImg = VK_NULL_HANDLE; s_normAlloc = VK_NULL_HANDLE; }
        for (u32 i = 0; i < 2; ++i) {
            if (s_ilView[i]) { vkDestroyImageView(VulkanHW.m_Device, s_ilView[i], nullptr); s_ilView[i] = VK_NULL_HANDLE; }
            if (s_ilImg[i])  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_ilImg[i], s_ilAlloc[i]); s_ilImg[i] = VK_NULL_HANDLE; s_ilAlloc[i] = VK_NULL_HANDLE; }
        }
        if (s_prevColorView) { vkDestroyImageView(VulkanHW.m_Device, s_prevColorView, nullptr); s_prevColorView = VK_NULL_HANDLE; }
        if (s_prevColor)     { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_prevColor, s_prevColorAlloc); s_prevColor = VK_NULL_HANDLE; s_prevColorAlloc = VK_NULL_HANDLE; }
        s_prevColorCleared = false;
        if (s_aoHistView) { vkDestroyImageView(VulkanHW.m_Device, s_aoHistView, nullptr); s_aoHistView = VK_NULL_HANDLE; }
        if (s_aoHist)     { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_aoHist, s_aoHistAlloc); s_aoHist = VK_NULL_HANDLE; s_aoHistAlloc = VK_NULL_HANDLE; }
        if (s_ilHistView) { vkDestroyImageView(VulkanHW.m_Device, s_ilHistView, nullptr); s_ilHistView = VK_NULL_HANDLE; }
        if (s_ilHist)     { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_ilHist, s_ilHistAlloc); s_ilHist = VK_NULL_HANDLE; s_ilHistAlloc = VK_NULL_HANDLE; }
        s_histCleared = false; s_histValid = false;
        s_normExtent = {};
        s_extent = {};
    }

    bool EnsureRTs(VkExtent2D sceneExtent)
    {
        VkExtent2D want{ sceneExtent.width  / 2 ? sceneExtent.width  / 2 : 1,
                         sceneExtent.height / 2 ? sceneExtent.height / 2 : 1 };
        if (s_img[0] && want.width == s_extent.width && want.height == s_extent.height)
            return true;
        DestroyRTs();
        s_extent = want;
        // AO only (R16F kills R8 contouring; bent normal retired → 4× less bandwidth
        // than the old RGBA16F). The FINAL [0] carries view depth in .g for the
        // temporal history (RG16F); the raw [1] stays R16F.
        const char* const kAoName[2] = { "SSAO.AO0", "SSAO.AO1" };
        const VkFormat    kAoFmt[2]  = { kAOFinalFormat, kAORawFormat };
        for (u32 i = 0; i < 2; ++i) {
            if (!VK::CreateImage2D(kAoFmt[i], want,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,   // debug readback
                    s_img[i], s_alloc[i], kAoName[i]))
                return false;
            s_view[i] = VK::CreateImageView(s_img[i], kAoFmt[i]);
            if (s_view[i] == VK_NULL_HANDLE) return false;
        }
        // SSIL ping-pong (half-res RGBA16F, MRT target 1 alongside AO). [0]=final, [1]=raw.
        const char* const kIlName[2] = { "SSAO.IL0", "SSAO.IL1" };
        for (u32 i = 0; i < 2; ++i) {
            if (!VK::CreateImage2D(kILFormat, want,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,   // [0] copied into the temporal IL history
                    s_ilImg[i], s_ilAlloc[i], kIlName[i]))
                return false;
            s_ilView[i] = VK::CreateImageView(s_ilImg[i], kILFormat);
            if (s_ilView[i] == VK_NULL_HANDLE) return false;
        }
        // Persistent half-res PREV-frame colour (linear HDR) — the IL gather source.
        // blit dst (capture) + sampled (gather). Cleared to black on first Execute.
        {
            if (!VK::CreateImage2D(kILFormat, want,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    s_prevColor, s_prevColorAlloc, "SSAO.PrevColor"))
                return false;
            s_prevColorView = VK::CreateImageView(s_prevColor, kILFormat);
            if (s_prevColorView == VK_NULL_HANDLE) return false;
            s_prevColorCleared = false;
        }
        // Temporal history pair (half-res): AO history = RG16F (matches s_img[0]:
        // AO + view depth), IL history = kILFormat (matches s_ilImg[0]). copy dst
        // (capture the final result) + sampled (reproject next frame). Cleared to
        // black on first Execute — depth 0 fails the disocclusion test, so a fresh
        // history is rejected rather than blended.
        {
            struct { VkImage* img; VmaAllocation* alloc; VkImageView* view; VkFormat fmt; const char* name; } hist[2] = {
                { &s_aoHist, &s_aoHistAlloc, &s_aoHistView, kAOFinalFormat, "SSAO.AOHist" },
                { &s_ilHist, &s_ilHistAlloc, &s_ilHistView, kILFormat,            "SSAO.ILHist" },
            };
            for (auto& h : hist) {
                if (!VK::CreateImage2D(h.fmt, want,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        *h.img, *h.alloc, h.name))
                    return false;
                *h.view = VK::CreateImageView(*h.img, h.fmt);
                if (*h.view == VK_NULL_HANDLE) return false;
            }
            s_histCleared = false; s_histValid = false;
        }
        // FULL-res NPC normal G-buffer (skinned pass renders into it, GTAO samples it).
        s_normExtent = sceneExtent;
        {
            if (!VK::CreateImage2D(kNormalFormat, sceneExtent,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    s_normImg, s_normAlloc, "SSAO.NPCNormal"))
                return false;
            s_normView = VK::CreateImageView(s_normImg, kNormalFormat);
            if (s_normView == VK_NULL_HANDLE) return false;
        }

        // Host buffer for the debug readback (final is RG16F = two halves per texel;
        // the stats read the first — AO — and skip the depth).
        s_dbgBuf.Destroy();
        s_dbgMap = nullptr;
        s_dbgCountdown = -1;
        s_dbgBuf.Create(VkDeviceSize(want.width) * want.height * sizeof(u16) * 2,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        s_dbgMap = static_cast<u16*>(s_dbgBuf.Map());

        ++s_generation;
        Msg("[VK SSAO] RTs ready (%ux%u half-res, gen %u, samples %u)",
            want.width, want.height, s_generation, SampleCount());
        return true;
    }

    // MRT: target 0 = AO (raw R16F for gtao, final RG16F for the blur), target 1 =
    // SSIL (RGBA16F). Both opaque, all channels, no blend. gtao + blur share this
    // two-attachment layout; only the AO format differs per stage.
    VkPipeline CreatePipe(VkShaderModule vs, VkShaderModule fs, VkFormat aoFmt)
    {
        const VkFormat fmts[2] = { aoFmt, kILFormat };
        const VkPipelineColorBlendAttachmentState blends[2] = {
            Fullscreen::OpaqueAttachment(), Fullscreen::OpaqueAttachment() };
        return Fullscreen::CreatePipelineMRT(vs, fs, fmts, 2, s_layout, blends, "SSAO");
    }

    // One MRT fullscreen draw into (AO, IL) at the half-res extent.
    void Draw(VkCommandBuffer cmd, VkImageView aoDst, VkImageView ilDst, VkPipeline pipe,
              VkDescriptorSet set, const SSAOPush& push)
    {
        const VkImageView views[2] = { aoDst, ilDst };
        Fullscreen::DrawMRT(cmd, views, 2, s_extent, pipe, s_layout, set, &push, sizeof(push));
    }
}  // anon namespace

// Set once the first Execute actually rendered AO — until then (and whenever
// the prepass is unavailable) Strength() keeps the receivers on plain 1.0.
static bool s_hasResult = false;

bool        Enabled()       { return s_inited && !s_failed && ps_r_ssao_enable != 0; }
VkImageView GetResultView() { return Enabled() ? s_view[0] : VK_NULL_HANDLE; }  // disabled → EnvLight uses white fallback
VkSampler   GetSampler()    { return s_sampLin; }
u32         Generation()    { return s_generation; }
float       Strength()      { return (Enabled() && s_hasResult && s_view[0]) ? kStrength : 0.f; }

// SSIL result (MRT target 1). Folded into the GTAO march, so it requires the
// GTAO pass to be running (r_ssao on) AND r_ssil on. Null otherwise → tonemap
// binds its fallback and skips the IL composite.
VkImageView GetILResultView() { return (Enabled() && s_hasResult && ps_r_ssil_enable != 0 && s_ilView[0]) ? s_ilView[0] : VK_NULL_HANDLE; }

// The prev-frame colour history. Handed out unconditionally once it exists: it is
// cleared to black on the first Execute, so a reader that samples it before any
// capture gets black rather than undefined memory.
VkImageView GetPrevColorView() { return s_prevColorView; }

// NPC normal G-buffer: the skinned pass renders into it (after the prepass,
// before Execute); GTAO samples it (binding 2). VK_NULL_HANDLE until EnsureRTs.
VkImage     GetNormalImage()  { return s_normImg; }
VkImageView GetNormalView()   { return s_normView; }
VkFormat    GetNormalFormat() { return kNormalFormat; }
VkExtent2D  GetNormalExtent() { return s_normExtent; }

// ⚠⚠⚠ THE NORMAL G-BUFFER AND THE COLOUR HISTORY ARE NOT AO DETAILS, AND TREATING
// THEM AS ONE COST A SESSION. They live in this file because GTAO was their first
// reader, but the FINAL GATHER takes its ray origins and its cosine lobe from the
// same normal image, and its screen-space radiance cache from the same history —
// so gating their EXISTENCE on r_ssao meant that switching ambient occlusion off
// silently switched GLOBAL ILLUMINATION off with it.
//
// ⭐That failure is invisible in the worst possible way: the probe lattice keeps
// answering, so the room stays lit and still reacts to where the player stands. It
// only goes FLAT — no light through a window, no bounce, no contact darkening —
// which reads as "the GI got worse", not as "a pass stopped running".
//
// Enabled() keeps its meaning: COMPUTE AO. This one means: SOMEBODY NEEDS THESE
// BUFFERS. Two independent systems, one shared resource, and now one gate each —
// the same producer/consumer trap CapturePrevColor below already warns about.
bool ResourcesWanted()
{
    return s_inited && !s_failed && (ps_r_ssao_enable != 0 || GatherPass::Wanted());
}

bool EnsureTargets(VkExtent2D sceneExtent)
{
    if (!ResourcesWanted()) return false;
    return EnsureRTs(sceneExtent);
}

bool Init()
{
    if (s_inited) return !s_failed;
    s_inited = true;

    if (!g_ShaderManager) { s_failed = true; return false; }
    VkShaderModule vs   = g_ShaderManager->Load("tonemap.vert.spv");   // shared fullscreen triangle
    VkShaderModule gtao = g_ShaderManager->Load("ssao.frag.spv");
    VkShaderModule blur = g_ShaderManager->Load("ssao_blur.frag.spv");
    if (vs == VK_NULL_HANDLE || gtao == VK_NULL_HANDLE || blur == VK_NULL_HANDLE) {
        Msg("![VK SSAO] ssao.{frag,blur}.spv missing — SSAO disabled");
        s_failed = true; return false;
    }

    // Set: 0 = scene depth, 1 = raw AO (blur only), 2 = NPC normal G-buffer
    // (GTAO only), 3 = prev-frame colour (GTAO+IL gather), 4 = raw IL (blur only),
    // 5 = motion vectors, 6 = AO history, 7 = IL history (blur temporal only),
    // 8 = TemporalUBO (blur reprojection: prevVP + eye + jitter).
    // Each pipeline statically uses a subset; the others' stale layout during a
    // draw is legal (all bindings are written each frame).
    constexpr auto kTex = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    constexpr auto kUbo = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    const u32 nSets = kFramesInFlight * 2;
    {
        VkDescriptorSet sets[kFramesInFlight * 2]{};
        if (!VK::MakeDescriptorSets({ kTex, kTex, kTex, kTex, kTex, kTex, kTex, kTex, kUbo }, nSets,
                                    s_setLayout, s_pool, sets,
                                    VK_SHADER_STAGE_FRAGMENT_BIT, "SSAO")) {
            s_failed = true; return false;
        }
        for (u32 i = 0; i < kFramesInFlight; ++i) {
            s_setGtao[i] = sets[i];
            s_setBlur[i] = sets[kFramesInFlight + i];
        }
    }

    // Per-slot reprojection constants, persistently mapped (rewritten every frame).
    // 256-byte slot stride: the largest minUniformBufferOffsetAlignment a desktop
    // GPU asks for — a misaligned dynamic slot is UB (the vk_env_light scar).
    {
        s_tempUboStride = (sizeof(TemporalUBO) + 255) & ~VkDeviceSize(255);
        s_tempUbo.Create(s_tempUboStride * kFramesInFlight, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        s_tempUboMap = static_cast<u8*>(s_tempUbo.Map());
        if (!s_tempUboMap) { Msg("![VK SSAO] temporal UBO map failed"); s_failed = true; return false; }
        std::memset(s_tempUboMap, 0, size_t(s_tempUboStride * kFramesInFlight));
    }

    auto makeSampler = [&](VkFilter f, VkSampler& out) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = f; si.minFilter = f;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxAnisotropy = 1.0f;
        return vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &out) == VK_SUCCESS;
    };
    if (!makeSampler(VK_FILTER_NEAREST, s_sampNear) || !makeSampler(VK_FILTER_LINEAR, s_sampLin)) {
        Msg("![VK SSAO] sampler create failed"); s_failed = true; return false;
    }

    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, (u32)sizeof(SSAOPush) };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &s_setLayout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_layout) != VK_SUCCESS) {
        s_failed = true; return false;
    }

    s_pipeGtao = CreatePipe(vs, gtao, kAORawFormat);     // → s_img[1]
    s_pipeBlur = CreatePipe(vs, blur, kAOFinalFormat);   // → s_img[0]
    if (s_pipeGtao == VK_NULL_HANDLE || s_pipeBlur == VK_NULL_HANDLE) { s_failed = true; return false; }

    // RTAO rides on these targets; a failure there is not fatal (no ray query on
    // the device, or the .spv is missing) — GTAO keeps running alone.
    RTAOPass::Init();

    Msg("[VK SSAO] init OK (GTAO half-res, radius %.1f, r2_ssao quality %u)", kRadius, ps_r_ao_quality);
    return true;
}

void Execute(VkCommandBuffer cmd, VkExtent2D sceneExtent)
{
    if (!Enabled()) return;
    if (!EnsureRTs(sceneExtent)) return;

    // Keep the camera-local BLAS current (no-op unless r_rtao). Must be OUTSIDE a
    // render pass — it records acceleration-structure builds — which is why it
    // sits here, before the first BeginRendering of this pass.
    RTScene::Update(cmd, RV().vCameraPosition);
    // Same rule for the per-frame skeleton structure (r_rt_dyn): builds recorded
    // here, after the PreSkin pass wrote the pool and before RTAO traces it.
    RTDyn::Update(cmd);

    // Temporal accumulation state (r_ssil_temporal). Reproject the previous frame's
    // FINAL AO/IL through the motion vectors and EMA-blend; the per-frame-jittered
    // gather (ssao.frag) makes each history a different realisation → averaging
    // removes the directional banding. Only reproject with CONTINUOUS history (it was
    // refreshed last frame) so an off→on toggle or a resize starts clean, and only
    // once the MV target has surely rendered (else it could be UNDEFINED layout).
    ++s_frame;
    // Temporal is now driven by EITHER r_ssao_temporal (AO, standalone) OR r_ssil_temporal
    // (legacy SSIL path). AO and IL share one per-frame jitter + one history pair, so the
    // blur EMA uses the stronger of the two α's.
    // ⚠ The SSIL α only counts while SSIL runs: with r_ssil off it used to win the
    // max() anyway (0.9 over the AO's 0.85) and quietly lengthen the AO's memory.
    const float temporalAlpha = (ps_r_ssil_enable != 0) ? std::max(ps_r_ssao_temporal, ps_r_ssil_temporal)
                                                         : ps_r_ssao_temporal;
    const bool temporalOn = (temporalAlpha > 0.0f);
    const bool histUsable = temporalOn && s_temporalWasOn && s_histValid;
    if (MotionVec::Enabled() && MotionVec::GetResultView() != VK_NULL_HANDLE) { if (s_mvWarm < 4) ++s_mvWarm; }
    else s_mvWarm = 0;
    // History reprojection mode for the blur (push.temporal.z):
    //   2 = from depth + prevVP (r_ssao_reproj 1, default) — THIS frame's camera,
    //       exact for statics, history depth-tested for disocclusion;
    //   1 = legacy MV target (r_ssao_reproj 0) — ⚠ that target is written AFTER
    //       this pass (MotionVecDyn, post-LODs), so it is the PREVIOUS frame's field;
    //   0 = same-pixel EMA (no valid source).
    // r_mv_prepass changed what "the motion target" means to this pass. It used to be
    // written after the world pass — one frame old here, and camera-only until the
    // overlays ran. Resolved in the prepass it is THIS frame's field and it already
    // carries the geometry that moves under its own power, which reconstruction from
    // depth can never know about: an NPC walking across a wall drags its own AO with
    // it instead of leaving the wall's history smeared over it.
    //   r_ssao_reproj 2 (default) = auto: the motion target when it was resolved
    //     early this frame, the in-blur reconstruction otherwise;
    //   1 = always reconstruct here (statics-exact, no dynamics);
    //   0 = always the motion target (⚠ one frame stale without r_mv_prepass).
    const bool  mvFresh     = MotionVec::ResolvedEarly();
    const bool  wantMV      = (ps_r_ssao_reproj == 0) || (ps_r_ssao_reproj >= 2 && mvFresh);
    const bool  mvReady     = histUsable && wantMV && (s_mvWarm >= 2)
                           && MotionVec::GetResultView() != VK_NULL_HANDLE;
    const bool  reprojDepth = histUsable && !mvReady;
    const float reprojMode  = mvReady ? 1.0f : (reprojDepth ? 2.0f : 0.0f);
    VkImageView mvBind  = mvReady ? MotionVec::GetResultView() : s_prevColorView;  // placeholder is valid SHADER_READ, never sampled when !mvReady
    float jitterPhase = float(s_frame) * 0.61803399f; jitterPhase -= std::floor(jitterPhase);

    // Reprojection constants for this slot. EnsureHistory rolls the MotionVec
    // cur/prev pair for THIS RV().frameIndex — the MV passes themselves run after us,
    // so without the call PrevVP() would still be last frame's "previous" (a two-
    // frame-old camera, i.e. the very lag this path exists to remove).
    const u32 slot = CommandManager.GetCurrentFrame() % kFramesInFlight;
    {
        MotionVec::EnsureHistory();
        TemporalUBO tu{};
        std::memcpy(tu.prevVP, &MotionVec::PrevVP(), sizeof(tu.prevVP));
        const Fvector eye = RV().vCameraPosition;
        tu.eye[0] = eye.x; tu.eye[1] = eye.y; tu.eye[2] = eye.z; tu.eye[3] = 0.0f;
        MotionVec::FrameJitter(tu.jitter[0], tu.jitter[1]);
        tu.jitter[2] = 0.0f; tu.jitter[3] = 0.0f;
        std::memcpy(s_tempUboMap + s_tempUboStride * slot, &tu, sizeof(tu));
    }

    // Refresh this slot's sets (fence-guarded): depth view can change on swapchain
    // recreate, AO/IL/prevColor/history views on RT recreate. 8 images + 1 UBO × 2 sets/frame.
    {
        VkDescriptorImageInfo depthI { s_sampNear, Swapchain.m_DepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo rawI   { s_sampNear, s_view[1],             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo normI  { s_sampNear, s_normView,            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo prevI  { s_sampLin,  s_prevColorView,       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ilrawI { s_sampNear, s_ilView[1],           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo mvI    { s_sampLin,  mvBind,                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo aoHistI{ s_sampLin,  s_aoHistView,          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ilHistI{ s_sampLin,  s_ilHistView,          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        // gtao uses 0,2,3; blur uses 0,1,4,5,6,7,8 — write all 9 to both sets so none is stale.
        const VkDescriptorImageInfo src[8] = { depthI, rawI, normI, prevI, ilrawI, mvI, aoHistI, ilHistI };
        for (VkDescriptorSet set : { s_setGtao[slot], s_setBlur[slot] }) {
            VK::DescriptorWriter dw(set);
            for (u32 b = 0; b < 8; ++b)
                dw.Image(b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, src[b]);
            dw.UniformBuffer(8, s_tempUbo.GetHandle(), sizeof(TemporalUBO), s_tempUboStride * slot);
            dw.Flush();
        }
    }

    // First use after (re)create: clear the prev-colour history to black so the
    // horizon gather reads valid (zero-IL) data until the first capture lands.
    if (!s_prevColorCleared) {
        ImageBarrier(cmd, s_prevColor, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black{};
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, s_prevColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        ImageBarrier(cmd, s_prevColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        s_prevColorCleared = true;
    }

    // First use: clear the temporal history pair to black so they're valid
    // SHADER_READ when the blur set binds them (sampled only when history is valid,
    // but the descriptor references them every frame). s_histValid stays false until
    // a real final result is copied in below.
    if (!s_histCleared) {
        VkClearColorValue black{};
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (VkImage h : { s_aoHist, s_ilHist }) {
            ImageBarrier(cmd, h, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, h, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            ImageBarrier(cmd, h, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        s_histCleared = true;
    }

    // Transition the AO + IL MRT pair for slot i together (one draw writes both).
    auto toColor = [&](u32 i) {
        const VkImageLayout old = s_first[i] ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ImageBarrier(cmd, s_img[i],   old, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ImageBarrier(cmd, s_ilImg[i], old, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        s_first[i] = false;
    };
    auto toRead = [&](u32 i) {
        ImageBarrier(cmd, s_img[i],   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ImageBarrier(cmd, s_ilImg[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };

    // Everything derived from the matrix that RENDERED the prepass depth —
    // RV().mProject is not maintained on this path (reads as identity).
    const ProjTerms pt = DeriveProjTerms(RV().mFullTransform);
    SSAOPush push{};
    push.camDir[0] = pt.dir.x; push.camDir[1] = pt.dir.y; push.camDir[2] = pt.dir.z;
    push.camDir[3] = ps_r_ssao_bias;   // grazing-surface horizon bias (ssao.frag rejects coplanar samples)
    push.camRightT[0] = pt.right.x * pt.tanX; push.camRightT[1] = pt.right.y * pt.tanX;
    push.camRightT[2] = pt.right.z * pt.tanX; push.camRightT[3] = pt.tanX;
    push.camTopT[0]   = pt.top.x  * pt.tanY;  push.camTopT[1]   = pt.top.y  * pt.tanY;
    push.camTopT[2]   = pt.top.z  * pt.tanY;  push.camTopT[3]   = pt.tanY;
    push.zp[0] = pt.p33;
    push.zp[1] = pt.p43;
    push.zp[2] = kRadius;
    push.zp[3] = float(SampleCount());
    push.res[0] = float(s_extent.width);
    push.res[1] = float(s_extent.height);
    push.res[2] = 1.f / float(s_extent.width);
    push.res[3] = 1.f / float(s_extent.height);
    push.dbg[0] = float(ps_r_ssao_debug);
    push.dbg[1] = ps_r_ssil_enable ? 1.0f : 0.0f;   // SSIL: gather prev-frame colour in the horizon march
    push.dbg[2] = kILFireClamp;
    push.dbg[3] = ps_r_ssil_enable ? ps_r_ssil_strength : 0.0f;   // baked into IL → forward ssilBoost; 0 when off
    push.temporal[0] = temporalOn ? temporalAlpha : 0.0f;          // EMA α (gather: gates jitter; blur: history weight)
    push.temporal[1] = temporalOn ? jitterPhase : 0.0f;            // per-frame slice/radial rotation (0 → spatial path)
    push.temporal[2] = reprojMode;                                 // 2 = depth+prevVP, 1 = MV target, 0 = same-pixel EMA
    push.temporal[3] = histUsable ? 1.0f : 0.0f;                   // blur may sample the history this frame

    // One-time dump of the reconstruction inputs — sanity vs the offline test
    // (expect _33 ≈ 1.0006, _43 ≈ -0.2 for zn 0.2 / zf 350, tan ≈ 0.6-1.1).
    static bool s_pushDiag = false;
    if (!s_pushDiag) { s_pushDiag = true;
        Msg("[VK SSAO] push (derived): _33=%.6f _43=%.6f tanX=%.4f tanY=%.4f dir=(%.2f,%.2f,%.2f)",
            pt.p33, pt.p43, pt.tanX, pt.tanY, pt.dir.x, pt.dir.y, pt.dir.z);
    }

    // 1) GTAO+IL: depth + prev-colour → raw AO [1] + raw IL [1].
    toColor(1);
    Draw(cmd, s_view[1], s_ilView[1], s_pipeGtao, s_setGtao[slot], push);

    // 1b) RTAO (r_rtao): hardware ray query against the camera-local BLAS,
    // MIN-blended over the raw AO still in the attachment. Deliberately here and
    // not as its own target — the blur, the temporal EMA and every AO consumer
    // downstream then need no knowledge of it at all. Same jitter phase, so the
    // EMA averages a different ray set each frame just like it does for GTAO.
    RTAOPass::Overlay(cmd, s_view[1], s_ilView[1], s_extent,
                      Swapchain.m_DepthView, s_normView, s_sampNear, pt, jitterPhase);
    toRead(1);

    // 2) Depth-aware 3×3 blur: raw [1] → final [0] (both AO and IL). When temporal
    // is on this draw also EMA-blends the reprojected history bound above.
    toColor(0);
    Draw(cmd, s_view[0], s_ilView[0], s_pipeBlur, s_setBlur[slot], push);
    toRead(0);

    // 3) Capture this frame's FINAL AO + IL as next frame's temporal history. Single-
    // buffered like prevColor: the dst→SHADER_READ barrier orders the next frame's
    // reprojection read after this copy (same queue, submission order). Only while
    // temporal is on — a gap invalidates the history so it isn't blended stale.
    if (temporalOn) {
        auto copyHist = [&](VkImage src, VkImage dst) {
            ImageBarrier(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy region{};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.extent = { s_extent.width, s_extent.height, 1 };
            vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ImageBarrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        };
        copyHist(s_img[0], s_aoHist);
        // IL history only while SSIL actually runs — with r_ssil off the IL
        // final is black anyway and the blur never samples uILhist (ilOn gate);
        // skipping saves a full-image RGBA16F copy per frame. A later ssil-on
        // starts against stale/black history, which the neighbourhood clamp
        // bounds and the EMA re-converges in ~1/(1-α) frames.
        if (ps_r_ssil_enable != 0)
            copyHist(s_ilImg[0], s_ilHist);
        s_histValid = true;
    } else {
        s_histValid = false;   // stale frames must not be reprojected after a gap
    }
    s_temporalWasOn = temporalOn;

    s_hasResult = true;

    // Debug readback (r_ssao_debug): periodically copy the final AO to the host
    // buffer; a few frames later (fence-safe by frames-in-flight) log stats.
    if (ps_r_ssao_debug && s_dbgMap) {
        if (s_dbgCountdown > 0 && --s_dbgCountdown == 0) {
            // Final is RG16F: texel i = halves [2i] (AO) and [2i+1] (view depth).
            const u32 n = s_extent.width * s_extent.height;
            auto aoAt = [&](u32 i) { return AoHalfToU8(s_dbgMap[i * 2]); };
            u32 mn = 255, mx = 0; u64 sum = 0; u32 below200 = 0;
            for (u32 i = 0; i < n; ++i) {
                const u8 v = aoAt(i);
                mn = std::min(mn, (u32)v); mx = std::max(mx, (u32)v);
                sum += v; below200 += (v < 200);
            }
            Msg("[VK SSAO] readback mode %d %ux%u: min %u max %u avg %.1f, <200: %.1f%%",
                ps_r_ssao_debug, s_extent.width, s_extent.height, mn, mx,
                double(sum) / n, 100.0 * below200 / n);
            for (u32 r = 1; r <= 3; ++r) {   // rows at 25/50/75% height, 5 taps each
                const u32 cy = s_extent.height * r / 4;
                Msg("[VK SSAO]   row %u%%: %u %u %u %u %u", r * 25,
                    aoAt(cy * s_extent.width + s_extent.width / 6),
                    aoAt(cy * s_extent.width + s_extent.width / 3),
                    aoAt(cy * s_extent.width + s_extent.width / 2),
                    aoAt(cy * s_extent.width + s_extent.width * 2 / 3),
                    aoAt(cy * s_extent.width + s_extent.width * 5 / 6));
            }
            // Full-image AO dump for offline analysis (the tooling that cracked
            // the полосы saga). Overwrites <cwd>\ssao_dump.bin on every readback
            // while r_ssao_debug is active; reader = _parked/ssao_dump_view.js
            // (detects the channel count from the file size — written de-interleaved
            // as ONE channel so the reader keeps seeing what it saw under R16F).
            if (FILE* f = fopen("ssao_dump.bin", "wb")) {
                fwrite(&s_extent.width, 4, 1, f);
                fwrite(&s_extent.height, 4, 1, f);
                for (u32 i = 0; i < n; ++i) fwrite(&s_dbgMap[i * 2], sizeof(u16), 1, f);
                fclose(f);
                Msg("[VK SSAO] AO dump written: ssao_dump.bin (%ux%u R16F)", s_extent.width, s_extent.height);
            }
        }
        if (s_dbgCooldown == 0 && s_dbgCountdown <= 0) {
            ImageBarrier(cmd, s_img[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy bic{};
            bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            bic.imageExtent      = { s_extent.width, s_extent.height, 1 };
            vkCmdCopyImageToBuffer(cmd, s_img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   s_dbgBuf.GetHandle(), 1, &bic);
            ImageBarrier(cmd, s_img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            s_dbgCountdown = 5;     // read after the in-flight fences cycled
            s_dbgCooldown  = 300;   // ~one log line per few seconds
        }
        if (s_dbgCooldown) --s_dbgCooldown;
    }
}

void CapturePrevColor(VkCommandBuffer cmd, VkImage srcImage, VkExtent2D srcExtent)
{
    // Snapshot THIS frame's composited HDR scene (half-res, linear) into the
    // history the NEXT frame reads. prevColor is SHADER_READ here (cleared on the
    // first Execute, left SHADER_READ by the previous capture).
    //
    // ⚠ TWO READERS NOW, AND THE GATE HAS TO NAME BOTH. SSIL gathers it in the
    // horizon march; the final gather uses it as a screen-space radiance cache.
    // Leaving the gate at r_ssil alone meant the gather's cache silently read a
    // buffer nobody was filling — black hits, i.e. a feature that costs its taps
    // and returns nothing. Same producer/consumer trap as the r_gather gates.
    // ⚠ ResourcesWanted, not Enabled: with AO off the gather is the ONLY reader of
    // this history, and gating on the AO switch is exactly the trap the note above
    // describes — the cache would ask for a buffer nobody fills and quietly return
    // black hits.
    if (!ResourcesWanted() || (ps_r_ssil_enable == 0 && !GatherPass::WantsPrevColor())
        || s_prevColor == VK_NULL_HANDLE
        || srcImage == VK_NULL_HANDLE)
        return;

    // ⚠⚠ THE FIRST-USE CLEAR HAS TO LIVE HERE TOO. It used to sit in Execute, which
    // does not run when AO is off — so with the gather as the only reader the flag
    // below never flipped and this function refused every frame, for the whole run.
    // Idempotent by the flag, not by the caller: whichever path reaches it first
    // gives the image a defined layout, and the other one skips it.
    if (!s_prevColorCleared) {
        ImageBarrier(cmd, s_prevColor, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black{};
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, s_prevColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        ImageBarrier(cmd, s_prevColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        s_prevColorCleared = true;
    }

    // src mip0 SHADER_READ → TRANSFER_SRC (ImageBarrier touches mip0 only, leaving
    // the rest of the HDR mip chain SHADER_READ for the tonemap that follows).
    ImageBarrier(cmd, srcImage,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(cmd, s_prevColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1]  = { int32_t(srcExtent.width), int32_t(srcExtent.height), 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1]  = { int32_t(s_extent.width), int32_t(s_extent.height), 1 };
    vkCmdBlitImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   s_prevColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    // src back to SHADER_READ (tonemap samples it); dst to SHADER_READ — this
    // barrier also synchronizes the next frame's gather read (same queue, in
    // submission order), so the single history buffer needs no ping-pong.
    ImageBarrier(cmd, srcImage,    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, s_prevColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    RTAOPass::Destroy();
    RTScene::Destroy();
    DestroyRTs();
    s_dbgBuf.Destroy(); s_dbgMap = nullptr; s_dbgCountdown = -1; s_dbgCooldown = 0;
    if (s_tempUboMap) { s_tempUbo.Unmap(); s_tempUboMap = nullptr; }
    s_tempUbo.Destroy(); s_tempUboStride = 0;
    if (s_pipeGtao)  { vkDestroyPipeline(VulkanHW.m_Device, s_pipeGtao, nullptr); s_pipeGtao = VK_NULL_HANDLE; }
    if (s_pipeBlur)  { vkDestroyPipeline(VulkanHW.m_Device, s_pipeBlur, nullptr); s_pipeBlur = VK_NULL_HANDLE; }
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_sampNear)  { vkDestroySampler(VulkanHW.m_Device, s_sampNear, nullptr); s_sampNear = VK_NULL_HANDLE; }
    if (s_sampLin)   { vkDestroySampler(VulkanHW.m_Device, s_sampLin, nullptr); s_sampLin = VK_NULL_HANDLE; }
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    for (u32 i = 0; i < kFramesInFlight; ++i) { s_setGtao[i] = VK_NULL_HANDLE; s_setBlur[i] = VK_NULL_HANDLE; }
    s_inited = false; s_failed = false; s_generation = 0; s_hasResult = false;
}

}}  // namespace VK::SSAOPass

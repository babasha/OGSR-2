// xrRenderVulkan — AMD FidelityFX Super Resolution 3.1.4 (FSR3 Upscaler), Vulkan backend.
//
// See vk_fsr.h for why this exists. This file is the whole integration: backend
// bring-up, context, the three shared resources, jitter, and the per-frame dispatch.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_fsr.h"

#ifndef _WIN32
// The FidelityFX SDK is only built for Windows here (_build_ffx_sdk.bat → the two .lib
// below). Off Windows FSR is never available — the same state a failed Init() leaves on
// Windows (Available() false), so every seam in CRender_Vulkan takes its native-res path:
// render extent == display, no jitter, the tonemap never picks FSR's output.
namespace VK { namespace Fsr {
bool Init() { Msg("[VK FSR] FidelityFX is not available on this platform"); return false; }
void Shutdown() {}
bool Inited() { return false; }
bool Available() { return false; }
bool Enabled() { return false; }
void GetRenderExtent(u32 displayW, u32 displayH, u32& outW, u32& outH) { outW = displayW; outH = displayH; }
bool Upscaling() { return false; }
void OnFrameBegin(u32, u32) {}
void EnsureOutput(u32, u32) {}
VkImageView OutputView() { return VK_NULL_HANDLE; }
VkImage OutputImage() { return VK_NULL_HANDLE; }
VkFormat OutputFormat() { return VK_FORMAT_UNDEFINED; }
bool OutputReady() { return false; }
bool ResolvedThisFrame() { return false; }
void SetResolvedThisFrame(bool) {}
void NewFrameJitter(u32, u32, u32) {}
void GetProjJitterNDC(float& outX, float& outY) { outX = outY = 0.f; }
void GetJitterPix(float& outX, float& outY) { outX = outY = 0.f; }
bool TakeHistoryReset() { return true; }
bool Evaluate(VkCommandBuffer, const Img&, const Img&, const Img&, u32, u32, u32, u32, bool) { return false; }
void SetExposureHint(float) {}
bool DescribeState(char*, u32) { return false; }
}} // namespace VK::Fsr
#else

#include "vk_dlss.h"        // Dlss::Upscaling() — the two upscalers are mutually exclusive
#include "HW_Vulkan.h"
#include "vk_image.h"       // VK::CreateImage2D / CreateImageView
#include "vk_barriers.h"    // ImageBarrier
#include "vk_vram_stats.h"  // VK::Vram::DestroyImage - our accounted allocator
#include "../../xr_3da/device.h"

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

// The SDK is built as two static libs by 3rd_party\Src\AMD_FidelityFX\sdk (see
// _build_ffx_sdk.bat). Named here rather than in the .vcxproj so the dependency
// travels with the only file that uses it — the LuaJIT precedent in this tree.
// ⚠The SDK's CMake writes BOTH configurations into the same bin\ffx_sdk folder,
// so a Debug SDK build silently overwrites the Release libs. Rebuild the SDK
// Release if the engine ever links a CRT-mismatched one.
#pragma comment(lib, "ffx_fsr3upscaler_x64.lib")
#pragma comment(lib, "ffx_backend_vk_x64.lib")

extern int   ps_r_fsr;
extern int   ps_r_fsr_quality;
extern int   ps_r_fsr_avail;
extern float ps_r_fsr_sharp;
extern int   ps_r_fsr_debug;

namespace VK { namespace Fsr {

namespace {

// ---- backend + context ------------------------------------------------------
bool                    s_inited      = false;
bool                    s_failed      = false;   // a real error; do not retry every frame
FfxInterface            s_iface       = {};
FfxDevice               s_device      = nullptr;
void*                   s_scratch     = nullptr;
FfxFsr3UpscalerContext  s_context     = {};
bool                    s_ctxLive     = false;
u32                     s_ctxRenderW  = 0, s_ctxRenderH = 0;   // maxRenderSize the context was built for
u32                     s_ctxDisplayW = 0, s_ctxDisplayH = 0;

// The three "shared" resources FSR3.1 emits for a downstream frame-interpolation
// pass. We do not interpolate frames, but the upscaler WRITES them unconditionally
// and ffxFsr3UpscalerContextDispatch has no null path for them — so they are
// allocated exactly as ffx_provider_fsr3upscale.cpp does it and then ignored.
FfxResourceInternal     s_dilatedDepth   = {};
FfxResourceInternal     s_dilatedMVs     = {};
FfxResourceInternal     s_prevNearDepth  = {};
bool                    s_sharedLive     = false;

// ---- output target ----------------------------------------------------------
VkImage         s_outImg   = VK_NULL_HANDLE;
VkImageView     s_outView  = VK_NULL_HANDLE;
VmaAllocation   s_outAlloc = VK_NULL_HANDLE;
u32             s_outW = 0, s_outH = 0;
constexpr VkFormat kOutFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

// ---- per-frame state --------------------------------------------------------
bool   s_resolved   = false;    // this frame's dispatch actually wrote the output
float  s_jitterPixX = 0.f, s_jitterPixY = 0.f;
u32    s_jitterW = 0, s_jitterH = 0;
int    s_jitterIndex = 0;
int    s_jitterCount = 1;
float  s_exposure   = 1.0f;

// History-reset detector — same three questions vk_dlss.cpp asks, and for the
// same reason: FSR reprojects its own previous OUTPUT through this frame's motion
// vectors, which is only meaningful while those vectors describe the change.
u32     s_lastEvalFrame = 0;
bool    s_evalChain     = false;
Fvector s_lastCamPos    = {};
bool    s_camPosValid   = false;
constexpr float kCamCutDistSq = 25.f * 25.f;   // a 25 m step is a teleport, not a walk

FfxFsr3UpscalerQualityMode QualityMode()
{
    const int q = (ps_r_fsr_quality < 0) ? 0 : (ps_r_fsr_quality > 4 ? 4 : ps_r_fsr_quality);
    return (FfxFsr3UpscalerQualityMode)q;
}

// FSR talks to us through this on FFX_MESSAGE_TYPE_ERROR / _WARNING. Routed into
// the engine log with the same ! / ~ prefixes everything else uses, because a
// silent upscaler that quietly does nothing is the exact failure this whole
// module exists to stop repeating.
void OnMessage(FfxMsgType type, const wchar_t* message)
{
    if (!message) return;
    char buf[1024];
    const size_t n = wcstombs(buf, message, sizeof(buf) - 1);
    buf[(n == (size_t)-1) ? 0 : n] = 0;
    if (type == FFX_MESSAGE_TYPE_ERROR) Msg("![VK FSR] %s", buf);
    else                                Msg("~[VK FSR] %s", buf);
}

void DestroyContext()
{
    if (s_ctxLive) {
        ffxFsr3UpscalerContextDestroy(&s_context);
        s_ctxLive = false;
        s_sharedLive = false;   // the shared resources live in the backend's effect context
    }
}

// (Re)build the upscaler context for these maximum dimensions. FSR wants the
// MAXIMUM render size it will ever see, not this frame's — with
// ENABLE_DYNAMIC_RESOLUTION it then accepts anything up to that per dispatch.
bool EnsureContext(u32 displayW, u32 displayH)
{
    if (!s_inited || s_failed) return false;
    // ⭐⭐⭐maxRenderSize IS THE DISPLAY SIZE, NOT THIS FRAME S RENDER SIZE.
    // The first version keyed the context on the CURRENT render extent, so every
    // r_fsr_quality change tore it down and built a new one - which is exactly what
    // ENABLE_DYNAMIC_RESOLUTION exists to avoid, and that flag was already set.
    // ⚠⚠Worse, the teardown ran from Evaluate, i.e. MID-RECORDING: destroying the
    // backend samplers invalidated the frame command buffer that referenced them, and
    // every later vkCmd in that frame failed. Validation named it outright -
    // "VkSampler 0x2d74... was destroyed" - and the context create right after it
    // returned FFX_ERROR_BACKEND_API_ERROR as collateral, which read as "FSR does not
    // work on this GPU". Keyed on the display size the context is built ONCE and a
    // quality change costs nothing.
    if (s_ctxLive && displayW == s_ctxDisplayW && displayH == s_ctxDisplayH)
        return true;

    // A dimension change tears the context down. ⚠The GPU may still be reading
    // the internal targets from a frame in flight, and ffxFsr3UpscalerContextDestroy
    // frees them immediately — the same class of hazard the DLSS feature rebuild
    // hit. Idle before the destroy; this fires on a resolution change only.
    if (s_ctxLive) {
        VK_DeviceWaitIdleLocked();
        DestroyContext();
    }

    FfxFsr3UpscalerContextDescription desc = {};
    desc.backendInterface       = s_iface;
    desc.maxRenderSize.width    = displayW;
    desc.maxRenderSize.height   = displayH;
    desc.maxUpscaleSize.width   = displayW;
    desc.maxUpscaleSize.height  = displayH;
    desc.fpMessage              = OnMessage;
    // ⭐The flag set is a set of claims about OUR buffers, and every one of them is
    // checked against this renderer rather than copied from a sample:
    //  HIGH_DYNAMIC_RANGE  — SceneColor is RGBA16F scene-referred, not tonemapped.
    //  AUTO_EXPOSURE       — we do have a CPU-smoothed exposure (SetExposureHint),
    //                        but feeding it as a 1x1 texture is a second thing that
    //                        can be wrong; auto is FSR computing it from the very
    //                        image it is about to upscale, which cannot desync.
    //  DYNAMIC_RESOLUTION  — r_render_scale can move between frames without a
    //                        context rebuild.
    //  NO depth-inverted   — ⚠this engine has NO reversed-Z (LESS_OR_EQUAL, near
    //                        0.2 m, D32F). Setting it would invert disocclusion.
    //  NO depth-infinite   — we have a real far plane (far_plane_render).
    //  NO MV jitter cancel — ⚠motion_vec.frag reprojects with UNJITTERED matrices,
    //                        so the vectors carry no jitter to cancel. Setting this
    //                        would subtract a wobble that was never added.
    desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE
               | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE
               | FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION;
    if (ps_r_fsr_debug) desc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;

    const FfxErrorCode rc = ffxFsr3UpscalerContextCreate(&s_context, &desc);
    if (rc != FFX_OK) {
        Msg("![VK FSR] context create failed (0x%08X) for max render %ux%u -> display %ux%u",
            (unsigned)rc, displayW, displayH, displayW, displayH);
        s_failed = true;
        return false;
    }
    s_ctxLive = true;
    s_ctxRenderW = displayW; s_ctxRenderH = displayH;
    s_ctxDisplayW = displayW;  s_ctxDisplayH = displayH;

    // The three downstream-sharing targets. ⚠Not optional: the dispatch registers
    // them unconditionally and a null there is an invalid-pointer error, not a
    // "skip that output". Allocated through the BACKEND (fpCreateResource), not
    // through our VMA — FSR owns their lifetime with the context.
    FfxFsr3UpscalerSharedResourceDescriptions shared = {};
    if (ffxFsr3UpscalerGetSharedResourceDescriptions(&s_context, &shared) == FFX_OK) {
        FfxCreateResourceDescription d0 = shared.dilatedDepth;
        FfxCreateResourceDescription d1 = shared.dilatedMotionVectors;
        FfxCreateResourceDescription d2 = shared.reconstructedPrevNearestDepth;
        const bool ok =
            s_iface.fpCreateResource(&s_iface, &d0, 0, &s_dilatedDepth)  == FFX_OK &&
            s_iface.fpCreateResource(&s_iface, &d1, 0, &s_dilatedMVs)    == FFX_OK &&
            s_iface.fpCreateResource(&s_iface, &d2, 0, &s_prevNearDepth) == FFX_OK;
        s_sharedLive = ok;
        if (!ok) Msg("![VK FSR] shared resource create failed — dispatch will be skipped");
    }

    FfxEffectMemoryUsage vram = {};
    ffxFsr3UpscalerContextGetGpuMemoryUsage(&s_context, &vram);
    Msg("~[VK FSR] context ready: render %ux%u -> display %ux%u, VRAM %.1f MB (total %.1f)",
        displayW, displayH, displayW, displayH,
        double(vram.totalUsageInBytes) / (1024.0 * 1024.0),
        double(vram.aliasableUsageInBytes) / (1024.0 * 1024.0));
    return true;
}

} // anonymous namespace

// ============================================================================
bool Init()
{
    if (s_inited) return true;
    if (s_failed) return false;
    if (VulkanHW.m_Device == VK_NULL_HANDLE || VulkanHW.m_PhysicalDevice == VK_NULL_HANDLE) {
        Msg("![VK FSR] device not ready — skipping init");
        return false;
    }

    // maxContexts 2: the upscaler is one, and the backend counts its own internal
    // contexts towards this. One is enough today and two costs a few KB of scratch.
    constexpr size_t kMaxContexts = 2;
    const size_t scratchSize = ffxGetScratchMemorySizeVK(VulkanHW.m_PhysicalDevice, kMaxContexts);
    s_scratch = xr_malloc(scratchSize);
    if (!s_scratch) { Msg("![VK FSR] out of memory for the %zu-byte backend scratch", scratchSize); s_failed = true; return false; }
    ZeroMemory(s_scratch, scratchSize);

    VkDeviceContext vkCtx = {};
    vkCtx.vkDevice         = VulkanHW.m_Device;
    vkCtx.vkPhysicalDevice = VulkanHW.m_PhysicalDevice;
    vkCtx.vkDeviceProcAddr = vkGetDeviceProcAddr;
    s_device = ffxGetDeviceVK(&vkCtx);

    const FfxErrorCode rc = ffxGetInterfaceVK(&s_iface, s_device, s_scratch, scratchSize, kMaxContexts);
    if (rc != FFX_OK) {
        Msg("![VK FSR] ffxGetInterfaceVK failed (0x%08X)", (unsigned)rc);
        xr_free(s_scratch); s_scratch = nullptr;
        s_failed = true;
        return false;
    }

    ffxFsr3UpscalerSetGlobalDebugMessage(nullptr, 0);
    s_inited = true;
    ps_r_fsr_avail = 1;

    const FfxVersionNumber v = ffxFsr3UpscalerGetEffectVersion();
    Msg("[VK FSR] FidelityFX Super Resolution %u.%u.%u ready (Vulkan backend, %.1f KB scratch)",
        (v >> 22) & 0x3FFu, (v >> 12) & 0x3FFu, v & 0xFFFu, double(scratchSize) / 1024.0);
    return true;
}

void Shutdown()
{
    if (!s_inited) return;
    VK_DeviceWaitIdleLocked();
    DestroyContext();
    if (s_outView) { vkDestroyImageView(VulkanHW.m_Device, s_outView, nullptr); s_outView = VK_NULL_HANDLE; }
    if (s_outImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_outImg, s_outAlloc); s_outImg = VK_NULL_HANDLE; s_outAlloc = VK_NULL_HANDLE; }
    s_outW = s_outH = 0;
    if (s_scratch) { xr_free(s_scratch); s_scratch = nullptr; }
    s_inited = false;
    ps_r_fsr_avail = 0;
    Msg("[VK FSR] shut down");
}

bool Inited()    { return s_inited; }
bool Available() { return s_inited && !s_failed; }

// ⚠MUTUALLY EXCLUSIVE WITH DLSS, and DLSS wins. Both write a display-res output
// and both own the jitter sequence; running them together would apply two jitters
// to one frame and leave the tonemap picking a base colour by luck. On the machine
// this was written for the question never arises (NGX cannot init on AMD), but a
// laptop with both a dGPU and an iGPU can flip between them across restarts.
bool Enabled() { return ps_r_fsr != 0 && Available() && !VK::Dlss::Upscaling(); }

void GetRenderExtent(u32 displayW, u32 displayH, u32& outW, u32& outH)
{
    outW = displayW; outH = displayH;
    if (!Available() || ps_r_fsr == 0) return;

    const FfxFsr3UpscalerQualityMode mode = QualityMode();
    if (mode == FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA) return;   // 1.0x — our DLAA

    uint32_t rw = displayW, rh = displayH;
    if (ffxFsr3UpscalerGetRenderResolutionFromQualityMode(&rw, &rh, displayW, displayH, mode) != FFX_OK)
        return;
    // ⚠Even dimensions, for exactly the reason r_render_scale documents: half-res
    // passes (bloom, SSAO) derive their extent by halving this one, and an odd
    // width there costs a column of pixels.
    outW = rw & ~1u;
    outH = rh & ~1u;
}

bool Upscaling()
{
    if (!Enabled()) return false;
    return QualityMode() != FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
}

void EnsureOutput(u32 displayW, u32 displayH)
{
    if (!s_inited) return;
    if (s_outImg != VK_NULL_HANDLE && displayW == s_outW && displayH == s_outH) return;
    if (s_outView) { vkDestroyImageView(VulkanHW.m_Device, s_outView, nullptr); s_outView = VK_NULL_HANDLE; }
    if (s_outImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_outImg, s_outAlloc); s_outImg = VK_NULL_HANDLE; s_outAlloc = VK_NULL_HANDLE; }
    if (displayW == 0 || displayH == 0) return;

    // STORAGE for FSR's final write, SAMPLED for the tonemap. TRANSFER_DST for the
    // same reason DLSS.Output carries it — an upscaler that clears its own output
    // makes that clear illegal without the flag, and the resulting VUID names this
    // image rather than the pass that issued the clear.
    if (!VK::CreateImage2D(kOutFmt, { displayW, displayH },
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            s_outImg, s_outAlloc, "FSR.Output"))
        return;
    s_outView = VK::CreateImageView(s_outImg, kOutFmt);
    if (s_outView == VK_NULL_HANDLE) {
        VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_outImg, s_outAlloc); s_outImg = VK_NULL_HANDLE; return;
    }
    s_outW = displayW; s_outH = displayH;
    Msg("[VK FSR] output target ready (%ux%u RGBA16F) img=%p", displayW, displayH, (void*)s_outImg);
}

VkImageView OutputView()   { return s_outView; }
VkImage     OutputImage()  { return s_outImg; }
VkFormat    OutputFormat() { return kOutFmt; }
bool        OutputReady()  { return s_outImg != VK_NULL_HANDLE; }

bool ResolvedThisFrame()       { return s_resolved; }
void SetResolvedThisFrame(bool ok) { s_resolved = ok; }
void SetExposureHint(float e)  { if (e > 0.f) s_exposure = e; }

void NewFrameJitter(u32 renderW, u32 renderH, u32 displayW)
{
    s_resolved = false;   // set again only by a dispatch that succeeds this frame
    s_jitterW = renderW; s_jitterH = renderH;

    // ⚠The phase count is a function of the RATIO, not a constant. FSR needs the
    // sequence length it computed for this ratio; a DLSS-length sequence here
    // reads as a soft, slowly-crawling image rather than an obvious break, which
    // is the kind of bug that survives a whole session.
    const int count = ffxFsr3UpscalerGetJitterPhaseCount((int)renderW, (int)(displayW ? displayW : renderW));
    s_jitterCount = (count > 0) ? count : 1;
    s_jitterIndex = (s_jitterIndex + 1) % s_jitterCount;
    // FSR's own Halton generator — 1-based index, output already in [-0.5, 0.5].
    if (ffxFsr3UpscalerGetJitterOffset(&s_jitterPixX, &s_jitterPixY, s_jitterIndex, s_jitterCount) != FFX_OK) {
        s_jitterPixX = s_jitterPixY = 0.f;
    }
}

void GetJitterPix(float& outX, float& outY) { outX = s_jitterPixX; outY = s_jitterPixY; }

void GetProjJitterNDC(float& outX, float& outY)
{
    const float w = s_jitterW ? float(s_jitterW) : 1.0f;
    const float h = s_jitterH ? float(s_jitterH) : 1.0f;
    outX = 2.0f * s_jitterPixX / w;
    outY = 2.0f * s_jitterPixY / h;
}

bool TakeHistoryReset()
{
    bool reset = false;
    if (!s_evalChain || RV().frameIndex != s_lastEvalFrame + 1) reset = true;   // gap in the eval chain
    if (Device.dwPrecacheFrame != 0)                           reset = true;   // level (re)load

    const Fvector cam = RV().vCameraPosition;
    if (s_camPosValid && cam.distance_to_sqr(s_lastCamPos) > kCamCutDistSq) reset = true;

    s_lastCamPos = cam; s_camPosValid = true;
    s_lastEvalFrame = RV().frameIndex; s_evalChain = true;
    return reset;
}

bool Evaluate(VkCommandBuffer cmd, const Img& color, const Img& depth, const Img& mv,
              u32 renderW, u32 renderH, u32 displayW, u32 displayH, bool reset)
{
    if (!Available() || cmd == VK_NULL_HANDLE) return false;
    // ⚠⚠NOTHING IS BUILT HERE. Evaluate runs mid-recording, and the one thing that
    // must never happen mid-recording is a teardown of objects this frame command
    // buffer already references. Context and output are made by OnFrameBegin, which
    // runs before anything in the frame has been bound. If they are not ready this
    // frame, the honest answer is false and the tonemap keeps its native path.
    if (!s_ctxLive) return false;
    if (!s_sharedLive) return false;
    if (!OutputReady()) return false;
    if (color.image == VK_NULL_HANDLE || depth.image == VK_NULL_HANDLE || mv.image == VK_NULL_HANDLE) return false;

    // ⭐⭐THE STATE WE DECLARE HERE IS THE STATE WE GET BACK. The VK backend records
    // each app resource's `initialState` on register and UnregisterResourcesVK
    // (called at the end of every dispatch, ffx_fsr3upscaler.cpp:1174) walks them
    // all back to it and flushes the barriers. Declaring UNORDERED_ACCESS — which
    // this backend maps to VK_IMAGE_LAYOUT_GENERAL — is therefore exactly the seam
    // contract in vk_fsr.h: the caller hands us GENERAL and gets GENERAL back, and
    // its post-evaluate barriers can keep using GENERAL as oldLayout.
    // ⚠Get this wrong and nothing crashes: the caller's next barrier simply has a
    // stale oldLayout, which is a validation error and undefined contents.
    constexpr FfxResourceStates kGeneral = FFX_RESOURCE_STATE_UNORDERED_ACCESS;

    auto wrap = [&](const Img& i, u32 w, u32 h, const wchar_t* name) -> FfxResource {
        VkImageCreateInfo ci = {};
        ci.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType   = VK_IMAGE_TYPE_2D;
        ci.format      = i.format;
        ci.extent      = { w, h, 1 };
        ci.mipLevels   = 1;
        ci.arrayLayers = 1;
        ci.samples     = VK_SAMPLE_COUNT_1_BIT;
        return ffxGetResourceVK((void*)i.image, ffxGetImageResourceDescriptionVK(i.image, ci), name, kGeneral);
    };

    FfxFsr3UpscalerDispatchDescription d = {};
    d.commandList   = ffxGetCommandListVK(cmd);
    d.color         = wrap(color, renderW, renderH, L"FSR_InputColor");
    d.depth         = wrap(depth, renderW, renderH, L"FSR_InputDepth");
    d.motionVectors = wrap(mv,    renderW, renderH, L"FSR_InputMotionVectors");
    d.exposure      = FfxResource();   // AUTO_EXPOSURE — FSR derives it from the colour
    d.reactive      = FfxResource();
    d.transparencyAndComposition = FfxResource();

    d.dilatedDepth                = s_iface.fpGetResource(&s_iface, s_dilatedDepth);
    d.dilatedMotionVectors        = s_iface.fpGetResource(&s_iface, s_dilatedMVs);
    d.reconstructedPrevNearestDepth = s_iface.fpGetResource(&s_iface, s_prevNearDepth);

    {
        VkImageCreateInfo ci = {};
        ci.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType   = VK_IMAGE_TYPE_2D;
        ci.format      = kOutFmt;
        ci.extent      = { displayW, displayH, 1 };
        ci.mipLevels   = 1;
        ci.arrayLayers = 1;
        ci.samples     = VK_SAMPLE_COUNT_1_BIT;
        d.output = ffxGetResourceVK((void*)s_outImg,
                                    ffxGetImageResourceDescriptionVK(s_outImg, ci, FFX_RESOURCE_USAGE_UAV),
                                    L"FSR_Output", kGeneral);
    }

    d.jitterOffset.x = s_jitterPixX;
    d.jitterOffset.y = s_jitterPixY;

    // ⭐MOTION VECTOR SCALE — the one number that silently ruins a temporal
    // upscaler. motion_vec.frag writes (prevUV - curUV) in [0,1] UV space (its own
    // header line 19 says so), and FSR wants RENDER PIXELS with the same
    // current->previous direction. So the scale is simply the render dimensions,
    // with no sign flip: a flip here does not break the image, it makes every
    // moving edge smear the wrong way, which reads as "FSR is blurry".
    d.motionVectorScale.x = float(renderW);
    d.motionVectorScale.y = float(renderH);

    d.renderSize.width   = renderW;
    d.renderSize.height  = renderH;
    d.upscaleSize.width  = displayW;
    d.upscaleSize.height = displayH;

    d.enableSharpening = ps_r_fsr_sharp > 0.001f;
    d.sharpness        = ps_r_fsr_sharp;
    // ⚠MILLISECONDS. FSR uses this to age its accumulation; handing it seconds
    // (fTimeDelta) makes the history behave as if the game ran at 1000x, which
    // shows up as ghosting that never resolves.
    d.frameTimeDelta   = RV().fTimeDelta * 1000.0f;
    d.preExposure      = 1.0f;
    d.reset            = reset;
    // ⭐Near/far are READ BACK OUT OF THE PROJECTION we actually rendered with,
    // not asked of the weather. far_plane_render is the number the projection is
    // BUILT from, and between the two sits rs_vis_distance plus whatever the
    // camera manager did with it - so sourcing it separately is one more thing
    // that can silently disagree with the depth buffer FSR is about to linearize.
    // D3D-style perspective (this engine has NO reversed-Z): _33 = f/(f-n),
    // _43 = -n*f/(f-n)  =>  n = -_43/_33,  f = -_43/(_33-1).
    const float p33 = RV().mProject._33, p43 = RV().mProject._43;
    const bool  projOk = (p33 > 1.0001f) && (p43 < 0.f);
    d.cameraNear       = projOk ? (-p43 / p33) : VIEWPORT_NEAR;
    d.cameraFar        = projOk ? (-p43 / (p33 - 1.0f)) : 1000.0f;
    d.cameraFovAngleVertical = deg2rad(RV().fFOV);
    d.viewSpaceToMetersFactor = 1.0f;   // this engine's world unit IS a metre
    d.flags = 0;

    const FfxErrorCode rc = ffxFsr3UpscalerContextDispatch(&s_context, &d);
    if (rc != FFX_OK) {
        // Throttled: a per-frame failure would bury the log, and the first line is
        // the one that matters.
        static u32 s_lastComplaint = 0;
        if (RV().frameIndex - s_lastComplaint > 300) {
            s_lastComplaint = RV().frameIndex;
            Msg("![VK FSR] dispatch failed (0x%08X) at render %ux%u -> %ux%u", (unsigned)rc, renderW, renderH, displayW, displayH);
        }
        return false;
    }
    return true;
}

// Called from CRender::Begin, next to the other frame-start resizes. ⭐This is the
// ONLY place the context and the output target are created or destroyed: at this
// point in the frame nothing of ours is bound to the command buffer yet, so a
// teardown here cannot invalidate it the way one from Evaluate did.
void OnFrameBegin(u32 displayW, u32 displayH)
{
    if (!Available() || ps_r_fsr == 0) return;
    if (!EnsureContext(displayW, displayH)) return;
    EnsureOutput(displayW, displayH);
}

bool DescribeState(char* buf, u32 bufSize)
{
    if (!buf || bufSize == 0) return false;
    if (!Available()) { xr_sprintf(buf, bufSize, "FSR not available on this device"); return false; }
    static const char* kMode[5] = { "NativeAA (1.0x)", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2.0x)", "Ultra performance (3.0x)" };
    xr_sprintf(buf, bufSize, "r_fsr %d, %s, sharpen %.2f | render %ux%u -> display %ux%u | resolved=%d",
               ps_r_fsr, kMode[(int)QualityMode()], ps_r_fsr_sharp,
               s_ctxRenderW, s_ctxRenderH, s_ctxDisplayW, s_ctxDisplayH, (int)s_resolved);
    return true;
}

}} // namespace VK::Fsr

#endif // _WIN32

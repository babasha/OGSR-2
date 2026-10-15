// xrRenderVulkan — NVIDIA DLSS 4.5 (NGX) integration. See vk_dlss.h.
//
// Increment 1: initialise NGX on the live VK device and probe capabilities. The
// required NGX device extensions (VK_NVX_binary_import, VK_NVX_image_view_handle,
// VK_KHR_push_descriptor) are already enabled at device creation (vk_core.h
// g_NgxDeviceExtensions → HW_Vulkan). Frame-Generation will need MORE extensions
// (optical flow / present-id) — added when the DLSS-G feature lands.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_dlss.h"

#if defined(__aarch64__)
// NVIDIA ships NGX as a static lib for Windows and Linux x86_64 only. On arm64 (the Switch
// rehearsal target) DLSS is never available — the same state a failed Init() leaves on x64
// (Available() false), so every seam in CRender_Vulkan takes its native-res path: render
// extent == display, no jitter, the tonemap never samples an upscaler output.
extern int ps_r_dlss_avail;
extern int ps_r_dlss_rr_avail;
extern int ps_r_dlss_nr_avail;
namespace VK { namespace Dlss {
bool Init()
{
    ps_r_dlss_avail = ps_r_dlss_rr_avail = ps_r_dlss_nr_avail = 0;
    Msg("[VK DLSS] NGX is not available on this platform (no arm64 SDK)");
    return false;
}
void Shutdown() {}
bool Inited() { return false; }
bool SuperResAvailable() { return false; }
bool FrameGenAvailable() { return false; }
bool Enabled() { return false; }
void GetRenderExtent(u32 displayW, u32 displayH, u32& outW, u32& outH) { outW = displayW; outH = displayH; }
bool Upscaling() { return false; }
void EnsureOutput(u32, u32) {}
VkImageView OutputView() { return VK_NULL_HANDLE; }
VkImage OutputImage() { return VK_NULL_HANDLE; }
VkFormat OutputFormat() { return VK_FORMAT_UNDEFINED; }
bool OutputReady() { return false; }
bool FeatureReady() { return false; }
void NewFrameJitter(u32, u32) {}
void GetProjJitterNDC(float& outX, float& outY) { outX = outY = 0.f; }
void GetJitterPix(float& outX, float& outY) { outX = outY = 0.f; }
bool EnsureFeature(VkCommandBuffer, u32, u32, u32, u32, u32) { return false; }
void ReleaseFeature() {}
bool EnsureFeatureSL(u32, u32, u32, u32) { return false; }
bool ResolvedThisFrame() { return false; }
void SetResolvedThisFrame(bool) {}
bool TakeHistoryReset() { return true; }
bool Evaluate(VkCommandBuffer, const Img&, const Img&, const Img&, u32, u32, bool) { return false; }
void SetExposureHint(float) {}
u32 VramReserveMB() { return 0; }
bool FeatureBlocked() { return false; }
bool RayReconAvailable() { return false; }
bool RayReconEnabled() { return false; }
bool EnsureFeatureRR(VkCommandBuffer, u32, u32, u32, u32, u32) { return false; }
bool EvaluateRR(VkCommandBuffer, const Img&, const Img&, const Img&, const Img&, const Img&, const Img&, u32, u32, bool) { return false; }
bool NeuralRenderAvailable() { return false; }
bool NeuralRenderEnabled() { return false; }
bool EnsureFeatureNR(VkCommandBuffer, u32, u32, u32, u32, u32) { return false; }
bool EvaluateNR(VkCommandBuffer, const Img&, const Img&, const Img&, u32, u32, bool) { return false; }
}} // namespace VK::Dlss
#else

#include "HW_Vulkan.h"
#include "vk_image.h"      // VK::CreateImage2D / CreateImageView
#include "vk_barriers.h"       // ImageBarrier — transition DLSS resources to GENERAL for NGX
#include "../../xr_3da/device.h"   // RV().frameIndex / dwPrecacheFrame / vCameraPosition — history-reset detection

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers.h"        // NVSDK_NGX_Parameter_GetI/GetUI wrappers
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_defs_dlssg.h"     // NVSDK_NGX_Parameter_FrameGeneration_* (MFG)
#include "nvsdk_ngx_helpers_dlssd_vk.h"  // NGX_VULKAN_{CREATE,EVALUATE}_DLSSD — Ray Reconstruction
#include "vk_rr_guides.h"             // VK::RRGuide — the albedo/normal/roughness G-buffer RR runs on
#include "vk_texture_stream.h"        // VK::TextureStreamer::FreeDeviceVram — the room gate asks it for the memory back
#include <atomic>                     // s_ngxReserveMB — read from the streamer's worker threads

extern u32 ps_r_dlss;           // console toggle (vk_console_min.cpp, global scope)
extern u32 ps_r_dlss_quality;   // 0=DLAA 1=Quality 2=Balanced 3=Performance 4=UltraPerf
extern u32 ps_r_dlss_preset;    // model preset hint: 0=driver default, 6=F(CNN), 10=J, 11=K (transformer)
extern int ps_r_dlss_exp;         // 1 = real exposure via 1x1 texture (default), 0 = NGX AutoExposure flag (old)
extern int ps_r_dlss_avail;       // r_dlss_avail — this probe's answer, published for the OPTIONS SCREEN to gate its DLSS group on
extern int ps_r_dlss_rr;          // r_dlss_rr — Ray Reconstruction master toggle (overrides Super Resolution)
extern u32 ps_r_dlss_rr_preset;   // r_dlss_rr_preset — RR model hint: 0=driver default, 4=D, 5=E (RR has its OWN preset enum)
extern int ps_r_dlss_rr_avail;    // r_dlss_rr_avail — the RR capability probe's answer, published like r_dlss_avail
extern int ps_r_dlss_nr;          // r_dlss_nr — Neural Rendering master toggle (overrides RR *and* Super Resolution)
extern u32 ps_r_dlss_nr_featid;   // r_dlss_nr_featid — NGX feature ID to create; no public constant exists for this network
extern u32 ps_r_dlss_nr_preset;   // r_dlss_nr_preset — DLSSNR.Hint.Render.Preset, 0 = driver default
extern int ps_r_dlss_nr_avail;    // r_dlss_nr_avail — published like r_dlss_rr_avail
extern u32 ps_r_dlss_nr_style;        // r_dlss_nr_style — DLSSNR.Style
extern u32 ps_r_dlss_nr_cfg;      // r_dlss_nr_cfg — bitmask of WHICH creation keys to send (the create-parameter sweep)
extern int ps_r_dlss_nr_debug;    // r_dlss_nr_debug — pipe NGX's own log (the network's only voice) into ours
extern float ps_r_dlss_nr_intensity;  // look knobs; < 0 = do not send the key, keep the network's own default
extern float ps_r_dlss_nr_structure;
extern float ps_r_dlss_nr_tone;
extern float ps_r_dlss_nr_skin;
extern int ps_r_dlss_vram;        // r_dlss_vram — MB of VRAM one NGX feature is assumed to need (0 = estimate from the display size)

namespace VK { namespace Dlss {

namespace {
    // Map r_dlss_quality → NGX perf/quality preset. Quality 0 = DLAA (native res AA).
    NVSDK_NGX_PerfQuality_Value PerfQualityForCvar()
    {
        switch (ps_r_dlss_quality) {
            case 1:  return NVSDK_NGX_PerfQuality_Value_MaxQuality;       // ~0.67x
            case 2:  return NVSDK_NGX_PerfQuality_Value_Balanced;         // ~0.58x
            case 3:  return NVSDK_NGX_PerfQuality_Value_MaxPerf;          // ~0.50x
            case 4:  return NVSDK_NGX_PerfQuality_Value_UltraPerformance; // ~0.33x
            default: return NVSDK_NGX_PerfQuality_Value_DLAA;             // native
        }
    }
}

namespace {
    bool s_inited  = false;
    bool s_srAvail = false;
    bool s_fgAvail = false;
    bool s_rrAvail = false;
    bool s_nrAvail = false;
    NVSDK_NGX_Parameter* s_caps = nullptr;

    // App/project identity for NGX telemetry. Any stable GUID string is fine for a
    // custom engine (no NVIDIA-assigned app id); ENGINE_TYPE_CUSTOM + our version.
    const char* kProjectId    = "b7f4e2a1-9c3d-4f6b-8a12-0e5d7c9a3b21";
    const char* kEngineVer     = "1.0";

    // NGX's log sink. ⚠Must be thread-safe and must not keep the pointer: NGX
    // documents that the string may be gone the moment this returns, and that any
    // number of threads may call it at once. Msg copies, so forwarding is enough.
    // ⚠The HEAD of the stream comes through unfiltered, on purpose. A fully filtered
    // sink that prints nothing is ambiguous — it cannot tell "NGX never called us"
    // apart from "NGX called us and nothing matched", and those two have completely
    // different fixes. Printing the first 150 settles it. After that only the lines
    // about this network get through: VERBOSE NGX is thousands of lines a second, and
    // a log nobody can read is the same as no log.
    volatile long s_ngxLogSeen = 0;
    void NVSDK_CONV NgxLogSink(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
    {
        if (message == nullptr) return;
        const long n = _InterlockedIncrement(&s_ngxLogSeen);
        const bool interesting = strstr(message, "DLSSNR") != nullptr
                              || strstr(message, "Skip feature") != nullptr;
        if (n <= 150 || interesting) Msg("[NGX #%ld] %s", n, message);
    }
}

bool Inited()             { return s_inited; }
bool SuperResAvailable()  { return s_inited && s_srAvail; }
bool FrameGenAvailable()  { return s_inited && s_fgAvail; }
// ⚠The master gate is "some DLSS feature can run", NOT "Super Resolution can run".
// It read s_srAvail alone, which quietly made Ray Reconstruction depend on SR being
// available. Harmless on real hardware (any GPU with RR has SR) but a false
// coupling — the kind that becomes a mystery on the day it finally bites.
bool Enabled()            { return s_inited && ps_r_dlss != 0 && (s_srAvail || (s_rrAvail && ps_r_dlss_rr != 0)); }

bool Init()
{
    if (s_inited) return true;
    if (VulkanHW.m_Instance == VK_NULL_HANDLE || VulkanHW.m_Device == VK_NULL_HANDLE) {
        Msg("![VK DLSS] device not ready — skipping NGX init");
        return false;
    }
    if (!g_bNgxExtensionsEnabled)
        Msg("~[VK DLSS] not all NGX device extensions were enabled — init may fail");

    // NGX looks for the feature snippet DLLs (nvngx_dlss.dll / nvngx_dlssg.dll) next
    // to the exe by default; app data path L"." = the working dir (bin_x64).
    // ⭐Pipe NGX's OWN log into ours (r_dlss_nr_debug). The DLSS-NR network reports
    // what it actually did only here — including the two lines that matter most:
    // "DLSSNR: Skip feature evaluate: Invalid ... rect configuration" and
    // "DLSSNR: enabled=%d localTone=%f localStructure=%f intensity=%f". Without it a
    // network that quietly passes the frame straight through is indistinguishable
    // from one that ran, because EvaluateFeature returns Success in both cases.
    static NVSDK_NGX_FeatureCommonInfo s_ngxCommon{};
    if (ps_r_dlss_nr_debug) {
        s_ngxCommon.LoggingInfo.LoggingCallback          = &NgxLogSink;
        s_ngxCommon.LoggingInfo.MinimumLoggingLevel      = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
        s_ngxCommon.LoggingInfo.DisableOtherLoggingSinks = false;
    }
    Msg("[VK DLSS] NGX init: logging sink %s (r_dlss_nr_debug %d)",
        ps_r_dlss_nr_debug ? "REQUESTED" : "off", ps_r_dlss_nr_debug);
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVer, L".",
        VulkanHW.m_Instance, VulkanHW.m_PhysicalDevice, VulkanHW.m_Device,
        vkGetInstanceProcAddr, vkGetDeviceProcAddr,
        ps_r_dlss_nr_debug ? &s_ngxCommon : nullptr, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        Msg("![VK DLSS] NGX_VULKAN_Init failed: 0x%08X (no RTX GPU / driver / DLLs?)", (unsigned)r);
        return false;
    }

    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&s_caps);
    if (NVSDK_NGX_FAILED(r) || s_caps == nullptr) {
        Msg("![VK DLSS] GetCapabilityParameters failed: 0x%08X", (unsigned)r);
        NVSDK_NGX_VULKAN_Shutdown1(VulkanHW.m_Device);
        return false;
    }

    int srAvail = 0, srNeedsDrv = 0, srDrvMaj = 0, srDrvMin = 0;
    int fgAvail = 0, fgNeedsDrv = 0, fgDrvMaj = 0, fgDrvMin = 0;
    int rrAvail = 0, rrNeedsDrv = 0, rrDrvMaj = 0, rrDrvMin = 0;
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_SuperSampling_Available,               &srAvail);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,      &srNeedsDrv);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor,   &srDrvMaj);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor,   &srDrvMin);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_FrameGeneration_Available,             &fgAvail);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver,    &fgNeedsDrv);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &fgDrvMaj);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &fgDrvMin);
    // ⭐RR advertises itself under "SuperSamplingDenoising", not under any name with
    // "RayReconstruction" in it — the feature was called DLSS-D before it was
    // renamed for marketing, and the parameter strings kept the old identity.
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,               &rrAvail);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,      &rrNeedsDrv);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor,   &rrDrvMaj);
    NVSDK_NGX_Parameter_GetI(s_caps, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor,   &rrDrvMin);

    // ⚠Neural Rendering has NO documented capability key: the 310.8 snippet is
    // unreleased and nothing in this SDK names one. Probe the two spellings its
    // parameter namespace makes plausible — and treat silence as UNKNOWN, not as
    // "unavailable". A negative answer we never actually asked for would disable the
    // feature on hardware that supports it; the create call is the honest verdict,
    // and it latches after one refusal (EnsureFeatureNR) so letting it run is cheap.
    int nrAvail = 0;
    const char* nrKey = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(s_caps, "DLSSNR.Available", &nrAvail)))
        nrKey = "DLSSNR.Available";
    else if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(s_caps, "NeuralRendering.Available", &nrAvail)))
        nrKey = "NeuralRendering.Available";

    s_srAvail = (srAvail != 0);
    s_fgAvail = (fgAvail != 0);
    s_rrAvail = (rrAvail != 0);
    s_nrAvail = nrKey ? (nrAvail != 0) : true;
    s_inited  = true;
    // ⭐Publish the answer. The options screen has no way to call in here (it
    // lives in xrGame, a different module) and a DLSS row on a card that cannot
    // run it is the same placebo this pass is removing everywhere else.
    ps_r_dlss_avail = s_srAvail ? 1 : 0;
    ps_r_dlss_rr_avail = s_rrAvail ? 1 : 0;
    ps_r_dlss_nr_avail = s_nrAvail ? 1 : 0;

    Msg("[VK DLSS] NGX initialised.");
    Msg("[VK DLSS]   Super Resolution : %s%s (min driver %d.%d)",
        s_srAvail ? "AVAILABLE" : "unavailable",
        srNeedsDrv ? " [NEEDS NEWER DRIVER]" : "", srDrvMaj, srDrvMin);
    Msg("[VK DLSS]   Frame Generation : %s%s (min driver %d.%d)",
        s_fgAvail ? "AVAILABLE" : "unavailable",
        fgNeedsDrv ? " [NEEDS NEWER DRIVER]" : "", fgDrvMaj, fgDrvMin);
    Msg("[VK DLSS]   Ray Reconstruct. : %s%s (min driver %d.%d)",
        s_rrAvail ? "AVAILABLE" : "unavailable",
        rrNeedsDrv ? " [NEEDS NEWER DRIVER]" : "", rrDrvMaj, rrDrvMin);
    // Say which of the two it is. "AVAILABLE" from a probe that found no key at all
    // is a guess, and printing it as though it were an answer is how a later session
    // ends up debugging the wrong half of this.
    Msg("[VK DLSS]   Neural Rendering : %s (%s)",
        s_nrAvail ? "AVAILABLE" : "unavailable",
        nrKey ? nrKey : "no capability key answered - assumed, create will decide");
    return true;
}

// ============================================================================
// Super Resolution (increment 2): feature lifecycle + jitter + output + evaluate.
// ============================================================================
namespace {
    NVSDK_NGX_Parameter* s_params  = nullptr;   // feature create/eval parameter block
    NVSDK_NGX_Handle*    s_feature = nullptr;
    // Ray Reconstruction feature. Its own parameter block: NGX parameters are a flat
    // key/value bag keyed by string, and SR and RR share key names (Color, Depth,
    // MotionVectors...) while disagreeing on others — one bag for two features means
    // whichever evaluated last leaves its resource pointers behind for the other.
    NVSDK_NGX_Parameter* s_paramsRR  = nullptr;
    NVSDK_NGX_Handle*    s_featureRR = nullptr;
    u32 s_rrRenderW = 0, s_rrRenderH = 0, s_rrDispW = 0, s_rrDispH = 0;
    u32 s_rrPreset  = 0xFFFFFFFFu;
    u32 s_rrInputsGen = 0xFFFFFFFFu;   // scene-target generation the RR feature was built against
    u32 s_rrGuideGen  = 0xFFFFFFFFu;   // guide G-buffer generation ditto
    u32 s_rrExpMode   = 0xFFFFFFFFu;
    // Neural Rendering feature. Its own parameter bag for exactly the reason RR has
    // one — and more so here: NR keys are namespaced "DLSSNR.*" while SR/RR use the
    // bare names, so a shared bag would carry BOTH sets of stale resource pointers.
    NVSDK_NGX_Parameter* s_paramsNR  = nullptr;
    NVSDK_NGX_Handle*    s_featureNR = nullptr;
    u32 s_nrRenderW = 0, s_nrRenderH = 0, s_nrDispW = 0, s_nrDispH = 0;
    u32 s_nrPreset    = 0xFFFFFFFFu;
    u32 s_nrStyle     = 0xFFFFFFFFu;   // ⚠a CREATE-time key too — left out of the cache it would silently do nothing
    u32 s_nrInputsGen = 0xFFFFFFFFu;   // scene-target generation the NR feature was built against
    u32 s_nrFeatId    = 0xFFFFFFFFu;   // feature ID it was actually created with
    // (s_nrAvail lives with the other availability flags above — Init() sets it, and
    // Init() is compiled before this block.)
    // ⚠The create-failure latch, and the reason a wrong r_dlss_nr_featid is cheap.
    // EnsureFeatureNR runs every frame and its rebuild path calls vkDeviceWaitIdle;
    // retrying a refused feature ID once a frame would stall the renderer far worse
    // than the network itself ever could. Holds the ID that failed, so changing the
    // cvar clears it and nothing else does.
    u32 s_nrFailedId  = 0xFFFFFFFFu;
    u32 s_nrFailedCfg = 0xFFFFFFFFu;   // ...and the key set it was refused WITH: sweeping r_dlss_nr_cfg must re-arm the attempt
    u32 s_nrCfg       = 0xFFFFFFFFu;   // key set the live feature was built with

    // ⚠WHY THERE IS NO HAND-ROLLED SNIPPET LOADER HERE, having tried one:
    // nvngx_dlssnr.dll cannot be brought up by us, and both doors are shut from the
    // driver side, not by anything this code does.
    //   1. Through the runtime (what this file does): nvngx.dll resolves a feature ID
    //      to a snippet FILENAME through a table shipped with the driver. 616.56's
    //      table holds the wide strings "dlss", "dlssd" and "dlssg" and no "dlssnr",
    //      so no feature ID can name this file — hence 0xBAD0000B
    //      (UnableToInitializeFeature, "the library for that feature could not be
    //      found") identically for every ID tried. The ID was never the variable.
    //   2. Calling the snippet's own exported Init directly: it resolves ITS CALLER
    //      (GetModuleHandleExW on the return address, then GetModuleFileNameW) and
    //      compares the path against the wide string "nvngx.dll" in its own .rdata.
    //      Anything that is not the NGX core gets 0xBAD00002 (PlatformError) before
    //      it touches the device. Verified against a live run, not inferred.
    // So this stays on the ordinary runtime path, which is the one that starts
    // working the day a driver ships a "dlssnr" entry — no code change needed then.
    u32 s_fRenderW = 0, s_fRenderH = 0, s_fDispW = 0, s_fDispH = 0;
    u32 s_fPreset  = 0xFFFFFFFFu;   // preset the live feature was built with (r_dlss_preset)
    u32 s_fInputsGen = 0xFFFFFFFFu; // input-target generation the feature was built against
    u32 s_fExpMode = 0xFFFFFFFFu;   // exposure mode the feature was built with (r_dlss_exp)

    // Real-exposure path (r_dlss_exp 1): 1x1 R32F texture cleared to the tonemap's
    // CPU-smoothed exposure each Evaluate — NGX normalizes its internal processing
    // by it (AutoExposure flag dropped from the feature). Prevents the transformer's
    // black undershoot on high-contrast sub-pixel detail (distant crowns vs sky).
    float         s_expHint  = 1.0f;
    VkImage       s_expImg   = VK_NULL_HANDLE;
    VmaAllocation s_expAlloc = VK_NULL_HANDLE;
    VkImageView   s_expView  = VK_NULL_HANDLE;

    // Display-res upscaled output (RGBA16F; DLSS writes via storage, tonemap samples).
    VkImage       s_outImg   = VK_NULL_HANDLE;
    VmaAllocation s_outAlloc = VK_NULL_HANDLE;
    VkImageView   s_outView  = VK_NULL_HANDLE;
    const VkFormat s_outFmt  = VK_FORMAT_R16G16B16A16_SFLOAT;
    u32 s_outW = 0, s_outH = 0;

    // Halton(2,3) sub-pixel jitter, in render-pixel space [-0.5, 0.5].
    u32   s_jitterPhase  = 0;
    float s_jitterPixX   = 0.0f, s_jitterPixY = 0.0f;
    u32   s_jitterCount  = 16;
    // Render dims captured by NewFrameJitter, so GetProjJitterNDC can convert to NDC
    // even on the first frames (before EnsureFeature has set the feature dims).
    u32   s_jitterW      = 0, s_jitterH = 0;

    float Halton(u32 index, u32 base)
    {
        float f = 1.0f, r = 0.0f;
        for (u32 i = index; i > 0; i /= base) { f /= float(base); r += f * float(i % base); }
        return r;
    }

    VkImageSubresourceRange FullRange(VkImageAspectFlags aspect)
    {
        VkImageSubresourceRange r{};
        r.aspectMask = aspect; r.levelCount = 1; r.layerCount = 1;
        return r;
    }

    NVSDK_NGX_Resource_VK WrapImage(const VK::Dlss::Img& im, u32 w, u32 h, VkImageAspectFlags aspect, bool readWrite)
    {
        return NVSDK_NGX_Create_ImageView_Resource_VK(
            im.view, im.image, FullRange(aspect), im.format, w, h, readWrite);
    }

    // Evaluate failures used to be logged behind `static bool once` — which made a
    // PERSISTENT failure (the interesting kind: the card is out of memory and every
    // frame fails) indistinguishable from a single hiccup, and invisible after the
    // first line. Say it once immediately, then keep a running count on a slow
    // heartbeat so a run that is silently showing a stale frame still says so.
    // ⭐⭐⭐WHAT NGX COSTS IN VRAM, AND WHETHER IT HAD ROOM.
    //
    // NGX allocates its tensors OUTSIDE VMA, so not one of the [VK VRAM] buckets can
    // see them — they surface only as a jump in "untracked", and only if someone
    // happens to diff two 5-second lines. Read the device-local heap either side of
    // CreateFeature and say the number outright.
    //
    // ⚠And say the HEADROOM, because our own priority scheme has no seat for NGX:
    // every render target is pinned at priority 1.0 (vk_image.cpp), streamed textures
    // sit at 0.25 (vk_texture.cpp), and NGX — untaggable, outside VMA — gets the
    // spec's default 0.5. Under pressure the driver therefore spills the DLSS working
    // set BEFORE it touches our targets, and a resident-in-system-RAM upscaler runs
    // several times its true cost while looking, in every log, exactly like a slow
    // upscaler. 25-08: enabling DLSS mid-level at 94% of budget cost 23-27 ms/frame
    // for ~26 s before it settled to 4 ms on its own.
    double s_ngxHeapBefore = 0.0;
    void NgxHeapMark()
    {
        double budgetMB = 0.0;
        VK::Vram::HeapTotalsMB(s_ngxHeapBefore, budgetMB);
    }
    void NgxHeapReport(const char* who)
    {
        double usedMB = 0.0, budgetMB = 0.0;
        VK::Vram::HeapTotalsMB(usedMB, budgetMB);
        const double tookMB = usedMB - s_ngxHeapBefore;
        const double freeMB = budgetMB - usedMB;
        Msg("[%s] NGX footprint: +%.0f MB outside VMA (device-local now %.0f / %.0f budget, "
            "headroom %.0f MB)", who, tookMB, usedMB, budgetMB, freeMB);
        // 384 MB is not a tuned constant — it is roughly what this feature just took,
        // i.e. "there is not a second one's worth of room". Treat it as the line below
        // which the driver has to start choosing, and it does not choose us.
        if (freeMB < 384.0)
            Msg("~[%s] only %.0f MB of VRAM headroom — NGX allocates outside VMA and carries the "
                "DEFAULT eviction priority (0.5) while every render target is pinned at 1.0, so "
                "this is the working set the driver spills first. Expect the upscale pass to cost "
                "several times its true price until the streamers settle. Enable DLSS BEFORE "
                "loading the level, or raise r_txstream_headroom / lower r_clpage_budget.", who, freeMB);
    }

    // ⚠⚠AND WHY THAT NUMBER IS ALWAYS "+0 MB". NGX does not commit its tensors inside
    // CreateFeature — the first Evaluate does. Every run in the log, the healthy ones
    // and the one that collapsed alike, therefore reports +0 and the footprint question
    // stayed unanswered for a week. Re-read the heap some frames later, once the commit
    // has actually happened, and print the figure the estimate below wants replacing by.
    u32    s_ngxLateFrame  = 0;          // frame to re-read on (0 = nothing pending)
    double s_ngxLateBefore = 0.0;
    char   s_ngxLateWho[12] = {0};
    void NgxHeapReportLater(const char* who)
    {
        s_ngxLateFrame  = RV().frameIndex + 120;
        s_ngxLateBefore = s_ngxHeapBefore;
        _snprintf(s_ngxLateWho, sizeof(s_ngxLateWho) - 1, "%s", who);
    }
    void NgxHeapTick()
    {
        if (!s_ngxLateFrame || RV().frameIndex < s_ngxLateFrame) return;
        s_ngxLateFrame = 0;
        double usedMB = 0.0, budgetMB = 0.0;
        VK::Vram::HeapTotalsMB(usedMB, budgetMB);
        Msg("[%s] NGX footprint after 120 frames: +%.0f MB outside VMA since create "
            "(device-local now %.0f / %.0f budget, headroom %.0f MB)",
            s_ngxLateWho, usedMB - s_ngxLateBefore, usedMB, budgetMB, budgetMB - usedMB);
    }

    // ⭐⭐⭐THE ROOM GATE, AND THE RUN THAT BOUGHT IT (28-08 19:37 log).
    //
    // `r_dlss 1` typed 22 minutes into a level, with 1450 MB of headroom left. The RR
    // feature CREATED fine, evaluated fine, and reported no error of any kind — and the
    // frame went 12 ms -> 240 ms (80 -> 4 fps) with 168 stalls, every one of them parked
    // in CRender::Begin on the frame fence. Four runs the same afternoon built the SAME
    // feature at ~5850 MB of headroom and paid +10 ms for it. The difference was never
    // the upscaler: with nowhere to live, NGX's working set is paged over PCIe every
    // frame, and (see the note above) nothing in this renderer holds a seat for it.
    //
    // ⚠So do not build a feature into a card that cannot hold it. Ask the streamer for
    // the memory FIRST, wait for the demotes to land, and if it never comes say so out
    // loud — silently charging the player 20x their frame time is the one outcome worth
    // any amount of code to avoid. ⚠And keep the seat once the feature is live:
    // VramReserveMB() below is what stops the streamer refilling the card behind it.
    u32 NgxNeedMB(u32 dispW, u32 dispH, bool heavy)
    {
        if (ps_r_dlss_vram > 0) return (u32)ps_r_dlss_vram;   // pinned by hand
        // ⚠⚠THIS IS NOT THE FEATURE'S FOOTPRINT — it is how much free VRAM the feature
        // needs to find in order to run at its true cost, which is a LARGER number and
        // the only one that predicts anything. Nobody knows the footprint: NGX allocates
        // outside VMA and the driver's usage counter barely reflects it (23-08 03:20, RR
        // live at 1440p: 526 MB of untracked for the WHOLE renderer). What the logs do
        // give is the free-memory line between working and not, at 2560x1440 RR:
        //     headroom 3656..5860 MB → +10 ms, 34-118 fps        (six runs, 26-08..28-08)
        //     headroom 1450 MB       → +228 ms, 4 fps            (28-08 19:37)
        //     streamer left ~490 MB  → 3 fps, GPU still only 36 ms (23-08 03:20)
        // So 1440p RR wants MORE than 1450 MB: 450 MB per display megapixel puts the
        // line at ~1580 MB, just above the case that collapsed, and ~2x below the
        // cheapest headroom that was ever observed working. SR at 150 is the same
        // ratio the two features' networks stand in.
        // ⚠A calibration, not a measurement — r_dlss_vram overrides it, and the
        // deferred footprint report above is what a real number would come from.
        const double mpix = (double(dispW) * double(dispH)) / (1024.0 * 1024.0);
        const u32 mb = (u32)(mpix * (heavy ? 450.0 : 150.0) + 0.5);
        return mb < 256u ? 256u : mb;
    }

    std::atomic<u32> s_ngxReserveMB{0};   // VRAM the LIVE feature needs kept free (0 = none)
    // ⚠The gate can hold a feature off for seconds at a time, and CRender::Begin jitters
    // the camera a whole frame BEFORE End() finds out whether anything will resolve it.
    // Left alone that is a shimmering image with no upscaler behind it — strictly worse
    // than plain native. Published so Begin can stop jittering while the gate is shut.
    bool s_roomBlocked = false;
    u32 s_roomTries     = 0;
    u32 s_roomNextFrame = 0;

    bool HaveRoomForNgx(const char* who, u32 needMB)
    {
        double usedMB = 0.0, budgetMB = 0.0;
        VK::Vram::HeapTotalsMB(usedMB, budgetMB);
        if (budgetMB <= 0.0) return true;    // driver told us nothing — do not block on a guess
        const double freeMB = budgetMB - usedMB;
        if (freeMB >= double(needMB)) { s_roomTries = 0; s_roomBlocked = false; return true; }

        s_roomBlocked = true;
        if (RV().frameIndex < s_roomNextFrame) return false;   // a demote plan is still in flight
        // 20 frames between attempts: the demotes ride the async IO path and the retire
        // ring only hands the old images back to the OS a few frames after that.
        s_roomNextFrame = RV().frameIndex + 20;
        if (++s_roomTries > 8) {
            // Backed off, not given up — the level keeps streaming and the answer can
            // change. ~15 s before asking again, and ONE line per cycle, not per frame.
            s_roomTries     = 0;
            s_roomNextFrame = RV().frameIndex + 900;
            Msg("~[%s] still only %.0f MB of VRAM free against the ~%u MB this feature needs — "
                "leaving it OFF rather than running it out of system memory (that is the 4 fps "
                "case). Enable it BEFORE loading the level, lower r_txstream_budget / "
                "r_clpage_budget, or pin the estimate with r_dlss_vram.", who, freeMB, needMB);
            return false;
        }
        if (s_roomTries == 1)
            Msg("~[%s] %.0f MB VRAM free, ~%u MB needed — demoting streamed textures before "
                "building the feature.", who, freeMB, needMB);
        VK::TextureStreamer::Instance().FreeDeviceVram(needMB);
        return false;
    }

    u32 s_evalFailN = 0, s_evalFailLastLog = 0;
    void NoteEvalFailure(const char* who, unsigned code)
    {
        ++s_evalFailN;
        if (s_evalFailN == 1 || RV().frameIndex - s_evalFailLastLog >= 300) {
            s_evalFailLastLog = RV().frameIndex;
            Msg("![%s] EvaluateFeature failed: 0x%08X (%u so far) — the upscaled target was NOT "
                "written; the tonemap falls back to the render-res scene this frame", who, code, s_evalFailN);
        }
    }
}

void EnsureOutput(u32 displayW, u32 displayH)
{
    if (!s_inited) return;
    if (s_outImg != VK_NULL_HANDLE && displayW == s_outW && displayH == s_outH) return;
    if (s_outView) { vkDestroyImageView(VulkanHW.m_Device, s_outView, nullptr); s_outView = VK_NULL_HANDLE; }
    if (s_outImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_outImg, s_outAlloc); s_outImg = VK_NULL_HANDLE; s_outAlloc = VK_NULL_HANDLE; }
    if (displayW == 0 || displayH == 0) return;

    // ⚠TRANSFER_DST is for NGX, not for us: nothing in this engine clears or copies
    // INTO the DLSS output, but the nvngx snippet does — it issues its own
    // vkCmdClearColorImage on the output resource inside EvaluateFeature. Without the
    // flag that clear is illegal, which is what VUID-vkCmdClearColorImage-image-00002
    // reported every single frame (144 hits once the message cap was lifted, 16-08).
    // ⭐The blamed image is DLSS.Output itself — handle 0x3929 against SceneColor's
    // 0x2DA/0x2DE/0x2E2 — which is how it was finally pinned on NGX rather than on
    // the engine's own frame-start clear of the HDR target.
    if (!VK::CreateImage2D(s_outFmt, { displayW, displayH },
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            s_outImg, s_outAlloc, "DLSS.Output"))
        return;
    s_outView = VK::CreateImageView(s_outImg, s_outFmt);
    if (s_outView == VK_NULL_HANDLE) {
        VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_outImg, s_outAlloc); s_outImg = VK_NULL_HANDLE; return;
    }
    s_outW = displayW; s_outH = displayH;
    // The handle is logged because validation blames THIS image for the frame-start
    // vkCmdClearColorImage (VUID-...-00002, "no TRANSFER_DST"), which clears
    // SceneColor — a different image, created with TRANSFER_DST and its own name.
    // The error appears in the same millisecond as this line in every run, so the
    // two handles are the thing to compare; print both rather than reason about it.
    Msg("[VK DLSS] output target ready (%ux%u RGBA16F) img=%p", displayW, displayH, (void*)s_outImg);
}

VkImageView OutputView()   { return s_outView; }
VkImage     OutputImage()  { return s_outImg; }
VkFormat    OutputFormat() { return s_outFmt; }
bool        OutputReady()  { return s_outImg != VK_NULL_HANDLE; }

// Under the SL route there is no raw NGX feature object — s_slShadow stands in
// for it (see EnsureFeatureSL). s_resolved = this frame's evaluate actually wrote
// OutputView; without it the tonemap sampled an undefined image after a failed
// SL evaluate ("blue screen").
namespace { bool s_slShadow = false; bool s_resolved = false; }
// ⚠EVERY occupant of the slot counts, Neural Rendering included. This is the gate
// the tonemap asks before it samples OutputView, and it listed SR, RR and the SL
// shadow — but not NR, whose whole point is that it runs with the other two
// RELEASED. The result: NR created its feature, evaluated successfully every frame,
// and the composite kept sampling raw scene colour because nothing here said the
// slot was occupied. A resolve nobody reads is indistinguishable from no resolve.
bool        FeatureReady() { return (s_feature != nullptr || s_featureRR != nullptr || s_featureNR != nullptr || s_slShadow) && s_outImg != VK_NULL_HANDLE; }
bool        ResolvedThisFrame()       { return s_resolved; }
void        SetResolvedThisFrame(bool ok) { s_resolved = ok; }

// ---------------------------------------------------------------------------
// Temporal-history reset (InReset). DLSS reprojects the previous OUTPUT through
// this frame's motion vectors — that only holds while the MVs actually describe
// how the scene changed. Three cases where they cannot, and the history has to be
// dropped or the transformer smears the OLD scene over the new one for ~10 frames:
//
//   1. we did not evaluate on the immediately preceding frame — r_dlss toggled
//      off→on, a loading screen, the main menu, alt-tab. The "previous output" NGX
//      still holds is minutes old and belongs to another place entirely.
//   2. the level is (re)loading (dwPrecacheFrame != 0).
//   3. the camera JUMPED: teleport, level transition, cutscene/camera cut. MVs are
//      derived from the view matrices and cannot express a discontinuity — every
//      pixel gets a garbage correspondence.
//
// ⚠A fast mouse turn is NOT a cut: rotation is exactly what motion vectors encode,
// and resetting on it would throw away the AA precisely when it is most visible.
// So the trigger is POSITION only. 10 m in one frame is ~50x a sprinting player at
// 60 fps and still far below any real teleport.
// ---------------------------------------------------------------------------
namespace {
    bool    s_evalChain    = false;        // we evaluated on frame s_lastEvalFrame
    u32     s_lastEvalFrame = 0;
    Fvector s_lastCamPos   = {};
    bool    s_camPosValid  = false;
    const float kCamCutDistSq = 10.0f * 10.0f;   // metres², squared to skip the sqrt
}

bool TakeHistoryReset()
{
    bool reset = false;

    if (!s_evalChain || RV().frameIndex != s_lastEvalFrame + 1) reset = true;   // gap in the eval chain
    if (Device.dwPrecacheFrame != 0)                           reset = true;   // level (re)load

    const Fvector cam = RV().vCameraPosition;
    if (s_camPosValid && cam.distance_to_sqr(s_lastCamPos) > kCamCutDistSq)    reset = true;

    s_lastCamPos    = cam;   s_camPosValid = true;
    s_lastEvalFrame = RV().frameIndex; s_evalChain = true;
    return reset;
}

void NewFrameJitter(u32 renderW, u32 renderH)
{
    s_resolved = false;   // set again only by a successful evaluate this frame
    NgxHeapTick();        // deferred VRAM footprint reading, if one is due (see NgxHeapReportLater)
    s_jitterW = renderW; s_jitterH = renderH;
    s_jitterPhase = (s_jitterPhase + 1) % s_jitterCount;
    // Halton is 1-based for a well-distributed sequence; map [0,1) → [-0.5, 0.5].
    s_jitterPixX = Halton(s_jitterPhase + 1, 2) - 0.5f;
    s_jitterPixY = Halton(s_jitterPhase + 1, 3) - 0.5f;
    // ⚠Neural Rendering takes NO jitter offset. The 310.8 snippet exports keys for
    // colour, depth, motion vectors, the sub-rects, the scales, Reset and the look
    // knobs — and nothing for a sub-pixel offset. A network that is never told the
    // offset cannot undo it, so a jittered input would just shimmer by up to a pixel
    // per frame. While NR owns the slot the camera sits on the pixel centre. The
    // sequence still ADVANCES so that switching back to SR mid-run resumes a
    // properly distributed phase instead of restarting from Halton(1).
    if (NeuralRenderEnabled()) { s_jitterPixX = 0.0f; s_jitterPixY = 0.0f; }
}

void GetJitterPix(float& outX, float& outY)
{
    outX = s_jitterPixX; outY = s_jitterPixY;
}

void GetProjJitterNDC(float& outX, float& outY)
{
    // Pixel offset → NDC: 2*px/dim. Use the render dims captured by NewFrameJitter
    // (the feature dims are still 0 before the first EnsureFeature).
    const float w = s_jitterW ? float(s_jitterW) : 1.0f;
    const float h = s_jitterH ? float(s_jitterH) : 1.0f;
    outX = 2.0f * s_jitterPixX / w;
    outY = 2.0f * s_jitterPixY / h;
}

namespace {
    // Cache the optimal render dims so we only query NGX when display size / quality changes.
    u32 s_reDispW = 0, s_reDispH = 0, s_reQual = 0xFFFFFFFFu;
    u32 s_reRenderW = 0, s_reRenderH = 0;
}

void GetRenderExtent(u32 displayW, u32 displayH, u32& outW, u32& outH)
{
    outW = displayW; outH = displayH;
    // Either feature can own the render extent — RR runs the SAME quality modes and
    // asks NGX for the same optimal dims. Gating on SR alone left an RR-only card
    // rendering at native while RR was built for an upscale it never got.
    if (!s_inited || !(s_srAvail || s_rrAvail) || displayW == 0 || displayH == 0) return;
    if (ps_r_dlss_quality == 0) { s_reDispW = displayW; s_reDispH = displayH; s_reQual = 0; s_reRenderW = displayW; s_reRenderH = displayH; return; }

    if (displayW == s_reDispW && displayH == s_reDispH && ps_r_dlss_quality == s_reQual) {
        outW = s_reRenderW; outH = s_reRenderH; return;   // cached
    }

    u32 optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0; float sharp = 0.0f;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        s_caps, displayW, displayH, PerfQualityForCvar(),
        &optW, &optH, &maxW, &maxH, &minW, &minH, &sharp);
    if (NVSDK_NGX_FAILED(r) || optW == 0 || optH == 0) {
        Msg("![VK DLSS] GetOptimalSettings failed (0x%08X) — falling back to native", (unsigned)r);
        s_reDispW = displayW; s_reDispH = displayH; s_reQual = ps_r_dlss_quality;
        s_reRenderW = displayW; s_reRenderH = displayH;   // cache the fallback so we don't re-query every frame
        return;
    }
    s_reDispW = displayW; s_reDispH = displayH; s_reQual = ps_r_dlss_quality;
    s_reRenderW = optW; s_reRenderH = optH;
    outW = optW; outH = optH;
    Msg("[VK DLSS] render extent %ux%u -> display %ux%u (quality %u)", optW, optH, displayW, displayH, ps_r_dlss_quality);
}

bool Upscaling()
{
    // ⚠Built on Enabled(), not on s_srAvail — the render extent must shrink for
    // whichever feature is actually going to run, and RR is a route of its own
    // (same quality presets, same optimal render dims). Reading s_srAvail here was
    // the same false SR↔RR coupling the comment on Enabled() describes.
    return Enabled() && ps_r_dlss_quality != 0;
}

// ============================================================================
//  RAY RECONSTRUCTION — feature lifecycle + evaluate. See the block comment in
//  vk_dlss.h for why this replaces Super Resolution rather than joining it.
// ============================================================================
bool RayReconAvailable() { return s_inited && s_rrAvail; }

bool RayReconEnabled()
{
    // ⚠RRGuide::Ready() is part of the gate, not an assert. RR without honest
    // albedo/normal/roughness is not "RR with a small artefact" — it is a network
    // told that every texture detail is lighting noise, and it mushes accordingly.
    // If the guide G-buffer failed to allocate we stay on Super Resolution.
    return s_inited && s_rrAvail && ps_r_dlss != 0 && ps_r_dlss_rr != 0 && VK::RRGuide::Ready();
}

bool NeuralRenderAvailable() { return s_inited && s_nrAvail; }

bool NeuralRenderEnabled()
{
    // ⚠The latch is part of the gate. Once NGX has refused this feature ID there is
    // nothing to route to, and reporting "enabled" would send the frame down a path
    // whose EnsureFeatureNR returns false every time — the tonemap would then gate
    // itself off and the screen would simply stop updating.
    return s_inited && s_nrAvail && ps_r_dlss != 0 && ps_r_dlss_nr != 0
        && !(s_nrFailedId == ps_r_dlss_nr_featid && s_nrFailedCfg == ps_r_dlss_nr_cfg);
}

namespace {
    // 1x1 R32F exposure texture, shared by both features (created once, reused).
    bool EnsureExpTex()
    {
        if (s_expImg != VK_NULL_HANDLE) return true;
        if (!VK::CreateImage2D(VK_FORMAT_R32_SFLOAT, { 1, 1 },
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                s_expImg, s_expAlloc, "DLSS.Exposure"))
            return false;
        s_expView = VK::CreateImageView(s_expImg, VK_FORMAT_R32_SFLOAT);
        if (s_expView == VK_NULL_HANDLE) {
            VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_expImg, s_expAlloc);
            s_expImg = VK_NULL_HANDLE; s_expAlloc = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    void ReleaseFeatureRR()
    {
        s_ngxReserveMB.store(0, std::memory_order_relaxed);   // the seat goes back to the streamer
        if (s_featureRR) { NVSDK_NGX_VULKAN_ReleaseFeature(s_featureRR); s_featureRR = nullptr; }
        s_evalChain   = false;   // whatever temporal history existed died with the feature
        s_camPosValid = false;
        s_rrRenderW = s_rrRenderH = s_rrDispW = s_rrDispH = 0;
        s_rrPreset  = 0xFFFFFFFFu;
        s_rrInputsGen = 0xFFFFFFFFu;
        s_rrGuideGen  = 0xFFFFFFFFu;
        s_rrExpMode   = 0xFFFFFFFFu;
    }

    void ReleaseFeatureNR()
    {
        s_ngxReserveMB.store(0, std::memory_order_relaxed);
        if (s_featureNR) { NVSDK_NGX_VULKAN_ReleaseFeature(s_featureNR); s_featureNR = nullptr; }
        s_evalChain   = false;   // the temporal history died with the feature, same as RR
        s_camPosValid = false;
        s_nrRenderW = s_nrRenderH = s_nrDispW = s_nrDispH = 0;
        s_nrPreset    = 0xFFFFFFFFu;
        s_nrStyle     = 0xFFFFFFFFu;
        s_nrInputsGen = 0xFFFFFFFFu;
        s_nrFeatId    = 0xFFFFFFFFu;
        s_nrCfg       = 0xFFFFFFFFu;
    }

    // Send a float knob only if the user actually moved it off -1. Setting every key
    // to a made-up neutral would overwrite the network's own tuning with our guess:
    // none of these ranges are documented, so "not sent" is the only honest default.
    void SetNrKnob(NVSDK_NGX_Parameter* p, const char* key, float v)
    {
        if (v >= 0.f) NVSDK_NGX_Parameter_SetF(p, key, v);
    }
}

bool EnsureFeatureRR(VkCommandBuffer cmd, u32 renderW, u32 renderH, u32 displayW, u32 displayH, u32 inputsGen)
{
    if (!s_inited || !s_rrAvail) return false;
    if (renderW == 0 || renderH == 0 || displayW == 0 || displayH == 0) return false;
    if (!VK::RRGuide::Ready()) return false;

    const u32 guideGen = VK::RRGuide::Generation();
    if (s_featureRR && renderW == s_rrRenderW && renderH == s_rrRenderH
        && displayW == s_rrDispW && displayH == s_rrDispH
        && ps_r_dlss_rr_preset == s_rrPreset && inputsGen == s_rrInputsGen
        && guideGen == s_rrGuideGen && u32(ps_r_dlss_exp != 0) == s_rrExpMode)
        return true;

    // Room gate — see HaveRoomForNgx. RR is the heavy one, and the check comes BEFORE
    // the idle + release below on purpose: a card with no room then keeps whatever is
    // already running instead of being left with nothing.
    if (!HaveRoomForNgx("VK RR", NgxNeedMB(displayW, displayH, true))) return false;

    // Same idle rule as the SR path, and for the same reason: in-flight frames may
    // still reference the feature's internals, and NGX caches state against the
    // INPUT images — evaluating a feature built before the targets were recreated
    // faults the GPU. Releasing SR here is the mutual exclusion: two live NGX
    // features mean two resident tensor sets.
    if (s_feature || s_featureRR) VK_DeviceWaitIdleLocked();
    if (s_feature) ReleaseFeature();
    ReleaseFeatureRR();

    if (s_paramsRR == nullptr) {
        NVSDK_NGX_Result pr = NVSDK_NGX_VULKAN_AllocateParameters(&s_paramsRR);
        if (NVSDK_NGX_FAILED(pr) || s_paramsRR == nullptr) {
            Msg("![VK RR] AllocateParameters failed: 0x%08X", (unsigned)pr); return false;
        }
    }

    // ⚠RR has its OWN preset enum and its own parameter keys — r_dlss_preset (K=11,
    // the SR transformer) is meaningless here. Only D (4) and E (5) are real models;
    // everything else in the enum is documented as "reverts to default behavior".
    {
        const u32 p = ps_r_dlss_rr_preset;
        NVSDK_NGX_Parameter_SetUI(s_paramsRR, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,             p);
        NVSDK_NGX_Parameter_SetUI(s_paramsRR, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,          p);
        NVSDK_NGX_Parameter_SetUI(s_paramsRR, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,         p);
        NVSDK_NGX_Parameter_SetUI(s_paramsRR, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,      p);
        NVSDK_NGX_Parameter_SetUI(s_paramsRR, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, p);
    }

    NVSDK_NGX_DLSSD_Create_Params cp{};
    cp.InDenoiseMode   = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    // Roughness rides in normals.w — one attachment instead of two, and the SDK is
    // explicit that this pairing REQUIRES the Packed mode at feature creation.
    cp.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    // We hand it the hardware depth buffer, same one the motion vectors came from.
    cp.InUseHWDepth    = NVSDK_NGX_DLSS_Depth_Type_HW;
    cp.InWidth         = renderW;
    cp.InHeight        = renderH;
    cp.InTargetWidth   = displayW;
    cp.InTargetHeight  = displayH;
    cp.InPerfQualityValue = PerfQualityForCvar();   // RR runs the SAME quality modes as SR
    cp.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (ps_r_dlss_exp == 0)
        cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    NgxHeapMark();
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSSD_EXT1(
        VulkanHW.m_Device, cmd, 1u, 1u, &s_featureRR, s_paramsRR, &cp);
    if (NVSDK_NGX_FAILED(r) || s_featureRR == nullptr) {
        Msg("![VK RR] CreateFeature(RayReconstruction) failed: 0x%08X", (unsigned)r);
        s_featureRR = nullptr; return false;
    }
    NgxHeapReport("VK RR");
    NgxHeapReportLater("VK RR");
    s_ngxReserveMB.store(NgxNeedMB(displayW, displayH, true), std::memory_order_relaxed);

    s_rrRenderW = renderW; s_rrRenderH = renderH; s_rrDispW = displayW; s_rrDispH = displayH;
    s_rrPreset  = ps_r_dlss_rr_preset;
    s_rrInputsGen = inputsGen;
    s_rrGuideGen  = guideGen;
    s_rrExpMode   = u32(ps_r_dlss_exp != 0);
    if (s_rrExpMode && !EnsureExpTex())
        Msg("![VK RR] exposure texture create failed — falling back to NGX defaults");

    {
        const float ratio = (renderW > 0 && renderH > 0)
            ? (float(displayW) / float(renderW)) * (float(displayH) / float(renderH)) : 1.0f;
        u32 phases = (u32)(8.0f * ratio + 0.5f);
        s_jitterCount = (phases < 8u) ? 8u : (phases > 64u ? 64u : phases);
    }
    s_slShadow = false;
    Msg("[VK RR] Ray Reconstruction feature created: render %ux%u -> display %ux%u (%s), "
        "jitter phases %u, preset %u, guides gen %u",
        renderW, renderH, displayW, displayH,
        (renderW == displayW) ? "DLAA" : "upscale", s_jitterCount, ps_r_dlss_rr_preset, guideGen);
    return true;
}

bool EvaluateRR(VkCommandBuffer cmd, const VK::Dlss::Img& color, const VK::Dlss::Img& depth,
                const VK::Dlss::Img& mv, const VK::Dlss::Img& normalRough,
                const VK::Dlss::Img& diffAlbedo, const VK::Dlss::Img& specAlbedo,
                u32 renderW, u32 renderH, bool reset)
{
    if (s_featureRR == nullptr || s_paramsRR == nullptr || s_outImg == VK_NULL_HANDLE) return false;
    if (color.image == VK_NULL_HANDLE || depth.image == VK_NULL_HANDLE || mv.image == VK_NULL_HANDLE) return false;
    if (normalRough.image == VK_NULL_HANDLE || diffAlbedo.image == VK_NULL_HANDLE ||
        specAlbedo.image == VK_NULL_HANDLE) return false;

    // Identical output barrier to the SR path — NGX clears the output itself inside
    // EvaluateFeature, so the destination access must cover TRANSFER as well as
    // COMPUTE (sync validation caught exactly that on the SR side).
    ImageBarrier(cmd, s_outImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);

    NVSDK_NGX_Resource_VK colorR = WrapImage(color,       renderW, renderH, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK depthR = WrapImage(depth,       renderW, renderH, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NVSDK_NGX_Resource_VK mvR    = WrapImage(mv,          renderW, renderH, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK nrmR   = WrapImage(normalRough, renderW, renderH, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK difR   = WrapImage(diffAlbedo,  renderW, renderH, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK spcR   = WrapImage(specAlbedo,  renderW, renderH, VK_IMAGE_ASPECT_COLOR_BIT, false);
    VK::Dlss::Img outImg{ s_outView, s_outImg, s_outFmt };
    NVSDK_NGX_Resource_VK outR   = WrapImage(outImg, s_outW, s_outH, VK_IMAGE_ASPECT_COLOR_BIT, true);

    NVSDK_NGX_Resource_VK expR{};
    const bool useExpTex = (s_rrExpMode == 1u) && s_expImg != VK_NULL_HANDLE;
    if (useExpTex) {
        ImageBarrier(cmd, s_expImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue cv{}; cv.float32[0] = (s_expHint > 1e-6f) ? s_expHint : 1.0f;
        VkImageSubresourceRange rr = FullRange(VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(cmd, s_expImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rr);
        ImageBarrier(cmd, s_expImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        VK::Dlss::Img expImg{ s_expView, s_expImg, VK_FORMAT_R32_SFLOAT };
        expR = WrapImage(expImg, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT, false);
    }

    NVSDK_NGX_VK_DLSSD_Eval_Params ep{};
    ep.pInColor           = &colorR;
    ep.pInOutput          = &outR;
    ep.pInDepth           = &depthR;
    ep.pInMotionVectors   = &mvR;
    ep.pInNormals         = &nrmR;
    // ⚠pInRoughness stays NULL on purpose: the feature was created with
    // Roughness_Mode_Packed, so RR reads it out of normals.w. Handing it a separate
    // texture as well is not "belt and braces" — the two would disagree.
    ep.pInRoughness       = nullptr;
    ep.pInDiffuseAlbedo   = &difR;
    ep.pInSpecularAlbedo  = &spcR;
    // Required input (see the note in vk_dlss.h): our specular does not move
    // independently of the surface, so the surface motion field IS its motion field.
    ep.pInMotionVectorsReflections = &mvR;
    if (useExpTex) ep.pInExposureTexture = &expR;
    ep.InJitterOffsetX    = s_jitterPixX;    // reported jitter: Y flipped (the verified sign)
    ep.InJitterOffsetY    = -s_jitterPixY;
    ep.InReset            = reset ? 1 : 0;
    ep.InRenderSubrectDimensions.Width  = renderW;
    ep.InRenderSubrectDimensions.Height = renderH;
    ep.InMVScaleX         = float(renderW);   // our MV is UV-space; RR wants render pixels
    ep.InMVScaleY         = float(renderH);
    // How much wall time the motion vectors span. The SDK: RR uses it "to determine
    // the amount to denoise or anti-alias based on the speed of the object from
    // motion vector magnitudes and fps". ⚠REAL time, not fTimeDelta — the game clock
    // is scaled by the time factor and pausing the world would tell RR the scene
    // froze while the camera kept moving.
    ep.InFrameTimeDeltaInMsec = RV().fTimeDeltaRealMS;

    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd, s_featureRR, s_paramsRR, &ep);
    if (NVSDK_NGX_FAILED(r)) { NoteEvalFailure("VK RR", (unsigned)r); return false; }
    return true;
}

// ============================================================================
//  Neural Rendering (DLSS-NR). Everything here goes through the GENERIC NGX entry
//  points and string keys — see the block comment in vk_dlss.h for why there is no
//  helper macro or Create_Params struct to use instead.
// ============================================================================
bool EnsureFeatureNR(VkCommandBuffer cmd, u32 renderW, u32 renderH, u32 displayW, u32 displayH, u32 inputsGen)
{
    if (!s_inited || !s_nrAvail) return false;
    if (renderW == 0 || renderH == 0 || displayW == 0 || displayH == 0) return false;

    const u32 featId = ps_r_dlss_nr_featid;
    // Refused once with this exact (id, key set); either cvar moving re-arms it.
    if (s_nrFailedId == featId && s_nrFailedCfg == ps_r_dlss_nr_cfg) return false;

    if (s_featureNR && renderW == s_nrRenderW && renderH == s_nrRenderH
        && displayW == s_nrDispW && displayH == s_nrDispH
        && ps_r_dlss_nr_preset == s_nrPreset && ps_r_dlss_nr_style == s_nrStyle && inputsGen == s_nrInputsGen
        && featId == s_nrFeatId && ps_r_dlss_nr_cfg == s_nrCfg)
        return true;

    // Room gate (see HaveRoomForNgx) — NR carries a full network of its own, so it is
    // priced with RR rather than with SR.
    if (!HaveRoomForNgx("VK NR", NgxNeedMB(displayW, displayH, true))) return false;

    // Same idle-then-release rule as the RR path, for the same two reasons: frames in
    // flight may still reference a feature's internals, and NGX caches state against
    // the INPUT images. Releasing the other two here IS the mutual exclusion.
    if (s_feature || s_featureRR || s_featureNR) VK_DeviceWaitIdleLocked();
    if (s_feature)   ReleaseFeature();
    if (s_featureRR) ReleaseFeatureRR();
    ReleaseFeatureNR();

    // ⚠SWEEP THE KEY SET AUTOMATICALLY, here, in one go. CreateFeature answers
    // FAIL_InvalidParameter without ever naming the parameter it disliked, so the set
    // has to be searched — and making a human retype a cvar between level loads to
    // search it is how a five-second question turns into an evening. The candidate
    // list starts with whatever r_dlss_nr_cfg asks for, then walks the combinations
    // that are actually plausible. A create refused on parameters is cheap: it fails
    // before the network is built.
    const float ratio = float(displayW) / float(renderW);
    u32 cand[10]; u32 nCand = 0;
    cand[nCand++] = ps_r_dlss_nr_cfg;   // whatever the player pinned, tried first
    // ⭐64 = the DLSSD-shaped keys, and the reason this list exists at all. We are
    // masquerading as feature 13, so the CORE may well validate this create against
    // RayReconstruction's contract and reject it before the NR network ever sees the
    // parameters — in which case no amount of DLSSNR.* spelling helps and only the
    // DLSSD keys do.
    const u32 rest[] = { 29u, 93u, 95u, 127u, 79u, 71u, 64u, 13u, 8u };
    for (u32 i = 0; i < (u32)(sizeof(rest)/sizeof(rest[0])) && nCand < 10u; ++i) {
        bool dup = false;
        for (u32 j = 0; j < nCand; ++j) if (cand[j] == rest[i]) { dup = true; break; }
        if (!dup) cand[nCand++] = rest[i];
    }

    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    u32 cfg = ps_r_dlss_nr_cfg;
    NgxHeapMark();
    for (u32 attempt = 0; attempt < nCand; ++attempt) {
        cfg = cand[attempt];

        // ⚠A FRESH parameter block per attempt, never a cached one. An NGX parameter
        // object is an ACCUMULATING key/value bag, so a key written by the previous
        // attempt is still sitting in it during the next — reuse would make clearing
        // a bit do nothing and quietly test the union of everything tried so far.
        // ⚠And allocate BEFORE anything writes into it: the object is an interface
        // pointer, so a null one is not a failed Set, it is a null-vtable call inside
        // NVSDK_NGX_Parameter_SetUI — a crash on the very first key, as shipped once.
        if (s_paramsNR) { NVSDK_NGX_VULKAN_DestroyParameters(s_paramsNR); s_paramsNR = nullptr; }
        NVSDK_NGX_Result pr = NVSDK_NGX_VULKAN_AllocateParameters(&s_paramsNR);
        if (NVSDK_NGX_FAILED(pr) || s_paramsNR == nullptr) {
            Msg("![VK NR] AllocateParameters failed: 0x%08X", (unsigned)pr);
            s_paramsNR = nullptr;
            s_nrFailedId = featId; s_nrFailedCfg = ps_r_dlss_nr_cfg;
            return false;
        }

        // Unconditional = what we actually know. The node masks, because every NGX
        // create is told which device it builds on and the SR/RR helpers write them
        // first. DLSSNR.Width/Height/Enabled, because those are the only sizing and
        // enable keys that appear in the SNIPPET'S OWN string table. Rest: candidates.
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_CreationNodeMask,   1u);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.Width",   renderW);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.Height",  renderH);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.Enabled", 1u);

        if (cfg & 1u) {
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_Width,     renderW);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_Height,    renderH);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_OutWidth,  displayW);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_OutHeight, displayH);
        }
        // A quality TIER is an SR/RR concept; this network sizes itself by a ratio and
        // has no PerfQualityValue anywhere in its strings.
        if (cfg & 2u)
            NVSDK_NGX_Parameter_SetI(s_paramsNR, NVSDK_NGX_Parameter_PerfQualityValue, PerfQualityForCvar());
        if (cfg & 4u)
            NVSDK_NGX_Parameter_SetI(s_paramsNR, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                                     NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                     NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
        // ⚠Sent even at 1.0. The first attempt set it only while upscaling, so the
        // native-resolution create sent no ratio at all — and "a required parameter
        // was not provided" is half of what InvalidParameter is documented to mean.
        if (cfg & 8u)
            NVSDK_NGX_Parameter_SetF(s_paramsNR, "DLSSNR.ScalingRatio", ratio);
        if (cfg & 16u) {
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.Hint.Render.Preset", ps_r_dlss_nr_preset);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.Style",              ps_r_dlss_nr_style);
        }
        // ⚠The addon's own vocabulary, not the network's — none of these strings exist
        // inside nvngx_dlssnr.dll. Kept only so the combination can be ruled out.
        if (cfg & 32u) {
            const bool up = (renderW != displayW) || (renderH != displayH);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.InputWidth",    renderW);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.InputHeight",   renderH);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.OutputWidth",   displayW);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.OutputHeight",  displayH);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.Output.Width",  displayW);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.Output.Height", displayH);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.Upscaling", up ? 1u : 0u);
            NVSDK_NGX_Parameter_SetF (s_paramsNR, "DLSSNR.Scale",     ratio);
        }
        // ⭐The RayReconstruction creation contract, because feature 13 is the door we
        // came through. If the core validates against DLSSD before dispatching, these
        // three are exactly what it is looking for and their absence is the refusal.
        if (cfg & 64u) {
            NVSDK_NGX_Parameter_SetI (s_paramsNR, NVSDK_NGX_Parameter_DLSS_Denoise_Mode,
                                      NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_DLSS_Roughness_Mode,
                                      NVSDK_NGX_DLSS_Roughness_Mode_Packed);
            NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_Use_HW_Depth,
                                      NVSDK_NGX_DLSS_Depth_Type_HW);
        }

        r = NVSDK_NGX_VULKAN_CreateFeature(cmd, (NVSDK_NGX_Feature)featId, s_paramsNR, &s_featureNR);
        if (NVSDK_NGX_SUCCEED(r) && s_featureNR != nullptr) break;

        s_featureNR = nullptr;
        Msg("~[VK NR] create attempt %u/%u (id %u, cfg %u): 0x%08X",
            attempt + 1u, nCand, featId, cfg, (unsigned)r);
        // A missing LIBRARY is not a parameter problem — sweeping key sets against it
        // is pure noise, so stop the moment that is what we are being told.
        if ((unsigned)r == 0xBAD0000Bu) break;
    }

    if (NVSDK_NGX_FAILED(r) || s_featureNR == nullptr) {
        s_featureNR  = nullptr;
        s_nrFailedId = featId; s_nrFailedCfg = ps_r_dlss_nr_cfg;   // one sweep per (id, pinned cfg)
        Msg("![VK NR] CreateFeature(id %u) refused every one of the %u key sets tried; last 0x%08X",
            featId, nCand, (unsigned)r);
        if ((unsigned)r == 0xBAD0000Bu)
            Msg("![VK NR]   ^ UnableToInitializeFeature = the snippet library could not be found. "
                "This driver's NGX loader knows dlss/dlssd/dlssg and no dlssnr, so no feature id "
                "resolves to nvngx_dlssnr.dll. A newer driver is the missing piece, not another id.");
        else if ((unsigned)r == 0xBAD00005u)
            Msg("![VK NR]   ^ InvalidParameter throughout: the library loads, but nothing we can "
                "spell gets past creation. That is the end of what is reachable without the "
                "driver-side support this network is waiting on.");
        return false;
    }
    NgxHeapReport("VK NR");
    NgxHeapReportLater("VK NR");
    s_ngxReserveMB.store(NgxNeedMB(displayW, displayH, true), std::memory_order_relaxed);

    s_nrRenderW = renderW; s_nrRenderH = renderH; s_nrDispW = displayW; s_nrDispH = displayH;
    s_nrPreset    = ps_r_dlss_nr_preset;
    s_nrStyle     = ps_r_dlss_nr_style;
    s_nrInputsGen = inputsGen;
    s_nrFeatId    = featId;
    // ⚠PIN the key set that won, into the cvar the cache is keyed on. Without this
    // the sweep's winner (93) never equals the pinned value (29), the dim/cfg cache
    // misses on the very next frame, and the feature is torn down and rebuilt every
    // single frame — device idle and all. It also means the answer survives into
    // user.ltx, so the sweep runs once in the lifetime of this machine, not once a run.
    ps_r_dlss_nr_cfg = cfg;
    s_nrCfg          = cfg;
    s_slShadow    = false;
    Msg("[VK NR] Neural Rendering feature created: id %u, cfg %u, render %ux%u -> display %ux%u (%s), "
        "ratio %.3f, preset %u, style %u", featId, cfg, renderW, renderH, displayW, displayH,
        (renderW == displayW && renderH == displayH) ? "native" : "upscale",
        ratio, ps_r_dlss_nr_preset, ps_r_dlss_nr_style);
    return true;
}

bool EvaluateNR(VkCommandBuffer cmd, const VK::Dlss::Img& color, const VK::Dlss::Img& depth,
                const VK::Dlss::Img& mv, u32 renderW, u32 renderH, bool reset)
{
    if (s_featureNR == nullptr || s_paramsNR == nullptr || s_outImg == VK_NULL_HANDLE) return false;
    if (color.image == VK_NULL_HANDLE || depth.image == VK_NULL_HANDLE || mv.image == VK_NULL_HANDLE) return false;

    // Identical output barrier to the other two routes, and identical reasoning: NGX
    // clears this image inside EvaluateFeature, so the destination access has to
    // cover TRANSFER as well as COMPUTE (sync validation caught exactly that on SR).
    ImageBarrier(cmd, s_outImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);

    NVSDK_NGX_Resource_VK colorR = WrapImage(color, renderW, renderH, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK depthR = WrapImage(depth, renderW, renderH, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NVSDK_NGX_Resource_VK mvR    = WrapImage(mv,    renderW, renderH, VK_IMAGE_ASPECT_COLOR_BIT, false);
    VK::Dlss::Img outImg{ s_outView, s_outImg, s_outFmt };
    NVSDK_NGX_Resource_VK outR   = WrapImage(outImg, s_outW, s_outH, VK_IMAGE_ASPECT_COLOR_BIT, true);

    NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, "DLSSNR.Color",  &colorR);
    NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, "DLSSNR.Depth",  &depthR);
    NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, "DLSSNR.MVec",   &mvR);
    NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, "DLSSNR.Output", &outR);

    // Sub-rects, spelled out rather than left to default. The scene targets are
    // allocated once at DISPLAY size and rendered into at RENDER size whenever DLSS
    // upscales, so "the whole resource" and "the part that has pixels in it" are not
    // the same rectangle — this is the same reason the SR path fills
    // InRenderSubrectDimensions instead of trusting the resource extents.
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.ColorSubrectBaseX",  0u);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.ColorSubrectBaseY",  0u);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.ColorSubrectWidth",  renderW);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.ColorSubrectHeight", renderH);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.DepthSubrectBaseX",  0u);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.DepthSubrectBaseY",  0u);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.DepthSubrectWidth",  renderW);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.DepthSubrectHeight", renderH);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.MVecSubrectBaseX",   0u);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.MVecSubrectBaseY",   0u);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.MVecSubrectWidth",   renderW);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.MVecSubrectHeight",  renderH);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.OutputSubrectBaseX",  0u);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.OutputSubrectBaseY",  0u);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.OutputSubrectWidth",  s_outW);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.OutputSubrectHeight", s_outH);

    // Our motion vectors are UV-space (prevUV − curUV); every NGX network here wants
    // them in render pixels, so the scale is the render extent — same as SR and RR.
    NVSDK_NGX_Parameter_SetF(s_paramsNR, "DLSSNR.MVecScaleX", float(renderW));
    NVSDK_NGX_Parameter_SetF(s_paramsNR, "DLSSNR.MVecScaleY", float(renderH));
    // Standard (non-reverse) depth, matching what the SR feature is created with.
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.DepthInverted", 0u);
    NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.Reset", reset ? 1u : 0u);

    SetNrKnob(s_paramsNR, "DLSSNR.Intensity",              ps_r_dlss_nr_intensity);
    SetNrKnob(s_paramsNR, "DLSSNR.LocalStructureStrength", ps_r_dlss_nr_structure);
    SetNrKnob(s_paramsNR, "DLSSNR.LocalToneStrength",      ps_r_dlss_nr_tone);
    SetNrKnob(s_paramsNR, "DLSSNR.SkinStructureStrength",  ps_r_dlss_nr_skin);

    // ⭐The EVALUATE-side sweep, and it exists for the reason the create-side one
    // found: creation only got through once it was handed RayReconstruction's
    // creation keys. We reach this network through feature 13, so the core plausibly
    // validates the evaluate against DLSSD's contract too — under the STANDARD key
    // names, which are a different vocabulary from the DLSSNR.* set above.
    // Bits: 1 std resources, 2 std scalars, 4 guide slots, 8 Backbuffer, 16 mask/UI.
    // Advances one step per failed frame, so the search costs frames, not restarts.
    static const u32 kEvalCand[] = { 31u, 15u, 27u, 11u, 25u, 9u, 8u, 24u, 0u };
    static const u32 kEvalN = (u32)(sizeof(kEvalCand)/sizeof(kEvalCand[0]));
    static u32  s_evalTry    = 0;
    static bool s_evalLocked = false;   // a set that worked; stop searching and STAY on it
    const u32 ecfg = kEvalCand[s_evalTry < kEvalN ? s_evalTry : kEvalN - 1u];

    if (ecfg & 1u) {
        NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, NVSDK_NGX_Parameter_Color,         &colorR);
        NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, NVSDK_NGX_Parameter_Output,        &outR);
        NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, NVSDK_NGX_Parameter_Depth,         &depthR);
        NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, NVSDK_NGX_Parameter_MotionVectors, &mvR);
    }
    if (ecfg & 2u) {
        NVSDK_NGX_Parameter_SetF (s_paramsNR, NVSDK_NGX_Parameter_MV_Scale_X, float(renderW));
        NVSDK_NGX_Parameter_SetF (s_paramsNR, NVSDK_NGX_Parameter_MV_Scale_Y, float(renderH));
        NVSDK_NGX_Parameter_SetF (s_paramsNR, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
        NVSDK_NGX_Parameter_SetF (s_paramsNR, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
        NVSDK_NGX_Parameter_SetI (s_paramsNR, NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,  renderW);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, renderH);
    }
    // ⚠The guide slots get the COLOUR image, and that is presence, not data. RR's
    // contract wants a normal/albedo/specular set that this path does not render; the
    // NR network reads none of them (it reads DLSSNR.* only), so what they point at
    // cannot reach the output. This is here to satisfy a validator, and it is a lie
    // that stays harmless exactly as long as that remains true.
    if (ecfg & 4u) {
        NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, NVSDK_NGX_Parameter_GBuffer_Normals,     &colorR);
        NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, NVSDK_NGX_Parameter_GBuffer_Albedo,      &colorR);
        NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, NVSDK_NGX_Parameter_GBuffer_Specular,    &colorR);
        NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, NVSDK_NGX_Parameter_GBuffer_SpecularMvec, &mvR);
    }
    // ⚠Backbuffer is not optional decoration: the snippet carries an explicit
    // "Invalid Backbuffer/active Output rect configuration" refusal, so it validates
    // this rect against the output's. At render==display the scene colour IS the
    // display-res image, so it is the honest thing to point at.
    if (ecfg & 8u) {
        NVSDK_NGX_Parameter_SetVoidPointer(s_paramsNR, "DLSSNR.Backbuffer", &colorR);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.BackbufferSubrectBaseX",  0u);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.BackbufferSubrectBaseY",  0u);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.BackbufferSubrectWidth",  renderW);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.BackbufferSubrectHeight", renderH);
    }
    // No ControlMask and no UI layer are supplied, so say so rather than leaving the
    // network to infer it from two null resources.
    if (ecfg & 16u) {
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.UseAutoMask",  1u);
        NVSDK_NGX_Parameter_SetUI(s_paramsNR, "DLSSNR.UICorrection", 0u);
    }

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_EvaluateFeature_C(cmd, s_featureNR, s_paramsNR, nullptr);
    if (NVSDK_NGX_FAILED(r)) {
        if (!s_evalLocked && s_evalTry < kEvalN) {
            Msg("~[VK NR] evaluate attempt %u/%u (ecfg %u): 0x%08X",
                s_evalTry + 1u, kEvalN, ecfg, (unsigned)r);
            ++s_evalTry;
            if (s_evalTry >= kEvalN)
                Msg("![VK NR] every evaluate key set was refused. Creation succeeds and evaluation "
                    "does not, which is as far as feature 13's door reaches.");
        }
        NoteEvalFailure("VK NR", (unsigned)r);
        return false;
    }
    // ⚠Lock, do not reset. Resetting the index would send the next frame back to
    // candidate 0 — the one that just failed — and the sweep would oscillate forever.
    if (!s_evalLocked) {
        s_evalLocked = true;
        Msg("[VK NR] evaluate ACCEPTED with ecfg %u — Neural Rendering is running.", ecfg);
    }
    return true;
}

bool EnsureFeature(VkCommandBuffer cmd, u32 renderW, u32 renderH, u32 displayW, u32 displayH, u32 inputsGen)
{
    if (!s_inited || !s_srAvail) return false;
    if (renderW == 0 || renderH == 0 || displayW == 0 || displayH == 0) return false;
    // Mutual exclusion, the other way round: the frame the user turns RR off, the SR
    // feature is rebuilt here and the RR tensor set must go with it.
    if (s_featureRR) { VK_DeviceWaitIdleLocked(); ReleaseFeatureRR(); }
    if (s_feature && renderW == s_fRenderW && renderH == s_fRenderH && displayW == s_fDispW && displayH == s_fDispH
        && ps_r_dlss_preset == s_fPreset && inputsGen == s_fInputsGen && u32(ps_r_dlss_exp != 0) == s_fExpMode)
        return true;   // already built for these dims + preset + input targets + exposure mode

    // Room gate (see HaveRoomForNgx) — same rule at SR's lighter footprint.
    if (!HaveRoomForNgx("VK DLSS", NgxNeedMB(displayW, displayH, false))) return false;

    // Rare path (quality/preset/window change, or the input targets were recreated —
    // e.g. the r_dlss off→on toggle resizes SceneColor/MV/depth while the feature
    // idles; reusing it then evaluates against DESTROYED images and hangs the GPU).
    // In-flight frames may still reference the old feature's internals — idle before
    // releasing it. (A render-extent change already idled in CRender::Begin; a
    // preset-only or generation-only change reaches here without it.)
    if (s_feature) VK_DeviceWaitIdleLocked();
    ReleaseFeature();

    if (s_params == nullptr) {
        NVSDK_NGX_Result pr = NVSDK_NGX_VULKAN_AllocateParameters(&s_params);
        if (NVSDK_NGX_FAILED(pr) || s_params == nullptr) {
            Msg("![VK DLSS] AllocateParameters failed: 0x%08X", (unsigned)pr); return false;
        }
    }

    // Model preset hint (r_dlss_preset), set for EVERY quality mode before create:
    // 11 = Preset K, the DLSS 4 transformer — markedly sharper reconstruction than the
    // old CNN presets. Matters most at UltraPerf, whose driver DEFAULT is still the
    // CNN preset F. 0 = leave the driver/OTA default.
    {
        const u32 p = ps_r_dlss_preset;
        NVSDK_NGX_Parameter_SetUI(s_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,             p);
        NVSDK_NGX_Parameter_SetUI(s_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,          p);
        NVSDK_NGX_Parameter_SetUI(s_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,         p);
        NVSDK_NGX_Parameter_SetUI(s_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,      p);
        NVSDK_NGX_Parameter_SetUI(s_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, p);
    }

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth        = renderW;
    cp.Feature.InHeight       = renderH;
    cp.Feature.InTargetWidth  = displayW;
    cp.Feature.InTargetHeight = displayH;
    // Preset MUST match the one GetRenderExtent queried for these render dims
    // (quality 0 → DLAA/native, 1..4 → Quality/Balanced/Perf/UltraPerf upscale).
    cp.Feature.InPerfQualityValue = PerfQualityForCvar();
    // HDR scene colour; MVs are at render res; standard (non-reverse) depth.
    // NO MVJittered (Option A): color/depth are jittered (CRender::Begin), but the MV
    // passes reproject with the UNJITTERED view-proj and re-apply the jitter only to
    // gl_Position — so motion vectors are jitter-free (NVIDIA's canonical input).
    // DLSS applies the sub-pixel offset itself from the InJitterOffset we pass to
    // Evaluate. Exposure: r_dlss_exp 1 (default) supplies the REAL tonemap exposure
    // as a 1x1 texture in Evaluate — NVIDIA's recommended path for raw-HDR input;
    // the AutoExposure flag (r_dlss_exp 0) let NGX guess and the transformer
    // undershot to BLACK on high-contrast sub-pixel foliage.
    cp.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (ps_r_dlss_exp == 0)
        cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    NgxHeapMark();
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cmd, 1u, 1u, &s_feature, s_params, &cp);
    if (NVSDK_NGX_FAILED(r) || s_feature == nullptr) {
        Msg("![VK DLSS] CreateFeature failed: 0x%08X", (unsigned)r);
        s_feature = nullptr; return false;
    }
    NgxHeapReport("VK DLSS");
    NgxHeapReportLater("VK DLSS");
    s_ngxReserveMB.store(NgxNeedMB(displayW, displayH, false), std::memory_order_relaxed);
    s_fRenderW = renderW; s_fRenderH = renderH; s_fDispW = displayW; s_fDispH = displayH;
    s_fPreset  = ps_r_dlss_preset;
    s_fInputsGen = inputsGen;
    s_fExpMode = u32(ps_r_dlss_exp != 0);

    // 1x1 R32F exposure texture for the real-exposure path (created once, reused —
    // shared with the RR path, see EnsureExpTex).
    if (s_fExpMode && !EnsureExpTex())
        Msg("![VK DLSS] exposure texture create failed — falling back to NGX defaults");
    // Jitter phase count scales with the upscale ratio: 8*(display/render)^2 so the
    // sub-pixel sample set covers the larger output grid (DLAA → 8). Capped at 64.
    {
        const float ratio = (renderW > 0 && renderH > 0)
            ? (float(displayW) / float(renderW)) * (float(displayH) / float(renderH)) : 1.0f;
        u32 phases = (u32)(8.0f * ratio + 0.5f);
        s_jitterCount = (phases < 8u) ? 8u : (phases > 64u ? 64u : phases);
    }
    Msg("[VK DLSS] SR feature created: render %ux%u -> display %ux%u (%s), jitter phases %u, preset %u",
        renderW, renderH, displayW, displayH,
        (renderW == displayW) ? "DLAA" : "upscale", s_jitterCount, ps_r_dlss_preset);
    s_slShadow = false;   // raw feature is the owner now
    return true;
}

// SL route: sl.dlss owns the NGX feature — keep only the dim cache + jitter phase
// count here, and make sure no raw feature doubles the VRAM (see vk_dlss.h).
bool EnsureFeatureSL(u32 renderW, u32 renderH, u32 displayW, u32 displayH)
{
    if (!s_inited || !s_srAvail) return false;
    if (renderW == 0 || renderH == 0 || displayW == 0 || displayH == 0) return false;
    if (s_slShadow && renderW == s_fRenderW && renderH == s_fRenderH
        && displayW == s_fDispW && displayH == s_fDispH)
        return true;

    // A raw feature from before r_dlss_sl flipped on is pure duplication under SL.
    // Same idle rule as EnsureFeature: in-flight frames may still reference it.
    if (s_feature) { VK_DeviceWaitIdleLocked(); ReleaseFeature(); }

    s_fRenderW = renderW; s_fRenderH = renderH; s_fDispW = displayW; s_fDispH = displayH;
    {
        const float ratio = (renderW > 0 && renderH > 0)
            ? (float(displayW) / float(renderW)) * (float(displayH) / float(renderH)) : 1.0f;
        u32 phases = (u32)(8.0f * ratio + 0.5f);
        s_jitterCount = (phases < 8u) ? 8u : (phases > 64u ? 64u : phases);
    }
    s_slShadow = true;
    Msg("[VK DLSS] SL feature shadow: render %ux%u -> display %ux%u (%s), jitter phases %u (NGX feature owned by sl.dlss)",
        renderW, renderH, displayW, displayH,
        (renderW == displayW) ? "DLAA" : "upscale", s_jitterCount);
    return true;
}

void ReleaseFeature()
{
    s_ngxReserveMB.store(0, std::memory_order_relaxed);
    if (s_feature) { NVSDK_NGX_VULKAN_ReleaseFeature(s_feature); s_feature = nullptr; }
    s_slShadow = false;
    s_evalChain = false;   // whatever history existed died with the feature
    s_camPosValid = false;
    s_fRenderW = s_fRenderH = s_fDispW = s_fDispH = 0;
    s_fPreset  = 0xFFFFFFFFu;
    s_fInputsGen = 0xFFFFFFFFu;
    s_fExpMode = 0xFFFFFFFFu;   // symmetry with ReleaseFeatureRR — every "what was it built with" field invalidates together
}

bool Evaluate(VkCommandBuffer cmd, const VK::Dlss::Img& color, const VK::Dlss::Img& depth,
              const VK::Dlss::Img& mv, u32 renderW, u32 renderH, bool reset)
{
    if (s_feature == nullptr || s_params == nullptr || s_outImg == VK_NULL_HANDLE) return false;
    if (color.image == VK_NULL_HANDLE || depth.image == VK_NULL_HANDLE || mv.image == VK_NULL_HANDLE) return false;

    // NGX wants its VK resources in GENERAL. The INPUTS arrive in GENERAL already —
    // the caller transitions them straight from their attachment layouts (see the
    // layout contract in vk_dlss.h). Routing them via SHADER_READ here, as this used
    // to, meant 4 layout transitions per frame for colour and depth where 2 suffice,
    // and on NVIDIA every hop out of DEPTH_ATTACHMENT decompresses the whole 1440p
    // depth surface.
    // The output is fully (over)written by NGX every frame and the caller leaves it in
    // SHADER_READ after sampling it in the tonemap — so discard the previous contents
    // (UNDEFINED old layout) rather than tracking SHADER_READ vs GENERAL across frames.
    //
    // ⚠EXPLICIT stage/access, because the layout-derived pair is wrong here: GENERAL
    // derives to COMPUTE_SHADER + SHADER_STORAGE_READ|WRITE, and NGX does not only run
    // compute on this image — it also issues its own vkCmdClearColorImage on it inside
    // EvaluateFeature. Sync validation caught exactly that, 72 times in one run:
    // "allows SHADER_STORAGE_* at COMPUTE_SHADER, but to prevent this hazard it must
    // allow TRANSFER_WRITE at CLEAR". So cover the transfer side as well.
    // ALL_TRANSFER rather than CLEAR_BIT: it subsumes clear/copy/blit, and NGX's exact
    // choice of transfer op is not ours to predict.
    ImageBarrier(cmd, s_outImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);

    NVSDK_NGX_Resource_VK colorR = WrapImage(color, renderW, renderH, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK depthR = WrapImage(depth, renderW, renderH, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NVSDK_NGX_Resource_VK mvR    = WrapImage(mv,    renderW, renderH, VK_IMAGE_ASPECT_COLOR_BIT, false);
    VK::Dlss::Img outImg{ s_outView, s_outImg, s_outFmt };
    NVSDK_NGX_Resource_VK outR   = WrapImage(outImg, s_outW, s_outH, VK_IMAGE_ASPECT_COLOR_BIT, true);

    // Real exposure (r_dlss_exp 1): clear the 1x1 texture to the tonemap's smoothed
    // exposure (published via SetExposureHint; one frame stale — it eases over
    // seconds, so that's noise-free) and hand it to NGX in GENERAL.
    NVSDK_NGX_Resource_VK expR{};
    const bool useExpTex = (s_fExpMode == 1u) && s_expImg != VK_NULL_HANDLE;
    if (useExpTex) {
        ImageBarrier(cmd, s_expImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue cv{}; cv.float32[0] = (s_expHint > 1e-6f) ? s_expHint : 1.0f;
        VkImageSubresourceRange rr = FullRange(VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(cmd, s_expImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rr);
        ImageBarrier(cmd, s_expImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        VK::Dlss::Img expImg{ s_expView, s_expImg, VK_FORMAT_R32_SFLOAT };
        expR = WrapImage(expImg, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT, false);
    }

    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor  = &colorR;
    ep.Feature.pInOutput = &outR;
    ep.Feature.InSharpness = 0.0f;
    ep.pInDepth          = &depthR;
    ep.pInMotionVectors  = &mvR;
    if (useExpTex) ep.pInExposureTexture = &expR;
    // Reported jitter sign (r_dlss_jitter_flip). The matrix jitter adds +jx/+jy in
    // CLIP space, but the scene rasterizes through a NEGATIVE-HEIGHT viewport (D3D
    // convention) while DLSS expects the offset in pixel space (y-down) — so the Y
    // sign we report is convention-dependent. The cvar flips what we TELL Evaluate
    // (bit0=X, bit1=Y) without touching the applied matrix, for a live A/B: the
    // wrong sign misaligns the temporal history by up to 1px/axis every frame and
    // the upscale never converges past bilinear.
    ep.InJitterOffsetX   = s_jitterPixX;     // reported jitter: Y flipped (the verified sign)
    ep.InJitterOffsetY   = -s_jitterPixY;
    ep.InReset           = reset ? 1 : 0;
    ep.InRenderSubrectDimensions.Width  = renderW;
    ep.InRenderSubrectDimensions.Height = renderH;
    // Our MV is UV-space (prevUV − curUV); DLSS wants render-pixel space → scale by
    // render dims. Sign is the #1 thing to flip if the image ghosts/smears.
    ep.InMVScaleX = float(renderW);
    ep.InMVScaleY = float(renderH);
    // Real wall time the motion vectors span — see the note in EvaluateRR.
    ep.InFrameTimeDeltaInMsec = RV().fTimeDeltaRealMS;

    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, s_feature, s_params, &ep);
    if (NVSDK_NGX_FAILED(r)) { NoteEvalFailure("VK DLSS", (unsigned)r); return false; }
    // Inputs stay GENERAL and the output stays GENERAL — the caller restores every
    // one of them to the layout the rest of the frame needs (layout contract above).
    return true;
}

void SetExposureHint(float exposure)
{
    s_expHint = exposure;
}

void Shutdown()
{
    if (!s_inited) return;
    ReleaseFeature();
    ReleaseFeatureRR();
    ReleaseFeatureNR();
    if (s_outView) { vkDestroyImageView(VulkanHW.m_Device, s_outView, nullptr); s_outView = VK_NULL_HANDLE; }
    if (s_outImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_outImg, s_outAlloc); s_outImg = VK_NULL_HANDLE; s_outAlloc = VK_NULL_HANDLE; }
    if (s_expView) { vkDestroyImageView(VulkanHW.m_Device, s_expView, nullptr); s_expView = VK_NULL_HANDLE; }
    if (s_expImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_expImg, s_expAlloc); s_expImg = VK_NULL_HANDLE; s_expAlloc = VK_NULL_HANDLE; }
    if (s_params)   { NVSDK_NGX_VULKAN_DestroyParameters(s_params); s_params = nullptr; }
    if (s_paramsRR) { NVSDK_NGX_VULKAN_DestroyParameters(s_paramsRR); s_paramsRR = nullptr; }
    if (s_paramsNR) { NVSDK_NGX_VULKAN_DestroyParameters(s_paramsNR); s_paramsNR = nullptr; }
    if (s_caps)    { NVSDK_NGX_VULKAN_DestroyParameters(s_caps); s_caps = nullptr; }
    NVSDK_NGX_VULKAN_Shutdown1(VulkanHW.m_Device);
    s_inited = s_srAvail = s_fgAvail = s_rrAvail = s_nrAvail = false;
    // ⚠The create-failure latch is cleared here too. It records "this NGX runtime
    // refused this ID", and the NGX runtime does not survive the shutdown — carrying
    // the verdict into a re-Init would refuse a feature nobody has asked yet.
    s_nrFailedId = 0xFFFFFFFFu;
    // The render-extent cache is keyed on (display, quality) and answered by the
    // capability block we just destroyed — leaving it primed would let a re-Init
    // hand out dims nobody re-queried. Same for the availability the options screen
    // reads: after shutdown nothing is available, and the screen must not keep
    // offering a DLSS row that no longer has an NGX behind it.
    s_outW = s_outH = 0;
    s_reDispW = s_reDispH = s_reRenderW = s_reRenderH = 0;
    s_reQual  = 0xFFFFFFFFu;
    s_evalFailN = 0; s_evalFailLastLog = 0;
    ps_r_dlss_avail = 0;
    ps_r_dlss_rr_avail = 0;
    ps_r_dlss_nr_avail = 0;
    s_ngxReserveMB.store(0, std::memory_order_relaxed);
    Msg("[VK DLSS] NGX shut down");
}


// VRAM the live NGX feature needs kept FREE for it, in MB (0 when none is running).
// ⚠Read by the texture streamer's budget fit, from its worker threads: NGX allocates
// outside VMA, so the streamer's own accounting cannot see the feature at all and will
// happily refill the card straight over the top of it — which is the 4 fps case in the
// 28-08 19:37 log, arriving the slow way instead of the sudden one.
u32 VramReserveMB() { return s_ngxReserveMB.load(std::memory_order_relaxed); }

// True while the room gate is holding a feature off for want of VRAM. CRender::Begin
// skips the sub-pixel jitter then: jittering an image nothing is going to reconstruct
// is a shimmer for no gain at all.
bool FeatureBlocked() { return s_roomBlocked; }
}}  // namespace VK::Dlss

#endif // __aarch64__ (NGX stub above)

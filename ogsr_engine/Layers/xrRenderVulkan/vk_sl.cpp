// xrRenderVulkan — NVIDIA Streamline (SL) integration. See vk_sl.h.
#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_sl.h"

#ifndef _WIN32
// Streamline ships for Windows only (sl.interposer.dll). Off Windows SL is simply
// never available — the same state Init() leaves behind when slInit fails there,
// so every caller already takes its raw-NGX / no-FG route.
namespace VK { namespace SL {
bool Init() { Msg("[VK SL] Streamline is not available on this platform"); return false; }
void Shutdown() {}
bool Inited() { return false; }
void OnDeviceReady(VkPhysicalDevice) {}
bool SuperResAvailable() { return false; }
bool ReflexAvailable() { return false; }
bool FrameGenAvailable() { return false; }
bool EvaluateSR(VkCommandBuffer, const SrImg&, const SrImg&, const SrImg&, const SrImg&, const Fmatrix&,
                const Fmatrix&, float, float, bool) { return false; }
bool SrResolvedThisFrame() { return false; }
void FrameBegin() {}
bool FrameGenWanted() { return false; }
void ConfigureFG(u32, u32, u32, u32, u32) {}
void MarkSimEnd() {}
void MarkRenderSubmit(bool) {}
void MarkPresent(bool) {}
void TagHudless(VkCommandBuffer, VkImage, u32, u32, VkFormat) {}
void DebugTick() {}
}}  // namespace VK::SL
#else

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"

#include <vector>                  // NvidiaAdapterVisible probe

#include "../../xr_3da/device.h"   // RV().frameIndex + camera vectors (SR constants)
#include "vk_pass_ssao.h"          // ProjTerms / DeriveProjTerms (projection rebuild for sl::Constants)
#include "HW_Vulkan.h"             // VulkanHW (device/allocator) — hudless image
#include "vk_image.h"              // VK::CreateImage2D / CreateImageView
#include "vk_barriers.h"           // VK::ImageBarrier — hudless copy transitions

extern u32 ps_r_dlss_quality;     // 0=DLAA 1=Q 2=B 3=P 4=UltraP (shared with the raw-NGX path)
extern u32 ps_r_dlss_preset;      // DLSS model preset hint (11 = K transformer)
// Уровень логирования Streamline: 0 = eOff (по умолчанию), 1 = eVerbose.
extern int ps_r_sl_log;
extern u32 ps_r_dlss;             // r_dlss — DLSS master (FG requires it: SR feeds FG)
extern int ps_r_dlss_sl;          // r_dlss_sl — SR routed through SL (FG needs one NGX owner)
extern int ps_r_dlss_fg;          // r_dlss_fg — Stage C: enable DLSS Frame Generation (MFG)
extern int ps_r_dlss_fg_mult;     // r_dlss_fg_mult — frame multiplier 2..6 (numFramesToGenerate = mult-1)
extern int ps_r_dlss_fg_debug;    // r_dlss_fg_debug — periodic DLSS-G + Reflex state log

namespace VK { namespace SL {

namespace {
    bool s_inited     = false;
    bool s_srAvail    = false;
    bool s_reflexAvail= false;
    bool s_fgAvail    = false;

    // Per-frame SL token — acquired ONCE in FrameBegin and shared by the SR eval,
    // Reflex sleep and every PCL marker (they must all agree on the frame index or
    // SL's temporal + pacing state desyncs). Null on frames FrameBegin didn't run.
    sl::FrameToken* s_frameToken = nullptr;
    bool s_fgWanted   = false;   // FG resolved ON this frame (set by FrameBegin)
    bool s_fgActive   = false;   // slDLSSGSetOptions currently eOn (drives the OFF edge)
    int  s_reflexMode = 0;       // last mode sent to slReflexSetOptions (0=off,1=lowlat,2=+boost)
    u32  s_fgMaxGen   = 0;       // driver's numFramesToGenerateMax (0 = not queried yet)
    u32  s_fgGen      = 0;       // last numFramesToGenerate sent (mult-1); for the debug log

    // Features SL should load at init (plugins must be found next to the exe).
    // DLSS_G = the official signed sl.dlss_g.dll (closed-source, from the SL
    // release package) — listed here so the support probe answers before the
    // FG integration lands; a missing/failed plugin just reports unsupported.
    const sl::Feature kFeatures[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };

    // Any stable GUID identifies the app to SL/NGX telemetry (custom engine → no
    // NVIDIA-assigned applicationId, so engine type + projectId are used).
    const char* kProjectId = "b7f4e2a1-9c3d-4f6b-8a12-0e5d7c9a3b21";

    void SL_CALLBACK_LogCb(sl::LogType type, const char* msg)
    {
        const char* pfx = (type == sl::LogType::eError) ? "!" : (type == sl::LogType::eWarn ? "~" : "");
        Msg("%s[VK SL] %s", pfx, msg ? msg : "");
    }
}

// Is an NVIDIA GPU visible to Vulkan RIGHT NOW? A TDR (the level-transition
// GPU-hang) can leave the NVIDIA ICD temporarily un-enumerated for the next
// launch; the engine then falls back to the iGPU while the SL interposer still
// injects NV-only device extensions into vkCreateDevice → fatal -7 at startup
// (seen 2026-07-10). Probe with a throwaway instance BEFORE slInit — the linked
// interposer passes calls through until slInit, so this sees the raw ICD list.
static bool NvidiaAdapterVisible()
{
    VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS || inst == VK_NULL_HANDLE)
        return true;   // can't probe — don't block SL on a probe failure
    u32 n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    bool nv = false;
    if (n) {
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(inst, &n, devs.data());
        for (u32 i = 0; i < n && !nv; ++i) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(devs[i], &p);
            nv = (p.vendorID == 0x10DE);
        }
    }
    vkDestroyInstance(inst, nullptr);
    return nv;
}

bool Init()
{
    if (s_inited) return true;

    // Diagnostic kill switch: no slInit → the linked interposer stays a pure
    // pass-through (raw driver behavior, no SL plugins in the frame). DLSS/FG
    // are unavailable in such a run — it exists to A/B driver-level anomalies
    // (first user: the VRS/shading-rate investigation, 18-07-2026).
    if (strstr(Core.Params, "-no_sl")) {
        Msg("[VK SL] -no_sl: Streamline disabled by command line (pass-through interposer)");
        return false;
    }

    // A sleeping laptop dGPU can take a second or two to power up and appear in
    // the ICD list — a single early probe missed it (17-07 02:07: SL saw only
    // the iGPU and stayed OFF while the device selector, seconds later, found
    // the RTX fine; 01:57 was worse — the whole engine silently ran on the
    // 780M). Retry the probe for ~3s before giving up: a truly absent/TDR'd
    // adapter costs one 3s startup delay, a dozing one gets caught.
    bool nvVisible = NvidiaAdapterVisible();
    for (u32 tryN = 0; !nvVisible && tryN < 5; ++tryN) {
        Sleep(600);
        nvVisible = NvidiaAdapterVisible();
        if (nvVisible)
            Msg("~[VK SL] NVIDIA adapter appeared on probe retry %u (dGPU was waking up)", tryN + 1);
    }
    if (!nvVisible) {
        Msg("![VK SL] no NVIDIA adapter visible to Vulkan after retries — Streamline OFF (engine can run on the fallback GPU)");
        return false;
    }

    sl::Preferences pref{};
    pref.featuresToLoad    = kFeatures;
    pref.numFeaturesToLoad = (uint32_t)(sizeof(kFeatures) / sizeof(kFeatures[0]));
    pref.renderAPI         = sl::RenderAPI::eVulkan;   // MANDATORY so SL adds the right VK extensions
    pref.engine            = sl::EngineType::eCustom;
    pref.engineVersion     = "1.0";
    pref.projectId         = kProjectId;
    pref.logMessageCallback= &SL_CALLBACK_LogCb;
    // eOff, а не eDefault — но ⛔НЕ РАДИ ПРОИЗВОДИТЕЛЬНОСТИ, это проверено и
    // не подтвердилось. Прибор cpu_budget показал поток `sl.log` на
    // 0.13-0.15 core-ms/КАДР (1.5% процесса), и гипотеза была, что он занят
    // логированием. A/B: с eOff тот же поток жжёт 0.131 против 0.132 — уровень
    // логирования на него не влияет вообще, Streamline держит его под что-то
    // своё. Оставлено eOff только за тишину в логе.
    // ⚠`r_sl_log 1` возвращает eVerbose, когда DLSS ведёт себя странно.
    pref.logLevel          = ps_r_sl_log ? sl::LogLevel::eVerbose : sl::LogLevel::eOff;
    // flags: keep the SL defaults (eDisableCLStateTracking | eAllowOTA |
    // eLoadDownloadedPlugins) + eUseFrameBasedResourceTagging — REQUIRED for the
    // slSetTagForFrame path Stage B uses (without it every tag call errors and the
    // DLSS output freezes). NO eUseManualHooking — we use the linked interposer,
    // so SL proxies vkCreateInstance/Device automatically (no slSetVulkanInfo needed).
    pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    sl::Result r = slInit(pref);
    if (r != sl::Result::eOk) {
        Msg("![VK SL] slInit failed: %d — Streamline OFF (raw-NGX path stays active)", (int)r);
        return false;
    }
    s_inited = true;
    Msg("[VK SL] Streamline initialised (interposer, Vulkan, features DLSS+Reflex+PCL).");
    return true;
}

void OnDeviceReady(VkPhysicalDevice phys)
{
    if (!s_inited || phys == VK_NULL_HANDLE) return;
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = (void*)phys;
    s_srAvail     = (slIsFeatureSupported(sl::kFeatureDLSS,   adapter) == sl::Result::eOk);
    s_reflexAvail = (slIsFeatureSupported(sl::kFeatureReflex, adapter) == sl::Result::eOk);
    s_fgAvail     = (slIsFeatureSupported(sl::kFeatureDLSS_G, adapter) == sl::Result::eOk);
    Msg("[VK SL]   DLSS Super Resolution : %s", s_srAvail     ? "SUPPORTED" : "unsupported");
    Msg("[VK SL]   Reflex                : %s", s_reflexAvail ? "SUPPORTED" : "unsupported");
    Msg("[VK SL]   DLSS Frame Generation : %s", s_fgAvail     ? "SUPPORTED" : "unsupported");
}

bool Inited()            { return s_inited; }
bool SuperResAvailable() { return s_inited && s_srAvail; }
bool ReflexAvailable()   { return s_inited && s_reflexAvail; }
bool FrameGenAvailable() { return s_inited && s_fgAvail; }

// ============================================================================
// Stage B — DLSS Super Resolution via sl.dlss.
// ============================================================================
namespace {
    bool s_srResolved = false;
    u32  s_optW = 0, s_optH = 0, s_optMode = 0xFFFFFFFFu, s_optPreset = 0xFFFFFFFFu;
    const sl::ViewportHandle kViewport{ 0u };

    sl::DLSSMode ModeForCvar()
    {
        switch (ps_r_dlss_quality) {
            case 1:  return sl::DLSSMode::eMaxQuality;
            case 2:  return sl::DLSSMode::eBalanced;
            case 3:  return sl::DLSSMode::eMaxPerformance;
            case 4:  return sl::DLSSMode::eUltraPerformance;
            default: return sl::DLSSMode::eDLAA;
        }
    }

    // Row-vector (X-Ray/D3D) 4x4 product: v*dest == (v*a)*b.
    void Mul44(Fmatrix& dest, const Fmatrix& a, const Fmatrix& b)
    {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                dest.m[i][j] = a.m[i][0]*b.m[0][j] + a.m[i][1]*b.m[1][j]
                             + a.m[i][2]*b.m[2][j] + a.m[i][3]*b.m[3][j];
    }

    // Full 4x4 inverse (cofactor) — Fmatrix::invert only handles affine 4x3,
    // but view-proj has a projective 4th column.
    bool Inv44(Fmatrix& out, const Fmatrix& s)
    {
        const float* m = &s.m[0][0];
        float inv[16];
        inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
        inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
        inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
        inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
        inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
        inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
        inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
        inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
        inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
        inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
        inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
        inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
        inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
        inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
        inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
        inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
        float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
        if (_abs(det) < 1e-20f) return false;
        det = 1.0f / det;
        float* o = &out.m[0][0];
        for (int i = 0; i < 16; ++i) o[i] = inv[i] * det;
        return true;
    }

    sl::float4x4 ToSl(const Fmatrix& f)
    {
        // Row-major copy (X-Ray Fmatrix is row-vector). The transpose A/B knob was
        // retired: sl::Constants matrices don't reach NGX Super Resolution anyway
        // (verified against the SL sources), so the convention only had to be
        // internally consistent — row form is correct for clipToPrevClip etc.
        sl::float4x4 r;
        for (int i = 0; i < 4; ++i)
            r.setRow(i, sl::float4(f.m[i][0], f.m[i][1], f.m[i][2], f.m[i][3]));
        return r;
    }

    sl::Resource MakeRes(const SrImg& im, VkImageLayout state)
    {
        sl::Resource r{};
        r.type   = sl::ResourceType::eTex2d;
        r.native = (void*)im.img;
        r.view   = (void*)im.view;
        r.state  = (uint32_t)state;
        r.width  = im.w; r.height = im.h;
        r.nativeFormat = (uint32_t)im.fmt;
        r.mipLevels = 1; r.arrayLayers = 1;
        r.flags = 0; r.usage = 0;
        return r;
    }
}

bool SrResolvedThisFrame() { return s_srResolved; }

bool EvaluateSR(VkCommandBuffer cmd, const SrImg& color, const SrImg& depth, const SrImg& mv,
                const SrImg& output, const Fmatrix& curVP, const Fmatrix& prevVP,
                float jitterPixX, float jitterPixY, bool reset)
{
    s_srResolved = false;
    if (!s_inited || !s_srAvail || cmd == VK_NULL_HANDLE) return false;
    if (!color.img || !depth.img || !mv.img || !output.img) return false;

    // Options — (re)sent when mode/output/preset change (cheap struct, SL dedupes).
    const u32 mode = (u32)ModeForCvar();
    if (mode != s_optMode || output.w != s_optW || output.h != s_optH || ps_r_dlss_preset != s_optPreset) {
        sl::DLSSOptions o{};
        o.mode            = (sl::DLSSMode)mode;
        o.outputWidth     = output.w;
        o.outputHeight    = output.h;
        o.colorBuffersHDR = sl::Boolean::eTrue;
        o.useAutoExposure = sl::Boolean::eTrue;   // v1 parity knob; real-exposure tag later
        const sl::DLSSPreset p = (sl::DLSSPreset)ps_r_dlss_preset;
        o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset = p;
        const sl::Result r = slDLSSSetOptions(kViewport, o);
        if (r != sl::Result::eOk) {
            static bool once = false;
            if (!once) { once = true; Msg("![VK SL] slDLSSSetOptions failed: %d", (int)r); }
            return false;
        }
        s_optMode = mode; s_optW = output.w; s_optH = output.h; s_optPreset = ps_r_dlss_preset;
        Msg("[VK SL] DLSS options: mode %u, output %ux%u, preset %u", mode, output.w, output.h, ps_r_dlss_preset);
    }

    // Shared frame token (acquired in FrameBegin) — the SR eval, Reflex and the PCL
    // markers must all use the SAME per-frame index. FrameBegin runs unconditionally
    // when SL is inited, so this is set on every presenting frame.
    sl::FrameToken* tok = s_frameToken;
    if (tok == nullptr) return false;

    // Constants — jitter-free camera terms (same conventions as the NGX path).
    const ProjTerms pt = DeriveProjTerms(curVP);
    Fmatrix proj; proj.identity();
    proj._11 = 1.0f / pt.tanX;
    proj._22 = 1.0f / pt.tanY;
    proj._33 = pt.p33; proj._34 = 1.0f;
    proj._43 = pt.p43; proj._44 = 0.0f;

    Fmatrix invProj, invCur, invPrev, clipToPrev, prevToClip;
    if (!Inv44(invProj, proj) || !Inv44(invCur, curVP) || !Inv44(invPrev, prevVP)) return false;
    Mul44(clipToPrev, invCur, prevVP);   // clip → world → prev clip
    Mul44(prevToClip, invPrev, curVP);

    // Reported jitter sign — Y flipped, the same answer NGX verified.
    const float jx = jitterPixX;
    const float jy = -jitterPixY;

    const float nearZ = (pt.p33 != 0.0f) ? (-pt.p43 / pt.p33) : 0.2f;
    const float farZ  = (pt.p33 != 1.0f) ? (pt.p33 * nearZ / (pt.p33 - 1.0f)) : 1000.0f;

    sl::Constants c{};
    c.cameraViewToClip = ToSl(proj);
    c.clipToCameraView = ToSl(invProj);
    c.clipToPrevClip   = ToSl(clipToPrev);
    c.prevClipToClip   = ToSl(prevToClip);
    c.jitterOffset     = { jx, jy };
    // sl.dlss computes the NGX MV scale as mvecScale * renderDim (dlssEntry.cpp:684)
    // — our MV is UV-space, so the correct factor here is 1.0, NOT the render dims
    // (that squared the scale ≈ ×850: history got rejected wherever MV != 0 — wind
    // foliage / any motion — which read as shimmer once render < display).
    c.mvecScale        = { 1.0f, 1.0f };
    c.cameraPinholeOffset = { 0.0f, 0.0f };
    c.cameraPos   = { RV().vCameraPosition.x, RV().vCameraPosition.y, RV().vCameraPosition.z };
    c.cameraUp    = { RV().vCameraTop.x,      RV().vCameraTop.y,      RV().vCameraTop.z };
    c.cameraRight = { RV().vCameraRight.x,    RV().vCameraRight.y,    RV().vCameraRight.z };
    c.cameraFwd   = { RV().vCameraDirection.x,RV().vCameraDirection.y,RV().vCameraDirection.z };
    c.cameraNear  = nearZ;
    c.cameraFar   = farZ;
    c.cameraFOV   = 2.0f * atanf(pt.tanY);
    c.cameraAspectRatio    = pt.tanX / pt.tanY;
    c.depthInverted        = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D      = sl::Boolean::eFalse;
    c.motionVectorsJittered= sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (slSetConstants(c, *tok, kViewport) != sl::Result::eOk) return false;

    // Resource tags. Inputs sit in SHADER_READ (CRender::End barriers), output in
    // GENERAL. Depth: VK aspect must be DEPTH-only (guide) — chain a SubresourceRange.
    sl::Resource colorR = MakeRes(color,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sl::Resource depthR = MakeRes(depth,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sl::Resource mvR    = MakeRes(mv,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sl::Resource outR   = MakeRes(output, VK_IMAGE_LAYOUT_GENERAL);
    sl::SubresourceRange depthRange{};
    depthRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthRange.baseMipLevel = 0; depthRange.levelCount = 1;
    depthRange.baseArrayLayer = 0; depthRange.layerCount = 1;
    depthR.next = &depthRange;

    // With FG on, DLSS-G's async pacer reads depth + motion vectors at PRESENT time
    // (after our command buffer is submitted), so those two must be tagged
    // eValidUntilPresent — the SR-only inputs (color) + output stay eOnlyValidNow.
    const sl::ResourceLifecycle mvDepthLife = s_fgWanted
        ? sl::ResourceLifecycle::eValidUntilPresent
        : sl::ResourceLifecycle::eOnlyValidNow;
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&colorR, sl::kBufferTypeScalingInputColor,  sl::ResourceLifecycle::eOnlyValidNow),
        sl::ResourceTag(&depthR, sl::kBufferTypeDepth,              mvDepthLife),
        sl::ResourceTag(&mvR,    sl::kBufferTypeMotionVectors,      mvDepthLife),
        sl::ResourceTag(&outR,   sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow),
    };
    if (slSetTagForFrame(*tok, kViewport, tags, 4, (sl::CommandBuffer*)cmd) != sl::Result::eOk) return false;

    const sl::BaseStructure* inputs[] = { &kViewport };
    const sl::Result r = slEvaluateFeature(sl::kFeatureDLSS, *tok, inputs, 1, (sl::CommandBuffer*)cmd);
    if (r != sl::Result::eOk) {
        static bool once = false;
        if (!once) { once = true; Msg("![VK SL] slEvaluateFeature(DLSS) failed: %d", (int)r); }
        return false;
    }
    s_srResolved = true;
    return true;
}

// ============================================================================
// Stage C — DLSS Frame Generation (MFG) via sl.dlss_g + Reflex/PCL pacing.
// ============================================================================
namespace {
    // FG is only allowed on top of SR-under-SL (one NGX owner) with the plugin
    // supported. Quality can be anything incl. DLAA (render==display).
    bool FgWantedNow()
    {
        return s_inited && s_fgAvail && ps_r_dlss_fg && ps_r_dlss && ps_r_dlss_sl;
    }

    // Reflex is a hard DLSS-G requirement. Set the mode only when it changes.
    void EnsureReflex(int mode)
    {
        if (mode == s_reflexMode) return;
        sl::ReflexOptions o{};
        o.mode = (mode >= 2) ? sl::ReflexMode::eLowLatencyWithBoost
               : (mode == 1) ? sl::ReflexMode::eLowLatency
                             : sl::ReflexMode::eOff;
        if (slReflexSetOptions(o) == sl::Result::eOk) {
            s_reflexMode = mode;
            Msg("[VK SL] Reflex mode -> %d", mode);
        }
    }

    void Marker(sl::PCLMarker m)
    {
        if (!s_fgWanted || s_frameToken == nullptr) return;
        slPCLSetMarker(m, *s_frameToken);
    }
}

bool FrameGenWanted() { return s_fgWanted; }

void FrameBegin()
{
    s_frameToken = nullptr;
    s_fgWanted   = false;
    if (!s_inited) return;

    // One token per presenting frame, from SL's own counter (matches the interposer's
    // present count). Shared with EvaluateSR + Reflex + PCL below.
    sl::FrameToken* tok = nullptr;
    if (slGetNewFrameToken(tok, nullptr) != sl::Result::eOk || tok == nullptr) return;
    s_frameToken = tok;

    s_fgWanted = FgWantedNow();

    // Diagnostic: the user asked for FG (r_dlss_fg 1) but a prerequisite is missing.
    // FG needs the whole chain r_dlss -> r_dlss_sl -> r_dlss_fg (+ plugin support),
    // so a bare "r_dlss_fg 1" silently does nothing. Log WHICH link is off, once per
    // state change, so it's obvious instead of an empty [VK SL FG] log.
    if (ps_r_dlss_fg && !s_fgWanted) {
        int miss = (!s_fgAvail ? 1 : 0) | (!ps_r_dlss ? 2 : 0) | (!ps_r_dlss_sl ? 4 : 0);
        static int s_lastMiss = -1;
        if (miss != s_lastMiss) {
            s_lastMiss = miss;
            Msg("~[VK SL] r_dlss_fg is 1 but Frame Generation is INACTIVE — missing:%s%s%s. "
                "Enable the full chain: r_dlss 1; r_dlss_sl 1; r_dlss_fg 1 (r_motion_vectors must stay 1).",
                (miss & 2) ? " r_dlss(SR master)" : "",
                (miss & 4) ? " r_dlss_sl(SR via Streamline)" : "",
                (miss & 1) ? " DLSS-G-unsupported-on-GPU" : "");
        }
    }

    if (s_fgWanted) {
        EnsureReflex(1);                      // Reflex ON (DLSS-G requires it)
        slReflexSleep(*s_frameToken);         // latency throttle at frame start
        slPCLSetMarker(sl::PCLMarker::eSimulationStart, *s_frameToken);
    } else if (s_fgActive) {
        // User just disabled FG — turn DLSS-G off once + drop Reflex.
        sl::DLSSGOptions off{}; off.mode = sl::DLSSGMode::eOff;
        slDLSSGSetOptions(kViewport, off);
        s_fgActive = false;
        EnsureReflex(0);
        Msg("[VK SL] DLSS-G disabled");
    }
}

void ConfigureFG(u32 numFramesToGenerate, u32 renderW, u32 renderH, u32 displayW, u32 displayH)
{
    if (!s_fgWanted) return;

    // Query the driver's max multiplier once (2x..6x depending on GPU) + clamp.
    if (s_fgMaxGen == 0) {
        sl::DLSSGState st{};
        if (slDLSSGGetState(kViewport, st, nullptr) == sl::Result::eOk && st.numFramesToGenerateMax > 0) {
            s_fgMaxGen = st.numFramesToGenerateMax;
            Msg("[VK SL] DLSS-G max frames to generate: %u (up to %ux)", s_fgMaxGen, s_fgMaxGen + 1);
        }
    }
    u32 gen = numFramesToGenerate ? numFramesToGenerate : 1u;
    if (s_fgMaxGen && gen > s_fgMaxGen) gen = s_fgMaxGen;

    sl::DLSSGOptions o{};
    o.mode                = sl::DLSSGMode::eOn;
    o.numFramesToGenerate = gen;
    o.mvecDepthWidth      = renderW;   o.mvecDepthHeight = renderH;   // FG inputs are render-res
    o.colorWidth          = displayW;  o.colorHeight     = displayH;  // backbuffer is display-res

    const sl::Result r = slDLSSGSetOptions(kViewport, o);
    if (r != sl::Result::eOk) {
        static bool once = false;
        if (!once) { once = true; Msg("![VK SL] slDLSSGSetOptions failed: %d", (int)r); }
        return;
    }
    s_fgGen = gen;
    if (!s_fgActive) {
        s_fgActive = true;
        Msg("[VK SL] DLSS-G enabled: %ux (%u interpolated), inputs %ux%u -> present %ux%u",
            gen + 1, gen, renderW, renderH, displayW, displayH);
    }
}

void MarkSimEnd()                 { Marker(sl::PCLMarker::eSimulationEnd); }
void MarkRenderSubmit(bool begin) { Marker(begin ? sl::PCLMarker::eRenderSubmitStart : sl::PCLMarker::eRenderSubmitEnd); }
void MarkPresent(bool begin)      { Marker(begin ? sl::PCLMarker::ePresentStart      : sl::PCLMarker::ePresentEnd); }

// ---- Hudless (UI-less backbuffer) capture for DLSS-G --------------------------
namespace {
    VkImage       s_hudImg   = VK_NULL_HANDLE;
    VmaAllocation s_hudAlloc = VK_NULL_HANDLE;
    VkImageView   s_hudView  = VK_NULL_HANDLE;
    u32           s_hudW = 0, s_hudH = 0;
    VkFormat      s_hudFmt = VK_FORMAT_UNDEFINED;

    void EnsureHudless(u32 w, u32 h, VkFormat fmt)
    {
        if (s_hudImg != VK_NULL_HANDLE && w == s_hudW && h == s_hudH && fmt == s_hudFmt) return;
        if (s_hudView) { vkDestroyImageView(VulkanHW.m_Device, s_hudView, nullptr); s_hudView = VK_NULL_HANDLE; }
        if (s_hudImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_hudImg, s_hudAlloc); s_hudImg = VK_NULL_HANDLE; s_hudAlloc = VK_NULL_HANDLE; }
        if (w == 0 || h == 0 || fmt == VK_FORMAT_UNDEFINED) return;

        // fmt == swapchain format (raw copy of it)
        if (!VK::CreateImage2D(fmt, { w, h },
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                s_hudImg, s_hudAlloc, "SL.Hudless"))
            return;
        s_hudView = VK::CreateImageView(s_hudImg, fmt);
        if (s_hudView == VK_NULL_HANDLE) {
            VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_hudImg, s_hudAlloc); s_hudImg = VK_NULL_HANDLE; return;
        }
        s_hudW = w; s_hudH = h; s_hudFmt = fmt;
        Msg("[VK SL] hudless buffer ready (%ux%u) for DLSS-G UI recomposition", w, h);
    }
}

void TagHudless(VkCommandBuffer cmd, VkImage swapImg, u32 w, u32 h, VkFormat fmt)
{
    if (!s_fgWanted || s_frameToken == nullptr || cmd == VK_NULL_HANDLE || swapImg == VK_NULL_HANDLE) return;
    EnsureHudless(w, h, fmt);
    if (s_hudImg == VK_NULL_HANDLE) return;

    // Snapshot the tonemapped, UI-less swapchain into the hudless buffer. The
    // swapchain is COLOR_ATTACHMENT here (post-tonemap); round-trip it through
    // TRANSFER_SRC and restore it so the UI pass can keep drawing onto it.
    VK::ImageBarrier(cmd, swapImg,  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VK::ImageBarrier(cmd, s_hudImg, VK_IMAGE_LAYOUT_UNDEFINED,                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { w, h, 1 };
    vkCmdCopyImage(cmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   s_hudImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VK::ImageBarrier(cmd, s_hudImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VK::ImageBarrier(cmd, swapImg,  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    SrImg hud{ s_hudImg, s_hudView, s_hudFmt, w, h };
    sl::Resource hr = MakeRes(hud, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sl::ResourceTag tag(&hr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent);
    slSetTagForFrame(*s_frameToken, kViewport, &tag, 1, (sl::CommandBuffer*)cmd);
}

namespace {
    // Log the shipped SL + NGX versions of each FG-relevant plugin ONCE — confirms
    // we're on the freshest Streamline stack (and which Reflex the driver bound).
    void LogFeatureVersionsOnce()
    {
        static bool done = false;
        if (done) return;
        done = true;
        struct { sl::Feature f; const char* name; } feats[] = {
            { sl::kFeatureDLSS_G, "DLSS-G (Frame Gen)" },
            { sl::kFeatureReflex, "Reflex" },
            { sl::kFeaturePCL,    "PCL" },
        };
        for (const auto& e : feats) {
            sl::FeatureVersion v{};
            if (slGetFeatureVersion(e.f, v) == sl::Result::eOk) {
                Msg("[VK SL FG] %-18s SL v%u.%u.%u  NGX v%u.%u.%u",
                    e.name, v.versionSL.major, v.versionSL.minor, v.versionSL.build,
                    v.versionNGX.major, v.versionNGX.minor, v.versionNGX.build);
            }
        }
    }

    void AppendStatus(char* buf, size_t cap, sl::DLSSGStatus st)
    {
        if (st == sl::DLSSGStatus::eOk) { xr_strcat(buf, cap, "OK"); return; }
        if (st & sl::DLSSGStatus::eFailResolutionTooLow)               xr_strcat(buf, cap, "ResTooLow ");
        if (st & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)     xr_strcat(buf, cap, "ReflexMissing ");
        if (st & sl::DLSSGStatus::eFailHDRFormatNotSupported)          xr_strcat(buf, cap, "HDRUnsupported ");
        if (st & sl::DLSSGStatus::eFailCommonConstantsInvalid)         xr_strcat(buf, cap, "BadConstants ");
        if (st & sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled) xr_strcat(buf, cap, "NoBackBufIdx ");
    }
}

void DebugTick()
{
    if (!ps_r_dlss_fg_debug || !s_inited) return;

    static u32 s_tick = 0;
    ++s_tick;
    if ((s_tick % 120u) != 0u) return;   // ~ once every 120 frames (both levels)

    LogFeatureVersionsOnce();

    // Level 1 (SAFE): only our own tracked state — no SL queries at all. Confirms FG
    // is configured on + the multiplier + Reflex mode + the GPU's max multiplier.
    Msg("[VK SL FG] FG active=%d  %ux (%u interpolated)  reflexMode=%d  maxGen=%u (up to %ux)",
        s_fgActive ? 1 : 0, s_fgGen + 1, s_fgGen, s_reflexMode, s_fgMaxGen, s_fgMaxGen + 1);

    // Level 2 (DEEP, may destabilise): slReflexGetState + slDLSSGGetState are marked
    // NOT thread-safe and the DLSS-G present pacer calls them on its own thread — a
    // concurrent query from this (render) thread can race the pacer (seen as
    // "Couldn't lock the mutex on sync present" -> a skipped present). So the live
    // frames-presented / low-latency read is opt-in at level 2 only; use it briefly.
    if (ps_r_dlss_fg_debug < 2) return;

    static bool warned = false;
    if (!warned) { warned = true; Msg("~[VK SL FG] level 2: querying SL state from the render thread (may race the FG pacer)"); }

    sl::ReflexState rs{};
    if (slReflexGetState(rs) == sl::Result::eOk) {
        Msg("[VK SL FG] Reflex: lowLatencyAvailable=%d latencyReport=%d",
            rs.lowLatencyAvailable ? 1 : 0, rs.latencyReportAvailable ? 1 : 0);
    }

    sl::DLSSGState st{};
    if (slDLSSGGetState(kViewport, st, nullptr) == sl::Result::eOk) {
        char status[128] = {0};
        AppendStatus(status, sizeof(status), st.status);
        Msg("[VK SL FG] DLSS-G: status=[%s] framesPresented=%u minDim=%u vram=%.0f MB vsyncAvail=%d",
            status, st.numFramesActuallyPresented, st.minWidthOrHeight,
            (double)st.estimatedVRAMUsageInBytes / (1024.0 * 1024.0),
            st.bIsVsyncSupportAvailable == sl::Boolean::eTrue ? 1 : 0);
    }
}

void Shutdown()
{
    if (!s_inited) return;
    if (s_fgActive) {
        sl::DLSSGOptions off{}; off.mode = sl::DLSSGMode::eOff;
        slDLSSGSetOptions(kViewport, off);
        s_fgActive = false;
    }
    if (s_hudView) { vkDestroyImageView(VulkanHW.m_Device, s_hudView, nullptr); s_hudView = VK_NULL_HANDLE; }
    if (s_hudImg)  { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_hudImg, s_hudAlloc); s_hudImg = VK_NULL_HANDLE; s_hudAlloc = VK_NULL_HANDLE; }
    slShutdown();
    s_inited = s_srAvail = s_reflexAvail = s_fgAvail = false;
    Msg("[VK SL] shut down");
}

}}  // namespace VK::SL
#endif // _WIN32

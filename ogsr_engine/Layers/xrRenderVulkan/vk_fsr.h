// xrRenderVulkan — AMD FidelityFX Super Resolution 3.1.4 (FSR3 Upscaler).
//
// ⭐WHY THIS EXISTS AT ALL: this renderer had exactly one temporal upscaler and it
// was NVIDIA-only. Measured on a Radeon 780M (pripyat_full, 2560x1600) the frame
// was 75 ms with `[VK] render extent 2560x1600 -> display 2560x1600, set by
// native` — 4.1 megapixels drawn in full, because r_dlss was on in user.ltx and
// NGX cannot initialise on an AMD device, so `Dlss::Enabled()` stayed false and
// NOTHING took its place. Screen-space passes were 63 of those 75 ms.
//
// ⚠⚠THE SEAM IS SHARED, THE MODULES ARE NOT. Every hard-won rule in vk_dlss.h
// applies here and is repeated where it bites, because the two upscalers are
// deliberately NOT refactored behind one façade: DLSS works, and rebuilding a
// working path to make room for a new one is how both end up broken. Each seam
// point in CRender_Vulkan asks `Dlss::Upscaling()` first and `Fsr::Upscaling()`
// second, and the two can never be on at once (see Enabled()).
//
// Version note: SDK v1.1.4 = FSR 3.1.4, the last release before FSR 4 — and FSR 4
// is RDNA4-only, so 3.1.4 is not a compromise here, it is the one that runs on
// the hardware this was written for.
#pragma once
#include "vk_core.h"

namespace VK { namespace Fsr {

// Backend + context bring-up on the live VK device. Idempotent; safe to call
// every frame (returns the cached answer). Unlike NGX this cannot fail for
// vendor reasons — the FidelityFX VK backend is plain compute — so a false here
// means a real error, and it is logged once.
bool Init();
void Shutdown();

bool Inited();       // the backend interface + upscaler context are live
bool Available();    // usable on this device (see the feature notes in Init)
bool Enabled();      // r_fsr != 0 && Available() && !Dlss::Upscaling()

// ⚠MIRRORS Dlss::GetRenderExtent EXACTLY, including the "quality 0 → display
// dims" rule: FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA is 1.0x, i.e. our DLAA —
// temporal AA at native res, no upscale. CRender::Begin sizes every scene target
// to what this returns.
void GetRenderExtent(u32 displayW, u32 displayH, u32& outW, u32& outH);
bool Upscaling();    // true when the current preset renders BELOW display res

// Same bundle shape as Dlss::Img so the call site reads the same.
struct Img {
    VkImageView view   = VK_NULL_HANDLE;
    VkImage     image  = VK_NULL_HANDLE;
    VkFormat    format = VK_FORMAT_UNDEFINED;
};

// ⭐Call once per frame from CRender::Begin, next to the other frame-start resizes.
// The ONLY place the context and the output target are built or torn down: doing it
// from Evaluate meant destroying objects the frame command buffer already referenced.
void OnFrameBegin(u32 displayW, u32 displayH);

// Display-res upscaled output
void        EnsureOutput(u32 displayW, u32 displayH);
VkImageView OutputView();
VkImage     OutputImage();
VkFormat    OutputFormat();
bool        OutputReady();

// ⚠⚠THE TONEMAP GATE. Cleared by NewFrameJitter, set by Evaluate's return value.
// The DLSS path shipped a bug where a failed evaluate never reached this gate and
// the tonemap composited an unwritten image as its base colour — black, or the
// last frame frozen. Do not let this one drift into `void`.
bool ResolvedThisFrame();
void SetResolvedThisFrame(bool ok);

// Per-frame sub-pixel jitter. ⚠NOT interchangeable with the DLSS one: FSR derives
// its phase count from the render/display ratio (ffxFsr3UpscalerGetJitterPhaseCount)
// and feeding it a sequence of the wrong length is a slow, subtle smear rather
// than an obvious break. Call ONCE per frame in CRender::Begin, then post-multiply
// the COMBINED view-proj by a clip translation from GetProjJitterNDC — the same
// reason spelled out in vk_dlss.h: our mFullTransform is already combined, so the
// proj._31/_32 trick does not apply.
void NewFrameJitter(u32 renderW, u32 renderH, u32 displayW);
void GetProjJitterNDC(float& outX, float& outY);   // NDC offset for the combined VP
void GetJitterPix(float& outX, float& outY);       // render-pixel offset handed to FSR

// Discard temporal history on a discontinuity the motion vectors cannot describe
// (level load, teleport, a gap in the eval chain). Call ONCE per evaluated frame —
// it rolls its own detector state — and pass the result to Evaluate as `reset`.
bool TakeHistoryReset();

// Run FSR3: color(render) + depth(render) + mv(render) + jitter → OutputView (display).
//
// ⚠LAYOUT CONTRACT, identical to Dlss::Evaluate: color/depth/mv must ALREADY be in
// GENERAL — the caller owns the input transitions because it is the one that knows
// their attachment layouts. Evaluate touches only its OWN output and leaves it in
// GENERAL for the caller to flip to SHADER_READ.
//
// ⚠RETURNS whether the output was actually written and the caller MUST forward it
// to SetResolvedThisFrame. See the note on the gate above.
bool Evaluate(VkCommandBuffer cmd, const Img& color, const Img& depth, const Img& mv,
              u32 renderW, u32 renderH, u32 displayW, u32 displayH, bool reset);

// Scene exposure. FSR is created with AUTO_EXPOSURE, so this is advisory only and
// exists so the two upscalers take the same call from the tonemap.
void SetExposureHint(float exposure);

// One line of "what is FSR actually doing", for the log and the `r_fsr` report.
// Returns false when nothing is running.
bool DescribeState(char* buf, u32 bufSize);

}} // namespace VK::Fsr

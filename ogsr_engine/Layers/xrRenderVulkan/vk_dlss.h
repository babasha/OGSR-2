// xrRenderVulkan — NVIDIA DLSS 4.5 integration (NGX).
//
// Increment 1: NGX init on the live VK device + capability probe. Logs what this
// GPU/driver actually supports (DLSS Super Resolution + DLSS-G Multi-Frame-Gen) so
// the rest of the integration (feature create → jitter → evaluate → present) can
// gate on real availability. Forward-friendly module (like VK::MotionVec) — does
// NOT revive the parked deferred CRenderTarget DLSS scaffold.
#pragma once
#include "vk_core.h"

namespace VK { namespace Dlss {

bool Init();       // NGX init + capability probe (call after the VK device is up). Idempotent.
void Shutdown();   // release capability params + NGX (call at renderer teardown).

bool Inited();             // NGX initialised OK on this device
bool SuperResAvailable();  // DLSS Super Resolution usable on this GPU/driver
bool FrameGenAvailable();  // DLSS-G / Multi-Frame Generation usable on this GPU/driver

// ----- Super Resolution (increment 2) ---------------------------------------
bool        Enabled();     // r_dlss != 0 && SuperResAvailable() (gate for the eval pass)

// Render<display upscale (increment 2c). Fills the recommended RENDER resolution
// for the current r_dlss_quality preset at this display size — NGX optimal
// settings, cached per (display,quality). quality 0 (DLAA) or query failure →
// returns display dims (native, render==display). CRender::Begin sizes all scene
// targets to this and DLSS upscales back to display.
void        GetRenderExtent(u32 displayW, u32 displayH, u32& outW, u32& outH);
bool        Upscaling();   // true when the current quality preset renders below display (render<display)

// A small input bundle so Evaluate stays decoupled from SceneColor/MotionVec/etc.
struct Img {
    VkImageView view  = VK_NULL_HANDLE;
    VkImage     image = VK_NULL_HANDLE;
    VkFormat    format = VK_FORMAT_UNDEFINED;
};

// Display-res upscaled output target (RGBA16F storage). Created on demand / resize.
void        EnsureOutput(u32 displayW, u32 displayH);
VkImageView OutputView();
VkImage     OutputImage();
VkFormat    OutputFormat();
bool        OutputReady();
bool        FeatureReady();   // the SR feature is built (Evaluate ran this frame → tonemap samples OutputView)

// Per-frame sub-pixel jitter (Halton 2,3). Call NewFrameJitter ONCE per frame in
// CRender::Begin (after Device.mFullTransform is final), then post-multiply the
// COMBINED view-proj by a clip translation T (T._41=jx, T._42=jy from
// GetProjJitterNDC) so clip.x += jx*clip.w — the naive proj._31/_32 trick only
// works on a SEPARATE proj matrix, not our combined mFullTransform. Evaluate is
// told the SAME offset in pixel space (s_jitterPix) to un-jitter the result.
void        NewFrameJitter(u32 renderW, u32 renderH);
void        GetProjJitterNDC(float& outX, float& outY);   // NDC offset for the combined view-proj
void        GetJitterPix(float& outX, float& outY);       // raw render-pixel jitter (SL path reports it too)

// (Re)create the DLSS Super Resolution feature for these dims. Records feature
// init into `cmd` on first build. render==display => DLAA (no upscale). Returns
// true when the feature is ready to Evaluate. `inputsGen` = generation of the
// input render targets (SceneColor) — the feature is REBUILT when it changes:
// NGX caches internal state against the input images, so evaluating a feature
// built before the targets were destroyed/recreated (e.g. the r_dlss off→on
// toggle resizes them while the feature idles) page-faults the GPU.
bool        EnsureFeature(VkCommandBuffer cmd, u32 renderW, u32 renderH, u32 displayW, u32 displayH, u32 inputsGen);
void        ReleaseFeature();

// SL path (r_dlss_sl): the NGX SR feature lives INSIDE sl.dlss — building the raw
// one too made VRAM hold TWO tensor sets, so a mid-game enable on a saturated card
// failed sl.dlss's own create (NGX 0xbad00002 OOM) and Streamline then threw out
// of its vkQueuePresentKHR hook. This variant keeps only the bookkeeping the
// render loop needs (dim cache + jitter phase count) and RELEASES any raw feature.
bool        EnsureFeatureSL(u32 renderW, u32 renderH, u32 displayW, u32 displayH);

// Did this frame's evaluate actually resolve into OutputView? Cleared each
// NewFrameJitter. The tonemap must not sample the DLSS output otherwise — a
// failed SL evaluate leaves it undefined (the "blue screen").
bool        ResolvedThisFrame();
void        SetResolvedThisFrame(bool ok);

// Should this frame's evaluate DISCARD the temporal history? DLSS reprojects the
// previous OUTPUT through this frame's motion vectors — valid only while the MVs
// actually describe the change. Call ONCE per evaluated frame (it rolls the
// detector's own state) and pass the result to Evaluate/SL::EvaluateSR as `reset`.
bool        TakeHistoryReset();

// Run DLSS: color(render) + depth + mv(render) + jitter → OutputView (display).
// `reset` = 1 discards temporal history (see TakeHistoryReset).
// ⚠LAYOUT CONTRACT: color/depth/mv must ALREADY be in GENERAL — the caller owns
// the input transitions (it is the one that knows their attachment layouts, and
// routing them through SHADER_READ on the way just doubled the transition count
// on a 1440p depth surface). Evaluate transitions only its OWN output and leaves
// it in GENERAL for the caller to flip to SHADER_READ.
//
// ⚠RETURNS whether the output was actually written, and the caller MUST forward it
// to SetResolvedThisFrame. A failed evaluate leaves OutputView undefined and the
// tonemap composites it as the base colour — a black or frozen screen. This used to
// be `void`: the SL route propagated its failure and the raw NGX route silently
// reported success, which is precisely the case VRAM pressure makes reachable.
bool        Evaluate(VkCommandBuffer cmd, const Img& color, const Img& depth, const Img& mv,
                     u32 renderW, u32 renderH, bool reset);

// Real scene exposure for DLSS (r_dlss_exp 1, default): the tonemap publishes its
// CPU-smoothed exposure here each frame; Evaluate feeds it to NGX as a 1x1
// exposure texture instead of the AutoExposure flag. Bounded internal range =
// the fix for black undershoot blotches on high-contrast sub-pixel foliage
// (R4's DLSS never sees them because it upscales the TONEMAPPED LDR image).
void        SetExposureHint(float exposure);

// ============================================================================
//  RAY RECONSTRUCTION (DLSS-D / NGX Feature_RayReconstruction) — r_dlss_rr
// ============================================================================
//  RR is not a second pass bolted next to Super Resolution: NVIDIA's own guide
//  says "when DLSS-RR is enabled, it effectively overrides DLSS-SR execution".
//  So it lives in the SAME slot, consumes the SAME jitter / jitter-free motion
//  vectors / display-res output image / tonemap gate, and the two features are
//  mutually exclusive — turning one on RELEASES the other (they each hold a full
//  tensor set; keeping both resident is how the SL path once hit an NGX OOM).
//
//  What it adds over SR is the guide G-buffer (vk_rr_guides): with albedo,
//  normal and roughness in hand the network reconstructs the LIGHTING rather
//  than the pixel, which is what lets it remove ray-traced noise instead of
//  smearing it. Without honest guides RR is strictly worse than SR — hence
//  RayReconEnabled() also requires RRGuide::Ready().
// ============================================================================
// VRAM (MB) the LIVE NGX feature needs kept free for it, 0 when none is running. The
// texture streamer adds this to its protected headroom — NGX allocates outside VMA, so
// nothing else in the renderer can see the feature is there. See vk_dlss.cpp's room gate.
u32 VramReserveMB();

// True while that gate is holding the feature off — no NGX feature will be built this
// frame, so the caller must NOT jitter the camera (nothing would resolve the jitter).
bool FeatureBlocked();

bool RayReconAvailable();   // NGX reports the RR feature usable on this GPU/driver
bool RayReconEnabled();     // r_dlss && r_dlss_rr && available && guides ready

// Same contract as EnsureFeature (dims + input generation → rebuild), plus the
// guide generation: NGX caches internal state against the input images, and the
// guide targets are created/destroyed by their own toggle.
bool EnsureFeatureRR(VkCommandBuffer cmd, u32 renderW, u32 renderH,
                     u32 displayW, u32 displayH, u32 inputsGen);

// Run Ray Reconstruction: color+depth+mv+guides (render res) → OutputView (display).
// ⚠SAME LAYOUT CONTRACT as Evaluate: every input must ALREADY be in GENERAL; only
// the output is transitioned here, and it is left in GENERAL for the caller.
// ⚠pInMotionVectorsReflections is NOT optional. The SDK guide marks sections
// 3.4.10 onward "Optional only" and 3.4.8 (specular motion vectors) / 3.4.9
// (specular hit distance) NOT — one of the two belongs to the required set, and we
// were passing neither. We hand it the ORDINARY motion vector field, and that is
// the honest value rather than a stand-in: this renderer traces no specular rays.
// Its specular is sky IBL plus a sun lobe — a reflection with no parallax of its
// own — so the screen motion of the specular IS the screen motion of the surface.
// A renderer with traced reflections would have to compute a real one.
// ⚠RETURNS "the output was written", same contract (and same reason) as Evaluate.
bool EvaluateRR(VkCommandBuffer cmd, const Img& color, const Img& depth, const Img& mv,
                const Img& normalRough, const Img& diffAlbedo, const Img& specAlbedo,
                u32 renderW, u32 renderH, bool reset);

// ============================================================================
//  NEURAL RENDERING (DLSS-NR / nvngx_dlssnr.dll) — r_dlss_nr
// ============================================================================
//  The 310.8 "neural rendering" network, and the third occupant of the ONE slot
//  SR and RR already share — one live NGX feature per frame, because each holds a
//  full tensor set and two resident sets is how this renderer reached an NGX OOM
//  once already. Enabling it RELEASES whichever of the other two was running.
//
//  Its input set is SR's, not RR's: colour + depth + motion vectors, no guides.
//  RR reconstructs the LIGHTING and therefore has to be told albedo/normal/
//  roughness; NR reworks the IMAGE, so it does not — which is also why it does not
//  depend on the ray-traced passes being on.
//
//  ⚠UNRELEASED FEATURE, NO PUBLIC HEADER. There is no NVSDK_NGX_Feature_* constant
//  for it and no *_Create_Params struct, so everything below drives the GENERIC
//  NVSDK_NGX_VULKAN_CreateFeature/EvaluateFeature entry points with the "DLSSNR.*"
//  parameter keys the snippet exports. The feature ID is a console variable
//  (r_dlss_nr_featid) rather than a constant for the same reason: it lives in the
//  reserved range past RayReconstruction (13) and the only way to confirm which
//  slot it occupies is to ask this driver. A refused create LATCHES so a wrong
//  guess costs one attempt rather than one attempt (and one device idle) a frame.
// ============================================================================
bool NeuralRenderAvailable();   // NGX did not refuse the feature on this GPU/driver
bool NeuralRenderEnabled();     // r_dlss && r_dlss_nr && available && the latch is clear

// Same contract as EnsureFeatureRR (dims + input generation → rebuild), without the
// guide generation. Also rebuilds on a preset change, and CLEARS the create-failure
// latch when r_dlss_nr_featid changes so the next ID can be tried from the console.
bool EnsureFeatureNR(VkCommandBuffer cmd, u32 renderW, u32 renderH,
                     u32 displayW, u32 displayH, u32 inputsGen);

// Run Neural Rendering: colour+depth+mv (render res) → OutputView (display res).
// ⚠SAME LAYOUT CONTRACT as Evaluate/EvaluateRR: every input must ALREADY be in
// GENERAL; only the output is transitioned here, and it is left in GENERAL.
// ⚠RETURNS "the output was written" — same contract, same reason, as the other two.
bool EvaluateNR(VkCommandBuffer cmd, const Img& color, const Img& depth, const Img& mv,
                u32 renderW, u32 renderH, bool reset);

}}  // namespace VK::Dlss

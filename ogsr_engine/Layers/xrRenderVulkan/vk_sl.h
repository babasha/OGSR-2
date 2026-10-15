// xrRenderVulkan — NVIDIA Streamline (SL) integration.
//
// Streamline is the framework that owns DLSS Super Resolution, Reflex and DLSS
// Frame Generation. We link sl.interposer.lib INSTEAD of vulkan-1.lib (see
// vulkan_renderer.props) so every vk* call routes through SL, letting it hook
// device creation + present. This module owns slInit (must run BEFORE the first
// Vulkan call) + the capability probe. Per-frame DLSS tagging/eval lives in the
// frame loop. Replaces the raw-NGX VK::Dlss path when SL is available.
#pragma once
#include "vk_core.h"

namespace VK { namespace SL {

// slInit with {DLSS, Reflex}. MUST be called before vkCreateInstance (interposer
// mode). Non-fatal: on failure SL stays off and the raw-NGX path is used instead.
bool Init();
void Shutdown();

bool Inited();

// After the VkPhysicalDevice exists, probe per-feature support (slIsFeatureSupported).
void OnDeviceReady(VkPhysicalDevice phys);
bool SuperResAvailable();   // DLSS SR usable via SL on this GPU
bool ReflexAvailable();     // Reflex usable via SL
bool FrameGenAvailable();   // DLSS-G (frame generation) usable via SL

// ---- Stage B: DLSS Super Resolution via sl.dlss (r_dlss_sl 1) -----------------
// Replaces raw-NGX Dlss::EnsureFeature/Evaluate with SL tags+constants+evaluate.
// Reuses the raw path's whole infra: jitter (Dlss::NewFrameJitter), jitter-free MV,
// render<display split, DlssOutput image, tonemap resolved-binding. FG needs SR to
// live under SL (one NGX owner), so this is the FG prerequisite.
struct SrImg {
    VkImage     img  = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat    fmt  = VK_FORMAT_UNDEFINED;
    u32         w = 0, h = 0;
};
// Record the SL DLSS evaluate into `cmd`. color/depth/mv = render-res inputs in
// SHADER_READ; output = display-res target in GENERAL (fully overwritten).
// curVP/prevVP = UNJITTERED combined view-proj (MotionVec history); jitter in
// render pixels (raw, r_dlss_jitter_flip applied inside, same as the NGX path).
bool EvaluateSR(VkCommandBuffer cmd, const SrImg& color, const SrImg& depth, const SrImg& mv,
                const SrImg& output, const Fmatrix& curVP, const Fmatrix& prevVP,
                float jitterPixX, float jitterPixY, bool reset);
bool SrResolvedThisFrame();   // last EvaluateSR succeeded (tonemap gate, pairs with Dlss::FeatureReady)

// ---- Stage C: DLSS Frame Generation (MFG) via sl.dlss_g (r_dlss_fg) -----------
// DLSS-G presents 1..5 AI-interpolated frames between each rendered one (2x..6x).
// It runs inside the interposer's vkQueuePresentKHR hook + an async pacer, so it
// needs: (a) ONE shared SL frame token per frame (FrameBegin, also used by the SR
// eval), (b) Reflex active — its own hard requirement — driven by slReflexSleep +
// the PCL latency markers below, (c) slDLSSGSetOptions(eOn) with the frame
// multiplier, (d) depth + motion vectors tagged eValidUntilPresent (EvaluateSR
// does this when FG is on). Gated r_dlss_fg; requires r_dlss + r_dlss_sl (SR under
// SL = one NGX owner) + FrameGenAvailable.

// CRender::Begin (post-acquire): acquire this frame's SL token (shared with the SR
// eval), and — when FG is wanted — run Reflex sleep + the simulation-start marker.
// Also flips DLSS-G off the frame the user disables r_dlss_fg.
void FrameBegin();

// True when DLSS-G is configured ON this frame (drives EvaluateSR's tag lifecycle
// + the render/present markers). Cheap; reads the resolved intent set by FrameBegin.
bool FrameGenWanted();

// CRender::End — set DLSS-G options for this frame (mode/multiplier/resolutions).
// numFramesToGenerate = mult-1 (1=2x .. 5=6x), clamped to the driver's reported max.
void ConfigureFG(u32 numFramesToGenerate, u32 renderW, u32 renderH, u32 displayW, u32 displayH);

// PCL latency markers — no-ops unless FG is active this frame. Bracket the CPU
// simulation, the render-submit, and the present so Reflex/DLSS-G can pace frames.
void MarkSimEnd();
void MarkRenderSubmit(bool begin);
void MarkPresent(bool begin);

// Hudless capture for DLSS-G (FG-only). After the tonemap composite the swapchain
// holds the final scene WITHOUT UI (the UI pass runs later). Snapshot it into a
// dedicated hudless buffer and tag it kBufferTypeHUDLessColor so Frame Generation
// keeps the real UI crisp on generated frames (composites it instead of warping it).
// `swapImg` must be in COLOR_ATTACHMENT here; left back in COLOR_ATTACHMENT after.
// No-op unless FG is active this frame. Call right after Tonemap, before the UI pass.
void TagHudless(VkCommandBuffer cmd, VkImage swapImg, u32 w, u32 h, VkFormat fmt);

// r_dlss_fg_debug — query + log DLSS-G state (status / frames actually presented /
// est. VRAM / max multiplier) and Reflex state (low-latency available, latency),
// plus the SL feature versions once. Verifies FG is really generating frames and
// Reflex is active. Call once per frame while FG is on; throttles itself.
void DebugTick();

}}  // namespace VK::SL

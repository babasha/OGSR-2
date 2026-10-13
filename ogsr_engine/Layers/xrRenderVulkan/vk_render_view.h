#pragma once
#include "../../xrCore/render_thread.h"
// RENDER_THREAD_PLAN step 2: the per-frame VIEW SNAPSHOT.
//
// The recording passes used to read the live `Device` globals, which the next frame_move
// rewrites -- the thing that makes a render thread impossible. CRender::Begin copies them
// here once; recording code reads RV().<same field name as Device>.
//   * the jitter (TAA/DLSS/FSR) lives ONLY in mFullTransform / mFullTransform_hud here;
//     mFullTransformUnjit / _hudUnjit are the clean ones. (Device is still jittered too,
//     until the last conversion group removes that.)
//   * frameIndex replaces Device.dwFrame as the "once per frame" key.
// ⚠Only code that runs between Begin and End may use RV(): from frame logic (before
// Begin) it is one frame old.
struct RenderView
{
    u32 frameIndex = 0;
    u32 dwWidth = 0, dwHeight = 0;
    float fTimeDelta = 0.f, fTimeDeltaReal = 0.f, fTimeDeltaRealMS = 0.f, fTimeGlobal = 0.f;
    u32 dwTimeGlobal = 0, dwTimeContinual = 0;
    Fvector vCameraPosition{}, vCameraDirection{}, vCameraTop{}, vCameraRight{};
    Fmatrix mView{}, mProject{};
    Fmatrix mFullTransform{}, mFullTransformUnjit{}, mInvFullTransform{};
    Fmatrix mFullTransform_hud{}, mFullTransform_hudUnjit{}, mFullTransform_hud2{};
    float fFOV = 0.f, fASPECT = 0.f;
    float jitterX = 0.f, jitterY = 0.f;
    // Two-slot pipeline: this frame's sequence number and the renderer's ring slot (seq % FRAMES_IN_FLIGHT), assigned at extract.
    // Every per-frame GPU ring the EXTRACT side writes (bones, particles, rain, UI vertices) is indexed by it; the record side
    // reads CommandManager.GetCurrentFrame(), which Begin_Render sets to the same value.
    u32 frameSeq = 0, ringSlot = 0;
    bool ringReady = true;   // false: the extract-side wait for the ring slot's previous GPU user failed (timeout / device lost) -- do not write its rings

    void Capture();                         // from Device, at the top of CRender::Begin
    void ApplyJitter(float jx, float jy);   // after Capture, once the jitter is known
};

inline RT::PerSlot<RenderView> g_RenderView;   // one per frame slot (two-slot pipeline); RV() picks by role
inline RenderView& RV() { return g_RenderView.cur(); }

// The camera frustum of the frame, per slot (CRender::Calculate fills it on the main thread). RImplementation.ViewBase is the
// same frustum for ENGINE code that runs during extract; the record side must read this copy, since the next Calculate
// rewrites ViewBase while the previous frame is still being recorded.
inline RT::PerSlot<CFrustum> g_ViewFrustum;
inline CFrustum& ViewFrustum() { return g_ViewFrustum.cur(); }

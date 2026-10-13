// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

//---------------------------------------------------------------------------
// Minimal CBackend / R_xforms_vk definitions needed to LINK the shared OGSR
// skeleton sources (CSkeletonX::_Render references these).
//
// The Vulkan renderer rasterizes through the retained-mode queue/pass path
// (vk_render_queue / vk_pass_world), NOT this immediate-mode CBackend. So the
// draw-side methods here are deliberately no-ops for now: skinned models LOAD
// and ANIMATE (CKinematics bone math, blending, motion playback all run), but
// are not yet drawn through this path. Wiring the skinned draw (bone SSBO +
// skinned pipeline + vkCmdDrawIndexed) is a separate, later milestone.
//
// The full immediate-mode backend lives in _parked/vk_R_Backend.cpp; it is not
// compiled (it pulls the deferred/R4-tied state we deliberately keep parked).
// If it is ever un-parked, remove the definitions below to avoid duplicates.
//---------------------------------------------------------------------------
#include "stdafx.h"
#include "vk_R_Backend.h"

// --- R_xforms_vk -----------------------------------------------------------
void R_xforms_vk::set_W(const Fmatrix& m)
{
    m_w = m;
    m_bInvWValid = false;
}

void R_xforms_vk::set_W_prev(const Fmatrix& m)
{
    m_w_prev = m;
    m_w_old = m;  // keep the R4-named alias in sync (read by shared SkeletonX.cpp)
}

// --- CBackend (skinned draw path — no-op until wired to the Vulkan queue) ---
void CBackend::set_Geometry(SGeometry* /*geom*/)
{
    // TODO(skinned-render): bind VB/IB from SGeometry into the command buffer.
}

void CBackend::Render(u32 /*prim*/, u32 /*baseV*/, u32 /*startV*/, u32 /*countV*/, u32 /*startI*/, u32 /*PC*/)
{
    // TODO(skinned-render): emit vkCmdDrawIndexed once the skinned pipeline and
    // bone-matrix SSBO binding exist.
}

void CBackend::get_ConstantDirect(const shared_str& /*n*/, u32 DataSize, void** pVData, void** pGData, void** pPData)
{
    // Hand back a scratch CPU buffer the skeleton fills with bone matrices.
    // Not uploaded to the GPU yet (skinned draw path not wired) — this just keeps
    // CSkeletonX::_Render's fill_array writing to valid memory instead of null.
    static thread_local u8 s_scratch[CBackend::MAX_BONES * sizeof(Fvector4) * 3];
    if (pVData)
        *pVData = (DataSize <= sizeof(s_scratch)) ? static_cast<void*>(s_scratch) : nullptr;
    if (pGData)
        *pGData = nullptr;
    if (pPData)
        *pPData = nullptr;
}

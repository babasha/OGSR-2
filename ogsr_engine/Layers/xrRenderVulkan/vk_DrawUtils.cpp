// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Stub CDUInterface (debug-draw helpers). Engine subsystems
// (debug builds, editor) call these freely, so silently dropping every draw
// is the right behaviour until world rendering lands.

#include "stdafx.h"
#include "vk_core.h"
#include "../../Include/xrRender/DrawUtils.h"

namespace {
class vkDrawUtils final : public CDUInterface
{
public:
    void DrawCross(const Fvector&, float, float, float, float, float, float, u32, BOOL) override {}
    void DrawCross(const Fvector&, float, u32, BOOL) override {}
    void DrawFlag(const Fvector&, float, float, float, float, u32, BOOL) override {}
    void DrawRomboid(const Fvector&, float, u32) override {}
    void DrawJoint(const Fvector&, float, u32) override {}

    void DrawSpotLight(const Fvector&, const Fvector&, float, float, u32) override {}
    void DrawDirectionalLight(const Fvector&, const Fvector&, float, float, u32) override {}
    void DrawPointLight(const Fvector&, float, u32) override {}

    void DrawSound(const Fvector&, float, u32) override {}
    void DrawLineSphere(const Fvector&, float, u32, BOOL) override {}

    void dbgDrawPlacement(const Fvector&, int, u32, LPCSTR, u32) override {}
    void dbgDrawVert(const Fvector&, u32, LPCSTR) override {}
    void dbgDrawEdge(const Fvector&, const Fvector&, u32, LPCSTR) override {}
    void dbgDrawFace(const Fvector&, const Fvector&, const Fvector&, u32, LPCSTR) override {}

    void DrawFace(const Fvector&, const Fvector&, const Fvector&, u32, u32, BOOL, BOOL) override {}
    void DrawLine(const Fvector&, const Fvector&, u32) override {}
    void DrawLink(const Fvector&, const Fvector&, float, u32) override {}
    void DrawFaceNormal(const Fvector&, const Fvector&, const Fvector&, float, u32) override {}
    void DrawFaceNormal(const Fvector*, float, u32) override {}
    void DrawFaceNormal(const Fvector&, const Fvector&, float, u32) override {}
    void DrawSelectionBox(const Fvector&, const Fvector&, u32*) override {}
    void DrawSelectionBoxB(const Fbox&, u32*) override {}
    void DrawIdentSphere(BOOL, BOOL, u32, u32) override {}
    void DrawIdentSpherePart(BOOL, BOOL, u32, u32) override {}
    void DrawIdentCone(BOOL, BOOL, u32, u32) override {}
    void DrawIdentCylinder(BOOL, BOOL, u32, u32) override {}
    void DrawIdentBox(BOOL, BOOL, u32, u32) override {}

    void DrawBox(const Fvector&, const Fvector&, BOOL, BOOL, u32, u32) override {}
    void DrawAABB(const Fvector&, const Fvector&, u32, u32, BOOL, BOOL) override {}
    void DrawAABB(const Fmatrix&, const Fvector&, const Fvector&, u32, u32, BOOL, BOOL) override {}
    void DrawOBB(const Fmatrix&, const Fobb&, u32, u32) override {}
    void DrawSphere(const Fmatrix&, const Fvector&, float, u32, u32, BOOL, BOOL) override {}
    void DrawSphere(const Fmatrix&, const Fsphere&, u32, u32, BOOL, BOOL) override {}
    void DrawCylinder(const Fmatrix&, const Fvector&, const Fvector&, float, float, u32, u32, BOOL, BOOL) override {}
    void DrawCone(const Fmatrix&, const Fvector&, const Fvector&, float, float, u32, u32, BOOL, BOOL) override {}
    void DrawPlane(const Fvector&, const Fvector2&, const Fvector&, u32, u32, BOOL, BOOL, BOOL) override {}
    void DrawPlane(const Fvector&, const Fvector&, const Fvector2&, u32, u32, BOOL, BOOL, BOOL) override {}
    void DrawRectangle(const Fvector&, const Fvector&, const Fvector&, u32, u32, BOOL, BOOL) override {}

    void DrawGrid() override {}
    void DrawPivot(const Fvector&, float) override {}
    void DrawAxis(const Fmatrix&) override {}
    void DrawObjectAxis(const Fmatrix&, float, BOOL) override {}
    void DrawSelectionRect(const Ivector2&, const Ivector2&) override {}

    void DrawIndexedPrimitive(int, u32, const Fvector&, const Fvector*, const u32&,
                              const u32*, const u32&, const u32&, float) override {}

    void OutText(const Fvector&, LPCSTR, u32, u32) override {}

    void OnDeviceDestroy() override {}
};

vkDrawUtils g_DUImpl_VK;
} // namespace

CDUInterface* GetDUImpl_VK() { return &g_DUImpl_VK; }

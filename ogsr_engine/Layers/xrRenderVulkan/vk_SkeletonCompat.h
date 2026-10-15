// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#ifndef vk_SkeletonCompat_H
#define vk_SkeletonCompat_H
#pragma once

// Compatibility layer for Skeleton classes in Vulkan renderer
// This file provides all necessary type mappings and stubs

// STEP 1: Pull in xrD3DDefs.h Vulkan branch (void* typedefs for D3D types)
//         + Vulkan equivalents BEFORE xrRender headers.
#include "vk_d3d_skeleton_compat.h"
#include "vk_Visual.h"
#include "vk_R_Backend.h"

// STEP 3: Include necessary engine headers
#include "../../xrCDB/ISpatial.h"     // For spatial optimization
#include "../xrRender/Shader.h"       // For ref_shader

// STEP 4: Type mappings
// Map dxRender_Visual to vkRender_Visual so that skeleton classes
// (CKinematics, FHierrarhyVisual) use the Vulkan visual hierarchy.
// IMPORTANT: ALL files that include SkeletonCustom.h must use this mapping!
#define dxRender_Visual vkRender_Visual

// monolith convention: RDEVICE = Device. Used in shared SkeletonAnimated /
// SkeletonRigid for fTimeGlobal / dwFrame access.
#ifndef RDEVICE
#define RDEVICE Device
#endif

// STEP 5-6: IRender_Mesh — provided here when FBasicVisual.h is suppressed by
// the `#define FBasicVisualH` trick. SkeletonX.h → FVisual.h needs IRender_Mesh
// as a base class, so we provide a minimal compatible version.
struct IRender_Mesh
{
    ref_geom            rm_geom;
    ID3DVertexBuffer*   p_rm_Vertices = nullptr;
    u32                 vBase = 0;
    u32                 vCount = 0;
    ID3DIndexBuffer*    p_rm_Indices = nullptr;
    u32                 iBase = 0;
    u32                 iCount = 0;
    u32                 dwPrimitives = 0;

    IRender_Mesh()                                         = default;
    virtual ~IRender_Mesh()                                = default;

private:
    IRender_Mesh(const IRender_Mesh&)                      = delete;
    void operator=(const IRender_Mesh&)                    = delete;
};

// STEP 7: Forward declare CRender - defined in rvk.h
class CRender;
extern CRender RImplementation;

// STEP 8: RCache - global backend instance
extern CBackend RCache;

// STEP 9: HW is now provided by ../xrRender/HW.h in stdafx.h
// No need for local stub anymore

// STEP 10: Console variables (used by Skeleton classes)
extern int ps_r1_SoftwareSkinning;
extern float ps_r__WallmarkTTL;

// STEP 11: FVF vertex formats are defined in FVF.h (xrRender)
// Don't redefine them here - let the wrapper files include the originals

// STEP 12: Software skinning stubs (used by SkeletonX.cpp)
// These are NOT used in Vulkan (ps_r1_SoftwareSkinning = 0), but code references them
struct VertexSoftwareStub { void* data; };
extern VertexSoftwareStub* _VertexStream;
extern void* _VS;

#endif // vk_SkeletonCompat_H

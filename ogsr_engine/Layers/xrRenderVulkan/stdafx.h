// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once

#include "../../xr_3da/stdafx.h"

// Render backend identifier (matches RENDER define style of other layers)
#define R_R1   1
#define R_R2   2
#define R_R3   3
#define R_R4   4
#define R_VK   5
#define RENDER R_VK

#define XRRENDER_VULKAN_EXPORTS

// Vertex-declaration / FVF constants as stored in the level and model files.
// Vendored (see the header) so nothing here includes <d3d9types.h> any more —
// shared Layers/xrRender/FVF.h and our vk_d3d_compat.h read them as plain
// integers identifying vertex layouts.
#include "../xrRender/xrVertexDeclTypes.h"
// dxgiformat.h — DXGI_FORMAT enum. Kept as the Windows SDK header because DDS
// files with a DX10 header literally store these numbers, and the NGX/DLSS SDK
// headers pull the same definition in; this is a header-only enum, no DLL or
// import library behind it. Off Windows, a verbatim copy of the same header.
#ifdef _WIN32
#  include <dxgiformat.h>
#else
#  include "../xrRender/dxgiformat_portable.h"
#endif

// Vulkan API + VMA — pulled into the PCH so vk_core.h (copied from monolith,
// it assumes stdafx provides them) and downstream files just `#include "vk_core.h"`.
#if defined(__SWITCH__) && !defined(VK_USE_PLATFORM_VI_NN)
#  define VK_USE_PLATFORM_VI_NN  // vkCreateViSurfaceNN: the Horizon surface (NWindow), see VK_CreatePlatformSurface
#endif
#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#  define VK_USE_PLATFORM_WIN32_KHR  // also set in vulkan_renderer.props
#endif
#include <vulkan/vulkan.h>
// vkCmdBindDescriptorSets -> wrapper that fills the zero offsets of the dynamic-UBO sets
// (Lighting UBO, grass generator params): cbuf in every stage on pre-Turing NVK.
#include "vk_dyn_sets.h"

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION           1003000
#include <vk_mem_alloc.h>

// VRAM attribution wrappers (VK::Vram::*) — in the PCH so every converted
// vmaCreate/Destroy(Buffer|Image) call site sees the declarations.
#include "vk_vram_stats.h"

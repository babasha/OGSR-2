// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — HDR scene colour target.
//
// All scene passes (world/skinned/grass/trees/LODs/sky/shafts/particles) render
// into this floating-point target instead of straight to the UNORM swapchain.
// Bright surfaces can exceed 1.0 (no hard clamp), and a final Tonemap pass
// applies exposure + a Reinhard-with-white operator to map it to the display —
// the R4 pipeline (combine → combine_tonemap). This is what lets shadows lift
// and highlights roll off instead of the flat, clamped LDR-direct look.
//
// One image per swapchain image index (mirrors the swapchain's own image set),
// so the in-flight discipline is identical to the swapchain images.
//
// FORMAT: chosen ONCE per process by r_scene_color_32 (see vk_console_min.cpp),
// B10G11R11_UFLOAT_PACK32 by default and R16G16B16A16_SFLOAT when it is 0 or the
// GPU lacks a needed feature. Ask Format() - never hard-code it: every scene
// pipeline bakes the answer in at build time and they must all agree.
#pragma once
#include "HW_Vulkan.h"

namespace VK { namespace SceneColor {

VkFormat    Format();                          // B10G11R11_UFLOAT_PACK32, or R16G16B16A16_SFLOAT (r_scene_color_32 0 / unsupported)
void        EnsureSize(VkExtent2D extent, u32 count);  // (re)create on size/count change
void        Destroy();
VkImage     GetImage(u32 index);
VkImageView GetView(u32 index);                // mip-0 only — colour attachment for scene passes
VkImageView GetSampleView(u32 index);          // full mip chain — sampled by the tonemap (avg luminance)
u32         MipLevels();                        // mip count (for the avg-luminance top mip = whole-image average)
VkExtent2D  Extent();                           // mip-0 size (the meter derives its readback mip's extent from it)
u32         Count();
u32         Generation();                      // bumped on every (re)create — tonemap rebinds on change

// Downsample mip0 (the rendered scene, in COLOR_ATTACHMENT) into the full mip
// chain via vkCmdBlitImage, leaving ALL mips SHADER_READ. The top mip is the
// whole-frame average luminance (R4 auto-exposure source). Call once per frame
// after the scene passes, before the tonemap samples.
void        GenerateMips(VkCommandBuffer cmd, u32 index);

}}  // namespace VK::SceneColor

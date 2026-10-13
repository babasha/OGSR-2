// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "../../Include/xrRender/UIShader.h"
#include <vulkan/vulkan.h>

/**
 * Extended UI Shader interface for Vulkan
 * Provides access to Vulkan-specific resources (descriptor sets, textures)
 *
 * Video methods exist so vkUISequenceVideoItem can forward CTheoraSurface
 * playback control to whichever shader currently owns the .ogm-backed
 * texture. Default impls are no-ops — only Theora-loaded shaders override.
 */
class IVkUIShader : public IUIShader
{
public:
    virtual VkDescriptorSet GetDescriptorSet() = 0;
    virtual u32 GetTextureWidth() const = 0;
    virtual u32 GetTextureHeight() const = 0;
    // The names the UI asked for (diagnostics: which texture / which blend shader a draw came from).
    virtual LPCSTR GetTextureName() const { return ""; }
    virtual LPCSTR GetShaderName() const  { return ""; }

    virtual bool HasVideo() const                { return false; }
    virtual void VideoPlay(BOOL /*looped*/, u32 /*time*/) {}
    virtual void VideoSync(u32 /*time*/)         {}
    virtual void VideoStop()                     {}
    virtual BOOL VideoIsPlaying()                { return FALSE; }
};

// Set by vkUIRender::SetShader; consumed by vkUISequenceVideoItem::CaptureTexture.
// Mirrors R4's RCache.get_ActiveTexture(0) — the video item asks "which UI
// shader was just bound for me to attach my Theora playback control to?".
extern IVkUIShader* g_pLastVkUIShader;

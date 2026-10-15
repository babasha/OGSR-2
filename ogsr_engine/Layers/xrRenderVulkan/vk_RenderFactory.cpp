// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - IRenderFactory implementation.
//
// CreateRenderDeviceRender hands back a working vkRenderDeviceRender (HW +
// swapchain + sync). Create{UIShader, UISequenceVideoItem, FontRender}
// return real implementations driving the UI render path — these are why
// the menu draws. Stats, WallMarkArray, Environment, Rain, LensFlare,
// Thunderbolt return inert stubs that satisfy the engine's invariant
// "factory always returns a non-null object" until the matching Vulkan
// subsystem ports over. The reference implementation we're grafting from
// is in _parked/vk_RenderFactory.cpp.

#include "stdafx.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_RenderFactory.h"
#include "vk_RenderDeviceRender.h"
#include "vk_ui_shader.h"
#include "vk_UIPipeline.h"
#include "vk_texture.h"
#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "HW_Vulkan.h"

#include "../../Include/xrRender/UIShader.h"
#include "../../Include/xrRender/UISequenceVideoItem.h"
#include "../../Include/xrRender/FontRender.h"
#include "../../Include/xrRender/StatsRender.h"
#include "../../Include/xrRender/WallMarkArray.h"
#include "../../Include/xrRender/UIRender.h"
#include "../../xr_3da/GameFont.h"
#include "../../xr_3da/MbHelpers.h"
#include "../../xr_3da/xrTheora_Surface.h"
#include "../../Include/xrRender/EnvironmentRender.h"
#include "../../Include/xrRender/RainRender.h"
#include "../../Include/xrRender/LensFlareRender.h"
#include "../../Include/xrRender/ThunderboltRender.h"
#include "../../Include/xrRender/ThunderboltDescRender.h"
#include "vk_rain.h"               // VK_Create{Rain,Thunderbolt,ThunderboltDesc,Flare}Render

#include <unordered_map>
#include <string>

vkRenderFactory RenderFactoryImpl_VK;

// ---------------------------------------------------------------------------
// vkUIShader_Real and vkFontRender_Real are at GLOBAL scope (not in the
// anonymous namespace below) because CGameFont declares them as friends to
// reach its protected members — friend lookup is by unqualified name and
// won't match a class defined inside an unnamed namespace.
// ---------------------------------------------------------------------------

// Real vkUIShader — inherits IVkUIShader so vkUIRender's dynamic_cast picks it
// up and feeds the bound texture's descriptor into vkCmdBindDescriptorSets.
//
// ----------------------------------------------------------------------------
// UI texture cache — shared across all vkUIShader_Real instances. Settings
// menu spawns hundreds of UIStatic widgets, each with its own IUIShader; without
// a cache we re-loaded the same DDS/OGM 130+ times (600ms+ stall on menu open,
// validation errors from leaking samplers, and the .ogm video item ended up
// pointing at one Theora playback while a different one rendered).
//
// Slots own the GPU resources for the lifetime of the engine (cleared on
// process exit). Shader instances hold non-owning pointers.
// ----------------------------------------------------------------------------
struct UITextureSlot
{
    VK::CVulkanTexture  texture;
    VkDescriptorSet     descSet  = VK_NULL_HANDLE;
    bool                hasTex   = false;

    // Theora playback state — only populated for .ogm-backed slots.
    CTheoraSurface*     pTheora  = nullptr;
    VK::CVulkanBuffer   videoStaging;
    xr_vector<u32>      yuvScratch;
    u32                 videoRealW = 0, videoRealH = 0;
    u32                 videoPadW  = 0, videoPadH  = 0;
    u32                 videoSyncTime        = 0xFFFFFFFF;  // last VideoSync
    u32                 videoLastUploadFrame = 0;           // dwFrame de-dup
    bool                nxFreezeVideo = false;              // Switch: the MAIN-MENU backdrop only (decoded twice, then a still)
    u32                 nxDecodes     = 0;

    bool hasVideo() const { return pTheora != nullptr; }
};

static std::unordered_map<std::string, UITextureSlot*> g_UITextureCache;
static std::mutex g_UITextureCacheMx;   // RENDER_THREAD_PLAN step 9: guards g_UITextureCache (loadTexture / VK_ClearUITextureCache)

// Teardown: free every cached UI-texture slot's GPU resources before the VMA
// allocator + device are destroyed. Each slot's VkImage / video staging buffer
// is released by ~UITextureSlot (its CVulkanTexture / CVulkanBuffer members'
// dtors call Destroy()); descSet objects are freed implicitly when VulkanUI's
// descriptor pool is destroyed. Without this the whole UI/map/font/HUD texture
// set (and a video staging buffer) leaked all the way to vmaDestroyAllocator →
// VMA "Some allocations were not freed" assert on exit. Called from
// VulkanUI::Destroy() (runs before CVulkanHW::DestroyDevice). Must run while the
// device + allocator are still valid.
void VK_ClearUITextureCache()
{
    std::lock_guard<std::mutex> uiTexLock(g_UITextureCacheMx);
    for (auto& kv : g_UITextureCache)
    {
        UITextureSlot* slot = kv.second;
        if (!slot) continue;
        if (slot->pTheora) xr_delete(slot->pTheora);
        xr_delete(slot);  // ~UITextureSlot → texture.Destroy() + videoStaging.Destroy()
    }
    g_UITextureCache.clear();
}

struct vkUIShader_Real final : IVkUIShader
{
    xr_string      m_TexName;
    xr_string      m_ShName;                 // blend shader the UI asked for (not honoured yet -- diagnostics)
    bool           m_inited = false;
    UITextureSlot* m_Slot   = nullptr;       // non-owning, lives in g_UITextureCache

    void Copy(IUIShader& other) override
    {
        auto* o = static_cast<vkUIShader_Real*>(&other);
        m_TexName = o->m_TexName;
        m_ShName  = o->m_ShName;
        m_inited  = o->m_inited;
        if (!m_TexName.empty()) loadTexture();
    }

    void create(LPCSTR sh, LPCSTR tex, bool /*no_cache*/) override
    {
        m_ShName  = sh ? sh : "";
        m_TexName = tex ? tex : "";
        m_inited  = true;
        if (!m_TexName.empty()) loadTexture();
    }

    bool inited() override { return m_inited; }

    VkDescriptorSet GetDescriptorSet() override
    {
        // Lazy per-bind decode for video-backed slots — render-pass-safe via
        // immediate cmd buffer. De-duped by dwFrame so multiple shaders sharing
        // the same Theora slot (because of texture caching) only tick once.
        if (m_Slot && m_Slot->pTheora) tickVideoSlot(*m_Slot);
        return m_Slot ? m_Slot->descSet : VK_NULL_HANDLE;
    }
    u32 GetTextureWidth()  const override { return (m_Slot && m_Slot->hasTex) ? m_Slot->texture.GetWidth()  : 1; }
    u32 GetTextureHeight() const override { return (m_Slot && m_Slot->hasTex) ? m_Slot->texture.GetHeight() : 1; }
    LPCSTR GetTextureName() const override { return m_TexName.c_str(); }
    LPCSTR GetShaderName() const override  { return m_ShName.c_str(); }

    // ---- Video API forwarded by vkUISequenceVideoItem ------------------------
    bool HasVideo() const override { return m_Slot && m_Slot->pTheora; }

    void VideoPlay(BOOL looped, u32 _time) override
    {
        if (!m_Slot || !m_Slot->pTheora) return;
        u32 t = (_time != 0xFFFFFFFF) ? _time : Device.dwTimeContinual;
        m_Slot->videoSyncTime = _time;
        m_Slot->pTheora->Play(looped, t);
    }
    void VideoSync(u32 _time) override { if (m_Slot) m_Slot->videoSyncTime = _time; }
    void VideoStop()          override { if (m_Slot && m_Slot->pTheora) m_Slot->pTheora->Stop(); }
    BOOL VideoIsPlaying()     override { return (m_Slot && m_Slot->pTheora) ? m_Slot->pTheora->IsPlaying() : FALSE; }

private:
    // Allocate descriptor set + bind it to slot.texture. Called once per slot
    // (right after the texture is created/loaded).
    static bool createSlotDescriptor(UITextureSlot& slot)
    {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = VulkanUI::s_DescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &VulkanUI::s_DescriptorSetLayout;
        if (vkAllocateDescriptorSets(VulkanHW.m_Device, &allocInfo, &slot.descSet) != VK_SUCCESS) {
            slot.descSet = VK_NULL_HANDLE;
            return false;
        }
        VK::DescriptorWriter(slot.descSet)
            .ImageSampler(0, slot.texture.GetView(), slot.texture.GetSampler())
            .Flush();
        return true;
    }

    // Sequence-file fallback: parse FPS-prefixed list and load the first frame
    // as a static DDS. Used only when .dds is missing for the requested name.
    static bool loadFirstSeqFrame(const xr_string& texName, UITextureSlot& slot)
    {
        string_path seqPath;
        if (!FS.exist(seqPath, "$game_textures$", texName.c_str(), ".seq")) return false;
        IReader* r = FS.r_open(seqPath);
        if (!r) return false;

        string1024 line;
        line[0] = 0;
        while (!r->eof()) {
            r->r_string(line, sizeof(line));
            size_t n = strlen(line);
            while (n && (line[n-1] == ' ' || line[n-1] == '\t' || line[n-1] == '\r')) line[--n] = 0;
            if (!line[0])                          continue;
            if (_stricmp(line, "cycled") == 0)     continue;
            bool isnum = true;
            for (char* c = line; *c; ++c) if (*c < '0' || *c > '9') { isnum = false; break; }
            if (isnum)                             continue;
            break;
        }
        FS.r_close(r);
        if (!line[0]) return false;

        string_path framePath;
        if (!FS.exist(framePath, "$game_textures$", line, ".dds")) return false;
        Msg("[VK-UIShader] %s missing .dds — using .seq frame %s", texName.c_str(), line);
        return slot.texture.LoadDDS(framePath, /*applyBCSwizzle*/ false); // see loadTexture
    }

    // .ogm initialiser — same flow as R4's SH_Texture.cpp:181 (Load Theora,
    // size + Play(loop), allocate dynamic RGBA8 texture + persistent staging).
    static bool tryLoadOgmSlot(const xr_string& texName, UITextureSlot& slot)
    {
        string_path fn;
        if (!FS.exist(fn, "$game_textures$", texName.c_str(), ".ogm")) return false;

        slot.pTheora = xr_new<CTheoraSurface>();
        if (!slot.pTheora->Load(fn)) {
            xr_delete(slot.pTheora);
            Msg("![VK-UIShader] failed to open .ogm: %s", fn);
            return false;
        }
        slot.videoRealW = slot.pTheora->Width(true);
        slot.videoRealH = slot.pTheora->Height(true);
        slot.videoPadW  = slot.pTheora->Width(false);
        slot.videoPadH  = slot.pTheora->Height(false);

        BOOL stop_at_end = (strstr(texName.c_str(), "intro\\") ||
                            strstr(texName.c_str(), "outro\\")) ? TRUE : FALSE;
        slot.pTheora->Play(!stop_at_end, Device.dwTimeContinual);
#ifdef __SWITCH__
        // Only the main-menu backdrop (ui_mm_*) is frozen after two frames. The game-start intro (intro_stalker...) and the
        // tutorial videos MUST play: CUISequencer waits for their end, and with them frozen the game stayed paused forever.
        slot.nxFreezeVideo = (strstr(texName.c_str(), "ui_mm_") != nullptr);
#endif

        const VkDeviceSize stagingBytes = VkDeviceSize(slot.videoPadW) * slot.videoRealH * 4;
        slot.videoStaging.Create(stagingBytes,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        slot.videoStaging.Map();
        slot.yuvScratch.assign(VkDeviceSize(slot.videoPadW) * slot.videoRealH, 0);

        slot.texture.Create(slot.videoPadW, slot.videoPadH, VK_FORMAT_R8G8B8A8_UNORM, 1,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        slot.texture.TransitionLayoutImmediate(VK_IMAGE_LAYOUT_UNDEFINED,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        slot.hasTex = true;
        return createSlotDescriptor(slot);
    }

    // Decode latest Theora frame, convert YUV→RGB, push to slot.texture.
    static void tickVideoSlot(UITextureSlot& slot)
    {
        if (slot.videoLastUploadFrame == Device.dwFrame) return;
        slot.videoLastUploadFrame = Device.dwFrame;

        const u32 t = (slot.videoSyncTime != 0xFFFFFFFF) ? slot.videoSyncTime
                                                         : Device.dwTimeContinual;
#ifdef __SWITCH__
        // Decode + YUV->RGB of the 1024x1024 menu video is ~80 ms of Cortex-A57 time per frame, on the main
        // thread: the menu cursor ran at ~5 fps (measured 30-09). Show the first frames as a still picture.
        if (slot.nxFreezeVideo && slot.nxDecodes >= 2) return;
#endif
        if (!slot.pTheora->Update(t)) return;
#ifdef __SWITCH__
        ++slot.nxDecodes;
#endif
        // First few decodes — confirm we got a fresh YUV frame (not just bind).
        static u32 decodes = 0;
        if (++decodes <= 3 || (decodes % 100) == 0)
            Msg("[VK-Theora] decoded frame #%u (t=%u)", decodes, t);

        const u32 padding = slot.videoPadW - slot.videoRealW;
        int pos = 0;
        slot.pTheora->DecompressFrame(slot.yuvScratch.data(), padding, pos);

        // YUV-packed-as-BGRA → BT.601 RGB → RGBA8 (Vulkan format-native).
        u32* dst = static_cast<u32*>(slot.videoStaging.m_Mapped);
        const u32 rowStride = slot.videoPadW;
        for (u32 row = 0; row < slot.videoRealH; ++row) {
            const u32* src = slot.yuvScratch.data() + row * rowStride;
            u32* out       = dst                    + row * rowStride;
            for (u32 col = 0; col < slot.videoRealW; ++col) {
                const u32 yuv = src[col];
                // color_rgba bits: [31:24]=255 [23:16]=Y [15:8]=U [7:0]=V.
                const int V = int( yuv        & 0xFF);
                const int U = int((yuv >> 8 ) & 0xFF);
                const int Y = int((yuv >> 16) & 0xFF);
                const int yC = Y - 16, uC = U - 128, vC = V - 128;
                int R = (298 * yC           + 409 * vC + 128) >> 8;
                int G = (298 * yC - 100 * uC - 208 * vC + 128) >> 8;
                int B = (298 * yC + 516 * uC           + 128) >> 8;
                R = R < 0 ? 0 : (R > 255 ? 255 : R);
                G = G < 0 ? 0 : (G > 255 ? 255 : G);
                B = B < 0 ? 0 : (B > 255 ? 255 : B);
                out[col] = u32(0xFF000000u) | (u32(B) << 16) | (u32(G) << 8) | u32(R);
            }
        }
        slot.videoStaging.Flush();

        VkCommandBuffer cmd = CommandManager.BeginImmediate();
        if (cmd == VK_NULL_HANDLE) return;

        slot.texture.TransitionLayout(cmd,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy region{};
        region.bufferOffset      = 0;
        region.bufferRowLength   = slot.videoPadW;
        region.bufferImageHeight = slot.videoRealH;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {slot.videoRealW, slot.videoRealH, 1};
        vkCmdCopyBufferToImage(cmd, slot.videoStaging.m_Buffer, slot.texture.GetImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        slot.texture.TransitionLayout(cmd,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CommandManager.EndAndSubmitImmediate(cmd);
    }

    void loadTexture()
    {
        if (!VulkanUI::s_DescriptorPool || !VulkanUI::s_DescriptorSetLayout) return;
        std::lock_guard<std::mutex> uiTexLock(g_UITextureCacheMx);

        // Cache hit: reuse the slot. No texture reload, no descriptor realloc.
        const std::string key{m_TexName.c_str()};
        auto it = g_UITextureCache.find(key);
        if (it != g_UITextureCache.end()) {
            m_Slot = it->second;
            return;
        }

        // Cache miss — populate a new slot and insert it.
        auto* slot = xr_new<UITextureSlot>();

        // .ogm beats .dds (R4 ordering — SH_Texture.cpp:181).
        if (!tryLoadOgmSlot(m_TexName, *slot)) {
            // Probe for a static .dds by existence first — R4 resolves
            // .ogm/.avi/.seq/.dds via FS.exist and never blind-opens. Without this
            // guard LoadDDS logs a scary "Failed to open" for every animated /
            // seq-only UI texture (e.g. ui_noise) that legitimately has no static
            // .dds, then loads fine through the .seq fallback right below.
            string_path full;
            bool loaded = false;
            // ⚠NO R<->B swap (06-10). BC blocks are byte-for-byte the same in D3D and
            // Vulkan; the old default swapped every DXT UI texture "because yellow
            // indicators came out blue", and so painted red blue instead: the claw
            // marks of a hit, the face in the inventory portrait. R1-R4 never swapped.
            if (FS.exist(full, "$game_textures$", m_TexName.c_str(), ".dds"))
                loaded = slot->texture.LoadDDS(full, /*applyBCSwizzle*/ false);
            if (!loaded) loaded = loadFirstSeqFrame(m_TexName, *slot);
            if (loaded) {
                slot->hasTex = true;
                createSlotDescriptor(*slot);
            }
        }

        g_UITextureCache.emplace(key, slot);
        m_Slot = slot;
        Msg("[VK-UIShader] tex=%-48s loaded=%d ogm=%d",
            m_TexName.c_str(), slot->hasTex ? 1 : 0, slot->pTheora ? 1 : 0);
    }
};

// Real video item — mirrors dxUISequenceVideoItem (just a thin wrapper). All
// playback work lives in vkUIShader_Real (matches R4 where CTexture owns
// pTheora and apply_theora). CaptureTexture grabs whichever IVkUIShader was
// most recently bound by SetShader, identical in spirit to R4 reading
// RCache.get_ActiveTexture(0) right after the SetShader call.
struct vkUISequenceVideoItem_Real final : IUISequenceVideoItem
{
    IVkUIShader* m_pShader = nullptr;

    void Copy(IUISequenceVideoItem& _in) override
    {
        m_pShader = static_cast<vkUISequenceVideoItem_Real*>(&_in)->m_pShader;
    }
    bool HasTexture()         override { return m_pShader != nullptr; }
    void CaptureTexture()     override { m_pShader = g_pLastVkUIShader; }
    void ResetTexture()       override { m_pShader = nullptr; }
    BOOL video_IsPlaying()    override { return m_pShader ? m_pShader->VideoIsPlaying() : FALSE; }
    void video_Sync(u32 t)    override { if (m_pShader) m_pShader->VideoSync(t); }
    void video_Play(BOOL loop, u32 t) override { if (m_pShader) m_pShader->VideoPlay(loop, t); }
    void video_Stop()         override { if (m_pShader) m_pShader->VideoStop(); }
};

// Real font renderer — owns a vkUIShader_Real for the font atlas texture, then
// emits per-glyph quads through ::UIRender. Field names + scale helpers match
// OGSR's CGameFont (different from monolith's PS.c / smart_strlen).
extern ENGINE_API BOOL     g_bRendering;
extern ENGINE_API Fvector2 g_current_font_scale;

struct vkFontRender_Real final : IFontRender
{
    xr_string         m_ShaderName;
    xr_string         m_TexName;
    vkUIShader_Real   m_Shader;

    void Initialize(LPCSTR cShader, LPCSTR cTexture) override
    {
        m_ShaderName = cShader  ? cShader  : "";
        m_TexName    = cTexture ? cTexture : "";
        Msg("[VK-Font] Initialize: shader=%s tex=%s",
            m_ShaderName.c_str(), m_TexName.c_str());
        m_Shader.create(m_ShaderName.c_str(), m_TexName.c_str(), false);
    }

    void OnRender(CGameFont& owner) override
    {
        VERIFY(g_bRendering);
        if (owner.strings.empty()) return;

        if (m_Shader.inited()) UIRender->SetShader(m_Shader);

        if (!(owner.uFlags & CGameFont::fsValid))
        {
            Fvector2 texSize;
            UIRender->GetActiveTextureResolution(texSize);
            owner.vTS.set((int)texSize.x, (int)texSize.y);
            owner.fTCHeight = owner.fHeight / float(owner.vTS.y);
            owner.uFlags |= CGameFont::fsValid;
        }

        for (u32 i = 0; i < owner.strings.size(); )
        {
            int count  = 1;
            int length = owner.SmartLength(owner.strings[i].string);
            while (i + count < owner.strings.size())
            {
                int L = owner.SmartLength(owner.strings[i + count].string);
                if (L + length < MAX_MB_CHARS) { ++count; length += L; }
                else break;
            }

            UIRender->StartPrimitive(length * 4, IUIRender::ptTriList, IUIRender::pttTL);

            const u32 last = i + count;
            for (; i < last; ++i)
            {
                const CGameFont::String& PS = owner.strings[i];
                wide_char wsStr[MAX_MB_CHARS];
                u32 len = owner.IsMultibyte()
                            ? mbhMulti2Wide(wsStr, nullptr, MAX_MB_CHARS, PS.string)
                            : xr_strlen(PS.string);
                if (!len) continue;

                float X     = float(iFloor(PS.x));
                float Y     = float(iFloor(PS.y));
                float S     = PS.height * g_current_font_scale.y * owner.GetHeightScale();
                float Y2    = Y + S;
                float fSize = 0;

                if (PS.align)
                    fSize = owner.IsMultibyte() ? owner.SizeOf_(wsStr) : owner.SizeOf_(PS.string);
                if (PS.align == CGameFont::alCenter)
                    X -= iFloor(fSize * 0.5f) * g_current_font_scale.x;
                else if (PS.align == CGameFont::alRight)
                    X -= iFloor(fSize) * g_current_font_scale.x;

                u32 clr  = PS.color;
                u32 clr2 = clr;
                if (owner.uFlags & CGameFont::fsGradient) {
                    clr2 = color_rgba(color_get_R(clr) / 2,
                                      color_get_G(clr) / 2,
                                      color_get_B(clr) / 2,
                                      color_get_A(clr));
                }

                for (u32 j = 0; j < len; ++j)
                {
                    Fvector l = owner.IsMultibyte()
                                  ? owner.GetCharTC(wsStr[1 + j])
                                  : owner.GetCharTC((u16)(u8)PS.string[j]);
                    float scw      = l.z * g_current_font_scale.x * owner.GetWidthScale();
                    float fTCWidth = l.z / owner.vTS.x;

                    if (!fis_zero(l.z))
                    {
                        float tu = l.x / owner.vTS.x;
                        float tv = l.y / owner.vTS.y;
                        UIRender->PushPoint(X,       Y2, 0, clr2, tu,            tv + owner.fTCHeight);
                        UIRender->PushPoint(X,       Y,  0, clr,  tu,            tv);
                        UIRender->PushPoint(X + scw, Y2, 0, clr2, tu + fTCWidth, tv + owner.fTCHeight);

                        UIRender->PushPoint(X,       Y,  0, clr,  tu,            tv);
                        UIRender->PushPoint(X + scw, Y2, 0, clr2, tu + fTCWidth, tv + owner.fTCHeight);
                        UIRender->PushPoint(X + scw, Y,  0, clr,  tu + fTCWidth, tv);
                    }

                    X += scw * owner.vInterval.x;

                    // Multibyte (Cyrillic) fonts encode the space as a zero/near-zero
                    // width glyph; the actual gap comes from fXStep. Without this the
                    // DX path applies, words run together — "шрифты без пробелов".
                    if (owner.IsMultibyte() && IsSpaceCharacter(wsStr[1 + j]))
                        X += owner.GetfXStep() * owner.vInterval.x * owner.GetWidthScale();
                }
            }

            UIRender->FlushPrimitive();
        }
    }
};

// Below: pure stubs that don't need friend access — keep them in anon ns.
namespace {

struct vkStatsRender_Stub final : IStatsRender
{
    void Copy(IStatsRender&)                         override {}
    void OutData(CGameFont&)                         override {}
    void GuardVerts(CGameFont&)                      override {}
    void GuardDrawCalls(CGameFont&)                  override {}
    void SetDrawParams(IRenderDeviceRender*)         override {}
};

// Real wallmark array: retains the decal TEXTURE NAMES the game appends from
// materials (SGameMtlPair CollideMarks). empty() must be honest — the bullet
// manager gates add_StaticWallmark on it (the old always-true stub meant the
// renderer never even got asked for bullet holes).
struct vkWallMarkArray_Real final : IWallMarkArray
{
    xr_vector<shared_str> m_names;

    void Copy(IWallMarkArray& in)  override { m_names = static_cast<vkWallMarkArray_Real&>(in).m_names; }
    void AppendMark(LPCSTR s)      override { if (s && s[0]) m_names.emplace_back(s); }
    void clear()                   override { m_names.clear(); }
    bool empty()                   override { return m_names.empty(); }
    wm_shader GenerateWallmark()   override
    {
        // wm_shader = FactoryPtr<IUIShader>; the texture name travels inside
        // the vkUIShader_Real (read back via VK_UIShaderTexName).
        wm_shader S;
        if (!m_names.empty())
            S->create("effects\\wallmark", m_names[::Random.randI((u32)m_names.size())].c_str());
        return S;
    }

    const char* PickName()
    {
        return m_names.empty() ? nullptr : m_names[::Random.randI((u32)m_names.size())].c_str();
    }
};

// (The external bridge functions for these types live AFTER the anonymous
// namespace closes — see VK_WallmarkArray_Pick / VK_UIShaderTexName below.)

// CEnvironment / CEnvDescriptor / WeatherFX subsystem ------------------------
// CGamePersistent ctor → IGame_Persistent ctor → CEnvironment ctor →
// m_pRender->OnDeviceCreate(). Without these stubs we crash *before* the menu
// even starts loading.

struct vkEnvironmentRender_Stub final : IEnvironmentRender
{
    void Copy(IEnvironmentRender&)               override {}
    void OnFrame(CEnvironment&)                  override {}
    void RenderSky(CBackend&, CEnvironment&)     override {}
    void RenderClouds(CBackend&, CEnvironment&)  override {}
    void OnDeviceCreate()                        override {}
    void OnDeviceDestroy()                       override {}
};

struct vkEnvDescriptorRender_Stub final : IEnvDescriptorRender
{
    void Copy(IEnvDescriptorRender&)             override {}
    void OnDeviceCreate(CEnvDescriptor&)         override {}
    void OnDeviceDestroy()                       override {}
    void OnPrepare(CEnvDescriptor&)              override {}
    void OnUnload(CEnvDescriptor&)               override {}
};

struct vkEnvDescriptorMixerRender_Stub final : IEnvDescriptorMixerRender
{
    void Copy(IEnvDescriptorMixerRender&)                  override {}
    void Destroy()                                          override {}
    void Clear()                                            override {}
    void lerp(IEnvDescriptorRender*, IEnvDescriptorRender*) override {}
};

// Rain / thunderbolt / flare renders are REAL now — see vk_rain.cpp.

struct vkLensFlareRender_Stub final : ILensFlareRender
{
    void Copy(ILensFlareRender&)                                  override {}
    void Render(CBackend&, CLensFlare&, BOOL, BOOL, BOOL)         override {}
    void OnDeviceCreate()                                          override {}
    void OnDeviceDestroy()                                         override {}
};

} // namespace

// Bridges for CRender::add_StaticWallmark (CRender_Vulkan.cpp can't see the
// concrete types defined in this TU). EXTERNAL linkage — outside the anonymous
// namespace above.
const char* VK_WallmarkArray_Pick(IWallMarkArray* A)
{
    return A ? static_cast<vkWallMarkArray_Real*>(A)->PickName() : nullptr;
}
const char* VK_UIShaderTexName(IUIShader* S)
{
    if (!S) return nullptr;
    auto* r = static_cast<vkUIShader_Real*>(S);
    return r->m_TexName.empty() ? nullptr : r->m_TexName.c_str();
}

// ---------------------------------------------------------------------------
// IRenderFactory implementations — only the menu-path ones return real stubs.
// ---------------------------------------------------------------------------

IUIShader* vkRenderFactory::CreateUIShader()                       { return xr_new<vkUIShader_Real>(); }
void       vkRenderFactory::DestroyUIShader(IUIShader* p)          { xr_delete(p); }

IUISequenceVideoItem* vkRenderFactory::CreateUISequenceVideoItem() { return xr_new<vkUISequenceVideoItem_Real>(); }
void                  vkRenderFactory::DestroyUISequenceVideoItem(IUISequenceVideoItem* p) { xr_delete(p); }

IFontRender* vkRenderFactory::CreateFontRender()                   { return xr_new<vkFontRender_Real>(); }
void         vkRenderFactory::DestroyFontRender(IFontRender* p)    { xr_delete(p); }

IStatsRender* vkRenderFactory::CreateStatsRender()                 { return xr_new<vkStatsRender_Stub>(); }
void          vkRenderFactory::DestroyStatsRender(IStatsRender* p) { xr_delete(p); }

IWallMarkArray* vkRenderFactory::CreateWallMarkArray()             { return xr_new<vkWallMarkArray_Real>(); }
void            vkRenderFactory::DestroyWallMarkArray(IWallMarkArray* p) { xr_delete(p); }

// xr_new keeps engine-side ownership semantics consistent with dxRenderFactory.
IRenderDeviceRender* vkRenderFactory::CreateRenderDeviceRender()
{
    return xr_new<vkRenderDeviceRender>();
}

void vkRenderFactory::DestroyRenderDeviceRender(IRenderDeviceRender* p)
{
    xr_delete(p);
}

// Environment / weather / effects ----------------------------------------------
IEnvironmentRender* vkRenderFactory::CreateEnvironmentRender()                   { return xr_new<vkEnvironmentRender_Stub>(); }
void                vkRenderFactory::DestroyEnvironmentRender(IEnvironmentRender* p) { xr_delete(p); }

IEnvDescriptorRender* vkRenderFactory::CreateEnvDescriptorRender()               { return xr_new<vkEnvDescriptorRender_Stub>(); }
void                  vkRenderFactory::DestroyEnvDescriptorRender(IEnvDescriptorRender* p) { xr_delete(p); }

IEnvDescriptorMixerRender* vkRenderFactory::CreateEnvDescriptorMixerRender()     { return xr_new<vkEnvDescriptorMixerRender_Stub>(); }
void                       vkRenderFactory::DestroyEnvDescriptorMixerRender(IEnvDescriptorMixerRender* p) { xr_delete(p); }

// Rain / thunderbolt / flare: REAL implementations live in vk_rain.cpp (the
// "Rain" pass draws what these build). Lens flare itself is still stubbed.
IRainRender*  vkRenderFactory::CreateRainRender()                                { return VK_CreateRainRender(); }
void          vkRenderFactory::DestroyRainRender(IRainRender* p)                 { xr_delete(p); }

ILensFlareRender* vkRenderFactory::CreateLensFlareRender()                       { return xr_new<vkLensFlareRender_Stub>(); }
void              vkRenderFactory::DestroyLensFlareRender(ILensFlareRender* p)   { xr_delete(p); }

IFlareRender* vkRenderFactory::CreateFlareRender()                               { return VK_CreateFlareRender(); }
void          vkRenderFactory::DestroyFlareRender(IFlareRender* p)               { xr_delete(p); }

IThunderboltRender* vkRenderFactory::CreateThunderboltRender()                   { return VK_CreateThunderboltRender(); }
void                vkRenderFactory::DestroyThunderboltRender(IThunderboltRender* p) { xr_delete(p); }

IThunderboltDescRender* vkRenderFactory::CreateThunderboltDescRender()           { return VK_CreateThunderboltDescRender(); }
void                    vkRenderFactory::DestroyThunderboltDescRender(IThunderboltDescRender* p) { xr_delete(p); }

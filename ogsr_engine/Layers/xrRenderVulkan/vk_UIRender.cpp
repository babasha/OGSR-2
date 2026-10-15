// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - vkUIRender: real IUIRender implementation.
//
// Adapted from the monolith UI render path. Differences worth knowing about:
// nothing here records into a command buffer -- vertices go into the frame's
// ring slot and every draw/scissor into VulkanUI's deferred queue, replayed by
// CRender::End (VulkanUI::ReplayDeferredUI); the RCache.stat.{calls,verts}
// counters are dropped, and we go through IVkUIShader (vk_ui_shader.h) for
// descriptor-set lookup.
//
// Vertex format: FVF::TL = vec4 pos + u32 color + vec2 uv = 28 bytes.
// pttTL = 4-component pos + colour + uv (UI default).
// pttLIT — supported too in case the engine asks for lit primitives.

#include "stdafx.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_core.h"
#include "vk_UIPipeline.h"
#include "vk_ui_shader.h"
#include "vk_swapchain.h"
#include "../../Include/xrRender/UIRender.h"
#include "../xrRender/FVF.h"   // FVF::TL, FVF::LIT
#include "vk_texture.h"        // CVulkanTexture::CreateFromData -- dynamic textures
#include "HW_Vulkan.h"         // VulkanHW.m_Device
#include <set>

// Tracks the last UI shader passed to SetShader so vkUISequenceVideoItem
// can grab it inside CaptureTexture (R4 equivalent: RCache.get_ActiveTexture(0)).
IVkUIShader* g_pLastVkUIShader = nullptr;
int g_vk_ui_dump = 0;   // vk_ui_dump: log every UI draw of one frame

namespace {

// vk_ui_dump: true for exactly one frame after the command (the next one, so it is a whole frame), then clears it.
bool UIDumpThisFrame()
{
    static u32 s_dumpFrame = 0;
    if (g_vk_ui_dump && !s_dumpFrame) s_dumpFrame = Device.dwFrame + 1;
    if (s_dumpFrame && Device.dwFrame > s_dumpFrame) { s_dumpFrame = 0; g_vk_ui_dump = 0; }
    return s_dumpFrame && Device.dwFrame == s_dumpFrame;
}

// A texture the caller handed us as pixels. See the note on DynTextureCreate in
// Include/xrRender/UIRender.h: an atlas a font engine just rasterised has no
// file behind it, so the name-based shader path cannot carry it.
struct VkDynTexture
{
    VK::CVulkanTexture tex;
    VkDescriptorSet    set = VK_NULL_HANDLE;
};

class vkUIRender final : public IUIRender
{
    ePrimitiveType m_PrimitiveType = ptNone;
    ePointType     m_PointType     = pttNone;
    u32            m_MaxVerts      = 0;
    u32            m_VertCount     = 0;
    IUIShader*     m_pCurrentShader = nullptr;
    VkDescriptorSet m_CurrentTextureSet = VK_NULL_HANDLE;

    FVF::TL*  m_pWriteTL      = nullptr;
    FVF::TL*  m_pWriteTLStart = nullptr;
    FVF::LIT* m_pWriteLIT     = nullptr;
    FVF::LIT* m_pWriteLITStart= nullptr;

public:
    void CreateUIGeom() override  { /* VulkanUI::Create() done at engine init */ }
    void DestroyUIGeom() override { /* VulkanUI::Destroy() at engine teardown */ }

    void* DynTextureCreate(const void* rgba, u32 w, u32 h) override
    {
        if (!rgba || !w || !h)
            return nullptr;

        auto* dt = xr_new<VkDynTexture>();
        // ⚠⚠The last argument is the TOTAL SIZE IN BYTES, not bytes per pixel.
        // It was 4, so exactly one pixel was ever uploaded and the rest of the
        // image kept whatever VRAM happened to hold -- a font atlas came out as
        // multicoloured noise that reads as broken hardware. The 1x1 white
        // texture below survived only because 1*1*4 IS 4.
        dt->tex.CreateFromData(rgba, w, h, VK_FORMAT_R8G8B8A8_UNORM, VkDeviceSize(w) * VkDeviceSize(h) * 4);
        if (dt->tex.GetView() == VK_NULL_HANDLE)
        {
            xr_delete(dt);
            Msg("![VK-UI] DynTextureCreate: %ux%u upload failed", w, h);
            return nullptr;
        }

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = VulkanUI::s_DescriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &VulkanUI::s_DescriptorSetLayout;
        if (vkAllocateDescriptorSets(VulkanHW.m_Device, &ai, &dt->set) != VK_SUCCESS)
        {
            // The pool is a fixed 256 sets shared with every menu texture. Worth
            // saying out loud rather than drawing an untextured rectangle and
            // leaving somebody to wonder why the font went blank.
            Msg("![VK-UI] DynTextureCreate: descriptor pool exhausted (256 sets)");
            xr_delete(dt);
            return nullptr;
        }

        VK::DescriptorWriter(dt->set)
            .ImageSampler(0, dt->tex.GetView(), dt->tex.GetSampler())
            .Flush();

        return dt;
    }

    void DynTextureDestroy(void* handle) override
    {
        auto* dt = static_cast<VkDynTexture*>(handle);
        if (!dt)
            return;
        // ⚠The GPU may still be reading this in a frame that has not retired.
        // The UI has no per-resource fence, so wait the device out -- destruction
        // happens at document teardown, not per frame, so the stall is not on any
        // hot path.
        // ⚠Join the render thread FIRST: it may be inside vkQueueSubmit/Present
        // for the previous frame, and vkDeviceWaitIdle requires every queue of the
        // device to be externally synchronised.
        RT_WaitIdle();
        VK_DeviceWaitIdleLocked();
        if (dt->set != VK_NULL_HANDLE)
            vkFreeDescriptorSets(VulkanHW.m_Device, VulkanUI::s_DescriptorPool, 1, &dt->set);
        dt->tex.Destroy();
        xr_delete(dt);
    }

    void SetDynTexture(void* handle) override
    {
        m_pCurrentShader = nullptr;
        g_pLastVkUIShader = nullptr;
        auto* dt = static_cast<VkDynTexture*>(handle);
        m_CurrentTextureSet = dt ? dt->set : VulkanUI::s_WhiteTextureSet;
    }

    void SetShader(IUIShader& shader) override
    {
        m_pCurrentShader = &shader;
        IVkUIShader* vkShader = dynamic_cast<IVkUIShader*>(&shader);
        g_pLastVkUIShader = vkShader;
        if (vkShader) {
            m_CurrentTextureSet = vkShader->GetDescriptorSet();
            // No texture NAME at all (hud\crosshair): vertex colour only, as in R4 -- pure white.
            const LPCSTR texName = vkShader->GetTextureName();
            if (m_CurrentTextureSet == VK_NULL_HANDLE && (!texName || !texName[0]) && VulkanUI::s_PlainTextureSet != VK_NULL_HANDLE) {
                m_CurrentTextureSet = VulkanUI::s_PlainTextureSet;
            } else if (m_CurrentTextureSet == VK_NULL_HANDLE) {
                m_CurrentTextureSet = VulkanUI::s_WhiteTextureSet;
                static std::set<const void*> warned;
                if (warned.insert(vkShader).second)
                    Msg("![VK-UI] SetShader: descriptor=NULL, using white fallback (texW=%u texH=%u)",
                        vkShader->GetTextureWidth(), vkShader->GetTextureHeight());
            }
        } else {
            m_CurrentTextureSet = VulkanUI::s_WhiteTextureSet;
        }
    }

    void SetAlphaRef(int) override { /* alpha handled by blend state */ }

    // ⭐Every UI command is QUEUED and replayed by VulkanUI::ReplayDeferredUI, which opens the pass first --
    // on the render thread for gameplay frames, on the main thread for menu/load frames. There was an
    // immediate path too (record straight into the frame buffer once the scene was done); it went with the
    // render-thread switch on 03-10. Its last bug is worth knowing: the pass opened only at the first DRAW, so
    // a scissor set before any draw was dropped -- the minimap's screen-sized 'ui_nomap2' backing (50% alpha,
    // clipped to the map) went out full-screen whenever the crosshair was hidden: "the image darkens when I
    // sprint". The queue cannot do that: a scissor is replayed inside the pass, in order.
    void SetScissor(Irect* rect) override
    {
        if (UIDumpThisFrame()) {
            if (rect) Msg("[VK-UI dump] f%u scissor %d,%d..%d,%d", Device.dwFrame, rect->x1, rect->y1, rect->x2, rect->y2);
            else      Msg("[VK-UI dump] f%u scissor reset", Device.dwFrame);
        }
        VulkanUI::UISlot& q = VulkanUI::UI();   // main thread: the slot of the frame being extracted
        if (q.count >= VulkanUI::MAX_DEFERRED_CMDS) { ++VulkanUI::s_FrameStats.droppedCmds; return; }
        auto& dcmd = q.cmds[q.count++];
        if (rect) {
            dcmd.type = VulkanUI::DeferredUICmd::Scissor;
            dcmd.scissorRect.offset.x      = rect->x1;
            dcmd.scissorRect.offset.y      = rect->y1;
            dcmd.scissorRect.extent.width  = rect->x2 - rect->x1;
            dcmd.scissorRect.extent.height = rect->y2 - rect->y1;
        } else {
            dcmd.type = VulkanUI::DeferredUICmd::ResetScissor;
        }
    }

    void GetActiveTextureResolution(Fvector2& res) override
    {
        if (m_pCurrentShader) {
            if (auto* vkShader = dynamic_cast<IVkUIShader*>(m_pCurrentShader)) {
                res.set(float(vkShader->GetTextureWidth()), float(vkShader->GetTextureHeight()));
                return;
            }
        }
        res.set(1024.0f, 1024.0f);
    }

    void StartPrimitive(u32 maxVerts, ePrimitiveType primType, ePointType pointType) override
    {
        m_PrimitiveType = primType;
        m_PointType     = pointType;
        m_MaxVerts      = maxVerts;
        m_VertCount     = 0;

        if (!VulkanUI::s_pMappedVB || VulkanUI::UI().dead) { m_pWriteTL = nullptr; m_pWriteLIT = nullptr; return; }

        const u32 vertexSize    = (pointType == pttTL) ? sizeof(FVF::TL) : sizeof(FVF::LIT);
        const u32 requiredBytes = maxVerts * vertexSize;

        VulkanUI::UISlot& q = VulkanUI::UI();
        if (q.vertexOffset + requiredBytes > VulkanUI::VERTEX_BUFFER_SIZE) {
            // CORRUPTING wrap — the GPU hasn't yet consumed pending draws,
            // resetting offset overwrites their vertex data. Bumping
            // VERTEX_BUFFER_SIZE is the right fix; log so we know if we hit it.
            static u32 lastWarnFrame = 0;
            if (Device.dwFrame > lastWarnFrame + 60) {
                Msg("![VK-UI] vertex buffer wrap mid-frame (size=%u, need=%u) — UI corruption likely",
                    (u32)VulkanUI::VERTEX_BUFFER_SIZE, requiredBytes);
                lastWarnFrame = Device.dwFrame;
            }
            q.vertexOffset = 0;
            ++VulkanUI::s_FrameStats.bufferWraps;
        }

        // Writes land in THIS frame-slot's ring region (s_VBBase) — see
        // VulkanUI::OnFrameBegin for the frames-in-flight discipline.
        u8* base = static_cast<u8*>(VulkanUI::s_pMappedVB) + q.vbBase + q.vertexOffset;
        if (pointType == pttTL) {
            m_pWriteTL      = reinterpret_cast<FVF::TL*>(base);
            m_pWriteTLStart = m_pWriteTL;
        } else {
            m_pWriteLIT      = reinterpret_cast<FVF::LIT*>(base);
            m_pWriteLITStart = m_pWriteLIT;
        }
    }

    void PushPoint(float x, float y, float z, u32 C, float u, float v) override
    {
        if (m_PointType == pttTL) {
            if (m_pWriteTL) { m_pWriteTL->set(x, y, z, 1.0f, C, u, v); ++m_pWriteTL; ++m_VertCount; }
        } else if (m_PointType == pttLIT) {
            if (m_pWriteLIT) { m_pWriteLIT->set(x, y, z, C, u, v); ++m_pWriteLIT; ++m_VertCount; }
        }
    }

    void FlushPrimitive() override
    {
        const u32 vertCount = m_VertCount;
        if (vertCount == 0) { m_pWriteTL = nullptr; m_pWriteLIT = nullptr; return; }

        // vk_ui_dump 1: every UI draw of the next frame, in order, with its vertex bounds (the scissor, not the
        // bounds, is what clips it -- SetScissor dumps too). How the sprint veil was found: diff a frame against one.
        if (UIDumpThisFrame() && m_PointType == pttTL && m_pWriteTLStart) {
            float x0 = flt_max, y0 = flt_max, x1 = -flt_max, y1 = -flt_max;
            for (u32 i = 0; i < vertCount; ++i) {
                const Fvector4& p = m_pWriteTLStart[i].p;
                x0 = _min(x0, p.x); y0 = _min(y0, p.y); x1 = _max(x1, p.x); y1 = _max(y1, p.y);
            }
            auto* vs = dynamic_cast<IVkUIShader*>(m_pCurrentShader);
            Msg("[VK-UI dump] f%u draw prim %d verts %u rect %.0f,%.0f..%.0f,%.0f col %08x tex '%s' sh '%s'", Device.dwFrame, int(m_PrimitiveType), vertCount,
                x0, y0, x1, y1, m_pWriteTLStart[0].color, vs ? vs->GetTextureName() : "<dyn>", vs ? vs->GetShaderName() : "");
        }

        const u32 vertexSize = (m_PointType == pttTL) ? sizeof(FVF::TL) : sizeof(FVF::LIT);

        // Select the pipeline matching the requested primitive topology. R4 maps
        // ptLineList/ptLineStrip to D3D line topology; without this the crosshair's
        // line vertices assemble into stray stretched triangles (the "sky spike").
        VkPipeline pipe = VulkanUI::s_Pipeline;
        if (m_PrimitiveType == IUIRender::ptLineList  && VulkanUI::s_PipelineLineList)  pipe = VulkanUI::s_PipelineLineList;
        else if (m_PrimitiveType == IUIRender::ptLineStrip && VulkanUI::s_PipelineLineStrip) pipe = VulkanUI::s_PipelineLineStrip;

        // Queued like everything else (see SetScissor): the vertices are already in this frame's ring slot.
        VulkanUI::UISlot& q = VulkanUI::UI();
        if (q.count < VulkanUI::MAX_DEFERRED_CMDS) {
            auto& dcmd = q.cmds[q.count++];
            dcmd.type               = VulkanUI::DeferredUICmd::Draw;
            dcmd.vertexBufferOffset = q.vbBase + q.vertexOffset;  // absolute (ring slot)
            dcmd.vertexCount        = vertCount;
            dcmd.textureSet         = m_CurrentTextureSet;
            dcmd.pipeline           = pipe;
        } else { ++VulkanUI::s_FrameStats.droppedCmds; }
        VulkanUI::s_FrameStats.totalVerts += vertCount;
        q.vertexOffset += vertCount * vertexSize;
        m_pWriteTL = nullptr; m_pWriteLIT = nullptr; m_VertCount = 0;
    }

    LPCSTR UpdateShaderName(LPCSTR /*tex_name*/, LPCSTR sh_name) override { return sh_name; }

    void CacheSetXformWorld(const Fmatrix&) override {}
    void CacheSetCullMode(CullMode)         override {}
};

vkUIRender g_UIRenderImpl_VK;
} // namespace

IUIRender* GetUIRenderImpl_VK() { return &g_UIRenderImpl_VK; }

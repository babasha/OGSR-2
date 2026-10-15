// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - VulkanUI subsystem implementation. See header for adaptation
// notes vs the monolith original.

#include "stdafx.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_rendering.h"     // VK::RenderingBuilder
namespace VK { VkExtent2D OutputExtent(); VkRect2D ScaleRectToOutput(VkRect2D r); }   // nfs28 CRender_Vulkan.cpp
#include "vk_UIPipeline.h"
#include "vk_render_view.h"        // RV(): the frame snapshot (screen size for the replay push)
#include "vk_buffer.h"
#include "vk_texture.h"
#include "vk_shaders.h"
#include "vk_swapchain.h"
#include "vk_gfx_pipeline.h"       // VK::GfxPipelineBuilder
#include "vk_barriers.h"           // VK::SceneAttachmentBarrier — inter-pass ordering
#include "vk_framegraph.h"         // VK::g_FrameGraph — swapchain image layout (see BeginUIPassInternal)
#include "vk_command_buffer.h"     // CVulkanCommandManager::FRAMES_IN_FLIGHT — VB ring slots
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"   // Device.dwWidth / dwHeight

VkCommandBuffer g_VkUI_FrameCmd = VK_NULL_HANDLE;

// Defined in vk_RenderFactory.cpp (global scope) — frees the engine-lifetime UI
// texture cache. Declared here so VulkanUI::Destroy can call it at teardown.
void VK_ClearUITextureCache();

namespace VulkanUI
{
    // Resources --------------------------------------------------------------
    static VK::CVulkanBuffer  s_VertexBuffer;
    static VK::CVulkanTexture s_WhiteTexture;
    static VK::CVulkanTexture s_PlainTexture;
    VkDescriptorSetLayout    s_DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool         s_DescriptorPool      = VK_NULL_HANDLE;
    VkPipelineLayout         s_PipelineLayout      = VK_NULL_HANDLE;
    VkPipeline               s_Pipeline            = VK_NULL_HANDLE;   // TRIANGLE_LIST (default UI quads)
    VkPipeline               s_PipelineLineList    = VK_NULL_HANDLE;   // LINE_LIST  (crosshair)
    VkPipeline               s_PipelineLineStrip   = VK_NULL_HANDLE;   // LINE_STRIP (UIWindow borders)
    VkDescriptorSet          s_WhiteTextureSet     = VK_NULL_HANDLE;
    VkDescriptorSet          s_PlainTextureSet     = VK_NULL_HANDLE;

    // Frame state ------------------------------------------------------------
    bool   s_bUIPassActive  = false;
    // ⚠Which buffer the open pass belongs to. vkCmdEndRendering on any OTHER one
    // is undefined behaviour, and this file has two ways to end up there: an
    // abandoned frame (see OnFrameBegin) and the async frame-split, which swaps
    // g_VkUI_FrameCmd to the second segment mid-frame (CRender_Vulkan.cpp).
    VkCommandBuffer s_UIPassCmd = VK_NULL_HANDLE;
    void*  s_pMappedVB      = nullptr;

    // Deferred command queue + vertex ring cursor: one per frame slot (see UISlot).
    RT::PerSlot<UISlot> g_UISlots;

    FrameStats s_FrameStats;

    // Swapchain layout is managed centrally now (single-layout convention): the
    // image is in COLOR_ATTACHMENT for the whole frame, so the UI pass does no
    // layout transition — only an inter-pass ordering barrier on entry.

    // ----- Create / Destroy -------------------------------------------------

    void Create()
    {
        Msg("[Vulkan UI] Creating UI infrastructure...");

        // 1. Vertex buffer (persistent-mapped) — a frame-fenced RING: one
        // VERTEX_BUFFER_SIZE region per in-flight frame. Each slot is written
        // only after CRender::Begin waited that slot's fence (OnFrameBegin),
        // so the CPU never overwrites vertex data the GPU still reads.
        s_VertexBuffer.Create(VERTEX_BUFFER_SIZE * CVulkanCommandManager::FRAMES_IN_FLIGHT,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        s_pMappedVB = s_VertexBuffer.Map();
        if (!s_pMappedVB) { Msg("![Vulkan UI] Failed to map vertex buffer"); return; }

        // 2. Descriptor set layout.
        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding         = 0;
        samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &samplerBinding;
        VK_CHECK(vkCreateDescriptorSetLayout(VulkanHW.m_Device, &layoutInfo, nullptr, &s_DescriptorSetLayout));

        // 3. Descriptor pool — 256 sets covers menu fonts + textures.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 256;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // FREE_DESCRIPTOR_SET so individual sets can be given back: dynamic UI
        // textures (a font atlas, say) come and go with a document, and without
        // this flag vkFreeDescriptorSets is illegal and the pool would only ever
        // drain. See IUIRender::DynTextureCreate.
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets       = 256;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        VK_CHECK(vkCreateDescriptorPool(VulkanHW.m_Device, &poolInfo, nullptr, &s_DescriptorPool));

        // 4. Pipeline layout: 1 descriptor set + push-constant for screen size.
        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushConstant.offset     = 0;
        pushConstant.size       = 8;  // vec2 screenSize

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount         = 1;
        pipelineLayoutInfo.pSetLayouts            = &s_DescriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges    = &pushConstant;
        VK_CHECK(vkCreatePipelineLayout(VulkanHW.m_Device, &pipelineLayoutInfo, nullptr, &s_PipelineLayout));

        // 5. Fallback texture — used when a UI shader's texture file is missing.
        // Solid white is hostile (paints menu bg pure white). Solid transparent
        // hides backgrounds entirely (looks empty). Use a dim Stalker-grey so
        // missing assets at least look like an intentional Zone-mood backdrop.
        u32 stalkerGrey = 0xFF1A2018u;  // R8G8B8A8: ARGB ≈ dark olive-grey
        s_WhiteTexture.CreateFromData(&stalkerGrey, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);

        // 6. Descriptor set for white texture.
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = s_DescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &s_DescriptorSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(VulkanHW.m_Device, &allocInfo, &s_WhiteTextureSet));

        VK::DescriptorWriter(s_WhiteTextureSet)
            .ImageSampler(0, s_WhiteTexture.GetView(), s_WhiteTexture.GetSampler())
            .Flush();

        // 6b. A REAL white, for shaders that have no texture by design (hud\crosshair,
        // line primitives): R4 drew those with the vertex colour alone. Through the
        // olive fallback above the white crosshair came out near-black (06-10). The
        // olive stays for what it was meant for -- a texture NAME that did not load.
        u32 plainWhite = 0xFFFFFFFFu;
        s_PlainTexture.CreateFromData(&plainWhite, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);
        VK_CHECK(vkAllocateDescriptorSets(VulkanHW.m_Device, &allocInfo, &s_PlainTextureSet));
        VK::DescriptorWriter(s_PlainTextureSet)
            .ImageSampler(0, s_PlainTexture.GetView(), s_PlainTexture.GetSampler())
            .Flush();

        // 7. Lazily create the SPIR-V loader, then load UI shaders.
        if (!g_ShaderManager) {
            g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
            Msg("[Vulkan UI] Created g_ShaderManager (lazy init)");
        }
        VkShaderModule vertShader = g_ShaderManager->Load("ui.vert.spv");
        VkShaderModule fragShader = g_ShaderManager->Load("ui.frag.spv");
        if (vertShader == VK_NULL_HANDLE || fragShader == VK_NULL_HANDLE) {
            Msg("![Vulkan UI] Failed to load UI shaders (ui.vert.spv / ui.frag.spv)");
            return;
        }

        // 8. Build the graphics pipeline. FVF::TL = vec4 pos + u32 color + vec2 uv = 28 B.
        const VkVertexInputAttributeDescription attributes[3]{
            { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0  },   // pos
            { 1, 0, VK_FORMAT_B8G8R8A8_UNORM,      16 },   // color
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,       20 },   // uv
        };
        // The three pipelines differ only in topology. UIRender drives ptLineList
        // (HUD crosshair) / ptLineStrip (UIWindow borders); without them the line
        // endpoints would assemble into stray stretched triangles (the "sky spike").
        auto makeUIPipe = [&](VkPrimitiveTopology topo, const char* tag) {
            return VK::GfxPipelineBuilder(s_PipelineLayout)
                .Vert(vertShader).Frag(fragShader)
                .Binding(0, 28).Attrs(attributes, 3)
                .Topology(topo)
                .Cull(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE)
                .Color(Swapchain.m_Format).BlendAlpha()
                .Build("UI %s", tag);   // depth disabled for UI
        };
        s_Pipeline = makeUIPipe(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, "triangles");
        if (s_Pipeline == VK_NULL_HANDLE)
            return;
        s_PipelineLineList  = makeUIPipe(VK_PRIMITIVE_TOPOLOGY_LINE_LIST,  "line list");
        s_PipelineLineStrip = makeUIPipe(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, "line strip");

        Msg("[Vulkan UI] UI infrastructure created successfully");
    }

    void Destroy()
    {
        if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
        Msg("[Vulkan UI] Destroying UI infrastructure...");
        VK_DeviceWaitIdleLocked();

        // Free the engine-lifetime UI texture cache (UI/map/font/HUD textures +
        // any video staging buffer) before we tear down the descriptor pool and
        // (later) the VMA allocator. Defined at global scope in vk_RenderFactory.cpp.
        ::VK_ClearUITextureCache();

        if (s_Pipeline)            { vkDestroyPipeline(VulkanHW.m_Device, s_Pipeline, nullptr);                   s_Pipeline            = VK_NULL_HANDLE; }
        if (s_PipelineLineList)    { vkDestroyPipeline(VulkanHW.m_Device, s_PipelineLineList, nullptr);           s_PipelineLineList    = VK_NULL_HANDLE; }
        if (s_PipelineLineStrip)   { vkDestroyPipeline(VulkanHW.m_Device, s_PipelineLineStrip, nullptr);          s_PipelineLineStrip   = VK_NULL_HANDLE; }
        if (s_PipelineLayout)      { vkDestroyPipelineLayout(VulkanHW.m_Device, s_PipelineLayout, nullptr);       s_PipelineLayout      = VK_NULL_HANDLE; }
        s_WhiteTexture.Destroy();
        s_PlainTexture.Destroy();
        if (s_DescriptorPool)     { vkDestroyDescriptorPool(VulkanHW.m_Device, s_DescriptorPool, nullptr);       s_DescriptorPool      = VK_NULL_HANDLE; }
        if (s_DescriptorSetLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_DescriptorSetLayout, nullptr); s_DescriptorSetLayout = VK_NULL_HANDLE; }
        if (s_pMappedVB)           { s_VertexBuffer.Unmap(); s_pMappedVB = nullptr; }
        s_VertexBuffer.Destroy();
        Msg("[Vulkan UI] UI infrastructure destroyed");
    }

    // ----- Frame UI pass ----------------------------------------------------

    // ⚠Diagnostic only, and capped: the UI pass state machine is what the 18-08
    // ESC-in-game crash turned out to live in, and every early-out below looks
    // identical from the outside -- "no UI this frame" -- while one of them ends
    // in a driver fault. `ui_pass_trace 1` prints the transitions; the cap keeps
    // a forgotten trace from filling a log.
    u32 s_TraceLeft = 0;
    void TraceUIPass(u32 lines) { s_TraceLeft = lines; }
    static void trace(const char* what, const void* a = nullptr, u32 b = 0)
    {
        if (!s_TraceLeft) return;
        --s_TraceLeft;
        Msg("~ [VK UIpass] %s cmd=%p n=%u frame=%u", what, a, b, Device.dwFrame);
    }

    void BeginUIPassInternal()
    {
        VkCommandBuffer cmd = g_VkUI_FrameCmd;
        if (!cmd)            { trace("begin REFUSED: no cmd"); return; }
        if (s_bUIPassActive) { trace("begin skipped: already active", cmd); return; }

        u32 imageIndex = Swapchain.m_CurrentImageIndex;
        if (imageIndex >= Swapchain.m_Images.size()) { trace("begin REFUSED: bad image index", cmd, imageIndex); return; }

        VkImage     img = Swapchain.m_Images[imageIndex];
        VkImageView view = Swapchain.m_ImageViews[imageIndex];
        if (!img || !view) { trace("begin REFUSED: null image/view", cmd, imageIndex); return; }

        // The comment that used to sit here said "image is already COLOR_ATTACHMENT
        // (Begin set it)" and that was FALSE: CRender::Begin transitions the HDR
        // SceneColor target, and deliberately leaves the SWAPCHAIN image alone
        // ("transitioned by the tonemap pass" — CRender_Vulkan.cpp). So on any frame
        // where the tonemap does not run — the loading screen and the main menu,
        // which have no scene — the UI opened a rendering scope on an image sitting
        // in PRESENT_SRC_KHR (or UNDEFINED on the very first use) while declaring
        // COLOR_ATTACHMENT_OPTIMAL. Caught 16-08 by `-vk_validation` as ~10×
        // VUID-vkCmdBeginRendering-pRenderingInfo-09592 clustered at level load.
        //
        // Routed through the frame graph's resource registry instead of a hand-placed
        // barrier, because the correct OLD layout is exactly what a hand-placed
        // barrier here cannot know: it is COLOR_ATTACHMENT when the tonemap ran and
        // PRESENT_SRC when it did not. Require() reads the tracked state, so it
        // transitions when needed and no-ops (into a plain write-after-write
        // ordering) when the tonemap already did it — which also subsumes the
        // SceneAttachmentBarrier this replaces, and more narrowly: that one was a
        // global memory barrier, this names the one image actually at stake.
        // ⚠Seeded at COLOR_ATTACHMENT_OUTPUT, not the default TOP_OF_PIPE. On a frame
        // where the UI is the FIRST toucher (menu, loading screen — no tonemap) this
        // seed becomes the srcStageMask of the transition Require emits, and the image
        // is the one vkAcquireNextImageKHR just handed over, whose semaphore the submit
        // waits at COLOR_ATTACHMENT_OUTPUT. TOP_OF_PIPE orders the transition against
        // nothing, so the write could land while the presentation engine still owns the
        // image — the same SYNC-HAZARD-WRITE-AFTER-READ the tonemap path had (16-08).
        VK::ImageState& scState = VK::g_FrameGraph.Track(img, VK_IMAGE_ASPECT_COLOR_BIT,
                                                         VK_IMAGE_LAYOUT_UNDEFINED,
                                                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
        scState.Require(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);

        // loadOp LOAD preserves the clear colour already in the image.
        // nfs28: OutputExtent = the swapchain extent, or the render-res rectangle the display crop stretches.
        VK::RenderingBuilder(VK::OutputExtent()).Color(view).BeginPlain(cmd);

        s_bUIPassActive = true;
        s_UIPassCmd     = cmd;
        trace("begin OK", cmd, imageIndex);
    }

    void EndUIPass()
    {
        if (!s_bUIPassActive) return;
        VkCommandBuffer cmd = g_VkUI_FrameCmd;
        if (!cmd) { s_bUIPassActive = false; s_UIPassCmd = VK_NULL_HANDLE; return; }
        // ⚠Only the buffer that opened it may close it -- see s_UIPassCmd. Ending
        // a pass on a stranger is the fault this whole guard chain is about.
        if (cmd != s_UIPassCmd) { s_bUIPassActive = false; s_UIPassCmd = VK_NULL_HANDLE; return; }

        vkCmdEndRendering(cmd);
        trace("end OK", cmd);
        s_UIPassCmd = VK_NULL_HANDLE;

        // No layout transition here: the image stays COLOR_ATTACHMENT. CRender::End
        // owns the single COLOR→PRESENT transition for the whole frame.
        Swapchain.m_bRenderedThisFrame = true;   // vestigial; End no longer branches on it
        s_bUIPassActive  = false;
        // NOTE: the vertex offset is NOT reset here — the GPU reads this
        // frame's vertex data long after EndUIPass records. OnFrameBegin
        // resets it when this slot's fence proves the read finished.
    }

    void OnFrameBegin()
    {
        // ⚠⚠A fresh command buffer has no rendering instance open, whatever the
        // last frame believed. Left sticky, this flag is a crash rather than a
        // glitch: BeginUIPassInternal returns early on it, every UI draw of the
        // new frame lands OUTSIDE a pass, and EndUIPass then calls
        // vkCmdEndRendering with none active -- ACCESS_VIOLATION in the driver.
        // It went sticky whenever a frame drew UI and then never reached End()
        // (Begin failed: no swapchain image, fence timeout, device lost).
        // 📏18-08: that is exactly the ESC-from-a-loaded-level crash; see the
        // long note in CRender::Begin.
        s_bUIPassActive = false;
        s_UIPassCmd     = VK_NULL_HANDLE;
    }

    void BeginExtract(u32 ringSlot, bool ringReady)
    {
        // Main thread, start of EVERY extracted frame -- also one that never reaches End (no swapchain image, fence
        // timeout, device lost): the old reset lived in Begin_Render and was skipped with it, so the count and the offset
        // grew past the ring and "wrapped" over draws still pending, and the base kept pointing at a region that was in flight.
        if (ringSlot >= CVulkanCommandManager::FRAMES_IN_FLIGHT) ringSlot = 0;
        UISlot& s = UI();
        s.count        = 0;
        s.vertexOffset = 0;
        s.vbBase       = ringSlot * (u32)VERTEX_BUFFER_SIZE;
        s.dead         = !ringReady;
    }

    void ReplayDeferredUI()
    {
        UISlot& q = UI();   // the slot of the frame being recorded (render thread)
        if (q.count == 0) return;
        VkCommandBuffer cmd = g_VkUI_FrameCmd;
        if (!cmd) { q.count = 0; return; }

        s_VertexBuffer.Flush();
        BeginUIPassInternal();
        // ⚠The pass may refuse to open (no swapchain image, no view). Recording
        // draws anyway is what the validation layer reported eleven times in a
        // row as "vkCmdDraw(): must be issued inside an active render pass" --
        // and the frame died on the vkCmdEndRendering that followed. Drop the
        // queue instead: a frame that cannot open its UI pass has no UI.
        if (!s_bUIPassActive) { q.count = 0; return; }
        if (s_Pipeline == VK_NULL_HANDLE) { q.count = 0; return; }

        for (u32 i = 0; i < q.count; ++i)
        {
            const DeferredUICmd& dcmd = q.cmds[i];
            switch (dcmd.type)
            {
            case DeferredUICmd::Draw: {
                VkPipeline pipe = dcmd.pipeline ? dcmd.pipeline : s_Pipeline;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                float screenSize[2] = { (float)RV().dwWidth, (float)RV().dwHeight };   // the frame snapshot, not live Device
                vkCmdPushConstants(cmd, s_PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, screenSize);
                VkBuffer     vbs[]     = { s_VertexBuffer.GetHandle() };
                VkDeviceSize offsets[] = { dcmd.vertexBufferOffset };
                vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offsets);
                VkDescriptorSet texSet = (dcmd.textureSet != VK_NULL_HANDLE) ? dcmd.textureSet : s_WhiteTextureSet;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_PipelineLayout, 0, 1, &texSet, 0, nullptr);
                vkCmdDraw(cmd, dcmd.vertexCount, 1, 0, 0);
                break;
            }
            case DeferredUICmd::Scissor:
            {
                const VkRect2D sr = VK::ScaleRectToOutput(dcmd.scissorRect);   // nfs28: Device px -> output px
                vkCmdSetScissor(cmd, 0, 1, &sr);
            }
                break;
            case DeferredUICmd::ResetScissor: {
                VkRect2D fullScissor{};
                fullScissor.extent = VK::OutputExtent();
                vkCmdSetScissor(cmd, 0, 1, &fullScissor);
                break;
            }
            }
        }
        q.count = 0;
    }
}  // namespace VulkanUI

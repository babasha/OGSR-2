// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// Minimal Dear ImGui Vulkan backend + profiler overlay. See vk_imgui.h.

#include "stdafx.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_rendering.h"       // VK::RenderingBuilder
#include "vk_imgui.h"
#include "vk_profiler.h"            // Prof::GetZones / GetMem / GetFrame
#include "vk_image.h"               // VK::CreateImage / CreateImageView
#include "vk_gfx_pipeline.h"        // VK::GfxPipelineBuilder
#include "HW_Vulkan.h"             // VulkanHW (device/allocator + single-time cmds)
#include "vk_swapchain.h"          // Swapchain.m_Format
#include "vk_shaders.h"            // g_ShaderManager (.spv loader)
#include "vk_command_buffer.h"     // CommandManager.GetCurrentFrame()

// Editor viewport state (CRender_Vulkan.cpp) — how many objects the host has placed.
namespace VKEditor { int HostModelCount(); }

#include "../../xrCore/xr_platform_keys.h"   // DIK_* -- the game editor's key map (embedded_editor feeds scan codes)
#include "../../../3rd_party/Src/imgui/imgui.h"
#include "../../../3rd_party/Src/imgui/imgui_internal.h"   // ImGuiContext::WithinFrameScope (game editor frame recovery)
#include "../../../3rd_party/Src/imgui/addons/ImGuizmo/ImGuizmo.h"

#include <mutex>

extern int ps_r_profiler;

namespace VK { namespace ImGuiVK {

namespace {

constexpr u32 kSlots = VK_FRAMES_IN_FLIGHT;

bool s_inited = false;
bool s_failed = false;

VkPipeline            s_pipeline   = VK_NULL_HANDLE;
VkPipelineLayout      s_layout     = VK_NULL_HANDLE;
VkDescriptorSetLayout s_setLayout  = VK_NULL_HANDLE;
VkDescriptorPool      s_pool       = VK_NULL_HANDLE;
VkDescriptorSet       s_fontSet    = VK_NULL_HANDLE;
VkSampler             s_sampler    = VK_NULL_HANDLE;

VkImage       s_fontImage = VK_NULL_HANDLE;
VmaAllocation s_fontAlloc = VK_NULL_HANDLE;
VkImageView   s_fontView  = VK_NULL_HANDLE;

struct FrameBuf {
    VkBuffer      vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
    VmaAllocation vbA = VK_NULL_HANDLE, ibA = VK_NULL_HANDLE;
    void*         vbMap = nullptr; void* ibMap = nullptr;
    VkDeviceSize  vbSize = 0, ibSize = 0;
};
FrameBuf s_frame[kSlots];

ImGuiContext* s_ctx = nullptr;

// ---- the embedded GAME editor (xrGame embedded_editor, Shift+F10) ----------
// Its own context: the engine drives it the DX way -- ImGui_Backend_NewFrame before the frame's logic (main thread),
// windows built in CLevel::OnFrame, ImGui::Render + ImGui_Backend_RenderDrawData in End_Render (render thread) --
// while the overlay above runs a whole NewFrame..Render of its own inside CRender::End. Sharing one context would
// interleave the two frames. The font atlas IS shared (one texture, one descriptor set).
// Its own vertex ring too: both can draw in the same frame, and the second upload would overwrite the first one's
// mapped data before the GPU read it.
ImGuiContext* s_gameCtx     = nullptr;
ImDrawData*   s_gamePending = nullptr;   // set by GameSubmit (End_Render), drawn and cleared by DrawGame (CRender::End)
FrameBuf      s_gameFrame[kSlots];

// Keeps whichever context the caller had current: the overlay switches to its own for one NewFrame..Render, and the
// game editor's context must still be the current one when xrGame calls ImGui next.
struct CtxScope
{
    ImGuiContext* prev;
    explicit CtxScope(ImGuiContext* c) : prev(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(c); }
    ~CtxScope() { ImGui::SetCurrentContext(prev); }
};

struct PushC { float scale[2]; float translate[2]; };

// ---- editor Log store (fed by the host via PushEditorLog) ------------------
struct LogLine { xr_string text; bool err; bool sel; };
xr_vector<LogLine> s_editorLog;
std::mutex         s_editorLogMtx;
bool               s_editorLogDirty = false; // a new line arrived → auto-scroll
bool               s_editorWantMouse = false; // ImGui captured the mouse this frame
constexpr size_t   kEditorLogMax = 4000;

// ---- editor Statistics overlay (mirrored from the SDK each frame) ----------
struct EdStats { float fps, rfps; int verts, tris, dips, lights, totalLights; bool valid; };
EdStats s_edStats{};

// ---- transform gizmo -------------------------------------------------------
// We are only the WIDGET here. The host (the SDK's IM_Manipulator) decides the
// operation, the coordinate space and the snap, owns the object list and the undo
// stack, and applies whatever comes back. We run ImGuizmo because it has to be drawn
// with OUR matrices: the host's ImGuizmo renders onto its DX9 surface, which sits
// UNDER our Vulkan child and is therefore invisible, and its SetRect covers the host's
// whole window rather than the inset viewport.
//
// The host sends a request, we service it during the next frame, and the host reads the
// outcome on its next call — one frame of lag, imperceptible during a drag and far
// simpler than trying to run a widget synchronously across two ImGui contexts.
struct GizmoState
{
    bool requested = false;     // host asked for a gizmo this frame
    bool live = false;          // ...and it was still asking when we drew
    int op = 0;                 // 0 translate, 1 rotate, 2 scale
    int mode = 0;               // 0 local, 1 world
    bool useSnap = false;
    float snap[3] = {0.f, 0.f, 0.f};
    Fmatrix matrix = {};        // in: object transform; out: manipulated transform
    Fmatrix delta = {};         // out: this frame's change
    bool changed = false;       // out: the manipulation moved something
    bool using_ = false;        // out: a drag is in progress
    bool over = false;          // out: the pointer is over a gizmo handle
};
GizmoState s_gizmo;

// ---- font atlas upload -----------------------------------------------------
bool CreateFontTexture()
{
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;

    // device-local image
    VK::ImageDesc fd;
    fd.format   = VK_FORMAT_R8G8B8A8_UNORM;
    fd.extent   = { (u32)w, (u32)h, 1 };
    fd.usage    = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    fd.memUsage = VMA_MEMORY_USAGE_AUTO;
    fd.name     = "ImGui.FontAtlas";
    if (!VK::CreateImage(fd, s_fontImage, s_fontAlloc))
        return false;

    // staging
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo baci{};
    baci.usage = VMA_MEMORY_USAGE_AUTO;
    baci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE; VmaAllocation stagingA = VK_NULL_HANDLE; VmaAllocationInfo sInfo{};
    if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &baci, &staging, &stagingA, &sInfo) != VK_SUCCESS) {
        Msg("![VK ImGui] font staging failed"); return false;
    }
    memcpy(sInfo.pMappedData, pixels, (size_t)bytes);
    vmaFlushAllocation(VulkanHW.m_Allocator, stagingA, 0, bytes);

    // copy + layout transitions on a one-shot command buffer
    VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
    auto barrier = [&](VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s_fontImage;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { (u32)w, (u32)h, 1 };
    vkCmdCopyBufferToImage(cmd, staging, s_fontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VulkanHW.EndSingleTimeCommands(cmd);
    VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, staging, stagingA);

    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = s_fontImage; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(VulkanHW.m_Device, &vci, nullptr, &s_fontView) != VK_SUCCESS) {
        Msg("![VK ImGui] font view failed"); return false;
    }
    io.Fonts->SetTexID((ImTextureID)(intptr_t)s_fontImage);   // unused (single texture)
    return true;
}

bool CreatePipeline()
{
    if (!g_ShaderManager) g_ShaderManager = xr_new<VK::CVulkanSPIRVLoader>();
    VkShaderModule vs = g_ShaderManager->Load("imgui.vert.spv");
    VkShaderModule fs = g_ShaderManager->Load("imgui.frag.spv");
    if (!vs || !fs) { Msg("![VK ImGui] shader load failed"); return false; }

    // set 0: combined image sampler (font), fragment
    if (!VK::MakeDescriptorSets({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER }, 1,
                                s_setLayout, s_pool, &s_fontSet,
                                VK_SHADER_STAGE_FRAGMENT_BIT, "ImGui.Font"))
        return false;

    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = -1000.f; si.maxLod = 1000.f; si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_sampler) != VK_SUCCESS) return false;

    VK::DescriptorWriter(s_fontSet).ImageSampler(0, s_fontView, s_sampler).Flush();

    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushC) };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &s_setLayout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &s_layout) != VK_SUCCESS) return false;

    VkVertexInputAttributeDescription attr[3]{
        { 0, 0, VK_FORMAT_R32G32_SFLOAT,  (u32)IM_OFFSETOF(ImDrawVert, pos) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,  (u32)IM_OFFSETOF(ImDrawVert, uv)  },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, (u32)IM_OFFSETOF(ImDrawVert, col) },
    };
    // ImGui's own blend: straight alpha over, and the destination alpha is kept
    // premultiplied (ONE_MINUS_SRC_ALPHA, not the ZERO the engine's BlendAlpha uses).
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = VK_TRUE;
    cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    s_pipeline = VK::GfxPipelineBuilder(s_layout)
        .Vert(vs).Frag(fs)
        .Binding(0, sizeof(ImDrawVert))
        .Attrs(attr, 3)
        .ColorBlend(Swapchain.m_Format, cb)
        .Build("ImGui");
    return s_pipeline != VK_NULL_HANDLE;
}

bool Init()
{
    if (s_inited) return true;
    if (s_failed) return false;
    if (VulkanHW.m_Device == VK_NULL_HANDLE || Swapchain.m_Format == VK_FORMAT_UNDEFINED) return false;

    IMGUI_CHECKVERSION();
    s_ctx = ImGui::CreateContext();
    CtxScope scope(s_ctx);   // CreateContext makes it current only when none is; the game editor's may be
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;            // no imgui.ini on disk
    io.LogFilename = nullptr;
    io.BackendRendererName = "vk_imgui";
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    ImGui::GetStyle().Alpha = 0.92f;

    if (!CreateFontTexture() || !CreatePipeline()) {
        Msg("![VK ImGui] init FAILED — overlay disabled");
        s_failed = true;
        return false;
    }
    s_inited = true;
    Msg("[VK ImGui] overlay ready");
    return true;
}

// grow/create this slot's vertex+index buffers to fit
void EnsureBuffers(FrameBuf& f, VkDeviceSize vbNeed, VkDeviceSize ibNeed)
{
    auto make = [&](VkBuffer& buf, VmaAllocation& alloc, void*& map, VkDeviceSize& cap,
                    VkDeviceSize need, VkBufferUsageFlags usage) {
        if (cap >= need && buf) return;
        if (buf) VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, buf, alloc);
        VkDeviceSize sz = need + (need / 2) + 4096;
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = sz; bci.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (VK::Vram::CreateBuffer(VulkanHW.m_Allocator, &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS) {
            buf = VK_NULL_HANDLE; cap = 0; map = nullptr; return;
        }
        map = info.pMappedData; cap = sz;
    };
    make(f.vb, f.vbA, f.vbMap, f.vbSize, vbNeed, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    make(f.ib, f.ibA, f.ibMap, f.ibSize, ibNeed, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

// Upload `dd` into this frame's slot of `ring` and draw it on top of `view`. Every texture is the font atlas (no
// window here binds an image of its own), so TextureId is not consulted.
void RecordDrawData(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent, ImDrawData* dd, FrameBuf* ring, const char* label)
{
    if (!dd || dd->TotalVtxCount == 0) return;

    const u32 slot = CommandManager.GetCurrentFrame() % kSlots;
    FrameBuf& f = ring[slot];
    const VkDeviceSize vbNeed = (VkDeviceSize)dd->TotalVtxCount * sizeof(ImDrawVert);
    const VkDeviceSize ibNeed = (VkDeviceSize)dd->TotalIdxCount * sizeof(ImDrawIdx);
    EnsureBuffers(f, vbNeed, ibNeed);
    if (!f.vb || !f.ib) return;

    // upload all command lists into the slot's buffers
    {
        ImDrawVert* vtx = (ImDrawVert*)f.vbMap;
        ImDrawIdx*  idx = (ImDrawIdx*)f.ibMap;
        for (int n = 0; n < dd->CmdListsCount; ++n) {
            const ImDrawList* cl = dd->CmdLists[n];
            memcpy(vtx, cl->VtxBuffer.Data, cl->VtxBuffer.Size * sizeof(ImDrawVert));
            memcpy(idx, cl->IdxBuffer.Data, cl->IdxBuffer.Size * sizeof(ImDrawIdx));
            vtx += cl->VtxBuffer.Size;
            idx += cl->IdxBuffer.Size;
        }
        vmaFlushAllocation(VulkanHW.m_Allocator, f.vbA, 0, vbNeed);
        vmaFlushAllocation(VulkanHW.m_Allocator, f.ibA, 0, ibNeed);
    }

    Prof::CmdBeginLabel(cmd, label, 0.9f, 0.7f, 0.2f);

    // loadOp LOAD — draw on top of the final frame. Scissor is per-draw below,
    // so this opens the scope and sets only the viewport.
    VK::RenderingBuilder(extent).Color(view).Begin(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &s_fontSet, 0, nullptr);

    VkViewport vp{ 0.f, 0.f, (float)extent.width, (float)extent.height, 0.f, 1.f };
    vkCmdSetViewport(cmd, 0, 1, &vp);

    PushC pc;
    pc.scale[0] = 2.0f / dd->DisplaySize.x;
    pc.scale[1] = 2.0f / dd->DisplaySize.y;
    pc.translate[0] = -1.0f - dd->DisplayPos.x * pc.scale[0];
    pc.translate[1] = -1.0f - dd->DisplayPos.y * pc.scale[1];
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushC), &pc);

    VkDeviceSize voff = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f.vb, &voff);
    vkCmdBindIndexBuffer(cmd, f.ib, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);

    // ImGui coordinates are in DisplaySize space; the image may be a different size (the Switch display crop, a
    // game editor laid out at Device.dwWidth/dwHeight). The viewport maps positions; clip rects need the same factor.
    const ImVec2 clipOff = dd->DisplayPos;
    const float  sx = (float)extent.width / dd->DisplaySize.x, sy = (float)extent.height / dd->DisplaySize.y;
    int globalVtx = 0, globalIdx = 0;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImDrawCmd* pcmd = &cl->CmdBuffer[c];
            float x0 = (pcmd->ClipRect.x - clipOff.x) * sx, y0 = (pcmd->ClipRect.y - clipOff.y) * sy;
            float x1 = (pcmd->ClipRect.z - clipOff.x) * sx, y1 = (pcmd->ClipRect.w - clipOff.y) * sy;
            if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
            if (x1 > extent.width)  x1 = (float)extent.width;
            if (y1 > extent.height) y1 = (float)extent.height;
            if (x1 <= x0 || y1 <= y0) continue;
            VkRect2D sc{ { (int32_t)x0, (int32_t)y0 }, { (u32)(x1 - x0), (u32)(y1 - y0) } };
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdDrawIndexed(cmd, pcmd->ElemCount, 1,
                             pcmd->IdxOffset + globalIdx, pcmd->VtxOffset + globalVtx, 0);
        }
        globalVtx += cl->VtxBuffer.Size;
        globalIdx += cl->IdxBuffer.Size;
    }

    vkCmdEndRendering(cmd);
    Prof::CmdEndLabel(cmd);
}

// ---- the profiler window ---------------------------------------------------
void BuildProfilerWindow()
{
    const Prof::ZoneStat* zones = nullptr;
    const u32 nz = Prof::GetZones(&zones);
    Prof::MemSnap   mem = Prof::GetMem();
    Prof::FrameInfo fr  = Prof::GetFrame();

    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("VK Profiler", nullptr, ImGuiWindowFlags_NoNav)) { ImGui::End(); return; }

    ImGui::Text("FPS %.0f   CPU %.2f ms   GPU %.2f ms", fr.fps, fr.cpuMs, fr.gpuMs);

    // frame-time history graph (kept locally; the zone rings are GPU-only)
    static float s_frameHist[120] = {};
    static int   s_fhHead = 0;
    s_frameHist[s_fhHead] = fr.cpuMs;
    s_fhHead = (s_fhHead + 1) % IM_ARRAYSIZE(s_frameHist);
    ImGui::PlotLines("##frame", s_frameHist, IM_ARRAYSIZE(s_frameHist), s_fhHead,
                     "CPU frame ms", 0.0f, 33.3f, ImVec2(-1, 48));

    ImGui::Separator();
    ImGui::TextDisabled("Pass GPU ms (last / min / max)");

    // scale bars to the slowest pass so the breakdown reads at a glance
    float maxMs = 0.1f;
    for (u32 z = 0; z < nz; ++z) if (zones[z].gpuMax > maxMs) maxMs = zones[z].gpuMax;

    for (u32 z = 0; z < nz; ++z) {
        const Prof::ZoneStat& s = zones[z];
        char ov[48];
        _snprintf(ov, sizeof(ov), "%.2f  (%.2f/%.2f)", s.gpuLast, s.gpuMin, s.gpuMax);
        ImGui::ProgressBar(s.gpuLast / maxMs, ImVec2(-150, 0), ov);
        ImGui::SameLine(); ImGui::Text("%s", s.name);
    }

    ImGui::Separator();
    const double usedMB = double(mem.usedBytes) / (1024.0 * 1024.0);
    const double budMB  = double(mem.budgetBytes) / (1024.0 * 1024.0);
    char vov[48];
    _snprintf(vov, sizeof(vov), "%.0f / %.0f MB", usedMB, budMB);
    ImGui::ProgressBar(budMB > 0 ? float(usedMB / budMB) : 0.f, ImVec2(-1, 0), vov);
    ImGui::Text("VRAM allocs %u in %u blocks", mem.allocCount, mem.blockCount);
    ImGui::TextDisabled("draws %u  inst %u  tris %u  pipe %u  desc %u",
                        fr.draws, fr.instances, fr.tris, fr.pipeBinds, fr.descBinds);
    ImGui::End();
}

// SPIKE 3 (editor-on-Vulkan): interactive panel proving editor UI runs on the
// Vulkan ImGui backend with (polled) mouse input. Button/slider/checkbox update
// on click — the same widgets the editor's panels use. See ROADMAP §12.
void BuildSpikeWindow()
{
    static int   clicks = 0;
    static float slider = 0.5f;
    static bool  check  = false;
    ImGui::SetNextWindowPos(ImVec2(40, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 210), ImGuiCond_FirstUseEver);
    ImGui::Begin("Spike 3 - ImGui on Vulkan");
    ImGui::TextWrapped("Editor-UI proof: this panel runs on the Vulkan ImGui backend "
                       "with polled mouse input. Try the button / slider.");
    ImGui::Separator();
    if (ImGui::Button("Click me")) { ++clicks; Msg("[VK][spike] ImGui button click #%d", clicks); }
    ImGui::SameLine();
    ImGui::Text("clicks: %d", clicks);
    ImGui::SliderFloat("value", &slider, 0.0f, 1.0f);
    ImGui::Checkbox("checkbox", &check);
    const ImVec2 mp = ImGui::GetIO().MousePos;
    ImGui::Text("mouse: %.0f, %.0f", mp.x, mp.y);
    ImGui::End();
}

// ---- editor Log window -----------------------------------------------------
// Mirrors the SDK's IM_Log (selectable lines, Clear / Clear selected) plus Copy.
void BuildEditorLogWindow()
{
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 260), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log", nullptr, ImGuiWindowFlags_NoNav))
    {
        ImGui::End();
        return;
    }

    const float footer = ImGui::GetFrameHeightWithSpacing() + 6.0f;
    ImGui::BeginChild("##loglines", ImVec2(0, -footer), true, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(s_editorLogMtx);
        int id = 0;
        for (LogLine& l : s_editorLog)
        {
            if (l.err) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
            ImGui::PushID(id++);
            ImGui::Selectable(l.text.c_str(), &l.sel);
            ImGui::PopID();
            if (l.err) ImGui::PopStyleColor(1);
        }
        // Auto-scroll to the bottom when new lines arrived and we're already near it.
        if (s_editorLogDirty && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
            ImGui::SetScrollHereY(1.0f);
        s_editorLogDirty = false;
    }
    ImGui::EndChild();

    if (ImGui::Button("Copy")) // selected lines if any, else all — to the OS clipboard
    {
        xr_string out;
        std::lock_guard<std::mutex> lk(s_editorLogMtx);
        bool any = false;
        for (const LogLine& l : s_editorLog) if (l.sel) { any = true; break; }
        for (const LogLine& l : s_editorLog)
            if (!any || l.sel) { out += l.text; out += "\r\n"; }
        ImGui::SetClipboardText(out.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        std::lock_guard<std::mutex> lk(s_editorLogMtx);
        s_editorLog.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear selected"))
    {
        std::lock_guard<std::mutex> lk(s_editorLogMtx);
        s_editorLog.erase(std::remove_if(s_editorLog.begin(), s_editorLog.end(),
                                         [](const LogLine& l) { return l.sel; }),
                          s_editorLog.end());
    }
    ImGui::End();
}

// ---- editor Statistics overlay ---------------------------------------------
// Mirrors the SDK's IM_Stats: borderless, transparent, white text, same fields.
//
// The numbers come from OUR renderer, not from the host's. Once the host stops drawing
// the scene itself (it hands the viewport to us), its DX9 counters describe nothing but
// its own UI — so reporting them here would show a near-empty scene while we are drawing
// thousands of objects. Ed_SetStats stays in the facade for a host that renders its own
// view, but the editor overlay reports the renderer actually producing the picture.
void BuildEditorStatsWindow()
{
    ImGui::SetNextWindowPos(ImVec2(60, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.0f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("Statistics", nullptr, flags))
    {
        // Only what this renderer actually measures. Deliberately NO tris/draw-call counts:
        // the VK backend has no RCache.stat, and the profiler's draw counters are dead —
        // Prof::CountDraw() exists but is called from nowhere, so they would always read 0.
        // Reporting a hard 0 next to a visibly full scene is worse than not reporting it;
        // wiring CountDraw into the draw path is the real fix if these are wanted.
        const Prof::FrameInfo fi = Prof::GetFrame();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::Text("FPS/RFPS:     %3.1f/%3.1f", Device.Statistic->fFPS, Device.Statistic->fRFPS);
        ImGui::Text("CPU/GPU ms:   %.2f/%.2f", fi.cpuMs, fi.gpuMs);
        ImGui::Text("OBJECTS:      %d", VKEditor::HostModelCount());
        // Lights come from the host: with no level loaded we run no light manager of our own.
        if (s_edStats.valid)
            ImGui::Text("LIGHT S/T:    %d/%d", s_edStats.lights, s_edStats.totalLights);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

// ---- screen-space orientation compass (RGB axis gizmo) ---------------------
// Bottom-left corner of the viewport, like the SDK's coordinate marker. Uses the
// camera basis so it reflects the current view orientation. Drawn via ImGui's
// foreground draw list — no dedicated pipeline needed.
void DrawAxisCompass(VkExtent2D extent)
{
    const Fvector& R = Device.vCameraRight;
    const Fvector& U = Device.vCameraTop;

    // World axes projected onto the screen basis: screen.x = dot(axis,Right),
    // screen.y = -dot(axis,Up) (screen y is down). Depth (dot with Dir) is not needed.
    auto project = [&](const Fvector& a) -> ImVec2 {
        return ImVec2(a.dotproduct(R), -a.dotproduct(U));
    };

    const float len = 34.0f;
    const ImVec2 org(58.0f, (float)extent.height - 58.0f); // bottom-left anchor
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    struct Axis { Fvector dir; ImU32 col; const char* name; };
    const Axis axes[3] = {
        { {1.f, 0.f, 0.f}, IM_COL32(230, 60, 60, 255),  "X" },
        { {0.f, 1.f, 0.f}, IM_COL32(70, 210, 70, 255),  "Y" },
        { {0.f, 0.f, 1.f}, IM_COL32(80, 130, 240, 255), "Z" },
    };
    for (const Axis& ax : axes)
    {
        const ImVec2 p = project(ax.dir);
        const ImVec2 tip(org.x + p.x * len, org.y + p.y * len);
        dl->AddLine(org, tip, ax.col, 2.0f);
        dl->AddText(ImVec2(tip.x - 4.f, tip.y - 7.f), ax.col, ax.name);
    }
    dl->AddCircleFilled(org, 3.0f, IM_COL32(200, 200, 200, 255));
}

// Run the host's gizmo request against OUR view/projection, which is the whole reason
// the widget lives on this side of the boundary.
void DrawGizmo(VkExtent2D extent)
{
    s_gizmo.live = s_gizmo.requested;
    s_gizmo.requested = false; // one frame per request; the host re-arms every frame
    s_gizmo.changed = false;
    if (!s_gizmo.live)
    {
        s_gizmo.using_ = false;
        s_gizmo.over = false;
        return;
    }

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(0.f, 0.f, (float)extent.width, (float)extent.height);

    const ImGuizmo::OPERATION op = s_gizmo.op == 1 ? ImGuizmo::ROTATE : (s_gizmo.op == 2 ? ImGuizmo::SCALE : ImGuizmo::TRANSLATE);
    const ImGuizmo::MODE mode = s_gizmo.mode == 1 ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

    s_gizmo.delta.identity();
    s_gizmo.changed = ImGuizmo::Manipulate((const float*)&Device.mView, (const float*)&Device.mProject, op, mode, (float*)&s_gizmo.matrix, (float*)&s_gizmo.delta,
                                           s_gizmo.useSnap ? s_gizmo.snap : nullptr);

    s_gizmo.using_ = ImGuizmo::IsUsing();
    s_gizmo.over = ImGuizmo::IsOver();
}

} // anonymous namespace

bool EditorWantsMouse() { return s_editorWantMouse; }

void SetGizmo(int op, int mode, const float* snap, const Fmatrix& xform)
{
    s_gizmo.requested = true;
    s_gizmo.op = op;
    s_gizmo.mode = mode;
    s_gizmo.useSnap = snap != nullptr;
    if (snap)
    {
        // ImGuizmo reads one float for translate/scale and three for rotate; copying
        // three is harmless for the former and required for the latter.
        s_gizmo.snap[0] = snap[0];
        s_gizmo.snap[1] = snap[0];
        s_gizmo.snap[2] = snap[0];
    }

    // Take the host's matrix as authoritative every frame: it reflects whatever the host
    // actually applied, including its own snapping and any edit from elsewhere.
    if (!s_gizmo.using_)
        s_gizmo.matrix = xform;
}

int GizmoResult(Fmatrix& out_xform, Fmatrix& out_delta)
{
    out_xform = s_gizmo.matrix;
    out_delta = s_gizmo.delta;

    // Consume the flag: a result describes ONE serviced frame. If the overlay stops
    // running — DrawOverlay bails early on a zero extent mid-resize, for instance — a
    // sticky flag would have the host re-apply the same delta every frame and walk the
    // object off on its own.
    const int changed = s_gizmo.changed ? 1 : 0;
    s_gizmo.changed = false;
    return changed;
}

bool GizmoIsUsing() { return s_gizmo.using_; }
bool GizmoWantsMouse() { return s_gizmo.over || s_gizmo.using_; }

void PushEditorStats(float fps, float rfps, int verts, int tris, int dips, int lights, int totalLights)
{
    s_edStats = { fps, rfps, verts, tris, dips, lights, totalLights, true };
}

void PushEditorLog(const char* text, bool isError)
{
    if (!text || !text[0]) return;
    std::lock_guard<std::mutex> lk(s_editorLogMtx);
    s_editorLog.push_back({ xr_string(text), isError, false });
    if (s_editorLog.size() > kEditorLogMax)
        s_editorLog.erase(s_editorLog.begin(), s_editorLog.begin() + (s_editorLog.size() - kEditorLogMax));
    s_editorLogDirty = true;
}

void DrawOverlay(VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D extent, float dt)
{
    // SPIKE 3: also run when -vk_spike is set (interactive editor-UI probe), not
    // only for the r_profiler>=2 overlay. -vk_editor adds the hosted-editor overlay
    // (Log window + axis compass over the viewport).
    static const bool s_spike  = Core.Params && strstr(Core.Params, "-vk_spike");
    static const bool s_editor = Core.Params && strstr(Core.Params, "-vk_editor");
    if (ps_r_profiler < 2 && !s_spike && !s_editor) return;
    if (!Init()) return;
    if (extent.width == 0 || extent.height == 0) return;

    CtxScope scope(s_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)extent.width, (float)extent.height);
    io.DeltaTime   = dt > 1e-5f ? dt : 1.0f / 60.0f;

    // SPIKE 3: polled mouse input (no imgui_impl_win32 in the tree). Enough to
    // prove editor-UI interactivity on Vulkan; best at the menu where the cursor
    // is free (in-level the game captures the mouse for look). ImGui draws its
    // own cursor so it's obvious the backend is receiving the mouse.
    if (s_spike) {
        io.MouseDrawCursor = true;
        POINT pt;
        if (GetCursorPos(&pt) && VulkanHW.m_hWnd) {
            ScreenToClient(VulkanHW.m_hWnd, &pt);
            io.AddMousePosEvent((float)pt.x, (float)pt.y);
        }
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    }

    // Editor overlay is INTERACTIVE (Log window is clickable). Feed polled mouse — but
    // NOT MouseDrawCursor: the host owns a real OS cursor (see EnforceCursor). Buttons
    // come from global key state; position from the viewport window's client space.
    if (s_editor) {
        POINT pt;
        if (GetCursorPos(&pt) && VulkanHW.m_hWnd) {
            ScreenToClient(VulkanHW.m_hWnd, &pt);
            io.AddMousePosEvent((float)pt.x, (float)pt.y);
        }
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    }

    ImGui::NewFrame();
    if (s_editor) ImGuizmo::BeginFrame();
    if (ps_r_profiler >= 2) BuildProfilerWindow();
    if (s_spike)            BuildSpikeWindow();
    if (s_editor)         { BuildEditorLogWindow(); BuildEditorStatsWindow(); DrawAxisCompass(extent); DrawGizmo(extent); }
    ImGui::Render();

    // Publish capture state so the editor orbit camera yields when the pointer is over
    // an ImGui window (the Log panel) — read next frame by CEditorViewport::PollInput.
    // The gizmo counts too, or dragging a handle would orbit the camera at the same time.
    if (s_editor)
        s_editorWantMouse = ImGui::GetIO().WantCaptureMouse || s_gizmo.over || s_gizmo.using_;
    RecordDrawData(cmd, swapchainView, extent, ImGui::GetDrawData(), s_frame, "ImGui Overlay");
}

// ---- the embedded GAME editor ----------------------------------------------
bool GameInit()
{
    if (s_gameCtx) return true;
    if (!Init()) return false;   // pipeline + the font atlas the game context shares
    ImFontAtlas* atlas = nullptr;
    {
        CtxScope scope(s_ctx);
        atlas = ImGui::GetIO().Fonts;
    }
    s_gameCtx = ImGui::CreateContext(atlas);
    ImGui::SetCurrentContext(s_gameCtx);   // from here on THE context xrGame talks to
    ImGuiIO& io = ImGui::GetIO();
    // Window layout persists in the user's app data: the editor's windows open at ImGui's default size (the Weather
    // panel cuts its labels), so a resize the player makes once has to survive the session.
    static string_path s_ini;
    FS.update_path(s_ini, "$app_data_root$", "imgui_editor.ini");
    io.IniFilename = s_ini;
    io.LogFilename = nullptr;
    io.BackendRendererName = "vk_imgui (game editor)";
    // embedded_editor feeds the legacy io.KeysDown[] with DirectInput scan codes (Editor_KeyPress), so the
    // navigation keys are mapped to the same codes -- without this a text field takes characters but no
    // Backspace, arrows or Enter.
    io.KeyMap[ImGuiKey_Tab]        = DIK_TAB;
    io.KeyMap[ImGuiKey_LeftArrow]  = DIK_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = DIK_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow]    = DIK_UP;
    io.KeyMap[ImGuiKey_DownArrow]  = DIK_DOWN;
    io.KeyMap[ImGuiKey_PageUp]     = DIK_PRIOR;
    io.KeyMap[ImGuiKey_PageDown]   = DIK_NEXT;
    io.KeyMap[ImGuiKey_Home]       = DIK_HOME;
    io.KeyMap[ImGuiKey_End]        = DIK_END;
    io.KeyMap[ImGuiKey_Insert]     = DIK_INSERT;
    io.KeyMap[ImGuiKey_Delete]     = DIK_DELETE;
    io.KeyMap[ImGuiKey_Backspace]  = DIK_BACK;
    io.KeyMap[ImGuiKey_Space]      = DIK_SPACE;
    io.KeyMap[ImGuiKey_Enter]      = DIK_RETURN;
    io.KeyMap[ImGuiKey_Escape]     = DIK_ESCAPE;
    io.KeyMap[ImGuiKey_KeypadEnter] = DIK_NUMPADENTER;
    io.KeyMap[ImGuiKey_A] = DIK_A; io.KeyMap[ImGuiKey_C] = DIK_C; io.KeyMap[ImGuiKey_V] = DIK_V;
    io.KeyMap[ImGuiKey_X] = DIK_X; io.KeyMap[ImGuiKey_Y] = DIK_Y; io.KeyMap[ImGuiKey_Z] = DIK_Z;
    ImGui::StyleColorsDark();
    Msg("[VK ImGui] game editor context ready");
    return true;
}

void GameNewFrame()
{
    // Join the render thread first. This runs before the frame's Begin, so the previous frame may still be in flight
    // (the frame the editor opens on overlapped), and that frame's DrawOverlay switches ImGui's current context --
    // a global -- for its own NewFrame..Render. The first call also builds GPU objects through the queue.
    RT_WaitIdle();
    if (!s_gameCtx && !GameInit()) return;
    ImGui::SetCurrentContext(s_gameCtx);
    // A frame the loop abandoned (Frame_Logic bails while load events run) was opened and never rendered or ended.
    if (s_gameCtx->WithinFrameScope)
        ImGui::EndFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)Device.dwWidth, (float)Device.dwHeight);
    io.DeltaTime   = Device.fTimeDeltaReal > 1e-5f ? Device.fTimeDeltaReal : 1.0f / 60.0f;
    // The pointer is moved by Editor_MouseMove from relative deltas; start it in the middle of the screen.
    if (io.MousePos.x < 0.f || io.MousePos.y < 0.f)
        io.MousePos = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::NewFrame();
}

void GameSubmit(ImDrawData* dd) { s_gamePending = dd; }

void DrawGame(VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D extent)
{
    // The draw data belongs to the game context and stays valid until its next NewFrame -- which the main thread
    // cannot reach before this frame is done: editor frames are joined in CRenderDevice::End.
    ImDrawData* dd = s_gamePending;
    s_gamePending = nullptr;
    if (!dd || !s_inited || extent.width == 0 || extent.height == 0) return;
    RecordDrawData(cmd, swapchainView, extent, dd, s_gameFrame, "ImGui Game Editor");
}

void Shutdown()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;
    for (FrameBuf* ring : { s_frame, s_gameFrame })
        for (u32 i = 0; i < kSlots; ++i) {
            FrameBuf& f = ring[i];
            if (f.vb) VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, f.vb, f.vbA);
            if (f.ib) VK::Vram::DestroyBuffer(VulkanHW.m_Allocator, f.ib, f.ibA);
            f = FrameBuf{};
        }
    // The game context borrows the overlay context's font atlas: it goes first.
    if (s_gameCtx) { ImGui::DestroyContext(s_gameCtx); s_gameCtx = nullptr; s_gamePending = nullptr; }
    if (s_pipeline)  { vkDestroyPipeline(VulkanHW.m_Device, s_pipeline, nullptr); s_pipeline = VK_NULL_HANDLE; }
    if (s_layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, s_layout, nullptr); s_layout = VK_NULL_HANDLE; }
    if (s_pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, s_pool, nullptr); s_pool = VK_NULL_HANDLE; }
    if (s_setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_setLayout, nullptr); s_setLayout = VK_NULL_HANDLE; }
    if (s_sampler)   { vkDestroySampler(VulkanHW.m_Device, s_sampler, nullptr); s_sampler = VK_NULL_HANDLE; }
    if (s_fontView)  { vkDestroyImageView(VulkanHW.m_Device, s_fontView, nullptr); s_fontView = VK_NULL_HANDLE; }
    if (s_fontImage) { VK::Vram::DestroyImage(VulkanHW.m_Allocator, s_fontImage, s_fontAlloc); s_fontImage = VK_NULL_HANDLE; }
    if (s_ctx)       { ImGui::DestroyContext(s_ctx); s_ctx = nullptr; }
    s_inited = false;
}

}} // namespace VK::ImGuiVK

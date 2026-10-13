// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once

#include "vk_core.h"

namespace VK
{

// ============================================================================
// Graphics pipeline builder
//
// Every graphics pipeline in this renderer was hand-filling the same nine
// structs (ia/vp/rs/ms/ds/cb/dyn/prci/pi) plus the shader-stage array — ~55
// lines per site, of which ~45 were byte-identical boilerplate. This collapses
// that to the handful of lines that actually differ.
//
// Defaults reproduce what every site was already writing:
//   topology TRIANGLE_LIST, polygon FILL, cull NONE, front face CCW,
//   lineWidth 1, 1 sample, viewportCount/scissorCount 1,
//   dynamic state { VIEWPORT, SCISSOR },
//   dynamic rendering with no attachments,
//   pDepthStencilState = nullptr until Depth() is called.
//
// So a call site states only its own facts. Build() always goes through the
// shared VkPipelineCache (PipelineCache::GetCacheObject) and logs a tagged
// error + returns VK_NULL_HANDLE on failure.
//
//   VkPipeline p = VK::GfxPipelineBuilder(layout)
//       .Vert(vs).Frag(fs)
//       .Binding(0, 32).Attr(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0)
//       .Depth(true, true)
//       .Color(VK::SceneColor::Format())
//       .DepthTarget(Swapchain.m_DepthFormat)
//       .Build("Trees gfx tcOff=%u", tcOffset);
// ============================================================================

class GfxPipelineBuilder
{
public:
    enum : u32
    {
        kMaxStages   = 5,    // VS, TCS, TES, GS, FS
        kMaxBindings = 4,
        kMaxAttribs  = 16,
        kMaxColor    = 8,
        kMaxDynamic  = 8,
    };

    explicit GfxPipelineBuilder(VkPipelineLayout layout);

    // ---- programmable stages (entry point is always "main") ----------------
    GfxPipelineBuilder& Vert(VkShaderModule m, const VkSpecializationInfo* spec = nullptr);
    GfxPipelineBuilder& Frag(VkShaderModule m, const VkSpecializationInfo* spec = nullptr);
    GfxPipelineBuilder& Geom(VkShaderModule m);
    // Tessellation pair. Also switches the topology to PATCH_LIST and emits
    // pTessellationState — the three things always done together.
    GfxPipelineBuilder& Tess(VkShaderModule tcs, VkShaderModule tes, u32 patchPoints = 3);

    // ---- vertex input ------------------------------------------------------
    GfxPipelineBuilder& Binding(u32 binding, u32 stride, VkVertexInputRate rate = VK_VERTEX_INPUT_RATE_VERTEX);
    GfxPipelineBuilder& Bindings(const VkVertexInputBindingDescription* b, u32 count);
    GfxPipelineBuilder& Attr(u32 location, u32 binding, VkFormat fmt, u32 offset);
    GfxPipelineBuilder& Attrs(const VkVertexInputAttributeDescription* a, u32 count);

    // ---- rasterizer / input assembly --------------------------------------
    GfxPipelineBuilder& Topology(VkPrimitiveTopology t);
    GfxPipelineBuilder& Cull(VkCullModeFlags mode, VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE);
    GfxPipelineBuilder& Polygon(VkPolygonMode mode, float lineWidth = 1.0f);
    // Baked depth bias (co-planar geometry that must NOT be re-biased per draw).
    GfxPipelineBuilder& DepthBias(float constant, float slope);
    // Enable depth bias and let the caller set it per draw (adds DEPTH_BIAS to
    // the dynamic state) — what every shadow/prepass caster wants.
    GfxPipelineBuilder& DynamicDepthBias();

    // ---- depth / stencil ---------------------------------------------------
    // Until this is called pDepthStencilState stays nullptr (colour-only passes).
    GfxPipelineBuilder& Depth(bool test, bool write, VkCompareOp op = VK_COMPARE_OP_LESS_OR_EQUAL);

    // ---- attachments (dynamic rendering) -----------------------------------
    // Adds one colour attachment: its format plus a matching blend state
    // (blending off, full RGBA write unless `mask` narrows it).
    GfxPipelineBuilder& Color(VkFormat fmt,
                              VkColorComponentFlags mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    // Blend modes for the LAST added colour attachment. Both keep the alpha
    // channel at (ONE, ZERO, ADD) — the form every site in this renderer uses.
    GfxPipelineBuilder& BlendAlpha();   // (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) — translucency, decals
    GfxPipelineBuilder& BlendAdd();     // (SRC_ALPHA, ONE)                 — emissive, marks
    // Colour attachment with a caller-supplied blend state, for the sites whose
    // equation is neither of the two above (UI, fullscreen, particles). Call it
    // once per attachment to describe an MRT set.
    GfxPipelineBuilder& ColorBlend(VkFormat fmt, const VkPipelineColorBlendAttachmentState& blend);
    GfxPipelineBuilder& DepthTarget(VkFormat fmt);

    // ---- escape hatches ----------------------------------------------------
    GfxPipelineBuilder& Dynamic(VkDynamicState s);
    // Extra pNext chained onto VkPipelineRenderingCreateInfo (VRS/FSR state).
    // The pointed-to struct must outlive Build().
    GfxPipelineBuilder& RenderingNext(const void* p);
    GfxPipelineBuilder& CreateFlags(VkPipelineCreateFlags f);

    // Creates the pipeline against the shared cache. `tagFmt` is printf-style
    // and names the pipeline in the failure log (no success log — call sites
    // that want one already have the context to word it better).
    VkPipeline Build(const char* tagFmt, ...);

private:
    VkPipelineLayout m_layout;

    VkPipelineShaderStageCreateInfo m_stages[kMaxStages]{};
    u32                             m_nStages = 0;
    VkPipelineShaderStageCreateInfo& AddStage(VkShaderStageFlagBits stage, VkShaderModule m);

    VkVertexInputBindingDescription   m_bindings[kMaxBindings]{};
    u32                               m_nBindings = 0;
    VkVertexInputAttributeDescription m_attribs[kMaxAttribs]{};
    u32                               m_nAttribs = 0;

    VkPrimitiveTopology m_topology  = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool                m_tess      = false;
    u32                 m_patchPts  = 3;

    VkCullModeFlags m_cull      = VK_CULL_MODE_NONE;
    VkFrontFace     m_front     = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPolygonMode   m_polygon   = VK_POLYGON_MODE_FILL;
    float           m_lineWidth = 1.0f;
    bool            m_biasOn    = false;
    float           m_biasConst = 0.0f;
    float           m_biasSlope = 0.0f;

    bool         m_hasDepth   = false;
    bool         m_depthTest  = false;
    bool         m_depthWrite = false;
    VkCompareOp  m_depthOp    = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkFormat                            m_colorFormats[kMaxColor]{};
    VkPipelineColorBlendAttachmentState m_blends[kMaxColor]{};
    u32                                 m_nColor = 0;
    VkFormat                            m_depthFormat = VK_FORMAT_UNDEFINED;

    VkDynamicState m_dynamic[kMaxDynamic] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    u32            m_nDynamic = 2;

    const void*             m_renderingNext = nullptr;
    VkPipelineCreateFlags   m_flags = 0;
};

// ============================================================================
// PIPELINE STATISTICS (VK_KHR_pipeline_executable_properties) — what the DRIVER
// compiled a pipeline into: registers per stage, occupancy, spill, instruction
// count. It exists because this renderer keeps measuring deltas that the removed
// WORK cannot explain — the crown ubershader priced one varying and five ALU ops
// at 0.45 ms — and "that smells like an occupancy cliff" is a guess. This turns it
// into a printed number, for any pipeline, not just the trees.
//
// TWO-PART CONTRACT, and getting it wrong yields an empty report rather than an
// error: the statistics only exist for pipelines CREATED with the capture bit, so
// r_pipe_stats must be 1 BEFORE the pipeline is built (i.e. before level load for
// anything built at load). Capture is not free — it makes the driver keep the
// compile artefacts — hence the default 0.
namespace PipeStats
{
// Flags to OR into a pipeline's create flags: the capture bit when the extension
// is up AND r_pipe_stats is on, otherwise 0. Call at pipeline creation.
VkPipelineCreateFlags CreateFlags();

// Print every executable's statistics for `pipe` under a caller-supplied name.
// Says why it can print nothing (extension off / not captured) rather than
// printing an empty block — a silent empty report reads as "no registers used".
void Dump(VkPipeline pipe, const char* nameFmt, ...);
} // namespace PipeStats

} // namespace VK

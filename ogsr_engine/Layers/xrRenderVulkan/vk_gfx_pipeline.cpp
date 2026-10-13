// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "../../xrCore/load_prof.h" // [rt prof]: named parts of a slow render-thread job
#include "vk_gfx_pipeline.h"
#include "vk_pipeline_cache.h"   // VK::PipelineCache::GetCacheObject
#include "HW_Vulkan.h"           // VulkanHW (device)

#include <stdarg.h>

// GLOBAL scope on purpose: an extern declared INSIDE namespace VK mangles as
// VK::ps_r_pipe_stats and fails to link — and a failed link here DELETES the exe.
extern int ps_r_pipe_stats;   // r_pipe_stats — capture driver pipeline statistics at CREATE time

namespace VK
{

GfxPipelineBuilder::GfxPipelineBuilder(VkPipelineLayout layout) : m_layout(layout) {}

VkPipelineShaderStageCreateInfo& GfxPipelineBuilder::AddStage(VkShaderStageFlagBits stage, VkShaderModule m)
{
    VERIFY(m_nStages < kMaxStages);
    VkPipelineShaderStageCreateInfo& s = m_stages[m_nStages++];
    s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    s.stage  = stage;
    s.module = m;
    s.pName  = "main";
    return s;
}

GfxPipelineBuilder& GfxPipelineBuilder::Vert(VkShaderModule m, const VkSpecializationInfo* spec)
{
    AddStage(VK_SHADER_STAGE_VERTEX_BIT, m).pSpecializationInfo = spec;
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Frag(VkShaderModule m, const VkSpecializationInfo* spec)
{
    AddStage(VK_SHADER_STAGE_FRAGMENT_BIT, m).pSpecializationInfo = spec;
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Geom(VkShaderModule m)
{
    AddStage(VK_SHADER_STAGE_GEOMETRY_BIT, m);
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Tess(VkShaderModule tcs, VkShaderModule tes, u32 patchPoints)
{
    AddStage(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tcs);
    AddStage(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, tes);
    m_tess     = true;
    m_patchPts = patchPoints;
    m_topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Binding(u32 binding, u32 stride, VkVertexInputRate rate)
{
    VERIFY(m_nBindings < kMaxBindings);
    m_bindings[m_nBindings++] = { binding, stride, rate };
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Bindings(const VkVertexInputBindingDescription* b, u32 count)
{
    VERIFY(m_nBindings + count <= kMaxBindings);
    for (u32 i = 0; i < count; ++i) m_bindings[m_nBindings++] = b[i];
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Attr(u32 location, u32 binding, VkFormat fmt, u32 offset)
{
    VERIFY(m_nAttribs < kMaxAttribs);
    m_attribs[m_nAttribs++] = { location, binding, fmt, offset };
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Attrs(const VkVertexInputAttributeDescription* a, u32 count)
{
    VERIFY(m_nAttribs + count <= kMaxAttribs);
    for (u32 i = 0; i < count; ++i) m_attribs[m_nAttribs++] = a[i];
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Topology(VkPrimitiveTopology t) { m_topology = t; return *this; }

GfxPipelineBuilder& GfxPipelineBuilder::Cull(VkCullModeFlags mode, VkFrontFace front)
{
    m_cull = mode; m_front = front; return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Polygon(VkPolygonMode mode, float lineWidth)
{
    m_polygon = mode; m_lineWidth = lineWidth; return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::DepthBias(float constant, float slope)
{
    m_biasOn = true; m_biasConst = constant; m_biasSlope = slope; return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::DynamicDepthBias()
{
    m_biasOn = true;
    return Dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS);
}

GfxPipelineBuilder& GfxPipelineBuilder::Depth(bool test, bool write, VkCompareOp op)
{
    m_hasDepth = true; m_depthTest = test; m_depthWrite = write; m_depthOp = op; return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::Color(VkFormat fmt, VkColorComponentFlags mask)
{
    VERIFY(m_nColor < kMaxColor);
    m_colorFormats[m_nColor] = fmt;
    VkPipelineColorBlendAttachmentState& ba = m_blends[m_nColor];
    ba = {};
    ba.blendEnable    = VK_FALSE;
    ba.colorWriteMask = mask;
    ++m_nColor;
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::BlendAlpha()
{
    VERIFY(m_nColor > 0);
    VkPipelineColorBlendAttachmentState& ba = m_blends[m_nColor - 1];
    ba.blendEnable         = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp        = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    ba.alphaBlendOp        = VK_BLEND_OP_ADD;
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::BlendAdd()
{
    VERIFY(m_nColor > 0);
    VkPipelineColorBlendAttachmentState& ba = m_blends[m_nColor - 1];
    ba.blendEnable         = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.colorBlendOp        = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    ba.alphaBlendOp        = VK_BLEND_OP_ADD;
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::ColorBlend(VkFormat fmt, const VkPipelineColorBlendAttachmentState& blend)
{
    VERIFY(m_nColor < kMaxColor);
    m_colorFormats[m_nColor] = fmt;
    m_blends[m_nColor]       = blend;
    ++m_nColor;
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::DepthTarget(VkFormat fmt) { m_depthFormat = fmt; return *this; }

GfxPipelineBuilder& GfxPipelineBuilder::Dynamic(VkDynamicState s)
{
    VERIFY(m_nDynamic < kMaxDynamic);
    m_dynamic[m_nDynamic++] = s;
    return *this;
}

GfxPipelineBuilder& GfxPipelineBuilder::RenderingNext(const void* p) { m_renderingNext = p; return *this; }
GfxPipelineBuilder& GfxPipelineBuilder::CreateFlags(VkPipelineCreateFlags f) { m_flags |= f; return *this; }

VkPipeline GfxPipelineBuilder::Build(const char* tagFmt, ...)
{
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = m_nBindings;
    vi.pVertexBindingDescriptions      = m_nBindings ? m_bindings : nullptr;
    vi.vertexAttributeDescriptionCount = m_nAttribs;
    vi.pVertexAttributeDescriptions    = m_nAttribs ? m_attribs : nullptr;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = m_topology;

    VkPipelineTessellationStateCreateInfo ts{ VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
    ts.patchControlPoints = m_patchPts;

    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = m_polygon;
    rs.cullMode    = m_cull;
    rs.frontFace   = m_front;
    rs.lineWidth   = m_lineWidth;
    if (m_biasOn)
    {
        rs.depthBiasEnable         = VK_TRUE;
        rs.depthBiasConstantFactor = m_biasConst;
        rs.depthBiasSlopeFactor    = m_biasSlope;
    }

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = m_depthTest  ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = m_depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp   = m_depthOp;

    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = m_nColor;
    cb.pAttachments    = m_nColor ? m_blends : nullptr;

    VkPipelineDynamicStateCreateInfo dynState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynState.dynamicStateCount = m_nDynamic;
    dynState.pDynamicStates    = m_dynamic;

    VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    prci.pNext                   = m_renderingNext;
    prci.colorAttachmentCount    = m_nColor;
    prci.pColorAttachmentFormats = m_nColor ? m_colorFormats : nullptr;
    prci.depthAttachmentFormat   = m_depthFormat;

    VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.pNext               = &prci;
    // Applied here rather than at every call site: r_pipe_stats is a whole-renderer
    // question ("what did the driver compile everything into"), and the bit does
    // nothing at all unless that cvar was on before this pipeline was built.
    pi.flags               = m_flags | PipeStats::CreateFlags();
    pi.stageCount          = m_nStages;
    pi.pStages             = m_stages;
    pi.pVertexInputState   = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pTessellationState  = m_tess ? &ts : nullptr;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = m_hasDepth ? &ds : nullptr;
    pi.pColorBlendState    = &cb;
    pi.pDynamicState       = &dynState;
    pi.layout              = m_layout;

    VkPipeline handle = VK_NULL_HANDLE;
    VkResult res;
    {
        LoadProf::Scope _lp("rt.pso_gfx"); // a pipeline made in the frame (an effect seen for the first time?)
        res = vkCreateGraphicsPipelines(VulkanHW.m_Device, PipelineCache::GetCacheObject(), 1, &pi, nullptr, &handle);
    }
    if (res != VK_SUCCESS)
    {
        char tag[256];
        va_list args;
        va_start(args, tagFmt);
        vsnprintf(tag, sizeof(tag), tagFmt, args);
        va_end(args);
        Msg("![VK] %s pipeline create failed (vk=%d)", tag, (int)res);
        return VK_NULL_HANDLE;
    }
    return handle;
}

// ============================================================================
// PipeStats — VK_KHR_pipeline_executable_properties (see the header for why).
// ============================================================================
namespace PipeStats
{

// Loaded once, lazily: the extension may not be enabled at all, and asking for a
// missing entry point every call would be the instrument doing more work than the
// thing it measures.
static PFN_vkGetPipelineExecutablePropertiesKHR pfnProps = nullptr;
static PFN_vkGetPipelineExecutableStatisticsKHR pfnStats = nullptr;
static bool s_loaded = false;

static bool Load()
{
    if (s_loaded) return pfnProps && pfnStats;
    s_loaded = true;
    if (!VulkanHW.m_bPipeStatsSupported) return false;
    pfnProps = (PFN_vkGetPipelineExecutablePropertiesKHR)
        vkGetDeviceProcAddr(VulkanHW.m_Device, "vkGetPipelineExecutablePropertiesKHR");
    pfnStats = (PFN_vkGetPipelineExecutableStatisticsKHR)
        vkGetDeviceProcAddr(VulkanHW.m_Device, "vkGetPipelineExecutableStatisticsKHR");
    return pfnProps && pfnStats;
}

VkPipelineCreateFlags CreateFlags()
{
    if (!VulkanHW.m_bPipeStatsSupported || ps_r_pipe_stats <= 0) return 0;
    return VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
}

void Dump(VkPipeline pipe, const char* nameFmt, ...)
{
    char name[256];
    {
        va_list args;
        va_start(args, nameFmt);
        vsnprintf(name, sizeof(name), nameFmt, args);
        va_end(args);
    }
    if (pipe == VK_NULL_HANDLE) { Msg("[VK PipeStats] %-34s not built", name); return; }
    if (!Load()) {
        Msg("![VK PipeStats] %s: VK_KHR_pipeline_executable_properties is not up on this device", name);
        return;
    }

    VkPipelineInfoKHR pi{};
    pi.sType    = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR;
    pi.pipeline = pipe;

    u32 nExec = 0;
    if (pfnProps(VulkanHW.m_Device, &pi, &nExec, nullptr) != VK_SUCCESS || nExec == 0) {
        // The usual cause is the honest one: this pipeline was created before
        // r_pipe_stats was set, so the driver kept nothing. Say that, because an
        // empty block would read as "no executables" rather than "you asked late".
        Msg("![VK PipeStats] %s: no executables — was it created with r_pipe_stats 1? (the cvar must be on BEFORE the pipeline is built)", name);
        return;
    }
    xr_vector<VkPipelineExecutablePropertiesKHR> execs(nExec);
    for (auto& e : execs) e.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    if (pfnProps(VulkanHW.m_Device, &pi, &nExec, execs.data()) != VK_SUCCESS) return;

    for (u32 i = 0; i < nExec; ++i)
    {
        VkPipelineExecutableInfoKHR ei{};
        ei.sType           = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR;
        ei.pipeline        = pipe;
        ei.executableIndex = i;

        u32 nStat = 0;
        if (pfnStats(VulkanHW.m_Device, &ei, &nStat, nullptr) != VK_SUCCESS || nStat == 0) {
            Msg("[VK PipeStats] %-34s %s: (no statistics)", name, execs[i].name);
            continue;
        }
        xr_vector<VkPipelineExecutableStatisticKHR> st(nStat);
        for (auto& s : st) s.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        if (pfnStats(VulkanHW.m_Device, &ei, &nStat, st.data()) != VK_SUCCESS) continue;

        // One line per executable with every statistic on it: the whole point is to
        // diff two pipelines by eye in a log, and a stat per line makes that a scroll.
        string4096 line;
        xr_sprintf(line, "[VK PipeStats] %-34s %-22s |", name, execs[i].name);
        for (u32 s = 0; s < nStat; ++s) {
            string256 one;
            switch (st[s].format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                xr_sprintf(one, " %s=%s", st[s].name, st[s].value.b32 ? "yes" : "no"); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                xr_sprintf(one, " %s=%lld", st[s].name, (long long)st[s].value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                xr_sprintf(one, " %s=%llu", st[s].name, (unsigned long long)st[s].value.u64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
                xr_sprintf(one, " %s=%.3f", st[s].name, st[s].value.f64); break;
            default:
                xr_sprintf(one, " %s=?", st[s].name); break;
            }
            if (xr_strlen(line) + xr_strlen(one) + 4 >= sizeof(line)) { xr_strcat(line, " ..."); break; }
            xr_strcat(line, one);
        }
        Msg("%s", line);
    }
}

} // namespace PipeStats

} // namespace VK

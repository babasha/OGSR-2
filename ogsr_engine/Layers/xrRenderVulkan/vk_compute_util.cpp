// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "../../xrCore/load_prof.h" // [rt prof]: named parts of a slow render-thread job
#include "vk_compute_util.h"
#include "vk_pipeline_cache.h"   // VK::PipelineCache::GetCacheObject
#include "vk_shaders.h"          // g_ShaderManager (SPIR-V loader)
#include "HW_Vulkan.h"           // VulkanHW (device)
#include "vk_gfx_pipeline.h"     // nfs37: VK::PipeStats::CreateFlags

namespace VK
{

VkPipelineLayout MakePipelineLayout(std::initializer_list<VkDescriptorSetLayout> setLayouts,
                                    u32 pushSize, VkShaderStageFlags pushStages)
{
    VkPushConstantRange pcr{ pushStages, 0, pushSize };

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = (u32)setLayouts.size();
    plci.pSetLayouts    = setLayouts.begin();   // initializer_list storage is contiguous
    if (pushSize > 0)
    {
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges    = &pcr;
    }

    VkPipelineLayout layout = VK_NULL_HANDLE;
    const VkResult res = vkCreatePipelineLayout(VulkanHW.m_Device, &plci, nullptr, &layout);
    if (res != VK_SUCCESS)
    {
        Msg("![VK] pipeline layout create failed (vk=%d)", (int)res);
        return VK_NULL_HANDLE;
    }
    return layout;
}

VkPipeline CreateComputePipeline(VkShaderModule cs, VkPipelineLayout layout, const char* tag)
{
    VkComputePipelineCreateInfo cp{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cp.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = cs;
    cp.stage.pName  = "main";
    cp.layout       = layout;
    cp.flags        = PipeStats::CreateFlags();   // nfs37: compute pipelines report to vk_pipestats too (0 unless r_pipe_stats)

    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult res;
    {
        LoadProf::Scope _lp("rt.pso_comp");
        res = vkCreateComputePipelines(VulkanHW.m_Device, VK::PipelineCache::GetCacheObject(), 1, &cp, nullptr, &pipe);
    }
    if (res != VK_SUCCESS)
    {
        Msg("![VK] compute pipeline '%s' create failed (vk=%d)", tag ? tag : "?", (int)res);
        return VK_NULL_HANDLE;
    }
    return pipe;
}

VkPipeline CreateComputePipeline(const char* spv, VkPipelineLayout layout, const char* tag)
{
    if (!tag) tag = spv;

    VkShaderModule cs = g_ShaderManager ? g_ShaderManager->Load(spv) : VK_NULL_HANDLE;
    if (cs == VK_NULL_HANDLE)
    {
        Msg("![VK] compute shader '%s' load failed (pipeline '%s')", spv, tag);
        return VK_NULL_HANDLE;
    }
    return CreateComputePipeline(cs, layout, tag);
}

// ============================================================================
// ComputeProgram — see vk_compute_util.h for why the quartet is one object.
// Each step logs through the helper it wraps, so a failure names the same tag it
// always did; the step itself only reports pass/fail.
// ============================================================================

bool ComputeProgram::Descriptors(std::initializer_list<VkDescriptorType> types, u32 poolSets,
                                 VkShaderStageFlags stages, const char* tag)
{
    setLayout = MakeSetLayout(types, stages, tag);
    if (setLayout == VK_NULL_HANDLE) return false;
    pool = MakeDescriptorPool(types, poolSets, tag);
    return pool != VK_NULL_HANDLE;
}

bool ComputeProgram::Alloc(u32 count, VkDescriptorSet* outSets, const char* tag) const
{
    if (pool == VK_NULL_HANDLE || setLayout == VK_NULL_HANDLE) return false;
    return AllocSets(pool, setLayout, count, outSets, tag);
}

bool ComputeProgram::Sets(std::initializer_list<VkDescriptorType> types, u32 count,
                          VkDescriptorSet* outSets, VkShaderStageFlags stages, const char* tag)
{
    return Descriptors(types, count, stages, tag) && Alloc(count, outSets, tag);
}

bool ComputeProgram::Layout(u32 pushSize, VkShaderStageFlags pushStages)
{
    if (setLayout == VK_NULL_HANDLE) return false;
    layout = MakePipelineLayout({ setLayout }, pushSize, pushStages);
    return layout != VK_NULL_HANDLE;
}

bool ComputeProgram::Compute(const char* spv, const char* tag)
{
    if (layout == VK_NULL_HANDLE) return false;
    pipeline = CreateComputePipeline(spv, layout, tag);
    return pipeline != VK_NULL_HANDLE;
}

bool ComputeProgram::Create(const char* spv, std::initializer_list<VkDescriptorType> types, u32 count,
                            VkDescriptorSet* outSets, u32 pushSize, const char* tag,
                            VkShaderStageFlags stages)
{
    // Push constants follow the SET stages: a compute program pushes to COMPUTE, a
    // vertex-stage one to VERTEX. Passes that push across two stages build the
    // layout themselves (see the header's third example).
    return Sets(types, count, outSets, stages, tag)
        && Layout(pushSize, stages)
        && Compute(spv, tag);
}

void ComputeProgram::Destroy()
{
    // Device gone (shutdown ordering) — the objects went with it; just forget them.
    if (VulkanHW.m_Device == VK_NULL_HANDLE)
    {
        pipeline = VK_NULL_HANDLE; layout = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE; setLayout = VK_NULL_HANDLE;
        return;
    }
    // Reverse creation order: pipeline, its layout, the pool (which frees every set
    // allocated from it), then the set layout.
    if (pipeline)  { vkDestroyPipeline(VulkanHW.m_Device, pipeline, nullptr);                 pipeline  = VK_NULL_HANDLE; }
    if (layout)    { vkDestroyPipelineLayout(VulkanHW.m_Device, layout, nullptr);             layout    = VK_NULL_HANDLE; }
    if (pool)      { vkDestroyDescriptorPool(VulkanHW.m_Device, pool, nullptr);               pool      = VK_NULL_HANDLE; }
    if (setLayout) { vkDestroyDescriptorSetLayout(VulkanHW.m_Device, setLayout, nullptr);     setLayout = VK_NULL_HANDLE; }
}

} // namespace VK

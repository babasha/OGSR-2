// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once

#include "vk_core.h"
#include "vk_descriptors.h"   // ComputeProgram wraps MakeSetLayout/MakeDescriptorPool/AllocSets
#include <initializer_list>

namespace VK
{

// ============================================================================
// Compute pipeline helpers
//
// Collapses the two byte-identical tails every compute pass hand-rolls:
//   * VkPipelineLayoutCreateInfo (+ optional push-constant range) -> vkCreatePipelineLayout
//   * VkComputePipelineCreateInfo (single COMPUTE stage, "main") -> vkCreateComputePipelines
// The pipeline is always built against the shared PipelineCache (some sites used
// VK_NULL_HANDLE before — routing them through the cache is identical output, just
// warm-startable). Both return VK_NULL_HANDLE on failure and log a tagged error.
// ============================================================================

// Create a pipeline layout from a set of descriptor-set layouts and an optional
// push-constant block. pushSize == 0 means no push-constant range.
VkPipelineLayout MakePipelineLayout(std::initializer_list<VkDescriptorSetLayout> setLayouts,
                                    u32 pushSize = 0,
                                    VkShaderStageFlags pushStages = VK_SHADER_STAGE_COMPUTE_BIT);

// Create a compute pipeline from a shader module + layout (entry point "main").
// `tag` names the pipeline in the failure log. Does NOT take ownership of `cs`.
VkPipeline CreateComputePipeline(VkShaderModule cs, VkPipelineLayout layout, const char* tag = "compute");

// Same, but loads the module by SPIR-V name through g_ShaderManager first — the
// load+create pair every pass repeated by hand. `tag` defaults to the shader name.
VkPipeline CreateComputePipeline(const char* spv, VkPipelineLayout layout, const char* tag = nullptr);



// ============================================================================
// ComputeProgram — the four handles a pass owns, created and destroyed together
//
// Every compute pass in this renderer keeps the same quartet: a set layout, the
// pool its sets came from, a pipeline layout, and the pipeline. The helpers above
// collapsed each CREATE step, but the quartet itself is still spelled out at BOTH
// ends — a ~6-line creator and, far away in Destroy(), four
// `if (h) { vkDestroy...; h = VK_NULL_HANDLE; }` lines naming the same four handles.
// vk_vsm.cpp alone carried 13 such teardown groups (57 of its Destroy()'s 112 lines).
//
// The hazard is the teardown half: a pass that gains a handle and forgets one line
// in Destroy() leaks it silently, and the leak only shows on device shutdown or a
// renderer restart. Bundling makes the four handles ONE object — a pass can no
// longer be half-freed.
//
// LAYERED on purpose, because the passes are not uniform: some allocate several set
// GROUPS out of one pool (a dyn and a static variant), some build their pipeline with
// GfxPipelineBuilder instead, some also bind a set layout they do not own. So:
//
//   p.Create(spv, types, N, sets, sizeof(Push), "Tag");   // the plain compute pass
//
//   p.Descriptors(types, N * 2, stages, "Tag");           // layout + pool only...
//   p.Alloc(N, setsDyn,    "Tag.dyn");                    // ...then several groups
//   p.Alloc(N, setsStatic, "Tag.static");
//   p.Layout(sizeof(Push)); p.Compute(spv, "Tag");
//
//   p.Sets(types, N, sets, VK_SHADER_STAGE_VERTEX_BIT, "Tag");   // graphics pass
//   p.layout   = VK::MakePipelineLayout({ p.setLayout, borrowedL }, sizeof(Push), ...);
//   p.pipeline = VK::GfxPipelineBuilder(p.layout)....Build("Tag");
//
// Every step is independently optional; Destroy() frees whatever is set. Handles stay
// PUBLIC — the point is joint lifetime, not encapsulation, and vkCmdBind* needs them
// raw anyway. Destroy() does NOT clear caller-held VkDescriptorSet arrays: those die
// with the pool, and the caller nulls its own copies exactly as it did before.
// ============================================================================
struct ComputeProgram
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool      pool      = VK_NULL_HANDLE;
    VkPipelineLayout      layout    = VK_NULL_HANDLE;
    VkPipeline            pipeline  = VK_NULL_HANDLE;

    // Set layout (positional bindings, see MakeSetLayout) + a pool sized for
    // `poolSets` copies of it. Allocate the sets with Alloc() below.
    bool Descriptors(std::initializer_list<VkDescriptorType> types, u32 poolSets,
                     VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT,
                     const char* tag = "program");

    // Allocate ONE GROUP of `count` sets of this program's layout out of its pool.
    // Callable more than once as long as the pool was sized for the total.
    bool Alloc(u32 count, VkDescriptorSet* outSets, const char* tag = "program") const;

    // Descriptors() + a single Alloc() of the same count — the usual case.
    bool Sets(std::initializer_list<VkDescriptorType> types, u32 count, VkDescriptorSet* outSets,
              VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT,
              const char* tag = "program");

    // Pipeline layout over this program's OWN set layout (+ optional push block).
    // A pass that also binds a borrowed set layout assigns `layout` directly instead.
    bool Layout(u32 pushSize = 0, VkShaderStageFlags pushStages = VK_SHADER_STAGE_COMPUTE_BIT);

    // Compute pipeline from `spv` against `layout` (which must already exist).
    bool Compute(const char* spv, const char* tag = nullptr);

    // Sets() + Layout() + Compute() — a whole plain compute pass in one call.
    bool Create(const char* spv, std::initializer_list<VkDescriptorType> types, u32 count,
                VkDescriptorSet* outSets, u32 pushSize = 0, const char* tag = "program",
                VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT);

    // Frees whichever handles are set and nulls them. Safe on a partially built
    // program (a failed Create) and safe to call twice.
    void Destroy();
};

} // namespace VK

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// CFrameGraph — lightweight render-graph for automatic barrier insertion.
// Step A1: infrastructure only, not yet wired into the render loop.
// Step A2 will import all RTs and replace scattered manual barriers.

#pragma once
#include "vk_core.h"
#include <functional>
#include <initializer_list>

namespace VK
{

// ---------------------------------------------------------------------------
// RGHandle — 4-byte typed handle (u16 index + u16 version for stale detection)
// ---------------------------------------------------------------------------
struct RGHandle
{
    u16 index   = 0xFFFF;   // 0xFFFF = invalid
    u16 version = 0;

    bool IsValid() const { return index != 0xFFFF; }
    static RGHandle Invalid() { return {}; }

    bool operator==(const RGHandle& o) const { return index == o.index && version == o.version; }
    bool operator!=(const RGHandle& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// RGUsage — how a pass uses a resource → maps to VkImageLayout + stage/access
// ---------------------------------------------------------------------------
enum class RGUsage : u8
{
    // Images
    COLOR_ATTACHMENT,       // COLOR_ATTACHMENT_OPTIMAL
    DEPTH_ATTACHMENT,       // DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    DEPTH_READ_ONLY,        // DEPTH_STENCIL_READ_ONLY_OPTIMAL
    SHADER_READ,            // SHADER_READ_ONLY_OPTIMAL (fragment)
    SHADER_READ_COMPUTE,    // SHADER_READ_ONLY_OPTIMAL (compute)
    STORAGE_IMAGE,          // GENERAL (compute read+write)
    STORAGE_IMAGE_READ,     // GENERAL (compute read-only)
    TRANSFER_SRC,
    TRANSFER_DST,
    PRESENT,
    // Buffers (layout field unused)
    UNIFORM_BUFFER,
    STORAGE_BUFFER_READ,
    STORAGE_BUFFER_WRITE,
    STORAGE_BUFFER_RW,
    // Buffer-specific read usages (no layout transition)
    INDIRECT_BUFFER,    // VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT + INDIRECT_COMMAND_READ
    VERTEX_BUFFER,      // VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT + VERTEX_ATTRIBUTE_READ
};

// ---------------------------------------------------------------------------
// RGResourceState — GPU resource state tracked per-resource
// ---------------------------------------------------------------------------
struct RGResourceState
{
    VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2        access = 0;
};

// ---------------------------------------------------------------------------
// RGResource — one slot in the resource table
// ---------------------------------------------------------------------------
struct RGResource
{
    union
    {
        VkImage  image  = VK_NULL_HANDLE;
        VkBuffer buffer;
    };
    bool               isBuffer   = false;
    VkImageAspectFlags aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
    u32                layerCount = 1;
    u32                mipLevels  = 1;
    u16                version    = 0;
    RGResourceState    state;
    const char*        name       = nullptr;
};

// ---------------------------------------------------------------------------
// RGPassUse — (resource, usage) pair declared by a pass
// ---------------------------------------------------------------------------
struct RGPassUse
{
    RGHandle handle;
    RGUsage  usage = RGUsage::SHADER_READ;
};

inline RGPassUse Use(RGHandle h, RGUsage u) { return { h, u }; }

// ---------------------------------------------------------------------------
// RGPassType
// ---------------------------------------------------------------------------
enum class RGPassType : u8
{
    GRAPHICS,   // auto-barrier before callback
    COMPUTE,    // auto-barrier before callback
    TRANSFER,   // auto-barrier before callback
    EXTERNAL,   // callback manages own barriers (shadow pass, grass, etc.)
                // NOTE: tracked state is NOT updated for EXTERNAL passes — the graph
                // has no way of knowing what layout/stage the resources end up in.
                // If an EXTERNAL pass is followed by non-EXTERNAL passes in the same
                // mini-graph that reference the same resources, call SetImageState()
                // or SetBufferState() after Execute() to fix up tracked state.
                // Current convention: EXTERNAL passes are always last in their
                // mini-graph batch, or all passes in the batch are EXTERNAL.
};

// ---------------------------------------------------------------------------
// RGPass — fixed-size pass descriptor (no heap allocation)
// ---------------------------------------------------------------------------
struct RGPass
{
    const char*   name       = nullptr;
    RGPassType    type       = RGPassType::GRAPHICS;
    bool          enabled    = true;
    RGPassUse     reads[8]   = {};  u8 readCount  = 0;
    RGPassUse     writes[8]  = {};  u8 writeCount = 0;
    std::function<void(VkCommandBuffer)> callback;
};

// ---------------------------------------------------------------------------
// CFrameGraph
// ---------------------------------------------------------------------------
class CFrameGraph
{
    static constexpr u32 MAX_RESOURCES = 64;
    static constexpr u32 MAX_PASSES    = 32;

    RGResource m_Resources[MAX_RESOURCES] = {};
    RGPass     m_Passes[MAX_PASSES]       = {};
    u32        m_ResourceCount = 0;
    u32        m_PassCount     = 0;

    void InsertBarriers(VkCommandBuffer cmd, RGPass& pass);

public:
    // Import an externally-managed image (render target, swapchain image, etc.)
    // initialLayout: current image layout before the first use in this frame
    RGHandle ImportImage(const char* name, VkImage image, VkImageLayout initialLayout,
                         VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                         u32 layerCount = 1, u32 mipLevels = 1);

    // Import an externally-managed buffer (UBO, SSBO, etc.)
    RGHandle ImportBuffer(const char* name, VkBuffer buffer,
                          VkPipelineStageFlags2 initialStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                          VkAccessFlags2 initialAccess = 0);

    // Add a render pass with explicit read/write resource lists.
    // Returns a handle to the pass (for future dependency tracking in A2+).
    RGHandle AddPass(const char* name, RGPassType type,
                     std::initializer_list<RGPassUse> reads,
                     std::initializer_list<RGPassUse> writes,
                     std::function<void(VkCommandBuffer)> cb);

    // Conditional variant — pass is registered but skipped during Execute if !enabled.
    RGHandle AddPassConditional(bool enabled, const char* name, RGPassType type,
                                std::initializer_list<RGPassUse> reads,
                                std::initializer_list<RGPassUse> writes,
                                std::function<void(VkCommandBuffer)> cb);

    // Compile() validates the graph (no-op for A1 — reserved for culling/reordering in A2+).
    void Compile();

    // Execute() walks passes in declaration order, inserts batched barriers, calls callbacks.
    void Execute(VkCommandBuffer cmd);

    // Reset() — clear passes, keep resource states (call each frame before re-recording).
    // Resource states are preserved so first-barrier of the next frame is correct.
    void Reset();

    // FullReset() — clear everything (call on resize or device recreate).
    void FullReset();

    // Override the tracked layout of a resource after an external operation.
    void SetImageLayout(RGHandle h, VkImageLayout layout);

    // Override the full tracked state (layout + stage + access) after an external operation.
    // Use when importing a resource whose writes came from a known pipeline stage
    // (e.g. rt_HDR written by COLOR_ATTACHMENT_OUTPUT when exposure pass didn't run).
    void SetImageState(RGHandle h, VkImageLayout layout,
                       VkPipelineStageFlags2 stage, VkAccessFlags2 access);

    // Override the tracked stage+access of a buffer after an external operation.
    // Use when an EXTERNAL pass (or manual barrier code) leaves a buffer in a known state.
    void SetBufferState(RGHandle h, VkPipelineStageFlags2 stage, VkAccessFlags2 access);

    // Find a previously imported resource by name (O(n), 64-slot table).
    RGHandle FindResource(const char* name) const;

    // Query the current tracked layout of an image resource by name.
    // Returns VK_IMAGE_LAYOUT_UNDEFINED when the resource has not been imported.
    VkImageLayout GetTrackedLayout(const char* name) const;

    u32 GetPassCount()     const { return m_PassCount; }
    u32 GetResourceCount() const { return m_ResourceCount; }
};

} // namespace VK

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - One detail-object type (grass / debris / small static).
//
// Mirrors monolith's CDetail. Stores a single mesh (VB + IB) plus the
// per-type render parameters (scale range, no-waving flag, bv volumes,
// texture/shader names). Many instances of one CDetail are spawned per
// level by the GPU-driven generator; this class only owns the geometry +
// metadata, never per-instance state.
//
// Texture resolution is deferred to Session B (when the render pipeline
// exists). Loader reads the texture name string but doesn't materialise
// the DDS yet.

#pragma once
#include "vk_core.h"
#include "vk_buffer.h"
#include "../xrRender/DetailFormat.h"

namespace VK
{

class CDetail
{
public:
    // Vertex layout fed into the grass VS (binding 0). Pos + UV +
    // normalised Y (used as wind-bend weight in detail_vs.glsl).
    struct Vertex
    {
        Fvector  pos;       // local-space position
        Fvector2 uv;        // texcoord
        float    height;    // (P.y - bb.min.y) / (bb.size.y), clamp 0..1
    };
    static_assert(sizeof(Vertex) == 24, "Detail vertex must be 24 bytes (matches detail_vs.glsl binding 0)");

public:
    // Geometry — uploaded device-local at Load() time, immutable afterwards.
    VK::CVulkanBuffer* m_VertexBuffer = nullptr;
    VK::CVulkanBuffer* m_IndexBuffer  = nullptr;
    u32                m_VertexCount  = 0;
    u32                m_IndexCount   = 0;
    u32                m_VertexStride = sizeof(Vertex);

    // Render params (from level.details).
    u32        m_Flags     = 0;     // DO_NO_WAVING etc. (DetailFormat.h)
    float      m_MinScale  = 0.5f;
    float      m_MaxScale  = 1.5f;

    // Bounding volumes — bv_sphere.R is read by GpuDetailObjInfo for
    // per-instance frustum culling on the compute side.
    Fsphere    bv_sphere{};
    Fbox       bv_bb{};

    // Texture / shader names parsed from the file. Texture loading is
    // deferred — Session B's render pipeline binds the DDS at draw time.
    shared_str m_TextureName;
    shared_str m_ShaderName;

public:
    CDetail();
    ~CDetail();

    void Load(IReader* S);
    void Unload();
};

}  // namespace VK

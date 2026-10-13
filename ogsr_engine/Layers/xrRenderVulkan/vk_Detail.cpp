// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - CDetail implementation. See vk_Detail.h for scope notes.
//
// Texture loading is intentionally deferred: at this stage we only need the
// geometry on the GPU and the metadata (texture name, scales, flags). The
// renderer in Session B looks up textures by name when binding descriptor
// sets per-type.

#include "stdafx.h"
#include "vk_Detail.h"

namespace VK
{

CDetail::CDetail() = default;

CDetail::~CDetail()
{
    Unload();
}

// File layout (matches monolith vk_Detail::Load and DX11 CDetail::Load):
//   Z-string  shaderName
//   Z-string  textureName
//   u32       flags
//   float     minScale
//   float     maxScale
//   u32       vCount
//   u32       iCount
//   { Fvector P; float u, v; }[vCount]   <-- 20 B fvfVertexIn (UV is FLOAT, not SHORT)
//   u16[iCount]
void CDetail::Load(IReader* S)
{
    string256 fnS, fnT;
    S->r_stringZ(fnS, sizeof(fnS));
    S->r_stringZ(fnT, sizeof(fnT));
    m_ShaderName  = fnS;
    m_TextureName = fnT;

    m_Flags    = S->r_u32();
    m_MinScale = S->r_float();
    m_MaxScale = S->r_float();

    const u32 vCount = S->r_u32();
    const u32 iCount = S->r_u32();
    m_VertexCount = vCount;
    m_IndexCount  = iCount;
    R_ASSERT(0 == (iCount % 3));

    if (vCount == 0) {
        Msg("![VK Detail] '%s' has 0 vertices", m_TextureName.c_str());
        return;
    }

    // Read raw fvfVertexIn (same struct as the file format).
    struct fvfVertexIn { Fvector P; float u, v; };
    static_assert(sizeof(fvfVertexIn) == 20, "fvfVertexIn must be 20 B");
    xr_vector<fvfVertexIn> rawVerts(vCount);
    S->r(rawVerts.data(), vCount * sizeof(fvfVertexIn));

    xr_vector<u16> indices(iCount);
    S->r(indices.data(), iCount * sizeof(u16));

    // Bounding volumes from raw positions.
    bv_bb.invalidate();
    for (u32 i = 0; i < vCount; ++i)
        bv_bb.modify(rawVerts[i].P);
    bv_bb.getsphere(bv_sphere.P, bv_sphere.R);

    // Convert to 24-byte VB layout: pos + uv + normalised height.
    xr_vector<Vertex> vertices(vCount);
    const float minY   = bv_bb.min.y;
    const float rangeY = bv_bb.max.y - minY;
    const float invY   = (rangeY > 0.01f) ? (1.0f / rangeY) : 0.0f;
    for (u32 i = 0; i < vCount; ++i) {
        vertices[i].pos    = rawVerts[i].P;
        vertices[i].uv.x   = rawVerts[i].u;
        vertices[i].uv.y   = rawVerts[i].v;
        vertices[i].height = (rawVerts[i].P.y - minY) * invY;  // 0=root, 1=tip
    }

    if (iCount == 0) {
        Msg("![VK Detail] '%s' has 0 indices", m_TextureName.c_str());
        return;
    }

    // Upload device-local. TRANSFER_SRC is requested in case Session C's
    // bindless path concatenates VBs via vkCmdCopyBuffer.
    const u32 vbSize = vCount * sizeof(Vertex);
    m_VertexBuffer = xr_new<VK::CVulkanBuffer>();
    m_VertexBuffer->Create(
        vbSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT  |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_VertexBuffer->Upload(vertices.data(), vbSize);

    const u32 ibSize = iCount * sizeof(u16);
    m_IndexBuffer = xr_new<VK::CVulkanBuffer>();
    m_IndexBuffer->Create(
        ibSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    m_IndexBuffer->Upload(indices.data(), ibSize);

    Msg("[VK Detail] '%s' v=%u i=%u scale=%.2f..%.2f bv_r=%.2f",
        m_TextureName.c_str(), vCount, iCount, m_MinScale, m_MaxScale, bv_sphere.R);
}

void CDetail::Unload()
{
    if (m_VertexBuffer) { m_VertexBuffer->Destroy(); xr_delete(m_VertexBuffer); }
    if (m_IndexBuffer)  { m_IndexBuffer->Destroy();  xr_delete(m_IndexBuffer);  }
    m_VertexCount = m_IndexCount = 0;
}

}  // namespace VK

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
// ============================================================================
// Vertex-declaration helpers for the Vulkan renderer.
//
// These functions work with D3DVERTEXELEMENT9 structures to parse level files.
// The types come from Layers/xrRender/xrVertexDeclTypes.h (vendored via the PCH)
// — the engine no longer includes any Direct3D header.
//
// Based on stalker-cordisproject/src/Common/PlatformLinux.inl
// ============================================================================

#ifndef VK_D3D_COMPAT_H
#define VK_D3D_COMPAT_H

// ============================================================================
// Helper Functions for Vertex Declarations
// ============================================================================

// Get size in bytes for a D3D declaration type
inline u32 VK_GetDeclTypeSize(BYTE type)
{
    switch (type)
    {
    case D3DDECLTYPE_FLOAT1:    return 4;
    case D3DDECLTYPE_FLOAT2:    return 8;
    case D3DDECLTYPE_FLOAT3:    return 12;
    case D3DDECLTYPE_FLOAT4:    return 16;
    case D3DDECLTYPE_D3DCOLOR:  return 4;
    case D3DDECLTYPE_UBYTE4:    return 4;
    case D3DDECLTYPE_SHORT2:    return 4;
    case D3DDECLTYPE_SHORT4:    return 8;
    case D3DDECLTYPE_UBYTE4N:   return 4;
    case D3DDECLTYPE_SHORT2N:   return 4;
    case D3DDECLTYPE_SHORT4N:   return 8;
    case D3DDECLTYPE_USHORT2N:  return 4;
    case D3DDECLTYPE_USHORT4N:  return 8;
    case D3DDECLTYPE_UDEC3:     return 4;
    case D3DDECLTYPE_DEC3N:     return 4;
    case D3DDECLTYPE_FLOAT16_2: return 4;
    case D3DDECLTYPE_FLOAT16_4: return 8;
    default:                    return 0;
    }
}

// Count elements in vertex declaration (excluding END marker)
inline u32 VK_GetDeclLength(const D3DVERTEXELEMENT9* dcl)
{
    u32 count = 0;
    while (dcl[count].Stream != 0xFF)  // D3DDECL_END() sets Stream to 0xFF
        count++;
    return count;
}

// Calculate vertex size from declaration for a specific stream
inline u32 VK_GetDeclVertexSize(const D3DVERTEXELEMENT9* dcl, UINT stream = 0)
{
    u32 size = 0;
    for (u32 i = 0; dcl[i].Stream != 0xFF; i++)
    {
        if (dcl[i].Stream == stream)
        {
            u32 offset = dcl[i].Offset + VK_GetDeclTypeSize(dcl[i].Type);
            if (offset > size)
                size = offset;
        }
    }
    return size;
}

// ============================================================================
// FVF (Flexible Vertex Format) Helpers
// ============================================================================

// Compute vertex stride from D3D FVF flags (mirrors D3DXGetFVFVertexSize)
inline u32 VK_GetFVFVertexSize(u32 fvf)
{
    u32 size = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ:    size += 12; break;
    case D3DFVF_XYZRHW: size += 16; break;
    case D3DFVF_XYZB1:  size += 16; break;
    case D3DFVF_XYZB2:  size += 20; break;
    case D3DFVF_XYZB3:  size += 24; break;
    case D3DFVF_XYZB4:  size += 28; break;
    case D3DFVF_XYZB5:  size += 32; break;
    case D3DFVF_XYZW:   size += 16; break;
    }
    if (fvf & D3DFVF_NORMAL)   size += 12;
    if (fvf & D3DFVF_PSIZE)    size += 4;
    if (fvf & D3DFVF_DIFFUSE)  size += 4;
    if (fvf & D3DFVF_SPECULAR) size += 4;
    u32 texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    size += texCount * 8;  // each tex coord set = FLOAT2
    return size;
}

// ============================================================================
// Vulkan Format Conversion
// ============================================================================

// Convert D3D declaration type to Vulkan format
inline VkFormat VK_DeclTypeToVkFormat(BYTE type)
{
    switch (type)
    {
    case D3DDECLTYPE_FLOAT1:    return VK_FORMAT_R32_SFLOAT;
    case D3DDECLTYPE_FLOAT2:    return VK_FORMAT_R32G32_SFLOAT;
    case D3DDECLTYPE_FLOAT3:    return VK_FORMAT_R32G32B32_SFLOAT;
    case D3DDECLTYPE_FLOAT4:    return VK_FORMAT_R32G32B32A32_SFLOAT;
    case D3DDECLTYPE_D3DCOLOR:  return VK_FORMAT_B8G8R8A8_UNORM;
    case D3DDECLTYPE_UBYTE4:    return VK_FORMAT_R8G8B8A8_UINT;
    case D3DDECLTYPE_SHORT2:    return VK_FORMAT_R16G16_SINT;
    case D3DDECLTYPE_SHORT4:    return VK_FORMAT_R16G16B16A16_SINT;
    case D3DDECLTYPE_UBYTE4N:   return VK_FORMAT_R8G8B8A8_UNORM;
    case D3DDECLTYPE_SHORT2N:   return VK_FORMAT_R16G16_SNORM;
    case D3DDECLTYPE_SHORT4N:   return VK_FORMAT_R16G16B16A16_SNORM;
    case D3DDECLTYPE_USHORT2N:  return VK_FORMAT_R16G16_UNORM;
    case D3DDECLTYPE_USHORT4N:  return VK_FORMAT_R16G16B16A16_UNORM;
    case D3DDECLTYPE_UDEC3:     return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case D3DDECLTYPE_DEC3N:     return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case D3DDECLTYPE_FLOAT16_2: return VK_FORMAT_R16G16_SFLOAT;
    case D3DDECLTYPE_FLOAT16_4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:                    return VK_FORMAT_UNDEFINED;
    }
}

// ============================================================================
// Vertex Declarator (svector-based container)
// ============================================================================
typedef svector<D3DVERTEXELEMENT9, MAXD3DDECLLENGTH + 1> VertexDeclarator;

#endif // VK_D3D_COMPAT_H

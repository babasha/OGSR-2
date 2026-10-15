// Vertex-declaration and FVF constants as stored in X-Ray's level and model
// files (.ogf / level.geom / *.dm). They were originally Direct3D 9 enums, so
// the on-disk data speaks D3D9 numbering and the names are kept identical to
// keep the loaders readable against the format docs.
//
// This header exists so the engine no longer includes <d3d9types.h>: the values
// below are file-format constants, not a graphics API dependency. Values are
// verbatim from the Windows SDK d3d9types.h and must not be renumbered — the
// asset files would stop parsing.

#ifndef xrVertexDeclTypes_included
#define xrVertexDeclTypes_included
#pragma once

#ifdef _d3d9TYPES_H_
#error "<d3d9types.h> was included before xrVertexDeclTypes.h - the engine must not pull in the D3D9 headers"
#endif

//////////////////////////////////////////////////////////////////////////
// Vertex element type / method / usage
//////////////////////////////////////////////////////////////////////////
typedef enum _D3DDECLTYPE
{
    D3DDECLTYPE_FLOAT1 = 0,
    D3DDECLTYPE_FLOAT2 = 1,
    D3DDECLTYPE_FLOAT3 = 2,
    D3DDECLTYPE_FLOAT4 = 3,
    D3DDECLTYPE_D3DCOLOR = 4,
    D3DDECLTYPE_UBYTE4 = 5,
    D3DDECLTYPE_SHORT2 = 6,
    D3DDECLTYPE_SHORT4 = 7,
    D3DDECLTYPE_UBYTE4N = 8,
    D3DDECLTYPE_SHORT2N = 9,
    D3DDECLTYPE_SHORT4N = 10,
    D3DDECLTYPE_USHORT2N = 11,
    D3DDECLTYPE_USHORT4N = 12,
    D3DDECLTYPE_UDEC3 = 13,
    D3DDECLTYPE_DEC3N = 14,
    D3DDECLTYPE_FLOAT16_2 = 15,
    D3DDECLTYPE_FLOAT16_4 = 16,
    D3DDECLTYPE_UNUSED = 17,
} D3DDECLTYPE;

typedef enum _D3DDECLMETHOD
{
    D3DDECLMETHOD_DEFAULT = 0,
    D3DDECLMETHOD_PARTIALU = 1,
    D3DDECLMETHOD_PARTIALV = 2,
    D3DDECLMETHOD_CROSSUV = 3,
    D3DDECLMETHOD_UV = 4,
    D3DDECLMETHOD_LOOKUP = 5,
    D3DDECLMETHOD_LOOKUPPRESAMPLED = 6,
} D3DDECLMETHOD;

typedef enum _D3DDECLUSAGE
{
    D3DDECLUSAGE_POSITION = 0,
    D3DDECLUSAGE_BLENDWEIGHT = 1,
    D3DDECLUSAGE_BLENDINDICES = 2,
    D3DDECLUSAGE_NORMAL = 3,
    D3DDECLUSAGE_PSIZE = 4,
    D3DDECLUSAGE_TEXCOORD = 5,
    D3DDECLUSAGE_TANGENT = 6,
    D3DDECLUSAGE_BINORMAL = 7,
    D3DDECLUSAGE_TESSFACTOR = 8,
    D3DDECLUSAGE_POSITIONT = 9,
    D3DDECLUSAGE_COLOR = 10,
    D3DDECLUSAGE_FOG = 11,
    D3DDECLUSAGE_DEPTH = 12,
    D3DDECLUSAGE_SAMPLE = 13,
} D3DDECLUSAGE;

typedef struct _D3DVERTEXELEMENT9
{
    WORD Stream;
    WORD Offset;
    BYTE Type;
    BYTE Method;
    BYTE Usage;
    BYTE UsageIndex;
} D3DVERTEXELEMENT9, *LPD3DVERTEXELEMENT9;

#define MAXD3DDECLLENGTH 64 // does not include the "end" marker element
#define D3DDECL_END() {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0}

//////////////////////////////////////////////////////////////////////////
// Primitive topology. The shared draw calls (CBackend::Render, dbg_Draw) still
// take it in this spelling; the Vulkan backend maps it to VkPrimitiveTopology.
//////////////////////////////////////////////////////////////////////////
typedef enum _D3DPRIMITIVETYPE
{
    D3DPT_POINTLIST = 1,
    D3DPT_LINELIST = 2,
    D3DPT_LINESTRIP = 3,
    D3DPT_TRIANGLELIST = 4,
    D3DPT_TRIANGLESTRIP = 5,
    D3DPT_TRIANGLEFAN = 6,
    D3DPT_FORCE_DWORD = 0x7fffffff,
} D3DPRIMITIVETYPE;

//////////////////////////////////////////////////////////////////////////
// Sampler-slot numbering used by SH_Texture.h to tag vertex-stage samplers.
//////////////////////////////////////////////////////////////////////////
#define D3DDMAPSAMPLER 256
#define D3DVERTEXTEXTURESAMPLER0 (D3DDMAPSAMPLER + 1)
#define D3DVERTEXTEXTURESAMPLER1 (D3DDMAPSAMPLER + 2)
#define D3DVERTEXTEXTURESAMPLER2 (D3DDMAPSAMPLER + 3)
#define D3DVERTEXTEXTURESAMPLER3 (D3DDMAPSAMPLER + 4)

//////////////////////////////////////////////////////////////////////////
// Fixed vertex format bits
//////////////////////////////////////////////////////////////////////////
#define D3DFVF_POSITION_MASK 0x400E
#define D3DFVF_XYZ 0x002
#define D3DFVF_XYZRHW 0x004
#define D3DFVF_XYZB1 0x006
#define D3DFVF_XYZB2 0x008
#define D3DFVF_XYZB3 0x00a
#define D3DFVF_XYZB4 0x00c
#define D3DFVF_XYZB5 0x00e
#define D3DFVF_XYZW 0x4002

#define D3DFVF_NORMAL 0x010
#define D3DFVF_PSIZE 0x020
#define D3DFVF_DIFFUSE 0x040
#define D3DFVF_SPECULAR 0x080

#define D3DFVF_TEXCOUNT_MASK 0xf00
#define D3DFVF_TEXCOUNT_SHIFT 8
#define D3DFVF_TEX1 0x100
#define D3DFVF_TEX2 0x200
#define D3DFVF_TEX4 0x400

#endif // xrVertexDeclTypes_included

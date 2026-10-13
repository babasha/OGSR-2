#ifndef xrD3DDefs_included
#define xrD3DDefs_included
#pragma once

// Opaque handle typedefs for the shared scene-graph / shader-object headers
// (FBasicVisual, sh_atomic, sh_texture, sh_rt, tss_def, ...). They keep the
// D3D-era spelling because the shared headers and the level/model file formats
// still speak it, but nothing behind them is Direct3D any more: the D3D11
// renderer is gone and the Vulkan side stores its own objects in the parallel
// vk_* files.
//
// All pointer types are `void*` so that `T*` parses and pointer arithmetic /
// null comparisons work. Sizeof matches a native pointer.

typedef void* ID3DBlob;
typedef void* ID3DInclude;
typedef void* ID3DQuery;

typedef void* ID3DVertexShader;
typedef void* ID3DPixelShader;

typedef void* ID3DTexture2D;
typedef void* ID3DTexture3D;
typedef void* ID3DBaseTexture;
typedef void* ID3DRenderTargetView;
typedef void* ID3DDepthStencilView;
typedef void* ID3DShaderResourceView;
typedef void* ID3D11UnorderedAccessView;

typedef void* ID3DVertexBuffer;
typedef void* ID3DIndexBuffer;
typedef void* ID3DInputLayout;

typedef void* ID3DState;

// Leftover from `tss_def.h::record()`. The method body lived in a D3D-only TU
// that no longer exists, but the declaration still has to parse.
struct IDirect3DStateBlock9;

// `tss_def.h::UpdateState(dx10State&)` — same story: declaration only.
class dx10State;

// Descriptor structs passed by reference / used in arrays by the shared
// headers. Nothing reads them; sizeof just has to be valid.
struct D3D_TEXTURE2D_DESC
{
    u32 Width;
    u32 Height;
    u32 MipLevels;
    u32 ArraySize;
    u32 Format;
    u32 SampleCount;
    u32 SampleQuality;
    u32 Usage;
    u32 BindFlags;
    u32 CPUAccessFlags;
    u32 MiscFlags;
};

struct D3D_RASTERIZER_DESC      { u32 _opaque[16]; };
struct D3D_DEPTH_STENCIL_DESC   { u32 _opaque[16]; };
struct D3D_BLEND_DESC           { u32 _opaque[20]; };
struct D3D_SAMPLER_DESC         { u32 _opaque[16]; };
struct D3D_INPUT_ELEMENT_DESC   { u32 _opaque[8];  };

#ifndef D3D_COMMONSHADER_SAMPLER_SLOT_COUNT
#  define D3D_COMMONSHADER_SAMPLER_SLOT_COUNT 16
#endif

#endif // xrD3DDefs_included

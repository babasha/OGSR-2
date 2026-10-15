#ifndef xrD3DDefs_included
#define xrD3DDefs_included
#pragma once

// Opaque handle typedefs for the shared scene-graph / shader-object headers
// (FBasicVisual, sh_atomic, sh_texture, ...). They keep the
// D3D-era spelling because the shared headers and the level/model file formats
// still speak it, but nothing behind them is Direct3D any more: the D3D11
// renderer is gone and the Vulkan side stores its own objects in the parallel
// vk_* files.
//
// All pointer types are `void*` so that `T*` parses and pointer arithmetic /
// null comparisons work. Sizeof matches a native pointer.

typedef void* ID3DVertexShader;
typedef void* ID3DPixelShader;

typedef void* ID3DBaseTexture;
typedef void* ID3DShaderResourceView;

typedef void* ID3DVertexBuffer;
typedef void* ID3DIndexBuffer;

typedef void* ID3DState;

#endif // xrD3DDefs_included

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — TESSENDORF FFT OCEAN.
//
// A spectral wave field: the sea is built in the Fourier domain from the Phillips
// spectrum and transformed into a displacement map every frame. Three cascades
// (three periodic tiles at very different sizes), each carrying only the band of
// wavelengths it can resolve, sampled together by world position.
//
// WHAT IT ADDS THAT THE ANALYTIC SWELL CANNOT. The nine-octave sum in
// water_common.glsl displaces along Y only, and the peak of a sine is round: it
// can make water that undulates but not water with a CREST. Tessendorf's field
// also displaces HORIZONTALLY, pulling water in toward each peak from both
// flanks — sharp on top, flat in the trough, which is what a wind sea looks like.
// It also hands over a Jacobian, so whitewater comes from "this water has folded
// over itself" instead of "this vertex is high and steep".
//
// WHAT IT DOES NOT DO, and must not be asked to. This is a model of an INFINITE
// DEEP OPEN SEA. It knows nothing about shores, depth, the size of the pond it is
// in, or the ceiling above it — and most of the water in this game is a flooded
// cellar or a puddle. So it is a SOURCE for the field, gated by the same fetch
// and shelter tests everything else is: full strength on open water, nothing
// indoors. The shore break, the pool mask, the ripple sim and the wetness map are
// all upstream of it and stay exactly as they were.
//
// Modelled on github.com/kentril0/WaterSurfaceRendering (MIT), which computes the
// same transform on the CPU with FFTW3. Here it is four compute dispatches — no
// third-party dependency, no per-frame upload, nothing on the game thread.
#pragma once
#include "vk_core.h"

namespace VK { namespace WaterFFT {

// Tile resolution and cascade count. ⚠ Must match water_fft_common.glsl — the
// shader hard-codes them because the FFT's shared-memory array is sized by N.
constexpr u32 kTexels   = 256;
constexpr u32 kCascades = 3;

// Create the images, views and pipelines WITHOUT a command buffer. Call from the
// water pass's own Init, before it writes any descriptor set that names these
// views: the pool-mask pass writes its set exactly once at startup, and without
// this the first Dispatch — which is what would otherwise create them — has not
// happened yet, so it would bake a VK_NULL_HANDLE in. Returns false if the
// shaders are missing.
bool EnsureCreated();

// Run the four stages. Call once per frame, OUTSIDE a render pass, before
// anything samples the result — which now includes the POOL MASK pass, not just
// the water draw. Cheap to call when disabled (transitions once, then returns).
void Dispatch(VkCommandBuffer cmd);

// True once the field holds a transform worth sampling. Callers bind a dummy
// view and pass 0 for the enable flag when this is false — the water shaders
// then run on the analytic swell alone, exactly as before this existed.
bool Ready();

// (Dx, h, Dz, foam) and (dh/dx, dh/dz, Jacobian, -) as 2D ARRAY views, one layer
// per cascade. Bound in VK_IMAGE_LAYOUT_GENERAL — they are storage images the
// compute stages write and the graphics stages sample, and never transition.
VkImageView DispView();
VkImageView DerivView();
VkSampler   GetSampler();

// Tile size of cascade `c` in metres. The shaders need all three to turn a world
// XZ into three sets of tile coordinates, so they travel in the water UBO.
float CascadeLen(u32 c);

void Destroy();

}}  // namespace VK::WaterFFT

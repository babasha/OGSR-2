// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Particle billboard render pass.
//
// Draws PAPI-simulated particle effects as camera-facing billboards over the
// already-rendered scene (depth-tested, no depth write). Registered after Sky
// so particles composite on top of the world. Owns the shared particle
// pipeline-per-blend-mode, the sprite texture descriptor cache, and a
// per-frame dynamic vertex ring.

#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"

enum EParticleBlendMode : int;

namespace VK {

namespace Vol { struct SmokeParticle; }   // vk_volumetrics.h — Stage-1 smoke media

// Lifecycle (called from CRender::create / device teardown).
bool ParticlePass_Init();
void ParticlePass_Destroy();

// Registered pass entry — iterates g_DynamicVisuals + g_HudVisuals for
// particle visuals. Three phases: world effects (scene projection), HUD
// effects (HUD-FOV projection + near depth range — muzzle flashes), and
// PBM_DISTORT effects (rendered into the distortion buffer for the tonemap).
void Pass_Particles(FrameContext& ctx);

// Stage-1 VMS: gather this frame's alpha-smoke (PBM_BLEND, world-phase) particles
// as participating media for the froxel grid — each live PAPI particle becomes a
// {world pos, radius, albedo, density}. Appends to `out`. Called before Vol::Execute
// (particles were simulated in CollectVisuals, so their data is valid).
void CollectSmokeParticles(xr_vector<VK::Vol::SmokeParticle>& out);

// RENDER_THREAD_PLAN step 7: the extract (PREPARE-phase) half of the particle pass. Collects the effects,
// resolves their textures, builds the sprite vertices into the frame's ring slot AND gathers the smoke
// media list. Pass_Particles / Pass_World only read what it produced.
void Particles_Extract();
// r_ptcl_mt: the billboard filling Particles_Extract handed to a worker, finished (the last prepare)
void Particles_ExtractFinish();
// This frame's smoke-media list (empty when Particles_Extract did not run for the current frame).
const xr_vector<VK::Vol::SmokeParticle>& SmokeParticlesFrame();

// Resources shared with the particle visual classes.
namespace ParticlePass {
    bool             Ready();
    VkPipelineLayout GetLayout();
    VkPipeline       GetPipeline(EParticleBlendMode mode);     // lazy per-mode
    // Load (or fetch from cache) the sprite texture's descriptor set, keyed by
    // texture name. Returns VK_NULL_HANDLE if the texture can't be loaded.
    VkDescriptorSet  GetTextureSet(const char* texture_name);
    // The same, but a cache miss is read on a background thread: VK_NULL_HANDLE with *pending = true until
    // it is ready -- draw nothing of it meanwhile, ask again next frame. For the frame's own users (wallmarks,
    // rain, sprites): a first use no longer reads the SD inside the record.
    VkDescriptorSet  GetTextureSetAsync(const char* texture_name, bool* pending);

    // Like GetTextureSet but returns the raw sprite image view (loading + caching
    // the texture if needed) — used by the GPU-particle bindless texture array.
    // VK_NULL_HANDLE if the texture can't be loaded.
    VkImageView      GetTextureView(const char* texture_name);

    // Heat-haze distortion buffer (PBM_DISTORT effects): full-res RGBA8, neutral
    // 0.5, written by Pass_Particles, consumed by the tonemap composite which
    // offsets the scene UVs by (rg - 0.5) * kDistortAmount. Created lazily the
    // first frame a distort effect is visible; null until then (tonemap skips).
    VkImageView      GetDistortView();
    u32              DistortGeneration();   // bumps on (re)create — rebind trigger

    // Decal pipelines (VK::Wallmarks): generic = MODULATE2X (R4
    // effects_wallmark parity); blood = alpha blend with the SSS
    // effects_wallmark_blood alpha remap (readable on dark clothing).
    VkPipeline       GetWallmarkPipeline();
    VkPipeline       GetBloodWallmarkPipeline();

    // Rain (VK::Pass_Rain): splashes = alpha blend, shape from tex.a, colour
    // from vertex tint (fx_rain rgb is a refraction normal map); drop streaks
    // = procedural shape (the fx_rain alpha reads empty — rain_drop.frag).
    VkPipeline       GetRainPipeline();
    VkPipeline       GetRainDropPipeline();
}

}  // namespace VK

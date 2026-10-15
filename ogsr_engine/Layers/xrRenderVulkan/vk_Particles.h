// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Vulkan-native particle visuals (effect + group).
//
// Dedicated classes built directly on PAPI (ParticleManager()) — they do NOT
// reuse the R4 PS::CParticleEffect/CParticleGroup visuals (too render-coupled).
// Simulation logic (OnFrame/Play/Stop/UpdateParent/Compile) is ported verbatim
// from the R4 sources; rendering is our own camera-facing billboard pass
// (see vk_pass_particles.cpp). The shared definitions PS::CPEDef / PS::CPGDef
// are loaded by vk_ParticleEffectDef.cpp + vk_PSLibrary.cpp.
//
// Deliberately light-weight header: only forward-declares the PS definition
// types so the render pass can include it without pulling the R4 PS headers.

#pragma once

#include "vk_core.h"
#include "vk_Visual.h"
#include "../../Include/xrRender/ParticleCustom.h"

namespace FVF { struct LIT; }
namespace PS  { class CPEDef; class CPGDef; }
namespace VK  { class CVulkanBuffer; }

// Particle blend modes (matches R4 CBlender_Particle oBlend / monolith port).
// Fixed underlying type so vk_pass_particles.h can opaquely forward-declare it.
enum EParticleBlendMode : int
{
    PBM_SET       = 0,  // ONE, ZERO            (opaque)
    PBM_BLEND     = 1,  // SRC_ALPHA, 1-SRC_A   (standard alpha)
    PBM_ADD       = 2,  // ONE, ONE             (additive — fire/sparks/muzzle)
    PBM_MUL       = 3,  // DST_COLOR, ZERO      (multiply)
    PBM_MUL_2X    = 4,  // DST_COLOR, SRC_COLOR (multiply 2x)
    PBM_ALPHA_ADD = 5,  // SRC_ALPHA, ONE       (alpha-weighted additive)
    PBM_DISTORT   = 6,  // distortion — skipped (no distortion buffer)
    PBM_COUNT     = 7,
};

class vkCParticleEffect;

// ----------------------------------------------------------------------------
// Common base for particle visuals. Added to g_DynamicVisuals by add_Visual;
// the particle pass filters by Type and flattens groups to leaf effects.
// ----------------------------------------------------------------------------
class vkParticleVisual : public vkRender_Visual, public IParticleCustom
{
public:
    IParticleCustom* dcast_ParticleCustom() override { return this; }

    // Append the renderable leaf effects (self for an effect, children for a
    // group) into `out`. The pass draws each leaf with the right blend pipeline.
    virtual void CollectEffects(xr_vector<vkCParticleEffect*>& out) = 0;
};

// ----------------------------------------------------------------------------
// Single particle effect — one PAPI effect + action list, sprite billboards.
// ----------------------------------------------------------------------------
class vkCParticleEffect final : public vkParticleVisual
{
public:
    PS::CPEDef* m_Def = nullptr;

    int  m_HandleEffect     = -1;
    int  m_HandleActionList = -1;

    EParticleBlendMode m_BlendMode = PBM_BLEND;

    enum
    {
        flRT_Playing      = (1 << 0),
        flRT_DefferedStop = (1 << 1),
        flRT_XFORM        = (1 << 2),
        flRT_HUDmode      = (1 << 3),
    };
    Flags8  m_RT_Flags;

    float   m_fElapsedLimit = 0.f;
    s32     m_MemDT         = 0;
    Fvector m_InitialPosition{};
    Fmatrix m_XFORM;

    // Cached texture descriptor set (shared, owned by the particle pass cache).
    VkDescriptorSet m_TextureSet = VK_NULL_HANDLE;
    bool            m_TextureResolved = false;

    // GPU-driven particles (Phase 3 #3): when r_gpu_particles routes this smoke
    // effect onto the GPU path, m_GpuProgram caches its program-registry slot
    // (-2 = unresolved, -1 = unsupported → CPU fallback, >=0 = slot) and
    // m_GpuAccum carries the fractional emit count between frames.
    int   m_GpuProgram = -2;
    float m_GpuAccum   = 0.0f;

    // #6: group-owned emitters with on-birth/on-play/on-dead children must keep
    // the CPU sim — child spawning hangs off the CPU per-particle callbacks.
    // Set by vkCParticleGroup::Compile; GpuClaimed() refuses these.
    bool  m_GpuNoRoute = false;

    // #5 per-instance spawn ownership (small-budget effects, maxP <= 8): each
    // sub-slot holds the spawn time of the particle occupying it; a new spawn
    // is only allowed into a slot whose occupant expired (one lifetime). This
    // mirrors the CPU per-instance pool for 1-billboard flames/glows — without
    // it, freed units of the SHARED program budget get snapped up by whichever
    // visible emitter wins the frame's race (stacked double flames on lucky
    // campfires, starvation on the rest).
    static constexpr u32 kGpuOwnSlots = 8;
    float m_GpuSpawnT[kGpuOwnSlots] = { -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f };

    // Off-screen emission gate: the game can keep an effect PLAYING while
    // never submitting its visual to the render lists (burn/smoke attached to
    // the hidden first-person actor) — the CPU path simulated those invisibly.
    // The registry emits off-frustum only for STATIONARY emitters (campfires)
    // or effects the game submitted recently. Stamped by Pass_Particles.
    float   m_GpuLastSubmitT = -1e9f;
    Fvector m_GpuLastPos     = {};
    float   m_GpuLastMoveT   = -1e9f;
    float   m_GpuLastTickT   = -1e9f;   // OnFrame ran — the game still owns/updates this effect
    bool    m_GpuHiddenKill  = false;   // one-shot: particles killed when ticking stopped (dead-queued object)

public:
    vkCParticleEffect();
    virtual ~vkCParticleEffect();

    BOOL Compile(PS::CPEDef* def);

    PS::CPEDef* GetDefinition() const { return m_Def; }
    int         GetHandleEffect() const { return m_HandleEffect; }
    EParticleBlendMode GetBlendMode() const { return m_BlendMode; }

    // Resolve (lazily) and return the shared texture descriptor set.
    VkDescriptorSet ResolveTextureSet();

    // True when r_gpu_particles routes this smoke effect onto the GPU path
    // (lazily resolves + caches m_GpuProgram). GPU draws it → CPU BuildVertices
    // skips it (returns 0) so the smoke isn't drawn twice.
    bool GpuClaimed();

    // Build camera-facing billboards into `dst` (FVF::LIT). Returns vertex count
    // written (6 per particle), clamped to `maxVerts`.
    u32 BuildVertices(FVF::LIT* dst, u32 maxVerts);
    // What BuildVertices would write without a cap (0 when it would write nothing). Main thread: it resolves the
    // GpuClaimed cache, so BuildVertices may then run on a worker (Particles_Extract, r_ptcl_mt).
    u32 PlannedVerts();

    // vkParticleVisual
    void CollectEffects(xr_vector<vkCParticleEffect*>& out) override;

    // IParticleCustom
    void OnDeviceCreate()  override {}
    void OnDeviceDestroy() override {}
    void UpdateParent(const Fmatrix& m, const Fvector& velocity, BOOL bXFORM) override;
    void OnFrame(u32 dt)   override;
    void Play()            override;
    void Stop(BOOL bDefferedStop = TRUE) override;
    BOOL IsPlaying()       override { return m_RT_Flags.is(flRT_Playing); }
    BOOL IsDeferredStopped() override { return m_RT_Flags.is(flRT_DefferedStop); }
    u32  ParticlesCount()  override;
    float GetTimeLimit()   override;
    const shared_str Name() override;
    void SetHudMode(BOOL b) override { m_RT_Flags.set(flRT_HUDmode, b); }
    BOOL GetHudMode()      override { return m_RT_Flags.is(flRT_HUDmode); }

private:
    EParticleBlendMode DetermineBlendMode() const;
};

// ----------------------------------------------------------------------------
// Particle group — N timed child effects (campfires, explosions, ...) PLUS the
// R4 on-play / on-birth / on-dead child spawning (ParticleGroup.cpp SItem):
// per-particle related children that follow their emitter particle, and free
// children fired once at a particle's birth/death (secondary smoke, sparks...).
// ----------------------------------------------------------------------------
namespace PAPI { struct Particle; }

class vkCParticleGroup final : public vkParticleVisual
{
public:
    PS::CPGDef* m_Def = nullptr;

    enum
    {
        flRT_Playing      = (1 << 0),
        flRT_DefferedStop = (1 << 1),
    };
    Flags8  m_RT_Flags;
    float   m_CurrentTime = 0.f;
    Fvector m_InitialPosition{};

    // One item per CPGDef::SEffect. `related` runs PARALLEL to the effect's
    // live PAPI particle array (R4 invariant: index i follows particle i —
    // both sides remove via swap-with-last, see the dead callback).
    struct SItem
    {
        vkCParticleEffect*            effect = nullptr;
        xr_vector<vkCParticleEffect*> related;   // flOnPlayChild — one per live particle
        xr_vector<vkCParticleEffect*> freeKids;  // flOnBirthChild/flOnDeadChild — fire & forget
    };
    xr_vector<SItem> m_Items;

    // Child spawn/stop (called from the PAPI birth/dead callbacks in
    // vk_ParticleGroup.cpp; public because the callbacks are free functions).
    void StartRelatedChild(u32 item, const char* eff_name, PAPI::Particle& m);
    void StopRelatedChild(u32 item, u32 idx);
    void StartFreeChild(u32 item, const char* eff_name, PAPI::Particle& m);

public:
    vkCParticleGroup();
    virtual ~vkCParticleGroup();

    BOOL Compile(PS::CPGDef* def);

    // vkParticleVisual
    void CollectEffects(xr_vector<vkCParticleEffect*>& out) override;

    // IParticleCustom
    void OnDeviceCreate()  override {}
    void OnDeviceDestroy() override {}
    void UpdateParent(const Fmatrix& m, const Fvector& velocity, BOOL bXFORM) override;
    void OnFrame(u32 dt)   override;
    void Play()            override;
    void Stop(BOOL bDefferedStop = TRUE) override;
    BOOL IsPlaying()       override { return m_RT_Flags.is(flRT_Playing); }
    BOOL IsDeferredStopped() override { return m_RT_Flags.is(flRT_DefferedStop); }
    u32  ParticlesCount()  override;
    float GetTimeLimit()   override;
    const shared_str Name() override;
    void SetHudMode(BOOL b) override;
    BOOL GetHudMode()      override;
};

// ----------------------------------------------------------------------------
// Factories (resolve against the loaded PS library — vk_PSLibrary.cpp).
// ----------------------------------------------------------------------------
vkCParticleEffect* vkCreateParticleEffect(const char* ped_name);   // PED → effect
vkParticleVisual*  vkCreateParticle(const char* name);             // PED or PGD; null if unknown

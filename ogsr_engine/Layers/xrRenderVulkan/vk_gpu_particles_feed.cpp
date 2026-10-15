// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// GPU-Driven Particles — Phase 3 step 2 piece #3: real-emitter feed.
//
// Walks the live .pe particle visuals each frame and appends one EmitterSample
// per playing smoke effect that translates to a GPU program. This lets level
// smoke (campfires, anomalies, ...) drive the GPU particle system automatically
// — no manual gp_spawn. Kept in its own TU so the PS/visual includes stay out
// of the clean GPU module (vk_gpu_particles.cpp).
//
// Runs on the render thread inside DispatchComputeAndDraw — the same
// g_DynamicVisuals walk CollectSmokeParticles already does, so no extra locking.

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_Particles.h"
#include "vk_gpu_particles.h"         // EmitterSample / Enabled / ResolveProgram / ProgramRate
#include "../../xr_3da/device.h"      // RV().fTimeGlobal — per-instance spawn-slot expiry
#include <mutex>

// Registry of live GPU-claimed effects. Emission runs from HERE (not from the
// frustum-visible visual lists): the CPU path keeps simulating off-screen
// emitters via shedule_Update, so a campfire is already burning when you
// arrive — the GPU path matches that by emitting for every live claimed
// effect regardless of visibility (the draw was never CPU-culled anyway, and
// the #5 budgets already count live OBJECTS, not visible ones).
// Registered by vkCParticleEffect::GpuClaimed on resolve, unregistered in its
// destructor (game thread) — hence the mutex.
namespace {
    std::mutex                       s_liveFxMx;
    xr_vector<vkCParticleEffect*>    s_liveFx;
}

void VK_GP_RegisterEffect(vkCParticleEffect* e)
{
    if (!e) return;
    std::lock_guard<std::mutex> g(s_liveFxMx);
    s_liveFx.push_back(e);
}

void VK_GP_UnregisterEffect(vkCParticleEffect* e)
{
    std::lock_guard<std::mutex> g(s_liveFxMx);
    for (size_t i = 0; i < s_liveFx.size(); ++i)
        if (s_liveFx[i] == e) { s_liveFx[i] = s_liveFx.back(); s_liveFx.pop_back(); return; }
}

void VK_GP_CollectWorldEmitters(xr_vector<VK::GPUParticles::EmitterSample>& out, float dt)
{
    using namespace VK::GPUParticles;
    if (!Enabled() || dt <= 0.0f) return;

    std::lock_guard<std::mutex> g(s_liveFxMx);
    const float now = RV().fTimeGlobal;
    for (vkCParticleEffect* e : s_liveFx)
    {
        if (!e || !e->IsPlaying()) continue;
        if (e->m_GpuProgram < 0) continue;           // registry only holds claimed, but be safe

        // Off-screen emission is for effects the game actually shows:
        // STATIONARY emitters (campfires keep burning behind your back) or
        // anything submitted to render recently. A PLAYING but never-submitted
        // MOVING emitter is a hidden attachment (burn/smoke stuck to the
        // first-person actor) — the CPU path simulated those invisibly, so
        // emitting them here would show smoke the game deliberately hides.
        const Fvector pos = e->m_RT_Flags.is(vkCParticleEffect::flRT_XFORM) ? e->m_XFORM.c
                                                                            : e->m_InitialPosition;
        if (!pos.similar(e->m_GpuLastPos, 0.02f)) { e->m_GpuLastPos = pos; e->m_GpuLastMoveT = now; }
        // Dead-queued objects stop being ticked (OnFrame): the game hides them
        // from render instantly (CPU wipes their particles with the object),
        // so besides stopping emission, kill their remaining particles. Safe
        // against false positives now: the kill has an age floor (gp_simulate)
        // and we reset the victim's own spawn ring, so a still-living effect
        // that trips this simply respawns next frame instead of blanking 10 s.
        // 3 s threshold: the off-screen scheduler can tick sparsely.
        const bool ticked = (now - e->m_GpuLastTickT) < 3.0f;
        if (!ticked) {
            if (!e->m_GpuHiddenKill) {
                e->m_GpuHiddenKill = true;
                QueueKill(e->m_GpuProgram, pos, 1.5f);
                for (u32 s = 0; s < vkCParticleEffect::kGpuOwnSlots; ++s) e->m_GpuSpawnT[s] = -1e9f;
            }
            continue;
        }
        e->m_GpuHiddenKill = false;
        const bool submitted  = (now - e->m_GpuLastSubmitT) < 1.0f;
        const bool stationary = (now - e->m_GpuLastMoveT)   > 1.0f;
        if (!submitted && !stationary) continue;

        const float rate = ProgramRate(e->m_GpuProgram);
        if (rate <= 0.0f) continue;

        e->m_GpuAccum += rate * dt;
        u32 cnt = (u32)e->m_GpuAccum;
        e->m_GpuAccum -= (float)cnt;

        // #5 per-instance ownership for small budgets (flames = ONE
        // billboard): only spawn into an OWN slot whose previous
        // occupant has lived out its lifetime. Refused spawns are
        // dropped, not banked — exactly PAPI's at-max birth drop.
        const u32 ownMax = ProgramMaxP(e->m_GpuProgram);
        if (cnt && ownMax && ownMax <= vkCParticleEffect::kGpuOwnSlots) {
            const float life = ProgramLife(e->m_GpuProgram);
            const float now  = RV().fTimeGlobal;
            u32 allow = 0;
            for (u32 k = 0; k < cnt; ++k) {
                bool found = false;
                for (u32 s = 0; s < ownMax; ++s)
                    if (now - e->m_GpuSpawnT[s] >= life) { e->m_GpuSpawnT[s] = now; found = true; break; }
                if (!found) break;
                ++allow;
            }
            cnt = allow;
        }

        // count == 0 samples still matter: the per-program alive cap (#5) =
        // m_MaxParticles × instance count, and an instance whose fractional
        // accumulator skipped this frame is still alive. Zero-count samples
        // never become spawn requests.

        EmitterSample es;
        es.program = (u32)e->m_GpuProgram;
        es.count   = cnt;
        es.pos     = e->m_RT_Flags.is(vkCParticleEffect::flRT_XFORM) ? e->m_XFORM.c
                                                                     : e->m_InitialPosition;
        es.vel.set(0, 0, 0);
        es.hud     = e->GetHudMode();
        out.push_back(es);
    }
}

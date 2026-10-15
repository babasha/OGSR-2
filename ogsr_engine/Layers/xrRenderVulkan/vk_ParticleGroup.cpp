// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - vkCParticleGroup: timed collection of child particle effects.
//
// Dedicated Vulkan particle group (NOT the R4 PS::CParticleGroup). Holds one
// child vkCParticleEffect per CPGDef::SEffect and plays/stops them on their
// [Time0,Time1] windows — the core of campfires, explosions, etc. — PLUS the
// R4 on-play / on-birth / on-dead child spawning (ported from ParticleGroup.cpp
// SItem + OnGroupParticleBirth/Dead): related children follow their emitter
// particle 1:1; free children are fired once at a particle's birth/death.
//
// The compat header (vk_FBasicVisual.h) maps dxRender_Visual -> vkRender_Visual
// so ParticleGroup.h (CPGDef lives there, next to the unused CParticleGroup)
// compiles.

#include "stdafx.h"

#define FBasicVisualH
#include "vk_FBasicVisual.h"
#include "CRender_Vulkan.h"
#include "vk_ModelPool.h"

#include "vk_Particles.h"
#include "../../xrParticles/psystem.h"     // PAPI:: — MUST precede ParticleGroup.h
#include "../xrRender/ParticleEffectDef.h" // PS::CPEDef (random-frame flags in the birth callback)
#include "../xrRender/ParticleGroup.h"     // PS::CPGDef

#include <algorithm>                       // std::min / std::remove

using namespace PS;

// ============================================================================
// PAPI callbacks for group-owned effects (R4 OnGroupParticleBirth/Dead).
// `param` = item index inside the group; `idx` = particle index — PAPI removes
// particles via swap-with-last, so StopRelatedChild mirrors that exactly to
// keep `related[i] follows particle[i]` true.
// ============================================================================
static void vk_OnGroupParticleBirth(void* owner, u32 param, PAPI::Particle& m, u32 /*idx*/)
{
    auto* PG = static_cast<vkCParticleGroup*>(owner);
    // `param` indexes both m_Items and m_Def->m_Effects (built 1:1). Guard the
    // smaller of the two and the m_Def pointer before either is dereferenced.
    if (!PG || !PG->m_Def || param >= PG->m_Items.size() || param >= PG->m_Def->m_Effects.size()) return;
    vkCParticleEffect* PE = PG->m_Items[param].effect;
    if (!PE) return;

    // Base birth behaviour — same as vk_OnEffectParticleBirth in
    // vk_ParticleEffect.cpp (the group callback REPLACES it on the handle).
    if (const CPEDef* PED = PE->GetDefinition())
    {
        if (PED->m_Flags.is(CPEDef::dfRandomFrame))
            m.frame = (u16)iFloor(Random.randI(PED->m_Frame.m_iFrameCount) * 255.f);
        if (PED->m_Flags.is(CPEDef::dfAnimated) && PED->m_Flags.is(CPEDef::dfRandomPlayback) && Random.randI(2))
            m.flags.set(PAPI::Particle::ANIMATE_CCW, TRUE);
    }

    const CPGDef::SEffect* eff = PG->m_Def->m_Effects[param];
    if (eff->m_Flags.is(CPGDef::SEffect::flOnBirthChild))
        PG->StartFreeChild(param, eff->m_OnBirthChildName.c_str(), m);
    if (eff->m_Flags.is(CPGDef::SEffect::flOnPlayChild))
        PG->StartRelatedChild(param, eff->m_OnPlayChildName.c_str(), m);
}

static void vk_OnGroupParticleDead(void* owner, u32 param, PAPI::Particle& m, u32 idx)
{
    auto* PG = static_cast<vkCParticleGroup*>(owner);
    if (!PG || !PG->m_Def || param >= PG->m_Items.size() || param >= PG->m_Def->m_Effects.size()) return;

    const CPGDef::SEffect* eff = PG->m_Def->m_Effects[param];
    if (eff->m_Flags.is(CPGDef::SEffect::flOnPlayChild))
        PG->StopRelatedChild(param, idx);
    if (eff->m_Flags.is(CPGDef::SEffect::flOnDeadChild))
        PG->StartFreeChild(param, eff->m_OnDeadChildName.c_str(), m);
}

// ============================================================================
// Child spawn helpers (R4 SItem::Start{Related,Free}Child / StopRelatedChild)
// ============================================================================
namespace {
// Position/velocity for a spawned child: world point of the emitter particle +
// its per-step velocity (R4 derives it from pos-posB).
void ChildSpawnTransform(vkCParticleEffect* emitter, vkCParticleEffect* child,
                         PAPI::Particle& m, Fmatrix& M, Fvector& vel)
{
    M.identity();
    vel.sub(m.pos, m.posB);
    const float step = child->GetDefinition() ? child->GetDefinition()->GetFStep() : 0.f;
    if (step > EPS_S) vel.div(step);
    if (emitter->m_RT_Flags.is(vkCParticleEffect::flRT_XFORM))
    {
        M.set(emitter->m_XFORM);
        M.transform_dir(vel);
    }
    Fvector p;
    M.transform_tiny(p, m.pos);
    M.c.set(p);
}
}  // namespace

void vkCParticleGroup::StartRelatedChild(u32 item, const char* eff_name, PAPI::Particle& m)
{
    vkCParticleEffect* C = vkCreateParticleEffect(eff_name);
    if (!C) return;   // unknown effect — keep parallel-array invariant broken-safe (see Stop below)
    vkCParticleEffect* E = m_Items[item].effect;
    C->SetHudMode(E ? E->GetHudMode() : FALSE);

    Fmatrix M; Fvector vel;
    ChildSpawnTransform(E, C, m, M, vel);
    C->Play();
    C->UpdateParent(M, vel, FALSE);
    m_Items[item].related.push_back(C);
}

void vkCParticleGroup::StopRelatedChild(u32 item, u32 idx)
{
    auto& rel = m_Items[item].related;
    if (idx >= rel.size()) return;   // defensive (R4 VERIFYs)
    vkCParticleEffect* C = rel[idx];
    if (C) { C->Stop(TRUE); m_Items[item].freeKids.push_back(C); }
    rel[idx] = rel.back();
    rel.pop_back();
}

void vkCParticleGroup::StartFreeChild(u32 item, const char* eff_name, PAPI::Particle& m)
{
    vkCParticleEffect* C = vkCreateParticleEffect(eff_name);
    if (!C) return;
    // R4 FATALs on a looped on-birth child (it would never die); we just refuse.
    if (C->GetTimeLimit() < 0.f)
    {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; Msg("![VK Particles] looped effect '%s' used as group child — skipped", eff_name); }
        xr_delete(C);
        return;
    }
    vkCParticleEffect* E = m_Items[item].effect;
    C->SetHudMode(E ? E->GetHudMode() : FALSE);

    Fmatrix M; Fvector vel;
    ChildSpawnTransform(E, C, m, M, vel);
    C->Play();
    C->UpdateParent(M, vel, FALSE);
    m_Items[item].freeKids.push_back(C);
}

// ============================================================================
// vkCParticleGroup
// ============================================================================
namespace {
void ClearItem(vkCParticleGroup::SItem& I)
{
    xr_delete(I.effect);
    for (auto*& e : I.related)  xr_delete(e);
    for (auto*& e : I.freeKids) xr_delete(e);
    I.related.clear();
    I.freeKids.clear();
}
}  // namespace

vkCParticleGroup::vkCParticleGroup()
{
    Type = MT_PARTICLE_GROUP;
    m_RT_Flags.zero();
    m_InitialPosition.set(0, 0, 0);
    vis.box.set(Fvector{ -0.1f, -0.1f, -0.1f }, Fvector{ 0.1f, 0.1f, 0.1f });
    vis.box.getsphere(vis.sphere.P, vis.sphere.R);
}

vkCParticleGroup::~vkCParticleGroup()
{
    for (auto& I : m_Items)
        ClearItem(I);
    m_Items.clear();
}

BOOL vkCParticleGroup::Compile(PS::CPGDef* def)
{
    m_Def = def;
    for (auto& I : m_Items)
        ClearItem(I);
    m_Items.clear();

    if (m_Def)
    {
        m_Items.resize(m_Def->m_Effects.size());
        for (size_t i = 0; i < m_Def->m_Effects.size(); ++i)
        {
            vkCParticleEffect* E = vkCreateParticleEffect(m_Def->m_Effects[i]->m_EffectName.c_str());
            m_Items[i].effect = E;
            // Group-owned effects report births/deaths to the GROUP so it can
            // spawn/stop children (replaces the effect's own callback set in
            // Compile; param = item index — R4 OnGroupParticleBirth scheme).
            if (E)
            {
                PAPI::ParticleManager()->SetCallback(E->GetHandleEffect(),
                    vk_OnGroupParticleBirth, vk_OnGroupParticleDead, this, (u32)i);
                // #6: child spawning hangs off the CPU per-particle callbacks —
                // an emitter with on-birth/on-play/on-dead children must keep
                // the CPU sim (GpuClaimed refuses it; the CHILDREN it spawns
                // are plain effects and may still route to the GPU).
                const CPGDef::SEffect* eff = m_Def->m_Effects[i];
                if (eff->m_Flags.is(CPGDef::SEffect::flOnBirthChild) ||
                    eff->m_Flags.is(CPGDef::SEffect::flOnPlayChild)  ||
                    eff->m_Flags.is(CPGDef::SEffect::flOnDeadChild))
                    E->m_GpuNoRoute = true;
            }
        }
    }
    return TRUE;
}

void vkCParticleGroup::CollectEffects(xr_vector<vkCParticleEffect*>& out)
{
    for (auto& I : m_Items)
    {
        if (I.effect) I.effect->CollectEffects(out);
        for (auto* e : I.related)  if (e) e->CollectEffects(out);
        for (auto* e : I.freeKids) if (e) e->CollectEffects(out);
    }
}

void vkCParticleGroup::Play()
{
    m_CurrentTime = 0.f;
    m_RT_Flags.set(flRT_DefferedStop, FALSE);
    m_RT_Flags.set(flRT_Playing, TRUE);
}

void vkCParticleGroup::Stop(BOOL bDefferedStop)
{
    if (bDefferedStop)
        m_RT_Flags.set(flRT_DefferedStop, TRUE);
    else
        m_RT_Flags.set(flRT_Playing, FALSE);

    for (auto& I : m_Items)
    {
        if (I.effect) I.effect->Stop(bDefferedStop);
        for (auto* e : I.related)  if (e) e->Stop(bDefferedStop);
        for (auto* e : I.freeKids) if (e) e->Stop(bDefferedStop);
        // Hard stop releases children immediately (R4 SItem::Stop).
        if (!bDefferedStop)
        {
            for (auto*& e : I.related)  xr_delete(e);
            for (auto*& e : I.freeKids) xr_delete(e);
            I.related.clear();
            I.freeKids.clear();
        }
    }
}

void vkCParticleGroup::UpdateParent(const Fmatrix& m, const Fvector& velocity, BOOL bXFORM)
{
    m_InitialPosition = m.c;
    // Children keep their own transforms (R4: SItem::UpdateParent moves the
    // emitter effect only; related children re-anchor in OnFrame).
    for (auto& I : m_Items)
        if (I.effect)
            I.effect->UpdateParent(m, velocity, bXFORM);
}

void vkCParticleGroup::OnFrame(u32 u_dt)
{
    if (m_Def && m_RT_Flags.is(flRT_Playing))
    {
        const float ct = m_CurrentTime;
        const float f_dt = float(u_dt) / 1000.f;

        for (size_t i = 0; i < m_Def->m_Effects.size(); ++i)
        {
            const CPGDef::SEffect* e = m_Def->m_Effects[i];
            if (!e->m_Flags.is(CPGDef::SEffect::flEnabled)) continue;
            vkCParticleEffect* I = (i < m_Items.size()) ? m_Items[i].effect : nullptr;
            if (!I) continue;

            if (I->IsPlaying())
            {
                if ((ct <= e->m_Time1) && (ct + f_dt >= e->m_Time1))
                    I->Stop(e->m_Flags.is(CPGDef::SEffect::flDefferedStop));
            }
            else
            {
                if (!m_RT_Flags.is(flRT_DefferedStop))
                    if ((ct <= e->m_Time0) && (ct + f_dt >= e->m_Time0))
                        I->Play();
            }
        }

        m_CurrentTime += f_dt;
        if ((m_CurrentTime > m_Def->m_fTimeLimit) && (m_Def->m_fTimeLimit > 0.f))
            if (!m_RT_Flags.is(flRT_DefferedStop))
                Stop(true);

        bool bPlaying = false;
        Fbox box;
        box.invalidate();
        for (size_t i = 0; i < m_Items.size(); ++i)
        {
            SItem& item = m_Items[i];
            const CPGDef::SEffect* def = m_Def->m_Effects[i];
            vkCParticleEffect* E = item.effect;

            if (E)
            {
                // Ticking the emitter fires the birth/dead callbacks above —
                // related/freeKids may grow/shrink during this call.
                E->OnFrame(u_dt);
                if (E->IsPlaying())
                {
                    bPlaying = true;
                    if (E->getVisData().box.is_valid())
                        box.merge(E->getVisData().box);

                    // Related children follow their emitter particle (R4
                    // SItem::OnFrame): related[i] anchors to particles[i].
                    if (def->m_Flags.is(CPGDef::SEffect::flOnPlayChild) && !item.related.empty())
                    {
                        PAPI::Particle* particles;
                        u32 p_cnt;
                        PAPI::ParticleManager()->GetParticles(E->GetHandleEffect(), particles, p_cnt);
                        const u32 n = std::min(p_cnt, (u32)item.related.size());
                        for (u32 k = 0; k < n; ++k)
                        {
                            vkCParticleEffect* C = item.related[k];
                            if (!C) continue;
                            PAPI::Particle& m = particles[k];
                            Fmatrix M;
                            M.translate(m.pos);
                            Fvector vel;
                            vel.sub(m.pos, m.posB);
                            const float step = C->GetDefinition() ? C->GetDefinition()->GetFStep() : 0.f;
                            if (step > EPS_S) vel.div(step);
                            C->UpdateParent(M, vel, FALSE);
                        }
                    }
                }
            }

            for (auto* C : item.related)
            {
                if (!C) continue;
                C->OnFrame(u_dt);
                if (C->IsPlaying())
                {
                    bPlaying = true;
                    if (C->getVisData().box.is_valid())
                        box.merge(C->getVisData().box);
                }
                else if (def->m_Flags.is(CPGDef::SEffect::flOnPlayChildRewind))
                    C->Play();
            }

            if (!item.freeKids.empty())
            {
                u32 rem = 0;
                for (auto*& C : item.freeKids)
                {
                    if (!C) continue;
                    C->OnFrame(u_dt);
                    if (C->IsPlaying())
                    {
                        bPlaying = true;
                        if (C->getVisData().box.is_valid())
                            box.merge(C->getVisData().box);
                    }
                    else { xr_delete(C); ++rem; }
                }
                if (rem)
                    item.freeKids.erase(
                        std::remove(item.freeKids.begin(), item.freeKids.end(), nullptr),
                        item.freeKids.end());
            }
        }

        if (m_RT_Flags.is(flRT_DefferedStop) && !bPlaying)
            m_RT_Flags.set(flRT_Playing | flRT_DefferedStop, FALSE);

        if (box.is_valid())
        {
            vis.box.set(box);
            vis.box.getsphere(vis.sphere.P, vis.sphere.R);
        }
    }
    else
    {
        vis.box.set(m_InitialPosition, m_InitialPosition);
        vis.box.grow(EPS_L);
        vis.box.getsphere(vis.sphere.P, vis.sphere.R);
    }
}

u32 vkCParticleGroup::ParticlesCount()
{
    u32 c = 0;
    for (auto& I : m_Items)
    {
        if (I.effect) c += I.effect->ParticlesCount();
        for (auto* e : I.related)  if (e) c += e->ParticlesCount();
        for (auto* e : I.freeKids) if (e) c += e->ParticlesCount();
    }
    return c;
}

float vkCParticleGroup::GetTimeLimit()
{
    return m_Def ? m_Def->m_fTimeLimit : -1.f;
}

const shared_str vkCParticleGroup::Name()
{
    if (!m_Def) return shared_str{};
    return m_Def->m_Name;
}

void vkCParticleGroup::SetHudMode(BOOL b)
{
    for (auto& I : m_Items)
        if (I.effect)
            I.effect->SetHudMode(b);
}

BOOL vkCParticleGroup::GetHudMode()
{
    return (!m_Items.empty() && m_Items[0].effect) ? m_Items[0].effect->GetHudMode() : FALSE;
}

#undef FBasicVisualH

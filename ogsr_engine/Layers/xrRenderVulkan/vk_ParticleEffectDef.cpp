// xrRenderVulkan - compatibility stub for shared X-Ray Engine code.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha) for the
// stub itself; the interface it mirrors is GSC Game World code (root LICENSE.md).
// Non-commercial use only; keep this notice on redistribution.

// xrRenderVulkan - Vulkan-native PS::CPEDef + PS::CPGDef implementations.
//
// The shared R4 ParticleEffectDef.cpp / ParticleGroup.cpp cannot be compiled
// for Vulkan: CPEDef::CreateShader() pulls the R4 shader-resource manager
// (ref_shader::create), and CPGDef is bundled in the same TU as the
// render-coupled CParticleGroup. So we provide the engine-agnostic definition
// logic here (verbatim from the R4 sources) and stub only the shader-resource
// bits — the Vulkan particle visual loads the sprite texture itself
// (see vk_ParticleEffect.cpp). PAPI action loading + frame/collision execution
// is identical to R4.
//
// The compat header (vk_FBasicVisual.h) maps dxRender_Visual -> vkRender_Visual
// so ParticleGroup.h (whose unused CParticleGroup derives dxParticleCustom)
// compiles. We never instantiate CParticleEffect/CParticleGroup here.

#include "stdafx.h"
#include <algorithm>   // std::sort -- the [VK Parts] top list

// FBasicVisualH must be defined before anything transitively pulls
// FBasicVisual.h (CRender_Vulkan.h does, via the dsgraph chain). CRender_Vulkan.h
// + vk_ModelPool.h also establish the xrD3DDefs Vulkan-branch typedefs that
// Shader.h -> tss_def.h needs (same preamble as the skeleton compat wrappers).
#define FBasicVisualH
#include "vk_FBasicVisual.h"
#include "CRender_Vulkan.h"
#include "vk_ModelPool.h"

#include "../../xrParticles/psystem.h"           // PAPI:: enums — MUST precede the PS headers
#include "../xrRender/ParticleEffectDef.h"
#include "../xrRender/ParticleEffectActions.h"   // pCreateEAction, EParticleAction::type
#include "../xrRender/ParticleGroup.h"           // CPGDef
#include "../../xr_3da/IGame_Level.h"            // g_pGameLevel (collision raypick)

using namespace PAPI;
using namespace PS;

extern float ps_particle_update_coeff;

// ============================================================================
// PS::CPEDef
// ============================================================================
CPEDef::CPEDef()
{
    m_Frame.InitDefault();
    m_uStep = 33;
    m_fStep = float(m_uStep) / 1000.f;
    m_MaxParticles = 0;
    m_CachedShader = nullptr;
    m_fTimeLimit = 0.f;
    m_fCollideOneMinusFriction = 1.f;
    m_fCollideResilience = 0.f;
    m_fCollideSqrCutoff = 0.f;
    m_VelocityScale.set(0.f, 0.f, 0.f);
    m_APDefaultRotation.set(-PI_DIV_2, 0.f, 0.f);
    m_Flags.zero();
}

// [VK Parts] (CRender_Vulkan.cpp prints it every ~5 s): what the CPU particle sim did, per
// effect definition -- particle-steps and collision ray picks. A small open-addressed table
// of atomics keyed by the CPEDef, because the sim runs on the TTAPI pool (r_particles_mt);
// it is read and cleared only after the fork-join, when no worker touches it.
namespace VK { namespace PartStats {
namespace {
struct Slot { std::atomic<const PS::CPEDef*> def{ nullptr }; std::atomic<u32> steps{ 0 }, rays{ 0 }; };
constexpr u32 kSlots = 128;
Slot s_tab[kSlots];
Slot* SlotOf(const PS::CPEDef* d)
{
    const u32 h = u32((uintptr_t(d) >> 4) * 2654435761u) >> 25;
    for (u32 i = 0; i < kSlots; ++i) {
        Slot& s = s_tab[(h + i) & (kSlots - 1)];
        const PS::CPEDef* cur = s.def.load(std::memory_order_acquire);
        if (cur == d) return &s;
        if (!cur && (s.def.compare_exchange_strong(cur, d) || cur == d)) return &s;
    }
    return nullptr;   // table full: the effect goes uncounted this window
}
}   // namespace
void AddSteps(const PS::CPEDef* d, u32 n) { if (Slot* s = SlotOf(d)) s->steps.fetch_add(n, std::memory_order_relaxed); }
void AddRays(const PS::CPEDef* d, u32 n)  { if (Slot* s = SlotOf(d)) s->rays.fetch_add(n, std::memory_order_relaxed); }
// Totals of the window + the top 5 effects by (steps + rays) into `top`; clears the table.
void Take(u64& steps, u64& rays, string512& top)
{
    struct Row { const PS::CPEDef* d; u32 st, ry; };
    Row rows[kSlots]; u32 n = 0;
    steps = rays = 0;
    for (Slot& s : s_tab) {
        const PS::CPEDef* d = s.def.load(std::memory_order_relaxed);
        if (!d) continue;
        const u32 st = s.steps.exchange(0), ry = s.rays.exchange(0);
        s.def.store(nullptr, std::memory_order_relaxed);
        steps += st; rays += ry;
        if (st || ry) rows[n++] = { d, st, ry };
    }
    std::sort(rows, rows + n, [](const Row& a, const Row& b) { return a.st + a.ry > b.st + b.ry; });
    top[0] = 0;
    for (u32 i = 0; i < n && i < 5; ++i) {
        string128 one;
        xr_sprintf(one, "%s%s %u/%u", i ? ", " : "", rows[i].d->m_Name.c_str(), rows[i].st, rows[i].ry);
        xr_strcat(top, one);
    }
}
}}   // namespace VK::PartStats

CPEDef::~CPEDef()
{
    for (auto& it : m_EActionList)
        xr_delete(it);
}

u32 CPEDef::GetUStep() const { return m_uStep * ps_particle_update_coeff; }
float CPEDef::GetFStep() { return m_fStep * ps_particle_update_coeff; }

// Vulkan path: the sprite texture is loaded by the particle VISUAL
// (vk_ParticleEffect.cpp) straight from m_TextureName — there is no R4
// ref_shader resource here, so these are no-ops.
void CPEDef::CreateShader() {}
void CPEDef::DestroyShader() {}

void CPEDef::SetName(LPCSTR name) { m_Name = name; }

void CPEDef::ExecuteAnimate(Particle* particles, u32 p_cnt, float dt) const
{
    const float speedFac = m_Frame.m_fSpeed * dt;
    for (u32 i = 0; i < p_cnt; i++)
    {
        Particle& m = particles[i];
        float f = (float(m.frame) / 255.f + ((m.flags.is(Particle::ANIMATE_CCW)) ? -1.f : 1.f) * speedFac);
        if (f > m_Frame.m_iFrameCount)
            f -= m_Frame.m_iFrameCount;
        if (f < 0.f)
            f += m_Frame.m_iFrameCount;
        m.frame = (u16)iFloor(f * 255.f);
    }
}

// NOTE: the R4 implementation removes particles on dfCollisionDel via
// owner->m_HandleEffect. We deliberately do not pull in the R4 CParticleEffect
// type here, so the collision-delete branch is skipped (these effects are very
// rare in STALKER). Bounce/friction collision is preserved.
void CPEDef::ExecuteCollision(PAPI::Particle* particles, u32 p_cnt, float dt, CParticleEffect* /*owner*/) const
{
    if (!g_pGameLevel)
        return;

    pVector pt{}, n{};
    for (int i = p_cnt - 1; i >= 0; i--)
    {
        Particle& m = particles[i];

        bool pick_needed;
        int pick_cnt = 0;
        do
        {
            pick_needed = false;
            Fvector dir;
            dir.sub(m.pos, m.posB);
            const float dist = dir.magnitude();
            if (dist >= EPS)
            {
                dir.div(dist);

                collide::rq_result RQ;
                const collide::rq_target RT = m_Flags.is(dfCollisionDyn) ? collide::rqtBoth : collide::rqtStatic;
                VK::PartStats::AddRays(this, 1);
                if (g_pGameLevel->ObjectSpace.RayPick(m.posB, dir, dist, RT, RQ, nullptr))
                {
                    pt.mad(m.posB, dir, RQ.range);
                    if (RQ.O)
                    {
                        n.set(0.f, 1.f, 0.f);
                    }
                    else
                    {
                        const CDB::TRI* T = g_pGameLevel->ObjectSpace.GetStaticTris() + RQ.element;
                        const Fvector* verts = g_pGameLevel->ObjectSpace.GetStaticVerts();
                        n.mknormal(verts[T->verts[0]], verts[T->verts[1]], verts[T->verts[2]]);
                    }

                    pick_cnt++;

                    if (m_Flags.is(dfCollisionDel))
                    {
                        // collision-delete unsupported on the Vulkan path (no owner handle).
                        break;
                    }

                    const float nmag = m.vel * n;
                    pVector vn(n * nmag);
                    pVector vt(m.vel - vn);

                    if (vt.length2() <= m_fCollideSqrCutoff)
                        m.vel = vt - vn * m_fCollideResilience;
                    else
                        m.vel = vt * m_fCollideOneMinusFriction - vn * m_fCollideResilience;
                    m.pos = m.posB + m.vel * dt;
                    pick_needed = true;
                }
            }
            else
            {
                m.pos = m.posB;
            }
        } while (pick_needed && (pick_cnt < 2));
    }
}

//------------------------------------------------------------------------------
// I/O
//------------------------------------------------------------------------------
BOOL CPEDef::Load(IReader& F)
{
    R_ASSERT(F.find_chunk(PED_CHUNK_VERSION));
    const u16 version = F.r_u16();
    if (version != PED_VERSION)
        return FALSE;

    R_ASSERT(F.find_chunk(PED_CHUNK_NAME));
    F.r_stringZ(m_Name);

    R_ASSERT(F.find_chunk(PED_CHUNK_EFFECTDATA));
    m_MaxParticles = F.r_u32();

    {
        const u32 action_list = F.find_chunk(PED_CHUNK_ACTIONLIST);
        R_ASSERT(action_list);
        m_Actions.w(F.pointer(), action_list);
    }

    F.r_chunk(PED_CHUNK_FLAGS, &m_Flags);

    if (m_Flags.is(dfSprite))
    {
        R_ASSERT(F.find_chunk(PED_CHUNK_SPRITE));
        F.r_stringZ(m_ShaderName);
        F.r_stringZ(m_TextureName);
    }

    if (m_Flags.is(dfFramed))
    {
        static_assert(sizeof(SFrame) == 28);
        R_ASSERT(F.find_chunk(PED_CHUNK_FRAME));
        F.r(&m_Frame, sizeof(SFrame));
    }

    if (m_Flags.is(dfTimeLimit))
    {
        R_ASSERT(F.find_chunk(PED_CHUNK_TIMELIMIT));
        m_fTimeLimit = F.r_float();
    }

    if (m_Flags.is(dfCollision))
    {
        R_ASSERT(F.find_chunk(PED_CHUNK_COLLISION));
        m_fCollideOneMinusFriction = F.r_float();
        m_fCollideResilience = F.r_float();
        m_fCollideSqrCutoff = F.r_float();
    }

    if (m_Flags.is(dfVelocityScale))
    {
        R_ASSERT(F.find_chunk(PED_CHUNK_VEL_SCALE));
        F.r_fvector3(m_VelocityScale);
    }

    if (m_Flags.is(dfAlignToPath))
    {
        if (F.find_chunk(PED_CHUNK_ALIGN_TO_PATH))
            F.r_fvector3(m_APDefaultRotation);
    }

    if (F.find_chunk(PED_CHUNK_EDATA))
    {
        m_EActionList.resize(F.r_u32());
        bool valid = false;
        for (auto& it : m_EActionList)
        {
            const PAPI::PActionEnum type = (PAPI::PActionEnum)F.r_u32();
            it = pCreateEAction(type);
            valid = it->Load(F);
            if (!valid)
                break;
        }
    }

    return TRUE;
}

void PS::CPEDef::Compile(EPAVec& v)
{
    m_Actions.clear();
    m_Actions.w_u32(v.size());
    int cnt = 0;
    for (EPAVecIt it = v.begin(); it != v.end(); ++it)
    {
        if ((*it)->flags.is(EParticleAction::flEnabled))
        {
            (*it)->Compile(m_Actions);
            cnt++;
        }
    }
    m_Actions.seek(0);
    m_Actions.w_u32(cnt);
}

BOOL CPEDef::Load2(CInifile& ini)
{
    if (ini.line_exist("_effect", "update_step"))
    {
        m_uStep = ini.r_u32("_effect", "update_step");
        m_fStep = float(m_uStep) / 1000.f;
    }
    m_MaxParticles = ini.r_u32("_effect", "max_particles");
    m_Flags.assign(ini.r_u32("_effect", "flags"));

    if (m_Flags.is(dfSprite))
    {
        m_ShaderName = ini.r_string("sprite", "shader");
        m_TextureName = ini.r_string("sprite", "texture");
    }

    if (m_Flags.is(dfFramed))
    {
        m_Frame.m_fTexSize = ini.r_fvector2("frame", "tex_size");
        m_Frame.reserved = ini.r_fvector2("frame", "reserved");
        m_Frame.m_iFrameDimX = ini.r_s32("frame", "dim_x");
        m_Frame.m_iFrameCount = ini.r_s32("frame", "frame_count");
        m_Frame.m_fSpeed = ini.r_float("frame", "speed");
    }

    if (m_Flags.is(dfTimeLimit))
        m_fTimeLimit = ini.r_float("timelimit", "value");

    if (m_Flags.is(dfCollision))
    {
        m_fCollideOneMinusFriction = ini.r_float("collision", "one_minus_friction");
        m_fCollideResilience = ini.r_float("collision", "collide_resilence");
        m_fCollideSqrCutoff = ini.r_float("collision", "collide_sqr_cutoff");
    }

    if (m_Flags.is(dfVelocityScale))
        m_VelocityScale = ini.r_fvector3("velocity_scale", "value");

    if (m_Flags.is(dfAlignToPath))
        m_APDefaultRotation = ini.r_fvector3("align_to_path", "default_rotation");

    const u32 count = ini.r_u32("_effect", "action_count");
    m_EActionList.resize(count);
    u32 action_id = 0;
    for (EPAVecIt it = m_EActionList.begin(); it != m_EActionList.end(); ++it, ++action_id)
    {
        string256 sect;
        xr_sprintf(sect, sizeof(sect), "action_%04d", action_id);
        const PAPI::PActionEnum type = (PAPI::PActionEnum)(ini.r_u32(sect, "action_type"));
        (*it) = pCreateEAction(type);
        (*it)->Load2(ini, sect);
    }

    Compile(m_EActionList);
    return TRUE;
}

void CPEDef::Save2(CInifile& ini)
{
    ini.w_u16("_effect", "version", PED_VERSION);
    ini.w_u32("_effect", "max_particles", m_MaxParticles);
    ini.w_u32("_effect", "flags", m_Flags.get());

    if (m_Flags.is(dfSprite))
    {
        ini.w_string("sprite", "shader", m_ShaderName.c_str());
        ini.w_string("sprite", "texture", m_TextureName.c_str());
    }
    if (m_Flags.is(dfFramed))
    {
        ini.w_fvector2("frame", "tex_size", m_Frame.m_fTexSize);
        ini.w_fvector2("frame", "reserved", m_Frame.reserved);
        ini.w_s32("frame", "dim_x", m_Frame.m_iFrameDimX);
        ini.w_s32("frame", "frame_count", m_Frame.m_iFrameCount);
        ini.w_float("frame", "speed", m_Frame.m_fSpeed);
    }
    if (m_Flags.is(dfTimeLimit))
        ini.w_float("timelimit", "value", m_fTimeLimit);
    if (m_Flags.is(dfCollision))
    {
        ini.w_float("collision", "one_minus_friction", m_fCollideOneMinusFriction);
        ini.w_float("collision", "collide_resilence", m_fCollideResilience);
        ini.w_float("collision", "collide_sqr_cutoff", m_fCollideSqrCutoff);
    }
    if (m_Flags.is(dfVelocityScale))
        ini.w_fvector3("velocity_scale", "value", m_VelocityScale);
    if (m_Flags.is(dfAlignToPath))
        ini.w_fvector3("align_to_path", "default_rotation", m_APDefaultRotation);

    ini.w_u32("_effect", "action_count", m_EActionList.size());
    u32 action_id = 0;
    for (EPAVecIt it = m_EActionList.begin(); it != m_EActionList.end(); ++it, ++action_id)
    {
        string256 sect;
        xr_sprintf(sect, sizeof(sect), "action_%04d", action_id);
        ini.w_u32(sect, "action_type", (*it)->type);
        (*it)->Save2(ini, sect);
    }
}

void CPEDef::Save(IWriter& F)
{
    F.open_chunk(PED_CHUNK_VERSION);
    F.w_u16(PED_VERSION);
    F.close_chunk();

    F.open_chunk(PED_CHUNK_NAME);
    F.w_stringZ(m_Name);
    F.close_chunk();

    F.open_chunk(PED_CHUNK_EFFECTDATA);
    F.w_u32(m_MaxParticles);
    F.close_chunk();

    F.open_chunk(PED_CHUNK_ACTIONLIST);
    F.w(m_Actions.pointer(), m_Actions.size());
    F.close_chunk();

    F.w_chunk(PED_CHUNK_FLAGS, &m_Flags, sizeof(m_Flags));

    if (m_Flags.is(dfSprite))
    {
        F.open_chunk(PED_CHUNK_SPRITE);
        F.w_stringZ(m_ShaderName);
        F.w_stringZ(m_TextureName);
        F.close_chunk();
    }
    if (m_Flags.is(dfFramed))
    {
        F.open_chunk(PED_CHUNK_FRAME);
        F.w(&m_Frame, sizeof(SFrame));
        F.close_chunk();
    }
    if (m_Flags.is(dfTimeLimit))
    {
        F.open_chunk(PED_CHUNK_TIMELIMIT);
        F.w_float(m_fTimeLimit);
        F.close_chunk();
    }
    if (m_Flags.is(dfCollision))
    {
        F.open_chunk(PED_CHUNK_COLLISION);
        F.w_float(m_fCollideOneMinusFriction);
        F.w_float(m_fCollideResilience);
        F.w_float(m_fCollideSqrCutoff);
        F.close_chunk();
    }
    if (m_Flags.is(dfVelocityScale))
    {
        F.open_chunk(PED_CHUNK_VEL_SCALE);
        F.w_fvector3(m_VelocityScale);
        F.close_chunk();
    }
    if (m_Flags.is(dfAlignToPath))
    {
        F.open_chunk(PED_CHUNK_ALIGN_TO_PATH);
        F.w_fvector3(m_APDefaultRotation);
        F.close_chunk();
    }

    F.open_chunk(PED_CHUNK_EDATA);
    F.w_u32(m_EActionList.size());
    for (const auto& it : m_EActionList)
    {
        F.w_u32(it->type);
        it->Save(F);
    }
    F.close_chunk();
}

// ============================================================================
// PS::CPGDef (verbatim from R4 ParticleGroup.cpp — engine-agnostic)
// ============================================================================
CPGDef::CPGDef()
{
    m_Flags.zero();
    m_fTimeLimit = 0.f;
}

CPGDef::~CPGDef()
{
    for (auto& e : m_Effects)
        xr_delete(e);
    m_Effects.clear();
}

void CPGDef::SetName(LPCSTR name) { m_Name = name; }

BOOL CPGDef::Load(IReader& F)
{
    R_ASSERT(F.find_chunk(PGD_CHUNK_VERSION));
    const u16 version = F.r_u16();
    if (version != PGD_VERSION)
    {
        Log("!Unsupported PG version. Load failed.");
        return FALSE;
    }

    R_ASSERT(F.find_chunk(PGD_CHUNK_NAME));
    F.r_stringZ(m_Name);

    F.r_chunk(PGD_CHUNK_FLAGS, &m_Flags);

    if (F.find_chunk(PGD_CHUNK_TIME_LIMIT))
        m_fTimeLimit = F.r_float();
    else
        m_fTimeLimit = 0.0f;

    const bool dont_calc_timelimit = m_fTimeLimit > 0.0f;
    if (F.find_chunk(PGD_CHUNK_EFFECTS))
    {
        m_Effects.resize(F.r_u32());
        for (auto& e : m_Effects)
        {
            e = xr_new<SEffect>();
            F.r_stringZ(e->m_EffectName);
            F.r_stringZ(e->m_OnPlayChildName);
            F.r_stringZ(e->m_OnBirthChildName);
            F.r_stringZ(e->m_OnDeadChildName);
            e->m_Time0 = F.r_float();
            e->m_Time1 = F.r_float();
            e->m_Flags.assign(F.r_u32());

            if (!dont_calc_timelimit)
                m_fTimeLimit = _max(m_fTimeLimit, e->m_Time1);
        }
    }
    return TRUE;
}

BOOL CPGDef::Load2(CInifile& ini)
{
    m_Flags.assign(ini.r_u32("_group", "flags"));
    m_Effects.resize(ini.r_u32("_group", "effects_count"));
    m_fTimeLimit = ini.r_float("_group", "timelimit");

    u32 counter = 0;
    string256 buff;
    for (EffectIt it = m_Effects.begin(); it != m_Effects.end(); ++it, ++counter)
    {
        *it = xr_new<SEffect>();
        xr_sprintf(buff, sizeof(buff), "effect_%04d", counter);
        (*it)->m_EffectName = ini.r_string(buff, "effect_name");
        (*it)->m_OnPlayChildName = ini.r_string(buff, "on_play_child");
        (*it)->m_OnBirthChildName = ini.r_string(buff, "on_birth_child");
        (*it)->m_OnDeadChildName = ini.r_string(buff, "on_death_child");
        (*it)->m_Time0 = ini.r_float(buff, "time0");
        (*it)->m_Time1 = ini.r_float(buff, "time1");
        (*it)->m_Flags.assign(ini.r_u32(buff, "flags"));
    }
    return TRUE;
}

void CPGDef::Save(IWriter& F)
{
    F.open_chunk(PGD_CHUNK_VERSION);
    F.w_u16(PGD_VERSION);
    F.close_chunk();

    F.open_chunk(PGD_CHUNK_NAME);
    F.w_stringZ(m_Name);
    F.close_chunk();

    F.w_chunk(PGD_CHUNK_FLAGS, &m_Flags, sizeof(m_Flags));

    F.open_chunk(PGD_CHUNK_EFFECTS);
    F.w_u32(m_Effects.size());
    for (const auto& e : m_Effects)
    {
        F.w_stringZ(e->m_EffectName);
        F.w_stringZ(e->m_OnPlayChildName);
        F.w_stringZ(e->m_OnBirthChildName);
        F.w_stringZ(e->m_OnDeadChildName);
        F.w_float(e->m_Time0);
        F.w_float(e->m_Time1);
        F.w_u32(e->m_Flags.get());
    }
    F.close_chunk();

    F.open_chunk(PGD_CHUNK_TIME_LIMIT);
    F.w_float(m_fTimeLimit);
    F.close_chunk();
}

void CPGDef::Save2(CInifile& ini)
{
    ini.w_u16("_group", "version", PGD_VERSION);
    ini.w_u32("_group", "flags", m_Flags.get());
    ini.w_u32("_group", "effects_count", m_Effects.size());
    ini.w_float("_group", "timelimit", m_fTimeLimit);

    u32 counter = 0;
    string256 buff;
    for (EffectIt it = m_Effects.begin(); it != m_Effects.end(); ++it, ++counter)
    {
        xr_sprintf(buff, sizeof(buff), "effect_%04d", counter);
        ini.w_string(buff, "effect_name", (*it)->m_EffectName.c_str());
        ini.w_string(buff, "on_play_child", (*it)->m_Flags.test(SEffect::flOnPlayChild) ? (*it)->m_OnPlayChildName.c_str() : "");
        ini.w_string(buff, "on_birth_child", (*it)->m_Flags.test(SEffect::flOnBirthChild) ? (*it)->m_OnBirthChildName.c_str() : "");
        ini.w_string(buff, "on_death_child", (*it)->m_Flags.test(SEffect::flOnDeadChild) ? (*it)->m_OnDeadChildName.c_str() : "");
        ini.w_float(buff, "time0", (*it)->m_Time0);
        ini.w_float(buff, "time1", (*it)->m_Time1);
        ini.w_u32(buff, "flags", (*it)->m_Flags.get());
    }
}

#undef FBasicVisualH

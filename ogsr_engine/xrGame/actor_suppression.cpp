#include "StdAfx.h"
#include "actor_suppression.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "CameraEffector.h"

// How hard one bullet presses, from its closeness (1 = through the eyes)
float g_suppress_gain = .3f;
// how long the pressure holds after the last bullet, and how fast it drains then
float g_suppress_hold = .8f; // s
float g_suppress_decay = .35f; // per second
// the flinch of one bullet at point blank, and the tremor at full pressure
float g_suppress_flinch_deg = 1.2f;
float g_suppress_tremor_deg = .35f;
// the player's spread at full pressure: 1 + this
float g_suppress_disp = 1.f;

#define eCESuppression ((ECamEffectorType)(cefNext + 19))

extern int g_dbg_npc_tactics;

namespace
{
float s_level{}; // 0..1
u32 s_last_time{};
// the flinch: an angular kick that dies away in a fraction of a second
float s_kick_h{}, s_kick_p{}, s_kick_b{};
float s_time{}; // the tremor's own clock

class CSuppressionEffector : public CEffectorCam
{
public:
    CSuppressionEffector() : CEffectorCam(eCESuppression, 100000.f) {}

    BOOL ProcessCam(SCamEffectorInfo& info) override
    {
        const float kick = _abs(s_kick_h) + _abs(s_kick_p) + _abs(s_kick_b);
        if (s_level <= 0.f && kick < EPS_S)
        {
            fLifeTime = 0.f;
            return FALSE;
        }

        s_time += Device.fTimeDelta;

        // a tremor of two frequencies that do not line up, so it does not read as a wobble
        const float tremor = deg2rad(g_suppress_tremor_deg) * s_level;
        const float h = s_kick_h + tremor * (.6f * _sin(s_time * 17.f) + .4f * _sin(s_time * 29.f + 1.3f));
        const float p = s_kick_p + tremor * (.6f * _sin(s_time * 21.f + .7f) + .4f * _sin(s_time * 13.f + 2.1f));
        const float b = s_kick_b + tremor * .5f * _sin(s_time * 11.f + .4f);

        Fmatrix M;
        M.identity();
        M.j.set(info.n);
        M.k.set(info.d);
        M.i.crossproduct(info.n, info.d);
        M.c.set(info.p);

        Fmatrix R;
        R.setHPB(h, p, b);
        Fmatrix mR;
        mR.mul(M, R);
        info.d.set(mR.k);
        info.n.set(mR.j);
        return TRUE;
    }
};
} // namespace

void ActorSuppression::near_miss(float closeness_sum, u32 count)
{
    s_level = std::min(1.f, s_level + closeness_sum * g_suppress_gain);
    s_last_time = Device.dwTimeGlobal;

    // flinch away from nothing in particular: a random jolt, the closer the harder
    const float flinch = deg2rad(g_suppress_flinch_deg) * std::min(1.f, closeness_sum);
    s_kick_h += ::Random.randF(-flinch, flinch);
    s_kick_p += ::Random.randF(-flinch, flinch * .5f);
    s_kick_b += ::Random.randF(-flinch, flinch) * .5f;

    if (g_dbg_npc_tactics)
        Msg("[supp] %u bullet(s) close, closeness %.2f -> pressure %.2f", count, closeness_sum, s_level);
}

void ActorSuppression::update(CActor& actor)
{
    const float dt = Device.fTimeDelta;
    if (!actor.g_Alive())
        s_level = 0.f;
    else if (Device.dwTimeGlobal > s_last_time + u32(g_suppress_hold * 1000.f))
        s_level = std::max(0.f, s_level - g_suppress_decay * dt);

    const float fade = std::exp(-dt * 12.f);
    s_kick_h *= fade;
    s_kick_p *= fade;
    s_kick_b *= fade;

    const float kick = _abs(s_kick_h) + _abs(s_kick_p) + _abs(s_kick_b);
    if ((s_level > 0.f || kick > EPS_S) && !actor.Cameras().GetCamEffector(eCESuppression))
        actor.Cameras().AddCamEffector(xr_new<CSuppressionEffector>());
}

float ActorSuppression::dispersion_factor() { return 1.f + g_suppress_disp * s_level; }

float ActorSuppression::level() { return s_level; }

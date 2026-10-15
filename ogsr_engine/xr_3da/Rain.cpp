#include "stdafx.h"

#include "Rain.h"
#include "IGame_Persistent.h"
#include "Environment.h"

#include "../xr_3da/perlin.h"

#include "Render.h"
#include "IGame_Level.h"
#include "xr_object.h"

CPerlinNoise1D* RainPerlin;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CEffect_Rain::CEffect_Rain()
{
    state = stIdle;

    snd_Ambient.create("ambient\\rain", st_Effect, sg_Undefined);

    RainPerlin = xr_new<CPerlinNoise1D>(Random.randI(0, 0xFFFF));
    RainPerlin->SetOctaves(2);
    RainPerlin->SetAmplitude(0.66666f);

    p_create();
}

CEffect_Rain::~CEffect_Rain()
{
    xr_delete(RainPerlin);
    snd_Ambient.destroy();

    p_destroy();
}

void CEffect_Rain::Prepare(Fvector2& offset, Fvector3& axis, float W_Velocity, float W_Direction)
{
    // Wind gust, to add variation.
    float Wind_Gust = RainPerlin->GetContinious(Device.fTimeGlobal * 0.3f) * 2.0f;
    // Wind velocity [ 0 ~ 1 ]
    float Wind_Velocity = W_Velocity + Wind_Gust; // g_pGamePersistent->Environment().CurrentEnv->wind_velocity * 0.001f
    clamp(Wind_Velocity, 0.0f, 1.0f);
    // Wind velocity controles the angle
    float pitch = drop_max_angle * Wind_Velocity;
    axis.setHP(W_Direction, pitch - PI_DIV_2);
    // Get distance
    float dist = _sin(pitch) * source_offset;
    float C = PI_DIV_2 - pitch;
    dist /= _sin(C);
    // 0 is North
    float fixNorth = W_Direction - PI_DIV_2;
    // Set offset
    offset.set(dist * _cos(fixNorth), dist * _sin(fixNorth));
}

// Born
void CEffect_Rain::Born(Item& dest, float radius, float speed, collide::rq_target tgt)
{
    ZoneScoped;

    // Prepare correct angle and distance to hit the player
    Fvector Rain_Axis = {0, -1, 0};
    Fvector2 Rain_Offset;
    float Wind_Direction = -g_pGamePersistent->Environment().CurrentEnv->wind_direction;

    // Wind Velocity [ From 0 ~ 1000 to 0 ~ 1 ]
    float Wind_Velocity = g_pGamePersistent->Environment().CurrentEnv->wind_velocity * 0.001f;
    clamp(Wind_Velocity, 0.0f, 1.0f);
    Prepare(Rain_Offset, Rain_Axis, Wind_Velocity, Wind_Direction);
    // Camera Position
    Fvector& view = Device.vCameraPosition;

    // Random Position
    float r = radius * 0.5f;
    Fvector2 RandomP = {::Random.randF(-r, r), ::Random.randF(-r, r)};
    // Aim ahead of where the player is facing
    Fvector FinalView = Fvector().mad(view, Device.vCameraDirection, 5.0f);
    // Random direction. Higher angle at lower velocity
    dest.D.random_dir(Rain_Axis, ::Random.randF(-drop_angle, drop_angle) * (1.5f - Wind_Velocity));
    // Set final destination
    dest.P.set(Rain_Offset.x + FinalView.x + RandomP.x, source_offset + view.y, Rain_Offset.y + FinalView.z + RandomP.y);
    // Set speed
    dest.fSpeed = ::Random.randF(drop_speed_min, drop_speed_max) * speed * clampr(Wind_Velocity * 1.5f, 0.5f, 1.0f);
    // Born

    float height = max_distance;
    const BOOL b_hit = RayPickCached(dest.P, dest.D, height, tgt);
    RenewItem(dest, height, b_hit);
}

// 10-10 (see Rain.h): the top of the static geometry under a 0.5 m cell, one vertical ray the first time it is asked.
float CEffect_Rain::HeightAt(float x, float z, float top)
{
    if (hcells.empty())
        hcells.resize(kHCells);
    const s32 kx = iFloor(x * (1.f / kHCell)), kz = iFloor(z * (1.f / kHCell));
    HCell& c = hcells[(u32(kx) * 73856093u ^ u32(kz) * 19349663u) & (kHCells - 1)];
    if (c.kx == kx && c.kz == kz && top <= c.top + 5.f)   // a camera 5+ m higher may see a roof the old ray started under
        return c.y;
    ++hprobes;
    const Fvector s{(float(kx) + .5f) * kHCell, top, (float(kz) + .5f) * kHCell};
    float range = max_distance * 2.f;
    c.kx = kx;
    c.kz = kz;
    c.top = top;
    c.y = RayPick(s, Fvector{0.f, -1.f, 0.f}, range, collide::rqtStatic) ? top - range : kHNone;
    return c.y;
}

BOOL CEffect_Rain::RayPickCached(const Fvector& s, const Fvector& d, float& range, collide::rq_target tgt)
{
    // only what the cache can answer: the static level, a ray that falls
    if (!hcache || tgt != collide::rqtStatic || d.y > -0.2f)
        return RayPick(s, d, range, tgt);
    const float inv = -1.f / d.y;   // metres along the ray per metre of fall
    const float top = s.y;
    float t = range + 1.f;
    float h = HeightAt(s.x, s.z, top);
    if (h > kHNone)
        t = (s.y - h) * inv;
    // the slanted ray against the height field: where it crosses the surface under its own point (2-3 steps converge for
    // the drift a drop has; over an eave it settles on one side or the other, a splash 0.25 m off at worst)
    for (int i = 0; i < 3 && t <= range; ++i)
    {
        const float hx = HeightAt(s.x + d.x * t, s.z + d.z * t, top);
        const float tn = (hx > kHNone) ? (s.y - hx) * inv : range + 1.f;
        if (_abs(tn - t) < 0.1f)
        {
            t = tn;
            break;
        }
        t = tn;
    }
    if (t > range)
        return FALSE;
    range = t < 0.f ? 0.f : t;   // the surface above the start: the drop ends at once (RayPick would hit at ~0)
    return TRUE;
}

BOOL CEffect_Rain::RayPick(const Fvector& s, const Fvector& d, float& range, collide::rq_target tgt)
{
    ZoneScoped;

    collide::rq_result RQ{};
    CObject* E = g_pGameLevel->CurrentViewEntity();
    const BOOL bRes = g_pGameLevel->ObjectSpace.RayPick(s, d, range, tgt, RQ, E);
    if (bRes)
        range = RQ.range;
    return bRes;
}

void CEffect_Rain::RenewItem(Item& dest, float height, BOOL bHit)
{
    dest.uv_set = Random.randI(2);
    if (bHit)
    {
        dest.dwTime_Life = Device.dwTimeGlobal + iFloor(1000.f * height / dest.fSpeed) - Device.dwTimeDelta;
        dest.dwTime_Hit = Device.dwTimeGlobal + iFloor(1000.f * height / dest.fSpeed) - Device.dwTimeDelta;
        dest.Phit.mad(dest.P, dest.D, height);
    }
    else
    {
        dest.dwTime_Life = Device.dwTimeGlobal + iFloor(1000.f * height / dest.fSpeed) - Device.dwTimeDelta;
        dest.dwTime_Hit = Device.dwTimeGlobal + iFloor(2 * 1000.f * height / dest.fSpeed) - Device.dwTimeDelta;
        dest.Phit.set(dest.P);
    }
}

void CEffect_Rain::OnFrame()
{
    if (!g_pGameLevel)
        return;

    ZoneScoped;

    // Parse states
    float rain_density = g_pGamePersistent->Environment().CurrentEnv->rain_density;
    float wind_velocity = g_pGamePersistent->Environment().CurrentEnv->wind_velocity * 0.001f;
    clamp(wind_velocity, 0.0f, 1.0f);
    wind_velocity *= (rain_density > 0.0f ? 1.0f : 0.0f); // Only when raining
    // 50% of the volume is by rain_density and 50% wind_velocity;
    float factor = rain_density * 0.5f + wind_velocity * 0.5f;
    static float hemi_factor = 0.f;

    CObject* E = g_pGameLevel->CurrentViewEntity();
    if (E && E->renderable_ROS())
    {
        const float* hemi_cube = E->renderable_ROS()->get_luminocity_hemi_cube();
        float hemi_val = _max(hemi_cube[0], hemi_cube[1]);
        hemi_val = _max(hemi_val, hemi_cube[2]);
        hemi_val = _max(hemi_val, hemi_cube[3]);
        hemi_val = _max(hemi_val, hemi_cube[5]);

        float f = hemi_val;
        float t = Device.fTimeDelta;
        clamp(t, 0.001f, 1.0f);
        hemi_factor = hemi_factor * (1.0f - t) + f * t;
    }

    switch (state)
    {
    case stIdle:
        if (factor < EPS_L)
        {
            if (snd_Ambient._feedback())
                snd_Ambient.stop();
            return;
        }
        if (snd_Ambient._feedback())
        {
            snd_Ambient.stop();
            return;
        }
        snd_Ambient.play(nullptr, sm_Looped | sm_2D);
        if (snd_Ambient._feedback()) //Simp: в редких случаях такое случается
            snd_Ambient.set_position({});
        snd_Ambient.set_range(source_offset, source_offset * 2.f);
        state = stWorking;
        break;
    case stWorking:
        if (factor < EPS_L)
        {
            snd_Ambient.stop();
            state = stIdle;
            return;
        }
        break;
    }

    // ambient sound
    if (snd_Ambient._feedback())
    {
        snd_Ambient.set_volume(_max(0.1f, factor) * hemi_factor);
    }
}

void CEffect_Rain::Render(CBackend& cmd_list)
{
    if (!g_pGameLevel)
        return;

    m_pRender->Render(cmd_list, *this);
}

void CEffect_Rain::Calculate()
{
    if (!g_pGameLevel)
        return;

    m_pRender->Calculate(*this);
}

// startup _new_ particle system
void CEffect_Rain::Hit(Fvector& pos)
{
    if (::Random.randF() > 0.2f)
        return;

    Particle* P = p_allocate();
    if (nullptr == P)
        return;

    const Fsphere& bv_sphere = m_pRender->GetDropBounds();

    P->time = particles_time;
    P->mXForm.rotateY(::Random.randF(PI_MUL_2));
    P->mXForm.translate_over(pos);
    P->mXForm.transform_tiny(P->bounds.P, bv_sphere.P);
    P->bounds.R = bv_sphere.R;
}

// initialize particles pool
void CEffect_Rain::p_create()
{
    // pool
    particle_pool.resize(max_particles);
    for (u32 it = 0; it < particle_pool.size(); it++)
    {
        Particle& P = particle_pool[it];
        P.prev = it ? (&particle_pool[it - 1]) : nullptr;
        P.next = (it < (particle_pool.size() - 1)) ? (&particle_pool[it + 1]) : nullptr;
    }

    // active and idle lists
    particle_active = nullptr;
    particle_idle = &particle_pool.front();
}

// destroy particles pool
void CEffect_Rain::p_destroy()
{
    // active and idle lists
    particle_active = nullptr;
    particle_idle = nullptr;

    // pool
    particle_pool.clear();
}

// _delete_ node from _list_
void CEffect_Rain::p_remove(Particle* P, Particle*& LST)
{
    VERIFY(P);
    Particle* prev = P->prev;
    P->prev = nullptr;
    Particle* next = P->next;
    P->next = nullptr;
    if (prev)
        prev->next = next;
    if (next)
        next->prev = prev;
    if (LST == P)
        LST = next;
}

// insert node at the top of the head
void CEffect_Rain::p_insert(Particle* P, Particle*& LST)
{
    VERIFY(P);
    P->prev = nullptr;
    P->next = LST;
    if (LST)
        LST->prev = P;
    LST = P;
}

// determine size of _list_
int CEffect_Rain::p_size(Particle* P)
{
    if (nullptr == P)
        return 0;
    int cnt = 0;
    while (P)
    {
        P = P->next;
        cnt += 1;
    }
    return cnt;
}

// alloc node
CEffect_Rain::Particle* CEffect_Rain::p_allocate()
{
    Particle* P = particle_idle;
    if (nullptr == P)
        return nullptr;
    p_remove(P, particle_idle);
    p_insert(P, particle_active);
    return P;
}

// xr_free node
void CEffect_Rain::p_free(Particle* P)
{
    p_remove(P, particle_active);
    p_insert(P, particle_idle);
}

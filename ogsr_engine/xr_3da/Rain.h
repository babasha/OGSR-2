// Rain.h: interface for the CRain class.
//
//////////////////////////////////////////////////////////////////////
#pragma once

#include "../xrCDB/xr_collide_defs.h"

inline const float max_desired_items = std::thread::hardware_concurrency() < 3u ? 1000.f : 2000.f;
inline const float min_desired_items = max_desired_items / 10.f;

constexpr float source_radius = 15;//12.5f;
constexpr float source_offset = 40.f; // 40

constexpr float max_distance = source_offset * 1.5f; // 1.25f;
constexpr float sink_offset = -(max_distance - source_offset);

constexpr float drop_length = 7.f;
constexpr float drop_width = 0.40f;
constexpr float drop_angle = deg2rad(15.0f); // 3.0
constexpr float drop_max_angle = deg2rad(35.f); // 10;
constexpr float drop_max_wind_vel = 20.0f;
constexpr float drop_speed_min = 40.f;
constexpr float drop_speed_max = 80.f;

constexpr int max_particles = 1000;
constexpr int particles_cache = 400;

constexpr float particles_time = .3f;

// refs
class ENGINE_API IRender_DetailModel;

#include "../Include/xrRender/FactoryPtr.h"
#include "../Include/xrRender/RainRender.h"
//
class ENGINE_API CEffect_Rain
{
    friend class vkRainRender;

private:
    struct Item
    {
        Fvector P;
        Fvector Phit;
        Fvector D;
        float fSpeed;
        u32 dwTime_Life;
        u32 dwTime_Hit;
        u32 uv_set;
        void invalidate() { dwTime_Life = 0; }
    };
    struct Particle
    {
        Particle *next, *prev;
        Fmatrix mXForm;
        Fsphere bounds;
        float time;
    };
    enum States
    {
        stIdle = 0,
        stWorking
    };

private:
    // Visualization	(rain) and (drops)
    FactoryPtr<IRainRender> m_pRender;

    // Data and logic
    xr_vector<Item> items;
    States state;

    // Particles
    xr_vector<Particle> particle_pool;
    Particle* particle_active;
    Particle* particle_idle;

    // Sounds
    ref_sound snd_Ambient;

    // Utilities
    void p_create();
    void p_destroy();

    void p_remove(Particle* P, Particle*& LST);
    void p_insert(Particle* P, Particle*& LST);
    int p_size(Particle* LST);
    Particle* p_allocate();
    void p_free(Particle* P);

    // Some methods
    // tgt: what the birth ray tests (10-10: the Vulkan renderer passes rqtStatic under r_rain_rq_static -- the dynamic objects
    // were ~0.13 ms a ray on the Switch, ~17 births a frame)
    void Born(Item& dest, float radius, float speed, collide::rq_target tgt = collide::rqtBoth);
    void Hit(Fvector& pos);
    BOOL RayPick(const Fvector& s, const Fvector& d, float& range, collide::rq_target tgt);
    void RenewItem(Item& dest, float height, BOOL bHit);

    // 10-10 the height cache (r_rain_hcache; the Vulkan renderer sets hcache each Calculate): a drop's ray against the STATIC
    // level is answered from the top surface of 0.5 m cells, each found once by a vertical ray -- the static world does not
    // move, and the births + wrap rays were ~5.8 % of the main thread in a big rainy firefight on the Switch (~1 ms).
    // RayPickCached: the slanted drop ray meets that height field (2-3 fixed-point steps); anything else -> RayPick.
    struct HCell
    {
        s32 kx = 0x7fffffff, kz = 0;
        float y = 0.f;      // the top of the static geometry, or kHNone
        float top = 0.f;    // the height the vertical ray was cast from (a camera far above it asks again)
    };
    static constexpr u32 kHCells = 1u << 16;
    static constexpr float kHCell = 0.5f;
    static constexpr float kHNone = -1e30f;
    xr_vector<HCell> hcells;
    u32 hprobes = 0;        // vertical rays cast (cache misses), for the renderer's stats
    float HeightAt(float x, float z, float top);
    BOOL RayPickCached(const Fvector& s, const Fvector& d, float& range, collide::rq_target tgt);

public:
    bool hcache = false;    // r_rain_hcache (set by the renderer)

private:
    void Prepare(Fvector2& offset, Fvector3& axis, float Wind_Vel, float Wind_Dir);

public:
    CEffect_Rain();
    ~CEffect_Rain();

    void Render(CBackend& cmd_list);
    void Calculate();
    void OnFrame();
    void InvalidateState() { state = stIdle; }
};

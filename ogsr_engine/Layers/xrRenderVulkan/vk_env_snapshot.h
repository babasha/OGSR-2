#pragma once
#include "../../xrCore/render_thread.h"
// RENDER_THREAD_PLAN step 3: the per-frame ENVIRONMENT SNAPSHOT.
//
// CEnvironment::OnFrame rewrites CurrentEnv in place every frame on the main thread (and
// scripts / the thunderbolt overwrite single fields of it). The recording passes read this
// copy instead: ES() has the same member names as CEnvironment (CurrentEnv, Current[2],
// wetness_factor, wind_anim, IsThunderboltActive(), ThunderboltFlash()), so a pass changes
// `g_pGamePersistent->Environment()` into `ES()` and nothing else.
//   * Filled at the top of CRender::Begin (next to RV().Capture()), from the logic-phase state.
//   * Texture names are std::string, not shared_str: a shared_str copy touches the source's
//     refcount, which the render thread must not do.
//   * Do not copy an EnvSnapshot: CurrentEnv/Current[] point into it.
// Pointers are null while the environment does not exist (loading, menu) -- same as the live one.
struct EnvDescSnap
{
    float exec_time = 0.f;
    std::string sky_texture_name, sky_texture_env_name, clouds_texture_name, m_identifier, lens_flare_id, tb_id;
    Fvector4 clouds_color{};
    Fvector3 sky_color{};
    float sky_rotation = 0.f, far_plane = 0.f, far_plane_render = 0.f;
    Fvector3 fog_color{};
    float fog_density = 0.f, fog_distance = 0.f;
    float rain_density = 0.f;
    Fvector3 rain_color{};
    float bolt_period = 0.f, bolt_duration = 0.f;
    float wind_velocity = 0.f, wind_velocity_old = 0.f, wind_direction = 0.f, wind_direction_old = 0.f;
    Fvector3 ambient{};
    Fvector4 hemi_color{}, hemi_color_unmodded{};
    Fvector3 sun_color{}, sun_dir{};
    float m_fSunShaftsIntensity = 0.f, m_fWaterIntensity = 0.f, m_fTreeAmplitudeIntensity = 0.f, m_fTreeAmplitudeIntensity_old = 0.f;
    float bloom_threshold = 0.f, bloom_exposure = 0.f, bloom_sky_intensity = 0.f;
    float weight = 0.f, fog_near = 0.f, fog_far = 0.f;   // mixer-only
};

struct EnvSnapshot
{
    const EnvDescSnap* CurrentEnv = nullptr;
    const EnvDescSnap* Current[2] = {nullptr, nullptr};
    float wetness_factor = 0.f;
    Fvector3 wind_anim{};
    bool thunderActive = false;
    Fvector thunderFlash{0.f, 0.f, 0.f};
    // The sun direction held steady through a thunderbolt (see EnvLight::SunDirVisual): the "last real sun" latch lives in
    // Capture (main thread) and the held value travels with the snapshot, so every pass of a frame sees the same sun.
    Fvector sunDirVisual{0.f, -1.f, 0.f};

    bool IsThunderboltActive() const { return thunderActive; }
    const Fvector& ThunderboltFlash() const { return thunderFlash; }

    void Capture();   // from g_pGamePersistent->Environment(), at the top of CRender::Begin

private:
    EnvDescSnap m_cur, m_c0, m_c1;
};

inline RT::PerSlot<EnvSnapshot> g_EnvSnapshot;   // one per frame slot, built in place (it points into itself)
inline EnvSnapshot& ES() { return g_EnvSnapshot.cur(); }

#pragma once

// ⭐The Switch's sound backend on audren (11-10-2026, "layer B"; plan in memory switch-audren-backend-plan).
//
// 📏Switch, OpenAL (10/11-10): the mix took ~12% of core 2 (2.2-2.8 ms per 21 ms buffer), and a sound START cost the
// sound thread 8-11 ms -- half of it opening the .ogg (FS + vorbis headers), the rest decoding the first block. The
// console has an audio DSP that mixes voices and decodes DSP-ADPCM by itself. Here: the same xrSound above
// (emitters, priorities, occlusion, snd_prop, HDR, auto-reverb parameters), and below it the DSP instead of OpenAL --
// mixing, resampling, pitch, the occlusion low-pass, and the short sounds' decode (.nxa, see SoundRender_Adpcm.h)
// for free; the 3D panning/attenuation OpenAL did is a few multiplies per target (CSoundRender_TargetNX).
//
// Chosen at start (sound.cpp): the Switch takes this one; `-snd_al` in the args = OpenAL, for A/B.
// Memory the DSP reads: the arena (snd_nx_arena_mb; resident .nxa payloads, least recently used out when a sound
// that is not resident starts) and 96 KB of PCM16 blocks per target.
#ifdef __SWITCH__

#include "SoundRender_Core.h"
#include "SoundRender_NxAudren.h"
#include "SoundRender_Environment.h"

#include <atomic>
#include <mutex>

class CSoundRender_TargetNX;
class PinnedWorker;

class CSoundRender_CoreNX : public CSoundRender_Core
{
    typedef CSoundRender_Core inherited;

public:
    struct Listener
    {
        Fvector position{}, right{1.f, 0.f, 0.f}, up{0.f, 1.f, 0.f}, dir{0.f, 0.f, 1.f};
    };

private:
    Listener m_listener;

    volatile float m_master{1.f};
    float m_master_set{-1.f};

    // the arena: offsets of free runs (sorted), 64-byte granules
    std::mutex m_arena_lock;
    struct Run
    {
        u32 off, size;
    };
    xr_vector<Run> m_free;
    xr_vector<CSoundRender_Source*> m_resident;
    u64 m_stamp{};
    u8* arena_alloc(u32 bytes);
    void arena_free(u8* p, u32 bytes);
    bool evict_one(); // the least recently used payload nobody plays

    // [nx audren] once per 5 s
    nxau::Stats m_stats{};
    float m_report_at{};
    u32 m_direct_starts{}, m_stream_starts{}, m_demand_loads{}, m_evictions{}, m_kept{}, m_not_kept{}, m_reverb_sets{};
    float m_demand_ms{}, m_commit_ms_sum{}, m_commit_ms_max{};
    void report();
    PinnedWorker* m_prefetch{};
    std::atomic<u32> m_prefetch_queued{}, m_prefetch_done{}, m_prefetch_us{};

protected:
    virtual void i_eax_set(const GUID*, u32, void*, u32) {}
    virtual void i_eax_get(const GUID*, u32, void*, u32) {}
    virtual void update_listener(const Fvector& P, const Fvector& D, const Fvector& N, float dt);
    virtual void i_commit();
    virtual void i_native_env(const CSoundRender_Environment* E);

    // the room (i_native_env) -> the DSP's reverb, a few times a second when it changed
    CSoundRender_Environment m_env;
    bool m_env_on{}, m_env_dirty{}, m_env_sent_on{};
    float m_env_next{}; // fTimer_Value
    nxau::Reverb m_rev_sent{};
    float m_wet_rolloff{}; // the room's RoomRolloffFactor: the send's distance attenuation (as EFX)

public:
    CSoundRender_CoreNX();
    virtual ~CSoundRender_CoreNX();

    virtual void _initialize(int stage);
    virtual void _clear();
    virtual void _restart();

    virtual void set_master_volume(float f) { m_master = f; }
    virtual const Fvector& listener_position() { return m_listener.position; }
    const Listener& listener() const { return m_listener; }
    bool reverb_on() const { return m_env_sent_on; }
    float wet_rolloff() const { return m_wet_rolloff; }

    virtual void nxa_keep(CSoundRender_Source* S, const u8* payload);
    virtual void nxa_drop(CSoundRender_Source* S);
    virtual const u8* nxa_payload_for_cpu(CSoundRender_Source* S, CSoundRender_Target* T);
    // a target starts / stops playing S's payload (DIRECT): true = resident and held; false = not (yet) -- it is loaded behind
    virtual void nxa_prefetch(CSoundRender_Source* S) { nxa_prefetch(S, false); }
    void nxa_prefetch(CSoundRender_Source* S, bool evict); // evict: room among the payloads nobody plays
    bool nxa_acquire(CSoundRender_Source* S);
    void nxa_release(CSoundRender_Source* S);
    void count_start(bool direct) { (direct ? m_direct_starts : m_stream_starts)++; }
};

#endif // __SWITCH__

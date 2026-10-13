#include "stdafx.h"

#ifdef __SWITCH__
#include "SoundRender_CoreNX.h"
#include "SoundRender_TargetNX.h"
#include "SoundRender_Source.h"
#include "SoundRender_Adpcm.h"
#include "../xrCore/pinned_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
constexpr u32 k_granule = 64; // audren wants 0x40-aligned wave data
constexpr u32 k_slot_bytes = 96 * 1024; // = TargetNX's

void log_to_msg(const char* s) { Msg("%s", s); }
} // namespace

CSoundRender_CoreNX::CSoundRender_CoreNX() { supports_float_pcm = false; }
CSoundRender_CoreNX::~CSoundRender_CoreNX() {}

void CSoundRender_CoreNX::_initialize(int stage)
{
    if (stage == 0)
        return;

    supports_float_pcm = false; // the voices take PCM16 (and the cache is half the size per second)
    nxau::set_log(log_to_msg);
    const int targets = _min(_max(snd_nx_voices, 16), _max(psSoundTargets, 16));
    if (!nxau::init(targets, size_t(snd_nx_arena_mb) * 1024 * 1024, k_slot_bytes, snd_nx_reverb != 0))
    {
        Msg("! SOUND: audren failed to start -- no sound");
        bPresent = FALSE;
        return;
    }
    {
        std::scoped_lock lk(m_arena_lock);
        m_free.clear();
        m_free.push_back({0, u32(nxau::arena_size())});
    }

    inherited::_initialize(stage);

    if (stage == 1)
    {
        for (int t = 0; t < targets; ++t)
        {
            CSoundRender_Target* T = xr_new<CSoundRender_TargetNX>(this, t);
            T->_initialize();
            s_targets.push_back(T);
        }
    }
    bNativeEnv = nxau::reverb_available();
    Msg("~ SOUND: audren backend, %d targets, arena %d MB, snd_nxa %d, reverb %s", targets, snd_nx_arena_mb, snd_nxa, bNativeEnv ? "DSP I3DL2" : "off");
    bReady = TRUE;
}

void CSoundRender_CoreNX::_clear()
{
    bReady = FALSE;
    if (m_prefetch)
        m_prefetch->Wait(); // its jobs point at sources
    CSoundRender_TargetNX::wait_openers();
    inherited::_clear(); // sources (their payloads back to the arena) and emitters
    for (CSoundRender_Target* T : s_targets)
    {
        T->_destroy();
        xr_delete(T);
    }
    s_targets.clear();
    nxau::shutdown();
}

void CSoundRender_CoreNX::_restart() { inherited::_restart(); }

void CSoundRender_CoreNX::update_listener(const Fvector& P, const Fvector& D, const Fvector& N, float dt)
{
    inherited::update_listener(P, D, N, dt);
    if (!m_listener.position.similar(P))
        bListenerMoved = TRUE;
    m_listener.position = P;
    m_listener.dir = D;
    m_listener.up = N;
    m_listener.right.crossproduct(N, D); // X-Ray is left-handed: up x forward = right
    if (m_listener.right.square_magnitude() > EPS_S)
        m_listener.right.normalize();
}

void CSoundRender_CoreNX::i_native_env(const CSoundRender_Environment* E)
{
    m_env_on = E != nullptr;
    if (E)
    {
        m_env = *E;
        m_env_dirty = true;
    }
}

namespace
{
// the engine's room (EAX 2 units, the same set EFX gets in i_efx_listener_set) -> the DSP's I3DL2, in its ranges
nxau::Reverb to_i3dl2(const CSoundRender_Environment& E)
{
    nxau::Reverb r;
    r.room_mb = std::clamp(E.Room, -10000.f, 0.f);
    r.room_hf_mb = std::clamp(E.RoomHF, -10000.f, 0.f);
    r.decay_time = std::clamp(E.DecayTime, 0.1f, 20.f);
    r.decay_hf_ratio = std::clamp(E.DecayHFRatio, 0.1f, 2.f);
    r.reflections_mb = std::clamp(E.Reflections, -10000.f, 1000.f);
    r.reflections_delay = std::clamp(E.ReflectionsDelay, 0.f, 0.3f);
    r.reverb_mb = std::clamp(E.Reverb, -10000.f, 2000.f);
    r.reverb_delay = std::clamp(E.ReverbDelay, 0.f, 0.1f);
    r.diffusion = std::clamp(E.EnvironmentDiffusion * 100.f, 0.f, 100.f);
    // density from the size, as the EFX path does (openal.org, 2014-03)
    r.density = std::clamp(powf(E.EnvironmentSize, 3.f) / 16.f, 0.f, 1.f) * 100.f;
    r.hf_reference = 5000.f;
    return r;
}

bool reverb_differs(const nxau::Reverb& a, const nxau::Reverb& b)
{
    auto mb = [](float x, float y) { return std::fabs(x - y) > 50.f; };
    auto rel = [](float x, float y) { return std::fabs(x - y) > 0.05f * _max(std::fabs(x), std::fabs(y)) + 1e-3f; };
    return mb(a.room_mb, b.room_mb) || mb(a.room_hf_mb, b.room_hf_mb) || mb(a.reflections_mb, b.reflections_mb) || mb(a.reverb_mb, b.reverb_mb) ||
        rel(a.decay_time, b.decay_time) || rel(a.decay_hf_ratio, b.decay_hf_ratio) || rel(a.reflections_delay, b.reflections_delay) ||
        rel(a.reverb_delay, b.reverb_delay) || std::fabs(a.diffusion - b.diffusion) > 3.f || std::fabs(a.density - b.density) > 3.f;
}
} // namespace

void CSoundRender_CoreNX::i_commit()
{
    if (!nxau::ready())
        return;
    const auto t0 = std::chrono::steady_clock::now();

    if (snd_nx_capture > 0.f)
    {
        if (!nxau::capture(snd_nx_capture))
            Msg("! [nx audren] capture busy or out of memory");
        snd_nx_capture = 0.f;
    }

    // ⭐the room: the engine lerps e_current every update; the DSP re-reads the parameters (and resets its feedback
    // state) whenever told to, so it is told at most 4 times a second and only when something moved noticeably
    if (nxau::reverb_available() && (m_env_on != m_env_sent_on || (m_env_dirty && fTimer_Value >= m_env_next)))
    {
        m_env_next = fTimer_Value + .25f;
        m_env_dirty = false;
        nxau::Reverb r = to_i3dl2(m_env);
        extern int snd_nx_rev_test;
        if (snd_nx_rev_test)
        {
            // a big hall (I3DL2 "hangar"-ish, loud tail): proves the effect by ear or capture whatever the room
            r.room_mb = 0.f, r.room_hf_mb = -100.f, r.decay_time = 6.f, r.decay_hf_ratio = 0.8f, r.reflections_mb = 0.f;
            r.reflections_delay = 0.02f, r.reverb_mb = 1000.f, r.reverb_delay = 0.03f, r.diffusion = 100.f, r.density = 100.f;
        }
        if (m_env_on != m_env_sent_on || reverb_differs(r, m_rev_sent))
        {
            nxau::reverb_set(r, m_env_on);
            m_rev_sent = r;
            m_env_sent_on = m_env_on;
            ++m_reverb_sets;
        }
        m_wet_rolloff = std::clamp(m_env.RoomRolloffFactor, 0.f, 10.f);
    }
    const float master = m_master;
    if (master != m_master_set)
    {
        m_master_set = master;
        nxau::master_volume(master);
    }
    for (CSoundRender_Target* T : s_targets)
        static_cast<CSoundRender_TargetNX*>(T)->commit();
    nxau::update(m_stats);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    m_commit_ms_sum += ms;
    m_commit_ms_max = _max(m_commit_ms_max, ms);
    report();
}

void CSoundRender_CoreNX::report()
{
    // on the sound timer (fTimer_Value, seconds)
    if (fTimer_Value >= m_report_at && fTimer_Value < m_report_at + 5.f)
        return;
    m_report_at = fTimer_Value;
    u32 live = 0, direct = 0;
    for (CSoundRender_Target* T : s_targets)
    {
        auto* N = static_cast<CSoundRender_TargetNX*>(T);
        if (N->live())
            ++live, direct += N->direct() ? 1 : 0;
    }
    u32 resident_kb = 0, resident_n = 0;
    {
        std::scoped_lock lk(m_arena_lock);
        u32 free_b = 0;
        for (const Run& r : m_free)
            free_b += r.size;
        resident_kb = u32((nxau::arena_size() - free_b) / 1024);
        resident_n = u32(m_resident.size());
    }
    const u32 n = _max(m_stats.updates, 1u);
    Msg("~ [nx audren] commit %.2f ms avg %.2f max (renderer update %.2f avg %.2f max, its IPC %.2f avg %.2f max) | voices live %u (direct %u) | starts direct %u stream %u | "
        "arena %u KB in %u (kept %u, not %u) | not resident at start %u (%.1f ms), evicted %u | dsp played %.1f voice-s, drops %u, update fails %u | "
        "reverb %s (state %u, %u sets, decay %.2f room %.0f)",
        m_commit_ms_sum / n, m_commit_ms_max, m_stats.update_ms_sum / n, m_stats.update_ms_max, m_stats.ipc_ms_sum / _max(m_stats.ipc_n, 1u), m_stats.ipc_ms_max, live, direct, m_direct_starts, m_stream_starts, resident_kb,
        resident_n, m_kept, m_not_kept, m_demand_loads, m_demand_ms, m_evictions, double(m_stats.played) / 44100.0, m_stats.dsp_drops, m_stats.failures,
        !nxau::reverb_available() ? "n/a" : (m_env_sent_on ? "on" : "off"), m_stats.reverb_state, m_reverb_sets, m_rev_sent.decay_time, m_rev_sent.room_mb);
    m_reverb_sets = 0;
    // sources made (any thread) and the background payload reads
    {
        const u32 hits = g_snd_sources.hits.exchange(0), loads = g_snd_sources.loads.exchange(0), lus = g_snd_sources.load_us.exchange(0);
        const u32 pq = m_prefetch_queued.exchange(0), pd = m_prefetch_done.exchange(0), pus = m_prefetch_us.exchange(0);
        const u32 ao = CSoundRender_TargetNX::s_async_opens.exchange(0), aus = CSoundRender_TargetNX::s_async_open_us.exchange(0);
        if (hits || loads || pq || pd || ao)
            Msg("~ [nx audren] sources: %u created (%.1f ms, %.2f each), %u found | payload prefetch: %u queued, %u read (%.1f ms on core 2) | stream opens off-thread %u (%.1f ms)", loads,
                lus / 1000.f, loads ? lus / 1000.f / loads : 0.f, hits, pq, pd, pus / 1000.f, ao, aus / 1000.f);
    }
    m_stats = {};
    m_commit_ms_sum = m_commit_ms_max = 0.f;
    m_direct_starts = m_stream_starts = m_demand_loads = m_evictions = 0;
    m_demand_ms = 0.f;
}

// ---- the arena ----------------------------------------------------------------------------------------------

u8* CSoundRender_CoreNX::arena_alloc(u32 bytes)
{
    bytes = (bytes + k_granule - 1) & ~(k_granule - 1);
    for (size_t i = 0; i < m_free.size(); ++i)
    {
        Run& r = m_free[i];
        if (r.size < bytes)
            continue;
        const u32 off = r.off;
        r.off += bytes;
        r.size -= bytes;
        if (!r.size)
            m_free.erase(m_free.begin() + i);
        return nxau::arena_base() + off;
    }
    return nullptr;
}

void CSoundRender_CoreNX::arena_free(u8* p, u32 bytes)
{
    bytes = (bytes + k_granule - 1) & ~(k_granule - 1);
    const u32 off = u32(p - nxau::arena_base());
    auto it = std::lower_bound(m_free.begin(), m_free.end(), off, [](const Run& r, u32 o) { return r.off < o; });
    it = m_free.insert(it, Run{off, bytes});
    // merge with the next, then the previous
    if (it + 1 != m_free.end() && it->off + it->size == (it + 1)->off)
    {
        it->size += (it + 1)->size;
        m_free.erase(it + 1);
    }
    if (it != m_free.begin() && (it - 1)->off + (it - 1)->size == it->off)
    {
        (it - 1)->size += it->size;
        m_free.erase(it);
    }
}

bool CSoundRender_CoreNX::evict_one()
{
    size_t best = size_t(-1);
    for (size_t i = 0; i < m_resident.size(); ++i)
    {
        CSoundRender_Source* S = m_resident[i];
        if (S->m_nxa_users > 0)
            continue;
        if (best == size_t(-1) || S->m_nxa_stamp < m_resident[best]->m_nxa_stamp)
            best = i;
    }
    if (best == size_t(-1))
        return false;
    CSoundRender_Source* S = m_resident[best];
    arena_free(S->m_nxa_payload, S->m_nxa_payload_size);
    S->m_nxa_payload = nullptr;
    m_resident[best] = m_resident.back();
    m_resident.pop_back();
    ++m_evictions;
    return true;
}

// a source is created (level load, or a first create at run time): keep its payload if there is room -- never
// evicting for it, the sounds played so far matter more than the ones merely declared
void CSoundRender_CoreNX::nxa_keep(CSoundRender_Source* S, const u8* payload)
{
    if (!nxau::ready())
        return;
    std::scoped_lock lk(m_arena_lock);
    u8* p = arena_alloc(S->m_nxa_payload_size);
    if (!p)
    {
        ++m_not_kept;
        return;
    }
    CopyMemory(p, payload, S->m_nxa_payload_size);
    nxau::flush(p, S->m_nxa_payload_size);
    S->m_nxa_payload = p;
    S->m_nxa_stamp = ++m_stamp;
    m_resident.push_back(S);
    ++m_kept;
}

void CSoundRender_CoreNX::nxa_drop(CSoundRender_Source* S)
{
    std::scoped_lock lk(m_arena_lock);
    if (!S->m_nxa_payload)
        return;
    arena_free(S->m_nxa_payload, S->m_nxa_payload_size);
    S->m_nxa_payload = nullptr;
    auto it = std::find(m_resident.begin(), m_resident.end(), S);
    if (it != m_resident.end())
    {
        *it = m_resident.back();
        m_resident.pop_back();
    }
}

const u8* CSoundRender_CoreNX::nxa_payload_for_cpu(CSoundRender_Source* S, CSoundRender_Target* T)
{
    // a target streams an .nxa only when it was not resident at its start: always its own open file -- the payload
    // may arrive (nxa_prefetch) and be evicted again while it plays
    return T->get_nxa_file();
}

// ⭐a source made from the index (no file opened when it was created): its payload is read here, on a background
// thread of core 2, into the arena if there is room -- the spawn that created it does not wait for the SD
void CSoundRender_CoreNX::nxa_prefetch(CSoundRender_Source* S, bool evict)
{
    if (!nxau::ready() || S->m_nxa_payload_size > nxau::arena_size())
        return;
    if (!m_prefetch)
        m_prefetch = xr_new<PinnedWorker>(2, 0x3B); // background priority; leaked like the other pinned workers
    m_prefetch_queued.fetch_add(1, std::memory_order_relaxed);
    m_prefetch->Run([this, S, evict] {
        const auto t0 = std::chrono::steady_clock::now();
        {
            std::scoped_lock lk(m_arena_lock);
            if (S->m_nxa_payload)
                return;
        }
        IReader* F = FS.r_open(S->pname.c_str());
        if (!F)
            return;
        if (F->length() >= sizeof(nxa::FileHeader) + S->m_nxa_payload_size)
        {
            std::scoped_lock lk(m_arena_lock);
            if (!S->m_nxa_payload)
            {
                u8* p = arena_alloc(S->m_nxa_payload_size);
                while (!p && evict && evict_one())
                    p = arena_alloc(S->m_nxa_payload_size);
                if (p)
                {
                    CopyMemory(p, static_cast<const u8*>(F->begin()) + sizeof(nxa::FileHeader), S->m_nxa_payload_size);
                    nxau::flush(p, S->m_nxa_payload_size);
                    S->m_nxa_payload = p;
                    S->m_nxa_stamp = ++m_stamp;
                    m_resident.push_back(S);
                    ++m_kept;
                }
                else
                    ++m_not_kept;
            }
        }
        FS.r_close(F);
        m_prefetch_done.fetch_add(1, std::memory_order_relaxed);
        m_prefetch_us.fetch_add(u32(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()),
                                std::memory_order_relaxed);
    });
}

bool CSoundRender_CoreNX::nxa_acquire(CSoundRender_Source* S)
{
    {
        std::scoped_lock lk(m_arena_lock);
        if (S->m_nxa_payload)
        {
            S->m_nxa_users++;
            S->m_nxa_stamp = ++m_stamp;
            return true;
        }
    }
    // not resident: this play streams it (CPU decode, its file opened off-thread) and the payload comes in behind,
    // making room among the ones nobody plays -- the sound thread never waits for the SD
    ++m_demand_loads;
    nxa_prefetch(S, true);
    return false;
}

void CSoundRender_CoreNX::nxa_release(CSoundRender_Source* S)
{
    std::scoped_lock lk(m_arena_lock);
    if (S->m_nxa_users > 0)
        S->m_nxa_users--;
}

#endif // __SWITCH__

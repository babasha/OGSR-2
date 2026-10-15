#include "stdafx.h"

#ifdef __SWITCH__
#include "SoundRender_TargetNX.h"
#include "SoundRender_CoreNX.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Source.h"
#include "SoundRender_Adpcm.h"

#include <chrono>
#include <cmath>

#include "../xrCore/pinned_worker.h"

extern float psSoundRolloff;
extern float psSoundTimeFactor;

namespace
{
constexpr u32 k_slot_bytes = 96 * 1024; // CSoundRender_CoreNX passes the same to nxau::init

// EFX's low-pass (AL_LOWPASS_GAINHF) is a high shelf at 5 kHz; RBJ's, shelf slope 1, for the DSP's 48 kHz
void high_shelf(float gain, float out[5])
{
    const float A = _sqrt(_max(gain, 1e-4f));
    const float w0 = 2.f * PI * 5000.f / 48000.f;
    const float cw = _cos(w0), sw = _sin(w0);
    const float alpha = sw * 0.5f * _sqrt(2.f);
    const float sA = _sqrt(A);
    const float b0 = A * ((A + 1.f) + (A - 1.f) * cw + 2.f * sA * alpha);
    const float b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cw);
    const float b2 = A * ((A + 1.f) + (A - 1.f) * cw - 2.f * sA * alpha);
    const float a0 = (A + 1.f) - (A - 1.f) * cw + 2.f * sA * alpha;
    const float a1 = 2.f * ((A - 1.f) - (A + 1.f) * cw);
    const float a2 = (A + 1.f) - (A - 1.f) * cw - 2.f * sA * alpha;
    out[0] = b0 / a0;
    out[1] = b1 / a0;
    out[2] = b2 / a0;
    out[3] = a1 / a0;
    out[4] = a2 / a0;
}
} // namespace

CSoundRender_TargetNX::CSoundRender_TargetNX(CSoundRender_CoreNX* core, int index) : m_index(index), m_core(core) {}
CSoundRender_TargetNX::~CSoundRender_TargetNX() {}

BOOL CSoundRender_TargetNX::_initialize() { return inherited::_initialize(); }

void CSoundRender_TargetNX::_destroy()
{
    for (int s = 0; s < 2; ++s)
        nxau::voice_drop(nxau::voice_id(m_index, s));
    m_live = false;
    if (m_held)
        m_core->nxa_release(m_held);
    m_held = nullptr;
}

void CSoundRender_TargetNX::_restart() {}

void CSoundRender_TargetNX::start(CSoundRender_Emitter* E)
{
    inherited::start(E);

    CSoundRender_Source* src = E->source();
    m_mode = mStream;
    // DIRECT: an .nxa whose payload is (or now gets) resident; attached tails switch sources mid-play -- stream those
    if (src->m_nxa && !(E->owner_data && E->owner_data->fn_attached[0].size()))
    {
        if (m_held)
            m_core->nxa_release(m_held), m_held = nullptr;
        if (m_core->nxa_acquire(src))
        {
            m_held = src;
            m_mode = mDirect;
        }
    }
    m_core->count_start(m_mode == mDirect);

    if (m_mode == mStream)
    {
        const u32 align = src->m_wformat.nBlockAlign;
        m_block_bytes = (k_slot_bytes / blocks) / align * align;
    }
}

void CSoundRender_TargetNX::fill_block_at(u32 block)
{
    u8* dst = nxau::stream_slot(m_index) + block * (k_slot_bytes / blocks);
    m_pEmitter->fill_block(dst, m_block_bytes);
    nxau::flush(dst, m_block_bytes);
}

void CSoundRender_TargetNX::fill_first_blocks()
{
    // as TargetA: snd_start_blocks decoded now, the rest one per update
    const u32 first = u32(_min(_max(snd_start_blocks, 1), int(blocks)));
    for (u32 b = 0; b < first; ++b)
        fill_block_at(b);
    m_ready = first;
    m_next = first % blocks;
    m_play_req = true;
}

PinnedWorker* CSoundRender_TargetNX::opener()
{
    static PinnedWorker* w = xr_new<PinnedWorker>(2, 0x3B); // background priority on core 2; leaked like the others
    return w;
}

void CSoundRender_TargetNX::wait_openers() { opener()->Wait(); }

void CSoundRender_TargetNX::render()
{
    if (m_mode == mStream)
    {
        // ⭐a stream whose file is not open yet: open it on a worker (📏Switch au4: a long .ogg's start was 12-24 ms of
        // the sound thread, 8-17 of it the open -- the whole file read from the SD archive and its vorbis headers).
        // update() fills and starts it when it is in, an update or two later.
        if (!wave && !m_waiting_open)
        {
            CSoundRender_Source* src = m_pEmitter->source();
            u32 gen;
            {
                std::scoped_lock lk(m_open_lock);
                gen = m_open_gen;
            }
            m_waiting_open = true;
            opener()->Run([this, src, gen] {
                const auto t0 = std::chrono::steady_clock::now();
                OggVorbis_File* o = src->m_nxa ? nullptr : xr_new<OggVorbis_File>();
                IReader* w = open_source(src, o);
                std::scoped_lock lk(m_open_lock);
                if (gen == m_open_gen && !m_opened)
                {
                    m_opened_w = w;
                    m_opened_ovf = o;
                    m_opened = true;
                }
                else
                {
                    if (o)
                    {
                        ov_clear(o);
                        xr_delete(o);
                    }
                    FS.r_close(w);
                }
                s_async_opens.fetch_add(1, std::memory_order_relaxed);
                s_async_open_us.fetch_add(u32(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()),
                                          std::memory_order_relaxed);
            });
        }
        if (!m_waiting_open)
            fill_first_blocks();
    }
    else
    {
        CSoundRender_Source* src = m_pEmitter->source();
        u32 s = m_pEmitter->get_cursor(true) / src->m_wformat.nBlockAlign;
        s -= s % nxa::samples_per_frame; // a frame start: the DSP reads the frame header there
        m_start_sample = s < src->m_nxa_samples ? s : 0;
        m_loop = m_pEmitter->m_current_state == CSoundRender_Emitter::stPlayingLooped ||
            m_pEmitter->m_current_state == CSoundRender_Emitter::stStartingLooped;
        m_play_req = true;
    }
    inherited::render();
}

void CSoundRender_TargetNX::rewind()
{
    inherited::rewind();
    // the emitter's cursor is back at 0: start over (commit drops the voices first)
    render();
}

void CSoundRender_TargetNX::stop()
{
    // any thread (ref_sound::stop): flag only, commit() drops the voices
    if (rendering || m_play_req || m_live)
        m_stop_req.store(true, std::memory_order_release);
    if (m_held)
        m_core->nxa_release(m_held), m_held = nullptr;
    {
        // an open in flight is for this sound: whatever it brings is closed (the gen no longer matches)
        std::scoped_lock lk(m_open_lock);
        ++m_open_gen;
        if (m_opened)
        {
            if (m_opened_ovf)
            {
                ov_clear(m_opened_ovf);
                xr_delete(m_opened_ovf);
            }
            FS.r_close(m_opened_w);
            m_opened = false;
        }
        m_waiting_open = false;
    }
    inherited::stop();
}

void CSoundRender_TargetNX::update()
{
    inherited::update();
    if (m_waiting_open)
    {
        // the worker's open is in: take it, decode the first block, start at the next commit
        {
            std::scoped_lock lk(m_open_lock);
            if (!m_opened)
                return;
            adopt(m_opened_w, m_opened_ovf);
            m_opened = false;
            m_opened_w = nullptr;
            m_opened_ovf = nullptr;
            m_waiting_open = false;
        }
        fill_first_blocks();
        return;
    }
    if (m_mode != mStream || !m_live || m_play_req)
        return;
    // refill what the DSP finished, in order; two at most (one is the normal case: a block lasts 140-280 ms)
    const int id = nxau::voice_id(m_index, 0);
    for (int k = 0; k < 2; ++k)
    {
        const int st = nxau::wb_state(id, int(m_next));
        if (st != nxau::wb_free && st != nxau::wb_done)
            break;
        fill_block_at(m_next);
        const u32 frames = m_block_bytes / m_pEmitter->source()->m_wformat.nBlockAlign;
        nxau::wb_add(id, int(m_next), nxau::stream_slot(m_index) + m_next * (k_slot_bytes / blocks), m_block_bytes, 0, int(frames), false);
        m_next = (m_next + 1) % blocks;
    }
}

void CSoundRender_TargetNX::fill_parameters(CSoundRender_Core* core)
{
    inherited::fill_parameters(core);
    CSoundRender_Emitter* E = m_pEmitter;
    if (!E)
        return;
    const CSoundRender_CoreNX::Listener& L = m_core->listener();

    // OpenAL's AL_INVERSE_DISTANCE_CLAMPED with AL_ROLLOFF_FACTOR psSoundRolloff (a relative source sits at its
    // position from the listener: a 2D sound at 0 = full level, centre)
    const Fvector pos = E->render_position();
    Fvector rel;
    if (E->b2D)
        rel = pos;
    else
        rel.sub(pos, L.position);
    const float dist = rel.magnitude();
    const float ref = _max(E->p_source.min_distance, 0.01f);
    const float d = _min(_max(dist, ref), _max(E->p_source.max_distance, ref));
    const float denom = ref + psSoundRolloff * (d - ref);
    const float att = denom > 0.f ? ref / denom : 1.f;

    float gain = E->smooth_volume * E->hdr_gain;
    clamp(gain, EPS_S, 1.f);
    m_volume = gain * att;

    // the reverb send, as EFX: full source gain, its own distance attenuation by the room's rolloff (0 = none --
    // a room rings the same wherever in it the sound is), and none of the occlusion filter
    float wet = 0.f;
    if (m_core->reverb_on())
    {
        const float wdenom = ref + m_core->wet_rolloff() * (d - ref);
        const float watt = wdenom > 0.f ? ref / wdenom : 1.f;
        wet = watt / _max(att, 1e-4f); // the voice's volume already carries att
        clamp(wet, 0.f, 64.f);
    }

    // mix[c*4 + o]: o = 0 dry L, 1 dry R, 2 send L, 3 send R
    for (float& m : m_mix)
        m = 0.f;
    const u32 ch = E->source()->m_wformat.nChannels;
    if (ch == 1)
    {
        // equal power between the two speakers by how far right the source is (behind = in front: stereo)
        float x = dist > 0.05f ? rel.dotproduct(L.right) / dist : 0.f;
        clamp(x, -1.f, 1.f);
        m_mix[0] = _sqrt(0.5f * (1.f - x));
        m_mix[1] = _sqrt(0.5f * (1.f + x));
        m_mix[2] = m_mix[3] = wet * 0.5f; // the DSP's I3DL2 sums its inputs
    }
    else
    {
        // OpenAL plays a stereo source unpanned (attenuated all the same)
        m_mix[0] = 1.f;
        m_mix[2] = wet * 0.5f;
        m_mix[4 + 1] = 1.f;
        m_mix[4 + 3] = wet * 0.5f;
    }

    m_pitch = E->p_source.freq * psSoundTimeFactor;
    clamp(m_pitch, 0.01f, 8.f);

    float hf = E->b2D ? 1.f : E->occluder_hf;
    clamp(hf, 0.f, 1.f);
    m_hf = hf;
}

void CSoundRender_TargetNX::apply_params()
{
    const int id0 = nxau::voice_id(m_index, 0), id1 = nxau::voice_id(m_index, 1);
    const bool filter_change = !fsimilar(m_hf, m_hf_set, 0.02f) || ((m_hf >= .999f) != (m_hf_set >= .999f));
    float k[5];
    if (filter_change)
    {
        m_hf_set = m_hf;
        high_shelf(m_hf, k);
    }
    if (m_mode == mDirect && m_nvoices == 2)
    {
        // a stereo .nxa: two mono voices, left and right
        nxau::voice_params(id0, m_volume, m_pitch, m_mix, 1);
        nxau::voice_params(id1, m_volume, m_pitch, m_mix + 4, 1);
    }
    else
        nxau::voice_params(id0, m_volume, m_pitch, m_mix, m_mode == mDirect ? 1 : int(m_pEmitter ? m_pEmitter->source()->m_wformat.nChannels : 1));
    if (filter_change)
        for (u32 v = 0; v < m_nvoices; ++v)
            nxau::voice_biquad(nxau::voice_id(m_index, int(v)), m_hf_set < .999f, k);
}

void CSoundRender_TargetNX::commit()
{
    const bool stop_req = m_stop_req.exchange(false, std::memory_order_acq_rel);
    if (stop_req || (m_play_req && m_live))
    {
        for (u32 v = 0; v < 2; ++v)
            nxau::voice_drop(nxau::voice_id(m_index, int(v)));
        m_live = false;
        m_nvoices = 0;
    }
    if (m_play_req)
    {
        m_play_req = false;
        if (!rendering || !m_pEmitter)
            return;
        CSoundRender_Source* src = m_pEmitter->source();
        const u32 ch = src->m_wformat.nChannels;
        const int rate = int(src->m_wformat.nSamplesPerSec);
        bool ok = true;
        if (m_mode == mDirect)
        {
            const u8* payload = src->m_nxa_payload;
            ok = payload != nullptr;
            m_nvoices = ok ? ch : 0;
            for (u32 c = 0; ok && c < ch; ++c)
            {
                const int id = nxau::voice_id(m_index, int(c));
                ok = nxau::voice_init(id, 1, 1, rate, nxa::coefs(payload, c));
                if (!ok)
                    break;
                const u8* data = nxa::data(payload, c, src->m_nxa_stride);
                const int total = int(src->m_nxa_samples);
                int slot = 0;
                if (m_start_sample > 0 || !m_loop)
                    ok = nxau::wb_add(id, slot++, data, src->m_nxa_stride, int(m_start_sample), total, false);
                if (ok && m_loop)
                    ok = nxau::wb_add(id, slot, data, src->m_nxa_stride, 0, total, true);
            }
        }
        else
        {
            const int id = nxau::voice_id(m_index, 0);
            m_nvoices = 1;
            ok = nxau::voice_init(id, int(ch), 0, rate, nullptr);
            const u32 frames = m_block_bytes / src->m_wformat.nBlockAlign;
            for (u32 b = 0; ok && b < m_ready; ++b)
                ok = nxau::wb_add(id, int(b), nxau::stream_slot(m_index) + b * (k_slot_bytes / blocks), m_block_bytes, 0, int(frames), false);
        }
        if (!ok)
        {
            Msg("! [nx audren] cannot start [%s] on target %d (%s)", src->pname.c_str(), m_index, m_mode == mDirect ? "direct" : "stream");
            for (u32 v = 0; v < 2; ++v)
                nxau::voice_drop(nxau::voice_id(m_index, int(v)));
            m_nvoices = 0;
            return;
        }
        m_hf_set = 1.f; // a new voice has no filter
        apply_params();
        for (u32 v = 0; v < m_nvoices; ++v)
            nxau::voice_start(nxau::voice_id(m_index, int(v)));
        m_live = true;
        return;
    }
    if (m_live && m_pEmitter)
        apply_params();
}

#endif // __SWITCH__

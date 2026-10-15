// See SoundRender_NxAudren.h. The only xrSound file that includes <switch.h>; no xrCore here.
//
// ⭐Our own audren driver (11-10-2026). libnx's audrv builds the renderer's input with NO effects section ("effects
// would go here") and its submix free list is broken past one submix, so it cannot carry a reverb. This is audrv's
// logic (libnx, ISC: voices with wave-buffer queues, channels, memory pools, mixes, one device sink) with the
// effects section added (EffectInParameterVersion1, 0xC0 bytes; layouts from Ryujinx.Audio, which emulates this
// same renderer) and exactly the mixes we use:
//
//   voices -> SUBMIX 1 (4 buffers: 0/1 dry L/R, 2/3 reverb send L/R)
//             effect I3dl2Reverb (Reverb3d) on the submix, in place on buffers 2/3, dry gain 0 -> wet only
//             submix -> FINAL MIX 0 (2 buffers): 0->L, 1->R, 2->L, 3->R -> device sink "MainAudioOut"
//
// The DSP computes the reverb; the CPU only sends parameters when the room changes.
#ifdef __SWITCH__
#include "SoundRender_NxAudren.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <thread>

#include <switch.h>

namespace nxau
{
namespace
{
constexpr int k_slots = 4; // wave buffers per voice (ours; the renderer has 4 per voice too)
constexpr int k_submix = 1;
constexpr u32 k_reverb_work = 1024 * 1024; // the DSP's delay lines (~1 s at 48 kHz) -- generous

#pragma pack(push, 1)
struct EffectIn // EffectInParameterVersion1
{
    u8 type; // 5 = Reverb3d (I3DL2)
    u8 is_new;
    u8 is_enabled;
    u8 _r1;
    s32 mix_id;
    u64 buffer_base;
    u64 buffer_size;
    u32 processing_order;
    u32 _r2;
    u8 specific[0xA0];
};
struct EffectOut
{
    u8 state;
    u8 _r[15];
};
struct I3dl2In // Reverb3dParameter
{
    u8 input[6];
    u8 output[6];
    u16 channel_count_max;
    u16 channel_count;
    u32 _r;
    u32 sample_rate;
    float room_hf, hf_reference, decay_time, hf_decay_ratio, room_gain, reflections_gain, reverb_gain, diffusion, reflection_delay, reverb_delay,
        density, dry_gain;
    u8 parameter_status; // 0 invalid (re-create), 1 new (re-read the parameters), 2 enabled
};
#pragma pack(pop)
static_assert(sizeof(EffectIn) == 0xC0, "effect in");
static_assert(sizeof(EffectOut) == 0x10, "effect out");
static_assert(sizeof(I3dl2In) <= 0xA0, "i3dl2");

struct VoiceEtc
{
    int next_free_channel; // indexed by CHANNEL id: the free channel list
    u32 consumed;
    u32 drops;
    u64 played;
    AudioDriverWaveBuf* first;
    AudioDriverWaveBuf* waiting;
    AudioDriverWaveBuf* last;
};

struct Drv
{
    AudioRendererConfig cfg{};
    int n_pools = 0, n_voices = 0, n_effects = 0;
    void* in_buf = nullptr;
    size_t in_size = 0;
    void* out_buf = nullptr;
    size_t out_size = 0;
    AudioRendererMemPoolInfoIn* pools = nullptr;
    AudioRendererChannelInfoIn* channels = nullptr;
    AudioRendererVoiceInfoIn* voices = nullptr;
    EffectIn* effects = nullptr;
    AudioRendererMixInfoIn* mixes = nullptr;
    AudioRendererSinkInfoIn* sinks = nullptr;
    VoiceEtc* etc = nullptr;
    int first_free_channel = -1, free_channels = 0;
    u16* node_counter = nullptr;
} g;

// ⭐The renderer's IPC off the caller (11-10 evening). audrenRequestUpdateAudioRenderer is a blocking call into the
// audio service: 0.4-1 ms on average, but 4-11 ms in a fight with 30-60 voices -- and xrSound's update runs on the
// second thread of the frame, which the main thread waits for (📏au7, a user's fight: [2nd] sound >=5 ms in 9 of 31
// reports, 0 with OpenAL). Now update() takes the previous call's answer (long done by then), copies the input and
// hands the call to this thread; the answer is read on the next update. Wave-buffer completion is seen one update
// later -- the stream targets queue 4 blocks of ~0.14 s, plenty.
void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

struct Async
{
    Thread th{};
    Mutex mx{};
    CondVar cv{};
    bool kick = false, done = false; // under mx
    volatile int stage = 0;          // the IPC thread: 0 waits for work, 1 in the call, 2 answered (diagnostics)
    bool started = false, inflight = false, stuck = false;
    bool quit = false;               // under mx
    void* snap = nullptr;            // the input as it was when the call was handed over
    Result rc = 0;
    float ipc_ms = 0.f;
} g_async;

void ipc_main(void*)
{
    for (;;)
    {
        mutexLock(&g_async.mx);
        while (!g_async.kick && !g_async.quit)
            condvarWait(&g_async.cv, &g_async.mx);
        const bool quit = g_async.quit;
        g_async.kick = false;
        mutexUnlock(&g_async.mx);
        if (quit)
            break;
        g_async.stage = 1;
        const u64 t0 = armGetSystemTick();
        const Result rc = audrenRequestUpdateAudioRenderer(g_async.snap, g.in_size, g.out_buf, g.out_size, nullptr, 0);
        const float ms = float(armTicksToNs(armGetSystemTick() - t0)) / 1e6f;
        mutexLock(&g_async.mx);
        g_async.rc = rc;
        g_async.ipc_ms = ms;
        g_async.done = true;
        g_async.stage = 2;
        condvarWakeAll(&g_async.cv);
        mutexUnlock(&g_async.mx);
    }
}

void async_kick()
{
    mutexLock(&g_async.mx);
    g_async.kick = true;
    g_async.done = false;
    g_async.inflight = true;
    condvarWakeAll(&g_async.cv);
    mutexUnlock(&g_async.mx);
}

// the answer of the call in flight: true when it came (rc = its result), false when none was in flight or it did not
// come within 500 ms -- then the IPC thread is reported once and the caller skips the update instead of hanging
bool async_wait(Result& rc, u64 timeout_ns = 500000000ull)
{
    rc = 0;
    if (!g_async.inflight)
        return false;
    mutexLock(&g_async.mx);
    while (!g_async.done)
        if (R_FAILED(condvarWaitTimeout(&g_async.cv, &g_async.mx, timeout_ns)) && !g_async.done)
            break;
    const bool came = g_async.done;
    if (came)
    {
        g_async.done = false;
        g_async.inflight = false;
        rc = g_async.rc;
    }
    mutexUnlock(&g_async.mx);
    if (!came && !g_async.stuck)
    {
        g_async.stuck = true;
        say("! [nx audren] the IPC thread did not answer in %llu ms (stage %d) -- updates skipped until it does", timeout_ns / 1000000ull, g_async.stage);
    }
    return came;
}

bool async_wait_brief()
{
    Result rc;
    return async_wait(rc, 1000000000ull);
}

bool g_ready = false;
int g_targets = 0;
uint8_t* g_arena = nullptr;
size_t g_arena_size = 0;
uint8_t* g_stream = nullptr;
size_t g_stream_slot = 0;
uint8_t* g_reverb_work = nullptr;
AudioDriverWaveBuf* g_wb = nullptr; // [voice][slot]
bool g_reverb = false;
bool g_reverb_sent_new = false;

// ⭐capture (snd_nx_capture): a second sink, a circular buffer the DSP writes the final mix into, frame by frame
// (240 samples x 2 channels x s16, planar per frame). Lets a run on the console be HEARD afterwards: the update
// copies what the DSP wrote into a heap buffer and a WAV lands next to the NRO.
constexpr u32 k_cap_frame = 240 * 2 * 2; // one renderer frame, both channels
constexpr u32 k_cap_ring = k_cap_frame * 128; // 640 ms; 122880 B = 30 pages
uint8_t* g_cap_ring = nullptr;
u32 g_cap_read = 0;
s16* g_cap_out = nullptr; // interleaved stereo
u32 g_cap_frames = 0, g_cap_frames_want = 0;
u32 g_cap_index = 0;
char g_cap_name[64] = {};

void (*g_log)(const char*) = nullptr;

void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log)
        g_log(buf);
    else
        fprintf(stderr, "%s\n", buf);
}

size_t page_up(size_t n) { return (n + 0xFFF) & ~size_t(0xFFF); }

void log_rc(const char* what, Result rc) { say("! [nx audren] %s failed: 0x%x (module %u, desc %u)", what, rc, R_MODULE(rc), R_DESCRIPTION(rc)); }

// ---- the driver (libnx audrv's logic) -------------------------------------------------------------------------

size_t in_size_for(const Drv& d)
{
    return sizeof(AudioRendererUpdateDataHeader) + sizeof(AudioRendererBehaviorInfoIn) + sizeof(AudioRendererMemPoolInfoIn) * d.n_pools +
        sizeof(AudioRendererChannelInfoIn) * d.n_voices + sizeof(AudioRendererVoiceInfoIn) * d.n_voices + sizeof(EffectIn) * d.n_effects +
        sizeof(AudioRendererMixInfoIn) * d.cfg.num_mix_objs + sizeof(AudioRendererSinkInfoIn) * d.cfg.num_sinks + sizeof(AudioRendererPerformanceBufferInfoIn);
}

size_t out_size_for(const Drv& d)
{
    return sizeof(AudioRendererUpdateDataHeader) + sizeof(AudioRendererMemPoolInfoOut) * d.n_pools + sizeof(AudioRendererVoiceInfoOut) * d.n_voices +
        sizeof(EffectOut) * d.n_effects + sizeof(AudioRendererSinkInfoOut) * d.cfg.num_sinks + sizeof(AudioRendererPerformanceBufferInfoOut) +
        sizeof(AudioRendererBehaviorInfoOut);
}

bool drv_create(const AudioRendererConfig& cfg)
{
    g.cfg = cfg;
    g.n_voices = cfg.num_voices;
    g.n_effects = cfg.num_effects;
    g.n_pools = cfg.num_effects + 4 * cfg.num_voices; // what the renderer expects (audrenGetMemPoolCount)
    g.in_size = in_size_for(g);
    g.out_size = out_size_for(g);
    g.in_buf = memalign(AUDREN_INPUT_PARAM_ALIGNMENT, g.in_size);
    g.out_buf = memalign(AUDREN_OUTPUT_PARAM_ALIGNMENT, g.out_size);
    g.etc = static_cast<VoiceEtc*>(calloc(size_t(g.n_voices), sizeof(VoiceEtc)));
    g.node_counter = static_cast<u16*>(calloc(size_t(g.n_voices), sizeof(u16)));
    if (!g.in_buf || !g.out_buf || !g.etc || !g.node_counter)
        return false;
    memset(g.in_buf, 0, g.in_size);
    memset(g.out_buf, 0, g.out_size);

    auto* hdr = static_cast<AudioRendererUpdateDataHeader*>(g.in_buf);
    hdr->revision = audrenGetRevision();
    hdr->behavior_sz = sizeof(AudioRendererBehaviorInfoIn);
    hdr->mempools_sz = sizeof(AudioRendererMemPoolInfoIn) * g.n_pools;
    hdr->channels_sz = sizeof(AudioRendererChannelInfoIn) * g.n_voices;
    hdr->voices_sz = sizeof(AudioRendererVoiceInfoIn) * g.n_voices;
    hdr->effects_sz = sizeof(EffectIn) * g.n_effects;
    hdr->mixes_sz = sizeof(AudioRendererMixInfoIn) * cfg.num_mix_objs;
    hdr->sinks_sz = sizeof(AudioRendererSinkInfoIn) * cfg.num_sinks;
    hdr->perfmgr_sz = sizeof(AudioRendererPerformanceBufferInfoIn);
    hdr->total_sz = u32(g.in_size);

    auto* behavior = reinterpret_cast<AudioRendererBehaviorInfoIn*>(hdr + 1);
    behavior->revision = audrenGetRevision();
    behavior->flags = 0;

    g.pools = reinterpret_cast<AudioRendererMemPoolInfoIn*>(behavior + 1);
    for (int i = 0; i < g.n_pools; ++i)
        g.pools[i].state = AudioRendererMemPoolState_Released;
    g.channels = reinterpret_cast<AudioRendererChannelInfoIn*>(g.pools + g.n_pools);
    for (int i = 0; i < g.n_voices; ++i)
    {
        g.channels[i].id = u32(i);
        g.etc[i].next_free_channel = i + 1 < g.n_voices ? i + 1 : -1;
    }
    g.first_free_channel = 0;
    g.free_channels = g.n_voices;
    g.voices = reinterpret_cast<AudioRendererVoiceInfoIn*>(g.channels + g.n_voices);
    g.effects = reinterpret_cast<EffectIn*>(g.voices + g.n_voices);
    g.mixes = reinterpret_cast<AudioRendererMixInfoIn*>(g.effects + g.n_effects);
    g.sinks = reinterpret_cast<AudioRendererSinkInfoIn*>(g.mixes + cfg.num_mix_objs);
    auto* perf = reinterpret_cast<AudioRendererPerformanceBufferInfoIn*>(g.sinks + cfg.num_sinks);
    perf->detail_target = AUDREN_NODEID(15, 0, 0);

    // final mix: 2 buffers (L, R) -- what the sink plays
    AudioRendererMixInfoIn& fin = g.mixes[AUDREN_FINAL_MIX_ID];
    fin.volume = 1.f;
    fin.sample_rate = 48000;
    fin.buffer_count = 2;
    fin.is_used = true;
    fin.mix_id = AUDREN_FINAL_MIX_ID;
    fin.node_id = AUDREN_NODEID(2, 0, 0);
    fin.dest_mix_id = AUDREN_UNUSED_MIX_ID;
    fin.dest_splitter_id = AUDREN_UNUSED_SPLITTER_ID;

    // the submix the voices play into: dry L/R, reverb send L/R
    AudioRendererMixInfoIn& sub = g.mixes[k_submix];
    sub.volume = 1.f;
    sub.sample_rate = 48000;
    sub.buffer_count = 4;
    sub.is_used = true;
    sub.mix_id = k_submix;
    sub._padding2 = u32(g.n_effects); // EffectCount: how many effects may run on this mix
    sub.node_id = AUDREN_NODEID(2, k_submix, 0);
    sub.dest_mix_id = AUDREN_FINAL_MIX_ID;
    sub.dest_splitter_id = AUDREN_UNUSED_SPLITTER_ID;
    sub.mix[0][0] = 1.f;
    sub.mix[1][1] = 1.f;
    sub.mix[2][0] = 1.f;
    sub.mix[3][1] = 1.f;

    // the device sink
    AudioRendererSinkInfoIn& sink = g.sinks[0];
    sink.type = AudioRendererSinkType_Device;
    sink.is_used = true;
    sink.node_id = AUDREN_NODEID(3, 0, 0);
    strncpy(sink.device_sink.name, AUDREN_DEFAULT_DEVICE_NAME, sizeof(sink.device_sink.name) - 1);
    sink.device_sink.input_count = 2;
    sink.device_sink.inputs[0] = 0;
    sink.device_sink.inputs[1] = 1;
    return true;
}

void drv_free()
{
    free(g.in_buf);
    free(g.out_buf);
    free(g.etc);
    free(g.node_counter);
    g = Drv{};
}

int pool_add(void* p, size_t size)
{
    for (int i = 0; i < g.n_pools; ++i)
        if (g.pools[i].state == AudioRendererMemPoolState_Released)
        {
            g.pools[i].address = p;
            g.pools[i].size = size;
            g.pools[i].state = AudioRendererMemPoolState_RequestAttach;
            return i;
        }
    return -1;
}

void queue_wavebufs(int id)
{
    AudioRendererVoiceInfoIn& v = g.voices[id];
    VoiceEtc& e = g.etc[id];
    AudioDriverWaveBuf* w = e.waiting;
    while (w && v.wavebuf_count < 4)
    {
        const int head = (v.wavebuf_head + v.wavebuf_count) & 3;
        w->state = AudioDriverWaveBufState_Queued;
        w->sequence_id = e.consumed + v.wavebuf_count;
        AudioRendererWaveBuf& r = v.wavebufs[head];
        r.address = w->data_raw;
        r.size = w->size;
        r.start_sample_offset = w->start_sample_offset;
        r.end_sample_offset = w->end_sample_offset;
        r.is_looping = w->is_looping;
        r.end_of_stream = false;
        r.sent_to_server = false;
        r.context_addr = w->context_addr;
        r.context_sz = w->context_sz;
        v.wavebuf_count++;
        w = w->next;
    }
    e.waiting = w;
}

void reset_voice_state(int id)
{
    VoiceEtc& e = g.etc[id];
    e.consumed = 0;
    e.drops = 0;
    e.played = 0;
    for (AudioDriverWaveBuf* w = e.first; w; w = w->next)
        w->state = AudioDriverWaveBufState_Done;
    e.first = e.waiting = e.last = nullptr;
}

void drv_voice_drop(int id)
{
    AudioRendererVoiceInfoIn& v = g.voices[id];
    if (!v.is_used)
        return;
    g.free_channels += int(v.channel_count);
    for (int i = int(v.channel_count) - 1; i >= 0; --i)
    {
        const int ch = int(v.channel_ids[i]);
        g.channels[ch].is_used = false;
        g.etc[ch].next_free_channel = g.first_free_channel;
        g.first_free_channel = ch;
    }
    memset(&v, 0, sizeof v);
    reset_voice_state(id);
}

bool drv_voice_init(int id, int nch, PcmFormat fmt, int rate)
{
    if (g.voices[id].is_used)
        drv_voice_drop(id);
    if (g.free_channels < nch)
        return false;
    AudioRendererVoiceInfoIn& v = g.voices[id];
    v.id = u32(id);
    v.node_id = AUDREN_NODEID(1, id, g.node_counter[id]++);
    v.is_new = true;
    v.is_used = true;
    v.state = AudioRendererVoicePlayState_Stopped;
    v.sample_format = fmt;
    v.sample_rate = u32(rate);
    v.channel_count = u32(nch);
    v.pitch = 1.f;
    v.volume = 1.f;
    v.dest_mix_id = k_submix;
    v.dest_splitter_id = AUDREN_UNUSED_SPLITTER_ID;
    for (int i = 0; i < 4; ++i)
        v.wavebufs[i].sent_to_server = true;
    g.free_channels -= nch;
    for (int i = 0; i < nch; ++i)
    {
        const int ch = g.first_free_channel;
        g.first_free_channel = g.etc[ch].next_free_channel;
        v.channel_ids[i] = u32(ch);
        g.channels[ch].is_used = true;
        memset(g.channels[ch].mix, 0, sizeof g.channels[ch].mix);
    }
    return true;
}

void drv_voice_update(int id, const AudioRendererVoiceInfoOut& o)
{
    AudioRendererVoiceInfoIn& v = g.voices[id];
    VoiceEtc& e = g.etc[id];
    // is_new / sent_to_server are cleared in drv_sent, when the input that carries them goes out: with the IPC on its
    // own thread this answer is one update old, and a voice started or a wave buffer queued since then has NOT been
    // sent yet (clearing them here silenced every voice -- dsp played 0, gc4)
    e.played = o.played_sample_count;
    e.drops = o.voice_drops_count;
    const u32 n = o.num_wavebufs_consumed - e.consumed;
    if (n)
    {
        v.wavebuf_count -= n;
        v.wavebuf_head = s16((v.wavebuf_head + n) & 3);
        e.consumed = o.num_wavebufs_consumed;
    }
    AudioDriverWaveBuf* w = e.first;
    for (u32 i = 0; w && i < n; ++i)
    {
        w->state = AudioDriverWaveBufState_Done;
        w = w->next;
    }
    if (w)
    {
        w->state = AudioDriverWaveBufState_Playing;
        e.first = w;
        queue_wavebufs(id);
    }
    else
        e.first = e.last = nullptr;
}

void drv_prepare()
{
    // the renderer processes used voices in this order
    for (int i = 0, j = 0; i < g.n_voices; ++i)
        if (g.voices[i].is_used)
            g.voices[i].sorting_order = u32(j++);
}

// which voice each slot held in the input that went out (its node_id; 0 = unused): with the IPC on its own thread a
// slot can be dropped and given to a new voice before that input's answer comes, and the old voice's answer (wave
// buffers consumed, samples played) must not land on the new one
constexpr int k_max_voices = 1024;
u32 g_sent_node[k_max_voices];

// right after the input went out: what it carried is no longer new
void drv_sent()
{
    for (int i = 0; i < g.n_effects; ++i)
        g.effects[i].is_new = 0;
    for (int i = 0; i < g.n_voices && i < k_max_voices; ++i)
        if (g.voices[i].is_used)
        {
            g.voices[i].is_new = false;
            for (int k = 0; k < 4; ++k)
                g.voices[i].wavebufs[k].sent_to_server = true;
            g_sent_node[i] = g.voices[i].node_id;
        }
        else
            g_sent_node[i] = 0;
}

// the renderer's answer in out_buf -> pools, voices
Result drv_answer()
{
    auto* oh = static_cast<AudioRendererUpdateDataHeader*>(g.out_buf);
    auto* opools = reinterpret_cast<AudioRendererMemPoolInfoOut*>(oh + 1);
    if (oh->mempools_sz != g.n_pools * sizeof(AudioRendererMemPoolInfoOut))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    for (int i = 0; i < g.n_pools; ++i)
        if (opools[i].new_state != AudioRendererMemPoolState_Invalid)
            g.pools[i].state = opools[i].new_state;
    auto* ovoices = reinterpret_cast<AudioRendererVoiceInfoOut*>(opools + g.n_pools);
    if (oh->voices_sz != g.n_voices * sizeof(AudioRendererVoiceInfoOut))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    for (int i = 0; i < g.n_voices && i < k_max_voices; ++i)
        if (g.voices[i].is_used && g_sent_node[i] == g.voices[i].node_id)
            drv_voice_update(i, ovoices[i]);
    return 0;
}

// synchronous: init, shutdown, and whenever the IPC thread is not running
Result drv_update()
{
    Result prev;
    async_wait(prev);
    drv_prepare();
    Result rc = audrenRequestUpdateAudioRenderer(g.in_buf, g.in_size, g.out_buf, g.out_size, nullptr, 0);
    if (R_FAILED(rc))
        return rc;
    drv_sent();
    return drv_answer();
}
} // namespace

void set_log(void (*fn)(const char*)) { g_log = fn; }

bool init(int targets, size_t arena_bytes, size_t stream_slot_bytes, bool reverb)
{
    if (g_ready)
        return true;
    g_targets = targets;
    AudioRendererConfig cfg = {};
    cfg.output_rate = AudioRendererOutputRate_48kHz;
    cfg.num_voices = targets * 2; // also the channel count: each target uses at most 2 channels
    cfg.num_effects = reverb ? 1 : 0;
    cfg.num_sinks = 2; // the device, and the capture ring
    cfg.num_mix_objs = 2; // final + the submix
    cfg.num_mix_buffers = 6; // 2 + 4

    Result rc = audrenInitialize(&cfg);
    if (R_FAILED(rc))
    {
        log_rc("audrenInitialize", rc);
        return false;
    }
    if (!drv_create(cfg))
    {
        say("! [nx audren] out of memory (driver)");
        shutdown();
        return false;
    }

    g_arena_size = page_up(arena_bytes);
    g_stream_slot = page_up(stream_slot_bytes);
    g_arena = static_cast<uint8_t*>(memalign(AUDREN_MEMPOOL_ALIGNMENT, g_arena_size));
    g_stream = static_cast<uint8_t*>(memalign(AUDREN_MEMPOOL_ALIGNMENT, g_stream_slot * size_t(targets)));
    g_wb = static_cast<AudioDriverWaveBuf*>(calloc(size_t(g.n_voices) * k_slots, sizeof(AudioDriverWaveBuf)));
    if (reverb)
        g_reverb_work = static_cast<uint8_t*>(memalign(AUDREN_MEMPOOL_ALIGNMENT, k_reverb_work));
    if (!g_arena || !g_stream || !g_wb || (reverb && !g_reverb_work))
    {
        say("! [nx audren] out of memory (arena %zu, streams %zu)", g_arena_size, g_stream_slot * size_t(targets));
        shutdown();
        return false;
    }
    memset(g_arena, 0, g_arena_size);
    memset(g_stream, 0, g_stream_slot * size_t(targets));
    armDCacheFlush(g_arena, g_arena_size);
    armDCacheFlush(g_stream, g_stream_slot * size_t(targets));
    if (g_reverb_work)
    {
        memset(g_reverb_work, 0, k_reverb_work);
        armDCacheFlush(g_reverb_work, k_reverb_work);
    }

    g_cap_ring = static_cast<uint8_t*>(memalign(AUDREN_MEMPOOL_ALIGNMENT, k_cap_ring));
    if (!g_cap_ring)
    {
        say("! [nx audren] out of memory (capture ring)");
        shutdown();
        return false;
    }
    memset(g_cap_ring, 0, k_cap_ring);
    armDCacheFlush(g_cap_ring, k_cap_ring);
    {
        AudioRendererSinkInfoIn& cs = g.sinks[1];
        cs.type = AudioRendererSinkType_CircularBuffer;
        cs.is_used = true;
        cs.node_id = AUDREN_NODEID(3, 1, 0);
        cs.circular_buffer_sink.buffer_ptr = g_cap_ring;
        cs.circular_buffer_sink.buffer_sz = k_cap_ring;
        cs.circular_buffer_sink.input_count = 2;
        cs.circular_buffer_sink.sample_count = 240;
        cs.circular_buffer_sink.last_read_offset = 0;
        cs.circular_buffer_sink.sample_format = PcmFormat_Int16;
        cs.circular_buffer_sink.inputs[0] = 0;
        cs.circular_buffer_sink.inputs[1] = 1;
    }

    if (pool_add(g_cap_ring, k_cap_ring) < 0 || pool_add(g_arena, g_arena_size) < 0 || pool_add(g_stream, g_stream_slot * size_t(targets)) < 0 ||
        (g_reverb_work && pool_add(g_reverb_work, k_reverb_work) < 0))
    {
        say("! [nx audren] mempools failed");
        shutdown();
        return false;
    }

    g_reverb = reverb;
    if (reverb)
    {
        EffectIn& e = g.effects[0];
        e.type = 5; // Reverb3d
        e.is_new = 1;
        e.is_enabled = 0; // until the first room
        e.mix_id = k_submix;
        e.buffer_base = u64(uintptr_t(g_reverb_work));
        e.buffer_size = k_reverb_work;
        e.processing_order = 0;
        I3dl2In p{};
        p.input[0] = 2, p.input[1] = 3;
        p.output[0] = 2, p.output[1] = 3;
        p.channel_count_max = 2;
        p.channel_count = 2;
        p.sample_rate = 48000;
        p.room_hf = -100.f, p.hf_reference = 5000.f, p.decay_time = 1.49f, p.hf_decay_ratio = 0.83f, p.room_gain = -1000.f;
        p.reflections_gain = -2602.f, p.reverb_gain = 200.f, p.diffusion = 100.f, p.reflection_delay = 0.007f, p.reverb_delay = 0.011f, p.density = 100.f;
        p.dry_gain = 0.f;
        p.parameter_status = 0;
        memcpy(e.specific, &p, sizeof p);
    }

    rc = drv_update();
    if (R_FAILED(rc))
    {
        log_rc("audren update (init)", rc);
        shutdown();
        return false;
    }
    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc))
    {
        log_rc("audrenStartAudioRenderer", rc);
        shutdown();
        return false;
    }
    g_ready = true;

    // the IPC thread: core 2, priority 0x2C -- it sleeps in the call nearly all the time, and when the answer comes it
    // must not queue behind the 0x2E/0x3B workers there. Without it update() stays synchronous.
    g_async.snap = memalign(AUDREN_INPUT_PARAM_ALIGNMENT, g.in_size);
    if (g_async.snap)
    {
        mutexInit(&g_async.mx);
        condvarInit(&g_async.cv);
        g_async.quit = false;
        if (R_SUCCEEDED(threadCreate(&g_async.th, ipc_main, nullptr, nullptr, 0x4000, 0x2C, 2)) && R_SUCCEEDED(threadStart(&g_async.th)))
            g_async.started = true;
        else
        {
            free(g_async.snap);
            g_async.snap = nullptr;
        }
    }
    say("~ [nx audren] up: %d voices, arena %zu KB, stream slots %d x %zu KB, reverb %s, revision 0x%x, in %zu B / out %zu B per update, IPC %s", g.n_voices,
        g_arena_size / 1024, targets, g_stream_slot / 1024, reverb ? "DSP I3DL2" : "off", audrenGetRevision(), g.in_size, g.out_size,
        g_async.started ? "on its own thread" : "synchronous");
    return true;
}

void shutdown()
{
    const bool stuck = g_async.started && g_async.inflight && !async_wait_brief();
    if (g_async.started && !stuck)
    {
        mutexLock(&g_async.mx);
        g_async.quit = true;
        condvarWakeAll(&g_async.cv);
        mutexUnlock(&g_async.mx);
        threadWaitForExit(&g_async.th);
        threadClose(&g_async.th);
        g_async.started = false;
        free(g_async.snap);
        g_async.snap = nullptr;
    }
    // a call that never came back: its thread, the session and every buffer it may still write are left alone
    if (stuck)
    {
        g_ready = false;
        return;
    }
    if (g.in_buf)
    {
        if (g_ready)
        {
            for (int i = 0; i < g.n_voices; ++i)
                drv_voice_drop(i);
            drv_update();
            audrenStopAudioRenderer();
        }
        audrenExit();
    }
    drv_free();
    g_ready = false;
    free(g_arena);
    free(g_stream);
    free(g_wb);
    free(g_reverb_work);
    free(g_cap_ring);
    free(g_cap_out);
    g_arena = g_stream = g_reverb_work = g_cap_ring = nullptr;
    g_cap_out = nullptr;
    g_cap_frames_want = 0;
    g_wb = nullptr;
}

bool ready() { return g_ready; }
uint8_t* arena_base() { return g_arena; }
size_t arena_size() { return g_arena_size; }
uint8_t* stream_slot(int target) { return g_stream + g_stream_slot * size_t(target); }
void flush(const void* p, size_t n) { armDCacheFlush(const_cast<void*>(p), n); }

bool voice_init(int id, int channels, int format, int rate, const void* coefs)
{
    const PcmFormat f = format == 1 ? PcmFormat_Adpcm : PcmFormat_Int16;
    if (!drv_voice_init(id, channels, f, rate))
        return false;
    if (format == 1)
    {
        g.voices[id].extra_params_ptr = coefs;
        g.voices[id].extra_params_sz = sizeof(AudioRendererAdpcmParameters);
    }
    for (int s = 0; s < k_slots; ++s)
        g_wb[id * k_slots + s].state = AudioDriverWaveBufState_Free;
    return true;
}

void voice_drop(int id)
{
    drv_voice_drop(id);
    for (int s = 0; s < k_slots; ++s)
        g_wb[id * k_slots + s].state = AudioDriverWaveBufState_Free;
}

void voice_start(int id) { g.voices[id].state = AudioRendererVoicePlayState_Started; }

void voice_params(int id, float volume, float pitch, const float* mix, int channels)
{
    AudioRendererVoiceInfoIn& v = g.voices[id];
    v.volume = volume;
    v.pitch = pitch;
    for (int c = 0; c < channels; ++c)
    {
        float* m = g.channels[v.channel_ids[c]].mix;
        for (int o = 0; o < 4; ++o)
            m[o] = mix[c * 4 + o];
    }
}

void voice_biquad(int id, bool on, const float* k)
{
    AudioRendererBiquadFilter& b = g.voices[id].biquads[0];
    if (!on)
    {
        b.enable = false;
        return;
    }
    auto q14 = [](float x) -> s16 {
        const float s = x * 16384.f;
        return s16(s > 32767.f ? 32767 : (s < -32768.f ? -32768 : int(s)));
    };
    // ⚠the DSP ADDS the feedback terms (y = b0 x + ... + d1 y1 + d2 y2 -- Ryujinx's BiquadFilterHelper): the
    // textbook a1, a2 go in negated
    b.enable = true;
    b.numerator[0] = q14(k[0]);
    b.numerator[1] = q14(k[1]);
    b.numerator[2] = q14(k[2]);
    b.denominator[0] = q14(-k[3]);
    b.denominator[1] = q14(-k[4]);
}

bool wb_add(int id, int slot, const void* data, size_t bytes, int start, int end, bool loop)
{
    AudioDriverWaveBuf& w = g_wb[id * k_slots + slot];
    if (w.state != AudioDriverWaveBufState_Free && w.state != AudioDriverWaveBufState_Done)
        return false;
    if (!data || (uintptr_t(data) & (AUDREN_BUFFER_ALIGNMENT - 1)))
        return false;
    w.data_raw = data;
    w.size = bytes;
    w.start_sample_offset = start;
    w.end_sample_offset = end;
    w.is_looping = loop;
    w.context_addr = nullptr;
    w.context_sz = 0;
    w.state = AudioDriverWaveBufState_Waiting;
    w.sequence_id = ~u32(0);
    w.next = nullptr;
    VoiceEtc& e = g.etc[id];
    if (e.first)
        e.last->next = &w;
    else
        e.first = &w;
    if (!e.waiting)
        e.waiting = &w;
    e.last = &w;
    queue_wavebufs(id);
    return true;
}

int wb_state(int id, int slot) { return int(g_wb[id * k_slots + slot].state); }

void master_volume(float v) { g.mixes[AUDREN_FINAL_MIX_ID].volume = v; }

bool reverb_available() { return g_ready && g_reverb; }

void reverb_set(const Reverb& r, bool on)
{
    if (!g_reverb)
        return;
    EffectIn& e = g.effects[0];
    I3dl2In p;
    memcpy(&p, e.specific, sizeof p);
    p.room_gain = r.room_mb;
    p.room_hf = r.room_hf_mb;
    p.decay_time = r.decay_time;
    p.hf_decay_ratio = r.decay_hf_ratio;
    p.reflections_gain = r.reflections_mb;
    p.reflection_delay = r.reflections_delay;
    p.reverb_gain = r.reverb_mb;
    p.reverb_delay = r.reverb_delay;
    p.diffusion = r.diffusion;
    p.density = r.density;
    p.hf_reference = r.hf_reference;
    p.dry_gain = 0.f;
    // the first time: 0 (the DSP builds its state); after that 1 = "new parameters, read them"
    p.parameter_status = g_reverb_sent_new ? 1 : 0;
    g_reverb_sent_new = true;
    memcpy(e.specific, &p, sizeof p);
    e.is_enabled = on ? 1 : 0;
}

namespace
{
void capture_write()
{
    // the file goes out on a thread of its own: ~5 MB to the SD is ~200 ms the sound thread must not wait
    struct Job
    {
        char name[64];
        s16* data;
        u32 frames;
    };
    Job* j = static_cast<Job*>(malloc(sizeof(Job)));
    memcpy(j->name, g_cap_name, sizeof j->name);
    j->data = g_cap_out;
    j->frames = g_cap_frames;
    g_cap_out = nullptr;
    g_cap_frames_want = 0;
    std::thread([j] {
        FILE* f = fopen(j->name, "wb");
        if (f)
        {
            const u32 bytes = j->frames * 240 * 2 * 2;
            auto w32 = [&](u32 v) { fwrite(&v, 4, 1, f); };
            auto w16 = [&](u16 v) { fwrite(&v, 2, 1, f); };
            fwrite("RIFF", 1, 4, f);
            w32(36 + bytes);
            fwrite("WAVEfmt ", 1, 8, f);
            w32(16), w16(1), w16(2), w32(48000), w32(48000 * 4), w16(4), w16(16);
            fwrite("data", 1, 4, f);
            w32(bytes);
            fwrite(j->data, 1, bytes, f);
            fclose(f);
            say("~ [nx audren] capture: %s, %.2f s", j->name, j->frames * 240 / 48000.0);
        }
        else
            say("! [nx audren] capture: cannot write %s", j->name);
        free(j->data);
        free(j);
    }).detach();
}

// what the DSP wrote since the last look (the ring, renderer frames, planar L then R)
void capture_poll(u32 last_written)
{
    if (last_written >= k_cap_ring || last_written % k_cap_frame)
        return;
    while (g_cap_read != last_written)
    {
        const uint8_t* fr = g_cap_ring + g_cap_read;
        if (g_cap_frames_want)
        {
            armDCacheFlush(const_cast<uint8_t*>(fr), k_cap_frame); // the DSP wrote it: drop our stale lines
            const s16* l = reinterpret_cast<const s16*>(fr);
            const s16* r = l + 240;
            s16* o = g_cap_out + size_t(g_cap_frames) * 480;
            for (int i = 0; i < 240; ++i)
                o[i * 2] = l[i], o[i * 2 + 1] = r[i];
            if (++g_cap_frames == g_cap_frames_want)
            {
                capture_write();
                g_cap_read = last_written;
                break;
            }
        }
        g_cap_read = (g_cap_read + k_cap_frame) % k_cap_ring;
    }
    g.sinks[1].circular_buffer_sink.last_read_offset = g_cap_read;
}
} // namespace

bool capture(float seconds)
{
    if (!g_ready || g_cap_frames_want || seconds <= 0.f)
        return false;
    g_cap_frames_want = u32(seconds * 200.f); // 200 renderer frames a second
    g_cap_out = static_cast<s16*>(malloc(size_t(g_cap_frames_want) * 480 * sizeof(s16)));
    if (!g_cap_out)
    {
        g_cap_frames_want = 0;
        return false;
    }
    g_cap_frames = 0;
    snprintf(g_cap_name, sizeof g_cap_name, "nxcap_%02u.wav", g_cap_index++ % 100);
    return true;
}

// the answer just taken (out_buf) -> statistics, the reverb's parameter status, the capture ring
static void answer_stats(Stats& st);

bool update(Stats& st)
{
    const u64 t0 = armGetSystemTick();
    Result rc = 0;
    bool answered = true;
    if (g_async.started)
    {
        // the previous update's call: normally finished long ago, so this does not wait
        const bool had = g_async.inflight;
        answered = async_wait(rc);
        if (had && !answered)
            return false; // still in the call (reported once): nothing new goes out until it comes back
        if (answered)
        {
            st.ipc_ms_sum += g_async.ipc_ms;
            if (g_async.ipc_ms > st.ipc_ms_max)
                st.ipc_ms_max = g_async.ipc_ms;
            st.ipc_n++;
            if (R_SUCCEEDED(rc))
                rc = drv_answer();
        }
    }
    else
    {
        rc = drv_update();
        const float ipc = float(armTicksToNs(armGetSystemTick() - t0)) / 1e6f;
        st.ipc_ms_sum += ipc;
        if (ipc > st.ipc_ms_max)
            st.ipc_ms_max = ipc;
        st.ipc_n++;
    }
    if (R_FAILED(rc))
    {
        if (st.failures++ < 5)
            log_rc("audren update", rc);
    }
    else if (answered)
        answer_stats(st);
    if (g_async.started)
    {
        // this update's input goes out on the IPC thread; out_buf is not touched here until the next update
        drv_prepare();
        memcpy(g_async.snap, g.in_buf, g.in_size);
        drv_sent();
        async_kick();
    }
    const float ms = float(armTicksToNs(armGetSystemTick() - t0)) / 1e6f;
    st.update_ms_sum += ms;
    if (ms > st.update_ms_max)
        st.update_ms_max = ms;
    st.updates++;
    return R_SUCCEEDED(rc);
}

static void answer_stats(Stats& st)
{
    static u32* s_drops = nullptr;
    static u64* s_played = nullptr;
    if (!s_drops)
    {
        s_drops = static_cast<u32*>(calloc(size_t(g.n_voices), sizeof(u32)));
        s_played = static_cast<u64*>(calloc(size_t(g.n_voices), sizeof(u64)));
    }
    for (int i = 0; i < g.n_voices; ++i)
        if (g.voices[i].is_used)
        {
            const u32 d = g.etc[i].drops;
            if (d > s_drops[i])
                st.dsp_drops += d - s_drops[i];
            s_drops[i] = d;
            const u64 p = g.etc[i].played;
            if (p > s_played[i])
                st.played += p - s_played[i];
            s_played[i] = p;
        }
        else
            s_drops[i] = 0, s_played[i] = 0;
    if (g_reverb)
    {
        // after the parameters went out once as "new", the next updates keep them ("enabled")
        I3dl2In p;
        memcpy(&p, g.effects[0].specific, sizeof p);
        if (p.parameter_status != 2 && g_reverb_sent_new)
        {
            p.parameter_status = 2;
            memcpy(g.effects[0].specific, &p, sizeof p);
        }
        auto* oh = static_cast<AudioRendererUpdateDataHeader*>(g.out_buf);
        const auto* oe = reinterpret_cast<const EffectOut*>(reinterpret_cast<const u8*>(oh + 1) + g.n_pools * sizeof(AudioRendererMemPoolInfoOut) +
                                                             g.n_voices * sizeof(AudioRendererVoiceInfoOut));
        st.reverb_state = oe[0].state;
    }
    {
        auto* oh = static_cast<AudioRendererUpdateDataHeader*>(g.out_buf);
        const auto* os = reinterpret_cast<const AudioRendererSinkInfoOut*>(reinterpret_cast<const u8*>(oh + 1) + g.n_pools * sizeof(AudioRendererMemPoolInfoOut) +
                                                                           g.n_voices * sizeof(AudioRendererVoiceInfoOut) + g.n_effects * sizeof(EffectOut));
        capture_poll(os[1].last_written_offset);
    }
}
} // namespace nxau
#endif // __SWITCH__

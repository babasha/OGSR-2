/* The seven SDL functions devkitPro's OpenAL Soft calls, implemented directly on libnx's audout service.
 *
 * WHY: portlibs' libopenal.a is built with OpenAL Soft's SDL2 backend, but linking libSDL2.a drags in SDL's
 * video code, and that pulls portlibs' libEGL/libGLESv2, which contain Mesa 20.1's copy of the util code that
 * NVK's libvulkan.a (Mesa 26) also carries: dozens of duplicate symbols and, worse, two ABIs of one library in
 * one binary. Audio is all OpenAL needs from SDL, so this file *is* that part of SDL, and no SDL library is
 * linked at all.
 *
 * Contract used by OpenAL Soft's sdl2 backend: SDL_OpenAudioDevice hands back a device id and a callback
 * (desired->callback(userdata, stream, len)) that we call from our own thread whenever a buffer has played.
 * audout is fixed at 48 kHz, 16-bit, stereo; the "obtained" spec says so and OpenAL adapts (it asks SDL to
 * allow any change). Buffers must be 0x1000-aligned and a multiple of 0x1000 bytes (1024 stereo frames).
 *
 * ⚠⚠THE 30-09 CRASH (renderSamples, null write) -- fixed 10-10. OpenAL Soft 1.21.1's backend has no lock() of its
 * own: its stop() is SDL_PauseAudioDevice(id, 1) and nothing else, and it relies on SDL's guarantee that when that
 * call returns, the callback is NOT running and will not run again. alcCloseDevice is stop() -> free the device's
 * mixing state -> ~backend (SDL_CloseAudioDevice). The first version here only set a flag, read OUTSIDE the lock,
 * so a callback already inside renderSamples kept mixing into the device while OpenAL tore it down (the engine's
 * device enumeration opens, starts and closes a device before the real one). Now: pause and close take the same
 * lock the callback runs under, and the flag is read under it -- the SDL contract.
 *
 * Second fix: audoutWaitPlayFinish / audoutGetReleasedAudioOutBuffer hand back ONE released buffer per call (libnx
 * passes an 8-byte out buffer), however many have played. The old loop refilled only that one, so after any late
 * wake-up the queue held fewer buffers for good -- crackle, then silence. Every wake now drains the released list.
 *
 * CORE (10-10): the mixer is this thread, so it is pinned to core 2 (the main thread owns core 0, the render
 * thread core 1) at a priority above the core-2 workers (0x2E particles/rays, 0x30 wallmarks, 0x3B prefetch):
 * a mix that misses its period is an audible click, a worker that waits 1-2 ms is not. Counters for the price
 * of the mix: xr_nx_audio_stats (printed by the engine as [nx audio]).
 */
#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include <SDL2/SDL_audio.h>

#define XRA_BUFFERS 4
#define XRA_RATE 48000
#define XRA_CHANNELS 2
#define XRA_ALIGN 0x1000u
#define XRA_CORE 2
#define XRA_PRIO 0x2B /* main thread 0x2C (core 0), PinnedWorker 0x2E, pthreads 0x3B */

typedef struct xr_audio_dev
{
    SDL_AudioCallback callback;
    void* userdata;
    AudioOutBuffer out[XRA_BUFFERS];
    void* mem[XRA_BUFFERS];
    u32 bytes;               /* per buffer, multiple of XRA_ALIGN */
    u32 frames;              /* per buffer */
    int paused;              /* under lock */
    volatile int running;
    pthread_t thread;
    pthread_mutex_t lock;    /* the callback runs under it; pause/close take it -> no callback in flight after them */
} xr_audio_dev;

static xr_audio_dev* g_dev;
static int g_audout_up;

/* Price of the mix, read-and-reset by xr_nx_audio_stats (any thread). */
static u64 g_mix_ticks, g_mix_max_ticks, g_mix_calls, g_starved, g_core_rc;
static u32 g_period_frames; /* of the open device, 0 = none (not g_dev: close frees it under a reader) */

static void stat_add(u64 ticks)
{
    u64 m;
    __atomic_add_fetch(&g_mix_ticks, ticks, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_mix_calls, 1, __ATOMIC_RELAXED);
    m = __atomic_load_n(&g_mix_max_ticks, __ATOMIC_RELAXED);
    while (ticks > m && !__atomic_compare_exchange_n(&g_mix_max_ticks, &m, ticks, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

/* avg/max: ms per callback; period_ms: what one buffer lasts (the budget); starved: wake-ups that found every
 * buffer played (the queue ran dry = an audible gap); calls: callbacks since the last read. 0 if no device. */
void xr_nx_audio_stats(float* avg_ms, float* max_ms, float* period_ms, unsigned* calls, unsigned* starved, unsigned* core_rc)
{
    const u64 t = __atomic_exchange_n(&g_mix_ticks, 0, __ATOMIC_RELAXED);
    const u64 m = __atomic_exchange_n(&g_mix_max_ticks, 0, __ATOMIC_RELAXED);
    const u64 c = __atomic_exchange_n(&g_mix_calls, 0, __ATOMIC_RELAXED);
    const u64 s = __atomic_exchange_n(&g_starved, 0, __ATOMIC_RELAXED);
    const double tick_ms = 1000.0 / (double)armGetSystemTickFreq();
    *avg_ms = c ? (float)((double)t * tick_ms / (double)c) : 0.f;
    *max_ms = (float)((double)m * tick_ms);
    *period_ms = (float)(1000.0 * g_period_frames / XRA_RATE);
    *calls = (unsigned)c;
    *starved = (unsigned)s;
    *core_rc = (unsigned)g_core_rc;
}

static void fill(xr_audio_dev* d, AudioOutBuffer* b)
{
    pthread_mutex_lock(&d->lock);
    if (d->paused || !d->callback)
        memset(b->buffer, 0, d->bytes);
    else {
        const u64 t0 = armGetSystemTick();
        d->callback(d->userdata, (Uint8*)b->buffer, (int)d->bytes);
        stat_add(armGetSystemTick() - t0);
    }
    pthread_mutex_unlock(&d->lock);
    b->buffer_size = d->bytes;
    b->data_size = d->bytes;
    b->data_offset = 0;
}

static int ours(xr_audio_dev* d, AudioOutBuffer* b)
{
    int i;
    for (i = 0; i < XRA_BUFFERS; ++i)
        if (b == &d->out[i])
            return 1;
    return 0; /* a buffer from a closed device (still in audout's released list): never touch it */
}

static void* audio_thread(void* p)
{
    xr_audio_dev* d = (xr_audio_dev*)p;
    int i;

    g_core_rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, XRA_CORE, 1u << XRA_CORE);
    svcSetThreadPriority(CUR_THREAD_HANDLE, XRA_PRIO);

    /* Prime every buffer so audout always has queued data. */
    for (i = 0; i < XRA_BUFFERS; ++i) {
        fill(d, &d->out[i]);
        audoutAppendAudioOutBuffer(&d->out[i]);
    }
    while (d->running) {
        AudioOutBuffer* released = NULL;
        u32 count = 0, got = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ULL /* 100 ms */)) || !count)
            continue;
        /* One buffer per call: take the rest too, or the queue shrinks for good after a late wake. */
        while (released && count && d->running) {
            if (ours(d, released)) {
                fill(d, released);
                audoutAppendAudioOutBuffer(released);
                ++got;
            }
            released = NULL;
            count = 0;
            if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)))
                break;
        }
        if (got >= XRA_BUFFERS)
            __atomic_add_fetch(&g_starved, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

int SDL_InitSubSystem(Uint32 flags)
{
    (void)flags;
    return 0;
}

const char* SDL_GetError(void) { return ""; }

int SDL_GetNumAudioDevices(int iscapture) { return iscapture ? 0 : 1; }

const char* SDL_GetAudioDeviceName(int index, int iscapture)
{
    return (!iscapture && index == 0) ? "Nintendo Switch audout" : NULL;
}

static void drain_released(void)
{
    AudioOutBuffer* b = NULL;
    u32 n = 0;
    int guard = 64;
    while (guard-- > 0 && R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&b, &n)) && n) {
    }
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                      SDL_AudioSpec* obtained, int allowed_changes)
{
    xr_audio_dev* d;
    u32 frames, i;
    (void)device;
    (void)allowed_changes;

    if (iscapture || g_dev || !desired)
        return 0; /* capture is not supported; one playback device at a time */

    if (!g_audout_up) {
        if (R_FAILED(audoutInitialize()))
            return 0;
        g_audout_up = 1;
    }
    drain_released(); /* leftovers of a previous open: freed memory, see ours() */
    if (R_FAILED(audoutStartAudioOut()))
        return 0;

    d = (xr_audio_dev*)calloc(1, sizeof(*d));
    if (!d)
        return 0;
    d->callback = desired->callback;
    d->userdata = desired->userdata;
    pthread_mutex_init(&d->lock, NULL);

    /* Round the requested period up to whole 1024-frame pages (the 0x1000-byte audout granule). */
    frames = desired->samples ? desired->samples : 1024;
    frames = (frames + 1023u) & ~1023u;
    d->frames = frames;
    d->bytes = frames * XRA_CHANNELS * (u32)sizeof(s16);
    d->bytes = (d->bytes + (XRA_ALIGN - 1)) & ~(XRA_ALIGN - 1);

    for (i = 0; i < XRA_BUFFERS; ++i) {
        d->mem[i] = memalign(XRA_ALIGN, d->bytes);
        if (!d->mem[i])
            goto fail;
        memset(d->mem[i], 0, d->bytes);
        d->out[i].next = NULL;
        d->out[i].buffer = d->mem[i];
        d->out[i].buffer_size = d->bytes;
        d->out[i].data_size = d->bytes;
        d->out[i].data_offset = 0;
    }

    if (obtained) {
        memset(obtained, 0, sizeof(*obtained));
        obtained->freq = XRA_RATE;
        obtained->format = AUDIO_S16SYS;
        obtained->channels = XRA_CHANNELS;
        obtained->samples = (Uint16)frames;
        obtained->size = d->bytes;
        obtained->callback = desired->callback;
        obtained->userdata = desired->userdata;
    }

    d->paused = 1; /* SDL opens devices paused; OpenAL un-pauses when it is ready */
    d->running = 1;
    g_dev = d;
    g_period_frames = frames;
    if (pthread_create(&d->thread, NULL, audio_thread, d) != 0) {
        g_dev = NULL;
        goto fail;
    }
    return 1; /* the one and only device id */

fail:
    for (i = 0; i < XRA_BUFFERS; ++i)
        free(d->mem[i]);
    pthread_mutex_destroy(&d->lock);
    free(d);
    audoutStopAudioOut();
    return 0;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
    xr_audio_dev* d = g_dev;
    if (dev != 1 || !d)
        return;
    /* Under the callback's lock: when this returns, no callback is running (OpenAL frees mixing state next). */
    pthread_mutex_lock(&d->lock);
    d->paused = pause_on ? 1 : 0;
    pthread_mutex_unlock(&d->lock);
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev)
{
    xr_audio_dev* d = g_dev;
    u32 i;
    if (dev != 1 || !d)
        return;
    pthread_mutex_lock(&d->lock);
    d->paused = 1;
    d->callback = NULL; /* the device it mixes for is being destroyed */
    pthread_mutex_unlock(&d->lock);
    d->running = 0;
    pthread_join(d->thread, NULL);
    audoutStopAudioOut();
    {
        bool flushed = false;
        audoutFlushAudioOutBuffers(&flushed); /* drop what is still queued: its memory is freed below */
    }
    drain_released();
    g_dev = NULL;
    g_period_frames = 0;
    for (i = 0; i < XRA_BUFFERS; ++i)
        free(d->mem[i]);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

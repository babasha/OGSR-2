// Switch-only: FPS and resolution for the console overlays (Status Monitor Overlay, Horizon OC Monitor).
//
// The overlays do not measure anything: they read SaltyNX's NxFpsSharedBlock from a 4 KB page SaltyNX shares, which its
// NX-FPS plugin fills from inside a game. SaltyNX never loads into a homebrew NRO, so we write the block ourselves
// (approach documented by the nfsmw-nx port, docs/console-overlays.md; client written here from SaltyNX's service
// interface: cmd 7 = page handle, cmd 6 = reserve {size} -> {result, offset}, cmd 0 = end session).
//
//  * once, on a background thread: connect to the "SaltySD" port, get and map the page, find our old block (the page
//    is only reset when SaltyNX loads into a game, so reserving on every launch would use it up) or reserve one, end
//    the session and close our handle -- the port takes one client, the user's overlay needs it;
//  * every present (xr_nx_overlay_present, called from the nwindowQueueBuffer wrapper in xr_nx_prof.cpp): set
//    pluginActive -- the overlay clears it and checks 100 ms later that the game set it back --, the frame time
//    ring, the frame number and the resolution; once per second also FPS and the 10 s average.
// Without SaltyNX the port does not exist (0xF201) and nothing happens.
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
struct ResolutionCalls
{
    uint16_t width, height, calls;
} __attribute__((packed));

struct NxFpsSharedBlock
{
    uint32_t MAGIC; // 0x465053
    uint8_t FPS;
    float FPSavg;
    bool pluginActive;
    uint8_t FPSlocked, FPSmode, ZeroSync, patchApplied;
    uint8_t API; // 3 = Vulkan
    uint32_t FPSticks[10];
    uint8_t Buffers, SetBuffers, ActiveBuffers, SetActiveBuffers;
    uint8_t displaySync;
    ResolutionCalls renderCalls[8];
    ResolutionCalls viewportCalls[8];
    bool forceOriginalRefreshRate, dontForce60InDocked, forceSuspend;
    uint8_t currentRefreshRate;
    float readSpeedPerSecond;
    uint8_t FPSlockedDocked;
    uint64_t frameNumber;
    int8_t expectedSetBuffers;
} __attribute__((packed));
static_assert(sizeof(NxFpsSharedBlock) == 174, "SaltyNX block layout");
constexpr uint32_t kMagic = 0x465053;

SharedMemory g_shm;
std::atomic<NxFpsSharedBlock*> g_block{nullptr};
Thread g_thread;

// written by the presenting thread only
u64 g_last_tick = 0, g_second_tick = 0, g_frames = 0, g_frames_at_second = 0;
unsigned g_tick_pos = 0;
float g_hist[10] = {};
unsigned g_hist_pos = 0;
uint16_t g_calls_last_second = 0;

NxFpsSharedBlock* find_block(u8* page)
{
    for (size_t off = 0; off + sizeof(NxFpsSharedBlock) <= 0x1000; off += 4)
    {
        uint32_t m;
        memcpy(&m, page + off, 4);
        if (m == kMagic)
            return reinterpret_cast<NxFpsSharedBlock*>(page + off);
    }
    return nullptr;
}

bool connect_once(int attempt)
{
    Handle port = INVALID_HANDLE;
    Result rc = svcConnectToNamedPort(&port, "SaltySD");
    if (R_FAILED(rc))
    {
        if (attempt == 0 || rc != 0xF201)
            fprintf(stderr, "[saltynx] connect SaltySD rc=0x%X%s\n", rc,
                    rc == 0xF201 ? " (SaltyNX not installed / not started)" : rc == 0x10801 ? " (no free session)" : "");
        return rc == 0xF201 && attempt > 3; // not installed: give up after a few seconds
    }
    Service s;
    serviceCreate(&s, port);
    Handle page_handle = INVALID_HANDLE;
    rc = serviceDispatch(&s, 7, .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &page_handle);
    bool done = false;
    if (R_SUCCEEDED(rc) && page_handle != INVALID_HANDLE)
    {
        shmemLoadRemote(&g_shm, page_handle, 0x1000, Perm_Rw);
        if (R_SUCCEEDED(shmemMap(&g_shm)))
        {
            u8* page = static_cast<u8*>(shmemGetAddr(&g_shm));
            NxFpsSharedBlock* b = find_block(page);
            const bool reused = b != nullptr;
            if (!b)
            {
                const struct
                {
                    u64 size, reserved;
                } in = {sizeof(NxFpsSharedBlock), 0};
                struct
                {
                    u64 result, offset, reserved;
                } out = {};
                rc = serviceDispatchInOut(&s, 6, in, out);
                if (R_SUCCEEDED(rc) && out.result == 0 && out.offset + sizeof(NxFpsSharedBlock) <= 0x1000)
                {
                    b = reinterpret_cast<NxFpsSharedBlock*>(page + out.offset);
                    memset(b, 0, sizeof(*b));
                    b->MAGIC = kMagic;
                    // a zero FPSticks ring makes the overlay divide by zero ("inf"): seed it with 30 fps
                    for (int k = 0; k < 10; ++k) // index, not range-for: no references into a packed struct
                        b->FPSticks[k] = (uint32_t)(armGetSystemTickFreq() / 30);
                }
                else
                    fprintf(stderr, "[saltynx] reserve rc=0x%X result=0x%llX offset=%lld\n", rc,
                            (unsigned long long)out.result, (long long)out.offset);
            }
            if (b)
            {
                b->API = 3;
                b->pluginActive = true;
                g_block.store(b);
                fprintf(stderr, "[saltynx] FPS block %s at page+0x%X (attempt %d)\n", reused ? "reused" : "reserved",
                        (unsigned)(reinterpret_cast<u8*>(b) - page), attempt);
                done = true;
            }
        }
        else
            fprintf(stderr, "[saltynx] shmemMap failed\n");
    }
    else
        fprintf(stderr, "[saltynx] page handle rc=0x%X\n", rc);
    serviceDispatch(&s, 0); // end the session on SaltyNX's side
    serviceClose(&s);       // and free the port: it takes one client, the user's overlay needs it
    return done;
}

void overlay_main(void*)
{
    for (int attempt = 0; attempt < 60; ++attempt)
    {
        if (connect_once(attempt))
            return;
        svcSleepThread(1000000000LL);
    }
}
} // namespace

extern "C" {

void xr_nx_overlay_init(void)
{
    if (R_SUCCEEDED(threadCreate(&g_thread, overlay_main, nullptr, nullptr, 0x8000, 0x3B, 2)))
        threadStart(&g_thread);
}

// Every present: ~30 bytes into already mapped memory, no system call.
void xr_nx_overlay_present(uint32_t width, uint32_t height)
{
    NxFpsSharedBlock* b = g_block.load(std::memory_order_relaxed);
    const u64 now = armGetSystemTick();
    ++g_frames;
    if (!b)
    {
        g_last_tick = now;
        return;
    }
    if (g_last_tick)
    {
        b->FPSticks[g_tick_pos] = (uint32_t)(now - g_last_tick);
        g_tick_pos = (g_tick_pos + 1) % 10;
    }
    g_last_tick = now;
    b->pluginActive = true;
    b->frameNumber = g_frames;
    // calls must never be 0xFFFF: that is the overlay's "are you alive" probe value
    const uint16_t calls = g_calls_last_second ? g_calls_last_second : 1;
    b->renderCalls[0] = {(uint16_t)width, (uint16_t)height, calls};
    b->viewportCalls[0] = {(uint16_t)width, (uint16_t)height, calls};

    const u64 freq = armGetSystemTickFreq();
    if (!g_second_tick)
        g_second_tick = now, g_frames_at_second = g_frames;
    if (now - g_second_tick >= freq)
    {
        const double secs = double(now - g_second_tick) / double(freq);
        const float fps = float(double(g_frames - g_frames_at_second) / secs);
        g_hist[g_hist_pos] = fps;
        g_hist_pos = (g_hist_pos + 1) % 10;
        float sum = 0.f;
        int n = 0;
        for (float v : g_hist)
            if (v > 0.f)
                sum += v, ++n;
        b->FPS = (uint8_t)(fps > 255.f ? 255.f : fps + 0.5f);
        b->FPSavg = n ? sum / float(n) : fps;
        b->API = 3;
        g_calls_last_second = (uint16_t)(fps > 0.f && fps < 65000.f ? fps + 0.5f : 1);
        g_second_tick = now;
        g_frames_at_second = g_frames;
    }
}

} // extern "C"

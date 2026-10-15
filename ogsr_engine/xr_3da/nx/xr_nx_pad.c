/* Horizon HID -> xr_nx_pad_state. See xr_nx_pad.h for why this is C. */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "xr_nx_pad.h"

/* Set on the way out (xr_nx_exit_process). ⚠⚠The background threads here are detached infinite loops, and the first
 * quit path (_exit) tore libnx down (services, stdio) WHILE they still run: the clock watch woke up, read 0 MHz from a closed
 * clkrst session, and its fprintf into the dead stderr was a Data Abort in sem_wait -- an atmosphere crash report on
 * every quit from the menu (03-10). Each loop now leaves as soon as it wakes and sees this. */
volatile int g_xr_nx_quit = 0;

/* ⚠⚠THE WAY OUT IS svcExitProcess, NOT _exit (03-10, second quit crash). Under hbloader `_exit` does not end the process:
 * libnx returns to the LOADER, which unmaps this NRO and loads the next one -- while all of the engine's ~16 threads
 * (workers, render, sound, the three above) are still alive. The first one to wake jumps into unmapped code: an
 * Instruction Abort whose report lists only the loader module. Telling each thread to stop is a race nobody can win
 * for threads that sleep for seconds; ending the PROCESS stops them all in the kernel. The console then goes to the
 * HOME menu rather than back to hbmenu, which is what "close the game" means anyway. */
void xr_nx_stderr_flush(void); /* xr_nx_link.c: stderr.log is written behind */

void xr_nx_exit_process(void)
{
    g_xr_nx_quit = 1;
    fflush(stdout);
    fflush(stderr);
    xr_nx_stderr_flush();
    svcExitProcess();
}

static PadState g_pad;
static int g_inited;

void xr_nx_pad_init(void)
{
    if (g_inited)
        return;
    /* Every player and handheld feed one PadState (buttons OR-ed). Player 1 + handheld alone missed sys-botbase's virtual
       controller, which attaches as another player: remote presses from the PC did nothing (07-10). */
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&g_pad);
    hidInitializeTouchScreen();
    g_inited = 1;
}

static uint32_t map_buttons(uint64_t k)
{
    uint32_t b = 0;
    if (k & HidNpadButton_A) b |= XRPAD_A;
    if (k & HidNpadButton_B) b |= XRPAD_B;
    if (k & HidNpadButton_X) b |= XRPAD_X;
    if (k & HidNpadButton_Y) b |= XRPAD_Y;
    if (k & HidNpadButton_StickL) b |= XRPAD_LSTICK;
    if (k & HidNpadButton_StickR) b |= XRPAD_RSTICK;
    if (k & HidNpadButton_L) b |= XRPAD_L;
    if (k & HidNpadButton_R) b |= XRPAD_R;
    if (k & HidNpadButton_ZL) b |= XRPAD_ZL;
    if (k & HidNpadButton_ZR) b |= XRPAD_ZR;
    if (k & HidNpadButton_Plus) b |= XRPAD_PLUS;
    if (k & HidNpadButton_Minus) b |= XRPAD_MINUS;
    if (k & HidNpadButton_Left) b |= XRPAD_DLEFT;
    if (k & HidNpadButton_Up) b |= XRPAD_DUP;
    if (k & HidNpadButton_Right) b |= XRPAD_DRIGHT;
    if (k & HidNpadButton_Down) b |= XRPAD_DDOWN;
    return b;
}

void xr_nx_pad_poll(xr_nx_pad_state* out)
{
    HidTouchScreenState touch;

    memset(out, 0, sizeof(*out));
    /* appletMainLoop() services HOME / sleep / focus messages; a title that never calls it is killed by
       the system. It returns false when the OS wants the title gone. */
    out->quit = appletMainLoop() ? 0 : 1;
    if (!g_inited)
        return;

    padUpdate(&g_pad);
    out->buttons = map_buttons(padGetButtons(&g_pad));

    {
        HidAnalogStickState l = padGetStickPos(&g_pad, 0);
        HidAnalogStickState r = padGetStickPos(&g_pad, 1);
        out->lx = (int16_t)l.x;
        out->ly = (int16_t)l.y;
        out->rx = (int16_t)r.x;
        out->ry = (int16_t)r.y;
    }

    memset(&touch, 0, sizeof(touch));
    if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0) {
        out->touch_count = (uint32_t)touch.count;
        for (int i = 0; i < 2 && i < touch.count; ++i) {
            out->touch_id[i] = touch.touches[i].finger_id;
            out->touch_x[i] = (int32_t)touch.touches[i].x;
            out->touch_y[i] = (int32_t)touch.touches[i].y;
        }
    }
}

/* Size of this title's compositor layer: 1280x720 handheld, 1920x1080 docked. The Vulkan swapchain follows the surface. */
void xr_nx_display_size(unsigned* w, unsigned* h)
{
    const int handheld = (appletGetOperationMode() == AppletOperationMode_Handheld);
    *w = handheld ? 1280u : 1920u;
    *h = handheld ? 720u : 1080u;
}


/* --- where is the main thread? ---------------------------------------------------------------------------
 * A homebrew NRO has no debugger. Every 4 s, for the first 30 s, this thread pauses the main thread, reads
 * its PC/LR/SP/FP, walks the frame-pointer chain and prints it to stderr.log, then lets it run again.
 * Addresses are module-relative once `self` (printed) is subtracted from nm's value for xr_nx_sampler.
 */
#include <pthread.h>
#include <stdio.h>
#include <malloc.h>

extern volatile int g_xr_nx_loading; /* below: 1 while the engine loads */

static void* xr_nx_sampler(void* arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 400; ++i) { /* 20 min: the interesting part starts AFTER the "press a key" screen */
        Handle h;
        ThreadContext ctx;
        Result rc;
        u64 tot = 0, used = 0;
        struct mallinfo mi;
        svcSleepThread(3000000000ULL);
        if (g_xr_nx_quit)
            break;
        svcGetInfo(&tot, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        /* ⚠mallinfo() only while loading (07-10): newlib walks the whole heap for it (and consolidates the fastbins)
         * under the malloc lock -- ~7.5 million small blocks in a 2 GB arena = ~25 ms in which no thread can allocate.
         * In the game that was a 25 ms frame every 3 s for the first 20 minutes of every session: [frame prof] showed
         * the same 25 ms landing in whatever part ran at the time (Lua, UpdateCL, a model, the scheduler). */
        if (g_xr_nx_loading)
        {
            mi = mallinfo();
            fprintf(stderr, "[mem %d] process total=%llu MB used=%llu MB | malloc arena=%d MB in-use=%d MB\n", i, (unsigned long long)(tot >> 20),
                    (unsigned long long)(used >> 20), (int)((unsigned)mi.arena >> 20), (int)((unsigned)mi.uordblks >> 20));
        }
        else
            fprintf(stderr, "[mem %d] process total=%llu MB used=%llu MB | malloc: not walked in the game\n", i, (unsigned long long)(tot >> 20),
                    (unsigned long long)(used >> 20));
        {
            extern unsigned long long g_xr_bucket_bytes[6], g_xr_bucket_count[6];
            fprintf(stderr, "[alloc %d] MB<4K=%llu(%lluk) <64K=%llu(%lluk) <256K=%llu(%lluk) <1M=%llu(%lluk) <2M=%llu(%lluk) >=2M=%llu(%lluk)\n", i,
                    g_xr_bucket_bytes[0] >> 20, g_xr_bucket_count[0] >> 10, g_xr_bucket_bytes[1] >> 20, g_xr_bucket_count[1] >> 10,
                    g_xr_bucket_bytes[2] >> 20, g_xr_bucket_count[2] >> 10, g_xr_bucket_bytes[3] >> 20, g_xr_bucket_count[3] >> 10,
                    g_xr_bucket_bytes[4] >> 20, g_xr_bucket_count[4] >> 10, g_xr_bucket_bytes[5] >> 20, g_xr_bucket_count[5] >> 10);
        }
        {
            extern void xr_nx_indirect_stats(unsigned long long out[4]);
            unsigned long long st[4];
            xr_nx_indirect_stats(st);
            fprintf(stderr, "[indirect %d] counted-calls=%llu uncounted-calls=%llu drawn=%llu of max=%llu\n", i, st[0], st[1], st[2], st[3]);
        }
        h = envGetMainThreadHandle();
        if (!h)
            continue;
        /* ⚠⚠The main thread is paused ONLY long enough to copy its registers and frame chain into locals. Printing while it
         * is paused deadlocked the process at random (30-09): the main thread is often inside fsdev_write holding libnx's
         * filesystem lock, the sampler's fprintf needs that same lock, and the paused thread can never release it -- the
         * whole game froze with no crash report and no further log lines from any thread. No I/O, no malloc, no locks here.
         */
        {
            unsigned long long chain[16];
            unsigned long long pc = 0, lr = 0, sp = 0;
            int n = 0, ok = 0;
            rc = svcSetThreadActivity(h, ThreadActivity_Paused);
            if (R_FAILED(rc)) {
                fprintf(stderr, "[sample %d] pause failed rc=0x%x\n", i, rc);
                continue;
            }
            rc = svcGetThreadContext3(&ctx, h);
            if (R_SUCCEEDED(rc)) {
                unsigned long long* fp = (unsigned long long*)ctx.fp;
                unsigned long long lo = ctx.sp, hi = ctx.sp + 0x1000000ULL;
                ok = 1;
                pc = ctx.pc.x;
                lr = ctx.lr;
                sp = ctx.sp;
                while (n < 16 && fp && ((unsigned long long)fp & 7) == 0 && (unsigned long long)fp >= lo && (unsigned long long)fp < hi) {
                    chain[n++] = fp[1];
                    if (fp[0] <= (unsigned long long)fp)
                        break;
                    fp = (unsigned long long*)fp[0];
                }
            }
            svcSetThreadActivity(h, ThreadActivity_Runnable);
            if (ok) {
                int d;
                fprintf(stderr, "[sample %d] self=%p pc=%llx lr=%llx sp=%llx\n", i, (void*)&xr_nx_sampler, pc, lr, sp);
                for (d = 0; d < n; ++d)
                    fprintf(stderr, "  #%d %llx\n", d, chain[d]);
            } else {
                fprintf(stderr, "[sample %d] getcontext failed rc=0x%x\n", i, rc);
            }
        }
    }
    return NULL;
}

/* Per-core load probes (v104): cumulative idle ticks of core i (system ticks, 19.2 MHz) and when they were read. */
volatile unsigned long long g_xr_nx_core_idle[3];
volatile unsigned long long g_xr_nx_core_stamp[3];

static void* xr_nx_core_probe(void* arg)
{
    const int core = (int)(intptr_t)arg;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    for (;;)
    {
        u64 idle = 0;
        if (R_SUCCEEDED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, (u64)-1)))
        {
            g_xr_nx_core_idle[core]  = idle;
            g_xr_nx_core_stamp[core] = armGetSystemTick();
        }
        svcSleepThread(100000000ULL);
        if (g_xr_nx_quit)
            break;
    }
    return NULL;
}

void xr_nx_start_core_probes(void)
{
    for (int c = 0; c < 3; ++c)
    {
        pthread_t t;
        if (pthread_create(&t, NULL, xr_nx_core_probe, (void*)(intptr_t)c) == 0)
            pthread_detach(t);
    }
}

void xr_nx_start_sampler(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, xr_nx_sampler, NULL) == 0)
        pthread_detach(t); /* rc=88 on Horizon, harmless */
}

/* --- GPU clock ---------------------------------------------------------------------------------------------------------
 * A homebrew NRO starts with whatever clocks the shell left (often the power-saving GPU step). The engine's own profiler
 * put a trivial 720p tonemap at ~90 ms, which is what a 76-307 MHz GM20B would do. Ask for the STOCK maximum of the current
 * mode (handheld 460.8 MHz, docked 768 MHz) -- no overclock -- and print before/after so a run shows whether it took.
 * The session stays open for the life of the process.
 */
#include <stdio.h>
static ClkrstSession g_xr_gpu_clk;
static int g_xr_gpu_clk_open;

void xr_nx_gpu_clock_boost(void)
{
    const u32 target = (appletGetOperationMode() == AppletOperationMode_Handheld) ? 460800000u : 768000000u;
    u32 before = 0, after = 0;
    Result rc;
    if (hosversionAtLeast(8, 0, 0)) {
        rc = clkrstInitialize();
        if (R_SUCCEEDED(rc))
            rc = clkrstOpenSession(&g_xr_gpu_clk, PcvModuleId_GPU, 3);
        if (R_FAILED(rc)) {
            fprintf(stderr, "[nx clk] clkrst open failed rc=0x%x\n", rc);
            return;
        }
        g_xr_gpu_clk_open = 1;
        clkrstGetClockRate(&g_xr_gpu_clk, &before);
        rc = clkrstSetClockRate(&g_xr_gpu_clk, target);
        clkrstGetClockRate(&g_xr_gpu_clk, &after);
    } else {
        rc = pcvInitialize();
        if (R_FAILED(rc)) {
            fprintf(stderr, "[nx clk] pcv init failed rc=0x%x\n", rc);
            return;
        }
        pcvGetClockRate(PcvModule_GPU, &before);
        rc = pcvSetClockRate(PcvModule_GPU, target);
        pcvGetClockRate(PcvModule_GPU, &after);
    }
    fprintf(stderr, "[nx clk] GPU %u Hz -> requested %u Hz (set rc=0x%x) -> now %u Hz\n", before, target, rc, after);
}

/* --- clock watch (v122) ------------------------------------------------------------------------------------------------
 * The boost above is applied ONCE. If the system later puts the GPU back to its own default (handheld 307.2 MHz), the
 * whole frame runs 1.5x slower and nothing says so. Every 500 ms: read GPU/CPU/EMC; if the GPU is below the stock maximum
 * of the CURRENT mode, set it again and say so. [nx clk] status every 3 s; [nx frame] reads the globals below.
 */
volatile unsigned g_xr_nx_gpu_mhz, g_xr_nx_cpu_mhz, g_xr_nx_emc_mhz, g_xr_nx_clk_resets;
/* Load-time CPU boost (07-10): while the engine says it is loading (device.cpp sets this every frame from the load
 * screen; 1 from process start, so the boot to the main menu counts too), the CPU runs at 1785 MHz -- the rate of the
 * system's own ApmCpuBoostMode_FastLoad, which games use for loading screens -- and back at the stock 1020 MHz after.
 * Not appletSetCpuBoostMode itself: that mode also drops the GPU to 76.8 MHz, against the GPU watch below.
 * A save load on Kordon was ~47 s, nearly all of it CPU: LZO, the save parse, 1050 objects spawned, their scripts. */
volatile int g_xr_nx_loading = 1;
void xr_nx_io_dump(const char* tag); /* cmake/nx_compat/nx_libc_shims.c: [nx io] SD read accounting */
#define XR_NX_CPU_STOCK 1020000000u
#define XR_NX_CPU_LOAD  1785000000u
static void* xr_nx_clock_watch(void* arg)
{
    (void)arg;
    ClkrstSession cpu, emc;
    const int haveCpu = g_xr_gpu_clk_open && R_SUCCEEDED(clkrstOpenSession(&cpu, PcvModuleId_CpuBus, 3));
    const int haveEmc = g_xr_gpu_clk_open && R_SUCCEEDED(clkrstOpenSession(&emc, PcvModuleId_EMC, 3));
    int boosted = 0;
    for (unsigned tick = 0;; ++tick)
    {
        svcSleepThread(boosted ? 100000000ULL : 500000000ULL); /* while loading: notice the end within a frame or six */
        if (g_xr_nx_quit)
            break;
        if (!g_xr_gpu_clk_open)
            continue;
        if (haveCpu && boosted != (g_xr_nx_loading != 0))
        {
            boosted = g_xr_nx_loading != 0;
            const Result rc = clkrstSetClockRate(&cpu, boosted ? XR_NX_CPU_LOAD : XR_NX_CPU_STOCK);
            u32 now = 0;
            clkrstGetClockRate(&cpu, &now);
            fprintf(stderr, "[nx clk] CPU %s: -> %u MHz (rc=0x%x)\n", boosted ? "load boost" : "back to stock", now / 1000000u, rc);
        }
        const u32 target = (appletGetOperationMode() == AppletOperationMode_Handheld) ? 460800000u : 768000000u;
        u32 gpu = 0, c = 0, e = 0;
        clkrstGetClockRate(&g_xr_gpu_clk, &gpu);
        if (haveCpu) clkrstGetClockRate(&cpu, &c);
        if (haveEmc) clkrstGetClockRate(&emc, &e);
        if (boosted && c + 1000000u < XR_NX_CPU_LOAD)   /* something (a clock sysmodule) took the boost back */
            clkrstSetClockRate(&cpu, XR_NX_CPU_LOAD);
        if (gpu + 1000000u < target)
        {
            const Result rc = clkrstSetClockRate(&g_xr_gpu_clk, target);
            u32 now = 0;
            clkrstGetClockRate(&g_xr_gpu_clk, &now);
            ++g_xr_nx_clk_resets;
            fprintf(stderr, "[nx clk] GPU fell to %u MHz -> re-set %u MHz (rc=0x%x) -> now %u MHz (#%u)\n", gpu / 1000000u,
                    target / 1000000u, rc, now / 1000000u, g_xr_nx_clk_resets);
            gpu = now;
        }
        g_xr_nx_gpu_mhz = gpu / 1000000u;
        g_xr_nx_cpu_mhz = c / 1000000u;
        g_xr_nx_emc_mhz = e / 1000000u;
        if (tick % (boosted ? 30 : 6) == 0)   /* ~3 s either way */
            fprintf(stderr, "[nx clk] gpu %u MHz cpu %u MHz emc %u MHz resets %u\n", g_xr_nx_gpu_mhz, g_xr_nx_cpu_mhz,
                    g_xr_nx_emc_mhz, g_xr_nx_clk_resets);
        /* in the game, the SD reads of the last 10 s (spawns, streaming); a load reports them per phase instead */
        if (!boosted && tick % 20 == 0)
            xr_nx_io_dump("10 s");
    }
    return NULL;
}

void xr_nx_start_clock_watch(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, xr_nx_clock_watch, NULL) == 0)
        pthread_detach(t);
}

/* Set from `-nx_noglass` (args.txt): skip the World/Glass pass (it measured ~89 % of a world frame on the console). */
int g_xr_nx_skip_glass = 0;


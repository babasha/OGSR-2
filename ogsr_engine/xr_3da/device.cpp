#include "stdafx.h"

#include "x_ray.h"
#include "Render.h"
#include "xr_input.h"
#include "IGame_Persistent.h"
#include "imgui.h"
#include "IGame_Level.h"
#include "splash.h"
#ifdef _MSC_VER
#include <mmsystem.h> // unused in this file (grepped) -- dead include
#endif
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <fstream>
#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"
#include "lua_vm_watch.h"
#include "fs_load_prefetch.h"   // LoadPrefetch::End when the precache is over
#include "cpu_budget.h"   // CpuBudget::Frame — цена кадра в ядро-миллисекундах
#include "../xrCore/load_prof.h"   // [frame prof]
#include "../xrCore/pinned_worker.h"   // snd_core: xrSound's update on a chosen core
ENGINE_API int g_frame_prof_ms = 0; // dbg_frame_prof: print the parts of a main-thread frame_move this long (ms); 0 = off
ENGINE_API float g_2nd_net_ms = 0.f; // the net tick of this frame's seqFrameMT (_NetworkProcessor adds, [nx 2nd] reads)
// dbg_rt_prof: the same for a render-thread job (record / end) this long; 0 = off. Switch: on at 100 ms (11-10) -- the
// rare rt_wait stalls of 180-590 ms in fights would not repeat in 16 test fights; armed, it names the part when one
// comes in real play. 📏gc7/gc8 fights with it at 60 ms: 60 fps, p95 17.1-17.4 ms, as without.
#ifdef __SWITCH__
ENGINE_API int g_rt_prof_ms = 100;
#else
ENGINE_API int g_rt_prof_ms = 0;
#endif
ENGINE_API int g_hitch_ms = 33;     // dbg_hitch_ms: [hitch] breakdown of a frame this long (a doubled 60 Hz frame by default)

// [rt prof] (08-10-2026): 📏Switch v236 the fight's last slow frames were rt_wait 23-61 ms -- the main
// thread waiting for the previous frame's render-thread job, with nothing saying what that job did.
// Its own Arm (LoadProf is per thread): the passes by name, rt.fence / rt.acquire / rt.submit /
// rt.present / rt.immediate, rt.pso_gfx / rt.pso_comp, texture and model parts if the job reaches them.
void RT_Profiled(const char* what, const std::function<void()>& job)
{
    if (g_rt_prof_ms <= 0)
    {
        job();
        return;
    }
    LoadProf::Arm arm;
    const u64 t0 = CPU::QPC();
    {
        LoadProf::Scope root(what);
        job();
    }
    const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
    static u32 s_printed = 0;
    if (ms >= g_rt_prof_ms && arm.outer && s_printed < 300 && !Device.dwPrecacheFrame)
    {
        ++s_printed;
        string2048 parts;
        arm.format(parts, sizeof(parts), 0.5);
        Msg("~ [rt prof] frame %u %s %.1f ms: %s", Device.dwFrame, what, ms, parts);
    }
}
#include "xr_platform_time.h"
#ifdef __SWITCH__
extern "C" volatile int g_xr_nx_loading; // nx/xr_nx_pad.c: CPU at the load-boost clock while set
#endif

//#define LOG_SECOND_THREAD_STATS

// ImGui backend seam, supplied by the renderer. Used to be the DX11 backend that
// lived in the render layer; the Vulkan renderer draws its own overlay
// (VK::ImGuiVK::DrawOverlay) and leaves these as no-ops.
void ImGui_Backend_NewFrame();
void ImGui_Backend_RenderDrawData(ImDrawData* draw_data);

ENGINE_API CRenderDevice Device;
ENGINE_API CLoadScreenRenderer load_screen_renderer;

ENGINE_API BOOL g_bRendering = FALSE;

extern ENGINE_API float psHUD_FOV;

u32 g_dwFPSlimit{};
// Сколько микросекунд перед дедлайном лимитера кадров дожидать спином (yield),
// а не таймером. Ноль = только таймер; 2000 = почти прежнее поведение, для A/B
// в одном прогоне. Консольный `r_fps_spin_us`, NoSave. См. хвост on_idle().
int g_fps_spin_us{250};

BOOL g_bLoaded = FALSE;

bool IsEditorShouldOpen();
bool is_editor_active{};
// The embedded editor's ImGui frame is open: set after ImGui_Backend_NewFrame (main thread, frame start), cleared by
// End_Render's ImGui::Render (render thread, while the main thread waits in End -- editor frames never overlap).
static bool s_imguiFrameOpen{};

extern ENGINE_API xr_list<fastdelegate::FastDelegate<bool()>> g_loading_events;

#ifdef __SWITCH__
extern "C" void xr_vk_wait_idle();
#endif

// ---------------------------------------------------------------------------------------------------------------
// RENDER THREAD.
// A persistent std::thread (2 MB stack through the pthread_create wrap), pinned to core 1 on the Switch, that
// executes queued jobs in order. busy = something queued or running.
// ⭐EVERY frame is recorded and presented here (03-10) -- gameplay, menu, load screen, precache, editor. The main
// thread does Begin + extract + UI emission and hands the rest over; there is no main-thread render path left.
// What differs between frames is only whether the main thread may RUN AHEAD (RT_CanOverlap):
//   gameplay  : overlap -- frame N's record + present run beside frame N+1's logic AND extract (two slots: RT::Slot); a
//               new frame only waits until at most one earlier frame is unfinished (RT_WaitFramesInFlight).
//   otherwise : End() joins before it returns -- the load steps, the precache bookkeeping (ClearTarget, the
//               warm-up query) and ImGui touch renderer state right after a frame, so nothing may still be in
//               flight there. Those frames gain no parallelism, and need none: the work between them is the load.
// The full join points (RT_WaitIdle): Begin of a non-gameplay frame (LoadDraw from Frame_Logic included), End, level load/unload, device reset/destroy, precache, and before anything that
// waits the whole device idle. 📏PC, uncapped gameplay: 625 -> 760 fps (main thread was 98% busy recording).
// ---------------------------------------------------------------------------------------------------------------
#ifdef __SWITCH__
extern "C" u32 svcSetThreadCoreMask(u32 thread, s32 preferred_core, u32 affinity_mask);   // libnx (no switch.h here)
extern "C" void xr_nx_render_thread_started(void);
#endif
namespace
{
struct RtState
{
    std::thread th;
    std::mutex mx;
    std::condition_variable cv, cvIdle, cvDone;
    u32 done = 0;   // frames whose End_Render job has run (RT_FrameDone)
    std::deque<std::function<void()>> q;
    bool busy = false, quit = false, started = false;
};
RtState g_rt;
// TWO-SLOT PIPELINE: the next frame may begin while the previous one is still being recorded (at most ONE frame in flight
// ahead of the one being extracted). XROS_RT_ONESLOT=1 restores the old behaviour (join before every Begin) for an A/B or a
// bisect of a hang.
bool g_rtTwoSlot = []() { const char* e = std::getenv("XROS_RT_ONESLOT"); return !(e && atoi(e) != 0); }();
bool g_beginOverlap = false;   // Frame_Extract: this frame is a gameplay frame, so Begin joins only down to one frame in flight
bool g_rtOverlap = false;   // the frame in flight was handed over with the main thread running ahead (main thread only)

// Instrument (`cpu_budget` >= 1, one line per 5 s): who waits for whom. Neither a busy-cycle count (cpu_budget) nor
// the GPU time says it -- with both threads and the GPU each well under 100%, the frame is bounded by the HANDOFF,
// and these three numbers are its price: the main thread blocked on the render thread, and the render thread with
// nothing to do (waiting for the main thread to hand over the next frame).
std::atomic<u64> g_rtMainWaitUs{0}, g_rtBusyUs{0}, g_rtIdleUs{0};

void rt_main()
{
    set_current_thread_name("X-RAY Render thread");
#ifdef __SWITCH__
    svcSetThreadCoreMask(0xFFFF8000u /*CUR_THREAD_HANDLE*/, 1, 1u << 1);
    xr_nx_render_thread_started(); // nx/xr_nx_prof.cpp: nx_cores.txt "rtprio=" + log of the final prio/mask
#endif
    RT::SetRole(RT::Render);
    for (;;)
    {
        std::function<void()> job;
        const u64 tIdle = CPU::QPC_us();
        {
            std::unique_lock<std::mutex> lk(g_rt.mx);
            g_rt.cv.wait(lk, [] { return !g_rt.q.empty() || g_rt.quit; });
            if (g_rt.q.empty() && g_rt.quit)
                break;
            job = std::move(g_rt.q.front());
            g_rt.q.pop_front();
        }
        const u64 tBusy = CPU::QPC_us();
        g_rtIdleUs.fetch_add(tBusy - tIdle, std::memory_order_relaxed);
        try { job(); }
        catch (...) { Msg("![rt] render thread job threw"); }
        g_rtBusyUs.fetch_add(CPU::QPC_us() - tBusy, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_rt.mx);
        if (g_rt.q.empty()) { g_rt.busy = false; g_rt.cvIdle.notify_all(); g_rt.cvDone.notify_all(); }   // RT_WaitFramesInFlight also wakes on "idle"
    }
}

void rt_ensure_started()
{
    if (g_rt.started) return;
    g_rt.started = true;
    g_rt.th = std::thread(rt_main);
    Msg("[rt] render thread started");
}
} // namespace

void RT_Enqueue(std::function<void()> job)
{
    rt_ensure_started();
    // The job belongs to the frame the main thread is extracting right now: it runs on the render thread with that
    // frame's slot (RT::Slot() there = tl_recSlot), whatever slot the main thread has moved on to by then.
    job = [slot = RT::Slot(), j = std::move(job)] { RT::SetRecordSlot(slot); j(); };
    std::lock_guard<std::mutex> lk(g_rt.mx);
    g_rt.q.push_back(std::move(job));
    g_rt.busy = true;
    g_rt.cv.notify_one();
}

// A frame is finished for the pipeline when its End_Render job has run (submit + present queued, its slot free for reuse).
void RT_FrameDone()
{
    std::lock_guard<std::mutex> lk(g_rt.mx);
    ++g_rt.done;
    g_rt.cvDone.notify_all();
}

// TWO-SLOT PIPELINE: blocks until at most `maxPending` begun frames are still unfinished. Extract of frame N+1 calls it with 1:
// frame N may still be recording, frame N-1 (which owns the slot N+1 is about to reuse) must be done. The idle escape is for a
// Begin that never got its End (nothing queued, so nothing can ever finish): do not wait for what cannot come.
void RT_WaitFramesInFlight(u32 maxPending)
{
    if (!g_rt.started || RT::tl_role == RT::Render) return;
    const u64 t0 = CPU::QPC_us();
    std::unique_lock<std::mutex> lk(g_rt.mx);
    g_rt.cvDone.wait(lk, [maxPending] { return RT::FrameSeq() - g_rt.done <= maxPending || !g_rt.busy; });
    g_rtMainWaitUs.fetch_add(CPU::QPC_us() - t0, std::memory_order_relaxed);
}

void RT_WaitIdle()
{
    if (!g_rt.started) return;
    // A job waiting for its own queue would wait forever. Code that joins "the renderer" can be reached from a
    // render-thread job too (a device-idle wait inside End, say); there the renderer IS this thread, and it is idle
    // with respect to everything queued before the current job.
    if (RT::tl_role == RT::Render) return;
    const u64 t0 = CPU::QPC_us();
    std::unique_lock<std::mutex> lk(g_rt.mx);
    g_rt.cvIdle.wait(lk, [] { return !g_rt.busy; });
    g_rtMainWaitUs.fetch_add(CPU::QPC_us() - t0, std::memory_order_relaxed);
}

// Joins the render thread at device destroy. The Switch leaves through _exit() and never needed it; on PC the static
// RtState destructor would otherwise destroy a joinable std::thread -> std::terminate on every exit.
void RT_Shutdown()
{
    if (!g_rt.started) return;
    {
        std::lock_guard<std::mutex> lk(g_rt.mx);
        g_rt.quit = true;
        g_rt.cv.notify_all();
    }
    if (g_rt.th.joinable())
        g_rt.th.join();
    g_rt.started = false;
    g_rt.quit = false;
    g_rt.done = RT::FrameSeq();   // everything begun has been drained
}

BOOL CRenderDevice::Begin()
{
    // No frame may begin over one still in flight on the render thread (see the RENDER THREAD note above) -- except that a
    // gameplay frame may begin while the PREVIOUS frame is still being recorded: its extract writes the other slot.
    if (g_beginOverlap) RT_WaitFramesInFlight(1); else RT_WaitIdle();

    switch (m_pRender->GetDeviceState())
    {
    case IRenderDeviceRender::dsOK: break;

    case IRenderDeviceRender::dsLost:
        // If the device was lost, do not render until we get it back
        Sleep(33);
        return FALSE;
        break;

    case IRenderDeviceRender::dsNeedReset:
        // Check if the device is ready to be reset
        Reset();
        break;

    default: R_ASSERT(0);
    }

    // A frame that is going to be extracted owns the next slot from here until its End_Render job has run. Everything the
    // extract side writes (RV/ES, visual lists, PREPARE products, UI queue...) goes to this slot, and every job queued
    // while it is current runs on the render thread bound to it (RT_Enqueue).
    RT::BeginFrameSeq();

    ::Render->Begin();

    R_ASSERT(g_bRendering == FALSE, "Something Strange");

    g_bRendering = TRUE;

    return TRUE;
}

void CRenderDevice::Clear() { ::Render->Clear(); }

// Wall clock of the precache window. The load phase timer bills these frames to
// 'st_client_synchronising' together with the spawn burst; this splits the two.
static u32 s_precacheT0 = 0;
static u32 s_precacheEndMs = 0;   // when the countdown reached zero
static u32 s_loadScreenT0 = 0;    // when the load screen went up
static u32 s_loadScreenFrames = 0;
static u32 s_frameMoveCalls = 0;   // does the GAME frame still advance while the screen is up?
static u32 s_precacheRan = 0;      // frames the countdown actually spent (it can stop early)

// The frame end that does NOT run ahead: everything the frame queued (the record, then End_Render) is finished when
// this returns. Load screen frames (LoadDraw) and every non-gameplay frame of the loop (Frame_Submit) end here.
void CRenderDevice::End()
{
    g_bRendering = FALSE;   // the extract window closes here, on the main thread
    RT_WaitIdle();     // the record job first: End_Main_Pre's precache bookkeeping touches the renderer (ClearTarget, warm-up query)
    End_Main_Pre();
    RT_Enqueue([this] { End_Render(); });
    RT_WaitIdle();
    End_Main_Post();
}

void CRenderDevice::End_Main_Pre()
{
    if (dwPrecacheFrame)
    {
        ::Sound->set_master_volume(0.f);

        const HWND insertPos = IsDebuggerPresent() ? HWND_NOTOPMOST : HWND_TOPMOST;

        SetWindowPos(Device.m_hWnd, insertPos, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        dwPrecacheFrame--;
        ++s_precacheRan;

        // ADAPTIVE STOP. The countdown is a fixed 60 because someone had to pick a
        // number; what the frames actually do is spread the renderer's per-frame
        // warm-up (one pipeline compile a frame, ~300 ms each from cold). Measured on
        // pripyat_full with a warm cache: the queue is dry by frame 30, and frames 30
        // to 60 create nothing while costing 40 ms of GPU apiece. Ask the renderer
        // instead of guessing -- a cold shader cache keeps its frames, a warm one
        // gives them back. The floor covers what does NOT report itself: the first
        // frames lazily create ~17 pipelines from real draws, and the game-side
        // scheduler runs in prefetch mode while the countdown lasts.
        // XROS_PRECACHE_ADAPTIVE=0 restores the fixed count.
        if (dwPrecacheFrame)
        {
            static const bool s_adaptive = []() {
                const char* e = std::getenv("XROS_PRECACHE_ADAPTIVE");
                return !(e && atoi(e) == 0);
            }();
            constexpr u32 kPrecacheFloor = 12;
            const u32 done = dwPrecacheTotal - dwPrecacheFrame;
            if (s_adaptive && done >= kPrecacheFloor && !::Render->PrecacheWarmupPending())
            {
                Msg("[precache] warm-up settled after %u of %u frames", done, dwPrecacheTotal);
                dwPrecacheFrame = 0;
            }
        }

        if (!load_screen_renderer.b_registered)
        {
            ::Render->ClearTarget();

            if (0 == dwPrecacheFrame)
            {
                ::Sound->set_master_volume(1.f);
            }
        }        

        if (0 == dwPrecacheFrame)
        {
            s_precacheEndMs = xrPlatform::TickCount();
            const u32 ms = s_precacheEndMs - s_precacheT0;
            Msg("[precache] %u of %u frames in %u ms (%.1f ms/frame)", s_precacheRan, dwPrecacheTotal, ms,
                s_precacheRan ? float(ms) / float(s_precacheRan) : 0.f);

            SetWindowPos(Device.m_hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

            //m_pRender->updateGamma();
            
            ::Sound->set_master_volume(1.f);

            //Memory.mem_compact();

            Msg("* MEMORY USAGE: %d K", Memory.mem_usage() / 1024);
            Msg("* End of synchronization A[%d] R[%d]", b_is_Active, b_is_Ready);
            LoadPrefetch::End();   // the load's reads are over: stop the reader, write this load's trace
        }
    }

}

void CRenderDevice::End_Render()
{
    // The frame is finished for the pipeline even if something below throws (rt_main swallows the exception): a leaked
    // count would make RT_WaitFramesInFlight wait for a frame that can never complete.
    struct FrameDoneGuard { ~FrameDoneGuard() { RT_FrameDone(); } } frameDone;
    // (g_bRendering is the EXTRACT window, owned by the main thread: closed in Frame_Submit / End, not here)

    extern BOOL g_appLoaded;

    // Only a frame the editor actually opened (on_idle's ImGui_Backend_NewFrame). End also closes load screen frames,
    // which never started one; rendering there would emit the previous frame's windows twice -- or, with no
    // context at all, fault.
    if (g_appLoaded && is_editor_active && s_imguiFrameOpen)
    {
        s_imguiFrameOpen = false;
        ImGui::Render();

        ImGui_Backend_RenderDrawData(ImGui::GetDrawData());
    }

    ::Render->End();
}

void CRenderDevice::End_Main_Post()
{
    vCameraPositionSaved = vCameraPosition;
    vCameraDirectionSaved = vCameraDirection;
    vCameraTopSaved = vCameraTop;
    vCameraRightSaved = vCameraRight;

    mFullTransformSaved = mFullTransform;
    mViewSaved = mView;
    mProjectSaved = mProject;
}

void CRenderDevice::PreCache(u32 amount, bool b_draw_loadscreen, bool b_wait_user_input)
{
    RT_WaitIdle();
    // The precache countdown IS 'st_client_synchronising': every one of these frames
    // draws the whole world behind an opaque load screen, and 60 of them is what the
    // phase costs. Measured on pripyat_full (18-08): the pipeline prewarm they exist
    // to spread finishes inside the first 30 -- a 30-frame and a 60-frame precache
    // create the SAME 34 pipelines, the second half creating none. Capping it to 30
    // cost 690 ms instead of 990. Left as an env knob rather than a new default
    // because the frames also carry the game-side scheduler in prefetch mode, and
    // what halving THAT costs the first seconds of play is not measured yet.
    static const u32 s_cap = []() -> u32 {
        const char* e = std::getenv("XROS_PRECACHE");
        const int   v = e ? atoi(e) : 0;
        return v > 0 ? (u32)v : 0u;
    }();
    if (s_cap && amount > s_cap)
    {
        Msg("[precache] XROS_PRECACHE caps %u frames -> %u", amount, s_cap);
        amount = s_cap;
    }

    dwPrecacheFrame = dwPrecacheTotal = amount;
    s_precacheT0 = xrPlatform::TickCount();
    s_precacheRan = 0;

    if (amount && b_draw_loadscreen && load_screen_renderer.b_registered == false)
    {
        load_screen_renderer.start(b_wait_user_input);
    }
}

ENGINE_API xr_list<fastdelegate::FastDelegate<bool()>> g_loading_events;

// Network-client frame fix: set by a network client (CLevel::net_start_client4) to
// force the per-frame "second thread" work (physics + the network-spawn burst
// queued to seqParallel) to be joined BEFORE the main-thread render, instead of
// running concurrently with it -- otherwise applying a flood of network spawns
// on the second thread while the renderer reads the scene races and hangs the
// client at the sync->gameplay transition. False for a fused single-player host,
// which keeps the concurrent split.
ENGINE_API bool g_net_client_st = false;

// ⭐⭐⭐Run the physics world inside the main frame instead of on the second
// thread. Set by a headless/dedicated host before the level loads.
//
// ⚠Why a server wants this. The stock split runs physics (seqFrameMT) next to
// the RENDER, which touches nothing physics owns. A host with no renderer has
// game logic where the render used to be, and the engine is full of "can not
// deactivate physics X during physics processing" fatals for exactly that
// overlap -- every one of them reachable the moment NPCs start dying. Measured
// 27-07: three different ones in three consecutive runs, and CPHWorld::FrameStep
// caught being ENTERED while another step was still inside it, which ODE does
// not survive quietly either.
//
// The cost is nothing worth counting: physics on this server measures 0.0 ms of
// a 4.9 ms frame at 200 fps with 500 objects online.
ENGINE_API bool g_ph_single_thread = false;

// ⭐⭐⭐FREEZE-HUNT WATCHDOG. Silent hangs stop the main loop with NOTHING in the
// log: no fatal, no stack, no teardown -- the log just ends mid-line and the
// process has to be killed, which writes no crash report either. A detached side
// thread survives the frozen main thread and is the only thing that can say which
// phase never returned.
//
// ⚠It used to start ONLY under g_net_client_st (thin network client). Measured
// 19-08: a fused local host froze after ~7 minutes and the watchdog had not run a
// single tick -- the instrument existed and was pointed at the wrong build. It is
// unconditional now, and deliberately has NO cvar: a gate you have to remember to
// switch on is the same failure again.
static std::atomic<const char*> g_nc_phase{"boot"};
static std::atomic<u32> g_nc_phase_frame{0};
static std::atomic<u32> g_nc_phase_seq{0}; // bumped by every NC_PHASE = "the loop moved"

namespace
{
constexpr u32 kWatchdogPollMs = 1000; // sample rate
// ⚠CALIBRATION, measured 19-08 -- do not lower this back to "a few seconds".
// At 3 s it fired TWICE on every ordinary level load, and both were legitimate:
// ~4 s in load_event (one load step, 4867 log lines of model_Create/bumps in it)
// and ~3 s in frame_move (the first-gameplay-frame spawn burst -- actor plus the
// whole inventory's model_Create). Two false alarms per load erode the signal
// exactly the way the old 5 s heartbeat did. Sub-second hitches already belong to
// the [stall] instrument; this one only owns FREEZES, and a freeze lasts until the
// process is killed -- noticing it at 10 s instead of 3 s costs nothing.
constexpr u32 kWatchdogStuckS = 10;
constexpr u32 kWatchdogRepeatS = 5; // re-report cadence while the freeze lasts

// ⚠⚠WHY THIS DOES NOT GO THROUGH Msg(). AddOne() takes a recursive_mutex
// (xrCore/log.cpp:53). A main thread hung WHILE HOLDING it would block the
// watchdog's own Msg() forever -- the one hang class that silences the instrument
// is inside the instrument's own output path. So the stall report goes to a
// private file with a private handle, flushed per line, sharing no lock with
// anything. Do not "simplify" this back into Msg().
// ⚠The stream is LEAKED on purpose: a function-local static would be destroyed by
// the atexit chain while this detached thread may still be inside operator<<, and
// this build already lost an exe to exactly that race (TexPrefetch's atexit
// destructor, 19-08). Nothing here needs to be reclaimed -- the OS closes it.
void watchdog_report(const char* text)
{
    static std::ofstream* out = [] {
        string_path path;
        if (logFName[0])
            xr_strconcat(path, logFName, ".watchdog.txt");
        else
            strcpy_s(path, "watchdog.txt");
        return new std::ofstream(path, std::ios::app);
    }();

    if (out->is_open())
    {
        *out << text << std::endl; // endl: flush every line, the process may not survive
    }
    OutputDebugString(text); // free, no lock, and visible under a debugger
    OutputDebugString("\n");
}
} // namespace

static void nc_watchdog_start()
{
    static bool s_started = false;
    if (s_started)
        return;
    s_started = true;
    try
    {
    std::thread([] {
        // ⭐Имя потока — не украшение: прибор cpu_budget считает ядро-миллисекунды
        // по потокам, и безымянный поток в отчёте неотличим от драйверного.
        set_current_thread_name("XR Watchdog");
        u32 lastSeq = u32(-1);
        u32 stuckS = 0;
        // ⚠Latched AT DETECTION, not read at report time. Reading the live phase on
        // recovery names where the loop is NOW -- which is by definition somewhere
        // else, since it just moved. Measured 19-08: the file said it stood in
        // load_event, the recovery line in the log said frame_move. The stuck phase
        // is the whole point of the line, so it is captured once and kept.
        const char* stuckPhase = "?";
        u32 stuckFrame = 0;

        for (;;)
        {
            Sleep(kWatchdogPollMs);

            const u32 seq = g_nc_phase_seq.load(std::memory_order_relaxed);
            if (seq != lastSeq)
            {
                // The loop moved. Report a stall only once it is OVER, and only
                // into the main log: the main thread is demonstrably alive again,
                // so Msg() cannot block here.
                if (stuckS >= kWatchdogStuckS)
                    Msg("!![watchdog] main loop was STALLED ~%u s in phase=%s frame=%u, recovered", stuckS, stuckPhase, stuckFrame);
                stuckS = 0;
                lastSeq = seq;
                continue;
            }

            stuckS += kWatchdogPollMs / 1000;
            if (stuckS < kWatchdogStuckS)
                continue;
            if (stuckS == kWatchdogStuckS)
            {
                stuckPhase = g_nc_phase.load(std::memory_order_relaxed);
                stuckFrame = g_nc_phase_frame.load(std::memory_order_relaxed);
            }
            // First tick over the threshold, then once every kWatchdogRepeatS --
            // a freeze that lasts until the process is killed still leaves a
            // trail with timestamps to line a memory dump up against.
            if (stuckS != kWatchdogStuckS && (stuckS - kWatchdogStuckS) % kWatchdogRepeatS != 0)
                continue;

            string512 line;
            xr_sprintf(line, "[watchdog] MAIN LOOP STUCK ~%u s in phase=%s frame=%u", stuckS, stuckPhase, stuckFrame);
            watchdog_report(line);
        }
    }).detach();
    }
    catch (...)
    {
        Msg("![watchdog] cannot start the thread -- running without it");
    }
}
// ---------------------------------------------------------------------------
// ⭐⭐WHERE A 700 ms FRAME ACTUALLY WENT.
//
// Measured 30-07: the headless server loses ~0.7 s of wall clock several times
// a minute. The drive integrates every millisecond of it in one step, so the
// next packet carries a 4.2 m stride with an honest 47 ms stamp and the client
// draws 90 m/s -- see wire-timestamp-stalls-and-jumps.
//
// ⚠The engine's own counters cannot find it. sv_perf reports "engine total
// 0.9 ms" and "worst engine frame 4.0 ms" across a window that demonstrably
// contained a 700 ms gap, because CStats only runs inside the phases it wraps
// and five of every six milliseconds are not inside any of them.
//
// So this stamps the shared QPC clock at each phase boundary and prints the
// breakdown for any frame over the threshold -- including the bucket BETWEEN
// frames, which is the one an in-frame profiler structurally cannot have and
// the one a descheduled process spends its time in.
namespace
{
constexpr u32 k_stall_ms = 150; // a frame this long is pathological on any box
constexpr int k_stall_slots = 24;   // v105: +extract/submit/ui_post marks

struct StallProbe
{
    const char* name[k_stall_slots];
    u64 us[k_stall_slots];
    int n{0};
    u64 prev_end_us{0};
};
StallProbe s_stall;

void stall_mark(const char* name);
} // namespace
// v118: lets the renderer split render_begin into fence / acquire on the same timeline. Main thread only.
extern "C" void xr_nx_stall_mark(const char* name);
extern "C" void xr_nx_stall_mark(const char* name)
{
    stall_mark(name);
}
namespace
{
void stall_mark(const char* name)
{
    if (s_stall.n < k_stall_slots)
    {
        s_stall.name[s_stall.n] = name;
        s_stall.us[s_stall.n] = CPU::QPC_us();
        ++s_stall.n;
    }
}

// ⚠Reported from the TOP of the next loop iteration, not the bottom of this
// one. The first cut of this probe measured only the inside of on_idle and
// found 6-10 ms in frames that had lost a full second -- the whole of the loss
// was in the tail nobody was timing (the Sleep, the message pump, and whatever
// the OS does with the process in between). A profiler that stops at the
// boundary of the thing it profiles cannot see the thing that is not running.
#ifdef __SWITCH__
} // namespace
// v118: render-thread / present timings (us), summed per [nx frame] window. 0 = Render_Record job, 1 = End_Render job,
// 2 = Swapchain.Present (whatever thread presents). stall_mark itself is main-thread only.
extern "C" void xr_nx_rt_add(int slot, unsigned long long us);
extern "C" volatile unsigned g_xr_nx_gpu_mhz, g_xr_nx_emc_mhz, g_xr_nx_clk_resets;   // nx/xr_nx_pad.c clock watch (v122)
static std::atomic<unsigned long long> g_nxRtUs[3];
extern "C" void xr_nx_rt_add(int slot, unsigned long long us)
{
    if (slot >= 0 && slot < 3) g_nxRtUs[slot].fetch_add(us, std::memory_order_relaxed);
}
// nfs28: nx_crop.txt -- the whole run with the display crop (CRender_Vulkan.cpp: composite + UI at render res,
// nwindowSetCrop stretches it).
// nfs29: ON by default (nx_nocrop.txt turns it off for a run).
extern "C" int g_xr_nx_crop;
int g_xr_nx_crop = 1;
// 06-10: 1 while a game screen is up (inventory, PDA, talk, any dialog; xrGame CHUDManager::RenderUI sets it every
// level frame). Such a frame goes out without the crop: the UI draws at the display resolution instead of being
// stretched from the render one -- at 720p the 24dp text was ~9 px tall and fell apart, 1 px borders vanished. The
// price is the composite at display res, ~0.5 ms (nfs28), only while the screen is open.
extern "C" int g_xr_nx_ui_native;
int g_xr_nx_ui_native = 0;
extern "C" double xr_nx_gpu2_window(void);   // Layers/xrRenderVulkan/CRender_Vulkan.cpp
extern "C" void xr_nx_xfer_take(uint32_t cnt[12], int64_t off[16], uint32_t rcnt[16], uint32_t rkind[16]);   // nfs35 xr_nx_throw.cpp
extern "C" volatile unsigned long long g_xr_nx_core_idle[3], g_xr_nx_core_stamp[3];   // nx/xr_nx_pad.c probes
extern "C" u32 svcGetInfo(u64* out, u32 id0, u32 handle, u64 id1);                    // libnx (no switch.h here)
static inline u64 nx_sys_tick() { u64 t; asm volatile("mrs %0, cntpct_el0" : "=r"(t)); return t; }
namespace
{
// [nx 2nd] (11-10): the second thread's frame task averaged over the [nx frame] window -- [2nd] is printed only with a
// hitch, so a steady 2.6 ms wait_2nd_tail in a fight had no breakdown. Written by the task (once per frame, before its
// future completes), read by the main thread in nx_frame_window after it waited for that future.
struct Nx2ndAcc
{
    double sound = 0, par = 0, gc = 0, mt = 0, net = 0, total = 0;
    u32 n = 0;
    struct Task { const char* name; double ms; u32 n; };
    Task task[24]{};
    int ntask = 0;
    void add_task(const char* name, double ms)
    {
        int k = 0;
        while (k < ntask && task[k].name != name)
            ++k;
        if (k == ntask)
        {
            if (ntask >= 24)
                return;
            task[ntask++] = {name, 0.0, 0};
        }
        task[k].ms += ms;
        ++task[k].n;
    }
};
Nx2ndAcc s_2nd_acc;

// [nx frame] (Switch v104): the CPU-side frame measure that does not need r_profiler -- the phase stamps above are QPC
// reads the loop takes anyway. Every 5 s of real time: frame avg/p50/p95/max, fps, and each phase's average share.
void nx_frame_window(u64 now, u64 total_us)
{
    struct Acc { const char* name; double ms; };
    static Acc    s_ph[k_stall_slots * 2];
    static int    s_nph = 0;
    static float  s_ft[2048];
    static u32    s_n = 0;
    static double s_sum = 0.0;
    static float  s_max = 0.f;
    static u64    s_t0 = 0;
    if (!s_t0) s_t0 = now;
    for (int i = 0; i < s_stall.n; ++i)
    {
        const u64 end = (i + 1 < s_stall.n) ? s_stall.us[i + 1] : now;
        const double ms = double(end - s_stall.us[i]) / 1000.0;
        int k = 0;
        while (k < s_nph && strcmp(s_ph[k].name, s_stall.name[i]) != 0)
            ++k;
        if (k == s_nph)
        {
            if (s_nph >= k_stall_slots * 2) continue;
            s_ph[s_nph].name = s_stall.name[i]; s_ph[s_nph].ms = 0.0; ++s_nph;
        }
        s_ph[k].ms += ms;
    }
    const float fms = float(total_us) / 1000.f;
    if (s_n < 2048) s_ft[s_n] = fms;
    ++s_n; s_sum += fms; if (fms > s_max) s_max = fms;
    if (now - s_t0 < 5000000ull)
        return;
    const u32 m = (s_n < 2048) ? s_n : 2048;
    std::sort(s_ft, s_ft + m);
    string1024 ph; ph[0] = 0;
    for (int k = 0; k < s_nph; ++k)
    {
        const double avg = s_ph[k].ms / double(s_n);
        if (avg < 0.05) continue;
        string64 one; xr_sprintf(one, "%s %.2f | ", s_ph[k].name, avg); xr_strcat(ph, one);
    }
    const double secs = double(now - s_t0) / 1e6;
    // busy% per core = 1 - idle ticks / elapsed ticks between two probe reads; main thread = its own ticks / elapsed.
    static unsigned long long s_idle0[3], s_st0[3];
    static u64 s_mainT0 = 0, s_mainS0 = 0;
    int busy[3] = {-1, -1, -1};
    for (int c = 0; c < 3; ++c)
    {
        const unsigned long long idle = g_xr_nx_core_idle[c], st = g_xr_nx_core_stamp[c];
        if (s_st0[c] && st > s_st0[c])
            busy[c] = int(100.0 - 100.0 * double(idle - s_idle0[c]) / double(st - s_st0[c]) + 0.5);
        s_idle0[c] = idle; s_st0[c] = st;
    }
    int mainBusy = -1;
    {
        u64 t = 0;
        const u64 st = nx_sys_tick();
        if (svcGetInfo(&t, 25 /*InfoType_ThreadTickCount*/, 0xFFFF8000u /*CUR_THREAD*/, ~0ull) == 0)
        {
            if (s_mainS0) mainBusy = int(100.0 * double(t - s_mainT0) / double(st - s_mainS0) + 0.5);
            s_mainT0 = t; s_mainS0 = st;
        }
    }
    const double rtRec = double(g_nxRtUs[0].exchange(0)) / 1000.0 / double(s_n);
    const double rtEnd = double(g_nxRtUs[1].exchange(0)) / 1000.0 / double(s_n);
    const double prs   = double(g_nxRtUs[2].exchange(0)) / 1000.0 / double(s_n);
    Msg("[nx frame] rt=%d n=%u fps=%.1f | frame avg=%.2f p50=%.2f p95=%.2f max=%.1f | busy%% core0 %d core1 %d core2 %d main %d | rt_rec %.2f rt_end %.2f present %.2f | gpu %u emc %u MHz resets %u | gpu2 %.2f | %s",
        g_rtOverlap ? 1 : 0, s_n, double(s_n) / secs, s_sum / double(s_n), s_ft[m / 2], s_ft[(m * 95) / 100], s_max,
        busy[0], busy[1], busy[2], mainBusy, rtRec, rtEnd, prs, g_xr_nx_gpu_mhz, g_xr_nx_emc_mhz, g_xr_nx_clk_resets,
        xr_nx_gpu2_window(), ph);
    if (s_2nd_acc.n)
    {
        Nx2ndAcc& a = s_2nd_acc;
        const double f = 1.0 / double(a.n);
        std::sort(a.task, a.task + a.ntask, [](const Nx2ndAcc::Task& x, const Nx2ndAcc::Task& y) { return x.ms > y.ms; });
        string1024 top; top[0] = 0;
        for (int t = 0; t < a.ntask && t < 6; ++t)
        {
            string128 one; xr_sprintf(one, "%s %.2f (x%.1f) | ", a.task[t].name ? a.task[t].name : "?", a.task[t].ms * f, double(a.task[t].n) * f); xr_strcat(top, one);
        }
        Msg("[nx 2nd] per frame: total %.2f = sound %.2f + seqParallel %.2f + script_gc %.2f + seqFrameMT %.2f | seqParallel by task: %s| seqFrameMT split: net %.2f physics %.2f",
            a.total * f, a.sound * f, a.par * f, a.gc * f, a.mt * f, top, a.net * f, (a.mt - a.net) * f);
        a = Nx2ndAcc{};
    }
    {
        // nfs35: per-frame transfer / barrier counts of this window + the top transfer call sites (xr_nx_throw.cpp).
        uint32_t c[12], rc[16], rk[16]; int64_t off[16];
        xr_nx_xfer_take(c, off, rc, rk);
        const double f = s_n ? 1.0 / double(s_n) : 0.0;
        Msg("[nx xfer] per frame: copyBuf %.1f fill %.1f update %.1f img2buf %.1f buf2img %.1f copyImg %.1f blit %.1f"
            " clear %.1f qreset %.1f | barrier %.1f barrier2 %.1f | dispatch %.1f",
            c[0] * f, c[1] * f, c[2] * f, c[3] * f, c[4] * f, c[5] * f, c[6] * f,
            c[7] * f, c[8] * f, c[9] * f, c[10] * f, c[11] * f);
        static const char* const kKind[] = {"copyBuf", "fill", "update", "img2buf", "buf2img", "copyImg", "blit", "clear", "qreset"};
        string1024 top; top[0] = 0;
        for (int t = 0; t < 16 && rc[t]; ++t) {
            string64 one; xr_sprintf(one, "%s@%+lld x%.1f | ", kKind[rk[t] < 9 ? rk[t] : 0], (long long)off[t], rc[t] * f);
            xr_strcat(top, one);
        }
        Msg("[nx xfer] top sites (offset to xr_nx_xfer_take): %s", top);
    }
    s_n = 0; s_sum = 0.0; s_max = 0.f; s_nph = 0; s_t0 = now;
}
#endif

// What the SECOND thread (Frame_Logic's TTAPI task) did in the last frame, in ms. The
// main thread's "wait_2nd_tail" only says THAT it waited for it -- on the Switch a
// nearby anomaly producing an artifact costs ~160 ms there, every time. Written by the
// task before its future completes, read by the main thread after it waited on that
// future (WaitSecondThread), so the future orders them. Printed with every [hitch] and
// [stall] line as [2nd].
struct SecondThreadFrame
{
    float sound = 0.f, seqParallel = 0.f, seqParallelMax = 0.f, scriptGc = 0.f, seqFrameMT = 0.f, seqFrameMTWorstMs = 0.f;
    u32   seqParallelN = 0, seqParallelMaxIdx = 0;
    const char* seqParallelMaxName = "-";
    const char* seqFrameMTWorst = "-";
};
static SecondThreadFrame s_2nd;

static void report_2nd()
{
    Msg("~ [2nd] last frame's second thread: sound %.1f | seqParallel %.1f (%u tasks, longest #%u %s %.1f) | script_gc %.1f | "
        "seqFrameMT %.1f (slowest %s %.1f) ms",
        s_2nd.sound, s_2nd.seqParallel, s_2nd.seqParallelN, s_2nd.seqParallelMaxIdx, s_2nd.seqParallelMaxName, s_2nd.seqParallelMax,
        s_2nd.scriptGc, s_2nd.seqFrameMT, s_2nd.seqFrameMTWorst, s_2nd.seqFrameMTWorstMs);
}

void stall_report()
{
    if (s_stall.n < 2)
        return;

    const u64 now = CPU::QPC_us();
    const u64 total_us = now - s_stall.us[0];
    // Precache frames legitimately take hundreds of milliseconds and would
    // drown the real finding in the noise of loading.
    if (Device.dwPrecacheFrame != 0)
        return;
#ifdef __SWITCH__
    nx_frame_window(now, total_us);
#endif

    // ⭐⭐⭐THE DISTRIBUTION, not only the catastrophes.
    //
    // USER-REPORTED "статтеры иногда проскакивают" -- and this probe, which
    // has been compiled into the client all along, printed NOTHING, because
    // 150 ms is a threshold for a pathology and what a player calls a stutter
    // is a 30-60 ms frame among 16 ms ones. A single threshold can only answer
    // "was there a disaster"; "иногда" is a question about a TAIL, and only a
    // distribution answers it.
    //
    // ⚠Every frame is counted -- no sampling. A 1-in-N sampler was measured
    // today to under-report a rate by exactly N, and a percentile taken from
    // sampled data would be worse than no number at all.
    {
        static const u32 k_edge_ms[] = {8, 12, 17, 25, 33, 50, 100, 150};
        constexpr int k_bins = 9;
        static u32 s_bin[k_bins] = {};
        static u32 s_n = 0, s_next = 0, s_worst_ms = 0, s_worst_frame = 0;

        const u32 ms = u32(total_us / 1000);
        int b = 0;
        while (b < 8 && ms >= k_edge_ms[b])
            ++b;
        ++s_bin[b];
        ++s_n;
        if (ms > s_worst_ms)
        {
            s_worst_ms = ms;
            s_worst_frame = Device.dwFrame;
        }

        if (s_n >= s_next)
        {
            s_next = s_n + 2000;
            // Percentiles read off the histogram from below, reported as the
            // UPPER EDGE of the bucket they land in -- the honest reading of
            // bucketed data, and stated as "<" so it cannot be quoted as exact.
            u32 acc = 0, p50 = 0, p95 = 0, p99 = 0;
            for (int i = 0; i < k_bins; ++i)
            {
                acc += s_bin[i];
                const u32 hi = (i < 8) ? k_edge_ms[i] : 9999;
                if (!p50 && acc * 2 >= s_n)
                    p50 = hi;
                if (!p95 && acc * 100 >= s_n * 95)
                    p95 = hi;
                if (!p99 && acc * 100 >= s_n * 99)
                    p99 = hi;
            }
            Msg("* [frametime] %u frames: <8=%u <12=%u <17=%u <25=%u <33=%u <50=%u <100=%u <150=%u 150+=%u | p50<%u p95<%u p99<%u ms | worst %u ms at frame %u", s_n,
                s_bin[0], s_bin[1], s_bin[2], s_bin[3], s_bin[4], s_bin[5], s_bin[6], s_bin[7], s_bin[8], p50, p95, p99, s_worst_ms, s_worst_frame);
        }

        // ⭐And the breakdown of the frames a PLAYER would notice, not only the
        // ones a profiler would. A doubled 60 Hz frame is the smallest thing
        // that reads as a hitch on screen. Bounded, but the running total is
        // printed with every line so an exhausted budget and a quiet game
        // cannot look the same.
        const u32 k_hitch_ms = u32(g_hitch_ms > 0 ? g_hitch_ms : 33); // dbg_hitch_ms (08-10: the goal is no frame over 20)
        static u32 s_hitches = 0, s_printed = 0;
        static u64 s_print_window = 0;
        // the budget refills every 30 s: 07-10 the first 60 went on the load and the walk to the
        // place, and the firefight that was being measured printed none
        if (now - s_print_window > 30000000ull)
        {
            s_print_window = now;
            s_printed = 0;
        }
        if (ms >= k_hitch_ms && ms < k_stall_ms)
        {
            ++s_hitches;
            if (s_printed < (k_hitch_ms < 33 ? 240u : 60u)) // a lower threshold catches many more
            {
                ++s_printed;
                string1024 hb;
                hb[0] = 0;
                for (int i = 0; i < s_stall.n; ++i)
                {
                    const u64 end = (i + 1 < s_stall.n) ? s_stall.us[i + 1] : now;
                    const float seg = float(end - s_stall.us[i]) / 1000.f;
                    if (seg < 1.f)
                        continue;
                    string64 one;
                    xr_sprintf(one, "%s %.0f | ", s_stall.name[i], seg);
                    xr_strcat(hb, one);
                }
                Msg("~ [hitch] frame %u took %u ms (#%u so far) : %s", Device.dwFrame, ms, s_hitches, hb);
                report_2nd();
            }
        }
    }

    if (total_us / 1000 < k_stall_ms)
        return;

    string1024 buf;
    buf[0] = 0;
    for (int i = 0; i < s_stall.n; ++i)
    {
        const u64 end = (i + 1 < s_stall.n) ? s_stall.us[i + 1] : now;
        const float ms = float(end - s_stall.us[i]) / 1000.f;
        if (ms < 1.f) // only the buckets carrying something
            continue;
        string64 one;
        xr_sprintf(one, "%s %.0f | ", s_stall.name[i], ms);
        xr_strcat(buf, one);
    }
    Msg("! [stall] loop iteration at frame %u took %.0f ms : %s", Device.dwFrame, float(total_us) / 1000.f, buf);
    report_2nd();
}
} // namespace

// Lets the process DRIVING the engine put its own marks on the same timeline.
// The headless server's loop lives outside this module (server_main.cpp) and
// the first split put 99% of a 800 ms stall in the single bucket between
// on_idle returning and being called again -- which is that loop, and which
// this module cannot break down on its own.
void Device_StallMark(const char* name) { stall_mark(name); }

// The seq bump is what the watchdog reads as liveness: any phase transition means
// the loop moved. Deliberately NOT dwFrame -- that only advances inside FrameMove,
// so a hang in render_process would look identical to a hang in frame_move.
#define NC_PHASE(name) \
    do { \
        g_nc_phase.store(name, std::memory_order_relaxed); \
        g_nc_phase_frame.store(Device.dwFrame, std::memory_order_relaxed); \
        g_nc_phase_seq.fetch_add(1, std::memory_order_relaxed); \
        stall_mark(name); \
    } while (0)

void CRenderDevice::OnCameraUpdated(bool from_actor)
{
    static u32 frame{u32(-1)};
    if (frame == dwFrame)
        return;

    frame = dwFrame;

    // Precache: spin the camera a full turn over the countdown, so every direction
    // around the spawn is drawn once before the player sees anything. That sweep is
    // also what keeps the shadow/streaming machinery churning for the whole phase --
    // XROS_PRECACHE_SWEEP=0 holds the spawn view instead, which is the A/B for
    // "how much of the 45 ms per precache frame does the sweep itself buy?".
    if (dwPrecacheFrame)
    {
        static const bool s_sweep = []() {
            const char* e = std::getenv("XROS_PRECACHE_SWEEP");
            return !(e && atoi(e) == 0);
        }();
        if (s_sweep)
        {
            float factor = float(dwPrecacheFrame) / float(dwPrecacheTotal);
            float angle = PI_MUL_2 * factor;

            vCameraDirection.set(_sin(angle), 0, _cos(angle));
            vCameraDirection.normalize();

            vCameraTop.set(0, 1, 0);
            vCameraRight.crossproduct(vCameraTop, vCameraDirection);

            mView.build_camera_dir(vCameraPosition, vCameraDirection, vCameraTop);
        }
    }

    // Matrices
    mInvView.invert(mView);
    mFullTransform.mul(mProject, mView);
    mInvFullTransform.invert_44(mFullTransform);

    ::Render->OnCameraUpdated(from_actor);

    ::Render->SetCacheXform(mView, mProject);

    mView_hud_old = mView_hud;
    mView_hud_old2 = mView_hud2;
    mProject_hud_old = mProject_hud;
    mProject_hud_old2 = mProject_hud2;
    mFullTransform_hud_old = mFullTransform_hud;
    mFullTransform_hud_old2 = mFullTransform_hud2;

    mView_old = mViewSaved;
    mProject_old = mProjectSaved;
    mFullTransform_old = mFullTransformSaved;

    ::Render->SetCacheXformOld(mView_old, mProject_old);

    // CurrentEnv is null until the first environment update: one run crashed here at startup (validation slowed the load).
    const float hudFarPlane = (g_pGamePersistent && g_pGamePersistent->Environment().CurrentEnv) ? g_pGamePersistent->Environment().CurrentEnv->far_plane_render : 400.f;
    mView_hud.build_camera_dir(Fvector{}, vCameraDirection, vCameraTop);
    mProject_hud.build_projection(deg2rad(psHUD_FOV <= 1.f ? psHUD_FOV * Device.fFOV : psHUD_FOV), fASPECT, HUD_VIEWPORT_NEAR,
                                  hudFarPlane);
    mFullTransform_hud.mul(mProject_hud, mView_hud);

    mView_hud2.set(mView);
    mProject_hud2.build_projection(deg2rad(psHUD_FOV <= 1.f ? psHUD_FOV * Device.fFOV : psHUD_FOV), fASPECT, HUD_VIEWPORT_NEAR,
                                   hudFarPlane);
    mFullTransform_hud2.mul(mProject_hud2, mView_hud2);
}

// snd_core (10-10-2026, Switch): which core runs xrSound's update (occlusion rays, snd_prop, auto-reverb, the ogg
// streams' decode). -1 = in the second-thread task itself, as before (a TTAPI worker, unpinned -- on the Switch that is
// core 0, next to the main thread); 0..2 = a PinnedWorker on that core, the second-thread task waiting for it. The
// order stays the same -- sound before seqParallel, nothing plays a sound meanwhile (CSoundRender_Core::play takes no
// lock) -- only the core changes. Core 2 is where the OpenAL mixer thread is pinned too (nx/xr_nx_sdl_audio.c).
#ifdef __SWITCH__
ENGINE_API int g_snd_core = -1; // 11-10: inline is faster in a fight (rspec_vk_switch.ltx has the numbers)
extern "C" void xr_nx_audio_stats(float* avg_ms, float* max_ms, float* period_ms, unsigned* calls, unsigned* starved, unsigned* core_rc);
#else
ENGINE_API int g_snd_core = -1;
#endif

struct _SoundProcessor : public pureFrame
{
    void OnFrame() override
    {
        ZoneScopedN("_SoundProcessor");

        Device.Statistic->Sound.Begin();
#ifdef __SWITCH__
        const u64 t0 = CPU::QPC();
#endif
        ::Sound->update(Device.vCameraPosition, Device.vCameraDirection, Device.vCameraTop /*, Device.Paused()*/);
        Device.Statistic->Sound.End();
#ifdef __SWITCH__
        // [nx audio] once per 5 s: the price of sound on the console. upd = this update (core snd_core),
        // mix = one OpenAL callback (core 2) against the period it has to fit in, starved = audible gaps.
        const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
        m_updMs += ms;
        m_updMax = std::max(m_updMax, ms);
        ++m_updN;
        if (Device.dwTimeContinual - m_lastPrint >= 5000)
        {
            float avg, mx, period;
            unsigned calls, starved, core_rc;
            xr_nx_audio_stats(&avg, &mx, &period, &calls, &starved, &core_rc);
            if (calls || m_updMs > 0.5)
                Msg("~ [nx audio] upd %.2f ms avg %.2f max (core %d) | mix %.2f ms avg %.2f max of %.1f, %u calls, starved %u (pin rc 0x%x)",
                    m_updN ? m_updMs / m_updN : 0.0, m_updMax, g_snd_core, avg, mx, period, calls, starved, core_rc);
            m_lastPrint = Device.dwTimeContinual;
            m_updMs = m_updMax = 0.0;
            m_updN = 0;
        }
#endif
    }
#ifdef __SWITCH__
    double m_updMs = 0.0, m_updMax = 0.0;
    u32 m_updN = 0, m_lastPrint = 0;
#endif
} g_sound_update;

static void SoundUpdateOnCore()
{
    if (g_snd_core < 0 || g_snd_core > 2)
    {
        g_sound_update.OnFrame();
        return;
    }
    static PinnedWorker* s_workers[3] = {};
    PinnedWorker*& w = s_workers[g_snd_core];
    if (!w)
        w = new PinnedWorker(g_snd_core); // leaked on purpose, see pinned_worker.h
    w->Run([] { g_sound_update.OnFrame(); });
    w->Wait();
}

#pragma todo("SIMP: для этого кода нужны переделки в xrSound")
/*
struct _SoundRender : public pureFrame
{
    virtual void OnFrame()
    {
        ZoneScopedN("_SoundRender");

        // Msg							("------------- sound: %d [%3.2f,%3.2f,%3.2f]",u32(Device.dwFrame),VPUSH(Device.vCameraPosition));
        Device.Statistic->Sound.Begin();
        ::Sound->render();
        Device.Statistic->Sound.End();
    }
} g_sound_render;
*/

// ---- RENDER_THREAD_PLAN step 1: the frame cut along the main/render seam. Same calls, same
// order as the monolithic on_idle had; the render thread (step 12) takes over Extract's
// record half and Submit.
bool CRenderDevice::Frame_Logic(std::future<void>& awaiter, std::chrono::high_resolution_clock::time_point& secondThreadStart)
{
    // if (b_is_Active)
    {
        NC_PHASE("frame_move");
        CpuBudget::PhaseScope _ph(CpuBudget::PH_FRAMEMOVE);
        if (g_frame_prof_ms > 0)
        {
            // [frame prof] (dbg_frame_prof <ms>, 07-10): the main thread's frame_move under one
            // LoadProf::Arm, so every part marked anywhere below it (stalker "sched", "lua:<file>:<line>",
            // planners, spawns, "level.events", "stalker.die", "ph.ragdoll"...) adds up in one line for a
            // frame this long. The [stall]/[hitch] lines only say "frame_move 44"; a kill on the Switch was
            // exactly that, twice per kill. ⚠The Arm swallows the inner ones: no [sched prof] lines meanwhile.
            LoadProf::Arm arm;
            const u64 t0 = CPU::QPC();
            {
                LoadProf::Scope root("frame_move");
                FrameMove();
            }
            const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
            static u32 s_printed = 0, s_last = 0;
            if (ms >= g_frame_prof_ms && arm.outer && s_printed < 500) // every one: a death is two slow frames back to back
            {
                s_last = dwTimeContinual;
                ++s_printed;
                string2048 parts;
                arm.format(parts, sizeof(parts), 0.5);
                Msg("~ [frame prof] frame %u frame_move %.1f ms: %s", dwFrame, ms, parts);
            }
        }
        else
            FrameMove();
    }

    if (!g_loading_events.empty()) // that is to avoid loading screen flickering
    {
        pApp->LoadDraw();

        return false;
    }

    OnCameraUpdated(false);

    secondThreadStart = std::chrono::high_resolution_clock::now();

    NC_PHASE("2nd_submit");
    CpuBudget::PhaseEnter(CpuBudget::PH_SUBMIT2ND);
    // allow secondary thread to do its job
    awaiter = TTAPI->submit([this] {
        ZoneScoped;

        // ⚠Everything this task touches -- seqParallel (ALife's switch
        // predicates run script per object), the collector, seqFrameMT's net
        // processor -- is a user of the one LuaJIT state, and the main thread
        // is free to render for the whole of it. Marking the window lets the
        // watch report anyone who reaches the VM ALONGSIDE this task, instead
        // of only the rarer case of two threads inside at the same instant.
        lua_vm_watch::worker_window vm_window;

        const auto SecondThreadTasksStartTime = std::chrono::high_resolution_clock::now();
        using msd = std::chrono::duration<float, std::milli>;
        auto t2nd = SecondThreadTasksStartTime;
        auto lap2nd = [&t2nd]() {   // ms since the previous lap
            const auto now = std::chrono::high_resolution_clock::now();
            const float r = msd(now - t2nd).count();
            t2nd = now;
            return r;
        };
        SecondThreadFrame st{};

        {
            CpuBudget::PhaseScope _ph(CpuBudget::PH_SOUND);
            SoundUpdateOnCore();
        }
        st.sound = lap2nd();

        // add_to_awaiters(TTAPI->submit([]() { g_sound_render.OnFrame(); }));

        {
            ZoneScopedN("seqParallel");
            CpuBudget::PhaseScope _ph(CpuBudget::PH_SEQPARALLEL);

            auto size = seqParallel.size();
            while (size > 0)
            {
                const auto ti = std::chrono::high_resolution_clock::now();
                const char* name = seqParallelNames.empty() ? "?" : seqParallelNames.front();
                if (g_frame_prof_ms > 0)
                {
                    // [2nd prof] (with dbg_frame_prof): one task of the second thread, part by
                    // part. 📏07-10-2026 Switch v215: the worst fight frame (92 ms) was a task
                    // the [2nd] line calls "?" -- 76 ms of it, the main thread waiting.
                    LoadProf::Arm arm;
                    const u64 t0 = CPU::QPC();
                    {
                        LoadProf::Scope root("task");
                        seqParallel.front()();
                    }
                    const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
                    static u32 s_printed = 0;
                    if (ms >= 15.0 && arm.outer && s_printed < 300)
                    {
                        ++s_printed;
                        string2048 parts;
                        arm.format(parts, sizeof(parts), 0.5);
                        Msg("~ [2nd prof] frame %u task #%u '%s' (names %u / tasks %u) %.1f ms: %s", dwFrame, st.seqParallelN, name, (u32)seqParallelNames.size(),
                            (u32)seqParallel.size(), ms, parts);
                    }
                }
                else
                    seqParallel.front()();
                seqParallel.pop_front();
                if (!seqParallelNames.empty()) seqParallelNames.pop_front();
                const float one = msd(std::chrono::high_resolution_clock::now() - ti).count();
                if (one > st.seqParallelMax) { st.seqParallelMax = one; st.seqParallelMaxIdx = st.seqParallelN; st.seqParallelMaxName = name; }
#ifdef __SWITCH__
                s_2nd_acc.add_task(name, one);
#endif
                ++st.seqParallelN;

                size--;
            }
        }
        st.seqParallel = lap2nd();

        if (g_pGameLevel && ps_lua_gc_method == gc_timeout)
        {
            const auto GCTime = (SecondThreadFreeTimeLast.count() / 2.0) * 1000.0;
            psLUA_GCTIMEOUT = std::max(psLUA_GCTIMEOUT_MIN, static_cast<int>(GCTime));
            // ⚠⚠⚠LuaJIT is not thread-safe, and this hands its garbage
            // collector to a worker while the main thread is free to be
            // running Lua. It survives a single-player game because the
            // script load is light and the window is narrow.
            //
            // ⚠Measured 28-07: a hundred NPCs whose binders run mod script
            // every frame closed that window. lj_gc_step took an access
            // violation inside gc_traverse_thread, on a pool thread, a
            // fraction of a second after a Lua error in an on_death
            // callback on the main thread. On a host that owns the world,
            // collecting inline is worth more than the parallelism.
            // ⭐20-08: the race above finally produced a NAMED stack, so this is
            // no longer a prediction. A DEP EXECUTE fault (c0000005) at a DATA
            // address, on TTAPI thread id16:
            //     lj_BC_FUNCC <- gc_call_finalizer (lj_gc.c:493)
            //                 <- lj_gc_step (lj_gc.c:707) <- CLevel::script_gc
            // while ANOTHER worker was inside CScriptEngine::lua_error with an
            // EMPTY traceback -- the VM was already inconsistent. The collector
            // and the net tick were two different threads in one LuaJIT state.
            //
            // Same corruption is the best explanation for that day's other two
            // failures: an anonymous access violation in _NetworkProcessor, and
            // a storm of 109 binder errors that ended in an assert because
            // STATE_Write wrote nothing.
            //
            // So: collect INLINE, exactly as the note above concluded and as
            // nothing had yet acted on -- the gate was g_ph_single_thread, which
            // is false on every host that owns a world.
            g_pGameLevel->script_gc(); // фаза PH_SCRIPTGC размечена ВНУТРИ script_gc
        }

        st.scriptGc = lap2nd();
        {
            ZoneScopedN("seqFrameMT");
            CpuBudget::PhaseScope _ph(CpuBudget::PH_SEQFRAMEMT);

            g_2nd_net_ms = 0.f;
            seqFrameMT.Process(rp_Frame, &st.seqFrameMTWorst, &st.seqFrameMTWorstMs); // only PHWorld and NetProcessor
        }
        st.seqFrameMT = lap2nd();
        s_2nd = st;
#ifdef __SWITCH__
        s_2nd_acc.sound += st.sound; s_2nd_acc.par += st.seqParallel; s_2nd_acc.gc += st.scriptGc; s_2nd_acc.mt += st.seqFrameMT;
        s_2nd_acc.net += g_2nd_net_ms;
        s_2nd_acc.total += st.sound + st.seqParallel + st.scriptGc + st.seqFrameMT; ++s_2nd_acc.n;
#endif

        const auto SecondThreadTasksEndTime = std::chrono::high_resolution_clock::now();
        SecondThreadTasksElapsedTime = SecondThreadTasksEndTime - SecondThreadTasksStartTime;
    });
    CpuBudget::PhaseLeave(CpuBudget::PH_SUBMIT2ND); // закрываем ОТПРАВКУ; тело лямбды считается на воркере

    // Network-client frame fix: for a network client, join the second thread
    // NOW -- before the render below -- so the network-spawn burst (seqParallel
    // -> ProcessGameSpawns) and physics (seqFrameMT) complete before the main
    // thread reads scene/render state. This serializes the frame, removing the
    // concurrency that otherwise races and hangs it right after "End of
    // synchronization". The later WaitSecondThread block becomes a no-op
    // (awaiter already waited, second_tasks already drained). No effect when
    // g_net_client_st is false (fused SP).
    if (g_net_client_st)
    {
        NC_PHASE("join_2nd");
        if (awaiter.valid())
            awaiter.wait();
        while (!second_tasks.empty())
        {
            if (second_tasks.back().valid())
                second_tasks.back().wait();
            second_tasks.pop_back();
        }
        NC_PHASE("post_join");

        // freeze-hunt heartbeat: the thin client's silent hangs leave no
        // trace in the log (last lines just stop). A 5-second pulse from
        // the device loop pins down WHERE frames stopped advancing.
        {
            static u32 s_hb_frames = 0;
            static u32 s_hb_last_ms = 0;
            ++s_hb_frames;
            if (dwTimeGlobal - s_hb_last_ms >= 5000)
            {
                Msg("[net client] device heartbeat: frame=%u (+%u since last)", dwFrame, s_hb_frames);
                s_hb_frames = 0;
                s_hb_last_ms = dwTimeGlobal;
            }
        }
    }
    return true;
}

bool CRenderDevice::RT_CanOverlap() const
{
    extern BOOL g_appLoaded;
    return g_appLoaded && g_pGameLevel && !load_screen_renderer.b_registered && dwPrecacheFrame == 0 &&
        b_is_Active && !is_editor_active && g_loading_events.empty();
}

bool CRenderDevice::Frame_Extract()
{
    // Join the previous frame's render before ANYTHING below touches renderer state (see RT_* in device.h). Begin joins
    // too; this one is here so the stall report names the wait.
    stall_mark("rt_wait");
    g_beginOverlap = g_rtTwoSlot && RT_CanOverlap();
    if (g_beginOverlap) RT_WaitFramesInFlight(1); else RT_WaitIdle();

    NC_PHASE("render_begin");
    bool begun;
    {
        CpuBudget::PhaseScope _ph(CpuBudget::PH_RBEGIN);
        begun = Begin();
    }
    g_beginOverlap = false;
    if (!begun)
        return false;

    ZoneScopedN("Render process");
    NC_PHASE("render_process");
    CpuBudget::PhaseScope _ph(CpuBudget::PH_RECORD);
    // CLevel::OnRender marks 'extract' (Calculate), 'submit' (Render->Render) and 'ui_post' inside this call.
    seqRender.Process(rp_Render);
    return true;
}

void CRenderDevice::Frame_Submit()
{
    ZoneScopedN("DoRender End");
    NC_PHASE("render_end");
    CpuBudget::PhaseScope _ph(CpuBudget::PH_PRESENT);
    // The record half was queued from CRender::Render. Everything the main thread emitted for this frame (UI,
    // bullets, HUD) is already in the deferred UI queue, so End_Render follows it on the render thread.
    g_rtOverlap = RT_CanOverlap();
    if (!g_rtOverlap)
    {
        End();   // menu / load screen / precache / editor: finished before the main thread goes on
        return;
    }
    if (CpuBudget::enabled)
    {
        static u32 s_lastMs = 0, s_frames = 0;
        static u64 s_t0 = 0;
        ++s_frames;
        const u32 nowMs = dwTimeContinual;
        if (!s_lastMs) { s_lastMs = nowMs; s_t0 = CPU::QPC_us(); g_rtMainWaitUs = 0; g_rtBusyUs = 0; g_rtIdleUs = 0; s_frames = 0; }
        else if (nowMs - s_lastMs >= 5000 && s_frames)
        {
            const double n = double(s_frames), wall = double(CPU::QPC_us() - s_t0) / 1000.0 / n;
            Msg("[rt] per frame (%u frames, %.3f ms wall): main thread WAITS for the render thread %.3f ms | render thread busy %.3f ms, IDLE %.3f ms",
                s_frames, wall, double(g_rtMainWaitUs.exchange(0)) / 1000.0 / n, double(g_rtBusyUs.exchange(0)) / 1000.0 / n,
                double(g_rtIdleUs.exchange(0)) / 1000.0 / n);
            s_lastMs = nowMs; s_t0 = CPU::QPC_us(); s_frames = 0;
        }
    }
    End_Main_Pre();   // nothing to do here (no precache while overlapping), kept for symmetry with End()
    g_bRendering = FALSE;   // the extract window closes here, on the main thread (End_Render may run long after)
    RT_Enqueue([this] {
#ifdef __SWITCH__
        const u64 t0 = CPU::QPC_us();
        RT_Profiled("end", [this] { End_Render(); });
        xr_nx_rt_add(1, CPU::QPC_us() - t0);
#else
        RT_Profiled("end", [this] { End_Render(); });
#endif
    });
    End_Main_Post();
}

#ifdef __SWITCH__
extern "C" int xr_nx_link_pop(char* out, int cap);
#endif
void CRenderDevice::on_idle()
{
#ifdef __SWITCH__
    // the load-time CPU boost follows the loading: the queued load events (the alife/cform/render phases run from
    // them with loadscreen=0 -- v190 missed exactly those), the load screen, and the precache frames behind it
    g_xr_nx_loading = (!g_loading_events.empty() || load_screen_renderer.b_registered || dwPrecacheFrame) ? 1 : 0;
    {
        // One line per 5 s: what is the loop waiting for? (precache countdown, autopause "press a key", load screen)
        static u32 s_nxLast = 0;
        const u32 nowMs = xrPlatform::TickCount();
        if (nowMs - s_nxLast >= 5000)
        {
            s_nxLast = nowMs;
            Msg("[nx state] dwFrame=%u precache=%u/%u paused=%d active=%d loadscreen=%d timeGlobal=%u loadEvents=%u", dwFrame,
                dwPrecacheFrame, dwPrecacheTotal, (int)Paused(), (int)b_is_Active, (int)load_screen_renderer.b_registered, dwTimeGlobal,
                (u32)g_loading_events.size());
        }
    }
#endif
    // Before every early return below: a hang during loading, or with the device
    // not ready, is exactly as silent as one in the middle of a frame.
    nc_watchdog_start();
    // Liveness, separate from the phase marks: the early returns below (device not
    // ready, loading events) run NO NC_PHASE, so a seq driven only by phases would
    // read a healthy loading screen as a frozen loop. Re-entering on_idle IS the
    // loop moving. The phase string is left alone so it still names where a hang
    // inside the frame actually sat.
    g_nc_phase_seq.fetch_add(1, std::memory_order_relaxed);

    is_editor_active = IsEditorShouldOpen();

    if (!b_is_Ready)
    {
        Sleep(100);
        return;
    }

#ifdef __SWITCH__
    // Console commands from the Wi-Fi debug link (nx/xr_nx_link.c), on this thread and never in the middle of a load step.
    if (g_loading_events.empty() && Console)
    {
        char cmd[512];
        while (xr_nx_link_pop(cmd, sizeof(cmd)))
        {
            Msg("[nx link] > %s", cmd);
            Console->Execute(cmd);
        }
    }
#endif
    if (!g_loading_events.empty())
    {
        RT_WaitIdle();   // a frame handed to the render thread must finish before any load step touches the renderer
        // Named so a hang INSIDE a load step reads as such instead of inheriting
        // the previous frame's phase.
        NC_PHASE("load_event");

        if (g_loading_events.front()())
            g_loading_events.pop_front();

        pApp->LoadDraw();

        return;
    }

    {
        ZoneScoped;

        // ⚠Reset and report HERE, not in message_loop: the headless server does
        // not run message_loop at all -- it drives the engine one frame at a
        // time from Ed_RenderFrame (x_ray.cpp), so a probe living in the loop
        // never executed and reported a clean zero while the audit was
        // measuring a 753 ms gap in the same seconds. on_idle is the one place
        // both drivers pass through.
        //
        // ⭐The marks from the END of the previous pass survive the reset long
        // enough to be reported, so the gap between "slept" and this line --
        // everything the driver loop does outside the frame -- is a bucket like
        // any other, and it is the bucket that turned out to hold the loss.
        // Цена кадра в ЯДРО-миллисекундах (cpu_budget). Здесь же, где
        // stall_report, и по той же причине: on_idle -- единственная точка,
        // через которую проходят оба драйвера кадра (клиент и хедлесс).
        CpuBudget::Frame();
        stall_report();
        s_stall.n = 0;
        stall_mark("frame_top");

#ifdef LOG_SECOND_THREAD_STATS
        const auto FrameStartTime = std::chrono::high_resolution_clock::now();
#endif

        if (psDeviceFlags.test(rsStatistic) || g_bForceStatGather)
            g_bEnableStatGather = TRUE;
        else
            g_bEnableStatGather = FALSE;

        if (is_editor_active)
        {
            ImGui_Backend_NewFrame(); // должно быть перед FrameMove
            s_imguiFrameOpen = ImGui::GetCurrentContext() != nullptr;
        }

        std::future<void> awaiter;
        std::chrono::high_resolution_clock::time_point SecondThreadStartTime;
        if (!Frame_Logic(awaiter, SecondThreadStartTime))
            return;

        Statistic->RenderTOTAL_Real.FrameStart();
        Statistic->RenderTOTAL_Real.Begin();

        if (b_is_Active)
        {
            ZoneScopedN("DoRender");

            if (Frame_Extract())
                Frame_Submit();
        }
        NC_PHASE("wait_2nd_tail");

        if (is_editor_active && s_imguiFrameOpen)   // not rendered this time (window inactive): close it unrendered
        {
            s_imguiFrameOpen = false;
            ImGui::EndFrame();
        }

        Statistic->RenderTOTAL_Real.End();
        Statistic->RenderTOTAL_Real.FrameEnd();

        Statistic->RenderTOTAL.accum = Statistic->RenderTOTAL_Real.accum;

#ifdef LOG_SECOND_THREAD_STATS
        const auto FrameEndTime = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::milli> FrameElapsedTime = FrameEndTime - FrameStartTime;
#endif

        {
            ZoneScopedN("WaitSecondThread");
            CpuBudget::PhaseScope _ph(CpuBudget::PH_WAIT2ND);

            const auto SecondThreadEndTime = std::chrono::high_resolution_clock::now();

#ifdef LOG_SECOND_THREAD_STATS
            bool show_stats{};
            if (awaiter.wait_for(FrameElapsedTime) == std::future_status::timeout)
                show_stats = true;
#else
            if (awaiter.valid())
#endif
            awaiter.wait();

            while (!second_tasks.empty())
            {
                if (second_tasks.back().valid())
                    second_tasks.back().wait();
                second_tasks.pop_back();
            }

            const std::chrono::duration<double, std::milli> SecondThreadElapsedTime = SecondThreadEndTime - SecondThreadStartTime;
            SecondThreadFreeTimeLast = SecondThreadElapsedTime - SecondThreadTasksElapsedTime;

#ifdef LOG_SECOND_THREAD_STATS
            if (show_stats && dwPrecacheFrame == 0)
            {
                Msg("##[%s] Second thread work time is too long! Avail: [%f]ms, used: [%f]ms, free: [%f]ms", __FUNCTION__, SecondThreadElapsedTime.count(),
                    SecondThreadTasksElapsedTime.count(), SecondThreadFreeTimeLast.count());
            }
#endif
        }

        extern void LogRenderDebugMessages();
        LogRenderDebugMessages();

        NC_PHASE("frame_end");
        FrameMark;
    }

    // ⭐The tail nobody was timing. b_is_Active is false on a headless server,
    // so this Sleep runs every single iteration -- and a 1 ms sleep is a
    // REQUEST, honoured at whatever granularity the system timer and the
    // scheduler feel like, on a box that is also running the client.
    stall_mark("idle_tail");
    if (!b_is_Active)
        Sleep(1);

    // ⭐r_fps_lock: a token bound to g_dwFPSlimit that NOTHING ever read. The
    // options screen has offered 60/120/144/240 for years and the frame rate has
    // never once changed — the same class of placebo as `rs_v_sync` and
    // `r_aa_mode`, and the reason this pass exists at all.
    //
    // ⚠A DEADLINE that carries, not a per-frame sleep: sleeping `period - work`
    // every frame accumulates the scheduler's overshoot and settles a few fps
    // under the target. The next deadline is the previous one plus the period, so
    // a frame that ran long is paid for by the next one running short.
    // ⚠And it must RESYNC after a stall (level load, alt-tab, a breakpoint),
    // otherwise the loop owes hundreds of frames and spins with no sleep until it
    // has caught up — which looks exactly like the limiter not working.
    if (g_dwFPSlimit)
    {
        CpuBudget::PhaseScope _ph(CpuBudget::PH_LIMITER);
        using clock = std::chrono::steady_clock;
        static clock::time_point s_next{};
        const auto period = std::chrono::nanoseconds(1000000000ull / u64(g_dwFPSlimit));

        auto now = clock::now();
        if (s_next.time_since_epoch().count() == 0 || now > s_next + period * 4)
            s_next = now; // first frame, or the loop fell far behind
        s_next += period;

        // ⭐ЖДАТЬ ДЕДЛАЙН НАДО ОЖИДАНИЕМ, А НЕ СПИНОМ.
        //
        // Здесь стояло `Sleep(left_ms - 1)` и `yield()` на последнюю миллисекунду.
        // Ядро при этом считает поток РАБОТАЮЩИМ: `yield()` на Windows -- это
        // SwitchToThread(), то есть крутящийся цикл. Замер `cpu_budget` при
        // fpslock60 показывал у главного потока 5.2 core-ms/кадр, из которых
        // около 1.0 -- ровно этот холостой цикл. Пока он там, ЛЮБОЙ замер цены
        // кадра на этой машине завышен на величину, сравнимую со статьями,
        // которые мы приехали оптимизировать (запись команд 1.37, планировщик 1.0).
        //
        // ⭐Ждём таймером ВЫСОКОГО РАЗРЕШЕНИЯ (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        // Win10 1803+): он точен до десятков микросекунд и не зависит от
        // системного тика, в отличие от Sleep. Последние `g_fps_spin_us`
        // микросекунд всё-таки отдаём yield'у -- страховка от недосыпа таймера,
        // чтобы дедлайн не переехал и лимитер не просел ниже цели.
        // ⚠Если таймер создать не удалось (старая система), поведение остаётся
        // прежним: Sleep до миллисекунды и спин на хвосте.
        static HANDLE s_hr_timer = []() -> HANDLE {
            HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!h) // до 1803 флага нет -- обычный таймер всё равно точнее Sleep
                h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            Msg(h ? "* [fps_lock] waitable timer: ON (idle wait, no spin)" : "! [fps_lock] no waitable timer -- falling back to Sleep+spin");
            return h;
        }();

        while ((now = clock::now()) < s_next)
        {
            const auto left_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(s_next - now).count();
            const auto spin_ns = std::chrono::nanoseconds(u64(g_fps_spin_us) * 1000).count();

            if (s_hr_timer && left_ns > spin_ns)
            {
                LARGE_INTEGER due;
                due.QuadPart = -((left_ns - spin_ns) / 100); // отрицательное = ОТНОСИТЕЛЬНО, в единицах 100 нс
                if (SetWaitableTimer(s_hr_timer, &due, 0, nullptr, nullptr, FALSE))
                {
                    WaitForSingleObject(s_hr_timer, INFINITE);
                    continue;
                }
            }

            const auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(s_next - now).count();
            if (!s_hr_timer && left_ms > 1)
                Sleep(DWORD(left_ms - 1));
            else
                std::this_thread::yield();
        }
    }

    stall_mark("slept");
}

void CRenderDevice::message_loop()
{
    MSG msg{};

    while (msg.message != WM_QUIT)
    {
        // ⭐Очередь надо ОСУШАТЬ, а не отпивать. Здесь стоял `if (PeekMessage)` —
        // ровно ОДНО сообщение за кадр. Всплеск ввода (мышь шлёт их пачками)
        // копится в очереди, и задержка ввода растёт вместе с временем кадра:
        // при 60 fps сообщение №5 из пачки ждёт своей очереди 83 мс.
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                break;

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;

        on_idle();
    }
}

void CRenderDevice::ShowMainWindow() const
{
    ShowWindow(m_hWnd, SW_SHOWNORMAL);
    SetForegroundWindow(m_hWnd);

    // ⚠️SWP_NOMOVE здесь был пропущен, и координаты 0,0 применялись всерьёз:
    // единственным эффектом вызова был переезд окна в начало ВИРТУАЛЬНОГО
    // рабочего стола. Это перечёркивало многомониторную установку рендера
    // (vk_RenderDeviceRender берёт mi.rcMonitor.left/top того монитора, на
    // котором окно живёт) — при игре на втором мониторе окно уезжало на
    // первый, сохраняя размер второго. Позиция принадлежит рендеру, не нам.
    SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

void CRenderDevice::Run()
{
    //	DUMP_PHASE;
    g_bLoaded = FALSE;

    Log("Starting engine...");

    set_current_thread_name("X-RAY Primary thread");
    mainThreadId = std::this_thread::get_id();
    RT::SetRole(RT::Main);
    Msg("Main thread id: [%u]", _Thrd_id());

    // Startup timers and calculate timer delta
    dwTimeGlobal = 0;
    Timer_MM_Delta = 0;
    {
        u32 time_mm = xrPlatform::TickCount();
        while (xrPlatform::TickCount() == time_mm)
            ; // wait for next tick
        u32 time_system = xrPlatform::TickCount();
        u32 time_local = TimerAsync();
        Timer_MM_Delta = time_system - time_local;
    }

    // Message cycle
    seqAppStart.Process(rp_AppStart);

    ::Render->ClearTarget();

    // Destroy LOGO
    HideSplash();

    ShowMainWindow();

    pInput->clip_cursor(true);

    Memory.mem_compact();

    g_bLoaded = TRUE;

    // Пока крутится кадровый цикл, машина не должна гасить экран и засыпать:
    // геймпад/мышь для системы «активностью» не считаются, а игрок при этом
    // за экраном. Перехват SC_SCREENSAVE в Device_wndproc — вторая, старая
    // половина того же лечения; современный способ — вот этот.
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

    message_loop();

    SetThreadExecutionState(ES_CONTINUOUS);

    g_bLoaded = FALSE;

    seqAppEnd.Process(rp_AppEnd);
}

u32 app_inactive_time = 0;
u32 app_inactive_time_start = 0;

void CRenderDevice::FrameMove()
{
    ZoneScoped;

    dwFrame++;

    u32 _old_continual = dwTimeContinual;
    dwTimeContinual = TimerMM.GetElapsed_ms() - app_inactive_time;
    dwTimeDeltaContinual = dwTimeContinual - _old_continual;

    fTimeDeltaReal = Timer.GetElapsed_sec();
    fTimeDeltaRealMS = static_cast<float>(Timer.GetElapsed_ms());
    if (!_valid(fTimeDeltaReal))
        fTimeDeltaReal = EPS_S + EPS_S;
    Timer.Start(); // previous frame

    {
        if (Paused())
            fTimeDelta = 0.0f;
        else
        {
            fTimeDelta = 0.1f * fTimeDelta + 0.9f * fTimeDeltaReal; // smooth random system activity - worst case ~7% error
            clamp(fTimeDelta, EPS_S + EPS_S, .1f); // limit to 10fps minimum
        }
        fTimeGlobal = TimerGlobal.GetElapsed_sec();

        u32 _old_global = dwTimeGlobal;
        dwTimeGlobal = TimerGlobal.GetElapsed_ms();
        dwTimeDelta = dwTimeGlobal - _old_global;
    }

    // Frame move
    ++s_frameMoveCalls;
    Statistic->EngineTOTAL.Begin();

    // Arm per-member seq profiling for THIS Process only (see pure.h): names the
    // seqFrame member EngineTOTAL's time goes to when every sub-timer reads 0.
    g_seq_profile_cb = g_seq_profile_hook;
    Device.seqFrame.Process(rp_Frame);
    g_seq_profile_cb = nullptr;

    Statistic->EngineTOTAL.End();
}

ENGINE_API BOOL bShowPauseString = TRUE;

void CRenderDevice::Pause(BOOL bOn, BOOL bTimer, BOOL bSound, LPCSTR reason)
{
    static int snd_emitters_ = -1;

    if (bOn)
    {
        if (!Paused())
            bShowPauseString = TRUE;

        if (bTimer)
        {
            g_pauseMngr->Pause(TRUE);
        }

        if (bSound && ::Sound)
        {
            snd_emitters_ = ::Sound->pause_emitters(true);
        }
    }
    else
    {
        if (bTimer && g_pauseMngr->Paused())
        {
            fTimeDelta = EPS_S + EPS_S;

            g_pauseMngr->Pause(FALSE);
        }

        if (bSound)
        {
            if (snd_emitters_ > 0) // avoid crash
            {
                snd_emitters_ = ::Sound->pause_emitters(false);
            }
        }
    }
}

BOOL CRenderDevice::Paused() { return g_pauseMngr->Paused(); }

void CRenderDevice::OnWM_Activate(WPARAM wParam, LPARAM lParam)
{
    const u16 fActive = LOWORD(wParam);
    const BOOL fMinimized = (BOOL)HIWORD(wParam);
    const BOOL bWndActive = (fActive != WA_INACTIVE) && !fMinimized ? TRUE : FALSE;

    const BOOL isGameActive = (!g_loading_events.empty() || (psDeviceFlags.is(rsAlwaysActive)) || bWndActive) ? TRUE : FALSE;

    if (g_bLoaded)
        pInput->clip_cursor(fActive != WA_INACTIVE);

    if (isGameActive != b_is_Active)
    {
        b_is_Active = isGameActive;

        if (b_is_Active)
        {
            seqAppActivate.Process(rp_AppActivate);
            app_inactive_time += TimerMM.GetElapsed_ms() - app_inactive_time_start;
        }
        else
        {
            app_inactive_time_start = TimerMM.GetElapsed_ms();
            seqAppDeactivate.Process(rp_AppDeactivate);
        }
    }
}

void CRenderDevice::SetAltScopeActive(bool bState)
{
    m_AltScopeActive = bState;
}

CLoadScreenRenderer::CLoadScreenRenderer() : b_registered(false) {}

void CLoadScreenRenderer::start(bool b_user_input)
{
    Device.seqRender.Add(this);
    b_registered = true;
    b_need_user_input = b_user_input;
    s_loadScreenT0 = xrPlatform::TickCount();
    s_loadScreenFrames = 0;
}

void CLoadScreenRenderer::stop()
{
    if (!b_registered)
        return;

    // THE number a player feels, and the one nobody was measuring: the load is not
    // over at "End of synchronization" -- the screen stays up for a further tail
    // (measured 30 ms to 1.6 s on pripyat_full) while whatever the precache deferred
    // finally runs. Printed here so every run carries both halves.
    const u32 now = xrPlatform::TickCount();
    Msg("[loadscreen] down after %u ms / %u frames | %u ms after the precache ended",
        now - s_loadScreenT0, s_loadScreenFrames, s_precacheEndMs ? now - s_precacheEndMs : 0);

    Device.seqRender.Remove(this);
#ifdef __SWITCH__
    xr_vk_wait_idle();
#endif
    pApp->DestroyLoadingScreen();
    b_registered = false;
    b_need_user_input = false;
}

void CLoadScreenRenderer::OnRender()
{
    if (!b_registered) // stop() already ran in this render pass (see CApplication::load_draw_internal)
        return;

    ++s_loadScreenFrames;

    // Two ways this screen ends, and only one of them was ever logged. With
    // `keypress_on_start on` the level does NOT start when loading finishes: it
    // arms an autopause and waits for a key, freezing the global clock (which is
    // also why the console channel goes deaf there). An unattended run therefore
    // sits on the backdrop forever and reads as a hang -- it is not one. Stamp the
    // moment the autopause is armed: THAT is when the load is over, key or no key.
    if (s_precacheEndMs && 0 == Device.dwPrecacheFrame)
    {
        const u32 since = xrPlatform::TickCount() - s_precacheEndMs;
        static bool s_armedReported = false;
        if (Device.Paused())
        {
            if (!s_armedReported)
            {
                s_armedReported = true;
                Msg("[loadscreen] autopause armed after %u ms / %u frames | %u ms after the precache ended (waiting for a key)",
                    xrPlatform::TickCount() - s_loadScreenT0, s_loadScreenFrames, since);
            }
        }
        else
        {
            static u32 s_lastReport = 0;
            if (since > 1000 && xrPlatform::TickCount() - s_lastReport > 1000)
            {
                s_lastReport = xrPlatform::TickCount();
                Msg("![loadscreen] STILL UP %u ms after the precache | frames=%u frameMove=%u events=%u active=%d",
                    since, s_loadScreenFrames, s_frameMoveCalls, (u32)g_loading_events.size(),
                    Device.b_is_Active ? 1 : 0);
            }
        }
    }

    pApp->load_draw_internal();
}

void CRenderDevice::time_factor(const float& time_factor)
{
    // [mod-compat diag] engine time factor ~0 freezes ALL object updates & physics
    // (CObjectList::Update gates on fTimeDelta) while the camera stays live — the
    // "мышь крутит, WASD мёртв" state. Every change is worth a log line.
    Msg("[Device] time_factor %.4f -> %.4f", Timer.time_factor(), time_factor);
    Timer.time_factor(time_factor);
    TimerGlobal.time_factor(time_factor);
    psSoundTimeFactor = time_factor; //--#SM+#--
}

bool OnMainThread() { return Device.OnMainThread(); } // for core

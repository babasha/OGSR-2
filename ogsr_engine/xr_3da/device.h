#pragma once

#include <atomic>

#include "pure.h"
#include "Render.h"
#include "../xrCore/FTimer.h"
#include "Stats.h"
#include <numeric>
#include <future>
#include <functional>
#include <chrono>
#include "../xrCore/render_thread.h"

extern u32 g_dwFPSlimit;
extern int g_fps_spin_us;

#define VIEWPORT_NEAR 0.2f
#define HUD_VIEWPORT_NEAR 0.005f

#define DEVICE_RESET_PRECACHE_FRAME_COUNT 10

#include "../Include/xrRender/FactoryPtr.h"
#include "../Include/xrRender/RenderDeviceRender.h"

class engine_impl;

#pragma pack(push, 4)

class IRenderDevice
{
};

class ENGINE_API CRenderDeviceData
{
public:

public:
    u32 dwWidth;
    u32 dwHeight;

    u32 dwPrecacheFrame;
    BOOL b_is_Ready;
    BOOL b_is_Active;

public:
    // Engine flow-control
    u32 dwFrame;

    float fTimeDelta{};
    float fTimeDeltaReal{}, fTimeDeltaRealMS{};
    float fTimeGlobal{};

    u32 dwTimeDelta{};
    u32 dwTimeGlobal{};
    u32 dwTimeContinual{};
    u32 dwTimeDeltaContinual{};

    // Cameras & projection
    Fvector vCameraPosition{};
    Fvector vCameraDirection{};
    Fvector vCameraTop{};
    Fvector vCameraRight{};

    Fmatrix mView{};
    Fmatrix mInvView{};
    Fmatrix mProject{};
    Fmatrix mFullTransform{};
    Fmatrix mInvFullTransform{};

    // Copies of corresponding members. Used for synchronization.
    Fvector vCameraPositionSaved{};
    Fvector vCameraDirectionSaved{};
    Fvector vCameraTopSaved{};
    Fvector vCameraRightSaved{};

    Fmatrix mViewSaved{};
    Fmatrix mProjectSaved{};
    Fmatrix mFullTransformSaved{};


    Fmatrix mView_old;
    Fmatrix mProject_old;
    Fmatrix mFullTransform_old;

    Fmatrix mView_hud, mView_hud2;
    Fmatrix mProject_hud, mProject_hud2;
    Fmatrix mFullTransform_hud, mFullTransform_hud2;

    Fmatrix mView_hud_old, mView_hud_old2;
    Fmatrix mProject_hud_old, mProject_hud_old2;
    Fmatrix mFullTransform_hud_old, mFullTransform_hud_old2;

    float fFOV;
    float fASPECT;

    // Генерирует псевдо-рандомное число в диапазоне от 0 до 1 https://stackoverflow.com/a/10625698
    template <typename T, size_t sizeDest>
    inline T NoiseRandom(const T (&args)[sizeDest]) const
    {
        constexpr double consts[]{23.14069263277926, 2.665144142690225};

        static_assert(sizeDest == std::size(consts));
        static_assert(std::is_floating_point_v<T>);

        constexpr auto frac = [](const double& x) {
            double res = x - std::floor(x);
            if constexpr (std::is_same_v<double, T>)
                return res;
            else
                return static_cast<T>(res);
        };

        return frac(std::cos(std::inner_product(std::begin(args), std::end(args), std::begin(consts), 0.0)) * 12345.6789);
    }

protected:
    u32 Timer_MM_Delta{};
    CTimer_paused Timer;
    CTimer_paused TimerGlobal;

    std::thread::id mainThreadId;

public:
    // Registrators
    CRegistrator<pureRender> seqRender;
    CRegistrator<pureFrame> seqFrame;

    CRegistrator<pureAppActivate> seqAppActivate;
    CRegistrator<pureAppDeactivate> seqAppDeactivate;
    CRegistrator<pureAppStart> seqAppStart;
    CRegistrator<pureAppEnd> seqAppEnd;

    CRegistrator<pureScreenResolutionChanged> seqResolutionChanged;

    HWND m_hWnd;

    // ⚠Must stay false for the render thread (step 12): it compares against the
    // thread that ran Create(), and the render thread is a different one.
    bool OnMainThread() const
    {
        extern BOOL g_bLoaded;
        return !g_bLoaded || std::this_thread::get_id() == mainThreadId;
    }
};

class ENGINE_API CRenderDeviceBase : public IRenderDevice, public CRenderDeviceData
{
public:
};

#pragma pack(pop)
// refs
class ENGINE_API CRenderDevice : public CRenderDeviceBase
{
private:
    // m_dwWindowStyle / m_rcWindowBounds / m_rcWindowClient сняты: заполнялись в
    // Device_Initialize и не читались НИ ОДНИМ файлом обоих деревьев, а хранили
    // при этом состояние окна ДО рестайла рендером, то есть заведомо ложное.
    CTimer TimerMM;

    void _Create();
    void _Destroy(BOOL bKeepTextures);
    void _SetupStates();

    xr_deque<fastdelegate::FastDelegate<void()>> seqParallel;
    xr_deque<const char*> seqParallelNames;   // add_to_seq_parallel's name per task, in lockstep

    xr_vector<std::future<void>> second_tasks;
    SpinLock second_tasks_lock;

public:
    LRESULT MsgProc(HWND, UINT, WPARAM, LPARAM);

    u32 dwPrecacheTotal;
    float fWidth_2, fHeight_2;
    void OnWM_Activate(WPARAM wParam, LPARAM lParam);

    IRenderDeviceRender* m_pRender;

    void DumpResourcesMemoryUsage() { m_pRender->ResourcesDumpMemoryUsage(); }

public:
    CRegistrator<pureFrame> seqFrameMT;
    CRegistrator<pureDeviceReset> seqDeviceReset;

    CStats* Statistic;

    CRenderDevice()
        : m_pRender(nullptr)
    {
        m_hWnd = nullptr;
        b_is_Active = FALSE;
        b_is_Ready = FALSE;
        Timer.Start();
    };

    void Pause(BOOL bOn, BOOL bTimer, BOOL bSound, LPCSTR reason);
    BOOL Paused();

    // Scene control
    void PreCache(u32 amount, bool b_draw_loadscreen, bool b_wait_user_input);
    BOOL Begin();
    void Clear();
    void End();
    // RENDER_THREAD_PLAN step 11: End() = End_Main_Pre() + End_Render() + End_Main_Post(), in this order.
    //   End_Main_Pre  : precache countdown / adaptive stop (main-thread bookkeeping; note it still calls
    //                   Render->ClearTarget once while the load screen is not up -- End() joins the record first).
    //   End_Render    : g_bRendering, ImGui draw data, Render->End() (submit + present) -- a render-thread job.
    //   End_Main_Post : the *Saved copies of the camera.
    // End() is the joined frame end (returns with the frame finished); Frame_Submit overlaps instead when it can.
    void End_Main_Pre();
    void End_Render();
    void End_Main_Post();
    void FrameMove();

#pragma warning(push)
#pragma warning(disable : 4366)
    IC CTimer_paused* GetTimerGlobal() { return &TimerGlobal; }
#pragma warning(pop)

    u32 TimerAsync() { return TimerGlobal.GetElapsed_ms(); }
    u32 TimerAsync_MMT() { return TimerMM.GetElapsed_ms() + Timer_MM_Delta; }

    // Creation & Destroying
    void ConnectToRender();
    void Create(void);
    void ShowMainWindow() const;
    void Run(void);
    void Destroy(void);
    void Reset(bool precache = true);

    void Initialize(void);

    void time_factor(const float& time_factor);
    inline const float time_factor() const
    {
        VERIFY(Timer.time_factor() == TimerGlobal.time_factor());
        return (Timer.time_factor());
    }

private:
    std::chrono::duration<double, std::milli> SecondThreadTasksElapsedTime{}, SecondThreadFreeTimeLast{};

public:
    // name: what the second thread's [2nd] line calls this task when it is the slowest
    // of its frame (a string literal -- stored, not copied). Kept in lockstep with seqParallel.
    ICF bool add_to_seq_parallel(const fastdelegate::FastDelegate<void()>& delegate, const char* name = nullptr)
    {
        auto I = std::find(seqParallel.begin(), seqParallel.end(), delegate);
        if (I != seqParallel.end())
            return false;
        seqParallel.push_back(delegate);
        seqParallelNames.push_back(name ? name : "?");
        return true;
    }

    ICF void remove_from_seq_parallel(const fastdelegate::FastDelegate<void()>& delegate)
    {
        for (size_t i = 0; i < seqParallel.size();)
        {
            if (seqParallel[i] == delegate)
            {
                seqParallel.erase(seqParallel.begin() + i);
                seqParallelNames.erase(seqParallelNames.begin() + i);
            }
            else
                ++i;
        }
    }

    // ⚠This is NOT dead, and assuming it was cost this hunt a wrong premise.
    // Two game-side callers are live -- CustomMonster.cpp and vision_client.cpp,
    // both submitting feel_vision_update -- so a pool task CAN be running
    // alongside the second-thread task. Those two happen not to touch Lua (the
    // VM watch sees no entries from them), which is exactly the property that
    // has to hold for anything submitted here: everything in flight next to the
    // frame task shares one non-reentrant LuaJIT state. See
    // xrCore/lua_vm_watch.h before adding a third caller.
    IC void add_to_awaiters(std::future<void> f)
    {
        second_tasks_lock.lock();
        second_tasks.push_back(std::move(f));
        second_tasks_lock.unlock();
    }

public:
    void on_idle();
    // on_idle's frame, cut in three along the future main/render thread seam
    // (RENDER_THREAD_PLAN step 1). Called back to back in the same order as before.
    //   Frame_Logic   = FrameMove + OnCameraUpdated + second-thread submit   (main thread)
    //   Frame_Extract = Begin + seqRender (CLevel::OnRender: Calculate = extract, then Render)
    //   Frame_Submit  = End (submit + present, on the render thread)
    // Frame_Logic returns false when the frame must be abandoned (loading events).
    bool Frame_Logic(std::future<void>& awaiter, std::chrono::high_resolution_clock::time_point& secondThreadStart);
    bool Frame_Extract();
    bool RT_CanOverlap() const;   // the main thread may run ahead of this frame (gameplay: no load screen / precache / editor)
    void Frame_Submit();
    bool on_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& result);

    void OnCameraUpdated(bool from_actor);

private:
    void message_loop();

public:
    bool m_AltScopeActive{};
    IC bool IsAltScopeActive() { return m_AltScopeActive; }
    void SetAltScopeActive(bool bState);
};

extern ENGINE_API CRenderDevice Device;

// Put a named wall-clock mark on the current loop iteration's timeline. Any
// iteration that turns out to have taken longer than the stall threshold gets
// its per-mark breakdown printed as "! [stall]". For a process that drives the
// engine from its own loop (the headless server), marking that loop's steps is
// the only way its time is attributable at all -- see device.cpp.
ENGINE_API void Device_StallMark(const char* name);

// The render thread. Every frame's record + present runs there. TWO-SLOT PIPELINE (xrCore/render_thread.h: RT::Slot, RT::PerSlot):
//   main  : [logic N+1][wait: frame N-1 done][Begin_Extract N+1 + extract N+1 + UI emission][logic N+2] ...
//   render:            [Begin_Render N + Render_Record N + End_Render N]  [Begin_Render N+1 + Render_Record N+1 + ...]
// In gameplay the extract of frame N+1 runs in parallel with the record + present of frame N: everything extract produces
// lives in the slot of its frame (snapshots, lists, PREPARE products, UI queue) and the GPU rings it writes are indexed by the
// frame's ring slot. At most one frame may be unfinished when a new one begins (RT_WaitFramesInFlight(1)). Menu / load
// screen / precache / editor frames are joined (RT_WaitIdle) before and inside End() instead. XROS_RT_ONESLOT=1 restores the
// old join before every Begin.
ENGINE_API void RT_Enqueue(std::function<void()> job);   // runs on the render thread, in order (starts it on first use)
ENGINE_API void RT_WaitIdle();                           // blocks until the render thread has nothing queued or running; no-op on it
ENGINE_API void RT_FrameDone();                       // render thread, end of End_Render: this frame's slot is free again
ENGINE_API void RT_Profiled(const char* what, const std::function<void()>& job); // render thread: job under [rt prof] (dbg_rt_prof <ms>)
ENGINE_API void RT_WaitFramesInFlight(u32 maxPending);   // main thread: block until at most maxPending begun frames are unfinished
ENGINE_API void RT_Shutdown();                           // drains and joins the render thread (device destroy)

extern ENGINE_API xr_list<fastdelegate::FastDelegate<bool()>> g_loading_events;

class ENGINE_API CLoadScreenRenderer : public pureRender
{
public:
    CLoadScreenRenderer();
    void start(bool b_user_input);
    void stop();
    virtual void OnRender();

    bool b_registered;
    bool b_need_user_input{};
};

extern ENGINE_API CLoadScreenRenderer load_screen_renderer;

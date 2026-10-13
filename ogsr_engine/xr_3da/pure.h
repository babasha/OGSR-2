#pragma once

#include "../xrCore/load_prof.h" // [frame prof]: CRegistrator::Process names each member

// messages
#define REG_PRIORITY_LOW 0x11111111ul
#define REG_PRIORITY_NORMAL 0x22222222ul
#define REG_PRIORITY_HIGH 0x33333333ul

// ⚠⚠The field it is stored in and compared with is `int Prio`. With `0xfffffffful` this was correct only where `unsigned long`
// is 32 bits (Windows/LLP64): on LP64 (Linux, Horizon) `Prio == -1` widens to 0xFFFFFFFFFFFFFFFF and NEVER equals 0xffffffff, so
// Remove() silently did nothing -- removed objects (the load screen, the tutorial sequencer, ...) stayed registered, kept being
// called after they were freed, and crashed the console three times in a row (30-09). A plain int constant is the same -1 everywhere.
#define REG_PRIORITY_INVALID (-1)

typedef void RP_FUNC(void* obj);

// Set by xrGame at startup: peek the pending Lua error text — a raw LuaJIT unwind
// (caught only as `catch(...)`, no std::exception) leaves it on the lua stack top.
// Lets engine-side guards/FATALs name the script error they intercepted.
extern ENGINE_API const char* (*g_lua_error_peek)();

// Per-member seq profiling (VK profiler): when g_seq_profile_cb is non-null,
// CRegistrator::Process times each member and reports (typeid name, ms).
// device.cpp arms it from g_seq_profile_hook around seqFrame.Process ONLY —
// EngineTOTAL grows on vistas while every sub-timer reads 0, so this names the
// seqFrame member the time actually goes to. Installed by the VK profiler.
using seq_profile_cb = void (*)(const char* typeName, float ms);
extern ENGINE_API seq_profile_cb g_seq_profile_cb;    // live only inside seqFrame.Process
extern ENGINE_API seq_profile_cb g_seq_profile_hook;  // provider (VK profiler); null = off

extern ENGINE_API RP_FUNC rp_Frame;
class ENGINE_API pureFrame
{
public:
    virtual void OnFrame(void) = 0;
};
extern ENGINE_API RP_FUNC rp_Render;
class ENGINE_API pureRender
{
public:
    virtual void OnRender(void) = 0;
};
extern ENGINE_API RP_FUNC rp_AppActivate;
class ENGINE_API pureAppActivate
{
public:
    virtual void OnAppActivate(void) = 0;
};
extern ENGINE_API RP_FUNC rp_AppDeactivate;
class ENGINE_API pureAppDeactivate
{
public:
    virtual void OnAppDeactivate(void) = 0;
};
extern ENGINE_API RP_FUNC rp_AppStart;
class ENGINE_API pureAppStart
{
public:
    virtual void OnAppStart(void) = 0;
};
extern ENGINE_API RP_FUNC rp_AppEnd;
class ENGINE_API pureAppEnd
{
public:
    virtual void OnAppEnd(void) = 0;
};
extern ENGINE_API RP_FUNC rp_DeviceReset;
class ENGINE_API pureDeviceReset
{
public:
    virtual void OnDeviceReset(void) = 0;
};
extern ENGINE_API RP_FUNC rp_ScreenResolutionChanged;
class ENGINE_API pureScreenResolutionChanged
{
public:
    virtual void OnScreenResolutionChanged(void) = 0;
};

template <class T>
class CRegistrator // the registrator itself
{
    //-----------------------------------------------------------------------------
    struct _REG_INFO
    {
        T* Object;
        int Prio;
    };

public:
    xr_vector<_REG_INFO> R;

    // constructor
    struct
    {
        u32 in_process : 1;
        u32 changed : 1;
    };

    CRegistrator()
    {
        in_process = false;
        changed = false;
    }

    constexpr void Add(T* object) { Add({object, REG_PRIORITY_NORMAL}); }
    constexpr void Add(T* object, const int priority) { Add({object, priority}); }

    void Add(_REG_INFO&& newMessage)
    {
        bool found = false;

        for (u32 i = 0; i < R.size(); i++)
        {
            if (R[i].Object == newMessage.Object)
            {
                if (R[i].Prio == newMessage.Prio)
                {
                    return; // found with same priority
                }

                R[i].Prio = newMessage.Prio;
                found = true;
            }
        }

        if (!found)
            R.emplace_back(newMessage);

        if (in_process)
            changed = true;
        else
            Resort();
    }

    void Remove(T* obj)
    {
        for (u32 i = 0; i < R.size(); i++)
        {
            if (R[i].Object == obj)
                R[i].Prio = REG_PRIORITY_INVALID;
        }

        if (in_process)
            changed = true;
        else
            Resort();
    }

    // worstName/worstMs (optional): the slowest member of this pass and its time -- the
    // second thread's [2nd] line names what in seqFrameMT took a stalled frame.
    void Process(RP_FUNC* f, const char** worstName = nullptr, float* worstMs = nullptr)
    {
        if (R.empty())
            return;

        in_process = true;

        // Per-member guard: an exception (Lua error on foreign-map content) thrown by
        // one member used to unwind the WHOLE chain — every later member (physics
        // step, scheduler, weather) silently skipped EVERY frame: the world freezes
        // while rendering runs. Name the offender, skip it, keep the chain alive.
        {
            for (u32 i = 0; i < R.size(); i++)
                if (R[i].Prio != REG_PRIORITY_INVALID && R[i].Object != nullptr)
                {
                    // Optional per-member timing (see g_seq_profile_cb above). The
                    // name is taken BEFORE the call — the member may unregister
                    // itself inside it. ~µs QPC cost, only while armed.
                    const seq_profile_cb prof = g_seq_profile_cb;
                    const char* profName = (prof || worstName) ? typeid(*R[i].Object).name() : nullptr;
                    CTimer profT;
                    if (prof || worstName) profT.Start();
                    // [frame prof]: each member under its class name -- 08-10 the Switch's frame_move had ~2.8 ms of
                    // "self" time nobody named. A no-op unless a LoadProf::Arm is up on this thread.
                    LoadProf::Scope lp(LoadProf::tls().armed ? typeid(*R[i].Object).name() : "");
                    try
                    {
                        f(R[i].Object);
                    }
                    catch (const std::exception& e)
                    {
                        static u32 s_throws = 0;
                        const u32 n = ++s_throws;
                        if ((n & (n - 1)) == 0) // 1,2,4,8,…
                            Msg("!![CRegistrator<%s>] member #%u (%s) threw [%s: %s] — skipped (total %u)", typeid(T).name(), i, typeid(*R[i].Object).name(), typeid(e).name(),
                                e.what(), n);
                    }
                    catch (...)
                    {
                        static u32 s_throws = 0;
                        const u32 n = ++s_throws;
                        if ((n & (n - 1)) == 0) // 1,2,4,8,…
                            Msg("!![CRegistrator<%s>] member #%u (%s) threw [non-std; lua top: %s] — skipped (total %u)", typeid(T).name(), i, typeid(*R[i].Object).name(),
                                g_lua_error_peek ? g_lua_error_peek() : "<no hook>", n);
                    }
                    if (prof || worstName)
                    {
                        const float ms = profT.GetElapsed_sec() * 1000.f;
                        if (prof) prof(profName, ms);
                        if (worstName && worstMs && ms > *worstMs) { *worstMs = ms; *worstName = profName; }
                    }
                }
        }

        if (changed)
            Resort();

        in_process = false;
    }

    void Resort(void)
    {
        if (!R.empty())
        {
            std::sort(std::begin(R), std::end(R), [](const auto& a, const auto& b) { return a.Prio > b.Prio; });
        }

        while (!R.empty() && R[R.size() - 1].Prio == REG_PRIORITY_INVALID)
        {
            R.pop_back();
        }

        if (R.empty())
            R.clear();

        changed = false;
    }
};

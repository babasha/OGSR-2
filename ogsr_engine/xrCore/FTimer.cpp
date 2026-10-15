#include "stdafx.h"

XRCORE_API BOOL g_bEnableStatGather = FALSE;
XRCORE_API BOOL g_bForceStatGather = FALSE;   // see FTimer.h — VK profiler stat gathering

void CStatTimer::FrameStart()
{
    accum = Duration();
    count = 0;
}

// Sub-ms resolution: GetElapsed_ms() truncates to whole milliseconds, which made
// every [VK CPU] line read "sched=1.00 engine=2.00" — useless for a logic budget.
void CStatTimer::FrameEnd() { result = GetElapsed_sec() * 1000.f; }

pauseMngr* g_pauseMngr;

pauseMngr& g_pauseMngr_get()
{
    static pauseMngr manager;
    g_pauseMngr = &manager;

    return manager;
}

pauseMngr::pauseMngr() : paused(false) { m_timers.reserve(3); }

void pauseMngr::Pause(const bool b)
{
    if (paused == b)
        return;

    for (auto& timer : m_timers)
    {
        timer->Pause(b);
    }

    paused = b;
}

void pauseMngr::Register(CTimer_paused& t) { m_timers.push_back(&t); }

void pauseMngr::UnRegister(CTimer_paused& t)
{
    const auto it = std::find(m_timers.cbegin(), m_timers.cend(), &t);
    if (it != m_timers.end())
        m_timers.erase(it);
}

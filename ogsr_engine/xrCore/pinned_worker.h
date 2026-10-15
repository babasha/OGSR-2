#pragma once

// PinnedWorker: one persistent thread on a chosen core that runs the jobs it is
// handed in order -- Run(f) queues a job, Wait() blocks until every job queued
// so far has finished, Busy() says whether any is still pending.
//
// ⭐Why (07-10-2026, Switch): the game has three cores (0-2, process mask 0x7;
// the A53s are off, core 3 is the system's) and libnx gives every new pthread
// ideal core 0 -- the main thread's. In a firefight core 0 ran 76-89%, core 1
// (the render thread) 40-50%, core 2 15-25%. Spreading the pool over 1/2
// (nx_cores.txt spread) changed nothing, because the main thread still WAITED
// for the fork-joins it started. Work moves off the main thread only when it
// runs while the main thread does something else -- this is the thread for that.
//
// On the Switch the thread pins itself (svcSetThreadCoreMask) and raises its
// priority above the background workers sharing its core (0x3B: HUD prefetch,
// boneFaces): equal priorities share a core in 10 ms quanta, and the main
// thread is waiting on this one. Elsewhere it is just a thread.
//
// Leaked on purpose by its owners (new, never delete): no join at exit, the
// thread sleeps on the condition variable when idle.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#ifdef __SWITCH__
extern "C" u32 svcSetThreadCoreMask(u32 thread, s32 preferred_core, u32 affinity_mask); // libnx
extern "C" u32 svcSetThreadPriority(u32 thread, u32 priority);                          // libnx
#endif

class PinnedWorker
{
public:
    // prio: Switch thread priority (lower = more urgent; the main thread is 0x2C, pthreads 0x3B)
    PinnedWorker(int core, int prio = 0x2E) : m_core(core), m_prio(prio) {}

    void Run(std::function<void()> f)
    {
        {
            std::lock_guard<std::mutex> g(m_mx);
            if (!m_started)
            {
                m_started = true;
                std::thread([this] { loop(); }).detach();
            }
            m_jobs.push_back(std::move(f));
            ++m_pending;
        }
        m_cv.notify_one();
    }

    void Wait()
    {
        std::unique_lock<std::mutex> l(m_mx);
        m_doneCv.wait(l, [this] { return m_pending == 0; });
    }

    bool Busy()
    {
        std::lock_guard<std::mutex> g(m_mx);
        return m_pending != 0;
    }

    // Switch: the priority from the next job on (0 = keep). 📏12-10-2026: the bones / particle workers on core 1 at 0x2E sat
    // below the render thread (0x2D) recording the previous frame there, while the main thread waited for them.
    void SetPrio(int prio)
    {
        if (prio > 0)
            m_wantPrio.store(prio, std::memory_order_relaxed);
    }

private:
    void loop()
    {
#ifdef __SWITCH__
        svcSetThreadCoreMask(0xFFFF8000u /*CUR_THREAD_HANDLE*/, m_core, 1u << m_core);
        svcSetThreadPriority(0xFFFF8000u, u32(m_prio));
#endif
        for (;;)
        {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> l(m_mx);
                m_cv.wait(l, [this] { return !m_jobs.empty(); });
                f = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
#ifdef __SWITCH__
            if (const int want = m_wantPrio.load(std::memory_order_relaxed); want > 0 && want != m_prio)
            {
                m_prio = want;
                svcSetThreadPriority(0xFFFF8000u, u32(m_prio));
            }
#endif
            f();
            {
                std::lock_guard<std::mutex> g(m_mx);
                --m_pending;
            }
            m_doneCv.notify_all();
        }
    }

    const int m_core;
    int m_prio;                       // the worker thread only, after the constructor
    std::atomic<int> m_wantPrio{0};   // SetPrio -> applied by the worker before its next job
    std::mutex m_mx;
    std::condition_variable m_cv, m_doneCv;
    std::deque<std::function<void()>> m_jobs;
    unsigned m_pending = 0;
    bool m_started = false;
};

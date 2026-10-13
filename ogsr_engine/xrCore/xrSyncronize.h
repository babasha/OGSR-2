#pragma once
#include <mutex>

// KRodin: Теперь это просто обёртка над std::recursive_mutex, т.к. мне лень кучу кода переделывать.

//#define PROFILE_CRITICAL_SECTIONS // TODO: Порезать этот трэш везде.

// ⚠️Enter/Leave are hand-paired all over the engine, and a long body between
// them is one exception away from abandoning the lock FOREVER: the throw unwinds
// straight past Leave, the next Enter blocks, and the process deadlocks with
// NOTHING in the log — no FATAL, no stack, frames simply stop.
//
// That is exactly how a session died 20-08: a null deref in xrServer::Update's
// respawn queue unwound out of a 177-line Enter/Leave region, CRegistrator's
// catch(...) logged "skipped" without releasing anything, and the next tick sat
// on csPlayers until the process was killed.
//
// lock()/unlock() exist so the standard guards work directly:
//     std::lock_guard<xrCriticalSection> lock(cs);
// Prefer that over Enter/Leave for anything longer than a couple of statements.
class xrCriticalSection final : std::recursive_mutex
{
    // ⚠19-09: __super -- MSVC-расширение ("непосредственный базовый класс"),
    // нет в GCC/Clang. Базовый класс здесь ровно один и не меняется, так что
    // прямое имя -- точный эквивалент, не обход смысла.
    using Base = std::recursive_mutex;

public:
    inline void Enter() { Base::lock(); }
    inline void Leave() { Base::unlock(); }
    inline bool TryEnter() { return Base::try_lock(); }

    // Named for std::lock_guard / std::unique_lock (BasicLockable).
    inline void lock() { Base::lock(); }
    inline void unlock() { Base::unlock(); }
};

class SpinLock
{
    std::atomic_flag locked = ATOMIC_FLAG_INIT;

public:
    void lock()
    {
        while (locked.test_and_set(std::memory_order_acquire))
        {
            ;
        }
    }
    void unlock() { locked.clear(std::memory_order_release); }
};

// xrSharedMutex: a reader-writer lock for the hot shared structures (ISpatial_DB, the particle manager).
//
// PC: std::shared_mutex. Switch: libstdc++'s shared_mutex is a pthread rwlock there (a mutex + condvars) with NO
// priority inheritance -- a 0x2D render thread or the 0x2C main thread waiting for a write held by a 0x3B background
// thread waits until the scheduler happens to run that thread, which on a busy core can take hundreds of ms (📏11-10:
// the same kind of wait made VMA's lock a suspect of 180-590 ms rt_wait stalls, see vma_impl.cpp).
// Here a writer holds a plain std::mutex (libnx Mutex = svcArbitrateLock: the kernel lends a waiter's priority to the
// owner) for its whole write, raises a flag and waits for the readers in flight to leave. A reader counts itself in
// with two atomics (no syscall, readers run side by side); only when it finds the flag up does it step back and wait on
// that mutex -- boosting the writer -- then tries again. Flag and count are seq_cst on both sides (a reader that missed
// the flag is seen by the writer's count check). Writer-preferring.
// ⚠Not recursive for readers: a shared lock taken again inside a shared lock deadlocks if a writer arrived in between.
#ifdef __SWITCH__
#include <atomic>
#include <thread>
#include <chrono>
class xrSharedMutex
{
    std::mutex m_writer;
    std::atomic<int> m_readers{0};
    std::atomic<bool> m_writing{false};

    void wait_readers_out()
    {
        for (int spins = 0; m_readers.load(std::memory_order_seq_cst) != 0; ++spins)
        {
            if (spins < 64)
                ;
            else if (spins < 256)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    }

public:
    void lock()
    {
        m_writer.lock();
        m_writing.store(true, std::memory_order_seq_cst);
        wait_readers_out();
    }
    bool try_lock()
    {
        if (!m_writer.try_lock())
            return false;
        m_writing.store(true, std::memory_order_seq_cst);
        if (m_readers.load(std::memory_order_seq_cst) != 0)
        {
            m_writing.store(false, std::memory_order_seq_cst);
            m_writer.unlock();
            return false;
        }
        return true;
    }
    void unlock()
    {
        m_writing.store(false, std::memory_order_seq_cst);
        m_writer.unlock();
    }

    void lock_shared()
    {
        for (;;)
        {
            m_readers.fetch_add(1, std::memory_order_seq_cst);
            if (!m_writing.load(std::memory_order_seq_cst))
                return;
            m_readers.fetch_sub(1, std::memory_order_seq_cst);
            // a writer is in: queue on its mutex (the kernel boosts it to this thread's priority), then try again
            m_writer.lock();
            m_writer.unlock();
        }
    }
    bool try_lock_shared()
    {
        m_readers.fetch_add(1, std::memory_order_seq_cst);
        if (!m_writing.load(std::memory_order_seq_cst))
            return true;
        m_readers.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }
    void unlock_shared() { m_readers.fetch_sub(1, std::memory_order_seq_cst); }
};
#else
#include <shared_mutex>
using xrSharedMutex = std::shared_mutex;
#endif

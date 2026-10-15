/*
 * Switch: pthread_rwlock_* without a system call on the uncontended path (07-10).
 *
 * WHY. std::shared_mutex is pthread_rwlock_t here, and newlib's (devkitA64) rdlock/unlock take a libnx mutex and, when
 * the LAST reader leaves, __libc_cond_broadcast -- svcSignalProcessWideKey -- whether anybody waits or not. One reader
 * is the common case, so every shared lock of the spatial DB (ISpatial q_ray / q_box / q_frustum), of every CDB tile a
 * ray crosses (xrCDB_tiled.h), of the particle manager's effects and actions, of an NPC's visible list... paid a kernel
 * call. 📏07-10 firefight in the Cordon village, 1 kHz samples of the main thread (nx_prof_raw.bin, v207):
 * svcSignalProcessWideKey 7.1% of its time; PAPI::CParticleManager::Update 63% rwlock rdlock/unlock; NxOcc::RayBlocked
 * 12% rwlock unlock -- with the main thread the one thing between the fight and 60 fps.
 *
 * HOW. The same 16 bytes (zero = unlocked, as PTHREAD_RWLOCK_INITIALIZER) read as { waiters, -, -, state }:
 *   state bits 0..29 readers, bit 30 a writer waits (new readers hold back: newlib prefers writers too, keep that),
 *   bit 31 a writer holds it. Taking and dropping are one CAS / atomic op. A thread that has spun a while sleeps on
 *   the state word (svcWaitForAddress, "while it still equals what I saw"), counted in `waiters`; a release signals the
 *   word only when that count is not zero. The count goes up before the sleep and the state changes before the count
 *   is read (both seq_cst), so a release either sees the sleeper or the sleeper's kernel compare sees the new state.
 *   A sleep is also capped at 2 ms -- a missed wake costs a frame's sliver, never a hang.
 *
 * Linked with -Wl,--wrap=pthread_rwlock_{rdlock,wrlock,unlock,tryrdlock,trywrlock} (xr_3da/CMakeLists.txt).
 * pthread_rwlock_init/destroy stay newlib's: init writes zeros, which is "unlocked" here too.
 */
#include <switch.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>

typedef struct
{
    uint32_t waiters;
    uint32_t unused1, unused2;
    uint32_t state;
} nx_rw;

_Static_assert(sizeof(nx_rw) <= sizeof(pthread_rwlock_t), "the rwlock must fit in pthread_rwlock_t");

#define RW_READERS 0x3FFFFFFFu
#define RW_WWAIT 0x40000000u
#define RW_WHELD 0x80000000u
#define RW_SPINS 64
#define RW_SLEEP_NS 2000000LL

static inline uint32_t rw_load(nx_rw* l) { return __atomic_load_n(&l->state, __ATOMIC_ACQUIRE); }

static inline int rw_cas(nx_rw* l, uint32_t* seen, uint32_t to)
{
    return __atomic_compare_exchange_n(&l->state, seen, to, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void rw_pause(nx_rw* l, uint32_t seen, int* spins)
{
    if (++*spins < RW_SPINS)
    {
        __asm__ __volatile__("yield");
        return;
    }
    __atomic_fetch_add(&l->waiters, 1, __ATOMIC_SEQ_CST);
    svcWaitForAddress(&l->state, ArbitrationType_WaitIfEqual, (s64)(s32)seen, RW_SLEEP_NS);
    __atomic_fetch_sub(&l->waiters, 1, __ATOMIC_SEQ_CST);
    *spins = 0;
}

static inline void rw_wake(nx_rw* l)
{
    if (__atomic_load_n(&l->waiters, __ATOMIC_SEQ_CST))
        svcSignalToAddress(&l->state, SignalType_Signal, 0, -1);
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t* rw)
{
    if (!rw)
        return EINVAL;
    nx_rw* l = (nx_rw*)rw;
    int spins = 0;
    for (;;)
    {
        uint32_t s = rw_load(l);
        if (!(s & (RW_WWAIT | RW_WHELD)))
        {
            if (rw_cas(l, &s, s + 1))
                return 0;
            continue;
        }
        rw_pause(l, s, &spins);
    }
}

int __wrap_pthread_rwlock_tryrdlock(pthread_rwlock_t* rw)
{
    if (!rw)
        return EINVAL;
    nx_rw* l = (nx_rw*)rw;
    uint32_t s = rw_load(l);
    while (!(s & (RW_WWAIT | RW_WHELD)))
        if (rw_cas(l, &s, s + 1))
            return 0;
    return EBUSY;
}

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t* rw)
{
    if (!rw)
        return EINVAL;
    nx_rw* l = (nx_rw*)rw;
    int spins = 0;
    for (;;)
    {
        uint32_t s = rw_load(l);
        if ((s & ~RW_WWAIT) == 0)
        {
            // free (perhaps with writers waiting, us among them): take it; another waiting writer sets WWAIT again
            if (rw_cas(l, &s, RW_WHELD))
                return 0;
            continue;
        }
        if (!(s & RW_WWAIT))
        {
            // hold new readers back while the present ones drain
            rw_cas(l, &s, s | RW_WWAIT);
            continue;
        }
        rw_pause(l, s, &spins);
    }
}

int __wrap_pthread_rwlock_trywrlock(pthread_rwlock_t* rw)
{
    if (!rw)
        return EINVAL;
    nx_rw* l = (nx_rw*)rw;
    uint32_t s = rw_load(l);
    while ((s & ~RW_WWAIT) == 0)
        if (rw_cas(l, &s, RW_WHELD))
            return 0;
    return EBUSY;
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t* rw)
{
    if (!rw)
        return EINVAL;
    nx_rw* l = (nx_rw*)rw;
    if (__atomic_load_n(&l->state, __ATOMIC_RELAXED) & RW_WHELD)
        __atomic_fetch_and(&l->state, ~RW_WHELD, __ATOMIC_SEQ_CST);
    else
        __atomic_fetch_sub(&l->state, 1, __ATOMIC_SEQ_CST);
    rw_wake(l);
    return 0;
}

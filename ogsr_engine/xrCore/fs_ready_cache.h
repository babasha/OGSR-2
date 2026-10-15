#pragma once

// FSReadyCache: files already read (and decompressed) by a background thread,
// waiting for the r_open that is about to ask for them.
//
// Why: on the Switch ~40% of a gameplay spawn is reading the object's files from
// the SD -- model, textures, .omf (v159/v160 [spawn prof]: model.read + tex.read
// + anim.read ~ 35-40 ms of a 115 ms soldier), all on the second thread while the
// main thread waits for it. The objects about to come online are known well in
// advance (ALife switches them at a fixed distance), so a worker reads their
// files first (xrGame, SpawnPrefetch) and CLocatorAPI::r_open hands the ready
// reader over instead of reading again.
//
// Ownership moves with Take(): the caller r_close()s it like any other reader.
// Bounded by a byte budget; the oldest unclaimed file is dropped first (objects
// that never came online). Header-only: the engine links statically, so the
// inline function's static is one object across modules.
#include <algorithm>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace FSReadyCache
{
struct Entry
{
    IReader* r;
    size_t bytes;
    u64 seq;
};

struct State
{
    std::mutex mx;
    std::unordered_map<std::string, Entry> files;
    std::atomic<u32> count{0};   // r_open's cheap "anything here at all?" test
    // 📏12-10-2026 Switch: files a prefetcher is reading right now. A spawn that wanted the same file read it a second
    // time in parallel -- two 18 MB reads of barman_animation.omf off the SD, 694 ms instead of ~390. It now waits for
    // the read under way (TakeOrWait) instead.
    std::unordered_set<std::string> inflight;
    std::atomic<u32> inflightCount{0};
    std::condition_variable cv;
    size_t bytes = 0;
    size_t budget = 0;
    size_t loadBudget = 0;   // the load prefetcher's (fs_load_prefetch.h) while a level loads; the larger one rules
    u64 seq = 0;
    // stats since the last Stats() call
    u32 hits = 0, put = 0, evicted = 0;
    size_t hitBytes = 0, evictedBytes = 0;
};

inline State& st()
{
    static State s;
    return s;
}

// Lower case, one kind of slash: the worker and r_open build the same path by
// different routes (update_path vs a name the caller resolved itself).
inline std::string Key(LPCSTR path)
{
    std::string k = path;
    for (char& c : k)
    {
        if (c == '/')
            c = '\\';
        else if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    }
    return k;
}

inline void SetBudget(size_t bytes)
{
    std::lock_guard<std::mutex> g(st().mx);
    st().budget = bytes;
}

inline void SetLoadBudget(size_t bytes)
{
    std::lock_guard<std::mutex> g(st().mx);
    st().loadBudget = bytes;
}

inline size_t Cap()
{
    std::lock_guard<std::mutex> g(st().mx);
    return std::max(st().budget, st().loadBudget);
}

inline size_t Bytes()
{
    std::lock_guard<std::mutex> g(st().mx);
    return st().bytes;
}

inline bool Has(const std::string& key)
{
    std::lock_guard<std::mutex> g(st().mx);
    return st().files.count(key) != 0;
}

// Takes ownership of r. Returns false (and closes r) when it does not fit or is already there.
inline bool Put(const std::string& key, IReader* r)
{
    State& s = st();
    const size_t sz = size_t(r->length());
    xr_vector<IReader*> drop;
    bool ok = true;
    {
        std::lock_guard<std::mutex> g(s.mx);
        const size_t cap = std::max(s.budget, s.loadBudget);
        if (sz > cap || s.files.count(key))
            ok = false;
        else
        {
            while (s.bytes + sz > cap && !s.files.empty())
            {
                auto oldest = s.files.begin();
                for (auto it = s.files.begin(); it != s.files.end(); ++it)
                    if (it->second.seq < oldest->second.seq)
                        oldest = it;
                s.bytes -= oldest->second.bytes;
                ++s.evicted;
                s.evictedBytes += oldest->second.bytes;
                drop.push_back(oldest->second.r);
                s.files.erase(oldest);
            }
            s.files.emplace(key, Entry{r, sz, ++s.seq});
            s.bytes += sz;
            ++s.put;
            s.count.store(u32(s.files.size()), std::memory_order_relaxed);
        }
    }
    for (IReader* d : drop)
        FS.r_close(d);
    if (!ok)
        FS.r_close(r);
    return ok;
}

inline IReader* Take(const std::string& key)
{
    State& s = st();
    std::lock_guard<std::mutex> g(s.mx);
    auto it = s.files.find(key);
    if (it == s.files.end())
        return nullptr;
    IReader* r = it->second.r;
    s.bytes -= it->second.bytes;
    ++s.hits;
    s.hitBytes += it->second.bytes;
    s.files.erase(it);
    s.count.store(u32(s.files.size()), std::memory_order_relaxed);
    r->seek(0);
    return r;
}

// The prefetcher's own thread: its r_open must never wait for its own read.
inline thread_local bool t_prefetcher = false;

// Prefetcher: claim the read of key. False when it is cached already or someone else is reading it.
inline bool BeginRead(const std::string& key)
{
    State& s = st();
    std::lock_guard<std::mutex> g(s.mx);
    if (s.files.count(key) || s.inflight.count(key))
        return false;
    s.inflight.insert(key);
    s.inflightCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Prefetcher: the read of key is over (Put done, or failed).
inline void EndRead(const std::string& key)
{
    State& s = st();
    {
        std::lock_guard<std::mutex> g(s.mx);
        if (s.inflight.erase(key))
            s.inflightCount.fetch_sub(1, std::memory_order_relaxed);
    }
    s.cv.notify_all();
}

// r_open: the ready reader, or -- when a prefetcher is reading it right now -- wait for that read (bounded), then take
// it. nullptr: read it yourself.
inline IReader* TakeOrWait(const std::string& key, u32 max_wait_ms)
{
    State& s = st();
    std::unique_lock<std::mutex> l(s.mx);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
    for (;;)
    {
        auto it = s.files.find(key);
        if (it != s.files.end())
        {
            IReader* r = it->second.r;
            s.bytes -= it->second.bytes;
            ++s.hits;
            s.hitBytes += it->second.bytes;
            s.files.erase(it);
            s.count.store(u32(s.files.size()), std::memory_order_relaxed);
            r->seek(0);
            return r;
        }
        if (t_prefetcher || !s.inflight.count(key))
            return nullptr;
        if (s.cv.wait_until(l, until) == std::cv_status::timeout)
            return nullptr;
    }
}

// Drops one unclaimed file (not counted as evicted). False if it is not there (claimed already).
inline bool Drop(const std::string& key)
{
    IReader* r = nullptr;
    {
        std::lock_guard<std::mutex> g(st().mx);
        auto it = st().files.find(key);
        if (it == st().files.end())
            return false;
        r = it->second.r;
        st().bytes -= it->second.bytes;
        st().files.erase(it);
        st().count.store(u32(st().files.size()), std::memory_order_relaxed);
    }
    FS.r_close(r);
    return true;
}

inline void Clear()
{
    xr_vector<IReader*> drop;
    {
        std::lock_guard<std::mutex> g(st().mx);
        for (auto& kv : st().files)
            drop.push_back(kv.second.r);
        st().files.clear();
        st().bytes = 0;
        st().count.store(0, std::memory_order_relaxed);
    }
    for (IReader* d : drop)
        FS.r_close(d);
}

struct Snapshot
{
    u32 files, hits, put, evicted;
    size_t bytes, hitBytes, evictedBytes;
};

inline Snapshot Stats(bool reset)
{
    State& s = st();
    std::lock_guard<std::mutex> g(s.mx);
    Snapshot r{u32(s.files.size()), s.hits, s.put, s.evicted, s.bytes, s.hitBytes, s.evictedBytes};
    if (reset)
        s.hits = s.put = s.evicted = 0, s.hitBytes = s.evictedBytes = 0;
    return r;
}
} // namespace FSReadyCache

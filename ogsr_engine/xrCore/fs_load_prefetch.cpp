#include "stdafx.h"

#include "fs_load_prefetch.h"
#include "fs_ready_cache.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef __SWITCH__
// libnx (no switch.h here)
extern "C" u32 svcSetThreadCoreMask(u32 thread, s32 preferred_core, u32 affinity_mask);
extern "C" u32 svcGetCurrentProcessorNumber(void);
extern "C" u32 svcGetInfo(u64* out, u32 id0, u32 handle, u64 id1);
#endif

namespace LoadPrefetch
{
#ifdef __SWITCH__
int g_mb = 160;
#else
int g_mb = 0;   // a PC load is CPU-bound, not waiting on the disk
#endif
int g_early = 1;
int g_core = 2;

double ThreadCpuMs()
{
#ifdef __SWITCH__
    u64 ticks = 0;   // InfoType_ThreadTickCount (25, fw 13+), all cores; 19.2 MHz system ticks
    if (svcGetInfo(&ticks, 25, 0xFFFF8000u /*CUR_THREAD_HANDLE*/, u64(-1)) != 0)
        return -1.0;
    return double(ticks) / 19200.0;
#else
    return -1.0;
#endif
}

namespace
{
using clock = std::chrono::steady_clock;

// An entry the load passed this many trace positions ago without taking it will
// not be taken (this load's objects differ from the recorded one's): dropped when
// the budget is full, so a stale guess cannot stall the reader.
constexpr u32 kStaleLag = 64;
// The load's position moves by at most this many entries per open.
constexpr u32 kMaxJump = 256;
// No End (a load that failed): the reader gives up on its own.
constexpr int kMaxSessionS = 180;

constexpr const char* kLastFile = "load_trace_last.txt";

thread_local bool t_worker = false;

struct Own
{
    u32 idx;
    std::string key;
    size_t bytes;
};

struct State
{
    std::atomic<bool> on{false};   // a session (replay and/or recording) is open
    std::mutex mx;                 // trace/index/rec/recSeen
    bool recording = false;
    xr_string replayLevel, recLevel;
    xr_vector<xr_string> trace;   // full paths to replay, in order
    xr_vector<std::string> traceKeys;
    std::unordered_map<std::string, u32> index;   // key -> first position in trace
    xr_vector<xr_string> rec;                     // this load's opens, in order
    std::unordered_set<std::string> recSeen;
    std::atomic<u32> consumerPos{0};   // one past the furthest trace entry the load has opened

    std::thread th;
    std::atomic<bool> stop{false};
    std::mutex ownMx;
    std::deque<Own> own;   // what the reader put, in put order

    clock::time_point t0;
    u32 pinRc = 0, workerCore = u32(-1), mainCore = u32(-1);
    std::atomic<u32> put{0}, skippedBehind{0}, skippedBig{0}, stale{0}, missing{0};
    std::atomic<u64> putBytes{0}, staleBytes{0}, readUs{0}, waitUs{0};
    std::atomic<u32> doneAtMs{0};
    double workerCpuMs = 0;
};

State& S()
{
    static State s;
    return s;
}

u32 ms_since(clock::time_point t) { return u32(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t).count()); }

void trace_file_name(string_path& out, LPCSTR level)
{
    string_path nm;
    xr_sprintf(nm, "load_trace_%s.txt", level);
    FS.update_path(out, "$app_data_root$", nm);
}

// Reads one line per file after the "level <name>" header.
bool read_trace(LPCSTR level, xr_vector<xr_string>& out)
{
    string_path fn;
    trace_file_name(fn, level);
    IReader* R = FS.r_open(fn);
    if (!R)
        return false;
    string_path line;
    bool header = true;
    while (!R->eof())
    {
        R->r_string(line, sizeof(line));
        if (header)
        {
            header = false;
            continue;
        }
        if (line[0])
            out.emplace_back(line);
    }
    FS.r_close(R);
    return !out.empty();
}

bool read_last_level(xr_string& level)
{
    string_path fn;
    FS.update_path(fn, "$app_data_root$", kLastFile);
    IReader* R = FS.r_open(fn);
    if (!R)
        return false;
    string_path line{};
    if (!R->eof())
        R->r_string(line, sizeof(line));
    FS.r_close(R);
    level = line;
    return !level.empty();
}

// Drops the reader's oldest unclaimed file once the load is kStaleLag entries past it.
bool drop_one_stale()
{
    State& s = S();
    const u32 pos = s.consumerPos.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(s.ownMx);
    while (!s.own.empty())
    {
        Own& o = s.own.front();
        if (!FSReadyCache::Has(o.key))
        {
            s.own.pop_front();   // claimed
            continue;
        }
        if (o.idx + kStaleLag >= pos)
            return false;
        if (FSReadyCache::Drop(o.key))
        {
            s.stale.fetch_add(1, std::memory_order_relaxed);
            s.staleBytes.fetch_add(o.bytes, std::memory_order_relaxed);
        }
        s.own.pop_front();
        return true;
    }
    return false;
}

void worker_loop()
{
    t_worker = true;
    State& s = S();
#ifdef __SWITCH__
    // Core 2: the main thread loads on core 0 and the render thread owns core 1.
    // The archive decompression of what this thread reads then runs beside the load.
    // (The ALife parse still runs 1.2-2x slower while this reads beside it -- v200,
    // reader on core 2, main on core 0: not a shared core. 0x3C as a lower priority
    // is refused, 0xE001: 0x3B is the lowest an application thread gets.)
    s.pinRc = svcSetThreadCoreMask(0xFFFF8000u /*CUR_THREAD_HANDLE*/, g_core, 1u << g_core);
    s.workerCore = svcGetCurrentProcessorNumber();
#endif
    const u32 n = u32(s.trace.size());
    for (u32 i = 0; i < n && !s.stop.load(std::memory_order_relaxed); ++i)
    {
        if (i < s.consumerPos.load(std::memory_order_relaxed))
        {
            s.skippedBehind.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const xr_string& full = s.trace[i];
        const std::string& key = s.traceKeys[i];
        const CLocatorAPI::file* f = FS.exist(full.c_str());
        if (!f)
        {
            s.missing.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const size_t sz = f->size_real;
        const size_t cap = FSReadyCache::Cap();
        if (sz > cap / 3)   // level.cform-sized files would hold the whole budget
        {
            s.skippedBig.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (FSReadyCache::Has(key))
            continue;

        const auto tw = clock::now();
        bool room = false;
        while (!s.stop.load(std::memory_order_relaxed))
        {
            if (FSReadyCache::Bytes() + sz <= FSReadyCache::Cap())
            {
                room = true;
                break;
            }
            if (drop_one_stale())
                continue;
            if (ms_since(s.t0) > u32(kMaxSessionS) * 1000)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        s.waitUs.fetch_add(u64(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - tw).count()), std::memory_order_relaxed);
        if (!room)
            break;
        // The load may have reached it while this thread waited.
        if (i < s.consumerPos.load(std::memory_order_relaxed))
        {
            s.skippedBehind.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const auto tr = clock::now();
        IReader* R = FS.r_open(full.c_str());
        if (!R)
            continue;
        s.readUs.fetch_add(u64(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - tr).count()), std::memory_order_relaxed);
        const size_t len = size_t(R->length());
        {
            std::lock_guard<std::mutex> g(s.ownMx);
            s.own.push_back(Own{i, key, len});
        }
        if (FSReadyCache::Put(key, R))
        {
            s.put.fetch_add(1, std::memory_order_relaxed);
            s.putBytes.fetch_add(len, std::memory_order_relaxed);
        }
    }
    s.doneAtMs.store(ms_since(s.t0), std::memory_order_relaxed);
    s.workerCpuMs = ThreadCpuMs();
}

// Stops the reader and drops what it put that nobody claimed. Returns the unclaimed count/bytes.
void stop_replay(u32& unusedN, u64& unusedB)
{
    State& s = S();
    s.stop.store(true, std::memory_order_relaxed);
    if (s.th.joinable())
        s.th.join();
    s.stop.store(false, std::memory_order_relaxed);
    unusedN = 0;
    unusedB = 0;
    {
        std::lock_guard<std::mutex> g(s.ownMx);
        for (const Own& o : s.own)
            if (FSReadyCache::Drop(o.key))
                ++unusedN, unusedB += o.bytes;
        s.own.clear();
    }
    FSReadyCache::SetLoadBudget(0);
}

void reset_stats()
{
    State& s = S();
    s.put = 0, s.skippedBehind = 0, s.skippedBig = 0, s.stale = 0, s.missing = 0;
    s.putBytes = 0, s.staleBytes = 0, s.readUs = 0, s.waitUs = 0, s.doneAtMs = 0;
    s.consumerPos = 0;
}

// Starts the reader on `level`'s trace. False if there is none.
bool start_replay(LPCSTR level)
{
    State& s = S();
    xr_vector<xr_string> trace;
    if (!read_trace(level, trace))
        return false;
    {
        std::lock_guard<std::mutex> g(s.mx);
        s.replayLevel = level;
        s.trace = std::move(trace);
        s.traceKeys.clear();
        s.index.clear();
        for (u32 i = 0; i < s.trace.size(); ++i)
        {
            s.traceKeys.push_back(FSReadyCache::Key(s.trace[i].c_str()));
            s.index.emplace(s.traceKeys.back(), i);
        }
    }
    reset_stats();
    s.t0 = clock::now();
#ifdef __SWITCH__
    s.mainCore = svcGetCurrentProcessorNumber();
#endif
    FSReadyCache::SetLoadBudget(size_t(g_mb) << 20);
    s.th = std::thread(worker_loop);
    return true;
}

void clear_replay()
{
    State& s = S();
    std::lock_guard<std::mutex> g(s.mx);
    s.replayLevel.clear();
    s.trace.clear();
    s.traceKeys.clear();
    s.index.clear();
}
} // namespace

bool Wants() { return S().on.load(std::memory_order_relaxed) && !t_worker; }

void MarkReaderThread() { t_worker = true; }

void NoteOpen(LPCSTR full, const std::string& key)
{
    State& s = S();
    std::lock_guard<std::mutex> g(s.mx);
    // Before the level is known (the ALife phase) the opens are not the recorded
    // ones -- a config or texture both read there and late in the trace threw the
    // position 800 entries ahead in v199, and the reader skipped them all.
    if (!s.recording)
        return;
    if (s.recSeen.insert(key).second)
        s.rec.emplace_back(full);
    auto it = s.index.find(key);
    if (it == s.index.end())
        return;
    const u32 next = it->second + 1;
    u32 cur = s.consumerPos.load(std::memory_order_relaxed);
    // One open far ahead of the rest is a file read early this time, not progress.
    if (next > cur + kMaxJump)
        return;
    while (next > cur && !s.consumerPos.compare_exchange_weak(cur, next, std::memory_order_relaxed))
        ;
}

void Begin()
{
    State& s = S();
    if (s.on.load())
    {
        u32 n;
        u64 b;
        stop_replay(n, b);
        clear_replay();
    }
    if (!g_mb)
        return;
    {
        std::lock_guard<std::mutex> g(s.mx);
        s.recording = false;
        s.rec.clear();
        s.recSeen.clear();
        s.recLevel.clear();
    }
    xr_string last;
    if (g_early && read_last_level(last) && start_replay(last.c_str()))
        Msg("[load prefetch] replaying '%s' (the last loaded level): %u files, %d MB ahead", last.c_str(), u32(s.trace.size()), g_mb);
    s.on.store(true);
}

void LevelKnown(LPCSTR level)
{
    State& s = S();
    if (!g_mb || !level || !level[0])
        return;
    if (s.on.load() && s.replayLevel.size() && s.replayLevel != level)
    {
        u32 n;
        u64 b;
        stop_replay(n, b);
        Msg("[load prefetch] guessed '%s', the level is '%s': %u files (%.1f MB) dropped", s.replayLevel.c_str(), level, n, double(b) / 1048576.0);
        clear_replay();
    }
    if (s.replayLevel.empty())
    {
        if (start_replay(level))
            Msg("[load prefetch] replaying '%s': %u files, %d MB ahead", level, u32(s.trace.size()), g_mb);
        else
            Msg("[load prefetch] no trace for '%s' yet: recording this load", level);
    }
    {
        std::lock_guard<std::mutex> g(s.mx);
        s.recording = true;
        s.recLevel = level;
        s.rec.clear();
        s.recSeen.clear();
    }
    s.on.store(true);
}

void End()
{
    State& s = S();
    if (!s.on.load())
        return;
    s.on.store(false);
    u32 unusedN = 0;
    u64 unusedB = 0;
    const bool replayed = !s.replayLevel.empty();
    stop_replay(unusedN, unusedB);

    if (replayed)
    {
        const double MB = 1.0 / 1048576.0;
        const u32 put = s.put, stale = s.stale;
        const u64 putB = s.putBytes, staleB = s.staleBytes;
        const u32 claimed = put - std::min(put, stale + unusedN);
        const u64 claimedB = putB - std::min(putB, staleB + unusedB);
        Msg("[load prefetch] '%s' %u ms: read %u of %u files (%.1f MB) in %.0f ms, waited for room %.0f ms, %s | claimed %u (%.1f MB) | stale %u (%.1f MB) | unclaimed at the end %u (%.1f MB) | load was ahead %u, too big %u, missing %u | reader on core %d (pin rc 0x%X, %.0f ms of CPU), main on core %d",
            s.replayLevel.c_str(), ms_since(s.t0), put, u32(s.trace.size()), putB * MB, double(s.readUs) / 1000.0, double(s.waitUs) / 1000.0,
            s.doneAtMs ? make_string("list done at %u ms", u32(s.doneAtMs)).c_str() : "list not done", claimed, claimedB * MB, stale, staleB * MB, unusedN,
            unusedB * MB, u32(s.skippedBehind), u32(s.skippedBig), u32(s.missing), int(s.workerCore), s.pinRc, s.workerCpuMs, int(s.mainCore));
    }
    clear_replay();

    xr_vector<xr_string> rec;
    xr_string level;
    {
        std::lock_guard<std::mutex> g(s.mx);
        rec.swap(s.rec);
        level = s.recLevel;
        s.recording = false;
        s.recSeen.clear();
        s.recLevel.clear();
    }
    if (level.empty() || rec.empty())
        return;
    string_path fn;
    trace_file_name(fn, level.c_str());
    if (IWriter* W = FS.w_open(fn))
    {
        string_path hdr;
        xr_sprintf(hdr, "level %s", level.c_str());
        W->w_string(hdr);
        for (const xr_string& f : rec)
            if (FS.exist(f.c_str()))   // not the probes for files that are not there (a missing _bump.dds)
                W->w_string(f.c_str());
        FS.w_close(W);
    }
    FS.update_path(fn, "$app_data_root$", kLastFile);
    if (IWriter* W = FS.w_open(fn))
    {
        W->w_string(level.c_str());
        FS.w_close(W);
    }
    Msg("[load prefetch] recorded '%s': %u files opened by the load", level.c_str(), u32(rec.size()));
}
} // namespace LoadPrefetch

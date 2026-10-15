#pragma once

// LoadPrefetch: reads the files of a level load ahead of the load, in the order
// the previous load of that level opened them.
//
// Why: on the Switch a Kordon save load (v198) reads ~11 s from the SD after the
// ALife phase, while the ALife phase itself (6.5 s) is CPU-bound with the SD
// idle (0.5 s of reads), and the spawn burst is two thirds CPU. Which files the
// load opens and in which order barely changes between loads of one level, so
// every load records the r_open order (load_trace_<level>.txt in
// $app_data_root$) and the next one replays it on a worker thread into
// FSReadyCache (fs_ready_cache.h), staying at most load_prefetch_mb ahead of the
// load; CLocatorAPI::r_open takes the ready reader instead of reading again.
//
// Begin   -- ALife starts loading a save: replay the level of the last load (the
//            save's level is not known until its objects are parsed).
// LevelKnown -- the client knows the level (CLevel::net_start_client3): a wrong
//            guess is dropped and that level's trace replayed; recording starts.
// End     -- the precache is over: stop, report, write the trace.
namespace LoadPrefetch
{
XRCORE_API extern int g_mb;   // load_prefetch_mb: MB held ahead of the load, 0 = off
XRCORE_API extern int g_early;   // load_prefetch_early: 1 = start in the ALife phase on a guess, 0 = once the level is known
XRCORE_API extern int g_core;    // load_prefetch_core: the core the reader is pinned to (Switch)

// CPU time of the calling thread, ms (Switch: kernel tick count; -1 elsewhere). Wall
// minus this = the time the thread waited (locks, I/O, preemption).
XRCORE_API double ThreadCpuMs();

XRCORE_API void Begin();
XRCORE_API void LevelKnown(LPCSTR level);
XRCORE_API void End();
// A thread that reads ahead on its own guesses (the spawn prefetcher): its opens are not the load's.
XRCORE_API void MarkReaderThread();

// CLocatorAPI::r_open: is this open part of a load being recorded/replayed (cheap), then note it.
bool Wants();
void NoteOpen(LPCSTR full, const std::string& key);
} // namespace LoadPrefetch

#pragma once
// ПОТОК РЕНДЕРА -- РАЗМЕТКА ПОТОКОВ (RENDER_THREAD_PLAN).
//
// Две роли: MAIN (логика + extract) и RENDER (запись команд, submit, present).
// ВСЕ кадры (игра, меню, загрузка, прекэш, редактор) записывает выделенный поток
// (RT::SetRole(RT::Render), device.cpp). Главный поток рендер не записывает никогда.
//
// RT_ASSERT_* НИКОГДА не падают: одна строка `![rt] wrong thread: <файл:строка>` на место за
// прогон. Такая строка в логе = код рендера вызван не из того потока.
#include <atomic>
#include "log.h"

namespace RT
{
enum Role : int { None = 0, Main = 1, Render = 2 };

inline thread_local int tl_role = None;

// TWO-SLOT PIPELINE. Everything the extract side (main thread) produces for a frame lives in a slot, and a frame owns
// slot (seq & 1) from its Device.Begin until its End_Render job has run: extract of frame N+1 writes the other slot while
// the render thread still records frame N from its own. Slot() answers "which slot am I" by ROLE:
//   * the render thread  : the slot of the job it is running (SetRecordSlot at job start);
//   * everyone else      : the slot the main thread currently extracts into (g_extSlot). Pool workers that run inside
//                          extract (bones_mt, ...) and the frame logic between frames land here too.
constexpr u32 kSlots = 2;
inline std::atomic<u32> g_extSlot{0};
inline thread_local u32 tl_recSlot = 0;

inline void SetRole(Role r) { tl_role = r; }
inline u32  Slot() { return tl_role == Render ? tl_recSlot : g_extSlot.load(std::memory_order_relaxed); }
inline void SetRecordSlot(u32 s) { tl_recSlot = s % kSlots; }
inline void SetExtractSlot(u64 s) { g_extSlot.store(u32(s % kSlots), std::memory_order_relaxed); }

// Frame sequence: Device.Begin counts every frame it lets through (menu, load screen and precache frames included -- each
// ends with exactly one End_Render job). A frame owns slot seq % kSlots and the renderer's ring slot seq % FRAMES_IN_FLIGHT.
// 64-bit: slot (seq % 2) and ring (seq % 3) must not skip a beat at a 2^32 wrap (2^32 is not a multiple of 3). The in-flight
// accounting (FrameSeq() - done) uses the low 32 bits, which stays correct modulo 2^32.
inline std::atomic<u64> g_frameSeq64{0};
inline u64  BeginFrameSeq() { const u64 s = g_frameSeq64.fetch_add(1, std::memory_order_relaxed) + 1; SetExtractSlot(s); return s; }
inline u32  FrameSeq() { return u32(g_frameSeq64.load(std::memory_order_relaxed)); }
inline u64  FrameSeq64() { return g_frameSeq64.load(std::memory_order_relaxed); }

// One instance per slot, picked by role. Objects are built in place (never copied or assigned between slots), so a
// type that points into itself (EnvSnapshot) is fine.
template <class T>
struct PerSlot
{
    T v[kSlots];
    T& cur() { return v[Slot()]; }
    T& at(u32 s) { return v[s % kSlots]; }
};
inline bool IsMainRole() { return tl_role == Main; }
inline bool IsRenderThread() { return tl_role == Render; }

inline void WrongThread(std::atomic<bool>& once, const char* file, int line, const char* want)
{
    if (!once.exchange(true, std::memory_order_relaxed))
        Msg("![rt] wrong thread: %s:%d (want %s)", file, line, want);
}
} // namespace RT

#define RT_ASSERT_RENDER() \
    do { if (!RT::IsRenderThread()) { static std::atomic<bool> _rt_once{false}; RT::WrongThread(_rt_once, __FILE__, __LINE__, "render"); } } while (0)
#define RT_ASSERT_MAIN() \
    do { if (!RT::IsMainRole()) { static std::atomic<bool> _rt_once{false}; RT::WrongThread(_rt_once, __FILE__, __LINE__, "main"); } } while (0)

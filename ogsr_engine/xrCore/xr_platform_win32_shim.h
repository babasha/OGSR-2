#pragma once
// ---------------------------------------------------------------------------
// ⚠⚠⚠19-09: ИЗМЕРИТЕЛЬНАЯ ЗАГЛУШКА, НЕ РЕАЛЬНЫЙ ПОРТ.
//
// Единственная цель -- узнать РЕАЛЬНОЕ число Windows-имён, которые xrCore.h
// и его собратья используют НАПРЯМУЮ (не через уже сделанные швы xr_platform_*),
// добавляя сюда символ за символом, пока `xrCore.h` не распарсится реальным
// clang/gcc. Список типов/функций ниже -- это и есть измерение: каждая
// строка -- то, что реальный порт обязан типизировать по-настоящему (сейчас
// это заглушки без функциональности, просто чтобы компилятор не встал).
// См. platform-seam-step5-windows-h-scope.md за итогами и честной оценкой.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <strings.h> // strcasecmp/strncasecmp -- POSIX аналог _stricmp/_strnicmp
#include <dirent.h>  // opendir/readdir/closedir -- POSIX-аналог _findfirst/_findnext
#include <fnmatch.h> // shell-style glob для _findfirst/_findnext (см. ниже про "*.*")
#include <sys/stat.h>
#ifdef __SWITCH__
#include "xr_platform_nx_mman.h" // Horizon: no <sys/mman.h>; mmap emulated by a copy
#else
#include <sys/mman.h> // mmap/munmap -- POSIX-аналог MapViewOfFile/UnmapViewOfFile
#endif
#include <fcntl.h>    // open/O_RDONLY -- POSIX-аналог CreateFile
#include <cctype>     // tolower -- _strlwr
#ifdef __SWITCH__
#include "xr_platform_nx_compat.h" // newlib gaps: ctype macros (_P/_X/_N...), gettid, thread names
#endif
#include <mutex>
#include <unordered_map>
#include <cerrno>

// ---------------------------------------------------------------------------
// MSVC secure CRT (strcpy_s/strcat_s/sprintf_s/sscanf_s) -- используются
// пачками по всему движку (71/25/2 файла грепом 19-09), в том числе через
// xr_strcpy/xr_strcat/xr_sprintf (_std_extensions.h) и xr_strconcat, который
// подключается ЛЮБЫМ файлом через vector.h -- без этого блока НИЧЕГО не
// парсится, не только xrCore.cpp. НАСТОЯЩИЕ функциональные реализации
// (bounds-checked), не заглушки -- как реальный secure CRT.
// ---------------------------------------------------------------------------
using errno_t = int;

inline errno_t strcpy_s(char* dest, size_t destsz, const char* src)
{
    if (!dest || !src || destsz == 0) return 22; // EINVAL
    const size_t n = strlen(src);
    if (n >= destsz) { dest[0] = 0; return 34; } // ERANGE
    memcpy(dest, src, n + 1);
    return 0;
}
template <size_t N>
inline errno_t strcpy_s(char (&dest)[N], const char* src) { return strcpy_s(dest, N, src); }

inline errno_t strcat_s(char* dest, size_t destsz, const char* src)
{
    if (!dest || !src) return 22;
    const size_t dl = strlen(dest), sl = strlen(src);
    if (dl + sl + 1 > destsz) return 34;
    memcpy(dest + dl, src, sl + 1);
    return 0;
}
template <size_t N>
inline errno_t strcat_s(char (&dest)[N], const char* src) { return strcat_s(dest, N, src); }

inline errno_t strncpy_s(char* dest, size_t destsz, const char* src, size_t count)
{
    if (!dest || !src || destsz == 0) return 22; // EINVAL
    size_t n = strlen(src);
    const bool truncate = (count == (size_t)-1); // _TRUNCATE
    if (!truncate && n > count) n = count;
    if (n >= destsz)
    {
        if (!truncate) { dest[0] = 0; return 34; } // ERANGE
        n = destsz - 1; // _TRUNCATE: copy what fits, cut the rest, no error
    }
    memcpy(dest, src, n);
    dest[n] = 0;
    return 0;
}
template <size_t N>
inline errno_t strncpy_s(char (&dest)[N], const char* src, size_t count) { return strncpy_s(dest, N, src, count); }

#include <alloca.h>
#define _alloca alloca

// ---------------------------------------------------------------------------------------------------------
// PATH SEPARATORS. The engine's paths are WINDOWS paths: archive entries, the FS tables and every
// FS.update_path() result use '\' ("gamedata\config\system.ltx", "_appdata_\logs\x.log"). POSIX and newlib
// need '/', and on the Switch's FAT32 a '\' is not even a legal name character, so an unconverted path fails
// outright. xr_ppath(p) is a temporary char[] copy of p with '\' -> '/', for every call that crosses into the
// OS (this file, fs_internal.h, log.cpp, VerifyPath). Found 30-09 when loose files in gamedata\ were silently
// never found (opendir("gamedata\\")) and the log landed in a file literally named "_appdata_\logs\...".
// ---------------------------------------------------------------------------------------------------------
struct xr_ppath
{
    char s[1300];
    explicit xr_ppath(const char* p)
    {
        size_t i = 0;
        for (; p && p[i] && i < sizeof(s) - 1; ++i)
            s[i] = p[i] == '\\' ? '/' : p[i];
        s[i] = 0;
    }
    operator const char*() const { return s; }
};

inline errno_t fopen_s(FILE** f, const char* path, const char* mode)
{
    *f = fopen(xr_ppath(path), mode);
    return *f ? 0 : errno;
}

// __int64 -- pre-<cstdint> MSVC builtin type keyword, used as a plain type
// specifier (e.g. `(__int64)offset`) wherever this codebase hasn't already
// switched to int64_t.
#define __int64 int64_t

// MSVC CRT _strlwr(char*) -- in-place lowercase, returns the same pointer.
// Distinct from xr_string_utils::_strlwr (xrstring.h), which this codebase
// also has and which operates on shared_str/xr_string -- both names are
// used, on different argument types, so both need to exist.
inline char* _strlwr(char* s) { for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p); return s; }
inline char* _strupr(char* s) { for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p); return s; }

inline int sprintf_s(char* dest, size_t destsz, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(dest, destsz, fmt, ap);
    va_end(ap);
    return n;
}
inline int vsprintf_s(char* dest, size_t destsz, const char* fmt, va_list ap) { return vsnprintf(dest, destsz, fmt, ap); }
template <size_t N>
inline int vsprintf_s(char (&dest)[N], const char* fmt, va_list ap) { return vsnprintf(dest, N, fmt, ap); }
template <size_t N>
inline int sprintf_s(char (&dest)[N], const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(dest, N, fmt, ap);
    va_end(ap);
    return n;
}

// ⚠sscanf_s различается от sscanf ТОЛЬКО для %s/%c/%[ (требуют доп. аргумент
// с размером буфера сразу после). Грепом 19-09: оба вызывающих места по всей
// кодовой базе используют лишь числовые спецификаторы (%f/%d/%u) -- для них
// sscanf_s и sscanf побайтово идентичны. Если появится %s-вызов, этот alias
// молча съест лишний size-аргумент (sscanf variadic) -- не наш случай сейчас.
#define sscanf_s sscanf

// MSVC CRT case-insensitive сравнение строк -- POSIX-имена другие. Реальных
// вызовов по кодовой базе 37 файлов (грепом), все берут это отсюда бесплатно
// раз этот заголовок и так подключается везде вместо <windows.h>.
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strcmpi strcasecmp // MSVC alias for _stricmp, same POSIX mapping
// _TRUNCATE -- strncpy_s/strcat_s's "truncate, don't fail" sentinel size.
#define _TRUNCATE ((size_t)-1)
#include <ctime>
#define _tzset tzset // MSVC CRT name vs POSIX name, same semantics

// _strdate/_strtime (MSVC CRT, "MM/DD/YY"/"HH:MM:SS") -- POSIX не несёт
// прямых эквивалентов, strftime даёт то же самое побайтово.
inline char* _strdate(char* buf) { time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv); strftime(buf, 9, "%m/%d/%y", &tmv); return buf; }
inline char* _strtime(char* buf) { time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv); strftime(buf, 9, "%H:%M:%S", &tmv); return buf; }

// OutputDebugString -- на Windows идёт отдельному дебаггеру мимо stdout;
// POSIX-аналога нет, stderr -- разумная замена (видно и без дебаггера).
inline void OutputDebugString(const char* s) { fputs(s, stderr); }

// _Thrd_id() -- деталь реализации MSVC STL (C11 threads.h), протекающая в
// log.cpp через <thread>. gettid() (glibc >=2.30) -- настоящий, а не
// синтетический ID: то же семантическое значение (реальный OS thread id),
// что и оригинальный вызов на Windows.
inline unsigned _Thrd_id() { return (unsigned)gettid(); }

// MSVC <yvals_core.h>: "_CONSTEXPR23" is `constexpr` when the toolset's C++23
// support covers the feature at the call site, otherwise empty. libstdc++/
// libc++ headers we build against don't use this macro, so a plain
// `constexpr` is the correct portable reading, not a placeholder.
#define _CONSTEXPR23 constexpr

// _itoa(value, buf, radix) -- MSVC CRT, no direct POSIX equivalent. Только
// вызывающие места по кодовой базе передают radix=10/16, поэтому достаточно
// snprintf для 10 и ручной hex-цикл для остального.
inline char* _itoa(int value, char* buf, int radix)
{
    if (radix == 16) { snprintf(buf, 16, "%x", (unsigned)value); return buf; }
    snprintf(buf, 16, "%d", value);
    return buf;
}
#include <cmath>
#define _copysign copysign // MSVC CRT name vs C99 std name
#include <cstdio>
#define _vsnprintf vsnprintf
#define _snprintf snprintf

// __assume(cond) -- MSVC optimizer hint ("compiler, treat cond as always
// true"), used codebase-wide as `NODEFAULT` (xrDebug_macros.h) in release
// builds. __builtin_unreachable() is GCC/Clang's real equivalent for the
// __assume(0) case this codebase actually calls (grepped: no non-zero
// callers) -- the ternary form stays a true expression-hint for any future
// non-zero use instead of only handling the zero case.
#define __assume(cond) ((cond) ? (void)0 : __builtin_unreachable())

// __forceinline -- MSVC keyword used raw (not via the codebase's own ICF
// macro) in a few third-party-adjacent files. GCC/Clang have no such
// keyword; `inline __attribute__((always_inline))` is their equivalent
// strength-of-request, matching what xrCore.h's own ICF macro already does
// for non-MSVC.
#define __forceinline inline __attribute__((always_inline))

// YieldProcessor -- MSVC intrinsic for a spin-wait pause (PAUSE instruction
// on x86), used by CDB::query::syncronize()'s busy-wait. _mm_pause() is the
// exact same instruction via the intrinsic both compilers share, not an
// approximation.
// ARM64: YIELD is the PAUSE counterpart (a hint, no architectural effect beyond that).
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
inline void YieldProcessor() { _mm_pause(); }
#elif defined(__aarch64__)
inline void YieldProcessor() { __asm__ __volatile__("yield" ::: "memory"); }
#else
inline void YieldProcessor() { __asm__ __volatile__("" ::: "memory"); }
#endif

using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using DWORD64 = uint64_t;
using DWORDLONG = uint64_t;
using BOOL = int;
using LONG = int32_t;
using ULONG = uint32_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using LONG64 = int64_t;
using ULONG64 = uint64_t;
using HANDLE = void*;
using HWND = void*;
using HMODULE = void*;
using HINSTANCE = void*;
using HRESULT = int32_t;
using LPCSTR = const char*;
using LPSTR = char*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using LPVOID = void*;
using LPCVOID = const void*;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;
using UINT = unsigned int;
using UINT_PTR = uintptr_t;
using SIZE_T = size_t;
using WCHAR = wchar_t;

// ZeroMemory -- winbase.h aliases this straight to RtlZeroMemory, which is
// itself just memset(dst, 0, len); no Windows-specific semantics to port.
#define ZeroMemory(dst, len) memset((dst), 0, (len))

// WAVEFORMATEX (mmreg.h) -- xrSound's WAV/OGG loader path only ever reads/
// writes these 7 fields as a plain PCM format descriptor (never a real
// mmsystem.h/waveOut* API call, see xrSound stdafx.h), so the struct layout
// is copied field-for-field rather than pulling in all of mmsystem.h.
struct WAVEFORMATEX
{
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
};
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IEEE_FLOAT 3

// MSVC-only fixed-width integer literal suffixes (i8/ui8/i16/ui16/i32/ui32/
// i64/ui64, e.g. 0x80000000ui32) are NOT shimmable as C++11 user-defined
// literals: GCC accepts operator""ui32 as the fallback for an unrecognized
// suffix, but Clang's lexer hard-errors ("invalid suffix") on any
// digit-adjacent suffix starting with 'i' or 'u', before UDL lookup is even
// considered -- confirmed empirically, not a missing declaration. No
// single-header fix exists; each of the small number of call sites
// (COMMON_AI/ai_sounds.h, xrGame/{HudItem,PHShell,ui_rml_menu}.cpp) needs
// its literal rewritten to a standard suffix (u/ull/etc.) instead.

using LPBYTE = BYTE*;

// FAR -- 16-bit-Windows-era far-pointer qualifier, kept only as an empty
// macro on Win32 itself (winnt.h: #define FAR) for source compatibility with
// even older code; eax.h's non-INITGUID DEFINE_GUID branch still emits it.
#define FAR

// Sleep(ms) -- winbase.h. usleep() takes microseconds and is deprecated but
// still POSIX; nanosleep would be the modern replacement, but every real
// caller here passes a small millisecond count (OpenALDeviceList.cpp's
// device-enumeration retry loop), so the precision difference is moot.
inline void Sleep(DWORD ms) { usleep(ms * 1000); }

// _ReturnAddress()/RtlCaptureStackBackTrace -- Luabind's pcall.cpp profiling
// path (lua_vm_watch, `lua_time 2`). __builtin_return_address(0) is the
// GCC/Clang equivalent of the MSVC intrinsic (same one instruction on x86,
// a frame-pointer walk elsewhere -- either way, "address in the immediate
// caller"). RtlCaptureStackBackTrace's own real semantics: skip
// FramesToSkip frames (not counting the call to itself), then fill up to
// FramesToCapture addresses -- glibc's backtrace() already includes its own
// call frame as entry 0, so that one extra skip accounts for it.
#define _ReturnAddress() __builtin_return_address(0)
#ifdef __SWITCH__
#include "xr_platform_nx_backtrace.h" // Horizon: no backtrace(); capture yields no frames
#elif defined(__ANDROID__)
#include "xr_platform_android_backtrace.h" // bionic has backtrace() only from API 33
#else
#include <execinfo.h>
#endif
inline unsigned short RtlCaptureStackBackTrace(unsigned long framesToSkip, unsigned long framesToCapture, void** backTrace, unsigned long* backTraceHash)
{
    if (backTraceHash) *backTraceHash = 0;
    const unsigned long want = framesToSkip + framesToCapture + 1;
    void* buf[64];
    const unsigned long cap = want < 64 ? want : 64;
    const int got = backtrace(buf, (int)cap);
    unsigned long n = 0;
    for (unsigned long i = framesToSkip + 1; i < (unsigned long)got && n < framesToCapture; ++i)
        backTrace[n++] = buf[i];
    return (unsigned short)n;
}
// A named (not anonymous) struct: an unnamed struct type used across a TU
// boundary (e.g. a virtual method's parameter type declared in a header,
// defined in a .cpp -- xrSound's CSoundRender_Core::i_eax_set) has no
// linkage, which g++ rejects with -fpermissive even though the vtable
// itself doesn't care about the parameter type's name.
struct _GUID { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; };
using GUID = _GUID;
// Marks GUID as already declared for headers (e.g. eax.h's OPENAL branch)
// that guard their own "struct _GUID {...} GUID" with #ifndef GUID_DEFINED --
// their DEFINE_GUID macro only ever aggregate-initializes by position
// ({l, w1, w2, {b1..b8}}), never by field name; the field names follow guiddef.h (xrGame's generate_id() formats Data1..Data4).
#define GUID_DEFINED
#define TRUE 1
#define FALSE 0
#define WINAPI
#define WINAPIV
#define CALLBACK
#define STDMETHODCALLTYPE
#define __stdcall
#define __cdecl
#define _cdecl
#define _stdcall
// VK_*/WM_* -- winuser.h, transcribed the same way xr_platform_keys.h's
// DIK_* are (real values copied from Windows Kits' WinUser.h, not from
// memory), for the small set Device_wndproc.cpp/Xr_input.cpp use by name.
// Lives in the general shim (reached by every TU via xrCore.h) rather than
// the DirectInput-specific xr_platform_keys.h, since e.g. Device_wndproc.cpp
// needs WM_SYSKEYDOWN without pulling in xr_input.h at all.
#define VK_TAB              0x09
#define VK_SHIFT            0x10
#define VK_CAPITAL          0x14
#define VK_DELETE           0x2E
#define VK_F4               0x73
#define VK_LCONTROL         0xA2
#define VK_RCONTROL         0xA3
#define VK_LMENU            0xA4
#define VK_RMENU            0xA5

#define WM_ACTIVATE                 0x0006
#define WM_CLOSE                    0x0010
#define WM_QUIT                     0x0012
#define WM_SETCURSOR                0x0020
#define WM_INPUTLANGCHANGEREQUEST   0x0050
#define WM_SYSKEYDOWN               0x0104
#define WM_SYSCOMMAND               0x0112

// WA_INACTIVE (winuser.h) -- WM_ACTIVATE's LOWORD(wParam) "deactivated" value,
// device.cpp's OnWM_Activate. The other two (WA_ACTIVE/WA_CLICKACTIVE) aren't
// compared against anywhere in this codebase (grepped), so only this one is needed.
#define WA_INACTIVE 0
#define WA_ACTIVE   1 // Level_network_start_client re-runs Device.OnWM_Activate(WA_ACTIVE, 0)

// LOWORD/HIWORD (windef.h) -- pull the low/high 16 bits out of a 32-bit
// value; WPARAM/LPARAM pack two 16-bit values this way in several window
// messages (Device_wndproc.cpp's WM_SETCURSOR: HTCLIENT in the low word).
#define LOWORD(l) ((WORD)(((uintptr_t)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((uintptr_t)(l)) >> 16) & 0xffff))

// Rest of Device_wndproc.cpp's WndProc: HTCLIENT/SC_*/cursor/DefWindowProc.
// There is no real Win32 message loop calling into this on this platform
// yet (window creation itself is still MSVC-only, see Device_create.cpp) --
// these exist only so the function bodies parse; never reached at runtime.
#define HTCLIENT 1
#define SC_SIZE 0xF000
#define SC_MOVE 0xF010
#define SC_MAXIMIZE 0xF030
#define SC_KEYMENU 0xF100
#define SC_SCREENSAVE 0xF140
#define SC_MONITORPOWER 0xF170
using HCURSOR = void*;
#define IDC_ARROW ((LPCSTR)32512)
inline HCURSOR LoadCursor(HINSTANCE, LPCSTR) { return nullptr; }
inline HCURSOR SetCursor(HCURSOR) { return nullptr; }
inline LRESULT DefWindowProc(HWND, UINT, WPARAM, LPARAM) { return 0; }

// SetWindowPos/HWND_TOPMOST family -- device.cpp's precache-screen
// always-on-top toggle. No-op: there is no real window on this platform yet
// (m_hWnd stays null, see Device_Initialize.cpp), so nothing to reorder.
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define HWND_TOPMOST ((HWND)-1)
#define HWND_NOTOPMOST ((HWND)-2)
inline BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }

// The rest of x_ray.cpp's WinMain_impl/Ed_InitEx bootstrap and message pump:
// process mutex, window-class registration, GetModuleHandle family, and the
// message loop itself. Same status as everything else in this block -- no
// real window/message-loop backend exists on this platform yet, so these
// are inert stand-ins that let the (otherwise perfectly portable) engine
// bootstrap sequence around them compile and run its real logic.
#define APIENTRY
#define READ_CONTROL 0x00020000L
inline HANDLE OpenMutex(DWORD, BOOL, const char*) { return nullptr; }
inline HANDLE CreateMutex(void*, BOOL, const char*) { return (HANDLE)(intptr_t)(-1); }
inline BOOL DisableProcessWindowsGhosting() { return TRUE; }

using HBRUSH = void*;
#define BLACK_BRUSH 4
inline HBRUSH GetStockObject(int) { return nullptr; }

#define CS_OWNDC 0x0020
#define WS_VISIBLE 0x10000000L
#define WS_CHILD 0x40000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
using WNDPROC = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);
struct WNDCLASS
{
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    void* hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
};
inline BOOL RegisterClass(const WNDCLASS*) { return TRUE; }
inline HWND CreateWindowEx(DWORD, const char*, const char*, DWORD, int, int, int, int, HWND, void*, HINSTANCE, void*) { return nullptr; }
inline BOOL DestroyWindow(HWND) { return TRUE; }

inline HMODULE GetModuleHandle(const char*) { return nullptr; }
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
inline BOOL GetModuleHandleExA(DWORD, LPCSTR, HMODULE* out) { if (out) *out = nullptr; return TRUE; }
inline DWORD GetModuleFileNameA(HMODULE, char* buf, DWORD size) { if (buf && size) buf[0] = 0; return 0; }
inline BOOL SetForegroundWindow(HWND) { return TRUE; }

struct MSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; };
#define PM_REMOVE 0x0001
// The only message this platform ever has: WM_QUIT, once PostQuitMessage was called (the main menu's "quit" ->
// KERNEL:quit -> CApplication::OnEvent). Without it the frame loop never ended and Quit did nothing (30-09, Switch).
inline volatile int g_xr_quit_posted = 0;
inline BOOL PeekMessage(MSG* m, HWND, UINT, UINT, UINT)
{
    if (!g_xr_quit_posted) return FALSE;
    if (m) { m->hwnd = nullptr; m->message = 0x0012 /*WM_QUIT*/; m->wParam = 0; m->lParam = 0; }
    return TRUE;
}
inline BOOL TranslateMessage(const MSG*) { return FALSE; }
inline LRESULT DispatchMessage(const MSG*) { return 0; }

// PostQuitMessage -- posts WM_QUIT to the (nonexistent, see above) message
// queue; x_ray.cpp's OnEvent(eQuit) calls it to unwind the real message loop.
// No-op here for the same reason PeekMessage always reports "nothing
// pending": there is no real queue on this platform yet.
inline void PostQuitMessage(int) { g_xr_quit_posted = 1; }

// CreateWaitableTimerExW family -- device.cpp's fps-limiter idle wait. The
// call site already has a designed-in, tested fallback for "no high-res
// timer available" (pre-Windows-10-1803 systems fall back to Sleep+spin the
// same way), so always reporting "unavailable" here is an honest use of
// that existing path, not a lie -- a real timerfd-backed port is a
// follow-up, not required for this to behave correctly.
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define TIMER_ALL_ACCESS 0x1F0003
#define INFINITE 0xFFFFFFFFu
inline HANDLE CreateWaitableTimerExW(void*, void*, DWORD, DWORD) { return nullptr; }
inline DWORD WaitForSingleObject(HANDLE, DWORD) { return 0; }
// SetWaitableTimer itself is declared further below, once LARGE_INTEGER exists.

// ShowWindow (winuser.h) -- CRenderDevice::ShowMainWindow, no-op alongside
// every other window call here (no real window yet).
#define SW_SHOWNORMAL 1
inline BOOL ShowWindow(HWND, int) { return TRUE; }

// SetThreadExecutionState (winbase.h) -- device.cpp's Run() tells Windows not
// to blank the display/sleep while the game is the foreground activity, then
// releases that request once the message loop exits. No real power-management
// daemon to talk to on this platform yet (same status as the rest of this
// no-op window/message-loop block), so this only has to let the call sites
// compile with the right constants, not actually inhibit sleep.
using EXECUTION_STATE = DWORD;
#define ES_SYSTEM_REQUIRED  0x00000001
#define ES_DISPLAY_REQUIRED 0x00000002
#define ES_CONTINUOUS       0x80000000
inline EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE) { return ES_CONTINUOUS; }

// The rest of x_ray.cpp's editor-facade window helpers.
#define WS_OVERLAPPED 0x00000000L
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
struct RECT { LONG left, top, right, bottom; };
inline BOOL GetClientRect(HWND, RECT* out) { if (out) *out = RECT{0, 0, 0, 0}; return TRUE; }

// ULARGE_INTEGER/GetDiskFreeSpaceEx (winbase.h) -- Device_Initialize.cpp's
// startup disk-space log line. statvfs() is the direct POSIX analog:
// f_bavail (blocks available to an unprivileged caller) matches
// GetDiskFreeSpaceEx's "free bytes available to caller" semantics more
// closely than f_bfree (which includes root-reserved blocks).
union ULARGE_INTEGER
{
    struct { DWORD LowPart; DWORD HighPart; };
    uint64_t QuadPart;
};
#include <sys/statvfs.h>
inline BOOL GetDiskFreeSpaceEx(const char* path, ULARGE_INTEGER*, ULARGE_INTEGER* totalBytes, ULARGE_INTEGER* freeBytes)
{
    struct statvfs st{};
    if (0 != statvfs(xr_ppath(path), &st)) return FALSE;
    if (totalBytes) totalBytes->QuadPart = (uint64_t)st.f_frsize * (uint64_t)st.f_blocks;
    if (freeBytes) freeBytes->QuadPart = (uint64_t)st.f_frsize * (uint64_t)st.f_bavail;
    return TRUE;
}

// __declspec(dllexport/dllimport/...) -- every module here already routes
// its own export macro through a _STATIC define (XRSOUND_API etc.) to
// avoid this, but ed_facade.h calls it raw and unguarded on a few
// standalone C-linkage functions (an SDK-editor-facing entry point list).
#define __declspec(x)
// _declspec -- MSVC's single-underscore synonym for the same keyword;
// x_ray.cpp's NvOptimusEnablement/AmdPowerXpressRequestHighPerformance
// (the Optimus/PowerXpress GPU-selection hint globals, read by the driver
// via GetProcAddress on the exe itself) use this spelling directly.
#define _declspec(x)

// Реальная (не заглушка) проверка через /proc/self/status: TracerPid != 0
// значит процесс кем-то трассируется (gdb/strace/любой ptrace-based
// дебаггер) -- Linux-specific аналог WinAPI IsDebuggerPresent().
inline BOOL IsDebuggerPresent()
{
#ifdef __SWITCH__
    // Horizon has no /proc: the fopen below only failed -- after a round trip to the
    // SD card's filesystem service, once per log line (AddOne asks every time). v202
    // stack samples: up to 6% of the main thread's load time in that fopen.
    return FALSE;
#endif
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[256];
    BOOL traced = FALSE;
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "TracerPid:", 10) == 0)
        {
            traced = atoi(line + 10) != 0;
            break;
        }
    }
    fclose(f);
    return traced;
}

// CONTEXT/PCONTEXT (register snapshot for a stack walk from a saved context,
// stacktrace_collector.h's BuildStackTrace overload) -- dead code on this
// platform: stacktrace_collector.cpp's POSIX branch never populates one, it
// always walks the calling thread via backtrace() instead (no per-thread
// register-context capture equivalent to SEH's, see that file). Kept only
// so the declaration parses.
struct CONTEXT { int _unused; };
using PCONTEXT = CONTEXT*;

// SEH-типы (xrDebug.h::LogStackTrace/DbgLogExceptionFilter). На Linux SEH нет
// вообще (см. platform-seam-step2-seh) -- код, который эти поля реально
// читает (fault-адрес, код исключения), мёртв на этой платформе уже
// АРХИТЕКТУРНО: ни один POSIX-обработчик здесь не строит настоящий
// _EXCEPTION_POINTERS (см. xrDebugNew.cpp -- SIGSEGV/SIGBUS идут через
// отдельный sigaction-путь, не через эту структуру). ExceptionCode -- не
// пустая заглушка (DbgLogExceptionFilter реально читает это поле), поэтому
// не void*, а типизированное поле, просто вечно нулевое на этой платформе.
struct EXCEPTION_RECORD { DWORD ExceptionCode = 0; };
struct _EXCEPTION_POINTERS { EXCEPTION_RECORD* ExceptionRecord; PCONTEXT ContextRecord; };

// EXCEPTION_EXECUTE_HANDLER -- значение SEH-фильтра (реальное число с
// Windows, 1), которое DbgLogExceptionFilter возвращает НАПРЯМУЮ (не через
// XR_SEH_EXCEPT, чей аргумент-фильтр на этой платформе и так отбрасывается
// макросом, см. xr_platform_seh.h) -- нужна сама константа, не заглушка.
inline constexpr LONG EXCEPTION_EXECUTE_HANDLER = 1;

// ---------------------------------------------------------------------------
// _finddata_t/_findfirst/_findnext/_findclose -- НАСТОЯЩАЯ (не заглушка)
// реализация через POSIX dirent.h, используется LocatorAPI::ProcessOne /
// RecurseScanPhysicalPath для сканирования archives/папок (грепом
// подтверждено -- единственный вызывающий во всём движке). Модель итерации
// (handle -> struct-per-entry на каждый _findnext) СОВПАДАЕТ с opendir/
// readdir -- это тоже per-entry API, поэтому перевод честный, не заглушка
// вокруг несовместимой модели.
//
// `_finddata64i32_t`/`_FINDDATA_T` (LocatorAPI_defs.h) -- отдельное на
// MSVC (64-бит size, 32-бит time) имя того же понятия; здесь оба имени --
// один и тот же тип, различие полей на MSVC этому коду не нужно (везде
// доступ только по имени поля, не по layout).
struct _finddata64i32_t
{
    char name[260];
    unsigned attrib;
    long long size;
    long time_write;
};
using _finddata_t = _finddata64i32_t;

#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20

struct xr_find_ctx
{
    DIR* dir;
    char dirpath[1024]; // directory part, всегда заканчивается на '/'
    char pattern[260];  // компонент после последнего разделителя
};

// ⚠Windows FindFirstFile/_findfirst трактует "*.*" как ДОС-легаси-особый
// случай "вообще все записи", а не буквальный glob (буквальный fnmatch
// потребовал бы точки в имени и молча выкинул бы папки и файлы без
// расширения -- RecurseScanPhysicalPath рекурсирует в папки именно через
// этот путь, так что буквальный fnmatch тут был бы настоящей регрессией).
// Грепом подтверждено: этот движок зовёт _findfirst ТОЛЬКО с "*.*".
inline bool xr_find_match(const char* pattern, const char* name)
{
    if (0 == strcmp(pattern, "*.*"))
        return true;
    return 0 == fnmatch(pattern, name, 0);
}

inline void xr_find_fill(const xr_find_ctx* ctx, const struct dirent* de, _finddata_t* out)
{
    strncpy(out->name, de->d_name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = 0;

    char full[1300];
    snprintf(full, sizeof(full), "%s%s", ctx->dirpath, de->d_name);

    struct stat st{};
    out->attrib = 0;
    out->size = 0;
    out->time_write = 0;
    if (0 == stat(full, &st))
    {
        out->size = (long long)st.st_size;
        out->time_write = (long)st.st_mtime;
        if (S_ISDIR(st.st_mode))
            out->attrib |= _A_SUBDIR;
    }
    if (de->d_name[0] == '.')
        out->attrib |= _A_HIDDEN;
}

inline intptr_t _findfirst(const char* pattern, _finddata_t* out)
{
    auto* ctx = new xr_find_ctx{};

    // The LAST separator of either kind ("gamedata\*.*", "gamedata\levels\*.*"), not '/' first.
    const char* slash = strrchr(pattern, '/');
    if (const char* bslash = strrchr(pattern, '\\'))
        if (!slash || bslash > slash)
            slash = bslash;

    if (slash)
    {
        const size_t n = (size_t)(slash - pattern) + 1;
        const size_t cap = sizeof(ctx->dirpath) - 1;
        const size_t cn = n < cap ? n : cap;
        memcpy(ctx->dirpath, pattern, cn);
        ctx->dirpath[cn] = 0;
        for (char* c = ctx->dirpath; *c; ++c) // opendir()/stat() below want '/'
            if (*c == '\\')
                *c = '/';
        strncpy(ctx->pattern, slash + 1, sizeof(ctx->pattern) - 1);
    }
    else
    {
        strcpy(ctx->dirpath, "./");
        strncpy(ctx->pattern, pattern, sizeof(ctx->pattern) - 1);
    }
    ctx->pattern[sizeof(ctx->pattern) - 1] = 0;

    ctx->dir = opendir(ctx->dirpath);
    if (!ctx->dir)
    {
        delete ctx;
        return -1;
    }

    struct dirent* de;
    while ((de = readdir(ctx->dir)) != nullptr)
    {
        if (xr_find_match(ctx->pattern, de->d_name))
        {
            xr_find_fill(ctx, de, out);
            return (intptr_t)ctx;
        }
    }
    closedir(ctx->dir);
    delete ctx;
    return -1;
}

inline int _findnext(intptr_t handle, _finddata_t* out)
{
    auto* ctx = (xr_find_ctx*)handle;
    struct dirent* de;
    while ((de = readdir(ctx->dir)) != nullptr)
    {
        if (xr_find_match(ctx->pattern, de->d_name))
        {
            xr_find_fill(ctx, de, out);
            return 0;
        }
    }
    return -1;
}

inline int _findclose(intptr_t handle)
{
    auto* ctx = (xr_find_ctx*)handle;
    if (ctx)
    {
        closedir(ctx->dir);
        delete ctx;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// CreateFile/CreateFileMapping/MapViewOfFile family -- НАСТОЯЩИЙ (не
// заглушка) POSIX-порт. Используется LocatorAPI::archive::open (архивы),
// FS.cpp::CVirtualFileReader (loose-файлы) и CMapStreamReader/
// CFileStreamReader (level.geom и другие потоковые чтения через скользящее
// окно, см. stream_reader.cpp) -- грепом подтверждено, других вызывающих во
// всём движке нет.
//
// Модель: на Windows CreateFileMapping даёт ОТДЕЛЬНЫЙ от файла хендл
// маппинга, который переживает закрытие файлового хендла и с которого
// можно MapViewOfFile НЕСКОЛЬКО РАЗ параллельно (у CMapStreamReader ровно
// так и происходит -- окно переоткрывается на каждый remap). На POSIX эту
// роль играет dup()-нутый файловый дескриптор: у него независимый от
// оригинала жизненный цикл (закрыть оригинал не мешает маппингу), и с него
// можно mmap() сколько угодно раз -- та же семантика, не заглушка вокруг
// несовместимой модели.
//
// UnmapViewOfFile(ptr) на Windows не принимает размер (ОС сама помнит
// размер вью по адресу); munmap(addr, length) на POSIX размер требует.
// Решение -- сторонняя таблица adres->длина, заполняемая MapViewOfFile и
// потребляемая UnmapViewOfFile, под мьютексом (геометрия грузится с
// воркер-потоков, см. render/upload level geometry straight from the
// mapped window).
// ---------------------------------------------------------------------------
union LARGE_INTEGER
{
    struct { DWORD LowPart; LONG HighPart; };
    long long QuadPart;
};
inline BOOL SetWaitableTimer(HANDLE, LARGE_INTEGER*, LONG, void*, void*, BOOL) { return FALSE; }

inline DWORD GetLastError() { return (DWORD)errno; }

// ---------------------------------------------------------------------------
// Thread/process pseudo-handles + per-thread/process CPU time -- used by
// cpu_budget.cpp (the frame-phase profiler). A "thread handle" on POSIX has
// nothing to actually open (no kernel object to acquire) -- OpenThread and
// GetCurrentThread just tag a tid so QueryThreadCycleTime knows which clock
// to build. tid 0 is reserved to mean "the calling thread" (Linux tids start
// at 1), matching CLOCK_THREAD_CPUTIME_ID's own "self" semantics -- also
// exactly how real GetCurrentThread() works (a fixed pseudo-handle, not a
// real per-call one).
// ---------------------------------------------------------------------------
inline constexpr intptr_t XR_THREAD_HANDLE_TAG = (intptr_t)1 << 62;
inline HANDLE xr_thread_handle(long tid) { return (HANDLE)(XR_THREAD_HANDLE_TAG | (intptr_t)(unsigned)tid); }
inline bool xr_is_thread_handle(HANDLE h) { return ((intptr_t)h & XR_THREAD_HANDLE_TAG) != 0; }
inline long xr_thread_handle_tid(HANDLE h) { return (long)((intptr_t)h & ~XR_THREAD_HANDLE_TAG); }

inline HANDLE GetCurrentThread() { return xr_thread_handle(0); }
inline HANDLE OpenThread(DWORD /*access*/, BOOL /*inherit*/, DWORD tid) { return xr_thread_handle((long)tid); }
inline DWORD GetCurrentThreadId() { return (DWORD)gettid(); }
inline HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)(-1); } // real Windows also returns a fixed pseudo-handle here, not a per-call one

// GetProcessMemoryInfo (psapi.h) -- Stats.cpp's debug memory overlay only
// ever reads WorkingSetSize/PagefileUsage. /proc/self/status's VmRSS is the
// direct analog of WorkingSetSize (resident pages); VmSize (total mapped
// virtual memory, not just what's actually swapped) is the closest analog
// of PagefileUsage available without CAP_SYS_PTRACE-gated /proc/self/smaps.
struct PROCESS_MEMORY_COUNTERS
{
    DWORD cb;
    size_t WorkingSetSize;
    size_t PagefileUsage;
};
inline BOOL GetProcessMemoryInfo(HANDLE, PROCESS_MEMORY_COUNTERS* pc, DWORD)
{
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[256];
    unsigned long vmrss_kb = 0, vmsize_kb = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "VmRSS:", 6) == 0) sscanf(line + 6, "%lu", &vmrss_kb);
        else if (strncmp(line, "VmSize:", 7) == 0) sscanf(line + 7, "%lu", &vmsize_kb);
    }
    fclose(f);
    pc->WorkingSetSize = (size_t)vmrss_kb * 1024;
    pc->PagefileUsage = (size_t)vmsize_kb * 1024;
    return TRUE;
}

// MEMORYSTATUSEX/GlobalMemoryStatusEx -- Stats.cpp's debug overlay. Windows'
// "page file" is this system's swap; "TotalPageFile" is documented as the
// overall commit limit (RAM + pagefile), so MemTotal+SwapTotal is the direct
// analog, same shape for the Avail variant via /proc/meminfo's MemAvailable
// (kernel-computed "usable without swapping", closer to Windows' intent
// than MemFree) + SwapFree.
struct MEMORYSTATUSEX
{
    DWORD dwLength;
    DWORD dwMemoryLoad;
    DWORDLONG ullTotalPhys;
    DWORDLONG ullAvailPhys;
    DWORDLONG ullTotalPageFile;
    DWORDLONG ullAvailPageFile;
    DWORDLONG ullTotalVirtual;
    DWORDLONG ullAvailVirtual;
    DWORDLONG ullAvailExtendedVirtual;
};
inline BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* ms)
{
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return FALSE;
    char line[256];
    unsigned long long mem_total_kb = 0, mem_avail_kb = 0, swap_total_kb = 0, swap_free_kb = 0;
    while (fgets(line, sizeof(line), f))
    {
        sscanf(line, "MemTotal: %llu kB", &mem_total_kb);
        sscanf(line, "MemAvailable: %llu kB", &mem_avail_kb);
        sscanf(line, "SwapTotal: %llu kB", &swap_total_kb);
        sscanf(line, "SwapFree: %llu kB", &swap_free_kb);
    }
    fclose(f);
    ms->ullTotalPhys = mem_total_kb * 1024ull;
    ms->ullAvailPhys = mem_avail_kb * 1024ull;
    ms->ullTotalPageFile = (mem_total_kb + swap_total_kb) * 1024ull;
    ms->ullAvailPageFile = (mem_avail_kb + swap_free_kb) * 1024ull;
    ms->ullTotalVirtual = ms->ullTotalPageFile;
    ms->ullAvailVirtual = ms->ullAvailPageFile;
    ms->ullAvailExtendedVirtual = 0;
    ms->dwMemoryLoad = mem_total_kb ? (DWORD)(100ull - (mem_avail_kb * 100ull / mem_total_kb)) : 0;
    return TRUE;
}

// tid!=0: the same TID_TO_CLOCKID construction glibc's own pthread_getcpuclockid
// uses internally -- works for any thread in the CALLING process without
// special permission, which is all this profiler ever queries.
#ifdef __SWITCH__
// Horizon has no CLOCK_THREAD_CPUTIME_ID / CLOCK_PROCESS_CPUTIME_ID. Reporting "not available" (FALSE, 0)
// is honest: the cpu_budget instrument prints zeros instead of wall time passed off as CPU time.
inline BOOL QueryThreadCycleTime(HANDLE, ULONG64* out) { if (out) *out = 0; return FALSE; }
inline BOOL QueryProcessCycleTime(HANDLE, ULONG64* out) { if (out) *out = 0; return FALSE; }
#else
inline BOOL QueryThreadCycleTime(HANDLE h, ULONG64* out)
{
    if (out) *out = 0;
    if (!xr_is_thread_handle(h)) return FALSE;
    const long tid = xr_thread_handle_tid(h);
    struct timespec ts{};
    const int rc = (tid == 0)
        ? clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)
        : clock_gettime((clockid_t)((~(clockid_t)tid << 3) | 2 /*CPUCLOCK_SCHED*/), &ts);
    if (rc != 0) return FALSE;
    // "Cycles" on this platform ARE nanoseconds -- see cycles_per_ms() in
    // cpu_budget.cpp, which returns the exact 1e6 ns/ms instead of a TSC
    // calibration on this path, so every existing cpm-based conversion in
    // that file keeps working unmodified.
    if (out) *out = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    return TRUE;
}

// QueryProcessCycleTime sums cycles across every thread the process has EVER
// had (including ones that exited) -- CLOCK_PROCESS_CPUTIME_ID is the same
// sum, same semantics, not an approximation.
inline BOOL QueryProcessCycleTime(HANDLE /*process*/, ULONG64* out)
{
    if (out) *out = 0;
    struct timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return FALSE;
    if (out) *out = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    return TRUE;
}
#endif // __SWITCH__ (no per-thread/per-process CPU clocks on Horizon)

#define GENERIC_READ 0x80000000u
#define FILE_SHARE_READ 0x00000001u
#define FILE_SHARE_WRITE 0x00000002u
#define OPEN_EXISTING 3u
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define PAGE_READONLY 0x02u
#define FILE_MAP_READ 0x0004u
#define FILE_ATTRIBUTE_READONLY 0x00000001u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
inline HANDLE xr_invalid_handle() { return (HANDLE)(intptr_t)(-1); }
#define INVALID_HANDLE_VALUE xr_invalid_handle()

struct SYSTEM_INFO { DWORD dwAllocationGranularity; DWORD dwPageSize; DWORD dwNumberOfProcessors; };
inline void GetSystemInfo(SYSTEM_INFO* si)
{
    const long ps = sysconf(_SC_PAGESIZE);
    si->dwPageSize = (DWORD)ps;
    // Windows' dwAllocationGranularity (historically 64 KB, a VirtualAlloc
    // artifact) exists to say "MapViewOfFile offsets must align to THIS, not
    // to the page size" -- mmap's alignment requirement IS the page size, so
    // that's the honest equivalent here, not an approximation.
    si->dwAllocationGranularity = (DWORD)ps;
    // dwNumberOfProcessors -- sysconf's online-CPU count is the direct POSIX
    // equivalent (same "logical processors available to this process right
    // now" semantics), used by _math.cpp/cpuid.cpp's non-MSVC branches.
    const long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = nproc > 0 ? (DWORD)nproc : 1;
}

inline HANDLE CreateFile(const char* path, DWORD /*access*/, DWORD /*share*/, void* /*sa*/, DWORD /*disposition*/, DWORD /*flags*/, HANDLE /*templateFile*/ = nullptr)
{
    const int fd = open(xr_ppath(path), O_RDONLY);
    return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)(intptr_t)fd;
}
// CreateFileA -- real Windows itself resolves the un-suffixed `CreateFile`
// name to this one via a macro (non-UNICODE builds), so aliasing it back to
// our own CreateFile is the exact same indirection, not an approximation.
// Call sites here use the "A" suffix directly (bypassing that macro), so the
// alias has to exist under its own name too.
#define CreateFileA CreateFile

// ---------------------------------------------------------------------------
// FindFirstFileA/FindNextFileA/FindClose + WIN32_FIND_DATAA -- REAL port
// (not the DOS-legacy "*.*"-means-everything shortcut the _findfirst family
// above uses): xrOnlineNet/content_integrity.cpp passes a genuine wildcard
// pattern, so this one has to glob for real via fnmatch(). Two
// separate WinAPI families exist in this codebase (the MSVC CRT
// _findfirst/_finddata_t one, and this raw-WinAPI one) because that's what
// the original code actually calls at each site -- kept as two shims rather
// than folded into one, matching the two distinct real APIs.
// ---------------------------------------------------------------------------
struct WIN32_FIND_DATAA
{
    DWORD dwFileAttributes;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    char cFileName[260];
};
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010

// GetFileAttributesA (winbase.h) -- content_integrity.cpp only ever checks
// it against INVALID_FILE_ATTRIBUTES as an existence probe, never reads the
// actual attribute bits, so a stat()-backed "does this path resolve" is a
// faithful enough port.
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
inline DWORD GetFileAttributesA(const char* path)
{
    struct stat st{};
    return (0 == stat(xr_ppath(path), &st)) ? (DWORD)0 : INVALID_FILE_ATTRIBUTES;
}

// SetFileAttributes -- every real caller in this codebase only ever passes
// FILE_ATTRIBUTE_NORMAL (clear read-only before overwriting a config file)
// or FILE_ATTRIBUTE_READONLY, so a chmod() toggling just the owner-write
// bit covers the only two attributes actually used.
inline BOOL SetFileAttributes(const char* path, DWORD attrs)
{
    struct stat st{};
    if (0 != stat(xr_ppath(path), &st)) return FALSE;
    mode_t mode = (attrs & FILE_ATTRIBUTE_READONLY) ? (st.st_mode & ~(mode_t)S_IWUSR) : (st.st_mode | S_IWUSR);
    return (0 == chmod(xr_ppath(path), mode)) ? TRUE : FALSE;
}

struct xr_win32_find_ctx
{
    DIR* dir;
    char dirpath[1024];
    char pattern[260];
};

inline void xr_win32_find_fill(const xr_win32_find_ctx* ctx, const struct dirent* de, WIN32_FIND_DATAA* out)
{
    strncpy(out->cFileName, de->d_name, sizeof(out->cFileName) - 1);
    out->cFileName[sizeof(out->cFileName) - 1] = 0;

    char full[1300];
    snprintf(full, sizeof(full), "%s%s", ctx->dirpath, de->d_name);

    struct stat st{};
    out->nFileSizeHigh = 0;
    out->nFileSizeLow = 0;
    out->dwFileAttributes = 0;
    if (0 == stat(full, &st))
    {
        out->nFileSizeHigh = (DWORD)(((unsigned long long)st.st_size) >> 32);
        out->nFileSizeLow = (DWORD)((unsigned long long)st.st_size & 0xFFFFFFFFu);
        if (S_ISDIR(st.st_mode)) out->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
}

inline HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* out)
{
    auto* ctx = new xr_win32_find_ctx{};

    const char* slash = strrchr(pattern, '/');
    if (const char* bslash = strrchr(pattern, '\\'))
        if (!slash || bslash > slash)
            slash = bslash;

    if (slash)
    {
        const size_t n = (size_t)(slash - pattern) + 1;
        const size_t cap = sizeof(ctx->dirpath) - 1;
        const size_t cn = n < cap ? n : cap;
        memcpy(ctx->dirpath, pattern, cn);
        ctx->dirpath[cn] = 0;
        for (char* c = ctx->dirpath; *c; ++c)
            if (*c == '\\')
                *c = '/';
        strncpy(ctx->pattern, slash + 1, sizeof(ctx->pattern) - 1);
    }
    else
    {
        strcpy(ctx->dirpath, "./");
        strncpy(ctx->pattern, pattern, sizeof(ctx->pattern) - 1);
    }
    ctx->pattern[sizeof(ctx->pattern) - 1] = 0;

    ctx->dir = opendir(ctx->dirpath);
    if (!ctx->dir)
    {
        delete ctx;
        return xr_invalid_handle();
    }

    struct dirent* de;
    while ((de = readdir(ctx->dir)) != nullptr)
    {
        if (0 == fnmatch(ctx->pattern, de->d_name, 0))
        {
            xr_win32_find_fill(ctx, de, out);
            return (HANDLE)ctx;
        }
    }
    closedir(ctx->dir);
    delete ctx;
    return xr_invalid_handle();
}

inline BOOL FindNextFileA(HANDLE handle, WIN32_FIND_DATAA* out)
{
    auto* ctx = (xr_win32_find_ctx*)handle;
    struct dirent* de;
    while ((de = readdir(ctx->dir)) != nullptr)
    {
        if (0 == fnmatch(ctx->pattern, de->d_name, 0))
        {
            xr_win32_find_fill(ctx, de, out);
            return TRUE;
        }
    }
    return FALSE;
}

inline BOOL FindClose(HANDLE handle)
{
    auto* ctx = (xr_win32_find_ctx*)handle;
    if (ctx)
    {
        closedir(ctx->dir);
        delete ctx;
    }
    return TRUE;
}

inline BOOL CloseHandle(HANDLE h)
{
    // A thread pseudo-handle (OpenThread/GetCurrentThread) has no fd behind
    // it -- must check BEFORE treating the bit pattern as a file descriptor,
    // or a tagged handle's low bits could alias a real open fd and close it.
    if (xr_is_thread_handle(h)) return TRUE;
    const int fd = (int)(intptr_t)h;
    if (fd < 0) return FALSE;
    return close(fd) == 0 ? TRUE : FALSE;
}

inline BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* out)
{
    struct stat st{};
    if (fstat((int)(intptr_t)h, &st) != 0) return FALSE;
    out->QuadPart = (long long)st.st_size;
    return TRUE;
}

inline DWORD SetFilePointer(HANDLE h, long distance, void* /*distanceHigh, unused: этот движок его не передаёт*/, DWORD method)
{
    int whence = SEEK_SET;
    if (method == FILE_CURRENT) whence = SEEK_CUR;
    else if (method == FILE_END) whence = SEEK_END;
    const off_t r = lseek((int)(intptr_t)h, (off_t)distance, whence);
    return r < 0 ? INVALID_SET_FILE_POINTER : (DWORD)r;
}

inline BOOL ReadFile(HANDLE h, void* buf, DWORD toRead, DWORD* outRead, void* /*overlapped*/)
{
    const ssize_t n = read((int)(intptr_t)h, buf, toRead);
    if (n < 0) { if (outRead) *outRead = 0; return FALSE; }
    if (outRead) *outRead = (DWORD)n;
    return TRUE;
}

inline HANDLE CreateFileMapping(HANDLE fileHandle, void* /*sa*/, DWORD /*protect*/, DWORD /*sizeHigh*/, DWORD /*sizeLow*/, const char* /*name*/)
{
    const int fd = dup((int)(intptr_t)fileHandle);
    return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)(intptr_t)fd;
}

inline std::mutex& xr_mmap_table_mutex() { static std::mutex m; return m; }
inline std::unordered_map<void*, size_t>& xr_mmap_table() { static std::unordered_map<void*, size_t> t; return t; }

// dwFileOffsetHigh -- грепом подтверждено, этот движок ВСЕГДА зовёт с 0,
// смещения не разделяются на high/low нигде. Собираем честно на все 64 бита
// на случай будущего вызывающего, вреда от этого нет.
inline void* MapViewOfFile(HANDLE mapHandle, DWORD /*access*/, DWORD offsetHigh, DWORD offsetLow, size_t size)
{
    const off_t offset = ((off_t)offsetHigh << 32) | (off_t)offsetLow;
    size_t map_size = size;
    if (map_size == 0) // Windows: 0 = "map to end of file"
    {
        struct stat st{};
        if (fstat((int)(intptr_t)mapHandle, &st) != 0 || (off_t)st.st_size <= offset)
            return nullptr;
        map_size = (size_t)(st.st_size - offset);
    }
    void* p = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, (int)(intptr_t)mapHandle, offset);
    if (p == MAP_FAILED)
        return nullptr;
    std::lock_guard<std::mutex> lk(xr_mmap_table_mutex());
    xr_mmap_table()[p] = map_size;
    return p;
}

inline DWORD GetCurrentDirectory(DWORD size, char* buf)
{
    if (!getcwd(buf, size)) { if (size) buf[0] = 0; return 0; }
    return (DWORD)strlen(buf);
}

// _stat/_unlink -- MSVC CRT names for the POSIX stat()/unlink() this codebase
// also calls without the underscore elsewhere; `struct _stat`/`_stat(...)`
// live in separate C++ lookup namespaces (tag vs. ordinary), so one macro
// covers both the type and the function call.
#define _stat stat
inline int xr_unlink_posix(const char* p) { return unlink(xr_ppath(p)); }
#define _unlink xr_unlink_posix

// _fseeki64/_ftelli64 -- MSVC CRT 64-bit file seek/tell. fseeko64/ftello64
// are glibc's own 64-bit-explicit names for the same operation (used
// instead of plain fseeko/ftello so this doesn't depend on _FILE_OFFSET_BITS
// being set the same way project-wide).
#ifdef __SWITCH__
#define _fseeki64 fseeko // newlib (LP64): off_t is already 64-bit, and it has no *o64 names
#define _ftelli64 ftello
#else
#define _fseeki64 fseeko64
#define _ftelli64 ftello64
#endif

// _W64 -- old MSVC marker macro (predates <cstdint>, flagged types that are
// 64-bit under Win64 for the long-retired /Wp64 tool). Empty on modern MSVC
// too; GCC/Clang never had it.
#define _W64

// __noop -- MSVC intrinsic used (via LogDbg/MsgDbg/FuncDbg, xrCore.h) to
// compile debug logging away to nothing in non-debug builds; discards its
// arguments without evaluating side effects, same as the real intrinsic.
#define __noop(...) ((void)0)

inline BOOL UnmapViewOfFile(const void* ptr)
{
    if (!ptr) return FALSE;
    size_t len = 0;
    {
        std::lock_guard<std::mutex> lk(xr_mmap_table_mutex());
        auto it = xr_mmap_table().find(const_cast<void*>(ptr));
        if (it == xr_mmap_table().end())
            return FALSE;
        len = it->second;
        xr_mmap_table().erase(it);
    }
    return munmap(const_cast<void*>(ptr), len) == 0 ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// xrCore.cpp — единственный вызывающий этих пяти имён во всём движке (грепом
// подтверждено 19-09). В отличие от NT-сисколлов выше у них есть настоящие
// POSIX эквиваленты -- это НАСТОЯЩИЕ функциональные реализации, не заглушки.
// ---------------------------------------------------------------------------
#include <unistd.h>
#include <climits>

// WinAPI GetCommandLine() отдаёт ИСХОДНУЮ строку командной строки процесса, которой
// на POSIX как единого объекта нет -- она приходит через argc/argv main(). Буфер ниже
// заполняет POSIX-main() в xr_3da/x_ray.cpp (argv, склеенные через пробел, как на
// Windows -- вместе с именем программы); до этого строка пустая.
inline char* xr_posix_command_line() { static char buf[8192] = {0}; return buf; }
inline char* GetCommandLine() { return xr_posix_command_line(); }

inline DWORD GetModuleFileName(void* /*hModule*/, char* buf, DWORD size)
{
    const ssize_t n = readlink("/proc/self/exe", buf, size ? size - 1 : 0);
    if (n <= 0) { if (size) buf[0] = 0; return 0; }
    buf[n] = 0;
    return (DWORD)n;
}

inline DWORD GetUserName(char* buf, DWORD* size)
{
    const char* name = getlogin();
    if (!name) name = "user";
    const size_t len = strlen(name);
    if (*size <= len) { *size = (DWORD)len + 1; return FALSE; }
    strcpy(buf, name);
    *size = (DWORD)len;
    return TRUE;
}

inline DWORD GetComputerName(char* buf, DWORD* size)
{
    if (gethostname(buf, *size) != 0) return FALSE;
    buf[*size - 1] = 0;
    *size = (DWORD)strlen(buf);
    return TRUE;
}

// POSIX-путь без буквы диска: drive всегда пуст, dir -- всё до последнего
// '/' включительно, fname/ext делятся по последней точке В ИМЕНИ файла
// (не в каталоге). Небезопасно по размеру буферов -- ТОЧНО как оригинальный
// MSVC _splitpath, вызывающий код по всему движку уже рассчитан на это.
inline void _splitpath(const char* path, char* drive, char* dir, char* fname, char* ext)
{
    if (drive) drive[0] = 0;
    // MSVC's _splitpath accepts BOTH separators, and the engine's own paths are backslash paths
    // ("gamedata\config\system.ltx" from FS.update_path). Splitting on '/' alone returned an empty
    // directory for them, which tripped `R_ASSERT(path && path[0])` on the first #include of system.ltx.
    const char* slash = strrchr(path, '/');
    if (const char* bslash = strrchr(path, '\\'))
        if (!slash || bslash > slash) slash = bslash;
    const char* base = slash ? slash + 1 : path;
    if (dir)
    {
        const size_t n = base - path;
        memcpy(dir, path, n);
        dir[n] = 0;
    }
    const char* dot = strrchr(base, '.');
    if (fname)
    {
        const size_t n = dot ? (size_t)(dot - base) : strlen(base);
        memcpy(fname, base, n);
        fname[n] = 0;
    }
    if (ext) strcpy(ext, dot ? dot : "");
}

// concurrency::concurrent_vector<T> (MSVC PPL, <concurrent_vector.h>) --
// xr_object_list.h's m_parallel_crows: concurrent push_back() from
// non-main threads (CObjectList::o_crow's `else` branch, Device.OnMainThread()
// false), drained with clear()/range-for from the main thread. A mutex
// around a plain std::vector has the same observable semantics (safe
// concurrent append, safe single-reader drain) though not PPL's lock-free
// implementation -- this is the only real caller in the engine (grepped),
// so a lock isn't a hot-path concern here.
#include <vector>
namespace concurrency
{
template <typename T>
class concurrent_vector
{
public:
    void push_back(const T& v) { std::lock_guard<std::mutex> lk(m_); data_.push_back(v); }
    void clear() { std::lock_guard<std::mutex> lk(m_); data_.clear(); }
    bool empty() const { std::lock_guard<std::mutex> lk(m_); return data_.empty(); }
    auto begin() { return data_.begin(); }
    auto end() { return data_.end(); }
    auto begin() const { return data_.begin(); }
    auto end() const { return data_.end(); }

private:
    mutable std::mutex m_;
    std::vector<T> data_;
};
} // namespace concurrency

// ---------------------------------------------------------------------------
// xrRenderVulkan (step 14). Constants copied from the Windows SDK 10.0.26100.0
// headers, not typed from memory.
// ---------------------------------------------------------------------------

// POINT / cursor family (winuser.h) -- only the editor viewport / spike ImGui
// input paths poll the OS cursor (editor embedding is Windows-only). With no
// cursor to report, GetCursorPos fails and every caller already early-outs on
// that; GetAsyncKeyState reports "up".
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
struct POINT { LONG x, y; };
inline BOOL GetCursorPos(POINT*) { return FALSE; }
inline BOOL SetCursorPos(int, int) { return FALSE; } // UICursor warps the OS cursor; no OS cursor here
inline BOOL ScreenToClient(HWND, POINT*) { return FALSE; }
inline short GetAsyncKeyState(int) { return 0; }
#define CURSOR_SHOWING 0x00000001
struct CURSORINFO { DWORD cbSize; DWORD flags; HCURSOR hCursor; POINT ptScreenPos; };
inline BOOL GetCursorInfo(CURSORINFO*) { return FALSE; }
inline int ShowCursor(BOOL show) { return show ? 0 : -1; }

#define E_NOTIMPL ((HRESULT)0x80004001L)
#define MAX_PATH 260

// CreateFile hints (winbase.h). The shim's CreateFile ignores its flags word.
#define FILE_FLAG_RANDOM_ACCESS   0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000

// SYSTEMTIME/GetLocalTime (minwinbase.h / sysinfoapi.h) -- local wall clock.
#include <ctime>
struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
inline void GetLocalTime(SYSTEMTIME* out)
{
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm t{};
    localtime_r(&ts.tv_sec, &t);
    out->wYear = WORD(t.tm_year + 1900);
    out->wMonth = WORD(t.tm_mon + 1);
    out->wDayOfWeek = WORD(t.tm_wday);
    out->wDay = WORD(t.tm_mday);
    out->wHour = WORD(t.tm_hour);
    out->wMinute = WORD(t.tm_min);
    out->wSecond = WORD(t.tm_sec);
    out->wMilliseconds = WORD(ts.tv_nsec / 1000000);
}

// PrefetchVirtualMemory (memoryapi.h) -- madvise(MADV_WILLNEED) is the direct
// analog: ask the kernel to start reading the pages in, return immediately.
using PVOID = void*;
struct WIN32_MEMORY_RANGE_ENTRY { PVOID VirtualAddress; SIZE_T NumberOfBytes; };
inline BOOL PrefetchVirtualMemory(HANDLE, uintptr_t count, WIN32_MEMORY_RANGE_ENTRY* ranges, ULONG)
{
    const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
    for (uintptr_t i = 0; i < count; ++i) {
        const uintptr_t b = uintptr_t(ranges[i].VirtualAddress) & ~(page - 1);
        const uintptr_t e = uintptr_t(ranges[i].VirtualAddress) + ranges[i].NumberOfBytes;
        madvise(reinterpret_cast<void*>(b), e - b, MADV_WILLNEED);
    }
    return TRUE;
}

// _putenv_s (stdlib.h, MSVC CRT) -- setenv writes the same process block getenv reads.
inline int _putenv_s(const char* name, const char* value) { return setenv(name, value, 1) == 0 ? 0 : errno; }

// _InterlockedIncrement (intrin.h) -- returns the incremented value, full barrier.
inline long _InterlockedIncrement(volatile long* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }

// ---------------------------------------------------------------------------
// xrGame, step 15 batch 2 -- the remaining Win32/CRT names xrGame uses directly.
// ---------------------------------------------------------------------------
using INT = int;

// memcpy_s / localtime_s (MSVC secure CRT). Bounds-checked like the real ones; note the MSVC
// localtime_s argument order (tm first) is the reverse of C11 Annex K.
inline errno_t memcpy_s(void* dest, size_t destsz, const void* src, size_t count)
{
    if (count == 0) return 0;
    if (!dest) return 22; // EINVAL
    if (!src || destsz < count) { memset(dest, 0, destsz); return !src ? 22 : 34; } // ERANGE
    memcpy(dest, src, count);
    return 0;
}
inline errno_t localtime_s(tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : 22; }

// __debugbreak (intrin.h) -- the script-exported "__debugbreak" stops a debugger right there.
#include <csignal>
inline void __debugbreak() { raise(SIGTRAP); }

// GetSystemMetrics (winuser.h) -- only UICursor asks for SM_CXSCREEN/SM_CYSCREEN, to choose
// between the OS cursor and relative deltas; 0 selects the delta path, which is also the only
// one that works here (GetCursorPos above reports failure).
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
inline int GetSystemMetrics(int) { return 0; }

// CoCreateGuid (combaseapi.h) -- random (version 4) GUID; generate_id() formats it as text.
#include <random>
inline HRESULT CoCreateGuid(GUID* g)
{
    std::random_device rd;
    for (int i = 0; i < 16; i += 4) { const uint32_t r = rd(); memcpy(reinterpret_cast<uint8_t*>(g) + i, &r, 4); }
    g->Data3 = uint16_t((g->Data3 & 0x0FFF) | 0x4000);
    g->Data4[0] = uint8_t((g->Data4[0] & 0x3F) | 0x80);
    return 0;
}

// MultiByteToWideChar (stringapiset.h), CP_UTF8 only. Writes UTF-16 code units -- WCHAR is 16-bit
// on Windows, and the one caller (UILines) passes a u16 cast to LPWSTR, so the output buffer is
// treated as char16_t here, not as the platform's 32-bit wchar_t. Returns the number of units
// written (0 = invalid/unsupported input), like the real one with cchWideChar > 0.
#define CP_ACP  0
#define CP_UTF8 65001
inline int MultiByteToWideChar(UINT cp, DWORD, const char* src, int n, LPWSTR dst_, int cap)
{
    if (cp != CP_UTF8 || !src) return 0;
    char16_t* dst = reinterpret_cast<char16_t*>(dst_);
    const unsigned char* s = reinterpret_cast<const unsigned char*>(src);
    const int len = n < 0 ? int(strlen(src)) + 1 : n;
    int i = 0, out = 0;
    while (i < len)
    {
        uint32_t c = s[i];
        int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (extra < 0 || i + extra >= len) return 0; // bad lead byte or truncated sequence
        c &= extra ? (0x3F >> extra) : 0x7F;
        for (int k = 1; k <= extra; ++k)
        {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
            c = (c << 6) | (s[i + k] & 0x3F);
        }
        i += extra + 1;
        const int units = c >= 0x10000 ? 2 : 1;
        if (dst && cap > 0)
        {
            if (out + units > cap) return 0; // ERROR_INSUFFICIENT_BUFFER
            if (units == 1) dst[out] = char16_t(c);
            else { c -= 0x10000; dst[out] = char16_t(0xD800 | (c >> 10)); dst[out + 1] = char16_t(0xDC00 | (c & 0x3FF)); }
        }
        out += units;
    }
    return out;
}

// GetUserDefaultUILanguage + GetLocaleInfoA(LOCALE_SISO639LANGNAME2) -- the game only wants the
// ISO 639-2 three-letter code of the UI language to preselect "rus"/"eng"/... from its list. The
// POSIX source is the locale environment (LC_ALL > LC_MESSAGES > LANG, e.g. "ru_RU.UTF-8").
using LANGID = uint16_t;
using LCID = DWORD;
using LCTYPE = DWORD;
#define LOCALE_SISO639LANGNAME2 0x00000067
inline LANGID GetUserDefaultUILanguage() { return 0; }
inline int GetLocaleInfoA(LCID, LCTYPE type, char* out, int cap)
{
    if (type != LOCALE_SISO639LANGNAME2 || !out || cap < 4) return 0;
    const char* env = nullptr;
    for (const char* v : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if ((env = getenv(v)) && *env) break;
    if (!env || strlen(env) < 2) return 0;
    static const struct { char iso1[3]; char iso2[4]; } map[] = {
        {"en", "eng"}, {"ru", "rus"}, {"uk", "ukr"}, {"be", "bel"}, {"pl", "pol"}, {"de", "deu"},
        {"fr", "fra"}, {"es", "spa"}, {"it", "ita"}, {"cs", "ces"}, {"pt", "por"}, {"hu", "hun"},
        {"tr", "tur"}, {"zh", "zho"}, {"ja", "jpn"}, {"ko", "kor"}};
    for (const auto& m : map)
        if (tolower(env[0]) == m.iso1[0] && tolower(env[1]) == m.iso1[1]) { memcpy(out, m.iso2, 4); return 4; }
    return 0;
}

// ShellExecuteA(…, "open", url, …) (shellapi.h) -- the start screen's web links. xdg-open is the
// desktop-neutral "open with the default handler"; spawned detached, no shell in between.
#include <spawn.h>
extern char** environ;
inline HINSTANCE ShellExecuteA(HWND, const char*, const char* file, const char*, const char*, int)
{
    if (!file || !*file) return reinterpret_cast<HINSTANCE>(2); // ERROR_FILE_NOT_FOUND (<= 32 = failure)
    pid_t pid;
    char* argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(file), nullptr};
    return reinterpret_cast<HINSTANCE>(posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0 ? 33 : 2);
}

// More MSVC CRT names used by xrGame/COMMON_AI directly.
#define _stat64i32 stat // `struct _stat64i32 st; _stat64i32(path, &st)` (sys/stat.h) -- the 64-bit-time stat
inline unsigned long long _strtoui64(const char* s, char** end, int base) { return strtoull(s, end, base); }
inline errno_t _strlwr_s(char* s, size_t n)
{
    if (!s || strnlen(s, n) == n) return 22; // EINVAL: not terminated within n
    _strlwr(s);
    return 0;
}
template <size_t N>
inline errno_t _strlwr_s(char (&s)[N]) { return _strlwr_s(s, N); }
// strncat_s: appends at most `count` chars, always terminates; ERANGE (and dest = "") if it can't fit.
inline errno_t strncat_s(char* dest, size_t destsz, const char* src, size_t count)
{
    if (!dest || !src || destsz == 0) return 22;
    const size_t dl = strnlen(dest, destsz);
    if (dl == destsz) return 22;
    const size_t sl = strnlen(src, count);
    if (dl + sl >= destsz) { dest[0] = 0; return 34; }
    memcpy(dest + dl, src, sl);
    dest[dl + sl] = 0;
    return 0;
}
template <size_t N>
inline errno_t strncat_s(char (&dest)[N], const char* src, size_t count) { return strncat_s(dest, N, src, count); }
inline DWORD GetCurrentProcessId() { return DWORD(getpid()); }
using LPPOINT = POINT*;

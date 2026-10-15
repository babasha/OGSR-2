#include "stdafx.h"
#include "xr_platform_time.h"

#ifndef _MSC_VER
#include <time.h>
#endif

// Единственный файл движка, где GetTickCount/GetTickCount64 разрешено звать
// напрямую -- см. обоснование в xr_platform_time.h.
//
// ⚠19-09: раньше здесь была ТОЛЬКО реализация под Windows -- ветка "порт на
// другую платформу меняет только этот файл" была написана в комментарии, но
// не сделана. Портируемая ветка ниже проверена РЕАЛЬНЫМ компилятором (WSL
// Arch Linux, gcc 16 и clang 22, `-std=c++20 -fsyntax-only`) как отдельный
// файл с этими же двумя функциями -- не весь движок (для него нужен ещё шов
// windows.h целиком), но САМА эта пара функций собирается на Linux.

namespace xrPlatform
{
#ifdef _MSC_VER
u32 TickCount() { return GetTickCount(); }
u64 TickCount64() { return GetTickCount64(); }
#else
u64 TickCount64()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000ull + (u64)ts.tv_nsec / 1000000ull;
}
u32 TickCount() { return (u32)TickCount64(); }
#endif
} // namespace xrPlatform

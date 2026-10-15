#pragma once

#define VC_EXTRALEAN // Exclude rarely-used stuff from Windows headers
#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#define DIRECTINPUT_VERSION 0x0800 //

#define NOGDICAPMASKS
//#define NOSYSMETRICS
#define NOMENUS
#define NOICONS
#define NOKEYSTATES
#define NODRAWTEXT
#define NOMEMMGR
//#define NOMETAFILE
#define NOSERVICE
#define NOCOMM
#define NOHELP
#define NOPROFILER
#define NOMCX
#define NOMINMAX
#define DOSWIN32
#define _WIN32_DCOM

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4005)
#include <windows.h>
#include <windowsx.h>
#pragma warning(pop)
#else
// ⚠⚠⚠19-09: ЗАГЛУШКА ДЛЯ ИЗМЕРЕНИЯ ОБЪЁМА, НЕ ДЛЯ СБОРКИ. <windows.h> тут
// включается БЕЗУСЛОВНО каждым файлом движка (xrCore.h подключает этот файл
// первым делом) -- реальный порт требует типизировать/заглушить каждое
// Windows-имя, которым в реальности пользуется xrCore и выше, а не только
// эти несколько типов. См. platform-seam-step5-windows-h-scope.md за
// подсчётом и методом.
#include "xr_platform_win32_shim.h"
#endif

// KRodin: фикс, чтобы intellisense нормально работал с многочисленными методами типа CUIWindow::SendMessage и тп.
#ifdef __INTELLISENSE__
#ifdef SendMessage
#undef SendMessage
#endif
#endif

// Engine paths are Windows paths ("a\b\c.ltx"); POSIX/newlib calls need '/'. XR_OSPATH(p) is p on MSVC and a
// separator-converted temporary everywhere else (xr_ppath, see xr_platform_win32_shim.h). Use it at every
// direct OS file call outside the shim (fopen/open/ofstream/std::filesystem).
#ifdef _MSC_VER
#define XR_OSPATH(p) (p)
#else
#define XR_OSPATH(p) (static_cast<const char*>(xr_ppath(p)))
#endif

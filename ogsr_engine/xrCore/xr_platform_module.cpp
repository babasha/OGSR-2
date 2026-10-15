#include "stdafx.h"
#include "xr_platform_module.h"

#if !defined(_MSC_VER) && !defined(__SWITCH__)
#include <dlfcn.h>
#endif

// Единственный файл движка, где LoadLibrary/GetProcAddress/FreeLibrary
// разрешено звать напрямую ради ЗАГРУЗКИ МОДУЛЕЙ -- см. обоснование в
// xr_platform_module.h.
//
// ⚠19-09: та же правка, что в xr_platform_time.cpp -- портируемая ветка
// раньше была только текстом в комментарии. dlopen/dlsym/dlclose проверены
// РЕАЛЬНЫМ компилятором (WSL Arch Linux, gcc 16 + clang 22) как отдельный
// файл с этой сигнатурой.

namespace xrPlatform
{
#ifdef _MSC_VER
void* LoadModule(const char* path) { return (void*)LoadLibraryA(path); }
void* GetSymbol(void* mod, const char* name) { return (void*)GetProcAddress((HMODULE)mod, name); }
void  UnloadModule(void* mod) { if (mod) FreeLibrary((HMODULE)mod); }
#elif defined(__SWITCH__)
// Horizon homebrew cannot load code at run time (no dlopen; a NRO is one static image, and every engine
// module is already a static library linked into it). "Module not found" is the honest answer.
void* LoadModule(const char*) { return nullptr; }
void* GetSymbol(void*, const char*) { return nullptr; }
void  UnloadModule(void*) {}
#else
void* LoadModule(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void* GetSymbol(void* mod, const char* name) { return mod ? dlsym(mod, name) : nullptr; }
void  UnloadModule(void* mod) { if (mod) dlclose(mod); }
#endif
} // namespace xrPlatform

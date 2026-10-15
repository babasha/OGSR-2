#pragma once

// ---------------------------------------------------------------------------
// ШОВ xrCore/platform, часть 2: загрузка модулей.
//
// ⭐Аудит 19-09 показал, что реальных кандидатов на "загрузка модуля как
// переносимая концепция" в движке ОДИН: `xr_3da/DiscordRPC.cpp` (опциональный
// плагин — Discord Rich Presence, LoadLibrary/GetProcAddress/FreeLibrary по
// экспортам DLL). Остальные найденные GetProcAddress-места (`cpuid.cpp`,
// `cpu_budget.cpp`, `lua_vm_watch.cpp`, `xrCore.cpp`, `xrDebugNew.cpp`,
// `_math.cpp`, `Device_Initialize.cpp`) резолвят NT-СИСКОЛЫ
// (NtQuerySystemInformation/NtQueryInformationThread/ZwSetTimerResolution/
// RtlGetVersion) — у которых на другой платформе нет эквивалента ВООБЩЕ;
// каждую такую фичу (загрузка CPU, стартовый адрес потока, разрешение
// таймера, версия ОС) придётся переписывать отдельно под целевую платформу,
// обёртка LoadLibrary/GetProcAddress тут не помогает. См. память
// `windows-dependency-inventory` (число файлов 17 там было переоценено —
// совпадение по grep, не по смыслу; поправлено в `platform-seam-step1-clocks`).
//
// Эта точка — для ОПЦИОНАЛЬНЫХ плагинов по образцу DiscordRPC: на Linux
// вместо LoadLibrary/GetProcAddress/FreeLibrary встанут dlopen/dlsym/dlclose,
// на Switch такой плагин просто не грузится (модуль возвращает nullptr).
// ---------------------------------------------------------------------------

namespace xrPlatform
{
// ⚠имя `UnloadModule`, не `FreeModule` -- `WinBase.h` несёт
// `#define FreeModule(h) FreeLibrary((h))` (наследие Win16), и объявление
// молча превращается в `FreeLibrary`, конфликтуя с настоящим WinAPI-именем;
// ошибка компилятора при этом указывает на СОВСЕМ другую сигнатуру.
// nullptr при неудаче (файла нет / не экспортирует нужные символы и т.п.).
XRCORE_API void* LoadModule(const char* path);
XRCORE_API void* GetSymbol(void* mod, const char* name);
XRCORE_API void  UnloadModule(void* mod);
} // namespace xrPlatform

#include "stdafx.h"

#if 0 //__cpp_lib_stacktrace

#include <stacktrace>

std::string BuildStackTrace(const char* header) { return header + std::to_string(std::stacktrace::current()); }

std::string BuildStackTrace(const char* header, PCONTEXT) { return BuildStackTrace(header); }

#elif defined(_MSC_VER)

#include <sstream>

#pragma warning(push)
#pragma warning(disable : 4091) // 'typedef ': ignored on left of '' when no variable is declared
#include <DbgHelp.h>
#pragma warning(pop)

#pragma comment(lib, "dbghelp.lib")

constexpr u16 MaxFuncNameLength = 4096, maxFramesCount = 512;

static bool symEngineInitialized{};
static std::mutex dbghelpMutex;   // dbghelp однопоточен -- один замок на весь файл

class ScopedSetWorkDir
{
    string_path WorkingDirSaved{};

public:
    ScopedSetWorkDir()
    {
        GetCurrentDirectory(sizeof(WorkingDirSaved), WorkingDirSaved);

        string_path ApplicationDir{}, fn{}, dr{}, di{};
        GetModuleFileName(nullptr, fn, sizeof(fn));
        _splitpath(fn, dr, di, nullptr, nullptr);
        xr_strconcat(ApplicationDir, dr, di);
        // Рабочий каталог для процесса надо устанавливать принудительно
        // в папку с движком, независимо откуда запустили. Иначе начинаются
        // чудеса типа игнорирования движком символов для стектрейсинга.
        SetCurrentDirectory(ApplicationDir);
        // Msg("~~[%s] Set workdir to [%s]", __FUNCTION__, ApplicationDir);
    }
    ~ScopedSetWorkDir()
    {
        SetCurrentDirectory(WorkingDirSaved);
        // Msg("--[%s] Reset workdir back to [%s]", __FUNCTION__, WorkingDirSaved);
    }
};

static bool InitializeSymbolEngine()
{
    if (!symEngineInitialized)
    {
        DWORD dwOptions = SymGetOptions();
        SymSetOptions(dwOptions | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

        if (SymInitialize(GetCurrentProcess(), nullptr, TRUE))
            symEngineInitialized = true;
    }

    return symEngineInitialized;
}

static void DeinitializeSymbolEngine()
{
    if (symEngineInitialized)
    {
        SymCleanup(GetCurrentProcess());

        symEngineInitialized = false;
    }
}

#ifdef _M_X64
#define MACHINE_TYPE IMAGE_FILE_MACHINE_AMD64
#else
#define MACHINE_TYPE IMAGE_FILE_MACHINE_I386
#endif

std::string BuildStackTrace(const char* header, PCONTEXT threadCtx)
{
    std::scoped_lock<decltype(dbghelpMutex)> lock(dbghelpMutex);

    ScopedSetWorkDir dbg_workdir{};

    std::stringstream traceResult;
    traceResult << header;

    if (!InitializeSymbolEngine())
    {
        const auto LastErr = GetLastError();
        traceResult << "[" << __FUNCTION__ << "] InitializeSymbolEngine failed with error: [" << LastErr << "], descr: [" << Debug.error2string(LastErr) << "]";
        return traceResult.str();
    }

    STACKFRAME stackFrame{};
#ifdef _M_X64
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrPC.Offset = threadCtx->Rip;
    stackFrame.AddrStack.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = threadCtx->Rsp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = threadCtx->Rbp;
#else
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrPC.Offset = threadCtx->Eip;
    stackFrame.AddrStack.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = threadCtx->Esp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = threadCtx->Ebp;
#endif
    auto lpstackFrame = reinterpret_cast<LPSTACKFRAME>(&stackFrame);

    u16 count_frames{};
    while (count_frames++ <= maxFramesCount)
    {
        BOOL result = StackWalk(MACHINE_TYPE, GetCurrentProcess(), GetCurrentThread(), lpstackFrame, threadCtx, nullptr, SymFunctionTableAccess, SymGetModuleBase, nullptr);

        if (!result || lpstackFrame->AddrPC.Offset == 0)
            break;

        // Module name
        IMAGEHLP_MODULE moduleInfo{};
        moduleInfo.SizeOfStruct = sizeof(moduleInfo);

        result = SymGetModuleInfo(GetCurrentProcess(), lpstackFrame->AddrPC.Offset, &moduleInfo);

        if (result)
            traceResult << "Module: [" << moduleInfo.ImageName << "]";

        // Address
        traceResult << ", AddrPC.Offset: [" << reinterpret_cast<const void*>(lpstackFrame->AddrPC.Offset) << "]";

        // Function info
        BYTE arrSymBuffer[MaxFuncNameLength]{};
        auto functionInfo = reinterpret_cast<PIMAGEHLP_SYMBOL>(&arrSymBuffer);
        functionInfo->SizeOfStruct = sizeof(*functionInfo);
        functionInfo->MaxNameLength = sizeof(arrSymBuffer) - offsetof(IMAGEHLP_SYMBOL, Name);
        DWORD_PTR dwFunctionOffset{};

        result = SymGetSymFromAddr(GetCurrentProcess(), lpstackFrame->AddrPC.Offset, &dwFunctionOffset, functionInfo);

        if (result)
        {
            traceResult << ", Fun: [" << functionInfo->Name << "()]";
            if (dwFunctionOffset)
                traceResult << " + [" << dwFunctionOffset << " byte(s)]";
        }

        // Source info
        DWORD dwLineOffset{};
        IMAGEHLP_LINE sourceInfo{};
        sourceInfo.SizeOfStruct = sizeof(sourceInfo);

        result = SymGetLineFromAddr(GetCurrentProcess(), lpstackFrame->AddrPC.Offset, &dwLineOffset, &sourceInfo);

        if (result)
        {
            traceResult << ", File-->Line: [" << sourceInfo.FileName << "-->" << sourceInfo.LineNumber << "]";
            if (dwLineOffset)
                traceResult << " + [" << dwLineOffset << " byte(s)]";
        }

        traceResult << "\n";
    }

    DeinitializeSymbolEngine();

    return traceResult.str();
}

// ---------------------------------------------------------------------------
// ⭐РАЗРЕШЕНИЕ ОДНОГО АДРЕСА В ИМЯ (см. заголовок).
//
// Отличие от BuildStackTrace: здесь НЕТ обхода стека -- адрес уже снят на
// горячем пути через `_ReturnAddress()`, который бесплатен. И символьный движок
// НЕ выключается после вызова: отчёт разрешает десятки адресов подряд, а
// SymInitialize по каждому из них перечитывал бы .pdb заново.
// ⚠dbghelp однопоточен -- замок общий с обходчиком стека, иначе два отчёта
// одновременно портят его внутреннее состояние.
// ---------------------------------------------------------------------------
XRCORE_API std::string ResolveCodeAddress(const void* addr)
{
    if (!addr)
        return "<null>";

    std::scoped_lock<decltype(dbghelpMutex)> lock(dbghelpMutex);

    ScopedSetWorkDir dbg_workdir{};

    std::stringstream out;

    if (!InitializeSymbolEngine())
    {
        out << addr;
        return out.str();
    }

    const DWORD64 pc = reinterpret_cast<DWORD64>(addr);

    BYTE arrSymBuffer[MaxFuncNameLength]{};
    auto functionInfo = reinterpret_cast<PIMAGEHLP_SYMBOL>(&arrSymBuffer);
    functionInfo->SizeOfStruct = sizeof(*functionInfo);
    functionInfo->MaxNameLength = sizeof(arrSymBuffer) - offsetof(IMAGEHLP_SYMBOL, Name);
    DWORD_PTR dwFunctionOffset{};

    if (SymGetSymFromAddr(GetCurrentProcess(), pc, &dwFunctionOffset, functionInfo))
        out << functionInfo->Name;
    else
        out << addr;

    DWORD dwLineOffset{};
    IMAGEHLP_LINE sourceInfo{};
    sourceInfo.SizeOfStruct = sizeof(sourceInfo);
    if (SymGetLineFromAddr(GetCurrentProcess(), pc, &dwLineOffset, &sourceInfo))
    {
        const char* slash = strrchr(sourceInfo.FileName, 0x5C);
        out << " (" << (slash ? slash + 1 : sourceInfo.FileName) << ":" << sourceInfo.LineNumber << ")";
    }

    return out.str();
}

std::string BuildStackTrace(const char* header)
{
    CONTEXT currentThreadCtx{};

    RtlCaptureContext(&currentThreadCtx); // GetThreadContext cann't be used on the current thread
    currentThreadCtx.ContextFlags = CONTEXT_FULL;

    return BuildStackTrace(header, &currentThreadCtx);
}

#else // !_MSC_VER, real port (not a stub): glibc backtrace()/dladdr(), no
      // external dependency (libunwind не нужен -- есть в glibc из коробки).

#include <sstream>
#ifdef __SWITCH__
#include "xr_platform_nx_backtrace.h" // Horizon: no backtrace(); capture yields no frames
#elif defined(__ANDROID__)
#include "xr_platform_android_backtrace.h" // bionic has backtrace() only from API 33
#else
#include <execinfo.h>
#endif
#ifndef __SWITCH__
#include <dlfcn.h> // dladdr; Horizon has neither
#endif
#include <cxxabi.h>
#include <memory>

constexpr int maxFramesCount = 512;

// ⭐dladdr() резолвит адрес по таблице символов ДИНАМИЧЕСКОГО линкера --
// прямой POSIX-аналог SymGetSymFromAddr/SymGetModuleInfo, без отдельного
// SymInitialize/SymCleanup (таблица уже загружена рантаймом). Демангл через
// __cxa_demangle -- тот же эффект, что SYMOPT_UNDNAME на Windows-ветке.
// ⚠Честное ограничение: dladdr видит только ЭКСПОРТИРУЕМЫЕ (динамические)
// символы, а без DWARF-читалки (libunwind даёт только раскрутку стека, не
// файл:строку) нет File-->Line, в отличие от dbghelp+PDB на Windows -- как и
// там, отсутствие результата уже штатно обрабатывается вызывающим кодом
// (см. `if (result)` вокруг File-->Line ниже по Windows-ветке).
#ifdef __SWITCH__
extern "C" int xr_nx_describe_addr(const void* addr, char* buf, unsigned size); // xr_3da/nx/xr_nx_prof.cpp
#endif
static std::string resolve_one(const void* addr)
{
    std::stringstream out;

#ifdef __SWITCH__
    // no dladdr on Horizon: an offset into the NRO's .text (xr_nx_prof.cpp), for addr2line against the build's .elf
    char buf[64];
    xr_nx_describe_addr(addr, buf, sizeof(buf));
    out << buf;
    return out.str();
#else
    Dl_info info{};
    if (dladdr(addr, &info) && info.dli_sname)
    {
        int status = 0;
        std::unique_ptr<char, void (*)(void*)> demangled(abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status), std::free);
        const char* name = (status == 0 && demangled) ? demangled.get() : info.dli_sname;

        out << "Fun: [" << name << "()]";
        if (info.dli_saddr)
        {
            const uintptr_t off = (uintptr_t)addr - (uintptr_t)info.dli_saddr;
            if (off)
                out << " + [" << off << " byte(s)]";
        }
        if (info.dli_fname)
            out << ", Module: [" << info.dli_fname << "]";
    }
    else
    {
        out << addr;
    }

    return out.str();
#endif
}

std::string BuildStackTrace(const char* header)
{
    std::stringstream traceResult;
    traceResult << header;

    void* frames[maxFramesCount];
    const int n = backtrace(frames, maxFramesCount);

    for (int i = 0; i < n; i++)
        traceResult << "AddrPC.Offset: [" << frames[i] << "], " << resolve_one(frames[i]) << "\n";

    return traceResult.str();
}

// ⚠Windows-ветка вызывает эту перегрузку ТОЛЬКО из обработчика структурного
// исключения (SEH) упавшего потока -- своего PCONTEXT у POSIX-модели
// исключений нет (см. xr_platform_seh.h/win32_shim.h про PCONTEXT-заглушку),
// а раскрутка ЧУЖОГО работающего потока без его приостановки невозможна и на
// Windows тоже (тем и продиктован сам SEH-путь) -- падаем на раскрутку стека
// ВЫЗЫВАЮЩЕГО потока, то же честное ограничение, не регресс относительно
// Windows-ветки.
std::string BuildStackTrace(const char* header, PCONTEXT) { return BuildStackTrace(header); }

XRCORE_API std::string ResolveCodeAddress(const void* addr)
{
    if (!addr)
        return "<null>";

    return resolve_one(addr);
}

#endif

#include "stdafx.h"
#include <system_error>

#include "xrDebug.h"
#include "xr_platform_seh.h"

#ifdef _MSC_VER
#include <new.h> // for _set_new_mode
#include <comdef.h> // for _com_error
#endif
#include <signal.h> // for signals

XRCORE_API xrDebug Debug;
XRCORE_API HWND gGameWindow = nullptr;
XRCORE_API bool ExitFromWinMain = false;

static bool error_after_dialog = false;

#include "stacktrace_collector.h"
#include <sstream>
#ifdef _MSC_VER
#include <VersionHelpers.h>
#include <shellapi.h>
#else
#include <pthread.h>
#include <cstdio>
#endif

static bool open_log()
{
#ifdef _MSC_VER
    if (logFName[0])
    {
        ShellExecute(nullptr, "open", logFName, nullptr, nullptr, SW_SHOW);
        return true;
    }
    return false;
#else
    // ⚠xrCore не знает об оконной/UI-системе движка (та лежит выше, в
    // xr_3da) -- честно не открываем внешний просмотрщик лога здесь вместо
    // угадывания какого-то конкретного, лог и так уже на диске.
    return false;
#endif
}

extern bool OnMainThread();

static void ShowErrorMessage(const char* msg, const bool show_msg = false)
{
    extern void LogRenderDebugMessages();
    LogRenderDebugMessages();

#ifdef _MSC_VER
    const bool on_ttapi_thread = !OnMainThread();

    if (!on_ttapi_thread)
    {
        ShowWindow(gGameWindow, SW_HIDE);

        while (ShowCursor(TRUE) < 0)
            ;
    }

    if (!IsDebuggerPresent())
    {
        if (!show_msg)
            if (open_log())
                return;

        if (!on_ttapi_thread)
            MessageBox(gGameWindow, msg, "FATAL ERROR", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    }
#else
    // ⚠Тот же честный пробел, что и open_log(): нет ни MessageBox, ни окна
    // движка на этом слое -- сообщение уже попало в лог из backend()/
    // gather_info() выше по стеку (xrDebug::backend), это лишь то немногое,
    // что xrCore способно добавить без зависимости на xr_3da.
    (void)show_msg;
    if (!IsDebuggerPresent())
        fprintf(stderr, "FATAL ERROR: %s\n", msg);
#endif
}

static const char* GetThreadName()
{
#ifdef _MSC_VER
    if (IsWindows10OrGreater())
    {
        static HMODULE KernelLib = GetModuleHandle("kernel32.dll");
        using FuncGetThreadDescription = HRESULT (*)(HANDLE, PWSTR*);
        static auto pGetThreadDescription = (FuncGetThreadDescription)GetProcAddress(KernelLib, "GetThreadDescription");

        if (pGetThreadDescription)
        {
            PWSTR wThreadName = nullptr;
            if (SUCCEEDED(pGetThreadDescription(GetCurrentThread(), &wThreadName)))
            {
                if (wThreadName)
                {
                    static string64 ResThreadName{};
                    WideCharToMultiByte(CP_OEMCP, 0, wThreadName, int(wcslen(wThreadName)), ResThreadName, sizeof(ResThreadName), nullptr, nullptr);
                    LocalFree(wThreadName);
                    if (ResThreadName && strlen(ResThreadName))
                        return ResThreadName;
                }
            }
        }
    }
    return "UNKNOWN";
#else
    // ⭐pthread_getname_np -- прямой POSIX-аналог GetThreadDescription (тот же
    // запрос "имя ПО ХЕНДЛУ текущего потока"), тот же 16-байтный предел
    // (glibc TASK_COMM_LEN), что и запись через pthread_setname_np в
    // _math.cpp -- реальный порт, не заглушка.
    static char name[16];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 && name[0])
        return name;
    return "UNKNOWN";
#endif
}

void LogStackTrace(const char* header, const bool dump_lua_locals)
{
    // __finally здесь всегда было пустым -- держало SEH-фрейм ради целостности
    // цепочки кадров при ручном разборе стека, самого cleanup не было. На
    // MSVC оставлено дословно; на прочих компиляторах пустой __finally не даёт
    // ничего сверх обычного блока (см. xr_platform_seh.h).
#ifdef _MSC_VER
    __try
    {
#endif
        if (auto pCrashHandler = Debug.get_crashhandler())
            pCrashHandler(dump_lua_locals);
        Log("********************************************************************************");
        // ⚠"..." __FUNCTION__ "..." склейка литералов не переносима: на
        // GCC/Clang __FUNCTION__ -- не строковый литерал, а массив (нельзя
        // склеить препроцессором), в отличие от MSVC. %s-стиль работает на
        // обоих компиляторах и уже используется так в остальном xrCore
        // (FS.cpp/xrMemory.cpp и т.д.).
        Msg("!![%s] Thread: [%s]", __FUNCTION__, GetThreadName());
        Log(BuildStackTrace(header));
        Log("********************************************************************************");
#ifdef _MSC_VER
    }
    __finally
    {}
#endif
}

// An access violation names the address and nothing else. A wild pointer, memory
// the allocator already handed back to the OS, and a live PAGE_NOACCESS guard
// page all print the same way in a stack trace -- and they have three different
// fixes. VirtualQuery separates them in one line, at the moment of the fault.
#ifdef _MSC_VER
static const char* fault_protect_name(DWORD p)
{
    switch (p & 0xFF)
    {
    case 0: return "---";
    case PAGE_NOACCESS: return "NOACCESS";
    case PAGE_READONLY: return "R";
    case PAGE_READWRITE: return "RW";
    case PAGE_WRITECOPY: return "WC";
    case PAGE_EXECUTE: return "X";
    case PAGE_EXECUTE_READ: return "RX";
    case PAGE_EXECUTE_READWRITE: return "RWX";
    case PAGE_EXECUTE_WRITECOPY: return "XWC";
    default: return "?";
    }
}

static void LogFaultAddressInfo(_EXCEPTION_POINTERS* pExceptionInfo)
{
    if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord)
        return;

    const EXCEPTION_RECORD& er = *pExceptionInfo->ExceptionRecord;
    if (er.ExceptionCode != EXCEPTION_ACCESS_VIOLATION && er.ExceptionCode != EXCEPTION_IN_PAGE_ERROR)
        return;
    if (er.NumberParameters < 2)
        return;

    const ULONG_PTR kind = er.ExceptionInformation[0];
    void* const addr = (void*)er.ExceptionInformation[1];
    const char* how = (kind == 0) ? "READ" : (kind == 1) ? "WRITE" : (kind == 8) ? "EXECUTE(DEP)" : "?";

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
    {
        Msg("!![fault] %s at 0x%p -- VirtualQuery failed, the address is not mapped in this process", how, addr);
        return;
    }

    const char* state = (mbi.State == MEM_COMMIT) ? "COMMIT" : (mbi.State == MEM_RESERVE) ? "RESERVE" : (mbi.State == MEM_FREE) ? "FREE" : "?";
    const char* type = (mbi.Type == MEM_PRIVATE) ? "PRIVATE" : (mbi.Type == MEM_MAPPED) ? "MAPPED" : (mbi.Type == MEM_IMAGE) ? "IMAGE" : "-";

    Msg("!![fault] %s at 0x%p | region 0x%p + 0x%llx | state=%s protect=%s (allocated as %s) type=%s | allocBase 0x%p", how, addr, mbi.BaseAddress,
        (unsigned long long)mbi.RegionSize, state, fault_protect_name(mbi.Protect), fault_protect_name(mbi.AllocationProtect), type, mbi.AllocationBase);

    // The two readings that matter, spelled out so the log needs no interpreter.
    if (mbi.State == MEM_COMMIT && (mbi.Protect & 0xFF) == PAGE_NOACCESS)
        Msg("!![fault]   -> COMMITTED but PAGE_NOACCESS: this is a live guard page, not a stray pointer");
    else if (mbi.State != MEM_COMMIT)
        Msg("!![fault]   -> not committed: the pointer outlived the memory it names");
}
#endif

void LogStackTrace(const char* header, _EXCEPTION_POINTERS* pExceptionInfo, bool dump_lua_locals)
{
    // См. комментарий у одноимённой перегрузки выше -- __finally пуст.
#ifdef _MSC_VER
    __try
    {
#endif
        if (auto pCrashHandler = Debug.get_crashhandler())
            pCrashHandler(dump_lua_locals);
        Log("********************************************************************************");
#ifdef _MSC_VER
        Msg("!![" __FUNCTION__ "] Thread: [%s], ExceptionCode: [%x]", GetThreadName(), pExceptionInfo->ExceptionRecord->ExceptionCode);
        LogFaultAddressInfo(pExceptionInfo);
        auto save = *pExceptionInfo->ContextRecord;
        Log(BuildStackTrace(header, pExceptionInfo->ContextRecord));
        *pExceptionInfo->ContextRecord = save;
#else
        // ⚠SEH не существует на GCC/Clang (xr_platform_seh.h) -- вызывающий
        // __except-фильтр, который строит настоящий _EXCEPTION_POINTERS,
        // сам не существует на этой платформе, так что этот путь мёртв
        // архитектурно, а не по недоделке (см. EXCEPTION_RECORD-заглушку в
        // xr_platform_win32_shim.h). Оставлено компилируемым, потому что
        // объявление в xrDebug.h разделяется с xrGame-файлами вне этого
        // модуля (SEH-обёртки CLevel::OnFrame и т.п.) -- падаем на ту же
        // раскрутку ВЫЗЫВАЮЩЕГО потока, что и BuildStackTrace(header,
        // PCONTEXT) в stacktrace_collector.cpp.
        (void)pExceptionInfo;
        Msg("!![%s] Thread: [%s]", __FUNCTION__, GetThreadName());
        Log(BuildStackTrace(header));
#endif
        Log("********************************************************************************");
#ifdef _MSC_VER
    }
    __finally
    {}
#endif
}

// ⭐⭐THE DIAGNOSTIC WAS THE STUTTER.
//
// Reported from play 30-07-2026: "когда я убил сталкера и когда меня убили
// какой-то статтер поймал". The stall probe named it in one line -- 518 ms,
// client side, all of it inside frame_move -- and the log says exactly what
// spent it:
//
//   13:41:39.461  [lua_pcall_failed] _g.script:268: attempt to index a nil value
//   13:41:39.462  [LogStackTrace] ExceptionCode: [e06d7363]
//   13:41:39.970  [CLevel::OnFrame] faulted (total 1)
//
// Half a second between the filter starting and the handler running, and it is
// all symbolisation. The mod's Lua error is a genuine defect, but it is a MOD
// defect; what the engine contributed was spending half a frame-second
// reporting it, in the middle of the frame, while the player watched.
//
// ⭐0xE06D7363 is the MSVC code for a THROWN C++ exception -- which is what a
// script error is here. For those the SEH context walk has nothing useful to
// say (in the log above it printed an empty trace), and the message that does
// have something to say -- the Lua error and its file:line -- has already been
// printed by the script engine. So they cost one line now.
//
// ⚠Hardware faults keep the full treatment: an access violation inside a frame
// is the thing this wrapper was built to catch, it is not supposed to happen,
// and its stack is the whole point (see the note over CLevel::OnFrame). Those
// are rate-limited rather than silenced -- the first few dump everything, and a
// storm of them cannot turn the log into the outage.
LONG DbgLogExceptionFilter(const char* header, _EXCEPTION_POINTERS* pExceptionInfo)
{
    constexpr DWORD k_cpp_exception = 0xE06D7363; // MSVC "throw" -- a script error, not a fault
    constexpr u32 k_max_full_dumps = 8;

    const DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    static u32 s_thrown = 0;
    static u32 s_dumps = 0;

    if (code == k_cpp_exception)
    {
        // One line, and only ever one line. The useful half was printed by
        // whoever threw.
        Msg("!![except] C++ exception passed through %.60s (total %u) -- stack omitted, see the error above it", header, ++s_thrown);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    if (s_dumps >= k_max_full_dumps)
    {
        Msg("!![except] fault %x at %s (total %u) -- stack omitted after %u full dumps", code, header, ++s_dumps, k_max_full_dumps);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    ++s_dumps;
    LogStackTrace(header, pExceptionInfo);

    return EXCEPTION_EXECUTE_HANDLER;
}

void gather_info(const char* expression, const char* description, const char* argument0, const char* argument1, const char* file, int line, const char* function,
                 char* assertion_info)
{
    auto buffer = assertion_info;
    auto endline = "\n";
    auto prefix = "[error]";
    bool extended_description = (description && !argument0 && strchr(description, '\n'));
    for (int i = 0; i < 2; ++i)
    {
        if (!i)
            buffer += sprintf(buffer, "%sFATAL ERROR%s%s", endline, endline, endline);
        buffer += sprintf(buffer, "%sExpression    : %s%s", prefix, expression, endline);
        buffer += sprintf(buffer, "%sFunction      : %s%s", prefix, function, endline);
        buffer += sprintf(buffer, "%sFile          : %s%s", prefix, file, endline);
        buffer += sprintf(buffer, "%sLine          : %d%s", prefix, line, endline);

        if (extended_description)
        {
            buffer += sprintf(buffer, "%s%s%s", endline, description, endline);
        }
        else
        {
            buffer += sprintf(buffer, "%sDescription   : %s%s", prefix, description, endline);
            if (argument0)
            {
                if (argument1)
                {
                    buffer += sprintf(buffer, "%sArgument 0    : %s%s", prefix, argument0, endline);
                    buffer += sprintf(buffer, "%sArgument 1    : %s%s", prefix, argument1, endline);
                }
                else
                    buffer += sprintf(buffer, "%sArguments     : %s%s", prefix, argument0, endline);
            }
        }

        buffer += sprintf(buffer, "%s", endline);
        if (!i)
        {
            Log(assertion_info);
            buffer = assertion_info;
            endline = "\r\n";
            prefix = "";
        }
    }

    buffer += sprintf(buffer, "See log file and minidump for detailed information\r\n");
    LogStackTrace("!!stack trace:\n");
}

void xrDebug::do_exit(const std::string& message)
{
    ShowErrorMessage(message.c_str(), true);

    if (!IsDebuggerPresent())
        quick_exit(EXIT_SUCCESS);
    else
        DEBUG_INVOKE;
}

void xrDebug::backend(const char* expression, const char* description, const char* argument0, const char* argument1, const char* file, int line, const char* function)
{
    static std::recursive_mutex CS;
    std::scoped_lock<decltype(CS)> lock(CS);

    string4096 assertion_info;
    gather_info(expression, description, argument0, argument1, file, line, function, assertion_info);

    // KRodin: save_mini_dump(nullptr) здесь не работает - происходит исключение внутри функции save_mini_dump(). Если сильно надо будет тут получать минидампы - придумать другой способ.
    auto endline = "\r\n";
    auto buffer = assertion_info + xr_strlen(assertion_info);
    buffer += sprintf(buffer, "%sPress OK to abort execution%s", endline, endline);

    error_after_dialog = true;

    // quick_exit below runs no static destructors, so anything still sitting in the
    // log stream buffer would be lost -- and this is exactly the log somebody will
    // read afterwards (see XROS_LOG_FLUSH in log.cpp).
    LogFlushNow();

    ShowErrorMessage(assertion_info);

    if (!IsDebuggerPresent())
        quick_exit(EXIT_SUCCESS);
    else
        DEBUG_INVOKE;
}

#ifdef _MSC_VER
const char* xrDebug::DXerror2string(const HRESULT code) const { return _com_error{code}.ErrorMessage(); }
#else
// ⚠Нет COM на POSIX (и нет D3D в Vulkan-only ветке движка -- см. память
// ogsr-cop-edition-is-the-same-engine) -- честный шестнадцатеричный код
// вместо придуманного текста COM-сообщения.
const char* xrDebug::DXerror2string(const HRESULT code) const
{
    static string64 desc_storage;
    snprintf(desc_storage, sizeof(desc_storage), "HRESULT 0x%08X", (unsigned)code);
    return desc_storage;
}
#endif

const char* xrDebug::error2string(const DWORD code) const
{
    static string1024 desc_storage;
#ifdef _MSC_VER
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, code, 0, desc_storage, sizeof(desc_storage) - 1, 0);
#else
    // ⭐Прямое соответствие: GetLastError() на этой платформе -- это errno
    // (xr_platform_win32_shim.h), поэтому strerror -- не приближение, а тот
    // же самый код, который реально был установлен.
    strncpy(desc_storage, strerror((int)code), sizeof(desc_storage) - 1);
    desc_storage[sizeof(desc_storage) - 1] = 0;
#endif
    return desc_storage;
}

void xrDebug::error(const HRESULT hr, const char* expr, const char* file, int line, const char* function) { backend(DXerror2string(hr), expr, 0, 0, file, line, function); }

void xrDebug::error(const HRESULT hr, const char* expr, const char* e2, const char* file, int line, const char* function)
{
    backend(DXerror2string(hr), expr, e2, 0, file, line, function);
}

void xrDebug::fail(const char* e1, const char* file, int line, const char* function) { backend("assertion failed", e1, 0, 0, file, line, function); }

void xrDebug::fail(const char* e1, const std::string& e2, const char* file, int line, const char* function) { backend(e1, e2.c_str(), 0, 0, file, line, function); }

void xrDebug::fail(const char* e1, const char* e2, const char* file, int line, const char* function) { backend(e1, e2, 0, 0, file, line, function); }

void xrDebug::fail(const char* e1, const char* e2, const char* e3, const char* file, int line, const char* function) { backend(e1, e2, e3, 0, file, line, function); }

void xrDebug::fail(const char* e1, const char* e2, const char* e3, const char* e4, const char* file, int line, const char* function)
{
    backend(e1, e2, e3, e4, file, line, function);
}

void __cdecl xrDebug::fatal(const char* file, int line, const char* function, const char* F, ...)
{
    string4096 strBuf;
    va_list args;
    va_start(args, F);
    std::vsnprintf(strBuf, sizeof(strBuf), F, args);
    va_end(args);

    backend("FATAL ERROR", strBuf, nullptr, nullptr, file, line, function);
}

void xrDebug::on_exception_in_thread()
{
    if (!IsDebuggerPresent())
    {
        open_log();

        quick_exit(EXIT_SUCCESS);
    }
    else
    {
        DEBUG_INVOKE;
    }
}

#ifdef _MSC_VER
static int out_of_memory_handler(size_t size)
{
    //Memory.mem_compact();
    size_t process_heap = mem_usage_impl(nullptr, nullptr);
    u32 eco_strings = g_pStringContainer->stat_economy();
    u32 eco_smem = g_pSharedMemoryContainer->stat_economy();
    Msg("* [x-ray]: process heap[%d K]", process_heap / 1024);
    Msg("* [x-ray]: economy: strings[%d K], smem[%d K]", eco_strings / 1024, eco_smem);
    FATAL("Out of memory. Memory request: [%d K]", size / 1024);
    return 1;
}
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4091) // 'typedef ': ignored on left of '' when no variable is declared
#include <DbgHelp.h>
#pragma warning(pop)

#pragma comment(lib, "Version.lib")
#pragma comment(lib, "dbghelp.lib")

static void save_mini_dump(_EXCEPTION_POINTERS* pExceptionInfo)
{
    XR_SEH_TRY
    {
        string_path szDumpPath;
        string64 t_stemp;

        timestamp(t_stemp);
        strcpy_s(szDumpPath, Core.ApplicationName);
        strcat_s(szDumpPath, "_");
        strcat_s(szDumpPath, Core.UserName);
        strcat_s(szDumpPath, "_");
        strcat_s(szDumpPath, t_stemp);
        strcat_s(szDumpPath, ".mdmp");

        XR_SEH_TRY
        {
            if (FS.path_exist("$logs$"))
                FS.update_path(szDumpPath, "$logs$", szDumpPath);
        }
        XR_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            string_path temp;
            strcpy_s(temp, szDumpPath);
            strcpy_s(szDumpPath, "logs/");
            strcat_s(szDumpPath, temp);
        }

        // create the file
        auto hFile = ::CreateFile(szDumpPath, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (INVALID_HANDLE_VALUE == hFile)
        {
            // try to place into current directory
            MoveMemory(szDumpPath, szDumpPath + 5, strlen(szDumpPath));
            hFile = ::CreateFile(szDumpPath, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (hFile != INVALID_HANDLE_VALUE)
        {
            _MINIDUMP_EXCEPTION_INFORMATION ExInfo;

            ExInfo.ThreadId = ::GetCurrentThreadId();
            ExInfo.ExceptionPointers = pExceptionInfo;
            ExInfo.ClientPointers = NULL;

            // write the dump
            auto dump_flags = MINIDUMP_TYPE(MiniDumpNormal | MiniDumpFilterMemory | MiniDumpScanMemory | MiniDumpWithFullMemoryInfo);

            BOOL bOK = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dump_flags, &ExInfo, nullptr, nullptr);
            if (bOK)
                Msg("--Saved dump file to [%s]", szDumpPath);
            else
                Msg("!!Failed to save dump file to [%s] (error [%s])", szDumpPath, Debug.error2string(GetLastError()));

            ::CloseHandle(hFile);
        }
        else
        {
            Msg("!!Failed to create dump file [%s] (error [%s])", szDumpPath, Debug.error2string(GetLastError()));
        }
    }
    XR_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Exception catched in function [" __FUNCTION__ "]");
    }
}
#endif

#ifdef _MSC_VER
static void format_message(char* buffer, const size_t& buffer_size)
{
    XR_SEH_TRY
    {
        auto error_code = GetLastError();
        if (error_code == ERROR_SUCCESS)
        {
            *buffer = 0;
            return;
        }

        void* message = nullptr;
        FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, nullptr, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (char*)&message, 0, nullptr);

        sprintf(buffer, "[error][%8d] : [%s]", error_code, (char*)message);
        LocalFree(message);
    }
    XR_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Exception catched in function [" __FUNCTION__ "]");
    }
}

static LONG WINAPI UnhandledFilter(_EXCEPTION_POINTERS* pExceptionInfo)
{
    if (!error_after_dialog)
    {
        string1024 error_message;
        format_message(error_message, sizeof(error_message));
        if (*error_message)
            Msg("\n%s", error_message);

        LogStackTrace("!!Unhandled exception stack trace:\n", pExceptionInfo, true);

        ShowErrorMessage("Fatal error occured\n\nPress OK to abort program execution");
    }

    save_mini_dump(pExceptionInfo);

    return EXCEPTION_EXECUTE_HANDLER;
}
#else
// ⭐Реальный редизайн (не построчный перевод) вместо SetUnhandledExceptionFilter:
// на POSIX аппаратные ошибки (SIGSEGV/SIGBUS -- то, что на Windows приходит
// как EXCEPTION_ACCESS_VIOLATION в UnhandledFilter) ловятся отдельным
// sigaction-обработчиком, не этой SEH-специфичной функцией -- см.
// posix_fatal_signal_handler ниже и её установку в xrDebug::_initialize().
// ⚠Не async-signal-safe в строгом смысле (Msg/Log используют мьютексы и
// буферизацию) -- тот же компромисс, что и у остального crash-пути этого
// файла: цель успеть напечатать причину падения в подавляющем большинстве
// случаев, а не 100%-гарантия совместимости с сигнальным контекстом.
static void posix_fatal_signal_handler(int sig)
{
    const char* name = (sig == SIGSEGV) ? "SIGSEGV" : (sig == SIGBUS) ? "SIGBUS" : "SIGNAL";
    Msg("!![signal] fatal %s in thread [%s]", name, GetThreadName());
    Log(BuildStackTrace("!!fatal signal stack trace:\n"));
    LogFlushNow();

    ShowErrorMessage("Fatal error occured\n\nPress OK to abort program execution");

    signal(sig, SIG_DFL);
    raise(sig); // re-raise so the process dies the normal way (correct exit status, core dump if enabled)
}
#endif

static void _terminate() //Вызывается при std::terminate()
{
    // What was thrown? Without this the log only says "Unexpected application termination".
    static char why[512];
    why[0] = 0;
    if (std::current_exception())
    {
        try
        {
            throw;
        }
        catch (const std::system_error& e)
        {
            snprintf(why, sizeof(why), "uncaught std::system_error: code=%d category=%s what=%s", e.code().value(), e.code().category().name(), e.what());
        }
        catch (const std::exception& e)
        {
            snprintf(why, sizeof(why), "uncaught std::exception: %s", e.what());
        }
        catch (...)
        {
            snprintf(why, sizeof(why), "uncaught exception of unknown type");
        }
    }
    else
        snprintf(why, sizeof(why), "terminate() without an active exception (abort/noexcept/pure virtual)");
    Msg("!![terminate] %s", why);
    LogFlushNow();
    fprintf(stderr, "[terminate] %s\n", why);
    Debug.backend("<no expression>", why[0] ? why : "Unexpected application termination", nullptr, nullptr, DEBUG_INFO);
}

static void handler_base(const char* reason_string) { Debug.backend("error handler is invoked!", reason_string, nullptr, nullptr, DEBUG_INFO); }

#ifdef _MSC_VER
static void invalid_parameter_handler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t reserved)
{
    string4096 expression_;
    string4096 function_;
    string4096 file_;
    size_t converted_chars = 0;

    if (expression)
        wcstombs_s(&converted_chars, expression_, sizeof(expression_), expression, (wcslen(expression) + 1) * 2 * sizeof(char));
    else
        strcpy_s(expression_, "");

    if (function)
        wcstombs_s(&converted_chars, function_, sizeof(function_), function, (wcslen(function) + 1) * 2 * sizeof(char));
    else
        strcpy_s(function_, __FUNCTION__);

    if (file)
        wcstombs_s(&converted_chars, file_, sizeof(file_), file, (wcslen(file) + 1) * 2 * sizeof(char));
    else
    {
        line = __LINE__;
        strcpy_s(file_, __FILE__);
    }

    Debug.backend("error handler is invoked!", expression_, nullptr, nullptr, file_, line, function_);
}
#endif

static void std_out_of_memory_handler() { handler_base("std: out of memory"); }

static void pure_call_handler() { handler_base("pure virtual function call"); }

static void abort_handler(int signal) { handler_base("application is aborting"); }

static void floating_point_handler(int signal) { handler_base("floating point error"); }

static void illegal_instruction_handler(int signal) { handler_base("illegal instruction"); }

static void termination_handler(int signal) { handler_base("termination with exit code 3"); }

void xrDebug::_initialize()
{
    std::atexit([] { R_ASSERT(ExitFromWinMain, "Unexpected application exit!"); });

    std::set_terminate(_terminate);

#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    signal(SIGABRT, abort_handler);
#ifdef _MSC_VER
    signal(SIGABRT_COMPAT, abort_handler);
#endif
    signal(SIGFPE, floating_point_handler);
    signal(SIGILL, illegal_instruction_handler);
    // signal(SIGSEGV, segment_violation);
    signal(SIGINT, 0);
    signal(SIGTERM, termination_handler);

#ifdef _MSC_VER
    _set_invalid_parameter_handler(&invalid_parameter_handler);

    _set_new_mode(1);
    _set_new_handler(&out_of_memory_handler);
#endif
    std::set_new_handler(&std_out_of_memory_handler);

#ifdef _MSC_VER
    _set_purecall_handler(&pure_call_handler);

    ::SetUnhandledExceptionFilter(UnhandledFilter);
#else
    // ⭐Реальная замена SetUnhandledExceptionFilter -- см. комментарий у
    // posix_fatal_signal_handler выше. _set_purecall_handler не имеет
    // POSIX-аналога (GCC/Clang зовут __cxa_pure_virtual, который сам
    // абортит без хука на уровне рантайма) -- честный пробел, а не
    // регресс: вызов чистой виртуальной функции всё так же валит процесс,
    // просто минуя handler_base()/Debug.backend().
#ifndef __ANDROID__ // Android: debuggerd's own handler writes a tombstone with the full native stack (adb logcat -s DEBUG)
    signal(SIGSEGV, posix_fatal_signal_handler);
    signal(SIGBUS, posix_fatal_signal_handler);
#endif
#endif

    //PreventSetUnhandledExceptionFilter();

    // Выключаем окно "Прекращена работа программы...". У нас своё окно для сообщений об ошибках есть.
    // SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
}

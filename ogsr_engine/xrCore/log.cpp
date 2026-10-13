#include "stdafx.h"

#include "log.h"
#include "load_prof.h" // [frame prof]/[rt prof]: when a Msg itself is the slow part

#include <fstream> //для std::ofstream
#include <iomanip> //для std::strftime
#include <array> //для std::array
#include <iostream>
#include <atomic>
#include <cstdlib>          // getenv — XROS_LOG_FLUSH
#include <thread>           // the log file written behind (Switch)
#include <chrono>

static LogCallback LogCB = nullptr;
#if defined(__SWITCH__) || defined(__ANDROID__)
void (*g_xr_nx_log_tap)(const char* line) = nullptr; // Android: logcat
#endif
#ifdef __SWITCH__
void (*g_xr_nx_stderr_flush)() = nullptr;
#endif
xr_vector<std::string> LogFile;
static std::ofstream logstream;
string_path logFName{};

// What a level load spends writing its own log: 14926 lines for one pripyat_full
// load, each of them an ofstream write followed by a flush -- i.e. a syscall per
// line, taken on whatever thread called Msg, and never counted by any phase timer
// because it is spread across all of them. Measured here so the argument about it
// is about a number: 6859 lines cost 60 ms of the render phases, 23 of them once
// the flush stopped being per-line. XROS_LOG_FLUSH=1 puts the old behaviour back.
namespace {
std::atomic<u64> g_logLines{0}, g_logClk{0}, g_logWriteClk{0};
#ifdef __SWITCH__
static int g_nx_trace_budget = 40; // first-boot breadcrumbs through Msg/Log/AddOne, then silence
#define NXT(...) do { if (g_nx_trace_budget > 0) { --g_nx_trace_budget; XRNX_TRACE(__VA_ARGS__); } } while (0)
#else
#define NXT(...) do {} while (0)
#endif
u32  g_logSinceFlush = 0;
u64  g_logLastFlushQpc = 0;
#ifdef __SWITCH__
constexpr u32 kLogFlushRun = 512;   // every write is a round trip to the SD card's filesystem service
constexpr u64 kLogFlushDivisor = 4; // 1/4 s
#else
constexpr u32 kLogFlushRun = 64;
constexpr u64 kLogFlushDivisor = 500; // 2 ms
#endif

bool LogFlushEachLine()
{
    static const bool s_each = [] {
        if (const char* e = std::getenv("XROS_LOG_FLUSH")) return atoi(e) != 0;
        return false;
    }();
    return s_each;
}

#ifdef __SWITCH__
// ⭐The log FILE is written behind, like stderr.log (nx/xr_nx_link.c, v203). 📏08-10-2026 Switch v238:
// [frame prof] log.file 13.7-15 ms on the main thread, 6.8 ms on the render thread -- the 64 KB filebuf
// filling, or a flush (every "!" line flushed at once; the scheduler's "! Single item ... took whole
// update frame" alone is ~350 a session), wrote to the SD on whatever thread called Msg. One such Msg
// was the scheduler's 56 ms frame. Now a line is appended to memory under a lock held for the append
// only; a thread on core 2 writes the pending text every 100 ms. LogFlushNow (fatal errors, exit)
// writes it out at once. Over 1 MB pending, lines are dropped and counted rather than waited for.
struct LogBehind
{
    std::mutex mx;        // pending / dropped: held for an append or a swap, never across I/O
    std::string pending;
    u64 dropped = 0;
    std::mutex file_mx;   // the writers of the file among themselves (the thread, LogFlushNow)
    bool started = false;
};
LogBehind g_lb;
constexpr size_t kLogBehindMax = 1u << 20;

void LogBehindWrite()
{
    std::lock_guard<std::mutex> fl(g_lb.file_mx);
    std::string out;
    u64 dropped;
    {
        std::lock_guard<std::mutex> l(g_lb.mx);
        out.swap(g_lb.pending);
        dropped = g_lb.dropped;
        g_lb.dropped = 0;
    }
    if (out.empty() && !dropped)
        return;
    logstream << out;
    if (dropped)
        logstream << "\n[log: " << dropped << " bytes dropped, the pending buffer was full]";
    logstream.flush();
}
}   // namespace

extern "C" u32 svcSetThreadCoreMask(u32 thread, s32 preferred_core, u32 affinity_mask); // libnx
extern "C" u32 svcSetThreadPriority(u32 thread, u32 priority);                          // libnx

// AddOne, under logCS: false = not started yet (the caller writes the line itself, as before).
static bool LogBehindAppend(const std::string& line)
{
    if (!g_lb.started)
    {
        g_lb.started = true; // logCS serialises the first callers
        std::thread([] {
            svcSetThreadCoreMask(0xFFFF8000u /*CUR_THREAD_HANDLE*/, 2, 1u << 2);
            svcSetThreadPriority(0xFFFF8000u, 0x3B);
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                LogBehindWrite();
            }
        }).detach();
    }
    std::lock_guard<std::mutex> l(g_lb.mx);
    if (g_lb.pending.size() + line.size() > kLogBehindMax)
        g_lb.dropped += line.size();
    else
        g_lb.pending += line;
    return true;
}
#else
}   // namespace
#endif

void LogProfGet(u64& lines, u64& clkTotal, u64& clkWrite)
{
    lines    = g_logLines.load(std::memory_order_relaxed);
    clkTotal = g_logClk.load(std::memory_order_relaxed);
    clkWrite = g_logWriteClk.load(std::memory_order_relaxed);
}

void LogFlushNow()
{
#ifdef __SWITCH__
    if (logstream.is_open()) LogBehindWrite(); // the pending text first (it flushes)
#endif
    if (logstream.is_open()) logstream.flush();
    g_logSinceFlush = 0;
#ifdef __SWITCH__
    if (g_xr_nx_stderr_flush)   // stderr.log is written behind (nx/xr_nx_link.c): a fatal error must not lose its tail
        g_xr_nx_stderr_flush();
#endif
}
static void AddOne(std::string& split, bool first_line)
{
    NXT("AddOne: entry (before static mutex)");
    LoadProf::Scope _lpLog("log"); // self time = waiting for logCS + formatting
    static std::recursive_mutex logCS;
    NXT("AddOne: static mutex constructed");
    std::scoped_lock lock(logCS);
    NXT("AddOne: locked");

    const u64 clkEnter = CPU::GetCLK();
#ifdef __SWITCH__
    static int s_nx_n = 0;
    const bool nxt = s_nx_n++ < 4; // breadcrumbs for the first few log lines only
    if (nxt) XRNX_TRACE("AddOne #%d: lock ok, first_line=%d", s_nx_n, (int)first_line);
#endif
    const bool isError = !split.empty() && split.front() == '!';   // before insert_time prefixes it
    g_logLines.fetch_add(1, std::memory_order_relaxed);

    if (IsDebuggerPresent())
    { //Вывод в отладчик студии
        OutputDebugString(split.c_str());
        OutputDebugString("\n");
    }

    if (LogCB)
    { LoadProf::Scope _lp("log.cb"); LogCB(split.c_str()); } //Вывод в логкаллбек
#if defined(__SWITCH__) || defined(__ANDROID__)
    if (g_xr_nx_log_tap)
    { LoadProf::Scope _lp("log.tap"); g_xr_nx_log_tap(split.c_str()); }
#endif

    auto insert_time = [&] {
        if (first_line)
        {
            string64 buf, curTime;
            using namespace std::chrono;
#ifdef __SWITCH__
            if (nxt) XRNX_TRACE("AddOne: system_clock::now...");
#endif
            const auto now = system_clock::now();
            const auto time = system_clock::to_time_t(now);
            const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) - duration_cast<seconds>(now.time_since_epoch());
#ifdef __SWITCH__
            if (nxt) XRNX_TRACE("AddOne: now ok (t=%lld), localtime...", (long long)time);
#endif
            std::strftime(buf, sizeof(buf), "%d.%m.%y %H:%M:%S", std::localtime(&time));
#ifdef __SWITCH__
            if (nxt) XRNX_TRACE("AddOne: localtime+strftime ok");
#endif
            sprintf_s(curTime, "\n[%s.%03lld] [%u] ", buf, ms.count(), _Thrd_id());
            split = curTime + split;
        }
        else
        {
            split = "\n" + split;
        }
    };

    if (!logstream.is_open())
        insert_time();

    static std::string last_str;
    static int items_count;

    //LogFile.push_back(split); //Вывод в консоль

    if (last_str == split)
    {
        std::string tmp = split;

        if (items_count == 0)
            items_count = 2;
        else
            items_count++;

        tmp += " [";
        tmp += std::to_string(items_count);
        tmp += "]";

        LogFile.erase(LogFile.end() - 1);
        LogFile.push_back(tmp);
    }
    else
    {
        // DUMP_PHASE;
        LoadProf::Scope _lp("log.vector"); // all the session's lines; a realloc moves every one
        LogFile.push_back(split);
        last_str = split;
        items_count = 0;
    }


    if (logstream.is_open())
    {
        insert_time();

        //Вывод в лог-файл
        const u64 clkWrite0 = CPU::GetCLK();
        LoadProf::Scope _lp("log.file");
#ifdef __SWITCH__
        if (LogBehindAppend(split))
        {
            g_logWriteClk.fetch_add(CPU::GetCLK() - clkWrite0, std::memory_order_relaxed);
            g_logClk.fetch_add(CPU::GetCLK() - clkEnter, std::memory_order_relaxed);
            return;
        }
#endif
        logstream << split;
        // The flush is what makes the log survive a hard kill (quick_exit in
        // xrDebug::backend runs no destructors -- it calls LogFlushNow instead), and
        // it costs a syscall per line: measured 40 ms of the render phases alone for
        // 6859 lines, ~100 ms over a whole level load. So it is bounded in TIME
        // instead of taken every line: errors flush immediately, everything else at
        // most 2 ms behind. XROS_LOG_FLUSH=1 restores the line-by-line behaviour.
        const u64 now = CPU::QPC();
        if (LogFlushEachLine() || isError || ++g_logSinceFlush >= kLogFlushRun
            || (now - g_logLastFlushQpc) * kLogFlushDivisor > CPU::QPCFreq()) {
            logstream.flush();
            g_logSinceFlush   = 0;
            g_logLastFlushQpc = now;
        }
        g_logWriteClk.fetch_add(CPU::GetCLK() - clkWrite0, std::memory_order_relaxed);
    }
    g_logClk.fetch_add(CPU::GetCLK() - clkEnter, std::memory_order_relaxed);
}

void Log(const std::string& str)
{
    if (str.empty())
        return;

    NXT("Log: enter, len=%zu", str.size());
    bool not_first_line = false;

    constexpr std::array<char, 5> color_codes{'-', '~', '!', '*', '#'}; //Зелёный, Жёлтый, Красный, Серый, Бирюзовый
    const char& color_s = str.front();
    const bool have_color = std::find(color_codes.begin(), color_codes.end(), color_s) != color_codes.end(); //Ищем в начале строки цветовой код

    xr_vector<std::string> substrs;
    size_t beg{};
    for (size_t end{}; (end = str.find("\n", end)) != std::string::npos; ++end) // Разбиваем текст по "\n"
    {
        substrs.push_back(str.substr(beg, end - beg));
        beg = end + 1;
    }
    substrs.push_back(str.substr(beg));

    NXT("Log: split ok, %zu parts", substrs.size());
    for (auto& str : substrs)
    {
        if (not_first_line && have_color)
        {
            str = ' ' + str;
            str = color_s + str; //Если надо, перед каждой строкой вставляем спец-символ цвета, чтобы в консоли цветными были все строки текста, а не только первая.
        }
        AddOne(str, !not_first_line);
        not_first_line = true;
    }
}

void Log(const char* s) { Log(std::string{s}); }

void __cdecl Msg(const char* format, ...)
{
    string4096 strBuf;
    va_list args;
    va_start(args, format);
    NXT("Msg: enter, fmt=%.48s", format);
    int buf_len = std::vsnprintf(strBuf, sizeof(strBuf), format, args);
    NXT("Msg: vsnprintf returned %d", buf_len);
    va_end(args);
    if (buf_len > 0)
        Log(strBuf);
}

/////////////////////////////////////////////////////////////////////////////////////////////
void Log(const char* msg, const Fvector& dop)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s (%f,%f,%f)", msg, dop.x, dop.y, dop.z);
    Log(buf);
}

void Log(const char* msg, const Fmatrix& dop)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s:\n%f,%f,%f,%f\n%f,%f,%f,%f\n%f,%f,%f,%f\n%f,%f,%f,%f\n", msg, dop.i.x, dop.i.y, dop.i.z, dop._14_, dop.j.x, dop.j.y, dop.j.z, dop._24_,
                  dop.k.x, dop.k.y, dop.k.z, dop._34_, dop.c.x, dop.c.y, dop.c.z, dop._44_);
    Log(buf);
}
/////////////////////////////////////////////////////////////////////////////////////////////

void SetLogCB(LogCallback cb) { LogCB = cb; }

void CreateLog(BOOL nl)
{
    if (!nl)
    {
        std::cout.rdbuf(logstream.rdbuf()); // redirect std::cout to log

        if (!strstr(Core.Params, "-no_unique_logs"))
        {
            string32 TimeBuf;
            using namespace std::chrono;
            const auto now = system_clock::now();
            const auto time = system_clock::to_time_t(now);
            std::strftime(TimeBuf, sizeof(TimeBuf), "%d-%m-%y_%H-%M-%S", std::localtime(&time));

            xr_strconcat(logFName, Core.ApplicationName, "_", Core.UserName, "_", TimeBuf, ".log");
        }
        else
        {
            xr_strconcat(logFName, Core.ApplicationName, "_", Core.UserName, ".log");
        }

        try
        {
            if (FS.path_exist("$logs$"))
            {
                FS.update_path(logFName, "$logs$", logFName);
            }
            else
            { //Для компрессора
                string_path temp;
                strcpy_s(temp, logFName);
                xr_strconcat(logFName, "logs\\", temp);
            }

            //logstream.imbue(std::locale(""));
            VerifyPath(logFName);
#ifdef __SWITCH__
            // libstdc++'s filebuf holds BUFSIZ (1 KB in newlib): the file got a write
            // to the SD every ~10 lines, on the logging thread, whatever the flush
            // cadence above (v202 stack samples: filebuf::xsputn -> fsdev_write).
            static char s_logBuf[64 * 1024];
            logstream.rdbuf()->pubsetbuf(s_logBuf, sizeof(s_logBuf));
#endif
            logstream.open(XR_OSPATH(logFName));
        }
        catch (...)
        {
            Debug.do_exit("Can't create log file!");
        }

        for (const auto& str : LogFile)
            logstream << str;

        logstream.flush();
    }

    LogFile.reserve(5000);
}

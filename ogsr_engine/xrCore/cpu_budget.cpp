#include "stdafx.h"

#include "cpu_budget.h"

#ifdef _MSC_VER
#include <TlHelp32.h>
#include <intrin.h>
#else
#include <dirent.h>
#include <ctime>
#endif
#include "stacktrace_collector.h"   // ResolveCodeAddress — символ по стартовому адресу
#include "xr_platform_time.h"       // xrPlatform::TickCount64
#include <chrono>
#include <algorithm>

namespace CpuBudget
{
int enabled = 0;

namespace
{
constexpr u32 kReportMs = 5000;

// ⭐Калибровка циклов в секунды. QueryThreadCycleTime возвращает такты TSC, а
// номинала TSC Windows не сообщает. Короткий спин с одновременным чтением
// __rdtsc и QueryPerformanceCounter даёт его прямо. Один раз за запуск и только
// при первом включении прибора — в кадре это не живёт.
// ⚠Если TSC на машине не инвариантный (очень старые CPU), число уедет вместе с
// частотой; поэтому оно печатается, а не прячется.
double cycles_per_ms()
{
    static double s_cpm = 0.0;
    if (s_cpm > 0.0)
        return s_cpm;

#ifdef _MSC_VER
    const u64 qf = CPU::QPCFreq();
    const u64 q0 = CPU::QPC();
    const u64 c0 = __rdtsc();
    const double want = double(qf) * 0.002;   // 2 мс
    u64 q1;
    do { q1 = CPU::QPC(); } while (double(q1 - q0) < want);
    const u64 c1 = __rdtsc();

    const double secs = double(q1 - q0) / double(qf);
    s_cpm = double(c1 - c0) / (secs * 1000.0);
    if (s_cpm < 1.0)
        s_cpm = 1.0;
    Msg("[CPUb] TSC calibrated: %.0f cycles/ms (%.2f GHz)", s_cpm, s_cpm / 1e6);
#else
    // QueryThreadCycleTime/QueryProcessCycleTime are backed by clock_gettime
    // on this platform (xr_platform_win32_shim.h) -- their unit IS nanoseconds,
    // exactly, so this conversion needs no calibration and has no measurement
    // error, unlike the TSC path above.
    s_cpm = 1000000.0;
    Msg("[CPUb] cycle unit: nanoseconds (clock_gettime), no TSC calibration on this platform");
#endif
    return s_cpm;
}

struct ThreadRec
{
    u32    tid{};
    char   name[48]{};
    u64    cycles{};     // последний снимок
    u64    delta{};      // за отчётный период
};

// ---------------------------------------------------------------------------
// ⭐КТО ТАКОЙ ПОТОК БЕЗ ИМЕНИ.
//
// 📏18-09 прибор показал: 46 потоков без описания жгут 1.00 core-ms/кадр -- на
// главном потоке столько же стоит вся запись командных буферов. Переименовать
// их через SetThreadDescription нельзя: часть создана не нами (драйвер Vulkan,
// NGX, звук, аллокатор), и до их кода мы не дотянемся.
//
// Зато у КАЖДОГО потока Windows помнит адрес стартовой функции
// (ThreadQuerySetWin32StartAddress). Модуль по этому адресу отвечает на вопрос
// "чей это поток" даже для чужого кода, а для нашего DbgHelp добавляет и имя
// функции. Это опознание вместо переименования, и оно покрывает всех.
//
// ⚠Спрашивается РАЗ В ОТЧЁТ и только у безымянных: NtQueryInformationThread --
// системный вызов, и в кадре ему делать нечего.
// ---------------------------------------------------------------------------
void start_site(HANDLE h, char* out, size_t cap)
{
    out[0] = 0;
#ifndef _MSC_VER
    // No portable way to resolve a thread's start address to a module+symbol
    // without capturing the entry function pointer at creation time (which we
    // don't have for threads we didn't create -- driver/NGX/sound/allocator).
    // Not actually needed here the way it is on Windows though: thread_name()
    // below reads /proc/self/task/<tid>/comm, which the kernel always sets --
    // unlike SetThreadDescription (opt-in, often missing), so this fallback
    // path is effectively unreachable on this platform.
    (void)h;
    return;
#else
    using FnNtQIT = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static auto ntq = (FnNtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (!ntq)
        return;

    PVOID start = nullptr;
    // 9 = ThreadQuerySetWin32StartAddress
    if (ntq(h, 9, &start, sizeof(start), nullptr) < 0 || !start)
        return;

    // Модуль -- без DbgHelp и без прав: адрес уже в нашем адресном пространстве.
    char mod[64] = "?";
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)start, &hm) && hm)
    {
        char full[MAX_PATH]{};
        if (GetModuleFileNameA(hm, full, sizeof(full)))
        {
            const char* base = strrchr(full, 0x5C);
            xr_strcpy(mod, sizeof(mod), base ? base + 1 : full);
        }
    }

    // Имя функции есть только там, где есть символы, то есть у нас.
    const std::string sym = ResolveCodeAddress(start);
    // ⚠Признак "это символ, а не адрес": строчные буквы. MSVC печатает указатель
    // прописным шестнадцатеричным, так что 'a'-'z' встречаются только в имени.
    const bool named = sym.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos;
    if (named)
        _snprintf(out, cap - 1, "%s!%s", mod, sym.c_str());
    else
        _snprintf(out, cap - 1, "%s+0x%IX", mod, (size_t)((u8*)start - (u8*)hm));
    out[cap - 1] = 0;
#endif
}

xr_vector<ThreadRec> g_threads;
u64  g_procCycles = 0;
u32  g_lastReport = 0;
u32  g_frames = 0;
bool g_primed = false;

void thread_name(HANDLE h, char* out, size_t cap)
{
    out[0] = 0;
#ifdef _MSC_VER
    static HMODULE kernel = GetModuleHandle("kernel32.dll");
    using FnGet = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static auto get_desc = kernel ? (FnGet)GetProcAddress(kernel, "GetThreadDescription") : nullptr;
    if (!get_desc)
        return;
    PWSTR wide{};
    if (FAILED(get_desc(h, &wide)) || !wide)
        return;
    WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int)cap, nullptr, nullptr);
    out[cap - 1] = 0;
    LocalFree(wide);
#else
    // /proc/<pid>/task/<tid>/comm is set by the kernel for EVERY thread
    // (defaults to the creating function's name, truncated to 15 chars) --
    // unlike SetThreadDescription this needs no cooperation from the thread's
    // own code, so it names threads Windows' path would call "unnamed".
    if (!xr_is_thread_handle(h))
        return;
    const long tid = xr_thread_handle_tid(h);
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", tid == 0 ? (long)gettid() : tid);
    if (FILE* f = fopen(path, "r"))
    {
        if (fgets(out, (int)cap, f))
        {
            const size_t n = strlen(out);
            if (n && out[n - 1] == '\n')
                out[n - 1] = 0;
        }
        fclose(f);
    }
#endif
}

// Снимок циклов по всем потокам процесса. Раз в отчёт.
void sample(xr_vector<ThreadRec>& out)
{
    out.clear();
#ifdef _MSC_VER
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid)
                continue;
            // LIMITED_INFORMATION хватает для QueryThreadCycleTime и
            // GetThreadDescription, и его дают без лишних прав.
            // ⚠QUERY_INFORMATION, не LIMITED: стартовый адрес по LIMITED не
            // отдаётся. Процесс наш, прав хватает.
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            ULONG64 cyc = 0;
            if (QueryThreadCycleTime(h, &cyc))
            {
                ThreadRec r;
                r.tid = te.th32ThreadID;
                r.cycles = cyc;
                thread_name(h, r.name, sizeof(r.name));
                if (!r.name[0])
                    start_site(h, r.name, sizeof(r.name));
                if (!r.name[0])
                    xr_strcpy(r.name, "unnamed");
                out.push_back(r);
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
#else
    // /proc/self/task/ lists one entry per live thread of THIS process, by
    // tid -- the direct POSIX analogue of a process-scoped thread snapshot
    // (CreateToolhelp32Snapshot already filters to one process too, via
    // th32OwnerProcessID above).
    DIR* d = opendir("/proc/self/task");
    if (!d)
        return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr)
    {
        if (de->d_name[0] == '.')
            continue;
        const long tid = atol(de->d_name);
        if (tid <= 0)
            continue;
        HANDLE h = OpenThread(0, FALSE, (DWORD)tid);
        ULONG64 cyc = 0;
        if (QueryThreadCycleTime(h, &cyc))
        {
            ThreadRec r;
            r.tid = (u32)tid;
            r.cycles = cyc;
            thread_name(h, r.name, sizeof(r.name));
            if (!r.name[0])
                start_site(h, r.name, sizeof(r.name));
            if (!r.name[0])
                xr_strcpy(r.name, "unnamed");
            out.push_back(r);
        }
    }
    closedir(d);
#endif
}

// «TTAPI thread id7» и «TTAPI thread id12» — одна и та же роль. Отчёт по ролям
// читается, отчёт по тридцати строкам — нет.
void role_of(const char* name, char* out, size_t cap)
{
    if (0 == strncmp(name, "TTAPI", 5))
        xr_strcpy(out, cap, "TTAPI workers");
    else
        xr_strcpy(out, cap, name);
}
} // namespace

// ---------------------------------------------------------------------------
// ⭐ФАЗЫ ГЛАВНОГО ПОТОКА В ЦИКЛАХ (`cpu_budget 2`). Подробности -- в заголовке.
//
// Устройство: стек фаз и ОДИН счётчик «сколько циклов потока было на последней
// границе». На каждой границе прошедшие с тех пор циклы падают в ту фазу,
// которая была на вершине стека, поэтому у вложенных фаз время СВОЁ, а не
// суммарное. Дно стека -- PH_NONE, туда уходит всё, что не размечено; оно и
// печатается строкой `unmarked`, чтобы разрыв «сумма частей против целого» был
// виден, а не додумывался.
// ---------------------------------------------------------------------------
namespace
{
constexpr int PH_NONE = -1;
constexpr int kPhaseStack = 8;

u32 g_mainTid = 0;

// Накопители ОБЩИЕ (фазы живут на разных потоках: запись и планировщик на
// главном, звук и физика на воркере), а курсор -- ПОТОКО-ЛОКАЛЬНЫЙ: циклы
// читаются у своего потока, и смешивать их нельзя.
std::atomic<u64> g_acc[PH_COUNT]{};
std::atomic<u64> g_unmarkedMain{};   // только главный поток: у воркеров "вне фаз" смысла не имеет
std::atomic<u32> g_phOverflow{};

struct PhaseCursor
{
    u64 mark{};            // счётчик СВОЕГО потока на последней границе
    int stack[kPhaseStack]{};
    int depth{};           // 0 = вершиной считается PH_NONE
};
thread_local PhaseCursor t_ph;

inline u64 self_cycles()
{
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return u64(c);
}

// Отдать накопленное текущей вершине и передвинуть границу.
// ⚠Первое обращение потока только ставит границу: иначе первой же фазе достались
// бы все циклы, накопленные потоком с момента его рождения.
inline void phase_charge()
{
    const u64 now = self_cycles();
    if (!t_ph.mark)
    {
        t_ph.mark = now;
        return;
    }
    const u64 d = (now >= t_ph.mark) ? (now - t_ph.mark) : 0;
    t_ph.mark = now;

    const int top = t_ph.depth > 0 ? t_ph.stack[t_ph.depth - 1] : PH_NONE;
    if (top == PH_NONE)
    {
        if (GetCurrentThreadId() == g_mainTid)
            g_unmarkedMain.fetch_add(d, std::memory_order_relaxed);
    }
    else
        g_acc[top].fetch_add(d, std::memory_order_relaxed);
}

const char* phase_name(int p)
{
    switch (p)
    {
    case PH_FRAMEMOVE: return "FrameMove (logic, self)";
    case PH_SCHED:     return "  sheduler";
    case PH_UPDATECL:  return "  UpdateCL objects";
    case PH_SUBMIT2ND: return "submit 2nd thread";
    case PH_RBEGIN:    return "render Begin";
    case PH_RECORD:    return "record commands";
    case PH_PRESENT:   return "submit + present";
    case PH_WAIT2ND:   return "wait 2nd thread";
    case PH_LIMITER:   return "fps limiter tail";
    case PH_SOUND:     return "sound update";
    case PH_SEQPARALLEL: return "seqParallel (ALife)";
    case PH_SCRIPTGC:  return "lua GC";
    case PH_SEQFRAMEMT: return "seqFrameMT (physics/net)";
    default:           return "?";
    }
}

// На каком потоке живёт фаза -- нужно только для сверки сумм в отчёте.
bool phase_on_main(int p) { return p <= PH_LIMITER; }
} // namespace

void PhaseEnter(Phase p)
{
    if (enabled < 2)
        return;
    phase_charge();
    if (t_ph.depth < kPhaseStack)
        t_ph.stack[t_ph.depth++] = int(p);
    else
        g_phOverflow.fetch_add(1, std::memory_order_relaxed);
}

void PhaseLeave(Phase p)
{
    if (enabled < 2)
        return;
    phase_charge();
    // Снимаем по СОВПАДЕНИЮ, а не слепо: если разметку когда-нибудь вложат
    // криво (ранний return внутри фазы), лучше оставить стек в покое и
    // потерять одну фазу, чем перепутать все последующие.
    if (t_ph.depth > 0 && t_ph.stack[t_ph.depth - 1] == int(p))
        --t_ph.depth;
    else
        g_phOverflow.fetch_add(1, std::memory_order_relaxed);
}

double CyclesPerMs() { return cycles_per_ms(); }

void Frame()
{
    if (!enabled)
    {
        g_primed = false;
        return;
    }

    if (!g_mainTid)
        g_mainTid = GetCurrentThreadId(); // on_idle -- это главный поток

    // Прайм курсора больше не нужен: phase_charge() сам ставит границу при
    // первом обращении КАЖДОГО потока (курсор потоко-локальный).

    ++g_frames;
    // xrPlatform::TickCount64, а не игровые часы: xrCore ниже Device по слоям, и
    // прибор о процессорном времени обязан считать реальные секунды в любом случае.
    const u32 now = (u32)xrPlatform::TickCount64();
    if (!g_primed)
    {
        cycles_per_ms();
        sample(g_threads);
        QueryProcessCycleTime(GetCurrentProcess(), &g_procCycles);
        g_lastReport = now;
        g_frames = 0;
        g_primed = true;
        return;
    }
    if (now - g_lastReport < kReportMs)
        return;

    const double secs = double(now - g_lastReport) / 1000.0;
    g_lastReport = now;

    xr_vector<ThreadRec> cur;
    sample(cur);
    ULONG64 procNow = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &procNow);

    // Дельты по tid. Потоки, появившиеся с прошлого раза, дают свою дельту от
    // нуля — это честно: их циклы действительно потрачены в этом окне.
    for (ThreadRec& c : cur)
    {
        u64 prev = 0;
        for (const ThreadRec& p : g_threads)
            if (p.tid == c.tid) { prev = p.cycles; break; }
        c.delta = (c.cycles >= prev) ? (c.cycles - prev) : c.cycles;
    }

    const double cpm = cycles_per_ms();
    const u32    frames = g_frames ? g_frames : 1;
    const double procMs = double(procNow - g_procCycles) / cpm;

    double sumMs = 0.0;
    for (const ThreadRec& c : cur)
        sumMs += double(c.delta) / cpm;

    Msg("[CPUb] %u frames in %.1f s (%.1f fps, %.2f ms wall/frame) | process %.2f core-ms/frame = %.2f cores busy",
        frames, secs, double(frames) / secs, secs * 1000.0 / double(frames),
        procMs / double(frames), procMs / (secs * 1000.0));

    // Свернуть по ролям.
    struct Role { char name[48]; double ms; u32 n; };
    xr_vector<Role> roles;
    for (const ThreadRec& c : cur)
    {
        char rn[48];
        role_of(c.name, rn, sizeof(rn));
        bool found = false;
        for (Role& r : roles)
            if (0 == xr_strcmp(r.name, rn)) { r.ms += double(c.delta) / cpm; ++r.n; found = true; break; }
        if (!found)
        {
            Role r{};
            xr_strcpy(r.name, sizeof(r.name), rn);
            r.ms = double(c.delta) / cpm;
            r.n = 1;
            roles.push_back(r);
        }
    }
    std::sort(roles.begin(), roles.end(), [](const Role& a, const Role& b) { return a.ms > b.ms; });

    const size_t show = roles.size() < 16 ? roles.size() : 16;
    for (size_t i = 0; i < show; ++i)
    {
        if (roles[i].ms / double(frames) < 0.005)
            break;
        Msg("[CPUb]   %-26s x%-3u %6.3f core-ms/frame  (%4.1f%%)", roles[i].name, roles[i].n,
            roles[i].ms / double(frames), 100.0 * roles[i].ms / (procMs > 0.0 ? procMs : 1.0));
    }
    // ⚠Разница = потоки, умершие внутри окна: их циклы ушли вместе с ними.
    if (procMs > 0.0 && (procMs - sumMs) / procMs > 0.02)
        Msg("[CPUb]   (%.2f core-ms/frame unattributed — threads that exited inside the window)",
            (procMs - sumMs) / double(frames));

    // ⭐Фазы главного потока (cpu_budget 2). Печатаются ПОСЛЕ таблицы потоков и
    // сверяются с его же строкой: сумма фаз + unmarked обязана совпасть с
    // циклами потока, иначе прибор врёт и по нему нельзя планировать.
    if (enabled >= 2 && g_mainTid)
    {
        phase_charge(); // закрыть текущий отрезок главного потока

        double mainMs = 0.0;
        for (const ThreadRec& c : cur)
            if (c.tid == g_mainTid) { mainMs = double(c.delta) / cpm; break; }

        double phMs[PH_COUNT];
        double mainPh = 0.0, workPh = 0.0;
        for (int i = 0; i < PH_COUNT; ++i)
        {
            phMs[i] = double(g_acc[i].exchange(0, std::memory_order_relaxed)) / cpm;
            if (phase_on_main(i)) mainPh += phMs[i]; else workPh += phMs[i];
        }
        const double unmarkedMs = double(g_unmarkedMain.exchange(0, std::memory_order_relaxed)) / cpm;
        const u32 ovf = g_phOverflow.exchange(0, std::memory_order_relaxed);

        Msg("[CPUb] phases (self time, cycles) | main: %.3f measured / %.3f accounted, workers: %.3f of %.3f%s",
            mainMs / double(frames), (mainPh + unmarkedMs) / double(frames),
            workPh / double(frames), procMs / double(frames) - mainMs / double(frames),
            ovf ? "  [!] phase stack misnesting" : "");
        for (int i = 0; i < PH_COUNT; ++i)
        {
            const double ms = phMs[i] / double(frames);
            if (ms < 0.003)
                continue;
            Msg("[CPUb]   %-26s %6.3f core-ms/frame  (%4.1f%% of frame)%s", phase_name(i), ms,
                procMs > 0.0 ? 100.0 * phMs[i] / procMs : 0.0, phase_on_main(i) ? "" : "  [worker]");
        }
        Msg("[CPUb]   %-26s %6.3f core-ms/frame  (%4.1f%% of frame)", "main: unmarked",
            unmarkedMs / double(frames), procMs > 0.0 ? 100.0 * unmarkedMs / procMs : 0.0);
    }

    g_threads.swap(cur);
    g_procCycles = procNow;
    g_frames = 0;
}
} // namespace CpuBudget

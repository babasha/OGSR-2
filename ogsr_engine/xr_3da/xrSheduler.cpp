#include "stdafx.h"
#include "../xrCore/cpu_budget.h"
#include "../xrCore/load_prof.h"
#include "xrSheduler.h"
#include "xr_object.h"
#include "lua_vm_watch.h"
#include "xr_platform_seh.h"
#include <chrono>
#include <algorithm>

float psShedulerCurrent = 10.f;
float psShedulerTarget = 10.f;
float psShedulerMax = 10.f;
constexpr float psShedulerReaction = 1.f; // 0.1f;

// ---------------------------------------------------------------------------
// ⭐ОЧЕРЕДЬ ПЛАНИРОВЩИКА — ДВОИЧНАЯ КУЧА ПО ДЕДЛАЙНУ.
//
// 📏Было: каждый кадр ProcessStep копировал ВСЕ записи очереди (954 на Кордоне,
// каждая с shared_str, то есть пара атомиков на копию), а по каждой сработавшей
// делал Items.erase(Items.begin() + it) -- удаление из СЕРЕДИНЫ вектора. 68
// срабатываний в кадре x ~477 сдвигов = около 33 тысяч перемещений элементов за
// кадр. Замер 18-09 при fpslock60: накладные очереди 0.30 мс/кадр, то есть 23%
// всей фазы планировщика и 6% главного потока -- на A57 это 3-4.5 мс из 16.7.
//
// Стало: min-куча по dwTimeForExecute. Пока вершина не просрочена, делать нечего
// вообще; на каждое срабатывание -- pop_heap + push_heap, 2*log2(954) ~ 20
// операций вместо 477.
//
// ⚠ИЗМЕНЕНИЕ ПОВЕДЕНИЯ, ОДНО: просроченные объекты теперь обрабатываются в
// порядке ДЕДЛАЙНА, а не в порядке вставки в вектор. Для уже просроченных это
// строго осмысленнее, но если мод где-то зависел от прежнего порядка -- вылезет
// здесь.
// ⚠Снятие с учёта НЕ трогает кучу: internal_Unregister и раньше помечал запись
// как Object = nullptr (ленивое удаление), а свойство кучи держится на
// dwTimeForExecute, которое при этом не меняется. Запись отсеется, когда
// всплывёт по сроку.
// ---------------------------------------------------------------------------
// Сам компаратор -- CSheduler::HeapCmp в заголовке (Item приватный).

//-------------------------------------------------------------------------------------
void CSheduler::Initialize() { m_processing_now = false; }

void CSheduler::Destroy()
{
    internal_Registration();

    ItemsRT.clear();
    Items.clear();
    Registration.clear();
}

void CSheduler::internal_Registration()
{
    for (u32 it = 0; it < Registration.size(); it++)
    {
        ItemReg& R = Registration[it];
        if (R.OP)
        {
            // register
            // search for paired "unregister"

            BOOL bFoundAndErased = FALSE;
            for (u32 pair = it + 1; pair < Registration.size(); pair++)
            {
                ItemReg& R_pair = Registration[pair];
                if ((!R_pair.OP) && (R_pair.Object == R.Object))
                {
                    bFoundAndErased = TRUE;
                    Registration.erase(Registration.begin() + pair);
                    break;
                }
            }

            // register if non-paired
            if (!bFoundAndErased)
            {
                //Msg("SCHEDULER: internal register [%s][%x][%s]", *R.Object->shedule_Name(), R.Object, R.RT ? "true" : "false");
                internal_Register(R.Object, R.RT);
            }
            //else
            //    Msg("SCHEDULER: internal register skipped, because unregister found [%s][%x][%s]", "unknown", R.Object, R.RT ? "true" : "false");
        }
        else
        {
            // unregister
            internal_Unregister(R.Object, R.RT);
        }
    }

    Registration.clear();
}

void CSheduler::internal_Register(ISheduled* object, BOOL RT)
{
    VERIFY(!O->shedule.b_locked);

    if (RT)
    {
        // Fill item structure
        auto& TNext = ItemsRT.emplace_back();
        TNext.dwTimeForExecute = Device.dwTimeGlobal;
        TNext.dwTimeOfLastExecute = Device.dwTimeGlobal;
        TNext.Object = object;
        TNext.scheduled_name = object->shedule_Name();
        object->shedule.b_RT = TRUE;
    }
    else
    {
        // Fill item structure && Insert into priority Queue
        auto& TNext = Items.emplace_back();
        TNext.dwTimeForExecute = Device.dwTimeGlobal;
        TNext.dwTimeOfLastExecute = Device.dwTimeGlobal;
        TNext.Object = object;
        TNext.scheduled_name = object->shedule_Name();
        object->shedule.b_RT = FALSE;
        // ⚠Поля заполняются ДО push_heap: куча переставляет элементы, и ссылка
        // TNext после неё указывала бы не туда.
        std::push_heap(Items.begin(), Items.end(), HeapCmp{});
    }
}

bool CSheduler::internal_Unregister(const ISheduled* object, BOOL RT)
{
    if (RT)
    {
        for (u32 i = 0; i < ItemsRT.size(); i++)
        {
            if (ItemsRT[i].Object == object)
            {
                ItemsRT.erase(ItemsRT.begin() + i);
                return true;
            }
        }
    }
    else
    {
        for (auto& Item : Items)
        {
            if (Item.Object == object)
            {
                Item.Object = nullptr;
                return true;
            }
        }
    }

    if (m_current_step_obj == object)
    {
        m_current_step_obj = nullptr;
        return true;
    }

    return false;
}

void CSheduler::Register(ISheduled* A, BOOL RT)
{
    auto& R = Registration.emplace_back();
    R.OP = TRUE;
    R.RT = RT;
    R.Object = A;
    R.Object->shedule.b_RT = RT;

    //Msg("SCHEDULER: register [%s][%x]", *A->shedule_Name(), A);
}

void CSheduler::Unregister(ISheduled* A, bool force)
{
    if (m_processing_now || force)
    {
        if (internal_Unregister(A, A->shedule.b_RT))
            return;
    }

    auto& R = Registration.emplace_back();
    R.OP = FALSE;
    R.RT = A->shedule.b_RT;
    R.Object = A;
}


// ---------------------------------------------------------------------------
// ⭐КТО ИМЕННО СЪЕДАЕТ ПЛАНИРОВЩИК (консольный `sched_stats 1`)
//
// 18-09-2026 замер показал: кадр упирается в CPU (5.3 мс против 2.3 GPU), и
// внутри CPU планировщик — один из двух главных потребителей (~1.1 мс). Но
// `[VK CPU] sched=1.12` не говорит НИ КТО, ни сколько из этого Lua: объекты
// обновляются вперемешку, а скрипты зовутся изнутри их же shedule_Update.
//
// Здесь на каждый вызов снимается два числа: полное время и прирост
// потоко-локального счётчика времени в Lua-VM (lua_vm_watch::lua_time_ns).
// Разность даёт C++-часть. Ключ — shedule_Name(): у менеджеров это осмысленные
// имена ("alife_simulator", "agent_manager"), у игровых объектов — их имя.
//
// ⚠Цена прибора: два обращения к часам на объект. При ~180 объектах в кадре это
// сотые доли миллисекунды, но по умолчанию он ВЫКЛЮЧЕН — чтобы никто не мерил
// кадр вместе с прибором, не зная об этом.
// ⚠Отчёт печатается по РЕАЛЬНЫМ часам (dwTimeContinual): игровые встают на паузе
// и масштабируются time_factor, и тогда отчёт не выйдет никогда.
// ---------------------------------------------------------------------------
int g_sched_stats = 0;

// ---------------------------------------------------------------------------
// ⭐РЫЧАГ ЧАСТОТЫ ОБЫЧНОЙ ОЧЕРЕДИ (консольный `sched_rate`, проценты).
//
// 100 = штатное расписание, 200 = каждый объект думает вдвое реже.
//
// Зачем он нужен. Замер 19-09 (`sched_stats 1`, Кордон/день, 60 fps): фаза
// планировщика 1.26 мс/кадр, из них ВНУТРИ Lua-VM 0.75 (64%), а накладные
// самой очереди -- 0.089. То есть движковой правкой здесь больше нечего
// брать: содержимое фазы -- чужой код (скрипты мода и ИИ на C++), и
// единственный оставшийся рычаг -- КАК ЧАСТО его звать. Этот множитель
// превращает рассуждение в ИЗМЕРИМЫЙ курс обмена "частота ИИ -> мс главного
// потока": без него любой разговор о "снизить частоту" -- гадание.
//
// ⚠ЭТО ИЗМЕНЕНИЕ ПОВЕДЕНИЯ ИГРЫ, не оптимизация. NPC с вдвое реже
// работающим расписанием медленнее реагирует; дефолт поэтому 100.
// ⚠RT-очередь (актёр, alife) рычагом НЕ затрагивается: у неё нет
// интервала вообще, у актёра свой гейт (`script_binder_period`).
// ⚠Верхний кламп 5 с: дальше объект начинает получать dt, который
// сам ProcessStep урезает до t_max (см. clamp ниже), и скриптовые таймеры
// начинают терять игровое время -- молча.
// ---------------------------------------------------------------------------
int g_sched_rate = 100;

namespace
{
struct SchedStat
{
    double ms{};      // полное время shedule_Update
    double lua_ms{};  // из них внутри Lua-VM
    u64    lua_n{};   // сколько РАЗ входили в VM
    u32    hits{};
    // ⭐18-09: ЧАСТОТА, А НЕ ТОЛЬКО ЦЕНА. Интервал апдейта объекта движок
    // считает как dwMin + (dwMax-dwMin) * shedule_Scale(), и база
    // CObject::shedule_Scale -- это distance/200, то есть дальние объекты
    // ДОЛЖНЫ думать реже. Проверить это можно только напечатав и scale, и
    // получившийся интервал: если у NPC в двухстах метрах scale около нуля,
    // throttling не работает, и это не проценты, а кратность.
    float  scale{};   // последнее значение shedule_Scale()
    u32    interval{};// последний вычисленный dwUpdate, мс
};
xr_map<shared_str, SchedStat> g_sched_stat;
u32    g_sched_frames = 0;

// ⭐НАКЛАДНЫЕ САМОГО ПЛАНИРОВЩИКА, ОТДЕЛЬНО ОТ ОБЪЕКТОВ.
// 📏18-09 на 60 fps: сумма shedule_Update = 1.12 мс/кадр, а фаза sched по
// таймеру движка = 2.09. Миллисекунда на главном потоке, которую никто не
// приписал ни одному объекту, -- это цена обхода очереди на 958 записей с
// erase из СЕРЕДИНЫ вектора по каждому сработавшему. Пока она не разложена,
// оптимизировать объекты бессмысленно: половина расхода не в них.
// ⭐ДЖИТТЕР И БЮДЖЕТ. Среднее по ProcessStep ничего не говорит о том, что
// случается в ХУДШЕМ кадре: один апдейт сталкера стоит ~0.45 мс, и если
// четверо совпали сроками, кадр получает полторы миллисекунды сразу. Именно
// это и должен срезать бюджет (rs_sheduler_max), поэтому его надо видеть.
double g_sched_ps_max = 0.0;     // худший ProcessStep за отчёт, мс
u32    g_sched_ps_hist[5] = {};  // <=0.5 / <=1 / <=2 / <=4 / >4 мс
u32    g_sched_breaks = 0;       // кадров, где бюджет оборвал обход
double g_sched_late_sum = 0.0;   // просрочка вершины кучи в КОНЦЕ ProcessStep, мс
u32    g_sched_late_max = 0;

double g_sched_reg_ms = 0.0;   // internal_Registration (три вызова за апдейт)
double g_sched_rt_ms  = 0.0;   // весь цикл ItemsRT, включая сами объекты
double g_sched_ps_ms  = 0.0;   // весь ProcessStep, включая сами объекты
u32    g_sched_last_ms = 0;

void sched_report()
{
    if (g_sched_stat.empty() || g_sched_frames == 0)
        return;

    xr_vector<std::pair<shared_str, SchedStat>> v;
    v.reserve(g_sched_stat.size());
    for (const auto& kv : g_sched_stat)
        v.push_back(kv);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });

    double totMs = 0.0, totLua = 0.0;
    u32    totHits = 0;
    for (const auto& kv : v) { totMs += kv.second.ms; totLua += kv.second.lua_ms; totHits += kv.second.hits; }

    const double f = double(g_sched_frames);
    Msg("[Sched] over %u frames: total %.3f ms/frame (of it Lua %.3f = %.0f%%), calls %.1f/frame, distinct objects %u",
        g_sched_frames, totMs / f, totLua / f, totMs > 0.0 ? totLua / totMs * 100.0 : 0.0, double(totHits) / f, u32(v.size()));
    // ⭐Строка, ради которой заведены аккумуляторы: сколько стоит САМ планировщик.
    const double overhead = (g_sched_rt_ms + g_sched_ps_ms) - totMs;
    Msg("[Sched] phase %.3f ms/f = registration %.3f + RT %.3f + ProcessStep %.3f | OBJECTS %.3f, QUEUE OVERHEAD %.3f (%.0f%% of the phase)",
        (g_sched_reg_ms + g_sched_rt_ms + g_sched_ps_ms) / f, g_sched_reg_ms / f, g_sched_rt_ms / f, g_sched_ps_ms / f,
        totMs / f, overhead / f,
        (g_sched_rt_ms + g_sched_ps_ms) > 0.0 ? 100.0 * overhead / (g_sched_rt_ms + g_sched_ps_ms) : 0.0);
    Msg("[Sched] ProcessStep: avg %.3f, worst %.3f ms | frames <=.5/<=1/<=2/<=4/>4: %u/%u/%u/%u/%u | budget %.2f ms stopped %.1f%% of frames | heap lateness avg %.1f, worst %u ms",
        g_sched_ps_ms / f, g_sched_ps_max,
        g_sched_ps_hist[0], g_sched_ps_hist[1], g_sched_ps_hist[2], g_sched_ps_hist[3], g_sched_ps_hist[4],
        psShedulerCurrent, 100.0 * double(g_sched_breaks) / f,
        g_sched_late_sum / f, g_sched_late_max);

    g_sched_reg_ms = g_sched_rt_ms = g_sched_ps_ms = 0.0;
    g_sched_ps_max = 0.0; g_sched_breaks = 0; g_sched_late_sum = 0.0; g_sched_late_max = 0;
    g_sched_ps_hist[0] = g_sched_ps_hist[1] = g_sched_ps_hist[2] = g_sched_ps_hist[3] = g_sched_ps_hist[4] = 0;

    const size_t n = v.size() < 12 ? v.size() : 12;
    for (size_t i = 0; i < n; ++i)
    {
        const SchedStat& s = v[i].second;
        Msg("[Sched]   %-30s %6.3f ms/f | Lua %6.3f (%3.0f%%) | %.3f ms/call | VM/call %.0f | every %4u ms (scale %.2f)",
            v[i].first.c_str(), s.ms / f, s.lua_ms / f, s.ms > 0.0 ? s.lua_ms / s.ms * 100.0 : 0.0,
            s.hits ? s.ms / double(s.hits) : 0.0,
            s.hits ? double(s.lua_n) / double(s.hits) : 0.0,
            s.interval, s.scale);
    }

    // ------------------------------------------------------------------
    // ⭐ХВОСТ И ГИСТОГРАММА ПО ИНТЕРВАЛУ.
    //
    // Топ-12 отвечает на вопрос "кто дороже всех", но НЕ на тот, ради
    // которого прибор заведён: 64% фазы -- это Lua мода, движковой правкой
    // её не достать, поэтому единственный настоящий рычаг здесь -- ЧАСТОТА.
    // А частоту нельзя обсуждать, не зная, в каком диапазоне периодов лежит
    // работа: 68 вызовов в кадре могут быть двенадцатью NPC по 8 Гц, а могут --
    // шестьюстами предметов по 1 Гц, и это разные разговоры.
    // ⚠Хвост считается вычитанием из целого, а не отдельным накопителем:
    // так он не может разойтись с суммой.
    // ------------------------------------------------------------------
    {
        double topMs = 0.0; u32 topHits = 0;
        const size_t ntop = v.size() < 12 ? v.size() : 12;
        for (size_t i = 0; i < ntop; ++i) { topMs += v[i].second.ms; topHits += v[i].second.hits; }
        const double tailMs = totMs - topMs;
        const u32    tailHits = totHits - topHits;
        Msg("[Sched] tail: %u objects outside top-12 | %.1f calls/frame | %.3f ms/frame (%.1f us/call)",
            u32(v.size() > ntop ? v.size() - ntop : 0), double(tailHits) / f, tailMs / f,
            tailHits ? tailMs * 1000.0 / double(tailHits) : 0.0);

        // Корзины по ПОСЛЕДНЕМУ вычисленному периоду dwUpdate. ⚠У RT-записей
        // периода нет вовсе (interval остаётся 0), они падают в первую корзину --
        // так и надо, они самые частые из всех.
        static const u32 edges[5] = {60, 120, 250, 500, 0xffffffff};
        const char* names[5] = {"<=60ms", "<=120ms", "<=250ms", "<=500ms", ">500ms"};
        u32 bObj[5] = {}, bHits[5] = {}; double bMs[5] = {}, bLua[5] = {};
        for (const auto& kv : v)
        {
            u32 b = 0; while (b < 4 && kv.second.interval > edges[b]) ++b;
            ++bObj[b]; bHits[b] += kv.second.hits; bMs[b] += kv.second.ms; bLua[b] += kv.second.lua_ms;
        }
        for (u32 b = 0; b < 5; ++b)
            if (bObj[b])
                Msg("[Sched]   period %-8s %4u obj | %5.1f calls/f | %6.3f ms/f (Lua %5.3f) | %.3f ms/call",
                    names[b], bObj[b], double(bHits[b]) / f, bMs[b] / f, bLua[b] / f,
                    bHits[b] ? bMs[b] / double(bHits[b]) : 0.0);
    }

    lua_vm_watch::lua_prof_dump();   // пофункциональный профиль Lua — тем же отчётом
    lua_vm_watch::lua_sample_dump();  // и сэмплирующий, если включён (`lua_sample`)

    g_sched_stat.clear();
    g_sched_frames = 0;
}
} // namespace
void CSheduler::ProcessStep()
{
    ZoneScoped;

    // Normal priority
    u32 dwTime = Device.dwTimeGlobal;

    const bool prefetch = Device.dwPrecacheFrame > 0;
    decltype(Items) ItemsProcessed;
    bool stopped{};
    //size_t cnt{};
    CTimer t_total;
    t_total.Start();

    // Вершина кучи -- самый ранний дедлайн. Как только он в будущем, в очереди
    // нечего делать: раньше на этом месте всё равно допрокручивался весь вектор.
    while (!Items.empty() && Items.front().dwTimeForExecute < dwTime)
    {
        std::pop_heap(Items.begin(), Items.end(), HeapCmp{});
        Item curr = std::move(Items.back());
        Items.pop_back();

        bool skip{curr.Object == nullptr}, shed_need{true};

        if (!skip)
        {
            XR_SEH_TRY
            {
                shed_need = curr.Object->shedule_Needed();
            }
            XR_SEH_EXCEPT(ExceptStackTrace("[CSheduler::ProcessStep] stack trace:\n"))
            {
                Msg("Scheduler tried to update object %s", *curr.scheduled_name);
                skip = true;
            }
        }

        // Запись уже извлечена из кучи. Если объект снят с учёта или сам сказал
        // "не надо" -- он в очередь НЕ возвращается; это прежнее поведение
        // (раньше erase стоял ровно здесь), объекты перерегистрируются сами.
        if (skip || !shed_need)
        {
            continue;
        }

        XR_SEH_TRY
        {
            // Calc next update interval
            const u32 dwMin = std::max(30u, curr.Object->shedule.t_min);
            const u32 dwMax = (1000u + curr.Object->shedule.t_max) / 2;

            const float scale = curr.Object->shedule_Scale();

            u32 dwUpdate = dwMin + iFloor(float(dwMax - dwMin) * scale);
            clamp(dwUpdate, std::max(dwMin, 20u), dwMax);

            // Рычаг частоты -- ПОСЛЕ штатного клампа, иначе dwMax его съест.
            if (g_sched_rate != 100)
                dwUpdate = u32(clampr<u64>(u64(dwUpdate) * u64(g_sched_rate) / 100ull, 20ull, 5000ull));

            const u32 elapsed = dwTime - curr.dwTimeOfLastExecute;

            m_current_step_obj = curr.Object;

            // if (!Core.DebugFlags.test(xrCore::dbg_DisableObjectsScheduler))
            if (g_sched_stats)
            {
                const u64 lua0 = lua_vm_watch::lua_time_ns();
                const u64 cal0 = lua_vm_watch::lua_calls();
                const auto t0 = std::chrono::steady_clock::now();
                curr.Object->shedule_Update(std::clamp(elapsed, 1u, std::max(curr.Object->shedule.t_max, 1000u)));
                const double ms = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / 1e6;
                const double lua = double(lua_vm_watch::lua_time_ns() - lua0) / 1e6;
                SchedStat& st = g_sched_stat[curr.scheduled_name];
                st.ms += ms; st.lua_ms += lua; st.lua_n += lua_vm_watch::lua_calls() - cal0; ++st.hits;
                st.scale = scale; st.interval = dwUpdate;
            }
            else
                curr.Object->shedule_Update(std::clamp(elapsed, 1u, std::max(curr.Object->shedule.t_max, 1000u)));

            if (!m_current_step_obj)
            {
                continue;
            }

            m_current_step_obj = nullptr;

            // Fill item structure
            auto& next = ItemsProcessed.emplace_back();
            next.dwTimeForExecute = dwTime + dwUpdate;
            next.dwTimeOfLastExecute = dwTime;
            next.Object = curr.Object;
            next.scheduled_name = curr.Object->shedule_Name();

            //cnt++;
        }
        XR_SEH_EXCEPT(ExceptStackTrace("[CSheduler::ProcessStep2] stack trace:\n"))
        {
            Msg("Scheduler tried to update object %s", *curr.scheduled_name);
            curr.Object = nullptr;
            continue;
        }

        // ⚠БЮДЖЕТ СРАВНИВАЛСЯ В ЦЕЛЫХ МИЛЛИСЕКУНДАХ, а нижняя граница
        // psShedulerCurrent жёстко равнялась 3 — то есть механизм НЕ МОГ выразить бюджет
        // меньше трёх миллисекунд. ⚠И второе, неочевидное: rs_sheduler_max -- НЕ
        // бюджет, а ПОТОЛОК. Цель psShedulerTarget каждый кадр без срыва уменьшается
        // и упирается в нижнюю границу клампа ⇒ реальный бюджет в игре всегда был 3 мс,
        // а не десять.
        // 📏Замер 19-09 при 60 fps (Кордон/день): ProcessStep в среднем 0.9 мс, худший
        // кадр 3.0-3.4 ⇒ ограничитель срабатывал на 0.3-1.3% кадров и больше не делал
        // ничего: выразить "держись в 0.5 мс" было НЕЧЕМ.
        // Сравнение переведено в мкс (GetElapsed_ms_total), нижняя граница клампа ниже
        // -- см. конец CSheduler::Update.
        // ⚠Дефолтное поведение всё же чуть сдвинулось: было "> 3" в целых (то есть
        // срыв на 4-й мс), стало "> 3.0". Замерено: разница в шуме.
        if (!prefetch && t_total.GetElapsed_ms_total() > psShedulerCurrent)
        {
            if (g_sched_stats)
                ++g_sched_breaks;
            // we have maxed out the load - increase heap
            psShedulerTarget += (psShedulerReaction * 3);

            // if (Core.DebugFlags.test(xrCore::dbg_TraceScheduler))
            {
                // Msg("Break ProcessStep. Processed: [%u], left in queue: [%u]", ItemsProcessed.size(), Items.size());
                if (ItemsProcessed.size() == 1) // кто то жрет все время на кадре
                    Msg("! Single item [%s] took whole update frame!!!", ItemsProcessed.front().scheduled_name.c_str());
            }

            stopped = true;
            break;
        }
    }

    //if (/*Core.DebugFlags.test(xrCore::dbg_TraceScheduler) &&*/ !prefetch && t_total.GetElapsed_ms() > 20)
    //    Msg("Long ProcessStep !!! duration [%u]ms. updated: [%u] objects!", t_total.GetElapsed_ms(), cnt);

    //if (prefetch)
    //    Msg("Prefetch frame, updated: [%u] objects!", cnt);

    // Push "processed" back -- каждый со своим новым дедлайном, по одному
    // просеиванию на запись.
    for (auto& processed : ItemsProcessed)
    {
        Items.push_back(std::move(processed));
        std::push_heap(Items.begin(), Items.end(), HeapCmp{});
    }

    // Беклог: насколько просрочена вершина кучи ПОСЛЕ обхода. Это
    // единственное число, по которому видно, голодает ли очередь при живом
    // бюджете: стабильная просрочка = растянутый период, РАСТУЩАЯ = очередь
    // не успевает вообще. O(1): вершина кучи и есть самый просроченный.
    if (g_sched_stats)
    {
        const u32 late = (!Items.empty() && Items.front().dwTimeForExecute < dwTime) ? (dwTime - Items.front().dwTimeForExecute) : 0u;
        g_sched_late_sum += double(late);
        if (late > g_sched_late_max)
            g_sched_late_max = late;
    }

    if (!stopped)
    {
        // always try to decrease target
        psShedulerTarget -= psShedulerReaction;
    }
}

extern ENGINE_API int g_cpu_off;
void CSheduler::Update()
{
    ZoneScoped;
    LoadProf::Scope _lp("scheduler"); // [frame prof]: what is not a stalker's own "sched"
    if (g_cpu_off)
        return;
    // Фаза главного потока в ЦИКЛАХ (cpu_budget 2): вложена в PH_FRAMEMOVE, её
    // время из объемлющей вычитается -- см. cpu_budget.h.
    CpuBudget::PhaseScope _ph(CpuBudget::PH_SCHED);

    // Initialize
    Device.Statistic->Sheduler.Begin();

    // ⚠Таймеры включаются только под sched_stats: три обращения к часам на
    // апдейт ничего не стоят, но прибор, который всегда включён, рано или
    // поздно становится частью замеряемого.
    std::chrono::steady_clock::time_point _t_reg0;
    if (g_sched_stats) _t_reg0 = std::chrono::steady_clock::now();
    internal_Registration();
    if (g_sched_stats) g_sched_reg_ms += double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _t_reg0).count()) / 1e6;

    m_processing_now = true;

    u32 dwTime = Device.dwTimeGlobal;

    std::chrono::steady_clock::time_point _t_rt0;
    if (g_sched_stats) _t_rt0 = std::chrono::steady_clock::now();
    {
        ZoneScopedN("ItemsRT");

        // Realtime priority. Each item is guarded: a C++ exception (Lua error on a
        // foreign map) thrown here used to unwind the WHOLE CSheduler::Update — no
        // normal queue, no shedule_Update→MakeMeCrow anywhere, the world freezes
        // (actor can't walk, NPCs statues) while rendering keeps going.
        for (auto& curr : ItemsRT)
        {
            R_ASSERT(curr.Object);

            try
            {
                if (!curr.Object->shedule_Needed())
                {
                    curr.dwTimeOfLastExecute = dwTime;
                    continue;
                }

                const u32 elapsed = dwTime - curr.dwTimeOfLastExecute;
                if (g_sched_stats)
                {
                    // Очередь реального времени: обновляется КАЖДЫЙ кадр, в отличие
                    // от обычной. Считаем отдельно (префикс RT:), иначе актёр и
                    // менеджеры смешались бы с редко обновляемыми NPC.
                    const u64 lua0 = lua_vm_watch::lua_time_ns();
                    const u64 cal0 = lua_vm_watch::lua_calls();
                    const auto t0 = std::chrono::steady_clock::now();
                    curr.Object->shedule_Update(elapsed);
                    const double ms = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / 1e6;
                    const double lua = double(lua_vm_watch::lua_time_ns() - lua0) / 1e6;
                    string128 key;
                    xr_sprintf(key, "RT:%s", curr.Object->shedule_Name().c_str());
                    SchedStat& st = g_sched_stat[shared_str(key)];
                    st.ms += ms; st.lua_ms += lua; st.lua_n += lua_vm_watch::lua_calls() - cal0; ++st.hits;
                }
                else
                    curr.Object->shedule_Update(elapsed);
                curr.dwTimeOfLastExecute = dwTime;
            }
            catch (const std::exception& e)
            {
                static u32 s_rt_throws = 0;
                const u32 n = ++s_rt_throws;
                if ((n & (n - 1)) == 0) // log at 1,2,4,8,… occurrences
                    Msg("!![CSheduler] RT item '%s' threw [%s: %s] — skipped (total %u)", curr.scheduled_name.c_str(), typeid(e).name(), e.what(), n);
                curr.dwTimeOfLastExecute = dwTime;
            }
            catch (...)
            {
                static u32 s_rt_throws = 0;
                const u32 n = ++s_rt_throws;
                if ((n & (n - 1)) == 0) // log at 1,2,4,8,… occurrences
                    Msg("!![CSheduler] RT item '%s' threw [non-std; lua top: %s] — skipped (total %u)", curr.scheduled_name.c_str(),
                        g_lua_error_peek ? g_lua_error_peek() : "<no hook>", n);
                curr.dwTimeOfLastExecute = dwTime;
            }
        }
    }

    if (g_sched_stats) g_sched_rt_ms += double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _t_rt0).count()) / 1e6;

    // Normal (sheduled)
    std::chrono::steady_clock::time_point _t_ps0;
    if (g_sched_stats) _t_ps0 = std::chrono::steady_clock::now();
    ProcessStep();
    if (g_sched_stats)
    {
        const double ps = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _t_ps0).count()) / 1e6;
        g_sched_ps_ms += ps;
        if (ps > g_sched_ps_max) g_sched_ps_max = ps;
        const u32 b = ps <= 0.5 ? 0u : ps <= 1.0 ? 1u : ps <= 2.0 ? 2u : ps <= 4.0 ? 3u : 4u;
        ++g_sched_ps_hist[b];
    }

    // 📏ИЗМЕРЕННЫЙ КУРС ОБМЕНА (19-09-2026, Кордон/день, фикс 60 fps, потато,
    // чередующиеся окна по 25 с в ОДНОМ прогоне, медианы):
    //   rs_sheduler_max   фаза   ProcessStep  вызовов/кадр  худший PS  просрочка
    //   10 (=бюджет 3)   1.21     0.89          68          3.0-3.4     ~5-30 мс
    //   1.5             1.49     1.10          67          2.12          48
    //   1.0             1.33     0.99          65          1.82          87
    //   0.75            1.20     0.88          59          1.36          90
    //   0.5             1.01     0.69          42          1.44         210
    //   0.25            0.87     0.56          37          1.76         300
    // ⭐Что из этого следует, и это НЕОЧЕВИДНО: бюджет 1.0-1.5 мс СРЕЗАЕТ
    // ХВОСТ (3.4 -> 1.8-2.1), но ПОДНИМАЕТ СРЕДНЕЕ на 0.04-0.22 мс при
    // НЕИЗМЕННОМ числе вызовов -- то есть чистое переупорядочивание работы
    // стоит денег. ⚠Механизм ЭТОГО УДОРОЖАНИЯ НЕ УСТАНОВЛЕН: гипотеза
    // "часть работы объекта пропорциональна dt" противоречит тому, что у
    // sched_rate 200 цена ЗА ВЫЗОВ НЕ выросла (14.1 мкс против 13.9 базы),
    // а у бюджета 0.5 при том же числе вызовов -- 16.3 мкс.
    // ⇒ Бюджет имеет смысл только НИЖЕ ~0.75 мс, где он уже РЕЖЕТ работу,
    // а не перекладывает её на следующий кадр.
    //
    // ⚠Нижняя граница была жёсткие 3.f. При rs_sheduler_max < 3 это давало бы
    // clamp(lo > hi) — в std::clamp это UB. Берём min: ниже 3 мс потолок становится
    // жёстким бюджетом (цель и потолок схлопываются в одно число).
    clamp(psShedulerTarget, std::min(3.f, psShedulerMax), psShedulerMax);

    psShedulerCurrent = 0.9f * psShedulerCurrent + 0.1f * psShedulerTarget;
    Device.Statistic->fShedulerLoad = psShedulerCurrent;

    m_processing_now = false;

    // ⚠ВТОРАЯ регистрация, и именно она делает работу: первая в начале апдейта
    // застаёт очередь пустой (замер: 0.000), а сюда приходит всё, что
    // зарегистрировалось и отписалось ЗА апдейт. Мерить надо обе, иначе фаза
    // не сходится с суммой частей -- на чём я и сел.
    std::chrono::steady_clock::time_point _t_reg1;
    if (g_sched_stats) _t_reg1 = std::chrono::steady_clock::now();
    internal_Registration();
    if (g_sched_stats) g_sched_reg_ms += double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _t_reg1).count()) / 1e6;

    // Отчёт по реальным часам: игровые встают на паузе и масштабируются.
    if (g_sched_stats)
    {
        ++g_sched_frames;
        const u32 now = Device.dwTimeContinual;
        if (g_sched_last_ms == 0) g_sched_last_ms = now;
        if (now >= g_sched_last_ms + 5000) { g_sched_last_ms = now; sched_report(); }
    }

    Device.Statistic->Sheduler.End();
}

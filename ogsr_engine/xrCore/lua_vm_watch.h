#pragma once

// Ownership tracker for the one LuaJIT state in this process.
//
// LuaJIT is not thread-safe and there is exactly ONE lua_State here, yet the
// frame has several plausible users: the main thread's object binders, the
// second thread (net_Update -> ALife predicates and the se_* classes), the
// garbage collector, and whatever else the scheduler or the server pool
// happens to run. On 20-08 that ended in a DEP EXECUTE fault at a data address
// inside gc_call_finalizer, with a second worker sitting in lua_error holding
// an empty traceback -- the VM had already come apart.
//
// ⚠This header changes NOTHING about who runs when. It only answers the
// question the fix depends on: WHICH threads actually reach the VM, through
// WHICH call paths, and do any two of them ever overlap. Pick the cure from
// that inventory, not from a guess.
//
// The tracker is deliberately not a lock: taking one here would paper over the
// overlap instead of naming it, and a lock that is never contended is
// indistinguishable from a lock that is never reached.
namespace lua_vm_watch
{
// Console cvar `lua_vm_watch`:
//   0 - off, no atomics, no bookkeeping (the entry/exit scopes still compile in)
//   1 - track ownership, name every thread the first time it reaches the VM,
//       and report overlaps with a stack trace (default)
//   2 - additionally print a stack trace the FIRST time a given thread reaches
//       the VM through a given site; this is what turns "thread id16 enters the
//       VM" into "thread id16 enters the VM from ALife's switch predicates"
extern int enabled;

// Outermost entry wins: nested calls on the same thread only move the depth.
void enter(const char* site);
void leave() noexcept;

// ⭐The window, not just the collision.
//
// Waiting for two threads to be inside the VM at the same INSTANT is waiting for
// luck: the second-thread task spends most of its time outside Lua, so a real
// race can exist for months and never be caught red-handed. Marking the window
// in which the frame's worker task is in flight turns the question into the
// answerable one -- did anyone else reach the VM WHILE it was running -- and that
// is the race, whether or not the two happened to be inside at the same moment.
//
// Open it around the body of the second-thread task in device.cpp.
void worker_window_open();
void worker_window_close() noexcept;

struct worker_window
{
    worker_window() { worker_window_open(); }
    ~worker_window() { worker_window_close(); }

    worker_window(const worker_window&) = delete;
    worker_window& operator=(const worker_window&) = delete;
};

// ⭐ВРЕМЯ ВНУТРИ VM — отдельно от трекера владения.
//
// Планировщик тратит ~1.1 мс кадра, и вопрос "сколько из этого Lua" нельзя
// закрыть ни профайлером рендера, ни таймерами движка: скрипты зовутся из
// shedule_Update каждого объекта, вперемешку с его же C++-работой. Здесь уже
// стоит scope на ВСЕХ входах в VM и уже различается вложенность — значит время
// считается ровно один раз на внешний вход, без двойного счёта.
//
// ⚠Счётчик ПОТОКО-ЛОКАЛЬНЫЙ: второй поток тоже заходит в VM (ALife, се-классы),
// и общий счётчик испортил бы атрибуцию по объектам. Планировщик снимает
// разницу на своём потоке — там это ровно его Lua.
// ⚠Замер идёт ДО проверки `enabled`: трекер владения и хронометраж включаются
// независимо, иначе "сколько Lua" работало бы только вместе с отладкой гонок.
// time_enabled 0 = ни одного обращения к часам.
extern int time_enabled;
u64 lua_time_ns();
u64 lua_calls();

// ⭐ПОФУНКЦИОННЫЙ ПРОФИЛЬ LUA. Замер 18-09: у NPC ~32 входа в VM за апдейт по
// ~8.75 мкс каждый — значит цена не в границе luabind (110 нс) и не в одном
// скрипте, а в трёх десятках функций, делающих реальную работу. Без имён
// дальше не пройти. Копится в luabind::detail::pcall под тем же `lua_time`.
// ⚠18-09: главная статья профиля называлась `[C]:-1` -- миллион вызовов за 5 с,
// и это ИМЯ ВЫЗЫВАЕМОГО: C-функция, то есть метод, зарегистрированный luabind.
// Такое имя не опознаёт ничего. Поэтому ключ теперь ПАРА: имя вызываемого плюс
// адрес возврата из pcall -- вызывающая сторона в движке. Адрес копится сырым
// (`_ReturnAddress()` бесплатен) и разрешается в имя ОДИН раз, при печати.
// `lua_time 2` добавляет ещё два кадра стека -- на случай, если luabind не
// заинлайнил шаблон и первый кадр оказался его собственным переходником.
void lua_prof_add(const char* key, double ms, void* const* ra);

// ⭐СЭМПЛИРУЮЩИЙ ПРОФАЙЛЬ LUA (консольный `lua_sample`, = интервал в тысячах
// инструкций VM; 0 = выкл, разумное значение 4).
//
// ЗАЧЕМ. Профиль в pcall видит только ВХОД движок→скрипт. Он честно называет
// `bind_stalker.script:410` = 143 мкс за вызов и на этом слепнет: всё, что
// внутри, -- это вызовы Lua→Lua, через pcall они не проходят. А цель Switch
// упирается именно в ПОСЛЕДОВАТЕЛЬНУЮ скриптовую часть, то есть в то, что
// внутри этих 143 мкс.
//
// КАК. Счётный хук LuaJIT (LUA_MASKCOUNT) раз в N тысяч инструкций снимает
// верхний кадр Lua-стека и копит два распределения: по ФУНКЦИЯМ (src:linedefined,
// self-время) и по СТРОКАМ (src:currentline). Первое говорит «кто», второе --
// «что именно внутри».
//
// ⚠Хук глушит JIT (ставит диспетч в интерпретатор). Для ЭТОГО движка искажение
// мало: замерено 18-09, без JIT кадр +0.6% -- `pairs()` и замыкания LuaJIT и так
// не компилирует. Но числа отсюда -- ПРОПОРЦИИ, а не абсолютные миллисекунды.
// ⚠Установка/снятие хука трогает диспетч-таблицу, общую на все потоки, поэтому
// делается ТОЛЬКО когда поток владеет VM единолично (owns_vm()). Это прибор для
// управляемого прогона, а не то, что включают в бою.
extern int sample_enabled;
bool owns_vm();                       // текущий поток -- единоличный владелец VM
void lua_sample_add(const char* func, const char* line);
void lua_sample_dump();
void lua_prof_dump();

// ⭐ПЕРЕХВАТ ДОЛГИХ ВХОДОВ В VM НА ОТРЕЗКЕ (05-10-2026, прибор `al_slow_ms`).
//
// Пофункциональный профиль выше копит СУММЫ за 5 с и не отвечает на вопрос
// «что именно было внутри ЭТОГО рывка в 36 мс». Перехват включается на одном
// потоке на отрезок (апдейт одного объекта ALife), и pcall на этом потоке
// записывает самые долгие входы с именами. Имя берётся только у входа, который
// уже оказался долгим (>= capture_min_ms), так что дешёвые вызовы платят лишь
// два чтения часов. Поток-локальный: другие потоки VM не задевает.
void capture_begin();
bool capture_active();
int capture_enter();                      // глубина вложенности входа
void capture_leave();
constexpr double capture_min_ms = 0.25;
void capture_note(const char* key, double ms, int depth);
// Останавливает перехват; при buf != nullptr печатает в него самые долгие входы.
void capture_end(char* buf, unsigned size);

// [spawn prof] / [sched prof] by script function: while a LoadProf Arm is alive on
// this thread, luabind::pcall opens a part named after the function it calls
// ("xr_motivator.script:120 net_spawn"), so the one "lua (x17)" part splits into
// what the scripts cost one by one. Names are interned (kept for the process).
bool prof_armed();
void prof_push(const char* key);
void prof_pop();

// Console command `lua_vm_watch_dump` -- the inventory gathered so far.
void dump();

struct scope
{
    explicit scope(const char* site) { enter(site); }
    ~scope() { leave(); }

    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;
};
} // namespace lua_vm_watch

#define LUA_VM_WATCH_SCOPE(site_literal) ::lua_vm_watch::scope _lua_vm_watch_scope_(site_literal)

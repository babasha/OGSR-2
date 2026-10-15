#pragma once

// ⭐ПРИБОР «КТО СДЕЛАЛ ALIFE UPDATE ДОЛГИМ» (консольный `al_slow_ms`, 0 = выкл).
//
// 📏05-10-2026, Switch v154: задача "ALife update" на втором потоке шла
// 21 -> 36 -> 37 -> 28 -> 18 мс подряд, и главный поток ждал её в конце кадра
// (wait_2nd_tail). Бюджет обхода (process_time 0.9 мс) режет обход только
// МЕЖДУ объектами -- значит, время ушло на один-два неделимых объекта, и прибор
// должен назвать именно их: имя, секцию, фазу (переключение online/offline или
// плановый апдейт) и самые долгие входы в Lua внутри.
// Печатается одна строка `[alife slow]`, только когда весь апдейт дольше порога.
// ⚠Объект может уйти из реестра в собственном апдейте, поэтому item_begin
// запоминает только ID, а имя читается после -- поиском по реестру.
class CSE_Abstract;

namespace alife_slow
{
extern int threshold_ms;

enum EPhase : char
{
    phase_switch = 'w',
    phase_scheduled = 's',
};

void item_begin(const CSE_Abstract* object);
void item_end(EPhase phase);

// Бюджет пачки планового апдейта, мкс (`al_sched_budget_us`, 0 = только штуки,
// как было). См. CALifeScheduleRegistry::CUpdatePredicate.
extern int sched_budget_us;
void note_budget_cut(u32 done_in_batch);
} // namespace alife_slow

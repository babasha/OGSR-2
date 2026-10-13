// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.
#include "stdafx.h"

#include <luabind/detail/pcall.hpp>
#include <luabind/error.hpp>

// Every luabind-initiated call into Lua funnels through here: call_function,
// call_member and functor<> all end at detail::pcall -- there is no second path
// through the headers. That makes this the one place worth asking WHICH THREAD
// is about to run script. See xrCore/lua_vm_watch.h.
#include "lua_vm_watch.h"
#include <chrono>
#ifdef _MSC_VER
#include <intrin.h>
#endif

// ⭐АДРЕС ВЫЗЫВАЮЩЕЙ СТОРОНЫ. Профиль по имени ВЫЗЫВАЕМОГО упёрся в статью
// `[C]:-1` (миллион вызовов за 5 с): вызываемое — C-функция, зарегистрированная
// luabind, и по такому имени не опознать ничего. Кто её зовёт, знает только
// адрес возврата из pcall. `_ReturnAddress()` — интринсик, ноль инструкций
// сверх уже существующего кадра; глубже одного кадра лезем только под
// `lua_time 2`, потому что обход стека стоит уже сотни наносекунд.
#ifdef _MSC_VER
#pragma intrinsic(_ReturnAddress)
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void**, unsigned long*);
#endif

// ---------------------------------------------------------------------------
// ⭐СЭМПЛИРУЮЩИЙ ПРОФАЙЛЬ LUA-СТЕКА (`lua_sample N`, N = тысяч инструкций VM).
//
// Профиль выше видит только ВХОД движок→скрипт. Внутри `bind_stalker:410`
// (143 мкс за вызов) идут вызовы Lua→Lua, и через pcall они не проходят вовсе.
// Счётный хук LuaJIT — единственный способ увидеть, что там.
//
// ⚠Хук ставится ЗДЕСЬ, а не при старте движка, по двум причинам: pcall — это
// точка, где lua_State заведомо есть и заведомо жив, и это же единственное
// место, где известно, что текущий поток владеет VM. lua_sethook правит
// диспетч-таблицу, общую на все потоки; шестнадцать воркеров ALife ходят в ту
// же VM, поэтому переключать хук на ходу можно только владельцу.
// ⚠Верхний кадр (level 0) — это self-время: где выполнение находилось ПРЯМО
// СЕЙЧАС. Это то, что нужно, чтобы отличить «функция дорогая сама» от
// «функция дорогая из-за того, что зовёт».
// ---------------------------------------------------------------------------
static void lua_sample_hook(lua_State* L, lua_Debug*)
{
	lua_Debug ar;
	if (!lua_getstack(L, 0, &ar)) return;
	if (!lua_getinfo(L, "Sl", &ar)) return;

	char kf[96], kl[96];
	const char* src = ar.short_src[0] ? ar.short_src : "?";
	// Хвост пути: полный `...\gamedata\scripts\x.script` в 96 байт не влезает,
	// а полезна в нём ровно последняя часть.
	if (const char* sl = strrchr(src, 0x5C)) src = sl + 1;
	_snprintf(kf, sizeof(kf) - 1, "%s:%d", src, ar.linedefined);
	_snprintf(kl, sizeof(kl) - 1, "%s#%d", src, ar.currentline);
	kf[sizeof(kf) - 1] = 0; kl[sizeof(kl) - 1] = 0;
	lua_vm_watch::lua_sample_add(kf, kl);
}

// Приводит состояние хука к тому, что просит квар. Зовётся из pcall и только
// когда поток — единоличный владелец VM.
static void lua_sample_sync(lua_State* L)
{
	static int s_installed = 0;   // 0 = хука нет, иначе — текущий интервал
	const int want = lua_vm_watch::sample_enabled;
	if (want == s_installed) return;
	if (!lua_vm_watch::owns_vm()) return;

	if (want <= 0)
	{
		lua_sethook(L, nullptr, 0, 0);
		Msg("~[LuaSamp] hook removed");
	}
	else
	{
		lua_sethook(L, lua_sample_hook, LUA_MASKCOUNT, want * 1000);
		Msg("~[LuaSamp] hook set: one sample every %d k VM instructions (JIT off while it is on)", want);
	}
	s_installed = want;
}

namespace luabind { namespace detail
{
	int pcall(lua_State *L, int nargs, int nresults)
	{
		LUA_VM_WATCH_SCOPE("luabind::pcall");

		lua_sample_sync(L);

		// ⭐ПОФУНКЦИОННЫЙ ПРОФИЛЬ (включается `lua_time 1`). Это ЕДИНСТВЕННАЯ
		// воронка вызовов движок→скрипт, поэтому имя берётся здесь и нигде ещё.
		// 📏Замер 18-09: у NPC ~32 входа в VM за апдейт по ~8.75 мкс каждый —
		// значит цена не в границе luabind (110 нс) и не в одном скрипте, а в
		// трёх десятках функций, делающих работу. Коарсные сайты lua_vm_watch
		// их не различают, отсюда этот прибор.
		// ⚠lua_getinfo не бесплатен ⇒ всё под флагом: при выключенном lua_time
		// не выполняется ни одной лишней инструкции.
		char prof_key[96];
		void* prof_ra[3] = { nullptr, nullptr, nullptr };
		bool prof_on = (lua_vm_watch::time_enabled != 0);
		std::chrono::steady_clock::time_point prof_t0;
		if (prof_on)
		{
			prof_key[0] = 0;
			prof_ra[0] = _ReturnAddress();
			if (lua_vm_watch::time_enabled >= 2)
				RtlCaptureStackBackTrace(1, 3, prof_ra, nullptr);
			lua_pushvalue(L, -(nargs + 1));   // копия вызываемой функции
			lua_Debug ar;
			if (lua_getinfo(L, ">nS", &ar))   // ">" ПОТРЕБЛЯЕТ копию со стека
				_snprintf(prof_key, sizeof(prof_key) - 1, "%s:%d %s",
				          ar.short_src[0] ? ar.short_src : "?", ar.linedefined,
				          ar.name ? ar.name : "");
			else
				lua_pop(L, 1);
			prof_key[sizeof(prof_key) - 1] = 0;
			if (!prof_key[0]) prof_on = false;
			else              prof_t0 = std::chrono::steady_clock::now();
		}

		// Перехват долгих входов на отрезке (lua_vm_watch::capture_*, прибор
		// `al_slow_ms`). Имя узнаётся ПОСЛЕ вызова и только у долгого входа, а
		// lua_pcall вызываемую функцию со стека снимает -- поэтому под неё
		// подкладывается копия, которая убирается после.
		const bool cap_on = lua_vm_watch::capture_active();
		int cap_idx = 0, cap_depth = 0;
		std::chrono::steady_clock::time_point cap_t0;
		if (cap_on)
		{
			cap_idx = lua_gettop(L) - nargs;
			lua_pushvalue(L, cap_idx);
			lua_insert(L, cap_idx);
			cap_depth = lua_vm_watch::capture_enter();
			cap_t0 = std::chrono::steady_clock::now();
		}

		pcall_callback_fun e = get_pcall_callback();
		int en = 0;
		if ( e )
		{
			int base = lua_gettop(L) - nargs;
			lua_pushcfunction(L, e);
			lua_insert(L, base);  // push pcall_callback under chunk and args
			en = base;
  		}
		// Под замером спавна/апдейта (LoadProf) — часть с именем вызываемой
		// функции: "lua (x17)" у спавна сталкера распадается на скрипты по одному.
		// Функция здесь на -(nargs+1): копии cap_on и pcall_callback легли ПОД неё.
		const bool lp_on = lua_vm_watch::prof_armed() && lua_isfunction(L, -(nargs + 1));
		if (lp_on)
		{
			char key[96] = "lua:?";
			lua_pushvalue(L, -(nargs + 1));
			lua_Debug ar;
			if (lua_getinfo(L, ">nS", &ar))   // ">" потребляет копию
			{
				const char* src = ar.short_src[0] ? ar.short_src : "?";
				for (const char* p = src; *p; ++p)
					if (*p == '\\' || *p == '/')
						src = p + 1;
				_snprintf(key, sizeof(key) - 1, "lua:%s:%d%s%s", src, ar.linedefined, ar.name ? " " : "", ar.name ? ar.name : "");
			}
			else
				lua_pop(L, 1);
			key[sizeof(key) - 1] = 0;
			lua_vm_watch::prof_push(key);
		}
		int result = lua_pcall(L, nargs, nresults, en);
		if (lp_on)
			lua_vm_watch::prof_pop();
		if ( en )
			lua_remove(L, en);  // remove pcall_callback
		if (cap_on)
		{
			lua_vm_watch::capture_leave();
			const double ms = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - cap_t0).count()) / 1e6;
			if (ms >= lua_vm_watch::capture_min_ms)
			{
				char key[96] = "?";
				lua_pushvalue(L, cap_idx);
				lua_Debug ar;
				if (lua_getinfo(L, ">nS", &ar))   // ">" потребляет копию
				{
					// short_src обрезан с НАЧАЛА пути ("...r gunslinger addon\..."):
					// нужно только имя файла.
					const char* src = ar.short_src[0] ? ar.short_src : "?";
					for (const char* p = src; *p; ++p)
						if (*p == '\\' || *p == '/')
							src = p + 1;
					_snprintf(key, sizeof(key) - 1, "%s:%d%s%s", src, ar.linedefined, ar.name ? " " : "", ar.name ? ar.name : "");
				}
				else
					lua_pop(L, 1);
				key[sizeof(key) - 1] = 0;
				lua_vm_watch::capture_note(key, ms, cap_depth);
			}
			lua_remove(L, cap_idx);
		}
		if (prof_on)
			lua_vm_watch::lua_prof_add(prof_key,
				double(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - prof_t0).count()) / 1e6, prof_ra);
		return result;
	}

	int resume_impl(lua_State *L, int nargs, int)
	{
		LUA_VM_WATCH_SCOPE("luabind::resume");

		return lua_resume(L, nargs);
	}

}}

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

#include <luabind/detail/class_rep.hpp>
#include <luabind/detail/stack_utils.hpp>
#include <luabind/detail/object_rep.hpp>
#include <luabind/detail/method_rep.hpp>


namespace luabind { namespace detail
{
	LUABIND_API void do_call_member_selection(lua_State* L, char const* name)
	{
		object_rep* obj = static_cast<object_rep*>(lua_touserdata(L, -1));
		lua_pop(L, 1); // pop self

		obj->crep()->get_table(L); // push the crep table
		lua_pushstring(L, name);
		lua_gettable(L, -2);
		lua_remove(L, -2); // remove the crep table

		{
			if (!lua_iscfunction(L, -1)) return;
			if (lua_getupvalue(L, -1, 3) == 0) return;
			detail::stack_pop p(L, 1);
			if (lua_touserdata(L, -1) != reinterpret_cast<void*>(0x1337)) return;
		}

		// this (usually) means the function has not been
		// overridden by lua, call the default implementation
		lua_pop(L, 1);
		obj->crep()->get_default_table(L); // push the crep table
		lua_pushstring(L, name);
		lua_gettable(L, -2);
		assert(!lua_isnil(L, -1));
		lua_remove(L, -2); // remove the crep table
	}
}}

namespace luabind { namespace detail
{
	// ⭐ТОЛЬКО ВОПРОС "ПЕРЕОПРЕДЕЛЕНО ЛИ", БЕЗ САМОГО ВЫЗОВА.
	//
	// Это первая половина do_call_member_selection выше, вырезанная дословно.
	// Вторая половина -- подстановка C-фоллбэка `name_static` из default-таблицы,
	// который затем зовётся через lua_pcall и делает ровно `ptr->inherited::f()`.
	// Обёртки DEFINE_LUA_WRAPPER_* могут вызвать эту базу напрямую, если знают,
	// что Lua метод не переопределил -- ради этого знания функция и существует.
	//
	// ⚠Ответ НЕ кэшируется. Он зависит от содержимого таблицы класса, а скрипт
	// вправе доопределить метод после первого вызова; кэш такое молча заморозил
	// бы, и сломанным оказался бы мод, а не движок. Выигрыш и так в том, что мы
	// не платим за pcall, разбор перегрузок и конвертацию аргументов.
	// ⚠Вход: self на вершине стека (как у do_call_member_selection). Стек
	// возвращается в исходное состояние.
	LUABIND_API bool do_member_is_overridden(lua_State* L, char const* name)
	{
		object_rep* obj = static_cast<object_rep*>(lua_touserdata(L, -1));
		lua_pop(L, 1); // pop self
		if (!obj)
			return true;

		obj->crep()->get_table(L); // push the crep table
		lua_pushstring(L, name);
		lua_gettable(L, -2);
		lua_remove(L, -2); // remove the crep table

		bool overridden = true;
		if (lua_iscfunction(L, -1) && lua_getupvalue(L, -1, 3) != 0)
		{
			if (lua_touserdata(L, -1) == reinterpret_cast<void*>(0x1337))
				overridden = false;
			lua_pop(L, 1); // the upvalue
		}
		lua_pop(L, 1); // the member we fetched
		return overridden;
	}
}}

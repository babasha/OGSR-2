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
#include <luabind/detail/object_rep.hpp>
#include <luabind/detail/method_rep.hpp>

//#define TEST_GARBAGE_COLLECTOR

#ifdef TEST_GARBAGE_COLLECTOR
	string256	debug_test_constructor, debug_test_destructor;
	LPCSTR		debug_class = "AttackError";//"stateAttackThreaten";
#endif

namespace luabind { namespace detail
{

	// dest is a function that is called to delete the c++ object this struct holds
	object_rep::object_rep(void* obj, class_rep* crep, int flags, void(*dest)(void*))
		: m_object(obj)
		, m_classrep(crep)
		, m_flags(flags)
		, m_destructor(dest)
		, m_dependency_cnt(1)
	{
		// if the object is owned by lua, a valid destructor must be given
		assert((((m_flags & owner) && dest) || !(m_flags & owner)) && "internal error, please report");
#ifdef TEST_GARBAGE_COLLECTOR
		sprintf(debug_test_constructor,"%s %x %x",m_classrep->name(),m_object,this);
		if (!sz_cmp(debug_class,m_classrep->name()))
			Log(debug_test_constructor);
#endif
	}

	object_rep::object_rep(class_rep* crep, int flags, detail::lua_reference const& table_ref)
		: m_object(0)
		, m_classrep(crep)
		, m_flags(flags)
		, m_lua_table_ref(table_ref)
		, m_destructor(0)
		, m_dependency_cnt(1)
	{
#ifdef TEST_GARBAGE_COLLECTOR
		sprintf(debug_test_constructor,"%s %x %x",m_classrep->name(),m_object,this);
		if (!sz_cmp(debug_class,m_classrep->name()))
			Log(debug_test_constructor);
#endif
	}

	object_rep::~object_rep() 
	{
#ifdef TEST_GARBAGE_COLLECTOR
		sprintf(debug_test_destructor,"%s %x %x",m_classrep->name(),m_object,this);
		if (!sz_cmp(debug_class,m_classrep->name()))
			Log(debug_test_destructor);
#endif
		if (m_flags & owner && m_destructor) m_destructor(m_object);
	}

	void object_rep::remove_ownership()
	{
		assert((m_flags & owner) && "cannot remove ownership of object that's not owned");
		assert(m_classrep->get_class_type() == class_rep::cpp_class
			|| m_classrep->bases().size() == 1 && "can only adopt c++ types or lua classes that derives from a c++ class");

        // daniel040727 Bogus assert above? C++ types can be adopted just fine
        // without a hierarchy?

		m_flags &= ~owner;
		// if this is a type with a wrapper we also have to
		// transform the wrappers weak_ref into a strong
		// reference, to make sure the lua part
		// stays alive as long as the c++ part stays
		// alive.
/*		if (m_classrep->get_class_type() == class_rep::cpp_class)
			m_classrep->adopt(m_flags & constant, m_object);
		else*/

        // daniel040727 I changed the above. It just seems wrong. adopt()
        // should only be called on the wrapper type.
        if (m_classrep->get_class_type() == class_rep::lua_class)
			m_classrep->bases().front().base->adopt(m_flags & constant, m_object);
	}

	void object_rep::set_destructor(void(*ptr)(void*))
	{
		m_destructor = ptr;
	}

	// ⭐OGSR, 04-10-2026: the dependencies live in the userdata's OWN
	// environment table, not in a table pinned through the registry.
	//
	// A registry reference has to be released by somebody, and that somebody
	// was the finalizer -- so any object that might ever become a nurse needed
	// __gc. With the table hanging off the userdata itself the collector sees
	// the link directly: the patient lives exactly as long as the nurse, and
	// there is nothing to release. That is what lets non-owning wrappers be
	// made without a finalizer at all (luaJIT_newuserdata_nofinalizer,
	// policy.hpp). `self` and `index` are stack indices of the nurse userdata
	// and the patient.
	void object_rep::add_dependency(lua_State* L, int self, int index)
	{
		const int top = lua_gettop(L);
		self = self > 0 ? self : top + self + 1;
		index = index > 0 ? index : top + index + 1;

		// The first dependency replaces the env every userdata is born with
		// (the creating function's environment, usually _G) with a table of
		// our own; m_dependency_cnt says whether that happened.
		if (m_dependency_cnt == 1)
		{
			lua_newtable(L);
			lua_setfenv(L, self);
		}

		lua_getfenv(L, self);
		lua_pushvalue(L, index);
		lua_rawseti(L, -2, m_dependency_cnt);
		lua_pop(L, 1);
		++m_dependency_cnt;
	}

	// OGSR: which classes the collector finalizes, and how many. 📏04-10-2026:
	// the atomic phase of LuaJIT's collector (indivisible, 12-14 ms a cycle on
	// the PC at 60 fps) is almost all "userdata to be finalized" -- ~6 MB of
	// dead luabind objects every ~3 s. Each one carries a __gc (this function),
	// so the source of that garbage is what to fix; this names it. Runs on the
	// thread that steps the collector, and gc_census_take is called from there.
	//
	// ⚠05-10-2026: the name is COPIED at finalization. Closing the lua_State
	// (loading a save) finalizes every object, then frees the class_reps; a
	// census that kept only the class_rep* read freed memory on the next
	// gc_census_take. The pointer stays only as the key.
	namespace
	{
		struct census_entry { unsigned n = 0, owned = 0; std::string name; };
		std::unordered_map<const class_rep*, census_entry> s_census;
		unsigned s_census_total = 0;
	}

	void gc_census_take(char* buf, unsigned size, unsigned top)
	{
		std::vector<std::pair<const class_rep*, census_entry>> v(s_census.begin(), s_census.end());
		std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
		int at = snprintf(buf, size, "%u finalized", s_census_total);
		for (unsigned i = 0; i < v.size() && i < top && at > 0 && unsigned(at) < size; ++i)
			at += snprintf(buf + at, size - at, " | %s %u (owned %u)", v[i].second.name.c_str(), v[i].second.n, v[i].second.owned);
		s_census.clear();
		s_census_total = 0;
	}

	int object_rep::garbage_collector(lua_State* L)
	{
		object_rep* obj = static_cast<object_rep*>(lua_touserdata(L, -1));

		{
			census_entry& e = s_census[obj->crep()];
			if (e.n++ == 0)
				e.name = obj->crep() && obj->crep()->name() ? obj->crep()->name() : "?";
			if (obj->flags() & owner)
				++e.owned;
			++s_census_total;
		}

		finalize(L, obj->crep());

		obj->~object_rep();
		return 0;
	}

}}


#include <luabind/detail/class_registry.hpp>

namespace luabind { namespace detail
{
	// OGSR 08-10-2026: what sits on LuaJIT's finalizer-candidate userdata list
	// right now -- the list every atomic phase walks in full (luaJIT_gc_udlist_walk).
	// 📏Switch v227: the "udata" part of the atomic phase was 4-7 ms although only
	// 100-400 KB got finalized a cycle, i.e. the walk over LIVE userdata. Grouped
	// by luabind class (instances), else by metatable; "pending" = not finalized
	// yet, i.e. walked again next cycle.
	void gc_udlist_census(lua_State* L, char* buf, unsigned size, unsigned top)
	{
		buf[0] = 0;
		class_registry* r = class_registry::get_registry(L);
		if (!r)
			return;
		const void* mt_inst[3]{};
		const int refs[3] = {r->cpp_instance(), r->cpp_instance_nogc(), r->lua_instance()};
		for (int i = 0; i < 3; ++i)
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, refs[i]);
			mt_inst[i] = lua_topointer(L, -1);
			lua_pop(L, 1);
		}
		const void* mt_class[2]{};
		const int crefs[2] = {r->cpp_class(), r->lua_class()};
		for (int i = 0; i < 2; ++i)
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, crefs[i]);
			mt_class[i] = lua_topointer(L, -1);
			lua_pop(L, 1);
		}
		lua_rawgeti(L, LUA_REGISTRYINDEX, r->lua_function());
		const void* mt_fn = lua_topointer(L, -1);
		lua_pop(L, 1);

		struct entry { unsigned n = 0, pending = 0; std::string name; };
		struct ctx_t
		{
			const void* const* inst;
			const void* const* cls;
			const void* fn;
			std::unordered_map<const void*, entry> by;
		} ctx{mt_inst, mt_class, mt_fn, {}};

		const unsigned total = luaJIT_gc_udlist_walk(L, [](void* c, void* payload, const void* mt, int pending) {
			ctx_t& x = *static_cast<ctx_t*>(c);
			const void* key = mt;
			const char* nm = nullptr;
			char tmp[32];
			if (mt && (mt == x.inst[0] || mt == x.inst[1] || mt == x.inst[2]))
			{
				const class_rep* crep = static_cast<object_rep*>(payload)->crep();
				key = crep;
				nm = crep && crep->name() ? crep->name() : "?";
			}
			else if (mt && (mt == x.cls[0] || mt == x.cls[1]))
				nm = "(class_rep)";
			else if (mt && mt == x.fn)
				nm = "(luabind function)";
			else if (!mt)
				nm = "(no metatable)";
			entry& e = x.by[key];
			if (e.n++ == 0)
			{
				if (!nm)
				{
					snprintf(tmp, sizeof(tmp), "mt:%p", mt);
					nm = tmp;
				}
				e.name = nm;
			}
			if (pending)
				++e.pending;
		}, &ctx);

		std::vector<const entry*> v;
		v.reserve(ctx.by.size());
		for (const auto& kv : ctx.by)
			v.push_back(&kv.second);
		std::sort(v.begin(), v.end(), [](const entry* a, const entry* b) { return a->n > b->n; });
		int at = snprintf(buf, size, "%u on the list, %u kinds", total, unsigned(v.size()));
		for (unsigned i = 0; i < v.size() && i < top && at > 0 && unsigned(at) < size; ++i)
			at += snprintf(buf + at, size - at, " | %s %u (pending %u)", v[i]->name.c_str(), v[i]->n, v[i]->pending);
	}
}}

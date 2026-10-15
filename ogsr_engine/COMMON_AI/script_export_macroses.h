////////////////////////////////////////////////////////////////////////////
//	Module 		: script_export_macroses.h
//	Created 	: 24.06.2004
//  Modified 	: 24.06.2004
//	Author		: Andy Kolomiets, Dmitriy Iassenev
//	Description : XRay Script export macroses
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <luabind/wrapper_base.hpp>
#include "lua_vm_watch.h"

#define DEFINE_LUABIND_CLASS_WRAPPER_0(a, b, c) luabind::class_<a, b>(c)

#define DEFINE_LUABIND_CLASS_WRAPPER_1(a, b, c, d) luabind::class_<a, b, d>(c)

#define DEFINE_LUABIND_CLASS_WRAPPER_2(a, b, c, d, e) luabind::class_<a, b, bases<d, e>>(c)

#define DEFINE_LUABIND_CLASS_WRAPPER_3(a, b, c, d, e, f) luabind::class_<a, b, bases<d, e, f>>(c)

#define DEFINE_LUABIND_CLASS_WRAPPER_4(a, b, c, d, e, f, g) luabind::class_<a, b, bases<d, e, f, g>>(c)

#define DEFINE_LUABIND_VIRTUAL_FUNCTION(a, b, c) .def(#c, &a::c, &b::c##_static)

#define DEFINE_LUABIND_VIRTUAL_FUNCTION_EXPLICIT_CONST_0(a, b, c, d) .def(#c, (d(a::*)() const)(&a::c), (d(*)(const a*))(&b::c##_static))

// Lua-ошибки из скриптовых серверных классов (se_smart_terrain и т.п.) приходят сюда C++-исключением
// (CScriptEngine::lua_error бросает — иначе luabind звал бы std::terminate). Одна сломанная
// скриптовая сущность не должна валить процесс: void-методы глотают ошибку и продолжают
// (историческое поведение OGSR-патча в proxy_member_void_caller), методы с результатом
// фоллбэчат на C++-базовую реализацию — как вёл бы себя нескриптовый объект.
// Reports one wrapper trap: names the callback AND recovers the Lua error text.
// Defined in script_engine.cpp -- see the note there for why catch(...) on its
// own could never say what was actually wrong.
void OgsrLuaWrapperTrap(const char* func_name);

#define OGSR_LUA_WRAPPER_TRAP(v_func_name) \
    catch (...) { OgsrLuaWrapperTrap(#v_func_name); }

// ---------------------------------------------------------------------------
// ⭐ОБХОД VM, КОГДА LUA МЕТОД НЕ ПЕРЕОПРЕДЕЛИЛ.
//
// 📏18-09-2026, Кордон: профиль по ВЫЗЫВАЮЩЕЙ стороне (`lua_time 2`) назвал
// первую статью — 672 тысячи вызовов за 5 с под именем вызываемого `[C]:-1`,
// то есть C-функция. Вызывающие: used_ai_locations / can_switch_online /
// can_switch_offline этих же обёрток, из CSafeMapIterator::update<CSwitchPredicate>.
//
// Механика была такая. Обёртка БЕЗУСЛОВНО шла в luabind::call_member; luabind в
// do_call_member_selection видел, что в таблице класса лежит его собственный
// диспетчер (апвэлью 0x1337) — то есть Lua метод не переопределил, — подставлял
// зарегистрированный фоллбэк `name_static` и звал ЕГО через lua_pcall. А
// `name_static` делает ровно `ptr->inherited::name()`. Полный круг по VM ради
// C++-метода, который у вызывающего уже был в руках.
//
// ⚠Вопрос "переопределено ли" задаётся КАЖДЫЙ раз, без кэша: скрипт вправе
// доопределить метод класса после первого вызова, и кэш заморозил бы ответ —
// сломанным оказался бы мод, а не движок. Экономится не поиск, а pcall с
// разбором перегрузок и конвертацией аргументов в обе стороны.
// ---------------------------------------------------------------------------
inline bool OgsrWrapperUsesBase(const luabind::wrap_base* self, const char* name)
{
    return !self->has_lua_override(name);
}

#define OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_0(v_func_name) \
    if (OgsrWrapperUsesBase(this, #v_func_name)) \
        return self_type::inherited::v_func_name();

#define OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_1(v_func_name, a1) \
    if (OgsrWrapperUsesBase(this, #v_func_name)) \
        return self_type::inherited::v_func_name(a1);

#define OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_2(v_func_name, a1, a2) \
    if (OgsrWrapperUsesBase(this, #v_func_name)) \
        return self_type::inherited::v_func_name(a1, a2);

#define DEFINE_LUA_WRAPPER_CONST_METHOD_0(v_func_name, ret_type) \
    virtual ret_type v_func_name() const { \
        OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_0(v_func_name) \
        try { return luabind::call_member<ret_type>(this, #v_func_name); } \
        OGSR_LUA_WRAPPER_TRAP(v_func_name) \
        return self_type::inherited::v_func_name(); } \
    static ret_type v_func_name##_static(const inherited* ptr) { return ptr->self_type::inherited::v_func_name(); }

#define DEFINE_LUA_WRAPPER_CONST_METHOD_1(v_func_name, ret_type, t1) \
    virtual ret_type v_func_name(t1 p1) const { \
        OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_1(v_func_name, p1) \
        try { return luabind::call_member<ret_type>(this, #v_func_name, p1); } \
        OGSR_LUA_WRAPPER_TRAP(v_func_name) \
        return self_type::inherited::v_func_name(p1); } \
    static ret_type v_func_name##_static(const inherited* ptr, t1 p1) { return ptr->self_type::inherited::v_func_name(p1); }

#define DEFINE_LUA_WRAPPER_METHOD_V0(v_func_name) \
    virtual void v_func_name() { \
        OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_0(v_func_name) \
        try { luabind::call_member<void>(this, #v_func_name); } \
        OGSR_LUA_WRAPPER_TRAP(v_func_name) } \
    static void v_func_name##_static(inherited* ptr) { ptr->self_type::inherited::v_func_name(); }

#define DEFINE_LUA_WRAPPER_METHOD_V1(v_func_name, t1) \
    virtual void v_func_name(t1 p1) { \
        OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_1(v_func_name, p1) \
        try { luabind::call_member<void>(this, #v_func_name, p1); } \
        OGSR_LUA_WRAPPER_TRAP(v_func_name) } \
    static void v_func_name##_static(inherited* ptr, t1 p1) { ptr->self_type::inherited::v_func_name(p1); }

#define DEFINE_LUA_WRAPPER_METHOD_0(v_func_name, ret_type) \
    virtual ret_type v_func_name() { \
        OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_0(v_func_name) \
        try { return luabind::call_member<ret_type>(this, #v_func_name); } \
        OGSR_LUA_WRAPPER_TRAP(v_func_name) \
        return self_type::inherited::v_func_name(); } \
    static ret_type v_func_name##_static(inherited* ptr) { return ptr->self_type::inherited::v_func_name(); }

#define DEFINE_LUA_WRAPPER_METHOD_1(v_func_name, ret_type, t1) \
    virtual ret_type v_func_name(t1 p1) { \
        OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_1(v_func_name, p1) \
        try { return luabind::call_member<ret_type>(this, #v_func_name, p1); } \
        OGSR_LUA_WRAPPER_TRAP(v_func_name) \
        return self_type::inherited::v_func_name(p1); } \
    static ret_type v_func_name##_static(inherited* ptr, t1 p1) { return ptr->self_type::inherited::v_func_name(p1); }

#define DEFINE_LUA_WRAPPER_METHOD_R2P1_V1(v_func_name, t1) \
    virtual void v_func_name(t1& p1) { \
        OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_1(v_func_name, p1) \
        try { this->template call<void>(#v_func_name, &p1); } \
        OGSR_LUA_WRAPPER_TRAP(v_func_name) } \
    static void v_func_name##_static(inherited* ptr, t1* p1) { ptr->self_type::inherited::v_func_name(*p1); }

#define DEFINE_LUA_WRAPPER_METHOD_R2P1_V2(v_func_name, t1, t2) \
    virtual void v_func_name(t1& p1, t2 p2) { \
        OGSR_WRAPPER_BASE_IF_NOT_OVERRIDDEN_2(v_func_name, p1, p2) \
        try { this->template call<void>(#v_func_name, &p1, p2); } \
        OGSR_LUA_WRAPPER_TRAP(v_func_name) } \
    static void v_func_name##_static(inherited* ptr, t1* p1, t2 p2) { ptr->self_type::inherited::v_func_name(*p1, p2); }

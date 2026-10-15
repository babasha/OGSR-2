////////////////////////////////////////////////////////////////////////////
//	Module 		: script_engine.cpp
//	Created 	: 01.04.2004
//  Modified 	: 01.04.2004
//	Author		: Dmitriy Iassenev
//	Description : XRay Script Engine
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "script_engine.h"
#include "ai_space.h"
#include "MainMenu.h"
#include "object_factory.h"
#include "lua_vm_watch.h"
#include <format>
#include <tracy/TracyLua.hpp>

// FILE_HEADER нужен для того, чтобы классы luabind'а регистрировались внутри модуля в котором находятся, а не в _G
// см. luabind/src/create_class.cpp
constexpr const char* FILE_HEADER =
"\
local function script_name() \
return '{0}' \
end; \
local this; \
module('{0}', package.seeall, function(m) this = m end); \
{1}";

static const char* get_lua_traceback(lua_State* L)
{
    luaL_traceback(L, L, nullptr, 0);
    auto tb = lua_tostring(L, -1);
    lua_pop(L, 1);
    return tb;
}

CScriptEngine::~CScriptEngine()
{
    // Leave the verdict in the log without anyone having to ask for it. A run
    // that ends cleanly should still say who had been inside the VM and whether
    // anything ever came in alongside the second-thread task -- otherwise the
    // only evidence is a console command nobody thought to type.
    lua_vm_watch::dump();

    close();

    Debug.set_crashhandler(nullptr);
}

void CScriptEngine::unload()
{
    extern void destroy_lua_wpn_params();
    destroy_lua_wpn_params();
    if (MainMenu())
    {
        MainMenu()->DestroyInternal(true);
    }

    xr_delete(g_object_factory);

    close(); // вызовем тут явно

    no_files.clear();
}

// [VK-PROBE] downgrade FATAL to Msg so script asserts (e.g. door init failures from inert objects) don't kill the run while we verify rendering. Revert when guards are in place.
// The extra Msg is gated on print_output's "printed" return — a repeat-suppressed
// error must not keep writing its own line either (see the throttle in print_output).
#define DEF_LUA_ERROR_TEMPLATE(L) \
    if (print_output(L, make_string("[%s]", __FUNCTION__).c_str(), LUA_ERRRUN)) \
        Msg("![%s]: %s", __FUNCTION__, lua_isstring(L, -1) ? lua_tostring(L, -1) : "");

int CScriptEngine::lua_panic(lua_State* L)
{
    DEF_LUA_ERROR_TEMPLATE(L)
    return 0;
}

void CScriptEngine::lua_error(lua_State* L)
{
    DEF_LUA_ERROR_TEMPLATE(L)
    // Если просто вернуться, luabind безусловно зовёт std::terminate() (call_member.hpp и родня) —
    // никакой try/catch выше не спасает. Бросаем сами: охраняемые места (OGSR_GUARD_BINDER и т.п.)
    // ловят и продолжают; неохраняемые заканчиваются terminate ровно как раньше.
    throw std::runtime_error("lua error (see log above)");
}

int CScriptEngine::lua_pcall_failed(lua_State* L)
{
    // [VK-PROBE] downgrade FATAL to Msg so script asserts (e.g. door init failures from inert objects) don't kill the run while we verify rendering. Revert when guards are in place.
    if (print_output(L, make_string("[%s]", __FUNCTION__).c_str(), LUA_ERRRUN))
        Msg("![lua_pcall_failed] %s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
    if (lua_isstring(L, -1))
        lua_pop(L, 1);
    return LUA_ERRRUN;
}

static void lua_cast_failed(lua_State* L, LUABIND_TYPE_INFO info)
{
    CScriptEngine::print_output(L, make_string("[%s]", __FUNCTION__).c_str(), LUA_ERRRUN);

    const char* info_name = info->name();

    Msg("!![%s] LUA error: cannot cast lua value to [%s]", __FUNCTION__, info_name);
    // FATAL("[%s] LUA error: cannot cast lua value to [%s]", __FUNCTION__, info_name); //KRodin: Тут наверное вылетать не надо.
}

static int auto_load(lua_State* L)
{
    if ((lua_gettop(L) < 2) || !lua_istable(L, 1) || !lua_isstring(L, 2))
    {
        lua_pushnil(L);
        return 1;
    }
    ai().script_engine().process_file_if_exists(lua_tostring(L, 2), false);
    lua_rawget(L, 1);
    return 1;
}

void CScriptEngine::setup_auto_load()
{
    lua_pushstring(lua(), GlobalNamespace);
    lua_gettable(lua(), LUA_GLOBALSINDEX);
    int value_index = lua_gettop(lua()); // alpet: во избежания оставления в стеке лишней метатаблицы
    luaL_newmetatable(lua(), "XRAY_AutoLoadMetaTable");
    lua_pushstring(lua(), "__index");
    lua_pushcfunction(lua(), auto_load);
    lua_settable(lua(), -3);
    // luaL_getmetatable(lua(), "XRAY_AutoLoadMetaTable");
    lua_setmetatable(lua(), value_index);

    xray_scripts.clear();
}

void CScriptEngine::init()
{
    // Msg("[CScriptEngine::init] Starting LuaJIT!");
    lua_State* LSVM = luaL_newstate(); //Запускаем LuaJIT. Память себе он выделит сам.
    R_ASSERT2(LSVM, "! ERROR : Cannot initialize LUA VM!");
    reinit(LSVM);
    luabind::open(LSVM); //Запуск луабинда

    tracy::LuaRegister(LSVM);

    //--------------Установка калбеков------------------//
    luabind::set_error_callback(lua_error);
    luabind::set_cast_failed_callback(lua_cast_failed);
    luabind::set_pcall_callback(lua_pcall_failed); // KRodin: НЕ ЗАКОММЕНТИРОВАТЬ НИ В КОЕМ СЛУЧАЕ!!!
    lua_atpanic(LSVM, lua_panic);
    //-----------------------------------------------------//
    extern void export_classes(lua_State * L);
    export_classes(LSVM); //Тут регистрируются все движковые функции, импортированные в скрипты
    luaL_openlibs(LSVM); //Инициализация функций LuaJIT
#ifdef __SWITCH__
    // LuaJIT is built without FFI here (interpreter only, see LuaJIT/src/CMakeLists.txt), but the stock
    // _g.script does FFI = require('ffi'). Give it a module whose every member raises a normal Lua error,
    // so the one script that actually uses it (ogse_signals' pcall-guarded timer) falls back cleanly.
    luaL_dostring(LSVM, "package.preload.ffi = function() return setmetatable({}, {__index = function(_, k) error('ffi.' .. tostring(k) .. ' is not available on this platform', 2) end}) end");
#endif
    // world_property(id, bool) is a value: every add_precondition/add_effect COPIES it, and it has only const methods.
    // 📏08-10-2026 Switch: ~450 of them per NPC coming online (state_mgr's GOAP graph + the schemes), 2.2 us and a dead
    // userdata each. One shared instance per (id, value); anything that is not (number, boolean) takes the stock path.
    luaL_dostring(LSVM, "do local ctor, cache = world_property, {} "
                        "world_property = function(id, v) "
                        "if type(id) ~= 'number' or (v ~= true and v ~= false) then return ctor(id, v) end "
                        "local k = v and id * 2 + 1 or id * 2 local p = cache[k] "
                        "if not p then p = ctor(id, v) cache[k] = p end return p end end");
    setup_auto_load(); //Построение метатаблицы
    bool save = m_reload_modules;
    m_reload_modules = true;
    process_file_if_exists(GlobalNamespace, false); //Компиляция _G.script
    m_reload_modules = save;

    m_stack_level = lua_gettop(LSVM); //?

    register_script_classes(); //Походу, запуск class_registrator.script
    object_factory().register_script(); //Регистрация классов

    lua_gc(ai().script_engine().lua(), LUA_GCSETSTEPMUL, 500);
    lua_gc(ai().script_engine().lua(), LUA_GCSETPAUSE, 100);

    // Msg("[CScriptEngine::init] LuaJIT Started!");
}

void CScriptEngine::parse_script_namespace(const char* name, char* ns, u32 nsSize, char* func, u32 funcSize)
{
    const char* p = strrchr(name, '.'); // not `auto`: newlib's strrchr returns char* even for a const input
    if (!p)
    {
        xr_strcpy(ns, nsSize, GlobalNamespace);
        p = name - 1;
    }
    else
    {
        VERIFY(u32(p - name + 1) <= nsSize);
        strncpy(ns, name, p - name);
        ns[p - name] = 0;
    }
    xr_strcpy(func, funcSize, p + 1);
}


static const char* ExtractFileName(const char* fname)
{
    const char* result = fname;
    for (size_t c = 0; c < strlen(fname); c++)
        if (fname[c] == '\\')
            result = &fname[c + 1];
    return result;
}

void CScriptEngine::CollectScriptFiles(const char* path)
{
    if (!strlen(path))
        return;
    string_path fname;
    auto folders = FS.file_list_open(path, FS_ListFolders);
    if (folders)
    {
        std::for_each(folders->begin(), folders->end(), [&](const char* folder) {
            if (strchr(folder, '.'))
            {
                strconcat(sizeof(fname), fname, path, folder);
                CollectScriptFiles(fname);
            }
        });
        FS.file_list_close(folders);
    }

    string_path buff;
    auto files = FS.file_list_open(path, FS_ListFiles);
    if (!files)
        return;
    std::for_each(files->begin(), files->end(), [&](const char* file) {
        strconcat(sizeof(fname), fname, path, file);
        if ((strstr(fname, ".script") /*|| strstr(fname, ".lua")*/) && FS.exist(fname))
        {
            const char* fstart = ExtractFileName(fname);
            strcpy_s(buff, fstart);
            _strlwr_s(buff);
            const char* nspace = strtok(buff, ".");
            xray_scripts.emplace(nspace, fname);
        }
    });
    FS.file_list_close(files);
}

bool CScriptEngine::LookupScript(string_path& fname, const char* base)
{
    string_path lc_base;
    if (xray_scripts.empty())
    {
        FS.update_path(lc_base, "$game_scripts$", "");
        CollectScriptFiles(lc_base);
    }
    strcpy_s(lc_base, base);
    _strlwr_s(lc_base);
    auto it = xray_scripts.find(lc_base);
    if (it != xray_scripts.end())
    {
        strcpy_s(fname, it->second.c_str());
        return true;
    }
    return false;
}

bool CScriptEngine::process_file_if_exists(const char* file_name, bool warn_if_not_exist) // KRodin: Функция проверяет существует ли скрипт на диске. Если существует - отправляет
                                                                                          // его в do_file. Вызывается из process_file, auto_load и не только.
{
    LUA_VM_WATCH_SCOPE("engine::process_file");

    if (!warn_if_not_exist && no_file_exists(file_name)) //Это для оптимизации, чтоб постоянно не проверять, отсутствует ли этот файл.
        return false;
    if (m_reload_modules || (*file_name && !namespace_loaded(file_name)))
    {
        string_path S;
        if (!LookupScript(S, file_name))
        {
            if (warn_if_not_exist)
                MsgDbg("[CScriptEngine::process_file_if_exists] Variable %s not found; No script by this name exists, either.", file_name);
            else
            {
                LogDbg("-------------------------");
                MsgDbg("[CScriptEngine::process_file_if_exists] WARNING: Access to nonexistent variable or loading nonexistent script '%s'", file_name);
                FuncDbg(print_stack());
                LogDbg("-------------------------");
                add_no_file(file_name);
            }
            return false;
        }
        m_reload_modules = false;
        return do_file(S, file_name);
    }
    return true;
}


bool CScriptEngine::process_file(const char* file_name) { return process_file_if_exists(file_name, true); }

bool CScriptEngine::process_file(const char* file_name, bool reload_modules)
{
    m_reload_modules = reload_modules;
    bool result = process_file_if_exists(file_name, true);
    m_reload_modules = false;
    return result;
}

void CScriptEngine::register_script_classes()
{
    luabind::functor<void> result;
    ASSERT_FMT(functor("class_registrator.register", result), "[%s] Cannot load class_registrator!", __FUNCTION__);
    result(const_cast<CObjectFactory*>(&object_factory()));
}

bool CScriptEngine::function_object(const char* function_to_call, luabind::object& object, int type)
{
    LUA_VM_WATCH_SCOPE("engine::function_object");

    if (!strlen(function_to_call))
        return false;
    string256 name_space, function;
    parse_script_namespace(function_to_call, name_space, sizeof(name_space), function, sizeof(function));
    if (xr_strcmp(name_space, GlobalNamespace))
    {
        auto file_name = strchr(name_space, '.');
        if (!file_name)
            process_file(name_space);
        else
        {
            *file_name = 0;
            process_file(name_space);
            *file_name = '.';
        }
    }

    if (!this->object(name_space, function, type))
        return false;
    auto lua_namespace = this->name_space(name_space);
    object = lua_namespace[function];
    return true;
}

void CScriptEngine::collect_all_garbage()
{
    LUA_VM_WATCH_SCOPE("engine::collect_all_garbage");

    lua_gc(lua(), LUA_GCCOLLECT, 0);
    lua_gc(lua(), LUA_GCCOLLECT, 0);
    lua_gc(lua(), LUA_GCCOLLECT, 0);
    lua_gc(lua(), LUA_GCCOLLECT, 0);
}

//*********************************************************************************************
void CScriptEngine::dump_state()
{
    LUA_VM_WATCH_SCOPE("engine::dump_state");

    static bool reentrantGuard = false;
    if (reentrantGuard)
        return;
    reentrantGuard = true;

    lua_State* L = lua();
    lua_Debug l_tDebugInfo;
    for (int i = 0; lua_getstack(L, i, &l_tDebugInfo); ++i)
    {
        lua_getinfo(L, "nSlu", &l_tDebugInfo);

        if (!xr_strcmp(l_tDebugInfo.what, "C"))
        {
            Msg("%2d : [C  ] %s", i, l_tDebugInfo.name ? l_tDebugInfo.name : "");
        }
        else
        {
            string_path temp;
            if (l_tDebugInfo.name)
                xr_sprintf(temp, "%s(%d)", l_tDebugInfo.name, l_tDebugInfo.linedefined);
            else
                xr_sprintf(temp, "function <%s:%d>", l_tDebugInfo.short_src, l_tDebugInfo.linedefined);

            Msg("%2d : [%3s] %s(%d) : %s", i, l_tDebugInfo.what, l_tDebugInfo.short_src, l_tDebugInfo.currentline, temp);
        }

        Msg("\tLocals:");
        const char* name;
        int VarID = 1;
        while ((name = lua_getlocal(L, &l_tDebugInfo, VarID++)) != NULL)
        {
            LogVariable(L, name, 1);

            lua_pop(L, 1); /* remove variable value */
        }
        m_dumpedObjList.clear();
        Msg("\tEnd");
    }
    reentrantGuard = false;
}

void CScriptEngine::LogTable(lua_State* l, LPCSTR S, int level)
{
    if (!lua_istable(l, -1))
        return;

    lua_pushnil(l); /* first key */
    while (lua_next(l, -2) != 0)
    {
        char sname[256];
        char sFullName[256];
        xr_sprintf(sname, "%s", lua_tostring(l, -2));
        xr_sprintf(sFullName, "%s.%s", S, sname);
        LogVariable(l, sFullName, level + 1);

        lua_pop(l, 1); /* removes `value'; keeps `key' for next iteration */
    }
}

void CScriptEngine::LogVariable(lua_State* l, const char* name, int level)
{
    const char* type;
    int ntype = lua_type(l, -1);
    type = lua_typename(l, ntype);

    auto tabBuffer = std::make_unique<char[]>(level + 1);
    memset(tabBuffer.get(), '\t', level);

    char value[128];

    switch (ntype)
    {
    case LUA_TFUNCTION: xr_strcpy(value, "[[function]]"); break;

    case LUA_TTHREAD: xr_strcpy(value, "[[thread]]"); break;

    case LUA_TNUMBER: xr_sprintf(value, "%f", lua_tonumber(l, -1)); break;

    case LUA_TBOOLEAN: xr_sprintf(value, "%s", lua_toboolean(l, -1) ? "true" : "false"); break;

    case LUA_TSTRING: xr_sprintf(value, "%.127s", lua_tostring(l, -1)); break;

    case LUA_TTABLE:
        if (level <= 3)
        {
            Msg("%s Table: %s", tabBuffer.get(), name);
            LogTable(l, name, level + 1);
            return;
        }
        else
        {
            xr_sprintf(value, "[...]");
        }
        break;

    case LUA_TUSERDATA: {
        auto obj = static_cast<luabind::detail::object_rep*>(lua_touserdata(l, -1));

        // Skip already dumped object
        if (m_dumpedObjList.find(obj) != m_dumpedObjList.end())
            return;
        m_dumpedObjList.insert(obj);

        auto& r = obj->get_lua_table();
        if (r.is_valid())
        {
            r.get(l);
            Msg("%s Userdata: %s", tabBuffer.get(), name);
            LogTable(l, name, level + 1);
            lua_pop(l, 1); // Remove userobject
            return;
        }
        else
        {
            // Dump class and element pointer if available
            if (const auto objectClass = obj->crep())
            {
                auto cpp_name = objectClass->type()->name();

                xr_sprintf(value, "(%s): %p", cpp_name ? cpp_name : objectClass->name(), obj->ptr());
            }
            else
                xr_strcpy(value, "[not available]");
        }
    }
    break;

    default: xr_strcpy(value, "[not available]"); break;
    }

    Msg("%s %s %s : %s", tabBuffer.get(), type, name, value);
}
//*********************************************************************************************

static void ScriptCrashHandler(bool dump_lua_locals)
{
    if (!Device.OnMainThread())
        return;

    try
    {
        Msg("***************************[ScriptCrashHandler]**********************************");
        ai().script_engine().print_stack();
        if (dump_lua_locals)
            ai().script_engine().dump_state();
        Msg("*********************************************************************************");
    }
    catch (...)
    {
        Msg("Can't dump script call stack - Engine corrupted");
    }
}

void CScriptEngine::close()
{
    LUA_VM_WATCH_SCOPE("engine::close");

    if (m_virtual_machine)
    {
        // Msg("[CScriptStorage] Closing LuaJIT - start");
        lua_close(m_virtual_machine); // Вот тут закрывается LuaJIT.
        // Msg("[CScriptStorage] Closing LuaJIT - end");
        m_virtual_machine = nullptr;
    }
}

void CScriptEngine::reinit(lua_State* LSVM)
{
    close();

    m_virtual_machine = LSVM;

    Debug.set_crashhandler(ScriptCrashHandler);
}

void CScriptEngine::print_stack()
{
    LUA_VM_WATCH_SCOPE("engine::print_stack");

    Log(get_lua_traceback(lua()));
}

void CScriptEngine::script_log(ScriptStorage::ELuaMessageType tLuaMessageType, const char* caFormat,
                                             ...) // Используется в очень многих местах //Очень много пишет в лог.
{
}

bool CScriptEngine::load_buffer(lua_State* L, const char* caBuffer, size_t tSize, const char* caScriptName,
                                 const char* caNameSpaceName) // KRodin: эта функция форматирует содержимое скрипта используя FILE_HEADER и после этого загружает его в lua
{
    int l_iErrorCode = 0;
    const std::string_view strbuf{caBuffer, tSize};
    if (strcmp(GlobalNamespace, caNameSpaceName)) // Все скрипты кроме _G
    {
        // KRodin: обращаться к _G только с большой буквы! Иначе он загрузится ещё раз и это неизвестно к чему приведёт!
        // Глобальное пространство инитится один раз после запуска луаджита, и никогда больше.
        if (!strcmp("_g", caNameSpaceName))
            return false;

        const std::string script = std::format(FILE_HEADER, caNameSpaceName, strbuf);

        // Log("[CScriptStorage::load_buffer(1)] Loading buffer:");
        // Log(script.c_str());
        l_iErrorCode = luaL_loadbuffer(L, script.c_str(), script.size(), caScriptName);
    }
    else //_G.script и только он.
    {
        // Log("[CScriptStorage::load_buffer(2)] Loading buffer:");
        // Log(strbuf.c_str());
        l_iErrorCode = luaL_loadbuffer(L, strbuf.data(), strbuf.size(), caScriptName);
    }
    if (l_iErrorCode)
    {
        print_output(L, caScriptName, l_iErrorCode);
        R_ASSERT(false); // НЕ ЗАКОММЕНТИРОВАТЬ!
        return false;
    }
    return true;
}

bool CScriptEngine::do_file(
    const char* caScriptName,
    const char* caNameSpaceName) // KRodin: эта функция открывает скрипт с диска и оправляет его содержимое в функцию load_buffer, после этого походу запускает скрипт.
{
    auto l_tpFileReader = FS.r_open(caScriptName);
    if (!l_tpFileReader)
    {
        // заменить на ассерт?
        Msg("!![CScriptStorage::do_file] Cannot open file [%s]", caScriptName);
        return false;
    }

    l_tpFileReader->skip_bom(caScriptName);

    string_path l_caLuaFileName;
    strconcat(sizeof(l_caLuaFileName), l_caLuaFileName, "@", caScriptName); // KRodin: приводит путь к виду @f:\games\s.t.a.l.k.e.r\gamedata\scripts\***.script

    bool loaded = load_buffer(lua(), reinterpret_cast<const char*>(l_tpFileReader->pointer()), (size_t)l_tpFileReader->elapsed(), l_caLuaFileName, caNameSpaceName);

    FS.r_close(l_tpFileReader);
    if (!loaded)
        return false;

    int l_iErrorCode = lua_pcall(lua(), 0, 0, 0); // KRodin: без этого скрипты не работают!
    if (l_iErrorCode)
    {
        print_output(lua(), caScriptName, l_iErrorCode);
        R_ASSERT(false); // НЕ ЗАКОММЕНТИРОВАТЬ!
        return false;
    }
    return true;
}

bool CScriptEngine::namespace_loaded(const char* name, bool remove_from_stack) // KRodin: видимо, функция проверяет, загружен ли скрипт.
{
    LUA_VM_WATCH_SCOPE("engine::namespace_loaded");

    lua_pushstring(lua(), GlobalNamespace);
    lua_rawget(lua(), LUA_GLOBALSINDEX);
    string256 S2;
    xr_strcpy(S2, name);
    auto S = S2;
    for (;;)
    {
        if (!xr_strlen(S))
        {
            VERIFY(lua_gettop(lua()) >= 1);
            lua_pop(lua(), 1);
            VERIFY(start == lua_gettop(lua()));
            return false;
        }
        auto S1 = strchr(S, '.');
        if (S1)
            *S1 = 0;
        lua_pushstring(lua(), S);
        lua_rawget(lua(), -2);
        if (lua_isnil(lua(), -1))
        {
            // lua_settop(lua(),0);
            VERIFY(lua_gettop(lua()) >= 2);
            lua_pop(lua(), 2);
            VERIFY(start == lua_gettop(lua()));
            return false; // there is no namespace!
        }
        else if (!lua_istable(lua(), -1))
        {
            // lua_settop(lua(), 0);
            VERIFY(lua_gettop(lua()) >= 1);
            lua_pop(lua(), 1);
            VERIFY(start == lua_gettop(lua()));
            R_ASSERT3(false, "Error : the namespace is already being used by the non-table object! Name: ", S);
            return false;
        }
        lua_remove(lua(), -2);
        if (S1)
            S = ++S1;
        else
            break;
    }
    if (!remove_from_stack)
        VERIFY(lua_gettop(lua()) == start + 1);
    else
    {
        VERIFY(lua_gettop(lua()) >= 1);
        lua_pop(lua(), 1);
        VERIFY(lua_gettop(lua()) == start);
    }
    return true;
}

bool CScriptEngine::object(const char* identifier, int type)
{
    LUA_VM_WATCH_SCOPE("engine::object");

    lua_pushnil(lua());
    while (lua_next(lua(), -2))
    {
        if (lua_type(lua(), -1) == type && !xr_strcmp(identifier, lua_tostring(lua(), -2)))
        {
            VERIFY(lua_gettop(lua()) >= 3);
            lua_pop(lua(), 3);
            VERIFY(lua_gettop(lua()) == start - 1);
            return true;
        }
        lua_pop(lua(), 1);
    }
    VERIFY(lua_gettop(lua()) >= 1);
    lua_pop(lua(), 1);
    VERIFY(lua_gettop(lua()) == start - 1);
    return false;
}

bool CScriptEngine::object(const char* namespace_name, const char* identifier, int type)
{
    if (xr_strlen(namespace_name) && !namespace_loaded(namespace_name, false))
    {
        VERIFY(lua_gettop(lua()) == start);
        return false;
    }
    bool result = object(identifier, type);
    VERIFY(lua_gettop(lua()) == start);
    return result;
}

luabind::object CScriptEngine::name_space(const char* namespace_name)
{
    LUA_VM_WATCH_SCOPE("engine::name_space");

    string256 S1;
    xr_strcpy(S1, namespace_name);
    auto S = S1;

    auto lua_namespace = luabind::get_globals(lua());

    for (;;)
    {
        if (!xr_strlen(S))
            return lua_namespace;
        auto I = strchr(S, '.');
        if (!I)
            return lua_namespace[S];
        *I = 0;
        lua_namespace = lua_namespace[S];
        S = I + 1;
    }
}

bool CScriptEngine::print_output(lua_State* L, const char* caScriptFileName,
                                  int errorCode) // KRodin: вызывается из нескольких мест, в т.ч. из калбеков lua_error, lua_pcall_failed, lua_cast_failed, lua_panic
{
    LUA_VM_WATCH_SCOPE("engine::print_output");

    auto Prefix = "";
    if (errorCode)
    {
        switch (errorCode)
        {
        case LUA_ERRRUN: Prefix = "SCRIPT RUNTIME ERROR"; break;
        case LUA_ERRMEM: Prefix = "SCRIPT ERROR (memory allocation)"; break;
        case LUA_ERRERR: Prefix = "SCRIPT ERROR (while running the error handler function)"; break;
        case LUA_ERRFILE: Prefix = "SCRIPT ERROR (while running file)"; break;
        case LUA_ERRSYNTAX: Prefix = "SCRIPT SYNTAX ERROR"; break;
        case LUA_YIELD: Prefix = "Thread is yielded"; break;
        default: NODEFAULT;
        }
    }

    // Repeat-offender throttle: a broken mod script erroring every server tick
    // (foreign-map smart terrains: ~600 identical errors/s while flying across the
    // map) turns traceback building + log flushing into 100+ ms/frame of pure
    // logging. Same error => full print for the first 3 occurrences, then one
    // reminder per 500. Returns whether anything was printed so callers can gate
    // their own per-error Msg lines on it.
    // ⚠⚠This throttle is touched from MORE THAN ONE THREAD: script errors arrive
    // both from the main frame and from TTAPI workers (ALife switch predicates,
    // the net processor). An unsynchronised std::map::operator[] rebalances a
    // red-black tree from two threads at once -- undefined behaviour whose two
    // usual outcomes are a traversal that never terminates (a silent freeze) and
    // a garbage dereference (an access violation that arrives as an anonymous
    // "threw [non-std]"). Both of those are exactly what this session kept
    // seeing, so the map gets a lock; it is only ever held for the counter.
    static xr_map<u32, u32> s_seen;
    static std::mutex s_seenLock;
    {
        std::lock_guard<std::mutex> lk(s_seenLock);
        if (s_seen.size() > 256)
            s_seen.clear();
    }

    if (!lua_isstring(L, -1)) // НЕ УДАЛЯТЬ! Иначе будут вылeты без лога!
    {
        // No message string to key on — key on the topmost script frame
        // (source:line, cheap via lua_getinfo); the full traceback is built only
        // when we actually print.
        string256 key = { '?', 0 };
        lua_Debug ar;
        for (int lvl = 0; lvl < 4; ++lvl)
        {
            if (!lua_getstack(L, lvl, &ar) || !lua_getinfo(L, "Sl", &ar))
                break;
            if (ar.currentline > 0)
            {
                xr_sprintf(key, "%s:%d", ar.short_src, ar.currentline);
                break;
            }
        }
        u32 n;
        {
            std::lock_guard<std::mutex> lk(s_seenLock);
            n = ++s_seen[crc32(key, (u32)xr_strlen(key))];
        }
        if (n > 3 && (n % 500) != 0)
            return false;

        auto traceback = get_lua_traceback(L);
        Msg("*********************************************************************************");
        if (n > 3)
            Msg("[print_output(%s)] %s! (repeat #%u, most suppressed)\n%s", caScriptFileName, Prefix, n, traceback);
        else
            Msg("[print_output(%s)] %s!\n%s", caScriptFileName, Prefix, traceback);
        Msg("*********************************************************************************");
        return true;
    }

    auto S = lua_tostring(L, -1);

    u32 n;
    {
        std::lock_guard<std::mutex> lk(s_seenLock);
        n = ++s_seen[crc32(S, (u32)xr_strlen(S))];
    }
    if (n > 3 && (n % 500) != 0)
        return false;

    auto traceback = get_lua_traceback(L);
    Msg("*********************************************************************************");
    if (n > 3)
        Msg("[print_output(%s)] %s (repeat #%u, most suppressed):\n%s\n%s", caScriptFileName, Prefix, n, S, traceback);
    else
        Msg("[print_output(%s)] %s:\n%s\n%s", caScriptFileName, Prefix, S, traceback);
    Msg("*********************************************************************************");
    return true;
}

// ⭐What catch(...) could never tell you.
//
// luabind is built with LUABIND_NO_EXCEPTIONS (Luabind/luabind/config.hpp), so a
// script error unwinds as an opaque throw and the MESSAGE STAYS ON THE LUA STACK.
// The wrapper's catch(...) sees a type it cannot inspect, which is why a session
// died to 109 log lines naming `can_switch_online`/`used_ai_locations`/
// `keep_saved_data_anyway` and not one of them said what was wrong. Reading the
// stack here recovers it.
//
// Throttled BY MESSAGE, not by count: CSafeMapIterator runs the ALife switch
// predicates over every object, so ONE broken script yields ~110 traps in a
// millisecond -- but a genuinely different error still prints immediately
// instead of scrolling away behind the storm.
//
// ⚠The counters are plain statics on purpose. A lost increment costs at worst a
// duplicated or skipped log line, and this runs while something is already
// failing -- it must not become one more lock on the fault path.
void OgsrLuaWrapperTrap(const char* func_name)
{
    LUA_VM_WATCH_SCOPE("engine::wrapper_trap");

    const char* err = "<no lua error on stack>";
    try
    {
        lua_State* L = ai().script_engine().lua();
        if (L && lua_gettop(L) > 0 && lua_isstring(L, -1))
            err = lua_tostring(L, -1);
    }
    catch (...)
    {
        err = "<lua peek failed>";
    }

    const u32 h = crc32(err, (u32)xr_strlen(err));
    static u32 s_hash = 0;
    static u32 s_n = 0;
    if (h != s_hash)
    {
        s_hash = h;
        s_n = 0;
    }
    const u32 n = ++s_n;
    if ((n & (n - 1)) != 0) // print at 1, 2, 4, 8, ... of the SAME message
        return;

    Msg("! [lua wrapper] '%s' threw a Lua error (x%u): %s", func_name, n, err);
}

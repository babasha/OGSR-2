////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder.cpp
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script objects binder
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "ai_space.h"
#include "script_engine.h"
#include "script_binder.h"
#include "xrServer_Objects_ALife.h"
#include "script_binder_object.h"
#include "script_game_object.h"
#include "GameObject.h"
#include "Level.h"

CScriptBinder::CScriptBinder() { init(); }

CScriptBinder::~CScriptBinder() { VERIFY(!m_object); }

void CScriptBinder::init()
{
    m_object = 0;
    m_binder_rt_last_ms = 0;
    m_binder_rt_dt = 0;
}

void CScriptBinder::clear()
{
    // try {
    xr_delete(m_object);
    //}
    // catch(...) {
    //	m_object			= 0;
    //}
    init();
}

void CScriptBinder::reinit()
{
    if (m_object)
    {
        m_object->reinit();
    }
}

void CScriptBinder::Load(LPCSTR section) {}

void CScriptBinder::reload(LPCSTR section)
{
    VERIFY(!m_object);

    // A network client runs no local game logic: object script binders ARE the
    // game logic (quest managers, spawn logic, mod systems), written against a
    // local ALife simulator that a network client deliberately does not have --
    // their main chunks dereference alife() at load time and crash. The
    // authoritative binders run on the process that owns the simulation; a
    // network client only renders replicated state.
    if (g_pGameLevel && !Level().Server)
        return;

    if (!pSettings->line_exist(section, "script_binding"))
        return;

    auto script_func_name = pSettings->r_string(section, "script_binding");
    luabind::functor<void> lua_function;
    if (!ai().script_engine().functor(script_func_name, lua_function))
    {
        Msg("!![CScriptBinder::reload] function [%s] not loaded!", script_func_name);
        return;
    }

    auto game_object = smart_cast<CGameObject*>(this);
    if (!game_object) //Объекта нет - значит тут делать нечего.
    {
        Msg("!![[CScriptBinder::reload] failed cast to CGameObject!");
        return;
    }

    lua_function(game_object->lua_game_object());

    if (m_object)
    {
        m_object->reload(section);
    }
}

BOOL CScriptBinder::net_Spawn(CSE_Abstract* DC)
{
    CSE_Abstract* abstract = (CSE_Abstract*)DC;
    CSE_ALifeObject* object = smart_cast<CSE_ALifeObject*>(abstract);
    if (object && m_object)
    {
        return (BOOL)m_object->net_Spawn(object);
    }
    return TRUE;
}

void CScriptBinder::net_Destroy()
{
    if (m_object)
    {
#ifdef DEBUG
        Msg("* Core object %s is UNbinded from the script object", smart_cast<CGameObject*>(this) ? *smart_cast<CGameObject*>(this)->cName() : "");
#endif // DEBUG
        m_object->net_Destroy();
    }
    xr_delete(m_object);
}

void CScriptBinder::set_object(CScriptBinderObject* object)
{
    VERIFY2(!m_object, "Cannot bind to the object twice!");
#ifdef DEBUG
    Msg("* Core object %s is binded with the script object", smart_cast<CGameObject*>(this) ? *smart_cast<CGameObject*>(this)->cName() : "");
#endif // DEBUG
    m_object = object;
}

// ---------------------------------------------------------------------------
// ⭐ПЕРИОД ОБНОВЛЕНИЯ БИНДЕРА У ОБЪЕКТОВ ОЧЕРЕДИ РЕАЛЬНОГО ВРЕМЕНИ, реальные мс
// (консольный `script_binder_period`; 0 = каждый кадр, историческое поведение).
//
// Актёр зарегистрирован в RT-очереди планировщика (Actor_Network.cpp:197), а она
// обновляется КАЖДЫЙ кадр без всякого интервала. Платится за это апдейт биндера
// игрока (`bind_stalker` -> менеджер сигналов OGSE), то есть скриптовая логика:
// радиация, укрытия, индикаторы, подписки `on_update`. Ничему из этого не нужны
// 60, а тем более 240 Гц.
//
// 📏A/B в ОДНОМ прогоне (Кордон/день, 238 fps, sched_stats + lua_time), строка
// `RT:single_player` и вся фаза планировщика:
//     period   актёр мс/кадр   из них Lua   планировщик всего
//     0        0.227           0.131        0.447   <- база, повтор в конце 0.224-0.231
//     33       0.140           0.047        0.371
//     100      0.130           0.036        0.355
// ⇒ дефолт 33 мс снимает 0.087 мс/кадр с ГЛАВНОГО потока (Lua актёра -64%), а
// 100 мс добавляет к этому лишь 0.01 -- не стоит лишней задержки логики.
// ⚠Остаток 0.09 мс у актёра -- движковая часть его апдейта (камера, управление,
// физика), она гейтом не режется и резаться не должна.
// ⚠Выигрыш пропорционален fps: при 60 fps период 33 мс = "через кадр", то есть
// около -0.065 мс. Слабому железу нужен период БОЛЬШЕ, а не такой же.
//
// ⚠Гейт стоит ТОЛЬКО на RT-пути (см. CGameObject::shedule_Update). У обычной
// очереди интервал уже есть: dwMin = max(30, t_min) мс, и общий гейт резал бы
// биндеры NPC вторично, вдвое против их собственного расписания.
//
// ⚠Часы РЕАЛЬНЫЕ (dwTimeContinual): частота опроса скриптов не должна зависеть
// ни от паузы, ни от time_factor. А вот dt в биндер уходит ИГРОВОЙ и
// НАКОПЛЕННЫЙ -- скриптовые таймеры считают им игровое время, и потерянные
// кадры сместили бы их тем сильнее, чем больше период.
// ---------------------------------------------------------------------------
int g_script_binder_period = 33;

bool CScriptBinder::binder_rt_gate(u32& time_delta)
{
    if (g_script_binder_period <= 0)
        return true;

    m_binder_rt_dt += time_delta;

    const u32 now = Device.dwTimeContinual;
    if (m_binder_rt_last_ms && now - m_binder_rt_last_ms < (u32)g_script_binder_period)
        return false;

    m_binder_rt_last_ms = now;
    time_delta = m_binder_rt_dt;
    m_binder_rt_dt = 0;
    return true;
}

void CScriptBinder::shedule_Update(u32 time_delta)
{
    if (m_object)
    {
        m_object->shedule_Update(time_delta);
    }
}

void CScriptBinder::save(NET_Packet& output_packet)
{
    if (m_object)
    {
        m_object->save(&output_packet);
    }
}

void CScriptBinder::load(IReader& input_packet)
{
    if (m_object)
    {
        m_object->load(&input_packet);
    }
}

BOOL CScriptBinder::net_SaveRelevant()
{
    if (m_object)
    {
        return m_object->net_SaveRelevant();
    }
    return FALSE;
}

void CScriptBinder::net_Relcase(CObject* object)
{
    // 08-10 Switch: called for EVERY object of the level x every destroyed one (CObjectList::ProcessDestroyQueue) -- a
    // corpse's loot was 4 ms of it on the sleeping objects alone. Most (inventory items) have no binder: no cast for them.
    if (!m_object)
        return;
    CGameObject* game_object = smart_cast<CGameObject*>(object);
    if (game_object)
    {
        m_object->net_Relcase(game_object->lua_game_object());
    }
}

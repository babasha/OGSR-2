////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder.h
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script objects binder
////////////////////////////////////////////////////////////////////////////

#pragma once

class CSE_Abstract;
class CScriptBinderObject;
class NET_Packet;

class CScriptBinder
{
protected:
    CScriptBinderObject* m_object;

    // ГЕЙТ ПЕРИОДА ДЛЯ БИНДЕРА RT-ОБЪЕКТОВ (консольный `script_binder_period`).
    // Объект очереди РЕАЛЬНОГО ВРЕМЕНИ (актёр) получает shedule_Update каждый
    // кадр, и вместе с ним каждый кадр зовётся его Lua-биндер. Обоснование и
    // замер -- в script_binder.cpp у binder_rt_gate().
    u32 m_binder_rt_last_ms{}; // dwTimeContinual последнего РАЗРЕШЁННОГО вызова
    u32 m_binder_rt_dt{}; // накопленный игровой dt с того вызова

public:
    CScriptBinder();
    virtual ~CScriptBinder();
    void init();
    void clear();
    virtual void reinit();
    virtual void Load(LPCSTR section);
    virtual void reload(LPCSTR section);
    virtual BOOL net_Spawn(CSE_Abstract* DC);
    virtual void net_Destroy();
    virtual void shedule_Update(u32 time_delta);
    // true = биндер пора обновить; time_delta заменяется на НАКОПЛЕННЫЙ dt,
    // чтобы у скриптов не потерялось игровое время пропущенных кадров.
    bool binder_rt_gate(u32& time_delta);
    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);
    virtual BOOL net_SaveRelevant();
    virtual void net_Relcase(CObject* object);
    void set_object(CScriptBinderObject* object);
    IC CScriptBinderObject* object();
};

#include "script_binder_inline.h"
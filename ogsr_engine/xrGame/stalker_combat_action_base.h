////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_combat_action_base.h
//	Created 	: 25.03.2004
//  Modified 	: 12.01.2007
//	Author		: Dmitriy Iassenev
//	Description : Stalker base combat action
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "stalker_base_action.h"

class CStalkerActionCombatBase : public CStalkerActionBase
{
protected:
    typedef CStalkerActionBase inherited;

private:
    // suppressing fire: the point the burst goes at, picked afresh every
    // fraction of a second around where the enemy went to ground
    Fvector m_suppress_point{};
    u32 m_suppress_repick{};
    bool m_suppress_blocked{};

public:
    // the squad's suppressor fires bursts at where the enemy went to ground,
    // without seeing him; false if this stalker is not the one, or sees him
    bool suppress();
    // ...the fire itself, at the spot: the commander's suppressor (ai_commander.h)
    bool suppress_at(const Fvector& spot);
    // the suppressor's own wall is in the way: he aims, does not fire
    bool suppress_blocked() const { return m_suppress_blocked; }
    CStalkerActionCombatBase(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void finalize();
    void select_queue_params(const float& distance, u32& min_queue_size, u32& max_queue_size, u32& min_queue_interval, u32& max_queue_interval) const;
    bool fire_make_sense() const;
    void fire();
    void aim_ready();
    void aim_ready_force_full();
    void play_panic_sound(u32 max_start_time = 0, u32 min_start_time = 0, u32 max_stop_time = 0, u32 min_stop_time = 0, u32 id = u32(-1));
    void play_attack_sound(u32 max_start_time = 0, u32 min_start_time = 0, u32 max_stop_time = 0, u32 min_stop_time = 0, u32 id = u32(-1));
    void play_start_search_sound(u32 max_start_time = 0, u32 min_start_time = 0, u32 max_stop_time = 0, u32 min_stop_time = 0, u32 id = u32(-1));
};

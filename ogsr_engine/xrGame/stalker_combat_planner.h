////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_combat_planner.h
//	Created 	: 25.03.2004
//  Modified 	: 27.09.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker combat planner
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "action_planner_action_script.h"

class CAI_Stalker;

class CStalkerCombatPlanner : public CActionPlannerActionScript<CAI_Stalker>
{
public:
    enum
    {
        POST_COMBAT_WAIT_INTERVAL = 3000,
        ENEMY_LOST_DELAY = 2500, // nobody in the fight has seen him this long: "where did he go?"
        ENEMY_LOST_WINDOW = 8000, // gone longer than this: the search lines take over instead
        RESUME_INTERVAL = 6000, // back in combat this soon after leaving it: carry on, do not start over
        SEARCH_DELAY = 8000, // nobody has seen him this long: go and look for him
        SIEGE_OWN_LOST = 1500, // ...when he holed up in a house: not seen by this man himself this long, back to the post
        SUPPRESS_START = 700, // lost from sight this long: he went to ground, fire into it (until ai_suppress_time)
    };

private:
    typedef CActionPlannerActionScript<CAI_Stalker> inherited;

private:
    u32 m_last_level_time;
    u16 m_last_enemy_id;
    bool m_last_wounded;

    u16 m_lost_enemy_id{u16(-1)};
    u32 m_enemy_seen_time{};
    u32 m_own_seen_time{}; // when this stalker himself last saw his enemy
    bool m_enemy_lost_cried{};
    u32 m_flank_loss{}; // the loss of him (m_enemy_seen_time) this stalker was sent round for

    u32 m_finalize_time{};
    u16 m_finalize_enemy_id{u16(-1)};

    u32 m_dbg_print_time{};
    u32 m_dbg_action{u32(-1)};
    u32 m_dbg_resumes{};

protected:
    void add_evaluators();
    void add_actions();
    void cry_enemy_lost();
    void update_suppression();
    void dbg_tactics();

public:
    void on_best_cover_changed(const CCoverPoint* new_cover, const CCoverPoint* old_cover);

public:
    CStalkerCombatPlanner(CAI_Stalker* object = 0, LPCSTR action_name = "");
    virtual ~CStalkerCombatPlanner();
    virtual void setup(CAI_Stalker* object, CPropertyStorage* storage);
    virtual void update();
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
    virtual void save(NET_Packet& packet);
    virtual void load(IReader& packet);
};

// What a stalker is doing, by name: the combat action when in the combat
// planner ("take_cover", "search"...), else which planner has him -- for the
// overlay (ai_debug_draw) and the autotest (ai_test).
void stalker_dbg_action(CAI_Stalker& npc, string32& out);

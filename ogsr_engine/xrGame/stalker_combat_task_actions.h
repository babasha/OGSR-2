#pragma once

// ⭐The plan's side of the commander's tasks (ai_commander.h): two world
// properties and two actions, in place of forcing the stock plan's four flags
// from outside (CStalkerCombatPlanner::update, ai_tactics_v2 0).
//
//   has task   the commander gave him one      at place   he is at its place, or it has none
//
//   reach_place          has task, not at place  -> at place
//   fight_from_place     has task, at place      -> no enemy (never reached: it holds while the task does)
//
// The stock cover cycle (take cover, look out, hold, go round, search) and the
// shooting from cover are for a man with no task; the weapon, grenades, the
// wounded, the sudden attack and panic stay the stock's for everybody.

#include "stalker_combat_action_base.h"
#include "stalker_property_evaluators.h"

class CStalkerPropertyEvaluatorHasTask : public CStalkerPropertyEvaluator
{
protected:
    typedef CStalkerPropertyEvaluator inherited;

public:
    CStalkerPropertyEvaluatorHasTask(CAI_Stalker* object = 0, LPCSTR evaluator_name = "");
    virtual _value_type evaluate();
};

class CStalkerPropertyEvaluatorAtPlace : public CStalkerPropertyEvaluator
{
protected:
    typedef CStalkerPropertyEvaluator inherited;

public:
    CStalkerPropertyEvaluatorAtPlace(CAI_Stalker* object = 0, LPCSTR evaluator_name = "");
    virtual _value_type evaluate();
};

// To the task's place: running or walking, gun on what the task watches,
// firing at him if he shows on the way.
class CStalkerActionReachPlace : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

private:
    Fvector m_target{};
    Fvector m_stuck_position{};
    u32 m_stuck_time{};
    Fvector m_completed_for{};
    u32 m_completed_since{};

public:
    CStalkerActionReachPlace(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

// At the place: holds it, crouched unless that sees nothing; fires at him when
// he shows, else what the task says -- bursts into where he went to ground, a
// look round, the gun on a window.
class CStalkerActionFightFromPlace : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

private:
    Fvector m_target{};
    u32 m_serial{u32(-1)}; // the task he holds the place for
    u32 m_arrived{};
    float m_heading{};
    bool m_done{};
    bool m_cried{};

public:
    CStalkerActionFightFromPlace(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

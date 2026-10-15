////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_combat_actions.h
//	Created 	: 25.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker combat action classes
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ai_monster_space.h"

#include "stalker_combat_action_base.h"
#include "random32.h"

class CCoverPoint;

//////////////////////////////////////////////////////////////////////////
// CStalkerActionGetItemToKill
//////////////////////////////////////////////////////////////////////////

class CStalkerActionGetItemToKill : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionGetItemToKill(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionMakeItemKilling
//////////////////////////////////////////////////////////////////////////

class CStalkerActionMakeItemKilling : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionMakeItemKilling(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionRetreatFromEnemy
//////////////////////////////////////////////////////////////////////////

class CStalkerActionRetreatFromEnemy : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionRetreatFromEnemy(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
    virtual _edge_value_type weight(const CSConditionState& condition0, const CSConditionState& condition1) const;
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionGetReadyToKill
//////////////////////////////////////////////////////////////////////////

class CStalkerActionGetReadyToKill : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

private:
    MonsterSpace::EBodyState m_body_state;
    MonsterSpace::EMovementType m_movement_type;
    bool m_affect_properties;
    bool m_enable_enemy_change;

public:
    CStalkerActionGetReadyToKill(bool affect_properties, CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionKillEnemy
//////////////////////////////////////////////////////////////////////////

class CStalkerActionKillEnemy : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionKillEnemy(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionTakeCover
//////////////////////////////////////////////////////////////////////////

class CStalkerActionTakeCover : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

private:
    MonsterSpace::EBodyState m_body_state;
    MonsterSpace::EMovementType m_movement_type;

public:
    CStalkerActionTakeCover(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionLookOut
//////////////////////////////////////////////////////////////////////////

class CStalkerActionLookOut : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

private:
    u32 m_last_change_time;
    CRandom32 m_crouch_look_out_random;

public:
    CStalkerActionLookOut(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionHoldPosition
//////////////////////////////////////////////////////////////////////////

class CStalkerActionHoldPosition : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionHoldPosition(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionGetDistance
//////////////////////////////////////////////////////////////////////////

class CStalkerActionGetDistance : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionGetDistance(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionDetourEnemy
//////////////////////////////////////////////////////////////////////////

class CStalkerActionDetourEnemy : public CStalkerActionCombatBase
{
private:
    const CCoverPoint* m_enemy_cover{};
    // not getting anywhere: where he stood when last checked, and when
    Fvector m_stuck_position{};
    u32 m_stuck_time{};

protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionDetourEnemy(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionSearchEnemy
//////////////////////////////////////////////////////////////////////////

class CStalkerActionSearchEnemy : public CStalkerActionCombatBase
{
private:
    const CCoverPoint* m_enemy_cover{};
    // another stalker already went to look: cover his last position instead
    bool m_watcher{};
    u32 m_watch_start{};
    const CCoverPoint* m_watch_cover{};
    u32 m_watch_cover_time{};
    Fvector m_watch_point{}; // the spot the cover was picked for
    bool m_watch_blind{}; // a blind wall, the only one there was: no use checking it still sees
    bool m_watch_stand{}; // crouched there he sees only the wall: watch standing
    // the siege: the opening of the house this watcher keeps, and the place
    // outside it he keeps it from when no cover there sees in
    int m_post{-1};
    u32 m_post_built{};
    u32 m_post_time{}; // when the post was last claimed
    bool m_post_surplus{}; // a house is being kept, but no post on it for him
    Fvector m_siege_spot{}; // ...and where the house is
    Fvector m_watch_ground{}; // the sector's point: with no cover there, as near to it as he may go
    u32 m_watch_ground_vertex{u32(-1)};
    u32 m_post_vertex{u32(-1)};
    Fvector m_post_position{};
    Fvector m_post_target{}; // in through the opening, at its height

    // The one who goes to look: walk up to where the enemy was last seen, look
    // round there, and if he is not there, call it and comb the hiding places
    // around -- the watchers join in then.
    enum ESearchPhase : u8
    {
        eSearchApproach,
        eSearchCheck,
        eSearchSweep,
    };
    ESearchPhase m_phase{eSearchApproach};
    u32 m_phase_time{};
    Fvector m_search_origin{}; // where we stood when it began: the enemy hides from there
    // the place being checked: a hiding place, or with none left a bit of open
    // ground near the spot
    bool m_sweep_going{};
    Fvector m_sweep_target{};
    u32 m_sweep_vertex{u32(-1)};
    u32 m_sweep_points{};
    bool m_sweep_arrived{};
    u32 m_sweep_arrive_time{};

    void execute_watch(const Fvector& enemy_position, u16 enemy_id, u32 seen);
    // ai_tactics_v2: the watcher's place and the post's through the position query
    void pick_watch_v2(const Fvector& enemy_position, u16 enemy_id);
    bool m_watch_ground_v2{}; // the place is bare ground (m_watch_ground), not a cover
    u32 m_watch_asked{};
    bool m_post_pending{}; // the post's place is being asked for
    // a siege post: false when the enemy was not last seen indoors
    bool execute_post(const Fvector& enemy_position, u16 enemy_id, u32 seen);
    // the searcher into a house: to the wall by its door first
    bool stack_at_door(u16 enemy_id, const Fvector& last_seen, u32 seen);
    bool m_stacked{};
    u32 m_stack_vertex{u32(-1)};
    u32 m_stack_time{};
    Fvector m_stack_position{};
    Fvector m_stack_aim{};
    void execute_sweep(const Fvector& enemy_position, u16 enemy_id);
    bool sweep_open_ground(const Fvector& center, u16 enemy_id);
    // ahead along `heading`, then right, left and behind; true once `duration` is up
    bool look_around(u32 since, u32 duration, float heading);
    static bool claim_search(u16 enemy_id, u16 npc);
    static u32 claim_watch_slot(u16 enemy_id, u16 npc, float bearing);


protected:
    typedef CStalkerActionCombatBase inherited;

public:
    enum
    {
        WATCH_TIME = 25000, // a watcher gives up on a hidden enemy after this long, unless the sweep began
        APPROACH_TIME = 45000, // walking up to his last position at most
        CHECK_TIME = 4000, // looking round at his last position
        SWEEP_TIME = 60000, // combing the area, from the searcher's call
        SWEEP_POINTS = 4, // hiding places one stalker checks
        SWEEP_LOOK_TIME = 3000, // looking round at each of them
    };

    CStalkerActionSearchEnemy(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionPostCombatWait
//////////////////////////////////////////////////////////////////////////

class CStalkerActionPostCombatWait : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionPostCombatWait(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionHideFromGrenade
//////////////////////////////////////////////////////////////////////////

class CStalkerActionHideFromGrenade : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionHideFromGrenade(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionSuddenAttack
//////////////////////////////////////////////////////////////////////////

class CStalkerActionSuddenAttack : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionSuddenAttack(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionKillEnemyIfPlayerOnThePath
//////////////////////////////////////////////////////////////////////////

class CStalkerActionKillEnemyIfPlayerOnThePath : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionKillEnemyIfPlayerOnThePath(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionCriticalHit
//////////////////////////////////////////////////////////////////////////

class CStalkerActionCriticalHit : public CStalkerActionCombatBase
{
protected:
    typedef CStalkerActionCombatBase inherited;

public:
    CStalkerActionCriticalHit(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

// the stalker currently walking up to where this enemy was last seen, u16(-1) if none
u16 stalker_search_holder(u16 enemy_id);
// only one stalker at a time rushes an enemy who has not seen him, whatever his squad
bool stalker_claim_sudden_attack(u16 enemy_id, u16 npc);
// drops every role this stalker holds against any enemy
void stalker_release_claims(u16 npc);
// when the searcher found this enemy gone from his last position and called
// for the area to be combed; 0 if he has not, or did it for an earlier loss
// elsewhere
u32 stalker_sweep_start(u16 enemy_id, const Fvector& last_seen);
// under the roof of the house this enemy is besieged in: no post or cover there
bool stalker_siege_inside(u16 enemy_id, const Fvector& position);
// The enemy was last seen (at level time `seen`) in a house with ways in: the
// watchers keep its openings. Holds: nobody walks in yet.
bool stalker_siege(u16 enemy_id, const Fvector& last_seen, u32 seen);
bool stalker_siege_holds(u16 enemy_id, const Fvector& last_seen, u32 seen);
// Suppressing fire: one stalker per enemy keeps firing at where he went to
// ground while up to two others go round him. Claimed and refreshed every
// update by CStalkerCombatPlanner::update_suppression; lapses within half a
// second of not being refreshed.
bool stalker_claim_suppress(u16 enemy_id, u16 npc);
u16 stalker_suppressor(u16 enemy_id);
bool stalker_claim_flank(u16 enemy_id, u16 npc);
bool stalker_is_flanker(u16 enemy_id, u16 npc);
// true for the first suppressor to call it out in ten seconds
bool stalker_first_to_cry_suppress(u16 enemy_id);

// ⭐For the overlay (ai_debug_draw) and the autotest (ai_test): what the fight
// against an enemy looks like from outside, read-only.
// The role a stalker holds against this enemy; `text` says which, e.g.
// "post door #2/1" -- what the overlay writes over his head.
enum EStalkerDbgRole : u8
{
    eStalkerDbgRoleNone,
    eStalkerDbgRoleSearcher,
    eStalkerDbgRoleSudden,
    eStalkerDbgRoleSuppressor,
    eStalkerDbgRoleFlanker,
    eStalkerDbgRolePost,
    eStalkerDbgRoleWatch,
};
EStalkerDbgRole stalker_dbg_role(u16 enemy_id, u16 npc, string64& text);
// The house this enemy is besieged in, as the siege read it: false if none.
struct SStalkerDbgOpening
{
    Fvector mouth, inside, dir;
    float width;
    bool door, blind;
};
struct SStalkerDbgSiege
{
    bool indoors{};
    u32 start{}; // level time the siege of this house began
    Fvector origin{};
    xr_vector<SStalkerDbgOpening> openings;
};
bool stalker_dbg_siege(u16 enemy_id, SStalkerDbgSiege& out);
// Forgets every claim and siege against this enemy: an autotest run starts
// clean, not inside the house a previous run left "held for 60 s" already.
void stalker_dbg_forget(u16 enemy_id);
// Draws the fights within `range` of `eye` into AIDebugDraw, by its mask.
void stalker_dbg_draw(const Fvector& eye, float range);

#include "stalker_combat_actions_inline.h"
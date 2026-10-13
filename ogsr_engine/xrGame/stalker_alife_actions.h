////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_alife_actions.h
//	Created 	: 25.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker alife action classes
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "stalker_base_action.h"

// ⭐⭐⭐HOW LONG THIS CREATURE SHOULD GO ON HOLDING ITS WEAPON, given that it
// has just (re-)entered an idle behaviour.
//
// The engine's intent is "a stalker who has arrived somewhere keeps his rifle
// out for half a minute, looks around, then slings it". It expressed that as a
// deadline stamped in initialize() -- which is correct exactly once, and wrong
// every time the action is RE-ENTERED without the creature having done
// anything.
//
// ⚠And it is re-entered constantly here. The mod's script layer takes the
// brain and gives it back every few seconds (measured 31-07-2026 on a live
// client: brain alternating alife_planner / <script> at a 6-17 s cadence), and
// each handover re-initialized the action, granted a fresh half-minute, and
// pulled the rifle back off the creature's back. USER-REPORTED as "сначала
// руки за спину, потом обратно".
//
// So the window is granted only to a creature that is NOT already carrying the
// thing slung. Already slung means the half-minute has been served -- serving
// it again is the bug. One function, both actions that carry this timer.
//
// ⚠The intent is read from the planner's stored world state, not from
// weapon_strapped(): that one answers false throughout the four transition
// states and would hand out a fresh window in the middle of a sling.
u32 stalker_weapon_handling_deadline(CAI_Stalker& object);

//////////////////////////////////////////////////////////////////////////
// CStalkerActionGatherItems
//////////////////////////////////////////////////////////////////////////

class CStalkerActionGatherItems : public CStalkerActionBase
{
protected:
    typedef CStalkerActionBase inherited;

public:
    CStalkerActionGatherItems(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

//////////////////////////////////////////////////////////////////////////
// CStalkerActionNoALife
//////////////////////////////////////////////////////////////////////////

class CStalkerActionNoALife : public CStalkerActionBase
{
protected:
    typedef CStalkerActionBase inherited;

protected:
    u32 m_stop_weapon_handling_time{};

public:
    CStalkerActionNoALife(CAI_Stalker* object, LPCSTR action_name = "");
    virtual void initialize();
    virtual void execute();
    virtual void finalize();
};

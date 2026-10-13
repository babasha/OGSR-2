////////////////////////////////////////////////////////////////////////////
//	Module 		: object_handler.h
//	Created 	: 11.03.2004
//  Modified 	: 11.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Object handler
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ai_monster_space.h"

#include "InventoryOwner.h"
#include "graph_engine_space.h"

class CAI_Stalker;
class CWeapon;
class CMissile;
class CFoodItem;
class CObjectHandlerPlanner;

class CObjectHandler : public CInventoryOwner
{
protected:
    typedef CInventoryOwner inherited;
    typedef GraphEngineSpace::_solver_value_type _value_type;
    typedef GraphEngineSpace::_solver_condition_type _condition_type;

private:
    int m_r_hand;
    int m_l_finger1;
    int m_r_finger2;

private:
    mutable int m_strap_bone0;
    mutable int m_strap_bone1;
    mutable ALife::_OBJECT_ID m_strap_object_id;

    // ⭐⭐⭐Whether the SERVER says this creature has its weapon on its back.
    //
    // Every reader of "is it strapped" -- the bones the weapon hangs off
    // (weapon_bones), the offset it hangs at (actualize_strap_mode) and the
    // torso cycle it plays (CStalkerAnimationManager::strapped) -- asks the
    // GOAP planner's eWorldPropertyStrapped. On a pure network client that
    // planner is never even INITIALIZED: CAI_Stalker::shedule_Update returns
    // at `if (!Level().Server)` before the object handler is built. So the
    // property reads false forever and a driven copy holds its rifle out in
    // both hands, permanently, whatever the host has it doing.
    //
    // Measured 30-07-2026: peaceful stalkers on the server sit in
    // `hands=StrappedIdle` (sv_npc_mind) while every one of them is drawn
    // armed on the client. The state existed and had no way across.
    //
    // ⚠This holds the ANSWER, not the inputs. The server sends the result of
    // its own weapon_strapped() -- which already accounts for whether the
    // content lets that weapon be strapped at all and for the four
    // mid-animation states -- so the copy re-derives nothing. Re-deriving is
    // what would need a planner.
    bool m_net_strapped;

    // ...and whether the hands are in the middle of the motion that puts it
    // there or takes it off. The engine's own rule, which weapon_strapped()
    // spells out with current_action_state_id(): during ALL FOUR transition
    // states the weapon is in the HANDS -- it is being carried between the
    // shoulder and the grip, and that is what the animation shows.
    //
    // ⚠Dropping this was my own regression, and the user saw it exactly:
    // "рука с оружием уходит за спину, ничего не происходит, потом возвращает
    // обратно и оружие исчезает". The weapon teleported at whatever moment the
    // 5 Hz poll happened to land, instead of at the end of the motion.
    bool m_net_transition;

    // ⭐⭐⭐...and WHERE THAT MOTION ENDS, plus when it started on THIS
    // machine's clock. Both exist for one reason: the two ends run different
    // clocks and the difference is visible.
    //
    // USER-REPORTED 31-07-2026, in the exact order it happens: "нпс убирает
    // руки за спину чтоб оставить автомат там, потом возвращает руки вперёд, но
    // автомат всё ещё в руке, а потом автомат телепортируется ему за спину."
    //
    // The client's one-shot strap animation gives up after net_transition_ms.
    // The HOST leaves its transition when k_weapon_transition_timeout expires
    // -- 1500 ms, and there are two such states back to back, because a
    // headless server has no skeleton to end the animation for real. So the
    // hands finished a second before the wire admitted the weapon had moved,
    // and the rifle sat in a hand that was no longer holding it out.
    //
    // ⭐The cure is not a longer wait. It is that ONE clock, on the machine
    // that DRAWS this, decides both "the motion is over" and "the weapon has
    // arrived" -- and the destination is known the moment the transition is
    // announced, so nothing needs to be waited for at all.
    bool m_net_transition_to_back;
    u32 m_net_transition_ms;

    // Which branch weapon_bones last took, so a change can be reported without
    // a line per frame per creature. -1 = nothing said yet.
    mutable s8 m_strap_shown;

protected:
    bool m_hammer_is_clutched;
    bool m_infinite_ammo;
    CObjectHandlerPlanner* m_planner;
    mutable bool m_inventory_actual;

public:
    bool m_clutched_hammer_enabled;

private:
    void actualize_strap_mode(CWeapon* weapon) const;

protected:
    IC void switch_torch(CInventoryItem* inventory_item, bool value);

public:
    CObjectHandler();
    virtual ~CObjectHandler();
    virtual void Load(LPCSTR section);
    virtual void reinit(CAI_Stalker* object);
    virtual void reload(LPCSTR section);
    virtual BOOL net_Spawn(CSE_Abstract* DC);
    virtual void update();
    virtual void OnItemTake(CInventoryItem* inventory_item);
    virtual void OnItemDrop(CInventoryItem* inventory_item);
    virtual void attach(CInventoryItem* inventory_item);
    virtual void detach(CInventoryItem* inventory_item);
    CInventoryItem* best_weapon() const;
    void set_goal(MonsterSpace::EObjectAction object_action, CGameObject* game_object = 0, u32 min_queue_size = -1, u32 max_queue_size = -1, u32 min_queue_interval = 300,
                  u32 max_queue_interval = 300);
    void set_goal(MonsterSpace::EObjectAction object_action, CInventoryItem* inventory_item, u32 min_queue_size = -1, u32 max_queue_size = -1, u32 min_queue_interval = 300,
                  u32 max_queue_interval = 300);
    bool goal_reached();
    IC bool hammer_is_clutched() const;
    IC bool const& infinite_ammo() const;
    IC CObjectHandlerPlanner& planner() const;
    void weapon_bones(int& b0, int& b1, int& b2) const;
    bool weapon_strapped() const;
    bool weapon_strapped(CWeapon* weapon) const;
    bool weapon_unstrapped() const;
    bool weapon_unstrapped(CWeapon* weapon) const;

    // ⭐⭐HOW LONG A DRAW OR A HOLSTER LASTS ON THE MACHINE THAT DRAWS IT.
    //
    // One constant, two consumers: the torso cycle (a one-shot with no end
    // event on this side, so it needs a deadline) and the weapon's attachment.
    // They were separate numbers on separate machines, and the gap between them
    // is exactly the defect above. Whatever this is, both leave the transition
    // together.
    static constexpr u32 net_transition_ms = 900;

    // GE_NPC_ACTIVE_ITEM carried the host's answer for this creature, plus
    // whether its hands are mid-motion right now and, if so, which way.
    IC void set_net_strapped(bool value, bool transition, bool to_back)
    {
        // Stamped when a NEW motion starts -- a fresh announcement of the one
        // already running must not restart its clock, or a 5 Hz repeat would
        // keep the weapon in transit for ever.
        if (transition && (!m_net_transition || m_net_transition_to_back != to_back))
            m_net_transition_ms = Device.dwTimeGlobal;

        m_net_strapped = value;
        m_net_transition = transition;
        m_net_transition_to_back = to_back;
    }

    // Where this weapon is HEADED -- the destination of a motion in progress,
    // or simply where it is when none is. ⚠Not the same as strapped_state(),
    // which answers "in the hands" for the length of every transition: a
    // report gated on that one says "in hands (holstering)" at the moment the
    // rifle reaches the back, which is how the receiving-end log came to
    // contradict the bones log three lines above it.
    IC bool net_destination() const { return m_net_transition ? m_net_transition_to_back : m_net_strapped; }

    // ⭐The one question, asked in one place, with two authorities behind it:
    // the planner on the machine that runs the AI, the wire on a machine that
    // only watches. Everything that used to read the GOAP property directly
    // now comes through here -- otherwise "the weapon hangs off the back" and
    // "the man stands as if empty-handed" could disagree, and half a strapped
    // stalker is worse than none.
    bool strapped_state() const;
};

#include "object_handler_inline.h"
////////////////////////////////////////////////////////////////////////////
//	Module 		: object_handler.cpp
//	Created 	: 11.03.2004
//  Modified 	: 11.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Object handler
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "../xrCore/load_prof.h"   // per-spawn breakdown
#include "object_handler.h"
#include "object_handler_space.h"
#include "object_handler_planner.h"
#include "ai_monster_space.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "WeaponMagazined.h"
#include "ef_storage.h"
#include "ef_pattern.h"
#include "ai/stalker/ai_stalker.h"
#include "Inventory.h"
#include "Torch.h"
#include "../Include/xrRender/Kinematics.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "ai_object_location.h"

#include "stalker_animation_manager.h"
#include "object_handler_planner_impl.h"
#include "EffectorShot.h"
#include "Level.h"

CObjectHandler::CObjectHandler()
{
    m_planner = xr_new<CObjectHandlerPlanner>();
    m_inventory_actual = false;
    m_net_strapped = false;
    m_net_transition = false;
    m_net_transition_to_back = false;
    m_net_transition_ms = 0;
    m_strap_shown = -1;
    //	m_last_enemy_for_best_weapon= 0;
}

CObjectHandler::~CObjectHandler() { xr_delete(m_planner); }

void CObjectHandler::Load(LPCSTR section) { inherited::Load(section); }

void CObjectHandler::reinit(CAI_Stalker* object)
{
    inherited::reinit();
    m_hammer_is_clutched = false;
    planner().setup(object);
    IKinematics* kinematics = smart_cast<IKinematics*>(planner().m_object->Visual());
    m_r_hand = kinematics->LL_BoneID(pSettings->r_string(*planner().m_object->cNameSect(), "weapon_bone0"));
    if (m_r_hand == BI_NONE)
        Msg("!![%s] weapon_bone [%s] not found in npc section [%s], npc visual [%s]", __FUNCTION__, pSettings->r_string(planner().m_object->cNameSect().c_str(), "weapon_bone0"),
            planner().m_object->cNameSect().c_str(), planner().object().cNameVisual().c_str());
    m_l_finger1 = kinematics->LL_BoneID(pSettings->r_string(*planner().m_object->cNameSect(), "weapon_bone1"));
    if (m_l_finger1 == BI_NONE)
        Msg("!![%s] weapon_bone [%s] not found in npc section [%s], npc visual [%s]", __FUNCTION__, pSettings->r_string(planner().m_object->cNameSect().c_str(), "weapon_bone1"),
            planner().m_object->cNameSect().c_str(), planner().object().cNameVisual().c_str());
    m_r_finger2 = kinematics->LL_BoneID(pSettings->r_string(*planner().m_object->cNameSect(), "weapon_bone2"));
    if (m_r_finger2 == BI_NONE)
        Msg("!![%s] weapon_bone [%s] not found in npc section [%s], npc visual [%s]", __FUNCTION__, pSettings->r_string(planner().m_object->cNameSect().c_str(), "weapon_bone2"),
            planner().m_object->cNameSect().c_str(), planner().object().cNameVisual().c_str());
    m_strap_object_id = ALife::_OBJECT_ID(-1);
    m_strap_bone0 = -1;
    m_strap_bone1 = -1;
    m_clutched_hammer_enabled = false;
    // A creature coming back online has whatever the host says next, not what
    // the last one at this address was doing.
    m_net_strapped = false;
    m_net_transition = false;
    m_net_transition_to_back = false;
    m_net_transition_ms = 0;
    m_strap_shown = -1;
}

void CObjectHandler::reload(LPCSTR section) { inherited::reload(section); }

BOOL CObjectHandler::net_Spawn(CSE_Abstract* DC)
{
    LoadProf::Scope _lpNetSpawn("objhandler.net_Spawn");
    if (!inherited::net_Spawn(DC))
        return (FALSE);

    CSE_Abstract* abstract = static_cast<CSE_Abstract*>(DC);
    CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(abstract);
    VERIFY(trader);

    m_infinite_ammo = !!trader->m_trader_flags.test(CSE_ALifeTraderAbstract::eTraderFlagInfiniteAmmo);
    return (TRUE);
}

void CObjectHandler::OnItemTake(CInventoryItem* inventory_item)
{
    inherited::OnItemTake(inventory_item);

    m_inventory_actual = false;

    planner().add_item(inventory_item);

    if (planner().object().g_Alive())
        switch_torch(inventory_item, true);

    if (inventory_item->useful_for_NPC() && (inventory_item->object().cNameSect() == m_item_to_spawn))
    {
        m_item_to_spawn = shared_str();
        m_ammo_in_box_to_spawn = 0;
    }

    CWeapon* weapon = smart_cast<CWeapon*>(inventory_item);
    if (weapon)
        planner().object().weapon_shot_effector().Initialize(weapon->camMaxAngle, weapon->camRelaxSpeed_AI, weapon->camMaxAngleHorz, weapon->camStepAngleHorz,
                                                             weapon->camDispertionFrac);
}

void CObjectHandler::OnItemDrop(CInventoryItem* inventory_item)
{
    inherited::OnItemDrop(inventory_item);

    m_inventory_actual = false;

    if (m_infinite_ammo && planner().object().g_Alive() && !inventory_item->useful_for_NPC())
    {
        CWeaponAmmo* weapon_ammo = smart_cast<CWeaponAmmo*>(inventory_item);
        if (weapon_ammo)
        {
            Level().spawn_item(*weapon_ammo->cNameSect(), planner().object().Position(), planner().object().ai_location().level_vertex_id(), planner().object().ID());
            m_item_to_spawn = weapon_ammo->cNameSect();
            m_ammo_in_box_to_spawn = weapon_ammo->m_boxSize;
        }
    }

    planner().remove_item(inventory_item);

    switch_torch(inventory_item, false);
}

CInventoryItem* CObjectHandler::best_weapon() const
{
    if (!planner().object().g_Alive())
        return (0);

    planner().object().update_best_item_info();
    return (planner().object().m_best_item_to_kill);
}

void CObjectHandler::update()
{
    START_PROFILE("Object Handler")
    planner().update();
    STOP_PROFILE
}

void CObjectHandler::set_goal(MonsterSpace::EObjectAction object_action, CGameObject* game_object, u32 min_queue_size, u32 max_queue_size, u32 min_queue_interval,
                              u32 max_queue_interval)
{
    planner().set_goal(object_action, game_object, min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
}

void CObjectHandler::set_goal(MonsterSpace::EObjectAction object_action, CInventoryItem* inventory_item, u32 min_queue_size, u32 max_queue_size, u32 min_queue_interval,
                              u32 max_queue_interval)
{
    set_goal(object_action, inventory_item ? &inventory_item->object() : 0, min_queue_size, max_queue_size, min_queue_interval, max_queue_interval);
}

bool CObjectHandler::goal_reached() { return (planner().solution().size() < 2); }

// Two authorities, one answer. See the comment on m_net_strapped for why a
// driven copy cannot compute this and must be told.
bool CObjectHandler::strapped_state() const
{
    if (!Level().Server)
    {
        // The same two clauses weapon_strapped() applies on the host, in the
        // same order: mid-motion means in the hands, otherwise the AI's intent.
        // The copy gets both as facts because it can compute neither.
        if (m_net_transition)
        {
            // ...but "mid-motion" is a claim about a MOTION, and the motion
            // runs here. While it plays, the weapon is in the hands, carried
            // between the shoulder and the grip. Once it is over, the weapon is
            // WHERE THE MOTION WAS TAKING IT -- which was announced at the
            // start and needs no confirmation from a host whose own transition
            // timer is twice as long. See m_net_transition_to_back.
            if ((Device.dwTimeGlobal - m_net_transition_ms) < net_transition_ms)
                return false;

            return m_net_transition_to_back;
        }

        return m_net_strapped;
    }

    return !!planner().m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped);
}

void CObjectHandler::weapon_bones(int& b0, int& b1, int& b2) const
{
    CWeapon* weapon = smart_cast<CWeapon*>(inventory().ActiveItem());

    // ⭐The decision that actually moves the model, reported when it CHANGES.
    // The animation and the weapon's transform are driven by two different
    // readers of the same state, and they have already been seen to disagree
    // (the intent channel erased the bit for one of them) -- so the one that
    // hangs the rifle says out loud what it decided and on which facts.
    const s8 branch = (weapon && weapon->can_be_strapped() && strapped_state()) ? 1 : 0;
    const bool say = (branch != m_strap_shown);
    m_strap_shown = branch;

    if (!weapon || !weapon->can_be_strapped() || !strapped_state())
    {
        if (say)
            Msg("~ [strap] bones: %u '%s' -> hands | wire/goap=%d can_be_strapped=%d weapon='%s'", planner().m_object ? planner().m_object->ID() : 0xffff,
                planner().m_object ? planner().m_object->cName().c_str() : "?", strapped_state() ? 1 : 0, (weapon && weapon->can_be_strapped()) ? 1 : 0,
                weapon ? weapon->cNameSect().c_str() : "<none>");
        if (weapon)
            weapon->strapped_mode(false);
        b0 = m_r_hand;
        b1 = m_r_finger2;
        b2 = m_l_finger1;
        return;
    }

    if (weapon->ID() != m_strap_object_id)
    {
        IKinematics* kinematics = smart_cast<IKinematics*>(planner().m_object->Visual());
        m_strap_bone0 = kinematics->LL_BoneID(weapon->strap_bone0());
        if (m_strap_bone0 == BI_NONE)
            Msg("!![%s] strap_bone [%s] not found in npc visual [%s], weapon: [%s]", __FUNCTION__, weapon->strap_bone0(), planner().object().cNameVisual().c_str(),
                weapon->cNameSect().c_str());
        m_strap_bone1 = kinematics->LL_BoneID(weapon->strap_bone1());
        if (m_strap_bone1 == BI_NONE)
            Msg("!![%s] strap_bone [%s] not found in npc visual [%s], weapon: [%s]", __FUNCTION__, weapon->strap_bone1(), planner().object().cNameVisual().c_str(),
                weapon->cNameSect().c_str());
        m_strap_object_id = weapon->ID();
    }

    // ⭐Where it actually went. USER-REPORTED the same evening: "оружие
    // исчезает" -- so "the strap branch was taken" is not enough, the bones it
    // was hung on have to be named. A BI_NONE here puts the weapon at a
    // transform built from garbage, which reads exactly like a model that
    // vanished.
    if (say)
        Msg("~ [strap] bones: %u '%s' -> STRAP BONES %d/%d ('%s'/'%s') | weapon='%s' offset_is_strap=%d", planner().m_object ? planner().m_object->ID() : 0xffff,
            planner().m_object ? planner().m_object->cName().c_str() : "?", m_strap_bone0, m_strap_bone1, weapon->strap_bone0() ? weapon->strap_bone0() : "-",
            weapon->strap_bone1() ? weapon->strap_bone1() : "-", weapon->cNameSect().c_str(), weapon->strapped_mode() ? 1 : 0);

    weapon->strapped_mode(true);
    b0 = m_strap_bone0;
    b1 = m_strap_bone1;
    b2 = b1;
}

bool CObjectHandler::weapon_strapped() const
{
    CWeapon* weapon = smart_cast<CWeapon*>(inventory().ActiveItem());
    if (!weapon)
        return (false);

    return (weapon_strapped(weapon));
}

void CObjectHandler::actualize_strap_mode(CWeapon* weapon) const
{
    VERIFY(weapon);

    if (!strapped_state())
    {
        weapon->strapped_mode(false);
        return;
    }

    THROW3(weapon->can_be_strapped(), "Cannot strap weapon", *weapon->cName());
    weapon->strapped_mode(true);
}

bool CObjectHandler::weapon_strapped(CWeapon* weapon) const
{
    VERIFY(weapon);

    if (!weapon->can_be_strapped())
        return (false);

    // ⚠The four mid-animation states are asked of the PLANNER, and a driven
    // copy has none -- current_action_state_id() would be read off a planner
    // that was never set up. It does not need them: the host already excluded
    // them before it sent its answer, so what arrives over the wire is the
    // value this whole function computes, not an input to it.
    if (Level().Server)
    {
        if ((planner().current_action_state_id() == ObjectHandlerSpace::eWorldOperatorStrapping2Idle) ||
            (planner().current_action_state_id() == ObjectHandlerSpace::eWorldOperatorStrapping) ||
            (planner().current_action_state_id() == ObjectHandlerSpace::eWorldOperatorUnstrapping2Idle) ||
            (planner().current_action_state_id() == ObjectHandlerSpace::eWorldOperatorUnstrapping))
        {
            return (false);
        }
    }

    actualize_strap_mode(weapon);

    return (weapon->strapped_mode());
}

bool CObjectHandler::weapon_unstrapped() const
{
    CWeapon* weapon = smart_cast<CWeapon*>(inventory().ActiveItem());
    if (!weapon)
        return (true);

    return (weapon_unstrapped(weapon));
}

bool CObjectHandler::weapon_unstrapped(CWeapon* weapon) const
{
    VERIFY(weapon);

    if (!weapon->can_be_strapped())
        return (true);

    // Same reason as weapon_strapped: no planner on a driven copy.
    if (Level().Server)
    {
        switch (planner().current_action_state_id())
        {
        case ObjectHandlerSpace::eWorldOperatorStrapping2Idle:
        case ObjectHandlerSpace::eWorldOperatorStrapping:
        case ObjectHandlerSpace::eWorldOperatorUnstrapping2Idle:
        case ObjectHandlerSpace::eWorldOperatorUnstrapping: return (false);
        }
    }

    actualize_strap_mode(weapon);

    VERIFY(!Level().Server || (planner().current_action_state_id() != ObjectHandlerSpace::eWorldOperatorStrapped) || weapon->strapped_mode());

    return (!weapon->strapped_mode());
}

IC void CObjectHandler::switch_torch(CInventoryItem* inventory_item, bool value)
{
    CTorch* torch = smart_cast<CTorch*>(inventory_item);
    if (torch && attached(torch) && planner().object().g_Alive())
        torch->Switch(value);
}

void CObjectHandler::attach(CInventoryItem* inventory_item)
{
    inherited::attach(inventory_item);
    switch_torch(inventory_item, true);
}

void CObjectHandler::detach(CInventoryItem* inventory_item)
{
    switch_torch(inventory_item, false);
    inherited::detach(inventory_item);
}

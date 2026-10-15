////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker_script.cpp
//	Created 	: 29.09.2003
//  Modified 	: 29.09.2003
//	Author		: Dmitriy Iassenev
//	Description : Stalker script functions
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "ai_stalker.h"
#include "../../stalker_animation_manager.h"
#include "../../script_entity_action.h"
#include "../../Torch.h"
#include "../../Inventory.h"
#include "../../Weapon.h"
#include "../../WeaponMagazined.h"
#include "../../../Include/xrRender/Kinematics.h"
#include "script_engine.h"
#include "../../sight_manager.h"
#include "../../stalker_movement_manager.h"
#include "../../ai_space.h"
// "does this creature currently mean to carry its weapon slung" -- see
// bfAssignObject's no-object branch.
#include "../../object_handler_planner.h"
#include "../../object_handler_space.h"

CWeapon* CAI_Stalker::GetCurrentWeapon() const { return (smart_cast<CWeapon*>(inventory().ActiveItem())); }

// CInventoryItem *CAI_Stalker::GetCurrentEquipment() const
//{
//     return inventory().m_slots[OUTFIT_SLOT].m_pIItem;
// }

CInventoryItem* CAI_Stalker::GetMedikit() const
{
#pragma todo("Dima to Dima : Return correct medikit")
    return (0);
}

CInventoryItem* CAI_Stalker::GetFood() const
{
#pragma todo("Dima to Dima : Return correct food")
    return (0);
}

void CAI_Stalker::ResetScriptData(void* P) { inherited::ResetScriptData(P); }

bool CAI_Stalker::bfAssignMovement(CScriptEntityAction* tpEntityAction)
{
    if (!inherited::bfAssignMovement(tpEntityAction))
        return (false);

    CScriptMovementAction& l_tMovementAction = tpEntityAction->m_tMovementAction;
    CScriptWatchAction& l_tWatchAction = tpEntityAction->m_tWatchAction;
    CScriptAnimationAction& l_tAnimationAction = tpEntityAction->m_tAnimationAction;
    CScriptObjectAction& l_tObjectAction = tpEntityAction->m_tObjectAction;

    CObjectHandler::set_goal(l_tObjectAction.m_tGoalType);

    movement().set_path_type(movement().path_type());
    movement().set_detail_path_type(l_tMovementAction.m_tPathType);
    movement().set_body_state(l_tMovementAction.m_tBodyState);
    movement().set_movement_type(l_tMovementAction.m_tMovementType);
    movement().set_mental_state(l_tAnimationAction.m_tMentalState);
    sight().setup(l_tWatchAction.m_tWatchType, &l_tWatchAction.m_tWatchVector);
    movement().update(Device.dwTimeDelta);
    sight().update();

    return (true);
}

bool CAI_Stalker::bfAssignWatch(CScriptEntityAction* tpEntityAction)
{
    if (!inherited::bfAssignWatch(tpEntityAction))
        return (false);

    CScriptWatchAction& l_tWatchAction = tpEntityAction->m_tWatchAction;

    //	float			&yaw = movement().m_head.target.yaw, &pitch = movement().m_head.target.pitch;

    switch (l_tWatchAction.m_tGoalType)
    {
    case CScriptWatchAction::eGoalTypeObject: {
        if (!xr_strlen(l_tWatchAction.m_bone_to_watch))
            l_tWatchAction.m_tpObjectToWatch->Center(l_tWatchAction.m_tWatchVector);
        else
        {
            CBoneInstance& l_tBoneInstance =
                smart_cast<IKinematics*>(l_tWatchAction.m_tpObjectToWatch->Visual())
                    ->LL_GetBoneInstance(smart_cast<IKinematics*>(l_tWatchAction.m_tpObjectToWatch->Visual())->LL_BoneID(l_tWatchAction.m_bone_to_watch));
            Fmatrix l_tMatrix;

            l_tMatrix = l_tBoneInstance.mTransform;
            l_tMatrix.mulA_43(l_tWatchAction.m_tpObjectToWatch->XFORM());

            l_tWatchAction.m_tWatchVector = l_tMatrix.c;
        }
        sight().setup(l_tWatchAction.m_tWatchType, &l_tWatchAction.m_tWatchVector);
        break;
    }
    case CScriptWatchAction::eGoalTypeDirection: {
        sight().setup(l_tWatchAction.m_tWatchType, &l_tWatchAction.m_tWatchVector);
        break;
    }
    case CScriptWatchAction::eGoalTypeWatchType: {
        break;
    }
    case CScriptWatchAction::eGoalTypeCurrent: {
        l_tWatchAction.m_tWatchType = SightManager::eSightTypeCurrentDirection;
        l_tWatchAction.m_bCompleted = true;
        return (false);
    }
    default: NODEFAULT;
    }

    if ((CScriptWatchAction::eGoalTypeWatchType != l_tWatchAction.m_tGoalType) && (angle_difference(movement().m_head.target.yaw, movement().m_head.current.yaw) < EPS_L) &&
        (angle_difference(movement().m_head.target.pitch, movement().m_head.current.pitch) < EPS_L))
        l_tWatchAction.m_bCompleted = true;
    else
        l_tWatchAction.m_bCompleted = false;

    return (!l_tWatchAction.m_bCompleted);
}

bool CAI_Stalker::bfAssignObject(CScriptEntityAction* tpEntityAction)
{
    CScriptObjectAction& l_tObjectAction = tpEntityAction->m_tObjectAction;
    CInventoryItem* l_tpInventoryItem = smart_cast<CInventoryItem*>(l_tObjectAction.m_tpObject);

    if (!inherited::bfAssignObject(tpEntityAction) || !l_tObjectAction.m_tpObject || !l_tpInventoryItem)
    {
        if (!inventory().ActiveItem())
        {
            CObjectHandler::set_goal(eObjectActionIdle);
        }
        else
        {
            // ⭐⭐⭐The script action named NO object, which means it has no
            // opinion about the weapon -- not "draw it".
            //
            // Forcing eObjectActionIdle here takes the rifle off the creature's
            // back for the duration of every scripted action that is about
            // something else (walk here, watch there), and the alife planner
            // slings it again the moment the script lets go. Measured
            // 30-07-2026 on a connected client: a stalker alone in an empty
            // field, mental=free, no enemy, no danger, drawing and holstering
            // every eight seconds forever, with the goal alternating
            // want:[idle=1] / want:[idle_strap=1] as script control came and
            // went. USER-REPORTED as "убирает через раз".
            //
            // ⚠The intent is read from the planner's world state, not from
            // weapon_strapped(): that one answers false throughout the four
            // transition states, so using it here would cancel a sling halfway
            // and start the cycle from the other side.
            const bool slung = CObjectHandler::planner().initialized() && CObjectHandler::planner().m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped);

            CObjectHandler::set_goal(slung ? eObjectActionStrapped : eObjectActionIdle, inventory().ActiveItem());
        }

        return ((l_tObjectAction.m_bCompleted = (CObjectHandler::goal_reached())) == false);
    }

    if (!l_tpInventoryItem->object().H_Parent())
        return (true);

    CWeapon* l_tpWeapon = smart_cast<CWeapon*>(inventory().ActiveItem());
    CWeaponMagazined* l_tpWeaponMagazined = smart_cast<CWeaponMagazined*>(inventory().ActiveItem());

    if (l_tpWeaponMagazined)
        l_tpWeaponMagazined->SetQueueSize(l_tObjectAction.m_dwQueueSize);

    switch (l_tObjectAction.m_tGoalType)
    {
    case eObjectActionIdle: {
        if (!l_tpWeapon)
        {
            l_tObjectAction.m_bCompleted = true;
            return false;
        }
        CObjectHandler::set_goal(eObjectActionIdle, l_tpInventoryItem);
        //			inventory().Action	(kWPN_FIRE,	CMD_STOP);
        return ((l_tObjectAction.m_bCompleted = (CObjectHandler::goal_reached())) == false);
        break;
    }
    case eObjectActionFire1: {
        CObjectHandler::set_goal(eObjectActionFire1, l_tpInventoryItem);
        if (inventory().ActiveItem() && l_tpWeapon)
        {
            if (!l_tpWeapon->GetAmmoElapsed() && !l_tpWeapon->GetAmmoCurrent())
                l_tObjectAction.m_bCompleted = true;
        }
        break;
    }
    case eObjectActionFire2: {
        CObjectHandler::set_goal(eObjectActionFire2, l_tpInventoryItem);
        if (inventory().ActiveItem())
        {
            if (!l_tpWeapon->GetAmmoElapsed() && !l_tpWeapon->GetAmmoCurrent())
                l_tObjectAction.m_bCompleted = true;
        }
        break;
    }
    case eObjectActionReload2:
    case eObjectActionReload1: {
        if (!l_tpWeapon)
        {
            l_tObjectAction.m_bCompleted = true;
            return false;
        }
        CObjectHandler::set_goal(eObjectActionReload1, l_tpInventoryItem);
        if (inventory().ActiveItem()->object().ID() == l_tObjectAction.m_tpObject->ID())
        {
            if (CWeapon::eReload == l_tpWeapon->GetState())
                l_tObjectAction.m_bCompleted = true;
        }
        else
            ai().script_engine().script_log(ScriptStorage::eLuaMessageTypeError, "cannot reload active item because it is not selected!");
        break;
    }
    case eObjectActionActivate: {
        CTorch* torch = smart_cast<CTorch*>(l_tObjectAction.m_tpObject);
        if (torch)
        {
            torch->Switch(true);
            break;
        }
        CObjectHandler::set_goal(eObjectActionIdle, l_tpInventoryItem);
        return ((l_tObjectAction.m_bCompleted = (CObjectHandler::goal_reached())) == false);

        break;
    }
    case eObjectActionDeactivate: {
        CTorch* torch = smart_cast<CTorch*>(l_tObjectAction.m_tpObject);
        if (torch)
        {
            torch->Switch(false);
            break;
        }
        //				inventory().Activate(u32(-1));
        CObjectHandler::set_goal(eObjectActionIdle);
        return ((l_tObjectAction.m_bCompleted = (CObjectHandler::goal_reached())) == false);
        break;
    }
    case eObjectActionUse: {
        CObjectHandler::set_goal(eObjectActionUse);
        return ((l_tObjectAction.m_bCompleted = (CObjectHandler::goal_reached())) == false);
        break;
    }
    case eObjectActionTake: {
        if (l_tObjectAction.m_tpObject->H_Parent() == this)
        {
            Msg("!![%s] item [%s] is already in the inventory!", __FUNCTION__, l_tObjectAction.m_tpObject->cName().c_str());
            l_tObjectAction.m_bCompleted = true;
            return false;
        }
        feel_touch_new(l_tObjectAction.m_tpObject);
        l_tObjectAction.m_bCompleted = true;
        break;
    }
    case eObjectActionDrop: {
        if (l_tObjectAction.m_tpObject->H_Parent() != this)
        {
            Msg("!![%s] item [%s] is not in the inventory!", __FUNCTION__, l_tObjectAction.m_tpObject->cName().c_str());
            l_tObjectAction.m_bCompleted = true;
            return false;
        }
        DropItemSendMessage(l_tObjectAction.m_tpObject);
        break;
    }
    default: NODEFAULT;
    }

    return (true);
}

bool CAI_Stalker::bfAssignAnimation(CScriptEntityAction* tpEntityAction)
{
    if (!inherited::bfAssignAnimation(tpEntityAction))
        return (false);

    if (xr_strlen(tpEntityAction->m_tAnimationAction.m_caAnimationToPlay))
    {
        animation().torso().reset();
        animation().legs().reset();
    }

    return (true);
}

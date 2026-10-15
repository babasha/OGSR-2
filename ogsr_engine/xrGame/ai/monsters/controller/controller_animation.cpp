#include "StdAfx.h"
#include "controller_animation.h"
#include "controller.h"
#include "../../../detail_path_manager.h"
#include "../../../Level.h"
#include "../control_direction_base.h"
#include "../control_path_builder_base.h"
#include "controller_direction.h"
#include "../monster_velocity_space.h"

const float _pmt_psy_attack_time = 0.5f;

void CControllerAnimation::reinit()
{
    m_controller = smart_cast<CController*>(m_object);

    load();
    inherited::reinit();

    set_body_state(eTorsoIdle, eLegsTypeStand);

    m_man->animation().add_anim_event(m_torso[eTorsoPsyAttack], _pmt_psy_attack_time, CControlAnimation::eAnimationHit);
    m_wait_torso_anim_end = false;
}

void CControllerAnimation::on_start_control(ControlCom::EControlType type)
{
    switch (type)
    {
    case ControlCom::eControlAnimation:
        m_man->subscribe(this, ControlCom::eventAnimationEnd);
        m_man->subscribe(this, ControlCom::eventTorsoAnimationEnd);
        m_man->subscribe(this, ControlCom::eventLegsAnimationEnd);

        on_switch_controller();
        break;
    }
}

void CControllerAnimation::on_stop_control(ControlCom::EControlType type)
{
    switch (type)
    {
    case ControlCom::eControlAnimation:
        m_man->unsubscribe(this, ControlCom::eventAnimationEnd);
        m_man->unsubscribe(this, ControlCom::eventTorsoAnimationEnd);
        m_man->unsubscribe(this, ControlCom::eventLegsAnimationEnd);
        break;
    }
}

void CControllerAnimation::on_event(ControlCom::EEventType type, ControlCom::IEventData* data)
{
    switch (type)
    {
    case ControlCom::eventAnimationEnd: {
        select_animation(true);
        m_state_attack = false;
        break;
    }
    case ControlCom::eventTorsoAnimationEnd:
        m_wait_torso_anim_end = false;
        select_torso_animation();
        break;
    case ControlCom::eventLegsAnimationEnd: select_legs_animation(); break;
    case ControlCom::eventAnimationSignal: {
        SAnimationSignalEventData* event_data = (SAnimationSignalEventData*)data;
        if (event_data->event_id == CControlAnimation::eAnimationHit)
        {
            if (event_data->motion == m_torso[eTorsoPsyAttack])
                m_controller->psy_fire();
            else
                check_hit(event_data->motion, event_data->time_perc);
            break;
        }
    }
    }
}

void CControllerAnimation::update_frame()
{
    inherited::update_frame();
    return;
}

void CControllerAnimation::load()
{
    IKinematicsAnimated* skeleton = smart_cast<IKinematicsAnimated*>(m_object->Visual());

    m_legs[eLegsStand] = skeleton->ID_Cycle_Safe("new_idle_0");
    m_legs[eLegsSteal] = skeleton->ID_Cycle_Safe("new_cr_idle_0");
    m_legs[eLegsRun] = skeleton->ID_Cycle_Safe("new_run_fwd_0");
    m_legs[eLegsWalk] = skeleton->ID_Cycle_Safe("new_walk_0");
    m_legs[eLegsBackRun] = skeleton->ID_Cycle_Safe("new_run_beack_0");
    m_legs[eLegsRunFwdLeft] = skeleton->ID_Cycle_Safe("stand_fwd_ls");
    m_legs[eLegsRunFwdRight] = skeleton->ID_Cycle_Safe("stand_fwd_rs");
    m_legs[eLegsRunBkwdLeft] = skeleton->ID_Cycle_Safe("stand_bwd_ls");
    m_legs[eLegsRunBkwdRight] = skeleton->ID_Cycle_Safe("stand_bwd_rs");
    m_legs[eLegsStealFwd] = skeleton->ID_Cycle_Safe("new_walk_steal_0");
    m_legs[eLegsStealBkwd] = skeleton->ID_Cycle_Safe("new_walk_steal_beack_0");

    m_legs[eLegsStealFwdLeft] = skeleton->ID_Cycle_Safe("steal_fwd_ls");
    m_legs[eLegsStealFwdRight] = skeleton->ID_Cycle_Safe("steal_fwd_rs");
    m_legs[eLegsStealBkwdLeft] = skeleton->ID_Cycle_Safe("steal_bwd_ls");
    m_legs[eLegsStealBkwdRight] = skeleton->ID_Cycle_Safe("steal_bwd_rs");

    m_legs[eLegsStandDamaged] = skeleton->ID_Cycle_Safe("new_run_fwd_0");
    m_legs[eLegsRunDamaged] = skeleton->ID_Cycle_Safe("new_run_fwd_0");
    m_legs[eLegsWalkDamaged] = skeleton->ID_Cycle_Safe("new_run_fwd_0");
    m_legs[eLegsBackRunDamaged] = skeleton->ID_Cycle_Safe("new_run_fwd_0");
    m_legs[eLegsRunStrafeLeftDamaged] = skeleton->ID_Cycle_Safe("new_run_fwd_0");
    m_legs[eLegsRunStrafeRightDamaged] = skeleton->ID_Cycle_Safe("new_run_fwd_0");

    m_torso[eTorsoIdle] = skeleton->ID_Cycle_Safe("new_torso_idle_0");
    m_torso[eTorsoSteal] = skeleton->ID_Cycle_Safe("new_torso_steal_0");
    m_torso[eTorsoPsyAttack] = skeleton->ID_Cycle_Safe("new_torso_attack_0");
    m_torso[eTorsoRun] = skeleton->ID_Cycle_Safe("new_torso_run_0");

    add_path_rotation(eLegsTypeRun, 0, eLegsRun);
    add_path_rotation(eLegsTypeRun, PI, eLegsBackRun);
    add_path_rotation(eLegsTypeRun, PI_DIV_4, eLegsRunFwdLeft);
    add_path_rotation(eLegsTypeRun, -PI_DIV_4, eLegsRunFwdRight);
    add_path_rotation(eLegsTypeRun, (PI - PI_DIV_4), eLegsRunBkwdLeft);
    add_path_rotation(eLegsTypeRun, -(PI - PI_DIV_4), eLegsRunBkwdRight);

    add_path_rotation(eLegsTypeStealMotion, 0, eLegsStealFwd);
    add_path_rotation(eLegsTypeStealMotion, PI, eLegsStealBkwd);
    add_path_rotation(eLegsTypeStealMotion, PI_DIV_4, eLegsStealFwdLeft);
    add_path_rotation(eLegsTypeStealMotion, -PI_DIV_4, eLegsStealFwdRight);
    add_path_rotation(eLegsTypeStealMotion, (PI - PI_DIV_4), eLegsStealBkwdLeft);
    add_path_rotation(eLegsTypeStealMotion, -(PI - PI_DIV_4), eLegsStealBkwdRight);

    // 1. link animation with action
    // 2. link animation with velocities and path velocities
    // 3.
}

void CControllerAnimation::add_path_rotation(ELegsActionType action, float angle, ELegsActionType type)
{
    SPathRotations rot;
    rot.angle = angle;
    rot.legs_motion = type;

    auto map_it = m_path_rotations.find(action);
    if (map_it == m_path_rotations.end())
    {
        PATH_ROTATIONS_VEC vec;
        vec.push_back(rot);
        m_path_rotations.insert(std::make_pair(action, vec));
    }
    else
    {
        map_it->second.push_back(rot);
    }
}

void CControllerAnimation::select_torso_animation()
{
    if (m_wait_torso_anim_end)
        return;

    SControlAnimationData* ctrl_data = (SControlAnimationData*)m_man->data(this, ControlCom::eControlAnimation);
    if (!ctrl_data)
        return;

    MotionID target_motion;

    // check fire animation
    if (m_controller->can_psy_fire())
    {
        target_motion = m_torso[eTorsoPsyAttack];
        m_wait_torso_anim_end = true;
    }
    else
    {
        target_motion = m_torso[m_current_torso_action];
    }

    if ((ctrl_data->torso.get_motion() != target_motion) || m_wait_torso_anim_end)
    {
        ctrl_data->torso.set_motion(target_motion);
        ctrl_data->torso.actual = false;
    }
}

void CControllerAnimation::select_legs_animation()
{
    // select from action
    ELegsActionType legs_action = eLegsUndefined;

    if (is_moving())
    {
        // if we are moving, get yaw from path
        float cur_yaw, target_yaw;
        m_man->direction().get_heading(cur_yaw, target_yaw);

        SPathRotations path_rot = get_path_rotation(cur_yaw);
        legs_action = path_rot.legs_motion;
    }
    else
    {
        // else select standing animation
        for (auto it = m_legs.begin(); it != m_legs.end(); it++)
        {
            if ((it->first & m_current_legs_action) == m_current_legs_action)
            {
                legs_action = it->first;
                break;
            }
        }
        VERIFY(legs_action != eLegsUndefined);
    }

    // start new animation
    SControlAnimationData* ctrl_data = (SControlAnimationData*)m_man->data(this, ControlCom::eControlAnimation);
    if (!ctrl_data)
        return;

    if (ctrl_data->legs.get_motion() != m_legs[legs_action])
        ctrl_data->legs.actual = false;

    ctrl_data->legs.set_motion(m_legs[legs_action]);
}

CControllerAnimation::SPathRotations CControllerAnimation::get_path_rotation(float cur_yaw)
{
    float target_yaw = m_man->path_builder().detail().direction().getH();
    target_yaw = angle_normalize(-target_yaw);

    float diff = angle_difference(cur_yaw, target_yaw);
    if (from_right(target_yaw, cur_yaw))
        diff = -diff;

    diff = angle_normalize(diff);

    auto it_best = m_path_rotations[m_current_legs_action].begin();
    float best_diff = flt_max;
    for (auto it = m_path_rotations[m_current_legs_action].begin(); it != m_path_rotations[m_current_legs_action].end(); ++it)
    {
        float angle_diff = angle_normalize(it->angle);

        float cur_diff = angle_difference(angle_diff, diff);
        if (cur_diff < best_diff)
        {
            best_diff = cur_diff;
            it_best = it;
        }
    }

    return (*it_best);
}

void CControllerAnimation::set_body_state(ETorsoActionType torso, ELegsActionType legs)
{
    m_current_legs_action = CControllerAnimation::eLegsTypeStealMotion;
    m_current_torso_action = CControllerAnimation::eTorsoSteal;
    // m_current_legs_action		= legs;
    // m_current_torso_action		= torso;
}

bool CControllerAnimation::is_moving()
{
    if (!m_man->path_builder().is_moving_on_path())
        return false;

    if (((m_current_legs_action & eLegsTypeStealMotion) != eLegsTypeStealMotion) && ((m_current_legs_action & eLegsTypeWalk) != eLegsTypeWalk) &&
        ((m_current_legs_action & eLegsTypeRun) != eLegsTypeRun))
        return false;

    return true;
}

void CControllerAnimation::on_switch_controller()
{
    if (m_controller->m_mental_state == CController::eStateDanger)
    {
        m_wait_torso_anim_end = false;
        set_body_state(eTorsoIdle, eLegsTypeStand);

        select_torso_animation();
        select_legs_animation();
    }
    else
    {
        select_animation(true);
    }
}

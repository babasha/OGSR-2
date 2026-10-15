////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_manager_update.cpp
//	Created 	: 25.02.2003
//  Modified 	: 13.12.2006
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager update cycle
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "game_object_space.h"
#include "script_callback_ex.h"

#include "stalker_movement_manager.h"
#include "stalker_animation_trace.h"

/*IC*/ void CStalkerAnimationManager::play_delayed_callbacks()
{
    if (!m_call_script_callback)
        return;

    m_call_script_callback = false;
    object().callback(GameObject::eScriptAnimation)();
}

IC bool CStalkerAnimationManager::script_callback() const
{
    if (script_animations().empty())
        return (false);

    return (object().callback(GameObject::eScriptAnimation));
}

IC bool CStalkerAnimationManager::need_update() const
{
    if (script_callback())
        return (true);

    return (non_script_need_update());
}

IC void CStalkerAnimationManager::update_tracks()
{
    if (!need_update())
        return;

    m_skeleton_animated->UpdateTracks();
}

IC void CStalkerAnimationManager::play_script_impl()
{
    global().reset();
    torso().reset();
    legs().reset();

    const CStalkerAnimationScript& selected = assign_script_animation();
    script().animation(selected.animation());
    script().play(m_skeleton_animated, script_play_callback, &object(), selected.use_movement_controller(), false, m_script_bone_part_mask);

    head().animation(assign_head_animation());
    head().play(m_skeleton_animated, head_play_callback, &object(), false);
}

bool CStalkerAnimationManager::play_script()
{
    if (script_animations().empty())
    {
        script().reset();
        return (false);
    }

    play_script_impl();

    return (true);
}

IC void CStalkerAnimationManager::play_global_impl(const MotionID& animation)
{
    torso().reset();
    legs().reset();

    global().animation(animation);
    global().play(m_skeleton_animated, global_play_callback, &object(), false, false, m_script_bone_part_mask);

    head().animation(assign_head_animation());
    head().play(m_skeleton_animated, head_play_callback, &object(), false);
}

bool CStalkerAnimationManager::play_global()
{
    const MotionID& global_animation = assign_global_animation();
    if (!global_animation)
    {
        global().reset();
        return (false);
    }

    play_global_impl(global_animation);

    return (true);
}

IC void CStalkerAnimationManager::play_head()
{
    head().animation(assign_head_animation());
    head().play(m_skeleton_animated, head_play_callback, &object(), false);
}

IC void CStalkerAnimationManager::play_torso()
{
    torso().animation(assign_torso_animation());
    torso().play(m_skeleton_animated, torso_play_callback, &object(), false);
}

void CStalkerAnimationManager::play_legs()
{
    float speed = 0.f;
    bool first_time = !legs().animation();
    bool result = legs().animation(assign_legs_animation());

    if (!first_time && !result && legs().blend())
    {
        float amount = legs().blend()->blendAmount;
        m_previous_speed = (m_current_speed - m_previous_speed) * amount + m_previous_speed;
    }

    legs().play(m_skeleton_animated, legs_play_callback, &object(), !fis_zero(m_current_speed), false);

    if (result && legs().blend())
    {
        float amount = legs().blend()->blendAmount;
        speed = (m_current_speed - m_previous_speed) * amount + m_previous_speed;
    }

    if (fis_zero(speed))
        return;

    if (!legs().blend())
        return;

    object().movement().setup_speed_from_animation(speed);
}

void CStalkerAnimationManager::update_impl()
{
    if (!object().g_Alive())
        return;

    // ⚠No MOTIONS → m_data_storage was never built (reload() bails on the same
    // test), so play_head/torso/legs would index a 0-sized vector and AV.
    // Bones are not the test: this host reads real bones from the .ogf and
    // still has no animation data.
    if (!m_skeleton_animated || m_skeleton_animated->LL_MotionsSlotCount() == 0)
        return;

    play_delayed_callbacks();
    update_tracks();

    // Traced on these exits too: a script or global clip owns every part while
    // it plays, and a trace that only sees the head/torso/legs path reports
    // nothing at all for exactly those stretches.
    if (play_script())
    {
        StalkerAnimTrace::Frame(&object());
        return;
    }

    if (play_global())
    {
        StalkerAnimTrace::Frame(&object());
        return;
    }

    play_head();
    play_torso();
    play_legs();

    torso().synchronize(m_skeleton_animated, m_legs);

    // ⭐Traced HERE, at the one point that runs for whoever drives the
    // animator: in single player that is the AI, on a network client it is the
    // puppet layer. Same code, same line format, two logs that can be diffed.
    StalkerAnimTrace::Frame(&object());
}

void CStalkerAnimationManager::update()
{
    START_PROFILE("stalker/client_update/animations")
    update_impl();
    STOP_PROFILE
}

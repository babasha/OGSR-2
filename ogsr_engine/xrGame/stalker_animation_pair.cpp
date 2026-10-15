////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_pair.cpp
//	Created 	: 25.02.2003
//  Modified 	: 19.11.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation pair
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_animation_pair.h"
#include "../xr_3da/motion.h"
#include "ai_debug.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/ai_monsters_anims.h"

void CStalkerAnimationPair::synchronize(IKinematicsAnimated* skeleton_animated, const CStalkerAnimationPair& stalker_animation) const
{
    if (!blend())
        return;

    CMotionDef* motion0 = skeleton_animated->LL_GetMotionDef(animation());
    VERIFY(motion0);
    if (!(motion0->flags & esmSyncPart))
        return;

    if (!stalker_animation.blend())
        return;

    CMotionDef* motion1 = skeleton_animated->LL_GetMotionDef(stalker_animation.animation());
    VERIFY(motion1);
    if (!(motion1->flags & esmSyncPart))
        return;

    blend()->timeCurrent = stalker_animation.blend()->timeCurrent;
}

void CStalkerAnimationPair::play_global_animation(IKinematicsAnimated* skeleton_animated, PlayCallback callback, CAI_Stalker* object, const u32& bone_part,
                                                  const bool& use_animation_movement_control)
{
    m_blend = 0;
    for (u16 i = 0; i < MAX_PARTS; ++i)
    {
        if (!(bone_part & (1 << i)))
            continue;

        CBlend* blend = 0;
        if (!m_blend)
        {
            blend = skeleton_animated->LL_PlayCycle(i, animation(), TRUE, callback, object);
            if (use_animation_movement_control)
                object->create_anim_mov_ctrl(blend);
        }
        else
            skeleton_animated->LL_PlayCycle(i, animation(), TRUE, 0, 0);

        if (blend && !m_blend)
            m_blend = blend;
    }
}

void CStalkerAnimationPair::play(IKinematicsAnimated* skeleton_animated, PlayCallback callback, CAI_Stalker* object, const bool& use_animation_movement_control,
                                 bool continue_interrupted_animation, const u32& bone_part)
{
    VERIFY(animation());
    if (actual())
    {
        return;
    }

    if (animation() != m_array_animation)
    {
        m_array_animation.invalidate();
        m_array = 0;
    }

    if (!global_animation())
    {
        float pos = 0.f;
        if (m_step_dependence && continue_interrupted_animation)
        {
            VERIFY(!m_blend || !fis_zero(m_blend->timeTotal));
            if (m_step_dependence && m_blend)
                pos = fmod(m_blend->timeCurrent, m_blend->timeTotal) / m_blend->timeTotal;
        }

        m_blend = skeleton_animated->PlayCycle(animation(), TRUE, callback, object);

        if (m_step_dependence && continue_interrupted_animation)
        {
            //			if (we were standing and now we are moving)
            //				pos						= 0.5f*Random.randI(2);
            if (m_blend)
                m_blend->timeCurrent = m_blend->timeTotal * pos;
        }
    }
    else
        play_global_animation(skeleton_animated, callback, object, bone_part, use_animation_movement_control);
    m_actual = true;

    if (m_step_dependence)
        object->CStepManager::on_animation_start(animation(), blend());
}

void CStalkerAnimationPair::select_animation(const ANIM_VECTOR& array, const ANIMATION_WEIGHTS* weights)
{
    if (!weights)
    {
        m_array_animation = array[::Random.randI(array.size())];
        VERIFY(m_array_animation);
        return;
    }

    float accumulator = 0.f;
    ANIMATION_WEIGHTS::const_iterator I = weights->begin(), B = I;
    ANIMATION_WEIGHTS::const_iterator E = weights->end();

    u32 array_size = array.size();
    if (array_size < weights->size())
        E = B + array_size;

    for (; I != E; ++I)
        accumulator += *I;

    float chosen = ::Random.randF() * accumulator;
    accumulator = 0.f;
    for (I = B; I != E; ++I)
    {
        if ((accumulator + *I) >= chosen)
            break;

        accumulator += *I;
        continue;
    }

    VERIFY(I != E);
    VERIFY(u32(I - B) < array.size());
    m_array_animation = array[I - B];
    VERIFY(m_array_animation);
}

MotionID CStalkerAnimationPair::select(const ANIM_VECTOR& array, const ANIMATION_WEIGHTS* weights)
{
    VERIFY(!array.empty());

    if (m_array == &array)
    {
        VERIFY(animation());
        return (animation());
    }

    m_array = &array;
    select_animation(array, weights);
    return (m_array_animation);
}

void CStalkerAnimationPair::on_animation_end()
{
    make_inactual();

    for (size_t size{m_callbacks.size()}, processed{1}; !m_callbacks.empty(); processed++)
    {
        //Вставил ассерт на случай если вдруг когда то появится багованый каллбек который сам себя не удалит.
        R_ASSERT(size >= processed , "!!Something strange in CStalkerAnimationPair::on_animation_end()");

        m_callbacks.front()();
    }
}

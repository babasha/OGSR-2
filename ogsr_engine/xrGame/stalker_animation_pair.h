////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_pair.h
//	Created 	: 25.02.2003
//  Modified 	: 19.11.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation pair
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "../Include/xrRender/KinematicsAnimated.h"
#include "ai/ai_monsters_anims.h"

class CBlend;
class CAI_Stalker;

class CStalkerAnimationPair
{
public:
    enum
    {
        all_bone_parts = u16(0xf),
    };

public:
    typedef xr_vector<float> ANIMATION_WEIGHTS;
    typedef std::pair<LPCSTR, LPCSTR> BLEND_ID;

public:
    using CALLBACK_ID = fastdelegate::FastDelegate<void()>;
    typedef xr_vector<CALLBACK_ID> CALLBACKS;

private:
    MotionID m_animation;
    CBlend* m_blend;
    bool m_actual;
    bool m_step_dependence;
    bool m_global_animation;
    const ANIM_VECTOR* m_array;
    MotionID m_array_animation;
    CALLBACKS m_callbacks;

private:
    void select_animation(const ANIM_VECTOR& array, const ANIMATION_WEIGHTS* weights);
    void play_global_animation(IKinematicsAnimated* skeleton_animated, PlayCallback callback, CAI_Stalker* object, const u32& bone_part,
                               const bool& use_animation_movement_control);

public:
    IC CStalkerAnimationPair();
    IC void reset();
    void synchronize(IKinematicsAnimated* skeleton_animated, const CStalkerAnimationPair& stalker_animation_pair) const;
    MotionID select(const ANIM_VECTOR& array, const ANIMATION_WEIGHTS* weights = 0);
    IC bool actual() const;
    IC bool animation(const MotionID& animation);
    IC const MotionID& animation() const;
    IC CBlend* blend() const;
    IC void step_dependence(bool value);
    IC bool step_dependence() const;
    IC void global_animation(bool global_animation);
    IC bool global_animation() const;
    IC void make_inactual();

public:
    void play(IKinematicsAnimated* skeleton_animated, PlayCallback callback, CAI_Stalker* object, const bool& use_animation_movement_control,
              bool continue_interrupted_animation = true, const u32& bone_part = all_bone_parts);

public:
    IC const CALLBACK_ID* callback(const CALLBACK_ID& callback) const;
    IC void add_callback(const CALLBACK_ID& callback);
    IC void remove_callback(const CALLBACK_ID& callback);
    void on_animation_end();
    IC bool need_update() const;
};

#include "stalker_animation_pair_inline.h"
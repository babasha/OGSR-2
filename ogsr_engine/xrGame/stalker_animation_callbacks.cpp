////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_callbacks.cpp
//	Created 	: 25.02.2003
//  Modified 	: 19.11.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager : bone callbacks
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "sight_manager.h"
#include "stalker_movement_manager.h"
#include "game_object_space.h"
#include "EffectorShot.h"

#define TEMPLATE_SPECIALIZATION template <int yaw_factor_non_fire, int pitch_factor_non_fire, int yaw_factor_fire, int pitch_factor_fire, int roll_factor>

#define _detail detail<yaw_factor_non_fire, pitch_factor_non_fire, yaw_factor_fire, pitch_factor_fire, roll_factor>

TEMPLATE_SPECIALIZATION
struct detail
{
    static void callback(CBoneInstance* B);
};

typedef detail<25, 0, 50, 50, 50> spine;
typedef detail<25, 0, 50, 50, 50> shoulder;
typedef detail<50, 100, 0, 0, -40> head;

TEMPLATE_SPECIALIZATION
void _detail::callback(CBoneInstance* B)
{
    CAI_Stalker* A = static_cast<CAI_Stalker*>(B->callback_param());
    VERIFY(_valid(B->mTransform));
    Fvector c = B->mTransform.c;
    Fmatrix spin;
    float yaw_factor = 0, pitch_factor = 0;
    if (A->sight().use_torso_look())
    {
        yaw_factor = yaw_factor_fire / 100.f;
        pitch_factor = pitch_factor_fire / 100.f;
    }
    else
    {
        yaw_factor = yaw_factor_non_fire / 100.f;
        pitch_factor = pitch_factor_non_fire / 100.f;
    }

    float effector_yaw = 0.f, effector_pitch = 0.f;
    if (A->weapon_shot_effector().IsActive())
    {
        Fvector temp;
        A->weapon_shot_effector().GetDeltaAngle(temp);
        effector_yaw = temp.y;
        VERIFY(_valid(effector_yaw));
        effector_pitch = temp.x;
        VERIFY(_valid(effector_pitch));
    }

    VERIFY(_valid(A->movement().head_orientation().current.yaw));
    VERIFY(_valid(A->movement().body_orientation().current.yaw));
    VERIFY(_valid(A->NET_Last.o_torso.pitch));

    // ⭐The servo's correction rides on the SAME factors as the open-loop part.
    //
    // It is added to the angle being scaled, not applied afterwards, so the
    // three bones keep sharing the work in the proportions the content was
    // tuned for (spine 25%, shoulder 25%, head 50%) -- the loop only changes
    // how much total rotation they are dividing up. Applying the bias whole to
    // every bone would triple it and bend the spine into the shape those
    // percentages exist to prevent. See CAI_Stalker::update_aim_servo.
    float yaw = angle_normalize_signed(
        -yaw_factor *
        angle_normalize_signed(A->movement().head_orientation().current.yaw + effector_yaw + A->aim_bias_yaw() - (A->movement().body_orientation().current.yaw)));
    float pitch = angle_normalize_signed(-pitch_factor * angle_normalize_signed(A->NET_Last.o_torso.pitch + effector_pitch + A->aim_bias_pitch()));
    VERIFY(_valid(yaw));
    VERIFY(_valid(pitch));

    spin.setXYZ(pitch, yaw, 0);
    VERIFY(_valid(spin));
    B->mTransform.mulA_43(spin);

    // Leaning out from cover (CAI_Stalker::update_lean): a roll about the
    // body's forward axis, model Z. Spine and shoulder share it, the head takes
    // part of it back so it stays nearer level, as a person's does. rotateZ
    // tips the up axis towards -X, hence the minus for + = right.
    if (roll_factor && !fis_zero(A->lean()))
    {
        Fmatrix roll;
        roll.rotateZ(-A->lean() * roll_factor / 100.f);
        B->mTransform.mulA_43(roll);
    }

    B->mTransform.c = c;
}

#undef TEMPLATE_SPECIALIZATION
#undef _detail

// ⭐ПРИГОДНЫ К ЧУЖОМУ ПОТОКУ (белый список в bone.h). Прочитано 19-09: колбэк
// читает состояние СВОЕГО сталкера (sight, weapon_shot_effector, movement
// orientation, NET_Last.o_torso, aim_bias, lean -- одно float, пишется в UpdateCL) и поворачивает ТОЛЬКО свою кость,
// сохраняя её позицию. Ничего не пишет за пределы `B->mTransform`.
// 📏Перепись: spine+shoulder (один и тот же шаблон) 32.8 и head 16.4 в кадр.
// ⚠Три typedef'а -- ДВА разных инстанцирования шаблона: spine и shoulder
// совпадают параметрами <25,0,50,50>, поэтому в переписи их видно одной
// строкой, и в список их достаточно внести тоже двумя записями.
static const bool s_stalkerAimCbMtSafe = (BoneCallbackMarkMtSafe(&spine::callback),
                                          BoneCallbackMarkMtSafe(&head::callback), true);

void CStalkerAnimationManager::assign_bone_callbacks()
{
    IKinematics* kinematics = smart_cast<IKinematics*>(m_visual);
    VERIFY(kinematics);

    LPCSTR section = *object().cNameSect();

    int head_bone = kinematics->LL_BoneID(pSettings->r_string(section, "bone_head"));
    kinematics->LL_GetBoneInstance(u16(head_bone)).set_callback(bctCustom, &head::callback, &object());

    int shoulder_bone = kinematics->LL_BoneID(pSettings->r_string(section, "bone_shoulder"));
    kinematics->LL_GetBoneInstance(u16(shoulder_bone)).set_callback(bctCustom, &shoulder::callback, &object());

    int spin_bone = kinematics->LL_BoneID(pSettings->r_string(section, "bone_spin"));
    kinematics->LL_GetBoneInstance(u16(spin_bone)).set_callback(bctCustom, &spine::callback, &object());
}

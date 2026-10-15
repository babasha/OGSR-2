#include "StdAfx.h"
#include "ai_monster_utils.h"
#include "../../Entity.h"
#include "../../ai_object_location.h"
#include "../../ai_space.h"
#include "level_graph.h"
#include "../../../Include/xrRender/Kinematics.h"
#include "basemonster/base_monster.h"

Fvector get_bone_position(CObject* object, LPCSTR bone_name)
{
    u16 bone_id = smart_cast<IKinematics*>(object->Visual())->LL_BoneID(bone_name);
    CBoneInstance& bone = smart_cast<IKinematics*>(object->Visual())->LL_GetBoneInstance(bone_id);

    Fmatrix global_transform;
    global_transform.mul(object->XFORM(), bone.mTransform);

    return (global_transform.c);
}

Fvector get_head_position(CObject* object)
{
    pcstr bone_name = "bip01_head";
    if (CBaseMonster* monster = smart_cast<CBaseMonster*>(object))
    {
        bone_name = monster->get_head_bone_name();
    }

    return get_bone_position(object, bone_name);
}

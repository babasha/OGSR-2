#include "StdAfx.h"
#include "PhysicObject.h"
#include "PhysicsShell.h"
#include "Physics.h"
#include "xrServer_Objects_ALife.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../xr_3da/xr_collide_form.h"
#include "game_object_space.h"

#ifdef ANIMATED_PHYSICS_OBJECT_SUPPORT
#include "PhysicsShellAnimator.h"
#endif

CPhysicObject::CPhysicObject(void)
{
    m_type = epotBox;
    m_mass = 10.f;
    m_collision_hit_callback = NULL;
}

CPhysicObject::~CPhysicObject(void) {}

BOOL CPhysicObject::net_Spawn(CSE_Abstract* DC)
{
    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeObjectPhysic* po = smart_cast<CSE_ALifeObjectPhysic*>(e);
    R_ASSERT(po);
    m_type = EPOType(po->type);
    m_mass = po->mass;
    m_collision_hit_callback = NULL;
    inherited::net_Spawn(DC);
    xr_delete(collidable.model);
    switch (m_type)
    {
    case epotBox:
    case epotFixedChain:
    case epotFreeChain:
    case epotSkeleton: collidable.model = xr_new<CCF_Skeleton>(this); break;

    default: NODEFAULT;
    }

    CPHSkeleton::Spawn(e);
    setVisible(TRUE);
    setEnabled(TRUE);

    // Bone-less stub visual → shell built with zero elements. Every PPhysicsShell
    // deref below (isBreakable, applyImpulse, getMass, Animated, position update)
    // crashes on `*elements.begin()`. Bail post-Spawn and let the object exist
    // visually-only without physics.
    auto* k_check = smart_cast<IKinematics*>(Visual());
    if (!k_check || k_check->LL_BoneCount() == 0)
        return TRUE;

    if (!PPhysicsShell()->isBreakable() && !CScriptBinder::object() && !CPHSkeleton::IsRemoving())
        SheduleUnregister();

#ifdef ANIMATED_PHYSICS_OBJECT_SUPPORT
    if (PPhysicsShell()->Animated())
    {
        processing_activate();
    }
#endif

    PHObjectPositionUpdate();

    // приложить небольшую силу для того, чтобы объект начал падать
    PPhysicsShell()->applyImpulse(Fvector().set(0.f, -1.0f, 0.f), 0.5f * PPhysicsShell()->getMass());

    return TRUE;
}

void CPhysicObject::SpawnInitPhysics(CSE_Abstract* D)
{
    CreatePhysicsShell(D);
    RunStartupAnim(D);
}
void CPhysicObject::RunStartupAnim(CSE_Abstract* D)
{
    if (Visual() && smart_cast<IKinematics*>(Visual()))
    {
        //		CSE_PHSkeleton	*po	= smart_cast<CSE_PHSkeleton*>(D);
        IKinematicsAnimated* PKinematicsAnimated = NULL;
        R_ASSERT(Visual() && smart_cast<IKinematics*>(Visual()));
        PKinematicsAnimated = smart_cast<IKinematicsAnimated*>(Visual());
        if (PKinematicsAnimated)
        {
            CSE_Visual* visual = smart_cast<CSE_Visual*>(D);
            R_ASSERT(visual);

            // ⚠⚠A missing startup animation must not be fatal, and the reason is
            // about WHOSE process dies. The name is authored per instance in the
            // SDK, so anything spawned at runtime -- sv_prop_spawn wearing a model
            // this code has never seen, a foreign spawn file, a scripted
            // placement -- arrives with it empty or wrong. The old pair of asserts
            // (R_ASSERT2 on empty, then PlayCycle's own FATAL on a name it cannot
            // find) turned that into a crash on EVERY CLIENT IN THE WORLD,
            // because a spawn packet goes to all of them: one administrator's typo
            // would end everyone's session.
            //
            // ⭐ID_Cycle_Safe is the same answer ZoneVisual.cpp already needed for
            // the same reason. A prop that stands still is a picture; a prop that
            // takes the server down is an outage.
            const MotionID motion = PKinematicsAnimated->ID_Cycle_Safe(visual->startup_animation);
            if (motion.valid())
                PKinematicsAnimated->PlayCycle(motion);
            else if (visual->startup_animation.size() && 0 != xr_strcmp(visual->startup_animation.c_str(), "$editor"))
            {
                // ⚠"$editor" is not a name anybody chose: CSE_Visual's
                // constructor assigns it to every visual entity ever built, so
                // it is what "the SDK never set one" looks like, and every
                // runtime spawn carries it. Warning about it would put a line in
                // the log for every crate on the map and say nothing. A name
                // that somebody DID author and that does not resolve is a real
                // mismatch and still worth hearing about.
                Msg("! [prop] '%s': no cycle '%s' on this model -- it will stand still", *cName(), visual->startup_animation.c_str());
            }
        }
        smart_cast<IKinematics*>(Visual())->CalculateBones_Invalidate();
        smart_cast<IKinematics*>(Visual())->CalculateBones();
    }
}
void CPhysicObject::net_Destroy()
{
#ifdef ANIMATED_PHYSICS_OBJECT_SUPPORT
    if (PPhysicsShell() && PPhysicsShell()->Animated())  // [VK stub] shell may be null when stub-kinematics bailed during net_Spawn
    {
        processing_deactivate();
    }
#endif

    inherited::net_Destroy();
    CPHSkeleton::RespawnInit();
}

void CPhysicObject::net_Save(NET_Packet& P)
{
    inherited::net_Save(P);
    CPHSkeleton::SaveNetState(P);
}
void CPhysicObject::CreatePhysicsShell(CSE_Abstract* e)
{
    CSE_ALifeObjectPhysic* po = smart_cast<CSE_ALifeObjectPhysic*>(e);
    CreateBody(po);
}

void CPhysicObject::CreateSkeleton(CSE_ALifeObjectPhysic* po)
{
    if (m_pPhysicsShell)
        return;
    if (!Visual())
        return;
    // P_build_Shell resolves fixed_bones via LL_BoneID — bone-less stub returns
    // BI_NONE and the function R_ASSERTs ("wrong fixed bone"). Skip the entire
    // shell build until skinned-mesh support lands.
    auto* k = smart_cast<IKinematics*>(Visual());
    if (!k || k->LL_BoneCount() == 0) return;

    LPCSTR fixed_bones = *po->fixed_bones;
    m_pPhysicsShell = P_build_Shell(this, !po->_flags.test(CSE_PHSkeleton::flActive), fixed_bones);
    ApplySpawnIniToPhysicShell(&po->spawn_ini(), m_pPhysicsShell, fixed_bones[0] != '\0');
    ApplySpawnIniToPhysicShell(k->LL_UserData(), m_pPhysicsShell, fixed_bones[0] != '\0');
}

void CPhysicObject::Load(LPCSTR section)
{
    inherited::Load(section);
    CPHSkeleton::Load(section);
}

void CPhysicObject::shedule_Update(u32 dt)
{
    inherited::shedule_Update(dt);
    CPHSkeleton::Update(dt);
}
void CPhysicObject::UpdateCL()
{
    inherited::UpdateCL();

#ifdef ANIMATED_PHYSICS_OBJECT_SUPPORT
    //Если наш физический объект анимированный, то
    //двигаем объект за анимацией
    if (m_pPhysicsShell && m_pPhysicsShell->PPhysicsShellAnimator())  // [VK stub] shell may be null when stub-kinematics bailed during net_Spawn
    {
        m_pPhysicsShell->PPhysicsShellAnimator()->OnFrame();
    }
#endif

    PHObjectPositionUpdate();
}
void CPhysicObject::PHObjectPositionUpdate()
{
    if (m_pPhysicsShell)
    {
        if (m_type == epotBox)
        {
            m_pPhysicsShell->Update();
            XFORM().set(m_pPhysicsShell->mXFORM);
        }
        else
            m_pPhysicsShell->InterpolateGlobalTransform(&XFORM());
    }
}

void CPhysicObject::AddElement(CPhysicsElement* root_e, int id)
{
    IKinematics* K = smart_cast<IKinematics*>(Visual());

    CPhysicsElement* E = P_create_Element();
    CBoneInstance& B = K->LL_GetBoneInstance(u16(id));
    E->mXFORM.set(K->LL_GetTransform(u16(id)));
    Fobb bb = K->LL_GetBox(u16(id));

    if (bb.m_halfsize.magnitude() < 0.05f)
    {
        bb.m_halfsize.add(0.05f);
    }
    E->add_Box(bb);
    E->setMass(10.f);
    E->set_ParentElement(root_e);
    B.set_callback(bctPhysics, m_pPhysicsShell->GetBonesCallback(), E);
    m_pPhysicsShell->add_Element(E);
    if (!(m_type == epotFreeChain && root_e == 0))
    {
        CPhysicsJoint* J = P_create_Joint(CPhysicsJoint::full_control, root_e, E);
        J->SetAnchorVsSecondElement(0, 0, 0);
        J->SetAxisDirVsSecondElement(1, 0, 0, 0);
        J->SetAxisDirVsSecondElement(0, 1, 0, 2);
        J->SetLimits(-M_PI / 2, M_PI / 2, 0);
        J->SetLimits(-M_PI / 2, M_PI / 2, 1);
        J->SetLimits(-M_PI / 2, M_PI / 2, 2);
        m_pPhysicsShell->add_Joint(J);
    }

    CBoneData& BD = K->LL_GetData(u16(id));
    for (vecBonesIt it = BD.children.begin(); BD.children.end() != it; ++it)
    {
        AddElement(E, (*it)->GetSelfID());
    }
}

void CPhysicObject::CreateBody(CSE_ALifeObjectPhysic* po)
{
    if (m_pPhysicsShell)
        return;
    IKinematics* pKinematics = smart_cast<IKinematics*>(Visual());
    switch (m_type)
    {
    case epotBox: {
        m_pPhysicsShell = P_build_SimpleShell(this, m_mass, !po->_flags.test(CSE_ALifeObjectPhysic::flActive));
        {
            // для SimpleShell нет привязки к костям и вообще не работают параметры из секцции collide
            // но объекты с таким type есть в all.spawn например визуалы сталкеров на Арене. я так и не нашел как оно раньше могло работать
            // пока сделаем фикс прямо тут

            if (*po->fixed_bones)
            {
                m_pPhysicsShell->get_ElementByStoreOrder(0)->Fix();

                CInifile& ini = po->spawn_ini();

                if (ini.line_exist("collide", "ignore_static"))
                {
                    m_pPhysicsShell->SetIgnoreStatic();
                }

                if (ini.line_exist("collide", "ignore_dynamic"))
                {
                    m_pPhysicsShell->SetIgnoreDynamic();
                }                
            }
        }
    }
    break;
    case epotFixedChain:
    case epotFreeChain: {
        m_pPhysicsShell = P_create_Shell();
        m_pPhysicsShell->set_Kinematics(pKinematics);
        AddElement(0, pKinematics->LL_GetBoneRoot()); //-V595
        m_pPhysicsShell->setMass1(m_mass);
    }
    break;

    case epotSkeleton: {
        // pKinematics->LL_SetBoneRoot(0);
        CreateSkeleton(po);
    }
    break;

    default: {
    }
    break;
    }

    // CreateSkeleton may have bailed early (bone-less stub) leaving the shell
    // un-built. Skip XFORM/AirResistance setup — net_Spawn's bone-count guard
    // handles the rest of the lifecycle.
    if (!m_pPhysicsShell) return;

    m_pPhysicsShell->mXFORM.set(XFORM());
    m_pPhysicsShell->SetAirResistance(0.001f, 0.02f);
    if (pKinematics)
    {
        SAllDDOParams disable_params;
        disable_params.Load(pKinematics->LL_UserData());
        m_pPhysicsShell->set_DisableParams(disable_params);
    }
    // m_pPhysicsShell->SetAirResistance(0.002f, 0.3f);
}

BOOL CPhysicObject::net_SaveRelevant()
{
    return TRUE; //! m_flags.test(CSE_ALifeObjectPhysic::flSpawnCopy);
}

BOOL CPhysicObject::UsedAI_Locations() { return (FALSE); }

void CPhysicObject::InitServerObject(CSE_Abstract* D)
{
    CPHSkeleton::InitServerObject(D);
    CSE_ALifeObjectPhysic* l_tpALifePhysicObject = smart_cast<CSE_ALifeObjectPhysic*>(D);
    if (!l_tpALifePhysicObject)
        return;
    l_tpALifePhysicObject->type = u32(m_type);
}
ICollisionHitCallback* CPhysicObject::get_collision_hit_callback() { return m_collision_hit_callback; }
void CPhysicObject::set_collision_hit_callback(ICollisionHitCallback* cc)
{
    xr_delete(m_collision_hit_callback);
    m_collision_hit_callback = cc;
}

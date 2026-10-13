#include "StdAfx.h"
#include "CustomZone.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/RenderVisual.h"
#include "ZoneVisual.h"
#include "xrServer_Objects_ALife_Monsters.h"

CVisualZone::CVisualZone() {}
CVisualZone::~CVisualZone() {}

BOOL CVisualZone::net_Spawn(CSE_Abstract* DC)
{
    BOOL ret = inherited::net_Spawn(DC);
    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeZoneVisual* Z = smart_cast<CSE_ALifeZoneVisual*>(e);
    IKinematicsAnimated* SA = smart_cast<IKinematicsAnimated*>(Visual());

    // ⚠⚠A ZONE THAT ARRIVES WITHOUT ANIMATION NAMES USED TO KILL THE CLIENT.
    //
    // Which cycles a visual anomaly plays is PER-INSTANCE data the SDK authored
    // into the level's spawn, not something its section carries -- so a zone
    // that came from anywhere else (a server placing one at runtime, a script)
    // arrives with both names empty. ID_Cycle asserts on a name it cannot find,
    // and an empty name is never found, so the whole game went down inside
    // net_Spawn with "can't find cycle: []". Measured 01-08-2026, live, from
    // the first anomaly the server ever placed.
    //
    // ⭐The safe lookup and a guard instead: a zone with no idle animation is a
    // still model that still burns, hums and can be walked into. That is a
    // cosmetic loss. Taking the player's process down is not, and net_Spawn
    // runs on data that came off the wire -- nothing arriving there should be
    // able to be fatal.
    m_attack_animation = SA->ID_Cycle_Safe(Z->attack_animation);
    m_idle_animation = SA->ID_Cycle_Safe(Z->startup_animation);

    if (m_idle_animation.valid())
        SA->PlayCycle(m_idle_animation);
    else
        Msg("! [%s]: %s has no idle cycle [%s] -- it will stand still", __FUNCTION__, cName().c_str(), Z->startup_animation.c_str());

    setVisible(TRUE);
    return ret;
}
void CVisualZone::net_Destroy() { inherited::net_Destroy(); }
void CVisualZone::AffectObjects()
{
    inherited::AffectObjects();
    //	smart_cast<IKinematicsAnimated*>(Visual())->PlayCycle(*m_attack_animation);
}
void CVisualZone::SwitchZoneState(EZoneState new_state)
{
    if (m_idle_animation.valid() && m_eZoneState == eZoneStateBlowout && new_state != eZoneStateBlowout)
    {
        //	IKinematicsAnimated*	SA=smart_cast<IKinematicsAnimated*>(Visual());
        smart_cast<IKinematicsAnimated*>(Visual())->PlayCycle(m_idle_animation);
    }

    inherited::SwitchZoneState(new_state);
}
void CVisualZone::Load(LPCSTR section)
{
    inherited::Load(section);
    m_dwAttackAnimaionStart = pSettings->r_u32(section, "attack_animation_start");
    m_dwAttackAnimaionEnd = pSettings->r_u32(section, "attack_animation_end");
    VERIFY2(m_dwAttackAnimaionStart < m_dwAttackAnimaionEnd, "attack_animation_start must be less then attack_animation_end");
}

void CVisualZone::UpdateBlowout()
{
    inherited::UpdateBlowout();

    // Guarded for the same reason as net_Spawn above: these ids may legitimately
    // be invalid now, and a blowout is the moment a zone is most likely to have
    // somebody standing in it.
    if (m_attack_animation.valid() && m_dwAttackAnimaionStart >= (u32)m_iPreviousStateTime && m_dwAttackAnimaionStart < (u32)m_iStateTime)
        smart_cast<IKinematicsAnimated*>(Visual())->PlayCycle(m_attack_animation);

    if (m_idle_animation.valid() && m_dwAttackAnimaionEnd >= (u32)m_iPreviousStateTime && m_dwAttackAnimaionEnd < (u32)m_iStateTime)
        smart_cast<IKinematicsAnimated*>(Visual())->PlayCycle(m_idle_animation);
}
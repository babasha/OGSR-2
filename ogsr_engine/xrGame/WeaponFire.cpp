// WeaponFire.cpp: implementation of the CWeapon class.
// function responsible for firing with CWeapon
//////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "Weapon.h"
#include "ParticlesObject.h"
#include "HUDManager.h"
#include "Entity.h"
#include "Actor.h"

#include "ActorEffector.h"
#include "EffectorShot.h"

#include "Level_Bullet_Manager.h"
#include "../xr_3da/IGame_Persistent.h"
#include "Level.h"
#include "sv_npc.h"
#include "CustomMonster.h" // net_note_fired -- the shot as a held wire flag

float CWeapon::GetWeaponDeterioration() { return conditionDecreasePerShot; };

void CWeapon::FireTrace(const Fvector& P, const Fvector& D)
{
    VERIFY(m_magazine.size());

    CCartridge& l_cartridge = m_magazine.back();
    //	Msg("ammo - %s", l_cartridge.m_ammoSect.c_str());
    VERIFY(u16(-1) != l_cartridge.bullet_material_idx);
    //-------------------------------------------------------------
#pragma todo("KRodin: мне кажется, или здесь должно быть && вместо & ? Надо б посмотреть, работает ли оно вообще.")
    l_cartridge.m_flags.set(CCartridge::cfTracer, (m_bHasTracers & !!l_cartridge.m_flags.test(CCartridge::cfTracer)));
    if (m_u8TracerColorID != u8(-1))
        l_cartridge.m_u8ColorID = m_u8TracerColorID;
    //-------------------------------------------------------------
    //повысить изношенность оружия с учетом влияния конкретного патрона
    //	float Deterioration = GetWeaponDeterioration();
    //	Msg("Deterioration = %f", Deterioration);
    if (Core.Features.test(xrCore::Feature::npc_simplified_shooting))
    {
        CActor* actor = smart_cast<CActor*>(H_Parent());
        if (actor)
            ChangeCondition(-GetWeaponDeterioration() * l_cartridge.m_impair);
    }
    else
        ChangeCondition(-GetWeaponDeterioration() * l_cartridge.m_impair);

    float fire_disp = GetFireDispersion(true);

    bool SendHit = SendHitAllowed(H_Parent());
    //выстерлить пулю (с учетом возможной стрельбы дробью)
    for (int i = 0; i < l_cartridge.m_buckShot; ++i)
    {
        FireBullet(P, D, fire_disp, l_cartridge, H_Parent()->ID(), ID(), SendHit);
    }

    if (m_bLightShotEnabled)
        Light_Start();

    // ⭐Everyone else has to be told. This is the only place a firearm passes
    // through once per shot with the fire point and direction the AI actually
    // used -- state_Fire recomputes them through g_fireParams right before
    // calling here, and every weapon class routes its shot through this method.
    //
    // ⚠Deliberately NOT get_LastFP(): on the headless server the weapon's
    // transform comes off a null kinematics stub, so the muzzle it reports is
    // not a place in the world. P and D are computed from the creature's
    // position and head orientation and are honest on both builds.
    if (Level().Server && H_Parent() && !smart_cast<CActor*>(H_Parent()))
    {
        SVNpc::PublishShot(u16(H_Parent()->ID()), u16(ID()), P, D);
        // ...and again in the update stream, as a held flag. The event carries
        // the flash; the flag carries the POSE, and a pose that is only true
        // for the instant of the shot is a pose the client sees on one frame in
        // four. See SNetCreatureIntent.
        if (CCustomMonster* shooter = smart_cast<CCustomMonster*>(H_Parent()))
            shooter->net_note_fired();
    }

    const Fvector ShotPos = Fvector().mad(P, D, 1.5f);
    g_pGamePersistent->GrassBendersAddShot(cast_game_object()->ID(), ShotPos, D, 3.0f, 20.0f, ps_ssfx_int_grass_params_2.z, ps_ssfx_int_grass_params_2.w);

    // Ammo
    m_magazine.pop_back();
    --iAmmoElapsed;

    //проверить не произошла ли осечка
    // CheckForMisfire();

    VERIFY((u32)iAmmoElapsed == m_magazine.size());
}

void CWeapon::Fire2Start() { bWorking2 = true; }
void CWeapon::Fire2End()
{
    //принудительно останавливать зацикленные партиклы
    if (m_pFlameParticles2 && m_pFlameParticles2->IsLooped())
        StopFlameParticles2();

    bWorking2 = false;
}

void CWeapon::StopShooting()
{
    // m_bPending = true;

    //принудительно останавливать зацикленные партиклы
    if (m_pFlameParticles && m_pFlameParticles->IsLooped())
        StopFlameParticles();

    if (!dont_interrupt_shot_anm)
        SwitchState(eIdle);

    bWorking = false;
    // if(IsWorking()) FireEnd();
}

void CWeapon::FireEnd()
{
    CShootingObject::FireEnd();
    ClearShotEffector();
}

void CWeapon::StartFlameParticles2() { CShootingObject::StartParticles(m_pFlameParticles2, *m_sFlameParticles2, get_LastFP2()); }
void CWeapon::StopFlameParticles2() { CShootingObject::StopParticles(m_pFlameParticles2); }
void CWeapon::UpdateFlameParticles2()
{
    if (m_pFlameParticles2)
        CShootingObject::UpdateParticles(m_pFlameParticles2, get_LastFP2());
}

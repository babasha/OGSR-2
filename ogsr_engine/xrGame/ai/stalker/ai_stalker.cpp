////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker.cpp
//	Created 	: 25.02.2003
//  Modified 	: 25.02.2003
//	Author		: Dmitriy Iassenev
//	Description : AI Behaviour for monster "Stalker"
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "../../../xrCore/load_prof.h"   // per-spawn breakdown
#include <atomic>
#include "../../npc_perf.h"
#include "xr_platform_seh.h"
#include "ai_stalker.h"
#include "../ai_monsters_misc.h"
#include "../../sv_motion_audit.h"
#include "../../sv_npc.h" // SVNpc::Steer -- a server order taken from the creature's own update
#include "../../Weapon.h"
#include "../../Hit.h"
#include "../../PHDestroyable.h"
#include "../../CharacterPhysicsSupport.h"
#include "../../script_entity_action.h"
#include "game_level_cross_table.h"
#include "game_graph.h"
#include "../../Inventory.h"
#include "../../Artifact.h"
#include "../../PHMovementControl.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../../cover_evaluators.h"
#include "../../xrServer.h"
#include "../../xr_level_controller.h"
#include "../../HUDManager.h"
#include "clsid_game.h"
#include "../../../Include/xrRender/Kinematics.h"
#include "character_info.h"
#include "../../Actor.h"
#include "../../relation_registry.h"
#include "../../stalker_animation_manager.h"
#include "../../stalker_planner.h"
#include "../../script_game_object.h"
#include "../../detail_path_manager.h"
#include "../../agent_manager.h"
#include "../../agent_corpse_manager.h"
#include "../../object_handler_planner.h"
#include "../../object_handler_space.h"
#include "../../memory_manager.h"
#include "../../sight_manager.h"
#include "../../ai_object_location.h"
#include "../../stalker_movement_manager.h"
#include "../../EntityCondition.h"
#include "script_engine.h"
#include "ai_stalker_impl.h"
#include "../../sound_player.h"
#include "../../stalker_sound_data.h"
#include "../../stalker_sound_data_visitor.h"
#include "ai_stalker_space.h"
#include "../../mt_config.h"
#include "../../EffectorShot.h"
#include "../../visual_memory_manager.h"
#include "../../enemy_manager.h"
#include "alife_human_brain.h"
#include "../../BoneProtections.h"
#include "../../stalker_animation_names.h"
#include "../../stalker_decision_space.h"
#include "../../agent_member_manager.h"
#include "../../location_manager.h"
#include "../../seniority_hierarchy_holder.h"
#include "../../team_hierarchy_holder.h"
#include "../../squad_hierarchy_holder.h"
#include "../../group_hierarchy_holder.h"

#ifdef DEBUG
#include "../../alife_simulator.h"
#include "../../alife_object_registry.h"
#include "Level.h"
#include "../../map_location.h"
#include "../../map_manager.h"
#endif // DEBUG

using namespace StalkerSpace;

extern int g_AI_inactive_time;

CAI_Stalker::CAI_Stalker()
{
    m_sound_user_data_visitor = 0;
    m_movement_manager = 0;
    m_group_behaviour = true;
    m_boneHitProtection = NULL;
    m_power_fx_factor = flt_max;
    m_wounded = false;
#ifdef DEBUG
    m_debug_planner = 0;
#endif // DEBUG
    m_registered_in_combat_on_migration = false;
}

CAI_Stalker::~CAI_Stalker()
{
    xr_delete(m_pPhysics_support);
    xr_delete(m_animation_manager);
    xr_delete(m_brain);
    xr_delete(m_sight_manager);
    xr_delete(m_weapon_shot_effector);
    xr_delete(m_sound_user_data_visitor);
}

void CAI_Stalker::reinit()
{
    CObjectHandler::reinit(this);
    sight().reinit();
    CCustomMonster::reinit();
    animation().reinit();
    movement().reinit();

    //загрузка спецевической звуковой схемы для сталкера согласно m_SpecificCharacter
    sound().sound_prefix(SpecificCharacter().sound_voice_prefix());

    LoadSounds(*cNameSect());

    m_pPhysics_support->in_Init();

    m_best_item_to_kill = 0;
    m_best_item_value = 0.f;
    m_best_ammo = 0;
    m_best_found_item_to_kill = 0;
    m_best_found_ammo = 0;
    m_item_actuality = false;
    m_sell_info_actuality = false;

    m_ce_close = xr_new<CCoverEvaluatorCloseToEnemy>(&movement().restrictions());
    m_ce_far = xr_new<CCoverEvaluatorFarFromEnemy>(&movement().restrictions());
    m_ce_best = xr_new<CCoverEvaluatorBest>(&movement().restrictions());
    m_ce_angle = xr_new<CCoverEvaluatorAngle>(&movement().restrictions());
    m_ce_safe = xr_new<CCoverEvaluatorSafe>(&movement().restrictions());
    m_ce_random_game = xr_new<CCoverEvaluatorRandomGame>(&movement().restrictions());
    m_ce_ambush = xr_new<CCoverEvaluatorAmbush>(&movement().restrictions());
    m_ce_best_by_time = xr_new<CCoverEvaluatorBestByTime>(&movement().restrictions());

    m_ce_close->set_inertia(3000);
    m_ce_far->set_inertia(3000);
    m_ce_best->set_inertia(1000);
    m_ce_angle->set_inertia(5000);
    m_ce_safe->set_inertia(1000);
    m_ce_random_game->set_inertia(3000);
    m_ce_ambush->set_inertia(3000);
    m_ce_best_by_time->set_inertia(1000);

    m_can_kill_enemy = false;
    m_can_kill_member = false;
    m_pick_distance = 0.f;
    m_pick_frame_id = 0;

    m_best_cover = 0;
    m_best_cover_actual = false;
    m_best_cover_value = flt_max;

    m_throw_actual = false;
    m_computed_object_position = Fvector().set(flt_max, flt_max, flt_max);
    m_computed_object_direction = Fvector().set(flt_max, flt_max, flt_max);

    m_throw_target_position = Fvector().set(flt_max, flt_max, flt_max);
    m_throw_ignore_object = nullptr;

    m_throw_position = Fvector().set(flt_max, flt_max, flt_max);
    m_throw_velocity = Fvector().set(flt_max, flt_max, flt_max);

    m_throw_collide_position = Fvector().set(flt_max, flt_max, flt_max);
    m_throw_enabled = false;

    m_last_throw_time = 0;

    m_can_throw_grenades = true;
    m_throw_time_interval = 20000;

    brain().CStalkerPlanner::m_storage.set_property(StalkerDecisionSpace::eWorldPropertyCriticallyWounded, false);

    {
        m_critical_wound_weights.clear();
        //		LPCSTR							weights = pSettings->r_string(cNameSect(),"critical_wound_weights");
        LPCSTR weights = SpecificCharacter().critical_wound_weights();
        string16 temp;
        for (int i = 0, n = _GetItemCount(weights); i < n; ++i)
            m_critical_wound_weights.push_back((float)atof(_GetItem(weights, i, temp)));
    }

    m_sight_enabled_before_animation_controller = true;
    m_update_rotation_on_frame = false;
}

void CAI_Stalker::LoadSounds(LPCSTR section)
{
    if (!g_Alive())
        return;

    LPCSTR	head_bone_name = pSettings->r_string(section, "bone_head");
    sound().add_deferred(pSettings->r_string(section, "sound_death"), 100, SOUND_TYPE_MONSTER_DYING, 0, u32(eStalkerSoundMaskDie), eStalkerSoundDie, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_anomaly_death"), 100, SOUND_TYPE_MONSTER_DYING, 0, u32(eStalkerSoundMaskDieInAnomaly), eStalkerSoundDieInAnomaly, head_bone_name, 0);
    sound().add_deferred(pSettings->r_string(section, "sound_hit"), 100, SOUND_TYPE_MONSTER_INJURING, 1, u32(eStalkerSoundMaskInjuring), eStalkerSoundInjuring, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_friendly_fire"), 100, SOUND_TYPE_MONSTER_INJURING, 1, u32(eStalkerSoundMaskInjuringByFriend), eStalkerSoundInjuringByFriend, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_panic_human"), 100, SOUND_TYPE_MONSTER_TALKING, 2, u32(eStalkerSoundMaskPanicHuman), eStalkerSoundPanicHuman, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_panic_monster"), 100, SOUND_TYPE_MONSTER_TALKING, 2, u32(eStalkerSoundMaskPanicMonster), eStalkerSoundPanicMonster, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_grenade_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 3, u32(eStalkerSoundMaskGrenadeAlarm), eStalkerSoundGrenadeAlarm, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_friendly_grenade_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 3, u32(eStalkerSoundMaskFriendlyGrenadeAlarm), eStalkerSoundFriendlyGrenadeAlarm, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_tolls"), 100, SOUND_TYPE_MONSTER_TALKING, 4, u32(eStalkerSoundMaskTolls), eStalkerSoundTolls, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskAlarm), eStalkerSoundAlarm, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_attack_no_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskAttackNoAllies), eStalkerSoundAttackNoAllies, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_attack_allies_single_enemy"), 100, SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskAttackAlliesSingleEnemy), eStalkerSoundAttackAlliesSingleEnemy, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_attack_allies_several_enemies"), 100, SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskAttackAlliesSeveralEnemies), eStalkerSoundAttackAlliesSeveralEnemies, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_backup"), 100, SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskBackup), eStalkerSoundBackup, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_detour"), 100, SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskDetour), eStalkerSoundDetour, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_search1_no_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskSearch1NoAllies), eStalkerSoundSearch1NoAllies, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_search1_with_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskSearch1WithAllies), eStalkerSoundSearch1WithAllies, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_humming"), 100, SOUND_TYPE_MONSTER_TALKING, 6, u32(eStalkerSoundMaskHumming), eStalkerSoundHumming, head_bone_name, 0);
    sound().add_deferred(pSettings->r_string(section, "sound_need_backup"), 100, SOUND_TYPE_MONSTER_TALKING, 4, u32(eStalkerSoundMaskNeedBackup), eStalkerSoundNeedBackup, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_running_in_danger"), 100, SOUND_TYPE_MONSTER_TALKING, 6, u32(eStalkerSoundMaskMovingInDanger), eStalkerSoundRunningInDanger, head_bone_name, xr_new<CStalkerSoundData>(this));
    //sound().add_deferred( pSettings->r_string( section, "sound_walking_in_danger" ), 100, SOUND_TYPE_MONSTER_TALKING, 6, u32(eStalkerSoundMaskMovingInDanger), eStalkerSoundWalkingInDanger, head_bone_name, xr_new<CStalkerSoundData>( this ) );
    sound().add_deferred(pSettings->r_string(section, "sound_kill_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskKillWounded), eStalkerSoundKillWounded, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_enemy_critically_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 4, u32(eStalkerSoundMaskEnemyCriticallyWounded), eStalkerSoundEnemyCriticallyWounded, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(pSettings->r_string(section, "sound_enemy_killed_or_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 4, u32(eStalkerSoundMaskEnemyKilledOrWounded), eStalkerSoundEnemyKilledOrWounded, head_bone_name, xr_new<CStalkerSoundData>(this));
    // the voice packs ship "one of ours is hit" lines that nothing played; the key is ours, so default to them
    sound().add_deferred(READ_IF_EXISTS(pSettings, r_string, section, "sound_friend_hit", "fight\\friend_hitted\\hit_friend_"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
                         u32(eStalkerSoundMaskFriendHit), eStalkerSoundFriendHit, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add_deferred(READ_IF_EXISTS(pSettings, r_string, section, "sound_enemy_lost", "fight\\searching_enemy\\enemy_lost_"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
                         u32(eStalkerSoundMaskEnemyLost), eStalkerSoundEnemyLost, head_bone_name, xr_new<CStalkerSoundData>(this));
    // shouted at a hidden enemy by the members covering his last position
    sound().add_deferred(READ_IF_EXISTS(pSettings, r_string, section, "sound_come_out", "fight\\threat\\threat_distant_,fight\\threat\\threat_close_"), 100,
                         SOUND_TYPE_MONSTER_TALKING, 5, u32(eStalkerSoundMaskComeOut), eStalkerSoundComeOut, head_bone_name, xr_new<CStalkerSoundData>(this));
    // said by the one who sets off to where the enemy was last seen; every pack has these
    sound().add_deferred(READ_IF_EXISTS(pSettings, r_string, section, "sound_search_go", "fight\\searching_enemy\\search_"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
                         u32(eStalkerSoundMaskSearchGo), eStalkerSoundSearchGo, head_bone_name, xr_new<CStalkerSoundData>(this));

    // Bandit and mercenary packs ship no fire/threat/enemy_lost lines at all, so these
    // slots were silent for them; borrow the nearest category the pack does have.
    sound().fallback(eStalkerSoundAttackNoAllies, eStalkerSoundAttackAlliesSingleEnemy);
    sound().fallback(eStalkerSoundSearch1NoAllies, eStalkerSoundSearch1WithAllies);
    sound().fallback(eStalkerSoundEnemyLost, eStalkerSoundSearch1WithAllies);
    sound().fallback(eStalkerSoundComeOut, eStalkerSoundSearch1WithAllies);
}

void CAI_Stalker::reload(LPCSTR section)
{
	if ( Ready() )
	  brain().setup( this );

    CCustomMonster::reload(section);
    if (!already_dead())
        CStepManager::reload(section);

    //	if (!already_dead())
    CObjectHandler::reload(section);

    //	inventory().m_slots[OUTFIT_SLOT].m_bUsable = false;

    if (!already_dead())
        sight().reload(section);

    if (!already_dead())
        movement().reload(section);

    m_fDispBase = READ_IF_EXISTS(pSettings, r_float, section, "disp_base", 0.f);
    m_fDispBase = deg2rad(m_fDispBase);

    m_disp_walk_stand = pSettings->r_float(section, "disp_walk_stand");
    m_disp_walk_crouch = pSettings->r_float(section, "disp_walk_crouch");
    m_disp_run_stand = pSettings->r_float(section, "disp_run_stand");
    m_disp_run_crouch = pSettings->r_float(section, "disp_run_crouch");
    m_disp_stand_stand = pSettings->r_float(section, "disp_stand_stand");
    m_disp_stand_crouch = pSettings->r_float(section, "disp_stand_crouch");
    m_disp_stand_stand_zoom = pSettings->r_float(section, "disp_stand_stand_zoom");
    m_disp_stand_crouch_zoom = pSettings->r_float(section, "disp_stand_crouch_zoom");

    m_min_queue_size_far = pSettings->r_u32(*cNameSect(), "weapon_min_queue_size_far"); // 1;
    m_max_queue_size_far = pSettings->r_u32(*cNameSect(), "weapon_max_queue_size_far"); // 6;
    m_min_queue_interval_far = pSettings->r_u32(*cNameSect(), "weapon_min_queue_interval_far"); // 500;
    m_max_queue_interval_far = pSettings->r_u32(*cNameSect(), "weapon_max_queue_interval_far"); // 1000;

    m_min_queue_size_medium = pSettings->r_u32(*cNameSect(), "weapon_min_queue_size_medium"); // 4;
    m_max_queue_size_medium = pSettings->r_u32(*cNameSect(), "weapon_max_queue_size_medium"); // 6;
    m_min_queue_interval_medium = pSettings->r_u32(*cNameSect(), "weapon_min_queue_interval_medium"); // 500;
    m_max_queue_interval_medium = pSettings->r_u32(*cNameSect(), "weapon_max_queue_interval_medium"); // 750;

    m_min_queue_size_close = pSettings->r_u32(*cNameSect(), "weapon_min_queue_size_close"); // 4;
    m_max_queue_size_close = pSettings->r_u32(*cNameSect(), "weapon_max_queue_size_close"); // 10;
    m_min_queue_interval_close = pSettings->r_u32(*cNameSect(), "weapon_min_queue_interval_close"); // 300;
    m_max_queue_interval_close = pSettings->r_u32(*cNameSect(), "weapon_max_queue_interval_close"); // 500;

    m_power_fx_factor = pSettings->r_float(section, "power_fx_factor");
    m_fast_can_kill_entity = READ_IF_EXISTS(pSettings, r_bool, section, "fast_can_kill_entity", false);
}

void CAI_Stalker::Die(CObject* who)
{
    LoadProf::Scope _lp("stalker.die"); // [frame prof]: 07-10 a kill = a 40-44 ms frame_move on the Switch
    notify_on_wounded_or_killed(who);

    SelectAnimation(XFORM().k, movement().detail().direction(), movement().speed());

    sound().clear_playing_sounds();

    //sound().set_sound_mask((u32)eStalkerSoundMaskDie);

    if (is_special_killer(who))
        sound().play(eStalkerSoundDieInAnomaly);
    else if (!m_headshot)
        sound().play(eStalkerSoundDie);

    m_hammer_is_clutched = m_clutched_hammer_enabled && !CObjectHandler::planner().m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped) && !::Random.randI(0, 2);

    inherited::Die(who);

    //запретить использование слотов в инвенторе
    inventory().SetSlotsUseful(false);

#pragma todo("KD: Поскольку весь лут непися пока обрабатывается в скриптах, в этом месте отключено удаление лута")
#if 0
	if (inventory().GetActiveSlot() >= inventory().m_slots.size())
		return;

	CInventoryItem					*active_item = inventory().m_slots[inventory().GetActiveSlot()].m_pIItem;
	if (!active_item)
		return;

	CWeapon							*weapon = smart_cast<CWeapon*>(active_item);
	if (!weapon)
		return;
	{
		TIItemContainer::iterator	I = inventory().m_all.begin();
		TIItemContainer::iterator	E = inventory().m_all.end();
		for ( ; I != E; ++I) {
			if (std::find(weapon->m_ammoTypes.begin(),weapon->m_ammoTypes.end(),(*I)->object().cNameSect()) == weapon->m_ammoTypes.end())
				continue;

			NET_Packet				packet;
			u_EventGen				(packet,GE_DESTROY,(*I)->object().ID());
			u_EventSend				(packet);
		}
	}
#endif
}

// ⭐⭐Where a creature's armour comes from, and it is the MODEL: both the
// per-bone protection table and the immunity overrides live in the .ogf's
// userdata chunk, not in the creature's section. Establishing that was the
// whole point of the 05-08-2026 armour work -- see sv_visual_armour.h, which
// reads the same chunk on a headless host that has no kinematics at all.
//
// Pulled out of net_Spawn so that re-dressing a live stalker redoes it: a
// creature drawn in an exoskeleton while still protected like the bandit model
// it spawned as is the exact disagreement this whole feature exists to avoid.
void CAI_Stalker::reload_bone_protection()
{
    IKinematics* pKinematics = smart_cast<IKinematics*>(Visual());
    if (!pKinematics)
        return;

    CInifile* ini = pKinematics->LL_UserData();
    if (!ini)
        return;

    if (ini->section_exist("immunities"))
    {
        LPCSTR imm_sect = ini->r_string("immunities", "immunities_sect");
        conditions().LoadImmunities(imm_sect, pSettings);
    }

    if (ini->line_exist("bone_protection", "bones_protection_sect"))
    {
        // ⚠Rebuilt, not patched: SBoneProtections resolves bone NAMES against
        // the kinematics it was given, so a table built for the previous
        // skeleton indexes bones that may not be in the same place on this one.
        xr_delete(m_boneHitProtection);
        m_boneHitProtection = xr_new<SBoneProtections>();
        m_boneHitProtection->reload(ini->r_string("bone_protection", "bones_protection_sect"), pKinematics);
    }
}

// ⭐⭐⭐MEASURED THE HARD WAY, 05-08-2026: reviving a stalker without this
// faulted CLevel::OnFrame 13000 times in two minutes on the host, once per
// frame, swallowed by the frame registrar.
//
// The mechanism is a guard that meant something other than what it said, which
// is a recurring shape in this engine:
//
//    CMemoryManager::remove_links   if (m_object->g_Alive()) { visual(); sound(); hit(); }
//    CAI_Stalker::net_Relcase       if (!g_Alive()) return;  agent_manager().remove_links(O)
//
// While the creature was DEAD those subsystems skipped their cleanup, so every
// object destroyed in the meantime left a dangling pointer in its memory. The
// guards read as "this subsystem is running"; they were also, silently, what
// kept the stale lists from ever being walked. Revival opens them onto exactly
// the wreckage they were hiding -- and it took a `sv_npc_strip` afterwards
// (which destroys objects, hence net_Relcase) to actually walk it.
//
// So: whatever the creature remembered from before it died is not merely stale,
// it is unsafe, and the only correct thing to do with it is throw it away.
void CAI_Stalker::OnServerRevive(float health_fraction)
{
    const bool was_dead = !g_Alive();

    inherited::OnServerRevive(health_fraction);
    if (!was_dead || !g_Alive())
        return;

    // ⚠⚠NOT memory().reinit(), which is what this did first and it was WRONG in
    // a way that took two builds to see: CVisualMemoryManager::reinit does
    // `if (!m_client) m_objects = 0`, so it NULLS the very squad pointer that
    // CEntityAlive::OnServerRevive just restored by re-registering the member --
    // and the null then sits harmlessly until the mod's AI asks this creature
    // what it can see, which is a different frame and a different stack.
    //
    // ⭐The stale memory is cleaned by the engine's own path instead: group
    // registration hands out fresh squad lists, and CVisualMemoryManager::
    // objects() drops destroyed entries every time it is read.

    // The pose. Death left the animation machine in its death cycle, so a body
    // brought back stands up as a corpse-shaped statue: the T-pose on the
    // ground the first live test produced.
    animation().reinit();
    animation().reload(this);

    Msg("~ [revive] %u '%s': back in its group, animation restarted", ID(), cName().c_str());
}

void CAI_Stalker::OnServerVisualChange(shared_str visual)
{
    // ⚠⚠Only while it is ALIVE. A corpse is a ragdoll: it owns a physics shell
    // built from the skeleton being replaced, and CCharacterPhysicsSupport::
    // in_ChangeVisual asserts it is not a stalker in exactly that case. Dressing
    // the dead is also nothing anybody asked for.
    if (!g_Alive())
    {
        Msg("! [visual] %u '%s' is dead -- refusing to re-dress a ragdoll", ID(), cName().c_str());
        return;
    }

    const shared_str was = cNameVisual();
    inherited::OnServerVisualChange(visual);
    if (cNameVisual() == was)
        return; // refused upstream (missing model), or already wearing it

    // Everything net_Spawn derives from the model, in that same order. The
    // animation set first: every other consumer reads bones through it.
    animation().reload(this);
    reload_bone_protection();

    // IK and any animation-driven collision were built against the old
    // skeleton. ⭐Safe here precisely BECAUSE of the g_Alive() gate above: a
    // living stalker has no physics shell, only a character capsule, and the
    // capsule does not come from the skeleton.
    if (CCharacterPhysicsSupport* cps = character_physics_support())
        cps->in_ChangeVisual();

    if (IKinematics* K = smart_cast<IKinematics*>(Visual()))
    {
        K->CalculateBones_Invalidate();
        K->CalculateBones();
    }

    Msg("~ [visual] %u '%s': '%s' -> '%s', armour and animations rebuilt", ID(), cName().c_str(), was.c_str(), cNameVisual().c_str());
}

void CAI_Stalker::Load(LPCSTR section)
{
    CCustomMonster::Load(section);
    CObjectHandler::Load(section);
    sight().Load(section);
    movement().Load(section);

    // skeleton physics
    m_pPhysics_support->in_Load(section);

    m_can_select_items = !!pSettings->r_bool(section, "can_select_items");
}

BOOL CAI_Stalker::net_Spawn(CSE_Abstract* DC)
{
    LoadProf::Scope _lpNetSpawn("stalker.net_Spawn");
    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeHumanStalker* tpHuman = smart_cast<CSE_ALifeHumanStalker*>(e);
    R_ASSERT(tpHuman);
    m_group_behaviour = !!tpHuman->m_flags.test(CSE_ALifeObject::flGroupBehaviour);

    if (!CObjectHandler::net_Spawn(DC) || !inherited::net_Spawn(DC))
        return (FALSE);

    set_money(tpHuman->m_dwMoney, false);

    {
        LoadProf::Scope _lp("stalker.anim_reload");
        animation().reload(this);
    }

    movement().m_head.current.yaw = movement().m_head.target.yaw = movement().m_body.current.yaw = movement().m_body.target.yaw = angle_normalize_signed(-tpHuman->o_torso.yaw);
    movement().m_body.current.pitch = movement().m_body.target.pitch = 0;

    // a pure network client has no AI map/graph by design: the stalker spawns
    // as a server-driven puppet (visual + physics shell), local AI is never
    // brought up -- its state arrives over the wire
    if (!Level().Server)
    {
        setEnabled(TRUE);
        return (TRUE);
    }

    if (ai().game_graph().valid_vertex_id(tpHuman->m_tGraphID))
        ai_location().game_vertex(tpHuman->m_tGraphID);

    if (ai().game_graph().valid_vertex_id(tpHuman->m_tNextGraphID) && movement().restrictions().accessible(ai().game_graph().vertex(tpHuman->m_tNextGraphID)->level_point()))
        movement().set_game_dest_vertex(tpHuman->m_tNextGraphID);

    R_ASSERT2(ai().get_game_graph() && ai().get_level_graph() && ai().get_cross_table() && (ai().level_graph().level_id() != u32(-1)),
              "There is no AI-Map, level graph, cross table, or graph is not compiled into the game graph!");

    setEnabled(TRUE);

    if (!Level().CurrentViewEntity())
        Level().SetEntity(this);

    if (!g_Alive())
        sound().set_sound_mask(u32(eStalkerSoundMaskDie));

    //загрузить иммунитеты из модельки сталкера
    {
        LoadProf::Scope _lp("stalker.bone_protection");
        reload_bone_protection();
    }

    //вычислить иммунета в зависимости от ранга
    static float novice_rank_immunity = pSettings->r_float("ranks_properties", "immunities_novice_k");
    static float expirienced_rank_immunity = pSettings->r_float("ranks_properties", "immunities_experienced_k");

    static float novice_rank_visibility = pSettings->r_float("ranks_properties", "visibility_novice_k");
    static float expirienced_rank_visibility = pSettings->r_float("ranks_properties", "visibility_experienced_k");

    static float novice_rank_dispersion = pSettings->r_float("ranks_properties", "dispersion_novice_k");
    static float expirienced_rank_dispersion = pSettings->r_float("ranks_properties", "dispersion_experienced_k");

    CHARACTER_RANK_VALUE rank = Rank();
    clamp(rank, 0, 1000);
    float rank_k = float(rank) / 1000.f;
    clamp(rank_k, 0.f, 1.f);
    m_fRankImmunity = novice_rank_immunity + (expirienced_rank_immunity - novice_rank_immunity) * rank_k;
    m_fRankVisibility = novice_rank_visibility + (expirienced_rank_visibility - novice_rank_visibility) * rank_k;
    m_fRankDisperison = expirienced_rank_dispersion + (novice_rank_dispersion - expirienced_rank_dispersion) * (1 - rank_k);

    if (!fis_zero(SpecificCharacter().panic_threshold()))
        m_panic_threshold = SpecificCharacter().panic_threshold();

    sight().setup(CSightAction(SightManager::eSightTypeCurrentDirection));

#ifdef DEBUG
    if (ai().get_alife() && !Level().MapManager().HasMapLocation("debug_stalker", ID()))
    {
        CMapLocation* map_location = Level().MapManager().AddMapLocation("debug_stalker", ID());

        map_location->SetHint(cName());
    }
#endif // DEBUG

    if (SpecificCharacter().terrain_sect().size())
    {
        movement().locations().Load(*SpecificCharacter().terrain_sect());
    }

    {
        LoadProf::Scope _lp("stalker.physics");
        m_pPhysics_support->in_NetSpawn(e);
    }

    return (TRUE);
}

void CAI_Stalker::net_Destroy()
{
    m_pPhysics_support->SyncNetState();
    inherited::net_Destroy();
    CInventoryOwner::net_Destroy();
    m_pPhysics_support->in_NetDestroy();

    Device.remove_from_seq_parallel(fastdelegate::MakeDelegate(this, &CAI_Stalker::update_object_handler));

    xr_delete(m_ce_close);
    xr_delete(m_ce_far);
    xr_delete(m_ce_best);
    xr_delete(m_ce_angle);
    xr_delete(m_ce_safe);
    xr_delete(m_ce_random_game);
    xr_delete(m_ce_ambush);
    xr_delete(m_ce_best_by_time);
    xr_delete(m_boneHitProtection);
}

void CAI_Stalker::net_Save(NET_Packet& P)
{
    inherited::net_Save(P);
    m_pPhysics_support->in_NetSave(P);
}

BOOL CAI_Stalker::net_SaveRelevant() { return (inherited::net_SaveRelevant() || BOOL(PPhysicsShell() != NULL)); }

void CAI_Stalker::net_Export(CSE_Abstract* E)
{
    R_ASSERT(Local());

    // export last known packet
    R_ASSERT(!NET.empty());
    net_update& N = NET.back();

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(E);
    creature->fHealth = GetfHealth();
    creature->timestamp = N.dwTimeStamp;
    creature->flags = 0;
    creature->o_Position = N.p_pos;
    creature->o_model = N.o_model;
    creature->o_torso.yaw = N.o_torso.yaw;
    creature->o_torso.pitch = N.o_torso.pitch;
    creature->o_torso.roll = N.o_torso.roll;

    // ⚠This function is a COPY of CCustomMonster::net_Export, not an extension
    // of it -- it never calls inherited. So every rule the base class has about
    // what the server puts on the wire has to be invoked here explicitly, or it
    // simply does not exist for stalkers. See net_Export_authority.
    net_Export_authority(creature);

    creature->s_team = u8(g_Team());
    creature->s_squad = u8(g_Squad());
    creature->s_group = u8(g_Group());

    CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(E);
    GameGraph::_GRAPH_ID l_game_vertex_id = ai_location().game_vertex_id();
    monster->m_tNextGraphID = l_game_vertex_id;
    monster->m_tPrevGraphID = l_game_vertex_id;
    if (ai().game_graph().valid_vertex_id(l_game_vertex_id))
    {
        monster->m_fDistanceFromPoint = Position().distance_to(ai().game_graph().vertex(l_game_vertex_id)->level_point());
        monster->m_fDistanceToPoint = Position().distance_to(ai().game_graph().vertex(l_game_vertex_id)->level_point());
    }
    else
    {
        monster->m_fDistanceFromPoint = 0;
        monster->m_fDistanceToPoint = 0;
    }

    CSE_ALifeHumanStalker* stalker = smart_cast<CSE_ALifeHumanStalker*>(E);
    stalker->m_start_dialog = m_sStartDialog;

    // ...and the audit sample, for the same reason: the base class's copy of
    // this call never runs for a stalker.
    SVMotionAudit::Note(u16(ID()), "export", creature->o_Position, creature->o_model, server_driven(), creature->timestamp);
}

// [oh prof]: v164 at the bridge -- two 100+ ms frames were one soldier's
// update_object_handler on the second thread, cause unknown. Prints the planner
// phases, the action it was in before/after and whatever load_prof points
// (tex.read, model.read, ps.create, inv.*) ran under it.
static LPCSTR oh_state_name(u32 action_id)
{
    static const char* names[] = {"Show", "Hide", "Drop", "Strapping", "Strapping2Idle", "Unstrapping", "Unstrapping2Idle", "Strapped", "Idle",
                                  "Aim1", "Aim2", "AimForceFull1", "AimForceFull2", "Reload1", "Reload2", "ForceReload1", "ForceReload2", "Fire1",
                                  "Fire2", "Switch1", "Switch2", "QueueWait1", "QueueWait2", "AimingReady1", "AimingReady2", "GetAmmo1", "GetAmmo2",
                                  "ThrowStart", "ThrowIdle", "Throw", "Threaten", "AfterThreaten", "Prepare", "Use"};
    const u32 s = action_id & 0xffff;
    return s < std::size(names) ? names[s] : "?";
}

LPCSTR oh_prop_name(u32 p); // object_handler_planner.cpp

// v166: both 100-ms frames were a search that found no plan and ran into the
// 8000-vertex cap. Say what the goal was and what the item looks like to the planner.
static void oh_explain_no_plan(CAI_Stalker& s)
{
    using namespace ObjectHandlerSpace;
    const CObjectHandlerPlanner& P = s.planner();
    auto section_of = [&](u16 id) -> LPCSTR {
        for (const PIItem it : s.inventory().m_all)
            if (it && it->object().ID() == id)
                return it->object().cNameSect().c_str();
        return "<not in inventory>";
    };
    string2048 buf;
    size_t len = 0;
    buf[0] = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (len + 1 < sizeof(buf))
        {
            int w;
            if constexpr (sizeof...(a) == 0) // a plain string: no format string without arguments (-Wformat-security)
                w = snprintf(buf + len, sizeof(buf) - len, "%s", fmt);
            else
                w = snprintf(buf + len, sizeof(buf) - len, fmt, a...);
            if (w > 0)
                len = std::min(len + size_t(w), sizeof(buf) - 1);
        }
    };
    for (const auto& c : P.target_state().conditions())
    {
        const u16 item = u16(c.condition() >> 16);
        put("goal item %u '%s' %s=%d |", item, item == 0xffff ? "<no items>" : section_of(item), oh_prop_name(c.condition() & 0xffff), int(c.value()));
        static const u32 probe[] = {eWorldPropertyHidden, eWorldPropertyStrapped, eWorldPropertyIdle,   eWorldPropertyIdleStrap, eWorldPropertyDropped,
                                    eWorldPropertyAmmo1,  eWorldPropertyEmpty1,   eWorldPropertyFull1,  eWorldPropertyReady1,    eWorldPropertyAimingReady1};
        for (const u32 p : probe)
        {
            const auto I = P.evaluators().find((u32(item) << 16) | p);
            if (I == P.evaluators().end())
                put(" %s:none", oh_prop_name(p));
            else
                put(" %s:%d", oh_prop_name(p), int((*I).second->evaluate()));
        }
    }
    // items the planner knows (evaluators are sorted by id, so one item's run together)
    put(" | planner items:");
    u32 last = u32(-1);
    for (const auto& e : P.evaluators())
    {
        const u32 item = e.first >> 16;
        if (item == last)
            continue;
        last = item;
        put(" %u'%s'", item, item == 0xffff ? "-" : section_of(u16(item)));
    }
    Msg("~ [oh prof]   no plan: %s", buf);
}

void CAI_Stalker::update_object_handler()
{
    if (!g_Alive())
        return;

    LoadProf::Arm _lpArm;
    const u64 _lpT0 = CPU::QPC();
    const u32 before = planner().initialized() ? u32(planner().current_action_id()) : u32(-1);
    {
        LoadProf::Scope _lpRoot("oh");
        CObjectHandler::update();
    }
    const double ms = 1000.0 * double(CPU::QPC() - _lpT0) / double(CPU::QPCFreq());
    // With the search capped (ai_oh_search_cap) a no-plan search is no longer
    // slow, but it is still the case worth naming: print it regardless of time,
    // the first 40 times in a session.
    static std::atomic<u32> s_no_plan_prints{0};
    const bool no_plan = _lpArm.c.failed && s_no_plan_prints.load(std::memory_order_relaxed) < 40;
    if ((ms >= 30.0 || no_plan) && _lpArm.outer)
    {
        if (no_plan)
            s_no_plan_prints.fetch_add(1, std::memory_order_relaxed);
        const u32 after = planner().initialized() ? u32(planner().current_action_id()) : u32(-1);
        const CInventoryItem* active = inventory().ActiveItem();
        string1024 parts;
        _lpArm.format(parts, sizeof(parts), 0.5);
        Msg("~ [oh prof] '%s' id %u %.0f ms: %s | action %u:%s -> %u:%s, active '%s' | searches %u, visited %u, no plan %u, operators %u, evaluators %u, items %u",
            cName().c_str(), ID(), ms, parts, before >> 16, oh_state_name(before), after >> 16, oh_state_name(after),
            active ? active->object().cNameSect().c_str() : "-", _lpArm.c.searches, _lpArm.c.visited, _lpArm.c.failed, u32(planner().operators().size()),
            u32(planner().evaluators().size()), u32(inventory().m_all.size()));
        if (_lpArm.c.failed)
            oh_explain_no_plan(*this);
    }

    /*try
    {
        try
        {
            CObjectHandler::update();
        }
#ifndef LUABIND_NO_EXCEPTIONS
        catch (luabind::cast_failed& message)
        {
            Msg("! Expression \"%s\" from luabind::object to %s", message.what(), message.info()->name());
            throw;
        }
#endif
        catch (std::exception& message)
        {
            Msg("! Expression \"%s\"", message.what());
            throw;
        }
        catch (...)
        {
            throw;
        }
    }
    catch (...)
    {
        Msg("!![CAI_Stalker::update_object_handler] Error in function CObjectHandler::update()");
        CObjectHandler::set_goal(eObjectActionIdle);
        CObjectHandler::update();
    }*/
}

void CAI_Stalker::create_anim_mov_ctrl(CBlend* b)
{
    inherited::create_anim_mov_ctrl(b);

    m_sight_enabled_before_animation_controller = sight().enabled();
    sight().enable(false);
}

void CAI_Stalker::destroy_anim_mov_ctrl()
{
    inherited::destroy_anim_mov_ctrl();

    sight().enable(m_sight_enabled_before_animation_controller);

    movement().m_head.current.yaw = movement().m_body.current.yaw;
    movement().m_head.current.pitch = movement().m_body.current.pitch;
    movement().m_head.target.yaw = movement().m_body.current.yaw;
    movement().m_head.target.pitch = movement().m_body.current.pitch;
}

void CAI_Stalker::UpdateCL()
{
    START_PROFILE("stalker")
    START_PROFILE("stalker/client_update")
    VERIFY2(PPhysicsShell() || getEnabled(), *cName());

    // server-driven puppet on a pure network client: no local AI, but the
    // interpolation baseline and the physics shell stay alive
    if (!Level().Server)
    {
        inherited::UpdateCL();
        // foot IK and animation collision read ghost movement state and
        // re-touch the placed model -- render-only while driven; the corpse
        // path keeps this (ragdoll interpolation lives here)
        if (!net_render_only_puppet())
            m_pPhysics_support->in_UpdateCL();
        return;
    }

    // ⭐Same call on the host as the thin client makes from its interpolator:
    // the servo consumes a look direction, and by this point both sides have
    // one. This is what "one code path" looks like in the small.
    // [frame prof] parts: 08-10 Switch v256, one stalker's UpdateCL took 22.8 ms in a firefight (esc_lager6), unnamed
    LoadProf::Scope _lpAim("stl.aim");
    update_aim_servo();
    update_lean();
    update_aim_miss();
    update_combat_restrictions();

    if (g_Alive())
    {
        if (g_mt_config.test(mtObjectHandler) && CObjectHandler::planner().initialized())
        {
            Device.add_to_seq_parallel(fastdelegate::MakeDelegate(this, &CAI_Stalker::update_object_handler), "stalker update_object_handler");
        }
        else
        {
            LoadProf::Scope _lpOh("stl.object_handler");
            START_PROFILE("stalker/client_update/object_handler")
            update_object_handler();
            STOP_PROFILE
        }

        if ((movement().speed(character_physics_support()->movement()) > EPS_L) && (eMovementTypeStand != movement().movement_type()) &&
            (eMentalStateDanger == movement().mental_state()))
        {
            if ((eBodyStateStand == movement().body_state()) && (eMovementTypeRun == movement().movement_type()))
            {
                sound().play(eStalkerSoundRunningInDanger);
            }
            else
            {
                //				sound().play	(eStalkerSoundWalkingInDanger);
            }
        }
    }

    LoadProf::Scope _lpInh("stl.inherited");   // CCustomMonster::UpdateCL (+ the scopes below nest in it: they subtract)
    START_PROFILE("stalker/client_update/inherited")
    inherited::UpdateCL();
    STOP_PROFILE

    START_PROFILE("stalker/client_update/physics")
    LoadProf::Scope _lpPh("stl.physics");
    m_pPhysics_support->in_UpdateCL();
    STOP_PROFILE

    if (g_Alive())
    {
        START_PROFILE("stalker/client_update/sight_manager")
        LoadProf::Scope _lpSight("stl.sight");
        VERIFY(!m_pPhysicsShell);
        XR_SEH_TRY
        {
            sight().update();
        }
        XR_SEH_EXCEPT(ExceptStackTrace("[" __FUNCTION__ "] stack trace:\n"))
        {
            XR_SEH_TRY
            {
                sight().setup(CSightAction(SightManager::eSightTypeCurrentDirection));
                sight().update();
            }
            XR_SEH_EXCEPT(ExceptStackTrace("[" __FUNCTION__ "] - 2 stack trace:\n"))
            {
                Msg("!![%s] error in sight().update() of NPC [%s]", __FUNCTION__, this->Name());
            }
        }

        Exec_Look(client_update_fdelta());
        STOP_PROFILE

        START_PROFILE("stalker/client_update/step_manager")
        LoadProf::Scope _lpStep("stl.step");
        CStepManager::update();
        STOP_PROFILE

        START_PROFILE("stalker/client_update/weapon_shot_effector")
        if (weapon_shot_effector().IsActive())
            weapon_shot_effector().Update();
        STOP_PROFILE
    }
    STOP_PROFILE
    STOP_PROFILE
}

void CAI_Stalker ::PHHit(SHit& H) { m_pPhysics_support->in_Hit(H, !g_Alive()); }

CPHDestroyable* CAI_Stalker::ph_destroyable() { return smart_cast<CPHDestroyable*>(character_physics_support()); }

#include "../../enemy_manager.h"

// [sched prof]: v170 -- after the fight at the bridge (and in the Cordon camp
// before it) frame_move took 60-110 ms with "Single item [<stalker>] took whole
// update frame": one stalker's scheduled update. Same breakdown as [spawn prof]:
// the NPC_ZONE parts, the planners (plan.*, act.*), and "lua" for the time inside
// the VM. At most one line per 250 ms and 300 a session -- printing costs on the SD.
int g_sched_prof_ms = 30; // al_sched_prof_ms: a stalker update at least this long is printed (needs dbg_frame_prof off: one Arm)
void CAI_Stalker::shedule_Update(u32 DT)
{
    if (m_inventory_spawn_pending)   // its items are still coming online (CLevel::ProcessGameSpawns): one or two frames
        return;
    LoadProf::Arm _lpArm;
    const u64 _lpT0 = CPU::QPC();
    {
        LoadProf::Scope _lpRoot("sched");
        shedule_Update_body(DT);
    }
    const double ms = 1000.0 * double(CPU::QPC() - _lpT0) / double(CPU::QPCFreq());
    if (ms >= double(g_sched_prof_ms) && _lpArm.outer)
    {
        static u32 s_last = 0, s_count = 0;
        const u32 now = Device.dwTimeContinual;
        if (s_count < 300 && now - s_last >= 250)
        {
            s_last = now;
            ++s_count;
            string1024 parts;
            _lpArm.format(parts, sizeof(parts), 0.5);
            Msg("~ [sched prof] '%s' id %u %.0f ms (dt %u): %s", cName().c_str(), ID(), ms, DT, parts);
        }
    }
}

void CAI_Stalker::shedule_Update_body(u32 DT)
{
    START_PROFILE("stalker")
    START_PROFILE("stalker/schedule_update")
    VERIFY2(getEnabled() || PPhysicsShell(), *cName());

    // server-driven puppet on a pure network client: skip local AI scheduling
    // entirely, keep the physics shell ticking
    if (!Level().Server)
    {
        m_pPhysics_support->in_shedule_Update(DT);
        return;
    }

    if (!CObjectHandler::planner().initialized())
    {
        START_PROFILE("stalker/client_update/object_handler")
        { NPC_ZONE(S_OBJHANDLER); update_object_handler(); }
        STOP_PROFILE
    }
    //	if (Position().distance_to(Level().CurrentEntity()->Position()) <= 50.f)
    //		Msg				("[%6d][SH][%s]",Device.dwTimeGlobal,*cName());
    // Queue shrink
    VERIFY(_valid(Position()));
    u32 dwTimeCL = Level().timeServer() - NET_Latency;
    VERIFY(!NET.empty());
    while ((NET.size() > 2) && (NET[1].dwTimeStamp < dwTimeCL))
        NET.pop_front();

    Fvector vNewPosition = Position();
    VERIFY(_valid(Position()));
    // *** general stuff
    float dt = float(DT) / 1000.f;

    if (g_Alive())
    {
        { NPC_ZONE(S_ANIMCB); animation().play_delayed_callbacks(); }

#ifndef USE_SCHEDULER_IN_AGENT_MANAGER
        { NPC_ZONE(S_AGENT); agent_manager().update(); }
#endif // USE_SCHEDULER_IN_AGENT_MANAGER

//		bool			check = !!memory().enemy().selected();
#if 0 // def DEBUG
		memory().visual().check_visibles();
#endif
        {
            // ⚠При mtAiVision здесь видна ТОЛЬКО цена постановки задачи:
            // сама видимость считается на воркере.
            NPC_ZONE(S_VISION);
            if (g_mt_config.test(mtAiVision))
                Device.add_to_seq_parallel(fastdelegate::MakeDelegate(this, &CCustomMonster::Exec_Visibility), "stalker visibility");
            else
            {
                START_PROFILE("stalker/schedule_update/vision")
                Exec_Visibility();
                STOP_PROFILE
            }
        }

        START_PROFILE("stalker/schedule_update/memory")

        START_PROFILE("stalker/schedule_update/memory/process")
        { NPC_ZONE(S_ENEMIES); process_enemies(); }
        STOP_PROFILE

        START_PROFILE("stalker/schedule_update/memory/update")
        { NPC_ZONE(S_MEMORY); memory().update(dt); }
        STOP_PROFILE

        STOP_PROFILE
        { NPC_ZONE(S_ANOMALY); anomaly_detector().update_schedule(); }
    }

    START_PROFILE("stalker/schedule_update/inherited")
    // https://github.com/OpenXRay/xray-16/commit/30add3fdf05472faaa954f1c18783783fb5dc5bb
    { NPC_ZONE(S_INHERITED); CEntityAlive::shedule_Update(DT); }
    STOP_PROFILE

    if (Remote())
    {}
    else
    {
        // here is monster AI call
        VERIFY(_valid(Position()));
        m_fTimeUpdateDelta = dt;
        Device.Statistic->AI_Think.Begin();
        NPC_ZONE(S_THINK);
        // A server order, if it has one, is taken here: under script control
        // it is executed by the lines just below, by this creature's own
        // movement manager. See SVNpc::Steer.
        SVNpc::Steer(this);
        if (GetScriptControl())
            ProcessScripts();
        else
#ifdef DEBUG
            if (Device.dwFrame > (spawn_time() + g_AI_inactive_time))
#endif
            Think();
        m_dwLastUpdateTime = Device.dwTimeGlobal;
        Device.Statistic->AI_Think.End();
        VERIFY(_valid(Position()));

        // Look and action streams
        float temp = conditions().health();
        if (temp > 0)
        {
            START_PROFILE("stalker/schedule_update/feel_touch")
            {
                NPC_ZONE(S_FEELTOUCH);
                Fvector C;
                float R;
                Center(C);
                R = Radius();
                feel_touch_update(C, R);
            }
            STOP_PROFILE

            START_PROFILE("stalker/schedule_update/net_update")
            net_update uNext;
            uNext.dwTimeStamp = Level().timeServer();
            uNext.o_model = movement().m_body.current.yaw;
            uNext.o_torso = movement().m_head.current;
            uNext.p_pos = vNewPosition;
            uNext.fHealth = GetfHealth();
            NET.push_back(uNext);
            STOP_PROFILE
        }
        else
        {
            START_PROFILE("stalker/schedule_update/net_update")
            net_update uNext;
            uNext.dwTimeStamp = Level().timeServer();
            uNext.o_model = movement().m_body.current.yaw;
            uNext.o_torso = movement().m_head.current;
            uNext.p_pos = vNewPosition;
            uNext.fHealth = GetfHealth();
            NET.push_back(uNext);
            STOP_PROFILE
        }
    }
    VERIFY(_valid(Position()));

    START_PROFILE("stalker/schedule_update/inventory_owner")
    { NPC_ZONE(S_INVOWNER); UpdateInventoryOwner(DT); }
    STOP_PROFILE

    //#ifdef DEBUG
    //	if (psAI_Flags.test(aiALife)) {
    //		smart_cast<CSE_ALifeHumanStalker*>(ai().alife().objects().object(ID()))->check_inventory_consistency();
    //	}
    //#endif

    START_PROFILE("stalker/schedule_update/physics")
    VERIFY(_valid(Position()));
    { NPC_ZONE(S_PHYSICS); m_pPhysics_support->in_shedule_Update(DT); }
    VERIFY(_valid(Position()));
    STOP_PROFILE
    STOP_PROFILE
    STOP_PROFILE
}

// ⭐AI LOD (`ai_think_lod`, 11-10, Switch). A big fight (~25 NPC) put the main thread at 81-90 %, and the stalker
// brain -- Think: memory, planner, Lua evaluators -- was ~15 % of it. The scheduler already spaces a stalker's
// updates by distance (CObject::shedule_Scale = distance / 200 -> 100 ms at the actor, ~180 ms at 30 m); this adds
// what the player cannot see: a stalker off screen and not right next to the camera thinks as if it were further
// away (scale floor ai_think_lod_scale; 0.25 = ~230 ms instead of ~130). Movement, aiming and animation stay per
// frame (UpdateCL); only how often it re-decides changes.
int g_ai_think_lod = 0;
float g_ai_think_lod_scale = 0.25f;
float g_ai_think_lod_near = 8.f;

float CAI_Stalker::shedule_Scale()
{
    const float base = inherited::shedule_Scale();
    if (!g_ai_think_lod || base >= g_ai_think_lod_scale)
        return base;
    Fvector to;
    to.sub(Position(), Device.vCameraPosition);
    const float d = to.magnitude();
    if (d < g_ai_think_lod_near)
        return base;
    to.div(d);
    // the horizontal half-angle of the view (fFOV is vertical, the screen 16:9) plus a margin
    static float s_fov = -1.f, s_cos = 0.f;
    if (s_fov != Device.fFOV)
    {
        s_fov = Device.fFOV;
        const float h = atanf(tanf(deg2rad(Device.fFOV) * 0.5f) * (16.f / 9.f)) + deg2rad(15.f);
        s_cos = cosf(std::min(h, PI * 0.95f));
    }
    if (to.dotproduct(Device.vCameraDirection) >= s_cos)
        return base; // on screen
    return g_ai_think_lod_scale;
}

float CAI_Stalker::Radius() const
{
    float R = inherited::Radius();
    CWeapon* W = smart_cast<CWeapon*>(inventory().ActiveItem());
    if (W)
        R += W->Radius();
    return R;
}

// ⛔OFF BY DEFAULT, AND THE REASON IS A MEASUREMENT, NOT CAUTION.
//
// The servo is correct and it does not work, because the loop is not closed:
// the correction reaches the bones, but the barrel does not move with them.
// Measured 29-07-2026 on a live client, two runs with the feedback sign
// inverted between them:
//
//     bias pinned at (+35, -35) deg  ->  barrel 60 deg off the look direction
//     bias pinned at (-35, +35) deg  ->  barrel 56 deg off, the SAME way
//
// A 70-degree swing of the input moved the output by four. Whatever
// CWeapon::get_LastFD reports for these creatures, it is not a function of the
// spine chain -- so no gain and no sign can converge it.
//
// ⭐The suspect, and the next thing to measure: CWeapon::UpdateXForm returns
// early for a weapon that is `parent->attached(this)`, leaving XFORM() -- and
// therefore vLastFD -- as whatever it last was. The hands log on the same runs
// shows these NPCs failing to seat the weapon in a slot (`put=0`), which is
// how an item ends up drawn as an attachment instead. If that is it, the muzzle
// flash and tracer are being placed off a stale transform too, which would
// matter more than the aim.
//
// Kept in the build because it is the only instrument that can tell whether
// that ever gets fixed: turn it on, read `dbg_aim`, and the error either
// converges or it does not.
int g_cl_aim_ik = 0;

void CAI_Stalker::update_aim_servo()
{
    // ⚠Once per frame whoever calls first. The host calls this from UpdateCL,
    // the thin client from the puppet interpolator, and a creature can be
    // reached by both paths in a fused session.
    if (m_aim_frame == Device.dwFrame)
        return;
    m_aim_frame = Device.dwFrame;

    // ⭐11-10-2026 (Switch fight): measure only when someone reads it -- the servo (ai_aim_ik) or its diagnostic
    // (dbg_aim). The measurement below reads get_LastFD, which rebuilds the weapon transform from the hand bones and so
    // ran CKinematics::CalculateBones of every armed stalker on the main thread inside UpdateCL, every frame: ~18 % of
    // the stalkers' UpdateCL in the sampler, and those skeletons were then already done when the parallel bone pass
    // (r_bones_mt) came to them. With both off the bias is zero anyway (see below).
    extern int g_dbg_aim;
    if (!g_cl_aim_ik && !g_dbg_aim)
    {
        m_aim_bias_yaw = m_aim_bias_pitch = 0.f;
        m_aim_err_deg = 0.f;
        return;
    }

    // ⚠MEASURE ALWAYS, CORRECT ONLY WHEN ASKED. The first cut returned here
    // when the servo was switched off, which silently took the INSTRUMENT with
    // it -- and the number it prints, how far the barrel misses, is exactly
    // what is wanted with the correction off: that is the baseline the shipped
    // aim delivers. An instrument that only works when the fix is on cannot
    // tell you whether the fix helped.
    if (!g_Alive())
    {
        m_aim_bias_yaw = m_aim_bias_pitch = 0.f;
        m_aim_err_deg = 0.f;
        return;
    }

    // Nothing to aim: no weapon means no barrel to measure, and the bias must
    // not be left standing on a body that has holstered.
    CWeapon* W = smart_cast<CWeapon*>(inventory().ActiveItem());
    if (!W || W->H_Parent() != this)
    {
        m_aim_bias_yaw = m_aim_bias_pitch = 0.f;
        m_aim_err_deg = 0.f;
        return;
    }

    // Where we want it pointed. The same two numbers the callbacks already
    // consume, and the same two the wire carries -- head yaw is where the
    // creature is looking, o_torso.pitch is how far up or down.
    const float want_yaw = movement().head_orientation().current.yaw;
    const float want_pitch = NET_Last.o_torso.pitch;

    // ⚠And where it IS pointed -- the measurement the shipped aim never took.
    // get_LastFD is rebuilt from the parent's hand bones (CWeapon::UpdateXForm),
    // so it already contains everything the percentages cannot know: the cycle
    // playing, the weapon's own model, the attach point.
    Fvector have = W->get_LastFD();
    if (have.magnitude() < EPS)
        return; // the weapon transform is not built yet -- nothing to measure
    have.normalize();

    // ⚠⚠SIGNS SETTLED BY MEASUREMENT, NOT BY READING THE CALLBACKS.
    //
    // The first cut negated both of these, reasoning from the minus signs in
    // stalker_animation_callbacks.cpp -- and those describe how a rotation is
    // APPLIED to a bone, not how a world direction is READ. The servo then had
    // positive feedback: the diagnostic showed the bias pinned at both clamps
    // (35.0, -35.0) with the error never shrinking, which is what divergence
    // looks like and is why the bias is clamped at all.
    //
    // ⭐What named it was the pairing in the log: have_yaw tracked want_yaw one
    // for one with a constant offset (want 0.03 -> have -1.08, want 0.42 ->
    // -1.44, want 0.77 -> -1.78). A measurement that tracks the target 1:1 is
    // the right measurement wearing a minus sign; the constant is the error the
    // loop is supposed to remove.
    const float have_yaw = atan2f(have.x, have.z);
    const float have_pitch = asinf(clampr(have.y, -1.f, 1.f));
    const float err_yaw = angle_normalize_signed(want_yaw - have_yaw);
    const float err_pitch = angle_normalize_signed(want_pitch - have_pitch);

    m_aim_err_deg = rad2deg(_sqrt(err_yaw * err_yaw + err_pitch * err_pitch));

    // ⚠A FRACTION of the error, not the error. The measurement is one frame
    // stale by construction -- get_LastFD reflects the bones built from LAST
    // frame's bias -- so feeding all of it back overshoots and rings. At 0.5 it
    // is inside a degree within three frames and cannot oscillate.
    if (!g_cl_aim_ik)
    {
        // Instrument only: hold the bias at zero so the error above is the
        // SHIPPED aim's own miss, with nothing of ours on top of it.
        m_aim_bias_yaw = m_aim_bias_pitch = 0.f;
    }
    else
    {
        const float gain = 0.5f;
        m_aim_bias_yaw += err_yaw * gain;
        m_aim_bias_pitch += err_pitch * gain;
    }

    // ...and bounded by what a spine can do. Past this the body has to come
    // round, which is the movement manager's job and not ours -- an unbounded
    // servo would happily fold the puppet in half chasing a target behind it.
    const float k_max_bias = deg2rad(35.f);
    clamp(m_aim_bias_yaw, -k_max_bias, k_max_bias);
    clamp(m_aim_bias_pitch, -k_max_bias, k_max_bias);

    // diag: the miss, in degrees, and the bias holding it there. Throttled by
    // count with a running total -- a silent counter and a converged servo must
    // not look the same.
    extern int g_dbg_aim;
    if (g_dbg_aim)
    {
        static u32 s_diag = 0;
        if (++s_diag % 121 == 1)
            Msg("~ [aim] id=%u servo=%d err=%.1fdeg (yaw %.1f pitch %.1f) bias=(%.1f %.1f)deg want=(%.2f %.2f) have=(%.2f %.2f) -- %u samples", ID(), g_cl_aim_ik,
                m_aim_err_deg, rad2deg(err_yaw), rad2deg(err_pitch), rad2deg(m_aim_bias_yaw), rad2deg(m_aim_bias_pitch), want_yaw, want_pitch, have_yaw, have_pitch,
                s_diag);
    }
}

void CAI_Stalker::spawn_supplies()
{
    inherited::spawn_supplies();
    CObjectHandler::spawn_supplies();
}

void CAI_Stalker::Think()
{
    START_PROFILE("stalker/schedule_update/think")
    u32 update_delta = Device.dwTimeGlobal - m_dwLastUpdateTime;

    START_PROFILE("stalker/schedule_update/think/brain")
    //	try {
    //		try {
    brain().update(update_delta);
//		}
#ifndef LUABIND_NO_EXCEPTIONS
//		catch (luabind::cast_failed &message) {
//			Msg						("! Expression \"%s\" from luabind::object to %s",message.what(),message.info()->name());
//			throw;
//		}
#endif
//		catch (std::exception &message) {
//			Msg						("! Expression \"%s\"",message.what());
//			throw;
//		}
//		catch (...) {
//			Msg						("! unknown exception occured");
//			throw;
//		}
//	}
//	catch(...) {
#ifdef DEBUG
//		Msg						("! Last action being executed : %s",brain().current_action().m_action_name);
#endif
    //		brain().setup			(this);
    //		brain().update			(update_delta);
    //	}
    STOP_PROFILE

    START_PROFILE("stalker/schedule_update/think/movement")
    if (!g_Alive())
        return;

    //	try {
    movement().update(update_delta);
//	}
#if 0 // ndef LUABIND_NO_EXCEPTIONS
	catch (luabind::cast_failed &message) {
		Msg						("! Expression \"%s\" from luabind::object to %s",message.what(),message.info()->name());
		movement().initialize	();
		movement().update		(update_delta);
		throw;
	}
	catch (std::exception &message) {
		Msg						("! Expression \"%s\"",message.what());
		movement().initialize	();
		movement().update		(update_delta);
		throw;
	}
	catch (...) {
		Msg						("! unknown exception occured");
		movement().initialize	();
		movement().update		(update_delta);
		throw;
	}
#endif // DEBUG

    STOP_PROFILE
    STOP_PROFILE
}

void CAI_Stalker::SelectAnimation(const Fvector& view, const Fvector& move, float speed)
{
    if (!Device.Paused())
        animation().update();
}

const SRotation CAI_Stalker::Orientation() const { return (movement().m_head.current); }

const MonsterSpace::SBoneRotation& CAI_Stalker::head_orientation() const { return (movement().head_orientation()); }

void CAI_Stalker::net_Relcase(CObject* O)
{
    inherited::net_Relcase(O);

    sight().remove_links(O);

    if (!g_Alive())
        return;

    agent_manager().remove_links(O);
    m_pPhysics_support->in_NetRelcase(O);
}

CMovementManager* CAI_Stalker::create_movement_manager() { return (m_movement_manager = xr_new<CStalkerMovementManager>(this)); }

CSound_UserDataVisitor* CAI_Stalker::create_sound_visitor() { return (m_sound_user_data_visitor = xr_new<CStalkerSoundDataVisitor>(this)); }

CMemoryManager* CAI_Stalker::create_memory_manager() { return (xr_new<CMemoryManager>(this, create_sound_visitor())); }

DLL_Pure* CAI_Stalker::_construct()
{
    m_pPhysics_support = xr_new<CCharacterPhysicsSupport>(CCharacterPhysicsSupport::etStalker, this);
    CCustomMonster::_construct();
    CObjectHandler::_construct();
    CStepManager::_construct();

    m_actor_relation_flags.zero();
    m_animation_manager = xr_new<CStalkerAnimationManager>();
    m_brain = xr_new<CStalkerPlanner>();
    m_sight_manager = xr_new<CSightManager>(this);
    m_weapon_shot_effector = xr_new<CWeaponShotEffector>();

    return (this);
}

bool CAI_Stalker::use_center_to_aim() const { return (!wounded() && (movement().body_state() != eBodyStateCrouch)); }

#ifdef DEBUG
void CAI_Stalker::UpdateCamera()
{
    float new_range = eye_range, new_fov = eye_fov;
    Fvector temp = eye_matrix.k;
    if (g_Alive())
    {
        update_range_fov(new_range, new_fov, memory().visual().current_state().m_max_view_distance * eye_range, eye_fov);
        if (weapon_shot_effector().IsActive())
            temp = weapon_shot_effector_direction(temp);
    }

    g_pGameLevel->Cameras().Update(eye_matrix.c, temp, eye_matrix.j, new_fov, .75f, new_range, 0);
}
#endif

bool CAI_Stalker::can_attach(const CInventoryItem* inventory_item) const
{
    if (already_dead())
        return (false);

    return (CObjectHandler::can_attach(inventory_item));
}

void CAI_Stalker::save(NET_Packet& packet)
{
    inherited::save(packet);
    CInventoryOwner::save(packet);
    brain().save(packet);
}

void CAI_Stalker::load(IReader& packet)
{
    inherited::load(packet);
    CInventoryOwner::load(packet);
    brain().load(packet);
}

void CAI_Stalker::load_critical_wound_bones()
{
    fill_bones_body_parts("head", critical_wound_type_head);
    fill_bones_body_parts("torso", critical_wound_type_torso);
    fill_bones_body_parts("hand_left", critical_wound_type_hand_left);
    fill_bones_body_parts("hand_right", critical_wound_type_hand_right);
    fill_bones_body_parts("leg_left", critical_wound_type_leg_left);
    fill_bones_body_parts("leg_right", critical_wound_type_leg_right);
}

void CAI_Stalker::fill_bones_body_parts(LPCSTR bone_id, const ECriticalWoundType& wound_type)
{
    LPCSTR body_parts_section_id = pSettings->r_string(cNameSect(), "body_parts_section_id");
    VERIFY(body_parts_section_id);

    LPCSTR body_part_section_id = pSettings->r_string(body_parts_section_id, bone_id);
    VERIFY(body_part_section_id);

    IKinematics* kinematics = smart_cast<IKinematics*>(Visual());
    VERIFY(kinematics);

    CInifile::Sect& body_part_section = pSettings->r_section(body_part_section_id);
    for (const auto& I : body_part_section.Data)
        m_bones_body_parts.insert(std::make_pair(kinematics->LL_BoneID(I.first), u32(wound_type)));
}

void CAI_Stalker::on_before_change_team() { m_registered_in_combat_on_migration = agent_manager().member().registered_in_combat(this); }

void CAI_Stalker::on_after_change_team()
{
    if (!m_registered_in_combat_on_migration)
        return;

    agent_manager().member().register_in_combat(this);
}

bool CAI_Stalker::unlimited_ammo() { return infinite_ammo() && CObjectHandler::planner().object().g_Alive(); }

CAgentManager& CAI_Stalker::agent_manager() const { return Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(g_Group()).agent_manager(); }

bool CAI_Stalker::CanPutInSlot(PIItem item, u32 slot)
{
    if (smart_cast<CArtefact*>(item))
        return false;

    return slot != OUTFIT_SLOT;
}

void CAI_Stalker::renderable_Render(u32 context_id, IRenderable* root)
{
    inherited::renderable_Render(context_id, root);

    if (!already_dead())
        CInventoryOwner::renderable_Render(context_id, root);

#ifdef DEBUG
    if (g_Alive())
    {
        if (psAI_Flags.test(aiAnimationStats))
            animation().add_animation_stats();
    }
#endif // DEBUG
}
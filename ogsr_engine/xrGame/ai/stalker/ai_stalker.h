////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker.h
//	Created 	: 25.02.2003
//  Modified 	: 25.02.2003
//	Author		: Dmitriy Iassenev
//	Description : AI Behaviour for monster "Stalker"
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ai_monster_space.h"
#include "ai/stalker/ai_stalker_space.h"
#include "stalker_animation_names.h"

#include "../../CustomMonster.h"
#include "../../object_handler.h"
#include "../../AI_PhraseDialogManager.h"
#include "../../step_manager.h"
#include "script_export_space.h"

#ifdef DEBUG
template <typename _object_type>
class CActionBase;

template <typename _object_type>
class CPropertyEvaluator;

template <typename _object_type, bool _reverse_search, typename _world_operator, typename _condition_evaluator, typename _world_operator_ptr, typename _condition_evaluator_ptr>
class CActionPlanner;

typedef CActionPlanner<CScriptGameObject, false, CActionBase<CScriptGameObject>, CPropertyEvaluator<CScriptGameObject>, CActionBase<CScriptGameObject>*,
                       CPropertyEvaluator<CScriptGameObject>*>
    script_planner;
#endif


class CALifeSimulator;
class CCharacterPhysicsSupport;
class CWeapon;
class CCoverPoint;
class CCoverEvaluatorCloseToEnemy;
class CCoverEvaluatorFarFromEnemy;
class CCoverEvaluatorBest;
class CCoverEvaluatorAngle;
class CCoverEvaluatorSafe;
class CCoverEvaluatorRandomGame;
class CCoverEvaluatorAmbush;
class CCoverEvaluatorBestByTime;
class CAgentManager;
class CMotionDef;
class CStalkerAnimationManager;
class CStalkerPlanner;
class CSightManager;
class CStalkerMovementManager;
class CStalkerSoundDataVisitor;
class CWeaponShotEffector;
struct SBoneProtections;
class CDangerLocation;
class CRestrictedObject;

class CAI_Stalker : public CCustomMonster, public CObjectHandler, public CAI_PhraseDialogManager, public CStepManager
{
private:
    typedef CCustomMonster inherited;

public:
    using inherited::evaluate;
    using inherited::useful;

private:
    CStalkerAnimationManager* m_animation_manager;
    CStalkerPlanner* m_brain{};
    CSightManager* m_sight_manager;
    CStalkerMovementManager* m_movement_manager;

#ifdef DEBUG
    const script_planner* m_debug_planner;
#endif

    // ALife
private:
    SBoneProtections* m_boneHitProtection;

    // weapon dispersion
public:
    float m_fDispBase;

private:
    float m_disp_walk_stand;
    float m_disp_walk_crouch;
    float m_disp_run_stand;
    float m_disp_run_crouch;
    float m_disp_stand_stand;
    float m_disp_stand_crouch;
    float m_disp_stand_stand_zoom;
    float m_disp_stand_crouch_zoom;
    bool m_fast_can_kill_entity;

private:
    float m_power_fx_factor;

private:
    float m_fRankDisperison;
    float m_fRankVisibility;
    float m_fRankImmunity;

    // best item/ammo selection members
public:
    bool m_item_actuality;
    CInventoryItem* m_best_item_to_kill;
    float m_best_item_value;
    CInventoryItem* m_best_ammo;
    const CInventoryItem* m_best_found_item_to_kill;
    const CInventoryItem* m_best_found_ammo;

    // covers being used
public:
    CCoverEvaluatorCloseToEnemy* m_ce_close;
    CCoverEvaluatorFarFromEnemy* m_ce_far;
    CCoverEvaluatorBest* m_ce_best;
    CCoverEvaluatorAngle* m_ce_angle;
    CCoverEvaluatorSafe* m_ce_safe;
    CCoverEvaluatorRandomGame* m_ce_random_game;
    CCoverEvaluatorAmbush* m_ce_ambush;
    CCoverEvaluatorBestByTime* m_ce_best_by_time;

    // physics support
public:
    CCharacterPhysicsSupport* m_pPhysics_support;

public:
    bool m_wounded{};
    bool m_headshot{};
    // Its inventory is still in the level's spawn queue (CLevel::ProcessGameSpawns spreads it over the next frames):
    // the scheduled update -- brain and script binder -- waits for it, so no script sees the NPC half-dressed
    // (death_manager's first update of a corpse strips and refills the loot exactly once).
    bool m_inventory_spawn_pending{};

    // ⭐⭐AIM AS A CLOSED LOOP: correction the spine/shoulder/head callbacks add
    // on top of what they already compute, driven by where the WEAPON actually
    // ends up pointing.
    //
    // The shipped aim is open-loop: the callbacks rotate each bone by a fixed
    // PERCENTAGE of the head-vs-body yaw difference (spine 25%, shoulder 25%,
    // head 50% -- literally the template arguments in
    // stalker_animation_callbacks.cpp). Nothing in that chain ever asks where
    // the barrel came out, so the result is "roughly towards", and it cannot be
    // otherwise: the answer depends on the animation playing, the weapon model
    // and the hand attach point, none of which those percentages know about.
    //
    // A servo does not need to know any of them. Measure the error between the
    // weapon's real forward axis and the direction we want, feed a fraction of
    // it back as bias, repeat. Converges in a few frames, self-corrects when
    // the animation or the weapon changes, and is the same code in single
    // player and over the network -- it consumes a DIRECTION, and both have one.
    float m_aim_bias_yaw{0.f};
    float m_aim_bias_pitch{0.f};
    float m_aim_err_deg{0.f}; // last measured miss, for the diagnostic
    u32 m_aim_frame{0}; // the servo runs once per frame, wherever it is called from

    // Leaning out from behind cover. There is no lean or loophole animation in
    // the content, so the spine and shoulder bones roll the upper body sideways
    // (stalker_animation_callbacks.cpp). The eye and the muzzle both hang off
    // the head bone, so the roll moves the view and the line of fire past the
    // edge of the wall too. Chosen by rays in update_lean, host only.
    float m_lean{0.f}; // current roll, radians, + = right
    float m_lean_target{0.f};
    u32 m_lean_check_time{0};
    // a high wall right in his face: no crouching there (see
    // CStalkerMovementManager::update)
    bool m_wall_ahead{false};
    u32 m_wall_check_time{0};
    // the restrictors lifted for a fight (CRestrictedObject::lift_restrictions)
    // come back this long after the last enemy is gone
    u32 m_no_enemy_since{0};
    void update_combat_restrictions();
    u8 m_lean_reason{0};
    // for the instrument: where the straight ray hit, where each side failed
    // (-1 = no room to lean that way, 0 = not tried), how far the head travels
    float m_lean_hit{0.f};
    float m_lean_side_hit[2]{}; // [0] left, [1] right
    float m_lean_shift{0.f};
    float lean_head_shift(float roll) const;
    // Stepping out to the edge of the cover, when a lean does not clear it: a
    // stalker in the middle of a wide wall sees nothing either way. Found by
    // the same rays (update_lean), walked to by the hold/look-out actions.
    Fvector m_peek_pos{};
    u32 m_peek_vertex{u32(-1)};
    u32 m_peek_time{0};
    float m_peek_side{0.f};
    bool find_peek_step(const Fvector& eye, const Fvector& right, const Fvector& target, float first_side);

    // The accuracy curve, as in F.E.A.R.: the first shots at a player just
    // spotted go past him, and the aim closes in while he stays in sight.
    // update_aim_miss keeps the offset, g_fireParams applies it to the shots.
    u32 m_aim_acquire_time{0}; // when the player came into sight this time
    u32 m_aim_seen_time{0}; // last time he was in sight
    float m_aim_miss_side{1.f}; // left or right of him, once per sighting
    float m_aim_miss_dir{0.f}; // phase of the sway
    float m_aim_miss_yaw{0.f};
    float m_aim_miss_pitch{0.f};
    void update_aim_miss();

public:
    CAI_Stalker();
    virtual ~CAI_Stalker();

    // Run one step of the aim servo. Called from UpdateCL on the host and from
    // the puppet interpolator on a thin client -- both have a look direction,
    // and that is all this needs.
    void update_aim_servo();
    IC float aim_bias_yaw() const { return m_aim_bias_yaw; }
    IC float aim_bias_pitch() const { return m_aim_bias_pitch; }

    void update_lean();
    IC float lean() const { return m_lean; }
    IC float lean_target() const { return m_lean_target; }
    IC bool wall_ahead() const { return m_wall_ahead; }
    LPCSTR lean_reason() const;
    IC float lean_hit() const { return m_lean_hit; }
    IC float lean_side_hit(int right) const { return m_lean_side_hit[right]; }
    IC float lean_shift() const { return m_lean_shift; }
    // a fresh point to step out to and shoot from, if there is one
    bool peek_step(const Fvector*& position, u32& vertex) const;

public:
    virtual CCharacterPhysicsSupport* character_physics_support() { return m_pPhysics_support; }
    virtual CPHDestroyable* ph_destroyable();
    virtual CAttachmentOwner* cast_attachment_owner() { return this; }
    virtual CInventoryOwner* cast_inventory_owner() { return this; }
    virtual CEntityAlive* cast_entity_alive() { return this; }
    virtual CEntity* cast_entity() { return this; }
    virtual CGameObject* cast_game_object() { return this; }
    virtual CPhysicsShellHolder* cast_physics_shell_holder() { return this; }
    virtual CParticlesPlayer* cast_particles_player() { return this; }
    virtual Feel::Sound* dcast_FeelSound() { return this; }
    virtual CAI_Stalker* cast_stalker() { return this; }
    virtual CCustomMonster* cast_custom_monster() { return this; }
    virtual CScriptEntity* cast_script_entity() { return this; }

public:
    void init();
    virtual void Load(LPCSTR section);
    virtual void reinit();
    virtual void reload(LPCSTR section);
    virtual void LoadSounds(LPCSTR section);

    virtual BOOL net_Spawn(CSE_Abstract* DC);

    // The server re-dressed this stalker (GE_NPC_VISUAL). Rebuilds everything
    // net_Spawn derives from the model: animations, per-bone armour, IK.
    virtual void OnServerVisualChange(shared_str visual);

    // The server revived this stalker. On top of what CEntityAlive undoes, a
    // stalker carries AI state that went stale while it was dead -- see the
    // implementation, this one crashed the host 13000 times before it was found.
    virtual void OnServerRevive(float health_fraction) override;

    // Per-bone armour out of the model's userdata -- the lookup net_Spawn does,
    // pulled out so re-dressing can redo it rather than copy it.
    void reload_bone_protection();

    virtual void net_Export(CSE_Abstract* E);
    virtual void net_Destroy();
    virtual void net_Save(NET_Packet& P);
    virtual BOOL net_SaveRelevant();
    virtual void net_Relcase(CObject* O);

    // save/load server serialization
    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);

    virtual void UpdateCL();
    virtual void shedule_Update(u32 dt);
    virtual float shedule_Scale(); // ai_think_lod (ai_stalker.cpp)
    void shedule_Update_body(u32 dt); // shedule_Update minus the [sched prof] wrapper
    virtual void Think();
    virtual void SelectAnimation(const Fvector& _view, const Fvector& _move, float speed);
    virtual BOOL UsedAI_Locations();

    virtual void g_WeaponBones(int& L, int& R1, int& R2);
    virtual void g_fireParams(CHudItem* pHudItem, Fvector& P, Fvector& D, const bool for_cursor = false) override;
    virtual void HitSignal(float P, Fvector& vLocalDir, CObject* who, s16 element);
    virtual void Die(CObject* who);

    virtual void OnEvent(NET_Packet& P, u16 type);
    virtual void feel_touch_new(CObject* O);

    virtual void renderable_Render(u32 context_id, IRenderable* root) override;
    virtual void Exec_Look(float dt);
    virtual void Hit(SHit* pHDS);
    virtual void PHHit(SHit& H);
    virtual BOOL feel_vision_isRelevant(CObject* who);
    virtual float Radius() const;
#ifdef DEBUG
    virtual void OnHUDDraw(CCustomHUD* hud, u32 context_id, IRenderable* root) override;
    virtual void OnRender();
#endif

    virtual bool useful(const CItemManager* manager, const CGameObject* object) const;
    virtual float evaluate(const CItemManager* manager, const CGameObject* object) const;
    virtual bool useful(const CEnemyManager* manager, const CEntityAlive* object) const;

    // PDA && Dialogs
    //	virtual void						ReceivePdaMessage					(u16 who, EPdaMsg msg, shared_str info_id);
    virtual void UpdateAvailableDialogs(CPhraseDialogManager* partner);

    // scripts
    virtual CWeapon* GetCurrentWeapon() const;
    virtual u32 GetWeaponAmmo() const;
    //	virtual CInventoryItem				*GetCurrentEquipment	() const; <- moved to InventoryOwner::GetCurrentOutfit
    virtual CInventoryItem* GetMedikit() const;
    virtual CInventoryItem* GetFood() const;
    virtual bool bfAssignMovement(CScriptEntityAction* tpEntityAction);
    virtual bool bfAssignWatch(CScriptEntityAction* tpEntityAction);
    virtual void ResetScriptData(void* P = 0);
    virtual bool bfAssignObject(CScriptEntityAction* tpEntityAction);
    virtual bool bfAssignAnimation(CScriptEntityAction* tpEntityAction);

    // physics
    virtual u16 PHGetSyncItemsNumber() { return inherited ::PHGetSyncItemsNumber(); }
    virtual CPHSynchronize* PHGetSyncItem(u16 item) { return inherited ::PHGetSyncItem(item); }
    virtual void PHUnFreeze() { return inherited ::PHUnFreeze(); }
    virtual void PHFreeze() { return inherited ::PHFreeze(); }

    // miscellanious functions
    void DropItemSendMessage(CObject* O);
    bool bfCheckForNodeVisibility(u32 dwNodeID, bool bIfRyPick = false);
    virtual ALife::ERelationType tfGetRelationType(const CEntityAlive* tpEntityAlive) const;
    virtual const SRotation Orientation() const;
    virtual const MonsterSpace::SBoneRotation& head_orientation() const;

    // InventoryOwner stuff
    virtual bool CanPutInSlot(PIItem item, u32 slot) override;

    //////////////////////////////////////////////////////////////////////////
    // action/evaluators support functions
    //////////////////////////////////////////////////////////////////////////
public:
    virtual void OnItemTake(CInventoryItem* inventory_item);
    virtual void OnItemDrop(CInventoryItem* inventory_item);
    bool item_to_kill();
    bool item_can_kill();
    bool remember_item_to_kill();
    bool remember_ammo();
    bool ready_to_kill();
    bool ready_to_detour();
    void update_best_item_info();
    virtual float GetWeaponAccuracy() const;
    virtual bool unlimited_ammo();
    virtual void spawn_supplies();
    CAgentManager& agent_manager() const;

    virtual bool human_being() const { return (true); }

    bool undetected_anomaly();
    bool inside_anomaly();

private:
    bool m_can_kill_member;
    bool m_can_kill_enemy;
    float m_pick_distance;
    u32 m_pick_frame_id;
    collide::rq_results rq_storage;

private:
    void can_kill_entity(const Fvector& position, const Fvector& direction, float distance, collide::rq_results& rq_storage);
    void can_kill_entity_from(const Fvector& position, Fvector direction, float distance);
    void update_can_kill_info();

public:
    bool can_kill_member();
    bool can_kill_enemy();
    float pick_distance();
    IC float start_pick_distance() const;
    bool fire_make_sense();
    bool can_fire_to_enemy(const CEntityAlive* enemy);

    LPCSTR Name() override { return CInventoryOwner::Name(); }
    
    virtual BOOL feel_touch_contact(CObject* O);
    virtual BOOL feel_touch_on_contact(CObject* O);

    //флаги, какие действия совершал актер по отношению к сталкеру
    //(помог, атаковал и т.д.)
    Flags32 m_actor_relation_flags;

    // ALife
private:
    struct CTradeItem
    {
        CInventoryItem* m_item;
        ALife::_OBJECT_ID m_owner_id;
        ALife::_OBJECT_ID m_new_owner_id;

        IC CTradeItem(CInventoryItem* item, ALife::_OBJECT_ID owner_id, ALife::_OBJECT_ID new_owner_id)
        {
            m_item = item;
            m_owner_id = owner_id;
            m_new_owner_id = new_owner_id;
        }

        IC bool operator<(const CTradeItem& trade_item) const;
        IC bool operator==(u16 id) const;
    };

private:
    CGameObject* m_trader_game_object;
    CInventoryOwner* m_current_trader;
    xr_vector<CTradeItem> m_temp_items;
    u32 m_total_money;
    bool m_sell_info_actuality;

protected:
    u32 fill_items(CInventory& inventory, CGameObject* old_owner, ALife::_OBJECT_ID new_owner_id);

    IC void buy_item_virtual(CTradeItem& item);
    void attach_available_ammo(CWeapon* weapon);
    void choose_food();
    void choose_weapon(ALife::EWeaponPriorityType weapon_priority_type);
    void choose_medikit();
    // void						choose_detector					();
    void choose_equipment();

    void select_items();
    void transfer_item(CInventoryItem* item, CGameObject* old_owner, CGameObject* new_owner);

    void update_sell_info();
    bool tradable_item(CInventoryItem* inventory_item, const u16& current_owner_id);
    bool can_sell(CInventoryItem const* item);
    bool can_take(CInventoryItem const* item);

    bool non_conflicted(const CInventoryItem* item, const CWeapon* new_weapon) const;
    bool enough_ammo(const CWeapon* new_weapon) const;
    bool conflicted(const CInventoryItem* item, const CWeapon* new_weapon, bool new_wepon_enough_ammo, int new_weapon_rank) const;
    void update_conflicted(CInventoryItem* item, const CWeapon* new_weapon);
    void remove_personal_only_ammo(const CInventoryItem* item);
    void on_after_take(const CGameObject* object);
    virtual bool AllowItemToTrade(CInventoryItem const* item, EItemPlace place) const;

public:
    IC CStalkerAnimationManager& animation() const;
    IC CStalkerPlanner& brain() const;
    IC CSightManager& sight() const;

private:
    CStalkerSoundDataVisitor* m_sound_user_data_visitor;

protected:
    virtual CSound_UserDataVisitor* create_sound_visitor();
    virtual CMemoryManager* create_memory_manager();
    virtual CMovementManager* create_movement_manager();

public:
    IC CStalkerMovementManager& movement() const;
    virtual DLL_Pure* _construct();

private:
    IC bool frame_check(u32& frame);
    virtual bool natural_weapon() const { return false; }
    virtual bool natural_detector() const { return false; }
    virtual bool use_center_to_aim() const;
    void process_enemies();

private:
    bool m_group_behaviour;

public:
    IC bool group_behaviour() const;
    virtual void update_range_fov(float& new_range, float& new_fov, float start_range, float start_fov);
    void update_object_handler();
    bool zoom_state() const;
    void react_on_grenades();
    void react_on_member_death();

private:
    CWeaponShotEffector* m_weapon_shot_effector;

public:
    virtual void on_weapon_shot_start(CWeapon* weapon);
    virtual void on_weapon_shot_stop(CWeapon* weapon);
    virtual void on_weapon_hide(CWeapon* weapon);
    IC CWeaponShotEffector& weapon_shot_effector() const;
    IC Fvector weapon_shot_effector_direction(const Fvector& current) const;
#ifdef DEBUG
    virtual void UpdateCamera();
#endif
    virtual bool can_attach(const CInventoryItem* inventory_item) const;
    virtual bool use_simplified_visual() const { return (already_dead()); };
#ifdef DEBUG
    void debug_planner(const script_planner* planner);
#endif

private:
    u32 m_min_queue_size_far;
    u32 m_max_queue_size_far;
    u32 m_min_queue_interval_far;
    u32 m_max_queue_interval_far;

    u32 m_min_queue_size_medium;
    u32 m_max_queue_size_medium;
    u32 m_min_queue_interval_medium;
    u32 m_max_queue_interval_medium;

    u32 m_min_queue_size_close;
    u32 m_max_queue_size_close;
    u32 m_min_queue_interval_close;
    u32 m_max_queue_interval_close;

public:
    IC u32 min_queue_size_far() const;
    IC u32 max_queue_size_far() const;
    IC u32 min_queue_interval_far() const;
    IC u32 max_queue_interval_far() const;

    IC u32 min_queue_size_medium() const;
    IC u32 max_queue_size_medium() const;
    IC u32 min_queue_interval_medium() const;
    IC u32 max_queue_interval_medium() const;

    IC u32 min_queue_size_close() const;
    IC u32 max_queue_size_close() const;
    IC u32 min_queue_interval_close() const;
    IC u32 max_queue_interval_close() const;

public:
    using on_best_cover_changed_delegate = fastdelegate::FastDelegate<void(const CCoverPoint*, const CCoverPoint*)>;

private:
    typedef xr_vector<on_best_cover_changed_delegate> cover_delegates;

private:
    cover_delegates m_cover_delegates;
    const CCoverPoint* m_best_cover;
    float m_best_cover_value;
    bool m_best_cover_actual;
    bool m_best_cover_can_try_advance;
    const CCoverPoint* m_best_cover_advance_cover;

private:
    float best_cover_value(const Fvector& position_to_cover_from);
    const CCoverPoint* find_best_cover(const Fvector& position_to_cover_from);
    void update_best_cover_actuality(const Fvector& position_to_cover_from);
    void on_best_cover_changed(const CCoverPoint* new_cover, const CCoverPoint* old_cover);
    // ai_tactics_v2: the same choice through the position query (ai_position_query.h)
    const CCoverPoint* best_cover_v2(const Fvector& position_to_cover_from);
    void ask_cover(const Fvector& position_to_cover_from, float radius);
    void adopt_cover(const CCoverPoint* cover, const Fvector& position_to_cover_from);

public:
    void best_cover_can_try_advance();
    const CCoverPoint* best_cover(const Fvector& position_to_cover_from);

public:
    void subscribe_on_best_cover_changed(const on_best_cover_changed_delegate& delegate);
    void unsubscribe_on_best_cover_changed(const on_best_cover_changed_delegate& delegate);

public:
    virtual void on_enemy_change(const CEntityAlive* enemy);
    virtual void on_restrictions_change();
    void on_cover_blocked(const CCoverPoint* cover);
    void on_danger_location_add(const CDangerLocation& location);
    void on_danger_location_remove(const CDangerLocation& location);

public:
    void wounded(bool value);
    bool wounded(const CRestrictedObject* object) const;
    IC bool wounded() const;

    // throwing grenades
private:
    // actuality parameters
    bool m_throw_actual;
    Fvector m_computed_object_position;
    Fvector m_computed_object_direction;
    // target parameters
    Fvector m_throw_target_position;
    CObject* m_throw_ignore_object;
    // computed
    Fvector m_throw_position;
    Fvector m_throw_velocity;
    // collision prediction
    Fvector m_throw_collide_position;
    bool m_throw_enabled;

    u32 m_last_throw_time;
    u32 m_throw_time_interval;

#ifdef DEBUG
    xr_vector<Fvector> m_throw_picks;
#endif // DEBUG

public:
    IC const bool& throw_enabled();

private:
    bool m_can_throw_grenades;

public:
    IC const bool& can_throw_grenades() const;
    IC void can_throw_grenades(const bool& value);

private:
    bool throw_check_error(float low, float high, const Fvector& start, const Fvector& velocity, const Fvector& gravity);
    void check_throw_trajectory(const float& throw_time);
    void throw_target_impl(const Fvector& position, CObject* throw_ignore_object);
    void compute_throw_miss(u32 const vertex_id);

public:
    virtual bool use_default_throw_force();
    virtual float missile_throw_force();
    virtual bool use_throw_randomness();
    void throw_target(const Fvector& position, CObject* throw_ignore_object = nullptr);
    void throw_target(const Fvector& position, u32 const vertex_id, CObject* throw_ignore_object = nullptr);
    IC const Fvector& throw_target() const;
    void update_throw_params();
    void on_throw_completed();
    IC const u32& last_throw_time() const;

#ifdef DEBUG
public:
    void dbg_draw_vision();
    void dbg_draw_visibility_rays();
#endif

    //////////////////////////////////////////////////////////////////////////
    // Critical Wounds
    //////////////////////////////////////////////////////////////////////////
private:
    virtual void load_critical_wound_bones();
    virtual bool critical_wound_external_conditions_suitable();
    virtual void critical_wounded_state_start();

    void fill_bones_body_parts(LPCSTR bone_id, const ECriticalWoundType& wound_type);

public:
    typedef xr_vector<float> CRITICAL_WOUND_WEIGHTS;

private:
    CRITICAL_WOUND_WEIGHTS m_critical_wound_weights;

public:
    bool critically_wounded();
    IC const CRITICAL_WOUND_WEIGHTS& critical_wound_weights() const;

private:
    bool can_cry_enemy_is_wounded() const;
    void on_critical_wound_initiator(const CAI_Stalker* critically_wounded);
    void on_enemy_wounded_or_killed(const CAI_Stalker* wounded_or_killed);
    void notify_on_wounded_or_killed(CObject* object);
    void notify_on_wounded_or_killed();
    void remove_critical_hit();
    //////////////////////////////////////////////////////////////////////////
private:
    bool m_registered_in_combat_on_migration;

public:
    virtual void on_before_change_team();
    virtual void on_after_change_team();

private:
    bool m_sight_enabled_before_animation_controller;

public:
    virtual void create_anim_mov_ctrl(CBlend* b);
    virtual void destroy_anim_mov_ctrl();

private:
    bool m_can_select_items;

public:
    IC const u32& throw_time_interval() const;
    IC void throw_time_interval(const u32& value);

public:
    DECLARE_SCRIPT_REGISTER_FUNCTION
};
add_to_type_list(CAI_Stalker)
#undef script_type_list
#define script_type_list save_type_list(CAI_Stalker)

#include "ai_stalker_inline.h"

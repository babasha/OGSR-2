// CustomMonster.h: interface for the CCustomMonster class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "entity_alive.h"
#include "script_entity.h"
#include "../xr_3da/Feel_Vision.h"
#include "../xr_3da/Feel_Sound.h"
#include "../xr_3da/Feel_Touch.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "associative_vector.h"
#include "ai/monsters/anomaly_detector.h"

namespace MonsterSpace
{
struct SBoneRotation;
};

class CMotionDef;
class IKinematicsAnimated;
class CMemoryManager;
class CItemManager;
class CEnemyManager;
class CDangerManager;
class CMovementManager;
class CSoundPlayer;
class CAI_Stalker;
class CDangerObject;
class CCustomMonsterScript;

class CCustomMonster : public CEntityAlive, public CScriptEntity, public Feel::Vision, public Feel::Sound, public Feel::Touch
{
private:
    typedef CEntityAlive inherited;
    friend class CCustomMonsterScript;

private:
    CMemoryManager* m_memory_manager;
    CMovementManager* m_movement_manager;
    CSoundPlayer* m_sound_player;

private:
    u32 m_client_update_delta;
    u32 m_last_client_update_time;

protected:
    struct SAnimState
    {
        MotionID fwd;
        MotionID back;
        MotionID ls;
        MotionID rs;

        void Create(IKinematicsAnimated* K, LPCSTR base);
    };

private:
    xr_vector<CLASS_ID> m_killer_clsids;

public:
    // Eyes
    Fmatrix eye_matrix;
    int eye_bone;
    float eye_fov;
    float eye_range;

    float m_fCurSpeed;

    u32 eye_pp_stage;
    u32 eye_pp_timestamp;
    Fvector m_tEyeShift;
    float m_fEyeShiftYaw;
    BOOL NET_WasExtrapolating;

    Fvector tWatchDirection;

    virtual void Think() = 0;

    float m_fTimeUpdateDelta;
    u32 m_dwLastUpdateTime;
    u32 m_current_update;
    //	Fmatrix				m_tServerTransform;

    u32 m_dwCurrentTime; // time updated in UpdateCL

    // Until when the server owns this creature's position -- see server_driven().
    u32 m_server_driven_until{0};
    u32 m_server_driven_at{0}; // when the last assertion arrived
    float m_server_driven_speed{0.f}; // measured across assertions, m/s
    float m_server_driven_yaw{0.f}; // the asserted heading, for net_Export

    // ⭐The motion this creature is ACTUALLY performing, differentiated from
    // the transform the server is about to put on the wire. Not the AI's
    // declared movement_type: on this build the server drives the bodies it
    // commands, so the two disagree exactly when it matters. Sampled once per
    // frame at export time -- see net_Export_authority.
    u32 m_intent_frame{0}; // last frame sampled (several clients export in one)
    u32 m_intent_ms{0};
    Fvector m_intent_pos{};
    float m_intent_yaw{0.f};
    float m_intent_vfwd{0.f}, m_intent_vstr{0.f}; // body frame, m/s
    float m_intent_turn{0.f}; // rad/s
    // ⚠Displacement and elapsed time are accumulated and divided ONCE, per
    // window. Averaging per-sample quotients instead reported a measured
    // 1.10 m/s walk as 0.72 -- see net_Export_intent for why.
    Fvector m_intent_acc{};
    float m_intent_acc_yaw{0.f};
    u32 m_intent_acc_ms{0};
    // ...and the style bits the picture cannot measure (crouch/wounded/aim/
    // mental) with the held weapon's animation slot, cached on the same frame
    // clock: they come from PostureOf, which walks the object handler's
    // planner, and this runs once per entity PER CONNECTED CLIENT.
    u8 m_intent_style{0};
    u8 m_intent_slot{0};
    // Latched moments. Both are faster than the wire: an automatic weapon
    // fires between two snapshots, and a flinch that is not held is a flag the
    // client sees on one frame in four.
    u32 m_intent_fire_ms{0};
    u32 m_intent_flinch_ms{0};
    // ...and the same shape for the sprint tariff. Latched rather than a plain
    // bool because the drive only writes it while it is driving: a creature
    // released mid-run would otherwise keep claiming to sprint for ever.
    u32 m_intent_sprint_ms{0};

    struct net_update
    {
        u32 dwTimeStamp; // server(game) timestamp
        float o_model; // model yaw
        SRotation o_torso; // torso in world coords
        Fvector p_pos; // in world coords
        float fHealth;

        // non-exported (temporal)

        net_update()
        {
            dwTimeStamp = 0;
            o_model = 0;
            o_torso.yaw = 0;
            o_torso.pitch = 0;
            p_pos.set(0, 0, 0);
            fHealth = 0.f;
        }
        void lerp(net_update& A, net_update& B, float f);
    };
    xr_deque<net_update> NET;
    net_update NET_Last;
    //------------------------------

    virtual BOOL feel_touch_on_contact(CObject*);
    virtual BOOL feel_touch_contact(CObject*);
    // utils
    void mk_orientation(Fvector& dir, Fmatrix& mR);
    void mk_rotation(Fvector& dir, SRotation& R);

    // stream executors
    virtual void Exec_Action(float dt);
    virtual void Exec_Look(float dt);
    void Exec_Visibility();
    void eye_pp_s0();
    void eye_pp_s1();
    void eye_pp_s2();

public:
    CCustomMonster();
    virtual ~CCustomMonster();

public:
    virtual CEntityAlive* cast_entity_alive() { return this; }
    virtual CEntity* cast_entity() { return this; }

public:
    virtual DLL_Pure* _construct();
    virtual BOOL net_Spawn(CSE_Abstract* DC);
    virtual void Die(CObject* who);

    virtual void HitSignal(float P, Fvector& vLocalDir, CObject* who);
    virtual void g_WeaponBones(int& /**L/**/, int& /**R1/**/, int& /**R2/**/){};
    virtual void shedule_Update(u32 DT);
    virtual void UpdateCL();

    virtual void net_Export(CSE_Abstract*);
    // The server-authority override applied on top of whatever an export built
    // out of NET.back(). Shared, because CAI_Stalker::net_Export is a copy of
    // the base one rather than an extension of it -- see the definition.
    void net_Export_authority(class CSE_ALifeCreatureAbstract* creature);
    // ...and the presentation intent that rides along with it: measured
    // velocity, turn rate, posture, what is in the hands. See the definition.
    void net_Export_intent(class CSE_ALifeCreatureAbstract* creature);
    // ...and the clips this process's animator is playing, per bone part. Empty
    // when there is no animator here. See SNetAnimState.
    void net_Export_anim(class CSE_ALifeCreatureAbstract* creature);
    virtual void net_Relcase(CObject* O);

    virtual void SelectAnimation(const Fvector& _view, const Fvector& _move, float speed) = 0;

    virtual bool is_base_monster_with_enemy() { return false; }

    // debug

    virtual bool bfExecMovement() { return (false); };

    IC bool angle_lerp_bounds(float& a, float b, float c, float d);
    IC void vfNormalizeSafe(Fvector& Vector);

public:
    virtual float ffGetFov() const { return eye_fov; }
    virtual float ffGetRange() const { return eye_range; }
    void set_fov(float new_fov);
    void set_range(float new_range);
    //	virtual	void				feel_touch_new			(CObject	*O);
    virtual BOOL feel_visible_isRelevant(CObject* O);
    virtual Feel::Sound* dcast_FeelSound() { return this; }
    virtual void Hit(SHit* pHDS);

    virtual void OnEvent(NET_Packet& P, u16 type);
    virtual void net_Destroy();
    virtual BOOL UsedAI_Locations();
    ///////////////////////////////////////////////////////////////////////
    virtual u16 PHGetSyncItemsNumber() { return inherited ::PHGetSyncItemsNumber(); }
    virtual CPHSynchronize* PHGetSyncItem(u16 item) { return inherited ::PHGetSyncItem(item); }
    virtual void PHUnFreeze() { return inherited ::PHUnFreeze(); }
    virtual void PHFreeze() { return inherited ::PHFreeze(); }
    ///////////////////////////////////////////////////////////////////////
public:
    virtual void Load(LPCSTR section);
    virtual void reinit();
    virtual void reload(LPCSTR section);
    virtual const SRotation Orientation() const;
    virtual float get_custom_pitch_speed(float def_speed) { return def_speed; }
    virtual bool human_being() const { return (false); }
    virtual void PitchCorrection();

    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);
    virtual BOOL net_SaveRelevant() { return inherited::net_SaveRelevant(); }

    virtual const MonsterSpace::SBoneRotation& head_orientation() const;

    virtual void UpdatePositionAnimation();
    virtual CPhysicsShellHolder* cast_physics_shell_holder() { return this; }
    virtual CParticlesPlayer* cast_particles_player() { return this; }
    virtual CCustomMonster* cast_custom_monster() { return this; }
    virtual CScriptEntity* cast_script_entity() { return this; }

    void load_killer_clsids(LPCSTR section);
    bool is_special_killer(CObject* obj);

    IC CMemoryManager& memory() const;
    virtual float feel_vision_mtl_transp(CObject* O, u32 element);
    virtual void feel_sound_new(CObject* who, int type, CSound_UserDataPtr user_data, const Fvector& Position, float power);
    virtual float feel_sound_range(CObject* who, int type);

    virtual bool useful(const CItemManager* manager, const CGameObject* object) const;
    virtual float evaluate(const CItemManager* manager, const CGameObject* object) const;
    virtual bool useful(const CEnemyManager* manager, const CEntityAlive* object) const;
    virtual float evaluate(const CEnemyManager* manager, const CEntityAlive* object) const;
    virtual bool useful(const CDangerManager* manager, const CDangerObject& object) const;
    virtual float evaluate(const CDangerManager* manager, const CDangerObject& object) const;

protected:
    float m_panic_threshold;

public:
    IC float panic_threshold() const;

private:
    CSound_UserDataVisitor* m_sound_user_data_visitor;

protected:
    virtual CSound_UserDataVisitor* create_sound_visitor();
    virtual CMemoryManager* create_memory_manager();
    virtual CMovementManager* create_movement_manager();

public:
    IC CMovementManager& movement() const;
    IC CSoundPlayer& sound() const;
    IC CSound_UserDataVisitor* sound_user_data_visitor() const;

protected:
    float m_far_plane_factor;
    float m_fog_density_factor;

public:
    virtual void update_range_fov(float& new_range, float& new_fov, float start_range, float start_fov);

public:
    void update_sound_player();
    virtual void on_restrictions_change();
    virtual LPCSTR visual_name(CSE_Abstract* server_entity);

private:
    bool m_already_dead;

public:
    IC const bool& already_dead() const { return (m_already_dead); };
    virtual bool use_simplified_visual() const { return (already_dead()); };
    virtual void on_enemy_change(const CEntityAlive* enemy);
    virtual CVisualMemoryManager* visual_memory() const;

public:
    IC float client_update_fdelta() const;
    IC const u32& client_update_delta() const;
    IC const u32& last_client_update_time() const;

    //////////////////////////////////////////////////////////////////////////
    // Critical Wounds
    //////////////////////////////////////////////////////////////////////////
public:
    typedef u32 CriticalWoundType;

private:
    typedef associative_vector<u16, CriticalWoundType> BODY_PART;

protected:
    u32 m_last_hit_time;
    float m_critical_wound_threshold;
    float m_critical_wound_decrease_quant;
    float m_critical_wound_accumulator;
    CriticalWoundType m_critical_wound_type;
    BODY_PART m_bones_body_parts;

protected:
    virtual void load_critical_wound_bones() {}

    virtual bool critical_wound_external_conditions_suitable() { return true; }
    virtual void critical_wounded_state_start() {}

    bool update_critical_wounded(const u16& bone_id, const float& power);

public:
    IC void critical_wounded_state_stop();

public:
    IC bool critically_wounded();
    IC const u32& critical_wound_type() const;

    //////////////////////////////////////////////////////////////////////////
private:
    bool m_invulnerable;

public:
    IC void invulnerable(const bool& invulnerable);
    IC bool invulnerable() const;

protected:
    bool m_update_rotation_on_frame;

private:
    bool m_movement_enabled_before_animation_controller;

public:
    virtual void create_anim_mov_ctrl(CBlend* b);
    virtual void destroy_anim_mov_ctrl();

    bool m_visible_for_zones;
    virtual bool IsVisibleForZones() { return m_visible_for_zones; }
    void ForceTransform(const Fmatrix& m) override;
    void net_ServerDriveTo(const Fvector& pos, float yaw) override;

    // ⚠While this is true the creature's position belongs to the server and
    // its own movement pipeline must not write it. Specifically CMovementManager
    // ::move_along_path, which on the branch a driven creature always takes
    // reads the position back off the physics capsule and overwrites whatever
    // the server just asserted -- five millimetres of every eight, so the
    // puppet still moves, just at a third of the speed it was given, and every
    // counter on the server still reads the full speed.
    //
    // It EXPIRES rather than being cleared. A server that stops driving simply
    // stops asserting, and a second later the creature is its own again: no
    // release call to forget, and no way for a crashed or paused host module to
    // leave a creature frozen out of its own body forever.
    bool server_driven() const { return Device.dwTimeGlobal < m_server_driven_until; }

    // Thin-client driven puppet is RENDER-ONLY: its local update stack reads
    // ghost state and fights the interpolator (see the definition for the
    // full list of offenders found in play).
    bool net_render_only_puppet() const;

    // How fast the server is actually moving it, measured from the assertions
    // themselves rather than declared. The animation selector reads this, so a
    // driven creature walks or runs to match how fast it is being carried
    // instead of sliding along in an idle pose.
    float server_driven_speed() const { return m_server_driven_speed; }
    void net_MoveTo(const Fmatrix& m) override;

    // This creature just fired / just took a hit. Latched into the intent
    // block for the next few snapshots, because both events are shorter than
    // the interval between them.
    void net_note_fired() { m_intent_fire_ms = Device.dwTimeGlobal; }
    void net_note_flinch() { m_intent_flinch_ms = Device.dwTimeGlobal; }
    // The drive says which tariff it is carrying this body at, so the client
    // can pair the run cycle with the same one. See SNetCreatureIntent::sSprint.
    void net_note_sprint() { m_intent_sprint_ms = Device.dwTimeGlobal; }

    // -----------------------------------------------------------------------------
    // Anomaly Detector
private:
    CAnomalyDetector* m_anomaly_detector;

public:
    CAnomalyDetector& anomaly_detector() { return (*m_anomaly_detector); }
    // -----------------------------------------------------------------------------
};

#include "CustomMonster_inline.h"

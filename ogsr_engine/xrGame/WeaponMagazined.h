#pragma once

#include "Weapon.h"
#include "HudSound.h"
#include "ai_sounds.h"

class ENGINE_API CMotionDef;

//размер очереди считается бесконечность
//заканчиваем стрельбу, только, если кончились патроны
#define WEAPON_ININITE_QUEUE -1

class CBinocularsVision;

class CWeaponMagazined : public CWeapon
{
    friend class CWeaponScript;

private:
    typedef CWeapon inherited;

protected:
    // Media :: sounds
    HUD_SOUND sndShow;
    HUD_SOUND sndHide;
    HUD_SOUND sndShot;
    HUD_SOUND sndEmptyClick;
    HUD_SOUND sndReload, sndReloadPartly, sndReloadJammed, sndReloadJammedLast;
    HUD_SOUND sndFireModes;
    HUD_SOUND sndZoomChange;
    HUD_SOUND sndTactItemOn;
    HUD_SOUND sndAimStart, sndAimEnd;
    HUD_SOUND sndItemOn;
    //звук текущего выстрела
    HUD_SOUND* m_pSndShotCurrent;

    virtual void StopHUDSounds();

    //дополнительная информация о глушителе
    LPCSTR m_sSilencerFlameParticles;
    LPCSTR m_sSilencerSmokeParticles;
    HUD_SOUND sndSilencerShot;

    ESoundTypes m_eSoundShow;
    ESoundTypes m_eSoundHide;
    ESoundTypes m_eSoundShot;
    ESoundTypes m_eSoundEmptyClick;
    ESoundTypes m_eSoundReload;

    // General
    //кадр момента пересчета UpdateSounds
    u32 dwUpdateSounds_Frame;

protected:
    virtual void OnMagazineEmpty();

    virtual void switch2_Idle();
    virtual void switch2_Fire();
    virtual void switch2_Fire2() {}
    void switch2_Empty(const bool empty_click_anim_play);
    virtual void switch2_Reload();
    virtual void switch2_Hiding();
    virtual void switch2_Hidden();
    virtual void switch2_Showing();

    virtual void OnShot();

    virtual void OnEmptyClick();

    virtual void OnAnimationEnd(u32 state);
    virtual void OnStateSwitch(u32 S, u32 oldState);

    virtual void UpdateSounds();

    bool TryReload();

protected:
    virtual void ReloadMagazine();
    void ApplySilencerKoeffs();

    virtual void state_Fire(float dt);

public:
    CWeaponMagazined(LPCSTR name = "AK74", ESoundTypes eSoundType = SOUND_TYPE_WEAPON_SUBMACHINEGUN);
    virtual ~CWeaponMagazined();

    virtual void Load(LPCSTR section);
    virtual CWeaponMagazined* cast_weapon_magazined() { return this; }

    // A shot this copy did not fire: the server's creature pulled the trigger
    // and GE_NPC_SHOT says where and which way. Presentation only -- see the
    // implementation for why this is not OnShot().
    void OnNetworkShot(const Fvector& P, const Fvector& D);

    virtual void SetDefaults();
    virtual void FireStart();
    virtual void FireEnd();
    virtual void Reload();
    virtual void Misfire() override;
    virtual void DeviceSwitch() override;

protected:
    virtual void DeviceUpdate() override;

public:
    virtual void UpdateCL();
    virtual BOOL net_Spawn(CSE_Abstract* DC);
    virtual void net_Destroy();
    virtual void net_Export(CSE_Abstract* E);

    virtual void OnH_A_Chield();

    virtual bool Attach(PIItem pIItem, bool b_send_event);
    virtual bool Detach(const char* item_section_name, bool b_spawn_item);
    virtual bool CanAttach(PIItem pIItem);
    virtual bool CanDetach(const char* item_section_name);

    virtual void InitAddons();
    virtual void InitZoomParams(LPCSTR section, bool useTexture);

    virtual bool Action(s32 cmd, u32 flags);
    virtual void UnloadMagazine(bool spawn_ammo = true);

    virtual void GetBriefInfo(xr_string& str_name, xr_string& icon_sect_name, xr_string& str_count);

    //////////////////////////////////////////////
    // для стрельбы очередями или одиночными
    //////////////////////////////////////////////
public:
    virtual bool SwitchMode();
    virtual bool SingleShotMode() { return 1 == m_iQueueSize; }
    virtual void SetQueueSize(int size);
    IC int GetQueueSize() const { return m_iQueueSize; };
    virtual bool StopedAfterQueueFired() { return m_bStopedAfterQueueFired; }
    virtual void StopedAfterQueueFired(bool value) { m_bStopedAfterQueueFired = value; }

protected:
    //максимальный размер очереди, которой можно стрельнуть
    int m_iQueueSize;
    //количество реально выстреляных патронов
    int m_iShotNum;
    //  [7/20/2005]
    //после какого патрона, при непрерывной стрельбе, начинается отдача (сделано из-зи Абакана)
    int m_iShootEffectorStart;
    Fvector m_vStartPos, m_vStartDir;
    //  [7/20/2005]
    //флаг того, что мы остановились после того как выстреляли
    //ровно столько патронов, сколько было задано в m_iQueueSize
    // ⚠Was never initialised: an NPC's weapon came in with garbage (8, 39, 221 --
    // 📏08-10-2026 Switch v231 [oh retry]), `!` on such a bool is still neither
    // true nor false, so the planner's QueueWait1 matched no operator and the
    // weapon planner ran its whole search for "no plan" (18-28 ms per NPC in a fight).
    bool m_bStopedAfterQueueFired{};
    //флаг того, что хотя бы один выстрел мы должны сделать
    //(даже если очень быстро нажали на курок и вызвалось FireEnd)
    bool m_bFireSingleShot;
    //режимы стрельбы
    bool m_bHasDifferentFireModes;
    xr_vector<int> m_aFireModes;
    int m_iCurFireMode;
    string16 m_sCurFireMode;
    int m_iPrefferedFireMode;
    u32 m_fire_zoomout_time = u32(-1);

    //переменная блокирует использование
    //только разных типов патронов
    bool m_bLockType;

    const char* m_str_count_tmpl;

    bool m_bFlameParticlesHideInZoom{};

protected:
    // режим выделения рамкой противников
    bool m_bVision;
    CBinocularsVision* m_binoc_vision;

    //////////////////////////////////////////////
    // режим приближения
    //////////////////////////////////////////////
public:
    virtual void OnZoomIn();
    virtual void OnZoomOut();
    virtual void OnZoomChanged();
    virtual void OnNextFireMode(bool = false);
    virtual void OnPrevFireMode(bool = false);
    virtual bool HasFireModes() { return m_bHasDifferentFireModes; };
    virtual int GetCurrentFireMode() { return m_bHasDifferentFireModes ? m_aFireModes[m_iCurFireMode] : 1; };
    virtual LPCSTR GetCurrentFireModeStr() { return m_sCurFireMode; };

    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);

    // dbg_wpn: print everything a reload depends on, from INSIDE the class.
    //
    // ⭐This exists because "the reload animation plays and nothing loads" has
    // survived three wrong diagnoses, each of them a guess about which engine
    // subsystem was to blame. A reload reads exactly six things, none of which
    // any log prints today, and one of them is the whole answer: the round
    // count, the size of the cartridge list behind it (they can disagree -- the
    // count is on the wire, the list is not), the ammo type it is looking for,
    // whether the inventory hands back a box for that type, how full the box
    // is, and whether the state machine ever reached eReload at all.
    void DbgDump() const;

protected:
    virtual bool AllowFireWhileWorking() { return false; }

    //виртуальные функции для проигрывания анимации HUD
    virtual void PlayAnimShow();
    virtual void PlayAnimHide();
    virtual void PlayAnimReload();
    virtual void PlayAnimIdle();

    bool LaserSwitch{}, TorchSwitch{}, HeadLampSwitch{}, NightVisionSwitch{};
    bool CartridgeInTheChamberEnabled{};
    u32 CartridgeInTheChamber{};

private:
    string128 guns_aim_anm;

protected:
    virtual const char* GetAnimAimName();

    virtual void PlayAnimAim();
    virtual void PlayAnimShoot();
    virtual void PlayAnimFakeShoot();
    virtual void PlayAnimDeviceSwitch() override;
    virtual void PlayAnimCheckMisfire();
    virtual void PlayReloadSound();

    virtual int ShotsFired() { return m_iShotNum; }
    virtual float GetWeaponDeterioration();

    virtual void OnDrawUI();
    virtual void net_Relcase(CObject* object);

    bool ScopeRespawn(PIItem);

    virtual void OnMotionMark(u32 state, const motion_marks& M) override;
    int CheckAmmoBeforeReload(u32& v_ammoType);

    bool ShouldPlayFlameParticles();
};

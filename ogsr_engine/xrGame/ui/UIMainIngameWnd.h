// UIMainIngameWnd.h:  окошки-информация в игре
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "UIProgressBar.h"

#include "../alife_space.h"

#include "UICarPanel.h"
#include "UIMotionIcon.h"
#include "../HudSound.h"
#include "../script_export_space.h"
#include "../ui_rml.h"

struct GAME_NEWS_DATA;

class CLAItem;
class CUIZoneMap;
class CUIArtefactPanel;
class CUIScrollView;
class CActor;
class CWeapon;
class CMissile;
class CInventoryItem;

class CUIMainIngameWnd : public CUIWindow
{
public:
    CUIMainIngameWnd();
    virtual ~CUIMainIngameWnd();

    virtual void Init();
    virtual void Draw();
    virtual void Update();

    bool OnKeyboardPress(int dik);

protected:
    CUIStatic UIStaticHealth;
    CUIStatic UIStaticArmor;
    CUIStatic UIStaticQuickHelp;
    CUIProgressBar UIHealthBar;
    CUIProgressBar UIArmorBar;
    CUICarPanel UICarPanel;
    CUIMotionIcon UIMotionIcon;
    CUIZoneMap* UIZoneMap;

    //иконка, показывающая количество активных PDA
    CUIStatic UIPdaOnline;

    //изображение оружия
    CUIStatic UIWeaponBack;
    CUIStatic UIWeaponSignAmmo;
    CUIStatic UIWeaponIcon;

    Frect UIWeaponIcon_rect;

public:
    CUIZoneMap* GetUIZoneMap() { return UIZoneMap; }

protected:
    // 5 статиков для отображения иконок:
    // - сломанного оружия
    // - радиации
    // - ранения
    // - голода
    // - усталости
    CUIStatic UIWeaponJammedIcon;
    CUIStatic UIRadiaitionIcon;
    CUIStatic UIWoundIcon;
    CUIStatic UIStarvationIcon;
    CUIStatic UIPsyHealthIcon;
    CUIStatic UIInvincibleIcon;
    CUIStatic UIThirstIcon;
    //	CUIStatic			UISleepIcon;
    //	CUIStatic			UIArtefactIcon;

    CUIScrollView* m_UIIcons{};

public:
    CUIArtefactPanel* m_artefactPanel{};

public:
    // Енумы соответсвующие предупреждающим иконкам
    enum EWarningIcons
    {
        ewiAll = 0,
        ewiWeaponJammed,
        ewiRadiation,
        ewiWound,
        ewiStarvation,
        ewiPsyHealth,
        ewiInvincible,
        ewiThirst,
        //		ewiSleep,
        //		ewiArtefact,
    };

    // Задаем цвет соответствующей иконке
    void SetWarningIconColor(EWarningIcons icon, const u32 cl);
    void TurnOffWarningIcon(EWarningIcons icon);

    // Пороги изменения цвета индикаторов, загружаемые из system.ltx
    typedef xr_map<EWarningIcons, xr_vector<float>> Thresholds;
    typedef Thresholds::iterator Thresholds_it;
    Thresholds m_Thresholds;

    // Енум перечисления возможных мигающих иконок
    enum EFlashingIcons
    {
        efiPdaTask = 0,
        efiMail
    };

    void SetFlashIconState_(EFlashingIcons type, bool enable);

    void AnimateContacts(bool b_snd);
    HUD_SOUND m_contactSnd;

    void ReceiveNews(GAME_NEWS_DATA* news);

protected:
    void SetWarningIconColor(CUIStatic* s, const u32 cl);
    void InitFlashingIcons(CUIXml* node);
    void DestroyFlashingIcons();
    void UpdateActiveItemInfo();

    void SetAmmoIcon(const shared_str& seсt_name);

    // ⭐The RmlUi HUD (ui_rml_hud.cpp) draws what this window computes. These hold
    // the parts of that snapshot that are produced in one place and consumed in
    // another (UpdateActiveItemInfo / RenderQuickInfos -> PushHud).
    void PushHud();
    bool m_hud_wpn_on{};
    xr_string m_hud_wpn_name;
    xr_string m_hud_ammo_text;
    xr_string m_hud_ammo_sect;
    xr_string m_hud_ammo_file;
    float m_hud_ammo_gw{1.f};
    bool m_hud_hint_on{};
    xr_string m_hud_hint;
    float m_hud_heading{};
    // The pick-up picture as UpdatePickUpItem composed it (UIRml::HudState::pickup*).
    u32 m_hud_pickup_serial{};
    int m_hud_pickup_parts{};
    UIRml::HudState::IconPart m_hud_pickup[UIRml::HudState::PICKUP_PARTS];

    // first - иконка, second - анимация
    DEF_MAP(FlashingIcons, EFlashingIcons, CUIStatic*);
    FlashingIcons m_FlashingIcons;

    //для текущего активного актера и оружия
    CActor* m_pActor;
    CWeapon* m_pWeapon;
    CMissile* m_pGrenade;
    CInventoryItem* m_pItem;

    // Отображение подсказок при наведении прицела на объект
    void RenderQuickInfos();

public:
    CUICarPanel& CarPanel() { return UICarPanel; };
    CUIMotionIcon& MotionIcon() { return UIMotionIcon; }
    void OnConnected();
    void reset_ui();

protected:
    CInventoryItem* m_pPickUpItem;
    CUIStatic UIPickUpItemIcon;

    float m_iPickUpItemIconX{};
    float m_iPickUpItemIconY{};
    float m_iPickUpItemIconWidth{};
    float m_iPickUpItemIconHeight{};

    void UpdatePickUpItem();

public:
    void SetPickUpItem(CInventoryItem* PickUpItem);

    DECLARE_SCRIPT_REGISTER_FUNCTION
};

add_to_type_list(CUIMainIngameWnd)
#undef script_type_list
#define script_type_list save_type_list(CUIMainIngameWnd)

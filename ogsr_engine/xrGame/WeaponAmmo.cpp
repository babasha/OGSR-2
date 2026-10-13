#include "StdAfx.h"
#include "WeaponAmmo.h"
#include "PhysicsShell.h"
#include "xrServer_Objects_ALife_Items.h"
#include "Actor_Flags.h"
#include "Inventory.h"
#include "Weapon.h"
#include "Level_Bullet_Manager.h"
#include "ai_space.h"
#include "../xr_3da/GameMtlLib.h"
#include "Level.h"
#include "string_table.h"

#define BULLET_MANAGER_SECTION "bullet_manager"

CCartridge::CCartridge()
{
    m_flags.assign(cfTracer | cfRicochet);
    m_ammoSect = NULL;
    m_kDist = m_kDisp = m_kHit = m_kImpulse = m_kPierce = 1.f;
    m_kAP = 0.0f;
    m_kAirRes = 0.0f;
    m_buckShot = 1;
    m_impair = 1.f;
    m_kSpeed = 1.f;

    bullet_material_idx = u16(-1);
}

// ---------------------------------------------------------------------
// Optional balance overlay: $game_config$\ammo_overrides.ltx
//
// Ammunition parameters live inside the packed content, and an .ltx can only
// be overridden as a whole file -- so retuning one number would mean shipping
// a copy of somebody else's config and keeping it in step with theirs. This
// reads a small side file instead: a section per round, carrying only the keys
// that differ. A rebalance is then a one-line diff that travels with the
// server, which matters because these numbers are meant to be tuned from
// telemetry rather than guessed once.
//
// Absent file means stock behaviour, so nothing here changes an install that
// does not opt in.
// ---------------------------------------------------------------------
namespace
{
CInifile* ammo_overlay_ini()
{
    static CInifile* s_ini = nullptr;
    static bool s_looked = false;
    if (s_looked)
        return s_ini;

    s_looked = true;
    string_path fname;
    FS.update_path(fname, "$game_config$", "ammo_overrides.ltx");
    if (FS.exist(fname))
    {
        s_ini = xr_new<CInifile>(fname, TRUE);
        Msg("* [ammo] balance overlay loaded from '%s'", fname);
    }
    return s_ini;
}

} // namespace

// Shared with anything that needs to report what a round is really worth --
// reading pSettings alone would miss the overlay and quietly describe the
// stock content instead of what the game is running.
float ammo_tuned(LPCSTR section, LPCSTR key, float from_content)
{
    CInifile* ini = ammo_overlay_ini();
    if (!ini || !ini->section_exist(section))
        return from_content;
    return READ_IF_EXISTS(ini, r_float, section, key, from_content);
}

void CCartridge::Load(LPCSTR section, u8 LocalAmmoType)
{
    m_ammoSect = section;
    m_LocalAmmoType = LocalAmmoType;

    m_kDist = pSettings->r_float(section, "k_dist");
    m_kDisp = pSettings->r_float(section, "k_disp");
    m_kHit = pSettings->r_float(section, "k_hit");
    m_kImpulse = pSettings->r_float(section, "k_impulse");
    m_kPierce = pSettings->r_float(section, "k_pierce");
    m_kAP = READ_IF_EXISTS(pSettings, r_float, section, "k_ap", 0.0f);
    m_kSpeed = READ_IF_EXISTS(pSettings, r_float, section, "k_bullet_speed", 1.0f);

    // Overlay last, so it wins over content for the numbers it names.
    m_kHit = ammo_tuned(section, "k_hit", m_kHit);
    m_kAP = ammo_tuned(section, "k_ap", m_kAP);
    m_kPierce = ammo_tuned(section, "k_pierce", m_kPierce);
    m_kDist = ammo_tuned(section, "k_dist", m_kDist);

    m_u8ColorID = READ_IF_EXISTS(pSettings, r_u8, section, "tracer_color_ID", 0);

    if (pSettings->line_exist(section, "k_air_resistance"))
        m_kAirRes = pSettings->r_float(section, "k_air_resistance");
    else
        m_kAirRes = pSettings->r_float(BULLET_MANAGER_SECTION, "air_resistance_k");

    m_flags.set(cfTracer, pSettings->r_bool(section, "tracer"));
    m_buckShot = pSettings->r_s32(section, "buck_shot");
    m_impair = pSettings->r_float(section, "impair");
    fWallmarkSize = pSettings->r_float(section, "wm_size");

    m_flags.set(cfCanBeUnlimited, TRUE);

    bool allow_ricochet = READ_IF_EXISTS(pSettings, r_bool, BULLET_MANAGER_SECTION, "allow_ricochet", true);
    m_flags.set(cfRicochet, READ_IF_EXISTS(pSettings, r_bool, section, "allow_ricochet", allow_ricochet));

    if (pSettings->line_exist(section, "can_be_unlimited"))
        m_flags.set(cfCanBeUnlimited, pSettings->r_bool(section, "can_be_unlimited"));

    if (pSettings->line_exist(section, "explosive"))
        m_flags.set(cfExplosive, pSettings->r_bool(section, "explosive"));

    if (pSettings->line_exist(section, "explode_particles"))
    {
        LPCSTR explode_particles = pSettings->r_string(section, "explode_particles");
        int cnt = _GetItemCount(explode_particles);
        xr_string tmp;
        for (int k = 0; k < cnt; ++k)
            m_ExplodeParticles.push_back(_GetItem(explode_particles, k, tmp));
    }

    bullet_material_idx = GMLib.GetMaterialIdx(WEAPON_MATERIAL_NAME);
    VERIFY(u16(-1) != bullet_material_idx);
    VERIFY(fWallmarkSize > 0);

    m_InvShortName = CStringTable().translate(pSettings->r_string(section, "inv_name_short"));
}

float CCartridge::Weight() const
{
    auto s = m_ammoSect.c_str();
    float res = 0;
    if (s)
    {
        float box = pSettings->r_float(s, "box_size");
        if (box > 0)
        {
            float w = pSettings->r_float(s, "inv_weight");
            res = w / box;
        }
    }
    return res;
}

CWeaponAmmo::CWeaponAmmo(void)
{
    m_weight = .2f;
    m_flags.set(Fbelt, TRUE);
}

CWeaponAmmo::~CWeaponAmmo(void) {}

void CWeaponAmmo::Load(LPCSTR section)
{
    inherited::Load(section);

    m_kDist = pSettings->r_float(section, "k_dist");
    m_kDisp = pSettings->r_float(section, "k_disp");
    m_kHit = pSettings->r_float(section, "k_hit");
    m_kImpulse = pSettings->r_float(section, "k_impulse");
    m_kPierce = pSettings->r_float(section, "k_pierce");
    m_kAP = READ_IF_EXISTS(pSettings, r_float, section, "k_ap", 0.0f);
    m_kSpeed = READ_IF_EXISTS(pSettings, r_float, section, "k_bullet_speed", 1.0f);

    // Overlay last, so it wins over content for the numbers it names.
    m_kHit = ammo_tuned(section, "k_hit", m_kHit);
    m_kAP = ammo_tuned(section, "k_ap", m_kAP);
    m_kPierce = ammo_tuned(section, "k_pierce", m_kPierce);
    m_kDist = ammo_tuned(section, "k_dist", m_kDist);

    m_u8ColorID = READ_IF_EXISTS(pSettings, r_u8, section, "tracer_color_ID", 0);

    if (pSettings->line_exist(section, "k_air_resistance"))
        m_kAirRes = pSettings->r_float(section, "k_air_resistance");
    else
        m_kAirRes = pSettings->r_float(BULLET_MANAGER_SECTION, "air_resistance_k");

    m_tracer = !!pSettings->r_bool(section, "tracer");
    m_buckShot = pSettings->r_s32(section, "buck_shot");
    m_impair = pSettings->r_float(section, "impair");
    fWallmarkSize = pSettings->r_float(section, "wm_size");

    R_ASSERT(fWallmarkSize > 0);

    m_boxSize = (u16)pSettings->r_s32(section, "box_size");
    m_boxCurr = m_boxSize;
}

BOOL CWeaponAmmo::net_Spawn(CSE_Abstract* DC)
{
    BOOL bResult = inherited::net_Spawn(DC);
    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeItemAmmo* l_pW = smart_cast<CSE_ALifeItemAmmo*>(e);
    m_boxCurr = l_pW->a_elapsed;

    if (m_boxCurr > m_boxSize)
        l_pW->a_elapsed = m_boxCurr = m_boxSize;

    return bResult;
}

void CWeaponAmmo::net_Destroy() { inherited::net_Destroy(); }

void CWeaponAmmo::OnH_B_Chield() { inherited::OnH_B_Chield(); }

void CWeaponAmmo::OnH_B_Independent(bool just_before_destroy)
{
    if (!Useful())
    {
        if (Local())
        {
            DestroyObject();
        }
        m_ready_to_destroy = true;
    }
    inherited::OnH_B_Independent(just_before_destroy);
}

bool CWeaponAmmo::Useful() const
{
    // Если IItem еще не полностью использованый, вернуть true
    return !!m_boxCurr;
}
/*
s32 CWeaponAmmo::Sort(PIItem pIItem)
{
    // Если нужно разместить IItem после this - вернуть 1, если
    // перед - -1. Если пофиг то 0.
    CWeaponAmmo *l_pA = smart_cast<CWeaponAmmo*>(pIItem);
    if(!l_pA) return 0;
    if(xr_strcmp(cNameSect(), l_pA->cNameSect())) return 0;
    if(m_boxCurr <= l_pA->m_boxCurr) return 1;
    else return -1;
}
*/
bool CWeaponAmmo::Get(CCartridge& cartridge)
{
    if (!m_boxCurr)
        return false;
    cartridge.m_ammoSect = cNameSect();
    cartridge.m_kDist = m_kDist;
    cartridge.m_kDisp = m_kDisp;
    cartridge.m_kHit = m_kHit;
    cartridge.m_kImpulse = m_kImpulse;
    cartridge.m_kPierce = m_kPierce;
    cartridge.m_kAP = m_kAP;
    cartridge.m_kAirRes = m_kAirRes;
    cartridge.m_u8ColorID = m_u8ColorID;
    cartridge.m_flags.set(CCartridge::cfTracer, m_tracer);
    cartridge.m_buckShot = m_buckShot;
    cartridge.m_impair = m_impair;
    cartridge.fWallmarkSize = fWallmarkSize;
    cartridge.bullet_material_idx = GMLib.GetMaterialIdx(WEAPON_MATERIAL_NAME);
    cartridge.m_InvShortName = NameShort();
    --m_boxCurr;
    if (m_pCurrentInventory)
        m_pCurrentInventory->InvalidateState();
    return true;
}

void CWeaponAmmo::renderable_Render(u32 context_id, IRenderable* root)
{
    if (!m_ready_to_destroy)
        inherited::renderable_Render(context_id, root);
}

void CWeaponAmmo::UpdateCL()
{
    VERIFY2(_valid(renderable.xform), *cName());
    inherited::UpdateCL();
    VERIFY2(_valid(renderable.xform), *cName());
}

void CWeaponAmmo::net_Export(CSE_Abstract* E)
{
    inherited::net_Export(E);
    CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(E);
    ammo->a_elapsed = m_boxCurr;
}

CInventoryItem* CWeaponAmmo::can_make_killing(const CInventory* inventory) const
{
    VERIFY(inventory);

    TIItemContainer::const_iterator I = inventory->m_all.begin();
    TIItemContainer::const_iterator E = inventory->m_all.end();
    for (; I != E; ++I)
    {
        CWeapon* weapon = smart_cast<CWeapon*>(*I);
        if (!weapon)
            continue;
        xr_vector<shared_str>::const_iterator i = std::find(weapon->m_ammoTypes.begin(), weapon->m_ammoTypes.end(), cNameSect());
        if (i != weapon->m_ammoTypes.end())
            return (weapon);
    }

    return (0);
}

float CWeaponAmmo::Weight() const
{
    if (m_boxSize > 0)
    {
        float res = inherited::Weight();
        res *= (float)m_boxCurr / (float)m_boxSize;
        return res;
    }
    return 0;
}

u32 CWeaponAmmo::Cost() const
{
    float res = (float)m_cost;
    res *= (float)m_boxCurr / (float)m_boxSize;
    // return (u32)roundf(res); // VC18 only
    return (u32)ceil(res + 0.5);
}

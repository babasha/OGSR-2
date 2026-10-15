#include "StdAfx.h"
#include "sv_damage_model.h"

#include "sv_world_lookup.h"
#include "xrServer_Objects.h"
#include "xrServer_Objects_ALife_Items.h"
#include "WeaponAmmo.h"

// The gate these ceilings feed, so a report can say what will be done with
// them. Defined in game_sv_base.cpp. ⚠At file scope on purpose: an `extern`
// written inside a function that lives in namespace SVDamage declares
// SVDamage::g_sv_hit_validate, which is a different symbol and links to
// nothing.
extern int g_sv_hit_validate;
extern float g_sv_hit_slack;

// Bone damage factor assumed when the victim's damage section cannot be read.
// A limb hit in stock content sits around a third of a torso hit, so this is
// deliberately below that.
float g_sv_health_bone_floor = 0.25f;

namespace
{
xr_vector<SVDamage::Profile> s_profiles;

// The ini key CHitImmunity::LoadImmunities reads for a hit type.
//
// Almost always "<token>_immunity", but eHitTypePhysicStrike is the exception:
// its token is "physic_strike" while the loader asks for
// "physic_strike_wound_immunity". Deriving the key from the token alone -- as
// this file did -- silently missed that one and left its immunity at the 1.0
// default, which is the UNSAFE direction: an immunity below 1 that the floor
// does not apply makes the floor bigger than the damage really was.
// Items whose placement the server has actually been told about. Only
// artefacts get in here, so this stays a handful of entries rather than one
// per object in the world.
struct ItemPlace
{
    u16 item{0xffff};
    SVDamage::Place place{SVDamage::Place::Belt};
};
xr_vector<ItemPlace> s_places;

void immunity_key(int hit_type, string128& out)
{
    if (hit_type == ALife::eHitTypePhysicStrike)
    {
        xr_strcpy(out, "physic_strike_wound_immunity");
        return;
    }
    strconcat(sizeof(out), out, ALife::g_cafHitType2String(ALife::EHitType(hit_type)), "_immunity");
}

// The hit type a section's line names. `fallback` is what the ENGINE assumes
// when the line is absent -- not a guess: it is the default in the loader that
// reads that same line.
int declared_hit_type(LPCSTR sect, LPCSTR line, int fallback)
{
    if (!pSettings->line_exist(sect, line))
        return fallback;
    return SVDamage::HitTypeFromName(pSettings->r_string(sect, line));
}

// The highest of the up-to-four difficulty entries on a hit_power-style line.
//
// ⚠The HIGHEST, not the first. The shooter picks its entry with its own
// g_SingleGameDifficulty, which is a CLIENT setting, so any of the four is a
// legitimate thing to see on the wire and a cap taken from one of them would
// refuse honest hits sent from the others.
float best_over_difficulties(LPCSTR sect, LPCSTR line)
{
    if (!pSettings->line_exist(sect, line))
        return 0.f;
    LPCSTR v = pSettings->r_string(sect, line);
    if (!v || !v[0])
        return 0.f;

    string128 buffer;
    float best = 0.f;
    const int n = _GetItemCount(v);
    for (int i = 0; i < n && i < 4; ++i)
    {
        const float f = (float)atof(_GetItem(v, i, buffer));
        if (f > best)
            best = f;
    }
    return best;
}

// How far above `blast` a legitimate explosion can land.
//
// CExplosive::Explode sends blast x CExplosive::ExplosionEffect, and that
// factor is NOT bounded by one: it averages, over test rays, sqrt(l_S/max_s) x
// TestPassEffect. TestPassEffect is a distance falloff and never exceeds 1, but
// l_S/max_s is min(d) x (|dir.x|/d.x + |dir.y|/d.y + |dir.z|/d.z) for the
// victim's bounding box, which peaks at sqrt(3) on a cube facing the blast
// corner-on. So the honest ceiling is blast x 3^(1/4) = 1.316.
//
// ⭐Worth the arithmetic rather than a round number: a cap has to sit above
// every honest hit or it refuses real damage, and "1.5 to be safe" would give
// a cheat half a grenade of free room.
constexpr float k_blast_geometry = 1.32f;
} // namespace

// ALife::g_tfString2HitType with the FATAL taken out.
//
// ⚠It really does FATAL on any string it does not recognise, and the cap now
// reads hit_type lines out of sections nobody vetted -- every section in the
// content, when sv_cap_audit runs. One typo in a mod's ini would take the
// server down from a *report*. A reader that can answer "I don't know" is the
// only kind that can be pointed at content it did not ship with.
//
// The names are g_tfString2HitType's own, deliberately: this has to agree with
// the loader that reads the same lines, or the ceiling would be attached to a
// type the client never sends. eHitTypePhysicStrike has no name here because it
// has none there either -- no content line can ask for it.
int SVDamage::HitTypeFromName(const char* s)
{
    if (!s || !s[0])
        return -1;
    if (!_stricmp(s, "burn"))
        return ALife::eHitTypeBurn;
    if (!_stricmp(s, "shock"))
        return ALife::eHitTypeShock;
    if (!_stricmp(s, "strike"))
        return ALife::eHitTypeStrike;
    if (!_stricmp(s, "wound"))
        return ALife::eHitTypeWound;
    if (!_stricmp(s, "radiation"))
        return ALife::eHitTypeRadiation;
    if (!_stricmp(s, "telepatic"))
        return ALife::eHitTypeTelepatic;
    if (!_stricmp(s, "fire_wound"))
        return ALife::eHitTypeFireWound;
    if (!_stricmp(s, "chemical_burn"))
        return ALife::eHitTypeChemicalBurn;
    if (!_stricmp(s, "explosion"))
        return ALife::eHitTypeExplosion;
    if (!_stricmp(s, "wound_2"))
        return ALife::eHitTypeWound_2;
    return -1;
}

void SVDamage::ResetCache() { s_profiles.clear(); }

void SVDamage::ResetPlaces() { s_places.clear(); }

void SVDamage::NoteItemPlace(u16 item_id, Place place)
{
    if (item_id == 0xffff)
        return;

    // Only artefacts move the floor, so only they are worth remembering.
    CSE_Abstract* item = SVWorld::Find(item_id);
    if (!item)
        return;
    LPCSTR sect = item->s_name.c_str();
    if (!sect || !pSettings->section_exist(sect) || !SectionIsArtefact(sect))
        return;

    for (ItemPlace& p : s_places)
        if (p.item == item_id)
        {
            p.place = place;
            return;
        }

    s_places.push_back(ItemPlace());
    s_places.back().item = item_id;
    s_places.back().place = place;
}

void SVDamage::ForgetItemPlace(u16 item_id)
{
    for (auto it = s_places.begin(); it != s_places.end(); ++it)
        if (it->item == item_id)
        {
            s_places.erase(it);
            return;
        }
}

bool SVDamage::ReadImmunities(const char* imm, float out[])
{
    if (!imm || !imm[0] || !pSettings->section_exist(imm))
        return false;

    string128 key;
    for (int i = 0; i < ALife::eHitTypeMax; ++i)
    {
        immunity_key(i, key);
        // ⚠Absent lines keep whatever `out` already held rather than resetting
        // to 1.0: this is layered over a section's immunities, and a model that
        // names only some types must not silently clear the rest.
        if (pSettings->line_exist(imm, key))
            out[i] = pSettings->r_float(imm, key);
    }
    return true;
}

SVDamage::Profile SVDamage::ProfileFor(const shared_str& section)
{
    for (const Profile& p : s_profiles)
        if (p.section == section)
            return p;

    s_profiles.push_back(Profile());
    Profile& p = s_profiles.back();
    p.section = section;
    for (int i = 0; i < ALife::eHitTypeMax; ++i)
    {
        p.immunity[i] = 1.f;
        p.health_part[i] = 1.f;
    }

    LPCSTR sect = section.c_str();
    if (!sect || !pSettings->section_exist(sect))
        return p;

    // Immunities -- CEntityAlive::Load reads them from immunities_sect.
    //
    // ⚠This is only the FIRST of two reads the engine does. CAI_Stalker::net_Spawn
    // re-reads immunities from the model's userdata over the top, so a creature's
    // final immunity depends on which model it wears -- see sv_visual_armour.h.
    if (pSettings->line_exist(sect, "immunities_sect"))
        ReadImmunities(pSettings->r_string(sect, "immunities_sect"), p.immunity);

    // Health hit parts -- CEntityCondition::LoadCondition, same lookup.
    LPCSTR cond = READ_IF_EXISTS(pSettings, r_string, sect, "condition_sect", sect);
    if (cond && pSettings->section_exist(cond))
    {
        const float def = READ_IF_EXISTS(pSettings, r_float, cond, "health_hit_part", 1.f);
        string128 key;
        for (int i = 0; i < ALife::eHitTypeMax; ++i)
        {
            strconcat(sizeof(key), key, "health_", ALife::g_cafHitType2String(ALife::EHitType(i)), "_hit_part");
            p.health_part[i] = READ_IF_EXISTS(pSettings, r_float, cond, key, def);
        }
    }

    // Bone factors. The server cannot tell which bone was hit -- a bone id
    // means nothing without the victim's skeleton -- so it assumes the least
    // damaging one.
    //
    // The strict minimum is NOT usable: stock content declares bones with a
    // factor of exactly zero (measured on [actor]: the minimum over the whole
    // damage section is 0.00), which would zero the floor for every bullet and
    // leave this whole check silently inert. So the smallest POSITIVE factor is
    // taken instead. The cost is that a hit which really did land on a
    // zero-damage bone is charged as if it had hit the least protected real
    // one; the client's own report pulls the ledger back down on the next
    // update, and sv_health_regen absorbs the rest.
    p.bone_floor = g_sv_health_bone_floor;
    if (pSettings->line_exist(sect, "damage"))
    {
        LPCSTR dmg = pSettings->r_string(sect, "damage");
        if (dmg && pSettings->section_exist(dmg))
        {
            float lowest = 0.f;
            string32 buffer;
            CInifile::Sect& bones = pSettings->r_section(dmg);
            for (const auto& line : bones.Data)
            {
                const float f = (float)atof(_GetItem(line.second.c_str(), 0, buffer));
                if (f > EPS_L && (lowest <= 0.f || f < lowest))
                    lowest = f;
            }
            if (lowest > 0.f)
                p.bone_floor = lowest;
        }
    }

    return p;
}

namespace
{
// One producer's answer, folded into the ceiling being built.
//
// The strongest wins. A section can make the same kind of hit two ways -- a
// knife whose second attack is also a `wound`, a grenade whose fragments are
// declared `explosion` like its blast -- and taking the weaker of the two
// would refuse the stronger honest hit.
void consider(SVDamage::ShotCap& cap, float power, const char* how, bool carried)
{
    if (power <= 0.f)
        return;
    if (cap.known && power <= cap.power)
        return;
    cap.known = true;
    cap.power = power;
    cap.how = how;
    cap.carried = carried;
    // Whoever wins owns the whole answer. The round only describes a firearm's
    // ceiling, so leaving it behind under a blast's number would name a
    // cartridge that had nothing to do with the hit.
    cap.ammo = shared_str();
    cap.exact_ammo = false;
}

// A firearm's bullet: hit_power x the loaded round's k_hit. Unchanged from
// when this was the only thing the cap knew, and measured: AK-74 with 5.45 FMJ
// caps at 0.6250 against 0.6250 on the wire point blank.
void firearm_cap(SVDamage::ShotCap& cap, const shared_str& sect, CSE_Abstract* source)
{
    const float hit_power = best_over_difficulties(sect.c_str(), "hit_power");
    if (hit_power <= 0.f)
        return;

    // Which round is loaded. The server carries the index, so this is usually
    // exact; when it is out of range (a weapon that never got an update, a
    // section whose ammo list changed) fall back to the strongest round the
    // weapon can chamber -- a cap has to err upward or it refuses honest shots.
    LPCSTR ammo_list = pSettings->r_string(sect, "ammo_class");
    const int rounds = _GetItemCount(ammo_list);
    if (rounds < 1)
        return;

    CSE_ALifeItemWeapon* w = smart_cast<CSE_ALifeItemWeapon*>(source);

    string128 buffer;
    float k_hit = 0.f;
    shared_str round;
    const int loaded = (w && int(w->ammo_type) < rounds) ? int(w->ammo_type) : -1;
    for (int i = 0; i < rounds; ++i)
    {
        if (loaded >= 0 && i != loaded)
            continue;
        string128 ammo;
        xr_strcpy(ammo, _GetItem(ammo_list, i, buffer));
        if (!pSettings->section_exist(ammo))
            continue;
        // Through ammo_tuned, so the balance overlay counts: the cap has to
        // follow the round as the GAME runs it, or a tuned round reads as a
        // cheat the moment it is fired.
        const float k = ammo_tuned(ammo, "k_hit", READ_IF_EXISTS(pSettings, r_float, ammo, "k_hit", 1.f));
        if (k > k_hit)
        {
            k_hit = k;
            round = ammo;
        }
    }
    if (k_hit <= 0.f)
        return;

    const float power = hit_power * k_hit;
    if (cap.known && power <= cap.power)
        return;

    consider(cap, power, "hit_power x k_hit", true);
    cap.ammo = round;
    cap.exact_ammo = (loaded >= 0);
}
} // namespace

SVDamage::ShotCap SVDamage::PowerCapFor(CSE_Abstract* source, u16 hit_type)
{
    ShotCap cap;
    if (!source || hit_type >= ALife::eHitTypeMax)
        return cap;

    const shared_str sect = source->s_name;
    if (!sect.size() || !pSettings->section_exist(sect.c_str()))
        return cap;

    const int want = int(hit_type);

    // Whether the source is a thing somebody carries. Decided from the ENTITY,
    // not the section, and it has to be: a monster's ini also declares
    // hit_power and hit_type, but a monster's real melee number comes out of
    // its attack animation table and can exceed that line -- capping a bite
    // with it would clamp honest damage. Only what a player can hold is capped
    // from a hit_power line.
    const bool item = (nullptr != smart_cast<CSE_ALifeInventoryItem*>(source));

    if (item && pSettings->line_exist(sect, "hit_power"))
    {
        // A firearm, as the content defines one: something that takes rounds.
        // Its bullets carry the type its `hit_type` names -- fire_wound unless
        // a mod says otherwise, which is the default LoadFireParams itself uses.
        const bool firearm = !!pSettings->line_exist(sect, "ammo_class");
        if (declared_hit_type(sect.c_str(), "hit_type", firearm ? ALife::eHitTypeFireWound : -1) == want)
        {
            if (firearm)
                firearm_cap(cap, sect, source);
            else
                consider(cap, best_over_difficulties(sect.c_str(), "hit_power"), "hit_power", true);
        }
    }

    // The second attack, and NOT as the other branch of the firearm question --
    // ⚠measured 30-07: this content's `wpn_knife` declares an ammo_class
    // (ammo_9x19_fmj, k_hit 1.0), so a knife takes the firearm path for its
    // primary. Reading hit_power_2 only where there was no ammo_class left the
    // alternative attack -- a whole hit type, wound_2 -- with no ceiling at all,
    // and the audit reported that type as one nothing in the content produces.
    if (item && declared_hit_type(sect.c_str(), "hit_type_2", -1) == want)
        consider(cap, best_over_difficulties(sect.c_str(), "hit_power_2"), "hit_power_2", true);

    // An explosion, and the fragments it throws. Both reach a victim as
    // separate hits with separate types, so they are separate ceilings --
    // capping a fragment with `blast` would be wrong in both directions.
    //
    // ⚠Not `carried`: the thrower has let go. A grenade in flight is still an
    // inventory item by class, but its parent link is gone, so demanding one
    // would exempt every explosion there is.
    if (pSettings->line_exist(sect, "blast") && declared_hit_type(sect.c_str(), "hit_type_blast", -1) == want)
        consider(cap, READ_IF_EXISTS(pSettings, r_float, sect, "blast", 0.f) * k_blast_geometry, "blast", false);

    if (pSettings->line_exist(sect, "frag_hit") && declared_hit_type(sect.c_str(), "hit_type_frag", -1) == want)
        consider(cap, READ_IF_EXISTS(pSettings, r_float, sect, "frag_hit", 0.f), "frag_hit", false);

    // An anomaly. CCustomZone::Power is max_start_power x RelativePower, and
    // RelativePower is 1 - attenuation x (dist/radius)^2 -- one at the centre,
    // falling to zero at the edge. With an attenuation below zero (content is
    // free to write one) it rises instead, so the ceiling follows the formula
    // rather than assuming the usual shape.
    //
    // ⚠m_fMaxPower is writable from Lua (`max_power` on the zone binding). A
    // mod that raises it at runtime would make this ceiling too tight, and the
    // symptom would be anomalies that stopped hurting. That is the single best
    // reason to leave sv_hit_validate at 1 until the census has watched a
    // session: level 1 reports, level 2 refuses.
    if (pSettings->line_exist(sect, "max_start_power") && declared_hit_type(sect.c_str(), "hit_type", -1) == want)
    {
        const float att = READ_IF_EXISTS(pSettings, r_float, sect, "attenuation", 1.f);
        const float peak = (att < 0.f) ? (1.f - att) : 1.f;
        consider(cap, READ_IF_EXISTS(pSettings, r_float, sect, "max_start_power", 0.f) * peak, "max_start_power", false);
    }

    if (cap.known)
        cap.weapon = sect;
    return cap;
}

float SVDamage::BoneFactor(const shared_str& section, const char* bone_name)
{
    if (!section.size() || !bone_name || !bone_name[0])
        return 1.f;
    if (!pSettings->line_exist(section, "damage"))
        return 1.f;

    LPCSTR dmg = pSettings->r_string(section, "damage");
    if (!dmg || !pSettings->section_exist(dmg))
        return 1.f;

    string32 buffer;
    if (pSettings->line_exist(dmg, bone_name))
        return (float)atof(_GetItem(pSettings->r_string(dmg, bone_name), 0, buffer));
    if (pSettings->line_exist(dmg, "default"))
        return (float)atof(_GetItem(pSettings->r_string(dmg, "default"), 0, buffer));
    return 1.f;
}

SVDamage::BoneArmour SVDamage::BoneArmourFor(const shared_str& section, const char* bone_name)
{
    if (!section.size())
        return BoneArmour();

    // ⚠This is NOT where CAI_Stalker finds a creature's armour, whatever the
    // comment here used to claim. The engine reads bones_protection_sect out of
    // the MODEL's userdata (ai_stalker.cpp, via IKinematics::LL_UserData); no
    // creature section in this content declares the line at all, which is why
    // every measurement the server made reported barm=0.000 for everybody.
    // ⭐The real lookup is SVVisualArmour::BoneArmourFor. This remains as the
    // fallback for content that does put the line on the section.
    LPCSTR prot = READ_IF_EXISTS(pSettings, r_string, section, "bones_protection_sect", nullptr);
    return ReadProtection(prot, bone_name);
}

SVDamage::BoneArmour SVDamage::ReadProtection(const char* prot, const char* bone_name)
{
    BoneArmour out;
    if (!prot || !prot[0] || !pSettings->section_exist(prot))
        return out;

    out.found = true;
    out.hit_fraction = READ_IF_EXISTS(pSettings, r_float, prot, "hit_fraction", 0.1f);

    // default first, then the bone's own line if it has one -- SBoneProtections
    // resolves in exactly that order.
    string32 buffer;
    auto read = [&](LPCSTR line) {
        LPCSTR v = pSettings->r_string(prot, line);
        out.koeff = (float)atof(_GetItem(v, 0, buffer));
        out.armour = (float)atof(_GetItem(v, 1, buffer));
    };
    if (pSettings->line_exist(prot, "default"))
        read("default");
    if (bone_name && bone_name[0] && pSettings->line_exist(prot, bone_name))
        read(bone_name);

    return out;
}

bool SVDamage::SectionIsArmour(const char* sect)
{
    // CCustomOutfit::Load reads these unconditionally, so their presence is
    // the signature of something that protects. Helmets and mod-added armour
    // pieces answer to it too, which is intended.
    return pSettings->line_exist(sect, "fire_wound_protection") && pSettings->line_exist(sect, "burn_protection");
}

void SVDamage::ResolveArmour(CSE_Abstract* owner, Armour& out)
{
    out = Armour();
    if (!owner)
        return;

    string128 key;
    for (u16 child_id : owner->children)
    {
        CSE_Abstract* child = SVWorld::Find(child_id);
        if (!child)
            continue;
        LPCSTR sect = child->s_name.c_str();
        if (!sect || !pSettings->section_exist(sect) || !SectionIsArmour(sect))
            continue;

        out.found = true;
        for (int i = 0; i < ALife::eHitTypeMax; ++i)
        {
            strconcat(sizeof(key), key, ALife::g_cafHitType2String(ALife::EHitType(i)), "_protection");
            const float v = READ_IF_EXISTS(pSettings, r_float, sect, key, 0.f);
            if (v > out.protection[i])
                out.protection[i] = v;
        }

        // The engine's own guaranteed minimum for bullets: however good the
        // armour, HitThruArmour never returns less than power * hit_fraction.
        if (pSettings->line_exist(sect, "bones_koeff_protection"))
        {
            LPCSTR bone_sect = pSettings->r_string(sect, "bones_koeff_protection");
            if (bone_sect && pSettings->section_exist(bone_sect))
            {
                const float frac = READ_IF_EXISTS(pSettings, r_float, bone_sect, "hit_fraction", 0.1f);
                if (frac < out.hit_fraction)
                    out.hit_fraction = frac;
            }
        }
        else if (out.hit_fraction > 0.1f)
            out.hit_fraction = 0.1f; // SBoneProtections' own default
    }
}

bool SVDamage::SectionIsArtefact(const char* sect)
{
    // CArtefact::Load reads hit_absorbation_sect unconditionally and only takes
    // the immunities when the section it names exists -- so the line plus a
    // real target is exactly what makes an item absorb hits. A dangling name is
    // not protection, and treating it as such would lower the floor for free.
    if (!pSettings->line_exist(sect, "hit_absorbation_sect"))
        return false;
    LPCSTR imm = pSettings->r_string(sect, "hit_absorbation_sect");
    return imm && pSettings->section_exist(imm);
}

void SVDamage::ResolveBelt(CSE_Abstract* owner, Belt& out)
{
    out = Belt();
    if (!owner)
        return;

    // How many artefacts could be on the belt at once. An outfit sets the count
    // (CInventory::BeltSlotsCount -> outfit->get_artefact_count), so the most
    // generous outfit the victim carries bounds it; with none, the inventory
    // defaults apply. Taking the LARGEST candidate is what lowers the floor,
    // which is the safe direction.
    u32 slots = pSettings->r_u32("inventory", "max_belt");
    {
        const u32 min_belt = READ_IF_EXISTS(pSettings, r_u32, "inventory", "min_belt", slots);
        if (min_belt > slots)
            slots = min_belt;
    }

    xr_vector<shared_str> carried;
    for (u16 child_id : owner->children)
    {
        CSE_Abstract* child = SVWorld::Find(child_id);
        if (!child)
            continue;
        LPCSTR sect = child->s_name.c_str();
        if (!sect || !pSettings->section_exist(sect))
            continue;

        if (SectionIsArmour(sect))
        {
            const u32 n = READ_IF_EXISTS(pSettings, r_u32, sect, "artefact_count", 0);
            if (n > slots)
                slots = n;
        }
        // Deliberately not exclusive with the branch above: an item that is
        // both would be counted twice, and double-counting protection only
        // pushes the floor further down.
        if (!SectionIsArtefact(sect))
            continue;

        // Skip only what the client has positively told us is off the belt.
        // Silence means unknown, and unknown must keep counting -- see Place.
        bool off_belt = false;
        for (const ItemPlace& p : s_places)
            if (p.item == child_id)
            {
                off_belt = (p.place == Place::NotBelt);
                break;
            }

        if (off_belt)
            ++out.excluded;
        else
            carried.push_back(child->s_name);
    }

    // The helmet slot takes an artefact too, and HitArtefactsOnBelt counts it
    // alongside the belt (belt_only is false on the hit path).
    ++slots;

    out.slots = slots;
    out.carried = u32(carried.size());
    if (carried.empty())
        return;

    out.found = true;

    // HitArtefactsOnBelt sums REDUCTIONS rather than multiplying factors:
    //   k = 1 - sum(1 - k_i), floored at zero.
    // So the strongest belt the victim could be wearing is the one holding the
    // `slots` artefacts with the largest reduction. Ones that make a hit worse
    // (k_i > 1) are left off: a real belt might well hold them, but that only
    // raises real damage, and the floor has to stay underneath it.
    //
    // Picking the best set per hit type independently describes no single real
    // belt, and does not need to -- a hit has one type, and each type's bound
    // is honest on its own.
    xr_vector<float> reductions;
    for (int t = 0; t < ALife::eHitTypeMax; ++t)
    {
        reductions.clear();
        string128 key;
        immunity_key(t, key);

        for (const shared_str& s : carried)
        {
            LPCSTR imm = pSettings->r_string(s.c_str(), "hit_absorbation_sect");
            const float k = READ_IF_EXISTS(pSettings, r_float, imm, key, 1.f);
            if (k < 1.f)
                reductions.push_back(1.f - k);
        }
        if (reductions.empty())
            continue;

        std::sort(reductions.begin(), reductions.end(), std::greater<float>());
        const u32 take = (u32(reductions.size()) < slots) ? u32(reductions.size()) : slots;

        float sum = 0.f;
        for (u32 i = 0; i < take; ++i)
            sum += reductions[i];

        out.k[t] = (sum < 1.f) ? (1.f - sum) : 0.f;
    }
}
void SVDamage::AuditDamage(const char* section)
{
    LPCSTR sect = (section && section[0]) ? section : "actor";
    if (!pSettings->section_exist(sect))
    {
        Msg("! [sv_damage] no such section '%s'", sect);
        return;
    }

    const Profile p = ProfileFor(sect);
    Msg("~ [sv_damage] '%s': fire_wound immunity=%.2f health_part=%.2f | wound %.2f/%.2f | explosion %.2f/%.2f | strike %.2f/%.2f", sect, p.immunity[ALife::eHitTypeFireWound],
        p.health_part[ALife::eHitTypeFireWound], p.immunity[ALife::eHitTypeWound], p.health_part[ALife::eHitTypeWound], p.immunity[ALife::eHitTypeExplosion],
        p.health_part[ALife::eHitTypeExplosion], p.immunity[ALife::eHitTypeStrike], p.health_part[ALife::eHitTypeStrike]);

    if (!pSettings->line_exist(sect, "damage"))
    {
        Msg("~ [sv_damage] no 'damage' line -- bone factors come from the visual's user data, which a server with no visuals cannot read; floor uses sv_health_bone_floor=%.2f",
            g_sv_health_bone_floor);
        return;
    }

    LPCSTR dmg = pSettings->r_string(sect, "damage");
    if (!dmg || !pSettings->section_exist(dmg))
    {
        Msg("! [sv_damage] damage section '%s' missing", dmg ? dmg : "?");
        return;
    }

    // The bone NAMES are all the server has. Turning the bone id that arrives
    // in a hit packet into one of these needs the victim's skeleton, which a
    // headless server does not load -- hence a single assumed factor for every
    // hit rather than the real one.
    float lowest_nonzero = 0.f, highest = 0.f;
    shared_str low_name, high_name;
    u32 zeros = 0, count = 0;
    string32 buffer;
    CInifile::Sect& bones = pSettings->r_section(dmg);
    for (const auto& line : bones.Data)
    {
        const float f = (float)atof(_GetItem(line.second.c_str(), 0, buffer));
        ++count;
        if (f <= EPS_L)
        {
            ++zeros;
            Msg("~ [sv_damage]   %-24s %.2f  (zero: content says a hit here costs nothing)", line.first.c_str(), f);
            continue;
        }
        if (lowest_nonzero <= 0.f || f < lowest_nonzero)
        {
            lowest_nonzero = f;
            low_name = line.first;
        }
        if (f > highest)
        {
            highest = f;
            high_name = line.first;
        }
        Msg("~ [sv_damage]   %-24s %.2f", line.first.c_str(), f);
    }

    Msg("~ [sv_damage] '%s': %u bones (%u zero). Floor assumes %.2f ('%s'); the worst case is %.2f ('%s') -- a hit there really costs %.1fx what the floor charges.", dmg, count,
        zeros, lowest_nonzero, low_name.size() ? low_name.c_str() : "?", highest, high_name.size() ? high_name.c_str() : "?",
        (lowest_nonzero > 0.f) ? (highest / lowest_nonzero) : 0.f);
}

void SVDamage::AuditCaps()
{
    // What kinds of producer the content has, in the order a reader wants them:
    // the thing a player holds first, the world's own hazards last.
    enum
    {
        kFirearm = 0,
        kHand,
        kCreature,
        kBlast,
        kFrag,
        kZone,
        kKinds
    };
    // ⚠The creature row is separate from the hand row because the GATE treats
    // them differently: only an inventory item is capped from a hit_power line.
    // One row for both would report a ceiling over 400 sections that nothing
    // enforces, which is the worst kind of wrong -- it reads as covered.
    static const char* kind_name[kKinds] = {"firearm hit_power x k_hit", "held, hit_power/hit_power_2", "CREATURE hit_power (not capped)", "blast x 1.32", "frag_hit",
                                            "max_start_power"};

    struct Best
    {
        u32 n{0};
        float power{0.f};
        shared_str sect;
    };
    Best best[ALife::eHitTypeMax][kKinds];

    // Sections that declare hit_power but are not something a player carries --
    // creatures, mostly. Named out loud because the scan CAN read a ceiling off
    // them and the gate deliberately will not: their real melee number lives in
    // an attack animation table, not in that line.
    u32 non_item_hit_power = 0;

    for (const auto& it : pSettings->sections())
    {
        LPCSTR sect = it.first.c_str();

        const bool has_hp = !!pSettings->line_exist(sect, "hit_power");
        const bool has_ammo = !!pSettings->line_exist(sect, "ammo_class");

        auto note = [&](int type, int kind, float power) {
            if (type < 0 || type >= ALife::eHitTypeMax || power <= 0.f)
                return;
            Best& b = best[type][kind];
            ++b.n;
            if (power > b.power)
            {
                b.power = power;
                b.sect = it.first;
            }
        };

        // A knife declares a slot; a dog does not. The scan has no entity to
        // ask, so this stands in for the smart_cast the gate does -- and it is
        // reported as its own row rather than folded in, because the two are
        // not treated alike.
        const bool held = !!pSettings->line_exist(sect, "slot");

        if (has_hp && has_ammo)
        {
            // Strongest round the weapon takes, which is what a cap falls back
            // to when the loaded one is unknown -- the same number sv_weapon_audit
            // prints, so the two reports can be laid side by side.
            LPCSTR ammo_list = pSettings->r_string(sect, "ammo_class");
            string128 buffer;
            float k_hit = 0.f;
            for (int i = 0, n = _GetItemCount(ammo_list); i < n; ++i)
            {
                string128 ammo;
                xr_strcpy(ammo, _GetItem(ammo_list, i, buffer));
                if (!pSettings->section_exist(ammo))
                    continue;
                const float k = ammo_tuned(ammo, "k_hit", READ_IF_EXISTS(pSettings, r_float, ammo, "k_hit", 1.f));
                if (k > k_hit)
                    k_hit = k;
            }
            note(declared_hit_type(sect, "hit_type", ALife::eHitTypeFireWound), kFirearm, best_over_difficulties(sect, "hit_power") * k_hit);
        }
        else if (has_hp)
        {
            note(declared_hit_type(sect, "hit_type", -1), held ? kHand : kCreature, best_over_difficulties(sect, "hit_power"));
            if (!held)
                ++non_item_hit_power;
        }

        // Independent of the ammo question -- a knife in this content has both.
        if (has_hp && held)
            note(declared_hit_type(sect, "hit_type_2", -1), kHand, best_over_difficulties(sect, "hit_power_2"));

        if (pSettings->line_exist(sect, "blast"))
            note(declared_hit_type(sect, "hit_type_blast", -1), kBlast, READ_IF_EXISTS(pSettings, r_float, sect, "blast", 0.f) * k_blast_geometry);
        if (pSettings->line_exist(sect, "frag_hit"))
            note(declared_hit_type(sect, "hit_type_frag", -1), kFrag, READ_IF_EXISTS(pSettings, r_float, sect, "frag_hit", 0.f));
        if (pSettings->line_exist(sect, "max_start_power"))
        {
            const float att = READ_IF_EXISTS(pSettings, r_float, sect, "attenuation", 1.f);
            const float peak = (att < 0.f) ? (1.f - att) : 1.f;
            note(declared_hit_type(sect, "hit_type", -1), kZone, READ_IF_EXISTS(pSettings, r_float, sect, "max_start_power", 0.f) * peak);
        }
    }

    Msg("~ [sv_cap] ==== the ceiling this content declares, per kind of damage ====");
    Msg("~ [sv_cap] read from the same ini lines the CLIENT reads to produce the hit; a claim above one of these is a number the world cannot make");

    for (int t = 0; t < ALife::eHitTypeMax; ++t)
    {
        // What the GATE would enforce, which is not the same as what the scan
        // can read: the creature row is readable and deliberately unenforced.
        float enforced = 0.f;
        bool any = false;
        for (int k = 0; k < kKinds; ++k)
        {
            if (best[t][k].n)
                any = true;
            if (k != kCreature && best[t][k].power > enforced)
                enforced = best[t][k].power;
        }

        if (!any)
        {
            // ⚠NOT the same as "impossible". A script can send any type from
            // Lua, physics sends its own strike, and both are legitimate. It
            // means only that nothing here will ever be refused.
            Msg("~ [sv_cap]   %-14s no producer in content -- claims of this kind pass unchecked", ALife::g_cafHitType2String(ALife::EHitType(t)));
            continue;
        }

        for (int k = 0; k < kKinds; ++k)
        {
            const Best& b = best[t][k];
            if (!b.n)
                continue;
            Msg("~ [sv_cap]   %-14s %-31s %3u section(s), strongest %8.4f  '%s'", ALife::g_cafHitType2String(ALife::EHitType(t)), kind_name[k], b.n, b.power, b.sect.c_str());
        }

        if (enforced <= 0.f)
            Msg("!  [sv_cap]   %-14s ...but every producer of it is one the gate does not cap -- claims of this kind still pass unchecked",
                ALife::g_cafHitType2String(ALife::EHitType(t)));
    }

    Msg("~ [sv_cap] %u section(s) declare hit_power without an inventory slot (creatures) -- readable, but NOT capped: a monster's real melee number comes from its attack "
        "animation table, and the host computes it anyway",
        non_item_hit_power);
    Msg("~ [sv_cap] never capped, by design: collision and fall damage (physics, no content line) and script hits (a mod may send any type). Unknown means pass.");

    Msg("~ [sv_cap] sv_hit_validate=%d (%s), sv_hit_slack=%.3f", g_sv_hit_validate, (g_sv_hit_validate >= 2) ? "CLAMPING" : (g_sv_hit_validate ? "measuring only" : "off"),
        g_sv_hit_slack);
}

void SVDamage::AuditWeapons(const char* filter)
{
    const bool all = !filter || !filter[0];
    u32 shown = 0;

    Msg("~ [sv_weapon] power a shot carries = weapon hit_power x round k_hit; k_ap is what the round takes off armour%s%s", all ? "" : ", filter=", all ? "" : filter);

    for (const auto& it : pSettings->sections())
    {
        LPCSTR sect = it.first.c_str();
        // A weapon is a section that names both a damage and something to feed
        // it -- the same two lines CShootingObject and CWeapon read.
        if (!pSettings->line_exist(sect, "hit_power") || !pSettings->line_exist(sect, "ammo_class"))
            continue;
        if (!all && !strstr(sect, filter))
            continue;

        // hit_power is per difficulty: master first, then veteran, stalker,
        // novice. Master is what an NPC always shoots with, so it is the one
        // number that describes the weapon regardless of who holds it.
        string128 buffer;
        LPCSTR hp_line = pSettings->r_string(sect, "hit_power");
        const float hit_power = (float)atof(_GetItem(hp_line, 0, buffer));

        LPCSTR ammo_list = pSettings->r_string(sect, "ammo_class");
        const int rounds = _GetItemCount(ammo_list);
        for (int i = 0; i < rounds; ++i)
        {
            string128 ammo;
            xr_strcpy(ammo, _GetItem(ammo_list, i, buffer));
            if (!pSettings->section_exist(ammo))
                continue;

            // Through ammo_tuned, so this reports the round as the game runs
            // it -- the balance overlay included -- rather than what the packed
            // content happens to say.
            const float k_hit = ammo_tuned(ammo, "k_hit", READ_IF_EXISTS(pSettings, r_float, ammo, "k_hit", 1.f));
            const float k_ap = ammo_tuned(ammo, "k_ap", READ_IF_EXISTS(pSettings, r_float, ammo, "k_ap", 0.f));
            const float k_pierce = ammo_tuned(ammo, "k_pierce", READ_IF_EXISTS(pSettings, r_float, ammo, "k_pierce", 1.f));
            const float k_dist = ammo_tuned(ammo, "k_dist", READ_IF_EXISTS(pSettings, r_float, ammo, "k_dist", 1.f));
            const float impair = READ_IF_EXISTS(pSettings, r_float, ammo, "impair", 1.f);
            const int buck = READ_IF_EXISTS(pSettings, r_s32, ammo, "buck_shot", 1);

            // Pellets each carry the full power, so a shotgun round is worth
            // its shot count -- ignoring that badly understates the weapon.
            string32 pellets = {};
            if (buck > 1)
                xr_sprintf(pellets, " x%d pellets", buck);

            // k_ap is the only one of these that reaches a victim's armour.
            // k_pierce is about punching through geometry, k_dist and impair
            // are range falloff -- printing them together is what shows whether
            // two rounds actually differ where it counts.
            Msg("~ [sv_weapon]   %-24s + %-24s = %.3f x %.2f = %.3f/pellet%s | ap=%.2f pierce=%.2f dist=%.2f impair=%.2f", sect, ammo, hit_power, k_hit, hit_power * k_hit, pellets,
                k_ap, k_pierce, k_dist, impair);
            ++shown;
        }
    }

    Msg("~ [sv_weapon] %u weapon/round pairs listed", shown);
}

void SVDamage::AuditBelt()
{
    // The hit types a belt can actually change the floor for. Radiation and
    // telepathy never reach health, so the floor already ignores them.
    const int types[] = {ALife::eHitTypeFireWound, ALife::eHitTypeWound, ALife::eHitTypeExplosion, ALife::eHitTypeBurn, ALife::eHitTypeStrike, ALife::eHitTypeChemicalBurn,
                         ALife::eHitTypeShock};

    const u32 max_belt = pSettings->r_u32("inventory", "max_belt");
    const u32 min_belt = READ_IF_EXISTS(pSettings, r_u32, "inventory", "min_belt", max_belt);
    u32 slots = (min_belt > max_belt) ? min_belt : max_belt;
    ++slots; // helmet, as in ResolveBelt

    // Where the capacity comes from, because it is the single number that
    // decides how weak this bound is, and it is read from content rather than
    // chosen. CInventory::BeltSlotsCount takes it from the WORN outfit
    // (artefact_count), falling back to min_belt bare -- so the widest outfit
    // in the content bounds any player who has one.
    u32 widest_outfit = 0;
    shared_str widest_sect;
    for (const auto& it : pSettings->sections())
    {
        LPCSTR sect = it.first.c_str();
        if (!SectionIsArmour(sect))
            continue;
        const u32 n = READ_IF_EXISTS(pSettings, r_u32, sect, "artefact_count", max_belt);
        if (n > widest_outfit)
        {
            widest_outfit = n;
            widest_sect = it.first;
        }
    }

    Msg("~ [sv_belt] artefacts scale a hit BEFORE armour (CActor::HitArtefactsOnBelt), so the floor multiplies by them too");
    Msg("~ [sv_belt] capacity: inventory max_belt=%u min_belt=%u, widest outfit artefact_count=%u ('%s') -> bound assumes %u (+1 helmet)", max_belt, min_belt, widest_outfit,
        widest_sect.size() ? widest_sect.c_str() : "none", slots);

    // Per hit type, the reductions the content offers, so the worst case a
    // full belt of the best artefacts could reach can be stated rather than
    // guessed at.
    xr_vector<float> best[ALife::eHitTypeMax];
    u32 matched = 0;
    shared_str strongest_sect;
    float strongest_fw = 0.f;

    for (const auto& it : pSettings->sections())
    {
        LPCSTR sect = it.first.c_str();
        if (!SectionIsArtefact(sect))
            continue;
        ++matched;

        LPCSTR imm = pSettings->r_string(sect, "hit_absorbation_sect");
        string128 key;
        bool protective = false;
        for (int t : types)
        {
            immunity_key(t, key);
            const float k = READ_IF_EXISTS(pSettings, r_float, imm, key, 1.f);
            if (k < 1.f)
            {
                best[t].push_back(1.f - k);
                protective = true;
            }
        }

        immunity_key(ALife::eHitTypeFireWound, key);
        const float fw = READ_IF_EXISTS(pSettings, r_float, imm, key, 1.f);
        if (protective && (1.f - fw) > strongest_fw)
        {
            strongest_fw = 1.f - fw;
            strongest_sect = it.first;
        }

        if (matched <= 8)
            Msg("~ [sv_belt]   '%s' -> '%s' fire_wound k=%.2f%s", sect, imm, fw, protective ? "" : " (no reduction on any health-bearing type)");
    }

    if (!matched)
    {
        Msg("~ [sv_belt] no artefact sections matched -- either this content has none, or hit_absorbation_sect is not how it spells absorption. The floor then ignores belts, "
            "which is only safe if nothing a player can carry reduces damage.");
        return;
    }

    Msg("~ [sv_belt] %u artefact sections; strongest single fire_wound reduction %.2f ('%s')", matched, strongest_fw, strongest_sect.size() ? strongest_sect.c_str() : "none");

    // The number that matters: a full belt of the best artefacts in this
    // content. Where that reaches 1.00 the floor collapses to zero and the
    // ledger stops constraining that player at all -- not a bug (the content
    // really does let them take nothing) but it has to be visible, because a
    // floor that is silently zero is exactly how this check dies unnoticed.
    for (int t : types)
    {
        if (best[t].empty())
            continue;
        std::sort(best[t].begin(), best[t].end(), std::greater<float>());
        const u32 take = (u32(best[t].size()) < slots) ? u32(best[t].size()) : slots;
        float sum = 0.f;
        for (u32 i = 0; i < take; ++i)
            sum += best[t][i];

        const float k = (sum < 1.f) ? (1.f - sum) : 0.f;
        if (k <= 0.f)
            Msg("! [sv_belt]   %-14s best %u of %u sum to %.2f -> floor x0.00: a player carrying these is UNCONSTRAINED by the ledger for this hit type", ALife::g_cafHitType2String(ALife::EHitType(t)),
                take, u32(best[t].size()), sum);
        else
            Msg("~ [sv_belt]   %-14s best %u of %u sum to %.2f -> floor x%.2f", ALife::g_cafHitType2String(ALife::EHitType(t)), take, u32(best[t].size()), sum, k);
    }
}

void SVDamage::AuditArmour()
{
    u32 matched = 0, with_bones = 0;
    float worst_frac = 1.f;
    float best_fw = 0.f;
    shared_str best_sect;

    Msg("~ [sv_armour] scanning %u ini sections for things the floor would treat as armour", u32(pSettings->sections().size()));
    for (const auto& it : pSettings->sections())
    {
        LPCSTR sect = it.first.c_str();
        if (!SectionIsArmour(sect))
            continue;
        ++matched;

        const float fw = READ_IF_EXISTS(pSettings, r_float, sect, "fire_wound_protection", 0.f);
        float frac = 0.1f; // SBoneProtections' own default when no section is named
        const bool bones = pSettings->line_exist(sect, "bones_koeff_protection") != 0;
        if (bones)
        {
            ++with_bones;
            LPCSTR bone_sect = pSettings->r_string(sect, "bones_koeff_protection");
            if (bone_sect && pSettings->section_exist(bone_sect))
                frac = READ_IF_EXISTS(pSettings, r_float, bone_sect, "hit_fraction", 0.1f);
        }

        if (frac < worst_frac)
            worst_frac = frac;
        if (fw > best_fw)
        {
            best_fw = fw;
            best_sect = it.first;
        }

        if (matched <= 8)
            Msg("~ [sv_armour]   '%s' fire_wound_protection=%.2f hit_fraction=%.3f%s", sect, fw, frac, bones ? "" : " (no bones_koeff_protection)");
    }

    if (!matched)
    {
        Msg("! [sv_armour] NOTHING matched -- every player would be charged an UNARMOURED floor, which can exceed what an armoured player really loses. Fix section_is_armour "
            "before enabling sv_health_authority 2.");
        return;
    }

    // The strongest armour in the content is what bounds the check: a bullet
    // floor is power * hit_fraction, so the smallest hit_fraction anywhere is
    // the least the floor can ever charge.
    Msg("~ [sv_armour] %u sections matched (%u with bone protection); toughest fire_wound=%.2f ('%s'), smallest hit_fraction=%.3f -- so an armoured bullet floor bottoms out at "
        "%.3f x power x immunity x health_part x bone",
        matched, with_bones, best_fw, best_sect.size() ? best_sect.c_str() : "?", worst_frac, worst_frac);
}


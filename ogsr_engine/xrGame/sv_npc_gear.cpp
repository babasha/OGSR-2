#include "StdAfx.h"
#include "xr_platform_seh.h"
#include "sv_npc_gear.h"

#include "sv_npc.h"
#include "sv_world_lookup.h"
#include "sv_damage_model.h"
#include "sv_visual_armour.h"

#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "Weapon.h"
#include "xrMessages.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../COMMON_AI/xrServer_Objects_ALife_Items.h"
#include "character_community.h"

#include <algorithm>

namespace
{
// The one item spawn_supplies always makes and no test is ever about.
const char* const PDA_SECTION = "device_pda";

const char* faction_of(u16 id)
{
    CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(SVWorld::Find(id));
    return (trader && trader->m_community_index != NO_COMMUNITY_INDEX) ? trader->CommunityName() : "-";
}

struct Carried
{
    shared_str section;
    u16 id{0};
    int rounds{-1}; // magazine for a weapon, box contents for ammo, -1 for neither
    bool armour{false};
};

// Everything the server entity says the creature owns. Offline or online, this
// is the same list -- which is the reason it, and not the object's inventory,
// is what the census is built on.
void carried_by(u16 id, xr_vector<Carried>& out)
{
    out.clear();

    CSE_Abstract* owner = SVWorld::Find(id);
    if (!owner)
        return;

    for (u16 child_id : owner->children)
    {
        CSE_Abstract* child = SVWorld::Find(child_id);
        if (!child)
            continue;

        Carried c;
        c.section = child->s_name;
        c.id = child_id;

        if (CSE_ALifeItemWeapon* w = smart_cast<CSE_ALifeItemWeapon*>(child))
            c.rounds = int(w->a_elapsed);
        else if (CSE_ALifeItemAmmo* a = smart_cast<CSE_ALifeItemAmmo*>(child))
            c.rounds = int(a->a_elapsed);

        LPCSTR sect = c.section.c_str();
        c.armour = sect && pSettings->section_exist(sect) && SVDamage::SectionIsArmour(sect);

        out.push_back(c);
    }
}

struct Sample
{
    u16 id{0};
    const char* faction{"-"};
    bool online{false};
    bool alive{false};

    string64 hands{}; // what it is holding right now, "<offline>" when unknown
    int mag{-1}; // rounds in that weapon
    string64 worn{}; // what is in the outfit slot

    u32 items{0};
    u32 loose_rounds{0}; // ammo in boxes, not counting magazines
    u32 weapons{0};

    float bullet{0.f}; // best fire_wound_protection it carries
    bool has_armour{false};

    string1024 signature{}; // the multiset of sections, sorted -- two creatures
                            // with the same string are equipped the same
};

// "ammo_5.45x39_fmj x2, device_pda, novice_outfit, wpn_ak74" -- sorted so that
// spawn order cannot make two identical loadouts look different.
void build_signature(const xr_vector<Carried>& items, string1024& out)
{
    out[0] = 0;

    xr_vector<shared_str> names;
    for (const Carried& c : items)
        names.push_back(c.section);
    std::sort(names.begin(), names.end(), [](const shared_str& a, const shared_str& b) { return xr_strcmp(a.c_str(), b.c_str()) < 0; });

    for (u32 i = 0; i < names.size();)
    {
        u32 n = 1;
        while (i + n < names.size() && !xr_strcmp(names[i].c_str(), names[i + n].c_str()))
            ++n;

        string128 piece;
        if (n > 1)
            xr_sprintf(piece, "%s x%u", names[i].c_str(), n);
        else
            xr_strcpy(piece, names[i].c_str());

        if (out[0])
            xr_strcat(out, sizeof(string1024), ", ");
        xr_strcat(out, sizeof(string1024), piece);

        i += n;
    }

    if (!out[0])
        xr_strcpy(out, "<nothing>");
}

Sample sample_of(u16 id)
{
    Sample s;
    s.id = id;
    s.faction = faction_of(id);

    xr_vector<Carried> items;
    carried_by(id, items);
    s.items = u32(items.size());

    for (const Carried& c : items)
    {
        if (smart_cast<CSE_ALifeItemAmmo*>(SVWorld::Find(c.id)))
            s.loose_rounds += u32(std::max(0, c.rounds));
        else if (c.rounds >= 0)
            ++s.weapons;
    }
    build_signature(items, s.signature);

    // Armour the same way the damage floor reads it: best of what is carried.
    // ⚠"Carried", not "worn" -- see the note in sv_damage_model.h. The worn
    // slot below is the online object's answer to the same question, and the
    // two disagreeing is worth seeing rather than hiding.
    SVDamage::Armour armour;
    SVDamage::ResolveArmour(SVWorld::Find(id), armour);
    s.has_armour = armour.found;
    s.bullet = armour.protection[ALife::eHitTypeFireWound];

    xr_strcpy(s.hands, "<offline>");
    xr_strcpy(s.worn, "<offline>");

    CObject* O = g_pGameLevel ? Level().Objects.net_Find(id) : nullptr;
    CInventoryOwner* io = smart_cast<CInventoryOwner*>(O);
    if (!io)
        return s;

    s.online = true;
    if (CEntityAlive* E = smart_cast<CEntityAlive*>(O))
        s.alive = !!E->g_Alive();

    CInventoryItem* active = io->inventory().ActiveItem();
    xr_strcpy(s.hands, active ? active->object().cNameSect().c_str() : "<empty>");
    if (CWeapon* w = smart_cast<CWeapon*>(active))
        s.mag = int(w->GetAmmoElapsed());

    xr_strcpy(s.worn, "<none>");
    if (io->inventory().m_slots.size() > OUTFIT_SLOT)
        if (CInventoryItem* outfit = io->inventory().m_slots[OUTFIT_SLOT].m_pIItem)
            xr_strcpy(s.worn, outfit->object().cNameSect().c_str());

    return s;
}

void print_group(const char* faction, const xr_vector<Sample>& g)
{
    if (g.empty())
        return;

    // Distinct loadouts, most common first. ⭐This is the line the whole file
    // is for: one row means the side is uniform, four rows mean the "identical
    // gear" the test claimed was never true.
    struct Bucket
    {
        const char* sig;
        u32 n;
    };
    xr_vector<Bucket> buckets;
    for (const Sample& s : g)
    {
        bool placed = false;
        for (Bucket& b : buckets)
            if (!xr_strcmp(b.sig, s.signature))
            {
                ++b.n;
                placed = true;
                break;
            }
        if (!placed)
            buckets.push_back({s.signature, 1});
    }
    std::sort(buckets.begin(), buckets.end(), [](const Bucket& a, const Bucket& b) { return a.n > b.n; });

    Msg("~ [gear] --- %s: %u creatures, %u distinct loadout%s ---", faction, u32(g.size()), u32(buckets.size()), (buckets.size() == 1) ? "" : "s");
    for (const Bucket& b : buckets)
        Msg("~ [gear]   x%-2u %s", b.n, b.sig);

    for (const Sample& s : g)
    {
        if (s.mag >= 0)
            Msg("~ [gear]   %u %s hands=%s mag=%d worn=%s | %u items, %u loose rounds, %u weapons | bullet armour %.2f%s", s.id, s.alive ? "alive" : "DEAD ", s.hands, s.mag,
                s.worn, s.items, s.loose_rounds, s.weapons, s.bullet, s.has_armour ? "" : " (none carried)");
        else
            Msg("~ [gear]   %u %s hands=%s          worn=%s | %u items, %u loose rounds, %u weapons | bullet armour %.2f%s", s.id, s.alive ? "alive" : "DEAD ", s.hands, s.worn,
                s.items, s.loose_rounds, s.weapons, s.bullet, s.has_armour ? "" : " (none carried)");
    }
}

void census_guarded(const xr_vector<u16>& ids)
{
    xr_vector<Sample> samples;
    for (u16 id : ids)
        samples.push_back(sample_of(id));

    u32 online = 0, empty_handed = 0;
    for (const Sample& s : samples)
    {
        if (s.online)
            ++online;
        if (s.online && s.mag < 0)
            ++empty_handed;
    }

    Msg("~ [gear] %u creatures, %u online, %u holding no weapon", u32(samples.size()), online, empty_handed);

    // ⭐⭐The protection that actually applies, which is NOT the outfit line
    // below it. Measured 28-07-2026: an outfit handed to a stalker never
    // reaches the outfit slot -- every census reads worn=<none> and the item
    // quietly leaves the inventory.
    //
    // ⭐⭐⭐And it is not the SECTION either, which is what this printed until
    // 05-08-2026: bones_protection_sect is declared by the creature's MODEL, in
    // its .ogf userdata, and by no creature section in this content. Grouping by
    // section therefore reported one armour for a whole squad that could be
    // wearing anything. Group by model, because the model IS the armour.
    xr_vector<shared_str> visuals;
    for (u16 id : ids)
    {
        CSE_Abstract* e = SVWorld::Find(id);
        CSE_Visual* v = e ? e->visual() : nullptr;
        if (!v || !v->get_visual())
            continue;
        const shared_str vis(v->get_visual());
        bool seen = false;
        for (const shared_str& s : visuals)
            if (!xr_strcmp(s.c_str(), vis.c_str()))
            {
                seen = true;
                break;
            }
        if (!seen)
            visuals.push_back(vis);
    }
    for (const shared_str& vis : visuals)
    {
        const SVVisualArmour::Model& m = SVVisualArmour::For(vis);
        if (!m.found)
        {
            Msg("~ [gear] model '%s' declares no bones_protection_sect -- creatures wearing it are UNARMOURED", vis.c_str());
            continue;
        }

        const SVDamage::BoneArmour torso = SVDamage::ReadProtection(m.protection.c_str(), "bip01_spine");
        const SVDamage::BoneArmour head = SVDamage::ReadProtection(m.protection.c_str(), "bip01_head");
        Msg("~ [gear] model '%s' -> '%s': torso %.2f, head %.2f, hit_fraction %.2f (this is what a bullet meets -- an outfit given to a creature does not)", vis.c_str(),
            m.protection.c_str(), torso.armour, head.armour, torso.hit_fraction);
    }

    xr_vector<const char*> factions;
    for (const Sample& s : samples)
    {
        bool seen = false;
        for (const char* f : factions)
            if (!xr_strcmp(f, s.faction))
            {
                seen = true;
                break;
            }
        if (!seen)
            factions.push_back(s.faction);
    }

    for (const char* f : factions)
    {
        xr_vector<Sample> g;
        for (const Sample& s : samples)
            if (!xr_strcmp(s.faction, f))
                g.push_back(s);
        print_group(f, g);
    }

    // ⭐The verdict a two-sided test actually needs, stated rather than left to
    // be eyeballed across two blocks.
    if (factions.size() == 2)
    {
        const char* sig[2] = {nullptr, nullptr};
        bool uniform[2] = {true, true};
        for (int i = 0; i < 2; ++i)
            for (const Sample& s : samples)
            {
                if (xr_strcmp(s.faction, factions[i]))
                    continue;
                if (!sig[i])
                    sig[i] = s.signature;
                else if (xr_strcmp(sig[i], s.signature))
                    uniform[i] = false;
            }

        const bool same = sig[0] && sig[1] && !xr_strcmp(sig[0], sig[1]) && uniform[0] && uniform[1];
        Msg("~ [gear] %s vs %s: the two sides are equipped %s", factions[0], factions[1], same ? "IDENTICALLY" : "DIFFERENTLY -- the fight is not a fair one");
    }
}
} // namespace

void SVGear::Census(const xr_vector<u16>& ids)
{
    if (!g_pGameLevel)
    {
        Msg("! [gear] no level");
        return;
    }

    xr_vector<u16> who = ids;
    if (who.empty())
        SVNpc::Ids(who);
    if (who.empty())
    {
        Msg("! [gear] no tracked puppets -- spawn some");
        return;
    }

    // Same guard as the mind census, for the same reason: this reads a dozen
    // half-initialised objects and one of them must not take the server down.
    // ⚠xrGame is built without /EH, so this catch is the ONLY thing that would
    // see an access violation here (see the frame-registrar note in pure.h).
    XR_SEH_TRY
    {
        census_guarded(who);
    }
    XR_SEH_EXCEPT(ExceptStackTrace("[gear] stack_trace:\n"))
    {
        Msg("!! [gear] the census itself faulted -- see the stack above");
    }
}

void SVGear::Templates(const char* filter)
{
    const bool all = !filter || !filter[0];

    u32 scanned = 0, armoured = 0, printed = 0;
    shared_str toughest;
    float toughest_armour = -1.f;

    for (const auto& it : pSettings->sections())
    {
        LPCSTR sect = it.first.c_str();
        if (!sect)
            continue;

        // A creature template, as far as anything here is concerned, is a
        // section that declares one of the two tables a hit on a creature goes
        // through. Nothing is inferred from names.
        const bool has_prot = pSettings->line_exist(sect, "bones_protection_sect") != 0;
        const bool has_dmg = pSettings->line_exist(sect, "damage") != 0;
        if (!has_prot && !has_dmg)
            continue;
        ++scanned;

        if (!all && !strstr(sect, filter))
            continue;

        const SVDamage::BoneArmour own = SVDamage::BoneArmourFor(it.first, nullptr);
        const SVDamage::Profile prof = SVDamage::ProfileFor(it.first);
        if (own.found)
        {
            ++armoured;
            if (own.armour > toughest_armour)
            {
                toughest_armour = own.armour;
                toughest = it.first;
            }
        }

        // ⚠Capped, and the cap is stated: a silent top-N reads as "that is all
        // of them", which is exactly the wrong impression for a content survey.
        if (printed < 40)
        {
            ++printed;
            if (own.found)
                Msg("~ [gear]   %-34s armour %.2f koeff %.2f hit_fraction %.2f | weakest bone factor %.2f", sect, own.armour, own.koeff, own.hit_fraction, prof.bone_floor);
            else
                Msg("~ [gear]   %-34s UNARMOURED (no bones_protection_sect) | weakest bone factor %.2f", sect, prof.bone_floor);
        }
    }

    Msg("~ [gear] %u creature templates declare a damage or protection table%s; %u of them are armoured", scanned, all ? "" : " (before the filter)", armoured);
    if (printed >= 40)
        Msg("~ [gear] ...only the first 40 shown -- narrow it with sv_npc_templates <substring>");
    if (toughest_armour >= 0.f)
        Msg("~ [gear] toughest template found: '%s' at armour %.2f -- spawn THAT to make a creature harder to kill, an outfit will not", toughest.c_str(), toughest_armour);
    else
        Msg("! [gear] nothing matched declares bones_protection_sect -- in THIS content no creature section ever does");

    // ⭐The lever this report was built to find is not in the sections it just
    // walked. Say where it actually is rather than leaving an empty answer.
    Msg("- [gear] armour here is a property of the MODEL, not the section: see sv_npc_models [filter] for the catalogue and sv_npc_visual to assign one");
}

u32 SVGear::Strip(const xr_vector<u16>& ids, bool bare)
{
    xr_vector<u16> who = ids;
    if (who.empty())
        SVNpc::Ids(who);
    if (who.empty())
    {
        Msg("! [gear] no tracked puppets to strip");
        return 0;
    }

    CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());

    // ⚠Collect first, destroy after. Both destroy paths below mutate the
    // owner's children as they go, so walking that vector while it is being
    // emptied is the classic way to strip half an inventory and call it done.
    xr_vector<u16> doomed;
    u32 kept = 0, before = 0;
    for (u16 id : who)
    {
        xr_vector<Carried> items;
        carried_by(id, items);
        before += u32(items.size());
        for (const Carried& c : items)
        {
            if (!bare && !xr_strcmp(c.section.c_str(), PDA_SECTION))
            {
                ++kept;
                continue;
            }
            doomed.push_back(c.id);
        }
    }

    // ⚠⚠THE trap in this file, and it cost a run. The server keeps TWO maps of
    // the world -- xrServer::entities, which holds only what is ONLINE, and the
    // A-Life registry, which holds everything. GE_DESTROY is processed against
    // the first one: sent for an offline item it finds no entity, returns
    // quietly, and destroys nothing at all. A loadout applied in the same batch
    // as the spawn -- which is exactly when it should be applied -- therefore
    // stripped precisely nothing while reporting success.
    //
    // So each item goes down the path that owns it: the event for an online
    // one (it has clients to be told), and A-Life's own release for an offline
    // one (the same call CSE_ALifeTraderAbstract uses to drop a child that
    // cannot be saved -- it detaches from the parent and frees the id).
    u32 sent = 0, released = 0;
    for (u16 item : doomed)
    {
        CSE_Abstract* e = SVWorld::Find(item);
        if (!e)
            continue;

        CSE_ALifeDynamicObject* dyn = smart_cast<CSE_ALifeDynamicObject*>(e);
        const bool online = dyn && dyn->m_bOnline;

        if (online)
        {
            // The real event rather than the server's destroy handler directly:
            // that one asserts on the sender being a known client, and the
            // normal channel also gets the removal out to anybody watching.
            NET_Packet P;
            CGameObject::u_EventGen(P, GE_DESTROY, item);
            CGameObject::u_EventSend(P);
            ++sent;
        }
        else if (alife && dyn)
        {
            alife->release(e, true);
            ++released;
        }
    }

    // ⭐Count what is GONE, not what was ordered. The first version of this
    // line reported "38 items destroyed" for a strip that removed none of
    // them, and the run it invalidated looked like a successful one.
    u32 after = 0;
    for (u16 id : who)
    {
        xr_vector<Carried> items;
        carried_by(id, items);
        after += u32(items.size());
    }
    const u32 gone = (before > after) ? (before - after) : 0;

    Msg("~ [gear] stripped %u creatures: %u of %u items gone (%u by event, %u released offline), %u PDAs kept", u32(who.size()), gone, u32(doomed.size()), sent, released, kept);
    if (gone < doomed.size())
        Msg("! [gear] %u item%s survived the strip -- they are still carried", u32(doomed.size()) - gone, (doomed.size() - gone == 1) ? "" : "s");

    return gone;
}

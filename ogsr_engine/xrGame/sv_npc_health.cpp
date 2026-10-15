#include "StdAfx.h"
#include "sv_npc_health.h"
#include "sv_damage_model.h"
#include "sv_bone_names.h"
#include "sv_visual_armour.h"
#include "sv_world_lookup.h"
#include "sv_telemetry.h"

#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "Level.h"
#include "alife_space.h"
#include "entity_alive.h"
#include "../Include/xrRender/Kinematics.h"
#include "../xr_3da/bone.h"

#include <map>
#include <mutex>

// Authoritative by default. Unlike the player ledger in sv_health.h -- which
// defaults to detect-only because a wrong bound there kills an innocent
// person -- there is no second opinion to be wrong against here: without this
// the creature simply never loses health. Immortal NPCs are the bug; taking
// the damage is the fix.
int SVNpcHealth::g_sv_npc_health = 2;

namespace
{
struct Entry
{
    float hp{1.f};
    float max{1.f};
    u32 hits{0};
    float taken{0.f};
    bool dead{false};
    // The engine has charged this creature at least once, so this entry is a
    // RECORD of what it spent and not the authority on it. See EngineOwns.
    bool engine_pays{false};
    float bled{0.f}; // health lost between blows, kept apart from what hits cost
};

xr_map<u16, Entry> s_book;
std::recursive_mutex s_lock;

// Every multiplier that went into one hit, kept apart so the report can show
// the arithmetic. A single number that is wrong is indistinguishable from a
// single number that is right.
struct Calc
{
    const char* bone_name{""};
    u16 bone{0};
    float power{0.f};
    float ap{0.f};
    u16 hit_type{0};
    float after_armour{0.f};
    float bone_armour{0.f};
    float hit_fraction{0.f};
    bool armour_found{false};
    shared_str armour_sect; // which protection set, so a report can name it
    bool immunities_from_model{false};
    float mod_mul{1.f}; // the mod's multiply_hit_from_actor, when it applies
    float bone_scale{1.f};
    float immunity{1.f};
    float health_part{1.f};
    float damage{0.f};
};

CSE_ALifeCreatureAbstract* creature_of(CSE_Abstract* e) { return smart_cast<CSE_ALifeCreatureAbstract*>(e); }

// The .ogf a creature is drawn from -- and, more to the point here, the file
// its bone order lives in. Only visual entities have one.
shared_str visual_of(CSE_Abstract* e)
{
    CSE_Visual* v = e ? e->visual() : nullptr;
    return (v && v->get_visual()) ? shared_str(v->get_visual()) : shared_str();
}

// A creature the SERVER simulates rather than a player's body.
//
// ⚠"e->owner == nullptr" is the WRONG test and cost a run: the host's
// in-process client claims ownership of the ENTIRE level when it comes up
// (xrServer_CL_connect.cpp says so in as many words), so every creature on the
// map has an owner and this ledger silently applied to nobody.
//
// The link that actually marks a player's body is the reverse one: a client
// points BACK at its controlled entity. Everything else is scenery, however it
// is owned.
bool ours(CSE_Abstract* e)
{
    if (!e || !creature_of(e))
        return false;
    const xrClientData* c = static_cast<const xrClientData*>(e->owner);
    return !(c && c->owner == e);
}

// ⭐⭐The mod's own multiplier on hits that came from a PLAYER's weapon.
//
// ⚠This file used to refuse to model it, and that was right at the time: the
// engine applied the damage and already knew about the mod, so reproducing it
// here would have been a second authority. That argument died with the guard
// above -- on a headless host the engine charges 0.0000, this ledger is the
// only authority left, and refusing to model the multiplier now means charging
// 1.7x too little for every shot a player fires.
//
// ⭐And it is not "re-implementing the script layer": it is ONE inherited ini
// line (`multiply_hit_from_actor`, defines.ltx, on the weapon base sections)
// that xr_motivator.script reads and applies. Read the same line, get the same
// number. Anything more elaborate the mod does stays the engine's business.
float actor_multiplier(CSE_Abstract* attacker, CSE_Abstract* weapon)
{
    // Only from a player: the mod's rule is "hit FROM ACTOR", and a creature
    // shooting another creature does not get it.
    if (!attacker || ours(attacker) || !weapon)
        return 1.f;

    // 13-10-2026: damage as in the original game -- mod_overrides' xr_motivator/bind_monster no longer apply the line,
    // so the ledger does not either (it has to price a hit the way the engine path does).
    (void)weapon;
    return 1.f;
}

void compute(CSE_Abstract* victim, u16 bone, float power, float ap, u16 hit_type, Calc& c, float attacker_mul = 1.f)
{
    c.bone = bone;
    c.power = power;
    c.ap = ap;
    c.hit_type = hit_type;

    const shared_str sect = victim->s_name;
    const shared_str vis = visual_of(victim);
    c.bone_name = SVBones::Name(vis, bone, victim->ID);
    SVVisualArmour::Prime(vis, victim->ID); // the userdata from the loaded skeleton, not the .ogf

    // 1. The creature's own per-bone armour, exactly as CAI_Stalker::Hit does
    //    it: subtract, then never let the result fall below the section's
    //    guaranteed fraction.
    //    ⚠Armour-piercing follows APPLY_ARMOR_PIERCING_TO_NPC, and this file
    //    MUST track that switch: the engine applies the hit on the owning
    //    machine while this ledger prices it here, and the two disagreeing
    //    about one bullet is the "two authorities" defect all over again.
    //    Enabled 05-08-2026 -- see build_config_defines.h for the measurement
    //    that decided it (five AK magazines into an exoskeleton's head).
    //    ⭐⭐⭐And the section is NOT where that armour lives. The engine reads
    //    bones_protection_sect out of the MODEL's userdata; asking the creature's
    //    section for it -- which is what this did until 05-08-2026 -- finds the
    //    line nowhere in this content, so every creature measured as unarmoured.
    //    Choosing a creature's model IS choosing its armour (sv_visual_armour.h).
    const SVDamage::BoneArmour ba = SVVisualArmour::BoneArmourFor(vis, sect, c.bone_name);
    c.armour_found = ba.found;
    c.armour_sect = SVVisualArmour::For(vis).protection;
    c.bone_armour = ba.armour;
    c.hit_fraction = ba.hit_fraction;

    // ⚠BEFORE armour, exactly where CAI_Stalker::Hit has it: the mod's callback
    // runs on entity_alive_before_hit, so the plate is subtracted from the
    // ALREADY multiplied power. Applying it afterwards would let armour eat a
    // bullet that in fact went through -- which is the whole difference between
    // "an exoskeleton is tough" and "an exoskeleton is immune".
    c.mod_mul = attacker_mul;
    float p = power * attacker_mul;
    if (ba.found && ALife::eHitTypeFireWound == hit_type)
    {
        float armour = ba.armour;
#ifdef APPLY_ARMOR_PIERCING_TO_NPC
        // Byte for byte what CAI_Stalker::Hit does with it (ai_stalker_fire.cpp):
        // the round's ap eats a share of the plate rather than of the damage.
        armour *= (1.f - ap);
#endif
        c.bone_armour = armour; // report what was really subtracted, not the plate's rating
        // ⚠The guaranteed fraction is of the MULTIPLIED power too (`p` before
        // the plate, not the wire number) -- CAI_Stalker::Hit compares against
        // HDS.power, which the mod has already scaled.
        const float floor = p * ba.hit_fraction;
        const float reduced = p - armour;
        p = (reduced < floor) ? floor : reduced;
    }
    c.after_armour = p;

    // 2. Which bone. This is the factor the server could never apply before:
    //    the engine keeps it in CBoneInstance params written by
    //    CDamageManager::init_bones, and a headless host has no bone instances
    //    to write them into -- so HitScale returned a zero and every hit cost
    //    nothing. Read straight from the same ini table instead.
    c.bone_scale = SVDamage::BoneFactor(sect, c.bone_name);
    p *= c.bone_scale;

    // 3. What the condition does with what is left.
    //
    //    ⚠Immunities are read TWICE by the engine and the second read wins:
    //    CEntityAlive::Load takes the section's immunities_sect, then
    //    CAI_Stalker::net_Spawn re-reads from the model's userdata over the top.
    //    Reproducing only the first would make an exoskeleton as fragile as the
    //    creature section it was spawned from.
    const SVDamage::Profile prof = SVDamage::ProfileFor(sect);
    float immunity[ALife::eHitTypeMax];
    std::copy(std::begin(prof.immunity), std::end(prof.immunity), std::begin(immunity));
    c.immunities_from_model = SVVisualArmour::ApplyImmunities(vis, immunity);

    if (hit_type < ALife::eHitTypeMax)
    {
        c.immunity = immunity[hit_type];
        c.health_part = prof.health_part[hit_type];
    }
    p *= c.immunity * c.health_part;

    c.damage = p > 0.f ? p : 0.f;
}

// ⭐⭐⭐Is the engine about to apply this hit itself?
//
// Measured 28-07-2026, live, with dbg_death: a stalker took 0.625 on the wire
// and lost 1.0625 of health, dying from one round in the calf while this ledger
// still had it at 0.375. The 1.7 came from the mod -- xr_motivator.script
// multiplies every magazined-weapon hit by `multiply_hit_from_actor`, declared
// in defines.ltx and inherited by every weapon. It is applied inside
// CAI_Stalker::Hit, on the creature's OWN game object, which the host runs in
// full and which is the thing that calls KillEntity.
//
// So there were two authorities on one value, and the effective damage model
// was whichever reached zero first. This ledger cannot win that argument and
// should not try: it would have to re-implement the mod's script layer, and
// then keep up with it forever. The engine path already knows all of it, and
// runs the SAME code the client runs -- which is what "like single player"
// means.
//
// ⚠What this ledger is still for: a creature with nothing to apply the hit for
// it. That is the case it was born in (a headless host had no skeleton, so
// HitScale returned zero and sixteen point-blank rounds cost nothing), and the
// case that comes back the moment a visual has no bones or the creature is
// simulated offline. So the question asked here is the exact one that decides
// it -- CAN the engine charge for this hit -- and not a proxy for it.
bool engine_applies(u16 id)
{
    if (!g_pGameLevel)
        return false;

    CObject* O = Level().Objects.net_Find(id);
    CEntityAlive* EA = smart_cast<CEntityAlive*>(O);
    if (!EA || !EA->Local() || !EA->g_Alive())
        return false;

    // ⚠⚠⚠THIS USED TO ASK `K->LL_BoneCount() > 0` AND THAT WENT SILENTLY WRONG.
    //
    // The reasoning was sound when written: the damage path reads CBoneInstance
    // params laid out by CDamageManager::init_bones, and a host with no
    // skeleton has no params, hence no damage. Then 28-07-2026 taught the
    // headless host to read real bones out of the .ogf -- and this predicate
    // started answering "yes, the engine can charge" for creatures whose bone
    // params were still never laid out. The engine then charged exactly 0.0000
    // per hit while this ledger politely stood aside.
    //
    // ⭐Measured 05-08-2026 with a human firing an AK point blank: 87 hits on
    // the wire, `taken=0.000`, hp 1.000, every telemetry record reading
    // dmg=0.0000 by="engine". A creature nobody could kill, and no error
    // anywhere -- the same immortality this ledger was built to end, returned
    // through its own guard.
    //
    // ⭐⭐The lesson is one this project has now paid for three times (see the
    // CCar recurrence in the skeleton work): a guard on LL_BoneCount is ALWAYS
    // a proxy for something else, and the something else has to be asked
    // directly. Here the real question is not "does it have bones" but "has
    // this engine ever actually taken health off this creature" -- which is a
    // FACT the ledger already records, in the same field NoteApplied calls
    // "proof, not inference".
    //
    // ⚠Cost of being wrong is now bounded and self-correcting in both
    // directions: at worst one hit is charged by both authorities on a creature
    // the engine does pay for, and NoteApplied overwrites hp with what the
    // engine really left behind on that very hit.
    if (SVNpcHealth::EngineOwns(id))
        return true;

    // ⚠⚠"At worst one hit charged by both" was NOT bounded: that first hit was
    // charged against a book that could be stale, and when the book said zero
    // the ledger force_kill()ed a creature the engine would only have wounded
    // (04-10-2026, in-process host: one AK round to the head, the engine priced
    // it 0.67 of 1.0, the ledger killed). Worse, the engine's hit then landed
    // on a corpse, so CEntityAlive::Hit skipped FightRegister / ATTACK and the
    // mod's on_hit -- the village never learned it had been attacked.
    //
    // So before any proof exists, ask the question the proof stands for:
    // will CDamageManager::HitScale price this hit above zero? init_bones lays
    // a non-zero factor on every bone (zero on the root is an R_ASSERT), and a
    // host that never laid the params out reads the zero construct() left.
    IKinematics* K = smart_cast<IKinematics*>(EA->Visual());
    if (!K || !K->LL_BoneCount())
        return false;
    return K->LL_GetBoneInstance(K->LL_GetBoneRoot()).get_param(0) > 0.f;
}

Entry& book_of(CSE_ALifeCreatureAbstract* cr)
{
    auto it = s_book.find(cr->ID);
    if (it != s_book.end())
        return it->second;

    Entry e;
    // Seeded from what the world says now. For a creature that has never been
    // hit this is its full health; taking it from here rather than from a
    // constant means a wounded spawn stays wounded.
    e.max = e.hp = cr->g_Health() > 0.f ? cr->g_Health() : 1.f;
    return s_book.emplace(cr->ID, e).first->second;
}
} // namespace

void SVNpcHealth::OnHit(NET_Packet& P)
{
    if (!g_sv_npc_health)
        return;

    const u32 saved = P.r_tell();

    u16 victim = 0xffff, attacker = 0xffff, weapon = 0xffff, bone = 0, aim = 0, hit_type = 0;
    Fvector dir, in_bone;
    float power = 0.f, impulse = 0.f, ap = 0.f;

    P.r_u16(victim);
    P.r_u16(attacker);
    P.r_u16(weapon);
    P.r_dir(dir);
    P.r_float(power);
    P.r_u16(bone);
    P.r_vec3(in_bone);
    P.r_float(impulse);
    P.r_u16(aim);
    P.r_u16(hit_type);
    if (ALife::eHitTypeFireWound == hit_type)
        P.r_float(ap);

    P.r_seek(saved);

    CSE_Abstract* e_victim = SVWorld::Find(victim);
    if (!ours(e_victim))
        return; // a player: sv_health.h owns that one, and by opposite rules

    CSE_ALifeCreatureAbstract* cr = creature_of(e_victim);

    // The engine is about to charge for this hit through the content's own
    // path. Charging it here as well is the second authority, and the two
    // disagree by everything the scripts do. NoteApplied() below records what
    // actually happened instead of what this file would have guessed.
    if (engine_applies(victim))
        return;

    Calc c;
    compute(e_victim, bone, power, ap, hit_type, c, actor_multiplier(SVWorld::Find(attacker), SVWorld::Find(weapon)));

    std::lock_guard<std::recursive_mutex> guard(s_lock);
    Entry& b = book_of(cr);
    if (b.dead)
        return;

    ++b.hits;
    b.taken += c.damage;

    if (g_sv_npc_health >= 2)
    {
        b.hp -= c.damage;
        // The world's own copy follows the ledger, so the census, the telemetry
        // and anything else that reads a creature's health see the truth rather
        // than the 1.000 they saw while this was missing.
        cr->s_Health(b.hp);
    }

    // Raw sample, not a counter: "how much damage did an AK do to a stalker's
    // chest" is answerable from these later, and no counter invented today
    // would have anticipated the question.
    {
        string512 fields;
        xr_sprintf(fields, "\"vid\":%u,\"kid\":%u,\"bone\":%u,\"bname\":\"%s\",\"pow\":%.4f,\"dmg\":%.4f,\"hp\":%.4f,\"ht\":%u,\"barm\":%.3f,\"bscale\":%.3f", victim, attacker, bone,
                   c.bone_name, power, c.damage, b.hp, hit_type, c.bone_armour, c.bone_scale);
        SVTelemetry::Raw("dmg", fields);
    }

    if (g_sv_npc_health >= 2 && b.hp <= 0.f)
    {
        b.dead = true;
        Msg("~ [sv_npc_health] %u '%s' killed by %u after %u hits totalling %.3f", victim, e_victim->s_name.c_str(), attacker, b.hits, b.taken);
        if (xrServer* SV = SVWorld::Server())
            SV->force_kill(e_victim, attacker);
    }
    else if (g_sv_npc_health == 1)
    {
        Msg("~ [sv_npc_health] %u '%s' bone %u '%s': %.4f power -> %.4f damage, would leave %.3f", victim, e_victim->s_name.c_str(), bone, c.bone_name, power, c.damage, b.hp - c.damage);
    }
}

void SVNpcHealth::Forget(u16 id)
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    s_book.erase(id);
}

void SVNpcHealth::Reset()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    s_book.clear();
    SVBones::Reset();
    SVVisualArmour::Reset();
}

void SVNpcHealth::NoteApplied(u16 victim, u16 attacker, u16 bone, u16 hit_type, float wire_power, float applied_power, float hp_before, float hp_after)
{
    if (!g_sv_npc_health)
        return;

    CSE_Abstract* e_victim = SVWorld::Find(victim);
    if (!ours(e_victim))
        return; // a player's body: sv_health.h owns that one

    CSE_ALifeCreatureAbstract* cr = creature_of(e_victim);
    if (!cr)
        return;

    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        Entry& b = book_of(cr);

        const float spent = hp_before - hp_after;

        // ⚠Proof means it CHARGED something, not merely that it ran. This said
        // plain `true`, so a hit the engine processed and priced at zero handed
        // the creature to an authority that could not pay -- one such hit was
        // enough to make it immortal for the rest of its life (05-08-2026).
        b.engine_pays = spent > 0.f;

        // ⚠⚠⚠AND THE HEALTH ONLY FOLLOWS AN ENGINE THAT ACTUALLY PAID.
        //
        // This wrote `b.hp = hp_after` unconditionally, which is right when the
        // engine owns the value and catastrophic when it does not: on a headless
        // host every hit is processed twice -- this ledger charges it, then the
        // engine charges 0.0000 and reports the object's untouched health. The
        // second report then OVERWROTE the first.
        //
        // ⭐Measured with a human emptying magazines into an exoskeleton:
        // hits=462, taken=17.235, hp=1.000. Seventeen health-bars' worth of
        // damage correctly computed, correctly subtracted, and handed straight
        // back on the next line. The creature could not be killed, and the
        // ledger's own dump was the only place the contradiction was visible.
        if (b.engine_pays)
        {
            b.hp = hp_after;
            b.taken += spent;
            ++b.hits;
            if (hp_after <= 0.f)
                b.dead = true;
        }

        b.max = hp_before > b.max ? hp_before : b.max;
    }

    // Same raw sample as before, and deliberately the same record name -- what
    // changed is that these are now the numbers that were REALLY spent, not a
    // second opinion about them. ⭐`pow` is what crossed the wire and `dmg` is
    // what the creature paid; the two differing is the content at work (the
    // mod's multiply_hit_from_actor, rank immunity, bone armour), and keeping
    // both is what makes that visible instead of arguable.
    string512 fields;
    xr_sprintf(fields, "\"vid\":%u,\"kid\":%u,\"bone\":%u,\"bname\":\"%s\",\"pow\":%.4f,\"applied\":%.4f,\"dmg\":%.4f,\"hp\":%.4f,\"ht\":%u,\"by\":\"engine\"", victim, attacker, bone,
               SVBones::Name(visual_of(e_victim), bone, e_victim->ID), wire_power, applied_power, hp_before - hp_after, hp_after, hit_type);
    SVTelemetry::Raw("dmg", fields);
}

void SVNpcHealth::NoteDrain(u16 id, float hp_after)
{
    if (!g_sv_npc_health)
        return;

    std::lock_guard<std::recursive_mutex> guard(s_lock);
    // Only creatures already on the books. An unwounded one has nothing to keep
    // in step, and creating an entry per creature per condition tick would turn
    // a ledger of the hurt into a census of the living.
    auto it = s_book.find(id);
    if (it == s_book.end() || it->second.dead)
        return;

    Entry& b = it->second;
    const float lost = b.hp - hp_after;
    if (lost > 0.f)
        b.bled += lost;
    b.hp = hp_after;
    if (hp_after <= 0.f)
        b.dead = true;
}

bool SVNpcHealth::EngineOwns(u16 id)
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    auto it = s_book.find(id);
    return (it != s_book.end()) && it->second.engine_pays;
}

bool SVNpcHealth::LiveHealth(u16 id, float& hp)
{
    if (!g_pGameLevel)
        return false;
    CEntityAlive* EA = smart_cast<CEntityAlive*>(Level().Objects.net_Find(id));
    if (!EA || !EA->Local())
        return false;
    hp = EA->GetfHealth();
    return true;
}

bool SVNpcHealth::IsServerCreature(u16 id) { return ours(SVWorld::Find(id)); }

void SVNpcHealth::PublishHealth(CSE_Abstract* e)
{
    CSE_ALifeCreatureAbstract* cr = creature_of(e);
    if (!cr || !g_pGameLevel)
        return;

    CEntityAlive* EA = smart_cast<CEntityAlive*>(Level().Objects.net_Find(e->ID));
    if (!EA || !EA->Local())
        return;

    // ⚠Publish whichever authority is actually spending the health -- the third
    // place the same mistake lived (05-08-2026). Copying the OBJECT's health
    // into the entity is correct only while the engine is the one taking it
    // off; on a headless host, where it charges nothing, this republished an
    // untouched 1.000 over everything this ledger had subtracted, once per
    // frame, for free.
    float hp = 0.f;
    bool dead = false;
    if (!EngineOwns(e->ID) && Ledger(e->ID, hp, dead))
    {
        cr->s_Health(hp);
        return;
    }

    cr->s_Health(EA->GetfHealth());
}

bool SVNpcHealth::Ledger(u16 id, float& hp, bool& dead)
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    auto it = s_book.find(id);
    if (it == s_book.end())
        return false;
    hp = it->second.hp;
    dead = it->second.dead;
    return true;
}

void SVNpcHealth::Dump()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    Msg("~ [sv_npc_health] mode %d, %u creatures on the books", g_sv_npc_health, u32(s_book.size()));
    for (const auto& e : s_book)
    {
        CSE_Abstract* v = SVWorld::Find(e.first);
        Msg("~ [sv_npc_health]   %u '%s' hp=%.3f/%.3f hits=%u taken=%.3f%s", e.first, v ? v->s_name.c_str() : "?", e.second.hp, e.second.max, e.second.hits, e.second.taken,
            e.second.dead ? " DEAD" : "");
    }
}

void SVNpcHealth::Explain(u16 id, float power, u16 bone)
{
    CSE_Abstract* v = SVWorld::Find(id);
    if (!v)
    {
        Msg("! [sv_npc_health] no entity %u", id);
        return;
    }
    if (!ours(v))
    {
        Msg("! [sv_npc_health] %u is somebody's controlled entity -- sv_health owns it", id);
        return;
    }

    const shared_str vis = visual_of(v);
    Calc c;
    compute(v, bone, power, 0.f, u16(ALife::eHitTypeFireWound), c);

    Msg("~ [sv_npc_health] %u '%s' visual '%s' (%u bones)", id, v->s_name.c_str(), vis.c_str(), SVBones::Count(vis));
    Msg("~ [sv_npc_health]   bone %u = '%s'", bone, c.bone_name[0] ? c.bone_name : "<unresolved>");
    Msg("~ [sv_npc_health]   power %.4f%s", c.power, (c.mod_mul != 1.f) ? " (before the mod's actor multiplier)" : "");
    if (c.mod_mul != 1.f)
        Msg("~ [sv_npc_health]   x mod multiply_hit_from_actor %.2f -> %.4f", c.mod_mul, c.power * c.mod_mul);
    if (c.armour_found)
        Msg("~ [sv_npc_health]   - bone armour %.3f from '%s' (hit_fraction %.3f) -> %.4f", c.bone_armour, c.armour_sect.size() ? c.armour_sect.c_str() : "<section>", c.hit_fraction,
            c.after_armour);
    else
    {
        // ⭐Three different failures used to print the same line, and the one
        // that was actually happening -- asking the section instead of the model
        // -- looked exactly like "this creature has none".
        Msg("~ [sv_npc_health]   - NO ARMOUR: model '%s' declares no bones_protection_sect, and neither does section '%s'", vis.c_str(), v->s_name.c_str());
        if (!SVBones::Count(vis))
            Msg("! [sv_npc_health]     ...and its .ogf could not be read at all -- bone names are unresolved too");
    }
    Msg("~ [sv_npc_health]   x bone scale  %.3f", c.bone_scale);
    Msg("~ [sv_npc_health]   x immunity    %.3f%s", c.immunity, c.immunities_from_model ? " (overridden by the model)" : "");
    Msg("~ [sv_npc_health]   x health part %.3f", c.health_part);
    Msg("~ [sv_npc_health]   = %.4f damage; rank immunity is NOT modelled (the one client multiplier left out)", c.damage);

    std::lock_guard<std::recursive_mutex> guard(s_lock);
    auto it = s_book.find(id);
    if (it != s_book.end())
        Msg("~ [sv_npc_health]   on the books at hp=%.3f/%.3f", it->second.hp, it->second.max);
}

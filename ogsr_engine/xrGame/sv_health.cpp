#include "StdAfx.h"
#include "sv_health.h"

#include "sv_damage_model.h"
#include "sv_hit_trail.h"
#include "sv_world_lookup.h"

#include "xrServer.h"
#include "xrServer_Objects.h"
#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer_Objects_ALife_Items.h"
#include "GameObject.h"
#include "Hit.h"

// Knobs -- see sv_health.h for the model they steer.
//
// Default 1 (detect and log) rather than 2: the ledger is only ever fed by
// entities a NETWORK client controls, so a single-player session never
// reaches this code, but the floor still has to be calibrated against real
// play before it is allowed to kill anybody. The same staging the movement
// validator shipped with.
int g_sv_health_authority = 1;

// Health a client may regain per second without the server seeing why.
// Covers outfit regeneration, artefacts, script trickle, and the plain
// rounding of a value that travels as a float -- anything slow. It must sit
// above every legitimate slow heal and far below the pace of a firefight,
// or a client that reports full health forever stops being mortal.
float g_sv_health_regen = 0.02f;

// Safety multiplier on the computed damage floor. 1.0 means "trust the
// floor as derived"; lower it if calibration shows honest players being
// clamped, raise it only once the floor is known to be too generous.
float g_sv_health_damage_scale = 1.f;

// sv_health_bone_floor lives with the rest of the content reading, in
// sv_damage_model.cpp.

namespace
{
constexpr u32 OUTFIT_REFRESH_MS = 10000; // how often a victim's armour is re-resolved
constexpr u32 MAX_TRACKED = 128; // players; the ledger is not for the NPC population

// How long a client may insist it is alive after the server has declared it
// dead before the ledger starts over. There is no respawn event to hook yet
// (the design's loop anomaly is not built), and a returning player must not
// stay pinned at -1 forever. A client that is merely lying only buys itself
// this window: the floor drains the health it just claimed all over again.
constexpr u32 DEAD_GRACE_MS = 30000;

// A charged hit is not a divergence yet. The server drains the ledger the
// moment it relays the hit, while the client only reports the loss a round
// trip later, so every ordinary exchange of fire opens a gap the width of the
// hits in flight -- which would otherwise fire a warning on every firefight
// and drown the one signal this log exists for.
//
// ⚠The excuse is bounded by the AMOUNT still in flight, never by time alone.
// A time window would be open continuously during sustained fire, which is
// exactly when a cheat matters: it would grant immunity precisely in combat.
constexpr u32 HIT_IN_FLIGHT_MS = 1500;



// ---------------------------------------------------------------------
// The ledger
// ---------------------------------------------------------------------
struct Ledger
{
    u16 entity{0xffff};
    bool seeded{false};
    bool dead{false};
    float hp{1.f};
    u32 last_ms{0};
    u32 dead_ms{0};

    u16 last_attacker{0xffff};
    u32 hit_ms{0}; // when the ledger last charged a hit the client has yet to see
    float flight{0.f}; // how much of that charge the client has not answered for yet
    u32 hits{0};
    float drained{0.f}; // total the floor has taken off, for calibration
    float reported{1.f}; // what the client last claimed, next to which drained is read
    float lost{0.f}; // total the client has actually admitted losing

    float bled{0.f}; // health lost with no hit behind it, for reporting only

    u32 gap_events{0};
    u32 gap_since{0};
    float worst_gap{0.f};

    SVDamage::Armour armour;
    u32 armour_ms{0};

    SVDamage::Belt belt;
    u32 belt_ms{0};
    bool belt_warned{false};
};

xr_vector<Ledger> s_ledger;

Ledger* find_ledger(u16 id)
{
    for (Ledger& L : s_ledger)
        if (L.entity == id)
            return &L;
    return nullptr;
}

Ledger& ledger_for(u16 id)
{
    if (Ledger* L = find_ledger(id))
        return *L;
    if (s_ledger.size() >= MAX_TRACKED)
        s_ledger.erase(s_ledger.begin()); // oldest out; players are few and long-lived
    s_ledger.push_back(Ledger());
    s_ledger.back().entity = id;
    return s_ledger.back();
}

const SVDamage::Armour& armour_for(Ledger& L, CSE_Abstract* e, u32 now)
{
    if (!L.armour_ms || (now - L.armour_ms) > OUTFIT_REFRESH_MS)
    {
        SVDamage::ResolveArmour(e, L.armour);
        L.armour_ms = now;
    }
    return L.armour;
}

const SVDamage::Belt& belt_for(Ledger& L, CSE_Abstract* e, u32 now)
{
    if (!L.belt_ms || (now - L.belt_ms) > OUTFIT_REFRESH_MS)
    {
        SVDamage::ResolveBelt(e, L.belt);
        L.belt_ms = now;

        // A belt that zeroes the floor switches this check off for that player,
        // and it does so legitimately: the content really can make a full belt
        // absorb a bullet completely (sv_belt_audit prints the arithmetic). The
        // server has no way to be stricter without killing honest players, so
        // the answer is not to clamp it but to refuse to let it happen quietly
        // -- an inert check that still logs "gaps=0" is the worst outcome here.
        if (!L.belt_warned && L.belt.found && L.belt.k[ALife::eHitTypeFireWound] <= 0.f)
        {
            L.belt_warned = true;
            Msg("! [sv_health] entity %u carries %u artefacts that zero the bullet floor (%u belt slots) -- the ledger cannot constrain this player's health while they hold them",
                L.entity, L.belt.carried, L.belt.slots);
        }
    }
    return L.belt;
}

// A LOWER bound on the health this hit must have cost, i.e. every factor
// resolved the way that favours the victim. Mirrors the order in
// CEntityCondition::ConditionHit: armour, then immunity, then the share of
// the hit that goes to health, then the bone factor where that applies.
float damage_floor(Ledger& L, CSE_Abstract* victim, u16 hit_type, float power, u32 now)
{
    if (hit_type >= ALife::eHitTypeMax || power <= 0.f)
        return 0.f;

    // Neither of these reaches health at all: radiation accumulates, psy hits
    // spend psy-health. Charging the ledger for them would kill honest players
    // standing in a radioactive room.
    if (hit_type == ALife::eHitTypeRadiation || hit_type == ALife::eHitTypeTelepatic)
        return 0.f;

    float d = power;

    // Artefacts come first, exactly as on the client: CActor::Hit runs
    // HitArtefactsOnBelt on the raw power and only then hands the result to
    // the outfit. Applying it here rather than at the end costs nothing (these
    // are all multiplications) but keeps the order readable against Actor.cpp.
    d *= belt_for(L, victim, now).k[hit_type];

    const SVDamage::Armour& armour = armour_for(L, victim, now);
    if (armour.found)
    {
        if (hit_type == ALife::eHitTypeFireWound)
        {
            // HitThruArmour subtracts bone armour but never below this share.
            d *= armour.hit_fraction;
        }
        else
        {
            float prot = armour.protection[hit_type];
            clamp(prot, 0.f, 1.f);
            // GetHitTypeProtection scales protection by a per-bone factor that
            // is at most 1, so 1 - protection is the strongest reduction the
            // outfit can possibly apply.
            d *= (1.f - prot);
        }
    }

    const SVDamage::Profile p = SVDamage::ProfileFor(victim->s_name);
    d *= p.immunity[hit_type];
    d *= p.health_part[hit_type];

    // Only the wound-shaped hits are scaled by the bone; strike, shock and
    // explosion are not (see the switch in ConditionHit).
    if (hit_type == ALife::eHitTypeBurn || hit_type == ALife::eHitTypeFireWound || hit_type == ALife::eHitTypeWound || hit_type == ALife::eHitTypeWound_2)
        d *= p.bone_floor;

    d *= g_sv_health_damage_scale;
    return (d > 0.f) ? d : 0.f;
}
} // namespace

void SVHealth::OnHit(NET_Packet& P)
{
    if (!g_sv_health_authority)
        return;

    // Layout is SHit::Write_Packet_Cont's, read from where the queued event
    // left its cursor (victim id first, see xrServer::Process_event GE_HIT).
    const u32 saved = P.r_tell();

    u16 victim = 0xffff, attacker = 0xffff, weapon = 0xffff, bone = 0, aim = 0, hit_type = 0;
    Fvector dir, in_bone;
    float power = 0.f, impulse = 0.f;

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

    P.r_seek(saved); // the real handler still has to read this packet

    // Only entities already under a network client's control have a ledger.
    // Everything else -- the whole NPC population -- is simulated by the host
    // and has nothing to lie about.
    Ledger* L = find_ledger(victim);
    if (!L || L->dead)
        return;

    CSE_Abstract* e_victim = SVWorld::Find(victim);
    if (!e_victim)
        return;

    const float d = damage_floor(*L, e_victim, hit_type, power, Device.dwTimeGlobal);
    const u32 now = Device.dwTimeGlobal;
    if (L->hit_ms && (now - L->hit_ms) > HIT_IN_FLIGHT_MS)
        L->flight = 0.f; // the previous burst has had its chance to come back

    L->hp -= d;
    L->drained += d;
    L->flight += d;
    L->hit_ms = now;
    ++L->hits;
    if (attacker != 0xffff && attacker != victim)
        L->last_attacker = attacker;
}

bool SVHealth::Report(u16 id, Account& out)
{
    const Ledger* L = find_ledger(id);
    if (!L || !L->seeded)
        return false;

    out.hp = L->hp;
    out.reported = L->reported;
    out.dead = L->dead;
    out.hits = L->hits;
    out.drained = L->drained;
    out.lost = L->lost;
    out.last_attacker = L->last_attacker;
    out.since_hit_ms = L->hit_ms ? (Device.dwTimeGlobal - L->hit_ms) : 0;
    out.bled = L->bled;
    return true;
}

void SVHealth::NoteDrain(u16 id, float hp_after)
{
    if (!g_sv_health_authority)
        return;

    Ledger* L = find_ledger(id);
    if (!L || !L->seeded || L->dead)
        return;

    // Counted against what the client last told us, because that is the only
    // number here that tracks real health. L.hp is a floor and is deliberately
    // left alone -- see the header.
    const float lost = L->reported - hp_after;
    if (lost > 0.f)
        L->bled += lost;
}

SVHealth::Verdict SVHealth::Reconcile(CSE_ALifeCreatureAbstract* creature, xrClientData* CL)
{
    Verdict v;
    if (!creature)
        return v;

    const float reported = creature->fHealth;
    v.health = reported;

    if (!g_sv_health_authority)
        return v;

    Ledger& L = ledger_for(creature->ID);
    const u32 now = Device.dwTimeGlobal;

    // First sight of this entity: the client's own number is the only state
    // anybody has, so it becomes the starting point.
    if (!L.seeded)
    {
        L.seeded = true;
        L.hp = reported;
        L.reported = reported;
        L.last_ms = now;

        // Resolve the damage model now rather than on the first hit: it costs
        // a handful of ini reads, and printing what it found is the only way
        // to tell a floor built from real content apart from one that quietly
        // fell back to defaults because the section names did not match.
        const SVDamage::Profile p = SVDamage::ProfileFor(creature->s_name);
        Msg("~ [sv_health] tracking entity %u ('%s') at %.3f -- fire_wound: immunity %.2f, health part %.2f; bone floor %.2f", creature->ID, creature->s_name.c_str(), reported,
            p.immunity[ALife::eHitTypeFireWound], p.health_part[ALife::eHitTypeFireWound], p.bone_floor);
        return v;
    }

    // Kept for calibration: "drained" is what the floor charged, "lost" is what
    // the client says it actually cost. The ratio between them is the number
    // sv_health_damage_scale has to be set from.
    // Dying is not damage taken: health goes to -1 on death, and counting that
    // drop would put a whole extra point into every calibration figure.
    if (reported < L.reported && reported >= 0.f)
        L.lost += (L.reported - reported);
    L.reported = reported;

    if (L.dead)
    {
        if (g_sv_health_authority < 2 || reported <= 0.f)
            return v; // detect-only, or the client agrees it is dead

        if (!L.dead_ms)
            L.dead_ms = now;
        if ((now - L.dead_ms) < DEAD_GRACE_MS)
        {
            v.health = -1.f; // dead for everyone else, whatever it claims
            return v;
        }

        // Insisting this long is what a respawn looks like from in here.
        Msg("! [sv_health] entity %u kept claiming %.3f health for %.0fs after dying -- re-seeding the ledger", L.entity, reported, float(now - L.dead_ms) / 1000.f);
        L.dead = false;
        L.dead_ms = 0;
        L.hits = 0;
        L.drained = 0.f;
        L.gap_events = 0;
        L.gap_since = 0;
        L.hp = reported;
        L.last_ms = now;
        return v;
    }

    // Regeneration allowance. Bounded on both sides for the same reason the
    // movement budget is: a burst of packets in one tick must still buy some,
    // and a client that went quiet must not come back with a minute of it.
    float dt = float(now - L.last_ms) / 1000.f;
    clamp(dt, 0.f, 5.f);
    L.last_ms = now;
    L.hp += g_sv_health_regen * dt;
    if (L.hp > 1.f)
        L.hp = 1.f;

    if (reported <= L.hp + EPS_L)
    {
        // The client says it is worse off than the server can prove. It knows
        // its own armour, bone and artefacts, so it is believed -- and the
        // ledger re-anchors to the truth, which is what keeps the floor's
        // inaccuracy from ever accumulating against an honest player.
        L.hp = reported;
        L.flight = 0.f; // the client has answered for whatever was outstanding
        L.gap_events = 0;
        L.gap_since = 0;
        if (reported <= 0.f)
        {
            L.dead = true; // died on its own; nothing left to enforce
            L.dead_ms = now;
        }
        return v;
    }

    const float gap = reported - L.hp;

    // Damage the client cannot have seen yet explains the gap by itself. Note
    // the excuse expires by AMOUNT: once a client owes more than was ever put
    // in flight, no volume of incoming fire hides it.
    if (L.hit_ms && (now - L.hit_ms) >= HIT_IN_FLIGHT_MS)
        L.flight = 0.f;
    if (gap <= L.flight + EPS_L)
        return v; // still in transit, nothing proven -- and nothing enforced

    if (gap > L.worst_gap)
        L.worst_gap = gap;
    if (!L.gap_since)
        L.gap_since = now;
    ++L.gap_events;

    // Throttled, and odd: with several clients an even modulus locks onto one
    // of them forever.
    if ((L.gap_events % 31) == 1)
        Msg("! [sv_health] client %u ('%s') claims %.3f health, server accounts for %.3f (gap %.3f, worst %.3f, %.0fs, %u hits took %.3f)%s", CL ? CL->ID.value() : 0,
            (CL && CL->ps) ? CL->ps->getName() : "?", reported, L.hp, gap, L.worst_gap, float(now - L.gap_since) / 1000.f, L.hits, L.drained,
            (g_sv_health_authority < 2) ? " (detect only)" : "");

    if (g_sv_health_authority < 2)
    {
        // Detection only. The ledger is deliberately NOT pulled up to the
        // claim: leaving it low is what makes the log show how far the two
        // have drifted, and drift is the number this needs calibrating
        // against. It self-corrects the moment the client reports a loss.
        return v;
    }

    v.health = L.hp;
    if (L.hp <= 0.f)
    {
        L.dead = true;
        L.dead_ms = now;
        v.health = -1.f;
        v.force_death = true;
        v.killer = L.last_attacker;
    }
    return v;
}

void SVHealth::OnEaten(u16 entity_id)
{
    Ledger* L = find_ledger(entity_id);
    if (!L || L->dead)
        return;

    // What was eaten and how much it heals is not worth modelling: granting
    // the full bar is the conservative reading, and the item itself was real
    // -- the server watched it change hands and be destroyed.
    L->hp = 1.f;
    L->gap_events = 0;
    L->gap_since = 0;
}

void SVHealth::OnDeath(u16 entity_id)
{
    if (Ledger* L = find_ledger(entity_id))
    {
        L->dead = true;
        L->dead_ms = Device.dwTimeGlobal;
    }
}

void SVHealth::Reset()
{
    s_ledger.clear();
    SVDamage::ResetCache();
    SVDamage::ResetPlaces();
    // The tape is keyed by entity id like everything else here, and ids are
    // only meaningful inside one world.
    SVHitTrail::Reset();
}

void SVHealth::Drain(float amount)
{
    if (s_ledger.empty())
    {
        Msg("- [sv_health] nothing is being tracked -- no network client has reported state yet");
        return;
    }
    for (Ledger& L : s_ledger)
    {
        if (L.dead)
            continue;
        L.hp -= amount;
        L.drained += amount;
        Msg("~ [sv_health] injected %.3f damage into entity %u -- ledger now %.3f", amount, L.entity, L.hp);
    }
}

namespace
{
// The nearest armed creature that is not the victim, so an injected hit gets
// the same attribution a real one would: a killer to credit and a weapon for
// the kill record to name.
//
// ⚠The attacker MUST be one the server has online. game_sv_GameState's
// GAME_EVENT_ON_HIT resolves it through the online entity map and silently
// drops the whole hit when the lookup fails -- an offline shooter produces a
// hit that no client ever sees, which is exactly how this was found.
void find_shooter(CSE_Abstract* victim, u16& attacker_id, u16& weapon_id)
{
    attacker_id = victim->ID; // self-inflicted unless somebody better turns up
    weapon_id = 0xffff;
    xrServer* SV = SVWorld::Server();
    if (!ai().get_alife() || !SV)
        return;

    float best = flt_max;
    for (const auto& it : ai().alife().objects().objects())
    {
        CSE_ALifeDynamicObject* O = it.second;
        if (!O || O->ID == victim->ID)
            continue;
        if (!SV->ID_to_entity(O->ID))
            continue; // offline: the relay would throw this hit away
        CSE_ALifeCreatureAbstract* c = smart_cast<CSE_ALifeCreatureAbstract*>(O);
        if (!c || !c->g_Alive())
            continue;

        const float d = O->o_Position.distance_to(victim->o_Position);
        if (d >= best)
            continue;

        u16 wpn = 0xffff;
        for (u16 child_id : O->children)
            if (CSE_Abstract* child = SVWorld::Find(child_id))
                if (smart_cast<CSE_ALifeItemWeapon*>(child))
                {
                    wpn = child_id;
                    break;
                }
        if (0xffff == wpn)
            continue; // unarmed: a hit from it would name no weapon

        best = d;
        attacker_id = O->ID;
        weapon_id = wpn;
    }
}
} // namespace

void SVHealth::InjectHit(float power, u16 hit_type, u16 bone, float ap)
{
    if (s_ledger.empty())
    {
        Msg("- [sv_health] nothing is being tracked -- no network client has reported state yet");
        return;
    }
    if (hit_type >= ALife::eHitTypeMax)
        hit_type = u16(ALife::eHitTypeFireWound);

    for (Ledger& L : s_ledger)
    {
        if (L.dead)
            continue;
        CSE_Abstract* victim = SVWorld::Find(L.entity);
        if (!victim)
            continue;

        u16 attacker_id = 0xffff, weapon_id = 0xffff;
        find_shooter(victim, attacker_id, weapon_id);

        // Bone 0 is the root by default: CDamageManager refuses to let content
        // give the root a zero hit factor, so a test hit there always does
        // something. Any other bone id is passed through untouched -- what a
        // given id means is the victim's skeleton's business, and comparing
        // what the client loses across ids is how the server learns it.
        Fvector dir;
        dir.set(0.f, 0.f, 1.f);
        Fvector in_bone;
        in_bone.set(0.f, 0.f, 0.f);

        // ap rides in the packet for bullets only, and it is what decides how
        // much of the victim's armour is ignored -- so this is the knob that
        // makes an armour-piercing round measurably different from a ball one.
        SHit hit(power, dir, nullptr, bone, in_bone, 0.f, ALife::EHitType(hit_type), ap, false);
        hit.GenHeader(u16(GE_HIT) & 0xffff, L.entity);
        hit.whoID = attacker_id;
        hit.weaponID = weapon_id;

        NET_Packet np;
        hit.Write_Packet(np);
        CGameObject::u_EventSend(np);

        CSE_Abstract* e_wpn = SVWorld::Find(weapon_id);
        Msg("~ [sv_health] injected a %s hit of %.3f ap=%.2f on entity %u bone %u, from %u with '%s' -- ledger was %.3f", ALife::g_cafHitType2String(ALife::EHitType(hit_type)),
            power, ap, L.entity, bone, attacker_id, e_wpn ? e_wpn->s_name.c_str() : "bare hands", L.hp);
    }
}

void SVHealth::InjectPlace(bool to_belt)
{
    if (s_ledger.empty())
    {
        Msg("- [sv_health] nothing is being tracked -- no network client has reported state yet");
        return;
    }

    u32 sent = 0;
    for (Ledger& L : s_ledger)
    {
        CSE_Abstract* owner = SVWorld::Find(L.entity);
        if (!owner)
            continue;

        for (u16 child_id : owner->children)
        {
            CSE_Abstract* child = SVWorld::Find(child_id);
            if (!child)
                continue;
            LPCSTR sect = child->s_name.c_str();
            if (!sect || !pSettings->section_exist(sect) || !SVDamage::SectionIsArtefact(sect))
                continue;

            // The same three lines CUIInventoryWnd::SendEvent_Item2Ruck sends
            // when a player drags an artefact off their belt: same event id,
            // same destination, same u16 body, same channel. Nothing about the
            // server's path can be right here and wrong for a real drag.
            NET_Packet P;
            CGameObject::u_EventGen(P, to_belt ? GEG_PLAYER_ITEM2BELT : GEG_PLAYER_ITEM2RUCK, L.entity);
            P.w_u16(child_id);
            CGameObject::u_EventSend(P);
            ++sent;

            Msg("~ [sv_health] told the world entity %u moved '%s' (%u) to the %s", L.entity, sect, child_id, to_belt ? "belt" : "ruck");
        }
    }

    if (!sent)
        Msg("- [sv_health] no artefacts on any tracked entity to move");
}

void SVHealth::Dump()
{
    Msg("~ [sv_health] authority=%d regen=%.3f/s damage_scale=%.2f bone_floor=%.2f -- %u tracked", g_sv_health_authority, g_sv_health_regen, g_sv_health_damage_scale,
        g_sv_health_bone_floor, u32(s_ledger.size()));

    // Who the server is actually broadcasting to. When the ledger and a client
    // disagree about damage, the first question is whether the events reached
    // that client at all -- SendBroadcast_LL silently skips anyone whose
    // bConnected is not set, so a client can be joined, exporting state and
    // counted as a player while receiving no events whatsoever.
    if (xrServer* SV = SVWorld::Server())
        for (u32 i = 0; i < SV->client_Count(); ++i)
        {
            xrClientData* C = (xrClientData*)SV->client_Get(i);
            if (!C)
                continue;
            Msg("~ [sv_health]   client %u '%s' local=%d connected=%d ready=%d entity=%d", C->ID.value(), C->ps ? C->ps->getName() : "?", C->flags.bLocal ? 1 : 0,
                C->flags.bConnected ? 1 : 0, C->net_Ready ? 1 : 0, C->owner ? int(C->owner->ID) : -1);
        }
    for (const Ledger& L : s_ledger)
    {
        CSE_Abstract* e = SVWorld::Find(L.entity);
        // floor/lost is the calibration ratio: 1.0 would mean the floor charges
        // exactly what the hits really cost, and anything above 1.0 means it is
        // charging MORE than reality -- the direction that hurts honest players.
        const float ratio = (L.lost > EPS_L) ? (L.drained / L.lost) : 0.f;
        string128 armour;
        if (L.armour.found)
            xr_sprintf(armour, "fw_prot=%.2f hit_frac=%.3f", L.armour.protection[ALife::eHitTypeFireWound], L.armour.hit_fraction);
        else
            xr_strcpy(armour, "none");

        // The belt is the factor with no visible symptom when it is wrong: a
        // floor too high by it looks exactly like a cheating client, so what it
        // resolved to has to be printed next to the ratio it explains.
        // `-N ruck` is the part worth watching: it only moves when a client has
        // actually told the server it stowed something, which is the one signal
        // that the placement feed is alive at all.
        string128 belt;
        if (L.belt.found)
            xr_sprintf(belt, "%u af/%u slots fw_k=%.2f -%u ruck", L.belt.carried, L.belt.slots, L.belt.k[ALife::eHitTypeFireWound], L.belt.excluded);
        else
            xr_sprintf(belt, "none/%u slots -%u ruck", L.belt.slots, L.belt.excluded);

        // ⭐`bled` sits next to `drained` on purpose: together they are the whole
        // of what left this body, and apart they are the difference between
        // "damage the server carried" and "damage nothing here ever saw". A
        // death report that cannot separate them blames the wrong thing.
        Msg("~ [sv_health]   id=%u '%s' ledger=%.3f%s client=%.3f hits=%u drained=%.3f bled=%.3f lost=%.3f floor/lost=%.2f gaps=%u worst=%.3f armour[%s] belt[%s]", L.entity,
            e ? e->name_replace() : "?", L.hp, L.dead ? " DEAD" : "", L.reported, L.hits, L.drained, L.bled, L.lost, ratio, L.gap_events, L.worst_gap, armour, belt);
    }
}

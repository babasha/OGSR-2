#pragma once

// =====================================================================
//  Server-authoritative health ("no godmode").
//
//  Health has always been the client's word. The owning client applies
//  every hit to itself and then reports the result in its update packet;
//  the server writes that number down and tells everyone else. A client
//  that simply never lowers the number is immortal, and in a world where
//  death costs the whole inventory that is the one cheat that breaks the
//  design outright.
//
//  This keeps a server-side ledger per network-controlled creature:
//
//    * every hit the server relays drains the ledger by a LOWER BOUND on
//      the damage that hit must have done (see damage_floor);
//    * an update may take the ledger DOWN to whatever the client reports
//      -- the client knows its armour, its bone, its artefacts, so when
//      it says it lost more than the floor, it is believed and the ledger
//      re-anchors to the truth;
//    * an update may only take the ledger UP by what the server can
//      account for: a slow regeneration allowance plus the healing items
//      it saw the client consume.
//
//  The re-anchoring is what makes this safe for honest players. As long
//  as the floor never exceeds the damage really taken, the ledger sits at
//  or above true health at all times, so an honest client is never
//  clamped -- while a client that under-reports diverges by the floor on
//  every hit and eventually reaches zero, where the server kills it.
//
//  What this deliberately does NOT do is reproduce the damage pipeline.
//  Every factor is the most victim-favourable value derivable from content,
//  because a floor that is occasionally too generous only lets a cheater
//  live longer, while one that is ever too harsh kills somebody who did
//  nothing wrong. The reading of those factors lives in sv_damage_model.h,
//  along with the audits that show what it found -- this file is only the
//  ledger built on top.
//
//  Console knobs (registered in console_commands.cpp):
//    sv_health_authority     0 = off, 1 = detect and log, 2 = enforce
//    sv_health_regen         health per second a client may gain unexplained
//    sv_health_damage_scale  safety multiplier on the computed floor
//    sv_health_dump          print the ledger
// =====================================================================

class NET_Packet;
class CSE_Abstract;
class CSE_ALifeCreatureAbstract;
class xrClientData;

namespace SVHealth
{
struct Verdict
{
    float health{1.f}; // what the server is willing to publish for this entity
    bool force_death{false}; // ledger hit zero and the client has not died on its own
    u16 killer{0xffff};
};

// GE_HIT as queued by the server. Reads the packet without leaving its
// read cursor moved, exactly like SVTelemetry::OnHit.
void OnHit(NET_Packet& P);

// Called once per update from a network client, after UPDATE_Read has put
// the client's claimed fHealth into the entity. Returns what the server
// accepts; the caller writes it back into the entity.
Verdict Reconcile(CSE_ALifeCreatureAbstract* creature, xrClientData* CL);

// What this ledger holds for one entity, for code that has to report on a
// death. False when no network client's body by that id is tracked -- the
// normal answer for the whole NPC population.
//
// ⚠⭐It exists because the death report used to ask sv_npc_health about EVERY
// victim, players included, and players are never on that book. The line it
// printed -- "ledger never saw it hit" -- was therefore true of every player
// death ever recorded, whether or not the server had seen the damage, and it
// was read for a week as evidence of a hole. There are two ledgers because
// there are two authority models (see the header comment); a report that knows
// about only one of them cannot tell "the server missed this" from "I asked
// the wrong book".
struct Account
{
    float hp{1.f}; // what the server can account for
    float reported{1.f}; // what the client last claimed
    bool dead{false};
    u32 hits{0}; // hits the floor has charged for
    float drained{0.f}; // ...and what they cost in total
    float lost{0.f}; // what the client has admitted losing
    u16 last_attacker{0xffff};
    u32 since_hit_ms{0}; // how long ago the last charge was; 0 = never charged
    float bled{0.f}; // health that left with no hit behind it
};
bool Report(u16 id, Account& out);

// Health this body lost without a hit -- bleeding, and anything else the
// condition update spends between blows.
//
// ⚠⚠IT DOES NOT MOVE THE FLOOR, and must not. The ledger's whole safety
// argument is that it never sits BELOW true health; subtracting a drain the
// client may not have applied yet would break that, and in enforcing mode a
// floor below the truth kills an honest player. This only counts, so a death
// can be explained.
//
// ⭐⭐WHY IT EXISTS, and it is the same lesson twice in one evening: the
// creature ledger was blind to bleeding and its watchdog accused an ordinary
// death of being a second authority. Measured 30-07-2026 at a PLAYER's death --
// three bullets took them to 0.2700 and BLEEDING took the rest. The report said
// "the server carried 1 hit(s)" and named a 0.0495 stumble as the last thing
// that happened, which reads as the cause and was not. Had that stumble not
// landed, the "no hit passed through the server" watchdog would have fired on a
// completely legitimate death.
//
// ⭐The general rule: an instrument that only hears about BLOWS cannot explain
// a death in a world where things also bleed -- it will either accuse the
// innocent or credit the wrong killer, and it did both today.
void NoteDrain(u16 id, float hp_after);

// GEG_PLAYER_ITEM_EAT: the server saw this entity consume something, so an
// unexplained jump in its health is now explained.
void OnEaten(u16 entity_id);

// The entity is confirmed dead -- stop tracking it.
void OnDeath(u16 entity_id);

// Level change or server shutdown.
void Reset();

// sv_health_dump
void Dump();

// sv_health_drain: take health off every tracked ledger without a hit having
// happened. Fault injection -- the enforcement path (clamp, then the server
// killing an entity whose client will not die) is otherwise unreachable
// without a second client willing to cheat, and it is the part that touches
// the world's death bookkeeping.
void Drain(float amount);

// sv_hit_test: send a real GE_HIT at every tracked entity, built with the
// engine's own SHit and dispatched down the same channel a fired bullet
// uses. Unlike Drain this exercises the MEASUREMENT side end to end -- the
// packet layout the parsers assume, the damage floor, the telemetry hit
// record -- and, because the hit is relayed to the victim's client like any
// other, it also produces the one number calibration needs: what the client
// really lost next to what the floor charged.
void InjectHit(float power, u16 hit_type, u16 bone, float ap);

// sv_place_test 0|1: send a real GEG_PLAYER_ITEM2RUCK / ITEM2BELT for every
// artefact a tracked entity owns, built and dispatched exactly as the
// inventory window builds it when a player drags one. The placement feed is
// the only thing that tells a belt from a rucksack (see sv_damage_model.h),
// and dragging is something only a human at a client can do -- so this is how
// the server's half of it gets exercised without one.
void InjectPlace(bool to_belt);
} // namespace SVHealth

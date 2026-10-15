#pragma once

// =====================================================================
//  WHAT DAMAGE ACTUALLY WENT THROUGH THE SERVER
//
//  Written 30-07-2026 for one line that could not be read: a player death
//  arrived carrying `cse_hp=1.0000; ledger never saw it hit`, and there was
//  no way to tell whether that meant "the killing blow bypassed the server"
//  or "the report asked the wrong book". Both are consistent with the log,
//  and they are opposite findings -- one is a hole in the authority model,
//  the other is a defect in the instrument.
//
//  ⭐So this records the hits at the ONE place every hit passes through
//  (GAME_EVENT_ON_HIT), on both of its exits: the ones the server relayed and
//  the ones it dropped. A death can then be put next to the damage that
//  reached its victim, and "nothing reached them" becomes a measurement
//  instead of an inference. That distinction is worth a ring buffer:
//  "nothing is there" is the one answer that also comes back from a broken
//  instrument, so it has to be produced by something that demonstrably
//  records when there IS something.
//
//  ⚠It is deliberately NOT the ledger. sv_health.h keeps a floor under what a
//  client claims and sv_npc_health.h keeps a creature's real health; both
//  decide things. This decides nothing and forgets everything after a few
//  hundred hits -- it is a tape, and its only job is to be believable.
//
//  ---------------------------------------------------------------------
//  The second question it answers: WHICH HIT TYPES CAN A CLIENT CLAIM?
//
//  The power cap in game_sv_base only inspected eHitTypeFireWound. Every other
//  type -- strike, wound, explosion, burn, shock, chemical -- was relayed with
//  whatever number arrived. Capping them blind would refuse honest damage
//  (knives, grenades, anomalies, falls), so the census came first: per type,
//  how many arrived, how big they got, and -- the part that decides whether a
//  cap is needed at all -- whether they came from a NETWORK client or from
//  the server's own simulation of the world.
//
//  ⭐30-07-2026: the caps landed (sv_damage_model.h), and the census grew the
//  column that makes them arguable -- how many claims the ceiling could be
//  computed for at all. A cap that reaches nothing and a cap that refuses
//  nothing produce the same silence, and only that column tells them apart.
//
//  Console:
//     sv_hit_trail [id]  the tape, all of it or one victim's
//     sv_hit_types       the per-type census
//     sv_hit_types_reset
//     sv_cap_audit       what ceiling the CONTENT declares, per type
// =====================================================================

class NET_Packet;

namespace SVHitTrail
{
// One hit at the server's entry point, with the cursor left where it was.
// `dropped` = the server refused to carry it, so nobody took this damage.
// `from_network` = a remote client claimed it, as opposed to the host's own
// simulation of monsters, anomalies and scripts.
void Note(NET_Packet& P, bool dropped, bool from_network, u32 sender);

// What the power gate made of one hit. `known` = a ceiling could be computed
// for this (source, type) pair at all; `over` = the claim sat above it.
//
// ⭐Kept apart from Note because it answers a different question. Note records
// what arrived; this records whether anything was in a position to refuse it.
// Only filled while sv_hit_validate is on -- with the gate off, nothing asks.
void NoteCap(u16 hit_type, bool from_network, bool known, bool over);

// Hits the server RELAYED at this victim within the window. Dropped ones do
// not count: they cost the victim nothing, and counting them would answer
// "was there damage" with a hit that never arrived.
u32 Count(u16 victim, u32 within_ms);

// The same, printed one line each, under the caller's tag.
void Print(u16 victim, u32 within_ms, const char* tag);

// sv_hit_trail -- 0xffff for every victim.
void Dump(u16 victim);

// sv_hit_types / sv_hit_types_reset
void Types();
void ResetTypes();

// Level change or server shutdown.
void Reset();
} // namespace SVHitTrail

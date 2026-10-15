#pragma once

// =====================================================================
//  DID THEY ACTUALLY HIT ANYTHING?
//
//  Asked from play, 30-07-2026, after a session that ended with the player
//  dead: "а они когда стреляли по мне вообще промахивались?" -- and the logs
//  could not answer it. The server publishes shots and records deaths; between
//  those two it knows nothing. So "they never miss" and "they miss constantly
//  and got lucky twice" are the same log.
//
//  Both halves already pass through one place each and neither was counted:
//    * shots -- SVNpc::PublishShot, once per shot the server broadcasts
//    * hits  -- GAME_EVENT_ON_HIT, the single entry point for every hit,
//               already parsed there for the power cap
//
//  ⚠A HIT IS NOT A SHOT, and the ratio is not a percentage without saying so:
//    * one bullet can register TWICE -- through a thigh and then a shin. The
//      second carries dmg 0.0000, which is why zero-power hits are counted
//      apart rather than dropped.
//    * a shotgun fires one shot and sends a hit PER PELLET.
//    * a hit whose attacker the server has offline is dropped before it
//      reaches anyone, and dropped damage that is not recorded as dropped
//      reads exactly like damage that landed.
//  So the table reports shots, hits, scoring hits and drops separately, and
//  leaves the arithmetic to whoever is asking. A single "accuracy %" would be
//  wrong for half the weapons in the game.
//
//  Console:
//     sv_accuracy        print the table (does not reset)
//     sv_accuracy_reset  start a fresh window
// =====================================================================

namespace SVAccuracy
{
// One shot left the barrel, as the server understands it.
void Shot(u16 shooter, u16 weapon);

// One hit event arrived. `power` is the value ON THE WIRE, before the content's
// own multipliers -- the only number the server sees at this point. `dropped`
// means it never reached a client, so it must not be read as damage delivered.
void Hit(u16 attacker, u16 victim, u16 bone, float power, bool dropped);

void Dump();
void Reset();
} // namespace SVAccuracy

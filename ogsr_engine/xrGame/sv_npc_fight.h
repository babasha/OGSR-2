#pragma once

// Make two creatures fight, and say why they would not have.
//
// ⚠Measured first, 27-07: two armed stalkers of hostile communities spawned on
// the same spot did not exchange a single shot in 70 seconds -- and it is NOT
// because their brains are idle. They walked 77 m apart under their own power
// (the server's driven counter stayed at 0.0 m), so movement, scheduling and
// A-Life all work. They simply never treated each other as enemies.
//
// ⭐So this hands the enemy over through the engine's OWN channel rather than
// faking a shot: CMemoryManager::make_object_visible_somewhen is what
// CAgentEnemyManager uses to share a target across a squad. Aiming, burst
// length, cover and reloading then stay the stalker's own business -- a
// server-puppeteered trigger would measure my arithmetic instead of the game's.
//
// It also reports is_relation_enemy in both directions, because if the content
// says they are not enemies then no amount of visibility will start a fight,
// and that distinction is the whole diagnosis.
class CCustomMonster;

namespace SVFight
{
// Introduce two creatures as enemies. Symmetric by default: both learn about
// the other, so the exchange is a fight and not an execution.
void Attack(u16 attacker, u16 target, bool mutual = true);

// Every tracked puppet, introduced to its nearest neighbour. ⚠This is the form
// a script should use: naming a creature by id is a guess that has already been
// wrong twice in this harness (ids move with whatever items spawned alongside).
void AttackAll();

// ---------------------------------------------------------------------
//  Why it is not shooting.
//
// "Zero shots" is one observation with a dozen possible causes, and the last
// two attempts at it each spent a build proving one guess wrong. A stalker's
// trigger sits at the end of a long chain -- see somebody, judge them an
// enemy, plan combat, raise the weapon, decide the shot is worth taking --
// and every link can fail silently.
//
// ⭐So this walks the WHOLE chain and prints each link's state in one pass:
// eyes (feel_vision), memory, enemy selection, the two GOAP planners, the
// object handler's weapon state machine, and the fire gate itself. One run
// then says which link is broken instead of one run per hypothesis.
//
// 0xffff = every tracked puppet.
void Explain(u16 id);

// The same block once a second for `seconds`, printed from the creature's own
// scheduled update.
//
// ⚠It has to be sampled there, not from the console: the console runs on the
// main thread while AI state is written from the object scheduler, and the
// interesting failures here are transient (an enemy that is selected for one
// update and dropped by the next). Sampling in the AI's own context makes the
// reading both race-free and correctly timed.
void Watch(int seconds);

// Hook for the above. A no-op unless Watch armed it, so an unwatched creature
// pays one comparison.
void WatchTick(CCustomMonster* monster);

// ---------------------------------------------------------------------
//  What a hundred of them are thinking, in one block.
//
// ⭐Explain() is the right instrument for ONE creature and the wrong one for a
// crowd: fifteen lines each, times a hundred, is a log nobody reads. The
// questions a crowd raises are different in kind anyway -- not "why is this one
// silent" but "are they all doing the same thing, and where are they going?"
//
// ⚠Measured 28-07, and this is what the instrument is for: fifty stalkers
// walked off on a bearing of 118 degrees, all three runs within two degrees of
// each other, and nothing in any existing report said where to or on whose
// orders. Three runs and a coordinate spreadsheet answered "they moved"; none
// of them answered "why". A histogram of the current GOAP action plus the
// destinations they are actually steering for answers it in one run.
//
// Grouped by community, because the interesting comparison is nearly always
// between the two sides and not across the pile.
//
// ⭐⭐It walks the WHOLE ONLINE WORLD, not the harness roster. It used to walk
// SVNpc::Ids, which was right while the puppets were the only creatures with
// anything to report and became wrong the moment the leash came off: with
// A-Life running the level the roster is empty, and the census answered "no
// tracked puppets" for a map with hundreds of creatures on it. ⚠It also cast
// every one of them to CAI_Stalker, so even the right population would have
// come back missing every mutant -- which is most of what is alive out there.
// Mutants have their own branch: a state machine instead of two GOAP planners,
// no mental state, no weapon.
void Census();

// ⭐One creature just changed its mind about where its weapon belongs.
//
// Printed at the EVENT rather than sampled, because that is what the question
// is: a stalker standing in a quiet field draws its rifle and puts it back
// every half-minute, and a once-a-second snapshot of a world that is mostly
// unchanged cannot say which input moved. This prints the inputs AT the flip --
// brain action, the goal the hands were given, mental state, enemy -- which is
// the shortest possible answer to "who asked for this".
void NoteStrapFlip(CObject* creature, bool strapped);

// The same block on a timer, sampled from the creatures' own update.
//
// ⭐One census says what they are doing; a sequence says what they DECIDED and
// when. "Everyone was idle, then at t+31 s four of them picked an enemy and
// three seconds later two were in the combat planner" is a story; two snapshots
// at either end of a run are two nouns.
void CensusWatch(int seconds, int period_ms = 1000);
} // namespace SVFight

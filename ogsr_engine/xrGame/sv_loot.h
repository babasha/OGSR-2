#pragma once

// =====================================================================
//  Items lying in the world, and who ends up owning them.
//
//  Two halves of one question, which is why they share a file.
//
//  ⭐The half that is a TOOL: sv_item_drop puts a real item on the ground
//  at a point. Until now every item this server created was born already
//  parented to somebody -- spawn_supplies at a creature, sv_npc_give into
//  an inventory -- so the one flow the extraction design is actually built
//  on ("a thing lies there; somebody goes and takes it") had never been
//  exercised deliberately, only as a side effect of corpses.
//
//  ⭐⭐The half that is SECURITY: an item changing hands is the single
//  point where the server decides who owns what, and in a persistent world
//  that is the most attacked decision there is. xrServer::Process_event_
//  ownership checks exactly three things today:
//
//    1. the item exists,
//    2. it has no parent yet,
//    3. the packet came either from the server's own client or from the
//       client that owns the TAKER.
//
//  Which means a client cannot make somebody ELSE pick something up -- but
//  it can make its own actor take ANY unowned item in the world, from any
//  distance, with no line of sight and no travel. Nothing in the server
//  looks at where the two of them are.
//
//  ⚠So this does NOT start by enforcing a rule. It starts by writing down
//  every transfer with the distance it happened at, because a threshold
//  invented before the distribution is known is a threshold that will
//  either let a cheat through or break honest looting -- and both failures
//  look identical from the server's side. Enforcement is a knob here
//  (sv_loot_max_dist), it is OFF by default, and it should stay off until
//  the report below has a shape.
//
//  ⭐What refusing actually costs, measured rather than assumed: NOTHING to
//  undo. Ownership here is GRANTED, not asserted -- a client asks and waits,
//  and the item enters an inventory only when the server broadcasts the take
//  back. So the gate below is a complete enforcement mechanism on its own; it
//  does not need a compensating packet, and sending one is actively fatal
//  (see the note in xrServer_process_event_ownership.cpp).
//
//  ⚠What "the report has a shape" means, precisely, and it is not done yet:
//  picking a thing up off the ground is only ONE of the flows through
//  Process_event_ownership. Looting a corpse, buying from a trader and
//  emptying a container all arrive as the same event, and only the first has
//  been measured (0.00 - 2.44 m). A gate set from those numbers alone would
//  refuse a legitimate trade.
// =====================================================================

class CSE_Abstract;

namespace SVLoot
{
// sv_item_drop: put `count` items of `section` on the ground at `at`, snapped
// to the AI graph so they land on a floor somebody can walk to rather than
// inside it. Ids are remembered so the report can say how many were taken.
void Drop(const char* section, int count, const Fvector& at, float spread);

// sv_item_list: what we dropped, and whether it is still lying there.
void List();

// sv_item_give: put an item straight into somebody's inventory, parented at
// spawn, for ANY entity -- a player included.
//
// ⭐⭐This exists because dropping it on the ground is not enough, and the
// reason is worth writing down. Measured 28-07-2026 with a live client: this
// mod replaces the engine's pickup entirely. Its actor_on_item_before_pickup
// callback ALWAYS returns false and instead schedules the take itself from a
// HUD animation (ogsr_actor_animation.script), so if its own gate declines --
// no HUD motion allowed, a blocked slot, input restricted -- nothing is sent
// at all and the server never learns that anybody tried. A player standing on
// a rifle simply cannot pick it up, and no server-side log says why.
//
// The server owning "you now have this" is also what the extraction design
// needs: rewarding, restocking and restoring a player's kit cannot depend on a
// client-side animation succeeding.
//
// ⚠SVNpc::Give is NOT this: it only walks the tracked puppet roster, so it
// cannot address a player at all.
void GiveTo(u16 owner_id, const char* section, int count);

// --- the ledger ------------------------------------------------------

// An item gained an owner. Called from the one place in the server where that
// can legitimately happen. `event_type` separates the flows -- see the note on
// xrServer::Process_event_ownership.
void OnTaken(CSE_Abstract* item, CSE_Abstract* new_owner, bool from_server_client, u16 event_type);

// ...and the refusals, which are the more interesting half for security: each
// one is a client asking for something the server would not give it.
enum class Refusal
{
    NoSuchItem, // the id names nothing
    AlreadyOwned, // the item already has a parent
    NotYourClient, // the sender does not own the taker
    TooFar // sv_loot_max_dist, when it is on
};
void OnRefused(u16 item_id, u16 taker_id, Refusal why);

// ⭐The gate itself, asked BEFORE the transfer is allowed. Returns false to
// refuse. With sv_loot_max_dist at 0 (the default) it always returns true and
// only measures.
//
// ⭐⭐It also refuses to police a flow nobody has measured. Reaching for a thing
// on the ground and taking one out of a corpse were both measured at arm's
// length (0.00-2.44 m and 0.76-1.31 m), so a distance rule means something for
// them. A PURCHASE has never produced a single sample here -- there is no
// trader in any of these tests -- and "how far from a shopkeeper may you stand"
// is a different question with a different answer. Enforcing a number invented
// for one flow on another is how an anti-cheat rule starts refusing honest
// players, so buying is exempt until somebody measures it.
bool AllowTake(CSE_Abstract* item, CSE_Abstract* new_owner, u16 event_type);

// sv_item_report / sv_item_log
void Report();
void Reset();
} // namespace SVLoot

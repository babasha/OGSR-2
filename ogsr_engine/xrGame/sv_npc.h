#pragma once

// =====================================================================
//  Server-driven NPCs.
//
//  The world already has stalkers in it, but nobody on the server side
//  can point at one and tell it what to do. Everything they do is stock
//  AI deciding for itself, which is fine for a single-player mod and
//  useless for a persistent world where the server is supposed to be
//  the author: the design's convoy escorts, anomaly-point guards and
//  the interceptors that make travelling alone a bad idea all need a
//  creature the SERVER moves and equips, not one that wandered there.
//
//  This is that handle. A puppet is an ordinary spawned stalker whose
//  position the server asserts every tick, the same primitive the
//  XROS_NPC_WALK_TEST hook proved end to end: ForceTransform on the
//  online object, and the existing entity-update path carries it to
//  every client, animation included -- the client picks walk or stand
//  from how fast the thing is moving (net_puppet_locomotion), so
//  driving at a walking pace is all it takes to look like walking.
//
//  ⚠What this deliberately is NOT is pathfinding. A puppet walks in a
//  straight line towards where it was told to go, with its height taken
//  from the AI graph so it follows the floor instead of the horizon. It
//  will walk through a wall if you aim it at one. Real navigation means
//  handing the order to the stalker's own movement manager and letting
//  its brain argue with us about it; that is a separate decision, and
//  straight-line authority is what the server-authoritative design
//  actually asks for first.
//
//  Console (registered in console_commands.cpp):
//    sv_npc_spawn [section] [count]   put puppets next to a player
//    sv_npc_list                      what exists and what it is doing
//    sv_npc_del <id|all>
//    sv_npc_tp <id> <x y z|actor>     move it now
//    sv_npc_move <id> <x y z|actor>   walk it there
//    sv_npc_follow <id> [entity]      keep walking towards someone
//    sv_npc_stop <id>
//    sv_npc_give <id> <section>       gear, ammo, outfit, anything
//    sv_npc_speed <id> <m/s>
// =====================================================================

// ⭐How a creature is CARRYING ITSELF, packed into one byte and sent beside
// what it holds (GE_NPC_ACTIVE_ITEM).
//
// ⚠The gap: the stalker animation set is indexed by posture, not just by
// speed. Its names are built as <state>torso_<weapon slot>_<action>_<n> with
// state in {cr_, norm_, dmg_norm_} -- so crouching in cover, reloading and
// drawing a weapon are DIFFERENT ANIMATIONS, and a remote client that knows
// only "which way and how fast" plays a standing man with a raised rifle
// through all three. Measured 29-07-2026: the server had five creatures in
// take_cover while every one of them stood upright on the client.
//
// Polled with the weapon: posture is a STATE (it lasts), unlike the shot, which
// is an event and needs GE_NPC_SHOT.
//
// ⚠⚠u16, and it MATTERS. This said `enum : u8` while the field it describes has
// always been written and read as a u16 -- harmless for the first five bits and
// silently fatal for the sixth: `npc_posture_strapped = 1 << 9` is 512, does not
// fit in the fixed underlying type, and MSVC truncated it to ZERO rather than
// refusing it. The sender then OR'd in nothing and the receiver masked with
// nothing, so a bit that was set on one side and tested on the other was never
// on the wire at all -- and both halves of the code read as correct.
// ⭐Caught by the measurement (no [strap] line on the client), never by reading.
enum : u16
{
    npc_posture_crouch = 1 << 0,
    npc_posture_wounded = 1 << 1,

    npc_posture_mental_shift = 2, // 2 bits
    npc_mental_free = 0,
    npc_mental_danger = 1,
    npc_mental_panic = 2,
    npc_posture_mental_mask = 0x3,

    npc_posture_action_shift = 4, // 3 bits
    npc_action_idle = 0,
    npc_action_aim = 1,
    npc_action_reload = 2,
    npc_action_unstrap = 3, // drawing
    npc_action_strap = 4, // putting away
    npc_posture_action_mask = 0x7,

    // walk / run / stand, same values as MonsterSpace::EMovementType so the
    // byte passes straight through to the animator
    npc_posture_move_shift = 7, // 2 bits
    npc_posture_move_mask = 0x3,

    // ⭐The weapon is ON ITS BACK, as opposed to being put there right now.
    //
    // A STATE, not an action, and that is why it needs a bit of its own next to
    // the action field above: `strap` there means the man is in the middle of
    // slinging it, and lasts a second; this lasts until something gives him a
    // reason to draw. Without it a copy saw the putting-away and then went
    // straight back to drawn -- see CObjectHandler::m_net_strapped.
    //
    // ⚠Bit 9. The field is written as u16 (PublishHands) even though the
    // comment above calls it a byte, so there was room; anything past bit 15
    // would need the packet to grow.
    npc_posture_strapped = 1 << 9,
};

class CAI_Stalker;

namespace SVNpc
{
// What a puppet is currently being told to do. There is no queue: an
// order replaces whatever came before it, because the point of this is a
// handle a human turns in real time, not a script to be composed.
enum class Order
{
    Hold, // stay put
    Goto, // walk to a fixed point, then Hold
    Follow, // keep walking towards another entity, stopping short of it
    Patrol, // walk a closed route of stops, for ever
    Guard // stay within a leash of a point; otherwise its own creature
};

// sv_npc_spawn: `count` puppets of `section`, ringed around a player so
// they do not land inside one another. `community` is a faction name from
// game_relations/communities (dolg, freedom, bandit, stalker, ...) and may
// be null to leave whatever the section's character profile decides.
//
// ⚠Faction is settable only at spawn: the entity resolves it from its
// profile the first time and an online creature has already handed it to
// the relation system.
void Spawn(const char* section, int count, const char* community);

// The same, at a point somebody chose, reporting what was created.
//
// This is the form content-authoring code wants: a human at a console spawns
// next to themselves and then talks to "all", but a scenario placing five
// guard details on five anomaly fields needs to tell each detail apart, and
// the only handle on a creature is the id it got.
void SpawnAt(const char* section, int count, const char* community, const Fvector& at, xr_vector<u16>& out_ids);

// sv_npc_visual <who> <model>: what a creature is DRAWN as -- and, in this
// content, what armour it has.
//
// ⭐⭐⭐The two are one decision. bones_protection_sect is declared by the
// model's .ogf userdata and by no creature section here, so dressing a guard
// in an exoskeleton model is what makes it hard to kill; giving it an outfit
// item does nothing at all (the item never reaches an outfit slot). See
// sv_visual_armour.h for how that was established.
//
// Offline creatures take the change in their spawn packet, which is the clean
// path. An online one is already drawn on every client, so it also needs the
// wire message -- see Redress below.
//
// Returns how many creatures were changed.
u32 SetVisual(const xr_vector<u16>& ids, const char* visual);

// sv_npc_list
void List();

// sv_npc_del: one puppet, or every one this module spawned.
void Remove(u16 id, bool all);

// Move now, versus walk there. Both end with the puppet holding.
void Teleport(u16 id, const Fvector& pos);
void OrderGoto(u16 id, const Fvector& pos);

// Keep walking towards an entity, stopping short of it. 0xffff picks a
// player automatically -- with one connected that is what is meant.
void OrderFollow(u16 id, u16 target);

void OrderHold(u16 id);

// sv_npc_face: point them at something.
//
// ⚠⚠This exists because NOTHING in the spawn path sets a creature's angle.
// alife->spawn_item leaves o_Angle at zero and CGameObject::net_Spawn builds
// the transform straight from it, so every creature a test places faces the
// same absolute direction -- which in a two-anchor skirmish means one side is
// looking at the enemy and the other is looking at the horizon. Measured
// 28-07-2026: the side on the far anchor acquired an enemy within seconds
// while the near side reported "no enemy" for the whole run and walked off,
// and that was read as an AI failure for three days.
//
// ⭐Effective at SPAWN time, while the creature is still offline: the angle is
// what the object is built from. Once it is online its own brain owns where it
// looks, and this can only shove the transform.
void OrderFace(u16 id, const Fvector& at);

// sv_npc_patrol: walk these stops in order, for ever, pausing `dwell` seconds
// at each. Fewer than two stops is a Goto.
//
// This is what makes a level feel occupied rather than decorated: a route that
// crosses a path a player wants to take is an encounter that happens because
// of where they went, not because something spawned near them.
void OrderPatrol(u16 id, const xr_vector<Fvector>& stops, float dwell);

// sv_npc_guard: keep within `leash` metres of a point.
//
// ⭐The order deliberately does NOT drive the creature while it is inside the
// leash -- it is released to its own brain, so it idles, looks around and
// fights like any stalker, and the server only intervenes when it has wandered
// too far. That split (the server owns WHERE, the creature owns WHAT IT DOES)
// is the whole reason a guard on an anomaly point can be authored by the
// server without turning it into a mannequin.
void OrderGuard(u16 id, const Fvector& anchor, float leash);

// sv_npc_give: spawn an item already parented to the puppet, the same
// call the settlement layer uses to arm what it populates.
//
// `announce` off is for callers that hand out a whole loadout: one line per
// item per creature is forty lines for a ten-man test, and the summary the
// caller prints instead says the same thing.
void Give(u16 id, const char* section, bool announce = true);

// sv_npc_speed: metres per second. Above roughly 2 the client's puppet
// locomotion reads it as a run.
void SetSpeed(u16 id, float mps);

// Where a bare "actor" argument resolves to: the entity of the first
// connected network client, falling back to the host's own. Returns
// 0xffff when there is nobody.
u16 ReferenceEntity();

// Ids of every tracked puppet. For layers that need to act on the roster
// without owning it -- and so that a test never has to name a creature by
// number, which is a guess: ids shift with whatever items spawned alongside.
void Ids(xr_vector<u16>& out);

// ⭐⭐⭐A stalker takes its order HERE, from its own update on the main thread.
//
// A human under a moving order is not carried by this module any more. Update
// decides where it should go and in which gear; the creature's own movement
// manager takes it there, under engine script control -- the same mechanism a
// single-player scene uses. Path, velocity table, turning, sight and the
// animator then all read ONE state, which is the whole point: the old drive
// moved the body behind the brain's back, and the brain's animator played what
// the brain believed. Measured 22-09-2026: body and brain headings more than
// 30 degrees apart on half of a guard's clip changes.
//
// An enemy hands the creature back to its brain for the fight; the order
// resumes when it is over. A scene some other script holds is left alone.
//
// Pulled by the creature rather than pushed by Update because Update runs on
// the network thread, and script control is brain state.
void Steer(CAI_Stalker* S);

// The same, narrowed to one community.
//
// ⭐⭐Two sides cannot be given two different orders without this. Every order
// here takes "an id or all", which is the right shape for a single squad and
// the wrong one for a test with two of them: `sv_npc_guard all <point>` posts
// BOTH factions on one spot. Measured 28-07-2026 -- the skirmish test could not
// be written at all until an order could name a side.
void IdsOfFaction(const char* community, xr_vector<u16>& out);

// One step of every outstanding order. Called from xrServer::Update.
void Update();

// ⭐Did the world honour the last despawns? `sv_npc_del` posts an event and
// prints "removed N" two milliseconds later, which is a request and was being
// read as an outcome -- live 05-08-2026 one of twenty-three creatures kept its
// object and lost everything else, and nothing said so for ten minutes. Called
// from Update; reports, does not repair.
void VerifyRemovals();

// Diagnostic hook: report where a watched creature's position is seen at a
// named point in the frame. A no-op unless sv_npc_watch names that id, so it
// costs one compare on the paths it is called from.
//
// ⚠This exists because three plausible explanations for a puppet walking at a
// third of its ordered speed were all wrong, and each cost a build-and-run to
// disprove. Sampling the position at the points that could be writing it
// answers the question in one run instead.
void WatchPos(u16 id, const char* tag, const Fvector& pos);

// ⭐Tell every client what the server's creatures are HOLDING.
//
// ⚠The gap this fills, found from play 28-07-2026: a stalker shot the player
// dead with an AK-74U from nine metres while appearing unarmed on their screen.
// The weapon was replicated -- the client had loaded its model -- but which
// item sits in a creature's hands is not on the wire anywhere: the only slot
// event in the protocol comes from the player's own inventory window and is
// handled by CActor alone, and the server entity of an item carries no slot at
// all. A remote copy's brain does not run, so nothing activates it either.
//
// An enemy who shoots but looks unarmed is not a cosmetic defect: it is an
// unfair fight, and on a server-authoritative build it reads as the server
// cheating.
//
// Polled rather than hooked: the state lives in CInventory, which changes from
// half a dozen places (the AI's object handler, scripts, death, item transfer),
// and a poll cannot be forgotten by the next thing that moves it.
void PublishHands();

// ⭐Tell every client that one of the server's creatures FIRED.
//
// Hooked, not polled, and that is forced: a shot is an instant, and the state
// it leaves behind (one fewer round) is not something a 5 Hz poll can turn
// back into "there were three shots, 90 ms apart". PublishHands can be a poll
// precisely because what it reports is a state; this reports an event.
//
// Called from CWeapon::FireTrace -- the one place every firearm passes through
// with the real fire point and direction in hand, once per shot.
void PublishShot(u16 shooter, u16 weapon, const Fvector& P, const Fvector& D);

// The posture bits for one creature, or 0 for anything that is not a stalker,
// read from the same facts the single-player animator reads.
//
// `action_state` comes back as the object handler's RAW current action id --
// not the compressed action above. The compressed form is for a client that
// picks cycle names by hand; the raw one is for a client that drives the
// shipped animator, which switches on exactly this number. Sending both costs
// two bytes and lets the two paths be compared against each other live.
u16 PostureOf(CObject* O, u16& action_state);

// Level change or shutdown -- the ids meant something only in that world.
void Reset();
} // namespace SVNpc

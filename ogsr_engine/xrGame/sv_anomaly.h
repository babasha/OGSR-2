#pragma once

// =====================================================================
//  Anomalies the SERVER puts on the map.
//
//  The design's economic loop starts at an anomalous field: it makes the
//  artefact, so it is the thing worth holding and the reason two clans
//  fight over a patch of ground (see the concept notes). sv_scenario can
//  already FIND the fields a level shipped with and post guards on them,
//  but nobody could add one, move one, or make one bigger -- an anomaly
//  was a fact of the level's spawn file, authored once in the SDK and
//  frozen. A persistent world whose operator cannot change where the
//  danger is has no levers on its own map.
//
//  This is that lever. Every verb here is server-side and available to an
//  administrator over rcon, so the same commands work typed on the host
//  console and typed by an admin standing in the world.
//
//  ⭐WHAT MAKES AN ANOMALY BIG IS ITS SHAPE, not its section. Section
//  (zone_mincer, zone_witches_galantine, ...) decides what it DOES: which
//  hit type, which particles, which artefacts it can birth. How far it
//  reaches is a sphere in CSE_Shape::shapes, which the SDK authored per
//  instance and which travels to clients inside the spawn packet
//  (CSE_ALifeSpaceRestrictor::STATE_Write -> cform_write). So placing one
//  means creating the entity, giving it a sphere, and only then
//  registering it -- alife().spawn_item takes `registration = false`
//  exactly for this.
//  ⚠A zone registered with no shape at all is not an error anybody sees:
//  CSpaceRestrictor::net_Spawn quietly substitutes a 1 m ball and logs a
//  line. That is a fully working anomaly the size of a football, which
//  reads as "the spawn did not take" rather than as a missing shape.
//
//  Console (registered in console_commands.cpp):
//    sv_anomaly_kinds [substring]     which sections of this content are zones
//    sv_anomaly_list [radius]         what stands near you, and how big it is
//    sv_anomaly_spawn <section> [radius] [x y z]
//    sv_anomaly_del <id|mine>
// =====================================================================

namespace SVAnomaly
{
// sv_anomaly_kinds: the sections this content declares with a zone class.
//
// ⭐Read from pSettings rather than listed here, because the answer is a
// property of the INSTALLED CONTENT and every mod changes it. A hardcoded
// list would be right on the machine it was written on and quietly wrong
// everywhere else -- the same reason sv_scenario finds fields instead of
// carrying coordinates.
void Kinds(const char* filter);

// sv_anomaly_list: every zone on the loaded level within `radius` of the
// caller, with its actual reach and whether we are the ones who put it there.
//
// ⚠The level filter is not optional: the A-Life registry holds the whole
// game's spawn, every level at once. Without it a listing on Pripyat
// cheerfully measures distances to anomalies in Cordon.
void List(float radius);

// sv_anomaly_spawn: one anomaly of `section`, reaching `radius` metres.
//
// `at` may be null, which means "where the person who typed this is
// standing" -- see Origin(). That is the ergonomic that matters: an
// operator walks to the spot they want the thing and names it, instead of
// reading coordinates off a debug overlay and typing them back in.
void Spawn(const char* section, float radius, const Fvector* at);

// sv_anomaly_size: change how far a zone reaches, live, without respawning it.
//
// ⭐Two writes, not one, and both are needed. The entity's own shape is
// updated so a client that connects LATER is told the new size the ordinary
// way; a GE_ZONE_RESHAPE event carries it to the clients that already hold a
// copy, because the shape travels only inside the spawn packet and there is
// no second sentence in the protocol for it.
//
// `id` may be 0xffff with `all_placed`, which resizes everything this module
// put down -- the shape of the command an operator wants after deciding a
// whole field is too timid.
void Resize(u16 id, bool all_placed, float radius);

// sv_anomaly_art: artefacts inside a zone, now.
//
// The half of the design's loop that has been missing since sv_scenario
// started posting guards on fields: the fields were worth holding in theory
// and had nothing on them in practice. A section of "" means "let the zone's
// own artefact table decide", which is what makes this usable as content
// rather than as a test fixture.
void SpawnArtefacts(u16 zone_id, const char* section, int count);

// sv_anomaly_del: one by id, or every one this module placed.
//
// ⚠Deliberately refuses to delete a zone the LEVEL shipped unless it is
// named by id: "mine" removing the map's own anomalies would be an
// unrecoverable edit to the world made by a command that reads like a
// cleanup.
void Remove(u16 id, bool all_placed);

// Where the administrator who is running this command stands.
//
// Resolved from g_admin_actor_account (set for the duration of an rcon
// command in xrServer_Connect.cpp) so that two admins in the world each
// place things at their own feet. Falls back to the reference player when
// the command was typed on the host console, where there is no author.
bool Origin(Fvector& out);

// Ids of every anomaly this module placed, for the layers that persist or
// report on them without owning the roster.
void Placed(xr_vector<u16>& out);

// The one placed most recently, or 0xffff.
//
// ⭐This is what makes the toolkit usable in a sentence: "put one here, now
// make it bigger, now fill it" is three commands about the same thing, and
// requiring the operator to read an id out of the log between each of them is
// the difference between a tool and a procedure. Spawned ids are also not
// guessable -- they shift with whatever else spawned alongside.
u16 LastPlaced();

// ⭐The placed anomalies, written down, per level.
//
// A field an operator laid out and that vanishes on the next restart is not a
// field: it is a demo. In a persistent world the anomalies ARE the map, so
// they have to outlive the process the same way the map does.
//
// ⚠A file of its own, deliberately -- NOT console variables and NOT user.ltx.
// This server never writes user.ltx at all (it has no clean exit), and the
// project has already been bitten three times by state that persisted through
// a config file nobody expected to be written. A file whose only content is
// "these zones, here, this big" can be read, diffed and deleted by hand.
//
// Save() is called after every change, so an operator never has to remember to.
void Save();

// ⭐⭐sv_anomaly_bake: publish what is placed right now into the CONTENT, so
// every server lays out the same map.
//
// Save() writes a working copy into this machine's runtime folder. That makes
// an operator's work survive a restart HERE, and it does nothing at all for the
// next server -- which has no such file and no way to get one. The world an
// admin lays out is level design; level design belongs in gamedata, next to
// everything else that describes the map, where it can be shipped, diffed and
// reviewed.
//
// Clients need nothing: anomalies are server entities and reach every player
// through the ordinary spawn packet. "On all servers, for all clients" is
// therefore one file in one place.
//
// ⚠Refuses to bake an empty set over an existing file. Clearing a map is a
// deliberate act and must not be reachable by typing a save command at the
// wrong moment.
void Bake();

// Put back what Save() wrote for the loaded level. Returns how many were
// placed.
//
// ⭐Reads the baked content file first and the working copy second, skipping
// anything the working copy repeats -- the two overlap by design, and placing a
// zone twice leaves two anomalies in one spot that nobody can tell apart later.
//
// ⚠Refuses when zones have already been placed in this world, unless `force`:
// running it twice is otherwise a silent way to double every anomaly on the
// map, and the second copy is indistinguishable from the first.
u32 Load(bool force);

// ⭐⭐Fields produce artefacts on their own, with nobody standing in them.
//
// The engine's own cycle cannot do this and it is not a bug: CCustomZone only
// leaves its idle state while something is INSIDE it, so a stock anomaly is a
// hazard you walk into rather than a place that makes something. This design
// needs the other shape -- "walk to the field, take what it made, carry it
// home" only works if the field made something before you got there.
//
// Rate and ceiling are both knobs (sv_anomaly_birth, sv_anomaly_birth_cap) and
// both are load-bearing: a rate on its own leaves a quiet field sitting on a
// fortune, which is the same thing as having no economy.
void Birth();

// sv_anomaly_birth_status: what each producing field is holding and when it
// goes again. ⭐Also the only thing that distinguishes "production is off" from
// "no section in this content has an artefact table" -- two states that produce
// identical silence.
void BirthStatus();

// The artefact sections the producing fields of the loaded level can make
// (the union of their tables), for the load-time model prewarm. Empty while
// production is off. The ALife thread, like Birth().
void LevelArtefactSections(xr_vector<shared_str>& out);

// One-shot autoload, driven from xrServer::Update.
//
// ⚠Not done at level-load time: the A-Life registry, the level graph and the
// cross table all have to be up before a zone can be filed anywhere, and the
// order they arrive in is not this module's business to know. Waiting for the
// facts it needs is cheaper to be right about than a hook in the right place.
void Tick();

// Level change or shutdown -- the ids meant something only in that world.
void Reset();
} // namespace SVAnomaly

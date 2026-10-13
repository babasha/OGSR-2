#pragma once

// =====================================================================
//  The furniture of the world: what the SERVER puts on the map besides
//  danger.
//
//  sv_anomaly gave the operator the hazards. This gives them the place.
//  A persistent world whose administrator can add an anomaly but not a
//  crate, a table, a bed or a fire has exactly half of a map editor --
//  and the half that cannot make anywhere feel lived in. Every verb here
//  is server-side and reachable over rcon, so the same commands work
//  typed on the host console and typed by an admin standing in the world.
//
//  ⭐WHAT MAKES A PROP IS ITS VISUAL, not its section. There is exactly
//  one placeable prop section in this content -- [physic_object], class
//  O_PHYS_S -- and every crate, chair, wardrobe and pile of junk on every
//  level is that same section wearing a different model. The model is a
//  per-INSTANCE field (CSE_Visual::visual_name) which the SDK authored
//  per object and which travels to clients inside the spawn packet. So
//  placing furniture means creating the entity, giving it a visual and an
//  angle, and only then registering it -- alife().spawn_item takes
//  `registration = false` for exactly this reason.
//
//  ⇒ the catalogue is the MESH LIBRARY, not the config. 647 .ogf files
//  under meshes\physics alone (box, stol, stul, shkaf, krovati, decor,
//  street_prop, light, ...) plus meshes\objects\dynamics and
//  meshes\ogse\prop. Models() reads them off the filesystem at runtime
//  for the same reason sv_anomaly_kinds reads sections at runtime: a list
//  compiled into the engine describes the machine it was written on and
//  quietly lies everywhere else.
//
//  ⚠A PLACED PROP IS FIXED, deliberately. A physics body that simulates
//  is simulated independently on every client -- there is no position
//  sync for props in this protocol -- so a barrel anybody can nudge is a
//  barrel that stands somewhere different for every player within a
//  minute. Props go down as epotBox with a non-empty fixed_bones, which
//  is what makes CPhysicObject::CreateBody call Fix() on the body: one
//  box hull, pinned, identical on every machine. Furniture you can shove
//  is a separate feature and needs a separate answer to that problem.
//
//  Console (registered in console_commands.cpp):
//    sv_prop_models [substring]      what models this install has
//    sv_prop_kinds [substring]       prop sections + what the level uses
//    sv_prop_list [radius]           what stands near you
//    sv_prop_spawn <model|section> [yaw] [x y z]
//    sv_prop_lamp <model|section> [yaw] [x y z]
//    sv_prop_campfire [yaw] [x y z]
//    sv_prop_clone <id> [x y z]
//    sv_prop_move <id|last> [x y z]  sv_prop_turn <id|last> <yaw>
//    sv_prop_lift <id|last> <dy>     sv_prop_del <id|mine>
//    sv_prop_save  sv_prop_load [force]  sv_prop_bake
// =====================================================================

namespace SVProp
{
// What kind of thing a placement is. Two, because they are two different
// server entities with two different sets of per-instance data -- and
// because a fire is not furniture: it lights the room.
enum EKind : u8
{
    kProp = 0, // CSE_ALifeObjectPhysic  -- crates, tables, wardrobes, decor
    kLamp = 1, // CSE_ALifeObjectHangingLamp -- campfires and lights
};

// sv_prop_models: the model library, read off the filesystem.
//
// ⭐The catalogue of what can be placed is a directory listing, not a
// config section list, and that is the whole shape of this module. Reads
// through FS so it sees models inside the packed .db archives exactly as
// the client will when it comes to draw one.
void Models(const char* filter);

// sv_prop_kinds: the sections that build a prop, AND what the loaded level
// actually uses.
//
// ⭐The second half is the useful one and it is the same lesson the
// anomaly toolkit learned the hard way: the WORLD is the authority on what
// this content does. A census of the props the map already stands on
// yields real, working recipes -- section plus visual -- including the
// ones whose per-instance data no config would ever reveal.
void Kinds(const char* filter);

// sv_prop_list: every prop and lamp on the loaded level within `radius`,
// with its model and whether we are the ones who put it there.
//
// ⚠The level filter is not optional: the A-Life registry holds the whole
// game's spawn, every level at once.
void List(float radius);

// sv_prop_spawn: one prop, facing `yaw_deg`.
//
// `what` is either a model path (physics\box\box_wood_01, with or without
// the .ogf) or the name of a prop section, and it is resolved in that
// order -- a name that is both is a model, because the model is what an
// operator means when they type one.
//
// `at` may be null, meaning "where the person who typed this is standing".
// That is the ergonomic that matters: an operator walks to the spot they
// want the thing and names it, instead of reading coordinates off a debug
// overlay and typing them back in.
// ⭐`tilt_deg` may be null, meaning upright. When it is not, .x is pitch and
// .z is bank, in degrees (.y is ignored -- yaw is its own argument, because
// yaw is what a human types and tilt is what a tool measures). It exists
// because ground is not flat: a crate stood bolt upright on a hillside has
// one corner buried and the opposite one in the air, and the client that can
// see the slope is the one that should say so.
void Spawn(const char* what, float yaw_deg, const Fvector* at, const Fvector* tilt_deg = nullptr);

// sv_prop_lamp / sv_prop_campfire: the same placement, as a light.
//
// ⭐A campfire in this content is not a model, it is a RECIPE: the section
// lights_hanging_lamp_campfire is a hanging lamp that the mod's own
// ogse_campfire binding attaches fire to. So "place a campfire" is "place
// a lamp of that section wearing the firewood model", and the fire comes
// from the content rather than from anything here.
// What KIND of light a placed lamp is. ⚠None of this can be read off a model:
// an .ogf knows its shape and nothing about what it emits, and the one field
// that used to be copied from the level -- the range -- came from whichever lamp
// happened to be first, which on this map is a floodlight. So it is asked for.
struct Light
{
    // Metres. 0 means the default, which is a room rather than a building.
    float range{0.f};
    // Degrees. 0 is a point light; anything else is a SPOT of that cone.
    // ⚠A spot shines along the light bone's forward axis, and a model with no
    // light bone falls back to its root -- so the beam follows the model's own
    // facing, which is what a floodlight's mesh is built around.
    float cone{0.f};
    // ⭐⭐Without this the light passes through walls, and that is not a bug in
    // the renderer: vk_light.cpp only lets shadow-flagged lights compete for the
    // shadow pool ("no fallback for random lamps"). A lamp placed unshadowed
    // lights the next room through the brickwork.
    bool shadow{true};
    // 0 flame, 1 warm white, 2 cold white. A colour, chosen from three that
    // cover what this content's fixtures actually are.
    int tone{1};
};

void Lamp(const char* what, float yaw_deg, const Fvector* at, const Fvector* tilt_deg = nullptr, const Light* light = nullptr);
void Campfire(float yaw_deg, const Fvector* at, const Fvector* tilt_deg = nullptr);

// sv_prop_clone: another one of THAT, here.
//
// ⭐The single most useful verb in the set, and it exists because of what
// the census above shows: the level is full of correctly-configured props
// whose per-instance fields (visual, angle, lamp colour, bone names) are
// the only place a working recipe is written down. Copying one is how an
// operator gets a thing that already works without knowing why it does.
void Clone(u16 id, const Fvector* at);

// Move / turn / lift what is already placed, live, without respawning it.
//
// ⭐Two writes, not one, and both are needed -- the same rule the anomaly
// resize obeys. The entity's own position and angle are updated so a
// client connecting LATER is told the new pose the ordinary way, and a
// GE_PROP_TRANSFORM event carries it to the clients that already hold a
// copy, because a pose travels only inside the spawn packet and there is
// no second sentence in the protocol for it.
//
// Any of them may be absent: null `pos`, null `yaw_deg`, a zero `lift` and
// null `tilt_deg` each mean "leave that alone". Yaw and tilt are separate
// arguments rather than one angle for exactly that reason -- turning a thing
// must not flatten the slope it is standing on, and re-seating it on new
// ground must not spin it.
void Transform(u16 id, bool all_placed, const Fvector* pos, const float* yaw_deg, float lift, const Fvector* tilt_deg = nullptr);

// sv_prop_del: one by id, or every one this module placed.
//
// ⚠Deliberately refuses to delete a prop the LEVEL shipped unless it is
// named by id: "mine" removing the map's own furniture would be an
// unrecoverable edit to the world made by a command that reads like a
// cleanup.
void Remove(u16 id, bool all_placed);

// Where the administrator who is running this command stands. Resolved
// from g_admin_actor_account so that two admins in the world each place
// things at their own feet; falls back to the reference player when the
// command was typed on the host console, where there is no author.
bool Origin(Fvector& out);

// The one placed most recently, or 0xffff. ⭐This is what makes the
// toolkit usable in a sentence: "put one here, now turn it, now nudge it
// up" is three commands about the same thing, and making the operator read
// an id out of the log between each of them is the difference between a
// tool and a procedure.
u16 LastPlaced();

// Ids of everything this module placed, for the layers that persist or
// report on them without owning the roster.
void Placed(xr_vector<u16>& out);

// ⭐The placed props, written down, per level.
//
// A room an operator furnished and that vanishes on the next restart is
// not a room: it is a demo. Save() is called after every change, so an
// operator never has to remember to.
//
// ⚠A file of its own -- NOT console variables and NOT user.ltx. This
// server never writes user.ltx at all (it has no clean exit), and the
// project has already been bitten by state that persisted through a config
// file nobody expected to be written.
void Save();

// ⭐⭐sv_prop_bake: publish what is placed right now into the CONTENT, so
// every server lays out the same world.
//
// Save() writes a working copy into this machine's runtime folder, which
// makes an operator's work survive a restart HERE and does nothing at all
// for the next server. A furnished settlement is level design, and level
// design belongs in gamedata next to everything else that describes the
// map, where it can be shipped, diffed and reviewed.
//
// ⚠Refuses to bake an empty set over an existing file. Clearing a map is a
// deliberate act and must not be reachable by typing a save command at the
// wrong moment.
void Bake();

// Put back what Save() and Bake() wrote for the loaded level. Returns how
// many were placed.
//
// ⭐Reads the baked content file first and the working copy second,
// skipping anything the working copy repeats -- the two overlap by design,
// and placing a prop twice leaves two crates in one spot that nobody can
// tell apart later.
u32 Load(bool force);

// One-shot autoload, driven from xrServer::Update.
//
// ⚠Not done at level-load time: the A-Life registry, the level graph and
// the cross table all have to be up before an object can be filed
// anywhere, and the order they arrive in is not this module's business to
// know. Waiting for the facts it needs is cheaper to be right about than a
// hook in the right place.
void Tick();

// Level change or shutdown -- the ids meant something only in that world.
void Reset();
} // namespace SVProp

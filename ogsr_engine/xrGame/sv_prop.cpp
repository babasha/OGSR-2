#include "StdAfx.h"
#include "sv_prop.h"

#include "sv_world_lookup.h"
#include "sv_npc.h" // ReferenceEntity -- the same "who counts as the player" answer

#include "xrServer.h"
#include "xrServer_Objects_ALife_All.h"
#include "clsid_game.h"
#include "GameObject.h"
#include "Level.h" // timeServer() for the broadcast stamp
#include "xrMessages.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h" // graph().actor() -- "is the world populated yet"
#include "../COMMON_AI/GRAPH/level_graph.h"
#include "../COMMON_AI/GRAPH/game_graph.h"
#include "../COMMON_AI/GRAPH/game_level_cross_table.h"

// Set for the duration of a command that arrived over rcon, so the verbs below
// can tell WHICH administrator is asking. Owned by console_commands.cpp.
extern u64 g_admin_actor_account;

namespace
{
// The one section in this content that builds a placeable prop.
//
// ⚠Named rather than searched for, and then VERIFIED by probe before an id is
// spent. A constant that turns out to be wrong on some other content shows up
// as one clear refusal naming the section, which is a far better failure than a
// heuristic picking a section that merely looks right.
constexpr const char* PROP_SECTION = "physic_object";

// A campfire is a PAIR, because that is how the SDK builds one and because each
// half needs a visual that content already uses in that role: the firewood is a
// prop, the fire is a hanging lamp whose own tiny model (physics\light\
// light_koster, 2 KB) exists for nothing else. Placing the light on the firewood
// model instead would be one object -- and would ask CHangingLamp to find a
// light bone on a mesh that was never built to have one.
constexpr const char* CAMPFIRE_LAMP_SECTION = "lights_hanging_lamp_campfire";
constexpr const char* CAMPFIRE_LAMP_FALLBACK = "lights_hanging_lamp";
constexpr const char* CAMPFIRE_LAMP_MODEL = "physics\\light\\light_koster";
constexpr const char* CAMPFIRE_WOOD_MODEL = "objects\\koster_palki_nastil";

// What a campfire's light is, when nothing on the level can be copied. Warm,
// short-ranged and unshadowed: a fire lights the two metres around it.
constexpr u32 FIRE_COLOUR = 0xffff8a3c;
constexpr float FIRE_RANGE = 7.f;
constexpr float FIRE_BRIGHTNESS = 1.f;

// ⚠⚠A placed lamp's reach is CLAMPED, and the reason is a bug report: a light
// switch hung on a wall lit the whole floor. The range was copied wholesale from
// the first lamp on the level, and on this map that donor is a floodlight -- so
// every hand-placed lamp inherited a floodlight's reach. Copying the colour of
// the content's own lighting is right; copying a number that turns a bedside
// lamp into a searchlight is not. An explicit range beats both.
constexpr float LAMP_RANGE_DEFAULT = 6.f;
constexpr float LAMP_RANGE_MIN = 1.f;
constexpr float LAMP_RANGE_MAX = 15.f;

// The three tones, in the order SVProp::Light::tone names them. Warm white is
// the default because most fixtures in this content are incandescent; the flame
// is for oil lamps and lanterns, whose light should read as fire and not as a
// bulb; the cold one is for fluorescent tubes and floodlights.
constexpr u32 LAMP_TONE[3] = {0xffff8a3c, 0xffffe0b4, 0xffe4eeff};

// The mass a placed prop is given. It is pinned, so this only decides how hard
// it is to shift should anything ever un-pin it -- but a zero mass is an ODE
// body with an undefined inertia tensor, which is a different class of problem.
constexpr float PROP_MASS = 60.f;

// ⭐Non-empty is the whole point, and the CONTENT of the string is never read.
//
// CPhysicObject::CreateBody, epotBox branch: `if (*po->fixed_bones)` ->
// get_ElementByStoreOrder(0)->Fix(). The field is a bone-name list only in the
// skeleton branch; for a simple box shell it is a flag that happens to be typed
// as a string. Pinning is what makes a prop the same object on every client --
// see the note in the header about why a simulating prop cannot be.
constexpr const char* PIN = "root";

// Where sv_prop_list looks when nobody says. A room, not a district.
constexpr float DEFAULT_LIST_RADIUS = 50.f;

// How many rows a listing will print before it stops and says how many it did
// not. A console that scrolls a thousand models off the top has answered nothing.
constexpr u32 MAX_ROWS = 120;

// One thing this module put on the map.
//
// The roster is kept for three reasons that all matter: "mine" has to mean
// something in sv_prop_del, persistence needs to know which objects the operator
// authored rather than re-saving the level's own spawn file back at itself, and
// a campfire has to come up and go down as one act even though it is two
// entities.
struct PlacedProp
{
    u16 id{0xffff};
    u16 pair{0xffff}; // the other half of a campfire, or 0xffff
    SVProp::EKind kind{SVProp::kProp};
    shared_str section;
    shared_str visual; // empty: whatever the section's own ini says
    Fvector pos{};
    // ⚠The WHOLE angle, not just yaw. It was a single float until furniture met
    // sloping ground: the wire (GE_PROP_TRANSFORM) and the entity (o_Angle) both
    // carried three components all along, so a tilted crate looked right until
    // the level was reloaded and the file handed back a yaw and two zeroes.
    Fvector angle{}; // radians, XYZ as o_Angle: x pitch, y yaw, z bank
};

xr_vector<PlacedProp> s_placed;
bool s_autoloaded = false;

bool ours(u16 id)
{
    for (const PlacedProp& P : s_placed)
        if (P.id == id)
            return true;
    return false;
}

// A model name the way the engine spells one: backslashes, lower case, no
// extension. Accepts every way a human writes it -- forward slashes off a file
// manager, a trailing .ogf off a directory listing, a leading meshes\ off the
// archive listing this was probably copied out of.
void normalise_model(const char* in, string_path& out)
{
    out[0] = 0;
    if (!in || !in[0])
        return;

    xr_strcpy(out, sizeof(string_path), in);
    for (char* p = out; *p; ++p)
        if (*p == '/')
            *p = '\\';
    xr_strlwr(out);

    const size_t n = xr_strlen(out);
    if (n > 4 && 0 == xr_strcmp(out + n - 4, ".ogf"))
        out[n - 4] = 0;

    // "meshes\physics\box\..." is what the .db listing prints, and typing it back
    // in is the obvious thing to do. $game_meshes$ already IS meshes\.
    if (0 == strncmp(out, "meshes\\", 7))
    {
        string_path tmp;
        xr_strcpy(tmp, sizeof(tmp), out + 7);
        xr_strcpy(out, sizeof(string_path), tmp);
    }
}

// ⭐Asked of the FILESYSTEM, before an entity id is spent and before anything
// reaches a client.
//
// ⚠⚠This is not politeness. A spawn packet naming a model that is not there
// reaches EVERY client in the world, and each of them fails to load it -- so a
// typo by one administrator is a fault delivered to everybody. The server has no
// renderer and cannot check much about a model, but it can check that it exists,
// and that catches the mistake an operator actually makes.
bool model_exists(const char* normalised)
{
    if (!normalised || !normalised[0])
        return false;
    string_path with_ext;
    xr_sprintf(with_ext, "%s.ogf", normalised);
    return !!FS.exist("$game_meshes$", with_ext);
}

// Whether a section builds what we are about to ask it to build, asked of the
// FACTORY rather than of a list.
//
// ⭐A probe is the only test that cannot be out of date: it runs exactly the
// machinery the spawn is about to run and looks at what came out. ⚠It costs no
// entity id -- F_entity_Create does not touch the generator; spawn_item assigns
// one afterwards -- and nothing is registered anywhere.
//
// This matters more here than it looks. [physic_object] declares class O_PHYS_S,
// which no header in this engine defines: the mod registers it from Lua
// (class_registrator.script binds it to CPhysicObject and to a scripted server
// object). Nothing compiled in can know that, and a probe does not have to.
bool section_builds(const char* section, SVProp::EKind kind)
{
    if (!section || !section[0] || !pSettings->section_exist(section) || !pSettings->line_exist(section, "class"))
        return false;

    CSE_Abstract* probe = F_entity_Create(section);
    if (!probe)
        return false;

    const bool ok = (kind == SVProp::kLamp) ? !!smart_cast<CSE_ALifeObjectHangingLamp*>(probe) : !!smart_cast<CSE_ALifeObjectPhysic*>(probe);
    F_entity_Destroy(probe);
    return ok;
}

// The section a placement of this kind uses, or null with a reason logged.
// ⭐⭐`fire` is not a detail of which section to prefer -- it decides whether the
// thing BURNS. The campfire section is a hanging lamp the mod binds
// ogse_campfire to, so a chandelier built on it comes with flames in it: a light
// placed for a ceiling has to start from the plain lamp section instead. That
// one substitution is most of what "hanging lamps work badly" meant.
const char* base_section(SVProp::EKind kind, bool fire)
{
    if (kind == SVProp::kLamp)
    {
        if (!fire)
        {
            if (section_builds(CAMPFIRE_LAMP_FALLBACK, kind))
                return CAMPFIRE_LAMP_FALLBACK;
            Msg("! [sv_prop] '%s' is not in this content -- using '%s', which may bring a fire with it", CAMPFIRE_LAMP_FALLBACK, CAMPFIRE_LAMP_SECTION);
        }
        if (section_builds(CAMPFIRE_LAMP_SECTION, kind))
            return CAMPFIRE_LAMP_SECTION;
        if (section_builds(CAMPFIRE_LAMP_FALLBACK, kind))
        {
            // Worth saying out loud: without the campfire section the light is a
            // plain lamp, so there will be light and no fire. "The fire did not
            // appear" and "the fire is not implemented" look identical in a dark
            // room.
            Msg("! [sv_prop] '%s' is not in this content -- using '%s', which lights but does not burn", CAMPFIRE_LAMP_SECTION, CAMPFIRE_LAMP_FALLBACK);
            return CAMPFIRE_LAMP_FALLBACK;
        }
        Msg("! [sv_prop] this content has no hanging-lamp section -- cannot place lights");
        return nullptr;
    }

    if (section_builds(PROP_SECTION, kind))
        return PROP_SECTION;

    Msg("! [sv_prop] '%s' does not build a physic object in this content -- sv_prop_kinds shows what does", PROP_SECTION);
    return nullptr;
}

// Level and game vertices for a point.
//
// ⚠A prop does NOT stand on the navmesh -- CSE_ALifeObjectPhysic turns
// flUsedAI_Locations off, and a crate on a roof is a perfectly good crate. The
// graph vertex is still required, and for one reason only: the A-Life graph
// registry files objects by game vertex, and add_online asserts that the
// vertex's level is the loaded one. So the nearest vertex is looked up to answer
// "which level is this", and the POSITION is left exactly where it was asked for.
bool graph_ids(const Fvector& pos, u32& level_vertex, GameGraph::_GRAPH_ID& game_vertex)
{
    if (!ai().get_level_graph() || !ai().get_cross_table())
        return false;

    const CLevelGraph& G = ai().level_graph();
    level_vertex = G.vertex_id(pos);
    if (!G.valid_vertex_id(level_vertex))
        level_vertex = G.nearest_vertex_id(pos); // off the mesh: the slow scan, once
    if (!G.valid_vertex_id(level_vertex))
        return false;

    game_vertex = ai().cross_table().vertex(level_vertex).game_vertex_id();
    return ai().get_game_graph() && ai().game_graph().valid_vertex_id(game_vertex);
}

// Everything on the loaded level that this module understands.
//
// ⚠⚠BOTH REGISTRIES, because a census that asks one of them under-reports --
// and under-reporting is the direction that hides content, the worst one for a
// report to be wrong in.
//
// The A-Life registry holds what the simulator took charge of; the server's
// entity map holds what is online. They overlap heavily and neither contains
// the other: a physic object is not necessarily A-Life's to keep, since
// CSE_ALifeObjectPhysic::can_save() defers to CSE_PHSkeleton::need_save(), so
// content whose crates do not save is reachable only through the server map.
// SVWorld::Find already had to consult both for exactly this reason and says so.
//
// ℹMeasured on pripyat_full 03-08-2026: this level ships TWENTY-TWO lamps and
// ZERO physic objects -- all its furniture is baked into the level mesh. So on
// this map the census is a list of lamps plus whatever this module has placed,
// and the model library, not the world, is the catalogue to browse.
//
// ⚠The level filter is the same one sv_anomaly needs and for the same reason:
// the A-Life registry holds the whole game's spawn at once, so an unfiltered
// listing on Pripyat cheerfully measures distances to crates in the Cordon. It
// applies only to entities that carry a graph id; the server map's own are on
// the loaded level by construction.
void collect(xr_vector<CSE_ALifeDynamicObjectVisual*>& out)
{
    out.clear();

    if (!ai().get_level_graph() || !ai().get_game_graph())
        return;

    const GameGraph::_LEVEL_ID here = ai().level_graph().level_id();

    auto mine = [](CSE_Abstract* e) -> CSE_ALifeDynamicObjectVisual* {
        if (CSE_ALifeObjectPhysic* p = smart_cast<CSE_ALifeObjectPhysic*>(e))
            return p;
        if (CSE_ALifeObjectHangingLamp* l = smart_cast<CSE_ALifeObjectHangingLamp*>(e))
            return l;
        return nullptr;
    };

    if (const CALifeSimulator* alife = ai().get_alife())
    {
        for (const auto& it : alife->objects().objects())
        {
            CSE_ALifeDynamicObjectVisual* v = mine(it.second);
            if (!v)
                continue;

            const GameGraph::_GRAPH_ID gvid = v->m_tGraphID;
            if (!ai().game_graph().valid_vertex_id(gvid))
                continue;
            if (ai().game_graph().vertex(gvid)->level_id() != here)
                continue;

            out.push_back(v);
        }
    }

    // ...and the ones the simulator never took charge of.
    if (xrServer* SV = SVWorld::Server())
    {
        for (const auto& it : SV->Entities())
        {
            CSE_ALifeDynamicObjectVisual* v = mine(it.second);
            if (!v)
                continue;
            // The two registries overlap: an A-Life object that is online is in
            // both, and listing it twice would double every count in the census.
            if (std::find(out.begin(), out.end(), v) == out.end())
                out.push_back(v);
        }
    }
}

// ⭐⭐A lamp the LEVEL already has, to copy the fields a runtime spawn cannot
// invent.
//
// Same shape of answer as the anomaly toolkit's copy_instance_data, and it
// exists for the same reason: colour, range and light texture are authored PER
// INSTANCE in the SDK, not carried by the section, so a fresh lamp entity has
// them all at their constructor defaults. The one that actually matters is the
// texture -- it names a real asset and no value invented here can be sure to.
const CSE_ALifeObjectHangingLamp* donor_lamp()
{
    xr_vector<CSE_ALifeDynamicObjectVisual*> world;
    collect(world);
    for (CSE_ALifeDynamicObjectVisual* v : world)
        if (const CSE_ALifeObjectHangingLamp* l = smart_cast<const CSE_ALifeObjectHangingLamp*>(v))
            if (l->light_texture.size())
                return l;
    return nullptr;
}

// Where the placed props for the loaded level are written.
//
// Per level, because that is the unit a prop belongs to -- a coordinate means
// nothing on another map, and a single shared file would need every line tagged
// and would still be one bad edit away from furnishing Pripyat with the Cordon's
// crates.
bool persist_path(string_path& out)
{
    const CALifeSimulator* alife = ai().get_alife();
    if (!alife || !ai().get_level_graph() || !ai().get_game_graph())
        return false;

    string_path leaf;
    xr_sprintf(leaf, "props_%s.ltx", alife->level_name().c_str());
    FS.update_path(out, "$app_data_root$", leaf);
    return true;
}

// ⭐⭐The BAKED props: the same lines, but in the content instead of in one
// machine's runtime folder. See the note on Bake() in the header.
//
// ⚠Flat name, no subfolder: update_path does not create directories, and a bake
// that silently fails because config\props\ does not exist would be the worst
// possible failure for a command whose whole job is durability.
bool baked_path(string_path& out)
{
    const CALifeSimulator* alife = ai().get_alife();
    if (!alife || !ai().get_level_graph() || !ai().get_game_graph())
        return false;

    string_path leaf;
    xr_sprintf(leaf, "props_%s.ltx", alife->level_name().c_str());
    FS.update_path(out, "$game_config$", leaf);
    return true;
}

// One line of a persisted file. Named at namespace scope because both files are
// read the same way and by the same reader.
struct Row
{
    string32 kind;
    string128 section;
    string_path visual;
    Fvector pos;
    Fvector angle_deg; // x pitch, y yaw, z bank -- pitch and bank absent in old files
};

// Returns false only when the file is not there, which is the ordinary case for
// both of them and not a fault.
bool read_rows(const char* fname, xr_vector<Row>& rows, u32& bad)
{
    FILE* f = fopen(fname, "rb");
    if (!f)
        return false;

    char line[1024];
    u32 lineno = 0;
    while (fgets(line, sizeof(line), f))
    {
        ++lineno;
        const char* p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p || *p == ';' || *p == '\n' || *p == '\r')
            continue;

        Row r{};
        // ⭐Seven fields or nine, and both are valid. The tilt columns were added
        // when furniture learned about slopes; files written before that are the
        // ones already sitting on operators' disks, and a reader that demanded
        // nine would have thrown away every prop anybody had placed so far. A
        // short line simply means upright, which is what it meant when it was
        // written.
        const int n = sscanf(p, "%31s %127s %259s %f %f %f %f %f %f", r.kind, r.section, r.visual, &r.pos.x, &r.pos.y, &r.pos.z, &r.angle_deg.y, &r.angle_deg.x,
                             &r.angle_deg.z);
        if (n != 9 && n != 7)
        {
            Msg("! [sv_prop] %s:%u is not 'kind section visual x y z yaw [pitch bank]' -- skipped", fname, lineno);
            ++bad;
            continue;
        }
        if (n == 7)
            r.angle_deg.x = r.angle_deg.z = 0.f;
        rows.push_back(r);
    }
    fclose(f);
    return true;
}

// ⚠The two files overlap by design -- baking does not erase what is already
// baked -- so the same prop can be described twice. Placing it twice leaves two
// crates in one spot, and the second is indistinguishable from the first
// afterwards. Centimetre rounding, because these numbers made a round trip
// through "%.3f" and comparing floats for equality across that is a coin toss.
bool same_placement(const Row& a, const Row& b)
{
    auto alike = [](float x, float y) { return _abs(x - y) < 0.01f; };
    return 0 == xr_strcmp(a.section, b.section) && 0 == xr_strcmp(a.visual, b.visual) && alike(a.pos.x, b.pos.x) && alike(a.pos.y, b.pos.y) && alike(a.pos.z, b.pos.z);
}

void write_rows(FILE* f)
{
    // Yaw stays the fourth number, where it has always been, and the tilt goes
    // on the end -- so a file written now is still readable by anything that
    // only knows the old shape, and the column an operator edits by hand has not
    // moved out from under them.
    for (const PlacedProp& P : s_placed)
        fprintf(f, "%s %s %s %.3f %.3f %.3f %.2f %.2f %.2f\n", (P.kind == SVProp::kLamp) ? "lamp" : "prop", P.section.c_str(), P.visual.size() ? P.visual.c_str() : "-",
                VPUSH(P.pos), rad2deg(P.angle.y), rad2deg(P.angle.x), rad2deg(P.angle.z));
}

// ⭐The one place an entity is actually built, so that every verb that puts
// something in the world -- spawn, lamp, campfire, clone, load -- agrees about
// pinning, angles and what has to happen before registration.
//
// Returns the new id, or 0xffff with a reason already logged.
u16 build(SVProp::EKind kind, const char* section, const char* visual, const Fvector& pos, const Fvector& angle, bool fire = false, const SVProp::Light* light = nullptr)
{
    CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());
    if (!alife)
    {
        Msg("! [sv_prop] no A-Life simulator -- is a level loaded?");
        return 0xffff;
    }

    u32 lvid = 0;
    GameGraph::_GRAPH_ID gvid = GameGraph::_GRAPH_ID(-1);
    if (!graph_ids(pos, lvid, gvid))
    {
        Msg("! [sv_prop] no AI graph vertex anywhere near (%.1f, %.1f, %.1f) -- cannot file an object there", VPUSH(pos));
        return 0xffff;
    }

    // ⭐registration = false is the whole trick. The visual and the angle have to
    // be on the entity BEFORE it is registered, because registering is what
    // eventually hands the object to the switch manager, and the spawn packet the
    // clients receive is written from the entity as it stands at that moment.
    CSE_Abstract* obj = alife->spawn_item(section, pos, lvid, gvid, ALife::_OBJECT_ID(-1), false);
    if (!obj)
    {
        Msg("! [sv_prop] spawn of '%s' failed", section);
        return 0xffff;
    }

    CSE_ALifeDynamicObject* dynamic = smart_cast<CSE_ALifeDynamicObject*>(obj);
    CSE_Visual* vis = smart_cast<CSE_Visual*>(obj);
    if (!dynamic || !vis)
    {
        // The probe said this section was the right kind and the factory
        // disagreed. Not recoverable here and not worth guessing at: say so and
        // leave the half-built object unregistered rather than putting something
        // shapeless into the world.
        Msg("!! [sv_prop] '%s' built a '%s' that is not a visual dynamic object -- not registering it", section, obj->s_name.c_str());
        return 0xffff;
    }

    if (visual && visual[0])
        vis->set_visual(visual);

    // The angle, which is the difference between furniture and a pile.
    // o_Angle.y is the yaw every other placement in this engine means; x and z
    // are the tilt, which is how a thing stands on ground that is not flat. The
    // client reads all three back with the same convention
    // (CGameObject::net_Spawn does XFORM().setXYZ(o_Angle)).
    obj->o_Angle = angle;

    if (kind == SVProp::kProp)
    {
        CSE_ALifeObjectPhysic* ph = smart_cast<CSE_ALifeObjectPhysic*>(obj);
        R_ASSERT(ph); // section_builds() already proved this
        ph->type = epotBox;
        ph->mass = PROP_MASS;
        ph->fixed_bones = PIN; // see the note on PIN: pinned, not decorative
    }
    else
    {
        CSE_ALifeObjectHangingLamp* lamp = smart_cast<CSE_ALifeObjectHangingLamp*>(obj);
        R_ASSERT(lamp);

        lamp->m_health = 100.f;

        // ⚠⚠Non-physical always (a light on a bracket must not be knocked off
        // one), but NOT unshadowed any more. vk_light.cpp gives shadow budget
        // only to lights the game flagged as casters -- an unflagged lamp is a
        // lamp whose light crosses walls, which is exactly what a room lit
        // through its own brickwork looked like.
        u16 f = CSE_ALifeObjectHangingLamp::flR1 | CSE_ALifeObjectHangingLamp::flR2;
        if (!light || light->shadow)
            f |= CSE_ALifeObjectHangingLamp::flCastShadow;
        if (light && light->cone > 0.f)
        {
            f |= CSE_ALifeObjectHangingLamp::flTypeSpot;
            lamp->spot_cone_angle = deg2rad(light->cone);
        }
        lamp->flags.assign(f);

        // ⭐The light itself comes from a lamp the LEVEL already has, and only
        // falls back to constants when there is none. A room lit by this content
        // was lit by somebody who chose these numbers for it; a fire-coloured
        // seven metres is right for a campfire and wrong for a ceiling, and
        // hard-coding it here was the second half of "hanging lamps look wrong".
        // ⚠light_texture is the one field that names an asset, so it is copied
        // in both cases. Empty is safe (set_texture takes null) -- it just means
        // an untextured point light, which for a fire is the right picture.
        const CSE_ALifeObjectHangingLamp* donor = donor_lamp();
        if (donor)
            lamp->light_texture = donor->light_texture;

        if (fire)
        {
            lamp->color = FIRE_COLOUR;
            lamp->brightness = FIRE_BRIGHTNESS;
            lamp->range = FIRE_RANGE;
        }
        else
        {
            const int tone = light ? std::clamp(light->tone, 0, 2) : 1;
            lamp->color = LAMP_TONE[tone];
            lamp->brightness = FIRE_BRIGHTNESS;
            lamp->range = (light && light->range > 0.f) ? light->range : LAMP_RANGE_DEFAULT;
        }

        // Clamped either way: a light with a hundred-metre reach is not a
        // mistake anyone should be able to make by typing one number wrong.
        clamp(lamp->range, LAMP_RANGE_MIN, LAMP_RANGE_MAX);

        // ⚠Bone names are left EMPTY on purpose. They are per-instance and
        // per-MODEL, so a name copied from another lamp is a name for somebody
        // else's skeleton. CHangingLamp::net_Spawn now falls back to the visual's
        // own root bone when it cannot resolve one, which is the only answer that
        // is right for a model this code has never seen.
    }

    alife->register_object(dynamic, true);
    return obj->ID;
}

// The roster entry, kept in one place so every caller records the same facts.
void remember(u16 id, u16 pair, SVProp::EKind kind, const char* section, const char* visual, const Fvector& pos, const Fvector& angle)
{
    PlacedProp P;
    P.id = id;
    P.pair = pair;
    P.kind = kind;
    P.section = section;
    P.visual = (visual && visual[0]) ? visual : "";
    P.pos = pos;
    P.angle = angle;
    s_placed.push_back(P);
}

// Position and angle of a placed object, sent to the clients that already hold a
// copy of it.
//
// ⚠⚠Broadcast DIRECTLY, not through CGameObject::u_EventSend. That helper posts
// the event back into this process's own server, where xrServer::Process_event
// switches on the type and R_ASSERTs on anything it does not implement -- so an
// event meant purely for clients kills the host the first time it is sent. That
// exact mistake took this server down the first time sv_anomaly_size was typed.
void broadcast_pose(u16 id, const Fvector& pos, const Fvector& angle)
{
    xrServer* SV = SVWorld::Server();
    if (!SV)
        return;

    NET_Packet P;
    P.w_begin(M_EVENT);
    P.w_u32(Level().timeServer());
    P.w_u16(GE_PROP_TRANSFORM);
    P.w_u16(id);
    P.w_vec3(pos);
    P.w_vec3(angle);
    SV->SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
}

// One placement, from a resolved model-or-section. Shared by Spawn and Lamp
// because the only thing that differs between them is which entity gets built.
void place(SVProp::EKind kind, const char* what, float yaw_deg, const Fvector* at, const Fvector* tilt_deg, const SVProp::Light* light)
{
    if (!what || !what[0])
    {
        Msg("! [sv_prop] name a model or a section -- sv_prop_models lists the models");
        return;
    }

    const char* section = nullptr;
    string_path model{};

    // Resolved model-first, because a model is what an operator means when they
    // type one, and because there is exactly one prop section in this content --
    // so "section or model" is really "the section, or which model it wears".
    normalise_model(what, model);
    if (model_exists(model))
    {
        section = base_section(kind, false); // a lamp asked for by name is a lamp, not a fire
        if (!section)
            return;
    }
    else if (section_builds(what, kind))
    {
        section = what;
        model[0] = 0; // the section's own ini decides
    }
    else
    {
        Msg("! [sv_prop] '%s' is neither a model in $game_meshes$ nor a %s section here", what, (kind == SVProp::kLamp) ? "hanging-lamp" : "physic-object");
        Msg("- [sv_prop] sv_prop_models %s   to see what is available", what);
        return;
    }

    Fvector pos;
    if (at)
        pos = *at;
    else if (!SVProp::Origin(pos))
        return;

    Fvector angle;
    angle.set(tilt_deg ? deg2rad(tilt_deg->x) : 0.f, deg2rad(yaw_deg), tilt_deg ? deg2rad(tilt_deg->z) : 0.f);

    const u16 id = build(kind, section, model, pos, angle, false, light);
    if (id == 0xffff)
        return;

    remember(id, 0xffff, kind, section, model, pos, angle);
    Msg("~ [sv_prop] placed %s id=%u '%s'%s%s facing %.0f%s at (%.1f, %.1f, %.1f)", (kind == SVProp::kLamp) ? "lamp" : "prop", id, section, model[0] ? " wearing " : "", model,
        yaw_deg, tilt_deg ? " tilted to the ground" : "", VPUSH(pos));
    SVProp::Save();
}
} // namespace

bool SVProp::Origin(Fvector& out)
{
    xrServer* SV = SVWorld::Server();

    // The administrator who typed it, when one did. Two operators in the same
    // world must not furnish each other's feet.
    if (SV && g_admin_actor_account)
    {
        for (u32 i = 0; i < SV->client_Count(); ++i)
        {
            xrClientData* C = (xrClientData*)SV->client_Get(i);
            if (C && C->owner && C->account_id == g_admin_actor_account)
            {
                out = C->owner->position();
                return true;
            }
        }
    }

    // Typed on the host console: there is no author, so it means "by the player",
    // which is what every other server-side verb here means too.
    if (CSE_Abstract* e = SVWorld::Find(SVNpc::ReferenceEntity()))
    {
        out = e->position();
        return true;
    }

    Msg("! [sv_prop] nowhere to put it -- no player entity exists yet, and no point was given");
    return false;
}

u16 SVProp::LastPlaced() { return s_placed.empty() ? u16(0xffff) : s_placed.back().id; }

void SVProp::Placed(xr_vector<u16>& out)
{
    out.clear();
    for (const PlacedProp& P : s_placed)
        out.push_back(P.id);
}

void SVProp::Models(const char* filter)
{
    // ⭐Read off the filesystem, which sees inside the packed .db archives
    // exactly as the client will when it comes to draw one. A list of model names
    // compiled into the engine would describe the install it was written on and
    // nothing else -- the same lesson the anomaly toolkit paid for once already.
    FS_FileSet found;
    FS.file_list(found, "$game_meshes$", FS_ListFiles, "*.ogf");

    if (found.empty())
    {
        Msg("! [sv_prop] no .ogf models visible under $game_meshes$ at all -- is gamedata mounted?");
        return;
    }

    const bool narrowed = filter && filter[0];
    string_path needle{};
    if (narrowed)
        normalise_model(filter, needle);

    u32 shown = 0, matched = 0;
    for (const FS_File& f : found)
    {
        if (narrowed && !strstr(f.name.c_str(), needle))
            continue;
        ++matched;
        if (shown >= MAX_ROWS)
            continue;

        // Without the extension, because that is the form every command here
        // takes and a listing you cannot paste back in is half a tool.
        xr_string name = f.name;
        if (name.size() > 4)
            name.erase(name.size() - 4);
        Msg("~ [sv_prop]   %s", name.c_str());
        ++shown;
    }

    if (!matched)
    {
        Msg("! [sv_prop] no model matches '%s' (%u .ogf in this install)", filter, u32(found.size()));
        Msg("- [sv_prop] try a folder: physics\\box  physics\\stol  physics\\shkaf  physics\\decor  objects\\dynamics");
        return;
    }

    // ⚠Said out loud when the list was cut. A listing that silently stops at a
    // limit reads as "that is all there is", which is the one thing it must never
    // be mistaken for.
    if (matched > shown)
        Msg("~ [sv_prop] %u models match%s%s -- showing %u; narrow it with sv_prop_models <substring>", matched, narrowed ? " " : "", narrowed ? filter : "", shown);
    else
        Msg("~ [sv_prop] %u model(s); sv_prop_spawn <model> [yaw]", matched);
}

void SVProp::Kinds(const char* filter)
{
    // The sections first: in this content there is one that builds a prop and two
    // that build a lamp, and knowing that is half of knowing how the tool works.
    Msg("~ [sv_prop] sections:");
    for (const char* s : {PROP_SECTION, CAMPFIRE_LAMP_SECTION, CAMPFIRE_LAMP_FALLBACK})
    {
        const bool prop = section_builds(s, kProp);
        const bool lamp = section_builds(s, kLamp);
        Msg("~ [sv_prop]   %-32s %s", s, prop ? "physic object" : (lamp ? "hanging lamp" : "NOT IN THIS CONTENT"));
    }

    // ⭐And then the census, which is the half that is actually useful.
    //
    // The level is full of correctly-configured props whose per-instance visual
    // is the only place a working recipe is written down. Reporting what the map
    // already stands on turns "I want one of those" into a model name an operator
    // can paste straight into sv_prop_spawn -- or an id for sv_prop_clone.
    xr_vector<CSE_ALifeDynamicObjectVisual*> world;
    collect(world);

    struct Recipe
    {
        shared_str section, visual;
        u32 count;
        u16 example;
    };
    xr_vector<Recipe> recipes;

    for (CSE_ALifeDynamicObjectVisual* v : world)
    {
        if (filter && filter[0] && !strstr(v->visual_name.c_str(), filter) && !strstr(v->s_name.c_str(), filter))
            continue;

        bool merged = false;
        for (Recipe& r : recipes)
            if (r.section == v->s_name && r.visual == v->visual_name)
            {
                ++r.count;
                merged = true;
                break;
            }
        if (!merged)
            recipes.push_back(Recipe{v->s_name, v->visual_name, 1, v->ID});
    }

    if (recipes.empty())
    {
        Msg("~ [sv_prop] this level stands on no props or lamps%s%s", (filter && filter[0]) ? " matching " : "", (filter && filter[0]) ? filter : "");
        return;
    }

    std::sort(recipes.begin(), recipes.end(), [](const Recipe& a, const Recipe& b) { return a.count > b.count; });

    Msg("~ [sv_prop] what this level uses (section / model / how many / an id to clone):");
    u32 shown = 0;
    for (const Recipe& r : recipes)
    {
        if (shown >= MAX_ROWS)
            break;
        Msg("~ [sv_prop]   %-26s %-44s x%-4u id=%u", r.section.c_str(), r.visual.size() ? r.visual.c_str() : "(from the section)", r.count, r.example);
        ++shown;
    }
    if (recipes.size() > shown)
        Msg("~ [sv_prop] ...and %u more kinds; narrow it with sv_prop_kinds <substring>", u32(recipes.size()) - shown);
}

void SVProp::List(float radius)
{
    if (radius <= 0.f)
        radius = DEFAULT_LIST_RADIUS;

    xr_vector<CSE_ALifeDynamicObjectVisual*> world;
    collect(world);

    if (world.empty())
    {
        Msg("~ [sv_prop] no props or lamps on this level at all");
        return;
    }

    Fvector ref;
    const bool have_ref = Origin(ref);

    u32 shown = 0, placed_shown = 0, within = 0;
    for (CSE_ALifeDynamicObjectVisual* v : world)
    {
        const float away = have_ref ? ref.distance_to(v->position()) : 0.f;
        if (have_ref && away > radius)
            continue;
        ++within;
        if (shown >= MAX_ROWS)
            continue;

        const bool lamp = !!smart_cast<CSE_ALifeObjectHangingLamp*>(v);
        const bool mine = ours(v->ID);
        if (mine)
            ++placed_shown;

        Msg("~ [sv_prop]   id=%-5u %-4s %-40s yaw=%-4.0f at (%.1f, %.1f, %.1f) %.0fm away%s", v->ID, lamp ? "lamp" : "prop",
            v->visual_name.size() ? v->visual_name.c_str() : v->s_name.c_str(), rad2deg(v->o_Angle.y), VPUSH(v->position()), away, mine ? "  [placed]" : "");
        ++shown;
    }

    if (within > shown)
        Msg("~ [sv_prop] ...and %u more within %.0fm", within - shown, radius);

    // Both totals, because they answer different questions: "is my command
    // working" is about the placed count, "what is here" is about all of it.
    Msg("~ [sv_prop] %u within %.0fm (%u placed by the server); %u on the level", within, radius, placed_shown, u32(world.size()));
}

void SVProp::Spawn(const char* what, float yaw_deg, const Fvector* at, const Fvector* tilt_deg) { place(kProp, what, yaw_deg, at, tilt_deg, nullptr); }

void SVProp::Lamp(const char* what, float yaw_deg, const Fvector* at, const Fvector* tilt_deg, const Light* light) { place(kLamp, what, yaw_deg, at, tilt_deg, light); }

void SVProp::Campfire(float yaw_deg, const Fvector* at, const Fvector* tilt_deg)
{
    const char* lamp_section = base_section(kLamp, true); // this one really is a fire
    const char* prop_section = base_section(kProp, false);
    if (!lamp_section || !prop_section)
        return;

    Fvector pos;
    if (at)
        pos = *at;
    else if (!Origin(pos))
        return;

    // ⭐Whatever the level's own campfires wear, if it has any. Content beats a
    // constant every time: this install's firewood model is named here only so
    // that a level with no campfire on it can still get one.
    string_path wood{};
    normalise_model(CAMPFIRE_WOOD_MODEL, wood);
    string_path fire{};
    normalise_model(CAMPFIRE_LAMP_MODEL, fire);

    xr_vector<CSE_ALifeDynamicObjectVisual*> world;
    collect(world);
    for (CSE_ALifeDynamicObjectVisual* v : world)
        if (smart_cast<CSE_ALifeObjectHangingLamp*>(v) && strstr(v->s_name.c_str(), "campfire") && v->visual_name.size())
        {
            normalise_model(v->visual_name.c_str(), fire);
            break;
        }

    if (!model_exists(wood))
    {
        Msg("! [sv_prop] no firewood model '%s' in this install -- placing the fire alone", wood);
        wood[0] = 0;
    }
    if (!model_exists(fire))
    {
        Msg("! [sv_prop] no campfire light model '%s' in this install", fire);
        return;
    }

    Fvector angle;
    angle.set(tilt_deg ? deg2rad(tilt_deg->x) : 0.f, deg2rad(yaw_deg), tilt_deg ? deg2rad(tilt_deg->z) : 0.f);

    // The fire first: if the pair cannot be completed, a fire with no logs is
    // still a campfire, whereas logs with no fire are a pile of sticks.
    const u16 fire_id = build(kLamp, lamp_section, fire, pos, angle, true);
    if (fire_id == 0xffff)
        return;

    u16 wood_id = 0xffff;
    if (wood[0])
        wood_id = build(kProp, prop_section, wood, pos, angle);

    remember(fire_id, wood_id, kLamp, lamp_section, fire, pos, angle);
    if (wood_id != 0xffff)
        remember(wood_id, fire_id, kProp, prop_section, wood, pos, angle);

    Msg("~ [sv_prop] campfire at (%.1f, %.1f, %.1f): fire id=%u%s", VPUSH(pos), fire_id, (wood_id != 0xffff) ? "" : " (no logs)");
    if (wood_id != 0xffff)
        Msg("~ [sv_prop]   logs id=%u -- deleting either half removes both", wood_id);
    Save();
}

void SVProp::Clone(u16 id, const Fvector* at)
{
    CSE_Abstract* src = SVWorld::Find(id);
    CSE_ALifeDynamicObjectVisual* v = smart_cast<CSE_ALifeDynamicObjectVisual*>(src);
    if (!v)
    {
        Msg("! [sv_prop] id=%u is not an object the server knows about -- sv_prop_list", id);
        return;
    }

    const bool lamp = !!smart_cast<CSE_ALifeObjectHangingLamp*>(v);
    const bool prop = !!smart_cast<CSE_ALifeObjectPhysic*>(v);
    if (!lamp && !prop)
    {
        Msg("! [sv_prop] id=%u is a '%s', which is neither a prop nor a lamp", id, v->s_name.c_str());
        return;
    }

    Fvector pos;
    if (at)
        pos = *at;
    else if (!Origin(pos))
        return;

    const EKind kind = lamp ? kLamp : kProp;
    const Fvector angle = v->o_Angle; // the donor's whole pose, tilt included

    // ⭐The donor's own section, not the base one. Cloning is how an operator
    // gets a thing that already works, and substituting a section here would
    // quietly hand back something that merely looks like it.
    const u16 fresh = build(kind, v->s_name.c_str(), v->visual_name.c_str(), pos, angle);
    if (fresh == 0xffff)
        return;

    remember(fresh, 0xffff, kind, v->s_name.c_str(), v->visual_name.c_str(), pos, angle);
    Msg("~ [sv_prop] cloned id=%u -> id=%u '%s' wearing %s at (%.1f, %.1f, %.1f)", id, fresh, v->s_name.c_str(), v->visual_name.c_str(), VPUSH(pos));
    Save();
}

void SVProp::Transform(u16 id, bool all_placed, const Fvector* pos, const float* yaw_deg, float lift, const Fvector* tilt_deg)
{
    xr_vector<u16> targets;
    if (all_placed)
    {
        for (const PlacedProp& P : s_placed)
            targets.push_back(P.id);
    }
    else
        targets.push_back(id);

    u32 done = 0;
    for (u16 target : targets)
    {
        CSE_ALifeDynamicObjectVisual* v = smart_cast<CSE_ALifeDynamicObjectVisual*>(SVWorld::Find(target));
        if (!v)
        {
            Msg("! [sv_prop] id=%u is not an object the server knows about", target);
            continue;
        }

        Fvector at = v->position();
        if (pos)
            at = *pos;
        at.y += lift;

        // ⭐Each part of the pose is replaced only if it was given, so turning
        // keeps the tilt the ground gave a thing and re-seating it keeps the
        // heading the operator chose.
        Fvector angle = v->o_Angle;
        if (yaw_deg)
            angle.y = deg2rad(*yaw_deg);
        if (tilt_deg)
        {
            angle.x = deg2rad(tilt_deg->x);
            angle.z = deg2rad(tilt_deg->z);
        }

        // ⭐The entity FIRST: this is the copy every later joiner is built from,
        // and leaving it stale would mean the crate stands in one place for the
        // people already here and somewhere else for anyone who walks up
        // afterwards. That class of disagreement is invisible until somebody
        // walks into a wall that is not drawn.
        v->o_Position = at;
        v->o_Angle = angle;

        // ⚠The graph vertex is deliberately NOT re-derived. It decides which
        // level the A-Life registry files this object under, and re-filing a
        // registered object behind the simulator's back is not a thing to do for
        // the sake of an edit measured in metres. An editor moves furniture
        // around a room; if something is ever dragged far enough to change game
        // vertex, it wants a fresh spawn and not a nudge.
        broadcast_pose(target, at, angle);

        for (PlacedProp& mine : s_placed)
            if (mine.id == target)
            {
                mine.pos = at;
                mine.angle = angle;
            }

        ++done;
    }

    if (done)
        Msg("~ [sv_prop] moved %u object(s)", done);
    Save();
}

void SVProp::Remove(u16 id, bool all_placed)
{
    xr_vector<u16> doomed;

    if (all_placed)
    {
        for (const PlacedProp& P : s_placed)
            doomed.push_back(P.id);
        s_placed.clear();
    }
    else
    {
        // A named id may be one of the level's own, and removing one on purpose
        // is a legitimate thing to want. Only "mine" is restricted.
        doomed.push_back(id);

        // ⭐...and its other half, when it has one. A campfire is two entities
        // and one thing; deleting the logs and leaving a fire burning over bare
        // ground is not what anybody meant by "delete the campfire".
        for (const PlacedProp& P : s_placed)
            if (P.id == id && P.pair != 0xffff)
            {
                doomed.push_back(P.pair);
                break;
            }

        for (u16 dead : doomed)
            for (auto it = s_placed.begin(); it != s_placed.end(); ++it)
                if (it->id == dead)
                {
                    s_placed.erase(it);
                    break;
                }
    }

    u32 removed = 0;
    for (u16 dead : doomed)
    {
        if (!SVWorld::Find(dead))
            continue;

        // Through the real GE_DESTROY event rather than by calling the server's
        // destroy handler directly: that handler asserts on the sender being a
        // known client, and the normal channel also broadcasts the removal to
        // everyone watching.
        NET_Packet P;
        CGameObject::u_EventGen(P, GE_DESTROY, dead);
        CGameObject::u_EventSend(P);
        ++removed;
    }

    Msg("~ [sv_prop] removed %u; %u placed left", removed, u32(s_placed.size()));
    Save();
}

void SVProp::Save()
{
    string_path fname;
    if (!persist_path(fname))
        return;

    // Plain stdio, the same choice sv_anomaly made and for the same reason: this
    // has to be on disk when the process dies, and this server has no clean exit
    // to flush anything at.
    FILE* f = fopen(fname, "wb");
    if (!f)
    {
        Msg("! [sv_prop] cannot write '%s' -- placed props will not survive a restart", fname);
        return;
    }

    fprintf(f, "; Props the server placed on this level. kind section visual x y z yaw pitch bank\n");
    fprintf(f, "; kind is 'prop' or 'lamp'; a visual of '-' means the section's own model.\n");
    fprintf(f, "; Delete a line to remove one, or delete the file to clear them all.\n");
    write_rows(f);
    fclose(f);
}

u32 SVProp::Load(bool force)
{
    if (!s_placed.empty() && !force)
    {
        Msg("! [sv_prop] %u objects are already placed -- loading again would double them. sv_prop_del mine first.", u32(s_placed.size()));
        return 0;
    }

    string_path fname;
    if (!persist_path(fname))
    {
        Msg("! [sv_prop] no level loaded -- nothing to load props into");
        return 0;
    }

    // Read BOTH files, and read them fully before placing anything: every
    // placement writes the working file back, and a reader still holding it open
    // would be parsing a file being rewritten underneath it.
    //
    // ⭐Baked first, because that is the world as it was authored; the working
    // file is whatever this machine has added since and has not published.
    xr_vector<Row> rows;
    u32 bad = 0;

    string_path baked{};
    u32 from_baked = 0;
    if (baked_path(baked) && read_rows(baked, rows, bad))
        from_baked = u32(rows.size());

    xr_vector<Row> local;
    if (read_rows(fname, local, bad))
    {
        for (const Row& r : local)
        {
            const bool dup = std::any_of(rows.begin(), rows.end(), [&](const Row& b) { return same_placement(r, b); });
            if (!dup)
                rows.push_back(r);
        }
    }

    if (rows.empty())
        return 0; // neither file exists, or both are empty -- the ordinary case

    u32 placed = 0;
    for (const Row& r : rows)
    {
        const EKind kind = (0 == xr_strcmp(r.kind, "lamp")) ? kLamp : kProp;
        const char* visual = (0 == xr_strcmp(r.visual, "-")) ? "" : r.visual;
        Fvector angle;
        angle.set(deg2rad(r.angle_deg.x), deg2rad(r.angle_deg.y), deg2rad(r.angle_deg.z));

        if (visual[0] && !model_exists(visual))
        {
            Msg("! [sv_prop] '%s' is not in this install -- skipping that line", visual);
            ++bad;
            continue;
        }

        // Whether this line was a fire is written in the section it names --
        // the file records what was built, so nothing has to be remembered
        // alongside it.
        const bool fire = (0 == xr_strcmp(r.section, CAMPFIRE_LAMP_SECTION));

        const u16 id = build(kind, r.section, visual, r.pos, angle, fire);
        if (id == 0xffff)
            continue;

        // ⚠Pairing is NOT restored, and that is deliberate rather than missing.
        // The file records placements, not the acts that made them; a campfire
        // read back is two independent objects standing in the same spot, which
        // is exactly what it looks like on the map. The only thing lost is that
        // deleting one half no longer takes the other with it, and inventing a
        // pairing from proximity would guess wrong the first time two campfires
        // stood side by side.
        remember(id, 0xffff, kind, r.section, visual, r.pos, angle);
        ++placed;
    }

    Msg("~ [sv_prop] %u of %u props restored (%u baked in '%s', %u local)%s", placed, u32(rows.size()), from_baked, baked, u32(rows.size()) - from_baked,
        bad ? " -- some lines were unusable" : "");
    return placed;
}

void SVProp::Bake()
{
    string_path baked;
    if (!baked_path(baked))
    {
        Msg("! [sv_prop] no level loaded -- nothing to bake");
        return;
    }

    if (s_placed.empty())
    {
        // Refused rather than obeyed: baking nothing would overwrite a good
        // content file with an empty one, and the command that did it reads like
        // a save. Emptying a map is a thing you should have to mean.
        Msg("! [sv_prop] nothing is placed -- refusing to bake an empty file over '%s'", baked);
        Msg("- [sv_prop] to really clear it, delete that file by hand");
        return;
    }

    FILE* f = fopen(baked, "wb");
    if (!f)
    {
        // ⚠The likely cause is that gamedata is packed, or read-only. Say so:
        // "bake failed" with no reason sends people looking at the wrong half.
        Msg("! [sv_prop] cannot write '%s' -- is gamedata packed or read-only?", baked);
        return;
    }

    fprintf(f, "; Props baked into the content for this level. kind section visual x y z yaw pitch bank\n");
    fprintf(f, "; Every server shipping this file furnishes the map the same way, and every\n");
    fprintf(f, "; client sees it because the server spawns these like any other entity.\n");
    fprintf(f, "; Written by sv_prop_bake. Safe to edit or diff by hand.\n");
    write_rows(f);
    fclose(f);

    Msg("* [sv_prop] baked %u objects into '%s'", u32(s_placed.size()), baked);

    // ⭐And empty the working copy, because everything in it is now content.
    // Left behind, its lines would be loaded a second time on the next start --
    // deduplicated, as it happens, but only by a rule that has to keep being
    // right. Nothing to dedupe is a better guarantee than dedupe that works.
    string_path work;
    if (persist_path(work))
    {
        if (FILE* w = fopen(work, "wb"))
        {
            fprintf(w, "; empty: these props are baked into the content now. See %s\n", baked);
            fclose(w);
        }
    }

    Msg("- [sv_prop] ship that file with gamedata and every server places them at startup");
}

void SVProp::Tick()
{
    if (!g_pGameLevel)
        return;

    const CALifeSimulator* alife = ai().get_alife();
    if (!alife || !ai().get_level_graph() || !ai().get_game_graph() || !ai().get_cross_table())
        return;

    // ⚠And the world has to be populated, not merely loaded. The A-Life actor is
    // the marker for that: it is what the switch manager measures everything else
    // against, and placing objects before it exists is asking a simulator that
    // has not started yet to file them.
    if (!alife->graph().actor())
        return;

    if (s_autoloaded)
        return;

    s_autoloaded = true; // one attempt per world, found anything or not
    Load(false);
}

void SVProp::Reset()
{
    s_placed.clear();
    s_autoloaded = false;
}

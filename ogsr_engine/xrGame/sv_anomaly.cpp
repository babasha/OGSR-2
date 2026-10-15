#include "StdAfx.h"
#include "sv_anomaly.h"

#include <mutex>

#include "sv_world_lookup.h"
#include "../xrCore/load_prof.h" // [net prof]: which part of sv.anomaly
#include "sv_npc.h" // ReferenceEntity -- the same "who counts as the player" answer

#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "clsid_game.h"
#include "GameObject.h"
#include "Level.h" // timeServer() for the broadcast stamp
#include "xrMessages.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h" // graph().actor() -- "is the world populated yet"
#include "alife_level_registry.h" // graph().level() -- the zones of this level only
#include "../COMMON_AI/GRAPH/level_graph.h"
#include "../COMMON_AI/GRAPH/game_graph.h"
#include "../COMMON_AI/GRAPH/game_level_cross_table.h"

// Set for the duration of a command that arrived over rcon, so the verbs below
// can tell WHICH administrator is asking. Owned by console_commands.cpp.
extern u64 g_admin_actor_account;

// sv_anomaly_birth: seconds between one anomalous field producing one artefact,
// with nobody standing in it. 0 turns the whole thing off.
//
// ⭐⭐THE STOCK CYCLE NEEDS A VISITOR, and that is backwards for this design.
// Read from the engine, not guessed: CCustomZone::shedule_Update clears
// m_bZoneActive every tick and only sets it when something is INSIDE the zone,
// and CheckForAwaking will not leave the idle state without it -- so an anomaly
// nobody is standing in never blows out and never births anything. That is
// right for a corridor shooter, where the anomaly is a hazard you walk past.
// It is wrong for a persistent world whose whole economic loop is "walk to the
// field, take what it made, carry it home": under the stock rule the field is
// empty when you arrive and only starts producing once you are already in it.
//
// ⚠A RATE, and separately a CEILING (sv_anomaly_birth_cap). A rate alone is a
// field with four hundred artefacts on it after a quiet night.
//
// ⚠⚠OFF BY DEFAULT. The in-process server of a single-player game runs this
// same Tick, and this content turns engine births off on EVERY zone
// (spawn_blowout_artefacts = off, noartf_* sections) -- artefacts come from the
// mod's own placement. A default of 300 s ignored that: 45 artefacts in the
// first 70 s on Cordon (log 11-10-2026), and since the nursery is not saved,
// every load handed each field another 3 on top of what the save kept. A
// persistent-world server turns production on explicitly (sv_anomaly_birth 300).
int g_sv_anomaly_birth = 0;

// How many uncollected artefacts one field may be sitting on. Counted, not
// assumed -- see Nursery.
int g_sv_anomaly_birth_cap = 3;

namespace
{
// The zone classes compiled into this engine, taken from the "Zones" block of
// object_factory_register.cpp.
//
// ⚠⚠THIS LIST IS NOT THE ANSWER, and believing it was cost this module its
// first version. A mod registers classes of its own from Lua
// (CObjectFactory::register_script_class), and THIS install's real anomalies
// are all of them: ZS_BFUZZ, ZS_MBALD, ZS_ZHARKA... none of which any list
// compiled into the engine can contain. Measured 01-08-2026: the level had 31
// producing anomaly fields; a list-based sv_anomaly_kinds offered three, and
// sv_anomaly_spawn refused the very sections the map is made of.
//
// It survives only as a seed for the census below, so that a section type which
// exists in content but happens to stand nowhere on the loaded level is still
// offered.
constexpr CLASS_ID ENGINE_ZONE_CLASSES[] = {
    CLSID_ZONE, CLSID_Z_MBALD, CLSID_Z_MINCER, CLSID_Z_ACIDF, CLSID_Z_GALANT, CLSID_Z_RADIO,
    CLSID_Z_BFUZZ, CLSID_Z_RUSTYH, CLSID_Z_DEAD, CLSID_Z_TORRID, CLSID_Z_AMEBA, CLSID_Z_NOGRAVITY,
};

// What a bare sv_anomaly_spawn reaches. Chosen against the level's own zones,
// which sv_anomaly_list measures: on pripyat_full they run roughly 3-10 m, so a
// default in that band produces something that reads as an anomaly rather than
// as a puddle or as a district.
constexpr float DEFAULT_RADIUS = 6.f;

// ⚠The lower bound is not politeness. A sphere small enough to sit between the
// points a walking man's capsule is tested at is an anomaly that never fires,
// and it looks identical to a broken one.
constexpr float MIN_RADIUS = 1.f;
constexpr float MAX_RADIUS = 100.f;

// What sv_anomaly_list covers when nobody says. Wide enough to take in the
// whole patch an operator is standing in, short enough that the answer is a
// screenful rather than the level's entire zone census.
constexpr float DEFAULT_LIST_RADIUS = 100.f;

// One anomaly this module put on the map.
//
// The roster is kept for two reasons that both matter: "mine" has to mean
// something in sv_anomaly_del, and whatever persists these across a restart
// needs to know which zones the operator authored rather than re-saving the
// level's own spawn file back at itself.
struct PlacedZone
{
    u16 id{0xffff};
    shared_str section;
    Fvector pos{};
    float radius{0.f};
};

xr_vector<PlacedZone> s_placed;

// ⭐The classes that are anomalies HERE: the engine's own, plus every class
// actually worn by a zone standing on the loaded level.
//
// The second half is what makes this right on modded content. The world is the
// authority on what a zone is in this install -- if something is filed in the
// A-Life registry as a custom zone, then its class is an anomaly class, whoever
// registered it and however.
void zone_classes(xr_vector<CLASS_ID>& out)
{
    out.clear();
    for (const CLASS_ID& z : ENGINE_ZONE_CLASSES)
        out.push_back(z);

    const CALifeSimulator* alife = ai().get_alife();
    if (!alife)
        return;

    for (const auto& it : alife->objects().objects())
    {
        if (!smart_cast<CSE_ALifeCustomZone*>(it.second))
            continue;
        const CLASS_ID c = it.second->m_tClassID;
        if (std::find(out.begin(), out.end(), c) == out.end())
            out.push_back(c);
    }
}

// Whether a section builds an anomaly, asked of the FACTORY rather than of a
// list.
//
// ⭐A probe is the only test that cannot be out of date: it runs exactly the
// machinery sv_anomaly_spawn is about to run and looks at what came out.
// ⚠It costs no entity id -- F_entity_Create does not touch the generator;
// spawn_item assigns one afterwards -- and nothing is registered anywhere.
bool is_zone_section(const char* section)
{
    if (!pSettings->section_exist(section) || !pSettings->line_exist(section, "class"))
        return false;

    CSE_Abstract* probe = F_entity_Create(section);
    if (!probe)
        return false;

    const bool zone = !!smart_cast<CSE_ALifeCustomZone*>(probe);
    F_entity_Destroy(probe);
    return zone;
}

// How far a zone actually reaches, in metres from its own origin.
//
// ⭐This is the number an operator means by "how big is it", and it is not
// stored anywhere as such: the shape is a list, it may hold several spheres and
// boxes, and each sits at its own offset. The outer reach is the max over all
// of them, which is also exactly what CCF_Shape::ComputeBounds ends up with.
float zone_reach(const CSE_ALifeSpaceRestrictor* zone)
{
    float reach = 0.f;
    for (const CShapeData::shape_def& S : zone->shapes)
    {
        if (S.type == 0)
            reach = std::max(reach, S.data.sphere.P.magnitude() + S.data.sphere.R);
        else
        {
            // The unit cube scaled by the matrix: its i/j/k lengths are the full
            // extents, so the half-diagonal is half the length of their sum in
            // quadrature.
            const Fmatrix& B = S.data.box;
            const float half = 0.5f * _sqrt(B.i.square_magnitude() + B.j.square_magnitude() + B.k.square_magnitude());
            reach = std::max(reach, B.c.magnitude() + half);
        }
    }
    return reach;
}

// The shape of a zone, said in words, because "radius 8" and "a box" are
// different facts and a listing that prints one number for both is lying about
// half the level.
void describe_shape(const CSE_ALifeSpaceRestrictor* zone, string64& out)
{
    u32 spheres = 0, boxes = 0;
    for (const CShapeData::shape_def& S : zone->shapes)
        (S.type == 0) ? ++spheres : ++boxes;

    if (zone->shapes.empty())
        xr_strcpy(out, "NO SHAPE"); // and therefore a 1 m ball on every client
    else if (spheres == 1 && !boxes)
        xr_sprintf(out, "r=%.1fm", zone_reach(zone));
    else
        xr_sprintf(out, "%.1fm across %u spheres %u boxes", zone_reach(zone), spheres, boxes);
}

// Level and game vertices for a point.
//
// ⚠A zone does NOT stand on the navmesh -- CSE_ALifeSpaceRestrictor turns
// flUsedAI_Locations off, and an anomaly over a pit is a perfectly good
// anomaly. The graph vertex is still required, and for one reason only: the
// A-Life graph registry files objects by game vertex, and add_online asserts
// that the vertex's level is the loaded one. So the nearest vertex is looked up
// to answer "which level is this", and the POSITION is left exactly where it
// was asked for.
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

// Every zone standing on the level that is loaded.
//
// ⚠The level filter is the same one sv_scenario needs and for the same reason:
// the A-Life registry holds the whole game's spawn at once.
void collect(xr_vector<CSE_ALifeCustomZone*>& out)
{
    out.clear();

    const CALifeSimulator* alife = ai().get_alife();
    if (!alife || !ai().get_level_graph() || !ai().get_game_graph())
        return;

    const GameGraph::_LEVEL_ID here = ai().level_graph().level_id();

    // ⭐This level's registry, not every object in the game: 📏07-10-2026
    // Switch v215 the 30 s survey walked all ~34k ALife objects with a
    // smart_cast each -- 20 ms of the second thread, the main thread waiting.
    // A zone is never anyone's child, so it is always in its level's registry.
    if (!alife->graph().actor() || alife->graph().level().level_id() != here)
        return;
    for (const auto& it : alife->graph().level().objects())
    {
        CSE_ALifeCustomZone* zone = smart_cast<CSE_ALifeCustomZone*>(it.second);
        if (!zone)
            continue;

        const GameGraph::_GRAPH_ID gvid = zone->m_tGraphID;
        if (!ai().game_graph().valid_vertex_id(gvid))
            continue;
        if (ai().game_graph().vertex(gvid)->level_id() != here)
            continue;

        out.push_back(zone);
    }
}

// ⭐⭐Per-instance data a runtime spawn cannot invent, taken from a zone of the
// same section that the LEVEL already has.
//
// ⚠⚠This exists because the first anomaly the server ever placed killed the
// player's client outright (01-08-2026, live): a visual zone plays cycles named
// in `attack_animation` / `startup_animation`, and those are authored PER
// INSTANCE in the SDK, not carried by the section. A fresh entity has them
// empty, CVisualZone::net_Spawn asked the model for cycle "", and ID_Cycle
// asserts on a name it cannot find.
//
// ⭐Same shape of answer as the class census: the WORLD knows what this content
// does, the code does not. A zone the level authored has correct names for its
// own model, and any instance of the same section will do.
// (The client no longer dies without them either -- see ZoneVisual.cpp -- but
// an anomaly that stands frozen is still the wrong picture, and this is the
// only place the right names exist.)
bool copy_instance_data(CSE_Abstract* fresh)
{
    CSE_ALifeZoneVisual* target = smart_cast<CSE_ALifeZoneVisual*>(fresh);
    if (!target)
        return true; // not a visual zone: nothing per-instance to borrow

    // Content first, where it says anything. The engine's own author left the
    // read commented out in CSE_ALifeZoneVisual's constructor, so some sections
    // do carry it -- and a section that names its own cycles outranks a guess
    // taken from a neighbour.
    if (pSettings->line_exist(target->s_name, "startup_animation"))
        target->startup_animation = pSettings->r_string(target->s_name, "startup_animation");
    if (pSettings->line_exist(target->s_name, "blast_animation"))
        target->attack_animation = pSettings->r_string(target->s_name, "blast_animation");

    if (target->startup_animation.size() && target->attack_animation.size())
        return true;

    const CALifeSimulator* alife = ai().get_alife();
    if (!alife)
        return false;

    // Two passes, best match first.
    //
    // ⭐Pass 1 is the MODEL, because cycle names live in the .ogf and a section
    // family (zone_burning_fuzz_weak / _average / _strong / _bottom_*) shares
    // one. Matching by SECTION NAME found nothing for the first anomaly tried,
    // and matching by model found nothing either -- for a reason worth writing
    // down: the level's instances carry a per-instance visual override
    // (`dynamics\anomaly\topolinypuh_big`) pointing at a model THIS CONTENT DOES
    // NOT HAVE, so the client falls back to the section's own
    // `physics\anomaly\topolinypuh` and logs it. Donor and target therefore
    // disagree about the model on paper while drawing the same one.
    //
    // ⭐Pass 2 is the CLASS, which survives that: whatever the spawn file claims
    // about visuals, two objects of the same zone class are the same kind of
    // anomaly, and a name that turns out not to fit is now harmless -- the
    // client resolves cycles safely and simply stands still.
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const auto& it : alife->objects().objects())
        {
            CSE_ALifeZoneVisual* donor = smart_cast<CSE_ALifeZoneVisual*>(it.second);
            if (!donor || donor == target)
                continue;

            if (pass == 0)
            {
                if (xr_strcmp(donor->visual_name, target->visual_name) != 0)
                    continue;
            }
            else if (donor->m_tClassID != target->m_tClassID)
                continue;

            if (!donor->startup_animation.size() && !donor->attack_animation.size())
                continue;

            if (!target->startup_animation.size())
                target->startup_animation = donor->startup_animation;
            if (!target->attack_animation.size())
                target->attack_animation = donor->attack_animation;

            Msg("~ [sv_anomaly] '%s' took its cycles from '%s' (%s): idle '%s', attack '%s'", target->s_name.c_str(), donor->s_name.c_str(),
                pass == 0 ? "same model" : "same class", target->startup_animation.c_str(), target->attack_animation.c_str());
            return true;
        }
    }

    return false;
}

bool ours(u16 id)
{
    for (const PlacedZone& P : s_placed)
        if (P.id == id)
            return true;
    return false;
}

// One attempt per world. Set whether or not the file existed: "there is
// nothing to load" is an answer, and retrying it every server tick would be a
// path lookup twenty times a second for the life of the process.
bool s_autoloaded = false;

typedef std::pair<shared_str, float> Weighted; // (artefact section, normalised probability)

// A zone's own artefact table, normalised. False when it has none worth using.
//
// ⚠line_exist is NOT the test. Sections inherit, so every zone in this content
// "has" an artefacts line -- 53 of the 56 inherit an EMPTY one. The table is
// only real if it parses into at least one entry with weight.
bool read_table_uncached(const char* section, xr_vector<Weighted>& out);

// The config does not change under a running game: one parse per section.
// 📏07-10-2026 Switch v219: the 30 s survey re-read every zone's table, 9-14 ms
// of the second thread even after walking only this level's objects.
bool read_table(const char* section, xr_vector<Weighted>& out)
{
    static xr_map<shared_str, xr_vector<Weighted>> s_cache; // empty = "no table"
    static std::mutex s_mx;
    const shared_str key(section);
    {
        std::lock_guard<std::mutex> g(s_mx);
        auto it = s_cache.find(key);
        if (it != s_cache.end())
        {
            out = it->second;
            return !out.empty();
        }
    }
    const bool ok = read_table_uncached(section, out);
    std::lock_guard<std::mutex> g(s_mx);
    s_cache.emplace(key, ok ? out : xr_vector<Weighted>());
    return ok;
}

bool read_table_uncached(const char* section, xr_vector<Weighted>& out)
{
    out.clear();
    if (!pSettings->line_exist(section, "artefacts"))
        return false;

    LPCSTR list = pSettings->r_string(section, "artefacts");
    if (!list || !list[0])
        return false;

    const u32 items = _GetItemCount(list) / 2; // pairs of (section, weight)
    float total = 0.f;
    for (u32 i = 0; i < items; ++i)
    {
        string256 name = {}, weight = {};
        _GetItem(list, 2 * i, name);
        _GetItem(list, 2 * i + 1, weight);
        const float w = float(atof(weight));
        if (!pSettings->section_exist(name) || w <= 0.f)
            continue;
        out.emplace_back(name, w);
        total += w;
    }

    if (out.empty() || fis_zero(total))
    {
        out.clear();
        return false;
    }

    for (Weighted& e : out)
        e.second /= total;
    return true;
}

// One artefact from `table`, somewhere inside the zone. Returns its id, or
// 0xffff.
u16 place_artefact(CSE_ALifeSpaceRestrictor* zone, const xr_vector<Weighted>& table, float reach, shared_str& what)
{
    CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());
    if (!alife || table.empty())
        return 0xffff;

    what = table.front().first;
    if (table.size() > 1)
    {
        const float roll = ::Random.randF(1.f);
        float sum = 0.f;
        for (const Weighted& e : table)
        {
            sum += e.second;
            if (roll < sum)
            {
                what = e.first;
                break;
            }
        }
    }

    // Scattered across the zone rather than stacked on its centre: a pile on
    // one coordinate is one pickup prompt fighting with the rest, and it does
    // not read as a field that produced them.
    //
    // ⭐sqrt of the roll, not the roll: without it the points bunch towards the
    // middle, because a disc has more area further out.
    const float ang = ::Random.randF(PI_MUL_2);
    const float dist = reach * _sqrt(::Random.randF(1.f));

    Fvector pos = zone->position();
    pos.x += dist * _cos(ang);
    pos.z += dist * _sin(ang);

    // Height off the AI graph where there is one: an artefact dropped at the
    // zone's own y can be inside the floor or a metre in the air, and the first
    // is unrecoverable.
    u32 lvid = 0;
    GameGraph::_GRAPH_ID gvid = GameGraph::_GRAPH_ID(-1);
    // Timed: on the Switch the second thread lost ~166 ms in the very frame a nearby
    // field produced -- the graph lookup or the A-Life spawn, this says which.
    const u64 t0 = CPU::QPC();
    if (!graph_ids(pos, lvid, gvid))
        return 0xffff;
    if (ai().level_graph().valid_vertex_id(lvid))
    {
        // ⚠A point off the walkable area keeps nothing of its own but the
        // vertex it was matched to: left at its x/z it lies inside a wall, or
        // -- the fences on Cordon's southern edge -- past the end of the level,
        // where nobody can walk to it.
        if (!ai().level_graph().inside(lvid, pos))
        {
            const Fvector v = ai().level_graph().vertex_position(lvid);
            pos.x = v.x;
            pos.z = v.z;
        }
        pos.y = ai().level_graph().vertex_plane_y(lvid, pos.x, pos.z) + 0.2f;
    }
    const u64 t1 = CPU::QPC();

    CSE_Abstract* art;
    {
        LoadProf::Scope _lp("anom.spawn_item");
        art = alife->spawn_item(*what, pos, lvid, gvid, ALife::_OBJECT_ID(-1));
    }
    const u64 t2 = CPU::QPC();
    const double k = 1000.0 / double(CPU::QPCFreq());
    if (k * double(t2 - t0) > 10.0)
        Msg("~ [sv_anomaly] placing '%s' took %.0f ms: graph lookup %.0f | alife spawn_item %.0f", what.c_str(), k * double(t2 - t0),
            k * double(t1 - t0), k * double(t2 - t1));
    return art ? art->ID : u16(0xffff);
}

// ⭐What a field has produced and not yet had taken from it.
//
// The cap lives on THIS, not on a rate: a field nobody visits for eight hours
// must not be sitting on ninety artefacts, and the only honest way to say "it
// is full" is to count what is still lying there. Ids that no longer resolve
// are gone -- picked up, or destroyed -- and the field is that much emptier.
struct Nursery
{
    u16 zone{0xffff};
    xr_vector<u16> born;
    u32 next_ms{0};
};

xr_vector<Nursery> s_nursery;
u32 s_next_survey_ms = 0;

// Where the placed zones for the loaded level are written.
//
// Per level, because that is the unit an anomaly belongs to -- a coordinate
// means nothing on another map, and a single shared file would need every line
// tagged and would still be one bad edit away from placing Pripyat's anomalies
// on the Cordon.
bool persist_path(string_path& out)
{
    const CALifeSimulator* alife = ai().get_alife();
    if (!alife || !ai().get_level_graph() || !ai().get_game_graph())
        return false;

    string_path leaf;
    xr_sprintf(leaf, "anomalies_%s.ltx", alife->level_name().c_str());
    FS.update_path(out, "$app_data_root$", leaf);
    return true;
}

// ⭐⭐The BAKED anomalies: the same lines, but in the content instead of in one
// machine's runtime folder.
//
// The file above is a working copy. It lives in $app_data_root$, it is rewritten
// after every single edit, and it belongs to whoever happens to be running this
// process -- which is exactly right for "I am trying things out" and exactly
// wrong for "this is where the anomalies on this map are". Another server does
// not have it and never will.
//
// A baked file is content. It sits next to the configs, it travels wherever
// gamedata travels, and every server that ships it lays out the same world.
// Nothing writes it except sv_anomaly_bake, so a test placement can no longer
// become permanent by accident -- which it could, and once put a lethal zone
// 4.7 m from the spawn point of a live session.
//
// ⚠Flat name, no subfolder: update_path does not create directories, and a
// bake that silently fails because config\anomalies\ does not exist would be
// the worst possible failure for a command whose whole job is durability.
bool baked_path(string_path& out)
{
    const CALifeSimulator* alife = ai().get_alife();
    if (!alife || !ai().get_level_graph() || !ai().get_game_graph())
        return false;

    string_path leaf;
    xr_sprintf(leaf, "anomalies_%s.ltx", alife->level_name().c_str());
    FS.update_path(out, "$game_config$", leaf);
    return true;
}

// One line of a persisted file. Named at namespace scope because both files are
// read the same way and by the same reader.
struct Row
{
    string128 section;
    Fvector pos;
    float radius;
};

// Returns false only when the file is not there, which is the ordinary case for
// both of them and not a fault.
bool read_rows(const char* fname, xr_vector<Row>& rows, u32& bad)
{
    FILE* f = fopen(fname, "rb");
    if (!f)
        return false;

    char line[512];
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
        if (sscanf(p, "%127s %f %f %f %f", r.section, &r.pos.x, &r.pos.y, &r.pos.z, &r.radius) != 5)
        {
            Msg("! [sv_anomaly] %s:%u is not 'section x y z radius' -- skipped", fname, lineno);
            ++bad;
            continue;
        }
        rows.push_back(r);
    }
    fclose(f);
    return true;
}

// ⚠The two files overlap by design -- baking does not erase what is already
// baked -- so the same zone can be described twice. Placing it twice would put
// two anomalies in one spot, and the second is indistinguishable from the first
// afterwards. Centimetre rounding, because these numbers made a round trip
// through "%.3f" and comparing floats for equality across that is a coin toss.
bool same_placement(const Row& a, const Row& b)
{
    // ⚠Not called "near": windows.h defines that as an empty macro, and the
    // lambda collapses into "auto = [](...)" -- an error that points at the
    // assignment and says nothing about where the name went.
    auto same = [](float x, float y) { return _abs(x - y) < 0.01f; };
    return 0 == xr_strcmp(a.section, b.section) && same(a.pos.x, b.pos.x) && same(a.pos.y, b.pos.y) && same(a.pos.z, b.pos.z) && same(a.radius, b.radius);
}
} // namespace

bool SVAnomaly::Origin(Fvector& out)
{
    xrServer* SV = SVWorld::Server();

    // The administrator who typed it, when one did. Two operators in the same
    // world must not place things at each other's feet.
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

    // Typed on the host console: there is no author, so it means "by the
    // player", which is what every other server-side verb here means too.
    if (CSE_Abstract* e = SVWorld::Find(SVNpc::ReferenceEntity()))
    {
        out = e->position();
        return true;
    }

    Msg("! [sv_anomaly] nowhere to put it -- no player entity exists yet, and no point was given");
    return false;
}

void SVAnomaly::Kinds(const char* filter)
{
    // ⚠Filtered by CLASS rather than probed section by section on purpose: a
    // probe per section means constructing every entity this content declares,
    // and a CSE_ALifeCustomZone constructor resolves hit_type through
    // g_tfString2HitType, which FATALs on a string it does not recognise. A
    // report that walks all of a mod's content must not be able to take the
    // server down over one typo in somebody's ini.
    xr_vector<CLASS_ID> classes;
    zone_classes(classes);

    u32 found = 0;
    for (const auto& it : pSettings->sections())
    {
        const auto& section = it.first;
        if (!pSettings->line_exist(section, "class"))
            continue;
        if (std::find(classes.begin(), classes.end(), TEXT2CLSID(pSettings->r_string(section, "class"))) == classes.end())
            continue;
        if (filter && filter[0] && !strstr(section.c_str(), filter))
            continue;

        // What it does and how hard, because those are the two things an
        // operator picks a section BY.
        //
        // ⚠"does it have an artefacts line" was here first and was worthless:
        // sections inherit from a base, line_exist resolves through the
        // inheritance, and so all 56 rows claimed to birth artefacts. A column
        // that reads the same on every row is not information -- it is
        // decoration that looks like a fact. What varies is the TABLE, so the
        // number of kinds in it is what goes here.
        const char* hit = pSettings->line_exist(section, "hit_type") ? pSettings->r_string(section, "hit_type") : "-";
        const float power = READ_IF_EXISTS(pSettings, r_float, section, "max_start_power", 0.f);
        const u32 arts = pSettings->line_exist(section, "artefacts") ? (_GetItemCount(pSettings->r_string(section, "artefacts")) / 2) : 0;

        Msg("~ [sv_anomaly]   %-28s hit=%-14s power=%-6.2f %u artefact kind(s)", section.c_str(), hit, power, arts);
        ++found;
    }

    if (!found)
        Msg("! [sv_anomaly] no zone sections%s%s in this content", (filter && filter[0]) ? " matching " : "", (filter && filter[0]) ? filter : "");
    else
        Msg("~ [sv_anomaly] %u zone sections over %u zone classes; sv_anomaly_spawn <section> [radius]", found, u32(classes.size()));
}

void SVAnomaly::List(float radius)
{
    if (radius <= 0.f)
        radius = DEFAULT_LIST_RADIUS;

    xr_vector<CSE_ALifeCustomZone*> zones;
    collect(zones);

    if (zones.empty())
    {
        Msg("~ [sv_anomaly] no zones on this level at all");
        return;
    }

    Fvector ref;
    const bool have_ref = Origin(ref);

    u32 shown = 0, placed_shown = 0;
    for (CSE_ALifeCustomZone* z : zones)
    {
        const float away = have_ref ? ref.distance_to(z->position()) : 0.f;
        if (have_ref && away > radius)
            continue;

        string64 shape{};
        describe_shape(z, shape);

        const bool mine = ours(z->ID);
        if (mine)
            ++placed_shown;

        Msg("~ [sv_anomaly]   id=%-5u %-26s %-14s at (%.1f, %.1f, %.1f) %.0fm away%s", z->ID, z->s_name.c_str(), shape, VPUSH(z->position()), away, mine ? "  [placed]" : "");
        ++shown;
    }

    // Both totals, because they answer different questions: "is my command
    // working" is about the placed count, "is this a field" is about all of it.
    Msg("~ [sv_anomaly] %u zones within %.0fm (%u placed by the server); %u on the level", shown, radius, placed_shown, u32(zones.size()));
}

void SVAnomaly::Spawn(const char* section, float radius, const Fvector* at)
{
    if (!section || !section[0])
    {
        Msg("! [sv_anomaly] sv_anomaly_spawn <section> [radius] [x y z] -- sv_anomaly_kinds lists the sections");
        return;
    }

    if (!pSettings->section_exist(section))
    {
        Msg("! [sv_anomaly] no such section '%s'", section);
        return;
    }

    // Checked BEFORE anything is created: spawn_item takes an entity id off the
    // server's generator on its way in, and an id spent on an object that is
    // then thrown away is an id nobody ever gets back.
    if (!is_zone_section(section))
    {
        Msg("! [sv_anomaly] '%s' is not a zone -- its class is '%s'. Run sv_anomaly_kinds for the ones that are.", section,
            pSettings->line_exist(section, "class") ? pSettings->r_string(section, "class") : "(none)");
        return;
    }

    Fvector pos;
    if (at)
        pos = *at;
    else if (!Origin(pos))
        return;

    if (radius <= 0.f)
        radius = DEFAULT_RADIUS;
    const float asked = radius;
    clamp(radius, MIN_RADIUS, MAX_RADIUS);

    CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());
    if (!alife)
    {
        Msg("! [sv_anomaly] no A-Life simulator -- is a level loaded?");
        return;
    }

    u32 lvid = 0;
    GameGraph::_GRAPH_ID gvid = GameGraph::_GRAPH_ID(-1);
    if (!graph_ids(pos, lvid, gvid))
    {
        Msg("! [sv_anomaly] no AI graph vertex anywhere near (%.1f, %.1f, %.1f) -- cannot file a zone there", VPUSH(pos));
        return;
    }

    // ⭐registration = false is the whole trick. The shape has to be on the
    // entity BEFORE it is registered, because registering is what eventually
    // hands the object to the switch manager, and the spawn packet the clients
    // receive is written from the entity as it stands at that moment.
    CSE_Abstract* obj = alife->spawn_item(section, pos, lvid, gvid, ALife::_OBJECT_ID(-1), false);
    if (!obj)
    {
        Msg("! [sv_anomaly] spawn of '%s' failed", section);
        return;
    }

    CSE_ALifeSpaceRestrictor* shaped = smart_cast<CSE_ALifeSpaceRestrictor*>(obj);
    CSE_ALifeDynamicObject* dynamic = smart_cast<CSE_ALifeDynamicObject*>(obj);
    if (!shaped || !dynamic)
    {
        // The class list said this was a zone and the factory disagreed. Not
        // recoverable here and not worth guessing at: say so and leave the
        // half-built object unregistered rather than putting something that is
        // not a restrictor into the world.
        Msg("!! [sv_anomaly] '%s' built a '%s' that is not a space restrictor -- not registering it", section, obj->s_name.c_str());
        return;
    }

    CShapeData::shape_def S{};
    S.type = 0; // sphere
    S.data.sphere.P.set(0.f, 0.f, 0.f); // local space: the zone's own origin
    S.data.sphere.R = radius;
    shaped->shapes.clear();
    shaped->shapes.push_back(S);

    // ⚠Before registration, like the shape, and for the same reason: what the
    // clients receive is written from the entity as it stands the moment it
    // goes online.
    if (!copy_instance_data(obj))
        Msg("!! [sv_anomaly] '%s' is a visual zone and this level has no instance of it to take animation names from -- it will stand still", section);

    alife->register_object(dynamic, true);

    PlacedZone P;
    P.id = obj->ID;
    P.section = section;
    P.pos = pos;
    P.radius = radius;
    s_placed.push_back(P);

    string64 clamped{};
    if (!fsimilar(asked, radius))
        xr_sprintf(clamped, " (asked for %.1f, clamped)", asked);

    Msg("~ [sv_anomaly] placed '%s' id=%u r=%.1fm at (%.1f, %.1f, %.1f)%s", section, obj->ID, radius, VPUSH(pos), clamped);
    Save();
}

void SVAnomaly::Resize(u16 id, bool all_placed, float radius)
{
    if (radius <= 0.f)
    {
        Msg("! [sv_anomaly] sv_anomaly_size <id|mine> <radius>");
        return;
    }

    const float asked = radius;
    clamp(radius, MIN_RADIUS, MAX_RADIUS);

    xr_vector<u16> targets;
    if (all_placed)
    {
        for (const PlacedZone& P : s_placed)
            targets.push_back(P.id);
    }
    else
        targets.push_back(id);

    u32 done = 0;
    for (u16 target : targets)
    {
        CSE_ALifeSpaceRestrictor* zone = smart_cast<CSE_ALifeSpaceRestrictor*>(SVWorld::Find(target));
        if (!zone)
        {
            Msg("! [sv_anomaly] id=%u is not a zone the server knows about", target);
            continue;
        }

        // ⭐The entity FIRST: this is the copy every later joiner is built from,
        // and leaving it stale would mean the anomaly is one size for the people
        // standing in it and another for anyone who walks up afterwards. That
        // class of disagreement is invisible until somebody dies to it.
        CShapeData::shape_def S{};
        S.type = 0;
        S.data.sphere.P.set(0.f, 0.f, 0.f);
        S.data.sphere.R = radius;
        zone->shapes.clear();
        zone->shapes.push_back(S);

        // ...and then the clients already holding one.
        //
        // ⚠⚠Broadcast DIRECTLY, not through CGameObject::u_EventSend. That
        // helper posts the event back into this process's own server, where
        // xrServer::Process_event switches on the type and R_ASSERTs on
        // anything it does not implement -- so an event meant purely for
        // clients kills the host the first time it is sent. Measured
        // 01-08-2026: the first sv_anomaly_size took the server down with
        // "Game Event not implemented!!!". GE_NPC_SHOT and GE_NPC_ACTIVE_ITEM
        // are sent this way for exactly the same reason; SVNpc::Remove can use
        // the helper only because GE_DESTROY is a type the server does handle.
        if (xrServer* SV = SVWorld::Server())
        {
            NET_Packet P;
            P.w_begin(M_EVENT);
            P.w_u32(Level().timeServer());
            P.w_u16(GE_ZONE_RESHAPE);
            P.w_u16(target);
            P.w_float(radius);
            SV->SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
        }

        for (PlacedZone& mine : s_placed)
            if (mine.id == target)
                mine.radius = radius;

        ++done;
    }

    string64 clamped{};
    if (!fsimilar(asked, radius))
        xr_sprintf(clamped, " (asked for %.1f, clamped)", asked);

    Msg("~ [sv_anomaly] %u zone(s) now reach %.1fm%s", done, radius, clamped);
    Save();
}

void SVAnomaly::SpawnArtefacts(u16 zone_id, const char* section, int count)
{
    CSE_ALifeSpaceRestrictor* zone = smart_cast<CSE_ALifeSpaceRestrictor*>(SVWorld::Find(zone_id));
    if (!zone)
    {
        Msg("! [sv_anomaly] id=%u is not a zone the server knows about -- sv_anomaly_list", zone_id);
        return;
    }

    if (count < 1)
        count = 1;
    clamp(count, 1, 32);

    // Where from, when nobody named a section: the zone's own artefact table,
    // which is the same line the client reads to decide what this anomaly can
    // ever produce. Naming a section by hand overrides it -- an operator
    // staging something specific should not have to fight the weights.
    xr_vector<Weighted> table;
    if (!section || !section[0])
    {
        if (!read_table(zone->s_name.c_str(), table))
        {
            Msg("! [sv_anomaly] '%s' has no artefact table of its own -- name a section: sv_anomaly_art <id> <section> [count]", zone->s_name.c_str());
            return;
        }
    }
    else
    {
        if (!pSettings->section_exist(section))
        {
            Msg("! [sv_anomaly] no such section '%s'", section);
            return;
        }
        table.emplace_back(section, 1.f);
    }

    const float reach = std::max(1.f, zone_reach(zone));

    u32 made = 0;
    for (int i = 0; i < count; ++i)
    {
        shared_str what;
        const u16 id = place_artefact(zone, table, reach, what);
        if (id == 0xffff)
            continue;

        Msg("~ [sv_anomaly]   '%s' id=%u", what.c_str(), id);
        ++made;
    }

    Msg("~ [sv_anomaly] %u artefact(s) in '%s' id=%u (within %.1fm of it)", made, zone->s_name.c_str(), zone_id, reach);
}

void SVAnomaly::Remove(u16 id, bool all_placed)
{
    xr_vector<u16> doomed;

    if (all_placed)
    {
        for (const PlacedZone& P : s_placed)
            doomed.push_back(P.id);
        s_placed.clear();
    }
    else
    {
        // A named id may be one of the level's own, and removing one on purpose
        // is a legitimate thing to want. Only "mine" is restricted.
        doomed.push_back(id);
        for (auto it = s_placed.begin(); it != s_placed.end(); ++it)
            if (it->id == id)
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

    Msg("~ [sv_anomaly] removed %u; %u placed zones left", removed, u32(s_placed.size()));
    Save();
}

void SVAnomaly::Save()
{
    string_path fname;
    if (!persist_path(fname))
        return;

    // Plain stdio, the same choice sv_telemetry made and for the same reason:
    // this has to be on disk when the process dies, and this server has no
    // clean exit to flush anything at.
    FILE* f = fopen(fname, "wb");
    if (!f)
    {
        Msg("! [sv_anomaly] cannot write '%s' -- placed anomalies will not survive a restart", fname);
        return;
    }

    fprintf(f, "; anomalies the server placed on this level. section x y z radius\n");
    fprintf(f, "; delete a line to remove one, or delete the file to clear them all.\n");
    for (const PlacedZone& P : s_placed)
        fprintf(f, "%s %.3f %.3f %.3f %.3f\n", P.section.c_str(), VPUSH(P.pos), P.radius);

    fclose(f);
}

u32 SVAnomaly::Load(bool force)
{
    if (!s_placed.empty() && !force)
    {
        Msg("! [sv_anomaly] %u zones are already placed -- loading again would double them. sv_anomaly_del mine first.", u32(s_placed.size()));
        return 0;
    }

    string_path fname;
    if (!persist_path(fname))
    {
        Msg("! [sv_anomaly] no level loaded -- nothing to load anomalies into");
        return 0;
    }

    // Read BOTH files, and read them fully before placing anything: Spawn()
    // writes the working file back after every zone, and a reader still holding
    // it open would be parsing a file being rewritten underneath it.
    //
    // ⭐Baked first, because that is the world as it was authored; the working
    // file is whatever this machine has added since and has not published.
    xr_vector<Row> rows;
    u32 bad = 0;

    string_path baked;
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
        const u32 before = u32(s_placed.size());
        Spawn(r.section, r.radius, &r.pos);
        if (u32(s_placed.size()) > before)
            ++placed;
    }

    Msg("~ [sv_anomaly] %u of %u anomalies restored (%u baked in '%s', %u local)%s", placed, u32(rows.size()), from_baked, baked, u32(rows.size()) - from_baked,
        bad ? " -- some lines were unreadable" : "");
    return placed;
}

void SVAnomaly::Bake()
{
    string_path baked;
    if (!baked_path(baked))
    {
        Msg("! [sv_anomaly] no level loaded -- nothing to bake");
        return;
    }

    if (s_placed.empty())
    {
        // Refused rather than obeyed: baking nothing would overwrite a good
        // content file with an empty one, and the command that did it reads
        // like a save. Emptying the map is a thing you should have to mean.
        Msg("! [sv_anomaly] nothing is placed -- refusing to bake an empty file over '%s'", baked);
        Msg("- [sv_anomaly] to really clear the map, delete that file by hand");
        return;
    }

    FILE* f = fopen(baked, "wb");
    if (!f)
    {
        // ⚠The likely cause is that gamedata is packed, or read-only. Say so:
        // "bake failed" with no reason sends people looking at the wrong half.
        Msg("! [sv_anomaly] cannot write '%s' -- is gamedata packed or read-only?", baked);
        return;
    }

    fprintf(f, "; Anomalies baked into the content for this level. section x y z radius\n");
    fprintf(f, "; Every server shipping this file lays out the same anomalies, and every\n");
    fprintf(f, "; client sees them because the server spawns them like any other entity.\n");
    fprintf(f, "; Written by sv_anomaly_bake. Safe to edit or diff by hand.\n");
    for (const PlacedZone& P : s_placed)
        fprintf(f, "%s %.3f %.3f %.3f %.3f\n", P.section.c_str(), VPUSH(P.pos), P.radius);
    fclose(f);

    Msg("* [sv_anomaly] baked %u anomalies into '%s'", u32(s_placed.size()), baked);

    // ⭐And empty the working copy, because everything in it is now content.
    // Left behind, its lines would be loaded a second time on the next start --
    // deduplicated, as it happens, but only by a rule that has to keep being
    // right. Nothing to dedupe is a better guarantee than dedupe that works.
    string_path work;
    if (persist_path(work))
    {
        if (FILE* w = fopen(work, "wb"))
        {
            fprintf(w, "; empty: these anomalies are baked into the content now. See %s\n", baked);
            fclose(w);
        }
    }

    Msg("- [sv_anomaly] ship that file with gamedata and every server places them at startup");
}

void SVAnomaly::Tick()
{
    if (!g_pGameLevel)
        return;

    const CALifeSimulator* alife = ai().get_alife();
    if (!alife || !ai().get_level_graph() || !ai().get_game_graph() || !ai().get_cross_table())
        return;

    // ⚠And the world has to be populated, not merely loaded. The A-Life actor
    // is the marker for that: it is what the switch manager measures everything
    // else against, and placing objects before it exists is asking a simulator
    // that has not started yet to file them.
    if (!alife->graph().actor())
        return;

    if (!s_autoloaded)
    {
        s_autoloaded = true; // one attempt per world, found anything or not
        Load(false);
    }

    Birth();
}

void SVAnomaly::Birth()
{
    if (g_sv_anomaly_birth <= 0)
        return;

    const u32 now = Device.dwTimeGlobal;

    // Re-survey occasionally rather than every tick: this walks the whole A-Life
    // registry, and the set of anomalies on a level changes when an operator
    // places one, not twenty times a second.
    if (now >= s_next_survey_ms)
    {
        s_next_survey_ms = now + 30000;
        LoadProf::Scope _lp("anom.survey");

        xr_vector<CSE_ALifeCustomZone*> zones;
        collect(zones);

        for (CSE_ALifeCustomZone* z : zones)
        {
            // Only fields that have something to give. In this content that is
            // three sections out of fifty-six, and a nursery entry for the
            // other fifty-three would be a timer that can never fire.
            xr_vector<Weighted> table;
            if (!read_table(z->s_name.c_str(), table))
                continue;

            bool known = false;
            for (const Nursery& n : s_nursery)
                if (n.zone == z->ID)
                    known = true;
            if (known)
                continue;

            Nursery n;
            n.zone = z->ID;
            // ⚠Staggered on purpose. Every field starting its clock at level
            // load means every field in the world produces in the same second,
            // for ever -- the map would pulse.
            n.next_ms = now + u32(::Random.randI(1, g_sv_anomaly_birth * 1000));
            s_nursery.push_back(n);
        }

        // Fields that are gone (an operator removed one, or the level changed
        // under us) take their nursery with them.
        s_nursery.erase(std::remove_if(s_nursery.begin(), s_nursery.end(), [](const Nursery& n) { return !SVWorld::Find(n.zone); }), s_nursery.end());
    }

    for (Nursery& n : s_nursery)
    {
        if (now < n.next_ms)
            continue;

        // ⭐The interval is jittered every time, not just at the start: a fixed
        // period is a field an experienced player can set a watch by, and the
        // point of a persistent world is that you do not know what you will
        // find when you get there.
        n.next_ms = now + u32(::Random.randI(g_sv_anomaly_birth * 500, g_sv_anomaly_birth * 1500));

        CSE_ALifeSpaceRestrictor* zone = smart_cast<CSE_ALifeSpaceRestrictor*>(SVWorld::Find(n.zone));
        if (!zone)
            continue;

        // What it made and nobody has taken. Pruned here rather than tracked by
        // events: an artefact can leave the world half a dozen ways (picked up,
        // sold, destroyed, despawned with its level), and a census that asks
        // "does this id still resolve" cannot miss one of them.
        n.born.erase(std::remove_if(n.born.begin(), n.born.end(), [](u16 id) { return !SVWorld::Find(id); }), n.born.end());

        if (int(n.born.size()) >= g_sv_anomaly_birth_cap)
            continue;

        xr_vector<Weighted> table;
        if (!read_table(zone->s_name.c_str(), table))
            continue;

        shared_str what;
        u16 id;
        {
            LoadProf::Scope _lp("anom.place");
            id = place_artefact(zone, table, std::max(1.f, zone_reach(zone)), what);
        }
        if (id == 0xffff)
            continue;

        n.born.push_back(id);
        LoadProf::Scope _lp("anom.msg"); // ⚠a Msg to the Wi-Fi debug link can block
        Msg("~ [sv_anomaly] '%s' id=%u produced '%s' id=%u (%u of %d on it now)", zone->s_name.c_str(), n.zone, what.c_str(), id, u32(n.born.size()), g_sv_anomaly_birth_cap);
    }
}

void SVAnomaly::LevelArtefactSections(xr_vector<shared_str>& out)
{
    out.clear();
    if (g_sv_anomaly_birth <= 0)
        return;

    xr_vector<CSE_ALifeCustomZone*> zones;
    collect(zones);
    xr_vector<Weighted> table;
    for (CSE_ALifeCustomZone* z : zones)
        if (read_table(z->s_name.c_str(), table))
            for (const Weighted& e : table)
                if (std::find(out.begin(), out.end(), e.first) == out.end())
                    out.push_back(e.first);
}

void SVAnomaly::BirthStatus()
{
    if (g_sv_anomaly_birth <= 0)
    {
        Msg("~ [sv_anomaly] production is OFF (sv_anomaly_birth 0)");
        return;
    }

    if (s_nursery.empty())
    {
        Msg("~ [sv_anomaly] no field on this level has an artefact table -- nothing can be produced. sv_anomaly_kinds shows which sections do.");
        return;
    }

    const u32 now = Device.dwTimeGlobal;
    for (Nursery& n : s_nursery)
    {
        CSE_Abstract* z = SVWorld::Find(n.zone);
        n.born.erase(std::remove_if(n.born.begin(), n.born.end(), [](u16 id) { return !SVWorld::Find(id); }), n.born.end());
        Msg("~ [sv_anomaly]   id=%-5u %-26s holding %u of %d, next in %us", n.zone, z ? z->s_name.c_str() : "?", u32(n.born.size()), g_sv_anomaly_birth_cap,
            (n.next_ms > now) ? (n.next_ms - now) / 1000 : 0);
    }

    Msg("~ [sv_anomaly] %u producing field(s), one artefact per ~%ds each, ceiling %d", u32(s_nursery.size()), g_sv_anomaly_birth, g_sv_anomaly_birth_cap);
}

u16 SVAnomaly::LastPlaced() { return s_placed.empty() ? u16(0xffff) : s_placed.back().id; }

void SVAnomaly::Placed(xr_vector<u16>& out)
{
    out.clear();
    for (const PlacedZone& P : s_placed)
        out.push_back(P.id);
}

void SVAnomaly::Reset()
{
    s_placed.clear();
    s_nursery.clear();
    s_next_survey_ms = 0;
    // The next world gets its own attempt: entity ids meant something only in
    // this one, and the level it loads may not be the level this one was.
    s_autoloaded = false;
}

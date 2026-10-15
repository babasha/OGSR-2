#include "StdAfx.h"
#include "sv_scenario.h"

#include "sv_npc.h"
#include "sv_world_lookup.h"

#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "../COMMON_AI/GRAPH/level_graph.h"
#include "../COMMON_AI/GRAPH/game_graph.h"

// Two zones nearer than this to each other belong to the same field, and a
// field needs at least this many zones to be worth fighting over.
//
// ⚠These are knobs rather than constants because the right values are a
// property of the LEVEL, not of the code. Measured on pripyat_full, whose 31
// anomalies fall out as:
//     join 40m -> 5 fields, but two are sprawls of 9 and 15 zones and one
//                 centre lands off the navmesh
//     join 18m -> 6 fields, none over 8 zones, every centre walkable
// The tighter answer is the one that matches what the map looks like: distinct
// patches a player would name separately. Anyone bringing this to another
// level should run sv_fields and turn these until it does the same there.
float g_sv_field_join = 18.f;
int g_sv_field_min_zones = 2;

namespace
{

// A field's leash never gets smaller than this, however tight the cluster:
// guards on a 3 m leash spend their lives being fetched.
constexpr float MIN_FIELD_RADIUS = 18.f;
constexpr float MAX_FIELD_RADIUS = 45.f;

// Alternating, because a contested map is the point. The names must exist in
// game_relations/communities; SVNpc reports it if they do not rather than
// taking the game down.
const char* const FACTIONS[] = {"dolg", "freedom"};

// What a guard carries. Ammunition matters as much as the weapon -- a guard
// with an empty rifle is scenery.
const char* const GUARD_GEAR[] = {"wpn_ak74", "ammo_5.45x39_ap", "ammo_5.45x39_ap"};

struct Field
{
    Fvector centre{}; // the mean of its anomalies
    Fvector post{}; // ...and the nearest place a creature can stand
    bool walkable{false};
    float radius{0.f};
    u32 zones{0};
};

struct Post
{
    u32 field{0};
    xr_vector<u16> guards;
    shared_str faction;
};

xr_vector<Field> s_fields;
bool s_scanned = false;

xr_vector<Post> s_posts;

// Every anomaly zone standing on the level that is loaded.
//
// ⚠The level filter is not optional and not cosmetic: the A-Life registry
// holds the WHOLE game's spawn, every level at once, so without it a scan on
// Pripyat happily clusters anomalies in Cordon and posts guards at coordinates
// on another map.
void collect_zones(xr_vector<Fvector>& out)
{
    out.clear();

    const CALifeSimulator* alife = ai().get_alife();
    if (!alife || !ai().get_level_graph() || !ai().get_game_graph())
        return;

    const GameGraph::_LEVEL_ID here = ai().level_graph().level_id();

    for (const auto& it : alife->objects().objects())
    {
        CSE_ALifeAnomalousZone* zone = smart_cast<CSE_ALifeAnomalousZone*>(it.second);
        if (!zone)
            continue;

        const GameGraph::_GRAPH_ID gvid = zone->m_tGraphID;
        if (!ai().game_graph().valid_vertex_id(gvid))
            continue;
        if (ai().game_graph().vertex(gvid)->level_id() != here)
            continue;

        out.push_back(zone->position());
    }
}

// Join zones into fields: single-link clustering, which is the right shape
// here -- a field is "anomalies you can walk between", not a tidy sphere, and
// a patch that bends round a building should stay one field.
void build_fields(const xr_vector<Fvector>& zones)
{
    s_fields.clear();

    xr_vector<bool> taken(zones.size(), false);
    xr_vector<u32> pending;

    for (u32 seed = 0; seed < zones.size(); ++seed)
    {
        if (taken[seed])
            continue;

        xr_vector<u32> members;
        pending.clear();
        pending.push_back(seed);
        taken[seed] = true;

        while (!pending.empty())
        {
            const u32 cur = pending.back();
            pending.pop_back();
            members.push_back(cur);

            for (u32 j = 0; j < zones.size(); ++j)
            {
                if (taken[j])
                    continue;
                if (zones[cur].distance_to(zones[j]) <= g_sv_field_join)
                {
                    taken[j] = true;
                    pending.push_back(j);
                }
            }
        }

        if (members.size() < u32(g_sv_field_min_zones))
            continue;

        Field f;
        f.zones = u32(members.size());
        for (u32 m : members)
            f.centre.add(zones[m]);
        f.centre.mul(1.f / float(members.size()));

        for (u32 m : members)
            f.radius = std::max(f.radius, f.centre.distance_to(zones[m]));
        clamp(f.radius, MIN_FIELD_RADIUS, MAX_FIELD_RADIUS);

        // ⚠Where the anomalies average out is not necessarily anywhere a
        // creature can stand -- the mean of a ring of zones can land inside a
        // building, in a pit, or over a hole in the navmesh. A field is only
        // holdable at a spot somebody could walk to, so resolve one now: this
        // is a per-field cost paid once, and the alternative is guards whose
        // posts are all measured from a point in mid-air.
        f.post = f.centre;
        if (ai().get_level_graph())
        {
            const CLevelGraph& G = ai().level_graph();
            u32 v = G.vertex_id(f.centre);
            if (!G.valid_vertex_id(v))
                v = G.nearest_vertex_id(f.centre); // the slow scan, once per field
            if (G.valid_vertex_id(v))
            {
                f.post = G.vertex_position(v);
                f.walkable = true;
            }
        }

        s_fields.push_back(f);
    }

    // Strongest first: the number of zones in a field is the closest thing to
    // "how much is this worth and how dangerous is it" available without
    // reading every zone's section.
    std::sort(s_fields.begin(), s_fields.end(), [](const Field& a, const Field& b) { return a.zones > b.zones; });
}

void scan()
{
    xr_vector<Fvector> zones;
    collect_zones(zones);
    build_fields(zones);
    s_scanned = true;

    Msg("~ [sv_scenario] %u anomaly zones on this level fall into %u fields of %u or more", u32(zones.size()), u32(s_fields.size()), u32(g_sv_field_min_zones));
}
} // namespace

void SVScenario::Fields(u32 max_report)
{
    // Always re-scan: this is the command somebody runs WHILE turning
    // sv_field_join, and a cached answer would silently ignore the knob.
    scan();

    if (s_fields.empty())
    {
        Msg("! [sv_scenario] no anomalous fields found -- either this level has none, or its zones are further than %.0fm apart", g_sv_field_join);
        return;
    }

    if (!max_report)
        max_report = u32(s_fields.size());

    // How far the player is from each field, because on a map this size that
    // is the first thing anybody wants to know and the coordinates alone do
    // not say it.
    bool have_ref = false;
    Fvector ref;
    ref.set(0.f, 0.f, 0.f);
    if (CSE_Abstract* e = SVWorld::Find(SVNpc::ReferenceEntity()))
    {
        ref = e->position();
        have_ref = true;
    }

    for (u32 i = 0; i < s_fields.size() && i < max_report; ++i)
    {
        const Field& f = s_fields[i];

        string64 away = {};
        if (have_ref)
            xr_sprintf(away, ", %.0fm from the player", ref.distance_to(f.post));

        // Report the post rather than the mean: it is where the guards will
        // actually be, and it is the coordinate an operator will want to
        // teleport to.
        string64 offset = {};
        if (f.walkable && f.post.distance_to(f.centre) > 2.f)
            xr_sprintf(offset, " (anomalies average out %.0fm away, off the navmesh)", f.post.distance_to(f.centre));

        Msg("~ [sv_scenario]   field %u: %u zones, holdable at (%.1f, %.1f, %.1f), radius %.0fm%s%s%s", i, f.zones, VPUSH(f.post), f.radius, away, offset,
            f.walkable ? "" : " -- NOWHERE WALKABLE, cannot be held");
    }
}

void SVScenario::Apply(u32 fields, u32 guards, const char* section)
{
    if (!s_scanned)
        scan();

    if (s_fields.empty())
    {
        Msg("! [sv_scenario] nothing to hold -- sv_fields found no anomalous fields");
        return;
    }

    if (!fields)
        fields = 5; // the design's number, and the one the loop was written round
    if (!guards)
        guards = 3;
    if (fields > s_fields.size())
        fields = u32(s_fields.size());

    u32 placed = 0;
    for (u32 i = 0; i < fields; ++i)
    {
        const Field& f = s_fields[i];

        // Already held? Applying twice should reinforce nothing and duplicate
        // nothing -- a scenario command an operator runs by accident must not
        // double the population of the map.
        bool already = false;
        for (const Post& p : s_posts)
            if (p.field == i)
                already = true;
        if (already)
        {
            Msg("~ [sv_scenario] field %u is already held -- skipping", i);
            continue;
        }

        if (!f.walkable)
        {
            Msg("! [sv_scenario] field %u has nowhere walkable near it -- leaving it unheld", i);
            continue;
        }

        const char* faction = FACTIONS[i % (sizeof(FACTIONS) / sizeof(FACTIONS[0]))];

        Post post;
        post.field = i;
        post.faction = faction;
        SVNpc::SpawnAt(section, int(guards), faction, f.post, post.guards);
        if (post.guards.empty())
        {
            Msg("! [sv_scenario] could not place anybody on field %u at (%.1f, %.1f, %.1f)", i, VPUSH(f.centre));
            continue;
        }

        // ⚠Each guard gets its OWN post on a ring around the field, never the
        // field centre. Sending a detail to one coordinate has them shove each
        // other off it for ever -- measured, and the reason sv_npc_guard
        // spreads a detail too.
        //
        // The ring sits at half the field radius: far enough apart to stand
        // clear of one another, close enough that all of them are guarding the
        // same thing.
        const float ring = std::max(4.f, f.radius * 0.5f);
        for (u32 g = 0; g < post.guards.size(); ++g)
        {
            const float ang = (PI_MUL_2 * float(g)) / float(post.guards.size());
            Fvector spot = f.post;
            spot.x += ring * _cos(ang);
            spot.z += ring * _sin(ang);

            for (const char* item : GUARD_GEAR)
                SVNpc::Give(post.guards[g], item);

            // Leash of the field's own radius: a guard is meant to work the
            // field, not stand on a mark. Anything tighter and the creature's
            // ordinary wandering trips the leash every few seconds.
            SVNpc::OrderGuard(post.guards[g], spot, f.radius);
        }

        Msg("~ [sv_scenario] field %u (%u zones, radius %.0fm) held by %u %s at (%.1f, %.1f, %.1f)", i, f.zones, f.radius, u32(post.guards.size()), faction, VPUSH(f.post));

        s_posts.push_back(post);
        ++placed;
    }

    Msg("~ [sv_scenario] %u fields now held; %u in total", placed, u32(s_posts.size()));
}

void SVScenario::Status()
{
    if (s_posts.empty())
    {
        Msg("~ [sv_scenario] nothing placed -- sv_scenario [fields] [guards]");
        return;
    }

    for (const Post& p : s_posts)
    {
        u32 alive = 0;
        for (u16 id : p.guards)
            if (SVWorld::Find(id))
                ++alive;

        const Field& f = s_fields[p.field];

        // Losses are the interesting number: a field whose detail is dead is a
        // field somebody took, which is the whole game.
        Msg("~ [sv_scenario]   field %u [%s] at (%.1f, %.1f, %.1f): %u of %u guards still there", p.field, p.faction.c_str(), VPUSH(f.post), alive,
            u32(p.guards.size()));
    }
}

void SVScenario::Clear()
{
    u32 removed = 0;
    for (const Post& p : s_posts)
        for (u16 id : p.guards)
        {
            SVNpc::Remove(id, false);
            ++removed;
        }

    s_posts.clear();
    Msg("~ [sv_scenario] removed %u guards; the fields are nobody's again", removed);
}

void SVScenario::Reset()
{
    s_posts.clear();
    s_fields.clear();
    s_scanned = false;
}

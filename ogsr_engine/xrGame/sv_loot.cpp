#include "StdAfx.h"
#include "sv_loot.h"

#include "sv_world_lookup.h"
#include "sv_npc.h"

#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "character_community.h"
#include "../COMMON_AI/GRAPH/level_graph.h"
#include "../COMMON_AI/GRAPH/game_level_cross_table.h"

#include <algorithm>
#include <mutex>

// sv_loot_log: print a line the moment an item changes hands. Off by default --
// on a populated level the mod's own A-Life loots corpses constantly and the
// interesting lines would drown.
int g_sv_loot_log = 0;

// sv_loot_max_dist: refuse a take from further away than this, in metres.
// 0 = measure only, which is the default and the honest starting point.
//
// ⚠Refusing here means the server does not broadcast the transfer, which is
// exactly what it already does for a packet from an untrusted client. The
// asking client may still believe it has the item until it is corrected -- a
// proper enforcement path sends GE_OWNERSHIP_REJECT back, and that is the next
// step, not this one. Turn this on to see what WOULD be refused before making
// it authoritative.
float g_sv_loot_max_dist = 0.f;

namespace
{
std::recursive_mutex s_lock;

struct Dropped
{
    u16 id{0};
    shared_str section;
    Fvector at{};
    u32 when{0};
    bool taken{false};
    u16 taken_by{0xffff};
};
xr_vector<Dropped> s_dropped;

struct Take
{
    u32 when{0};
    u16 item{0};
    shared_str item_sect;
    u16 taker{0};
    shared_str taker_sect;
    const char* faction{"-"};
    float dist{-1.f};
    // ⚠Both positions kept, not just the distance between them. A single
    // "0.00 m" cannot be told apart from "the server could not tell where
    // either of them was", and that difference is the whole security question.
    Fvector item_at{};
    Fvector taker_at{};
    float from_where_it_lay{-1.f}; // only for items we dropped, and never synced
    bool server_client{false};
    bool ours{false}; // one of the items sv_item_drop put there
    u16 kind{0}; // the event: off the ground, out of something, or bought
};

// ⭐The distinction the gate depends on. Three different flows arrive at the
// same handler, and only one of them has any business being close-range.
const char* flow_name(u16 event_type)
{
    switch (event_type)
    {
    case GE_OWNERSHIP_TAKE: return "off the ground";
    case GE_TRANSFER_TAKE: return "out of an inventory";
    case GE_TRADE_BUY: return "bought";
    }
    return "?";
}
xr_vector<Take> s_takes;

u32 s_refused[4] = {};
constexpr u32 MAX_TAKES = 512;

const char* faction_of(CSE_Abstract* e)
{
    CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(e);
    return (trader && trader->m_community_index != NO_COMMUNITY_INDEX) ? trader->CommunityName() : "-";
}

const char* refusal_name(SVLoot::Refusal r)
{
    switch (r)
    {
    case SVLoot::Refusal::NoSuchItem: return "the id names no item";
    case SVLoot::Refusal::AlreadyOwned: return "the item already has an owner";
    case SVLoot::Refusal::NotYourClient: return "the sender does not own the taker";
    case SVLoot::Refusal::TooFar: return "further than sv_loot_max_dist";
    }
    return "?";
}

// Put a point on the floor the same way the puppet orders do: the AI graph is
// what the level considers walkable, so an item snapped to it is an item a
// creature can actually reach. Dropped into geometry it is a thing nobody will
// ever pick up, which would look exactly like an AI failure.
bool ground_at(const Fvector& in, Fvector& out, u32& lvid, GameGraph::_GRAPH_ID& gvid)
{
    out = in;
    if (!ai().get_level_graph() || !ai().get_cross_table())
        return false;

    const CLevelGraph& G = ai().level_graph();
    lvid = G.vertex_id(in);
    if (!G.valid_vertex_id(lvid))
        lvid = G.nearest_vertex_id(in);
    if (!G.valid_vertex_id(lvid))
        return false;

    out.y = G.vertex_plane_y(lvid, in.x, in.z);
    gvid = ai().cross_table().vertex(lvid).game_vertex_id();
    return true;
}
} // namespace

void SVLoot::Drop(const char* section, int count, const Fvector& at, float spread)
{
    if (!section || !section[0] || !pSettings->section_exist(section))
    {
        Msg("! [loot] no such section '%s'", section ? section : "");
        return;
    }

    CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());
    if (!alife)
    {
        Msg("! [loot] no A-Life simulator -- is a level loaded?");
        return;
    }

    if (count < 1)
        count = 1;
    if (spread < 0.f)
        spread = 0.f;

    u32 made = 0;
    for (int i = 0; i < count; ++i)
    {
        Fvector want = at;
        if (spread > 0.f && count > 1)
        {
            const float ang = (PI_MUL_2 * float(i)) / float(count);
            want.x += spread * _cos(ang);
            want.z += spread * _sin(ang);
        }

        Fvector pos;
        u32 lvid = 0;
        GameGraph::_GRAPH_ID gvid = GameGraph::_GRAPH_ID(-1);
        if (!ground_at(want, pos, lvid, gvid))
        {
            Msg("! [loot] (%.1f, %.1f, %.1f) has no walkable vertex near it -- an item there could never be reached", VPUSH(want));
            continue;
        }

        // ⭐Parent 0xffff is the whole point: every other spawn in this server
        // hands the item straight to somebody. This one leaves it lying there.
        CSE_Abstract* item = alife->spawn_item(section, pos, lvid, gvid, ALife::_OBJECT_ID(-1));
        if (!item)
        {
            Msg("! [loot] spawn of '%s' failed", section);
            continue;
        }

        {
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            s_dropped.push_back(Dropped());
            s_dropped.back().id = item->ID;
            s_dropped.back().section = item->s_name;
            s_dropped.back().at = pos;
            s_dropped.back().when = Device.dwTimeGlobal;
        }
        ++made;

        Msg("~ [loot] dropped '%s' id=%u at (%.1f, %.1f, %.1f)", section, item->ID, VPUSH(pos));
    }

    Msg("~ [loot] %u of %d on the ground; %u tracked", made, count, u32(s_dropped.size()));
}

void SVLoot::GiveTo(u16 owner_id, const char* section, int count)
{
    if (!section || !section[0] || !pSettings->section_exist(section))
    {
        Msg("! [loot] no such section '%s'", section ? section : "");
        return;
    }

    CSE_Abstract* owner = SVWorld::Find(owner_id);
    if (!owner)
    {
        Msg("! [loot] no entity %u to give '%s' to", owner_id, section);
        return;
    }

    CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());
    if (!alife)
        return;

    if (count < 1)
        count = 1;

    Fvector pos;
    u32 lvid = 0;
    GameGraph::_GRAPH_ID gvid = GameGraph::_GRAPH_ID(-1);
    if (!ground_at(owner->position(), pos, lvid, gvid))
    {
        Msg("! [loot] %u stands nowhere the AI graph knows -- cannot spawn an item on it", owner_id);
        return;
    }

    // ⚠⚠⚠The recipient must be an ALife OBJECT, not merely an entity the server
    // knows about. These are two different registries and the difference is not
    // cosmetic: an item is linked into its owner's inventory by
    // CALifeSimulatorBase::register_object, which looks the parent up in ALife's
    // registry alone. A parent that is not there used to take the server down
    // with an access violation and no message (28-07-2026, first live session).
    //
    // ⭐It is refused HERE as well as guarded there, because the guard can only
    // say "that did not work" while this can say WHY, and the why is worth
    // knowing: the second player to connect -- and any player reconnecting while
    // the first actor is still held -- is given a cloned body by
    // xrServer::Perform_spawn_net_actor, and that clone is deliberately kept out
    // of ALife. So it cannot own anything, and "sv_item_give actor" silently
    // meant a DIFFERENT body from the one the operator was looking at.
    // (asked through the const simulator: the object registry is only public
    // that way, and this is a read)
    if (!ai().alife().objects().object(owner_id, true))
    {
        Msg("! [loot] %u '%s' is not an ALife object -- it cannot own anything, so '%s' was NOT given", owner_id, owner->s_name.c_str(), section);
        Msg("! [loot]   this is a body spawned for a network client (Perform_spawn_net_actor); the ALife actor is a different entity -- sv_npc_gear will show you both");
        return;
    }

    // ⚠⚠Is the recipient ALREADY a live object? Asked of the object layer, not
    // of the A-Life flag, because the case this whole branch exists for is an
    // owner A-Life does not control -- a player's actor, created by the server.
    const bool owner_is_live = g_pGameLevel && Level().Objects.net_Find(owner_id);

    u32 made = 0, forced = 0;
    for (int i = 0; i < count; ++i)
    {
        // ⚠The owner is re-resolved by id every pass rather than held across
        // the loop: spawn_item registers an object, and registration runs
        // on_register / spawn_supplies, which are entitled to create and destroy
        // entities. A pointer that was valid when the command was typed is not
        // guaranteed to be valid one item later.
        owner = SVWorld::Find(owner_id);
        if (!owner || !ai().alife().objects().object(owner_id, true))
        {
            Msg("! [loot] %u stopped existing after %u item%s -- giving up on the rest", owner_id, made, made == 1 ? "" : "s");
            break;
        }

        // Parented at spawn: the item never exists loose in the world, so there
        // is no window in which the pickup could fail or somebody else could
        // reach it first. That is the whole point.
        CSE_Abstract* item = alife->spawn_item(section, pos, lvid, gvid, owner->ID);
        if (!item)
        {
            Msg("! [loot] spawn of '%s' failed", section);
            continue;
        }
        ++made;

        // ⭐⭐The half that was missing, measured against a live client
        // 28-07-2026. Registering the item with A-Life is not the same as it
        // EXISTING for anybody: an item in an inventory is carried online by
        // its OWNER's switch, and an owner who is already online never switches
        // again. Give something to a creature A-Life spawned and it works,
        // because that creature dragged its children online with it; give the
        // same thing to a player's actor and the item stays an offline record
        // -- the server prints "it now carries N things" and the player's
        // rucksack is empty, with nothing anywhere saying why.
        //
        // add_online is the engine's own "make it exist for clients": it runs
        // server().Process_spawn, and the client's spawn handler turns a spawn
        // with a parent into an ownership take by itself.
        CSE_ALifeDynamicObject* dyn = smart_cast<CSE_ALifeDynamicObject*>(item);
        if (owner_is_live && dyn && !dyn->m_bOnline)
        {
            alife->add_online(dyn);
            ++forced;
        }
    }

    Msg("~ [loot] gave %u '%s' x%u '%s'; it now carries %u things%s", owner_id, owner->s_name.c_str(), made, section, u32(owner->children.size()),
        forced ? " (switched online for the client)" : (owner_is_live ? "" : " -- owner is OFFLINE, the item will appear when it comes online"));
}

void SVLoot::List()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);

    if (s_dropped.empty())
    {
        Msg("~ [loot] nothing has been dropped");
        return;
    }

    u32 taken = 0, gone = 0;
    for (Dropped& d : s_dropped)
    {
        CSE_Abstract* e = SVWorld::Find(d.id);
        if (!e)
        {
            ++gone;
            Msg("~ [loot]   %u '%s' NO LONGER EXISTS (destroyed, or its id was recycled)", d.id, d.section.c_str());
            continue;
        }

        // ⭐The authoritative answer to "was it picked up", and it does not
        // depend on our having seen the event: an item with a parent is an item
        // somebody owns.
        if (0xffff != e->ID_Parent)
        {
            ++taken;
            CSE_Abstract* owner = SVWorld::Find(e->ID_Parent);
            Msg("~ [loot]   %u '%s' held by %u '%s' [%s]", d.id, d.section.c_str(), e->ID_Parent, owner ? owner->s_name.c_str() : "?", owner ? faction_of(owner) : "-");
        }
        else
            Msg("~ [loot]   %u '%s' still on the ground at (%.1f, %.1f, %.1f)", d.id, d.section.c_str(), VPUSH(e->o_Position));
    }

    Msg("~ [loot] %u dropped: %u picked up, %u still lying there, %u gone", u32(s_dropped.size()), taken, u32(s_dropped.size()) - taken - gone, gone);
}

bool SVLoot::AllowTake(CSE_Abstract* item, CSE_Abstract* new_owner, u16 event_type)
{
    if (g_sv_loot_max_dist <= 0.f || !item || !new_owner)
        return true;

    // Never police an unmeasured flow -- see the note on the declaration.
    if (event_type == GE_TRADE_BUY)
        return true;

    const float d = item->position().distance_to(new_owner->position());
    if (d <= g_sv_loot_max_dist)
        return true;

    Msg("! [loot] REFUSED: %u '%s' tried to take '%s' from %.1f m (limit %.1f)", new_owner->ID, new_owner->s_name.c_str(), item->s_name.c_str(), d, g_sv_loot_max_dist);
    OnRefused(item->ID, new_owner->ID, Refusal::TooFar);
    return false;
}

void SVLoot::OnTaken(CSE_Abstract* item, CSE_Abstract* new_owner, bool from_server_client, u16 event_type)
{
    if (!item || !new_owner)
        return;

    Take t;
    t.kind = event_type;
    t.when = Device.dwTimeGlobal;
    t.item = item->ID;
    t.item_sect = item->s_name;
    t.taker = new_owner->ID;
    t.taker_sect = new_owner->s_name;
    t.faction = faction_of(new_owner);
    t.server_client = from_server_client;

    // ⚠Measured BEFORE the parentage is rebuilt, because an item that has an
    // owner reports the owner's position: half a frame later this number is
    // always 0.0 and the whole point of recording it is gone.
    t.item_at = item->position();
    t.taker_at = new_owner->position();
    t.dist = t.item_at.distance_to(t.taker_at);

    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        for (Dropped& d : s_dropped)
            if (d.id == t.item)
            {
                d.taken = true;
                d.taken_by = t.taker;
                t.ours = true;
                // ⭐The one distance nothing can have quietly rewritten: where
                // we PUT it, against where the taker says it is. The entity's
                // own position is whatever the last update wrote, and if that
                // update came from the taker it is worth exactly nothing as
                // evidence.
                t.from_where_it_lay = d.at.distance_to(t.taker_at);
                break;
            }

        if (s_takes.size() >= MAX_TAKES)
            s_takes.erase(s_takes.begin());
        s_takes.push_back(t);
    }

    if (g_sv_loot_log || t.ours)
        Msg("~ [loot] %u '%s' [%s] took '%s' (item %u) %s at %.1f m%s%s", t.taker, t.taker_sect.c_str(), t.faction, t.item_sect.c_str(), t.item, flow_name(t.kind), t.dist,
            t.ours ? " -- one we dropped" : "", t.server_client ? "" : " -- ON A CLIENT'S WORD");
}

void SVLoot::OnRefused(u16 item_id, u16 taker_id, Refusal why)
{
    const int idx = int(why);
    if (idx >= 0 && idx < 4)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        ++s_refused[idx];
    }

    if (g_sv_loot_log)
        Msg("~ [loot] refused: item %u to %u -- %s", item_id, taker_id, refusal_name(why));
}

void SVLoot::Report()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);

    Msg("~ [loot] === ownership ledger ===");
    List();

    if (s_takes.empty())
    {
        Msg("~ [loot] no item changed hands");
    }
    else
    {
        // ⭐Raw samples, sorted, then the shape -- not an average. A mean
        // distance over honest looting plus one teleported take is a number
        // that hides the only sample that mattered.
        xr_vector<float> d;
        u32 by_client = 0;
        for (const Take& t : s_takes)
        {
            d.push_back(t.dist);
            if (!t.server_client)
                ++by_client;
        }
        std::sort(d.begin(), d.end());

        Msg("~ [loot] %u takes recorded (%u of them on a client's word, %u by the server itself)", u32(s_takes.size()), by_client, u32(s_takes.size()) - by_client);
        Msg("~ [loot] distance at the moment of the take: min %.2f, median %.2f, p90 %.2f, max %.2f m", d.front(), d[d.size() / 2], d[(d.size() * 9) / 10], d.back());

        // ⭐⭐Per FLOW, because that is what a threshold has to survive. Picking
        // something off the ground happens at arm's length; taking it out of a
        // corpse or buying it need not, and a gate set from the first number
        // would refuse the other two. A single pooled median hides exactly that.
        for (u16 kind : {u16(GE_OWNERSHIP_TAKE), u16(GE_TRANSFER_TAKE), u16(GE_TRADE_BUY)})
        {
            xr_vector<float> k;
            for (const Take& t : s_takes)
                if (t.kind == kind)
                    k.push_back(t.dist);
            if (k.empty())
            {
                Msg("! [loot]   %-20s NOT MEASURED -- no sample, so no threshold may be set for it", flow_name(kind));
                continue;
            }
            std::sort(k.begin(), k.end());
            Msg("~ [loot]   %-20s %u takes: median %.2f, p90 %.2f, max %.2f m", flow_name(kind), u32(k.size()), k[k.size() / 2], k[(k.size() * 9) / 10], k.back());
        }

        // The last few, in full: a report that only aggregates cannot be
        // checked against the log by hand.
        const u32 show = std::min<u32>(12, u32(s_takes.size()));
        for (u32 i = u32(s_takes.size()) - show; i < s_takes.size(); ++i)
        {
            const Take& t = s_takes[i];
            if (t.from_where_it_lay >= 0.f)
                Msg("~ [loot]   %6.1fs %u '%s' [%s] <- '%s' (%u) | entity says %.2f m, from where it LAY %.2f m | item (%.1f, %.1f) taker (%.1f, %.1f)", float(t.when) / 1000.f,
                    t.taker, t.taker_sect.c_str(), t.faction, t.item_sect.c_str(), t.item, t.dist, t.from_where_it_lay, t.item_at.x, t.item_at.z, t.taker_at.x, t.taker_at.z);
            else
                Msg("~ [loot]   %6.1fs %u '%s' [%s] <- '%s' (%u) | entity says %.2f m | item (%.1f, %.1f) taker (%.1f, %.1f)", float(t.when) / 1000.f, t.taker,
                    t.taker_sect.c_str(), t.faction, t.item_sect.c_str(), t.item, t.dist, t.item_at.x, t.item_at.z, t.taker_at.x, t.taker_at.z);
        }

        // ⭐⭐Whether the number the gate would use is EVIDENCE at all. If an
        // item's entity position has already been synced to whoever is taking
        // it by the time the server sees the request, then the server cannot
        // tell a walk-up from a claim made across the map -- and a distance
        // gate built on it would be decoration.
        u32 zeroed = 0, with_drop = 0, disagreed = 0;
        for (const Take& t : s_takes)
        {
            if (t.dist < 0.05f)
                ++zeroed;
            if (t.from_where_it_lay >= 0.f)
            {
                ++with_drop;
                if (_abs(t.from_where_it_lay - t.dist) > 0.5f)
                    ++disagreed;
            }
        }
        if (with_drop && disagreed)
            Msg("! [loot] %u of %u takes disagree with where the item was actually left (%u of all %u read as 0.00 m): the entity's position had already followed its new "
                "owner, so THAT number is not evidence of anything",
                disagreed, with_drop, zeroed, u32(s_takes.size()));
        else if (with_drop)
            Msg("~ [loot] the entity's position matched where the item was left in all %u checkable takes -- the distance is worth gating on", with_drop);
    }

    u32 total_refused = 0;
    for (u32 i = 0; i < 4; ++i)
        total_refused += s_refused[i];
    Msg("~ [loot] %u refusals: %u unknown item, %u already owned, %u wrong client, %u too far", total_refused, s_refused[0], s_refused[1], s_refused[2], s_refused[3]);

    // ⭐⭐The security statement, said out loud rather than left to be inferred
    // from the numbers above it.
    if (g_sv_loot_max_dist > 0.f)
        Msg("~ [loot] distance gate: ON at %.1f m for reaching and looting; purchases are exempt (never measured)", g_sv_loot_max_dist);
    else
        Msg("! [loot] distance gate: OFF -- the server accepts a take at ANY range. It verifies only that the item is unowned and that the packet came from the taker's own "
            "client. Set sv_loot_max_dist once the numbers above have a shape.");
}

void SVLoot::Reset()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    s_takes.clear();
    for (u32 i = 0; i < 4; ++i)
        s_refused[i] = 0;
    Msg("~ [loot] ledger cleared (%u dropped items still tracked)", u32(s_dropped.size()));
}

void SVLoot::OnWorldGone()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    s_dropped.clear();
    s_takes.clear();
    for (u32 i = 0; i < 4; ++i)
        s_refused[i] = 0;
}

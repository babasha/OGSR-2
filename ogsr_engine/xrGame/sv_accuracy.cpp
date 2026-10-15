#include "StdAfx.h"
#include "sv_accuracy.h"
#include "sv_world_lookup.h"
#include "../COMMON_AI/xrServer_Objects_ALife_Monsters.h" // CSE_ALifeCreatureActor

namespace
{
// Bounded: this is a diagnostic, and a level with a hundred shooters must not
// grow a row per corpse for two hours.
constexpr u32 k_max = 64;

struct SRow
{
    u16 id{0xffff};
    u32 shots{0};
    u32 hits{0}; // hit events attributed to this shooter
    u32 scoring{0}; // ...of which carried power (a through-and-through's second
                    // registration carries none)
    u32 dropped{0}; // never reached a client: the attacker was offline
    float power_sum{0.f};
    float power_max{0.f};
    u16 last_victim{0xffff};
    u32 on_player{0}; // hits landed on an entity the server calls a player
};

SRow s_row[k_max];
u32 s_n = 0;

SRow* row(u16 id, bool create)
{
    for (u32 i = 0; i < s_n; ++i)
        if (s_row[i].id == id)
            return &s_row[i];
    if (!create || s_n >= k_max)
        return nullptr;
    SRow* r = &s_row[s_n++];
    r->id = id;
    return r;
}

LPCSTR who(u16 id)
{
    CSE_Abstract* e = SVWorld::Find(id);
    return e ? e->name_replace() : "?";
}

// A hit on the man at the keyboard is the interesting column, and "is this a
// player" is a question about the entity, not about its id -- the actor is not
// always 0 once somebody reconnects (see extraction-two-world-registries).
bool is_player(u16 id)
{
    CSE_Abstract* e = SVWorld::Find(id);
    return e && (nullptr != smart_cast<CSE_ALifeCreatureActor*>(e));
}
} // namespace

void SVAccuracy::Shot(u16 shooter, u16 /*weapon*/)
{
    if (SRow* r = row(shooter, true))
        ++r->shots;
}

void SVAccuracy::Hit(u16 attacker, u16 victim, u16 /*bone*/, float power, bool dropped)
{
    SRow* r = row(attacker, true);
    if (!r)
        return;

    ++r->hits;
    r->last_victim = victim;
    if (dropped)
        ++r->dropped;
    if (power > EPS)
    {
        ++r->scoring;
        r->power_sum += power;
        r->power_max = _max(r->power_max, power);
    }
    if (is_player(victim))
        ++r->on_player;
}

void SVAccuracy::Dump()
{
    if (!s_n)
    {
        Msg("~ [accuracy] nothing recorded yet -- no shots have been published and no hits have arrived");
        return;
    }

    Msg("~ [accuracy] ================ shots and hits ================");
    Msg("~ [accuracy] %-6s %-22s %6s %6s %7s %7s %8s %8s %s", "id", "who", "shots", "hits", "scoring", "on you", "avg pow", "max pow", "dropped");
    for (u32 i = 0; i < s_n; ++i)
    {
        const SRow& r = s_row[i];
        if (!r.shots && !r.hits)
            continue;
        Msg("~ [accuracy] %-6u %-22s %6u %6u %7u %7u %8.4f %8.4f %u", r.id, who(r.id), r.shots, r.hits, r.scoring, r.on_player, r.scoring ? (r.power_sum / float(r.scoring)) : 0.f,
            r.power_max, r.dropped);
    }

    // ⚠Stated rather than divided. A shotgun sends a hit per pellet and a
    // through-and-through registers twice, so hits/shots is not a percentage
    // and printing one would invite exactly the wrong conclusion.
    Msg("~ [accuracy] hits/shots is NOT an accuracy percentage: pellets and through-and-throughs both register more than once");
    Msg("~ [accuracy] a shooter with shots and no hits is missing; one with hits and no shots is firing through a path that does not publish");
}

void SVAccuracy::Reset()
{
    s_n = 0;
    for (u32 i = 0; i < k_max; ++i)
        s_row[i] = SRow();
}

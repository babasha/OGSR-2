#include "StdAfx.h"
#include "ai_commander.h"

#include "Level.h"
#include "Actor.h"
#include "ai_space.h"
#include "level_graph.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_space.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "visual_memory_manager.h"
#include "memory_space.h"
#include "stalker_movement_manager.h"
#include "sound_player.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "Weapon.h"
#include "ai_object_location.h"
#include "ai_house.h"
#include "ai_position_query.h"
#include "ai_threat_map.h"
#include "ai_debug_draw.h"
#include "stalker_combat_actions.h"

extern int g_dbg_npc_tactics;
extern float g_ai_suppress_time;

namespace
{
using namespace Commander;
using PositionQuery::EKind;

constexpr float FIGHT_RANGE = 60.f; // the commander runs the fight this near the player; farther, the stock way
constexpr float FIGHT_KEEP = 80.f; // ...and keeps a man in it up to this far
constexpr u32 THINK_EVERY = 250; // and thinks this often, events aside
constexpr u32 ACTIVE = 1000; // reported this recently: in the fight now (a script may take him out a while)
constexpr u32 KEEP_SHORT = 2000; // away this long, a man loses a role that moves (suppress, flank, search, assault, sweep)
constexpr u32 KEEP = 15000; // ...and any role, and his place in the fight: he is gone
constexpr float AT_PLACE = 1.2f; // this near his place, he is at it

constexpr u32 SUPPRESS_START = 700; // lost from sight this long: he went to ground, fire into it
constexpr u32 SEARCH_DELAY = 8000; // lost this long: go and look for him
constexpr float SUPPRESS_RANGE = 70.f;
constexpr float FLANK_RANGE = 50.f; // only men this near the spot go round it
constexpr u32 FLANKERS = 2;

constexpr float WATCH_REPICK = 8.f; // the spot moved this far: a watcher's place is asked anew
constexpr u32 NOTHING_RETRY = 3000; // no place found: asked again after this long

constexpr u32 SLOTS = 3; // men on a door: one in front, one either side; a window takes two in front
constexpr float POST_NEAR = 3.5f, POST_FAR = 14.f, POST_MARK = 7.f; // a post in front of an opening
constexpr float POST_FAR_WIDE = 20.f; // ...asked again wider, when there is nowhere in front
constexpr float POST_SPACING = 3.f; // no two posts nearer than this
constexpr float DOOR_SPACING = 1.5f; // the men beside a door: not on top of one another
constexpr u32 BLIND_RETRY = 30000; // an opening nothing outside sees into is tried again after this long

constexpr u32 RING_HOLD = 10000; // every opening kept this long: one goes in
constexpr u32 SIEGE_MIN = 20000; // ...but not before the siege is this old
constexpr u32 SIEGE_HOLD = 60000; // ...and after this long at the latest, ringed or not
constexpr u32 STACK_TIME = 2000; // crouched by the door before going in
constexpr u32 ASSAULT_RETRY = 10000; // the man going in fell: the next one after this long

constexpr float SWEEP_TAKEN = 6.f; // a hiding place this close to one already checked is the same place
constexpr float SWEEP_MOVED = 10.f; // heard of this far from where the sweep began: a new search
constexpr float SWEEP_RADIUS = 25.f;
constexpr float SWEEP_NEAR = 6.f; // with the threat map: a sweeper looks round one likely place, this far

// ⭐With the threat map (ai_threat_map.h): "where he is" is its likeliest place,
// which a sound only nudges, and a house is his when most of the map is in it.
constexpr float HOUSE_MASS = .5f; // this likely in a house: he is besieged in it
constexpr float LEAVE_MASS = .15f; // ...and out of it, once it holds less than this of the map
constexpr u32 LEAVE_UNSEEN = 5000; // ...and nobody has seen him for this long
constexpr u32 SEARCH_LOOKS = 4; // the searcher goes on to the next likeliest place this many times, then the sweep
constexpr u32 INSIDE_LOOKS = 3; // the man gone in looks round this many likely places of the house
constexpr float INSIDE_MASS = .3f; // ...while the house is still this likely

enum EMode : u8
{
    eModeEngage, // in sight, or just out of it
    eModeLost, // gone to ground: suppress and go round
    eModeSearch, // gone a while: one looks, the others watch
    eModeSweep, // not where he was: everybody combs
    eModeSiege, // in a house
    eModeDone, // combed for SWEEP_TIME: let go
};
constexpr LPCSTR MODE_NAME[] = {"engage", "lost", "search", "sweep", "siege", "done"};

struct SOpening : AIHouse::SOpening
{
    bool blind{};
    u32 blind_time{};
    bool full[SLOTS]{}; // no room outside for that man
};
using SHouse = AIHouse::THouse<SOpening>;

bool struck_off(const SOpening& o) { return o.blind && Device.dwTimeGlobal < o.blind_time + BLIND_RETRY; }

struct SMember
{
    u16 id{u16(-1)};
    u32 reported{};
    Fvector position{};
    bool sees{};
    u32 own_seen{}; // when he himself last saw the enemy
    bool automatic{}; // an automatic with rounds in it
    STask task;
    u32 serial{};
    u32 since{}; // when he got this role
    int opening{-1}; // a post or a door: which opening...
    u32 slot{}; // ...and which man on it
    EKind asked{PositionQuery::eKindCount}; // the answer he waits for
    Fvector asked_for{}; // the spot it was asked round
    u32 asked_time{};
    u32 stage{}; // assault: 0 by the door, 1 going in; sweep: places looked at
    u32 retries{}; // answers thrown back as another man's place meanwhile
    int watching{-1}; // a watcher round a house: the opening his gun is on
};

struct SFight
{
    u16 enemy{u16(-1)};
    xr_vector<SMember> members;
    u32 next_think{};
    EMode mode{eModeEngage};
    u32 mode_time{};

    // where he is: the freshest anybody remembers
    Fvector spot{};
    u32 spot_vertex{u32(-1)};
    u32 spot_time{};
    u32 seen_time{}; // anybody's eyes on him, last
    // ...and where they last had him: the house is read from this. 📏06-10 19:44:
    // a sound put the remembered spot 4 m out past a window, the house was
    // "left" and the siege dropped with six men watching him through it.
    Fvector seen_spot{};
    u32 seen_spot_time{};

    SHouse house;
    u32 house_start{}; // which house the posts were given for (its start)
    u32 ring_time{}; // every opening kept since; 0 not
    u32 assault_time{};
    u32 watch_version{}; // the house's version the watchers were placed for

    Fvector search_from{}; // where the searcher stood when he set out: he hides from there
    u32 sweep_start{};
    Fvector sweep_from{};
    xr_vector<Fvector> checked; // the hiding places somebody went to look at
    xr_map<u32, u32> cried; // sound -> when it was last called out
};

xr_map<u16, SFight> s_fights;

CAI_Stalker* stalker(u16 id) { return smart_cast<CAI_Stalker*>(Level().Objects.net_Find(id)); }
float flat(const Fvector& a, const Fvector& b) { return _sqrt(_sqr(a.x - b.x) + _sqr(a.z - b.z)); }
// Another place to look at: aside, or a floor up or down -- 📏06-10 23:13, the
// man gone in stood over the cellar the threat map had him in and "found nobody".
bool elsewhere(const Fvector& a, const Fvector& b) { return flat(a, b) > AIHouse::RESPOT || _abs(a.y - b.y) > 2.f; }
bool active(const SMember& m) { return Device.dwTimeGlobal < m.reported + ACTIVE; }
bool moving_role(ERole r) { return r == eRoleSuppress || r == eRoleFlank || r == eRoleSearch || r == eRoleAssault || r == eRoleSweep; }

SMember* member(SFight& f, u16 id)
{
    for (SMember& m : f.members)
        if (m.id == id)
            return &m;
    return nullptr;
}

SFight* fight_of(u16 enemy)
{
    if (!PositionQuery::g_ai_tactics_v2)
        return nullptr;
    const auto I = s_fights.find(enemy);
    return I != s_fights.end() ? &I->second : nullptr;
}

void log_task(const SFight& f, const SMember& m, LPCSTR what)
{
    if (!g_dbg_npc_tactics)
        return;
    CAI_Stalker* S = stalker(m.id);
    const Fvector at = m.task.place ? m.task.position : m.position;
    Msg("[cmd] %u '%s' %s: %s%s (%.1f,%.1f,%.1f) | mode %s", m.id, S ? S->Name() : "?", what, RoleName(m.task.role), m.task.place ? " at" : m.task.pending ? " asking" : " here",
        VPUSH(at), MODE_NAME[f.mode]);
}

void touch(SMember& m) { m.task.serial = ++m.serial; }

void set_task(SMember& m, const STask& task)
{
    m.task = task;
    touch(m);
}

void release(SFight& f, SMember& m)
{
    if (m.task.role == eRoleNone)
        return;
    if (m.asked != PositionQuery::eKindCount)
        PositionQuery::Cancel(m.id, m.asked);
    m.asked = PositionQuery::eKindCount;
    set_task(m, {});
    m.opening = -1;
    m.watching = -1;
    m.stage = 0;
}

// Where everybody else in the fight is going to be: his place, else where he
// stands. `posts`: the men on the house only.
void others(const SFight& f, u16 self, xr_vector<Fvector>& out, bool posts = false)
{
    for (const SMember& m : f.members)
    {
        if (m.id == self)
            continue;
        const bool on_house = m.task.role == eRolePost || m.task.role == eRoleDoor;
        if (posts && !on_house)
            continue;
        if (m.task.place)
            out.push_back(m.task.position);
        else if (!posts)
            out.push_back(m.position);
    }
}

void house_of(const SFight& f, PositionQuery::SRequest& r)
{
    if (!f.house.indoors)
        return;
    r.house = true;
    r.house_origin = f.house.origin;
    r.house_radius = f.house.radius;
    r.house_baked = f.house.baked;
}

void ask(SFight& f, SMember& m, PositionQuery::SRequest&& r, const Fvector& round)
{
    m.asked = r.kind;
    m.asked_for = round;
    m.asked_time = Device.dwTimeGlobal;
    m.task.pending = true;
    PositionQuery::Issue(std::move(r));
}

Fvector floor_of(const SOpening& o) { return Fvector(o.mouth).sub(Fvector{0.f, o.height, 0.f}); }

// Asks where his role puts him, or sets it when there is nothing to ask.
void place(SFight& f, SMember& m, CAI_Stalker& S)
{
    using namespace PositionQuery;
    STask& t = m.task;
    t.place = false;
    t.pending = false;
    SRequest r;
    r.npc = m.id;
    r.self = S.Position();
    r.threat = f.spot;
    r.focus = f.spot;
    switch (t.role)
    {
    case eRoleSuppress: break;
    case eRoleFlank: {
        // off the squad's side of the spot, seeing it
        Fvector squad{};
        u32 n = 0;
        for (const SMember& o : f.members)
            if (o.id != m.id)
            {
                squad.add(o.position);
                ++n;
            }
        if (n)
            squad.div(float(n));
        else
            squad = S.Position();
        r.kind = eFlank;
        r.focus_dir = Fvector().sub(squad, f.spot);
        r.focus_dir.y = 0.f;
        r.focus_dir.normalize_safe();
        r.near_ = 8.f;
        r.ideal = 15.f;
        r.far_ = 28.f;
        others(f, m.id, r.friends);
        r.spacing = 3.f;
        GatherCovers(r, S, f.spot, r.far_, 40, r.ideal);
        GatherRings(r, S, f.spot, {12.f, 18.f}, 8);
        ask(f, m, std::move(r), f.spot);
        break;
    }
    case eRoleWatch: {
        // ⭐Round a house: on one of its openings, from farther out than the
        // posts -- the nearest with the fewest eyes on it, a blind one too.
        // 📏06-10 20:05: watching the house's middle, the ring stared at its
        // log walls (21-93 man-seconds "a wall in the face" a fight), and
        // the window nobody could post at went unwatched.
        const bool siege = f.mode == eModeSiege;
        m.watching = -1;
        if (siege)
        {
            float best = flt_max;
            for (u32 k = 0; k < f.house.openings.size(); ++k)
            {
                u32 eyes = 0;
                for (const SMember& o : f.members)
                    eyes += o.id != m.id && (o.watching == int(k) || (o.opening == int(k) && (o.task.role == eRolePost || o.task.role == eRoleDoor))) ? 1 : 0;
                const float cost = flat(S.Position(), f.house.openings[k].mouth) + 12.f * float(eyes);
                if (cost < best)
                {
                    best = cost;
                    m.watching = int(k);
                }
            }
        }
        // the floor in front of it: the view is tested a metre up, about its sill
        const Fvector round = m.watching >= 0 ? floor_of(f.house.openings[m.watching]) : siege ? f.house.spot : f.spot;
        r.kind = eWatch;
        r.focus = round;
        r.near_ = siege ? 4.f : 6.f;
        r.ideal = siege ? 10.f : 14.f;
        r.far_ = siege ? 16.f : 22.f;
        house_of(f, r);
        others(f, m.id, r.friends);
        r.spacing = 3.f;
        GatherCovers(r, S, round, r.far_, 40, r.ideal);
        GatherRings(r, S, round, {r.ideal - 4.f, r.ideal, r.ideal + 4.f}, 8);
        t.focus = m.watching >= 0 ? f.house.openings[m.watching].inside : round;
        ask(f, m, std::move(r), round);
        break;
    }
    case eRolePost: {
        const SOpening& o = f.house.openings[m.opening];
        const Fvector floor = floor_of(o);
        const Fvector side{o.dir.z, 0.f, -o.dir.x};
        r.kind = ePost;
        r.focus = o.inside;
        r.focus_dir = o.dir;
        r.focus_floor = floor;
        r.near_ = POST_NEAR;
        r.ideal = POST_MARK;
        r.far_ = POST_FAR;
        // the bake's places that see in through it, first: the cap keeps them
        for (u8 w = 0; w < o.watches; ++w)
            GatherPoint(r, S, ai().level_graph().vertex_position(o.watch[w]));
        if (m.stage > 0)
        {
            // nowhere straight in front: anywhere outside that sees in, at an
            // angle too, and farther
            r.front = .2f;
            r.near_ = 2.f;
            r.far_ = POST_FAR_WIDE;
            r.eyes = true; // watching it is better than striking it off
            r.looks = 32;
        }
        // seeing in is the post's job, from the eyes as the bake checked it; he
        // stands or leans to fire when the enemy shows
        r.eyes = true;
        house_of(f, r);
        others(f, m.id, r.friends, true);
        r.spacing = POST_SPACING;
        GatherCovers(r, S, floor, r.far_, 40, POST_MARK);
        for (const float out : {7.f, 5.f, 9.f, 11.f, 4.f})
            for (const float aside : {0.f, 2.f, -2.f})
                GatherPoint(r, S, Fvector(floor).mad(o.dir, out).mad(side, aside));
        t.focus = o.inside;
        ask(f, m, std::move(r), o.inside);
        break;
    }
    case eRoleDoor: {
        // against the wall beside it, out of the line from inside, the doorway
        // in sight: whoever comes out comes out at arm's length
        const SOpening& o = f.house.openings[m.opening];
        const Fvector floor = floor_of(o);
        const Fvector side{o.dir.z, 0.f, -o.dir.x};
        r.kind = eDoor;
        r.threat = Fvector(o.mouth).mad(o.dir, -1.5f); // deep in the room
        r.focus = Fvector(o.mouth).mad(o.dir, .8f); // the doorway
        r.focus_dir = o.dir;
        r.focus_floor = floor;
        others(f, m.id, r.friends, true);
        r.spacing = DOOR_SPACING;
        for (const float sign : {1.f, -1.f})
            for (const float out : {o.width * .5f + 1.f, o.width * .5f + 1.6f, o.width * .5f + 2.3f})
                GatherPoint(r, S, Fvector(floor).mad(side, sign * out).mad(o.dir, .7f));
        t.focus = r.focus;
        ask(f, m, std::move(r), o.mouth);
        break;
    }
    case eRoleSearch: {
        t.place = true;
        t.vertex = f.spot_vertex;
        t.position = f.spot;
        t.look = CStalkerActionSearchEnemy::CHECK_TIME;
        t.turn = true;
        break;
    }
    case eRoleAssault: {
        if (m.stage == 0 && m.opening >= 0)
        {
            // a breath by the door, crouched, gun on the doorway
            const SOpening& o = f.house.openings[m.opening];
            t.place = true;
            t.vertex = S.ai_location().level_vertex_id();
            t.position = S.Position();
            t.focus = Fvector(o.mouth).mad(o.dir, .5f);
            t.look = STACK_TIME;
            t.stand = false;
        }
        else
        {
            // in, to where he was last seen in the house -- with the threat map,
            // where in it he most likely is now
            Fvector in = f.house.spot;
            ThreatMap::SEstimate e;
            if (ThreatMap::Estimate(f.enemy, e) && e.house >= 0 && e.house == f.house.baked)
                in = e.house_point;
            t.place = true;
            t.vertex = ai().level_graph().vertex_id(in);
            t.position = in;
            t.focus = in;
            t.look = CStalkerActionSearchEnemy::CHECK_TIME;
            t.turn = true;
            t.stand = true;
        }
        break;
    }
    case eRoleSweep: {
        if (m.stage >= CStalkerActionSearchEnemy::SWEEP_POINTS)
        {
            // his places are done: hold, gun on the spot, until the time is up
            t.focus = f.spot;
            break;
        }
        r.kind = PositionQuery::eSweep;
        r.threat = f.search_from;
        t.focus = f.sweep_from;
        // ⭐with the threat map: round the likeliest place nobody has looked at
        // and nobody is on his way to
        xr_vector<Fvector> likely;
        ThreatMap::Likeliest(f.enemy, likely);
        for (const Fvector& l : likely)
        {
            const bool taken = std::any_of(f.checked.begin(), f.checked.end(), [&l](const Fvector& c) { return flat(c, l) < SWEEP_TAKEN; }) ||
                std::any_of(f.members.begin(), f.members.end(), [&](const SMember& o) { return o.id != m.id && o.task.role == eRoleSweep && o.task.place && flat(o.task.position, l) < SWEEP_TAKEN; });
            if (taken)
                continue;
            r.focus = l;
            r.far_ = SWEEP_NEAR;
            r.friends = f.checked;
            others(f, m.id, r.friends);
            r.spacing = 3.f;
            GatherPoint(r, S, l);
            GatherRings(r, S, l, {2.5f, 5.f}, 6);
            GatherCovers(r, S, l, SWEEP_NEAR, 24, 0.f);
            t.focus = l;
            ask(f, m, std::move(r), l);
            return;
        }
        // the searcher round the spot itself, the others out on their own side of it
        Fvector center = f.sweep_from;
        Fvector away = Fvector().sub(S.Position(), f.sweep_from);
        away.y = 0.f;
        if (m.stage > 0 || away.square_magnitude() > _sqr(4.f))
        {
            if (away.square_magnitude() > EPS_L)
                center.mad(away.normalize(), m.stage == 0 ? 10.f : 6.f);
        }
        r.focus = center;
        r.far_ = SWEEP_RADIUS;
        r.friends = f.checked;
        others(f, m.id, r.friends);
        r.spacing = SWEEP_TAKEN;
        GatherCovers(r, S, center, SWEEP_RADIUS, 48, 0.f);
        GatherRings(r, S, center, {6.f, 12.f}, 6);
        ask(f, m, std::move(r), center);
        break;
    }
    default: break;
    }
}

void give(SFight& f, SMember& m, ERole role, CAI_Stalker& S, int opening = -1, u32 slot = 0)
{
    release(f, m);
    STask t;
    t.role = role;
    t.focus = f.spot;
    t.run = role == eRolePost || role == eRoleDoor || role == eRoleWatch || role == eRoleFlank;
    t.stand = role == eRoleDoor || role == eRoleSearch || role == eRoleSweep || role == eRoleAssault;
    t.suppress = role == eRoleSuppress;
    set_task(m, t);
    m.since = Device.dwTimeGlobal;
    m.opening = opening;
    m.slot = slot;
    m.stage = 0;
    m.retries = 0;
    place(f, m, S);
    log_task(f, m, "gets");

    // the role said out loud: the squad hears who goes where
    if (!S.agent_manager().member().can_cry_noninfo_phrase())
        return;
    switch (role)
    {
    // after the "covering fire!", not over it
    case eRoleFlank: S.sound().play(StalkerSpace::eStalkerSoundDetour, 2500, 1000, 8000, 6000); break;
    case eRoleSearch:
        if (FirstToCry(f.enemy, StalkerSpace::eStalkerSoundSearchGo, 30000))
            S.sound().play(StalkerSpace::eStalkerSoundSearchGo, 0, 0, 10000, 8000);
        break;
    case eRoleAssault: S.sound().play(StalkerSpace::eStalkerSoundAttackAlliesSingleEnemy, 0, 0, 6000, 4000); break;
    default: break;
    }
}

// The answer to what was asked for him, once it is there.
void poll(SFight& f, SMember& m)
{
    if (m.asked == PositionQuery::eKindCount)
        return;
    PositionQuery::SResult a;
    if (!PositionQuery::Poll(m.id, m.asked, a))
    {
        // forgotten by the query (it keeps nothing 600 frames): ask anew
        if (!PositionQuery::Pending(m.id, m.asked))
        {
            m.asked = PositionQuery::eKindCount;
            m.task.pending = false;
            f.next_think = Device.dwTimeGlobal;
        }
        return;
    }
    m.asked = PositionQuery::eKindCount;
    STask t = m.task;
    t.pending = false;
    // ⭐Asked together, answered apart: each answer was scored without the
    // others' -- 📏06-10 19:44, eight watchers sent to one point. Given to
    // another man meanwhile: asked again, that place now among the friends.
    if (a.found && m.retries < 3)
    {
        const bool posts = t.role == eRolePost || t.role == eRoleDoor;
        const float spacing = t.role == eRoleDoor ? DOOR_SPACING : t.role == eRoleSweep ? SWEEP_TAKEN : t.role == eRolePost ? POST_SPACING : 3.f;
        const bool taken = std::any_of(f.members.begin(), f.members.end(), [&](const SMember& o) {
            const bool on_house = o.task.role == eRolePost || o.task.role == eRoleDoor;
            return o.id != m.id && o.task.place && !o.task.pending && (!posts || on_house) && flat(o.task.position, a.point.position) < spacing * .9f;
        });
        if (taken)
            if (CAI_Stalker* S = stalker(m.id))
            {
                ++m.retries;
                m.task.pending = false;
                place(f, m, *S);
                return;
            }
    }
    if (a.found)
    {
        t.place = true;
        t.vertex = a.point.vertex;
        t.position = a.point.position;
        t.stand = a.stand || t.role == eRoleDoor || t.role == eRoleSweep;
        if (t.role == eRoleSweep)
        {
            f.checked.push_back(t.position);
            t.look = CStalkerActionSearchEnemy::SWEEP_LOOK_TIME;
            t.turn = true;
        }
        set_task(m, t);
        log_task(f, m, "placed");
        return;
    }

    switch (t.role)
    {
    case eRolePost:
    case eRoleDoor: {
        SOpening& o = f.house.openings[m.opening];
        if (g_dbg_npc_tactics)
            Msg("[cmd] %u: nowhere to keep %s #%d (man %u)%s: %s", m.id, o.door ? "door" : "window", m.opening, m.slot + 1, m.stage ? ", asked wide" : "", a.why);
        if (t.role == eRolePost && m.stage == 0)
        {
            // straight in front there is nowhere: wider
            m.stage = 1;
            if (CAI_Stalker* S = stalker(m.id))
            {
                place(f, m, *S);
                return;
            }
        }
        // the first man in front of a window finds nowhere: nobody will, it is
        // struck off a while; another man, or by a door: no room for him
        if (t.role == eRolePost && m.slot == 0 && !o.door)
        {
            o.blind = true;
            o.blind_time = Device.dwTimeGlobal;
        }
        else
            o.full[m.slot] = true;
        if (g_dbg_npc_tactics)
            Msg("[cmd] %u finds nowhere to keep %s #%d (man %u): %s", m.id, o.door ? "door" : "window", m.opening, m.slot + 1, o.blind ? "struck off" : "no room");
        release(f, m);
        f.next_think = Device.dwTimeGlobal;
        return;
    }
    case eRoleFlank:
        release(f, m);
        return;
    case eRoleSweep:
        // nowhere left to look: hold where he is until the time is up
        m.stage = CStalkerActionSearchEnemy::SWEEP_POINTS;
        set_task(m, t);
        return;
    default:
        // a watcher with nowhere: where he stands, gun on the spot; asked again later
        set_task(m, t);
        return;
    }
}

void begin_sweep(SFight& f, const Fvector& from)
{
    f.sweep_start = Device.dwTimeGlobal;
    f.sweep_from = f.spot;
    f.search_from = from;
    f.checked.clear();
    f.checked.push_back(f.spot); // he has just looked there
    f.next_think = Device.dwTimeGlobal;
}

bool sweep_on(const SFight& f)
{
    if (!f.sweep_start || f.seen_time > f.sweep_start)
        return false;
    // with the threat map the spot wanders as the sweep looks round; only a new
    // sound of him, elsewhere, ends it
    if (ThreatMap::On())
        return f.spot_time <= f.sweep_start || f.sweep_from.distance_to_sqr(f.spot) <= _sqr(SWEEP_MOVED);
    return f.sweep_from.distance_to_sqr(f.spot) <= _sqr(SWEEP_MOVED);
}

bool siege_on(const SFight& f)
{
    return f.house.indoors && std::any_of(f.house.openings.begin(), f.house.openings.end(), [](const SOpening& o) { return !struck_off(o); });
}

// The house he is in: read from wherever he is thought to be until one is kept
// -- a shot heard from inside is what tells a squad he holed up -- and a kept
// one is read again, or left, only for where he was seen.
void read_house(SFight& f)
{
    SHouse& h = f.house;
    const bool kept = h.indoors;
    Fvector spot = kept ? f.seen_spot : f.spot;
    u32 time = kept ? f.seen_spot_time : f.spot_time;
    ThreatMap::SEstimate e;
    if (ThreatMap::Estimate(f.enemy, e))
    {
        if (!kept)
        {
            // ⭐a house is his when most of the map is in it -- not when one
            // sound came from by its wall
            if (e.house < 0 || e.house_mass < HOUSE_MASS)
                return;
            spot = e.house_point;
            time = e.info_time;
        }
        else if (h.baked >= 0 && e.house != h.baked && Device.dwTimeGlobal > f.seen_time + LEAVE_UNSEEN && ThreatMap::HouseMass(f.enemy, h.baked) < LEAVE_MASS)
        {
            // what the men see of it says he is not in it any more: read from
            // where he likely is, which leaves it
            if (g_dbg_npc_tactics)
                Msg("[cmd] the threat map has him out of the house: %.0f%% in it, likeliest (%.1f,%.1f,%.1f)", 100.f * ThreatMap::HouseMass(f.enemy, h.baked), VPUSH(e.position));
            const u32 version = h.version;
            AIHouse::Update(h, e.position, g_dbg_npc_tactics != 0);
            h.seen = std::max(e.info_time, h.seen + 1);
            if (h.version != version || !h.indoors)
                f.next_think = Device.dwTimeGlobal;
            return;
        }
    }
    if (!time)
        return;
    if (!h.built || (time > h.seen && Device.dwTimeGlobal > h.built + AIHouse::REREAD && h.spot.distance_to_sqr(spot) > _sqr(AIHouse::RESPOT)))
    {
        const u32 version = h.version;
        AIHouse::Update(h, spot, g_dbg_npc_tactics != 0);
        h.seen = time;
        if (h.version != version)
            f.next_think = Device.dwTimeGlobal;
    }
}

bool at_place(const SMember& m)
{
    return !m.task.place || m.task.pending ||
        (flat(m.position, m.task.position) < AT_PLACE && _abs(m.position.y - m.task.position.y) < 2.f);
}

// ---------------------------------------------------------------- the modes

void lost(SFight& f, u32 lost_for)
{
    SMember* suppressor = nullptr;
    u32 flankers = 0;
    for (SMember& m : f.members)
    {
        if (m.task.role == eRoleSuppress)
            suppressor = &m;
        flankers += m.task.role == eRoleFlank ? 1 : 0;
        // what other modes gave
        if (m.task.role != eRoleSuppress && m.task.role != eRoleFlank)
            release(f, m);
    }

    // one with an automatic fires into where he went to ground...
    const bool fire = g_ai_suppress_time > 0.f && lost_for < u32(g_ai_suppress_time * 1000.f);
    if (suppressor && !fire)
    {
        release(f, *suppressor);
        suppressor = nullptr;
    }
    if (!suppressor && fire)
    {
        SMember* best = nullptr;
        float best_cost = flt_max;
        for (SMember& m : f.members)
        {
            const float distance = flat(m.position, f.spot);
            if (!active(m) || !m.automatic || m.task.role != eRoleNone || distance > SUPPRESS_RANGE)
                continue;
            // who saw him last sees best where he went
            const float cost = distance + (m.own_seen ? float(Device.dwTimeGlobal - m.own_seen) * .002f : 20.f);
            if (cost < best_cost)
            {
                best_cost = cost;
                best = &m;
            }
        }
        if (best)
            if (CAI_Stalker* S = stalker(best->id))
            {
                give(f, *best, eRoleSuppress, *S);
                suppressor = best;
            }
    }

    // ...and up to two go round him, under that fire, once per loss of him
    if (!suppressor || flankers >= FLANKERS || Device.dwTimeGlobal > f.mode_time + 1500)
        return;
    while (flankers < FLANKERS)
    {
        SMember* best = nullptr;
        float best_distance = FLANK_RANGE;
        for (SMember& m : f.members)
        {
            const float distance = flat(m.position, f.spot);
            if (active(m) && m.task.role == eRoleNone && distance < best_distance)
            {
                best_distance = distance;
                best = &m;
            }
        }
        CAI_Stalker* S = best ? stalker(best->id) : nullptr;
        if (!S)
            return;
        give(f, *best, eRoleFlank, *S);
        ++flankers;
    }
}

void search(SFight& f)
{
    SMember* searcher = nullptr;
    for (SMember& m : f.members)
        if (m.task.role == eRoleSearch)
            searcher = &m;
    if (!searcher)
    {
        float best = flt_max;
        for (SMember& m : f.members)
        {
            const float distance = flat(m.position, f.spot) + (m.task.role == eRoleWatch ? 3.f : 0.f);
            if (active(m) && distance < best)
            {
                best = distance;
                searcher = &m;
            }
        }
        CAI_Stalker* S = searcher ? stalker(searcher->id) : nullptr;
        if (!S)
            return;
        give(f, *searcher, eRoleSearch, *S);
        f.search_from = S->Position();
    }
    else if (elsewhere(searcher->task.position, f.spot) && (!ThreatMap::On() || searcher->stage < SEARCH_LOOKS))
    {
        // heard of somewhere else on his way, or (the threat map) what he saw
        // on his way says he is likelier elsewhere: there
        if (CAI_Stalker* S = stalker(searcher->id))
            place(f, *searcher, *S);
        ++searcher->stage;
        touch(*searcher);
    }

    // the rest round the spot, gun on it
    for (SMember& m : f.members)
    {
        if (&m == searcher || !active(m))
            continue;
        CAI_Stalker* S = stalker(m.id);
        if (!S)
            continue;
        if (m.task.role != eRoleWatch)
            give(f, m, eRoleWatch, *S);
        else if (m.asked == PositionQuery::eKindCount &&
                 (m.asked_for.distance_to_sqr(f.spot) > _sqr(WATCH_REPICK) || (!m.task.place && Device.dwTimeGlobal > m.asked_time + NOTHING_RETRY)))
            place(f, m, *S);
    }
}

void sweep(SFight& f)
{
    if (Device.dwTimeGlobal > f.sweep_start + CStalkerActionSearchEnemy::SWEEP_TIME)
        return; // eModeDone
    for (SMember& m : f.members)
    {
        if (!active(m) || m.task.role == eRoleSweep)
            continue;
        if (CAI_Stalker* S = stalker(m.id))
            give(f, m, eRoleSweep, *S);
    }
}

void done(SFight& f)
{
    // the squad combed the area for SWEEP_TIME: he is gone. Everybody lets go
    // together, as the sweep's clock is the same for all.
    for (SMember& m : f.members)
        if (!m.task.give_up)
        {
            STask t = m.task;
            t.role = eRoleSweep;
            t.give_up = true;
            t.place = false;
            set_task(m, t);
        }
}

// Is someone keeping this opening: in front of it, or for a door beside it, at his place.
bool kept(const SFight& f, u32 k)
{
    for (const SMember& m : f.members)
        if (m.opening == int(k) && (m.task.role == eRolePost || m.task.role == eRoleDoor) && m.task.place && at_place(m) && active(m))
            return true;
    return false;
}

void siege(SFight& f)
{
    SHouse& h = f.house;
    // another house: the posts were for the last one
    if (f.house_start != h.start)
    {
        f.house_start = h.start;
        f.ring_time = 0;
        f.assault_time = 0;
        for (SMember& m : f.members)
            if (m.task.role == eRolePost || m.task.role == eRoleDoor || m.task.role == eRoleAssault)
                release(f, m);
    }

    for (SMember& m : f.members)
    {
        // what the open-ground modes gave
        if (m.task.role == eRoleSuppress || m.task.role == eRoleFlank || m.task.role == eRoleSearch || m.task.role == eRoleSweep)
            release(f, m);
        // an opening struck off meanwhile
        if ((m.task.role == eRolePost || m.task.role == eRoleDoor) && (m.opening < 0 || u32(m.opening) >= h.openings.size() || struck_off(h.openings[m.opening])))
            release(f, m);
    }

    // the first man on every opening; then beside the doors; then a second man
    // in front of the windows. The vacancies go to the men nearest them -- the
    // pairs in order of distance, not first come.
    struct SRound
    {
        u32 slot;
        int door; // -1 any, 0 windows, 1 doors
    };
    static constexpr SRound rounds[] = {{0, -1}, {1, 1}, {2, 1}, {1, 0}};
    for (const SRound& round : rounds)
    {
        struct SPair
        {
            float cost;
            u32 opening;
            SMember* m;
        };
        xr_vector<SPair> pairs;
        for (u32 k = 0; k < h.openings.size(); ++k)
        {
            const SOpening& o = h.openings[k];
            if (struck_off(o) || o.full[round.slot] || (round.door >= 0 && o.door != !!round.door))
                continue;
            const bool taken = std::any_of(f.members.begin(), f.members.end(), [&](const SMember& m) { return m.opening == int(k) && m.slot == round.slot; });
            if (taken)
                continue;
            const Fvector floor = floor_of(o);
            for (SMember& m : f.members)
                if (active(m) && (m.task.role == eRoleNone || m.task.role == eRoleWatch))
                    pairs.push_back({flat(m.position, floor), k, &m});
        }
        std::sort(pairs.begin(), pairs.end(), [](const SPair& a, const SPair& b) { return a.cost < b.cost; });
        xr_vector<u32> filled;
        for (const SPair& p : pairs)
        {
            if (p.m->task.role != eRoleNone && p.m->task.role != eRoleWatch)
                continue; // given a post this round already
            if (std::find(filled.begin(), filled.end(), p.opening) != filled.end())
                continue;
            CAI_Stalker* S = stalker(p.m->id);
            if (!S)
                continue;
            const bool beside = h.openings[p.opening].door && round.slot > 0;
            give(f, *p.m, beside ? eRoleDoor : eRolePost, *S, int(p.opening), round.slot);
            filled.push_back(p.opening);
        }
    }

    // the rest round the house
    for (SMember& m : f.members)
    {
        if (!active(m))
            continue;
        CAI_Stalker* S = stalker(m.id);
        if (!S)
            continue;
        if (m.task.role == eRoleNone)
            give(f, m, eRoleWatch, *S);
        else if (m.task.role == eRoleWatch && m.asked == PositionQuery::eKindCount &&
                 (m.watching < 0 || f.watch_version != h.version || (!m.task.place && Device.dwTimeGlobal > m.asked_time + NOTHING_RETRY)))
            place(f, m, *S);
    }

    f.watch_version = h.version;

    // ringed: every opening kept
    bool ringed = true;
    for (u32 k = 0; k < h.openings.size() && ringed; ++k)
        ringed = struck_off(h.openings[k]) || kept(f, k);
    if (!ringed)
        f.ring_time = 0;
    else if (!f.ring_time)
    {
        f.ring_time = Device.dwTimeGlobal;
        if (g_dbg_npc_tactics)
            Msg("[cmd] siege: every opening kept, %u s after it began", (Device.dwTimeGlobal - h.start) / 1000);
    }

    // ⭐Once ringed a while, one of the men by the door goes in.
    const u32 age = Device.dwTimeGlobal - h.start;
    const bool go = (f.ring_time && Device.dwTimeGlobal > f.ring_time + RING_HOLD && age > SIEGE_MIN) || age > SIEGE_HOLD;
    const bool going = std::any_of(f.members.begin(), f.members.end(), [](const SMember& m) { return m.task.role == eRoleAssault && active(m); });
    if (!go || going || (f.assault_time && Device.dwTimeGlobal < f.assault_time + ASSAULT_RETRY))
        return;
    SMember* who = nullptr;
    float best = flt_max;
    for (SMember& m : f.members)
    {
        if (!active(m))
            continue;
        // beside a door and there first; then a man in front of one; then anyone
        const float rank = m.task.role == eRoleDoor && m.task.place && at_place(m) ? 0.f : m.task.role == eRoleDoor ? 100.f : m.task.role == eRolePost && h.openings[m.opening].door ? 200.f : 300.f;
        const float cost = rank + flat(m.position, h.spot);
        if (cost < best)
        {
            best = cost;
            who = &m;
        }
    }
    CAI_Stalker* S = who ? stalker(who->id) : nullptr;
    if (!S)
        return;
    const bool by_door = who->task.role == eRoleDoor && who->task.place && at_place(*who);
    const int door = by_door ? who->opening : -1;
    give(f, *who, eRoleAssault, *S, door, SLOTS); // no slot: his place by the door is free for the next man
    if (!by_door)
    {
        // nobody by a door: straight in
        who->stage = 1;
        place(f, *who, *S);
        touch(*who);
    }
    f.assault_time = Device.dwTimeGlobal;
}

void prune(SFight& f)
{
    for (u32 i = 0; i < f.members.size();)
    {
        SMember& m = f.members[i];
        CAI_Stalker* S = stalker(m.id);
        const u32 away = Device.dwTimeGlobal - m.reported;
        if (!S || !S->g_Alive() || away > KEEP)
        {
            if (m.asked != PositionQuery::eKindCount)
                PositionQuery::Cancel(m.id, m.asked);
            f.members.erase(f.members.begin() + i);
            continue;
        }
        if (away > KEEP_SHORT && moving_role(m.task.role))
            release(f, m);
        ++i;
    }
}

void think(SFight& f)
{
    const u32 now = Device.dwTimeGlobal;
    f.next_think = now + THINK_EVERY;
    prune(f);
    if (f.members.empty())
        return;

    // ⭐"where he is": the threat map's likeliest place, a sound only nudges it
    ThreatMap::SEstimate estimate;
    if (ThreatMap::Estimate(f.enemy, estimate))
    {
        f.spot = estimate.position;
        f.spot_vertex = estimate.vertex;
        f.spot_time = std::max(f.spot_time, estimate.info_time);
    }

    read_house(f);

    // seen or heard of again after letting go: a new fight
    if (f.mode == eModeDone && std::max(f.seen_time, f.spot_time) > f.mode_time)
        f.sweep_start = 0;

    EMode mode;
    const u32 lost_for = f.seen_time ? now - f.seen_time : u32(-1);
    if (sweep_on(f))
        mode = now > f.sweep_start + CStalkerActionSearchEnemy::SWEEP_TIME ? eModeDone : eModeSweep;
    else if (f.mode == eModeDone && std::max(f.seen_time, f.spot_time) <= f.mode_time)
        mode = eModeDone;
    else if (siege_on(f))
        mode = eModeSiege;
    else if (lost_for < SUPPRESS_START)
        mode = eModeEngage;
    else if (lost_for < SEARCH_DELAY)
        mode = eModeLost;
    else
        mode = eModeSearch;

    if (mode != f.mode)
    {
        if (g_dbg_npc_tactics)
            Msg("[cmd] fight against %u: %s -> %s, %u men, spot (%.1f,%.1f,%.1f)", f.enemy, MODE_NAME[f.mode], MODE_NAME[mode], u32(f.members.size()), VPUSH(f.spot));
        f.mode = mode;
        f.mode_time = now;
        if (mode == eModeEngage || mode == eModeSweep)
            for (SMember& m : f.members)
                if (!(mode == eModeSweep && m.task.role == eRoleSweep))
                    release(f, m);
        if (mode == eModeSweep)
            for (SMember& m : f.members)
                m.stage = 0;
    }

    switch (mode)
    {
    case eModeEngage: break;
    case eModeLost: lost(f, lost_for); break;
    case eModeSearch: search(f); break;
    case eModeSweep: sweep(f); break;
    case eModeSiege: siege(f); break;
    case eModeDone: done(f); break;
    }
}
} // namespace

bool Commander::Applies(const CAI_Stalker& npc)
{
    if (!PositionQuery::g_ai_tactics_v2)
        return false;
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    const CActor* actor = Actor();
    if (!enemy || !actor || enemy != actor)
        return false;
    const float distance_sqr = npc.Position().distance_to_sqr(actor->Position());
    if (distance_sqr < _sqr(FIGHT_RANGE))
        return true;
    // in it already: a man does not drop out of it for a step past the line
    const auto I = s_fights.find(enemy->ID());
    return distance_sqr < _sqr(FIGHT_KEEP) && I != s_fights.end() && member(I->second, u16(npc.ID()));
}

void Commander::Report(CAI_Stalker& npc)
{
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    if (!enemy)
        return;
    const u32 now = Device.dwTimeGlobal;
    SFight& f = s_fights[enemy->ID()];
    f.enemy = enemy->ID();
    const u16 id = u16(npc.ID());
    SMember* m = member(f, id);
    if (!m)
    {
        f.members.push_back({});
        m = &f.members.back();
        m->id = id;
        f.next_think = now;
    }
    else if (!active(*m))
        f.next_think = now; // back from a script's step aside
    m->reported = now;
    m->position = npc.Position();
    ThreatMap::Look(f.enemy, npc);

    const bool sees = npc.memory().visual().visible_now(enemy);
    if (sees && now > f.seen_time + SUPPRESS_START)
        f.next_think = now; // found again
    m->sees = sees;
    if (sees)
    {
        m->own_seen = now;
        f.seen_time = now;
    }
    const CMemoryInfo info = npc.memory().memory(enemy);
    const bool map = ThreatMap::On();
    // the threat map begins from what the first man remembers; a sound or a
    // hit is a guess a few metres wide
    if (map && info.m_object && !ThreatMap::Exists(f.enemy))
        ThreatMap::Seed(f.enemy, info.m_object_params.m_position, info.m_object_params.m_level_vertex_id, info.m_visual_info ? 0.f : info.m_sound_info ? 4.f : 8.f,
                        info.m_level_time);
    if (!map && info.m_object && info.m_level_time > f.spot_time)
    {
        f.spot = info.m_object_params.m_position;
        f.spot_vertex = info.m_object_params.m_level_vertex_id;
        f.spot_time = info.m_level_time;
    }
    for (const CVisibleObject& v : npc.memory().visual().objects())
        if (v.m_object == enemy && v.m_level_time > f.seen_spot_time)
        {
            f.seen_spot = v.m_object_params.m_position;
            f.seen_spot_time = v.m_level_time;
            ThreatMap::Seen(f.enemy, v.m_object_params.m_position, v.m_object_params.m_level_vertex_id, v.m_level_time);
        }
    const CWeapon* weapon = smart_cast<const CWeapon*>(npc.best_weapon());
    m->automatic = weapon && weapon->ef_weapon_type() == 6 && weapon->GetAmmoElapsed() > 0;

    poll(f, *m);
    if (now >= f.next_think)
        think(f);
}

const Commander::STask* Commander::Task(const CAI_Stalker& npc)
{
    if (!Applies(npc))
        return nullptr;
    SFight* f = fight_of(npc.memory().enemy().selected()->ID());
    SMember* m = f ? member(*f, u16(npc.ID())) : nullptr;
    return m && m->task.role != eRoleNone ? &m->task : nullptr;
}

bool Commander::AtPlace(const CAI_Stalker& npc)
{
    const STask* t = Task(npc);
    return !t || !t->place || t->pending || (flat(npc.Position(), t->position) < AT_PLACE && _abs(npc.Position().y - t->position.y) < 2.f);
}

void Commander::Done(const CAI_Stalker& npc)
{
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    SFight* f = enemy ? fight_of(enemy->ID()) : nullptr;
    SMember* m = f ? member(*f, u16(npc.ID())) : nullptr;
    CAI_Stalker* S = m ? stalker(m->id) : nullptr;
    if (!S)
        return;
    switch (m->task.role)
    {
    case eRoleSearch:
        if (ThreatMap::On() && m->stage < SEARCH_LOOKS && elsewhere(m->task.position, f->spot))
        {
            // not here, but the map has a likelier place: on to it
            ++m->stage;
            place(*f, *m, *S);
            touch(*m);
            log_task(*f, *m, "looks on");
            break;
        }
        // not here: "spread out, find him"
        log_task(*f, *m, "found nobody");
        S->sound().play(StalkerSpace::eStalkerSoundEnemyLost, 0, 0, 10000, 8000);
        begin_sweep(*f, f->search_from);
        break;
    case eRoleAssault:
        if (m->stage == 0)
        {
            ++m->stage;
            place(*f, *m, *S);
            touch(*m);
            log_task(*f, *m, "goes in");
        }
        else if (ThreatMap::SEstimate e; m->stage < INSIDE_LOOKS && ThreatMap::Estimate(f->enemy, e) && e.house >= 0 && e.house == f->house.baked && e.house_mass >= INSIDE_MASS &&
                 elsewhere(m->task.position, e.house_point))
        {
            // another room, a corner he did not see from here
            ++m->stage;
            place(*f, *m, *S);
            touch(*m);
            log_task(*f, *m, "looks on inside");
        }
        else
        {
            log_task(*f, *m, "found nobody inside");
            S->sound().play(StalkerSpace::eStalkerSoundEnemyLost, 0, 0, 10000, 8000);
            begin_sweep(*f, f->house.origin);
        }
        break;
    case eRoleSweep:
        ++m->stage;
        place(*f, *m, *S);
        touch(*m);
        break;
    default: break;
    }
}

void Commander::Unreachable(const CAI_Stalker& npc)
{
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    SFight* f = enemy ? fight_of(enemy->ID()) : nullptr;
    SMember* m = f ? member(*f, u16(npc.ID())) : nullptr;
    if (!m || !m->task.place)
        return;
    STask t = m->task;
    t.position = npc.Position();
    t.vertex = npc.ai_location().level_vertex_id();
    set_task(*m, t);
    log_task(*f, *m, "can get no nearer");
}

bool Commander::FirstToCry(u16 enemy, u32 sound, u32 interval)
{
    SFight* f = fight_of(enemy);
    if (!f)
        return true;
    u32& last = f->cried[sound];
    if (last && Device.dwTimeGlobal < last + interval)
        return false;
    last = Device.dwTimeGlobal;
    return true;
}

bool Commander::Owns(u16 enemy) { return !!fight_of(enemy); }

bool Commander::InsideHouse(u16 enemy, const Fvector& p)
{
    const SFight* f = fight_of(enemy);
    return f && AIHouse::Inside(f->house, p);
}

bool Commander::Suppressor(u16 enemy, u16 npc) { return Suppressor(enemy) == npc; }

u16 Commander::Suppressor(u16 enemy)
{
    if (SFight* f = fight_of(enemy))
        for (const SMember& m : f->members)
            if (m.task.role == eRoleSuppress && active(m))
                return m.id;
    return u16(-1);
}

bool Commander::Flanker(u16 enemy, u16 npc)
{
    SFight* f = fight_of(enemy);
    const SMember* m = f ? member(*f, npc) : nullptr;
    return m && m->task.role == eRoleFlank;
}

u16 Commander::Searcher(u16 enemy)
{
    if (SFight* f = fight_of(enemy))
        for (const SMember& m : f->members)
            if ((m.task.role == eRoleSearch || m.task.role == eRoleAssault) && active(m))
                return m.id;
    return u16(-1);
}

LPCSTR Commander::RoleName(ERole role)
{
    static constexpr LPCSTR names[eRoleCount] = {"none", "suppress", "flank", "post", "door", "watch", "search", "assault", "sweep"};
    return role < eRoleCount ? names[role] : "?";
}

bool Commander::DbgHouse(u16 enemy, SStalkerDbgSiege& out)
{
    out = {};
    const SFight* f = fight_of(enemy);
    if (!f || !f->house.start)
        return false;
    const SHouse& h = f->house;
    out.indoors = h.indoors;
    out.start = h.start;
    out.origin = h.origin;
    out.openings.reserve(h.openings.size());
    for (const SOpening& o : h.openings)
        out.openings.push_back({o.mouth, o.inside, o.dir, o.width, o.door, struck_off(o)});
    return true;
}

EStalkerDbgRole Commander::DbgRole(u16 enemy, u16 npc, string64& text)
{
    text[0] = 0;
    SFight* f = fight_of(enemy);
    const SMember* m = f ? member(*f, npc) : nullptr;
    if (!m || m->task.role == eRoleNone)
        return eStalkerDbgRoleNone;
    const STask& t = m->task;
    LPCSTR how = t.give_up ? " (letting go)" : t.pending ? " (asking)" : !t.place ? "" : at_place(*m) ? "" : " (going)";
    switch (t.role)
    {
    case eRoleSuppress: xr_sprintf(text, "suppress%s", how); return eStalkerDbgRoleSuppressor;
    case eRoleFlank: xr_sprintf(text, "flank%s", how); return eStalkerDbgRoleFlanker;
    case eRolePost:
    case eRoleDoor: {
        const bool door = m->opening >= 0 && u32(m->opening) < f->house.openings.size() && f->house.openings[m->opening].door;
        xr_sprintf(text, "post %s #%d/%u%s%s", door ? "door" : "window", m->opening, m->slot + 1, t.role == eRoleDoor ? " beside" : "", how);
        return eStalkerDbgRolePost;
    }
    case eRoleWatch: xr_sprintf(text, "watch%s", how); return eStalkerDbgRoleWatch;
    case eRoleSearch: xr_sprintf(text, "searcher%s", how); return eStalkerDbgRoleSearcher;
    case eRoleAssault: xr_sprintf(text, "assault: %s%s", m->stage == 0 ? "by the door" : "in", how); return eStalkerDbgRoleSearcher;
    case eRoleSweep: xr_sprintf(text, "sweep %u/%u%s", m->stage, u32(CStalkerActionSearchEnemy::SWEEP_POINTS), how); return eStalkerDbgRoleSearcher;
    default: return eStalkerDbgRoleNone;
    }
}

void Commander::DbgDrawHouses(const Fvector& eye, float range)
{
    using namespace AIDebugDraw;
    if (!PositionQuery::g_ai_tactics_v2)
        return;
    for (const auto& [enemy, f] : s_fights)
    {
        const SHouse& h = f.house;
        if (!h.indoors || eye.distance_to_sqr(h.origin) > _sqr(range + h.radius))
            continue;
        Ring(h.origin, h.radius, color_rgba(200, 200, 200, 70));
        Text(Fvector(h.origin).add(Fvector{0.f, 3.f, 0.f}), color_rgba(255, 255, 255, 230), 0, "house: %u opening(s), held %u s%s | %s", u32(h.openings.size()),
             (Device.dwTimeGlobal - h.start) / 1000, f.ring_time ? ", ringed" : "", MODE_NAME[f.mode]);
        for (u32 k = 0; k < h.openings.size(); ++k)
        {
            const SOpening& o = h.openings[k];
            const bool off = struck_off(o);
            const u32 color = off ? color_rgba(150, 150, 150, 200) : o.door ? color_rgba(60, 255, 120, 255) : color_rgba(80, 200, 255, 255);
            const Fvector side{o.dir.z, 0.f, -o.dir.x};
            const Fvector l = Fvector(o.mouth).mad(side, o.width * .5f), r = Fvector(o.mouth).mad(side, -o.width * .5f);
            const Fvector up{0.f, .35f, 0.f};
            Line(l, r, color);
            Line(Fvector(l).sub(up), Fvector(l).add(up), color);
            Line(Fvector(r).sub(up), Fvector(r).add(up), color);
            Line(o.mouth, Fvector(o.mouth).mad(o.dir, 1.f), color);
            Cross(o.inside, .1f, color);
            Text(Fvector(o.mouth).add(Fvector{0.f, .55f, 0.f}), color, 0, "#%u %s %.1fm%s", k, o.door ? "door" : o.knee ? "window (knee)" : "window", o.width, off ? " blind" : "");
            for (const SMember& m : f.members)
            {
                if (m.opening != int(k) || !m.task.place || (m.task.role != eRolePost && m.task.role != eRoleDoor))
                    continue;
                const Fvector at = Fvector(m.task.position).add(Fvector{0.f, 1.f, 0.f});
                Cross(at, .3f, color);
                Line(at, m.task.focus, subst_alpha(color, 120));
            }
        }
    }
}

bool Commander::DbgSpot(u16 enemy, Fvector& spot, u32& seen_time)
{
    const SFight* f = fight_of(enemy);
    if (!f || !f->spot_time)
        return false;
    spot = f->spot;
    seen_time = f->seen_time;
    return true;
}

void Commander::Forget(u16 enemy)
{
    const auto I = s_fights.find(enemy);
    if (I == s_fights.end())
        return;
    for (const SMember& m : I->second.members)
        if (m.asked != PositionQuery::eKindCount)
            PositionQuery::Cancel(m.id, m.asked);
    s_fights.erase(I);
    ThreatMap::Forget(enemy);
}

void Commander::Reset() { s_fights.clear(); }

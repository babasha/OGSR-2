#include "StdAfx.h"
#include "ai_house.h"

#include "Level.h"
#include "ai_space.h"
#include "level_graph.h"
#include <atomic>
#include <thread>

// What was the siege's house reading (stalker_combat_actions.cpp, 06-10), moved
// here as it was so that the commander reads houses the same way.
namespace
{
constexpr u32 RAYS = 72; // 5 degrees: a window a metre wide 6 m off is two rays
constexpr float SCAN = 25.f; // how far along a ray to look for the open sky
constexpr float ROOF = 12.f; // something this close overhead: a roof
constexpr float MIN_WIDTH = .6f; // narrower is a crack, not a window
constexpr float EAVES = 2.f; // the sky begins at most this far past the wall the opening is in

float ray_range(const Fvector& from, const Fvector& dir, float range)
{
    collide::rq_result result;
    return Level().ObjectSpace.RayPick(from, dir, range, collide::rqtStatic, result, nullptr) ? result.range : range;
}

Fvector ray_dir(float angle) { return Fvector{_sin(angle), 0.f, _cos(angle)}; }

// How far along `dir` from `from` the open sky begins; 0 if the ray hits
// something first -- a wall, or the far side of a room with a roof all over it.
float sky_distance(const Fvector& from, const Fvector& dir, float& hit)
{
    hit = ray_range(from, dir, SCAN);
    for (float d = 1.f; d < hit - .3f; d += 1.f)
        if (!AIHouse::Roofed(Fvector(from).mad(dir, d)))
            return d;
    return 0.f;
}

// A man walks out by it: on the AI map, from a metre inside the wall at `wall`
// along `dir` from `spot`, to past it, crossing the wall line in the opening
// itself -- not through a door next to it. 📏06-10 15:49: the izba's one door
// failed a straight line a metre out: a porch, its steps going off sideways.
// So the map's own links, a few metres round the opening.
bool walks_out(const Fvector& spot, const Fvector& dir, float wall, float half_width)
{
    constexpr float reach = 5.f; // how far round the opening to walk the map: off a roofed porch too
    constexpr float past = .6f; // this far past the wall line is out
    constexpr u32 most = 200; // vertices to look at, at most
    const CLevelGraph& graph = ai().level_graph();
    const Fvector mouth = Fvector(spot).mad(dir, wall);
    const Fvector side{dir.z, 0.f, -dir.x};
    const auto along = [&](const Fvector& p) { return (p.x - mouth.x) * dir.x + (p.z - mouth.z) * dir.z; };
    const auto across = [&](const Fvector& p) { return (p.x - mouth.x) * side.x + (p.z - mouth.z) * side.z; };
    const auto close_by = [&](const Fvector& p) { return _sqr(p.x - mouth.x) + _sqr(p.z - mouth.z) < _sqr(reach); };

    u32 start = graph.vertex_id(Fvector(spot).mad(dir, std::max(wall - 1.f, 0.f)));
    if (!graph.valid_vertex_id(start))
        start = graph.vertex_id(spot);
    if (!graph.valid_vertex_id(start))
        return false;

    xr_vector<u32> seen{start};
    for (u32 next = 0; next < seen.size() && seen.size() < most; ++next)
    {
        const u32 vertex = seen[next];
        const Fvector here = graph.vertex_position(vertex);
        CLevelGraph::const_iterator I, E;
        graph.begin(vertex, I, E);
        for (; I != E; ++I)
        {
            const u32 neighbour = graph.value(vertex, I);
            if (!graph.valid_vertex_id(neighbour) || std::find(seen.begin(), seen.end(), neighbour) != seen.end())
                continue;
            const Fvector there = graph.vertex_position(neighbour);
            if (!close_by(there))
                continue;
            const float a0 = along(here), a1 = along(there);
            // over the wall line: only within the opening
            if (a0 <= 0.f && a1 > 0.f)
            {
                const float t = a0 / (a0 - a1);
                if (_abs(across(here) + (across(there) - across(here)) * t) > half_width + .35f)
                    continue;
            }
            // past the wall line, straight on out of it: not round the next room
            // to that room's own door
            if (a1 > 0.f && _abs(across(there)) > half_width + 1.5f)
                continue;
            // out under the sky: past an inner doorway is only the next room
            if (a1 > past && !AIHouse::Roofed(Fvector(there).add(Fvector{0.f, .5f, 0.f})))
                return true;
            seen.push_back(neighbour);
        }
    }
    return false;
}
} // namespace

bool AIHouse::Roofed(const Fvector& point) { return ray_range(Fvector(point).add(Fvector{0.f, .2f, 0.f}), Fvector{0.f, 1.f, 0.f}, ROOF) < ROOF; }

namespace
{
bool read_baked(const Fvector& spot, xr_vector<AIHouse::SOpening>& found, int* house, AIHouse::ERead& result);
}

AIHouse::ERead AIHouse::Read(const Fvector& spot, xr_vector<SOpening>& found, int* house)
{
    found.clear();
    if (house)
        *house = -1;
    ERead baked;
    if (read_baked(spot, found, house, baked))
        return baked;

    // live: rays out from the spot (the siege, 06-10)
    if (!Roofed(Fvector(spot).add(Fvector{0.f, 1.f, 0.f})))
        return eOutdoors;

    constexpr float heights[] = {.4f, 1.2f, 1.7f}; // knee: a door; chest and head: a window
    float sky[std::size(heights)][RAYS], hit[std::size(heights)][RAYS];
    u32 walled = 0;
    for (u32 i = 0; i < RAYS; ++i)
    {
        const Fvector dir = ray_dir(float(i) * PI_MUL_2 / float(RAYS));
        bool out = false;
        for (u32 h = 0; h < std::size(heights); ++h)
        {
            sky[h][i] = sky_distance(Fvector(spot).add(Fvector{0.f, heights[h], 0.f}), dir, hit[h][i]);
            out = out || sky[h][i] > 0.f;
        }
        walled += out ? 0 : 1;
    }
    // under a roof but open round: a shed, a porch -- no house to keep
    if (walled < RAYS / 2)
        return eShed;

    const auto open = [&](u32 i) {
        i %= RAYS;
        return sky[0][i] > 0.f || sky[1][i] > 0.f || sky[2][i] > 0.f;
    };

    u32 first_wall = 0;
    while (open(first_wall))
        ++first_wall; // walled >= RAYS / 2: there is one

    const float step = PI_MUL_2 / float(RAYS);
    for (u32 i = first_wall + 1; i < first_wall + RAYS;)
    {
        if (!open(i))
        {
            ++i;
            continue;
        }
        const u32 begin = i;
        bool door = false, chest = false;
        float out = flt_max; // the nearest the sky came: just past the wall
        while (i < first_wall + RAYS && open(i))
        {
            const u32 k = i % RAYS;
            door = door || (sky[0][k] > 0.f && sky[1][k] > 0.f);
            chest = chest || sky[1][k] > 0.f;
            for (u32 h = 0; h < std::size(heights); ++h)
                if (sky[h][k] > 0.f)
                    out = std::min(out, sky[h][k]);
            ++i;
        }
        const u32 count = i - begin;
        // the wall line: where the rays either side of it, the jambs, hit at its
        // height; the sky begins only past the eaves, a metre out at times.
        // 📏06-10 15:31: jambs well short of the sky are an inner doorway the
        // rays went through on their way to a window -- read as the house's door,
        // it put two men "beside the door" in the player's front room. Then the
        // wall is about where the roof ends.
        const u32 h = chest || door ? 1 : 2;
        const float jamb = std::min(hit[h][(begin + RAYS - 1) % RAYS], hit[h][i % RAYS]);
        const float wall = jamb < out && jamb > out - EAVES ? jamb : std::max(out - 1.f, .5f);
        const float width = float(count) * step * wall;
        if (width < MIN_WIDTH)
            continue;

        SOpening opening;
        opening.dir = ray_dir((float(begin) + float(count - 1) * .5f) * step);
        // 📏06-10 15:23: a one-door izba read as three doors -- low windows, or
        // the spot on a bench, let the knee-high ray out too. A door is one a man
        // walks out by, on the AI map (walks_out).
        opening.knee = door;
        if (door)
            door = walks_out(spot, opening.dir, wall, width * .5f);
        const float height = door || chest ? 1.2f : 1.7f;
        opening.mouth = Fvector(spot).mad(opening.dir, wall);
        opening.mouth.y += height;
        opening.height = height;
        opening.inside = Fvector(opening.mouth).mad(opening.dir, -.3f);
        opening.width = width;
        opening.door = door;
        found.push_back(opening);
    }

    // a door or a window with a post or a frame in the middle reads as two
    for (u32 a = 0; a < found.size(); ++a)
        for (u32 b = a + 1; b < found.size();)
        {
            if (found[a].mouth.distance_to_sqr(found[b].mouth) >= _sqr(SAME_OPENING))
            {
                ++b;
                continue;
            }
            if (found[b].width > found[a].width)
            {
                found[b].door = found[b].door || found[a].door;
                found[a] = found[b];
            }
            else
                found[a].door = found[a].door || found[b].door;
            found.erase(found.begin() + b);
        }
    return eIndoors;
}

void AIHouse::Log(LPCSTR why, const Fvector& spot, bool indoors, u32 openings, u32 doors, u32 held_s)
{
    Msg("[tac] siege %s: from (%.1f,%.1f,%.1f) %s, %u opening(s), %u door(s), held %u s", why, VPUSH(spot), indoors ? "indoors" : "outdoors", openings, doors, held_s);
}

void AIHouse::LogOpening(u32 k, const SOpening& o)
{
    Msg("[tac]   #%u %s bearing %.0f, %.1f m wide, at (%.1f,%.1f,%.1f)%s", k, o.door ? "door" : "window", rad2deg(atan2f(o.dir.x, o.dir.z)), o.width, VPUSH(o.mouth),
        o.knee && !o.door ? " (open to the knee, no way out on the AI map)" : "");
}

// ================================================================ the bake

namespace
{
using namespace AIHouse;

constexpr u32 CACHE_MAGIC = 0x53484941; // "AIHS"
constexpr u32 CACHE_VERSION = 4;
constexpr float WALL_REACH = 8.f; // enclosure: a wall this near in a direction
constexpr u32 WALLED = 5; // ...in this many of the eight: walled round. 6 left out an entrance hall with its door open (06-10 20:56); a porch has 4 at most
constexpr u32 MIN_VERTICES = 4; // smaller is a niche, a doghouse
constexpr u32 MAX_VERTICES = 8000; // bigger is a hangar, a tunnel: no house to ring
constexpr float OPENING_HEIGHT = 1.2f; // a mouth's height over the floor inside
constexpr float SKY_PAST = 2.5f; // a window: the sky this near past the wall (eaves)
constexpr float WINDOW_MERGE = 1.f; // rays out this near along a wall: one window
constexpr float DOOR_MERGE = 2.5f; // 1.5 left the izba's one door two: out onto its porch both ways round the doorway (06-10 21:57)

// Link i of an AI-map vertex points this way: read off the map itself, from
// the first vertices that have each link (calibrate). 📏06-10 20:56: a table
// from the path code had x the wrong way -- the izba's west door "faced" east.
Fvector LINK_DIR[4] = {{1.f, 0.f, 0.f}, {0.f, 0.f, -1.f}, {-1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}};

void calibrate(const CLevelGraph& graph, u32 n)
{
    bool known[4]{};
    for (u32 v = 0; v < n && !(known[0] && known[1] && known[2] && known[3]); ++v)
        for (int i = 0; i < 4; ++i)
        {
            const u32 w = graph.vertex(v)->link(i);
            if (known[i] || !graph.valid_vertex_id(w))
                continue;
            Fvector d = Fvector().sub(graph.vertex_position(w), graph.vertex_position(v));
            d.y = 0.f;
            // to the axis it is nearest
            LINK_DIR[i] = _abs(d.x) > _abs(d.z) ? Fvector{d.x > 0.f ? 1.f : -1.f, 0.f, 0.f} : Fvector{0.f, 0.f, d.z > 0.f ? 1.f : -1.f};
            known[i] = true;
        }
}

// Walking on out from this vertex leads under the open sky, not into a closet
// or along a covered gallery to another room of the same house.
bool leads_out(const CLevelGraph& graph, const xr_vector<u8>& flags, u32 start)
{
    constexpr u32 STEPS = 40;
    xr_vector<u32> seen{start};
    for (u32 k = 0; k < seen.size() && seen.size() < STEPS; ++k)
    {
        if (flags[seen[k]] == 0)
            return true;
        for (int i = 0; i < 4; ++i)
        {
            const u32 w = graph.vertex(seen[k])->link(i);
            if (graph.valid_vertex_id(w) && flags[w] != 3 && std::find(seen.begin(), seen.end(), w) == seen.end())
                seen.push_back(w);
        }
    }
    return false;
}

struct SHouseBaked
{
    Fvector origin{};
    float radius{};
    u32 vertices{};
    xr_vector<SOpening> openings;
};

xr_vector<SHouseBaked> s_houses;
xr_vector<std::pair<u32, u16>> s_vertex_house; // sorted by vertex
std::atomic<bool> s_ready{false};
std::atomic<bool> s_stop{false};
std::thread s_baker;

bool blocked(const Fvector& from, const Fvector& to, float slack)
{
    Fvector dir = Fvector().sub(to, from);
    const float length = dir.magnitude();
    const float range = length - slack;
    if (range <= 0.f)
        return false;
    dir.div(length);
    collide::rq_result hit;
    return !!Level().ObjectSpace.RayPick(from, dir, range, collide::rqtStatic, hit, nullptr);
}

// A man at `p` sees `target`: crouched or standing eyes, straight or leaning.
bool sees(const Fvector& p, const Fvector& target)
{
    Fvector way = Fvector().sub(target, p);
    way.y = 0.f;
    const float distance = way.magnitude();
    if (distance < 1.f)
        return true;
    way.div(distance);
    const Fvector side{way.z, 0.f, -way.x};
    for (const float height : {1.f, 1.6f})
    {
        const Fvector eye = Fvector(p).add(Fvector{0.f, height, 0.f});
        if (!blocked(eye, target, .3f))
            return true;
        for (const float sign : {1.f, -1.f})
        {
            const Fvector aside = Fvector(eye).mad(side, sign * .5f);
            if (!blocked(eye, aside, 0.f) && !blocked(aside, target, .3f))
                return true;
        }
    }
    return false;
}

template <class F>
void parallel_range(u32 count, F&& body)
{
    u32 threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    if (threads <= 1 || count < 4096)
    {
        body(0u, count);
        return;
    }
    const u32 grain = (count + threads - 1) / threads;
    xr_vector<std::thread> pool;
    for (u32 t = 1; t < threads; ++t)
    {
        const u32 lo = t * grain, hi = std::min(count, lo + grain);
        if (lo < hi)
            pool.emplace_back([&body, lo, hi] { body(lo, hi); });
    }
    body(0u, std::min(count, grain));
    for (std::thread& t : pool)
        t.join();
}

struct SEdge
{
    u32 side; // LINK_DIR index
    Fvector at; // on the wall line, at the floor inside
};

// Rays this near along one wall are one opening: its middle, its width. A
// door's ways out are merged round a corner too: a doorway in a corner, or
// two cells of one doorway left one way and one the other.
void merge(xr_vector<SEdge>& edges, float merge_distance, float cell, bool door, xr_vector<SOpening>& out)
{
    xr_vector<bool> used(edges.size(), false);
    for (u32 a = 0; a < edges.size(); ++a)
    {
        if (used[a])
            continue;
        xr_vector<u32> group{a};
        used[a] = true;
        for (u32 k = 0; k < group.size(); ++k)
            for (u32 b = 0; b < edges.size(); ++b)
                if (!used[b] && (door || edges[b].side == edges[a].side || edges[b].at.distance_to(edges[group[k]].at) < cell * 1.5f) && edges[b].at.distance_to(edges[group[k]].at) < merge_distance)
                {
                    used[b] = true;
                    group.push_back(b);
                }
        Fvector dir{};
        for (const u32 i : group)
            dir.add(LINK_DIR[edges[i].side]);
        if (dir.square_magnitude() < EPS_L)
            dir = LINK_DIR[edges[a].side];
        dir.normalize();
        const Fvector side{dir.z, 0.f, -dir.x};
        Fvector mid{};
        float lo = flt_max, hi = -flt_max;
        for (const u32 i : group)
        {
            mid.add(edges[i].at);
            const float s = side.dotproduct(edges[i].at);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        mid.div(float(group.size()));
        SOpening o;
        o.dir = dir;
        o.height = OPENING_HEIGHT;
        o.mouth = Fvector(mid).add(Fvector{0.f, OPENING_HEIGHT, 0.f});
        o.inside = Fvector(o.mouth).mad(dir, -.3f);
        o.width = hi - lo + cell;
        o.door = door;
        o.knee = door;
        out.push_back(o);
    }
}

// The places outside that see in through it, nearest the mark first. A window
// with none is no window: read through a gap, a frame, a bush.
void watch_points(const CLevelGraph& graph, const xr_vector<u8>& flags, SOpening& o)
{
    const Fvector floor = Fvector(o.mouth).sub(Fvector{0.f, o.height, 0.f});
    const Fvector side{o.dir.z, 0.f, -o.dir.x};
    const Fvector into[] = {o.inside, Fvector(o.mouth).mad(o.dir, -1.2f), Fvector(o.mouth).mad(o.dir, -2.7f)};
    o.watches = 0;
    for (const float out : {5.f, 7.f, 3.5f, 9.f, 12.f})
        for (const float aside : {0.f, -1.5f, 1.5f, -3.f, 3.f})
        {
            if (o.watches >= MAX_WATCH)
                return;
            const u32 v = graph.vertex_id(Fvector(floor).mad(o.dir, out).mad(side, aside));
            if (!graph.valid_vertex_id(v) || flags[v] == 3)
                continue;
            const Fvector p = graph.vertex_position(v);
            if (_abs(p.y - floor.y) > 3.f || std::find(o.watch, o.watch + o.watches, v) != o.watch + o.watches)
                continue;
            if (std::any_of(std::begin(into), std::end(into), [&](const Fvector& t) { return sees(p, t); }))
                o.watch[o.watches++] = v;
        }
}

void bake()
{
    CTimer timer;
    timer.Start();
    const CLevelGraph& graph = ai().level_graph();
    const u32 n = graph.header().vertex_count();
    const float cell = graph.header().cell_size();
    calibrate(graph, n);
    Msg("[ai_house] links: 0 (%.0f,%.0f) 1 (%.0f,%.0f) 2 (%.0f,%.0f) 3 (%.0f,%.0f)", LINK_DIR[0].x, LINK_DIR[0].z, LINK_DIR[1].x, LINK_DIR[1].z, LINK_DIR[2].x, LINK_DIR[2].z,
        LINK_DIR[3].x, LINK_DIR[3].z);

    // 1. under a roof (bit 1), and walled round too (bit 2): in a house
    xr_vector<u8> flags(n, 0);
    std::atomic<u32> roofed{0};
    parallel_range(n, [&](u32 lo, u32 hi) {
        for (u32 v = lo; v < hi && !s_stop; ++v)
        {
            const Fvector p = graph.vertex_position(v);
            if (!Roofed(p))
                continue;
            ++roofed;
            u32 walled = 0;
            const Fvector eye = Fvector(p).add(Fvector{0.f, 1.3f, 0.f});
            for (u32 k = 0; k < 8; ++k)
            {
                const float a = float(k) * PI_DIV_4;
                walled += ray_range(eye, Fvector{_sin(a), 0.f, _cos(a)}, WALL_REACH) < WALL_REACH ? 1 : 0;
            }
            flags[v] = walled >= WALLED ? 3 : 1;
        }
    });
    if (s_stop)
        return;
    const float ms_flags = timer.GetElapsed_ms_total();

    // 2. the houses: connected runs of such vertices
    xr_vector<u32> house_of(n, u32(-1));
    xr_vector<SHouseBaked> houses;
    xr_vector<std::pair<u32, u16>> vertex_house;
    u32 phantoms = 0;
    for (u32 seed = 0; seed < n && !s_stop; ++seed)
    {
        if (flags[seed] != 3 || house_of[seed] != u32(-1))
            continue;
        xr_vector<u32> members{seed};
        house_of[seed] = u32(-2);
        for (u32 k = 0; k < members.size(); ++k)
        {
            const CLevelGraph::CVertex* vx = graph.vertex(members[k]);
            for (int i = 0; i < 4; ++i)
            {
                const u32 w = vx->link(i);
                if (graph.valid_vertex_id(w) && flags[w] == 3 && house_of[w] == u32(-1))
                {
                    house_of[w] = u32(-2);
                    members.push_back(w);
                }
            }
        }
        if (members.size() < MIN_VERTICES || members.size() > MAX_VERTICES || houses.size() >= 65535)
            continue;
        const u32 index = u32(houses.size());
        for (const u32 v : members)
            house_of[v] = index;

        SHouseBaked h;
        h.vertices = u32(members.size());
        for (const u32 v : members)
            h.origin.add(graph.vertex_position(v));
        h.origin.div(float(members.size()));
        for (const u32 v : members)
            h.radius = std::max(h.radius, graph.vertex_position(v).distance_to_xz(h.origin) + cell);

        // 3. its doors: where the map walks out of it; its windows: rays out of
        // its walls at chest and head height that reach the sky just past them
        xr_vector<SEdge> doors, windows;
        for (const u32 v : members)
        {
            const Fvector p = graph.vertex_position(v);
            const CLevelGraph::CVertex* vx = graph.vertex(v);
            for (u32 i = 0; i < 4; ++i)
            {
                const u32 w = vx->link(int(i));
                const Fvector wall = Fvector(p).mad(LINK_DIR[i], cell * .5f);
                if (graph.valid_vertex_id(w))
                {
                    if (flags[w] != 3 && leads_out(graph, flags, w))
                        doors.push_back({i, wall});
                    continue;
                }
                bool out = false;
                for (const float height : {1.1f, 1.6f})
                    for (const float turn : {0.f, .5f, -.5f})
                    {
                        if (out)
                            break;
                        const Fvector& d0 = LINK_DIR[i];
                        const Fvector dir{d0.x * _cos(turn) - d0.z * _sin(turn), 0.f, d0.x * _sin(turn) + d0.z * _cos(turn)};
                        const Fvector from = Fvector(p).add(Fvector{0.f, height, 0.f});
                        const float edge = cell * .5f;
                        const float reach = ray_range(from, dir, edge + SKY_PAST + 1.f);
                        for (float s = edge + .5f; s < std::min(reach - .2f, edge + SKY_PAST) && !out; s += .5f)
                        {
                            const Fvector at = Fvector(from).mad(dir, s);
                            // into another room of the house, not out: an inner wall's
                            // doorway seen through, then the outer door (06-10 21:12)
                            const u32 under = graph.vertex_id(Fvector(at).sub(Fvector{0.f, height, 0.f}));
                            if (graph.valid_vertex_id(under) && flags[under] == 3)
                                break;
                            out = !Roofed(at);
                        }
                    }
                if (out)
                    windows.push_back({i, wall});
            }
        }
        merge(doors, DOOR_MERGE, cell, true, h.openings);
        xr_vector<SOpening> found_windows;
        merge(windows, WINDOW_MERGE, cell, false, found_windows);
        for (SOpening& o : found_windows)
        {
            const bool at_door = std::any_of(h.openings.begin(), h.openings.end(), [&](const SOpening& d) { return d.door && d.mouth.distance_to(o.mouth) < SAME_OPENING; });
            if (at_door)
                continue;
            watch_points(graph, flags, o);
            if (!o.watches)
            {
                ++phantoms;
                continue;
            }
            h.openings.push_back(o);
        }
        for (SOpening& o : h.openings)
            if (o.door)
                watch_points(graph, flags, o);

        for (const u32 v : members)
            vertex_house.push_back({v, u16(index)});
        houses.push_back(std::move(h));
    }
    if (s_stop)
        return;
    std::sort(vertex_house.begin(), vertex_house.end());

    u32 doors = 0, windows = 0;
    for (const SHouseBaked& h : houses)
        for (const SOpening& o : h.openings)
            (o.door ? doors : windows) += 1;
    Msg("[ai_house] baked %u house(s), %u door(s), %u window(s) (%u phantom window(s) dropped) from %u vertices (%u roofed) in %.0f ms (flags %.0f)", u32(houses.size()), doors,
        windows, phantoms, n, roofed.load(), timer.GetElapsed_ms_total(), ms_flags);

    s_houses = std::move(houses);
    s_vertex_house = std::move(vertex_house);
}

void cache_name(string_path& out)
{
    const xrGUID& guid = ai().level_graph().header().guid();
    string64 name;
    xr_sprintf(name, "ai_houses_%016llx%016llx.bin", (unsigned long long)guid.g[0], (unsigned long long)guid.g[1]);
    FS.update_path(out, "$app_data_root$", name);
}

void cache_save()
{
    string_path fn;
    cache_name(fn);
    IWriter* w = FS.w_open(fn);
    if (!w)
    {
        Msg("![ai_house] could not write '%s'", fn);
        return;
    }
    w->w_u32(CACHE_MAGIC);
    w->w_u32(CACHE_VERSION);
    w->w_u32(ai().level_graph().header().vertex_count());
    w->w_u32(u32(sizeof(SOpening)));
    w->w_u32(u32(s_houses.size()));
    for (const SHouseBaked& h : s_houses)
    {
        w->w_fvector3(h.origin);
        w->w_float(h.radius);
        w->w_u32(h.vertices);
        w->w_u32(u32(h.openings.size()));
        for (const SOpening& o : h.openings)
            w->w(&o, sizeof(o));
    }
    w->w_u32(u32(s_vertex_house.size()));
    w->w(s_vertex_house.data(), u32(s_vertex_house.size() * sizeof(s_vertex_house[0])));
    FS.w_close(w);
}

bool cache_load()
{
    string_path fn;
    cache_name(fn);
    if (!FS.exist(fn))
        return false;
    IReader* r = FS.r_open(fn);
    if (!r)
        return false;
    bool ok = r->length() > 16 && r->r_u32() == CACHE_MAGIC && r->r_u32() == CACHE_VERSION && r->r_u32() == ai().level_graph().header().vertex_count() && r->r_u32() == sizeof(SOpening);
    if (ok)
    {
        xr_vector<SHouseBaked> houses(r->r_u32());
        for (SHouseBaked& h : houses)
        {
            r->r_fvector3(h.origin);
            h.radius = r->r_float();
            h.vertices = r->r_u32();
            h.openings.resize(r->r_u32());
            for (SOpening& o : h.openings)
                r->r(&o, sizeof(o));
        }
        xr_vector<std::pair<u32, u16>> vertex_house(r->r_u32());
        r->r(vertex_house.data(), u32(vertex_house.size() * sizeof(vertex_house[0])));
        ok = r->elapsed() == 0;
        if (ok)
        {
            s_houses = std::move(houses);
            s_vertex_house = std::move(vertex_house);
        }
    }
    FS.r_close(r);
    return ok;
}

const SHouseBaked* baked_house(int house) { return s_ready && house >= 0 && u32(house) < s_houses.size() ? &s_houses[house] : nullptr; }

// The baked house he is in -- in a doorway, the one it opens out of.
bool read_baked(const Fvector& spot, xr_vector<SOpening>& found, int* house, ERead& result)
{
    if (!s_ready)
        return false;
    const CLevelGraph& graph = ai().level_graph();
    const u32 v = graph.vertex_id(spot);
    int h = graph.valid_vertex_id(v) && _abs(graph.vertex_position(v).y - spot.y) < 2.f ? HouseAt(v) : -1;
    if (h < 0 && graph.valid_vertex_id(v))
        for (int i = 0; i < 4 && h < 0; ++i)
        {
            const u32 w = graph.vertex(v)->link(i);
            if (graph.valid_vertex_id(w))
                h = HouseAt(w);
        }
    if (h < 0)
    {
        result = Roofed(Fvector(spot).add(Fvector{0.f, 1.f, 0.f})) ? eShed : eOutdoors;
        return true;
    }
    found = s_houses[h].openings;
    if (house)
        *house = h;
    result = eIndoors;
    return true;
}
} // namespace

void AIHouse::BakeStart()
{
    BakeStop();
    s_houses.clear();
    s_vertex_house.clear();
    if (!ai().get_level_graph())
        return;
    CTimer timer;
    timer.Start();
    if (cache_load())
    {
        s_ready = true;
        Msg("[ai_house] %u house(s) from the cache in %.1f ms", u32(s_houses.size()), timer.GetElapsed_ms_total());
        return;
    }
    // the first load of this level: on a worker, the fights read houses live meanwhile
    s_stop = false;
    s_baker = std::thread([] {
        bake();
        if (s_stop)
            return;
        cache_save();
        s_ready = true;
    });
}

void AIHouse::BakeStop()
{
    s_stop = true;
    if (s_baker.joinable())
        s_baker.join();
    s_ready = false;
    s_stop = false;
}

bool AIHouse::Baked() { return s_ready; }

int AIHouse::HouseAt(u32 vertex)
{
    if (!s_ready)
        return -1;
    const auto I = std::lower_bound(s_vertex_house.begin(), s_vertex_house.end(), std::pair<u32, u16>{vertex, u16(0)});
    return I != s_vertex_house.end() && I->first == vertex ? int(I->second) : -1;
}

int AIHouse::HouseAt(const Fvector& p)
{
    if (!s_ready)
        return -1;
    const CLevelGraph& graph = ai().level_graph();
    const u32 v = graph.vertex_id(p);
    // a point off the map (a cover's exact spot, a mouth) or a floor up: no house
    if (!graph.valid_vertex_id(v) || _abs(graph.vertex_position(v).y - p.y) > 2.f)
        return -1;
    return HouseAt(v);
}

float AIHouse::HouseRadius(int house)
{
    const SHouseBaked* h = baked_house(house);
    return h ? h->radius : RADIUS;
}

Fvector AIHouse::HouseOrigin(int house)
{
    const SHouseBaked* h = baked_house(house);
    return h ? h->origin : Fvector{};
}

void AIHouse::Describe(const Fvector& p)
{
    const int house = HouseAt(p);
    const SHouseBaked* h = baked_house(house);
    if (!h)
    {
        Msg("[ai_house] (%.1f,%.1f,%.1f): %s", VPUSH(p), s_ready ? "in no baked house" : "houses not baked yet");
        return;
    }
    Msg("[ai_house] (%.1f,%.1f,%.1f): house #%d, middle (%.1f,%.1f,%.1f), %.1f m across, %u vertices, %u opening(s)", VPUSH(p), house, VPUSH(h->origin), h->radius * 2.f,
        h->vertices, u32(h->openings.size()));
    for (u32 k = 0; k < h->openings.size(); ++k)
    {
        const SOpening& o = h->openings[k];
        Msg("[ai_house]   #%u %s bearing %.0f, %.1f m wide, at (%.1f,%.1f,%.1f), %u place(s) outside see in", k, o.door ? "door" : "window", rad2deg(atan2f(o.dir.x, o.dir.z)),
            o.width, VPUSH(o.mouth), o.watches);
    }
}

#include "ai_debug_draw.h"

void AIHouse::DbgDraw(const Fvector& eye, float range)
{
    using namespace AIDebugDraw;
    if (!s_ready)
        return;
    const CLevelGraph& graph = ai().level_graph();
    for (const SHouseBaked& h : s_houses)
    {
        if (eye.distance_to_sqr(h.origin) > _sqr(range + h.radius))
            continue;
        Ring(h.origin, h.radius, color_rgba(160, 160, 160, 60));
        for (const SOpening& o : h.openings)
        {
            const u32 color = o.door ? color_rgba(60, 255, 120, 200) : color_rgba(80, 200, 255, 200);
            const Fvector side{o.dir.z, 0.f, -o.dir.x};
            Line(Fvector(o.mouth).mad(side, o.width * .5f), Fvector(o.mouth).mad(side, -o.width * .5f), color);
            Line(o.mouth, Fvector(o.mouth).mad(o.dir, .7f), color);
            for (u8 w = 0; w < o.watches; ++w)
                Cross(Fvector(graph.vertex_position(o.watch[w])).add(Fvector{0.f, .1f, 0.f}), .15f, subst_alpha(color, 120));
        }
    }
}

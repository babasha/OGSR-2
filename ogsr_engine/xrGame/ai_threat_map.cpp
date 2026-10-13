#include "StdAfx.h"
#include "ai_threat_map.h"

#include "Level.h"
#include "ai_space.h"
#include "level_graph.h"
#include "ai/stalker/ai_stalker.h"
#include "ai_position_query.h"
#include "ai_house.h"
#include "ai_debug_draw.h"

int ThreatMap::g_on = 1;

namespace
{
using namespace ThreatMap;

constexpr u32 STEP_EVERY = 250; // a map is stepped this often
constexpr float MAX_DT = 2.f; // a longer gap (a pause, a load) spreads no farther than this

// ⭐How fast what he may have done spreads: a diffusion constant, m²/s. 3 puts
// half of it within ~7 m after 8 s lost (when the search begins) and ~13 m after
// 30 s -- a man hiding, not one running flat out, who is the likelier.
constexpr float SPREAD = 3.f;
constexpr float BETA = .8f; // each pass moves this much of a vertex's mass to its four neighbours
constexpr u32 MAX_PASSES = 24;
constexpr u32 MAX_CELLS = 4096; // ~2000 m² of AI map: what spreads past it is "outside"
constexpr float MIN_MASS = 1e-5f; // a vertex is not added to the map for less

// the eyes
constexpr float CLEAR_RANGE = 35.f; // farther, a man in cover is not made out for sure
constexpr float CLEARED = .15f; // a vertex seen empty keeps this much of what it held
constexpr float LOOKED_AT = .03f; // a vertex with less than this of the likeliest one's share is not worth a ray
constexpr u32 RAYS = 64; // per map, per step
constexpr u32 BLOCKED_STEPS = 8; // a vertex no eye saw is not tried again for this many steps
constexpr float BODY = .9f; // the ray goes this high over the vertex: a crouched man's chest
constexpr float ARMS_LENGTH = 1.5f; // nearer than this, seen without a ray
constexpr u32 EYE_FRESH = 1000; // a man who has not reported for this long looks at nothing
constexpr u32 SEEN_HOLD = 1000; // just seen: what he stands on is not cleared

// the ears. A sound of his is where he was within a couple of metres when the
// man who heard it was near, and vaguer the farther that man was.
constexpr float SOUND_SIGMA = 2.f, SOUND_SIGMA_PER_M = .1f;
constexpr float SOUND_NEW = .3f; // a sound brings this much of the map round where it sounded...
constexpr float SOUND_FLOOR = .05f; // ...and what is far from it keeps this much of its own
constexpr float SAME_SOUND = 1.5f; // several men hear one sound: one event
constexpr u32 SAME_SOUND_TIME = 500;
constexpr u32 BLOB_CELLS = 1500;

// what is published
constexpr float KEEP_LIKELIEST = .6f; // the likeliest place stays while it holds this much of the best one's share
constexpr float NEAR_RADIUS = 3.f;
constexpr float SPREAD_SHARE = .8f;
constexpr u32 LIKELIEST = 12;
constexpr float LIKELIEST_APART = 4.f;
constexpr u32 DRAWN = 800;

enum : int
{
    eWall = -1, // no link that way
    eUnknown = -2, // not looked up yet
    eFull = -3, // a vertex not on the map, and none added now
};

struct SCell
{
    u32 vertex{};
    Fvector position{};
    int house{-1};
    int n[4]{eUnknown, eUnknown, eUnknown, eUnknown};
    u32 rayed{}; // the step it was last looked at with a ray
    bool blocked{}; // ...and no eye saw it then
};

struct SEvent
{
    enum EType : u8
    {
        eSeen,
        eHeard,
        eSeed,
    } type{};
    Fvector position{};
    u32 vertex{u32(-1)};
    float sigma{};
    u32 time{};
};

struct SEye
{
    Fvector position{};
    Fvector direction{}; // flat
    float cos_half{};
    float range{};
};

struct SPublished
{
    SEstimate estimate;
    xr_vector<std::pair<int, float>> houses;
    xr_vector<Fvector> likeliest;
    xr_vector<std::pair<Fvector, float>> cells; // for the overlay only
    float max_mass{};
};

struct SMap
{
    u16 enemy{u16(-1)};

    // ---- the main thread's
    xr_vector<SEvent> inbox;
    xr_map<u16, std::pair<u32, SEye>> eyes; // man -> when he reported, his eyes
    u32 last_step{}, next_step{};
    u32 seen_in{}; // the latest sighting handed in
    bool forget{};
    SPublished published;

    // ---- handed to the second thread with a step
    xr_vector<SEvent> events;
    xr_vector<SEye> looking;
    float dt{};
    u32 step_time{};
    bool draw{};

    // ---- the second thread's
    xr_vector<SCell> cells;
    xr_vector<float> mass, next;
    xr_unordered_map<u32, u32> index; // vertex -> cell
    float outside{};
    bool seeded{};
    u32 steps{};
    u32 info_time{}, seen_time{};
    u32 estimate_vertex{u32(-1)};
    SPublished out;
    u32 rays{};
    float ms{};
};

xr_map<u16, SMap> s_maps;
xr_vector<SMap*> s_work;
SStats s_stats;

float flat_sqr(const Fvector& a, const Fvector& b) { return _sqr(a.x - b.x) + _sqr(a.z - b.z); }

u32 vertex_of(const Fvector& p, u32 vertex)
{
    const CLevelGraph& graph = ai().level_graph();
    // a memory's vertex can lag its position: 📏06-10 23:14, the player moved
    // back into the izba by the autotest, remembered on the vertex of the
    // cellar 45 m off he had left -- and the map began there
    if (graph.valid_vertex_id(vertex))
    {
        const Fvector at = graph.vertex_position(vertex);
        if (flat_sqr(at, p) < _sqr(2.f) && _abs(at.y - p.y) < 3.f)
            return vertex;
    }
    vertex = graph.vertex_id(p);
    return graph.valid_vertex_id(vertex) ? vertex : graph.nearest_vertex_id(p);
}

// ---------------------------------------------------------------- the second thread

u32 add_cell(SMap& m, u32 vertex)
{
    const u32 i = u32(m.cells.size());
    m.index.emplace(vertex, i);
    SCell c;
    c.vertex = vertex;
    c.position = ai().level_graph().vertex_position(vertex);
    c.house = AIHouse::HouseAt(vertex);
    m.cells.push_back(c);
    m.mass.push_back(0.f);
    if (m.next.size() < m.cells.size())
        m.next.push_back(0.f);
    return i;
}

int find_cell(const SMap& m, u32 vertex)
{
    const auto I = m.index.find(vertex);
    return I != m.index.end() ? int(I->second) : -1;
}

void clear_map(SMap& m)
{
    m.cells.clear();
    m.mass.clear();
    m.next.clear();
    m.index.clear();
    m.outside = 0.f;
}

// Vertices round `e` by the AI map, weighted as a bell `sigma` wide; summing to 1.
void blob(const SEvent& e, xr_vector<std::pair<u32, float>>& out)
{
    out.clear();
    const CLevelGraph& graph = ai().level_graph();
    if (!graph.valid_vertex_id(e.vertex))
        return;
    if (e.sigma <= EPS_L)
    {
        out.emplace_back(e.vertex, 1.f);
        return;
    }
    const float reach_sqr = _sqr(2.5f * e.sigma);
    const float k = 1.f / (2.f * _sqr(e.sigma));
    xr_vector<u32> open{e.vertex};
    xr_unordered_map<u32, bool> seen;
    seen.emplace(e.vertex, true);
    float total = 0.f;
    for (u32 head = 0; head < open.size() && open.size() < BLOB_CELLS; ++head)
    {
        const u32 v = open[head];
        const float w = expf(-flat_sqr(graph.vertex_position(v), e.position) * k);
        out.emplace_back(v, w);
        total += w;
        const auto* vx = graph.vertex(v);
        for (int j = 0; j < 4; ++j)
        {
            const u32 n = vx->link(j);
            if (!graph.valid_vertex_id(n) || !seen.emplace(n, true).second)
                continue;
            if (flat_sqr(graph.vertex_position(n), e.position) <= reach_sqr)
                open.push_back(n);
        }
    }
    if (total > EPS_L)
        for (auto& [v, w] : out)
            w /= total;
}

void seed(SMap& m, const SEvent& e)
{
    clear_map(m);
    xr_vector<std::pair<u32, float>> b;
    blob(e, b);
    for (const auto& [v, w] : b)
        if (m.cells.size() < MAX_CELLS)
            m.mass[add_cell(m, v)] = w;
    m.seeded = !m.cells.empty();
    m.info_time = std::max(m.info_time, e.time);
}

void heard(SMap& m, const SEvent& e)
{
    m.info_time = std::max(m.info_time, e.time);
    // a little of everything moves to round where it sounded -- he may have
    // got farther than the spread thought -- then the sound's likelihood
    xr_vector<std::pair<u32, float>> b;
    blob(e, b);
    for (float& p : m.mass)
        p *= 1.f - SOUND_NEW;
    m.outside *= 1.f - SOUND_NEW;
    for (const auto& [v, w] : b)
    {
        int i = find_cell(m, v);
        if (i < 0 && m.cells.size() < MAX_CELLS)
            i = int(add_cell(m, v));
        if (i >= 0)
            m.mass[i] += SOUND_NEW * w;
        else
            m.outside += SOUND_NEW * w;
    }
    const float k = 1.f / (2.f * _sqr(e.sigma));
    for (u32 i = 0; i < m.cells.size(); ++i)
        m.mass[i] *= SOUND_FLOOR + (1.f - SOUND_FLOOR) * expf(-flat_sqr(m.cells[i].position, e.position) * k);
    m.outside *= SOUND_FLOOR;
}

// The neighbour of cell i its link j leads to: a cell, eWall, or eFull.
int neighbour(SMap& m, u32 i, int j, float share)
{
    int n = m.cells[i].n[j];
    if (n != eUnknown)
        return n;
    const CLevelGraph& graph = ai().level_graph();
    const u32 w = graph.vertex(m.cells[i].vertex)->link(j);
    if (!graph.valid_vertex_id(w))
        return m.cells[i].n[j] = eWall;
    n = find_cell(m, w);
    if (n < 0)
    {
        if (share < MIN_MASS || m.cells.size() >= MAX_CELLS)
            return eFull; // not kept: asked again next time
        n = int(add_cell(m, w));
    }
    return m.cells[i].n[j] = n;
}

// ⭐What he may have done in dt: each pass moves BETA of every vertex's mass
// evenly to its four neighbours; a wall sends its share back. A pass spreads it
// by about BETA*h²/2 per axis (h: the AI map's cell), so dt needs 2*SPREAD*dt
// of that.
void diffuse(SMap& m, float dt)
{
    if (dt <= 0.f)
        return;
    const float h = ai().level_graph().header().cell_size();
    const u32 passes = std::min(MAX_PASSES, u32(iFloor(2.f * SPREAD * dt / (BETA * h * h * .5f) + .5f)));
    for (u32 pass = 0; pass < passes; ++pass)
    {
        m.next.assign(m.cells.size(), 0.f);
        const u32 count = u32(m.cells.size()); // added this pass: moved on the next
        for (u32 i = 0; i < count; ++i)
        {
            const float p = m.mass[i];
            if (p < MIN_MASS * .01f)
            {
                m.next[i] += p;
                continue;
            }
            m.next[i] += p * (1.f - BETA);
            const float share = p * BETA * .25f;
            for (int j = 0; j < 4; ++j)
            {
                const int n = neighbour(m, i, j, share);
                if (n == eWall)
                    m.next[i] += share;
                else if (n == eFull)
                    m.outside += share;
                else
                    m.next[n] += share;
            }
        }
        m.mass.swap(m.next);
        m.mass.resize(m.cells.size(), 0.f);
    }
}

// Static geometry only, a collider on the stack: safe off the main thread
// (as in ai_position_query.cpp).
bool blocked(SMap& m, const Fvector& from, const Fvector& to)
{
    Fvector dir = Fvector().sub(to, from);
    const float length = dir.magnitude();
    if (length < EPS_L)
        return false;
    dir.div(length);
    ++m.rays;
    collide::rq_result hit;
    return !!Level().ObjectSpace.RayPick(from, dir, length - .2f, collide::rqtStatic, hit, nullptr);
}

// ⭐What the men look at and find empty. The likeliest vertices first, a
// budget of rays a step; one no eye saw waits a couple of seconds before it is
// tried again, or the vertices behind the walls of his house would take every
// ray, every step.
void clear(SMap& m)
{
    if (m.looking.empty() || m.cells.empty())
        return;
    float max_mass = 0.f;
    for (const float p : m.mass)
        max_mass = std::max(max_mass, p);
    const float worth = max_mass * LOOKED_AT;

    struct SCandidate
    {
        float mass;
        u32 cell;
        u32 eye;
    };
    xr_vector<SCandidate> candidates;
    for (u32 i = 0; i < m.cells.size(); ++i)
    {
        if (m.mass[i] < worth)
            continue;
        SCell& c = m.cells[i];
        if (c.blocked && m.steps < c.rayed + BLOCKED_STEPS)
            continue;
        const Fvector target = Fvector(c.position).add(Fvector{0.f, BODY, 0.f});
        u32 best = u32(-1);
        float best_sqr = flt_max;
        for (u32 k = 0; k < m.looking.size(); ++k)
        {
            const SEye& eye = m.looking[k];
            const float d_sqr = eye.position.distance_to_sqr(target);
            if (d_sqr > _sqr(eye.range) || d_sqr >= best_sqr)
                continue;
            Fvector way{target.x - eye.position.x, 0.f, target.z - eye.position.z};
            const float flat = way.magnitude();
            if (flat > ARMS_LENGTH && way.dotproduct(eye.direction) < eye.cos_half * flat)
                continue;
            best = k;
            best_sqr = d_sqr;
        }
        if (best == u32(-1))
            continue;
        if (best_sqr < _sqr(ARMS_LENGTH))
            m.mass[i] *= CLEARED;
        else
            candidates.push_back({m.mass[i], i, best});
    }
    if (candidates.size() > RAYS)
    {
        std::nth_element(candidates.begin(), candidates.begin() + RAYS, candidates.end(), [](const SCandidate& a, const SCandidate& b) { return a.mass > b.mass; });
        candidates.resize(RAYS);
    }
    for (const SCandidate& c : candidates)
    {
        SCell& cell = m.cells[c.cell];
        cell.rayed = m.steps;
        cell.blocked = blocked(m, m.looking[c.eye].position, Fvector(cell.position).add(Fvector{0.f, BODY, 0.f}));
        if (!cell.blocked)
            m.mass[c.cell] *= CLEARED;
    }
}

void normalize(SMap& m)
{
    double total = m.outside;
    for (const float p : m.mass)
        total += p;
    if (total < 1e-9)
    {
        // looked at everywhere he could be: he is nowhere near
        std::fill(m.mass.begin(), m.mass.end(), 0.f);
        m.outside = 1.f;
        return;
    }
    const float k = float(1.0 / total);
    for (float& p : m.mass)
        p *= k;
    m.outside *= k;
}

// The cap is near: what holds next to nothing goes to "outside".
void compact(SMap& m)
{
    if (m.cells.size() < MAX_CELLS * 9 / 10)
        return;
    xr_vector<SCell> cells;
    xr_vector<float> mass;
    cells.reserve(m.cells.size());
    mass.reserve(m.cells.size());
    for (u32 i = 0; i < m.cells.size(); ++i)
    {
        if (m.mass[i] < MIN_MASS)
        {
            m.outside += m.mass[i];
            continue;
        }
        SCell c = m.cells[i];
        for (int& n : c.n)
            if (n != eWall)
                n = eUnknown;
        cells.push_back(c);
        mass.push_back(m.mass[i]);
    }
    m.cells.swap(cells);
    m.mass.swap(mass);
    m.next.clear();
    m.index.clear();
    for (u32 i = 0; i < m.cells.size(); ++i)
        m.index.emplace(m.cells[i].vertex, i);
}

void publish(SMap& m)
{
    SPublished& out = m.out;
    out.houses.clear();
    out.likeliest.clear();
    out.cells.clear();
    out.max_mass = 0.f;
    SEstimate& e = out.estimate;
    e = {};
    if (!m.seeded || m.cells.empty())
        return;

    u32 best = 0;
    for (u32 i = 1; i < m.cells.size(); ++i)
        if (m.mass[i] > m.mass[best])
            best = i;
    out.max_mass = m.mass[best];
    // the likeliest place stays put while it is nearly as likely: no hopping
    // between two vertices of one room
    const int kept = find_cell(m, m.estimate_vertex);
    const u32 at = kept >= 0 && m.mass[kept] >= KEEP_LIKELIEST * m.mass[best] ? u32(kept) : best;
    m.estimate_vertex = m.cells[at].vertex;

    e.valid = true;
    e.position = m.cells[at].position;
    e.vertex = m.cells[at].vertex;
    e.outside = m.outside;
    e.info_time = m.info_time;
    e.seen_time = m.seen_time;

    // how near, how wide; and by house
    xr_vector<std::pair<float, float>> by_distance; // (distance², mass)
    by_distance.reserve(m.cells.size());
    xr_map<int, std::pair<float, u32>> houses; // house -> (mass, its likeliest cell)
    for (u32 i = 0; i < m.cells.size(); ++i)
    {
        const SCell& c = m.cells[i];
        const float d_sqr = flat_sqr(c.position, e.position);
        if (d_sqr < _sqr(NEAR_RADIUS) && _abs(c.position.y - e.position.y) < 2.f)
            e.near_mass += m.mass[i];
        by_distance.emplace_back(d_sqr, m.mass[i]);
        if (c.house >= 0)
        {
            auto& h = houses[c.house];
            if (h.first == 0.f || m.mass[i] > m.mass[h.second])
                h.second = i;
            h.first += m.mass[i];
        }
    }
    std::sort(by_distance.begin(), by_distance.end());
    const float want = SPREAD_SHARE * (1.f - m.outside);
    float sum = 0.f;
    for (const auto& [d_sqr, p] : by_distance)
    {
        sum += p;
        if (sum >= want)
        {
            e.spread = _sqrt(d_sqr);
            break;
        }
    }
    for (const auto& [house, h] : houses)
    {
        out.houses.emplace_back(house, h.first);
        if (h.first > e.house_mass)
        {
            e.house = house;
            e.house_mass = h.first;
            e.house_point = m.cells[h.second].position;
        }
    }

    // the likeliest places, apart
    xr_vector<u32> order(m.cells.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    const u32 top = std::min(u32(order.size()), m.draw ? std::max(DRAWN, 256u) : 256u);
    std::partial_sort(order.begin(), order.begin() + top, order.end(), [&m](u32 a, u32 b) { return m.mass[a] > m.mass[b]; });
    for (u32 k = 0; k < top && out.likeliest.size() < LIKELIEST; ++k)
    {
        const Fvector& p = m.cells[order[k]].position;
        if (m.mass[order[k]] < MIN_MASS)
            break;
        const bool apart = std::none_of(out.likeliest.begin(), out.likeliest.end(), [&p](const Fvector& q) { return flat_sqr(p, q) < _sqr(LIKELIEST_APART); });
        if (apart)
            out.likeliest.push_back(p);
    }
    if (m.draw)
        for (u32 k = 0; k < std::min(top, DRAWN); ++k)
            out.cells.emplace_back(m.cells[order[k]].position, m.mass[order[k]]);
}

void step(SMap& m)
{
    CTimer timer;
    timer.Start();
    m.rays = 0;
    ++m.steps;

    u32 seen = 0;
    for (const SEvent& e : m.events)
        switch (e.type)
        {
        case SEvent::eSeen:
            // he is where he was seen: the map is that vertex
            if (e.time < m.seen_time)
                break;
            clear_map(m);
            m.mass[add_cell(m, e.vertex)] = 1.f;
            m.seeded = true;
            m.seen_time = e.time;
            m.info_time = std::max(m.info_time, e.time);
            m.estimate_vertex = e.vertex;
            seen = e.time;
            break;
        case SEvent::eSeed:
            if (!m.seeded)
                seed(m, e);
            break;
        case SEvent::eHeard:
            if (!m.seeded)
                seed(m, e);
            else if (e.time > m.seen_time + SEEN_HOLD)
                heard(m, e);
            break;
        }
    m.events.clear();

    if (m.seeded)
    {
        // from the sighting on, not from the last step
        const float dt = seen ? std::clamp(float(int(m.step_time - seen)) / 1000.f, 0.f, m.dt) : m.dt;
        diffuse(m, dt);
        if (m.step_time > m.seen_time + SEEN_HOLD)
            clear(m);
        normalize(m);
        compact(m);
    }
    publish(m);
    m.ms = timer.GetElapsed_sec() * 1000.f;
}

struct SWorker
{
    void run()
    {
        if (!g_pGameLevel || g_pGameLevel->is_removing_objects())
            return;
        for (SMap* m : s_work)
            step(*m);
    }
} s_worker;

SMap* find(u16 enemy)
{
    const auto I = s_maps.find(enemy);
    return I != s_maps.end() && !I->second.forget ? &I->second : nullptr;
}

// The map of this enemy, a new one if none; null while one forgotten is still
// there (it goes at the next Frame, and the next report begins it anew).
SMap* get(u16 enemy)
{
    SMap& m = s_maps[enemy];
    if (m.forget)
        return nullptr;
    m.enemy = enemy;
    return &m;
}
} // namespace

bool ThreatMap::On() { return PositionQuery::g_ai_tactics_v2 && g_on; }

void ThreatMap::Seen(u16 enemy, const Fvector& position, u32 vertex, u32 time)
{
    SMap* map = On() ? get(enemy) : nullptr;
    if (!map || time <= map->seen_in)
        return;
    SMap& m = *map;
    m.seen_in = time;
    vertex = vertex_of(position, vertex);
    if (!ai().level_graph().valid_vertex_id(vertex))
        return;
    // the latest is enough: it collapses the map
    m.inbox.erase(std::remove_if(m.inbox.begin(), m.inbox.end(), [](const SEvent& e) { return e.type == SEvent::eSeen; }), m.inbox.end());
    m.inbox.push_back({SEvent::eSeen, position, vertex, 0.f, time});
    ++s_stats.seen;

    // the commander has it at once, not a step later
    SPublished& p = m.published;
    SEstimate& e = p.estimate;
    e.valid = true;
    e.position = ai().level_graph().vertex_position(vertex);
    e.vertex = vertex;
    e.near_mass = 1.f;
    e.spread = 0.f;
    e.outside = 0.f;
    e.info_time = std::max(e.info_time, time);
    e.seen_time = std::max(e.seen_time, time);
    e.house = AIHouse::HouseAt(vertex);
    e.house_mass = e.house >= 0 ? 1.f : 0.f;
    e.house_point = e.position;
    p.houses.clear();
    if (e.house >= 0)
        p.houses.emplace_back(e.house, 1.f);
    p.likeliest.assign(1, e.position);
}

void ThreatMap::Heard(u16 enemy, const Fvector& listener, const Fvector& position)
{
    SMap* m = On() ? find(enemy) : nullptr;
    if (!m)
        return;
    const u32 now = Device.dwTimeGlobal;
    if (now < m->seen_in + SEEN_HOLD)
        return; // in sight: a sound adds nothing
    const float sigma = SOUND_SIGMA + SOUND_SIGMA_PER_M * listener.distance_to(position);
    for (SEvent& e : m->inbox)
        if (e.type == SEvent::eHeard && now < e.time + SAME_SOUND_TIME && e.position.distance_to_sqr(position) < _sqr(SAME_SOUND))
        {
            e.sigma = std::min(e.sigma, sigma);
            return;
        }
    const u32 vertex = vertex_of(position, u32(-1));
    if (!ai().level_graph().valid_vertex_id(vertex))
        return;
    m->inbox.push_back({SEvent::eHeard, position, vertex, sigma, now});
    m->published.estimate.info_time = std::max(m->published.estimate.info_time, now);
    ++s_stats.heard;
}

void ThreatMap::Seed(u16 enemy, const Fvector& position, u32 vertex, float sigma, u32 time)
{
    SMap* map = On() ? get(enemy) : nullptr;
    if (!map || map->published.estimate.valid || !map->inbox.empty() || map->seen_in)
        return;
    SMap& m = *map;
    vertex = vertex_of(position, vertex);
    if (!ai().level_graph().valid_vertex_id(vertex))
        return;
    m.inbox.push_back({SEvent::eSeed, position, vertex, sigma, time});
    // the commander has somewhere to start from at once
    SEstimate& e = m.published.estimate;
    e.valid = true;
    e.position = ai().level_graph().vertex_position(vertex);
    e.vertex = vertex;
    e.spread = sigma;
    e.info_time = time;
    e.house = AIHouse::HouseAt(vertex);
    e.house_mass = e.house >= 0 ? .5f : 0.f;
    e.house_point = e.position;
    m.published.likeliest.assign(1, e.position);
}

void ThreatMap::Look(u16 enemy, const CAI_Stalker& npc)
{
    SMap* m = On() ? find(enemy) : nullptr;
    if (!m)
        return;
    SEye eye;
    eye.position = npc.eye_matrix.c;
    eye.direction.set(npc.eye_matrix.k.x, 0.f, npc.eye_matrix.k.z);
    eye.direction.normalize_safe();
    eye.cos_half = _cos(deg2rad(npc.ffGetFov()) * .5f);
    eye.range = std::min(npc.ffGetRange(), CLEAR_RANGE);
    m->eyes[u16(npc.ID())] = {Device.dwTimeGlobal, eye};
}

bool ThreatMap::Exists(u16 enemy) { return On() && find(enemy) && find(enemy)->published.estimate.valid; }

bool ThreatMap::Estimate(u16 enemy, SEstimate& out)
{
    const SMap* m = On() ? find(enemy) : nullptr;
    if (!m || !m->published.estimate.valid)
        return false;
    out = m->published.estimate;
    return true;
}

float ThreatMap::HouseMass(u16 enemy, int house)
{
    const SMap* m = On() ? find(enemy) : nullptr;
    if (!m || house < 0)
        return 0.f;
    for (const auto& [h, mass] : m->published.houses)
        if (h == house)
            return mass;
    return 0.f;
}

void ThreatMap::Likeliest(u16 enemy, xr_vector<Fvector>& out)
{
    out.clear();
    if (const SMap* m = On() ? find(enemy) : nullptr)
        out = m->published.likeliest;
}

void ThreatMap::Forget(u16 enemy)
{
    const auto I = s_maps.find(enemy);
    if (I != s_maps.end())
        I->second.forget = true;
}

void ThreatMap::Frame()
{
    // the last step, joined: what it found is what the main thread reads now
    for (SMap* m : s_work)
    {
        // a sighting or the seed came after this step was handed out: what the
        // main thread set at once stands until the next step folds it in --
        // else "where he is" would go back to before it for a step
        const bool behind = m->seen_in > m->out.estimate.seen_time || (!m->out.estimate.valid && m->published.estimate.valid);
        if (!behind)
            m->published = m->out;
        ++s_stats.steps;
        s_stats.rays += m->rays;
        s_stats.worker_ms += m->ms;
        s_stats.step_max_ms = std::max(s_stats.step_max_ms, m->ms);
        s_stats.cells_max = std::max(s_stats.cells_max, u32(m->cells.size()));
    }
    s_work.clear();
    for (auto I = s_maps.begin(); I != s_maps.end();)
        I = I->second.forget ? s_maps.erase(I) : std::next(I);
    if (!g_pGameLevel || !On())
        return;

    const u32 now = Device.dwTimeGlobal;
    for (auto& [enemy, m] : s_maps)
    {
        if (now < m.next_step || (!m.seeded && m.inbox.empty()))
            continue;
        m.dt = m.last_step ? std::min(float(now - m.last_step) / 1000.f, MAX_DT) : 0.f;
        m.step_time = now;
        m.last_step = now;
        m.next_step = now + STEP_EVERY;
        m.events.swap(m.inbox);
        m.inbox.clear();
        m.looking.clear();
        for (auto I = m.eyes.begin(); I != m.eyes.end();)
        {
            if (now > I->second.first + 5 * EYE_FRESH)
            {
                I = m.eyes.erase(I);
                continue;
            }
            if (now < I->second.first + EYE_FRESH)
                m.looking.push_back(I->second.second);
            ++I;
        }
        m.draw = AIDebugDraw::on(AIDebugDraw::eThreat);
        s_work.push_back(&m);
    }
    if (!s_work.empty())
        Device.add_to_seq_parallel(fastdelegate::MakeDelegate(&s_worker, &SWorker::run), "ai threat map");
}

void ThreatMap::Reset()
{
    Device.remove_from_seq_parallel(fastdelegate::MakeDelegate(&s_worker, &SWorker::run));
    s_work.clear();
    s_maps.clear();
}

void ThreatMap::DbgDraw(const Fvector& eye, float range)
{
    using namespace AIDebugDraw;
    if (!On())
        return;
    for (const auto& [enemy, m] : s_maps)
    {
        const SPublished& p = m.published;
        const SEstimate& e = p.estimate;
        if (!e.valid || eye.distance_to_sqr(e.position) > _sqr(range + e.spread))
            continue;
        // a bar on each likely vertex, as tall and as warm as it is likely
        const float top = p.max_mass > 0.f ? p.max_mass : 1.f;
        for (const auto& [at, mass] : p.cells)
        {
            const float rel = std::clamp(mass / top, 0.f, 1.f);
            const u32 color = color_rgba(255, u32(220.f * (1.f - rel)), 40, u32(90.f + 165.f * rel));
            Line(Fvector(at).add(Fvector{0.f, .05f, 0.f}), Fvector(at).add(Fvector{0.f, .1f + 1.4f * rel, 0.f}), color);
        }
        for (const Fvector& l : p.likeliest)
            Ring(Fvector(l).add(Fvector{0.f, .1f, 0.f}), .5f, color_rgba(255, 160, 0, 200));
        const u32 color = color_rgba(255, 60, 60, 255);
        Cross(Fvector(e.position).add(Fvector{0.f, 1.f, 0.f}), .6f, color);
        Text(Fvector(e.position).add(Fvector{0.f, 2.2f, 0.f}), color, 0, "threat: here %.0f%%, 80%% within %.1f m, gone %.0f%% | house #%d %.0f%% | seen %.1f s ago", 100.f * e.near_mass,
             e.spread, 100.f * e.outside, e.house, 100.f * e.house_mass, e.seen_time ? float(Device.dwTimeGlobal - e.seen_time) / 1000.f : -1.f);
    }
}

const ThreatMap::SStats& ThreatMap::Stats() { return s_stats; }

void ThreatMap::ResetStats() { s_stats = {}; }

void ThreatMap::Dump()
{
    const SStats& s = s_stats;
    Msg("[tm] ai_threat_map=%d | %u map(s) | steps %u, %.3f ms a step on average, %.3f ms most | rays %u | cells most %u | events: seen %u, heard %u", g_on, u32(s_maps.size()), s.steps,
        s.steps ? s.worker_ms / float(s.steps) : 0.f, s.step_max_ms, s.rays, s.cells_max, s.seen, s.heard);
}

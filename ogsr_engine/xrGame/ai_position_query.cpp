#include "StdAfx.h"
#include "ai_position_query.h"

#include "Level.h"
#include "ai_space.h"
#include "level_graph.h"
#include "cover_point.h"
#include "cover_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager.h"
#include "stalker_movement_restriction.h"
#include "ai_debug_draw.h"
#include "ai_house.h"

int PositionQuery::g_ai_tactics_v2 = 0;
float PositionQuery::g_budget_ms = 1.f;

namespace
{
using namespace PositionQuery;

constexpr u32 MAX_CANDIDATES = 64;
constexpr u32 RAY_ACCEPT = 3; // the rays stop once this many places passed them...
constexpr float MAX_RISE = 3.f; // a candidate this far above or below the man is another floor
constexpr float ROOF = 12.f; // something this close overhead: a roof
constexpr float LEAN = .5f; // how far a lean puts the gun aside
constexpr float FACE_WALL = 1.5f; // a wall this near, the way he must look: a wall in the face

// One candidate as scored. Lower is better throughout, as in the stock
// evaluators and the overlay that shows both.
struct SScored
{
    SPoint point;
    float cheap{}; // the tests without rays
    float score{}; // ...and with them
    char verdict{' '}; // ' ' passed; else what threw it out (the overlay's legend)
    bool stand{};
    bool rayed{};
};

struct SJob
{
    SRequest request;
    xr_vector<SScored> scored;
    enum EStage : u8
    {
        eCheap,
        eRays,
        eDone,
    } stage{eCheap};
    u32 next{}, accepted{}, looked{};
    SResult result;
    u32 issued_frame{};
    u32 done_frame{}; // set by the second thread when answered: the latency, apart from when it is collected
    u32 id{};
};

xr_map<u32, SJob*> s_jobs; // (npc << 8 | kind) -> the request waiting or answered
xr_vector<SJob*> s_work; // what the second thread works on this frame; main thread hands it over in Frame
SStats s_stats;
xr_map<u32, u32> s_last_asked; // (npc << 8 | kind) -> level time of the last ask
u32 s_next_id = 1;
float s_frame_ms = 0.f; // written by the second thread, read by the main thread a frame later
u32 s_frame_rays = 0;

u32 key(u16 npc, EKind kind) { return (u32(npc) << 8) | kind; }

float flat(const Fvector& a, const Fvector& b) { return _sqrt(_sqr(a.x - b.x) + _sqr(a.z - b.z)); }

// Static geometry only, through a collider of its own on the stack: safe off
// the main thread (CObjectSpace::_RayPick).
bool blocked(const Fvector& from, const Fvector& to, float slack)
{
    Fvector dir = Fvector().sub(to, from);
    const float length = dir.magnitude();
    const float range = length - slack;
    if (range <= 0.f)
        return false;
    dir.div(length);
    ++s_frame_rays;
    collide::rq_result hit;
    return !!Level().ObjectSpace.RayPick(from, dir, range, collide::rqtStatic, hit, nullptr);
}

bool roofed(const Fvector& p)
{
    ++s_frame_rays;
    collide::rq_result hit;
    return !!Level().ObjectSpace.RayPick(Fvector(p).add(Fvector{0.f, .7f, 0.f}), Fvector{0.f, 1.f, 0.f}, ROOF, collide::rqtStatic, hit, nullptr);
}

enum EView : u8
{
    eViewNone,
    eViewCrouch,
    eViewStand,
};

// What a man at `p` sees of `target`, crouched or standing, straight on or
// leaning aside (with room to lean). `gun`: from the muzzle's height, not the
// eyes' -- a place must let him shoot, not only look (06-10 16:00, the head
// over a fence whose gun was not).
EView view(const Fvector& p, const Fvector& target, float slack, bool gun)
{
    Fvector way = Fvector().sub(target, p);
    way.y = 0.f;
    const float distance = way.magnitude();
    if (distance < 1.f)
        return eViewCrouch;
    way.div(distance);
    const Fvector side{way.z, 0.f, -way.x};
    for (const auto [height, result] : {std::pair{gun ? .9f : 1.f, eViewCrouch}, std::pair{gun ? 1.4f : 1.6f, eViewStand}})
    {
        const Fvector eye = Fvector(p).add(Fvector{0.f, height, 0.f});
        if (!blocked(eye, target, slack))
            return result;
        for (const float sign : {1.f, -1.f})
        {
            const Fvector aside = Fvector(eye).mad(side, sign * LEAN);
            if (!blocked(eye, aside, 0.f) && !blocked(aside, target, slack))
                return result;
        }
    }
    return eViewNone;
}

float covered_from(const SPoint& point, const Fvector& from)
{
    float yaw, pitch;
    Fvector().sub(from, point.position).getHP(yaw, pitch);
    const CLevelGraph& graph = ai().level_graph();
    return std::min(graph.high_cover_in_direction(yaw, point.vertex), graph.low_cover_in_direction(yaw, point.vertex));
}

// ---------------------------------------------------------------- the tests

void cheap_tests(SJob& job)
{
    const SRequest& r = job.request;
    job.scored.clear();
    job.scored.reserve(r.candidates.size());
    const float current = flat(r.self, r.threat);

    for (const SPoint& point : r.candidates)
    {
        SScored s;
        s.point = point;
        const Fvector& p = point.position;
        const float travel = flat(p, r.self);
        float nearest_friend = flt_max;
        for (const Fvector& f : r.friends)
            nearest_friend = std::min(nearest_friend, flat(f, p));

        const auto out = [&s](char verdict) { s.verdict = verdict; };
        if (nearest_friend < r.spacing)
            out('n');
        else
            switch (r.kind)
            {
            case eCover: {
                const float distance = flat(p, r.threat);
                if (distance < r.near_ || distance > r.far_)
                {
                    out('d');
                    break;
                }
                // ⭐not past him on the way there (cover_evaluators.cpp, rx_facer)
                const Fvector way = Fvector().sub(p, r.self);
                const float length_sqr = way.square_magnitude();
                const float t = length_sqr > EPS_L ? std::clamp(Fvector().sub(r.threat, r.self).dotproduct(way) / length_sqr, 0.f, 1.f) : 0.f;
                const float closest = r.threat.distance_to(Fvector().mad(r.self, way, t));
                if (closest < 4.f && closest < current - 1.f)
                {
                    out('p');
                    break;
                }
                // the stock's two: how hidden from him the place is, and a wall
                // right towards him (open ground that way is worth less)
                Fvector to_threat = Fvector().sub(r.threat, p);
                const bool open_towards = !!ai().level_graph().neighbour_in_direction(to_threat, point.vertex);
                s.cheap = covered_from(point, r.threat) * 4.f + (open_towards ? 2.f : 0.f) + _abs(distance - r.ideal) * .05f + travel * .08f;
                break;
            }
            case eWatch: {
                const float distance = flat(p, r.focus);
                if (distance < r.near_ || distance > r.far_)
                {
                    out('d');
                    break;
                }
                // round the spot, apart: the nearer another man's bearing on
                // it, the worse -- the watchers' sectors as a cost, not slots
                const float bearing = atan2f(p.x - r.focus.x, p.z - r.focus.z);
                float closest_angle = PI;
                for (const Fvector& f : r.friends)
                    if (flat(f, r.focus) > 2.f)
                        closest_angle = std::min(closest_angle, angle_difference(bearing, atan2f(f.x - r.focus.x, f.z - r.focus.z)));
                const float crowding = closest_angle < deg2rad(45.f) ? (deg2rad(45.f) - closest_angle) * 4.f : 0.f;
                s.cheap = _abs(distance - r.ideal) * .15f + covered_from(point, r.focus) * 1.5f + travel * .03f + crowding + (point.cover ? 0.f : .75f);
                break;
            }
            case eFlank: {
                const float distance = flat(p, r.focus);
                if (distance < r.near_ || distance > r.far_)
                {
                    out('d');
                    break;
                }
                // off the squad's side: the bigger the angle at the spot between
                // the squad and the place, the better, a right angle at best;
                // apart from the other flankers as the watchers are
                const float bearing = atan2f(p.x - r.focus.x, p.z - r.focus.z);
                const float off_squad = angle_difference(bearing, atan2f(r.focus_dir.x, r.focus_dir.z));
                if (off_squad < deg2rad(40.f))
                {
                    out('s');
                    break;
                }
                float closest_angle = PI;
                for (const Fvector& f : r.friends)
                    if (flat(f, r.focus) > 2.f)
                        closest_angle = std::min(closest_angle, angle_difference(bearing, atan2f(f.x - r.focus.x, f.z - r.focus.z)));
                const float crowding = closest_angle < deg2rad(45.f) ? (deg2rad(45.f) - closest_angle) * 4.f : 0.f;
                s.cheap = _abs(off_squad - PI_DIV_2) * 1.5f + _abs(distance - r.ideal) * .1f + covered_from(point, r.focus) * 1.5f + travel * .03f + crowding + (point.cover ? 0.f : .5f);
                break;
            }
            case eDoor: {
                // the nearer the doorway the better, on the side he comes from
                const Fvector side{r.focus_dir.z, 0.f, -r.focus_dir.x};
                const float his_side = side.dotproduct(Fvector().sub(r.self, r.focus_floor));
                const float this_side = side.dotproduct(Fvector().sub(p, r.focus_floor));
                s.cheap = flat(p, r.focus_floor) + (his_side * this_side < 0.f ? 1.f : 0.f);
                break;
            }
            case eSweep: {
                const float distance = flat(p, r.focus);
                if (distance > r.far_)
                {
                    out('d');
                    break;
                }
                // where he would hide from us: hidden from where we came, near the
                // spot, not far to walk
                s.cheap = covered_from(point, r.threat) * 2.f + distance * .08f + travel * .04f;
                break;
            }
            case ePost: {
                Fvector away = Fvector().sub(p, r.focus_floor);
                away.y = 0.f;
                const float distance = away.magnitude();
                if (away.dotproduct(r.focus_dir) < r.front)
                {
                    out('f');
                    break;
                }
                if (distance < r.near_ || distance > r.far_)
                {
                    out('d');
                    break;
                }
                // as the siege scored them: about SIEGE_WATCH_MARK off, covered from the house
                s.cheap = _abs(distance - r.ideal) * .25f + covered_from(point, r.focus_floor) * 2.f + travel * .02f + (point.cover ? 0.f : .75f);
                break;
            }
            default: break;
            }
        s.score = s.cheap;
        job.scored.push_back(s);
    }
    std::sort(job.scored.begin(), job.scored.end(), [](const SScored& a, const SScored& b) {
        if ((a.verdict == ' ') != (b.verdict == ' '))
            return a.verdict == ' ';
        return a.cheap < b.cheap;
    });
    job.next = 0;
    job.stage = SJob::eRays;
}

// One candidate's rays. Returns false once there is nothing more to look at.
bool ray_tests(SJob& job)
{
    const SRequest& r = job.request;
    if (job.next >= job.scored.size() || job.accepted >= RAY_ACCEPT || job.looked >= r.looks)
        return false;
    SScored& s = job.scored[job.next++];
    if (s.verdict != ' ')
        return false; // sorted: the rest are thrown out already
    ++job.looked;
    s.rayed = true;
    const Fvector& p = s.point.position;

    // ⭐not under the roof of the house he is kept in (06-10 15:37, 15:51:
    // stalkers in the izba's entrance hall, a log wall from the player)
    if (r.house && (r.house_baked >= 0 ? AIHouse::HouseAt(s.point.vertex) == r.house_baked : flat(p, r.house_origin) < r.house_radius && roofed(p)))
    {
        s.verdict = 'i';
        return true;
    }

    switch (r.kind)
    {
    case eCover: {
        // hidden crouched from where he would shoot from...
        const Fvector threat_eye = Fvector(r.threat).add(Fvector{0.f, 1.5f, 0.f});
        if (!blocked(threat_eye, Fvector(p).add(Fvector{0.f, .9f, 0.f}), .3f))
        {
            s.verdict = 'e';
            return true;
        }
        // ...and able to look out at him: a cover that sees nothing even
        // standing or leaning is a wall in the face (06-10 01:10, the bandit
        // staring at a gate for a minute and a half)
        const EView v = view(p, Fvector(r.threat).add(Fvector{0.f, 1.f, 0.f}), std::min(3.f, flat(p, r.threat) * .3f), true);
        s.score = s.cheap + (v == eViewNone ? 4.f : v == eViewStand ? .3f : 0.f);
        s.stand = v == eViewStand;
        break;
    }
    case eWatch: {
        const Fvector target = Fvector(r.focus).add(Fvector{0.f, 1.f, 0.f});
        const EView v = view(p, target, std::min(3.f, flat(p, r.focus) * .3f), false);
        if (v == eViewNone)
        {
            // blind to it -- and if what blinds him is right at his face, not
            // even a place to stand and wait (the fallback in finish): 📏06-10
            // 18:55, watchers on the ring looking at a fence board from 0.5 m
            const Fvector eye = Fvector(p).add(Fvector{0.f, 1.5f, 0.f});
            Fvector way = Fvector().sub(target, eye);
            way.y = 0.f;
            way.normalize_safe();
            s.verdict = blocked(eye, Fvector(eye).mad(way, FACE_WALL), 0.f) ? 'w' : 'v';
            return true;
        }
        s.stand = v == eViewStand;
        s.score = s.cheap + (s.stand ? .3f : 0.f);
        break;
    }
    case eFlank: {
        const Fvector target = Fvector(r.focus).add(Fvector{0.f, 1.f, 0.f});
        const EView v = view(p, target, std::min(3.f, flat(p, r.focus) * .3f), true);
        if (v == eViewNone)
        {
            s.verdict = 'v';
            return true;
        }
        s.stand = v == eViewStand;
        s.score = s.cheap + (s.stand ? .3f : 0.f);
        break;
    }
    case eDoor: {
        // hidden from inside the room, and the doorway in sight: whoever comes
        // out comes out at arm's length (the siege's men beside the door)
        const Fvector eye = Fvector(p).add(Fvector{0.f, 1.5f, 0.f});
        if (!blocked(r.threat, eye, 0.f) || blocked(eye, r.focus, .2f))
        {
            s.verdict = 'v';
            return true;
        }
        s.stand = true;
        break;
    }
    case eSweep: s.stand = true; break;
    case ePost: {
        // in through it to anywhere in the room behind: by the window, a step
        // in, the middle (06-10 16:14)
        // ⭐Best: the opening itself, straight from the eyes, no lean -- as a man
        // holding a post looks, and as the autotest checks it. 📏06-10 22:05: a post
        // whose place saw only a point 2.7 m in, leaning, held 'his' window 90 s and
        // never had it in sight.
        {
            const float slack = .3f;
            const float heights[] = {r.eyes ? 1.f : .9f, r.eyes ? 1.6f : 1.4f};
            EView straight = eViewNone;
            for (u32 h = 0; h < 2 && straight == eViewNone; ++h)
                if (!blocked(Fvector(p).add(Fvector{0.f, heights[h], 0.f}), r.focus, slack))
                    straight = h == 0 ? eViewCrouch : eViewStand;
            if (straight != eViewNone)
            {
                s.stand = straight == eViewStand;
                s.score = s.cheap + (s.stand ? .2f : 0.f);
                break;
            }
        }
        const Fvector into[] = {r.focus, Fvector(r.focus).mad(r.focus_dir, -1.2f), Fvector(r.focus).mad(r.focus_dir, -2.7f)};
        EView best = eViewNone;
        for (const Fvector& target : into)
        {
            const EView v = view(p, target, .3f, !r.eyes);
            if (v == eViewCrouch)
            {
                best = v;
                break;
            }
            if (v != eViewNone)
                best = v;
        }
        if (best == eViewNone)
        {
            s.verdict = 'v';
            return true;
        }
        // only leaning, or only into the room behind: worse than straight in
        s.stand = best == eViewStand;
        s.score = s.cheap + 1.5f + (s.stand ? .2f : 0.f);
        return true; // a fallback: not counted towards RAY_ACCEPT, the straight ones are looked for on
    }
    default: break;
    }
    ++job.accepted;
    return true;
}

void finish(SJob& job)
{
    const SScored* best = nullptr;
    for (const SScored& s : job.scored)
        if (s.rayed && s.verdict == ' ' && (!best || s.score < best->score))
            best = &s;
    // a cover nobody is hidden in: the least bad of what was looked at, rather
    // than none -- the stock always found one too
    bool fallback = false;
    if (!best && job.request.kind == eCover)
        for (const SScored& s : job.scored)
            if (s.rayed && s.verdict == 'e')
            {
                best = &s;
                break;
            }
    // ⭐a watcher with nowhere that sees the spot: the best of the rest, standing.
    // 📏06-10 18:47: the spot is where he was last seen, INSIDE the izba; almost
    // nothing outside sees it through the walls, three asks in four came back
    // empty and the watchers stood where they were, faces to a fence. The stock
    // put them on their sector's ring regardless of view; so does this, but on
    // the best-placed point of the ring, not a fixed slot.
    if (!best && job.request.kind == eWatch)
        for (const SScored& s : job.scored)
            if (s.rayed && s.verdict == 'v')
            {
                best = &s;
                fallback = true;
                break;
            }
    job.result = {};
    // why, for the logs: the count of each verdict but "passed"
    {
        u32 count[128]{};
        for (const SScored& s : job.scored)
            ++count[u8(s.verdict) & 127];
        string32& why = job.result.why;
        for (u32 c = 33; c < 127; ++c)
            if (count[c] && xr_strlen(why) + 6 < sizeof(why))
                xr_sprintf(why + xr_strlen(why), sizeof(why) - xr_strlen(why), "%s%c%u", why[0] ? " " : "", char(c), count[c]);
    }
    if (best)
    {
        job.result.found = true;
        job.result.point = best->point;
        job.result.score = best->score;
        job.result.stand = best->stand || fallback;
    }
    job.done_frame = Device.dwFrame;
    job.stage = SJob::eDone;
}

// The second thread's share: as much as fits in the budget, the rest next frame.
struct SWorker
{
    void run()
    {
        if (!g_pGameLevel || g_pGameLevel->is_removing_objects())
            return;
        CTimer timer;
        timer.Start();
        s_frame_rays = 0;
        for (SJob* job : s_work)
        {
            if (job->stage == SJob::eCheap)
                cheap_tests(*job);
            while (job->stage == SJob::eRays)
            {
                if (!ray_tests(*job))
                    finish(*job);
                if (timer.GetElapsed_sec() * 1000.f > g_budget_ms)
                    break;
            }
            if (timer.GetElapsed_sec() * 1000.f > g_budget_ms)
                break;
        }
        s_frame_ms = timer.GetElapsed_sec() * 1000.f;
    }
} s_worker;

void debug_candidates(const SJob& job)
{
    if (!AIDebugDraw::on(AIDebugDraw::eCandidates))
        return;
    static constexpr u8 channel[eKindCount] = {AIDebugDraw::eChannelQueryCover, AIDebugDraw::eChannelQueryWatch, AIDebugDraw::eChannelQueryPost,
                                               AIDebugDraw::eChannelQueryFlank, AIDebugDraw::eChannelQueryDoor,  AIDebugDraw::eChannelQuerySweep};
    for (const SScored& s : job.scored)
    {
        // never looked at with rays: shown as such, '-'
        const char verdict = s.verdict != ' ' ? s.verdict : s.rayed ? ' ' : '-';
        AIDebugDraw::Candidate(job.request.npc, channel[job.request.kind], job.id, s.point.position, s.score, verdict);
    }
}
} // namespace

void PositionQuery::Issue(SRequest&& request)
{
    CTimer timer;
    timer.Start();
    const u32 k = key(request.npc, request.kind);
    auto I = s_jobs.find(k);
    SJob* job = I != s_jobs.end() ? I->second : (s_jobs[k] = xr_new<SJob>());
    *job = {};
    job->request = std::move(request);
    if (job->request.candidates.size() > MAX_CANDIDATES)
        job->request.candidates.resize(MAX_CANDIDATES);
    job->issued_frame = Device.dwFrame;
    job->id = s_next_id++;
    ++s_stats.issued;
    ++s_stats.issued_kind[job->request.kind];
    s_last_asked[key(job->request.npc, job->request.kind)] = Device.dwTimeGlobal;
    s_stats.main_ms += timer.GetElapsed_sec() * 1000.f;
}

bool PositionQuery::Poll(u16 npc, EKind kind, SResult& out)
{
    const auto I = s_jobs.find(key(npc, kind));
    if (I == s_jobs.end() || I->second->stage != SJob::eDone)
        return false;
    SJob* job = I->second;
    out = job->result;
    ++s_stats.answered;
    s_stats.found += out.found ? 1 : 0;
    s_stats.frames_waited_max = std::max(s_stats.frames_waited_max, job->done_frame - job->issued_frame);
    debug_candidates(*job);
    xr_delete(job);
    s_jobs.erase(I);
    return true;
}

bool PositionQuery::Pending(u16 npc, EKind kind)
{
    const auto I = s_jobs.find(key(npc, kind));
    return I != s_jobs.end() && I->second->stage != SJob::eDone;
}

void PositionQuery::Cancel(u16 npc)
{
    for (auto I = s_jobs.begin(); I != s_jobs.end();)
    {
        if ((I->first >> 8) == npc)
        {
            xr_delete(I->second);
            I = s_jobs.erase(I);
        }
        else
            ++I;
    }
}

void PositionQuery::Cancel(u16 npc, EKind kind)
{
    const auto I = s_jobs.find(key(npc, kind));
    if (I == s_jobs.end())
        return;
    xr_delete(I->second);
    s_jobs.erase(I);
}

void PositionQuery::GatherCovers(SRequest& r, CAI_Stalker& npc, const Fvector& center, float radius, u32 most, float prefer)
{
    CTimer timer;
    timer.Start();
    xr_vector<CCoverPoint*> nearest;
    ai().cover_manager().covers().nearest(center, radius, nearest);
    // the cap keeps the likeliest: those about `prefer` off -- a watcher's ring,
    // not the covers against the house wall
    std::sort(nearest.begin(), nearest.end(), [&center, prefer](const CCoverPoint* a, const CCoverPoint* b) {
        return _abs(flat(a->position(), center) - prefer) < _abs(flat(b->position(), center) - prefer);
    });
    const CStalkerMovementRestrictor restrictor(&npc, true, false);
    u32 taken = 0;
    for (const CCoverPoint* cover : nearest)
    {
        if (taken >= most || r.candidates.size() >= MAX_CANDIDATES)
            break;
        if (_abs(cover->position().y - npc.Position().y) > MAX_RISE)
            continue;
        if (!npc.movement().accessible(cover->level_vertex_id()) || !restrictor(cover))
            continue;
        r.candidates.push_back({cover->position(), cover->level_vertex_id(), cover});
        ++taken;
    }
    s_stats.main_ms += timer.GetElapsed_sec() * 1000.f;
}

void PositionQuery::GatherRings(SRequest& r, CAI_Stalker& npc, const Fvector& center, std::initializer_list<float> radii, u32 per_ring)
{
    CTimer timer;
    timer.Start();
    const CLevelGraph& graph = ai().level_graph();
    u32 ring = 0;
    for (const float radius : radii)
    {
        // each ring turned half a step from the last: no two candidates on one bearing
        for (u32 i = 0; i < per_ring && r.candidates.size() < MAX_CANDIDATES; ++i)
        {
            const float angle = PI_MUL_2 * (float(i) + (ring & 1 ? .5f : 0.f)) / float(per_ring);
            const Fvector at{center.x + radius * _sin(angle), center.y, center.z + radius * _cos(angle)};
            const u32 vertex = graph.vertex_id(at);
            if (!graph.valid_vertex_id(vertex) || !npc.movement().accessible(vertex))
                continue;
            const Fvector p = graph.vertex_position(vertex);
            if (_abs(p.y - npc.Position().y) > MAX_RISE)
                continue;
            r.candidates.push_back({p, vertex, nullptr});
        }
        ++ring;
    }
    s_stats.main_ms += timer.GetElapsed_sec() * 1000.f;
}

void PositionQuery::GatherPoint(SRequest& r, CAI_Stalker& npc, const Fvector& at)
{
    if (r.candidates.size() >= MAX_CANDIDATES)
        return;
    const CLevelGraph& graph = ai().level_graph();
    const u32 vertex = graph.vertex_id(at);
    if (!graph.valid_vertex_id(vertex) || !npc.movement().accessible(vertex))
        return;
    r.candidates.push_back({graph.vertex_position(vertex), vertex, nullptr});
}

void PositionQuery::Frame()
{
    // last frame's second-thread share, now joined
    if (!s_work.empty())
    {
        s_stats.worker_ms += s_frame_ms;
        s_stats.worker_frame_max_ms = std::max(s_stats.worker_frame_max_ms, s_frame_ms);
        s_stats.rays += s_frame_rays;
        s_frame_ms = 0.f;
        s_frame_rays = 0;
    }
    s_work.clear();
    if (!g_pGameLevel)
        return;
    // nobody came for these: the man died, went offline or stopped asking
    constexpr u32 FORGOTTEN = 600;
    for (auto I = s_jobs.begin(); I != s_jobs.end();)
    {
        if (Device.dwFrame > I->second->issued_frame + FORGOTTEN)
        {
            xr_delete(I->second);
            I = s_jobs.erase(I);
        }
        else
            ++I;
    }
    for (auto& [k, job] : s_jobs)
        if (job->stage != SJob::eDone)
            s_work.push_back(job);
    if (s_work.empty())
        return;
    // oldest first: a request carried over is not starved by new ones
    std::sort(s_work.begin(), s_work.end(), [](const SJob* a, const SJob* b) { return a->id < b->id; });
    Device.add_to_seq_parallel(fastdelegate::MakeDelegate(&s_worker, &SWorker::run), "ai position query");
}

void PositionQuery::Reset()
{
    Device.remove_from_seq_parallel(fastdelegate::MakeDelegate(&s_worker, &SWorker::run));
    s_work.clear();
    for (auto& [k, job] : s_jobs)
        xr_delete(job);
    s_jobs.clear();
    s_last_asked.clear();
}

u32 PositionQuery::LastAsked(u16 npc, EKind kind)
{
    const auto I = s_last_asked.find(key(npc, kind));
    return I != s_last_asked.end() ? I->second : 0;
}

const PositionQuery::SStats& PositionQuery::Stats() { return s_stats; }

void PositionQuery::ResetStats() { s_stats = {}; }

void PositionQuery::Dump()
{
    const SStats& s = s_stats;
    Msg("[pq] v2=%d budget %.2f ms | issued %u (cover %u, watch %u, post %u, flank %u, door %u, sweep %u), answered %u (%u found), waiting %u | rays %u | main %.2f ms total, second thread %.2f ms total, %.2f ms most in a frame | longest wait %u frames",
        g_ai_tactics_v2, g_budget_ms, s.issued, s.issued_kind[eCover], s.issued_kind[eWatch], s.issued_kind[ePost], s.issued_kind[eFlank], s.issued_kind[eDoor], s.issued_kind[eSweep], s.answered, s.found, u32(s_jobs.size()), s.rays, s.main_ms, s.worker_ms, s.worker_frame_max_ms, s.frames_waited_max);
}

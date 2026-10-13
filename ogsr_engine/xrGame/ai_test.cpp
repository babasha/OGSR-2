#include "StdAfx.h"
#include "ai_test.h"

#include "Level.h"
#include "Actor.h"
#include "Actor_Flags.h"
#include "xrServer.h"
#include "game_sv_single.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "ai/stalker/ai_stalker.h"
#include "InventoryOwner.h"
#include "relation_registry.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "enemy_manager.h"
#include "stalker_movement_manager.h"
#include "stalker_combat_actions.h"
#include "stalker_combat_planner.h"
#include "level_graph.h"
#include "game_level_cross_table.h"
#include "ai_space.h"
#include "ai_position_query.h"
#include "ai_house.h"
#include "ai_commander.h"
#include "ai_threat_map.h"
#include "game_graph.h"
#include "Hit.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "../Include/xrRender/Kinematics.h"

namespace
{
constexpr u32 SAMPLE_MS = 250; // the fight is looked at four times a second
constexpr u32 PROGRESS_MS = 5000; // a line in the log this often
constexpr u32 ONLINE_TIMEOUT = 20000; // the spawned men should be in the world well within this
constexpr u32 ONLINE_SETTLE = 1000; // ...and in it this long before they are set on him
constexpr float WATCH_RANGE = 35.f; // an opening looked at from farther is not kept
constexpr float NO_ROLE_RADIUS = 15.f; // the goal: nobody without a role farther from the house
constexpr float ALL_WATCHED_GOAL = 10.f; // the goal: every opening watched this soon after the siege began
constexpr float SLOW_FRAME_MS = 33.3f;
constexpr float WALL_REACH = .7f; // a wall this near the eyes the way he looks: "nose in the wall"

// 📏06-10: where every siege of the Cordon village izba began, its front room
constexpr LPCSTR IZBA_LEVEL = "l01_escape";
const Fvector IZBA_SPOT{-220.4f, -19.1f, -163.1f};
// ...and its one door (the user: "у избы 1 дверь"), as the siege's walks_out
// found it in five runs of 06-10 15:23-15:31: bearing -72, 0.8 m. Scored as a
// fact of the scenario, because the AI's own reading from the front room
// misses it (17:13-17:33: 2 openings read, no door).
const Fvector IZBA_DOOR{-222.3f, -17.9f, -159.0f};
constexpr float IZBA_DOOR_BEARING = -72.f;
constexpr float IZBA_DOOR_WIDTH = .8f;
constexpr float DOOR_REACH = 2.5f; // the first man in this near the door came in by it
// step 2, the threat map: "where he is" moving this far between two samples
// while nobody sees him is a jump -- a sound that carried the spot off
constexpr float SPOT_JUMP = 4.f;
constexpr u32 UNSEEN = 1000; // nobody's eyes on him for this long: unseen
// ...and slip=T: at T s of the fight he walks out of the izba by the AI map,
// quietly, to hide in another house -- where the search must go to find him
constexpr float SLIP_SPEED = 2.f; // a careful walk, m/s
constexpr float SLIP_MIN = 15.f, SLIP_MAX = 45.f; // the house he hides in is this far from the izba
constexpr u32 SLIP_NODES = 80000; // the route's search over the AI map gives up past this many vertices

SStalkerDbgOpening izba_door()
{
    const float a = deg2rad(IZBA_DOOR_BEARING);
    const Fvector dir{_sin(a), 0.f, _cos(a)};
    return {IZBA_DOOR, Fvector(IZBA_DOOR).mad(dir, -.3f), dir, IZBA_DOOR_WIDTH, true, false};
}

constexpr u32 ROLES = eStalkerDbgRoleWatch + 1;
constexpr LPCSTR ROLE_NAME[ROLES] = {"none", "searcher", "sudden", "suppress", "flank", "post", "watch"};

struct SNpc
{
    u16 id{};
    string64 name{};
    bool online{};
    bool dead{};
    float fight_s{}; // seconds with the player as his enemy
    float role_s[ROLES]{}; // seconds in each role
    float no_role_far_s{}; // seconds standing with no role farther than NO_ROLE_RADIUS
    float worst_far{}; // ...and the farthest he stood so
    Fvector last_position{}; // at the previous sample: standing, or on his way
    bool placed{};
};

struct SOpeningTrack
{
    Fvector mouth{};
    bool door{}, blind{};
    bool scenario{}; // a fact of the scenario, not the AI's reading
    float first{-1.f}; // first watched, s of the run
    float watched_s{};
};

struct SRun
{
    enum EPhase : u8
    {
        eIdle,
        eSpawning,
        eRunning,
        eBetweenRuns, // the last run's men going away; the next starts at resume_time
    };
    EPhase phase{eIdle};
    u32 resume_time{};

    // as asked
    u32 runs{1};
    int v2{-1}; // ai_tactics_v2 for the run; -1 = as it is
    int tm{-1}; // ai_threat_map for the run; -1 = as it is
    u32 count{10};
    float seconds{90.f};
    string64 section{"esc_stalker_respawn_2"};
    float bearing{108.f}, dist{35.f};
    bool keep{};
    bool spot_given{};
    Fvector spot{IZBA_SPOT};
    shared_str level;

    u32 spawn_time{}, online_time{}, engage_time{};
    u32 next_sample{}, last_sample{}, next_progress{};
    xr_vector<SNpc> npcs;

    // the siege, as the AI reads the house
    u32 siege_start{};
    float t_siege{-1.f};
    xr_vector<SOpeningTrack> openings;
    float t_all_watched{-1.f};
    float all_watched_s{};
    float siege_s{}; // seconds with a siege on

    // the first man in
    float t_entry{-1.f};
    string64 entry_who{};
    int entry_opening{-1};
    bool entry_door{}, entry_after_ring{};
    Fvector entry_at{};

    float t_first_sight{-1.f};

    // step 2: where the fight thinks he is, against where he is, while unseen
    xr_vector<float> spot_error;
    u32 spot_jumps{};
    bool spot_known{};
    Fvector last_spot{};

    // slip=T
    float slip_at{-1.f}; // s of the fight; <0 off
    bool slip_to_given{};
    Fvector slip_to{};
    xr_vector<Fvector> slip_path;
    u32 slip_next{};
    Fvector slip_at_pos{}; // where the walk has him: the transform set lands a frame late, so not read back
    float t_slip{-1.f}, t_hidden{-1.f}, t_slip_seen{-1.f}, t_found{-1.f};
    float slip_closest{flt_max}; // the nearest any man came to him once hidden

    // frames, while the fight runs
    double frame_sum{};
    u32 frames{}, slow_frames{};
    float frame_max{};

    // kill=T: what a death costs the frames around it
    float kill_every{-1.f}, kill_from{10.f};
    u32 next_kill{};
    struct SDeath
    {
        u32 time{};
        string64 who{};
        bool by_player{}, reported{};
    };
    xr_vector<SDeath> deaths;
    static constexpr u32 RING = 1024; // ~17 s at 60 fps: the window around a death is 4
    u32 ring_time[RING]{};
    float ring_ms[RING]{};
    u32 ring_n{};

    // ⭐step 3, the places themselves: standing with a wall in his face, or a
    // place chosen under the roof of the house he besieges (not the man going in)
    float wall_s{}, dest_inside_s{};
    string128 wall_who{}, dest_inside_who{}; // the last man so, with his role and action

    // the last sample, for the progress line
    u32 last_alive{}, last_fight{}, last_sees{}, last_inside{}, last_watched{}, last_far{};
    u32 last_roles[ROLES]{};
};

SRun s_run;

// ⭐A series. 📏06-10 17:13-17:33, four runs of the same fight: the siege began
// at t=0 three times and at t=72 once; the first man went in at 15, 68, never,
// 67 s. One run says little; what a change does shows over several.
struct SResult
{
    float ring_after{-1.f}; // every opening watched, s after the siege began; -1 never
    float idle_far_s{}; // man-seconds standing with no role beyond NO_ROLE_RADIUS
    float t_siege{-1.f};
    float t_entry{-1.f};
    bool entry_door{}, entry_after_ring{};
    u32 passed{};
    float frame_avg{};
    float wall_s{}, dest_inside_s{};
    // the position query's cost over the run (ai_tactics_v2)
    u32 pq_answered{};
    float pq_main_ms_per_s{}, pq_worker_frame_max_ms{};
    // step 2
    float spot_error_median{-1.f};
    u32 spot_jumps{};
    float tm_step_max_ms{};
    // slip=T
    bool slipped{}, seen_slipping{};
    float found_after{-1.f}; // s after he hid; -1 never
    float slip_closest{-1.f};
};
struct SSeries
{
    bool active{};
    bool god_was{}; // as it was before the first run: restore() puts it back
    int v2_was{}; // ...and ai_tactics_v2
    int tm_was{}; // ...and ai_threat_map
    xr_vector<SResult> results;
};
SSeries s_series;

float t_rel(u32 time) { return float(int(time - s_run.engage_time)) / 1000.f; }

CAI_Stalker* stalker(u16 id) { return smart_cast<CAI_Stalker*>(Level().Objects.net_Find(id)); }

float flat_distance(const Fvector& a, const Fvector& b) { return _sqrt(_sqr(a.x - b.x) + _sqr(a.z - b.z)); }

// Is this man keeping this opening: near enough, outside in front of it, facing
// it within his eye cone, and nothing static between his eyes and a point just
// inside it. Bushes do not count -- he sees a man through them, if late.
bool watches(CAI_Stalker& npc, const SStalkerDbgOpening& o)
{
    const Fvector eye = npc.eye_matrix.c;
    if (Fvector().sub(eye, o.mouth).dotproduct(o.dir) <= 0.f)
        return false; // from inside the house, or behind its wall
    Fvector to = Fvector().sub(o.inside, eye);
    const float range = to.magnitude();
    if (range > WATCH_RANGE || range < EPS_L)
        return false;

    Fvector look, flat{to.x, 0.f, to.z};
    look.setHP(-npc.movement().head_orientation().current.yaw, 0.f);
    flat.normalize_safe();
    if (rad2deg(acosf(std::clamp(look.dotproduct(flat), -1.f, 1.f))) > npc.eye_fov * .5f)
        return false;

    to.div(range);
    collide::rq_result hit;
    return !Level().ObjectSpace.RayPick(eye, to, range - .3f, collide::rqtStatic, hit, nullptr);
}

void restore()
{
    if (s_series.active)
    {
        psActorFlags.set(AF_GODMODE, s_series.god_was);
        PositionQuery::g_ai_tactics_v2 = s_series.v2_was;
        ThreatMap::g_on = s_series.tm_was;
    }
    s_series = {};
    s_run = {};
}

void remove_npcs()
{
    auto game = smart_cast<game_sv_Single*>(Level().Server ? Level().Server->game : nullptr);
    if (!game || !ai().get_alife())
        return;
    u32 removed = 0;
    for (const SNpc& npc : s_run.npcs)
    {
        CSE_ALifeDynamicObject* object = ai().alife().objects().object(npc.id, true);
        if (!object)
            continue;
        // as alife():release does it: an object in the world is destroyed by the
        // world, one out of it released by ALife
        if (object->m_bOnline)
        {
            if (CGameObject* game_object = smart_cast<CGameObject*>(Level().Objects.net_Find(npc.id)))
                game_object->DestroyObject();
        }
        else
            game->alife().release(object, true);
        ++removed;
    }
    Msg("[ai_test] %u spawned stalker(s) removed", removed);
}

void progress_line(u32 now)
{
    const SRun& r = s_run;
    Msg("[ai_test] t=%5.1f alive %u fight %u sees %u | roles: post %u watch %u search %u supp %u flank %u none %u (beyond %.0f m: %u) | openings %u/%u watched%s | inside %u | frame avg %.1f max %.1f ms",
        t_rel(now), r.last_alive, r.last_fight, r.last_sees, r.last_roles[eStalkerDbgRolePost], r.last_roles[eStalkerDbgRoleWatch], r.last_roles[eStalkerDbgRoleSearcher],
        r.last_roles[eStalkerDbgRoleSuppressor], r.last_roles[eStalkerDbgRoleFlanker], r.last_roles[eStalkerDbgRoleNone], NO_ROLE_RADIUS, r.last_far, r.last_watched,
        u32(r.openings.size()), r.t_siege < 0.f ? " (no siege yet)" : "", r.last_inside, r.frames ? float(r.frame_sum / r.frames) : 0.f, r.frame_max);
}

SResult report(LPCSTR why)
{
    SRun& r = s_run;
    SResult result;
    const u32 now = Device.dwTimeGlobal;
    const float ran = r.engage_time ? t_rel(now) : 0.f;
    Msg("[ai_test] ===== siege_izba run %u of %u (%s, ai_tactics_v2 %d): %u x '%s', %.1f s of %.0f =====", u32(s_series.results.size()) + 1, r.runs, why, PositionQuery::g_ai_tactics_v2, u32(r.npcs.size()), r.section, ran,
        r.seconds);
    if (!r.engage_time)
    {
        Msg("[ai_test] the fight never started");
        return result;
    }

    u32 doors = 0, blind = 0, read = 0;
    for (const SOpeningTrack& o : r.openings)
    {
        read += o.scenario ? 0 : 1;
        doors += !o.scenario && o.door ? 1 : 0;
        blind += o.blind ? 1 : 0;
    }
    if (r.t_siege < 0.f)
        Msg("[ai_test] siege: never began -- nobody read the player as holed up in a house");
    else
        Msg("[ai_test] siege: began t=%.1f s | %u opening(s) as the AI read them: %u door(s), %u struck off as blind", r.t_siege, read, doors, blind);
    const u32 first_read = !r.openings.empty() && r.openings[0].scenario ? 1 : 0; // the scenario's door goes first
    for (u32 k = 0; k < r.openings.size(); ++k)
    {
        const SOpeningTrack& o = r.openings[k];
        string64 first = "never";
        if (o.first >= 0.f)
            xr_sprintf(first, "t=%.1f (+%.1f s)", o.first, o.first - r.t_siege);
        Msg("[ai_test]   %s %-6s (%.1f,%.1f,%.1f)%s: first watched %s, watched %.0f%% of the siege", o.scenario ? "scenario" : make_string("AI #%u", k - first_read).c_str(),
            o.door ? "door" : "window", VPUSH(o.mouth), o.blind ? " blind" : "", first, r.siege_s > 0.f ? 100.f * o.watched_s / r.siege_s : 0.f);
    }

    u32 passed = 0;
    // 1. every opening under watch, soon
    {
        const float after = r.t_all_watched >= 0.f ? r.t_all_watched - r.t_siege : -1.f;
        const bool pass = after >= 0.f && after <= ALL_WATCHED_GOAL;
        result.ring_after = after;
        result.t_siege = r.t_siege;
        passed += pass ? 1 : 0;
        if (after >= 0.f)
            Msg("[ai_test] [%s] all openings under watch: first at t=%.1f, %.1f s after the siege began (goal <= %.0f s); all held %.0f%% of the siege", pass ? "PASS" : "FAIL",
                r.t_all_watched, after, ALL_WATCHED_GOAL, r.siege_s > 0.f ? 100.f * r.all_watched_s / r.siege_s : 0.f);
        else
            Msg("[ai_test] [FAIL] all openings under watch: never (goal <= %.0f s after the siege began)", ALL_WATCHED_GOAL);
    }
    // 2. nobody idle far off
    {
        float man_s = 0.f, worst = 0.f;
        LPCSTR worst_name = "-";
        for (const SNpc& npc : r.npcs)
        {
            man_s += npc.no_role_far_s;
            if (npc.worst_far > worst)
            {
                worst = npc.worst_far;
                worst_name = npc.name;
            }
        }
        const bool pass = man_s < SAMPLE_MS / 1000.f; // not one sample
        result.idle_far_s = man_s;
        passed += pass ? 1 : 0;
        Msg("[ai_test] [%s] nobody standing without a role beyond %.0f m of the house: %.1f man-seconds; farthest '%s' at %.1f m", pass ? "PASS" : "FAIL", NO_ROLE_RADIUS, man_s, worst_name,
            worst);
    }
    // 3. in through the door, once it is surrounded
    {
        if (r.t_entry < 0.f)
            Msg("[ai_test] [FAIL] first man in: nobody went in");
        else
        {
            const bool pass = r.entry_door && r.entry_after_ring;
            result.t_entry = r.t_entry;
            result.entry_door = r.entry_door;
            result.entry_after_ring = r.entry_after_ring;
            passed += pass ? 1 : 0;
            Msg("[ai_test] [%s] first man in: '%s' at t=%.1f %s at (%.1f,%.1f,%.1f); every opening watched before: %s", pass ? "PASS" : "FAIL", r.entry_who, r.t_entry,
                r.entry_door ? "by the door" : "NOT by the door", VPUSH(r.entry_at), r.entry_after_ring ? "yes" : "no");
        }
    }
    result.passed = passed;
    result.frame_avg = r.frames ? float(r.frame_sum / r.frames) : 0.f;

    // step 3: the places, and what choosing them cost
    result.wall_s = r.wall_s;
    result.dest_inside_s = r.dest_inside_s;
    Msg("[ai_test] [%s] places: %.1f man-seconds standing with a wall in the face%s%s%s | %.1f going to a place inside the house%s%s%s", r.wall_s + r.dest_inside_s < SAMPLE_MS / 1000.f ? "PASS" : "FAIL",
        r.wall_s, r.wall_who[0] ? " (last '" : "", r.wall_who, r.wall_who[0] ? "')" : "", r.dest_inside_s, r.dest_inside_who[0] ? " (last '" : "", r.dest_inside_who,
        r.dest_inside_who[0] ? "')" : "");
    const PositionQuery::SStats& pq = PositionQuery::Stats();
    result.pq_answered = pq.answered;
    result.pq_main_ms_per_s = ran > 0.f ? pq.main_ms / ran : 0.f;
    result.pq_worker_frame_max_ms = pq.worker_frame_max_ms;
    Msg("[ai_test] position query (ai_tactics_v2 %d): %u asked (cover %u, watch %u, post %u), %u answered (%u found), %u rays | main thread %.2f ms a second, second thread %.2f ms a second, %.2f ms most in a frame | longest wait %u frames",
        PositionQuery::g_ai_tactics_v2, pq.issued, pq.issued_kind[PositionQuery::eCover], pq.issued_kind[PositionQuery::eWatch], pq.issued_kind[PositionQuery::ePost], pq.answered, pq.found, pq.rays, result.pq_main_ms_per_s, ran > 0.f ? pq.worker_ms / ran : 0.f, pq.worker_frame_max_ms,
        pq.frames_waited_max);

    {
        xr_vector<float> errors = r.spot_error;
        std::sort(errors.begin(), errors.end());
        result.spot_error_median = errors.empty() ? -1.f : errors[errors.size() / 2];
        result.spot_jumps = r.spot_jumps;
        Msg("[ai_test] where he is thought to be (ai_threat_map %d), while unseen: %.0f s, %.1f m off him by the median, %.1f m at the 90th percentile, %.1f m at most | %u jump(s) over %.0f m",
            ThreatMap::g_on, float(errors.size()) * SAMPLE_MS / 1000.f, result.spot_error_median, errors.empty() ? -1.f : errors[errors.size() * 9 / 10],
            errors.empty() ? -1.f : errors.back(), r.spot_jumps, SPOT_JUMP);
        if (r.t_slip >= 0.f)
        {
            result.slipped = true;
            result.seen_slipping = r.t_slip_seen >= 0.f && (r.t_hidden < 0.f || r.t_slip_seen < r.t_hidden);
            result.found_after = r.t_found >= 0.f ? r.t_found - r.t_hidden : -1.f;
            result.slip_closest = r.slip_closest < flt_max ? r.slip_closest : -1.f;
            string32 found = "never";
            if (r.t_found >= 0.f)
                xr_sprintf(found, "%.1f s after", result.found_after);
            Msg("[ai_test] slip: out at t=%.1f, hidden at t=%.1f %.1f m off; %s; found where he hid: %s; nearest a man came: %.1f m", r.t_slip, r.t_hidden,
                r.slip_path.empty() ? 0.f : flat_distance(r.slip_path.back(), r.spot), result.seen_slipping ? "SEEN on the way" : "unseen on the way", found, result.slip_closest);
        }
        const ThreatMap::SStats& tm = ThreatMap::Stats();
        result.tm_step_max_ms = tm.step_max_ms;
        Msg("[ai_test] threat map: %u steps, %.3f ms a step on the second thread, %.3f ms most | %u rays (%.0f a second) | %u cells at most | %u sightings, %u sounds", tm.steps,
            tm.steps ? tm.worker_ms / float(tm.steps) : 0.f, tm.step_max_ms, tm.rays, ran > 0.f ? float(tm.rays) / ran : 0.f, tm.cells_max, tm.seen, tm.heard);
    }

    float role_s[ROLES]{}, fight_s = 0.f;
    u32 dead = 0;
    for (const SNpc& npc : r.npcs)
    {
        for (u32 i = 0; i < ROLES; ++i)
            role_s[i] += npc.role_s[i];
        fight_s += npc.fight_s;
        dead += npc.dead ? 1 : 0;
    }
    Msg("[ai_test] roles, man-seconds: none %.0f | searcher %.0f | sudden %.0f | suppress %.0f | flank %.0f | post %.0f | watch %.0f", role_s[0], role_s[1], role_s[2],
        role_s[3], role_s[4], role_s[5], role_s[6]);
    string32 sight = "never";
    if (r.t_first_sight >= 0.f)
        xr_sprintf(sight, "t=%.1f", r.t_first_sight);
    Msg("[ai_test] fight: %.1f men in it on average of %u, first sight of the player %s, %u dead", ran > 0.f ? fight_s / ran : 0.f, u32(r.npcs.size()), sight, dead);
    for (const SNpc& npc : r.npcs)
    {
        u32 top = 0;
        for (u32 i = 1; i < ROLES; ++i)
            if (npc.role_s[i] > npc.role_s[top])
                top = i;
        Msg("[ai_test]   %5u '%s': in the fight %.0f s, mostly %s (%.0f s), no role beyond %.0f m %.1f s (farthest %.1f m)%s", npc.id, npc.name, npc.fight_s, ROLE_NAME[top],
            npc.role_s[top], NO_ROLE_RADIUS, npc.no_role_far_s, npc.worst_far, npc.dead ? ", dead" : !npc.online ? ", never came online" : "");
    }
    Msg("[ai_test] frames: avg %.1f ms, max %.1f ms, %u over %.0f ms of %u", r.frames ? float(r.frame_sum / r.frames) : 0.f, r.frame_max, r.slow_frames, SLOW_FRAME_MS, r.frames);
    Msg("[ai_test] RESULT: %u/3 PASS", passed);
    return result;
}

void summary()
{
    const auto& results = s_series.results;
    const u32 n = u32(results.size());
    u32 ring = 0, idle = 0, door = 0, all = 0;
    xr_vector<float> ring_after, idle_s, entry, frame, wall, inside, pq_main, pq_worker, spot_error, tm_ms;
    u32 jumps = 0;
    for (const SResult& x : results)
    {
        if (x.spot_error_median >= 0.f)
            spot_error.push_back(x.spot_error_median);
        tm_ms.push_back(x.tm_step_max_ms);
        jumps += x.spot_jumps;
    }
    for (const SResult& x : results)
    {
        wall.push_back(x.wall_s);
        inside.push_back(x.dest_inside_s);
        pq_main.push_back(x.pq_main_ms_per_s);
        pq_worker.push_back(x.pq_worker_frame_max_ms);
    }
    for (const SResult& x : results)
    {
        ring += x.ring_after >= 0.f && x.ring_after <= ALL_WATCHED_GOAL ? 1 : 0;
        idle += x.idle_far_s < SAMPLE_MS / 1000.f ? 1 : 0;
        door += x.t_entry >= 0.f && x.entry_door && x.entry_after_ring ? 1 : 0;
        all += x.passed == 3 ? 1 : 0;
        if (x.ring_after >= 0.f)
            ring_after.push_back(x.ring_after);
        idle_s.push_back(x.idle_far_s);
        if (x.t_entry >= 0.f)
            entry.push_back(x.t_entry);
        frame.push_back(x.frame_avg);
    }
    const auto median = [](xr_vector<float>& v) {
        if (v.empty())
            return -1.f;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    Msg("[ai_test] ===== SERIES of %u runs =====", n);
    Msg("[ai_test] every opening watched <= %.0f s: %u/%u runs (median %.1f s, in %u runs at all)", ALL_WATCHED_GOAL, ring, n, median(ring_after), u32(ring_after.size()));
    Msg("[ai_test] nobody idle beyond %.0f m: %u/%u runs (median %.1f man-seconds)", NO_ROLE_RADIUS, idle, n, median(idle_s));
    Msg("[ai_test] in by the door after the ring: %u/%u runs (somebody went in: %u, median t=%.1f)", door, n, u32(entry.size()), median(entry));
    Msg("[ai_test] places, median of runs: wall in the face %.1f man-s, going inside the house %.1f man-s", median(wall), median(inside));
    Msg("[ai_test] position query, median of runs: main thread %.2f ms a second, most on the second thread in a frame %.2f ms", median(pq_main), median(pq_worker));
    Msg("[ai_test] where he is thought to be, unseen: %.1f m off him (median of the runs' medians), %u jump(s) over %.0f m in all | threat map step at most %.3f ms (median of runs)",
        median(spot_error), jumps, SPOT_JUMP, median(tm_ms));
    {
        u32 slipped = 0, seen = 0, found = 0;
        xr_vector<float> found_after, closest;
        for (const SResult& x : results)
        {
            if (!x.slipped)
                continue;
            ++slipped;
            seen += x.seen_slipping ? 1 : 0;
            if (x.found_after >= 0.f)
            {
                ++found;
                found_after.push_back(x.found_after);
            }
            if (x.slip_closest >= 0.f)
                closest.push_back(x.slip_closest);
        }
        if (slipped)
            Msg("[ai_test] slip: %u run(s), seen on the way out in %u; found where he hid in %u (median %.1f s after); nearest a man came, median %.1f m", slipped, seen, found,
                median(found_after), median(closest));
    }
    Msg("[ai_test] frame avg, median of runs: %.1f ms", median(frame));
    Msg("[ai_test] SERIES RESULT: %u/%u runs passed all three", all, n);
}

void finish(LPCSTR why)
{
    s_series.results.push_back(report(why));
    const bool more = !xr_strcmp(why, "time") && s_series.results.size() < s_run.runs;
    // a series removes each run's men whatever keep says: ten more a run pile up
    if (!s_run.keep || more)
        remove_npcs();
    if (more)
    {
        // the same run again, from scratch: only what was asked carries over
        SRun next;
        next.runs = s_run.runs;
        next.v2 = s_run.v2;
        next.tm = s_run.tm;
        next.slip_at = s_run.slip_at;
        next.slip_to_given = s_run.slip_to_given;
        next.slip_to = s_run.slip_to;
        next.count = s_run.count;
        next.seconds = s_run.seconds;
        xr_strcpy(next.section, s_run.section);
        next.bearing = s_run.bearing;
        next.dist = s_run.dist;
        next.keep = s_run.keep;
        next.kill_every = s_run.kill_every;
        next.kill_from = s_run.kill_from;
        next.spot_given = s_run.spot_given;
        next.spot = s_run.spot;
        next.level = s_run.level;
        next.phase = SRun::eBetweenRuns;
        next.resume_time = Device.dwTimeGlobal + 3000; // the bodies gone first
        s_run = next;
        Msg("[ai_test] run %u of %u in 3 s", u32(s_series.results.size()) + 1, s_run.runs);
        return;
    }
    if (s_series.results.size() > 1)
        summary();
    restore();
}

void start()
{
    SRun& r = s_run;
    if (!s_series.active)
    {
        s_series.active = true;
        s_series.god_was = !!psActorFlags.test(AF_GODMODE); // restore() puts back this, on every way out
        s_series.v2_was = PositionQuery::g_ai_tactics_v2;
        s_series.tm_was = ThreatMap::g_on;
    }
    CActor* actor = Actor();
    auto game = smart_cast<game_sv_Single*>(Level().Server ? Level().Server->game : nullptr);
    if (!actor || !game || !ai().get_alife())
    {
        Msg("! [ai_test] needs a single-player game with the player in it");
        restore();
        return;
    }
    if (!r.spot_given && xr_strcmp(Level().name().c_str(), IZBA_LEVEL))
    {
        Msg("! [ai_test] the izba is on %s, this is %s: load a Cordon save, or give at=x,y,z", IZBA_LEVEL, Level().name().c_str());
        restore();
        return;
    }
    r.level = Level().name();

    psActorFlags.set(AF_GODMODE, TRUE);
    if (r.v2 >= 0)
        PositionQuery::g_ai_tactics_v2 = r.v2;
    if (r.tm >= 0)
        ThreatMap::g_on = r.tm;
    actor->MoveActor(r.spot, Fvector{0.f, 0.f, 0.f});
    // a clean start: no siege left "held for a minute" by a previous run
    stalker_dbg_forget(actor->ID());

    const float angle = deg2rad(r.bearing);
    const Fvector center = Fvector(r.spot).mad(Fvector{_sin(angle), 0.f, _cos(angle)}, r.dist);
    const float radius = r.count > 1 ? 2.5f + .4f * float(r.count) : 0.f;
    for (u32 i = 0; i < r.count; ++i)
    {
        const float a = PI_MUL_2 * float(i) / float(r.count);
        Fvector pos = Fvector(center).add(Fvector{radius * _cos(a), 0.f, radius * _sin(a)});
        u32 vertex = ai().level_graph().vertex_id(pos);
        if (!ai().level_graph().valid_vertex_id(vertex))
            vertex = ai().level_graph().nearest_vertex_id(pos);
        if (!ai().level_graph().valid_vertex_id(vertex))
            continue;
        pos = ai().level_graph().vertex_position(vertex);
        if (CSE_Abstract* spawned = game->alife().spawn_item(r.section, pos, vertex, ai().cross_table().vertex(vertex).game_vertex_id(), u16(-1)))
            r.npcs.push_back({spawned->ID});
    }
    if (r.npcs.empty())
    {
        Msg("! [ai_test] nobody spawned around (%.1f,%.1f,%.1f)", VPUSH(center));
        restore();
        return;
    }

    r.phase = SRun::eSpawning;
    r.spawn_time = Device.dwTimeGlobal;
    Msg("[ai_test] siege_izba: player at (%.1f,%.1f,%.1f) (immortal for the run), %u x '%s' at (%.1f,%.1f,%.1f), %.0f m at %.0f deg; %.0f s of fight", VPUSH(r.spot),
        u32(r.npcs.size()), r.section, VPUSH(center), r.dist, r.bearing, r.seconds);
}

// ⭐slip=T: a route by the AI map from where he stands to another house --
// slip_to=, else the one 15-45 m off farthest from where the squad came from.
bool slip_route(SRun& r, const Fvector& from_position)
{
    // where he hides the men can go: 📏06-10 23:25, a cellar behind a
    // restrictor -- the man going in stood on the floor over it
    CAI_Stalker* probe = nullptr;
    for (const SNpc& npc : r.npcs)
        if (CAI_Stalker* S = npc.online && !npc.dead ? stalker(npc.id) : nullptr)
        {
            probe = S;
            break;
        }
    const CLevelGraph& graph = ai().level_graph();
    u32 from = graph.vertex_id(from_position);
    if (!graph.valid_vertex_id(from))
        from = graph.nearest_vertex_id(from_position);
    if (!graph.valid_vertex_id(from))
        return false;
    u32 to = u32(-1);
    if (r.slip_to_given)
    {
        to = graph.vertex_id(r.slip_to);
        if (!graph.valid_vertex_id(to))
            to = graph.nearest_vertex_id(r.slip_to);
        if (!graph.valid_vertex_id(to))
            return false;
    }
    const float angle = deg2rad(r.bearing);
    const Fvector squad = Fvector(r.spot).mad(Fvector{_sin(angle), 0.f, _cos(angle)}, r.dist);
    const int home = AIHouse::HouseAt(from);
    const Fvector start = graph.vertex_position(from);

    xr_unordered_map<u32, u32> parent;
    parent.emplace(from, from);
    xr_vector<u32> open{from};
    float best = -1.f;
    for (u32 head = 0; head < open.size() && open.size() < SLIP_NODES; ++head)
    {
        const u32 v = open[head];
        if (v == to)
            break;
        const Fvector p = graph.vertex_position(v);
        const float away = flat_distance(p, start);
        if (!r.slip_to_given)
        {
            const int house = AIHouse::HouseAt(v);
            if (house >= 0 && house != home && away >= SLIP_MIN && away <= SLIP_MAX && flat_distance(p, squad) > best && (!probe || probe->movement().accessible(v)))
            {
                best = flat_distance(p, squad);
                to = v;
            }
        }
        const auto* vx = graph.vertex(v);
        for (int j = 0; j < 4; ++j)
        {
            const u32 n = vx->link(j);
            if (!graph.valid_vertex_id(n) || flat_distance(graph.vertex_position(n), start) > SLIP_MAX + 15.f || !parent.emplace(n, v).second)
                continue;
            open.push_back(n);
        }
    }
    if (!graph.valid_vertex_id(to) || parent.find(to) == parent.end())
        return false;
    r.slip_path.clear();
    for (u32 v = to; v != from; v = parent[v])
        r.slip_path.push_back(graph.vertex_position(v));
    std::reverse(r.slip_path.begin(), r.slip_path.end());
    r.slip_next = 0;
    r.slip_at_pos = from_position;
    return !r.slip_path.empty();
}

// A frame of the walk: along the route at SLIP_SPEED, facing the way he goes.
void slip_walk(SRun& r, CActor& actor, u32 now)
{
    float left = SLIP_SPEED * Device.fTimeDelta;
    Fvector p = r.slip_at_pos;
    Fvector heading{0.f, 0.f, 1.f};
    static u32 s_lag_line = 0;
    if (actor.Position().distance_to(p) > 2.f && now > s_lag_line + 1000)
    {
        s_lag_line = now;
        Msg("! [ai_test] t=%.1f slip: the player is at (%.1f,%.1f,%.1f), %.1f m from where the walk has him", t_rel(now), VPUSH(actor.Position()), actor.Position().distance_to(p));
    }
    while (left > 0.f && r.slip_next < r.slip_path.size())
    {
        const Fvector& target = r.slip_path[r.slip_next];
        Fvector way = Fvector().sub(target, p);
        const float d = way.magnitude();
        if (d > EPS_L)
            heading = Fvector(way).div(d);
        if (d <= left)
        {
            p = target;
            left -= d;
            ++r.slip_next;
        }
        else
        {
            p.mad(heading, left);
            left = 0.f;
        }
    }
    float yaw, pitch;
    heading.getHP(yaw, pitch);
    r.slip_at_pos = p;
    actor.MoveActor(p, Fvector{0.f, -yaw, 0.f});
    if (r.slip_next >= r.slip_path.size())
    {
        r.t_hidden = t_rel(now);
        Msg("[ai_test] t=%.1f he is hidden at (%.1f,%.1f,%.1f), %.1f m from the izba, after %.1f s of walking%s", r.t_hidden, VPUSH(p), flat_distance(p, r.spot), r.t_hidden - r.t_slip,
            r.t_slip_seen >= 0.f ? " (seen on the way)" : ", unseen");
    }
}

// kill=T: the nearest of them alive shot dead by the player -- the hit a bullet
// of his would send (GE_HIT from him, his weapon, the head), so the death runs
// the whole way a real one does: the scripts' death callbacks, the corpse, the
// loot, the news.
void kill_one(u32 now)
{
    SRun& r = s_run;
    CActor* actor = Actor();
    CAI_Stalker* victim = nullptr;
    float best = flt_max;
    for (const SNpc& npc : r.npcs)
    {
        CAI_Stalker* S = npc.online && !npc.dead ? stalker(npc.id) : nullptr;
        if (!S || !S->g_Alive())
            continue;
        const float d = S->Position().distance_to_sqr(actor->Position());
        if (d < best)
        {
            best = d;
            victim = S;
        }
    }
    if (!victim)
    {
        Msg("[ai_test] t=%.1f kill: nobody of them left alive", t_rel(now));
        r.kill_every = -1.f;
        return;
    }
    IKinematics* K = smart_cast<IKinematics*>(victim->Visual());
    const u16 head = K ? K->LL_BoneID("bip01_head") : BI_NONE;
    Fvector dir = Fvector().sub(victim->Position(), actor->Position());
    dir.y = 0.f;
    dir.normalize_safe();

    SHit hit;
    hit.GenHeader(GE_HIT, victim->ID());
    hit.whoID = actor->ID();
    const PIItem weapon = actor->inventory().ActiveItem();
    hit.weaponID = weapon ? weapon->object().ID() : actor->ID();
    hit.dir = dir;
    hit.power = 10.f;
    hit.p_in_bone_space.set(0.f, 0.f, 0.f);
    hit.impulse = 100.f;
    hit.hit_type = ALife::eHitTypeFireWound;
    hit.boneID = head == BI_NONE ? 0 : head;
    hit.ap = 0.f;
    hit.aim_bullet = false;
    NET_Packet P;
    hit.Write_Packet(P);
    actor->u_EventSend(P);

    SRun::SDeath& d = r.deaths.emplace_back();
    d.time = now;
    d.by_player = true;
    xr_strcpy(d.who, victim->Name());
    Msg("[ai_test] t=%.1f kill: the player shoots '%s' (%.0f m) in the head", t_rel(now), victim->Name(), _sqrt(best));
}

// The frames 2 s before a death against the 2 s after it, once the after is in.
void report_deaths(u32 now)
{
    constexpr u32 SIDE = 2000;
    SRun& r = s_run;
    const u32 n = std::min(r.ring_n, SRun::RING);
    for (u32 k = 0; k < r.deaths.size(); ++k)
    {
        SRun::SDeath& d = r.deaths[k];
        if (d.reported || now < d.time + SIDE)
            continue;
        d.reported = true;
        double sum[2]{};
        u32 cnt[2]{}, slow[2]{};
        float worst[2]{};
        for (u32 i = 0; i < n; ++i)
        {
            const u32 at = (r.ring_n - 1 - i) % SRun::RING;
            const u32 t = r.ring_time[at];
            // the frame that ended as the shot went is before it
            const int side = t <= d.time ? (d.time - t <= SIDE ? 0 : -1) : (t - d.time <= SIDE ? 1 : -1);
            if (side < 0)
                continue;
            const float ms = r.ring_ms[at];
            sum[side] += ms;
            ++cnt[side];
            slow[side] += ms > SLOW_FRAME_MS ? 1 : 0;
            worst[side] = std::max(worst[side], ms);
        }
        Msg("[ai_test] death %u '%s' (%s) t=%.1f | before avg %.1f max %.0f >33ms %u | after avg %.1f max %.0f >33ms %u", k + 1, d.who, d.by_player ? "player" : "fight",
            t_rel(d.time), cnt[0] ? float(sum[0] / cnt[0]) : 0.f, worst[0], slow[0], cnt[1] ? float(sum[1] / cnt[1]) : 0.f, worst[1], slow[1]);
    }
}

// Everybody in the world and settled: set them on him, and tell them where he is.
void engage(u32 now)
{
    SRun& r = s_run;
    CActor* actor = Actor();
    // whatever was made of him before he was back in the izba
    stalker_dbg_forget(actor->ID());
    CInventoryOwner* actor_owner = smart_cast<CInventoryOwner*>(actor);
    u32 hostile = 0;
    for (SNpc& npc : r.npcs)
    {
        CAI_Stalker* S = npc.online ? stalker(npc.id) : nullptr;
        if (!S || !S->g_Alive())
            continue;
        if (CInventoryOwner* owner = smart_cast<CInventoryOwner*>(S); owner && actor_owner)
            RELATION_REGISTRY().SetRelationType(owner, actor_owner, ALife::eRelationTypeEnemy);
        // "he is in the izba": what a shot heard from there would tell them
        S->memory().make_object_visible_somewhen(actor);
        hostile += S->is_relation_enemy(actor) ? 1 : 0;
    }
    r.engage_time = now;
    r.next_kill = now + u32(r.kill_from * 1000.f);
    PositionQuery::ResetStats(); // the run's own cost
    ThreatMap::ResetStats();
    r.next_sample = now;
    r.next_progress = now + PROGRESS_MS;
    r.phase = SRun::eRunning;
    u32 online = 0;
    for (const SNpc& npc : r.npcs)
        online += npc.online ? 1 : 0;
    Msg("[ai_test] t=0: %u of %u in the world, %u hostile to the player -- the fight begins", online, u32(r.npcs.size()), hostile);
    // the house as the bake read it (step 5): its doors and windows
    AIHouse::Describe(r.spot);
}

void sample(u32 now)
{
    SRun& r = s_run;
    const float dt = r.last_sample ? float(now - r.last_sample) / 1000.f : SAMPLE_MS / 1000.f;
    r.last_sample = now;
    r.next_sample = now + SAMPLE_MS;

    CActor* actor = Actor();
    const u16 aid = actor->ID();
    psActorFlags.set(AF_GODMODE, TRUE); // whatever a script did with it

    SStalkerDbgSiege siege;
    const bool besieged = stalker_dbg_siege(aid, siege) && siege.indoors;
    if (besieged && siege.start != r.siege_start)
    {
        // a new siege, not the same one read again: it is scored afresh
        r.siege_start = siege.start;
        // already on when the run began (the village kept fighting since the
        // last run): from t=0, not "never"
        r.t_siege = std::max(0.f, t_rel(siege.start));
        r.openings.clear();
        r.t_all_watched = -1.f;
        r.all_watched_s = 0.f;
        r.siege_s = 0.f;
        Msg("[ai_test] t=%.1f the siege begins: %u opening(s)", r.t_siege, u32(siege.openings.size()));
    }
    // what is scored: the izba's door as a fact of the scenario, first, then
    // the openings as the siege read them -- those only ever get added to
    xr_vector<SStalkerDbgOpening> scored;
    if (besieged)
    {
        if (!r.spot_given)
            scored.push_back(izba_door());
        scored.insert(scored.end(), siege.openings.begin(), siege.openings.end());
        r.siege_s += dt;
        for (u32 k = 0; k < scored.size(); ++k)
        {
            if (k >= r.openings.size())
                r.openings.push_back({scored[k].mouth, false, false, !r.spot_given && k == 0});
            r.openings[k].mouth = scored[k].mouth;
            r.openings[k].door = scored[k].door;
            r.openings[k].blind = scored[k].blind;
        }
    }

    // the house reaches as far from the player as its farthest opening
    float house_radius = 5.f;
    for (const SOpeningTrack& o : r.openings)
        house_radius = std::max(house_radius, flat_distance(o.mouth, r.spot) + .5f);

    xr_vector<bool> watched(r.openings.size(), false);
    r.last_alive = r.last_fight = r.last_sees = r.last_inside = r.last_far = r.last_watched = 0;
    std::fill(std::begin(r.last_roles), std::end(r.last_roles), 0u);
    for (SNpc& npc : r.npcs)
    {
        if (!npc.online || npc.dead)
            continue;
        CAI_Stalker* S = stalker(npc.id);
        if (!S || !S->g_Alive())
        {
            npc.dead = true;
            Msg("[ai_test] t=%.1f '%s' is dead", t_rel(now), npc.name);
            // the player's kills are on the list from the shot; the fight's
            // own are seen here, up to a sample late
            const bool listed = std::any_of(r.deaths.begin(), r.deaths.end(), [&](const SRun::SDeath& d) { return !xr_strcmp(d.who, npc.name); });
            if (!listed)
            {
                SRun::SDeath& d = r.deaths.emplace_back();
                d.time = now;
                xr_strcpy(d.who, npc.name);
            }
            continue;
        }
        ++r.last_alive;

        const CEntityAlive* enemy = S->memory().enemy().selected();
        if (enemy && enemy->ID() == aid)
        {
            ++r.last_fight;
            npc.fight_s += dt;
            if (S->memory().visual().visible_now(actor))
            {
                ++r.last_sees;
                if (r.t_first_sight < 0.f)
                    r.t_first_sight = t_rel(now);
            }
        }

        string64 role_text;
        const EStalkerDbgRole role = stalker_dbg_role(aid, npc.id, role_text);
        npc.role_s[role] += dt;
        ++r.last_roles[role];
        // "standing" far off with nothing to do: not one running in from the
        // spawn point -- a walk covers ~0.4 m between two samples
        const bool standing = npc.placed && flat_distance(S->Position(), npc.last_position) < .15f;
        npc.last_position = S->Position();
        npc.placed = true;
        const float far_off = flat_distance(S->Position(), r.spot);
        if (role == eStalkerDbgRoleNone && standing && far_off > NO_ROLE_RADIUS)
        {
            ++r.last_far;
            npc.no_role_far_s += dt;
            npc.worst_far = std::max(npc.worst_far, far_off);
        }

        // a wall in his face where he stands: static geometry within an arm's
        // length the way his head looks (06-10 01:10, 15:49)
        if (standing)
        {
            Fvector look;
            look.setHP(-S->movement().head_orientation().current.yaw, 0.f);
            collide::rq_result hit;
            if (Level().ObjectSpace.RayPick(S->eye_matrix.c, look, WALL_REACH, collide::rqtStatic, hit, nullptr))
            {
                r.wall_s += dt;
                string32 action;
                stalker_dbg_action(*S, action);
                xr_sprintf(r.wall_who, "%s: %s, %s", npc.name, role_text[0] ? role_text : "no role", action);
            }
        }
        // where he is going, under the roof of the house he besieges -- the
        // searcher's job is to go in; nobody else's place is in there
        const u32 dest = S->movement().level_dest_vertex_id();
        if (besieged && role != eStalkerDbgRoleSearcher && ai().level_graph().valid_vertex_id(dest))
        {
            const Fvector at = ai().level_graph().vertex_position(dest);
            if (flat_distance(at, r.spot) < house_radius && stalker_siege_inside(aid, at))
            {
                r.dest_inside_s += dt;
                string32 action;
                stalker_dbg_action(*S, action);
                xr_sprintf(r.dest_inside_who, "%s: %s, %s", npc.name, role_text[0] ? role_text : "no role", action);
            }
        }

        // under the roof of THIS house: within its openings of the player, not
        // the next shed -- 📏06-10 17:13: a man under a roof 11 m off was "in"
        const bool inside = besieged && flat_distance(S->Position(), r.spot) < house_radius && stalker_siege_inside(aid, S->Position());
        if (inside)
        {
            ++r.last_inside;
            if (r.t_entry < 0.f)
            {
                r.t_entry = t_rel(now);
                xr_strcpy(r.entry_who, npc.name);
                r.entry_at = S->Position();
                r.entry_after_ring = r.t_all_watched >= 0.f;
                // by the door: next to the scenario's one; given a spot of his
                // own, the nearest opening the siege read and what it took it for
                float best = flt_max;
                for (u32 k = 0; k < scored.size(); ++k)
                {
                    const float d = flat_distance(S->Position(), scored[k].mouth);
                    if (d < best && (!r.spot_given ? k == 0 : true))
                    {
                        best = d;
                        r.entry_opening = int(k);
                        r.entry_door = r.spot_given ? scored[k].door : d < DOOR_REACH;
                    }
                }
                Msg("[ai_test] t=%.1f '%s' is the first in the house, at (%.1f,%.1f,%.1f), %.1f m from %s", r.t_entry, npc.name, VPUSH(r.entry_at), best,
                    !r.spot_given ? "the door" : r.entry_door ? "a door" : "a window");
            }
            continue; // a man inside keeps no opening from outside
        }

        for (u32 k = 0; k < scored.size(); ++k)
            if (!watched[k] && watches(*S, scored[k]))
                watched[k] = true;
    }

    // every opening, the blind ones too: a window nobody found a place for is a
    // window the player walks out of
    bool all = besieged && !r.openings.empty();
    for (u32 k = 0; k < r.openings.size(); ++k)
    {
        SOpeningTrack& o = r.openings[k];
        if (!watched[k])
        {
            all = false;
            continue;
        }
        ++r.last_watched;
        o.watched_s += dt;
        if (o.first < 0.f)
            o.first = t_rel(now);
    }
    if (all)
    {
        r.all_watched_s += dt;
        if (r.t_all_watched < 0.f)
        {
            r.t_all_watched = t_rel(now);
            Msg("[ai_test] t=%.1f every opening is watched (%.1f s after the siege began)", r.t_all_watched, r.t_all_watched - r.t_siege);
        }
    }

    // slip=T: seen on his way out, found where he hid, how near they came
    if (r.t_slip >= 0.f)
    {
        const float t = t_rel(now);
        if (r.last_sees && r.t_slip_seen < 0.f)
        {
            r.t_slip_seen = t;
            Msg("[ai_test] t=%.1f seen %s", t, r.t_hidden < 0.f ? "slipping out" : "where he hid");
        }
        if (r.t_hidden >= 0.f)
        {
            if (r.last_sees && r.t_found < 0.f)
            {
                r.t_found = t;
                Msg("[ai_test] t=%.1f found where he hid, %.1f s after", t, t - r.t_hidden);
            }
            for (const SNpc& npc : r.npcs)
                if (CAI_Stalker* S = npc.online && !npc.dead ? stalker(npc.id) : nullptr)
                    r.slip_closest = std::min(r.slip_closest, flat_distance(S->Position(), actor->Position()));
        }
    }

    // ⭐step 2: how far "where he is" is from him while nobody sees him, and
    // how often it jumps
    Fvector spot;
    u32 seen_time = 0;
    if (Commander::DbgSpot(aid, spot, seen_time))
    {
        const bool unseen = now > seen_time + UNSEEN;
        if (unseen)
        {
            r.spot_error.push_back(flat_distance(spot, actor->Position()));
            if (r.spot_known && flat_distance(spot, r.last_spot) > SPOT_JUMP)
            {
                ++r.spot_jumps;
                Msg("[ai_test] t=%.1f \"where he is\" jumps %.1f m unseen: (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f), %.1f m from him", t_rel(now), flat_distance(spot, r.last_spot),
                    VPUSH(r.last_spot), VPUSH(spot), r.spot_error.back());
            }
        }
        r.last_spot = spot;
        r.spot_known = true;
    }

    if (now >= r.next_progress)
    {
        r.next_progress = now + PROGRESS_MS;
        progress_line(now);
    }
}
} // namespace

bool AITest::Running() { return s_run.phase != SRun::eIdle; }

void AITest::Command(LPCSTR args)
{
    string256 verb{};
    if (!args || sscanf(args, "%255s", verb) != 1)
        verb[0] = 0;

    if (!xr_strcmp(verb, "stop"))
    {
        if (Running())
            finish("stopped");
        else
            Msg("[ai_test] nothing is running");
        return;
    }
    if (!xr_strcmp(verb, "status"))
    {
        if (s_run.phase == SRun::eRunning)
            progress_line(Device.dwTimeGlobal);
        else
            Msg("[ai_test] %s", Running() ? "spawning" : "nothing is running");
        return;
    }
    if (xr_strcmp(verb, "siege_izba"))
    {
        Msg("[ai_test] usage: ai_test siege_izba [runs=1] [v2=0|1] [tm=0|1] [slip=T] [slip_to=x,y,z] [count=10] [time=90] [section=esc_stalker_respawn_2] [bearing=108] [dist=35] [keep=0] [at=x,y,z|smart] [kill=T] [kill_from=10] | stop | status");
        return;
    }
    if (!g_pGameLevel)
        return;
    if (Running())
    {
        Msg("[ai_test] already running: ai_test stop first");
        return;
    }

    s_run = {};
    SRun& r = s_run;
    string256 rest{};
    xr_strcpy(rest, strstr(args, verb) + xr_strlen(verb));
    // word by word; not strtok_s, which the Switch's libc does not have
    string256 token{};
    int used = 0;
    for (LPCSTR cursor = rest; sscanf(cursor, "%255s%n", token, &used) == 1; cursor += used)
    {
        float x, y, z;
        int n;
        if (sscanf(token, "runs=%d", &n) == 1)
            r.runs = u32(std::clamp(n, 1, 20));
        else if (sscanf(token, "v2=%d", &n) == 1)
            r.v2 = n ? 1 : 0;
        else if (sscanf(token, "tm=%d", &n) == 1)
            r.tm = n ? 1 : 0;
        else if (sscanf(token, "slip_to=%f,%f,%f", &x, &y, &z) == 3)
        {
            r.slip_to.set(x, y, z);
            r.slip_to_given = true;
        }
        else if (sscanf(token, "slip=%f", &x) == 1)
            r.slip_at = std::clamp(x, 0.f, 300.f);
        else if (sscanf(token, "count=%d", &n) == 1)
            r.count = u32(std::clamp(n, 1, 30));
        else if (sscanf(token, "time=%f", &x) == 1)
            r.seconds = std::clamp(x, 5.f, 600.f);
        else if (sscanf(token, "bearing=%f", &x) == 1)
            r.bearing = x;
        else if (sscanf(token, "dist=%f", &x) == 1)
            r.dist = std::clamp(x, 5.f, 150.f);
        else if (sscanf(token, "keep=%d", &n) == 1)
            r.keep = n != 0;
        else if (sscanf(token, "kill_from=%f", &x) == 1)
            r.kill_from = std::clamp(x, 0.f, 600.f);
        else if (sscanf(token, "kill=%f", &x) == 1)
            r.kill_every = x > 0.f ? std::clamp(x, .5f, 600.f) : -1.f;
        else if (sscanf(token, "at=%f,%f,%f", &x, &y, &z) == 3)
        {
            r.spot.set(x, y, z);
            r.spot_given = true;
        }
        else if (!strncmp(token, "at=", 3))
        {
            // a smart terrain of this level by name: where on the AI map it stands
            const CSE_ALifeDynamicObject* zone = nullptr;
            if (g_pGameLevel && ai().get_alife())
                for (const auto& [id, o] : ai().alife().objects().objects())
                    if (smart_cast<CSE_ALifeSmartZone*>(o) && !xr_strcmp(o->name_replace(), token + 3) && ai().game_graph().valid_vertex_id(o->m_tGraphID) &&
                        ai().game_graph().vertex(o->m_tGraphID)->level_id() == ai().level_graph().level_id())
                        zone = o;
            u32 vertex = zone ? ai().level_graph().vertex_id(zone->o_Position) : u32(-1);
            if (zone && !ai().level_graph().valid_vertex_id(vertex))
                vertex = ai().level_graph().nearest_vertex_id(zone->o_Position);
            if (!zone || !ai().level_graph().valid_vertex_id(vertex))
            {
                Msg("! [ai_test] no smart terrain '%s' on this level", token + 3);
                return;
            }
            r.spot = ai().level_graph().vertex_position(vertex);
            r.spot_given = true;
        }
        else if (!strncmp(token, "section=", 8))
        {
            if (!pSettings->section_exist(token + 8))
            {
                Msg("! [ai_test] no section '%s'", token + 8);
                return;
            }
            xr_strcpy(r.section, token + 8);
        }
        else
        {
            Msg("! [ai_test] what is '%s'?", token);
            return;
        }
    }
    start();
}

void AITest::Frame()
{
    SRun& r = s_run;
    if (r.phase == SRun::eIdle)
        return;
    // the level went away under the run: its ids mean nothing here
    if (!g_pGameLevel || !Actor() || r.level != Level().name())
    {
        Msg("! [ai_test] the level or the player is gone: run dropped");
        restore();
        return;
    }

    const u32 now = Device.dwTimeGlobal;
    if (r.phase == SRun::eBetweenRuns)
    {
        if (now >= r.resume_time)
            start();
        return;
    }
    if (r.phase == SRun::eSpawning)
    {
        bool all = true;
        for (SNpc& npc : r.npcs)
        {
            if (npc.online)
                continue;
            if (CAI_Stalker* S = stalker(npc.id))
            {
                npc.online = true;
                xr_strcpy(npc.name, S->Name());
            }
            else
                all = false;
        }
        if (all && !r.online_time)
            r.online_time = now;
        // the move into the izba lands a frame late, and out of a cellar later
        // still: 📏06-10 23:27, a run began with every man remembering him
        // where the last run's walk had left him, 45 m off
        const bool home = Actor()->Position().distance_to_sqr(r.spot) < _sqr(2.f);
        if (!home)
            Actor()->MoveActor(r.spot, Fvector{0.f, 0.f, 0.f});
        const bool settled = r.online_time && now >= r.online_time + ONLINE_SETTLE && home;
        if (settled || now > r.spawn_time + ONLINE_TIMEOUT)
            engage(now);
        return;
    }

    if (Device.fTimeDelta > 0.f)
    {
        const float ms = Device.fTimeDelta * 1000.f;
        r.frame_sum += ms;
        ++r.frames;
        r.slow_frames += ms > SLOW_FRAME_MS ? 1 : 0;
        r.frame_max = std::max(r.frame_max, ms);
        r.ring_time[r.ring_n % SRun::RING] = now;
        r.ring_ms[r.ring_n % SRun::RING] = ms;
        ++r.ring_n;
    }
    if (r.kill_every > 0.f && now >= r.next_kill)
    {
        kill_one(now);
        r.next_kill = now + u32(r.kill_every * 1000.f);
    }
    report_deaths(now);

    if (r.slip_at >= 0.f && r.t_slip < 0.f && t_rel(now) >= r.slip_at)
    {
        if (slip_route(r, Actor()->Position()))
        {
            r.t_slip = t_rel(now);
            Msg("[ai_test] t=%.1f he slips out: %u vertices to (%.1f,%.1f,%.1f), %.1f m from the izba, house #%d", r.t_slip, u32(r.slip_path.size()), VPUSH(r.slip_path.back()),
                flat_distance(r.slip_path.back(), r.spot), AIHouse::HouseAt(r.slip_path.back()));
        }
        else
        {
            Msg("! [ai_test] slip: no route to another house");
            r.slip_at = -1.f;
        }
    }
    if (r.t_slip >= 0.f && r.t_hidden < 0.f)
        slip_walk(r, *Actor(), now);

    if (now >= r.next_sample)
        sample(now);
    if (now >= r.engage_time + u32(r.seconds * 1000.f))
        finish("time");
}

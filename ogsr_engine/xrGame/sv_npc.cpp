#include "StdAfx.h"
#include "sv_npc.h"

#include "sv_world_lookup.h"
#include "sv_visual_armour.h"

#include "xrServer.h"
#include "xrServer_Objects.h"
#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "Actor.h"
#include "xrMessages.h"
#include "game_sv_single.h"
#include "../COMMON_AI/GRAPH/level_graph.h"
#include "../COMMON_AI/GRAPH/game_level_cross_table.h"
#include "../COMMON_AI/GRAPH/graph_engine.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "character_community.h"
#include "PhysicsShellHolder.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "Weapon.h" // the magazine, for the reload trace below
#include "sv_npc_health.h"
// The flip explainer lives with the rest of the "why is it doing that" reports.
#include "sv_npc_fight.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager.h"
#include "cl_anim_blendspace.h" // ClAnim::GaitSpeeds -- one table, both ends
#include "object_handler_space.h"
#include "object_handler_planner.h"
#include "sv_motion_audit.h"
#include "sv_accuracy.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "script_entity_action.h" // a stalker's order is carried out as a script action -- see SVNpc::Steer
#include "sight_manager_space.h"
#include "detail_path_manager.h" // path() for the steer diagnostic; brings detail_path_manager_space.h
#include "movement_manager_space.h" // MovementManager::ePathTypeLevelPath
#include "ai_object_location.h" // a creature's own level vertex, for a steered goal off the mesh

#include <mutex>

// sv_npc_own_motion: 1 (default) = while the server is driving a puppet, the
// server owns where it is. 0 = leave the creature's own simulation in charge
// and watch it take the position back.
//
// ⚠This is not a tuning knob, it is the difference between the orders working
// and not. Left to itself an online stalker writes its own position every
// UpdateCL from its physics capsule (CCustomMonster::UpdatePositionAnimation ->
// CMovementManager::move_along_path -> GetPosition -> XFORM().translate_over),
// so a puppet asked for 1.6 m/s walked at 0.5: we asserted 1.6 m of motion a
// second and the creature quietly took back 1.1 of it, five millimetres at a
// time at 200 Hz. Kept as a switch only because the A/B is what proved it.
int g_sv_npc_own_motion = 1;

// sv_npc_watch: -1 = nobody (the default), 0 = whichever puppet is first in
// the roster, >0 = that entity id. The 0 form exists because a spawned id is
// not knowable when the harness composes its console stages, and having to
// read one run's log to configure the next is how a two-minute answer becomes
// a twenty-minute one.
int g_sv_npc_watch = -1;

// sv_npc_trace: one line per second per moving puppet -- where it is, which
// waypoint it is working on, how much closer it has got. Off by default; a
// server nobody is debugging should not pay for it, and at 200 Hz a per-tick
// version would bury the log in the thing it is meant to explain.
//
// ⭐This exists because "drove 60 m, ended up 23 m away" is not a diagnosis:
// the counters prove the server spent the distance, only a trace says where it
// went. Registered in console_commands.cpp.
int g_sv_npc_trace = 0;

namespace
{
// The only stalker section this content will actually spawn -- measured
// during the settlement work, and the reason that layer hardcodes it too.
// Anything else is accepted here, it just may not be a creature.
constexpr const char* DEFAULT_SECTION = "stalker";

// ⚠⚠1.6 WAS THE WORST NUMBER AVAILABLE, and it was every driven creature's
// only speed for the whole project.
//
// The content authors two gaits and pairs each with a speed: walk 1.10, run
// 3.00 (plus the danger and panic rows). 1.6 sits between them, so the client
// -- correctly -- renders it as 74% walk blended with 26% run, forever. That is
// the "why do they only have one kind of walking, why can't they run" from
// play: nothing was ever asking them to run, and the one speed they did get was
// a gait nobody authored.
//
// It survives only as the fallback for a creature whose section names no
// velocity table. Everything else now moves at a speed the content actually
// has -- see drive_speed.
constexpr float DEFAULT_SPEED = 1.6f;

// ⛔THREE DISTANCE BANDS USED TO LIVE HERE, and they were the wrong shape.
//
// walk/run/sprint chosen by how far there was left to go: RUN_ENTER 6,
// SPRINT_ENTER 12, each with an exit. It failed in both directions and both
// failures were measured, not argued:
//
//   - the third gear never engaged. A follower running at 3.0 behind a
//     sprinting player peaks at about 7 m of gap and holds it, so a 12 m
//     threshold is unreachable in play while looking perfectly reasonable in
//     the source. (At 18 m, the first attempt, it was hopeless.)
//   - adding "...or the target is faster than my run" made it engage, and
//     flap: 112 gear changes in one session, because a follower standing ON
//     its slot with 0.0 m to cover still saw a moving target and asked for a
//     sprint. Sprinting in place.
//
// A distance cannot answer this. The question is a SPEED -- match what is
// being chased, plus enough to close what is left -- and then the gear is
// whichever one delivers it. See drive_speed.
constexpr float ARRIVE_EPS = 0.35f; // close enough to call a Goto done
constexpr float FOLLOW_GAP = 2.5f; // how far behind a followed entity to stop

// How far a puppet may be from where we left it before we call that somebody
// else's doing. Not zero: the engine settles a capsule by a few centimetres on
// its own, and counting that as interference would report every tick.
constexpr float EXTERNAL_EPS = 0.1f;

// A puppet counts as making progress only when it gets meaningfully nearer the
// goal than its own best so far. Measuring against the PREVIOUS tick instead
// would call a puppet oscillating over a doorway "progressing" half the time.
constexpr float PROGRESS_EPS = 0.5f;

// How long a puppet may spend its whole distance budget without ever getting
// nearer its goal before we conclude the route is no longer usable and search
// again. Generous: a legitimate detour around a building can spend a while
// walking away from the goal, and replanning from inside a doorway every two
// seconds is its own kind of stuck.
constexpr u32 STUCK_MS = 8000;

// After this many failed replans a guard stops believing in its post. Three,
// because one is a door and two is bad luck, and by the third the honest
// conclusion is that there is no route.
constexpr u32 STUCK_GIVE_UP = 3;

// Guard: how far inside the leash a fetched creature has to get before it is
// released again. Without this it is released on the boundary and strays back
// out on its next step, which reads as a guard twitching in and out of the
// server's hands rather than standing watch.
constexpr float LEASH_HYSTERESIS = 0.4f;

constexpr float DEFAULT_LEASH = 12.f;
constexpr int MAX_PATROL_STOPS = 16;

// Path following. Level-graph vertices sit about a metre apart, so the
// waypoint tolerance has to be under that or a puppet oscillates around one.
constexpr float WAYPOINT_EPS = 0.6f;

// How far a followed target may drift from where the current path was aimed
// before it is worth searching again. Too small and every step re-runs A* for
// a path that barely changed; too large and the puppet walks to where you
// were a while ago.
constexpr float REPATH_DIST = 3.f;

// Bounds on the search itself. This runs inside the server tick, so it is
// capped rather than left at the engine's defaults (65500 visited nodes):
// a puppet that cannot find a path is a much smaller problem than a server
// that stalls for a frame trying.
// Largest step Update will integrate at once, in seconds. See the note where
// it is applied: this is a hitch guard, NOT a tick budget, and setting it
// below the real tick spacing quietly throttles every puppet. Which is why the
// bound in force is DERIVED from the spacing this server is measured to have,
// with these three as its shape: a large multiple of a tick, floored at about
// two wire intervals' worth of travel, and never above the old ceiling.
constexpr float HITCH_CLAMP = 3.f;
constexpr float HITCH_FLOOR = 0.10f;
constexpr float HITCH_TICKS = 8.f;

// ⭐How fast a driven body may turn, radians per second.
//
// Reported from play 30-07-2026, watching a shuttle: "визуально выглядит
// намного лучше, только на разворотах супер резко". The drive took its heading
// straight from the direction of the next path point, with no bound at all, so
// a reversal put the body through 180 degrees between one tick and the next --
// and the client, which reads heading off the transform it receives, drew that
// as an instant spin.
//
// ⚠It must stay well ABOVE the turn rate ordinary path curvature asks for, or
// it stops being a smoothing bound and becomes a HEADING LIMITER: the body
// would then permanently lag its own direction of travel and walk sideways
// down every bend. At 8 rad/s a 30-degree bend costs 65 ms and a full reversal
// about 0.4 s, which is roughly what a person on foot takes.
constexpr float TURN_RATE = 8.f;

constexpr float PATH_MAX_RANGE = 250.f;
constexpr u32 PATH_MAX_ITERATIONS = 8000;
constexpr u32 PATH_MAX_VISITED = 8192;

struct Puppet
{
    u16 id{0xffff};
    SVNpc::Order order{SVNpc::Order::Hold};
    Fvector target{};
    u16 follow{0xffff};
    // ⚠An OVERRIDE, not the speed. Set only by sv_npc_speed, and only then
    // does it win -- otherwise the gait below decides. Tests that pin a speed
    // to measure something keep working unchanged; everything else stops being
    // carried at one constant.
    float speed{DEFAULT_SPEED};
    bool speed_explicit{false};
    // Whether this creature is currently running, with hysteresis: the choice
    // is made from how far it still has to go, and a bare threshold on that
    // makes a follower flap between gaits every time its target pauses.
    bool running{false};
    bool sprinting{false}; // on the panic tariff -- the third gear, see drive_speed
    // ⭐The heading actually asserted last tick, so the next one can be a step
    // TOWARDS the desired heading rather than a jump to it. Reported from play:
    // "на разворотах супер резко". See TURN_RATE.
    float yaw{0.f};
    bool yaw_valid{false};
    u8 gait_reported{0xff}; // last gear announced, so the log fires on change only
    // Speed of whatever this puppet is following, over a window. The third
    // gear is keyed off it: you sprint because the man ahead is sprinting,
    // not because he has got a certain number of metres away.
    Fvector follow_pos{};
    u32 follow_ms{0};
    float follow_speed{0.f};
    // ⚠The resolved pair, cached against the state it was resolved for. The
    // drive ticks at ~200 Hz per creature and the lookup behind it walks two
    // ini sections plus a shared_str map -- cheap once, wasteful ten thousand
    // times a second. Re-resolved only when the state that selects the row
    // changes.
    float gait_walk{0.f}, gait_run{0.f};
    // ⚠The sprint tariff is cached SEPARATELY, and that separation is the whole
    // point: the gear is decided by comparing against walk and run, so those
    // two must not themselves depend on the gear. See drive_speed.
    float gait_sprint{0.f};
    u8 gait_state{0xff}, gait_mental{0xff};
    u8 gait_gear{0}; // 0 walk, 1 run, 2 sprint -- the state the hysteresis carries

    // Metres this module has actually asserted. These creatures have their
    // own brains and will wander on their own once A-Life puts them online,
    // so "it moved" proves nothing about whether the SERVER moved it -- this
    // counter is the difference between the two.
    float driven{0.f};

    // The route, as level-graph vertices, and how far along it we are.
    // Empty means "no route": either the search failed or it has not been
    // asked for yet, and the puppet falls back to walking straight at the
    // goal -- which is what it always did before, so a failed search
    // degrades to the old behaviour rather than to standing still.
    xr_vector<u32> path;
    u32 path_at{0};
    Fvector path_goal{}; // what the current route was aimed at
    bool path_failed{false}; // searched and found nothing

    // ⚠Whether a search has been RUN for the current goal -- not whether it
    // produced a route. An empty path is a legitimate outcome (already on the
    // destination's vertex, or the last few metres are a straight line), and
    // triggering off `path.empty()` instead meant re-running the search every
    // single tick. With an off-navmesh destination that is the full-level
    // scan every tick: 183 fps down to 2.2.
    bool path_valid{false};

    // The destination's vertex, resolved once when the order was given.
    u32 dest_vertex{u32(-1)};

    // The level-graph vertex this puppet was last standing on. Kept because
    // every graph lookup is cheap FROM a known vertex and ruinously expensive
    // without one -- see vertex_near.
    u32 at_vertex{u32(-1)};

    // ⚠Where we last put it. `driven` answers "did WE move it"; this answers
    // the other half, "did somebody ELSE" -- the tick after we assert a
    // position the object should still be there, so any difference at the top
    // of the next tick was made by another owner of this object. Without it a
    // puppet that gets teleported to its goal reads exactly like one that
    // walked there, which is precisely the report the previous session could
    // not explain.
    Fvector asserted{};
    bool asserted_valid{false};
    float external{0.f}; // metres somebody else moved it
    u32 external_events{0};
    u32 external_logged{0};

    // Progress bookkeeping, for the trace and for noticing a puppet that is
    // spending its distance budget without getting anywhere.
    u32 trace_ms{0};
    Fvector order_from{}; // where it stood when the order was given
    float order_dist{0.f}; // and how far the goal was then
    float driven_at_check{0.f}; // driven the last time progress was judged
    float closest{0.f}; // nearest it has ever been to the goal
    u32 progress_ms{0}; // when it last got closer than `closest`

    // ⚠Splitting "we asked for X and the object refused it on the spot" from
    // "we asked for X, the object took it, and something put it back later".
    // Both show up as a puppet walking too slowly and they have nothing else
    // in common: the first is a clamp inside the move call, the second is a
    // second owner writing the same field between our ticks.
    float clamped{0.f};
    u32 frame_at_assert{0};
    u32 frames_between{0}; // sum, for the mean below
    u32 frame_samples{0};

    // Patrol: the stops, which one is next, and when the pause at the current
    // one runs out.
    xr_vector<Fvector> stops;
    u32 stop_at{0};
    float dwell{0.f};
    u32 dwell_until{0};

    // Guard: where it belongs and how far it may stray before the server
    // fetches it. `hauling` is the state, and it needs hysteresis -- fetching
    // a creature to exactly the leash boundary means fetching it again on the
    // next step it takes.
    Fvector anchor{};
    float leash{0.f};
    bool hauling{false};

    u32 stuck_events{0};

    // ⭐⭐The order in the form a STALKER carries it out itself (SVNpc::Steer).
    // Update decides, on the network thread; the creature applies, from its own
    // update on the main one. Both under s_lock.
    bool steer{false}; // wants to be moved: under script control, walking to steer_goal
    Fvector steer_goal{};
    // ⚠The goal's level vertex, resolved HERE and handed over explicitly. Left
    // to the script action, a position becomes a vertex through the HINTED
    // lookup (neighbours of where the creature stands) and, failing that, a
    // probe "in the direction of" the point -- measured 22-09-2026: a goto that
    // made 14 m and then stood 5 m short rebuilding a 240-point path every two
    // seconds, and a follower that ran 35 m the wrong way.
    u32 steer_vertex{u32(-1)};
    u8 steer_gear{0}; // 0 walk, 1 run, 2 sprint (panic) -- see drive_speed
    // Bumped whenever the goal or gear changed enough to be worth a new
    // movement order; the creature re-issues only when its applied version
    // differs, so a follower is not re-pathed every tick of a slow walk.
    u32 steer_version{0};
    u32 steer_applied{0};
    // When the gear first differed from the one ordered; a new order for a new
    // gear only once it has held for a moment -- see STEER_GEAR_HOLD_MS.
    u32 steer_gear_since{0};
    bool steer_fighting{false}; // handed back to its brain for a fight -- logged on change
};

xr_vector<Puppet> s_puppets;

// ⚠⚠This module is touched from TWO threads and it took a bug to notice.
// Update() reaches us from xrServer::Update <- CLevel::net_Update <-
// _NetworkProcessor::OnFrame, and that runs inside seqFrameMT, which
// device.cpp submits to the TTAPI pool -- a DIFFERENT thread from the one the
// console runs on, and a different pool thread from frame to frame (which is
// why one puppet's log line carried thread 40808 and the next one's 26568).
// Every order below therefore mutates a vector that the server tick is walking
// at ~200 Hz: a push_back that reallocates while Update holds `Puppet& P` is a
// use-after-free, and an erase mid-iteration is worse.
//
// The lock is held only across roster access, never across the engine calls
// that orders make (a spawn, a GE_DESTROY, or the full-level vertex scan, any
// of which can take hundreds of milliseconds) -- blocking the server tick that
// long would stall every client, so those run outside it against ids that were
// snapshotted inside.
std::recursive_mutex s_lock;

// When Update last ran, for its own delta -- see the note there.
u32 s_last_update_ms = 0;

// Despawns waiting to be checked. See Remove/VerifyRemovals: the console
// command posts a request, and this is what asks whether the world honoured it.
struct PendingDel
{
    u16 id;
    u32 due_ms;
};
xr_vector<PendingDel> s_pending_del;

// Observed server tick rate. Reported by sv_npc_list because it is what
// decides how coarse a puppet's motion is, and it is not the render frame
// rate -- guessing at it once already cost a wrong walking speed.
u32 s_ticks = 0;
u32 s_tick_ms = 0;
float s_dt_lost = 0.f; // seconds thrown away by the hitch clamp

// Which entity WatchPos actually reports on, resolved from g_sv_npc_watch each
// tick so that "0" can mean "the first puppet" without the caller knowing ids.
u16 s_watch_id = 0xffff;

Puppet* find(u16 id)
{
    for (Puppet& P : s_puppets)
        if (P.id == id)
            return &P;
    return nullptr;
}

// Every order takes 0xffff to mean "all of them". A squad is the normal
// unit here -- an escort, a patrol, the group that ambushes a convoy -- so
// addressing them one at a time would be the unusual case, not the handy one.
template <typename F>
u32 for_each(u16 id, F fn)
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);

    u32 touched = 0;
    for (Puppet& P : s_puppets)
        if (id == 0xffff || P.id == id)
        {
            fn(P);
            ++touched;
        }

    if (!touched)
        Msg("! [sv_npc] %u is not a tracked puppet -- sv_npc_list", id);
    return touched;
}

// Assert a position on a creature the server is driving.
//
// ⚠The wrong call here is not a style question -- it is most of a night. A
// creature that is simulated locally rewrites its own position every frame
// from its physics capsule, so writing the transform (ForceTransform,
// net_MoveTo) is a suggestion the very next frame overrules. Only
// net_ServerDriveTo claims ownership for the duration; see the note on
// CCustomMonster::server_driven.
// ⭐⭐HOW FAST TO CARRY A DRIVEN CREATURE -- at a speed the content HAS.
//
// The old answer was DEFAULT_SPEED for everyone, forever, which is why driven
// NPCs had one gait: 1.6 m/s is not a walk and not a run, it is the blend of
// the two the client is obliged to draw when told that number.
//
// This asks the creature's own velocity table, through the SAME function the
// client uses to pair a cycle with a playback rate (ClAnim::GaitSpeeds). Both
// ends therefore land on the same number by construction, and the feet cannot
// slide against the ground by the difference between two lookups -- there is
// only one lookup.
//
// ⚠The creature's mental and body state come from the creature, not from the
// order. The server drives the BODY; what state its brain is in is still the
// brain's business, and it is the same state SVNpc::PostureOf puts on the wire
// for the client to pair against. A stalker who panics mid-escort keeps the
// run cycle and gets the panic tariff -- which is what a sprint is here.
static float drive_speed(Puppet& P, CGameObject* GO, float remaining, bool steered = false)
{
    if (P.speed_explicit)
        return P.speed;

    // ⚠⚠A STEERED stalker's mental state is not an input here -- it is an
    // OUTPUT: SVNpc::Steer puts it in panic to get the sprint tariff. Reading it
    // back made the reference depend on the gear (panic row: run 6.00, so
    // "needs 3.22" fell below run and dropped to run; free row: 3.00, so back up
    // to sprint), and every flap re-issued the order: ~1000 new movement orders
    // in 90 s, a path rebuilt on every update and a creature that never left
    // the spot. The same trap the note below describes, re-entered by a new door.
    u8 state_class = 0, mental = npc_mental_free;
    if (CAI_Stalker* S = steered ? nullptr : smart_cast<CAI_Stalker*>(GO))
    {
        if (MonsterSpace::eBodyStateCrouch == S->movement().body_state())
            state_class = 1;
        else if (S->wounded())
            state_class = 2;

        switch (S->movement().mental_state())
        {
        case MonsterSpace::eMentalStateDanger: mental = npc_mental_danger; break;
        case MonsterSpace::eMentalStatePanic: mental = npc_mental_panic; break;
        default: break;
        }
    }

    // ⭐⭐⭐ASK HOW FAST IT NEEDS TO GO, THEN PICK THE GEAR THAT COVERS IT.
    //
    // Three independent distance bands were the wrong shape twice over. Keyed
    // on the gap alone the third gear never fired (a follower behind a
    // sprinting player peaks at ~7 m and holds it). Bolting a "target is
    // faster than my run" test onto it fired -- and then flapped 112 times in
    // one session, because a follower standing ON its slot with the gap at
    // 0.0 m still saw a moving target and asked for a sprint. Sprinting in
    // place is not a gear problem, it is a question problem.
    //
    // The honest question is a speed, not a distance: match whatever is being
    // chased, plus enough to close the gap that is left. Then the gear is
    // simply the slowest one that delivers it, and both failures go away --
    // nothing to sprint for when the target is walking, nothing to argue about
    // when it is not.
    float needed = P.follow_speed; // keep up...
    if (remaining > 1.f)
        needed += _min(remaining - 1.f, 8.f) * 0.45f; // ...and close what is left

    // ⚠⚠⚠THE REFERENCE MUST NOT DEPEND ON THE GEAR. This is where the second
    // version of this function ate itself.
    //
    // It compared `needed` against P.gait_run -- and P.gait_run was re-read
    // from the PANIC row while sprinting, because that is where the sprint
    // speed comes from. So the boundary moved with the gear: in run gear the
    // test was against 3.00, in sprint gear against 6.00, and a target doing
    // 3.72 sat above the first and below the second. Up, down, up, down, every
    // tick -- **1246 gear changes in one session**, worse than the flapping it
    // was written to fix, and for a reason no threshold tuning could reach.
    //
    // So the row for the DECISION is the creature's own, always, and the panic
    // tariff is cached beside it as a third number that only ever gets used
    // once the decision is made.
    if (P.gait_state != state_class || P.gait_mental != mental)
    {
        P.gait_state = state_class;
        P.gait_mental = mental;
        P.gait_walk = P.gait_run = P.gait_sprint = 0.f;
        ClAnim::GaitSpeeds(GO->cNameSect().c_str(), state_class, mental, false /*forwards*/, P.gait_walk, P.gait_run);

        // The third gear exists only standing and upright: there is no panic
        // crouch and no panic walk in the table.
        if (0 == state_class)
        {
            float sprint_walk = 0.f, sprint_run = 0.f;
            if (ClAnim::GaitSpeeds(GO->cNameSect().c_str(), state_class, npc_mental_panic, false, sprint_walk, sprint_run))
                P.gait_sprint = sprint_run;
        }
    }

    // Hysteresis on the gear, against those stable references.
    const float walk_ref = (P.gait_walk > EPS) ? P.gait_walk : 1.1f;
    const float run_ref = (P.gait_run > EPS) ? P.gait_run : 3.f;
    const bool has_sprint = (P.gait_sprint > run_ref + EPS);
    const float up = 1.05f, down = 0.85f;

    u8 gear = P.gait_gear;
    if (0 == gear && needed > walk_ref * up)
        gear = 1;
    else if (1 == gear && needed > run_ref * up && has_sprint)
        gear = 2;
    else if (2 == gear && needed < run_ref * down)
        gear = 1;
    else if (1 == gear && needed < walk_ref * down)
        gear = 0;

    P.gait_gear = gear;
    P.running = (gear >= 1);
    P.sprinting = (2 == gear);

    // ⭐Tell the client this body is on the sprint tariff. It does not pair the
    // cycle from it (the measured velocity already does that -- see the note in
    // CLegsBlendSpace::Build); it is the server stating what it is doing, and
    // it is what the intent diagnostic prints.
    if (P.sprinting)
        if (CCustomMonster* M = smart_cast<CCustomMonster*>(GO))
            M->net_note_sprint();

    const float chosen = (2 == gear) ? P.gait_sprint : ((1 == gear) ? P.gait_run : P.gait_walk);
    const float result = (chosen > EPS) ? chosen : P.speed; // no table for this section: the old constant stands

    // ⭐The gear, named out loud, and only when it changes. Without this the
    // only evidence of which gear is in force is the speed itself, and reading
    // a gear off a speed is how the 18-metre threshold stayed broken while
    // looking correct -- "no sprint samples" and "sprint fires but is slow"
    // produce the same log.
    if (P.gait_reported != gear)
    {
        P.gait_reported = gear;
        static const char* names[] = {"walk", "run", "SPRINT"};
        Msg("~ [sv_npc] %u gait -> %s at %.2f m/s (needs %.2f, %.1fm to cover, target doing %.2f m/s)", P.id, names[gear], result, needed, remaining, P.follow_speed);
    }
    return result;
}

void assert_position(CGameObject* GO, const Fvector& pos, float yaw)
{
    if (g_sv_npc_own_motion)
    {
        GO->net_ServerDriveTo(pos, yaw);
        return;
    }

    // The A/B that proved it: transform only, creature keeps its own position.
    Fmatrix F;
    F.rotateY(yaw);
    F.c = pos;
    GO->net_MoveTo(F);
}

// The online game object for an id, if it has one. Puppets are only
// drivable while online -- ForceTransform is a client-side object call, and
// an offline entity has no bones, no spatial and nobody watching it.
CGameObject* online(u16 id)
{
    if (!g_pGameLevel || id == 0xffff)
        return nullptr;
    return smart_cast<CGameObject*>(Level().Objects.net_Find(id));
}

// The level-graph vertex for a position.
//
// ⚠⚠There are three lookups in CLevelGraph and picking the wrong one is
// expensive in two different ways:
//
//   vertex_id(pos)            std::lower_bound on the packed x,z. O(log N),
//                             correct for ANY position. This is the one.
//   vertex_id(hint, pos)      trusts `hint`: if the position is not in it,
//                             it searches only that vertex's NEIGHBOURS.
//                             Fine for "where did this thing step to", wrong
//                             for anywhere else -- aiming a puppet 50m away
//                             with this returned a vertex next to the puppet,
//                             and the route collapsed to two vertices.
//   nearest_vertex_id(pos)    a LINEAR SCAN of every vertex in the level. The
//                             engine's own comment calls it a "very slow full
//                             search"; on Pripyat it costs ~0.5s. Calling it
//                             once per puppet per tick took the server from
//                             213 fps to 1.8.
//
// So: the cheap correct one first, the hint only as recovery when there is
// no vertex under the point at all, and the scan as a last resort.
// Per-tick: where a puppet just stepped to. Cheap paths only -- this runs
// for every puppet every frame and must never reach the scan.
u32 vertex_near(const Fvector& pos, u32 hint)
{
    if (!ai().get_level_graph())
        return u32(-1);
    const CLevelGraph& G = ai().level_graph();

    const u32 v = G.vertex_id(pos);
    if (G.valid_vertex_id(v))
        return v;

    // Nothing under the point -- a gap in the mesh, or a step that clipped an
    // edge. The puppet came from somewhere, so walk out from there.
    if (G.valid_vertex_id(hint))
        return G.vertex_id(hint, pos);

    return u32(-1); // keep the previous height rather than pay for a scan
}

// One-off: an arbitrary point somebody typed or picked, which may be nowhere
// near the navmesh.
//
// ⚠This is where the scan is allowed, and it has to be: falling back to the
// hinted lookup here is what made "walk 50m over there" return a vertex next
// to the puppet and collapse the route to two vertices. Wrong once per order
// is worse than slow once per order.
u32 vertex_anywhere(const Fvector& pos)
{
    if (!ai().get_level_graph())
        return u32(-1);
    const CLevelGraph& G = ai().level_graph();

    const u32 v = G.vertex_id(pos);
    if (G.valid_vertex_id(v))
        return v;

    return G.nearest_vertex_id(pos); // off the mesh: find the real nearest
}

// Put a position on the floor. The AI graph is what the level actually
// considers walkable, so its plane is a better answer than a raycast into
// whatever geometry happens to be under the point -- and it is the same
// height the stock AI would have walked at. `hint` is updated in place so
// the next call is cheap.
Fvector snap_to_floor(const Fvector& pos, u32& hint)
{
    Fvector out = pos;
    const u32 v = vertex_near(pos, hint);
    if (ai().get_level_graph() && ai().level_graph().valid_vertex_id(v))
    {
        hint = v;
        out.y = ai().level_graph().vertex_plane_y(v, pos.x, pos.z);
    }
    return out;
}

// The same, for a point that may be anywhere. Used when an order names a
// destination rather than when a puppet takes a step.
//
// ⚠If the point is off the navmesh the destination becomes the nearest
// walkable vertex, not the point itself. Keeping the literal point means a
// puppet walks to the edge of the mesh and then spends the rest of its life
// unable to arrive, sliding off the floor as it tries -- which is exactly
// what a test destination picked off a map produced.
Fvector snap_anywhere(const Fvector& pos, u32& out_vertex, bool& moved)
{
    moved = false;
    out_vertex = u32(-1);
    if (!ai().get_level_graph())
        return pos;

    const CLevelGraph& G = ai().level_graph();

    out_vertex = G.vertex_id(pos);
    if (G.valid_vertex_id(out_vertex))
    {
        Fvector out = pos; // on the mesh: keep exactly where they were sent
        out.y = G.vertex_plane_y(out_vertex, pos.x, pos.z);
        return out;
    }

    out_vertex = G.nearest_vertex_id(pos);
    if (!G.valid_vertex_id(out_vertex))
        return pos;

    moved = true;
    return G.vertex_position(out_vertex);
}

// Ask the engine's own A* for a route, over the same level graph the stock
// AI walks. Reusing it rather than writing a search means a puppet goes
// where an NPC could have gone -- doors, stairs and all -- instead of where
// the geometry happens to allow a straight line.
bool build_path(Puppet& P, const Fvector& from, const Fvector& to)
{
    P.path.clear();
    P.path_at = 0;
    P.path_goal = to;
    P.path_failed = true;
    P.path_valid = true; // a search was run for this goal, whatever it found

    if (!ai().get_level_graph())
        return false;

    // Start from the puppet's own vertex (cheap); the destination is an
    // arbitrary point and gets the correct lookup even if it costs (see
    // vertex_anywhere -- getting this backwards collapsed every route).
    const u32 v_from = vertex_near(from, P.at_vertex);
    if (ai().level_graph().valid_vertex_id(v_from))
        P.at_vertex = v_from;

    // Reuse the vertex the order already resolved; only pay for a fresh
    // lookup when the goal has moved (a Follow whose target walked off).
    u32 v_to = P.dest_vertex;
    if (!ai().level_graph().valid_vertex_id(v_to))
    {
        v_to = vertex_anywhere(to);
        P.dest_vertex = v_to;
    }

    if (!ai().level_graph().valid_vertex_id(v_from) || !ai().level_graph().valid_vertex_id(v_to))
        return false;

    if (v_from == v_to)
    {
        // Already on the destination's vertex: no route to walk, just close
        // the last metre in a straight line.
        P.path_failed = false;
        return true;
    }

    GraphEngineSpace::CBaseParameters params(PATH_MAX_RANGE, PATH_MAX_ITERATIONS, PATH_MAX_VISITED);
    if (!ai().graph_engine().search(ai().level_graph(), v_from, v_to, &P.path, params))
    {
        P.path.clear();
        Msg("! [sv_npc] %u found no route from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f) -- walking straight at it", P.id, VPUSH(from), VPUSH(to));
        return false;
    }

    // A fresh route starts at the vertex NEAREST the puppet, and that vertex
    // regularly lies BEHIND it -- graph vertices sit ~a metre apart. Walking
    // to it first meant every re-route began with a metre-scale step
    // backwards, and a Follow re-routes every couple of seconds while its
    // target moves: on the remote client that read as periodic micro-freezes
    // ("прикольные фризики" from play, a 1.1 m rewind in the snapshot dump).
    // Skip leading waypoints that do not take the path forward.
    while (P.path_at + 1 < P.path.size())
    {
        const Fvector w0 = ai().level_graph().vertex_position(P.path[P.path_at]);
        const Fvector w1 = ai().level_graph().vertex_position(P.path[P.path_at + 1]);
        Fvector seg;
        seg.sub(w1, w0);
        seg.y = 0.f;
        Fvector rel;
        rel.sub(w0, from);
        rel.y = 0.f;
        // underfoot, or reaching it means moving against the path's own
        // onward direction
        if (rel.magnitude() < 0.5f || seg.dotproduct(rel) < 0.f)
            ++P.path_at;
        else
            break;
    }

    // Worth a line: it says where the puppet believed it was starting from,
    // which is the first thing in doubt when it ends up somewhere unexpected.
    Msg("~ [sv_npc] %u routed from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f): %u vertices (lead skip %u)", P.id, VPUSH(from), VPUSH(to), u32(P.path.size()), P.path_at);

    P.path_failed = false;
    return true;
}

// Give a freshly spawned creature a faction.
//
// CSE_ALifeTraderAbstract resolves m_community_index from the character
// profile, but ONLY while it is still NO_COMMUNITY_INDEX -- so setting it
// after the spawn wins, and it wins before the entity has gone online or
// been sent to anybody, which is what makes this stick.
//
// ⚠The name has to be one of game_relations/communities. An unknown one
// used to take the game down with it (a "loner" attempt did, during the
// settlement work), so this resolves with the no-assert overload and
// reports instead.
bool set_community(CSE_Abstract* obj, const char* name)
{
    CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(obj);
    if (!trader)
    {
        Msg("! [sv_npc] '%s' is not a trader-capable creature -- it has no faction to set", obj ? obj->s_name.c_str() : "?");
        return false;
    }

    const CHARACTER_COMMUNITY_INDEX idx = CHARACTER_COMMUNITY::IdToIndex(name, NO_COMMUNITY_INDEX, true);
    if (NO_COMMUNITY_INDEX == idx)
    {
        Msg("! [sv_npc] '%s' is not a community in game_relations/communities", name);
        return false;
    }

    trader->m_community_index = idx;

    // s_team is what the relation system actually compares in combat, and
    // the profile path sets the two together -- leaving it behind would give
    // a Duty guard that reads as Duty in the UI and shoots like a loner.
    CHARACTER_COMMUNITY comm;
    comm.set(idx);
    if (CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(obj))
        creature->s_team = comm.team();

    return true;
}

// `hint` in/out, as everywhere here: a spawn near a known vertex must not
// pay for a full-level scan either -- four of them in a row is what made
// sv_npc_spawn take over a second per creature.
bool graph_ids(const Fvector& pos, u32& level_vertex, GameGraph::_GRAPH_ID& game_vertex, u32& hint)
{
    if (!ai().get_level_graph() || !ai().get_cross_table())
        return false;

    // Correctness over cost: a creature placed on a vertex that is not really
    // under it is a creature standing in a wall, and this runs once per spawn.
    level_vertex = vertex_anywhere(pos);
    if (!ai().level_graph().valid_vertex_id(level_vertex))
        return false;
    hint = level_vertex;

    game_vertex = ai().cross_table().vertex(level_vertex).game_vertex_id();
    return true;
}
} // namespace

u16 SVNpc::ReferenceEntity()
{
    xrServer* SV = SVWorld::Server();
    if (!SV)
        return 0xffff;

    // A connected network client first: when somebody is playing, "the
    // player" means them, not the host's own fused entity.
    u16 local = 0xffff;
    for (u32 i = 0; i < SV->client_Count(); ++i)
    {
        xrClientData* C = (xrClientData*)SV->client_Get(i);
        if (!C || !C->owner)
            continue;
        if (C->flags.bLocal)
        {
            local = C->owner->ID;
            continue;
        }
        if (C->flags.bConnected)
            return C->owner->ID;
    }
    return local;
}

void SVNpc::Spawn(const char* section, int count, const char* community)
{
    const u16 ref = ReferenceEntity();
    CSE_Abstract* anchor = SVWorld::Find(ref);
    if (!anchor)
    {
        Msg("! [sv_npc] nobody to spawn next to -- no player entity exists yet");
        return;
    }

    xr_vector<u16> ignored;
    SpawnAt(section, count, community, anchor->position(), ignored);
}

void SVNpc::SpawnAt(const char* section, int count, const char* community, const Fvector& origin, xr_vector<u16>& out_ids)
{
    out_ids.clear();

    LPCSTR sect = (section && section[0]) ? section : DEFAULT_SECTION;
    if (!pSettings->section_exist(sect))
    {
        Msg("! [sv_npc] no such section '%s'", sect);
        return;
    }
    if (count < 1)
        count = 1;

    CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());
    if (!alife)
    {
        Msg("! [sv_npc] no A-Life simulator -- is a level loaded?");
        return;
    }

    // One proper lookup for the spawn point, then every body around it is
    // found from that vertex: a squad costs one graph search, not one per body.
    u32 hint = vertex_anywhere(origin);

    u32 spawned = 0;
    for (int i = 0; i < count; ++i)
    {
        // Ring them around the anchor rather than stacking them on one
        // point: several bodies in the same spot is how the physics layer
        // gets asked to resolve a pile it cannot (see the crowd-gate work).
        const float ang = (PI_MUL_2 * float(i)) / float(count);
        const float radius = 3.f + 1.5f * float(i / 8);
        Fvector pos = origin;
        pos.x += radius * _cos(ang);
        pos.z += radius * _sin(ang);
        pos = snap_to_floor(pos, hint);

        u32 lvid = 0;
        GameGraph::_GRAPH_ID gvid = GameGraph::_GRAPH_ID(-1);
        if (!graph_ids(pos, lvid, gvid, hint))
        {
            Msg("! [sv_npc] '%s' has no AI graph vertex near (%.1f, %.1f, %.1f) -- cannot place a creature there", sect, VPUSH(pos));
            continue;
        }

        CSE_Abstract* obj = alife->spawn_item(sect, pos, lvid, gvid, ALife::_OBJECT_ID(-1));
        if (!obj)
        {
            Msg("! [sv_npc] spawn of '%s' failed", sect);
            continue;
        }

        const bool factioned = (community && community[0]) ? set_community(obj, community) : false;

        {
            // Only the roster mutation is locked -- spawn_item above is engine
            // work measured in milliseconds and the server tick is waiting.
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            s_puppets.push_back(Puppet());
            s_puppets.back().id = obj->ID;
            s_puppets.back().target = pos;
            s_puppets.back().at_vertex = lvid; // seeded, so its first tick is cheap too
        }
        out_ids.push_back(obj->ID);
        ++spawned;

        Msg("~ [sv_npc] spawned '%s' id=%u at (%.1f, %.1f, %.1f)%s%s", sect, obj->ID, VPUSH(pos), factioned ? " as " : "", factioned ? community : "");
    }

    Msg("~ [sv_npc] %u of %d spawned; %u puppets tracked", spawned, count, u32(s_puppets.size()));
}

u32 SVNpc::SetVisual(const xr_vector<u16>& ids, const char* visual)
{
    // ⭐Resolved BEFORE anybody is touched. A typo that changed half a squad and
    // then refused the rest would leave a mixed group nobody ordered -- the same
    // rule sv_npc_loadout follows when it parses its list before stripping.
    string_path model;
    if (!SVVisualArmour::Resolve(visual, model))
        return 0;

    const shared_str vis(model);
    const SVVisualArmour::Model& m = SVVisualArmour::For(vis);

    u32 changed = 0, online = 0, refused = 0;
    for (u16 id : ids)
    {
        CSE_Abstract* e = SVWorld::Find(id);
        if (!e)
        {
            Msg("! [sv_npc] no entity %u", id);
            ++refused;
            continue;
        }

        CSE_Visual* v = e->visual();
        if (!v)
        {
            Msg("! [sv_npc] %u '%s' has no visual to change", id, e->s_name.c_str());
            ++refused;
            continue;
        }

        // ⭐The ENTITY first, always. It is what a spawn packet is written from,
        // so a client joining a minute later sees the new body by the ordinary
        // path -- and it is what SVDamage reads the armour off, so the server's
        // arithmetic and the player's eyes cannot drift apart.
        v->set_visual(model);
        ++changed;

        CSE_ALifeDynamicObject* dyn = smart_cast<CSE_ALifeDynamicObject*>(e);
        if (!dyn || !dyn->m_bOnline)
            continue;

        ++online;

        // The host's own copy is a real object with a real skeleton; it rebuilds
        // itself the same way a remote one does. Done here rather than left to
        // the broadcast, which the host deliberately ignores (Level().Server).
        if (CObject* O = Level().Objects.net_Find(id))
            if (CGameObject* GO = smart_cast<CGameObject*>(O))
                GO->OnServerVisualChange(shared_str(model));

        // ...and everybody who is already drawing it. GE_NPC_VISUAL exists
        // because visual_name only ever travels inside the spawn packet, and
        // nothing re-sends that to a client which already holds a copy.
        if (xrServer* SV = SVWorld::Server())
        {
            NET_Packet P;
            P.w_begin(M_EVENT);
            P.w_u32(Level().timeServer());
            P.w_u16(GE_NPC_VISUAL);
            P.w_u16(id);
            P.w_stringZ(model);
            SV->SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
        }
    }

    // ⭐Say what the armour actually became. "Changed 5 models" is not the thing
    // anybody wanted to know -- the reason to change a model here is protection,
    // and leaving that to be looked up separately is how it goes unchecked.
    if (m.found)
    {
        const SVDamage::BoneArmour torso = SVDamage::ReadProtection(m.protection.c_str(), "bip01_spine");
        const SVDamage::BoneArmour head = SVDamage::ReadProtection(m.protection.c_str(), "bip01_head");
        Msg("~ [sv_npc] %u creatures now wear '%s' -> protection '%s': torso %.2f, head %.2f, hit_fraction %.2f", changed, model, m.protection.c_str(), torso.armour, head.armour,
            torso.hit_fraction);
    }
    else
        Msg("! [sv_npc] %u creatures now wear '%s' -- this model declares NO protection, they are unarmoured", changed, model);

    if (refused)
        Msg("! [sv_npc] %u refused", refused);

    if (online)
        Msg("~ [sv_npc] %u of them were ONLINE -- re-dressed in place and announced to every client", online);

    return changed;
}

void SVNpc::List()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);

    const float hz = (s_tick_ms > 0) ? (1000.f * float(s_ticks) / float(s_tick_ms)) : 0.f;
    // ⚠A flat "time was lost" threshold cries wolf now that the hitch guard is
    // tight enough to actually catch a hitch: every 0.7 s gap drops ~0.6 s by
    // design. What matters is the FRACTION -- how much slower the puppets walk
    // than they were asked to -- so print the number and judge it against the
    // run, not against a constant.
    const float elapsed = float(s_tick_ms) / 1000.f;
    const float lost_pct = (elapsed > EPS) ? (100.f * s_dt_lost / elapsed) : 0.f;
    Msg("~ [sv_npc] %u puppets (reference entity for 'actor' = %u); server tick %.1f Hz over %u ticks | %.2f s dropped by the hitch guard (%.1f%%)%s",
        u32(s_puppets.size()), ReferenceEntity(), hz, s_ticks, s_dt_lost, lost_pct, (lost_pct > 5.f) ? " -- WARNING: puppets walk noticeably slower than asked" : "");

    for (const Puppet& P : s_puppets)
    {
        CSE_Abstract* e = SVWorld::Find(P.id);
        CGameObject* GO = online(P.id);

        // Whether it is online is the first thing to know: an offline puppet
        // takes orders but nothing will move, and it is invisible to clients.
        string128 what;
        switch (P.order)
        {
        case Order::Goto: xr_sprintf(what, "goto (%.1f, %.1f, %.1f)", VPUSH(P.target)); break;
        case Order::Follow: xr_sprintf(what, "follow %u", P.follow); break;
        case Order::Patrol: xr_sprintf(what, "patrol %u/%u stops", P.stop_at + 1, u32(P.stops.size())); break;
        case Order::Guard:
            xr_sprintf(what, "guard (%.1f, %.1f, %.1f) leash %.0fm%s", VPUSH(P.anchor), P.leash, P.hauling ? " FETCHING" : "");
            break;
        default: xr_strcpy(what, "hold"); break;
        }

        // The route is worth showing: "straight" next to an order that should
        // have found a way around something is the symptom of a failed search,
        // and it is otherwise indistinguishable from working.
        string64 route;
        if (!P.path.empty())
            xr_sprintf(route, "route %u/%u", P.path_at, u32(P.path.size()));
        else
            xr_strcpy(route, P.path_failed ? "NO ROUTE (straight)" : "straight");

        CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(e);
        LPCSTR faction = (trader && trader->m_community_index != NO_COMMUNITY_INDEX) ? trader->CommunityName() : "-";

        Fvector pos;
        pos.set(0.f, 0.f, 0.f);
        if (GO)
            pos = GO->Position();
        else if (e)
            pos = e->position();

        // ⚠`external` next to `driven` is the whole point of keeping both: the
        // two together say who owns this creature's position. A puppet showing
        // driven=0.8 external=41 is not walking anywhere, whatever the order
        // line claims.
        string32 stuck = {};
        if (P.stuck_events)
            xr_sprintf(stuck, " stuck=%u", P.stuck_events);

        // ⚠Whether it is still ALIVE, which this line used to leave out entirely.
        // Three corpses were listed here as "online ... hold", indistinguishable
        // from three creatures standing around waiting for orders, while
        // sv_npc_gear said DEAD about the same three. Two censuses of one roster
        // disagreeing is worse than either being wrong: it makes whichever one
        // you happened to read the answer.
        CSE_ALifeCreatureAbstract* cr = smart_cast<CSE_ALifeCreatureAbstract*>(e);
        const bool dead = cr && cr->g_Health() <= 0.f;

        // `orders` = movement orders handed to a steered stalker (SVNpc::Steer).
        // A handful per goal is healthy; hundreds means the order is being
        // rewritten faster than the creature can walk it.
        Msg("~ [sv_npc]   id=%u '%s' [%s] %s%s pos=(%.1f, %.1f, %.1f) speed=%.1f %s | %s carrying=%u driven=%.1fm external=%.1fm/%u orders=%u%s", P.id,
            e ? e->s_name.c_str() : "?", faction, dead ? "DEAD " : "", GO ? "online" : "OFFLINE", VPUSH(pos), P.speed, what, route, e ? u32(e->children.size()) : 0, P.driven,
            P.external, P.external_events, P.steer_version, stuck);
    }
}

void SVNpc::Remove(u16 id, bool all)
{
    // Snapshot first, destroy after: u_EventSend goes through the server's
    // packet path and is not something to hold the tick's lock across.
    xr_vector<u16> doomed;
    u32 left = 0;
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        for (auto it = s_puppets.begin(); it != s_puppets.end();)
        {
            if (!all && it->id != id)
            {
                ++it;
                continue;
            }
            doomed.push_back(it->id);
            it = s_puppets.erase(it);
            if (!all)
                break;
        }
        left = u32(s_puppets.size());
    }

    u32 removed = 0;
    xr_vector<PendingDel> pending;
    for (u16 dead : doomed)
    {
        if (!SVWorld::Find(dead))
            continue;

        // Through the real GE_DESTROY event rather than by calling the
        // server's destroy handler directly: that handler asserts on the
        // sender being a known client, and going down the normal channel
        // also gets the removal broadcast to everyone watching.
        NET_Packet P;
        CGameObject::u_EventGen(P, GE_DESTROY, dead);
        CGameObject::u_EventSend(P);
        ++removed;

        // ⭐⭐ASK, THEN CHECK. Measured 05-08-2026: this command reported
        // "removed 23; 0 left" two milliseconds after being typed, and one of
        // those twenty-three was still walking around and thinking ten minutes
        // later -- its ALife record and server entity gone, its level object
        // never told. The line above is a REQUEST posted into the event queue;
        // it was being read as an outcome.
        //
        // ⚠What made that expensive is that nothing downstream could name it:
        // the roster had already forgotten the creature, the entity map never
        // had it, and the only symptom was an anonymous fault storm in the AI
        // (see CStalkerPropertyEvaluatorSmartTerrainTask::evaluate). So the
        // despawn now watches its own outcome and says the id out loud.
        pending.push_back({dead, Device.dwTimeGlobal + 3000});
    }

    // Under the lock, and after the sends: the checker runs on the server tick
    // thread, the same split every other roster access here observes.
    if (!pending.empty())
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        s_pending_del.insert(s_pending_del.end(), pending.begin(), pending.end());
    }

    Msg("~ [sv_npc] removed %u; %u left", removed, left);
}

// Did the world actually do what the despawn asked? Called from Update, which
// is the only thing that keeps running after the roster is empty.
//
// ⚠Reports, does not repair. Destroying the leftover from here would mean
// touching the object list from the server tick's pool thread, and -- more to
// the point -- an automatic sweep would hide the defect it is evidence of. The
// crash the leftover used to cause is guarded now; what is still wanted is one
// live sighting with an id attached.
void SVNpc::VerifyRemovals()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    if (s_pending_del.empty())
        return;

    const u32 now = Device.dwTimeGlobal;
    for (auto it = s_pending_del.begin(); it != s_pending_del.end();)
    {
        if (now < it->due_ms)
        {
            ++it;
            continue;
        }

        const u16 id = it->id;
        it = s_pending_del.erase(it);

        // ⚠⚠Walk the objects; do NOT ask map_NETID. The zombie this check exists
        // to catch is precisely the one that may not be IN map_NETID: if its id
        // slot was taken over by another object (see CObjectList::net_Register),
        // net_Find hands back the newcomer -- or nothing at all -- and this
        // check would have cheerfully reported "gone, the normal case" while
        // the zombie walked past it. An instrument blind to its own quarry is
        // worse than no instrument: it produces evidence of absence.
        CObject* O = nullptr;
        for (u32 i = 0, n = Level().Objects.o_count(); i < n && !O; ++i)
        {
            CObject* C = Level().Objects.o_get_by_iterator(i);
            if (C && C->ID() == id)
                O = C;
        }

        if (!O)
            continue; // gone -- the normal case

        // ⚠Marked is NOT the same as gone, and three seconds is about 180
        // frames: the one-frame window between setDestroy and the sweep has
        // long closed. An object still standing here with the flag set was
        // told and did not go, which is a different defect from never being
        // told -- and it looks identical from the outside, because a marked
        // object keeps its scheduler registration and goes on thinking.
        if (O->getDestroy())
        {
            Msg("!! [sv_npc] %u '%s' was marked for destruction 3 s ago and is STILL in the level -- the sweep did not take it", u32(id), O->cName().c_str());
            continue;
        }

        // Which of the two ways it survived: never told, or told through a slot
        // that no longer pointed at it.
        const bool in_netmap = (Level().Objects.net_Find(id) == O);

        if (SVWorld::Find(id))
        {
            // Entity still there too: the destroy was refused outright rather
            // than half-applied. Different defect, still worth saying.
            Msg("!! [sv_npc] %u '%s' was asked to despawn 3 s ago and is still fully present -- the destroy was refused", u32(id), O->cName().c_str());
            continue;
        }

        Msg("!! [sv_npc] %u '%s' OUTLIVED ITS OWN DESPAWN: server entity gone, object still running and scheduled%s. This is the 05-08 orphan; see sv_orphans.", u32(id),
            O->cName().c_str(), in_netmap ? ", and it still owns its id slot (so the destroy was never delivered to it)" : ", and ANOTHER object holds its id slot (its destroy went there instead)");
    }
}

void SVNpc::Teleport(u16 id, const Fvector& pos)
{
    // Resolved once, for everybody: it is one point, and it is an arbitrary
    // one, so it gets the lookup that is right rather than the one that is fast.
    u32 dest_vertex = u32(-1);
    bool moved = false;
    const Fvector at = snap_anywhere(pos, dest_vertex, moved);

    const u32 n = for_each(id, [&](Puppet& P) {
        P.at_vertex = dest_vertex;
        P.order = Order::Hold;
        P.target = at;
        P.path.clear();
        P.path_failed = false;
        P.path_valid = false;
        P.dest_vertex = u32(-1);
        P.asserted_valid = false; // we are the ones moving it this time
        P.order_dist = 0.f; // a new journey, measured from wherever it lands

        if (CGameObject* GO = online(P.id))
        {
            Fmatrix F = GO->XFORM();
            F.c = at;
            GO->ForceTransform(F);
        }
        else if (CSE_Abstract* e = SVWorld::Find(P.id))
            e->position() = at;
    });

    if (n)
        Msg("~ [sv_npc] %u moved to (%.1f, %.1f, %.1f)", n, VPUSH(pos));
}

void SVNpc::OrderGoto(u16 id, const Fvector& pos)
{
    // One destination, resolved once and properly -- see snap_anywhere.
    u32 dest_vertex = u32(-1);
    bool moved = false;
    const Fvector at = snap_anywhere(pos, dest_vertex, moved);
    if (!ai().get_level_graph() || !ai().level_graph().valid_vertex_id(dest_vertex))
    {
        Msg("! [sv_npc] (%.1f, %.1f, %.1f) has no walkable vertex anywhere near it", VPUSH(pos));
        return;
    }
    if (moved)
        Msg("~ [sv_npc] (%.1f, %.1f, %.1f) is off the navmesh -- walking to (%.1f, %.1f, %.1f) instead", VPUSH(pos), VPUSH(at));

    const u32 n = for_each(id, [&](Puppet& P) {
        P.order = Order::Goto;
        P.target = at;
        P.follow = 0xffff;
        P.path.clear();
        P.path_failed = false;
        P.path_valid = false; // a new order deserves a fresh search
        P.dest_vertex = dest_vertex; // already resolved above, once for everybody
        P.order_dist = 0.f;
    });

    if (n)
        Msg("~ [sv_npc] %u walking to (%.1f, %.1f, %.1f)", n, VPUSH(pos));
}

void SVNpc::OrderFollow(u16 id, u16 target)
{
    if (target == 0xffff)
        target = ReferenceEntity();
    if (target == 0xffff)
    {
        Msg("! [sv_npc] nothing to follow -- no player entity exists");
        return;
    }

    const u32 n = for_each(id, [&](Puppet& P) {
        if (P.id == target)
            return; // nothing follows itself
        P.order = Order::Follow;
        P.follow = target;
        P.path.clear();
        P.path_failed = false;
        P.path_valid = false;
        P.dest_vertex = u32(-1); // a moving target: resolved on each replan
        P.order_dist = 0.f;
    });

    if (n)
        Msg("~ [sv_npc] %u following %u", n, target);
}

void SVNpc::OrderPatrol(u16 id, const xr_vector<Fvector>& stops, float dwell)
{
    if (stops.empty())
    {
        Msg("! [sv_npc] a patrol needs at least one stop");
        return;
    }

    // Every stop is resolved to the navmesh ONCE, here, for everybody -- the
    // full-level scan a point off the mesh costs is affordable per order and
    // ruinous per tick.
    xr_vector<Fvector> snapped;
    snapped.reserve(stops.size());
    for (const Fvector& s : stops)
    {
        u32 v = u32(-1);
        bool moved = false;
        const Fvector at = snap_anywhere(s, v, moved);
        if (!ai().get_level_graph() || !ai().level_graph().valid_vertex_id(v))
        {
            Msg("! [sv_npc] patrol stop (%.1f, %.1f, %.1f) has no walkable vertex near it -- dropping it", VPUSH(s));
            continue;
        }
        if (moved)
            Msg("~ [sv_npc] patrol stop (%.1f, %.1f, %.1f) is off the navmesh -- using (%.1f, %.1f, %.1f)", VPUSH(s), VPUSH(at));
        snapped.push_back(at);
    }

    if (snapped.empty())
    {
        Msg("! [sv_npc] none of those stops are anywhere a creature can walk");
        return;
    }

    // Start each puppet at a DIFFERENT stop. A squad given one route otherwise
    // walks it as a single clump, which covers the route no better than one
    // creature would and looks like a bug.
    u32 seed = 0;
    const u32 n = for_each(id, [&](Puppet& P) {
        P.order = Order::Patrol;
        P.stops = snapped;
        P.stop_at = seed++ % u32(snapped.size());
        P.dwell = dwell;
        P.dwell_until = 0;
        P.follow = 0xffff;
        P.path.clear();
        P.path_failed = false;
        P.path_valid = false;
        P.dest_vertex = u32(-1);
        P.order_dist = 0.f;
        P.asserted_valid = false;
    });

    if (n)
        Msg("~ [sv_npc] %u patrolling %u stops, pausing %.0fs at each", n, u32(snapped.size()), dwell);
}

void SVNpc::OrderGuard(u16 id, const Fvector& anchor, float leash)
{
    u32 v = u32(-1);
    bool moved = false;
    const Fvector at = snap_anywhere(anchor, v, moved);
    if (!ai().get_level_graph() || !ai().level_graph().valid_vertex_id(v))
    {
        Msg("! [sv_npc] (%.1f, %.1f, %.1f) has no walkable vertex anywhere near it", VPUSH(anchor));
        return;
    }
    if (leash <= 0.f)
        leash = DEFAULT_LEASH;

    // ⚠Every guard gets its OWN spot around the post, never the same one.
    //
    // Sending a detail to a single coordinate looks harmless and is not: two
    // creatures fetched to the same point cannot both be there, so the physics
    // shoves them apart about a metre at a time, each shove puts one back
    // outside the radius that would have released it, and the pair spend the
    // rest of the session walking into each other three metres from a post
    // neither can reach. Measured, on the first two-guard run: sixteen shove
    // events and five replans in seventy seconds.
    u32 total = 0;
    for_each(id, [&](Puppet&) { ++total; });
    if (!total)
        return;

    // Comfortably inside the release radius, so a guard on its spot is a guard
    // the server has let go of rather than one it is still fetching.
    const float ring = (total > 1) ? std::min(3.f, leash * LEASH_HYSTERESIS * 0.6f) : 0.f;

    u32 index = 0;
    const u32 n = for_each(id, [&](Puppet& P) {
        Fvector spot = at;
        if (ring > 0.f)
        {
            const float ang = (PI_MUL_2 * float(index)) / float(total);
            spot.x += ring * _cos(ang);
            spot.z += ring * _sin(ang);

            // Cheap snap only: the spot is metres from a vertex we already
            // have, so the hinted lookup finds it, and falling back to the
            // post itself is better than paying for a full-level scan per
            // guard on the rare occasion it does not.
            const u32 sv = vertex_near(spot, v);
            if (ai().level_graph().valid_vertex_id(sv))
                spot.y = ai().level_graph().vertex_plane_y(sv, spot.x, spot.z);
            else
                spot = at;
        }
        ++index;

        P.order = Order::Guard;
        P.anchor = spot;
        P.leash = leash;
        P.hauling = false;
        P.follow = 0xffff;
        P.path.clear();
        P.path_failed = false;
        P.path_valid = false;
        P.dest_vertex = u32(-1);
        P.order_dist = 0.f;
        P.asserted_valid = false;
    });

    if (n)
        Msg("~ [sv_npc] %u guarding (%.1f, %.1f, %.1f) on a %.1fm leash, spread over a %.1fm ring", n, VPUSH(at), leash, ring);
}

void SVNpc::OrderHold(u16 id)
{
    const u32 n = for_each(id, [](Puppet& P) {
        P.order = Order::Hold;
        P.asserted_valid = false;
        // Nothing to release: the server's claim on a creature's position
        // expires by itself a second after the last assertion. A puppet that
        // is supposed to STAY somewhere wants Guard, not Hold -- Hold hands it
        // back to its own brain, which will happily wander off.
    });
    if (n)
        Msg("~ [sv_npc] %u holding", n);
}

void SVNpc::OrderFace(u16 id, const Fvector& at)
{
    u32 turned = 0, already_online = 0, too_close = 0;

    for_each(id, [&](Puppet& P) {
        CSE_Abstract* e = SVWorld::Find(P.id);
        if (!e)
            return;

        Fvector d;
        d.sub(at, e->position());
        d.y = 0.f;
        if (d.magnitude() < 0.5f)
        {
            ++too_close;
            return;
        }

        // The engine's own convention: XFORM().setXYZ(o_Angle) makes the
        // forward vector k = (sin(yaw), 0, cos(yaw)), so this is its inverse.
        const float yaw = atan2f(d.x, d.z);
        e->o_Angle.y = yaw;
        ++turned;

        // Already online: the entity angle is now only what it would respawn
        // with, so shove the transform too. ⚠Best effort by nature -- an online
        // creature rewrites its own transform from its physics capsule every
        // update, and its head is aimed by its own sight manager.
        if (CGameObject* GO = online(P.id))
        {
            ++already_online;
            Fmatrix F;
            F.setXYZ(e->o_Angle);
            F.c = GO->XFORM().c;
            GO->ForceTransform(F);
        }
    });

    if (!turned && !too_close)
        return;

    Msg("~ [sv_npc] %u turned to face (%.1f, %.1f, %.1f)%s%s", turned, VPUSH(at), already_online ? " -- " : "",
        already_online ? "WARNING: some were already online, where their own brain owns where they look; face them in the spawn batch" : "");
    if (too_close)
        Msg("! [sv_npc] %u were already standing on the point and were left alone", too_close);
}

void SVNpc::SetSpeed(u16 id, float mps)
{
    clamp(mps, 0.f, 20.f);
    const u32 n = for_each(id, [&](Puppet& P) {
        P.speed = mps;
        P.speed_explicit = true; // an order to hold this number, gait be damned
    });
    if (n)
        Msg("~ [sv_npc] %u pinned at %.2f m/s (overrides the gait)", n, mps);
}

void SVNpc::Ids(xr_vector<u16>& out)
{
    out.clear();
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    for (const Puppet& P : s_puppets)
        out.push_back(P.id);
}

void SVNpc::IdsOfFaction(const char* community, xr_vector<u16>& out)
{
    out.clear();
    if (!community || !community[0])
        return;

    std::lock_guard<std::recursive_mutex> guard(s_lock);
    for (const Puppet& P : s_puppets)
    {
        CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(SVWorld::Find(P.id));
        if (!trader || trader->m_community_index == NO_COMMUNITY_INDEX)
            continue;
        if (!xr_strcmp(trader->CommunityName(), community))
            out.push_back(P.id);
    }
}

void SVNpc::Give(u16 id, const char* section, bool announce)
{
    if (!section || !section[0] || !pSettings->section_exist(section))
    {
        Msg("! [sv_npc] no such section '%s'", section ? section : "");
        return;
    }

    CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());
    if (!alife)
        return;

    // Who to arm is decided under the lock; the arming itself is a spawn per
    // body and runs outside it.
    xr_vector<u16> targets;
    xr_vector<u32> hints;
    for_each(id, [&](Puppet& P) {
        targets.push_back(P.id);
        hints.push_back(P.at_vertex);
    });

    for (u32 i = 0; i < targets.size(); ++i)
    {
        CSE_Abstract* owner = SVWorld::Find(targets[i]);
        if (!owner)
            continue;

        // Parented at spawn, which is how the settlement layer arms what it
        // populates -- the item never exists loose in the world, so there is
        // no window where somebody else could pick it up first.
        const Fvector pos = owner->position();
        u32 lvid = 0;
        GameGraph::_GRAPH_ID gvid = GameGraph::_GRAPH_ID(-1);
        u32 hint = hints[i];
        if (!graph_ids(pos, lvid, gvid, hint))
        {
            Msg("! [sv_npc] no AI graph vertex near %u to spawn '%s' at", targets[i], section);
            continue;
        }

        CSE_Abstract* item = alife->spawn_item(section, pos, lvid, gvid, owner->ID);
        if (!item)
        {
            Msg("! [sv_npc] spawn of '%s' failed", section);
            continue;
        }

        if (announce)
            Msg("~ [sv_npc] gave %u '%s' (item %u); it now carries %u things", targets[i], section, item->ID, u32(owner->children.size()));
    }
}

void SVNpc::Update()
{
    if (!g_pGameLevel)
        return;

    // See the note on s_lock: this runs on a TTAPI pool thread while the
    // console mutates the roster on the main one.
    std::lock_guard<std::recursive_mutex> guard(s_lock);

    // ⭐Before the empty-roster exit, deliberately: the despawn that needs
    // checking is exactly the one that emptied the roster, and after
    // `sv_npc_del all` this function is the only thing still running.
    VerifyRemovals();

    if (s_puppets.empty())
        return;

    s_watch_id = (g_sv_npc_watch < 0) ? u16(0xffff) : (g_sv_npc_watch == 0 ? s_puppets.front().id : u16(g_sv_npc_watch));

    // ⚠Not Device.fTimeDelta. That is the RENDER frame's delta, and this runs
    // on the server tick, which has its own rate -- on the headless server the
    // two differ by roughly an order of magnitude, so a puppet asked for
    // 1.6 m/s walked at 0.14. Measured here instead, against the same clock
    // the rest of the server bookkeeping uses.
    const u32 now = Device.dwTimeGlobal;
    if (!s_last_update_ms)
    {
        s_last_update_ms = now;
        return;
    }

    float dt = float(now - s_last_update_ms) / 1000.f;
    s_tick_ms += (now - s_last_update_ms);
    ++s_ticks;
    s_last_update_ms = now;
    if (dt <= 0.f)
        return;

    // ⭐⭐A HITCH IS NOT A TELEPORT -- AND AT 3 s THIS GUARD NEVER SAID SO.
    //
    // Measured 30-07 with the wire audit: this server loses roughly 0.7 s of
    // wall clock a few times a minute. sv_perf cannot see it (engine total is
    // 0.9 ms of a 6.0 ms frame, so the loss happens outside its timers) but
    // Device.dwTimeGlobal can, and the drive integrated every millisecond of
    // it in a single step: a 692 ms gap, then one step of 4.235 m -- 706 ms of
    // travel at 6 m/s, matching the gap to 2%. The very next packet carried
    // that 4.235 m with an honest 47 ms stamp, and the client, which did
    // nothing wrong with it (dt == raw), drew 90 m/s.
    //
    // ⭐So the residual "they jerk when they run fast" was never the clock and
    // never the stamp. Across three runs: 0 clock stalls in ~20000 samples,
    // 0 stamp stalls, stamp tracking real time on 250 packets of 250. The wire
    // was faithfully reporting a body that really did move that far.
    //
    // ⚠The bound still may not sit BELOW the real tick spacing or it silently
    // becomes a SPEED LIMIT rather than a safety net -- at 0.25 s a puppet
    // asked for 1.6 m/s walked at 0.36, and read as a physics or animation
    // problem rather than the arithmetic it was. So it is derived from the
    // spacing this server is measured to have rather than picked: a large
    // multiple of a tick (6.2 ms here, so the floor is what binds), and the
    // old value kept as a ceiling for a server slow enough to need it.
    //
    // The time over the bound is DROPPED, not repaid. During the gap nothing
    // else on the server ran either -- no physics, no AI, no damage -- so
    // there is no world for the puppet to be out of step with, and making it
    // sprint afterwards to recover a schedule nobody observed would put the
    // super-step back in by another door. s_dt_lost keeps the cost visible.
    const float mean_tick = s_ticks ? (float(s_tick_ms) / float(s_ticks) / 1000.f) : 0.f;
    const float hitch = _min(HITCH_CLAMP, _max(HITCH_FLOOR, HITCH_TICKS * mean_tick));
    if (dt > hitch)
    {
        s_dt_lost += dt - hitch;
        dt = hitch;
    }

    for (Puppet& P : s_puppets)
    {
        if (P.order == Order::Hold)
        {
            P.steer = false; // its own creature
            continue;
        }

        CGameObject* GO = online(P.id);
        if (!GO)
            continue; // offline: the order stands, nothing to drive

        CEntityAlive* EA = smart_cast<CEntityAlive*>(GO);
        if (EA && !EA->g_Alive())
        {
            P.order = Order::Hold; // a corpse holds still, and belongs to the physics
            P.steer = false;
            continue;
        }

        // ⭐A human walks on its own legs: its movement manager is told where
        // to go (SVNpc::Steer) and nothing here touches its transform. Only
        // what has no such manager to speak to -- a mutant -- is still carried.
        const bool steered = (nullptr != smart_cast<CAI_Stalker*>(GO));

        const Fvector from = GO->Position();

        Fvector goal = P.target;
        float stop_at = ARRIVE_EPS;

        if (P.order == Order::Follow)
        {
            CSE_Abstract* t = SVWorld::Find(P.follow);
            if (!t)
                continue;
            CGameObject* tgo = online(P.follow);
            goal = tgo ? tgo->Position() : t->position();
            stop_at = FOLLOW_GAP;

            // ⭐⭐HOW FAST THE THING WE ARE CHASING IS GOING.
            //
            // The third gear was first keyed off the GAP, and measurement
            // killed that: chasing a sprinting player at 3.0 m/s, the gap
            // peaked at 7.2 m against a 12 m threshold, so the gear never
            // engaged -- the follower very nearly keeps up, it just never
            // quite catches up. Distance was the wrong question.
            //
            // The right one is the one a person actually asks: is he going
            // faster than I can run? Sampled over a window rather than per
            // tick, for the same reason the intent velocity is -- the drive
            // ticks far faster than a position changes, and a per-tick
            // quotient of that is mostly zeros. See dont-average-quotients.
            {
                const u32 win = now - P.follow_ms;
                if (!P.follow_ms || win > 1000)
                {
                    P.follow_ms = now;
                    P.follow_pos = goal;
                    P.follow_speed = 0.f;
                }
                else if (win >= 200)
                {
                    Fvector d;
                    d.sub(goal, P.follow_pos);
                    d.y = 0.f;
                    P.follow_speed = d.magnitude() / (float(win) / 1000.f);
                    P.follow_ms = now;
                    P.follow_pos = goal;
                }
            }

            // A formation slot instead of one shared goal. Every follower of
            // this target aimed at the same point, and puppets driven along
            // independently planned paths know nothing about each other --
            // four followers walked INTO each other. Slots sit on a ring
            // around the target, assigned by roster order among this
            // target's followers (stable while the roster is).
            u32 slot = 0, nslots = 0;
            for (const Puppet& q : s_puppets)
            {
                if (q.order != Order::Follow || q.follow != P.follow)
                    continue;
                if (q.id == P.id)
                    slot = nslots;
                ++nslots;
            }
            if (nslots > 1)
            {
                const float a = (2.f * PI) * float(slot) / float(nslots);
                goal.x += FOLLOW_GAP * _sin(a);
                goal.z += FOLLOW_GAP * _cos(a);
                stop_at = 0.9f;
            }
        }
        else if (P.order == Order::Patrol)
        {
            if (P.stops.empty())
                continue;
            if (now < P.dwell_until)
            {
                P.steer = false;
                continue; // pausing at a stop: released, so it looks around
            }
            goal = P.stops[P.stop_at % P.stops.size()];
        }
        else if (P.order == Order::Guard)
        {
            goal = P.anchor;

            Fvector away;
            away.sub(goal, from);
            away.y = 0.f;
            const float strayed = away.magnitude();

            if (!P.hauling)
            {
                // ⭐Inside the leash the server does nothing at all, and that is
                // the point: no assertion means the claim on the creature's
                // position lapses within a second and it goes back to being an
                // ordinary stalker -- idling, turning, taking cover, shooting.
                // A guard that the server pins in place every tick is a
                // mannequin standing at a coordinate.
                if (strayed <= P.leash)
                {
                    P.steer = false;
                    continue;
                }

                P.hauling = true;
                P.path_valid = false; // fetch it from wherever it actually got to
                P.dest_vertex = u32(-1);
                P.order_dist = 0.f;
                Msg("~ [sv_npc] %u strayed %.1fm from its post (leash %.1fm) -- fetching it back", P.id, strayed, P.leash);
            }

            // Bring it well inside before letting go again -- see LEASH_HYSTERESIS.
            stop_at = P.leash * LEASH_HYSTERESIS;
        }

        // ⚠Did anybody else move this thing since we last put it somewhere?
        // A creature spawned into the world has other owners -- its own brain,
        // the script logic the level binds to it, the A-Life switch that puts
        // it online, the physics settle -- and every one of them writes the
        // same field we do. `driven` cannot see any of that; it only ever adds
        // what WE claimed. So a puppet that reports arriving after 0.8 m looks
        // identical to one that walked the whole way, and the only way to tell
        // is to check that the position we asserted last tick is still there.
        if (P.asserted_valid)
        {
            Fvector slip;
            slip.sub(from, P.asserted);
            slip.y = 0.f;
            const float slipped = slip.magnitude();

            // ⚠Accumulate ALWAYS, threshold only the reporting. Gating the sum
            // on "is this event big enough to mention" is how the first version
            // of this detector reported external=0.0 for a puppet losing two
            // thirds of its speed: the interference arrived as five millimetres
            // a tick, which is nothing to look at and 1.1 m/s at 200 Hz. Small
            // and constant beats large and rare, and only the sum sees it.
            P.external += slipped;
            if (slipped > EXTERNAL_EPS)
            {
                ++P.external_events;

                // Loud for the first few, then thinned out: this fires per
                // tick when it fires at all, and a 200 Hz tick would bury the
                // log in the thing it is trying to report.
                if (P.external_logged < 8 || (P.external_events % 256) == 0)
                {
                    ++P.external_logged;
                    Msg("! [sv_npc] %u moved by something else: left at (%.1f, %.1f, %.1f), found at (%.1f, %.1f, %.1f) -- %.2fm (event %u, %.1fm total, we drove %.1fm)", P.id,
                        VPUSH(P.asserted), VPUSH(from), slipped, P.external_events, P.external, P.driven);
                }
            }
        }

        // Distance to the real destination decides arrival; the route only
        // decides which way to step towards it.
        Fvector to_goal;
        to_goal.sub(goal, from);
        to_goal.y = 0.f;
        const float dist = to_goal.magnitude();

        // The order's own baseline, taken on its first driven tick rather than
        // when it was given: a puppet that was still offline then had no
        // position to measure from.
        if (P.order_dist <= 0.f)
        {
            P.order_from = from;
            P.order_dist = dist;
            P.closest = dist;
            P.progress_ms = now;
        }
        else if (dist < P.closest - PROGRESS_EPS)
        {
            P.closest = dist;
            P.progress_ms = now;
        }

        if (dist <= stop_at)
        {
            switch (P.order)
            {
            case Order::Goto:
                P.order = Order::Hold;
                P.steer = false;
                P.path.clear();
                P.asserted_valid = false;
                Msg("~ [sv_npc] %u arrived at (%.1f, %.1f, %.1f) -- we drove %.1fm, something else moved it %.1fm over %u events", P.id, VPUSH(goal), P.driven, P.external,
                    P.external_events);
                break;

            case Order::Patrol:
                // On to the next stop, after a pause. The pause matters: a
                // patrol that turns on its heel the instant it arrives reads as
                // clockwork, and standing still for a few seconds is also when
                // the creature's own brain gets to look around.
                Msg("~ [sv_npc] %u reached patrol stop %u of %u, pausing %.0fs", P.id, P.stop_at + 1, u32(P.stops.size()), P.dwell);
                P.stop_at = (P.stop_at + 1) % u32(P.stops.size());
                P.dwell_until = now + u32(P.dwell * 1000.f);
                P.steer = false;
                P.path.clear();
                P.path_valid = false;
                P.dest_vertex = u32(-1);
                P.order_dist = 0.f;
                P.asserted_valid = false;
                break;

            case Order::Guard:
                // Back on post. Release it: from here it is its own creature
                // again until it strays.
                Msg("~ [sv_npc] %u back on post at (%.1f, %.1f, %.1f) -- released, we drove %.1fm", P.id, VPUSH(P.anchor), P.driven);
                P.hauling = false;
                P.steer = false;
                P.path.clear();
                P.path_valid = false;
                P.dest_vertex = u32(-1);
                P.order_dist = 0.f;
                P.asserted_valid = false;
                break;

            default:
                // A Follow just waits for its target to move again rather than
                // jittering against the gap.
                break;
            }
            continue;
        }

        // ⚠A puppet that has spent its whole budget for a while without ever
        // getting nearer is not walking, it is grinding against something the
        // route did not know about -- a door that shut, a creature standing in
        // a corridor, geometry the navmesh is optimistic about. Search again
        // from where it actually is; the alternative is a guard that spends the
        // rest of the session pressed into a wall while every counter says it
        // is walking at 1.6 m/s.
        // ⚠Only when it is meaningfully short of the goal. A puppet settling
        // onto the boundary of its stop radius makes no "progress" either, and
        // reporting that as stuck turns an arrival into a replan loop.
        if ((now - P.progress_ms) > STUCK_MS && dist > stop_at + 1.f)
        {
            ++P.stuck_events;
            P.progress_ms = now;
            P.closest = dist; // re-baseline, or it trips again immediately
            P.path_valid = false;
            P.dest_vertex = u32(-1);
            Msg("! [sv_npc] %u has not got closer to (%.1f, %.1f, %.1f) in %us -- still %.1fm away, replanning (%u so far)", P.id, VPUSH(goal), STUCK_MS / 1000, dist,
                P.stuck_events);

            // ⭐A guard that has failed this many times is not being delayed,
            // its post is unreachable -- and the commonest way for that to
            // happen is subtle: a spot picked on a ring around a field can land
            // on a rooftop or ledge that is perfectly walkable navmesh and has
            // no route up to it from the ground the guard is standing on. It
            // will keep planning routes there for ever.
            //
            // So take the ground it CAN hold. Standing watch a few metres from
            // an ideal mark is a guard; walking into a wall under one is not.
            if (P.order == Order::Guard && P.stuck_events >= STUCK_GIVE_UP)
            {
                Msg("~ [sv_npc] %u cannot reach its post at (%.1f, %.1f, %.1f) -- standing watch at (%.1f, %.1f, %.1f) instead", P.id, VPUSH(P.anchor), VPUSH(from));
                P.anchor = from;
                P.hauling = false;
                P.steer = false;
                P.stuck_events = 0;
                P.path.clear();
                P.asserted_valid = false;
                continue;
            }

            if (steered)
                ++P.steer_version; // "replan" for a creature that plans its own route: order it again
        }

        // ⭐⭐A HUMAN IS TOLD, NOT CARRIED. Everything below this block -- the
        // route walk, the distance budget, the turn-rate bound, the transform
        // assertion -- is this module doing a movement manager's job for a
        // creature that has one. For a stalker that job goes back to its owner:
        // the goal and the gear are decided here, and SVNpc::Steer hands them
        // to the creature's own movement manager, from its own update.
        if (steered)
        {
            drive_speed(P, GO, dist - stop_at, true); // the gear: same decision, same hysteresis

            // A follower walks to a point short of the man it follows -- the
            // gap is the arrival test above, and a movement order to the
            // target's own position would walk straight into him.
            Fvector to = goal;
            if (P.order == Order::Follow && stop_at > 1.f && dist > stop_at)
            {
                Fvector d;
                d.sub(goal, from);
                d.y = 0.f;
                d.mul((dist - stop_at) / dist);
                to.add(from, d);
                to.y = goal.y;
            }

            // ⚠A gear change is worth a new order only once it has held: a
            // follower closing on its slot passes sprint -> run -> walk in a few
            // seconds and hovers on the walk/run edge while its goal slides, and
            // every order is a new path. Measured: 44 orders in two minutes on
            // one follower, most of them gear flips. (A moved goal still counts
            // at once -- going the wrong way is worse than a late change of pace.)
            constexpr u32 STEER_GEAR_HOLD_MS = 700;
            bool gear_changed = (P.steer_gear != P.gait_gear);
            if (!gear_changed)
                P.steer_gear_since = 0;
            else if (!P.steer_gear_since)
            {
                P.steer_gear_since = now;
                gear_changed = false;
            }
            else if (now - P.steer_gear_since < STEER_GEAR_HOLD_MS)
                gear_changed = false;

            Fvector moved;
            moved.sub(to, P.steer_goal);
            if (!P.steer || gear_changed || moved.magnitude() > REPATH_DIST)
            {
                P.steer_gear_since = 0;
                // The vertex under the goal, exactly (O(log N), no scan). Off
                // the mesh, the order's own vertex -- the followed creature's,
                // or the one the order resolved when it was given -- and the
                // goal moves onto it, because a destination outside its vertex
                // is a detail path that never completes.
                const CLevelGraph& G = ai().level_graph();
                u32 v = G.vertex_id(to);
                if (!G.valid_vertex_id(v))
                {
                    if (P.order == Order::Follow)
                        if (CGameObject* tgo = online(P.follow))
                            v = tgo->ai_location().level_vertex_id();
                    if (!G.valid_vertex_id(v))
                        v = P.dest_vertex;
                    if (!G.valid_vertex_id(v))
                        v = vertex_near(to, GO->ai_location().level_vertex_id());
                    if (G.valid_vertex_id(v))
                        to = G.vertex_position(v);
                }
                if (!G.valid_vertex_id(v))
                    continue; // nowhere to send it this tick

                P.steer = true;
                P.steer_goal = to;
                P.steer_vertex = v;
                P.steer_gear = P.gait_gear;
                ++P.steer_version;
            }
            continue;
        }

        // (Re)plan only when no search has been run for this goal, or when a
        // followed target has wandered far enough from what it was aimed at.
        Fvector goal_drift;
        goal_drift.sub(goal, P.path_goal);
        const bool drifted = goal_drift.magnitude() > REPATH_DIST;
        if (!P.path_valid || drifted)
        {
            if (drifted)
                P.dest_vertex = u32(-1); // the goal moved; its vertex is stale
            build_path(P, from, goal);
        }

        // Walk the route as a polyline, spending a distance budget.
        //
        // ⚠It has to consume as many waypoints as the budget covers, not stop
        // at the first one. Level-graph vertices sit about a metre apart, so
        // capping each step at the next vertex silently capped SPEED at one
        // vertex per tick -- which on a slow tick made a puppet asked for
        // 1.6 m/s walk at 0.8, and looked like a physics problem rather than
        // the arithmetic mistake it was.
        float budget = drive_speed(P, GO, dist - stop_at) * dt;
        if (budget > dist - stop_at)
            budget = dist - stop_at;

        Fvector pos = from;
        Fvector heading;
        heading.set(0.f, 0.f, 0.f);

        while (budget > EPS_L && P.path_at < P.path.size())
        {
            const Fvector wp = ai().level_graph().vertex_position(P.path[P.path_at]);
            Fvector d;
            d.sub(wp, pos);
            d.y = 0.f;
            const float leg = d.magnitude();

            if (leg <= WAYPOINT_EPS)
            {
                ++P.path_at; // already there
                continue;
            }

            heading = d;
            if (leg <= budget)
            {
                pos.x = wp.x;
                pos.z = wp.z;
                budget -= leg;
                ++P.path_at;
            }
            else
            {
                d.mul(budget / leg);
                pos.add(d);
                budget = 0.f;
            }
        }

        if (P.path_at >= P.path.size())
            P.path.clear(); // route walked out; the rest is a straight line

        // One line a second while it is under way. `closed` is the number that
        // matters: a puppet spending its whole budget every tick and closing
        // nothing is walking in circles, and no counter says that on its own.
        if (g_sv_npc_trace && (now - P.trace_ms) >= 1000)
        {
            P.trace_ms = now;
            Msg("~ [sv_npc] trace %u at (%.1f, %.1f, %.1f) dist=%.1f (closed %.1f of %.1f) wp %u/%u | driven=%.1f clamped=%.1f external=%.1f | frames/assert=%.2f", P.id,
                VPUSH(from), dist, P.order_dist - dist, P.order_dist, P.path_at, u32(P.path.size()), P.driven, P.clamped, P.external,
                P.frame_samples ? (float(P.frames_between) / float(P.frame_samples)) : 0.f);
        }

        // Whatever budget the route did not use goes straight at the goal --
        // this is also the whole of the movement when there is no route.
        if (budget > EPS_L)
        {
            Fvector d;
            d.sub(goal, pos);
            d.y = 0.f;
            const float leg = d.magnitude();
            if (leg > EPS_L)
            {
                heading = d;
                d.mul((leg <= budget ? leg : budget) / leg);
                pos.add(d);
            }
        }

        Fvector moved;
        moved.sub(pos, from);
        moved.y = 0.f;
        const float step = moved.magnitude();
        if (step < EPS_L)
            continue;

        pos = snap_to_floor(pos, P.at_vertex);
        Fvector delta = heading;

        // Face where it is going. The client reads heading off the transform
        // it receives, so a puppet that never turns walks sideways.
        //
        // rotateY(a) puts the forward axis at (sin a, 0, cos a), so the angle
        // that points it down `delta` is atan2(x, z) -- not the atan2(z, x)
        // that the usual maths convention would suggest.
        //
        // ⚠net_ServerDriveTo -- not ForceTransform, and not net_MoveTo either.
        // All three move an object and only one of them moves a creature the
        // server is driving:
        //   ForceTransform     a TELEPORT; set_movement_position settles the
        //                      capsule through a physics world step, per
        //                      puppet per tick at ~200 Hz.
        //   net_MoveTo         warps the capsule and the transform, which is
        //                      enough for a client puppet nobody simulates --
        //                      but a locally simulated creature rewrites its
        //                      transform from NET_Last every frame, so most of
        //                      the step is gone before the next tick.
        //   net_ServerDriveTo  writes the position where the creature's own
        //                      update reads it from. See the note on it.
        WatchPos(P.id, "svAssert:pre", from);
        const float want_yaw = atan2f(delta.x, delta.z);

        // ⭐Step towards the heading, do not jump to it. `delta` reverses in a
        // single tick when a route doubles back, and the client renders the
        // heading it is sent -- so an unbounded assert here is an instant spin
        // on screen. See TURN_RATE for why the bound is generous.
        //
        // ⚠First tick has nothing to turn from: adopt the heading outright,
        // or every puppet would spend half a second rotating out of whatever
        // direction it happened to spawn facing.
        float assert_yaw = want_yaw;
        if (P.yaw_valid)
        {
            const float most = TURN_RATE * dt;
            const float diff = angle_difference_signed(want_yaw, P.yaw);
            if (_abs(diff) > most)
                assert_yaw = angle_normalize_signed(P.yaw + (diff > 0.f ? most : -most));
        }
        P.yaw = assert_yaw;
        P.yaw_valid = true;

        assert_position(GO, pos, assert_yaw);
        WatchPos(P.id, "svAssert:post", GO->Position());
        // Read back off the object, not from what we asked for: the question
        // this audit answers is what the creature IS, not what it was told.
        SVMotionAudit::Note(P.id, "assert", GO->Position(), assert_yaw, true);
        P.driven += step;

        // Remember it AFTER the move, and read it back off the object rather
        // than trusting what we asked for -- if the engine adjusted the
        // position on the way in, that adjustment is not somebody else moving
        // the puppet and must not be counted as one.
        P.asserted = GO->Position();
        P.asserted_valid = true;

        // ...but do count how much it adjusted, separately. This is the half
        // of the loss that happens inside the move call; `external` is the
        // half that happens after it.
        Fvector refused;
        refused.sub(P.asserted, pos);
        refused.y = 0.f;
        P.clamped += refused.magnitude();

        // How many render frames pass between one assertion and the next. One
        // means we get a say every frame; more means the server tick and the
        // frame that updates this object are not the same thing, and whatever
        // runs in between gets the last word.
        if (P.frame_at_assert)
        {
            P.frames_between += (Device.dwFrame - P.frame_at_assert);
            ++P.frame_samples;
        }
        P.frame_at_assert = Device.dwFrame;
    }
}

void SVNpc::Steer(CAI_Stalker* S)
{
    static const shared_str k_owner = "sv_npc";

    std::lock_guard<std::recursive_mutex> guard(s_lock);

    Puppet* P = find(u16(S->ID()));
    LPCSTR holder = S->GetScriptControlName();
    const bool ours = S->GetScriptControl() && holder && (0 == xr_strcmp(holder, k_owner.c_str()));

    // ⭐What the creature is actually doing with the order, every two seconds
    // per steered creature (sv_npc_trace 1): who holds it, what is queued,
    // whether the movement manager has a path, of which TYPE, to which vertex,
    // and is moving along it. "It was told" and "it walks" are different claims,
    // and only this line tells them apart -- it is what found both the A-Life
    // path creeping back (ptype=0) and the premature "done" (completed=1).
    if (g_sv_npc_trace && P && (P->steer || ours))
    {
        static xr_map<u16, u32> s_next;
        u32& next = s_next[P->id];
        if (Device.dwTimeGlobal >= next)
        {
            next = Device.dwTimeGlobal + 2000;
            CStalkerMovementManager& mv = S->movement();
            const CScriptEntityAction* cur = S->GetCurrentAction();
            Fvector to = P->steer_goal;
            to.sub(S->Position());
            to.y = 0.f;
            const Fvector& dd = mv.detail().dest_position();
            const auto& dpath = mv.detail().path();
            Fvector pend = dpath.empty() ? S->Position() : dpath.back().position;
            Msg("~ [steer] %u want=%d held=%s%s | queued=%d moveDone=%d | move=%u mental=%u enabled=%d path=%u at %u completed=%d failed=%d speed=%.2f | goal %.1fm away"
                " | ptype=%u lvl_dest=%u want_v=%u my_v=%u | det_dest=(%.1f %.1f %.1f) path_end=(%.1f %.1f %.1f) me=(%.1f %.1f %.1f)",
                P->id, P->steer ? 1 : 0, S->GetScriptControl() ? (holder ? holder : "?") : "-", ours ? "(ours)" : "", cur ? 1 : 0,
                cur ? (cur->m_tMovementAction.m_bCompleted ? 1 : 0) : -1, u32(mv.movement_type()), u32(mv.mental_state()), mv.enabled() ? 1 : 0,
                u32(dpath.size()), mv.detail().curr_travel_point_index(), mv.path_completed() ? 1 : 0, mv.detail().failed() ? 1 : 0, mv.speed(), to.magnitude(),
                u32(mv.path_type()), mv.level_dest_vertex_id(), P->steer_vertex, S->ai_location().level_vertex_id(), VPUSH(dd), VPUSH(pend), VPUSH(S->Position()));
        }
    }

    bool want = P && P->steer && S->g_Alive();

    // ⭐A fight belongs to the brain. Under script control the planner does not
    // run at all, so a follower held to its order would walk past an enemy
    // shooting at it. Hand it back while there is somebody to fight; the order
    // resumes, re-issued, the moment there is not.
    if (want && S->memory().enemy().selected())
    {
        want = false;
        if (!P->steer_fighting)
        {
            P->steer_fighting = true;
            Msg("~ [sv_npc] %u has an enemy -- its own brain fights, the order waits", P->id);
        }
    }
    else if (P && P->steer_fighting && !S->memory().enemy().selected())
    {
        P->steer_fighting = false;
        Msg("~ [sv_npc] %u no enemy left -- back to its order", P->id);
    }

    if (!want)
    {
        if (ours)
        {
            S->SetScriptControl(false, k_owner);
            if (P)
                P->steer_applied = 0; // whatever it was told is gone with the control
        }
        return;
    }

    // Somebody else's scene -- a mod script holding this creature for a scripted
    // moment. Not ours to take; the order simply waits for it to end.
    if (S->GetScriptControl() && !ours)
        return;

    if (!ours)
    {
        S->SetScriptControl(true, k_owner);
        if (!S->GetScriptControl())
            return; // the creature refuses capture right now (can_script_capture)
        P->steer_applied = 0;
    }

    if (P->steer_applied == P->steer_version)
        return; // told already -- walking it, or arrived and waiting for a new goal
    P->steer_applied = P->steer_version;

    // walk / run / sprint. The sprint is the run clip on the panic tariff --
    // there is no separate sprint clip (see drive_speed).
    const MonsterSpace::EMovementType move = (P->steer_gear >= 1) ? MonsterSpace::eMovementTypeRun : MonsterSpace::eMovementTypeWalk;
    const MonsterSpace::EMentalState mental = (2 == P->steer_gear) ? MonsterSpace::eMentalStatePanic : MonsterSpace::eMentalStateFree;

    CScriptEntityAction order;
    order.m_tMovementAction = CScriptMovementAction(MonsterSpace::eBodyStateStand, move, DetailPathManager::eDetailPathTypeSmooth, P->steer_goal);
    // the vertex resolved in Update, not the action's own hinted lookup -- see steer_vertex
    order.m_tMovementAction.m_tGoalType = CScriptMovementAction::eGoalTypePathNodePosition;
    order.m_tMovementAction.m_tNodeID = P->steer_vertex;
    order.m_tAnimationAction = CScriptAnimationAction(mental);
    order.m_tWatchAction = CScriptWatchAction(SightManager::eSightTypePathDirection);
    order.m_tActionCondition = CScriptActionCondition(CScriptActionCondition::MOVEMENT_FLAG);

    // ⚠⚠The level path has to be the CURRENT path type before the action runs.
    // CAI_Stalker::bfAssignMovement does set_path_type(path_type()), and the
    // stalker's setter writes the TARGET while its getter reads the CURRENT: so
    // whatever path the creature was on -- A-Life's game path to its smart
    // terrain -- is written back over the level path the base action has just
    // chosen, and the next movement update makes it current again. Measured
    // 22-09-2026 with ptype on the steer line: 0 (game path) under our order,
    // det_dest alternating between two far A-Life waypoints, a goto standing
    // 6 m short and a follower running 35 m the other way.
    // Setting only the target is not enough either: AddAction runs the action at
    // once (empty queue), before any update copies target to current. Hence
    // the update here -- the same call bfAssignMovement itself ends with.
    // (Single-player scripts get away with it because move_mgr sets the path
    // type an update BEFORE it gives the order.)
    //
    // ⚠And the destination goes in through the manager's own setters before that
    // update, not only through the action: otherwise the update plans a level
    // path to the OLD destination (A-Life's), the action then swaps the vertex
    // in underneath without invalidating it, and its completion test sees a
    // completed path -- measured: a 10-point path, "done" on the spot, 25 m short.
    Fvector goal = P->steer_goal;
    S->movement().set_path_type(MovementManager::ePathTypeLevelPath);
    S->movement().set_level_dest_vertex(P->steer_vertex);
    S->movement().set_desired_position(&goal);
    S->movement().update(0);
    S->ClearActionQueue();
    S->AddAction(&order);

    static u32 s_diag = 0;
    if (++s_diag <= 40 || 0 == (s_diag % 100))
        Msg("~ [sv_npc] %u steered: its own legs to (%.1f, %.1f, %.1f) at %s (order v%u)", P->id, VPUSH(P->steer_goal),
            (2 == P->steer_gear) ? "SPRINT" : (1 == P->steer_gear ? "run" : "walk"), P->steer_version);
}

void SVNpc::PublishHands()
{
    xrServer* SV = SVWorld::Server();
    if (!SV || !g_pGameLevel)
        return;

    const u32 now = Device.dwTimeGlobal;

    static u32 s_next_poll = 0;
    if (now < s_next_poll)
        return;
    s_next_poll = now + 200; // four times slower than a man can swap weapons

    // ⭐Everything gets re-announced periodically, not only on change. A client
    // that connects mid-fight missed every earlier announcement, and so did one
    // whose item had not finished spawning when the change went out -- the
    // creature would then stand there empty-handed forever. Cheap enough to be
    // the answer to both: a couple of u16 per creature.
    static u32 s_next_full = 0;
    const bool full = (now >= s_next_full);
    if (full)
        s_next_full = now + 5000;

    static xr_map<u16, u64> s_last; // (item << 32) | (posture << 16) | action -- see the change test

    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O || O->getDestroy())
            continue;

        CEntityAlive* EA = smart_cast<CEntityAlive*>(O);
        if (!EA || !EA->Local())
            continue;

        CInventoryOwner* IO = smart_cast<CInventoryOwner*>(O);
        if (!IO)
            continue;

        // A player's body announces its own weapon through the events its
        // client already sends; only creatures the server drives are silent.
        const u16 id = u16(O->ID());
        if (!SVNpcHealth::IsServerCreature(id))
            continue;

        CInventoryItem* act = IO->inventory().ActiveItem();
        const u16 item = act ? u16(act->object().ID()) : u16(0xffff);
        u16 action_state = u16(-1);
        const u16 posture = SVNpc::PostureOf(O, action_state);

        // The pair, not the item alone: posture changes far more often than
        // what is held (a man crouches, reloads and stands back up without
        // ever swapping weapons), and a change test that only watched the item
        // would hold every one of those back for the five-second re-announce.
        // item, posture and the raw action id together: any of the three
        // changing is a change the client has to be told about
        const u64 stamp = (u64(item) << 32) | (u64(posture) << 16) | u64(action_state);
        auto it = s_last.find(id);
        const bool item_changed = (it == s_last.end() || u16(it->second >> 32) != item);
        const bool changed = (it == s_last.end() || it->second != stamp);

        // ⭐The flip, explained at the moment it happens. USER-REPORTED as
        // "убирает через раз": some creatures change their mind about the
        // weapon every half-minute while standing in a quiet field, and which
        // input moves is not readable from a snapshot of a world that is
        // otherwise still.
        if (it != s_last.end())
        {
            const bool was = 0 != (u16(it->second >> 16) & npc_posture_strapped);
            const bool now = 0 != (posture & npc_posture_strapped);
            if (was != now)
                SVFight::NoteStrapFlip(O, now);
        }
        // diag: the reload channel at its SOURCE. USER-REPORTED "ни одной
        // перезарядки не видел", while the host was measured to reload honestly
        // (magazines 12->31, 25->30, 7->30 across one fight). Printed on the
        // EDGE and with the magazine, so the log says both that the host entered
        // the state and how long it stayed there -- an announcement that never
        // leaves and one that leaves and is dropped downstream look identical
        // from the client.
        {
            const u8 act_now = u8((posture >> npc_posture_action_shift) & npc_posture_action_mask);
            const u8 act_was = (it != s_last.end()) ? u8((u16(it->second >> 16) >> npc_posture_action_shift) & npc_posture_action_mask) : u8(0xff);
            static u32 s_diag_reload = 0;
            if (act_now != act_was && (npc_action_reload == act_now || npc_action_reload == act_was) && s_diag_reload < 60)
            {
                ++s_diag_reload;
                CWeapon* w = smart_cast<CWeapon*>(act);
                Msg("~ [reload] server: %u '%s' action %u -> %u (op=%u) mag=%d/%d", id, O->cName().c_str(), act_was, act_now, action_state, w ? w->GetAmmoElapsed() : -1,
                    w ? w->GetAmmoMagSize() : -1);
            }
        }

        if (!full && !changed)
            continue;
        s_last[id] = stamp;

        // Only on a real change of WEAPON, so a fight does not drown the log:
        // posture changes several times a second per creature.
        if (item_changed)
            Msg("~ [hands] server: %u '%s' now holds %s (id %u)", id, O->cName().c_str(), act ? act->object().cName().c_str() : "nothing", item);

        NET_Packet P;
        P.w_begin(M_EVENT);
        P.w_u32(Level().timeServer());
        P.w_u16(GE_NPC_ACTIVE_ITEM);
        P.w_u16(id);
        P.w_u16(item);
        P.w_u16(posture);
        P.w_u16(action_state);
        SV->SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
    }
}

u16 SVNpc::PostureOf(CObject* O, u16& action_state)
{
    action_state = u16(-1);

    CAI_Stalker* S = smart_cast<CAI_Stalker*>(O);
    if (!S)
        return 0;

    action_state = u16(S->CObjectHandler::planner().current_action_state_id() & 0xffff);

    u16 st = 0;
    st |= u16(u16(S->movement().movement_type() & npc_posture_move_mask) << npc_posture_move_shift);
    if (MonsterSpace::eBodyStateCrouch == S->movement().body_state())
        st |= npc_posture_crouch;
    if (S->wounded())
        st |= npc_posture_wounded;

    // ⭐⭐Where the weapon BELONGS, which is not the same question as
    // weapon_strapped().
    //
    // ⚠weapon_strapped() answers "is it on the back right now", and says NO
    // while a strapping animation plays -- correct in a game with a screen,
    // where the weapon really is in transit. This host has no skeleton to end
    // that animation, so the planner re-enters Strapping over and over, and
    // the answer oscillated at the poll rate. Measured on the connected
    // client: the same creature flipped STRAP BONES / hands eight times in a
    // couple of minutes, which is a rifle flickering between shoulder blade
    // and hand.
    //
    // eWorldPropertyStrapped is a stored member of the planner's world state
    // (CObjectPropertyEvaluatorMember), written by an action's effect and not
    // recomputed per tick -- so it states the intent and holds still. A copy
    // that cannot run the animation should follow the intent.
    {
        CObjectHandlerPlanner& oh = S->CObjectHandler::planner();
        if (oh.initialized() && oh.m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped))
            st |= npc_posture_strapped;
    }

    u8 mental = npc_mental_free;
    switch (S->movement().mental_state())
    {
    case MonsterSpace::eMentalStateDanger: mental = npc_mental_danger; break;
    case MonsterSpace::eMentalStatePanic: mental = npc_mental_panic; break;
    default: break;
    }
    st |= u16(mental << npc_posture_mental_shift);

    // What the creature is DOING with the weapon, taken from the same place
    // the single-player animator takes it: the object handler's current action.
    // CStalkerAnimationManager::unknown_object_animation switches on exactly
    // this id, so the client can pick the same cycle from the same fact.
    u16 action = npc_action_idle;
    switch (action_state)
    {
    case ObjectHandlerSpace::eWorldOperatorStrapping:
    case ObjectHandlerSpace::eWorldOperatorStrapping2Idle: action = npc_action_strap; break;

    case ObjectHandlerSpace::eWorldOperatorUnstrapping:
    case ObjectHandlerSpace::eWorldOperatorUnstrapping2Idle: action = npc_action_unstrap; break;

    case ObjectHandlerSpace::eWorldOperatorReload1:
    case ObjectHandlerSpace::eWorldOperatorReload2:
    case ObjectHandlerSpace::eWorldOperatorForceReload1:
    case ObjectHandlerSpace::eWorldOperatorForceReload2:
    case ObjectHandlerSpace::eWorldOperatorGetAmmo1:
    case ObjectHandlerSpace::eWorldOperatorGetAmmo2: action = npc_action_reload; break;

    // Firing has no cycle of its own in the content -- the engine plays the
    // aim set through a burst (see unknown_object_animation), and the muzzle
    // flash is what says a shot happened.
    case ObjectHandlerSpace::eWorldOperatorAim1:
    case ObjectHandlerSpace::eWorldOperatorAim2:
    case ObjectHandlerSpace::eWorldOperatorAimForceFull1:
    case ObjectHandlerSpace::eWorldOperatorAimForceFull2:
    case ObjectHandlerSpace::eWorldOperatorAimingReady1:
    case ObjectHandlerSpace::eWorldOperatorAimingReady2:
    case ObjectHandlerSpace::eWorldOperatorQueueWait1:
    case ObjectHandlerSpace::eWorldOperatorQueueWait2:
    case ObjectHandlerSpace::eWorldOperatorFire1:
    case ObjectHandlerSpace::eWorldOperatorFire2: action = npc_action_aim; break;

    default: break;
    }
    st |= u16(action << npc_posture_action_shift);
    return st;
}

void SVNpc::PublishShot(u16 shooter, u16 weapon, const Fvector& P, const Fvector& D)
{
    xrServer* SV = SVWorld::Server();
    if (!SV || !g_pGameLevel)
        return;

    // Only creatures the server itself simulates. A player's shot is presented
    // by the machine that pulled the trigger, and re-broadcasting it would put
    // a second flash on the shooter's own screen.
    if (!SVNpcHealth::IsServerCreature(shooter))
        return;

    // ⭐Counted here rather than at the trigger, because this is the shot the
    // world is told about -- and it is the only half of "did they hit anything"
    // that anybody had. The other half arrives in GAME_EVENT_ON_HIT.
    SVAccuracy::Shot(shooter, weapon);

    NET_Packet Pk;
    Pk.w_begin(M_EVENT);
    Pk.w_u32(Level().timeServer());
    Pk.w_u16(GE_NPC_SHOT);
    Pk.w_u16(shooter);
    Pk.w_u16(weapon);
    Pk.w_vec3(P);
    Pk.w_dir(D);
    SV->SendBroadcast(BroadcastCID, Pk, net_flags(TRUE, TRUE));

    // diag: throttled hard -- an automatic weapon is ten of these a second per
    // shooter, and a firefight is five shooters. Enough to answer "is the
    // server sending anything at all", which is the question this end can
    // answer and the client end cannot.
    // ⚠A modulus alone answers "is anything being sent" and nothing else: at
    // 1-in-61 the log cannot distinguish one shot from sixty, which is the
    // difference between "the channel works" and "there is a firefight". The
    // running total is printed on a clock instead, so the RATE is readable.
    static u32 s_shots = 0;
    static u32 s_next_report = 0;
    ++s_shots;
    if (s_shots == 1)
        Msg("~ [shot] server: %u fired weapon %u from (%.1f, %.1f, %.1f) -- first shot published", shooter, weapon, VPUSH(P));
    if (Device.dwTimeGlobal >= s_next_report)
    {
        s_next_report = Device.dwTimeGlobal + 5000;
        Msg("~ [shot] server: %u published so far", s_shots);
    }
}

void SVNpc::WatchPos(u16 id, const char* tag, const Fvector& pos)
{
    if (g_sv_npc_watch < 0 || s_watch_id != id)
        return;

    // A few frames' worth and then stop: this samples at ~200 Hz from several
    // points in the frame, and the answer is visible in the first handful of
    // frames or not at all. A cap beats a rate limit here -- what matters is
    // seeing every sample of a few CONSECUTIVE frames, in order, not a thin
    // spread over a minute.
    static u32 s_lines = 0;
    if (s_lines >= 240)
        return;
    ++s_lines;

    Msg("~ [sv_npc] watch f=%u %-14s (%.3f, %.3f, %.3f)", Device.dwFrame, tag, VPUSH(pos));
}

void SVNpc::Reset()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);

    s_puppets.clear();
    s_last_update_ms = 0; // the next tick starts a fresh clock, not a huge delta
    s_ticks = 0;
    s_tick_ms = 0;
    s_dt_lost = 0.f;
}

#include "StdAfx.h"
#include "Entity.h"
#include "entity_alive.h"
#include "xrServer_Objects.h"
#include "Level.h"
#include "xrMessages.h"
#include "game_cl_base.h"
#include "NET_Queue.h"
#include "Physics.h"
#include "xrServer.h"
#include "Actor.h"
#include "../xr_3da/CameraBase.h"
#include "ai_space.h"
#include "saved_game_wrapper.h"
#include "level_graph.h"
#include "clsid_game.h"

#include "HUDManager.h"
#include "UIGameSP.h"
#include "../xrOnlineNet/NET_GNS_Transport.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "GameObject.h"
#include "../Include/xrRender/RenderVisual.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../xr_3da/SkeletonMotions.h" // CMotionDef: the authored rate and the loop flag
#include "../Include/xrRender/animation_blend.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "Weapon.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager.h"
#include "sv_npc.h" // the posture byte's layout -- written by SVNpc::PostureOf
#include "stalker_animation_manager.h"
#include "object_handler_planner.h"
#include "object_handler.h" // strapped_state() -- the one answer all four readers share
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "cl_anim_blendspace.h"

// 1 = drive the SHIPPED animator from replicated state; 0 = the hand-rolled
// cycle selector below. An A/B switch, not a preference -- see
// net_puppet_native_animation.
int g_cl_anim_native = 1;

// 1 = the legs are a continuous blend space fed from the wire's intent; 0 =
// one cycle at a time, picked from a speed band. Wins over cl_anim_native,
// because two animators on one set of blends is the fault this build has
// already paid for six times. See cl_anim_blendspace.h.
int g_cl_anim_blend = 1;

// 1 = believe what the server says the creature is doing (SNetCreatureIntent);
// 0 = infer it from the rendered path, the way this layer used to. Kept
// switchable because "the wire is better than the estimate" is a claim that has
// to be measurable, and because a sender that authors no intent (a player's own
// body) falls back to the estimate anyway -- so the fallback path must stay
// exercised rather than rot.
int g_cl_net_intent = 1;

// 1 = interpolate between the two buffered snapshots that BRACKET the render
// time; 0 = the shipped behaviour, which only ever held the newest two and so
// clamped every frame. Kept switchable because the difference is the whole
// diagnosis -- see the ring in SNetImportTrack.
int g_cl_net_interp_ring = 1;

// 1 = when the server sends its animator's clips (SNetAnimState), play exactly
// those and let every local selector stand down; 0 = the local selectors (blend
// space, torso table) even when clips arrive. The A/B between "the server's
// animator" and "our guess at it", flippable live.
int g_cl_net_anim = 1;

// ⚠⚠A CAPPED DETECTOR THAT HAS RUN OUT LOOKS EXACTLY LIKE A CLEAN RESULT.
//
// These two used to stop after 40 lines apiece. On 29-07-2026 that cost a
// whole conclusion: an A/B of the animation path was run, the "after" side
// showed zero foreign writes, and the zero meant nothing -- the counters had
// been exhausted before the switch was even thrown. A silent detector and an
// absent defect are indistinguishable, and only one of them is good news.
//
// So they now COUNT everything, print the first 40 in full, and keep printing
// a running total after that. Exposed as globals so a test can read the count
// and zero it between the two halves of a comparison (cl_foreign_reset).
u32 g_net_foreign_yaw = 0;
u32 g_net_foreign_pos = 0;

// Same trap, third instrument: the snapshot diag is capped too, and in a fight
// a dozen creatures burn its whole budget in seconds. Set to an entity id to
// print only that one, with a budget that outlasts a recording window.
int g_cl_snap_watch = 0;

// --- network-client inbound per-entity state import --------------------------
// For remote clients the server appends a [u16 id][u8 size][UPDATE_Write
// payload] stream after the game-state part of M_UPDATE
// (xrServer::SendUpdatesToAll). The fused local path never carries that tail,
// so this parser is a natural no-op in single player. Payloads are decoded
// through lazily-created CSE mirrors (same pattern as the outbound export) and
// the decoded position/heading is applied through ForceTransform -- the same
// call the server uses when applying a client's state, which keeps the
// object's own movement/interpolation from snapping it back next frame.
static xr_map<u16, CSE_Abstract*> s_net_import_mirrors;

// movement bookkeeping per entity: first ever applied position (jump targeting),
// the last two network snapshots (the per-frame interpolator renders the
// puppet ~120 ms behind now, lerping a->b -- raw 20 Hz ForceTransform snapping
// reads as "teleporting" to the eye), a smoothed speed for locomotion-band
// selection and the band currently playing on the puppet's skeleton
enum : u8
{
    puppet_band_none = 0,
    puppet_band_idle,
    puppet_band_walk,
    puppet_band_run,
};
// ⭐⭐⭐One buffered snapshot. See the ring in SNetImportTrack for why a PAIR
// was never enough to do what the interpolator's own comments claim it does.
struct SNetSnap
{
    u32 t{0};
    Fvector pos{};
    float yaw{0.f}, aim{0.f}, pitch{0.f};
    // ⭐What the sender says it was DOING over the interval ending here. Kept
    // per snapshot rather than as one "latest" value on the track, because the
    // renderer is ~120-250 ms behind the wire: the newest intent describes a
    // moment the puppet has not reached yet. Sampled with the pair it belongs
    // to, the velocity in here is exactly the velocity of the segment being
    // drawn -- animation and picture cannot disagree.
    SNetCreatureIntent intent;
    // ⭐The clips the server's animator was playing at this stamp, with their
    // phase. Same reason to keep it per snapshot: the picture is rendered
    // behind the wire, and the clip belongs to the moment it was sampled at.
    SNetAnimState anim;
};
constexpr u32 k_net_ring = 16; // ~800 ms at 20 Hz -- more than any render-behind we clamp to

struct SNetImportTrack
{
    Fvector first;
    Fvector pos_a, pos_b;
    float yaw_a, yaw_b;
    // where the creature is LOOKING, as opposed to where its model faces. The
    // wire has carried this all along (o_torso, next to o_model, in the very
    // same update) and we ignored it, so every puppet stared straight down its
    // own walking direction -- "они не целятся" from play while the server's
    // telemetry showed them shooting each other at nine metres. Interpolated on
    // the same timeline as the position; consumed by the engine's own
    // spine/shoulder bone callbacks (see the apply in the tick).
    float aim_a{0.f}, aim_b{0.f};
    float pitch_a{0.f}, pitch_b{0.f};
    SNetCreatureIntent intent_a, intent_b;
    u32 t_a, t_b;
    // wire stamp of snapshot b (server clock). The transport delivers in
    // bursts, so ARRIVAL times cannot pace the snapshots: two packets landing
    // in one frame collapse the interpolation interval to zero and the puppet
    // steps once per burst instead of gliding. The stamp deltas carry the
    // sender's true cadence; the local timeline is rebuilt from them.
    u32 ts_b{0};
    // smoothed interval between ACCEPTED snapshots. The state rate is not one
    // number: a driven walk refreshes at the ~20 Hz build rate, but a guard
    // standing on its post only changes at the stalker's ~10 Hz schedule push
    // (measured 107-127 ms spans in play) -- a fixed 120 ms render-behind sat
    // right on top of those and kept falling into hold-then-catch-up. The
    // interpolator lags each puppet by ~2.3 of ITS OWN intervals instead.
    float dt_ema{50.f};
    // current render-behind, slewed toward the dt_ema target: snapping the
    // lag when the stream changes tempo rewinds/fast-forwards the render
    // time -- a jerk this interpolator would be introducing itself
    float behind_cur{120.f};
    // ⭐⭐⭐THE BUFFER THE RENDER-BEHIND ALWAYS ASSUMED IT HAD.
    //
    // pos_a/pos_b hold the two most recent snapshots, and t_b tracks now. The
    // tick renders at now - behind_cur, and behind_cur is ~2.3 snapshot
    // intervals -- so the render time sits a whole interval BEFORE t_a, every
    // frame, and the lerp takes its `rt <= t_a` branch and returns pos_a
    // verbatim. The puppet therefore did not interpolate at all: it held still
    // and stepped once per arriving snapshot, 20 times a second, while its
    // legs animated at frame rate.
    //
    // ⚠Measured 29-07-2026 before this existed, on a stalker walking a patrol
    // at a steady 1.60 m/s (server assert CV 0.027, i.e. a glide): the RENDERED
    // root came out mean 1.385 m/s with sd 2.484 and peaks of 9.96 -- the exact
    // signature of "moves on 29% of frames, frozen on the rest". The pose next
    // to it was clean (accel mean 1.5 against the root's 194), which is what
    // named the transform rather than the animator. Reported from play as
    // micro-vibration while walking.
    //
    // ⭐The lesson worth keeping: the comment above behind_cur described a
    // buffer of ~2.5 intervals, and reviewers (me, twice) read the comment and
    // believed the data structure implemented it. Two evenings of animation
    // work sat downstream of a lerp that never ran.
    SNetSnap ring[k_net_ring];
    u32 ring_n{0}; // total pushed, never wrapped -- index with % k_net_ring
    // synthetic milliseconds inserted while the wire stamp stood still (see
    // the dedup in the apply path) -- subtracted from the next honest delta
    u32 ts_synth{0};
    // last yaw/position this interpolator wrote to the transform -- lets the
    // tick detect (and name) any OTHER writer moving the puppet between frames
    float yaw_written{0.f};
    bool yaw_written_valid{false};
    Fvector pos_written{};
    // speed of the RENDERED model (EMA of per-frame displacement). The
    // animation must match what is on screen, not what the wire claims:
    // wire-speed banding kept the walk cycle playing through interpolator
    // holds ("ходят на месте") and lagged every stop/start by ~a second.
    float spd_render{0.f};
    // band debounce: a fast speed estimate restarted the legs cycle every
    // 200-600 ms around the thresholds (48 idle<->walk restarts a minute --
    // "путаются в ногах"). A candidate band must hold this long to commit.
    u8 band_cand{0};
    u32 band_cand_ms{0};
    // torso cycle index currently playing (-1 = none yet); indexed by the
    // held weapon's animation slot, so the puppet stops walking in the
    // unarmed hunch while carrying a rifle
    s8 torso_idx{-1};
    // ...and whether that cycle is the AIMING one. Twisting the spine towards
    // a target while the arms hold the weapon at rest reads as a man looking
    // at you, not a man shooting at you: the aim cycle is what brings the
    // rifle up to the shoulder.
    bool torso_aim{false};
    // when the last shot arrived. The aim pose is latched off shots rather
    // than replicated on its own -- see the GE_NPC_SHOT handler.
    u32 shot_ms{0};
    // how the creature is carrying itself: crouch/wounded/mental/weapon
    // action, packed by SVNpc::PostureOf. The stalker animation set is indexed
    // by all of it -- see net_puppet_prefix.
    u16 posture{0}; // COMMITTED posture -- what the puppet is being animated as
    u16 posture_raw{0}; // what the wire last said
    u16 posture_cand{0}; // a change waiting out the debounce below
    u32 posture_cand_ms{0};
    // the object handler's raw action id, for the path that drives the shipped
    // animator (which switches on exactly this number)
    u16 action_state{u16(-1)};
    u8 played_action{0xff}; // the effective action the playing torso was built from
    u8 played_class{0xff}; // ...and the state class
    u8 played_var{0xff}; // ...and which take of it (calm vs alert, standing vs moving)
    u8 legs_state{0xff}; // the state class the playing LEGS were built from
    // the one-shot transition (draw/holster) currently being timed. It has no
    // end event on this side, so it needs a deadline -- see the torso selector.
    u8 transition_action{0xff};
    u32 transition_ms{0};
    // feet-to-ground sync: the legs blend currently playing, the engine-set
    // playback speed it started with, and its motion -- the motion validates
    // the pooled blend slot before retuning (slots are reused)
    CBlend* legs_blend{nullptr};
    float legs_base_speed{0.f};
    MotionID legs_motion;
    // cl_anim_trace: throttle for the legs trace, and the previous cycle clock
    // so the trace can say whether the blend ADVANCES rather than only whether
    // it was started. "cycle PLAYING" in the log is the ORDER, not the result.
    u32 legs_trace_ms{0};
    float legs_trace_time{-1.f};
    u16 dbg_id{0xffff}; // whose track this is, for the trace only
    // Ground speed the CURRENT legs cycle was authored for. Human puppets take
    // it from the stalker table below; a monster takes its own, out of the same
    // ltx lines its server-side drive reads -- a dog's run is not 3.0 m/s and
    // pretending otherwise is what makes its feet skate.
    float legs_native{0.f};
    // windowed speed estimate: updates arrive in bursts (several per frame,
    // then a gap), so instantaneous dist/dt is garbage -- accumulate distance
    // over a ~400 ms wall-clock window instead and derive speed per window
    float acc_dist;
    u32 win_ms;
    float speed_ema;
    u8 band;
    // ⭐The intent belonging to the segment currently on screen, and the two
    // numbers everything downstream should be reading instead of guessing:
    // ground speed and its signed forward component. When the sender authors no
    // intent they fall back to spd_render, so every consumer has one input
    // whether or not the wire is helping.
    SNetCreatureIntent intent;
    float speed_auth{0.f};
    float fwd_auth{0.f};
    // ⭐⭐The server's clips (SNetAnimState), newest arrived -- the fallback when
    // the ring is off or still empty.
    SNetAnimState anim_b;
    // What this client started on each part to follow them. The blend pointer
    // is into a pooled slot, so it is trusted only while that slot still plays
    // the same motion.
    struct SAnimPlay
    {
        MotionID motion;
        CBlend* blend{nullptr};
    };
    SAnimPlay anim_play[SNetAnimState::kParts];
    // Who owns this skeleton right now: the server's clips (true) or the local
    // selectors below (false). Exactly one of them, switched in one place.
    bool anim_owned{false};
    // The legs, when they are a blend space rather than a cycle at a time.
    ClAnim::CLegsBlendSpace legs_bs;
    bool actor_torso_started{false}; // a remote player's upper body is posed once
    // The server has this creature dead and we have acted on it: the local copy
    // was killed and its transform is no longer imported. Latched, because both
    // of those must happen exactly once.
    bool dead{false};
};
static xr_map<u16, SNetImportTrack> s_net_import_track;
// Puppets the server has killed and whose local copy still has to be told.
// See where this is filled for why it is a queue and not a call.
static xr_vector<u16> s_pending_deaths;

// hysteresis keeps jittery per-update speed estimates from flapping the
// animation between idle and run several times a second
static u8 net_puppet_pick_band(u8 cur, float v)
{
    switch (cur)
    {
    case puppet_band_idle: return v > 3.2f ? puppet_band_run : (v > 0.5f ? puppet_band_walk : puppet_band_idle);
    case puppet_band_walk: return v > 3.2f ? puppet_band_run : (v < 0.25f ? puppet_band_idle : puppet_band_walk);
    case puppet_band_run: return v < 0.25f ? puppet_band_idle : (v < 2.4f ? puppet_band_walk : puppet_band_run);
    default: return v > 3.f ? puppet_band_run : (v > 0.3f ? puppet_band_walk : puppet_band_idle);
    }
}

static CBlend* net_puppet_play_first_valid(IKinematicsAnimated* ka, std::initializer_list<LPCSTR> names, LPCSTR diag_section, bool always_log = false, MotionID* out_motion = nullptr)
{
    // diag: resolution of cycle names is the blind spot -- an unknown name is
    // skipped silently and the puppet stays frozen, so log the first attempts.
    // always_log bypasses the cap: the 25-NPC burst eats all 48 slots before a
    // remote-player actor ever gets here, hiding exactly the case under study
    static u32 s_diag_plays = 0;
    for (LPCSTR n : names)
    {
        MotionID m = ka->ID_Cycle_Safe(n);
        if (m.valid())
        {
            // ⚠⚠Play it LOOPED whatever the content says. Measured on a dog:
            // stand_run_0 is authored STOP-AT-END, so the puppet ran the cycle
            // once and froze on its last frame -- the cycle clock parked at
            // timeTotal-EPS and never moved again, which on screen is "the dogs
            // run but hardly move their paws". In single player the creature's
            // own animator re-picks and restarts these cycles constantly; a
            // puppet has no animator, so the loop has to be asked for here.
            CMotionDef* def = ka->LL_GetMotionDef(m);
            CBlend* B = def ? ka->LL_PlayCycle(def->bone_or_part, m, TRUE, def->Accrue(), def->Falloff(), def->Speed(), FALSE, nullptr, nullptr, 0) : ka->PlayCycle(m, TRUE);
            // The motion is the durable handle: a whole-body cycle plays on
            // every partition and hands back no blend at all, so the pointer
            // cannot be the record of what is running.
            if (out_motion)
                *out_motion = m;
            if (always_log || s_diag_plays < 48)
            {
                ++s_diag_plays;
                // ⚠Print the RESULT, not the order. This line used to say
                // "PLAYING" the moment the name resolved, so a PlayCycle that
                // handed back nothing read in the log exactly like a running
                // cycle -- and the legs it never started looked like a defect
                // somewhere else entirely.
                Msg("* [netupd] puppet '%s': cycle '%s' %s", diag_section, n, B ? "PLAYING" : "REFUSED -- PlayCycle returned no blend");
            }
            return B;
        }
    }
    if (always_log || s_diag_plays < 48)
    {
        ++s_diag_plays;
        Msg("! [netupd] puppet '%s': NO cycle resolved (tried '%s'...)", diag_section, names.size() ? *names.begin() : "<none>");
    }
    return nullptr;
}

// Feet-to-ground sync. The walk/run cycles are authored for the stalker AI's
// own locomotion speeds; a puppet carried at any other speed slides its feet
// across the ground (reported from play). Scale the LEGS blend playback so the
// stride rate follows the measured speed. Native speeds are the CONTENT's, not
// guesses: the norm_* cycles are the FREE-state locomotion, and this mod's
// m_stalker.ltx says free_stand_walk_forward=1.1, free_stand_run_forward=3.0
// (the first estimate of 1.35 for walk left a visible ~20% slide at the
// server's 1.6 m/s drive speed).
static const float k_puppet_native_speed[] = {0.f, 0.f, 1.10f, 3.00f}; // per band: none, idle, walk, run

// Same switch as the stalker animation trace -- one instrument, not a second
// cvar to leak into user.ltx.
extern int g_cl_anim_trace;
extern int g_cl_anim_trace_id;

// How far the upper body may be turned away from the model heading before the
// body itself has to come round. Roughly what a person can do carrying a rifle.
static const float k_puppet_max_twist = deg2rad(55.f);
// ...and how much of that a puppet is allowed to KEEP while standing still.
// Nothing stops a man who is not going anywhere from turning to face what he
// is watching, and single player renders exactly that: the movement manager
// walks m_body round towards m_head at a turn rate. A guard left at the full
// twist reads as looking over his shoulder at you, which is a different thing
// from aiming at you.
static const float k_puppet_stand_twist = deg2rad(22.f);
static const float k_puppet_max_pitch = deg2rad(50.f);
// The ground speed a species' own content says this gait runs at -- the very
// lines its server-side drive loads (CControlMovementBase::load). The first item
// of the ltx value is the linear velocity.
static float net_puppet_species_native(LPCSTR sect, u8 band)
{
    LPCSTR line = (band == puppet_band_walk) ? "Velocity_WalkFwdNormal" : ((band == puppet_band_run) ? "Velocity_RunFwdNormal" : nullptr);
    if (!line || !sect || !pSettings->section_exist(sect) || !pSettings->line_exist(sect, line))
        return 0.f;

    string32 buffer;
    return float(atof(_GetItem(pSettings->r_string(sect, line), 0, buffer)));
}

// ⚠⚠Set the rate on EVERY blend carrying this motion, found by motion rather
// than by the pointer PlayCycle handed back.
//
// PlayCycle returns a blend only when the cycle belongs to ONE partition. A
// whole-body cycle -- which is what a monster's locomotion cycles are -- is
// started on all parts and returns nullptr BY DESIGN (SkeletonAnimated.cpp, the
// BI_NONE branch). The old code read that nullptr as "nothing started", so
// feet-to-ground sync was dead for every mutant on the level while the cycles
// themselves ran perfectly well. It read as a dog skating at 7 m/s with its legs
// ambling, and it survived this long only because no mutant had ever moved.
struct CLegsSpeedSetter : public IterateBlendsCallback
{
    MotionID motion;
    float speed{0.f};
    u32 touched{0};

    void operator()(CBlend& B) override
    {
        if (B.motionID == motion && B.blend_state() != CBlend::eFREE_SLOT)
        {
            B.speed = speed;
            ++touched;
        }
    }
};

// Find a blend the cycle actually started, when PlayCycle handed none back.
struct CLegsBlendProbe : public IterateBlendsCallback
{
    MotionID motion;
    CBlend* found{nullptr};
    float speed{0.f};

    void operator()(CBlend& B) override
    {
        if (!found && B.motionID == motion && B.blend_state() != CBlend::eFREE_SLOT)
        {
            found = &B;
            speed = B.speed;
        }
    }
};

// One place records what the legs are now running, for both callers: the motion
// (the durable handle), the rate the content authored the cycle at, and the
// ground speed that rate was authored FOR.
static void net_puppet_note_legs(IKinematicsAnimated* ka, SNetImportTrack& trk, CBlend* B, MotionID m, float native)
{
    trk.legs_motion = m;
    trk.legs_native = native;
    trk.legs_blend = B;
    trk.legs_base_speed = 0.f;

    if (!ka || !m.valid())
        return;

    // ⚠The base rate comes from the MOTION DEF, never from a blend. Reading it
    // back off the blend re-reads a speed this very function's retune has
    // already scaled, so the same cycle re-picked a few times decayed
    // geometrically towards the clamp floor -- measured 1.600, 0.960, 0.576.
    if (CMotionDef* def = ka->LL_GetMotionDef(m))
        trk.legs_base_speed = def->Speed();

    // The pointer is only wanted so the trace can watch the cycle clock; a
    // whole-body cycle hands none back, so go and find one.
    if (!trk.legs_blend)
    {
        CLegsBlendProbe probe;
        probe.motion = m;
        ka->LL_IterateBlends(probe);
        trk.legs_blend = probe.found;
    }
}

static void net_puppet_retune_legs(IKinematicsAnimated* ka, SNetImportTrack& trk)
{
    u32 touched = 0;
    float applied = -1.f;

    if (ka && trk.legs_motion.valid() && trk.legs_native > 0.f && trk.legs_base_speed > 0.f)
    {
        float factor = trk.speed_auth / trk.legs_native;
        clamp(factor, 0.6f, 1.7f);

        CLegsSpeedSetter setter;
        setter.motion = trk.legs_motion;
        setter.speed = applied = trk.legs_base_speed * factor;
        ka->LL_IterateBlends(setter);
        touched = setter.touched;
    }

    // ⭐The trace runs whatever happened above, including the do-nothing cases:
    // placed after the guards it printed nothing at all, and "one of several
    // early exits" is not a diagnosis. `touched` is the number that matters --
    // zero there means the retune is talking to nobody.
    if (g_cl_anim_trace)
    {
        const u32 now_ms = Device.dwTimeGlobal;
        if (now_ms - trk.legs_trace_ms >= 1000)
        {
            trk.legs_trace_ms = now_ms;
            // ⭐dt is the cycle CLOCK's own movement since the previous line. It
            // is the only number here that says the animation RUNS: rate, base
            // and touched can all look healthy on a blend nobody advances.
            CBlend* const b = trk.legs_blend;
            const bool ours = b && (b->motionID == trk.legs_motion) && (b->blend_state() != CBlend::eFREE_SLOT);
            const float dt = (ours && trk.legs_trace_time >= 0.f) ? (b->timeCurrent - trk.legs_trace_time) : -1.f;
            Msg("~ [legs] id=%u band=%u motion=%c native=%.2f spd_auth=%.2f base=%.3f -> speed=%.3f touched=%u | ours=%c t=%.3f/%.3f dt=%.3f", trk.dbg_id, trk.band,
                trk.legs_motion.valid() ? '+' : '-', trk.legs_native, trk.speed_auth, trk.legs_base_speed, applied, touched, ours ? '+' : '-',
                ours ? b->timeCurrent : -1.f, ours ? b->timeTotal : -1.f, dt);
            trk.legs_trace_time = ours ? b->timeCurrent : -1.f;
        }
    }
}

// Puppet locomotion: no AI runs on a thin client, so nothing ever selects an
// animation cycle -- stalkers render in bind pose, monsters freeze after the
// in_NetSpawn "death_init" one-shot. The renderer advances already-playing
// blends on its own (CKinematicsAnimated::UpdateTracks via CalculateBones), so
// it is enough to PlayCycle a looped motion here whenever the speed band
// inferred from consecutive network positions changes. Stalker skeletons are
// partitioned (legs/torso/head) -- a legs cycle alone leaves the upper body in
// bind pose, hence the torso+head pair alongside it.
// -net_jump_npc follow-cam target: once the jump fires, the observer's camera is
// re-aimed at this entity every interpolate tick, so the puppet stays framed no
// matter how either side drifts (one-shot aiming lost the target within seconds)
static u16 g_net_watch_id = 0xffff;

// How long a shot keeps the puppet in the aiming pose. Long enough to bridge
// the gaps in a burst and the walk between two bursts, short enough that a
// fight that ended does not leave a field of men holding a bead on nothing.
static const u32 k_puppet_aim_hold_ms = 2500;

// ⚠A posture change must hold this long before the puppet is re-animated on
// it. Measured 29-07-2026 from play: the server's crouch flag alternated with
// a period of exactly 200 ms -- the poll interval -- and every alternation
// restarted the walk cycle from its first frame, which read as "будто
// телепортируются в 3 fps". Same lesson, same number as the speed-band
// debounce above: what the animator needs is not the freshest fact but a
// stable one.
static const u32 k_puppet_posture_debounce_ms = 250;
// Same reasoning, same number, for the locomotion band: a candidate must hold
// this long before the legs are allowed to change what they are playing.
static const u32 k_puppet_band_debounce_ms = 250;

// A draw/holster cycle is a one-shot with no end event on this side, so
// PlayCycle holds its last frame for as long as the server keeps reporting
// the transition -- "руки застревают". Give it a deadline instead and fall
// back to the resting/aiming set. Родня: the-event-that-never-arrives.
//
// ⭐The SAME deadline the weapon's attachment uses, and deliberately not a
// second number: when the hands finish the motion the rifle has arrived, and
// when these two were separate constants on separate machines the user watched
// the hands come forward with the weapon still in them.
static const u32 k_puppet_transition_ms = CObjectHandler::net_transition_ms;

// The state prefix every stalker cycle name starts with. The engine's own
// table is state_names[] = {"cr_", "norm_", "dmg_norm_"} -- posture is not a
// modifier applied on top of an animation, it picks a different animation.
static u8 net_puppet_state_class(const SNetImportTrack& trk)
{
    if (trk.posture & npc_posture_crouch)
        return 1;
    if (trk.posture & npc_posture_wounded)
        return 2;
    return 0;
}
static LPCSTR net_puppet_prefix(const SNetImportTrack& trk)
{
    switch (net_puppet_state_class(trk))
    {
    case 1: return "cr_";
    case 2: return "dmg_norm_";
    default: return "norm_";
    }
}

// ⭐⭐⭐WHICH TAKE OF THE CYCLE. The stalker set ships two of nearly everything
// and the difference is not a variation, it is a MOOD: _0 is the alert gait and
// carry (weapon up, shoulders low), _1 is the relaxed one. Single player picks
// between them off the mental state, silently, by array index -- see the long
// comment on gait_variants in cl_anim_blendspace.cpp for the three tables.
//
// Everything here asked for _0. That is why a stalker with his rifle on his
// back still walked around like a man expecting contact: "остаются в позе
// крадущихся".
static bool net_puppet_calm(const SNetImportTrack& trk)
{
    if (0 != net_puppet_state_class(trk))
        return false; // crouched or wounded is not a calm way to carry yourself
    return npc_mental_free == u8((trk.posture >> npc_posture_mental_shift) & npc_posture_mental_mask);
}

// Defined below with the locomotion machinery; the torso needs the same answer
// about what this body is doing, and there is only one place that decides it.
static u8 net_puppet_commit_band(SNetImportTrack& trk);

// Start the legs cycle for the band the track is already in, under the state
// prefix it is already in. Split out because posture changes the legs SET
// without changing the band, and the band machinery must not be disturbed to
// say so: withdrawing the band to force a re-pick also skipped its debounce,
// and a flapping crouch flag then restarted the walk several times a second.
static void net_puppet_play_legs(IKinematicsAnimated* ka, SNetImportTrack& trk, LPCSTR sect)
{
    if (trk.band == puppet_band_none)
        return;
    // Same two-take rule as the blend space, so flipping cl_anim_blend does not
    // also flip what the animator is being asked for.
    const bool calm = net_puppet_calm(trk);
    const u8 mental = u8((trk.posture >> npc_posture_mental_shift) & npc_posture_mental_mask);
    string64 move, move0;
    if (trk.band == puppet_band_idle)
    {
        xr_sprintf(move, "idle_%d", (calm && npc_mental_free == mental) ? 1 : 0);
        xr_strcpy(move0, "idle_0");
    }
    else
    {
        LPCSTR gait = (trk.band == puppet_band_walk) ? "walk" : "run";
        xr_sprintf(move, "%s_fwd_%d", gait, (calm && npc_mental_danger != mental) ? 1 : 0);
        xr_sprintf(move0, "%s_fwd_0", gait);
    }
    string64 legs, legs_alt, legs_plain;
    xr_sprintf(legs, "%s%s", net_puppet_prefix(trk), move);
    xr_sprintf(legs_alt, "%s%s", net_puppet_prefix(trk), move0);
    xr_sprintf(legs_plain, "norm_%s", move0);
    MotionID m;
    CBlend* B = net_puppet_play_first_valid(ka, {legs, legs_alt, legs_plain}, sect, false, &m);
    net_puppet_note_legs(ka, trk, B, m, (trk.band < u8(sizeof(k_puppet_native_speed) / sizeof(k_puppet_native_speed[0]))) ? k_puppet_native_speed[trk.band] : 0.f);
    net_puppet_retune_legs(ka, trk);
}

// Commit a posture change only once it has held. BOTH animation paths read the
// committed value, never the wire's latest word -- see the debounce constant.
static void net_puppet_commit_posture(SNetImportTrack& trk)
{
    if (trk.posture_raw == trk.posture)
        return;
    const u32 now_ms = Device.dwTimeGlobal;
    if (trk.posture_raw != trk.posture_cand)
    {
        trk.posture_cand = trk.posture_raw;
        trk.posture_cand_ms = now_ms;
        return;
    }
    if (now_ms - trk.posture_cand_ms >= k_puppet_posture_debounce_ms)
        trk.posture = trk.posture_raw;
}

// The upper body, on its own clock. What a creature holds, how it carries
// itself and whether it is shooting change for reasons that have nothing to do
// with how fast it is walking, so this cannot live inside the speed-band
// switch: a stalker who opened fire standing still would have kept the resting
// pose until he happened to take a step.
static void net_puppet_torso(CGameObject* GO, SNetImportTrack& trk, bool owns_legs = true)
{
    IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(GO->Visual());
    if (!ka)
        return;

    const u32 now_ms = Device.dwTimeGlobal;

    // The legs change set with the state too, and standing up out of cover is
    // not a change of SPEED -- the locomotion selector would never notice it.
    // Re-play them directly for the band they are already in.
    //
    // ⚠Unless somebody else owns the legs. The blend space holds its cycles by
    // hand, and a PlayCycle from here would fade them out from under it -- two
    // authorities over one partition, which is the fault this file already
    // carries six comments about.
    const u8 state_class = net_puppet_state_class(trk);
    if (trk.legs_state != state_class)
    {
        const bool first = (trk.legs_state == 0xff);
        trk.legs_state = state_class;
        if (!first && owns_legs)
            net_puppet_play_legs(ka, trk, GO->cNameSect().c_str());
    }

    // torso cycles are indexed by the held weapon's animation slot (see
    // CStalkerAnimationManager::object_slot); the unarmed set (_0_) on a body
    // carrying a rifle reads as a crouched sneak -- "идут в позе крадусь".
    //
    // ⭐Taken from the wire when the sender authors it: the local inventory is
    // a replica whose items arrive on their own schedule, so a puppet could
    // spend the first seconds after a spawn holding a rifle the animator did
    // not know about yet.
    int tidx = 0;
    if (g_cl_net_intent && trk.intent.valid())
        tidx = int(trk.intent.weapon_slot());
    else if (CInventoryOwner* io = smart_cast<CInventoryOwner*>(GO))
        if (CWeapon* w = smart_cast<CWeapon*>(io->inventory().ActiveItem()))
            tidx = int(w->animation_slot());

    // ⭐A weapon on the back is not a weapon in the hands. The engine's own
    // animator makes exactly this substitution -- CStalkerAnimationManager
    // returns no_object_animation() when strapped() rather than the weapon set
    // for the slot -- and it has to be made here too, or the man walks around
    // in a rifle carry with nothing in his arms.
    //
    // ⭐⭐Asked through the SEAM, not off the wire's bit. The bit is what the
    // host said; strapped_state() is what this machine has decided to do with
    // it, and the bones the rifle actually hangs from are already hung off
    // that. Reading the raw bit here made the fourth reader of one fact, on its
    // own debounce, and the torso could return to the in-hands pose while the
    // rifle was still in the hand.
    bool strapped = (0 != (trk.posture & npc_posture_strapped));
    if (CObjectHandler* OH = smart_cast<CObjectHandler*>(GO))
        strapped = OH->strapped_state();
    if (strapped)
        tidx = 0;

    u8 action = u8((trk.posture >> npc_posture_action_shift) & npc_posture_action_mask);
    const u8 mental = u8((trk.posture >> npc_posture_mental_shift) & npc_posture_mental_mask);

    // Draw and holster are one-shots. Play each ONCE and then stop claiming
    // it: without an end event the blend holds its last frame, and the server
    // can sit in "strapping" long after the motion is over -- reported from
    // play as hands stuck behind the back before the weapon came out again.
    if (action == npc_action_strap || action == npc_action_unstrap)
    {
        if (trk.transition_action != action)
        {
            trk.transition_action = action; // a new one starts now
            trk.transition_ms = now_ms;
        }
        else if (now_ms - trk.transition_ms >= k_puppet_transition_ms)
            action = npc_action_idle; // it has had its time
    }
    else
        trk.transition_action = 0xff; // left the transition; the next one is new

    // A shot forces the aim set for a moment. The posture poll runs at 5 Hz
    // and reports a STATE; a burst is faster than that, and a creature that
    // just fired is unambiguously aiming. Cheap insurance against the gap.
    //
    // ⭐⭐INSURANCE FOR A GAP, NEVER AN OVERRIDE OF A STATED FACT.
    //
    // This filled in silence and overwrote speech. USER-REPORTED "ни одной
    // перезарядки не видел" while the host was measured to reload honestly:
    // the hold is 2500 ms, a reload is 2000 ms (untimed_state_duration), and it
    // begins the instant the last round leaves the barrel -- so the announced
    // reload lay ENTIRELY inside this window, every time, by construction. No
    // puppet has ever played one. The same swallow applies to a holster
    // ordered right after a fight.
    const u8 wire_action = action;
    if (npc_action_idle == action && trk.shot_ms && (Device.dwTimeGlobal - trk.shot_ms) < k_puppet_aim_hold_ms)
        action = npc_action_aim;

    // ⭐⭐THE ENGINE'S OWN TORSO TABLE, read off the same facts in the same
    // order, instead of a name assembled by hand.
    //
    // Both shipped selectors (no_object_animation for empty hands or a slung
    // rifle, unknown_object_animation for one in them) end in the SAME tail:
    //
    //   free + standing : animation[9].A[1]           -> torso_N_idle_1
    //   free + moving   : animation[7 + type].A[1]    -> torso_N_walk_1 / run_1
    //   otherwise       : animation[6].A[0/2/3]       -> torso_N_aim_0 / _2 / _3
    //
    // Two things were wrong here and both showed on screen. We played _idle_0,
    // a take single player never selects for anybody -- the alert carry, which
    // on a man with his weapon on his back reads as a sneak. And we ignored
    // MOVEMENT entirely, so a calm stalker walked with a torso frozen at rest
    // instead of the walking carry that swings with the stride.
    const bool calm = net_puppet_calm(trk);
    // ⭐The rendered speed, the same number the legs are animated from --
    // standing() and movement_type() are what single player consults here, and
    // on a driven body the picture is the only one of those that cannot lie.
    const u8 band = net_puppet_commit_band(trk);
    const bool standing = (band == puppet_band_idle || band == puppet_band_none);
    const bool running = (band == puppet_band_run);

    enum
    {
        t_idle,
        t_walk,
        t_run,
        t_aim,
        t_reload,
        t_strap,
        t_unstrap,
        t_count
    };
    static LPCSTR const k_act[t_count] = {"idle_", "walk_", "run_", "aim_", "reload_", "strap_", "unstrap_"};

    u8 act_id = t_idle;
    int var = 1;
    switch (action)
    {
    case npc_action_reload: act_id = t_reload; var = 0; break;
    case npc_action_strap: act_id = t_strap; var = 0; break;
    case npc_action_unstrap: act_id = t_unstrap; var = 0; break;
    // Actually aiming at something. The engine's fire/aim branch keeps the
    // standing take while walking and only swaps it at a run.
    case npc_action_aim: act_id = t_aim; var = running ? 3 : 0; break;
    default:
        if (calm)
        {
            act_id = standing ? t_idle : (running ? t_run : t_walk);
            var = 1;
        }
        else
        {
            act_id = t_aim;
            var = standing ? 0 : (running ? 3 : 2);
        }
        break;
    }

    const bool aim = (t_aim == act_id);

    // diag: the reload channel at the receiving end, printed BEFORE the change
    // guard below. A line placed after it could never report the one outcome
    // worth reporting -- "the wire carried it and this machine dropped it" --
    // because that is exactly the path that returns early. ⭐Same lesson as the
    // trace that sat behind four guards and printed nothing.
    static u32 s_diag_reload = 0;
    if ((npc_action_reload == wire_action || npc_action_reload == action) && s_diag_reload < 60)
    {
        ++s_diag_reload;
        const bool held = (trk.torso_idx >= 0 && s8(tidx) == trk.torso_idx && trk.played_action == act_id && trk.played_class == state_class && trk.played_var == u8(var));
        Msg("~ [reload] client: id=%u wire=%u -> action=%u shot=%dms tidx=%d act_id=%u var=%d %s", GO->ID(), wire_action, action,
            trk.shot_ms ? int(Device.dwTimeGlobal - trk.shot_ms) : -1, tidx, act_id, var, held ? "(already playing, no re-play)" : "-> re-play");
    }

    // Re-played only on a real change of what is actually being ASKED FOR --
    // weapon slot, state class, action and take. Comparing the raw posture byte
    // instead would re-trigger on bits the animation does not depend on, and
    // would miss the timeout above forcing a transition back to rest.
    // ⚠The take belongs in this key as much as the action does: idle_1 and
    // aim_0 are different clips, and leaving it out would hold whichever one
    // happened to start first through every mood change that followed.
    if (trk.torso_idx >= 0 && s8(tidx) == trk.torso_idx && trk.played_action == act_id && trk.played_class == state_class && trk.played_var == u8(var))
        return;
    trk.torso_idx = s8(tidx);
    trk.torso_aim = aim;
    trk.played_action = act_id;
    trk.played_class = state_class;
    trk.played_var = u8(var);

    LPCSTR sect = GO->cNameSect().c_str();
    LPCSTR pfx = net_puppet_prefix(trk);
    // One step back at a time: this take may be missing for this slot, this
    // action may be missing under this prefix, and the crouch or wounded set
    // may be missing for this visual entirely. ⚠The rest pose it lands on keeps
    // the MOOD -- falling from a calm walk onto the alert carry would put back
    // exactly the pose this change exists to remove.
    string64 c1, c2, c3, c4;
    xr_sprintf(c1, "%storso_%d_%s%d", pfx, tidx, k_act[act_id], var);
    xr_sprintf(c2, "%storso_%d_%s0", pfx, tidx, k_act[act_id]);
    xr_sprintf(c3, "%storso_%d_%s%d", pfx, tidx, calm ? "idle_" : "aim_", calm ? 1 : 0);
    xr_sprintf(c4, "norm_torso_%d_%s0", tidx, calm ? "idle_" : "aim_");
    // ⚠The last two are the guarantee that SOMETHING poses this torso -- both
    // are measured to exist on this content -- and the calm take goes first
    // even here. A skeleton left with no blend at all renders as a fan of
    // spikes, so the wrong mood beats no pose; it just should not be the wrong
    // mood by default.
    // ⚠The reload play bypasses the global 48-line cap on cycle resolution: a
    // fight eats those slots long before anybody reloads, and "which name in
    // the chain actually resolved" is the whole remaining question once the
    // action is known to arrive. A missing torso_N_reload_0 falls silently
    // through to the rest pose and looks exactly like a dropped announcement.
    static u32 s_diag_reload_play = 0;
    const bool log_play = (t_reload == act_id) && (s_diag_reload_play < 40);
    if (log_play)
        ++s_diag_reload_play;
    net_puppet_play_first_valid(ka, {c1, c2, c3, c4, "norm_torso_0_idle_1", "norm_torso_0_idle_0"}, sect, log_play);
    net_puppet_play_first_valid(ka, {"head_idle_0"}, sect);

    // diag: posture is a new channel and its cycles must be seen to resolve --
    // the plays above are throttled by a global cap that the spawn burst eats.
    // ⚠`want=` and not `cycle=`: this is the FIRST name of the chain, not the
    // one that resolved. The line above says which one actually started, and
    // the two have already differed.
    static u32 s_diag_posture = 0;
    if (s_diag_posture < 60)
    {
        ++s_diag_posture;
        // ⚠Both strapped values, because they are allowed to differ for the
        // length of a motion and the difference is the whole point: `wire` is
        // what the host said, `seam` is what this machine is drawing.
        Msg("* [netupd] posture id=%u '%s' -> crouch=%d wounded=%d strapped(wire=%d seam=%d) mental=%u calm=%d band=%u action=%u want='%s'", GO->ID(), sect,
            (trk.posture & npc_posture_crouch) ? 1 : 0, (trk.posture & npc_posture_wounded) ? 1 : 0, (trk.posture & npc_posture_strapped) ? 1 : 0, strapped ? 1 : 0, mental,
            calm ? 1 : 0, band, action, c1);
    }
}

// GE_NPC_ACTIVE_ITEM carried a posture byte for this puppet.
void net_puppet_note_posture(u16 id, u16 posture, u16 action_state)
{
    // Recorded, not applied: the animator reads the COMMITTED value, and the
    // commit waits out a debounce (see net_puppet_torso). Announcements arrive
    // only on change, so the waiting has to be done on a clock rather than
    // here -- a flap that resolves without a further announcement would
    // otherwise never be un-done.
    auto it = s_net_import_track.find(id);
    if (it != s_net_import_track.end())
    {
        it->second.posture_raw = posture;
        it->second.action_state = action_state;
    }
}

// ⭐⭐⭐The other way round: instead of imitating the single-player animator,
// RUN it.
//
// Everything above this line is a hand-built copy of a selector the engine
// already ships -- pick a cycle name from a speed band, PlayCycle it. It can
// never be "1 в 1 как в сингле", and not because of tuning: the real
// CStalkerAnimationPair CROSS-BLENDS between animations, while PlayCycle
// RESTARTS. Restarting where the engine blends is what "дёрганое" IS, and no
// threshold removes it.
//
// The shipped animator needs no brain -- it needs FACTS: what body state, what
// mental state, what movement type, how fast, and what the object handler is
// doing with the weapon. Every one of those arrives over the wire from the
// machine that does have a brain. So write them where it reads them and call
// it. Exactly the move that made aiming work in one go (feed the engine's own
// bone callbacks instead of rotating bones by hand).
//
// ⚠Kept behind a switch with the hand-rolled path still in place, because
// "identical to single player" is a claim that has to be MEASURED before it is
// believed -- cl_anim_trace prints both in one format, and cl_anim_native
// flips between them without a rebuild.
// ⭐⭐⭐ONE ANSWER TO "IS THIS CREATURE WALKING", SHARED BY BOTH ANIMATORS.
//
// The wire carries the creature's own movement_type, and on a puppet that is
// the AI's INTENT, not what is happening to the body: the server drives the
// transform directly, so a creature its AI believes is standing can be covering
// ground, and one it believes is walking can be held in place. The server's own
// roster says as much -- for a follower, the distance moved by SOMETHING ELSE
// ran 782 m against our driver's 270 m.
//
// And even when that value is right it is late: the posture broadcast is polled
// every 200 ms on the server and then held another 250 ms by the debounce here,
// so up to 450 ms -- seventy centimetres of gliding on idle legs at 1.6 m/s,
// and a step or two shorter than that never gets the walk posture at all.
//
// Reported from play in BOTH directions on the same afternoon: "ходят за мной
// не двигая ног" (body moving, wire said stand) and, measured twenty minutes
// later on the same creature, feet at 1.2 m/s under a root doing 0.008 (body
// still, wire said walk).
//
// So the legs follow the RENDERED speed, the one number that cannot disagree
// with the picture. Hysteresis and debounce live here, once, for both paths.
static u8 net_puppet_commit_band(SNetImportTrack& trk)
{
    // ⭐speed_auth, not spd_render: the wire's own measurement when the sender
    // provides one, the rendered estimate when it does not. Changing the INPUT
    // here rather than only inside the blend space keeps the animator A/B
    // honest -- otherwise switching animators would also switch what they are
    // being told, and the comparison would measure two things at once.
    const u8 want = net_puppet_pick_band(trk.band, trk.speed_auth);
    if (want == trk.band)
    {
        trk.band_cand = want; // candidate withdrawn
        return trk.band;
    }
    if (puppet_band_none == trk.band)
        return trk.band = want; // a fresh puppet starts its cycle at once

    const u32 now_ms = Device.dwTimeGlobal;
    if (want != trk.band_cand)
    {
        trk.band_cand = want;
        trk.band_cand_ms = now_ms;
        return trk.band;
    }
    if (now_ms - trk.band_cand_ms < k_puppet_band_debounce_ms)
        return trk.band;
    return trk.band = want;
}

static bool net_puppet_native_animation(CGameObject* GO, SNetImportTrack& trk)
{
    if (!g_cl_anim_native)
        return false;

    CAI_Stalker* S = smart_cast<CAI_Stalker*>(GO);
    if (!S || !S->g_Alive())
        return false;

    IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(GO->Visual());
    if (!ka || ka->LL_MotionsSlotCount() == 0)
        return false;

    const u16 p = trk.posture;
    const MonsterSpace::EBodyState body = (p & npc_posture_crouch) ? MonsterSpace::eBodyStateCrouch : MonsterSpace::eBodyStateStand;

    MonsterSpace::EMentalState mental = MonsterSpace::eMentalStateFree;
    switch ((p >> npc_posture_mental_shift) & npc_posture_mental_mask)
    {
    case npc_mental_danger: mental = MonsterSpace::eMentalStateDanger; break;
    case npc_mental_panic: mental = MonsterSpace::eMentalStatePanic; break;
    default: break;
    }

    // ⚠Crouching is only legal outside the free state (the engine's own
    // setters assert it). A creature that crouches is by definition not calm,
    // so this is a repair of an impossible pair, not a policy.
    if (MonsterSpace::eBodyStateCrouch == body && MonsterSpace::eMentalStateFree == mental)
        mental = MonsterSpace::eMentalStateDanger;

    // ⚠NOT (p >> npc_posture_move_shift): that is the wire's movement_type, and
    // it is neither timely nor about this body -- see net_puppet_commit_band.
    // The style bits above (crouch, mental, wounded) still come from the wire:
    // those the picture cannot measure.
    MonsterSpace::EMovementType move = MonsterSpace::eMovementTypeStand;
    switch (net_puppet_commit_band(trk))
    {
    case puppet_band_walk: move = MonsterSpace::eMovementTypeWalk; break;
    case puppet_band_run: move = MonsterSpace::eMovementTypeRun; break;
    default: break; // idle, or not picked yet: standing
    }

    // ⭐The speed the animator sees is the one that matches the ground the
    // model covers -- the sender's measurement of it where there is one, the
    // rendered estimate otherwise. standing() consults it first.
    S->movement().net_set_state(body, mental, move, trk.speed_auth);
    S->CObjectHandler::planner().m_net_forced_state = trk.action_state;

    // ⚠The capsule too. standing() falls through to the physics character's
    // velocity when the character is enabled, and a driven puppet's capsule is
    // pinned at zero by every net_MoveTo -- which would tell the animator that
    // a walking man is standing still.
    if (CPHMovementControl* mc = S->character_physics_support()->movement())
        if (mc->CharacterExist() && mc->IsCharacterEnabled())
            mc->DisableCharacter();

    S->animation().update();
    return true;
}

static void net_puppet_locomotion(CGameObject* GO, CSE_Abstract* mirror, SNetImportTrack& trk)
{
    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(mirror);
    if (!creature || creature->fHealth <= 0.f)
        return; // corpse posing is a separate feature

    const bool is_stalker = (nullptr != smart_cast<CSE_ALifeHumanStalker*>(mirror));
    const bool is_actor = (nullptr != smart_cast<CSE_ALifeCreatureActor*>(mirror));

    if (is_stalker)
        net_puppet_commit_posture(trk); // debounce first -- every path animates off the committed value

    // ⭐⭐⭐THE LEGS AS A BLEND SPACE.
    //
    // Three animators now exist for this body and exactly one may run: the
    // blend space, the shipped CStalkerAnimationManager, and the hand-rolled
    // band selector. They are ordered rather than combined for the reason this
    // file keeps repeating -- two writers of one value is the defect class that
    // has cost this build six separate evenings.
    //
    // The blend space owns the LEGS only. The torso keeps its own clock below,
    // and the aim layer keeps feeding the engine's bone callbacks from the
    // tick: what a man is doing with his weapon and how fast he is walking are
    // independent facts and were never the same decision.
    if (g_cl_anim_blend && (is_stalker || is_actor))
    {
        if (IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(GO->Visual()))
        {
            ClAnim::CLegsBlendSpace::SInput bs;
            bs.speed = trk.speed_auth;
            bs.fwd = trk.fwd_auth;
            bs.state_class = is_actor ? 0 : net_puppet_state_class(trk);
            // ⭐The tariff, not the cycle: panic is where the sprint lives.
            // Read off the committed posture so it debounces with the rest.
            bs.mental = is_actor ? u8(npc_mental_free) : u8((trk.posture >> npc_posture_mental_shift) & npc_posture_mental_mask);
            // ⭐Read straight off the wire rather than out of the debounced
            // posture: the sprint bit is the SERVER'S statement about how fast
            // it is carrying this body right now, and holding it back through a
            // debounce would pair the cycle with last quarter-second's tariff.
            bs.sprint = (0 != g_cl_net_intent) && trk.intent.valid() && trk.intent.sprinting();
            bs.actor = is_actor;
            trk.legs_bs.Update(ka, GO->cNameSect().c_str(), GO->ID(), bs, Device.fTimeDelta);

            if (is_stalker)
                net_puppet_torso(GO, trk, false /*the blend space owns the legs*/);
            else if (!trk.actor_torso_started && trk.legs_bs.built())
            {
                // A remote player's body: the actor's torso set, posed once.
                // Nothing replicates a player's upper-body state yet, so there
                // is no clock for it to run on.
                trk.actor_torso_started = true;
                LPCSTR sect = GO->cNameSect().c_str();
                net_puppet_play_first_valid(ka, {"norm_torso_5_aim_0", "norm_torso_0_idle_0"}, sect, true);
                net_puppet_play_first_valid(ka, {"head_idle_0"}, sect, true);
            }
            return;
        }
    }
    else if (trk.legs_bs.built())
    {
        // Switched off at runtime (the A/B). Manual blends have no curve to
        // carry them to zero, so they must be handed back explicitly before
        // another animator is allowed near this partition.
        if (IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(GO->Visual()))
            trk.legs_bs.Release(ka);
        trk.band = puppet_band_none; // whoever takes over starts its cycle at once
        trk.legs_state = 0xff;
    }

    // ⚠Read BEFORE the torso runs, because the torso now commits the band too:
    // it is indexed by the movement class in single player, and there is one
    // place that decides what that is. Sampling `prev` afterwards would see
    // this frame's answer as the previous one and the legs would never be
    // told the band had changed.
    const u8 prev = trk.band;
    trk.dbg_id = u16(GO->ID());

    if (is_stalker)
    {
        if (net_puppet_native_animation(GO, trk))
            return;
        net_puppet_torso(GO, trk);
    }

    IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(GO->Visual());
    if (!ka)
        return;

    const u8 band = net_puppet_commit_band(trk); // hysteresis + debounce live there
    if (band == prev)
    {
        // same band, fresh speed estimate: keep the stride rate tracking it
        net_puppet_retune_legs(ka, trk);
        return;
    }

    // diag: first few band switches prove locomotion cycles really start
    static u32 s_diag_bands = 0;
    if (s_diag_bands < 24)
    {
        ++s_diag_bands;
        Msg("* [netupd] puppet '%s' id=%u band=%u speed-band change", GO->cNameSect().c_str(), GO->ID(), band);
    }

    LPCSTR sect = GO->cNameSect().c_str();
    if (smart_cast<CSE_ALifeCreatureActor*>(mirror))
    {
        // remote-player puppet: the actor model is a stalker skeleton -- legs
        // share the norm_* trio, but the torso set is the actor's own (SActorState
        // uses norm_torso_5_aim_0; the NPC norm_torso_0_idle_0 may be absent)
        LPCSTR legs = band == puppet_band_idle ? "norm_idle_0" : (band == puppet_band_walk ? "norm_walk_fwd_0" : "norm_run_fwd_0");
        MotionID m;
        CBlend* B = net_puppet_play_first_valid(ka, {legs}, sect, true /*actor puppet: never throttle this diag*/, &m);
        net_puppet_note_legs(ka, trk, B, m, (band < u8(sizeof(k_puppet_native_speed) / sizeof(k_puppet_native_speed[0]))) ? k_puppet_native_speed[band] : 0.f);
        net_puppet_retune_legs(ka, trk);
        if (prev == puppet_band_none)
        {
            net_puppet_play_first_valid(ka, {"norm_torso_5_aim_0", "norm_torso_0_idle_0"}, sect, true);
            net_puppet_play_first_valid(ka, {"head_idle_0"}, sect, true);
        }
    }
    else if (smart_cast<CSE_ALifeHumanStalker*>(mirror))
    {
        // one place builds the legs cycle name, for both callers -- the band
        // change here and the state change in net_puppet_torso
        net_puppet_play_legs(ka, trk, sect);
        // the torso is handled by net_puppet_torso above, on its own clock
    }
    else if (smart_cast<CSE_ALifeMonsterBase*>(mirror))
    {
        // cycle names differ per species -- probe the common variants and skip
        // silently if this skeleton has none of them
        CBlend* B = nullptr;
        MotionID m;
        switch (band)
        {
        case puppet_band_idle: B = net_puppet_play_first_valid(ka, {"stand_idle_0", "idle_0"}, sect, false, &m); break;
        case puppet_band_walk: B = net_puppet_play_first_valid(ka, {"stand_walk_fwd_0", "walk_fwd_0", "stand_walk_0"}, sect, false, &m); break;
        default: B = net_puppet_play_first_valid(ka, {"stand_run_0", "stand_run_fwd_0", "run_fwd_0", "run_0"}, sect, false, &m); break;
        }
        // A dog's run is not the stalker table's 3.0 m/s -- ask this species'
        // own ltx, the same lines its drive reads on the server.
        net_puppet_note_legs(ka, trk, B, m, net_puppet_species_native(sect, band));
        net_puppet_retune_legs(ka, trk);
    }
}

// ⭐⭐⭐Exactly one animator owns a puppet's skeleton, and the switch between
// them happens here and only here.
//
// Whoever gives the body up leaves blends behind, and they are not alike:
//  - the blend space's are MANUAL and never fade on their own -- released always;
//  - towards the server's clips, everything else may stay: every clip the
//    server path starts cross-fades out whatever is on its part, exactly as a
//    clip change does, so the switch is a blend instead of a pop. (It happens
//    whenever the server starts or stops driving a creature, so it is seen.)
//  - towards the local selectors, the skeleton is cleared: the blend space
//    builds its cycles without fading anything, and a server clip left
//    looping underneath would never go away.
// Every "what I last played" memory on both sides is forgotten either way.
static void net_puppet_hand_over(CGameObject* GO, SNetImportTrack& trk, bool server_clips)
{
    if (server_clips == trk.anim_owned)
        return;

    if (IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(GO->Visual()))
    {
        if (trk.legs_bs.built())
            trk.legs_bs.Release(ka);
        if (!server_clips)
            for (u16 p = 0; p < MAX_PARTS; ++p)
                ka->LL_CloseCycle(p, u8(-1));
    }
    // The shipped animator's pairs remember blends too (cl_anim_native path).
    if (CAI_Stalker* S = smart_cast<CAI_Stalker*>(GO))
        S->animation().reinit();

    for (auto& ap : trk.anim_play)
        ap = SNetImportTrack::SAnimPlay{};
    trk.band = puppet_band_none;
    trk.legs_state = 0xff;
    trk.legs_blend = nullptr;
    trk.torso_idx = -1;
    trk.played_action = trk.played_class = trk.played_var = 0xff;
    trk.actor_torso_started = false;
    trk.anim_owned = server_clips;

    static u32 s_diag = 0;
    if (++s_diag <= 24 || 0 == (s_diag % 100))
        Msg("* [netupd] puppet id=%u '%s': animated by %s", GO->ID(), GO->cNameSect().c_str(), server_clips ? "the SERVER's animator (its clips)" : "the local selectors");
}

// ⭐⭐⭐PLAY WHAT THE SERVER'S ANIMATOR IS PLAYING.
//
// Per bone part: if the server has moved to a different clip, start it with the
// clip's own accrue/falloff -- the same LL_PlayCycle the server's
// CStalkerAnimationPair made, so the cross-fade runs the same curve here. Then
// keep the phase on the server's: its phase at the snapshot, carried forward to
// the render time at the clip's rate. Small drift is pulled in over a few
// frames, a big one (a missed clip start, a server hitch) is snapped.
//
// No threshold here decides WHAT to play. That is the whole point.
static void net_puppet_server_clips(CGameObject* GO, SNetImportTrack& trk, const SNetAnimState& st, u32 st_t, u32 rt)
{
    IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(GO->Visual());
    if (!ka)
        return;
    const u16 slots = ka->LL_MotionsSlotCount();
    const float ahead = (rt > st_t) ? float(rt - st_t) / 1000.f : 0.f; // render time past the snapshot

    bool changed = false;
    for (u16 p = 0; p < SNetAnimState::kParts; ++p)
    {
        if (!st.has(p))
            continue; // the server plays nothing new there; what is playing fades on its own curve
        const SNetAnimState::Part& in = st.part[p];

        // ⚠Range-checked, because a MotionID is an index into THIS visual's
        // motion sets and only agrees with the server's while both loaded the
        // same .omf list. A mismatch must cost a wrong clip, never a read past
        // the end of a table (LL_GetMotionDef does not check).
        if (in.slot >= slots)
            continue;
        const u32 count = u32(const_cast<shared_motions&>(ka->LL_MotionsSlot(in.slot)).motion_map()->size());
        if (in.idx >= count)
            continue;

        const MotionID m(in.slot, in.idx);
        const CMotionDef* def = ka->LL_GetMotionDef(m);
        if (!def)
            continue;
        const float speed = st.speed(p);

        SNetImportTrack::SAnimPlay& play = trk.anim_play[p];
        CBlend* B = play.blend;
        // the slot is pooled: trust it only while it still plays our clip
        if (B && (CBlend::eFREE_SLOT == B->blend_state() || B->motionID != play.motion))
            B = play.blend = nullptr;

        bool started = false;
        if (!B || play.motion != m)
        {
            B = ka->LL_PlayCycle(p, m, TRUE, def->Accrue(), def->Falloff(), speed, def->StopAtEnd(), nullptr, nullptr, 0);
            play.motion = m;
            play.blend = B;
            started = true;
            changed = true;
            if (!B)
                continue;
        }

        B->speed = speed;

        float want = in.time_ms / 1000.f + ahead * speed;
        const float total = B->timeTotal;
        if (total > EPS)
        {
            if (B->stop_at_end)
            {
                if (want > total - END_EPS)
                    want = total - END_EPS;
            }
            else
                want = fmodf(want, total);
        }
        float err = want - B->timeCurrent;
        if (!B->stop_at_end && total > EPS)
        {
            if (err > total * 0.5f)
                err -= total;
            else if (err < -total * 0.5f)
                err += total;
        }

        // ⭐The server started this SAME clip over again: its phase is back near
        // the start while ours is well into it. In single player that is a new
        // blend cross-fading in from the old one, so it is one here too --
        // hopping the running blend back would be a jump the server never made.
        //
        // Told apart from a loop wrapping round by where OUR phase is: a wrap
        // happens from the last moments of the clip, a restart from anywhere
        // else. Measured on the first cut (which only caught jumps over 250 ms):
        // the aim and fire clips of a fighting stalker are restarted by the
        // server a fifth of a second in, and every one of those read as a hop.
        const float back = B->timeCurrent - want; // unwrapped: how far the server's phase is behind ours
        const bool near_end = !B->stop_at_end && total > EPS && (total - B->timeCurrent) < 0.3f * _max(speed, 0.1f);
        bool restarted = false;
        if (!started && want < 0.3f && back > 0.1f && !near_end)
        {
            B = ka->LL_PlayCycle(p, m, TRUE, def->Accrue(), def->Falloff(), speed, def->StopAtEnd(), nullptr, nullptr, 0);
            play.blend = B;
            restarted = true;
            if (!B)
                continue;
            B->speed = speed;
        }

        // ⭐How well the phase is followed, over every part-frame. The phase is
        // SET, not chased: the server's phase carried to render time is smooth
        // in render time, so assigning it is smooth -- while chasing it depends
        // on the local clock advancing the blend, and it does not advance at all
        // for a body the renderer is not drawing. So `err` here is just how far
        // the value moved since last frame's assignment: about a frame's worth
        // of playback normally, and a HOP when the server's own phase jumped.
        {
            static u32 s_n = 0, s_hops = 0, s_starts = 0, s_restarts = 0, s_next = 3000;
            static u32 s_hist[4] = {};
            static u32 s_hop_diag = 0;
            if (started)
                ++s_starts;
            else if (restarted)
                ++s_restarts;
            else
            {
                ++s_n;
                const float a = _abs(err);
                ++s_hist[a < 0.03f ? 0 : (a < 0.1f ? 1 : (a < 0.25f ? 2 : 3))];
                if (a >= 0.1f)
                {
                    ++s_hops;
                    if (s_hop_diag < 24)
                    {
                        ++s_hop_diag;
                        const std::pair<LPCSTR, LPCSTR> nm = ka->LL_MotionDefName_dbg(m);
                        Msg("~ [netupd] server clip HOP id=%u part=%u '%s' local=%.3f want=%.3f (srv %.3f + %.3f ahead x %.2f) total=%.3f loop=%d", GO->ID(), p,
                            nm.first ? nm.first : "?", B->timeCurrent, want, in.time_ms / 1000.f, ahead, speed, total, B->stop_at_end ? 0 : 1);
                    }
                }
            }
            if (s_n >= s_next)
            {
                s_next = s_n + 3000;
                Msg("* [netupd] server clips: %u part-frames | phase step <30ms %u, <100ms %u, <250ms %u, >=250ms %u | hops (>=100ms) %.2f%% | clip starts %u, restarts %u", s_n,
                    s_hist[0], s_hist[1], s_hist[2], s_hist[3], 100.f * float(s_hops) / float(s_n), s_starts, s_restarts);
            }
        }

        B->timeCurrent = want;
    }

    // cl_anim_trace 2 on the client side of the same creature: one line per
    // change, the clip names by part. Diff against the server's `[anim]` lines.
    if (changed && g_cl_anim_trace && (g_cl_anim_trace_id < 0 || u16(g_cl_anim_trace_id) == u16(GO->ID())))
    {
        auto name = [ka](const MotionID& m) -> LPCSTR {
            if (!m.valid())
                return "-";
            const std::pair<LPCSTR, LPCSTR> nm = ka->LL_MotionDefName_dbg(m);
            return nm.first ? nm.first : "?";
        };
        Msg("~ [anim-net] id=%u t=%u | legs='%s' | torso='%s' | head='%s' | behind=%ums", GO->ID(), Device.dwTimeGlobal, name(trk.anim_play[0].motion),
            name(trk.anim_play[1].motion), name(trk.anim_play[2].motion), Device.dwTimeGlobal - rt);
    }
}

// GE_NPC_SHOT arrived for this puppet: latch the aiming pose. Called from
// CGameObject::OnEvent -- the track map is private to this translation unit
// and there is no reason for it to stop being.
//
// ⚠A creature with no track yet is ignored on purpose rather than given one:
// tracks are created by the update stream, which owns their contents, and an
// empty one here would look like a puppet that had been seen standing still.
void net_puppet_note_shot(u16 id)
{
    auto it = s_net_import_track.find(id);
    if (it != s_net_import_track.end())
        it->second.shot_ms = Device.dwTimeGlobal;
}

void net_import_mirrors_free()
{
    for (auto& it : s_net_import_mirrors)
        F_entity_Destroy(it.second);
    s_net_import_mirrors.clear();
    s_net_import_track.clear();
    s_pending_deaths.clear(); // ids from the world being torn down mean nothing in the next one
}

static void cl_Process_EntityUpdates(NET_Packet& P)
{
    u32 diag_applied = 0, diag_skipped = 0, diag_moved = 0;
    // diag: sampled full dump of the tail framing -- entries can go missing
    // between the server's write and this parse, and the counters alone don't
    // say WHICH ids made it
    static u32 s_diag_pass = 0;
    const bool dump_pass = (P.r_elapsed() >= 3) && (++s_diag_pass % 63 == 1);
    string512 dump_ids;
    u32 dump_len = 0;
    if (dump_pass)
        dump_len = (u32)sprintf_s(dump_ids, "* [netupd] cl tail %u bytes:", P.r_elapsed());
    while (P.r_elapsed() >= 3) // u16 id + u8 size header
    {
        u16 id = P.r_u16();
        u8 size = P.r_u8();
        if (P.r_elapsed() < size)
        {
            // diag: a truncated entry means the size framing lied upstream --
            // everything after it in this packet is silently lost
            static u32 s_diag_trunc = 0;
            if (s_diag_trunc < 8)
            {
                ++s_diag_trunc;
                Msg("! [netupd] cl parse: TRUNCATED entry id=%u size=%u elapsed=%u -- dropping rest of tail", id, size, P.r_elapsed());
            }
            break; // truncated tail -- drop the rest
        }
        const u32 next = P.r_tell() + size;
        // diag: high ids are the runtime-spawned actors -- trace their arrival
        if (id >= 2000)
        {
            static u32 s_diag_hi = 0;
            if (++s_diag_hi % 63 == 1)
                Msg("* [netupd] cl parse: entry id=%u size=%u", id, size);
        }
        if (dump_pass && dump_len + 8 < sizeof(dump_ids))
            dump_len += (u32)sprintf_s(dump_ids + dump_len, sizeof(dump_ids) - dump_len, " %u", id);

        do
        {
            CObject* O = Level().Objects.net_Find(id);
            // our own controlled entity is client-authoritative -- never apply
            // An update for our OWN controlled entity used to be dropped here as a
            // pointless echo. It is not an echo any more: the server excludes that
            // entity from our stream unless it REFUSED where we said we were, so
            // anything arriving for it is a correction and has to be obeyed.
            if (!O || O->getDestroy())
            {
                ++diag_skipped;
                // diag: an id we keep receiving updates for but can't resolve to
                // an object means its spawn never landed here -- log the first few
                if (!O)
                {
                    static u32 s_diag_unknown = 0;
                    if (s_diag_unknown < 16)
                    {
                        ++s_diag_unknown;
                        Msg("! [netupd] cl apply: update for UNKNOWN id=%u (no local object)", id);
                    }
                }
                break;
            }
            CGameObject* GO = smart_cast<CGameObject*>(O);
            if (!GO)
            {
                ++diag_skipped;
                break;
            }

            CSE_Abstract*& mirror = s_net_import_mirrors[id];
            if (mirror && 0 != xr_strcmp(mirror->name(), *GO->cNameSect()))
            {
                // id was reused by an object of a different section
                F_entity_Destroy(mirror);
                mirror = nullptr;
            }
            if (!mirror)
            {
                mirror = F_entity_Create(*GO->cNameSect());
                if (!mirror)
                {
                    ++diag_skipped;
                    break;
                }
                mirror->ID = id;
            }
            mirror->UPDATE_Read(P);

            // ⭐The presentation intent, written after the entity's own payload
            // inside the SAME size-prefixed chunk -- see
            // xrServer::write_entity_update for why it is not part of
            // UPDATE_Read itself (short version: UPDATE_Read is also how
            // `all.spawn` is loaded, and appending to it kills level load).
            //
            // ⚠Presence is decided by what is left in the chunk, not by a
            // version: a sender that writes no intent simply leaves the chunk
            // ending where UPDATE_Read stopped, and this reads nothing. That
            // keeps the field optional per entity rather than per build.
            if (CSE_ALifeCreatureAbstract* intent_owner = smart_cast<CSE_ALifeCreatureAbstract*>(mirror))
            {
                intent_owner->o_intent.clear();
                if (P.r_tell() + 5u <= next)
                {
                    P.r_u8(intent_owner->o_intent.flags);
                    P.r_u8(intent_owner->o_intent.slot);
                    P.r_s8(intent_owner->o_intent.v_fwd);
                    P.r_s8(intent_owner->o_intent.v_str);
                    P.r_s8(intent_owner->o_intent.turn);
                }

                // ...and the server animator's clips, same rule: present only
                // if the chunk still has bytes. A part that would run past the
                // end of the chunk voids the whole block rather than half of it.
                SNetAnimState& A = intent_owner->o_anim;
                A.clear();
                if (P.r_tell() + 1u <= next)
                {
                    const u8 mask = P.r_u8();
                    bool whole = true;
                    for (u32 p = 0; p < SNetAnimState::kParts; ++p)
                        if (mask & (1u << p))
                        {
                            if (P.r_tell() + 6u > next)
                            {
                                whole = false;
                                break;
                            }
                            P.r_u8(A.part[p].slot);
                            P.r_u16(A.part[p].idx);
                            P.r_u16(A.part[p].time_ms);
                            P.r_u8(A.part[p].speed_q);
                        }
                    A.mask = whole ? mask : u8(0);
                }
            }

            // Correction for the entity we drive: the server did not accept our
            // position and this is where it says we actually are. Snap and stop --
            // no interpolation track, we are not a puppet to ourselves.
            if (O == Level().CurrentControlEntity())
            {
                if (CSE_ALifeCreatureAbstract* self = smart_cast<CSE_ALifeCreatureAbstract*>(mirror))
                {
                    Fmatrix F = GO->XFORM();
                    F.c = self->o_Position;
                    GO->net_MoveTo(F);
                    GO->spatial_move();

                    static u32 s_corrections = 0;
                    if ((++s_corrections % 31) == 1)
                        Msg("! [sv_move] position corrected by server to (%.2f, %.2f, %.2f) -- corrections=%u", VPUSH(self->o_Position), s_corrections);
                }
                ++diag_applied;
                break;
            }

            // first pass: creatures only -- position + model heading; classes
            // without a ForceTransform override make it a no-op
            if (CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(mirror))
            {
                ++diag_applied;

                // snapshot bookkeeping: the puppet is NOT moved here -- the
                // per-frame interpolator (net_import_interpolate_tick) renders
                // between the two most recent snapshots. Here we only record
                // them, count movers (>5 cm; a walking NPC covers ~7 cm per
                // 20 Hz update) and smooth the speed for the locomotion band.
                SNetImportTrack* trk;
                const u32 now_ms = Device.dwTimeGlobal;
                auto trk_it = s_net_import_track.find(id);
                if (trk_it == s_net_import_track.end())
                {
                    trk = &s_net_import_track[id];
                    trk->first = trk->pos_a = trk->pos_b = creature->o_Position;
                    trk->yaw_a = trk->yaw_b = creature->o_model;
                    trk->aim_a = trk->aim_b = creature->o_torso.yaw;
                    trk->pitch_a = trk->pitch_b = creature->o_torso.pitch;
                    trk->intent_a = trk->intent_b = trk->intent = creature->o_intent;
                    trk->anim_b = creature->o_anim;
                    trk->t_a = trk->t_b = now_ms;
                    trk->ts_b = creature->timestamp;
                    trk->acc_dist = 0.f;
                    trk->win_ms = now_ms;
                    trk->speed_ema = 0.f;
                    trk->band = puppet_band_none;
                    // first sight: place the puppet directly, the interpolator
                    // takes over from the second snapshot on.
                    // net_ServerDriveTo, not net_MoveTo -- see the interpolator.
                    GO->net_ServerDriveTo(creature->o_Position, creature->o_model);
                    // the drive writes XFORM directly; nothing else updates the
                    // spatial tree for a disabled puppet (no UpdateCL), so its
                    // sphere would stay at the spawn point forever and the main
                    // view's q_frustum culls it from the wrong position
                    GO->spatial_move();
                }
                else
                {
                    trk = &trk_it->second;
                    const u32 ts = creature->timestamp;
                    // Fresh means the stamp advanced OR the position moved.
                    // The stamp alone is a LIAR here: the host's in-process
                    // export refreshes o_Position at frame rate, but the
                    // stamp only changes on the ~10 Hz schedule push -- a
                    // stamp-only dedup threw away every second build TOGETHER
                    // WITH ITS REAL POSITION, and the surviving snapshots
                    // paired ~10 Hz stamps with positions off the 20 Hz build
                    // grid: strides alternated 0.17/0.25 m at a "constant"
                    // dt, the rendered speed pulsed +/-30%, and the walk read
                    // as jerky no matter how smooth the interpolation was.
                    const bool stamp_fresh = (ts != trk->ts_b);
                    Fvector dmv = creature->o_Position;
                    dmv.sub(trk->pos_b);
                    const bool pos_moved = dmv.magnitude() > 0.01f;
                    if (stamp_fresh || pos_moved)
                    {
                        Fvector d = creature->o_Position;
                        d.sub(trk->pos_b);
                        d.y = 0.f;
                        const float dist = d.magnitude();
                        if (dist > 0.05f)
                            ++diag_moved;
                        if (dist > 8.f)
                        {
                            // server-side teleport: snap, do not glide across the map
                            trk->pos_a = creature->o_Position;
                            trk->yaw_a = creature->o_model;
                            trk->aim_a = creature->o_torso.yaw;
                            trk->pitch_a = creature->o_torso.pitch;
                            trk->intent_a = creature->o_intent;
                            trk->t_a = now_ms;
                            trk->t_b = now_ms;
                            // and drop the history with it: snapshots from
                            // before a teleport would otherwise bracket the
                            // render time and glide the puppet across the map,
                            // which is the very thing this branch exists to
                            // prevent.
                            trk->ring_n = 0;
                            trk->acc_dist = 0.f;
                            trk->win_ms = now_ms;
                            trk->speed_ema = 0.f;
                            trk->ts_synth = 0;
                        }
                        else
                        {
                            trk->acc_dist += dist;
                            trk->pos_a = trk->pos_b;
                            trk->yaw_a = trk->yaw_b;
                            trk->aim_a = trk->aim_b;
                            trk->pitch_a = trk->pitch_b;
                            trk->intent_a = trk->intent_b;
                            trk->t_a = trk->t_b;
                            // Pace the local timeline by the SENDER's stamp
                            // deltas, not by arrival: the transport delivers in
                            // bursts (several packets per frame, then a gap),
                            // and arrival-stamped snapshots collapse the lerp
                            // interval to zero -- the puppet held still, then
                            // stepped once per burst, which read as ~10-15 fps
                            // walking from play. Bounded drift keeps a stalled
                            // stream or a garbage stamp from running the
                            // timeline away from real time.
                            // dt: from the stamp when it advanced; the build
                            // cadence (50 ms) when only the position moved.
                            // ts_synth remembers how much synthetic time was
                            // inserted so the next honest stamp delta is not
                            // double-counted.
                            u32 dt_wire = 50;
                            if (stamp_fresh && ts > trk->ts_b)
                            {
                                const u32 raw = ts - trk->ts_b;
                                dt_wire = raw > trk->ts_synth ? raw - trk->ts_synth : 30;
                                if (dt_wire > 250)
                                    dt_wire = 250;
                                if (dt_wire < 30)
                                    dt_wire = 30;
                                trk->ts_synth = 0;
                            }
                            else if (!stamp_fresh)
                                trk->ts_synth += dt_wire;
                            u32 t_next = trk->t_b + dt_wire;
                            const u32 lo = now_ms > 300 ? now_ms - 300 : 0;
                            const u32 hi = now_ms + 150;
                            if (t_next < lo)
                                t_next = lo;
                            if (t_next > hi)
                                t_next = hi;
                            trk->t_b = t_next;
                            trk->dt_ema += (float(dt_wire) - trk->dt_ema) * 0.2f;

                            // diag: the raw accepted snapshot stream -- stamp
                            // cadence and wire yaw with no smoothing in the
                            // way. Only MOVING snapshots (a standing guard
                            // burned the whole cap in seconds), capped.
                            // ⚠`dt` here is the RECONSTRUCTED interval, not the
                            // sender's. It is clamped to [30,250], defaults to
                            // 50 when only the position moved, and carries
                            // ts_synth corrections -- so a speed computed from
                            // it measures this reconstruction as much as it
                            // measures the wire. `raw` is the sender's own
                            // stamp delta, untouched; when the two disagree the
                            // roughness is being manufactured HERE, and when
                            // they agree it arrived that way.
                            // ⚠The cap is not a formality. A dozen creatures at
                            // 20 Hz spend 1000 lines in four seconds, so in the
                            // very case worth measuring -- a fight -- the diag
                            // is exhausted before the recording window opens,
                            // and the harness reads its silence as "nothing
                            // arrived". cl_snap_watch narrows it to the one
                            // entity under test and pays for a long window.
                            static u32 s_diag_snaps = 0;
                            const bool watched = g_cl_snap_watch > 0 && id == u16(g_cl_snap_watch);
                            if (dist > 0.03f && (watched ? s_diag_snaps < 30000 : (g_cl_snap_watch <= 0 && s_diag_snaps < 1000)))
                            {
                                ++s_diag_snaps;
                                const u32 raw_delta = (ts > trk->ts_b) ? (ts - trk->ts_b) : 0;
                                Msg("* [netupd] snap id=%u dt=%u raw=%u yaw=%.2f look=%.2f/%.2f pos=(%.2f, %.2f) d=%.3f", id, dt_wire, raw_delta, creature->o_model,
                                    creature->o_torso.yaw, creature->o_torso.pitch, creature->o_Position.x, creature->o_Position.z, dist);
                            }
                        }
                        trk->pos_b = creature->o_Position;
                        trk->yaw_b = creature->o_model;
                        trk->aim_b = creature->o_torso.yaw;
                        trk->pitch_b = creature->o_torso.pitch;
                        trk->intent_b = creature->o_intent;
                        trk->anim_b = creature->o_anim;
                        trk->ts_b = ts;

                        // ...and into the ring on the SAME timeline as t_b, so
                        // the tick can find whichever pair actually brackets
                        // its render time instead of being handed the newest
                        // two and clamping.
                        {
                            SNetSnap& s = trk->ring[trk->ring_n % k_net_ring];
                            s.t = trk->t_b;
                            s.pos = trk->pos_b;
                            s.yaw = trk->yaw_b;
                            s.aim = trk->aim_b;
                            s.pitch = trk->pitch_b;
                            s.intent = trk->intent_b;
                            s.anim = trk->anim_b;
                            ++trk->ring_n;
                        }
                    }
                }
                // ⭐The server owns creature health (sv_npc_health.h), so a
                // negative number here is the death arriving. Without this the
                // body just stopped animating and stayed on its feet -- the
                // locomotion selector below returns early for a corpse and
                // there was nothing else to make it fall. Reported from play:
                // "стоял и не падал" while being shot at a body the server had
                // already killed a minute earlier.
                //
                // Killing the local copy is what GE_DIE would do, and it is
                // idempotent: if the event did arrive and already did the work,
                // g_Alive() is false and this is a no-op. Which of the two
                // happened is visible in the log line.
                if (creature->fHealth <= 0.f && !trk->dead)
                {
                    trk->dead = true;
                    // ⚠QUEUED, not done here. Death is heavy -- physics shell,
                    // inventory handover, callbacks -- and this is the middle of
                    // parsing an update packet. Killing from here overflowed the
                    // player HUD's 64-blend list a few seconds later; the engine
                    // runs its own GE_DIE from ProcessGameEvents for the same
                    // reason. Drained at the top of the interpolate tick.
                    s_pending_deaths.push_back(u16(GO->ID()));
                }
                // A corpse is left alone from here on: the interpolator below
                // skips dead tracks, because a ragdoll that gets its transform
                // rewritten every tick is a body pinned upright.
                if (trk->dead)
                    break;

                // close the speed window: wall-clock based, so burst delivery of
                // several updates in one frame cannot fake a huge speed.
                // (Kept as a diagnostic estimate; locomotion is now driven per
                // frame from the RENDERED displacement in the interpolate tick
                // -- wire speed lagged every stop/start and kept the walk
                // cycle playing through holds.)
                if (now_ms - trk->win_ms >= 400)
                {
                    const float win_speed = trk->acc_dist / (float(now_ms - trk->win_ms) / 1000.f);
                    trk->speed_ema += (win_speed - trk->speed_ema) * 0.5f;
                    trk->acc_dist = 0.f;
                    trk->win_ms = now_ms;
                }

                // debug hook: -net_jump_npc teleports the controlled entity next
                // to the first live networked creature that has MOVED at least
                // 1 m from where we first saw it -- a stationary guard proves
                // nothing about movement really arriving over the wire
                static bool s_jumped = false;
                if (!s_jumped && creature->fHealth > 0.f && trk->first.distance_to(creature->o_Position) > 1.f && strstr(Core.Params, "-net_jump_npc"))
                {
                    if (CObject* me = Level().CurrentControlEntity())
                    {
                        if (CGameObject* meGO = smart_cast<CGameObject*>(me))
                        {
                            s_jumped = true;
                            g_net_watch_id = id;    // follow-cam target (tracked every frame below)
                            Fmatrix MF = meGO->XFORM();
                            MF.c = creature->o_Position;
                            MF.c.x += 3.f;
                            MF.c.y += 0.5f;
                            meGO->ForceTransform(MF);
                            Msg("* [netupd] debug jump: moved controlled entity next to id=%u '%s' at (%.1f, %.1f, %.1f)", id, GO->cNameSect().c_str(), VPUSH(creature->o_Position));
                        }
                    }
                }
            }
        } while (false);

        P.r_seek(next); // exact framing regardless of how much was consumed
    }

    if (dump_pass)
        Msg("%s", dump_ids);

    // diag: throttled
    if (diag_applied || diag_skipped)
    {
        static u32 s_diag = 0;
        if (++s_diag % 63 == 1)
            Msg("* [netupd] cl apply: applied=%u moved=%u skipped=%u", diag_applied, diag_moved, diag_skipped);
    }
}

// Per-frame puppet interpolator (called from CLevel::OnFrame on the thin
// client): renders every tracked creature ~120 ms behind now, lerping between
// its two most recent network snapshots. Raw 20 Hz snapping reads as
// "teleporting"; a render-behind buffer of ~2.5 update intervals absorbs the
// delivery jitter and keeps the puppet on a continuous path.
void net_import_interpolate_tick()
{
    if (Level().Server)
        return; // fused SP / host: no networked puppets
    if (s_net_import_track.empty())
        return;

    // -net_jump_npc follow-cam: keep the observer's first-person camera locked
    // on the jumped-to target (test hook -- lets an automated screenshot prove
    // the puppet mesh really draws, with zero manual input)
    if (g_net_watch_id != 0xffff)
    {
        if (CObject* me = Level().CurrentControlEntity())
            if (CActor* meA = smart_cast<CActor*>(me))
                if (CObject* tgt = Level().Objects.net_Find(g_net_watch_id))
                {
                    Fvector dir;
                    dir.sub(tgt->Position(), meA->Position());
                    dir.y = 0.f;
                    if (dir.magnitude() > 0.1f)
                    {
                        dir.normalize();
                        meA->cam_Active()->Set(-dir.getH(), 0, 0);
                    }
                }
    }

    // Deaths the update stream reported, applied here rather than where they
    // were noticed: a normal per-frame point, with no packet half-parsed.
    if (!s_pending_deaths.empty())
    {
        for (u16 id : s_pending_deaths)
        {
            CObject* O = Level().Objects.net_Find(id);
            CEntityAlive* EA = smart_cast<CEntityAlive*>(O);
            if (!EA || O->getDestroy())
                continue;
            const bool still_alive = EA->g_Alive();
            Msg("* [netupd] puppet '%s' id=%u is dead on the server; local copy %s", EA->cNameSect().c_str(), id, still_alive ? "still alive -- killing it now" : "already dead");

            // ⚠Hand the legs back before the body becomes a ragdoll. A manual
            // blend has no curve to carry it to zero (see CBlend::eManual), so
            // one left behind holds its last weight for as long as the corpse
            // exists -- a locomotion pose quietly arguing with the physics that
            // is supposed to own the body from here on.
            auto trk_it = s_net_import_track.find(id);
            if (trk_it != s_net_import_track.end() && trk_it->second.legs_bs.built())
                if (IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(EA->Visual()))
                    trk_it->second.legs_bs.Release(ka);

            if (still_alive)
                EA->Die(nullptr);
        }
        s_pending_deaths.clear();
    }

    const u32 now = Device.dwTimeGlobal;

    // ⭐⭐⭐NO BODY WITHOUT AN ANIMATION.
    //
    // A skinned mesh with ZERO blends is not merely unanimated: nothing ever
    // runs LL_UpdateTracks on it, its bone instances stay as constructed, and
    // the skinning matrices handed to the shader are identities against a mesh
    // that expects bind-pose inverses. The vertices then leave for infinity.
    // Seen 29-07-2026 and photographed: a T-posed body under a fan of grey
    // spikes a hundred metres tall, diagnosed from `[VK Skinned] up-diag:
    // bones=50 blends=0`.
    //
    // ⚠The body in question was a PLAYER's, left behind by a reconnect: the
    // server keeps it deliberately ("waits to be re-adopted"), so it is a
    // creature nobody drives, nobody updates, and therefore nothing above this
    // loop ever touches -- the interpolator only knows entities the wire is
    // still talking about. That makes the guard belong here rather than in any
    // one spawn path: whatever the reason a body has no animation, it must not
    // be allowed to render as garbage.
    {
        static u32 s_next_idle_sweep = 0;
        if (now >= s_next_idle_sweep)
        {
            s_next_idle_sweep = now + 500; // cheap enough twice a second
            for (u32 i = 0; i < Level().Objects.o_count(); ++i)
            {
                CObject* O = Level().Objects.o_get_by_iterator(i);
                if (!O || O->getDestroy() || !O->getVisible())
                    continue;

                // ⚠⚠LOOK BEFORE FILTERING. Two earlier versions of this guard
                // put the IKinematicsAnimated cast FIRST and skipped on failure,
                // so the one case that mattered -- a skeleton carrying no motion
                // data at all -- was discarded before it could be named, and the
                // guard reported a clean world while the artefact was on screen.
                // Bone count comes from the plain IKinematics, which every
                // skeleton has.
                IKinematics* K = smart_cast<IKinematics*>(O->Visual());
                if (!K)
                    continue;
                const u16 bone_count = K->LL_BoneCount();
                if (bone_count < 20)
                    continue; // props and weapons legitimately never animate

                IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(O->Visual());
                u32 blends = 0;
                if (ka)
                    for (u32 p = 0; p < 4; ++p)
                        blends += ka->LL_PartBlendsCount(p);
                if (blends)
                    continue;

                CEntityAlive* EA = smart_cast<CEntityAlive*>(O);
                const bool is_control = (O == Level().CurrentControlEntity());
                const bool is_corpse = (EA && !EA->g_Alive());

                // ⚠⚠⚠LOG WIDELY, ACT NARROWLY -- and this is why.
                //
                // The first version of the rule below acted on every skeleton it
                // could not pose, and within seconds of shipping it was hiding
                // `meshes\brkbl#*.ogf`: the level's BREAKABLE props carry a
                // 29-33 bone skeleton for their debris and are posed by physics,
                // never by an animation. So "no blends" is perfectly normal for
                // them, and the guard was deleting level geometry -- a worse
                // defect than the one it was written to prevent.
                //
                // Only a CREATURE's body is expected to be animated, so only a
                // creature's body may be judged by this rule. Everything else is
                // reported once -- so the guard stays readable -- and left
                // exactly as it is.
                static u32 s_skip_diag = 0;
                if (!EA || is_control || is_corpse)
                {
                    // ⚠A FLAT CAP LETS THE COMMON CASE CROWD OUT THE ONE UNDER
                    // STUDY. The first cut printed the first eight findings and
                    // every one of them was a `meshes\brkbl#*` prop, so the
                    // 50-bone skeleton actually producing the artefact -- which
                    // the renderer kept reporting as bones=50 blends=0 -- was
                    // never named. Anything the size of a person, and anything
                    // that is a creature at all, is named regardless of how many
                    // props came first.
                    const bool worth_naming = (bone_count >= 40) || (nullptr != EA);
                    // ...still bounded: this loop runs twice a second.
                    if (s_skip_diag < (worth_naming ? 40u : 8u))
                    {
                        ++s_skip_diag;
                        Msg("~ [netupd] unposed skeleton: id=%u '%s' sect='%s' bones=%u animatable=%d vis=%d -- left alone (%s)", O->ID(), O->cName().c_str(),
                            O->cNameSect().c_str(), bone_count, ka ? 1 : 0, O->getVisible() ? 1 : 0,
                            !EA ? "not a creature" : (is_control ? "controlled by the player" : "corpse: the ragdoll poses it"));
                    }
                    continue;
                }

                // Try to pose it. Actor and stalker skeletons name their idle
                // differently, and a visual with no motion data cannot be posed
                // by anyone -- which is the case that produced the spikes.
                LPCSTR sect = O->cNameSect().c_str();
                bool posed = false;
                if (ka)
                {
                    posed = (nullptr != net_puppet_play_first_valid(ka, {"norm_idle_0", "stand_idle_0", "idle_0"}, sect, true));
                    net_puppet_play_first_valid(ka, {"norm_torso_5_aim_0", "norm_torso_0_idle_0"}, sect, true);
                    net_puppet_play_first_valid(ka, {"head_idle_0"}, sect, true);
                }

                // ⭐⭐⭐AND THE INVARIANT, WHATEVER THE REASON: a skinned mesh
                // that nothing can pose must not be drawn. Its bone matrices are
                // identities against a mesh expecting bind-pose inverses, so it
                // renders as a fan of spikes tens of metres long. Reported from
                // play twice, and it survived two fixes that each addressed one
                // possible CAUSE -- this addresses the symptom, which is the part
                // that must never reach the screen.
                static u32 s_idle_diag = 0;
                if (s_idle_diag < 16 || 0 == (s_idle_diag % 60))
                    Msg("! [netupd] unposed skeleton: id=%u '%s' bones=%u animatable=%d alive=%d pos=(%.1f, %.1f, %.1f) -- %s", O->ID(), O->cName().c_str(), bone_count,
                        ka ? 1 : 0, EA ? (EA->g_Alive() ? 1 : 0) : -1, VPUSH(O->Position()), posed ? "idle started" : "NOT DRAWN");
                ++s_idle_diag;

                if (!posed)
                    O->setVisible(FALSE);
            }
        }
    }

    for (auto& it : s_net_import_track)
    {
        SNetImportTrack& trk = it.second;
        if (trk.t_a == trk.t_b)
            continue; // single snapshot so far -- placed directly at apply time
        if (trk.dead)
            continue; // a corpse ragdolls on its own; writing its transform pins it upright

        // render-behind per puppet, ~2.3 of ITS observed snapshot intervals
        // (see dt_ema): ~120 ms behind a 20 Hz driven walk, ~250 ms behind a
        // 10 Hz standing guard -- a fixed 120 ms sat exactly on the slower
        // stream's interval and stuttered on every body turn. Slewed, not
        // snapped: the lag target moves when the stream changes tempo (drive
        // picks up / releases), and jumping the render time is itself a jerk.
        {
            float target = 2.3f * trk.dt_ema;
            clamp(target, 110.f, 320.f);
            float step = target - trk.behind_cur;
            const float max_step = 60.f * Device.fTimeDelta; // <=60 ms of drift per second
            clamp(step, -max_step, max_step);
            trk.behind_cur += step;
        }
        const u32 render_behind_ms = u32(trk.behind_cur);
        const u32 rt = now > render_behind_ms ? now - render_behind_ms : 0;

        CObject* O = Level().Objects.net_Find(it.first);
        if (!O || O->getDestroy() || O == Level().CurrentControlEntity())
            continue;
        CGameObject* GO = smart_cast<CGameObject*>(O);
        if (!GO)
            continue;

        // The pair the render time is actually between. With the ring on this
        // is a search; with it off it is the newest two, which is what shipped
        // and what the counters below were written to convict.
        Fvector p_a = trk.pos_a, p_b = trk.pos_b;
        float y_a = trk.yaw_a, y_b = trk.yaw_b, a_a = trk.aim_a, a_b = trk.aim_b, q_a = trk.pitch_a, q_b = trk.pitch_b;
        // ⚠The intent of the LATER snapshot of the rendered pair, not of the
        // newest one to arrive. The velocity in it is the average over the
        // interval (a -> b), which is precisely the segment being drawn this
        // frame; taking the freshest instead would animate a movement the
        // puppet is still ~120 ms away from making.
        SNetCreatureIntent in_b = trk.intent_b;
        u32 tt_a = trk.t_a, tt_b = trk.t_b;
        // ⭐The server's clips as of the EARLIER snapshot of the rendered pair:
        // that is what was playing when the render time was last sampled, and
        // its phase is carried forward from there. (The intent takes the later
        // one because it is an average over the segment; a clip is a state at
        // an instant.)
        const SNetAnimState* anim_src = &trk.anim_b;
        u32 anim_t = trk.t_b;
        if (g_cl_net_interp_ring && trk.ring_n >= 2)
        {
            const u32 have = _min(trk.ring_n, k_net_ring);
            const u32 base = trk.ring_n - have; // oldest still held
            for (u32 i = base; i + 1 < trk.ring_n; ++i)
            {
                const SNetSnap& s0 = trk.ring[i % k_net_ring];
                const SNetSnap& s1 = trk.ring[(i + 1) % k_net_ring];
                if (rt >= s0.t && rt <= s1.t && s1.t > s0.t)
                {
                    p_a = s0.pos; y_a = s0.yaw; a_a = s0.aim; q_a = s0.pitch; tt_a = s0.t;
                    p_b = s1.pos; y_b = s1.yaw; a_b = s1.aim; q_b = s1.pitch; tt_b = s1.t;
                    in_b = s1.intent;
                    anim_src = &s0.anim;
                    anim_t = s0.t;
                    break;
                }
            }
            // outside the buffered span: fall through to the clamp branches
            // below against the OLDEST/NEWEST held, not against the newest two
            const SNetSnap& oldest = trk.ring[base % k_net_ring];
            const SNetSnap& newest = trk.ring[(trk.ring_n - 1) % k_net_ring];
            if (rt < oldest.t)
            {
                p_a = p_b = oldest.pos; y_a = y_b = oldest.yaw; a_a = a_b = oldest.aim; q_a = q_b = oldest.pitch;
                tt_a = tt_b = oldest.t;
                in_b = oldest.intent;
                anim_src = &oldest.anim;
                anim_t = oldest.t;
            }
            else if (rt > newest.t)
            {
                p_a = p_b = newest.pos; y_a = y_b = newest.yaw; a_a = a_b = newest.aim; q_a = q_b = newest.pitch;
                tt_a = tt_b = newest.t;
                in_b = newest.intent;
                anim_src = &newest.anim;
                anim_t = newest.t;
            }
        }
        trk.intent = in_b;

        // ⭐⭐⭐WHO ANIMATES THIS BODY. When the server sends its animator's
        // clips, they own the whole skeleton and every local selector below --
        // the blend space, the torso table, the body-yaw repairs -- stands
        // down: those exist to GUESS what an animator would do, and here the
        // answer arrives. cl_net_anim 0 puts the guessers back for an A/B.
        const bool server_clips = (0 != g_cl_net_anim) && anim_src->valid() && (nullptr != smart_cast<CAI_Stalker*>(GO));

        // ⭐The proof, kept in the code that owns it: how many frames actually
        // INTERPOLATE, versus how many clamp to an endpoint and render a held
        // position. A healthy interpolator is almost all "inside"; the shipped
        // pair-only version measured ~0% inside and stepped once per snapshot.
        {
            static u32 s_in = 0, s_before = 0, s_after = 0, s_next = 0;
            if (rt < tt_a)
                ++s_before;
            else if (rt > tt_b)
                ++s_after;
            else
                ++s_in;
            const u32 tot = s_in + s_before + s_after;
            if (tot >= s_next)
            {
                s_next = tot + 2000;
                Msg("* [netupd] interp branches over %u frames: inside=%.0f%% before-a=%.0f%% after-b=%.0f%% (behind %.0fms, span %ums, ring %s)", tot,
                    100.f * float(s_in) / float(tot), 100.f * float(s_before) / float(tot), 100.f * float(s_after) / float(tot), trk.behind_cur, tt_b - tt_a,
                    g_cl_net_interp_ring ? "on" : "off");
            }
        }

        // ⭐⭐⭐THE TAIL OF THE LATE FRAMES, WHICH IS WHERE THE STUTTER LIVES.
        //
        // The census above says 37% of entity-frames render past the newest
        // sample. It does not say by HOW MUCH, and those are different
        // defects: 5 ms past is a rounding error, 300 ms past is a puppet
        // frozen and then snapped.
        //
        // ⚠And the arithmetic says these frames should not exist at all. The
        // wire was measured today at 19.3 packets/s carrying 1162 entity
        // updates/s across ~86 visible entities -- about 13.5 Hz each, one
        // sample every ~74 ms -- against a 320 ms render delay. A sample
        // should always be waiting. So every late frame is a GAP in the tail
        // of a distribution whose mean is healthy, and a mean is exactly what
        // cannot find it. USER-REPORTED "иногда проскакивают" is a statement
        // about a tail.
        //
        // ⚠⚠Counted per entity-frame, all of them, no sampling: the per-entity
        // rate above was first read off a log line that prints 1 in 63 and
        // came out 63x too low. A percentile from sampled data would be worse
        // than no number.
        if (rt > tt_b)
        {
            static const u32 k_edge_ms[] = {5, 10, 25, 50, 100, 200, 500, 1000};
            constexpr int k_bins = 9;
            static u32 s_bin[k_bins] = {};
            static u32 s_n = 0, s_next = 0, s_worst = 0;
            static u16 s_worst_id = 0;

            const u32 age = rt - tt_b; // how far past the newest sample we are
            int b = 0;
            while (b < 8 && age >= k_edge_ms[b])
                ++b;
            ++s_bin[b];
            ++s_n;
            if (age > s_worst)
            {
                s_worst = age;
                s_worst_id = it.first;
            }

            if (s_n >= s_next)
            {
                s_next = s_n + 20000;
                u32 acc = 0, p50 = 0, p95 = 0, p99 = 0;
                for (int i = 0; i < k_bins; ++i)
                {
                    acc += s_bin[i];
                    const u32 hi = (i < 8) ? k_edge_ms[i] : 9999;
                    if (!p50 && acc * 2 >= s_n)
                        p50 = hi;
                    if (!p95 && acc * 100 >= s_n * 95)
                        p95 = hi;
                    if (!p99 && acc * 100 >= s_n * 99)
                        p99 = hi;
                }
                Msg("* [netupd] gap tail over %u late entity-frames: <5=%u <10=%u <25=%u <50=%u <100=%u <200=%u <500=%u <1000=%u 1000+=%u | p50<%u p95<%u p99<%u ms | "
                    "worst %u ms (id=%u)",
                    s_n, s_bin[0], s_bin[1], s_bin[2], s_bin[3], s_bin[4], s_bin[5], s_bin[6], s_bin[7], s_bin[8], p50, p95, p99, s_worst, s_worst_id);
            }
        }

        Fvector pos;
        float yaw, aim, aim_pitch;
        if (rt >= tt_b)
        {
            // ran out of fresh snapshots -- hold the last one (no extrapolation:
            // overshooting a stopped NPC looks worse than a short hitch)
            pos = p_b;
            yaw = y_b;
            aim = a_b;
            aim_pitch = q_b;
        }
        else if (rt <= tt_a)
        {
            pos = p_a;
            yaw = y_a;
            aim = a_a;
            aim_pitch = q_a;
        }
        else
        {
            const float k = float(rt - tt_a) / float(tt_b - tt_a);
            pos.lerp(p_a, p_b, k);
            yaw = y_a + angle_normalize_signed(y_b - y_a) * k;
            aim = a_a + angle_normalize_signed(a_b - a_a) * k;
            aim_pitch = q_a + (q_b - q_a) * k;
        }

        // diag: name any OTHER writer that rotated the puppet since our last
        // write -- one-frame 180-degree flips have to come from somewhere.
        // rotateY(a) puts forward at (sin a, 0, cos a), so yaw = atan2(x, z).
        if (trk.yaw_written_valid)
        {
            const float found = atan2f(GO->XFORM().k.x, GO->XFORM().k.z);
            const float drift = angle_normalize_signed(found - trk.yaw_written);
            if (_abs(drift) > 0.7f)
            {
                ++g_net_foreign_yaw;
                if (g_net_foreign_yaw <= 40)
                    Msg("! [netupd] yaw id=%u FOREIGN WRITE: wrote %.2f, found %.2f (drift %.2f)", it.first, trk.yaw_written, found, drift);
                else if (0 == (g_net_foreign_yaw % 200))
                    Msg("! [netupd] yaw FOREIGN WRITE total=%u (last id=%u drift %.2f)", g_net_foreign_yaw, it.first, drift);
            }
            const float pos_drift = GO->XFORM().c.distance_to(trk.pos_written);
            if (pos_drift > 0.05f)
            {
                ++g_net_foreign_pos;
                if (g_net_foreign_pos <= 40)
                    Msg("! [netupd] pos id=%u FOREIGN WRITE: drift %.3fm (wrote %.2f,%.2f,%.2f found %.2f,%.2f,%.2f)", it.first, pos_drift, VPUSH(trk.pos_written),
                        VPUSH(GO->XFORM().c));
                else if (0 == (g_net_foreign_pos % 200))
                    Msg("! [netupd] pos FOREIGN WRITE total=%u (last id=%u drift %.3fm)", g_net_foreign_pos, it.first, pos_drift);
            }
        }

        // Rendered speed: EMA of the model's actual per-frame displacement --
        // the ground truth the animation below is matched against. Time
        // constant ~300 ms: the first cut used ~65 ms and any micro-hold
        // crashed the estimate through the band threshold and back within
        // three frames.
        if (trk.yaw_written_valid && Device.fTimeDelta > EPS)
        {
            float inst = pos.distance_to(trk.pos_written) / Device.fTimeDelta;
            clamp(inst, 0.f, 10.f);
            float alpha = Device.fTimeDelta / 0.30f;
            clamp(alpha, 0.f, 1.f);
            trk.spd_render += (inst - trk.spd_render) * alpha;
        }

        // ⭐⭐⭐ONE ANSWER TO "WHAT IS THIS CREATURE DOING", AND IT IS THE
        // SENDER'S.
        //
        // Everything below used to read spd_render: an EMA of the rendered
        // displacement, smoothed over 300 ms because anything faster flapped
        // through the band thresholds. It was the best number available while
        // the wire carried only a position, and it is strictly worse than the
        // one the wire carries now -- the server measured the same motion
        // without a lag constant, before the interpolator ever touched it.
        //
        // ⚠The estimate stays as the fallback rather than being deleted: a
        // player's own body authors no intent (its client does not run this
        // export), so the old path still animates remote players and has to
        // keep working.
        const bool use_intent = (0 != g_cl_net_intent) && trk.intent.valid();
        trk.speed_auth = use_intent ? trk.intent.speed() : trk.spd_render;
        trk.fwd_auth = use_intent ? trk.intent.fwd() : trk.spd_render;

        // Style bits, repacked into the posture byte the selectors below
        // already speak. Same layout, same meaning -- but arriving with the
        // segment being rendered instead of on a 5 Hz poll of its own, which
        // is where "the crouch flag alternated with a period of exactly the
        // poll interval" came from.
        if (use_intent)
        {
            u16 posture = 0;
            if (trk.intent.flags & SNetCreatureIntent::fCrouch)
                posture |= npc_posture_crouch;
            if (trk.intent.flags & SNetCreatureIntent::fWounded)
                posture |= npc_posture_wounded;
            posture |= u16(u16(trk.intent.mental()) << npc_posture_mental_shift);
            posture |= u16(u16(trk.intent.action() & npc_posture_action_mask) << npc_posture_action_shift);
            // ⚠⚠KEEP the bits this channel does not carry. The intent knows
            // crouch, wounded, mental and action; whether the weapon is on the
            // creature's back arrives on a channel of its own
            // (GE_NPC_ACTIVE_ITEM, 5 Hz). Rebuilding the whole word from the
            // intent therefore ERASED that bit at the segment rate -- measured:
            // the client logged "weapon ON THE BACK" from the packet and the
            // animator went on picking the rifle cycle, because the two were
            // reading different copies of the same word.
            posture |= u16(trk.posture_raw & npc_posture_strapped);
            trk.posture_raw = posture;
            // A shot is a fact now, not something inferred from an event that
            // may or may not have survived the trip. The latch below is the
            // same one GE_NPC_SHOT drives -- the server holds the flag across
            // the gaps inside a burst, so this simply keeps it topped up.
            if (trk.intent.flags & SNetCreatureIntent::fFire)
                trk.shot_ms = Device.dwTimeGlobal;
        }

        // ⭐THE ONE LINE THAT CONVICTS OR CLEARS THIS WHOLE CHANNEL: what the
        // sender said, next to what the receiver would have guessed, for the
        // same creature at the same instant. If `wire` and `est` agree, the
        // intent is honest and the estimator was merely late; if `valid=0`,
        // nothing on this puppet is using the wire at all and every conclusion
        // drawn from its animation is about the fallback path.
        //
        // ⚠Printed per id and throttled by count, never capped -- an exhausted
        // counter and a healthy channel look identical in a log.
        {
            static u32 s_diag_intent = 0;
            if (++s_diag_intent % 301 == 1)
                Msg("* [netupd] intent id=%u valid=%d wire=%.2f (fwd %.2f str %.2f turn %.2f) est=%.2f flags=0x%02x slot=%u act=%u", it.first, trk.intent.valid() ? 1 : 0,
                    trk.intent.speed(), trk.intent.fwd(), trk.intent.strafe(), trk.intent.turn_rate(), trk.spd_render, trk.intent.flags, trk.intent.weapon_slot(),
                    trk.intent.action());
        }

        // ⚠Does this creature OWN a sideways cycle right now?
        // CStalkerAnimationManager::legs_move_animation returns the FORWARD
        // animation unconditionally outside the danger state -- strafe and
        // backpedal exist in the combat set only, and a calm stalker in single
        // player turns its body instead of sliding. Both rules below need this:
        // keeping a facing that disagrees with the course buys a strafe pose
        // that was never going to play, and the forward cycle then runs across a
        // sideways slide. Reported from play: "идут в бок, поддёргиваясь, а
        // анимация вперёд", seen while walking backwards and sideways myself.
        const u8 mental_now = u8((trk.posture >> npc_posture_mental_shift) & npc_posture_mental_mask);
        const bool has_strafe_cycles = (npc_mental_free != mental_now);

        // Face where you are actually going: the wire yaw can lag or contest
        // the path (fetch-drive vs AI facing), and rendering it verbatim had
        // the body walking sideways along its own track ("ходят боком").
        //
        // ⚠Not with the server's clips: then the wire yaw IS the body the
        // server's animator chose those clips for (a strafe clip under a body
        // facing the target, a turn-in-place under a body that is turning), and
        // "correcting" it would put the legs at an angle to their own motion.
        if (!server_clips && use_intent)
        {
            // ⭐The sideways component is now a MEASUREMENT, arriving in the
            // body's own frame -- there is nothing left to infer from the
            // shape of the path. Turn the body into the course only when this
            // set owns no cycle that could show the drift, and only while
            // genuinely moving forwards: a backpedal has its own cycles (see
            // the blend space), and rotating the body to face a reversing
            // course would spin the puppet around.
            if (!has_strafe_cycles && trk.fwd_auth > 0.2f)
            {
                float drift = atan2f(trk.intent.strafe(), trk.intent.fwd());
                clamp(drift, -PI / 3.f, PI / 3.f);
                yaw = angle_normalize_signed(yaw + drift);
            }
        }
        else if (!server_clips)
        {
            // No intent from this sender: infer the course from the shape of
            // the rendered path, as this layer did before the wire carried it.
            Fvector seg;
            seg.sub(p_b, p_a); // the pair being rendered, not the newest pair
            seg.y = 0.f;
            if (trk.spd_render > 0.4f && seg.magnitude() > 0.03f)
            {
                const float heading = atan2f(seg.x, seg.z);
                if (!has_strafe_cycles || _abs(angle_normalize_signed(heading - yaw)) < PI / 3.f)
                    yaw = heading;
            }
        }

        // Body catch-up. A spine can twist; it cannot spin. When the creature
        // looks further off its model heading than a body allows -- a guard
        // whose route course points one way while the AI tracks a target
        // behind him -- turning only the bones would fold the puppet in half.
        // The body follows, exactly as the single-player movement manager
        // makes m_body chase m_head past its limit; the rate limiter below
        // then makes it a pivot instead of a snap.
        //
        // ⚠With the server's clips the server's own movement manager already
        // did exactly this, and the yaw on the wire is its result.
        if (!server_clips)
        {
            // Walking: the legs own the heading and the spine takes the rest.
            // Standing: turn and face it.
            //
            // ⚠RAMPED, not switched. A hard threshold here made the allowance
            // jump 33 degrees the instant the speed estimate crossed it, and a
            // creature idling near that speed had its body yanked between two
            // targets every frame -- reported from play as micro-vibration
            // while walking. The body's own turn limiter cannot smooth that:
            // the target itself was oscillating.
            float t = (trk.speed_auth - 0.15f) / 0.75f;
            clamp(t, 0.f, 1.f);
            const float allow = k_puppet_stand_twist + (k_puppet_max_twist - k_puppet_stand_twist) * t;
            const float over = angle_normalize_signed(aim - yaw);
            if (_abs(over) > allow)
            {
                // ⭐WHICH ONE GIVES WAY depends on who owns a cycle for it.
                // With the danger set the body can strafe, so pulling the BODY
                // round to the look direction still animates honestly. In the
                // free set the only leg cycle is forward (see the note on
                // has_strafe_cycles above), so pulling the body off its course
                // reinstates exactly the sideways slide this block sits beside
                // -- there the LOOK gives way and the spine simply stops at its
                // limit, which is what a calm man walking past you does.
                // Standing is unchanged either way: then the body turns to face.
                if (has_strafe_cycles || trk.speed_auth <= 0.4f)
                    yaw = angle_normalize_signed(aim - (over > 0.f ? allow : -allow));
                else
                    aim = angle_normalize_signed(yaw + (over > 0.f ? allow : -allow));
            }
        }

        // Turn-rate limit. The wire yaw is allowed to step: measured streams
        // flip a whole pi between consecutive snapshots when the server's
        // fetch-back drive (heading to the post) alternates with the freed
        // AI's own facing. Whatever arrives, the RENDERED body turns no
        // faster than a person can -- ~290 deg/s covers every honest turn
        // and converts source flips into a quick continuous pivot.
        if (trk.yaw_written_valid)
        {
            const float d = angle_normalize_signed(yaw - trk.yaw_written);
            // With the server's clips the heading has to keep up with a
            // turn-in-place clip that the server timed to its own body speed
            // (up to a full turn a second); the limiter then only catches the
            // source flips it was written for.
            const float max_turn = (server_clips ? PI_MUL_2 : 5.0f) * Device.fTimeDelta;
            float step = d;
            clamp(step, -max_turn, max_turn);
            yaw = angle_normalize_signed(trk.yaw_written + step);
        }

        // Locomotion banding + stride retune, per frame from the rendered
        // speed: a stop reads as idle within ~a tenth of a second, a hold in
        // the interpolator stops the feet with it -- "walking in place" and
        // second-long band lag both die here.
        {
            auto mit = s_net_import_mirrors.find(it.first);
            if (mit != s_net_import_mirrors.end() && mit->second)
            {
                net_puppet_hand_over(GO, trk, server_clips);
                if (server_clips)
                    net_puppet_server_clips(GO, trk, *anim_src, anim_t, rt);
                else
                    net_puppet_locomotion(GO, mit->second, trk);
            }
        }
        // diag: heartbeat of the yaw stream (odd modulus -- even ones lock
        // to one puppet when two alternate)
        {
            static u32 s_diag_yaw = 0;
            if (++s_diag_yaw % 301 == 1)
                Msg("* [netupd] yaw id=%u a=%.2f b=%.2f span=%ums write=%.2f", it.first, trk.yaw_a, trk.yaw_b, trk.t_b - trk.t_a, yaw);
        }

        // net_ServerDriveTo, NOT net_MoveTo, and the difference is the whole
        // bug it fixes. On a network client Local() is true for everything
        // (cl_Process_Spawn forces the flag), so a living creature runs its
        // own UpdateCL every frame, which ends with
        //     XFORM().translate_over(NET_Last.p_pos);
        // -- a bare transform write (net_MoveTo) is a suggestion the puppet's
        // own update overrules from its local capsule at its own cadence, and
        // on screen that read as ~3 fps stutter with the walk cycle sliding
        // over the ground (28-07 live session). The drive writes NET_Last and
        // claims ownership (server_driven(), 1 s TTL), so move_along_path
        // leaves the position alone and reports the measured speed to the
        // animation selector -- the same fix already proven on the host in
        // sv_npc. (Not ForceTransform either: its collision-correct settle
        // loop stepped the whole ODE world thousands of times a second.)
        GO->net_ServerDriveTo(pos, yaw);

        // Aim. NOT a new bone-rotating layer: the engine already has one, and
        // it is already installed on this puppet -- CStalkerAnimationManager::
        // assign_bone_callbacks runs from net_Spawn BEFORE the thin-client
        // early-out, so spine/shoulder/head carry their custom callbacks here
        // exactly as in single player. What they were missing is inputs: with
        // no AI on this machine the three values they read (head yaw, body yaw,
        // NET_Last torso pitch) were frozen at their spawn values, so the
        // callbacks faithfully applied a zero rotation every frame. Hand them
        // the wire's aim and the shipped, tuned aiming math does the rest --
        // the same reason the locomotion layer plays the content's own cycles
        // instead of posing bones by hand.
        //   The frames line up by construction: the AI writes
        //   o_model = m_body.current.yaw and o_torso = m_head.current in the
        //   same push (CAI_Stalker::shedule_Update), so the wire's difference
        //   IS the twist the callbacks want.
        if (CAI_Stalker* S = smart_cast<CAI_Stalker*>(GO))
        {
            float twist = angle_normalize_signed(aim - yaw);
            // the body catch-up above already keeps this inside the limit in
            // the steady state; the turn-rate limiter can leave it outside for
            // a few frames during a fast target switch, and a broken spine for
            // a few frames is still a broken spine
            clamp(twist, -k_puppet_max_twist, k_puppet_max_twist);
            clamp(aim_pitch, -k_puppet_max_pitch, k_puppet_max_pitch);

            S->movement().m_head.current.yaw = angle_normalize_signed(yaw + twist);
            S->movement().m_head.target.yaw = S->movement().m_head.current.yaw;
            S->movement().m_body.current.yaw = yaw;
            S->movement().m_body.target.yaw = yaw;
            // the pitch input is read straight off NET_Last by the callbacks;
            // on a render-only puppet nothing else writes it (the whole
            // UpdateCL stack is gated off -- see net_render_only_puppet)
            S->NET_Last.o_torso.pitch = aim_pitch;

            // ⭐...and then close the loop on where the barrel actually ended
            // up. Written AFTER the three inputs above, because that is what it
            // measures the error against. A render-only puppet never reaches
            // CAI_Stalker::UpdateCL, so this is the only place it runs on a
            // thin client -- same function, same servo as the host.
            S->update_aim_servo();

            // diag: the aim actually applied, against what the wire asked for.
            // A twist that is always 0 means the wire carries no aim (or the
            // server is driving the heading straight at the target already);
            // a twist pinned at the limit means the body catch-up is losing.
            static u32 s_diag_aim = 0;
            if (++s_diag_aim % 301 == 1)
                Msg("* [netupd] aim id=%u body=%.2f look=%.2f twist=%.1fdeg pitch=%.1fdeg", it.first, yaw, aim, rad2deg(twist), rad2deg(aim_pitch));
        }

        trk.yaw_written = yaw;
        trk.pos_written = pos;
        trk.yaw_written_valid = true;
        // keep the spatial sphere riding along (ISpatial::spatial_move no-ops
        // when still inside the same node, so this is cheap per frame)
        GO->spatial_move();
    }
}

void CLevel::ClientReceive()
{

    m_dwRPC = 0;
    m_dwRPS = 0;

    for (NET_Packet* P = net_msg_Retreive(); P; P = net_msg_Retreive())
    {
        //-----------------------------------------------------
        m_dwRPC++;
        m_dwRPS += P->B.count;
        //-----------------------------------------------------
        u16 m_type;
        P->r_begin(m_type);
        switch (m_type)
        {
        case M_SPAWN: {
            if (!m_bGameConfigStarted || !bReady)
            {
                Msg("Unconventional M_SPAWN received : cgf[%s] | bReady[%s]", (m_bGameConfigStarted) ? "true" : "false", (bReady) ? "true" : "false");
                break;
            }
            /*/
            cl_Process_Spawn(*P);
            /*/
            game_events->insert(*P);
            if (g_bDebugEvents)
                ProcessGameEvents();
            //*/
        }
        break;
        case M_EVENT:
            game_events->insert(*P);
            if (g_bDebugEvents)
                ProcessGameEvents();
            break;
        case M_EVENT_PACK: {
            NET_Packet tmpP;
            while (!P->r_eof())
            {
                tmpP.B.count = P->r_u8();
                P->r(&tmpP.B.data, tmpP.B.count);
                tmpP.timeReceive = P->timeReceive;

                game_events->insert(tmpP);
                if (g_bDebugEvents)
                    ProcessGameEvents();
            }
        }
        break;
        case M_UPDATE: {
            game->net_import_update(*P);
            cl_Process_EntityUpdates(*P); // per-entity tail for remote clients (no-op when absent)
        }
        break;
        case M_ADMIN_REPLY: {
            // What the server made of an `rcon` command. Printed as-is: it is
            // already the server's own console output (or its refusal), and the
            // admin asked to see it.
            string1024 text{};
            P->r_stringZ(text);
            Msg("[rcon] %s", text);
        }
        break;
        case M_SV_CONFIG_NEW_CLIENT: InitializeClientGame(*P); break;
        case M_SV_CONFIG_GAME: game->net_import_state(*P); break;
        case M_SV_CONFIG_FINISHED:
            game_configured = TRUE;
            Msg("- Game configuring : Finished ");
            break;
        case M_RELOAD_GAME:
        case M_LOAD_GAME:
        case M_CHANGE_LEVEL: {
            // diagnostic: this message tears the level down (KERNEL:disconnect below)
            // -- always log which type asked for it and who we think sent it
            Msg("- net: received %s -> level restart", (m_type == M_RELOAD_GAME) ? "M_RELOAD_GAME" : ((m_type == M_LOAD_GAME) ? "M_LOAD_GAME" : "M_CHANGE_LEVEL"));

            // костыль для автозакрытия любого диалога в случае смены уровня
            if (HUD().GetUI()->MainInputReceiver())
            {
                HUD().GetUI()->StartStopMenu(HUD().GetUI()->MainInputReceiver(), true);
            }

            if (m_type == M_LOAD_GAME)
            {
                string256 saved_name;
                P->r_stringZ(saved_name);
                if (xr_strlen(saved_name) && ai().get_alife())
                {
                    CSavedGameWrapper wrapper(saved_name);
                    if (wrapper.level_id() == ai().level_graph().level_id())
                    {
                        Engine.Event.Defer("Game:QuickLoad", size_t(xr_strdup(saved_name)), 0);

                        break;
                    }
                }
            }

            Engine.Event.Defer("KERNEL:disconnect");
            Engine.Event.Defer("KERNEL:start", size_t(xr_strdup(*m_caServerOptions)), size_t(xr_strdup(*m_caClientOptions)));
        }
        break;
        case M_SAVE_GAME: {
            //ClientSave();
        }
        break;
        case M_AUTH_CHALLENGE: {
            OnBuildVersionChallenge();
        }
        break;
        case M_CLIENT_CONNECT_RESULT: {
            OnConnectResult(P);
        }
        break;
        case M_CHANGE_LEVEL_GAME: {
            Msg("- M_CHANGE_LEVEL_GAME Received");
            {
                const char* m_SO = m_caServerOptions.c_str();
                //					const char* m_CO = m_caClientOptions.c_str();

                m_SO = strchr(m_SO, '/');
                if (m_SO)
                    m_SO++;
                m_SO = strchr(m_SO, '/');

                string128 LevelName = "";
                string128 GameType = "";

                P->r_stringZ(LevelName);
                P->r_stringZ(GameType);

                string4096 NewServerOptions{};
                sprintf_s(NewServerOptions, "%s/%s", LevelName, GameType);

                if (m_SO)
                    strcat_s(NewServerOptions, m_SO);
                m_caServerOptions = NewServerOptions;

                Engine.Event.Defer("KERNEL:disconnect");
                Engine.Event.Defer("KERNEL:start", size_t(xr_strdup(*m_caServerOptions)), size_t(xr_strdup(*m_caClientOptions)));
            };
        }
        break;
        case M_CHANGE_SELF_NAME: {
            net_OnChangeSelfName(P);
        }
        break;
        default: break;
        }

        net_msg_Release();
    }

    //	if (!g_bDebugEvents) ProcessGameSpawns();
}

void CLevel::OnMessage(void* data, u32 size)
{
    IPureClient::OnMessage(data, size);

};

namespace
{
// installs the receive side of the network-client path -- the external
// client backend calls this hook instead of including xrGame/Level.h
// directly (see NET_GNS_Transport.h's DeliverToClientFn comment for why).
// No-op everywhere Server is non-null (the local/in-memory singleplayer
// path never touches this).
void DeliverToLevel(void* data, u32 size) { Level().OnMessage(data, size); }

struct GnsClientDeliveryInstaller
{
    GnsClientDeliveryInstaller() { NET_GNS::g_DeliverToClient = &DeliverToLevel; }
} g_gnsClientDeliveryInstaller;
} // namespace

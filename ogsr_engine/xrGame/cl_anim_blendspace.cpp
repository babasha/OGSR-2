#include "StdAfx.h"
#include "cl_anim_blendspace.h"
#include "cl_anim_ledger.h"

#include "Level.h"
#include "GameObject.h"
#include "stalker_velocity_holder.h" // the engine's own [mental][body][type][dir] speed table
#include "stalker_velocity_collection.h"
#include "sv_npc.h" // npc_mental_* -- the same numbering the wire carries
#include "../Include/xrRender/RenderVisual.h"
#include "../Include/xrRender/animation_blend.h"
#include "../xr_3da/SkeletonMotions.h"

namespace ClAnim
{
// ⚠THE NUMBERS OF LAST RESORT.
//
// Reached only when a creature's section names no velocity table AND its clips
// animate in place -- i.e. when nothing in the content will say how fast this
// gait is supposed to travel. They match this mod's stalker table so the
// failure is graceful rather than obviously wrong, but a build log line saying
// "hardcoded" is a bug report: it means the lookup below missed.
static const float k_hardcoded_walk = 1.10f;
static const float k_hardcoded_run = 3.00f;

// ⭐⭐WHERE THE SPEEDS REALLY COME FROM, once the clips turn out not to know.
//
// Measuring the root was the first choice and it failed on this content: every
// locomotion cycle animates IN PLACE (measured, 0 of 11 -- see
// cl_anim_rootmotion), because in X-Ray the AI carries the body and the clip
// only moves the feet. So the pairing "this cycle means this speed" is not in
// the animation at all.
//
// It is in the CONFIG, though, and not as folklore: CStalkerVelocityCollection
// builds the AI's own movement speeds out of <section>/movement_speeds, and
// those are exactly the speeds at which single player moves a body while
// playing these cycles. Reading the same table is therefore not "guessing from
// an ltx" -- it is asking the one authority that defines the pairing.
//
// ⚠And it is per SECTION. The previous version of this code carried two
// constants copied out of m_stalker.ltx by hand, which were wrong by 20% on
// the first attempt and would have been silently wrong again for any creature
// whose table differs -- a mod, a monster, a second stalker archetype.
// ⭐⭐TWO CLIPS, EIGHT SPEEDS -- and the pairing is the engine's to make.
//
// The stalker set has exactly two locomotion cycles (`movement_names[] =
// {"walk_", "run_"}`), but the velocity table underneath has a whole ladder:
// free walk/run, danger stand walk/run forward and backward, danger crouch the
// same, and panic run. What people call the stalker's SPRINT is not a third
// clip -- it is `run_fwd_0` played at `panic_stand_run_forward`.
//
// ⚠So the pairing "this cycle means this speed" needs the MENTAL state, and
// the first version of this file ignored it: it always read the free row, so a
// creature running in danger or panic got the clip of a run matched to the
// speed of a stroll, and its legs fell behind the ground by exactly the
// difference between the two tariffs. The mental state has been on the wire
// since the intent block shipped; this just started asking for it.
//
// ⚠⚠And the lookup is the engine's own accessor rather than string keys,
// because the table is not a flat namespace: CStalkerVelocityCollection is
// indexed [mental][body][type][direction] and only some combinations exist.
// Free is stand-and-forward only, panic is run-forward only. Asking for a
// combination the row does not have returns a neighbouring cell in a release
// build, where the VERIFY that would have caught it is compiled out -- so the
// routing below is not politeness, it is the guard.
bool GaitSpeeds(LPCSTR sect, u8 state_class, u8 mental, bool back, float& walk, float& run)
{
    if (!sect || !pSettings->section_exist(sect) || !pSettings->line_exist(sect, "movement_speeds"))
        return false;
    LPCSTR vel_section = pSettings->r_string(sect, "movement_speeds");
    if (!vel_section || !pSettings->section_exist(vel_section))
        return false;

    const CStalkerVelocityCollection& v = stalker_velocity_holder().collection(vel_section);

    using namespace MonsterSpace;
    const EMovementDirection dir = back ? eMovementDirectionBackward : eMovementDirectionForward;
    // The cycle prefix already encodes the body: cr_* is crouched, and crouch
    // is only legal outside the free state (the engine's own setters assert
    // it). dmg_norm_* is wounded, which has no row of its own -- danger
    // standing is the nearest honest one.
    const EBodyState body = (1 == state_class) ? eBodyStateCrouch : eBodyStateStand;

    // Free has forward-standing entries only. Anything else -- crouched,
    // backing up, or simply not calm -- belongs to the danger row.
    const bool free_row = (npc_mental_free == mental) && (0 == state_class) && !back;

    if (free_row)
    {
        walk = v.velocity(eMentalStateFree, eBodyStateStand, eMovementTypeWalk, eMovementDirectionForward);
        run = v.velocity(eMentalStateFree, eBodyStateStand, eMovementTypeRun, eMovementDirectionForward);
    }
    else
    {
        walk = v.velocity(eMentalStateDanger, body, eMovementTypeWalk, dir);
        run = v.velocity(eMentalStateDanger, body, eMovementTypeRun, dir);

        // ⭐THE SPRINT. Panic has one entry and it is a forward standing run --
        // there is no panic walk and no panic crouch, so only the run node
        // takes the panic tariff and the walk node stays on danger.
        if (npc_mental_panic == mental && eBodyStateStand == body && !back)
            run = v.velocity(eMentalStatePanic, eBodyStateStand, eMovementTypeRun, eMovementDirectionForward);
    }

    return (walk > EPS) || (run > EPS);
}

// Below this the creature is standing as far as the legs are concerned; above
// the second, it is walking. In between the two cross-fade. The band is narrow
// on purpose: there is no clip for "walking at 0.2 m/s", and time-warping the
// walk that far down reads as slow motion rather than as a slow walk.
static const float k_idle_lo = 0.15f;
static const float k_idle_hi = 0.55f;
// How far a clip may be time-warped from what it was authored for before the
// result stops looking like the same gait.
static const float k_rate_min = 0.55f;
static const float k_rate_max = 1.70f;

// ------------------------------------------------------------------------
// Root motion
// ------------------------------------------------------------------------

// Decode one quantized translation key of the root bone. Mirrors the render
// path's QT8_2T/QT16_2T -- kept local rather than shared because those live
// inside the renderer's key-blending header and pulling it in here would drag
// the whole animation-mixing machinery into a measurement.
static bool root_key(CMotion& M, u32 frame, Fvector& T)
{
    if (!M.test_flag(flTKeyPresent))
    {
        T.set(M._initT); // no translation channel at all: the clip is in place
        return false;
    }
    if (M.test_flag(flTKey16IsBit))
    {
        const CKeyQT16& K = M._keysT16[frame];
        T.set(float(K.x1) * M._sizeT.x + M._initT.x, float(K.y1) * M._sizeT.y + M._initT.y, float(K.z1) * M._sizeT.z + M._initT.z);
    }
    else
    {
        const CKeyQT8& K = M._keysT8[frame];
        T.set(float(K.x1) * M._sizeT.x + M._initT.x, float(K.y1) * M._sizeT.y + M._initT.y, float(K.z1) * M._sizeT.z + M._initT.z);
    }
    return true;
}

bool CycleMetrics(IKinematicsAnimated* ka, MotionID m, SCycleMetrics& out)
{
    out = SCycleMetrics();
    if (!ka || !m.valid())
        return false;

    CMotion* M = ka->LL_GetRootMotion(m);
    CMotionDef* def = ka->LL_GetMotionDef(m);
    if (!M || !def)
        return false;

    const float authored = def->Speed() > EPS ? def->Speed() : 1.f;
    out.keys = M->get_count();
    out.time_s = M->GetLength() / authored;
    if (out.time_s <= EPS)
        return false;

    if (out.keys < 2)
    {
        out.measured = true;
        out.in_place = true;
        return true;
    }

    Fvector t0, t1;
    if (!root_key(*M, 0, t0) || !root_key(*M, out.keys - 1, t1))
    {
        // No translation channel on the root. That is a fact about the content,
        // not a failure to read it -- say so, so the caller can distinguish it
        // from a broken measurement and reach for the assumed speed knowingly.
        out.measured = true;
        out.in_place = true;
        return true;
    }

    Fvector d;
    d.sub(t1, t0);
    d.y = 0.f; // vertical bob is not travel
    // The keys span count-1 intervals, but the cycle is count intervals long:
    // the last key wraps back to the first. Scale up so the number is metres
    // per CYCLE and not metres per key span.
    out.dist_m = d.magnitude() * float(out.keys) / float(out.keys - 1);
    out.measured = true;
    // A centimetre and a half over a whole stride is bob, not travel.
    out.in_place = (out.dist_m < 0.05f);
    if (out.in_place)
        out.dist_m = 0.f;
    return true;
}

// ------------------------------------------------------------------------
// Cycle names
// ------------------------------------------------------------------------

static LPCSTR state_prefix(u8 state_class)
{
    switch (state_class)
    {
    case 1: return "cr_";
    case 2: return "dmg_norm_";
    default: return "norm_";
    }
}

// ⭐⭐⭐THE CONTENT SHIPS TWO OF EVERY LOCOMOTION CYCLE AND WE ONLY EVER ASKED
// FOR ONE.
//
// `norm_walk_fwd_0` and `norm_walk_fwd_1` are not two takes of the same walk:
// _0 is the DANGER gait -- weapon up, shoulders low, short steps -- and _1 is
// the relaxed one. Single player chooses between them by MENTAL STATE, and it
// never says so in a name; it indexes:
//
//   moving legs : m_movement[type][dir].A[ mental != danger ]  (legs_move_animation)
//   idle legs   : m_in_place           [ mental == free ]      (legs_no_move_animation)
//   torso       : [9].A[1] / [7+type].A[1] when free+standing, [6].A[..] = aim_ otherwise
//
// The puppet layer asked for _0 unconditionally -- every creature, every state,
// both animators -- so every NPC in multiplayer was animated as a man who
// expects to be shot at. USER-REPORTED as "остаются в позе крадущихся" once
// they started slinging their rifles and the alert stance stopped being
// plausible for a man with nothing in his hands.
//
// ⚠A[] is COMPACTED (CAniVector::Load skips names the content does not have),
// so .A[1] means "the second cycle that EXISTS", not "the one ending in 1".
// They coincide on this content -- both variants resolve, measured -- and where
// they would not, the fallback chain below lands on the _0 we play today.
static void gait_variants(u8 state_class, u8 mental, bool actor, int& idle_var, int& move_var)
{
    // ⚠A remote player's body keeps the alert set. Its torso is posed with the
    // actor's own aim cycle, and relaxed legs under a rifle at the shoulder is
    // a pairing nobody authored. The crouch and wounded sets stay on _0 too:
    // neither is a calm way to carry yourself, and neither is guaranteed to
    // have a second variant at all.
    const bool calm = !actor && (0 == state_class);
    idle_var = (calm && npc_mental_free == mental) ? 1 : 0;
    // ⚠Not symmetric, and deliberately so: the engine's idle rule tests
    // `== free` while its movement rule tests `!= danger`, which puts a
    // panicking man on the relaxed run and the alert idle. Mirrored rather than
    // tidied -- matching single player is the whole point.
    move_var = (calm && npc_mental_danger != mental) ? 1 : 0;
}

// ⚠The fallback chain is where a request quietly becomes a DIFFERENT request:
// ask for "panic_walk_fwd_0", get "norm_walk_fwd_0", animate a panicking man as
// a strolling one, and nothing anywhere says so. `id` is carried in only so the
// ledger can report that -- see cl_anim_ledger.h.
static MotionID resolve(IKinematicsAnimated* ka, u16 id, std::initializer_list<LPCSTR> names)
{
    bool first = true;
    MotionID found;
    for (LPCSTR n : names)
    {
        MotionID m = ka->ID_Cycle_Safe(n);
        ClAnimLedger::Wanted(id, n, m.valid(), first);
        first = false;
        if (m.valid() && !found.valid())
        {
            found = m;
            // Untraced: stop at the first hit, exactly as before. Only a
            // creature under the ledger pays for walking the whole chain.
            if (!ClAnimLedger::watching(id))
                return found;
        }
    }
    return found;
}

// ------------------------------------------------------------------------
// The blend space
// ------------------------------------------------------------------------

bool CLegsBlendSpace::NodesAlive() const
{
    for (const SNode& n : m_node)
    {
        if (!n.B)
            return false;
        // ⚠Blend slots are pooled and reused. Ours is only still ours while it
        // holds the motion we started on it AND is still in the state we put it
        // in -- anything else played on this partition fades our blends, and a
        // faded blend frees its slot to somebody else's cycle.
        if (n.B->blend_state() != CBlend::eManual || n.B->motionID != n.motion)
            return false;
    }
    return true;
}

bool CLegsBlendSpace::Build(IKinematicsAnimated* ka, LPCSTR diag_sect, u16 diag_id, u8 state_class, u8 mental, bool sprint, bool actor, bool back)
{
    m_built = false;

    // ⛔THE SPRINT BIT IS DELIBERATELY NOT USED HERE. It was, for one build,
    // and it was wrong twice over.
    //
    // `native` is what the CLIP travels at -- it is the denominator of the time
    // warp. The panic tariff is not that: it is a speed the server carries the
    // body at, using the same run clip, and in this mod it is 6.00 m/s against
    // a run clip the content otherwise pairs with 3.00-3.60. Writing 6.00 into
    // native tells the blend space "this clip covers six metres a second", so
    // it plays at rate 1.00 and the feet turn over at half the rate the ground
    // demands. That is maximum skating, produced by the very field meant to
    // prevent it.
    //
    // Nothing is lost by ignoring it: the measured velocity already arrives on
    // the wire, so a body being carried at 6.00 against a native of 3.60 warps
    // to 1.67 and the legs go as fast as the clamp allows. The bit stays on the
    // wire as a statement of intent (it is printed by the intent diagnostic),
    // but the pairing is a property of the clip and comes from the clip's row.
    (void)sprint;

    // A remote player's body is a stalker skeleton but carries the actor's own
    // sets: it has no cr_/dmg_norm_ locomotion to fall back to.
    LPCSTR pfx = actor ? "norm_" : state_prefix(state_class);

    // Which of the two takes of each gait this creature's state calls for.
    int idle_var = 0, move_var = 0;
    gait_variants(state_class, mental, actor, idle_var, move_var);

    LPCSTR dir = back ? "back" : "fwd";
    string64 s_idle, s_walk, s_run, s_idle0, s_walk0, s_run0, s_walk_p, s_run_p;
    xr_sprintf(s_idle, "%sidle_%d", pfx, idle_var);
    xr_sprintf(s_walk, "%swalk_%s_%d", pfx, dir, move_var);
    xr_sprintf(s_run, "%srun_%s_%d", pfx, dir, move_var);
    // ...and the same request with the variant given up, one step before the
    // prefix and the direction are given up in turn.
    xr_sprintf(s_idle0, "%sidle_0", pfx);
    xr_sprintf(s_walk0, "%swalk_%s_0", pfx, dir);
    xr_sprintf(s_run0, "%srun_%s_0", pfx, dir);
    xr_sprintf(s_walk_p, "%swalk_fwd_0", pfx);
    xr_sprintf(s_run_p, "%srun_fwd_0", pfx);

    MotionID m_i = resolve(ka, diag_id, {s_idle, s_idle0, "norm_idle_0", "stand_idle_0", "idle_0"});
    MotionID m_a = resolve(ka, diag_id, {s_walk, s_walk0, s_walk_p, "norm_walk_fwd_0", "walk_fwd_0"});
    MotionID m_b = resolve(ka, diag_id, {s_run, s_run0, s_run_p, "norm_run_fwd_0", "run_fwd_0"});

    // Idle is the one that cannot be missing: it is what a standing puppet is
    // made of, and a skeleton with no blends at all renders as a fan of spikes
    // (see the unposed-skeleton guard in the interpolator).
    if (!m_i.valid())
        return false;
    if (!m_a.valid())
        m_a = m_i;
    if (!m_b.valid())
        m_b = m_a;

    const MotionID want[n_nodes] = {m_i, m_a, m_b};

    // ⭐Played in order and only then taken over. Each PlayCycle fades whatever
    // else is on this partition -- including the node played just before it --
    // so the takeover has to happen after the last one, not between them. What
    // the previous set is left in is eFalloff, which is exactly right: the old
    // gait fades out under the new one instead of being cut.
    for (int i = 0; i < n_nodes; ++i)
    {
        m_node[i] = SNode();
        m_node[i].motion = want[i];
        m_node[i].B = ka->PlayCycle(want[i], TRUE);
        // Stage 2 of the ledger: accepted, or refused and nobody would know.
        if (ClAnimLedger::watching(diag_id))
        {
            const std::pair<LPCSTR, LPCSTR> nm = ka->LL_MotionDefName_dbg(want[i]);
            ClAnimLedger::Played(diag_id, nm.first ? nm.first : "?", m_node[i].B ? m_node[i].B->bone_or_part : u16(0), m_node[i].B);
        }
    }

    // ⚠All or nothing. Taking a blend over one at a time and bailing halfway
    // would strand the ones already switched to eManual: nothing fades them,
    // and m_built stays false so Release would not close them either -- blends
    // pinned at a weight with no owner, for the life of the visual.
    for (const SNode& n : m_node)
        if (!n.B)
        {
            if (m_node[0].B)
                ka->LL_CloseCycle(m_node[0].B->bone_or_part, 1 << 0);
            for (SNode& reset : m_node)
                reset = SNode();
            return false;
        }

    for (int i = 0; i < n_nodes; ++i)
    {
        SNode& n = m_node[i];
        n.authored_speed = n.B->speed > EPS ? n.B->speed : 1.f;
        n.B->set_manual_state();
        n.B->blendAmount = 0.f;
        CycleMetrics(ka, n.motion, n.metrics);
    }

    // How fast each gait is meant to travel: the clip's own root motion first,
    // the creature's velocity table second, a hardcoded pair only if neither
    // exists. Which one won is printed below, because a slide that comes from
    // the wrong pairing and a slide that comes from a bad time warp look
    // identical on screen and are fixed in different places.
    float cfg_walk = 0.f, cfg_run = 0.f;
    const bool from_cfg = GaitSpeeds(diag_sect, state_class, mental, back, cfg_walk, cfg_run);
    const float fallback[n_nodes] = {0.f, from_cfg && cfg_walk > EPS ? cfg_walk : k_hardcoded_walk, from_cfg && cfg_run > EPS ? cfg_run : k_hardcoded_run};
    LPCSTR source[n_nodes] = {"", "", ""};
    for (int i = n_move_a; i <= n_move_b; ++i)
    {
        const float measured = m_node[i].metrics.native_speed();
        if (measured > EPS)
        {
            m_node[i].native = measured;
            source[i] = "measured from root motion";
        }
        else
        {
            m_node[i].native = fallback[i];
            source[i] = from_cfg ? "from the creature's velocity table" : "HARDCODED -- no root motion and no velocity table";
        }
    }

    m_part = m_node[0].B->bone_or_part;
    m_state_class = state_class;
    m_mental = mental;
    m_idle_var = s8(idle_var);
    m_move_var = s8(move_var);
    m_sprint = sprint;
    m_actor = actor;
    m_back = back;
    m_built = true;

    // ⚠Bounded, but never silently: after the cap it keeps a running total, so
    // a quiet log means "no more set changes", not "the counter ran out". That
    // distinction cost a whole conclusion once already.
    static u32 s_builds = 0;
    ++s_builds;
    if (s_builds <= 24 || 0 == (s_builds % 200))
    {
        // ⭐The names that RESOLVED, not the ones that were asked for. Asking
        // for the calm take and silently getting the alert one back off the
        // fallback chain is the exact failure this whole change repairs, and a
        // line printing `idle_1` while `idle_0` plays would have hidden it --
        // the same instrument defect as a counter that counts the order.
        const std::pair<LPCSTR, LPCSTR> n_i = ka->LL_MotionDefName_dbg(m_node[n_idle].motion);
        const std::pair<LPCSTR, LPCSTR> n_a = ka->LL_MotionDefName_dbg(m_node[n_move_a].motion);
        const std::pair<LPCSTR, LPCSTR> n_b = ka->LL_MotionDefName_dbg(m_node[n_move_b].motion);
        Msg("* [blendspace] '%s' built %s%s set (mental=%u, wanted idle_%d/move_%d): got '%s' / '%s' / '%s'; walk %.2f m/s (%s), run %.2f m/s (%s), part=%u -- %u builds so far",
            diag_sect, pfx, back ? "back" : "fwd", mental, idle_var, move_var, n_i.first ? n_i.first : "?", n_a.first ? n_a.first : "?", n_b.first ? n_b.first : "?",
            m_node[n_move_a].native, source[n_move_a], m_node[n_move_b].native, source[n_move_b], m_part, s_builds);
    }
    return true;
}

void CLegsBlendSpace::Release(IKinematicsAnimated* ka)
{
    // ⚠A manual blend has no curve left to carry it to zero, so it will hold
    // its last weight forever unless somebody closes it. That is the price of
    // owning the weight -- see CBlend::eManual.
    if (ka && m_built && m_part != u16(-1))
        ka->LL_CloseCycle(m_part, 1 << 0);
    for (SNode& n : m_node)
        n = SNode();
    m_built = false;
    m_state_class = 0xff;
    m_mental = 0xff;
    m_idle_var = -1;
    m_move_var = -1;
    m_sprint = false;
    m_part = u16(-1);
}

void CLegsBlendSpace::Update(IKinematicsAnimated* ka, LPCSTR diag_sect, u16 diag_id, const SInput& in, float dt)
{
    if (!ka)
        return;

    // Which direction the set faces, with a band around the decision: at a dead
    // stop the sign of a velocity is noise, and rebuilding the set on it would
    // put back the very restart this class exists to remove.
    // ⚠⚠ONE LIVE EDGE IS NOT A BAND: decide the direction only while there IS
    // one.
    //
    // The exit sat at -0.10, which a creature at a dead stop clears -- fwd is
    // exactly 0.00 when nothing moves. So a stalker pausing between backward
    // steps left the back set, and the next step rebuilt it. Measured 01-08 in
    // play: 358 flips in four minutes across eight creatures, NINETY-SIX of
    // them "-> fwd at fwd=0.00 speed=0.00". Every one is three PlayCycles that
    // restart the gait from its first frame, which on screen is the sliding
    // this class was written to remove.
    //
    // ⭐At a standstill the idle node carries the entire weight, so which
    // direction the two move nodes hold is INVISIBLE. There is nothing to
    // decide, and deciding anyway costs a rebuild. Keep whatever was there and
    // let the next real step speak.
    bool want_back = m_back;
    if (in.speed > 0.15f)
    {
        if (in.fwd < -0.35f)
            want_back = true;
        else if (in.fwd > 0.10f)
            want_back = false;
    }

    // diag: THE SIGN, which is the entire question for a backpedal.
    //
    // USER-REPORTED "неестественно быстро назад и проскальзывают". The back
    // cycles are here and Build resolves them (walk_back_*, run_back_*), so
    // this is not missing content -- either the number never arrives negative
    // at all, or it arrives too small to cross the band. Those are repaired in
    // different places, and a log that only fires on the flip cannot tell them
    // apart: no line would mean both "never went backwards" and "went
    // backwards at -0.30 forty times".
    //
    // ⭐So: every new record low, AND every flip. The record keeps a running
    // minimum, so a quiet log after a fight is evidence, not an exhausted
    // counter.
    {
        static float s_fwd_min = 0.f;
        static u32 s_lows = 0;
        if (in.fwd < s_fwd_min - 0.05f && s_lows < 40)
        {
            s_fwd_min = in.fwd;
            ++s_lows;
            Msg("~ [back] id=%u new low fwd=%.2f speed=%.2f (band needs < -0.35) want_back=%d", diag_id, in.fwd, in.speed, want_back ? 1 : 0);
        }
        if (want_back != m_back)
            Msg("~ [back] id=%u FLIP -> %s at fwd=%.2f speed=%.2f", diag_id, want_back ? "BACK" : "fwd", in.fwd, in.speed);
    }

    // ⚠⚠REBUILD ONLY WHEN THE CYCLES CHANGE. Speeds are data, not structure.
    //
    // The first version put the mental state and the sprint bit in this key,
    // on the reasoning that they change the node speeds. They do -- but a
    // speed is one float, and rebuilding to update it costs three PlayCycles
    // and leaves three orphaned blends decaying on the partition. Measured in
    // play: **1200 rebuilds in one session** across three creatures.
    //
    // ⭐And it was not merely wasteful, it produced a visible defect. Every
    // rebuild leaves the previous set fading on the same bones, so the blend
    // count per bone climbs; at MAX_BLENDED CBlendInstance::blend_add starts
    // EVICTING the lowest-weight blend, which is always one of ours sitting at
    // zero. Evict the node that is about to carry weight 1.0 and the bone is
    // left with a mix that sums to zero -- MixInterlerp then returns the first
    // blend's raw key verbatim, and since a zero-weight node also has speed 0,
    // that key never advances. Reported from play exactly so: one creature
    // stopped in a frozen running pose while the others stood normally.
    //
    // So: prefix, direction and skeleton kind rebuild. Tariffs are re-read in
    // place, below.
    //
    // ⚠And now the VARIANT, because it is on the same side of that line: calm
    // and alert are two different clips, not two speeds for one clip. It is
    // keyed on the derived variants rather than on `mental` itself, so the
    // states that share a clip (crouched danger and crouched panic, say) still
    // do not rebuild -- which is the whole reason mental was taken out of this
    // key in the first place.
    int want_idle_var = 0, want_move_var = 0;
    gait_variants(in.state_class, in.mental, in.actor, want_idle_var, want_move_var);

    const bool stale = !m_built || m_state_class != in.state_class || m_actor != in.actor || m_back != want_back || m_idle_var != s8(want_idle_var) ||
        m_move_var != s8(want_move_var) || !NodesAlive();
    if (stale)
    {
        // ⚠Throttled, and per instance. A visual with no usable cycles fails to
        // build every time, and retrying that per frame is a PlayCycle storm on
        // a skeleton that cannot use it. (A shared throttle would let one such
        // skeleton hold up every other puppet's first build.)
        if (!m_built && Device.dwTimeGlobal < m_next_retry)
            return;
        if (!Build(ka, diag_sect, diag_id, in.state_class, in.mental, in.sprint, in.actor, want_back))
        {
            m_next_retry = Device.dwTimeGlobal + 1000;
            return;
        }
    }

    // The tariff, re-read in place when the row changes. One table lookup, no
    // blends touched -- see the rebuild key above for what this replaced.
    if (m_mental != in.mental)
    {
        m_mental = in.mental;
        float w = 0.f, r = 0.f;
        if (GaitSpeeds(diag_sect, m_state_class, m_mental, m_back, w, r))
        {
            if (w > EPS)
                m_node[n_move_a].native = w;
            if (r > EPS)
                m_node[n_move_b].native = r;
        }
    }

    const float v = _abs(in.speed);
    const float v_walk = m_node[n_move_a].native;
    const float v_run = m_node[n_move_b].native;

    float w_idle = 0.f, w_a = 0.f, w_b = 0.f;
    if (v <= k_idle_lo)
        w_idle = 1.f;
    else if (v < k_idle_hi)
    {
        const float t = (v - k_idle_lo) / (k_idle_hi - k_idle_lo);
        w_idle = 1.f - t;
        w_a = t;
    }
    else if (v_run > v_walk + EPS && v >= v_walk)
    {
        float t = (v - v_walk) / (v_run - v_walk);
        clamp(t, 0.f, 1.f);
        w_a = 1.f - t;
        w_b = t;
    }
    else
        w_a = 1.f;

    // Time warp. The blended gait's own speed is the weighted mean of what its
    // clips travel at; the rate that stops the feet sliding is the ratio of the
    // speed being asked for to that. Clamped, because a walk played at 3x is
    // not a run -- past the clamp the slide comes back, and that is the honest
    // failure mode (the alternative is a gait nobody authored).
    const float wm = w_a + w_b;
    float rate = 1.f;
    float phase_rate = 0.f;
    if (wm > EPS)
    {
        const float native = (w_a * v_walk + w_b * v_run) / wm;
        const float cycle_s = (w_a * m_node[n_move_a].metrics.time_s + w_b * m_node[n_move_b].metrics.time_s) / wm;
        if (native > EPS)
        {
            rate = v / native;
            clamp(rate, k_rate_min, k_rate_max);
        }
        if (cycle_s > EPS)
            phase_rate = rate / cycle_s;
    }

    m_phase += phase_rate * dt;
    m_phase -= float(iFloor(m_phase)); // wrap, keeping the fraction

    const float w[n_nodes] = {w_idle, w_a, w_b};
    for (int i = 0; i < n_nodes; ++i)
    {
        SNode& n = m_node[i];
        if (!n.B)
            continue;
        n.weight = w[i];
        // ⚠Never let the whole mix reach zero: the key blender divides by the
        // sum of the weights and falls back to the first blend's raw key when
        // it is zero, which is a pose nobody chose.
        n.B->blendAmount = (w[i] > EPS_S) ? w[i] : 0.f;

        if (n_idle == i)
        {
            // The idle loop is a breathing cycle, not a stride: it has no phase
            // relationship to the feet and is left to run at its own pace.
            n.B->speed = n.authored_speed;
            continue;
        }

        // ⭐ONE PHASE FOR THE WHOLE MIX. Two locomotion clips blended at
        // independent phases average a left foot against a right foot, and the
        // result is the shuffle that gives cheap blend spaces their reputation.
        // The lock is written every frame; the speed is set alongside it so the
        // pose stays right even on a frame this update does not reach.
        n.B->speed = phase_rate * n.B->timeTotal;
        float t = m_phase * n.B->timeTotal;
        clamp(t, 0.f, n.B->timeTotal - EPS_S);
        n.B->timeCurrent = t;
    }
    if (w_idle <= EPS_S && wm <= EPS_S && m_node[n_idle].B)
        m_node[n_idle].B->blendAmount = EPS_S; // never a mix of nothing

    // ⭐Stage 3 of the ledger, and the only stage that can tell a clip that is
    // MISSING from one that is present, accepted, and then quietly not running.
    // Done after the weights are written, so what it checks is what the mixer
    // just asked for rather than last frame's intention.
    if (ClAnimLedger::watching(diag_id))
        for (int i = 0; i < n_nodes; ++i)
            ClAnimLedger::Audit(diag_id, u32(i), ka, m_node[i].B, m_node[i].motion, m_node[i].weight);

    // Heartbeat: the weights and the rate actually in force. Throttled by count
    // rather than capped, for the same reason as the build log.
    static u32 s_beats = 0;
    if (++s_beats % 601 == 1)
        Msg("* [blendspace] id=%u v=%.2f w=(%.2f %.2f %.2f) rate=%.2f phase=%.2f native=(%.2f %.2f) sprint=%d", diag_id, v, w_idle, w_a, w_b, rate, m_phase, v_walk, v_run,
            in.sprint ? 1 : 0);
}

// ------------------------------------------------------------------------
// Diagnostics
// ------------------------------------------------------------------------

void DumpRootMotion(u16 id)
{
    CObject* O = nullptr;
    if (0 == id || u16(-1) == id)
        O = Level().CurrentControlEntity();
    else
        O = Level().Objects.net_Find(id);

    if (!O)
    {
        Msg("! [rootmotion] no object for id=%u (0 = the entity you control)", id);
        return;
    }

    IKinematicsAnimated* ka = smart_cast<IKinematicsAnimated*>(O->Visual());
    if (!ka)
    {
        Msg("! [rootmotion] id=%u '%s' has no animated visual", O->ID(), O->cName().c_str());
        return;
    }

    // ⭐Every locomotion cycle the puppet layer can select, in one table. The
    // question this answers -- "do these clips carry root translation, or does
    // the AI carry the body?" -- decides whether the playback rate can be
    // derived at all, and it had been assumed in both directions on different
    // evenings without ever being asked.
    static LPCSTR names[] = {
        "norm_idle_0", "norm_walk_fwd_0", "norm_run_fwd_0", "norm_walk_back_0", "norm_run_back_0", "cr_idle_0", "cr_walk_fwd_0", "cr_run_fwd_0",
        "dmg_norm_idle_0", "dmg_norm_walk_fwd_0", "dmg_norm_run_fwd_0", "stand_idle_0", "walk_fwd_0", "run_fwd_0",
    };

    Msg("~ [rootmotion] id=%u '%s' sect='%s'", O->ID(), O->cName().c_str(), O->cNameSect().c_str());
    Msg("~ [rootmotion] %-22s %6s %8s %9s %9s  %s", "cycle", "keys", "cycle s", "dist m", "m/s", "verdict");
    u32 travelling = 0, present = 0;
    for (LPCSTR n : names)
    {
        MotionID m = ka->ID_Cycle_Safe(n);
        if (!m.valid())
            continue;
        ++present;
        SCycleMetrics mt;
        if (!CycleMetrics(ka, m, mt))
        {
            Msg("~ [rootmotion] %-22s %6s %8s %9s %9s  UNREADABLE", n, "-", "-", "-", "-");
            continue;
        }
        if (!mt.in_place)
            ++travelling;
        Msg("~ [rootmotion] %-22s %6u %8.3f %9.3f %9.3f  %s", n, mt.keys, mt.time_s, mt.dist_m, mt.native_speed(), mt.in_place ? "IN PLACE (speed assumed)" : "travels");
    }
    Msg("~ [rootmotion] %u of %u present cycles carry root translation. %s", travelling, present,
        travelling ? "Playback rate can be derived from the data." : "Content animates in place -- the assumed ltx speeds stand.");
}
} // namespace ClAnim

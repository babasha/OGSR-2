#pragma once

// =====================================================================
//  Is the motion the server ASSERTS actually smooth?
//
//  ⚠Two sessions of work on the client's presentation started from an
//  assumption nobody ever checked. What has been measured about the stream is
//  its CADENCE (dt=50 ms, dead regular), its STEP SIZE (0.083 m, dead regular)
//  and who else writes the transform (zero foreign yaw writes). All three say
//  the pipe is disciplined. None of them says the PATH is smooth.
//
//  A path can arrive perfectly on time, in perfectly equal steps, and still
//  turn a corner every tick, reverse into its own tail on a replan, or advance
//  by equal distances across unequal amounts of time. A client rendering that
//  faithfully looks exactly like a client with a broken interpolator -- which
//  is the symptom that eight rounds of interpolator work never fully removed.
//
//  So this measures the SOURCE, and it does it where the two things that can
//  disagree are both visible:
//
//    body    -- where the creature actually IS, once a frame, whoever moved it
//               (CCustomMonster::UpdateCL)
//    assert  -- what the drive told the creature to be (SVNpc::Update)
//    export  -- what actually went out on the wire (CCustomMonster::net_Export)
//
//  ⚠`body` was added 01-08 after the audit returned ZERO samples on a dog that
//  was visibly running: the other sites all sit downstream of the drive or of
//  an export to a connected client, and a creature moving under its own brain
//  on a headless host passes through neither. An instrument that cannot see
//  the subject reports silence, and silence reads like health.
//
//  If those two differ, the defect is upstream of the network entirely, and
//  no amount of receiver-side smoothing was ever going to fix it.
//
//  ⭐Runs on the HEADLESS server with no client, no GPU and nobody watching --
//  which makes it the one place in this project where a motion defect can be
//  chased in a loop instead of a play session.
//
//  Console:
//     sv_motion_audit <id>     start recording that creature (0 = stop+report)
//     sv_motion_audit_all      every driven puppet (heavier)
// =====================================================================

class CGameObject;

namespace SVMotionAudit
{
// A pose the server believes in, sampled at a named point in the frame.
// `driven` records whether the server's claim was live at that moment: the
// handoff between the drive and the creature's own brain is a known suspect
// (the "rollback on release" defect), and a sample that cannot say which side
// wrote it cannot convict either.
//
// ⭐`stamp` is the creature's OWN timestamp -- the number that leaves in the
// packet and paces the client's interpolator. It is deliberately a third
// clock, not a substitute for either of the two the recorder takes itself:
//
//   Device.dwTimeGlobal  real milliseconds on this box, at the sample
//   Level().timeServer() the clock the stamp is READ FROM, at the sample
//   stamp                the value that is actually IN the mirror, going out
//
// They can only disagree in three ways, and each one names a different
// culprit: stamp frozen while timeServer advances = the mirror was not
// refreshed (export did not run); timeServer frozen while dwTimeGlobal
// advances = the clock itself stalls; all three advancing evenly while the
// position jumps = the body really did move that far, and the wire is honest.
// Without the third number the first two cases are indistinguishable, which
// is exactly how "the stamp stalls" got inferred from its consequence.
void Note(u16 id, const char* site, const Fvector& pos, float yaw, bool driven, u32 stamp = 0);

// Called once per server tick from xrServer::Update, so the recorder can
// stamp real time and close out per-tick bookkeeping.
void Tick();

// sv_motion_audit
void Start(u16 id);
void Stop();

// Level change / shutdown.
void Reset();
} // namespace SVMotionAudit

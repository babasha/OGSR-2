#pragma once

// =====================================================================
//  WANTED -> PLAYED? -> DID IT DO WHAT WAS ASKED?
//
//  ⭐The existing trace (stalker_animation_trace) answers "what is playing".
//  This answers the question that actually blocks animation work: what was
//  ASKED FOR and did not happen -- because on this path a request can die in
//  four different ways and every one of them is silent.
//
//    1. THE NAME IS NOT IN THE MODEL. The selector walks a fallback chain
//       ("panic_walk_fwd_0", then "norm_walk_fwd_0", then "walk_fwd_0") and
//       uses whatever resolves. Nothing says the first choice was missing, so
//       a whole mental state can be silently animating as another one.
//    2. THE POOL REFUSED IT. LL_PlayCycle returns null and the caller sees a
//       null pointer it may or may not check.
//    3. THE BLEND WAS EVICTED. At MAX_BLENDED, CBlendInstance::blend_add drops
//       the LOWEST-WEIGHT blend -- which is precisely the node just added, the
//       one about to be driven to 1.0. This has already cost this project a
//       session: 1200 rebuilds a minute ended in a FROZEN POSE, and it was
//       found only because a human said "замершая", not "бежит на месте".
//    4. IT PLAYS BUT DOES NOT ADVANCE. A manual blend whose speed is zero, or
//       whose weight never reaches what the mixer wanted, renders as a pose
//       rather than an animation and looks identical to a missing clip.
//
//  ⚠None of the four produces a log line today, and three of them look the
//  same on screen. Hence a ledger rather than another per-frame dump: the
//  per-name summary is what catches the rare one, the live lines are what
//  catch the sequence.
//
//  Console:
//     cl_anim_ask <id>     watch one creature (0 = off)
//     cl_anim_ask_dump     the summary table, then reset
// =====================================================================

class IKinematicsAnimated;
struct CBlend;
// ⚠struct, not class. MSVC mangles the two differently, so a forward
// declaration that disagrees with the definition links against nothing --
// which it did, once, and the only symptom is an unresolved external naming a
// function that is plainly right there.
struct MotionID;

namespace ClAnimLedger
{
extern int g_ask_id; // cl_anim_ask -- 0 is off, otherwise the entity id

inline bool watching(u16 id) { return g_ask_id > 0 && u16(g_ask_id) == id; }

// Stage 1. One call per candidate in a fallback chain, in the order tried, so
// the ledger can say "wanted X, not in this model, settled for Y".
void Wanted(u16 id, LPCSTR name, bool present, bool first_choice);

// Stage 2. What came back from the engine for the request just made.
void Played(u16 id, LPCSTR name, u16 part, CBlend* B);

// Stage 3. Once per frame per node: is the blend still ours, is it advancing,
// did its weight get where the mixer wanted it. `want` is the weight the caller
// intends this node to hold right now.
void Audit(u16 id, u32 slot, IKinematicsAnimated* ka, CBlend* B, const MotionID& expect, float want);

void Dump();
void Reset();
} // namespace ClAnimLedger

#include "StdAfx.h"
#include "cl_anim_ledger.h"

#include "../Include/xrRender/KinematicsAnimated.h"
#include "../Include/xrRender/animation_blend.h"

int ClAnimLedger::g_ask_id = 0;

namespace
{
// ⚠Bounded on purpose. This runs per frame per node on a traced creature, and
// an unbounded table of names is a leak nobody notices in a two-hour session.
constexpr u32 k_max_names = 64;

// A name and what became of every request for it. The SUMMARY is the point:
// the failures worth finding here are rare, and a rare line in a per-frame
// stream is a line nobody reads.
struct SName
{
    string64 name{};
    // ⚠⚠FIRST CHOICE AND FALLBACK ARE NOT THE SAME REQUEST, and pooling them
    // makes the summary lie. The chain exists precisely because its later
    // entries are expected to be absent: "walk_fwd_0 missing 98 times" is the
    // chain working, not a defect. Only a FIRST choice going missing means the
    // creature is animating as something it is not.
    u32 wanted{0}; // considered at all, in any position of the chain
    u32 first{0}; // ...as the first choice
    u32 missing{0}; // not in the model, in any position
    u32 miss_first{0}; // ...and it was the first choice. This one is the defect.
    u32 played{0}; // the engine accepted it
    u32 refused{0}; // ...or returned nothing
    u32 evicted{0}; // accepted, then the slot was taken by another motion
    u32 stalled{0}; // held a weight but its clock did not move
    u32 weak{0}; // never reached the weight the mixer wanted
};

SName s_names[k_max_names];
u32 s_name_n = 0;

SName* find_name(LPCSTR name, bool create)
{
    for (u32 i = 0; i < s_name_n; ++i)
        if (0 == xr_strcmp(s_names[i].name, name))
            return &s_names[i];
    if (!create || s_name_n >= k_max_names)
        return nullptr;
    SName* n = &s_names[s_name_n++];
    xr_strcpy(n->name, name);
    return n;
}

// Per-node state, so stage 3 can see CHANGE rather than a snapshot: a blend
// that is frozen and a blend that is playing look identical in one frame.
struct SSlot
{
    CBlend* B{nullptr};
    float last_time{-1.f};
    float max_amount{0.f};
    u32 still_frames{0};
    u32 born_ms{0};
    bool reported_evicted{false};
    bool reported_stalled{false};
    bool reported_weak{false};
    string64 name{};
};

constexpr u32 k_max_slots = 8;
SSlot s_slot[k_max_slots];

LPCSTR motion_name(IKinematicsAnimated* ka, const MotionID& m)
{
    if (!ka || !m.valid())
        return "-";
    const std::pair<LPCSTR, LPCSTR> nm = ka->LL_MotionDefName_dbg(m);
    return nm.first ? nm.first : "?";
}
} // namespace

void ClAnimLedger::Wanted(u16 id, LPCSTR name, bool present, bool first_choice)
{
    if (!watching(id) || !name || !name[0])
        return;

    if (SName* n = find_name(name, true))
    {
        ++n->wanted;
        if (first_choice)
            ++n->first;
        if (!present)
        {
            ++n->missing;
            if (first_choice)
                ++n->miss_first;
        }
    }

    // ⭐Only the FIRST choice going missing is news. A fallback that is absent
    // is the chain doing its job; the first choice being absent means the
    // creature is animating as something it is not, and that is invisible
    // everywhere else.
    if (!present && first_choice)
        Msg("! [animask] id=%u WANTED '%s' -- NOT IN THIS MODEL, falling back", id, name);
}

void ClAnimLedger::Played(u16 id, LPCSTR name, u16 part, CBlend* B)
{
    if (!watching(id) || !name)
        return;

    if (SName* n = find_name(name, true))
    {
        if (B)
            ++n->played;
        else
            ++n->refused;
    }

    if (!B)
    {
        Msg("! [animask] id=%u '%s' -- REFUSED by the engine (no blend returned; pool full?)", id, name);
        return;
    }
    Msg("~ [animask] id=%u '%s' -> playing on part %u (slot %p, speed %.2f, total %.2fs)", id, name, part, B, B->speed, B->timeTotal);
}

void ClAnimLedger::Audit(u16 id, u32 slot, IKinematicsAnimated* ka, CBlend* B, const MotionID& expect, float want)
{
    if (!watching(id) || slot >= k_max_slots)
        return;

    SSlot& S = s_slot[slot];
    LPCSTR want_name = motion_name(ka, expect);

    // ⚠⚠A FRESH REQUEST IS NOT AN EVICTION, AND THE POINTER CANNOT TELL THEM
    // APART. The blend pool reuses slot ADDRESSES: rebuild the space for the
    // backwards set and the new norm_walk_back_0 can land on the very address
    // norm_walk_fwd_0 just vacated. Keying the reset on the pointer alone
    // therefore compared the new node against the old node's name and reported
    // seven evictions that never happened -- measured, every one of them a
    // walk_fwd<->walk_back pair on a reversal.
    //
    // The distinction that does hold: if what the CALLER is asking for has
    // changed, this is a new request; if the caller still wants the same cycle
    // and the slot is playing another, that is an eviction.
    if (S.B != B || 0 != xr_strcmp(S.name, want_name))
    {
        S.B = B;
        S.last_time = -1.f;
        S.max_amount = 0.f;
        S.still_frames = 0;
        S.born_ms = Device.dwTimeGlobal;
        S.reported_evicted = S.reported_stalled = S.reported_weak = false;
        xr_strcpy(S.name, want_name);
    }

    if (!B)
        return;

    // ⭐⭐EVICTION, and it is detected by IDENTITY rather than by absence. At
    // MAX_BLENDED the engine reuses the lowest-weight slot for somebody else's
    // motion; the pointer stays valid and keeps looking like a blend, it is
    // simply no longer OURS. Comparing what the slot is playing against what we
    // asked it to play is the only thing that separates the two.
    const bool freed = (B->blend_state() == CBlend::eFREE_SLOT);
    LPCSTR now_playing = motion_name(ka, B->motionID);
    const bool hijacked = (0 != xr_strcmp(now_playing, S.name));

    if ((freed || hijacked) && !S.reported_evicted)
    {
        S.reported_evicted = true;
        if (SName* n = find_name(S.name, false))
            ++n->evicted;
        Msg("! [animask] id=%u '%s' -- EVICTED after %u ms: the slot now holds '%s'%s", id, S.name, Device.dwTimeGlobal - S.born_ms, freed ? "(free)" : now_playing,
            freed ? "" : " -- MAX_BLENDED reached, lowest weight dropped");
        return;
    }

    S.max_amount = _max(S.max_amount, B->blendAmount);

    // Did its clock move? A manual blend with speed 0, or one nobody advances,
    // holds a POSE -- which on screen is indistinguishable from a missing clip.
    if (want > 0.05f)
    {
        if (S.last_time >= 0.f && fsimilar(S.last_time, B->timeCurrent, EPS_L))
            ++S.still_frames;
        else
            S.still_frames = 0;

        if (S.still_frames > 30 && !S.reported_stalled)
        {
            S.reported_stalled = true;
            if (SName* n = find_name(S.name, false))
                ++n->stalled;
            Msg("! [animask] id=%u '%s' -- PLAYING BUT NOT ADVANCING: weight %.2f, speed %.2f, time stuck at %.3f for %u frames", id, S.name, B->blendAmount, B->speed,
                B->timeCurrent, S.still_frames);
        }
    }
    S.last_time = B->timeCurrent;

    // ...and did the weight the mixer asked for ever arrive? Half a second is
    // long past any accrue curve, so a node still far below its target by then
    // is being held down by something else.
    if (want > 0.5f && (Device.dwTimeGlobal - S.born_ms) > 500 && !S.reported_weak && S.max_amount < want * 0.5f)
    {
        S.reported_weak = true;
        if (SName* n = find_name(S.name, false))
            ++n->weak;
        Msg("! [animask] id=%u '%s' -- WEIGHT NEVER ARRIVED: wanted %.2f, best seen %.2f after %u ms", id, S.name, want, S.max_amount, Device.dwTimeGlobal - S.born_ms);
    }
}

void ClAnimLedger::Dump()
{
    if (!s_name_n)
    {
        Msg("~ [animask] nothing recorded -- cl_anim_ask <id> first, and the creature has to be visible to this process");
        return;
    }

    Msg("~ [animask] ================ request ledger ================");
    Msg("~ [animask] %-28s %6s %6s %7s %7s %7s %7s %7s %5s", "cycle", "1st", "asked", "MISS1st", "missing", "played", "refused", "evicted", "bad");
    for (u32 i = 0; i < s_name_n; ++i)
    {
        const SName& n = s_names[i];
        Msg("~ [animask] %-28s %6u %6u %7u %7u %7u %7u %7u %5u", n.name, n.first, n.wanted, n.miss_first, n.missing, n.played, n.refused, n.evicted,
            n.stalled + n.weak);
    }

    // ⚠The verdict, and it counts ONLY what is actually wrong. The first
    // version summed the `missing` column and announced "392 requests went
    // wrong" for a run in which nothing went wrong at all: those were fallback
    // names doing their job. A summary that cries wolf is worse than no
    // summary, because it trains you to skip the line that matters.
    u32 bad = 0;
    for (u32 i = 0; i < s_name_n; ++i)
        bad += s_names[i].miss_first + s_names[i].refused + s_names[i].evicted + s_names[i].stalled + s_names[i].weak;
    Msg("~ [animask] *** %u cycle names seen, %u requests went wrong", s_name_n, bad);
    Msg("~ [animask] MISS1st/refused/evicted/bad are defects; `missing` on a fallback name is the chain working as intended");
    Reset();
}

void ClAnimLedger::Reset()
{
    s_name_n = 0;
    for (u32 i = 0; i < k_max_slots; ++i)
        s_slot[i] = SSlot();
}

#include "StdAfx.h"
#include "shot_latency.h"

#include "alife_space.h"
#include "Level.h"

#include <algorithm>
#include <mutex>

// Off by default -- see the header.
int ShotLatency::g_shot_trace = 0;

namespace
{
// ⚠The two ends run on different threads inside ONE process: a host simulates
// its NPCs' bullets on the frame thread (CBulletManager) while the server tick
// that dequeues their GE_HITs comes off the TTAPI pool. Unlocked, an OnImpact
// could grow the ledger under an OnApplied reading it. Same bug the sv_npc
// roster had, found the same way -- so it is locked from the start here.
std::recursive_mutex s_lock;
// A test fires a burst, not a battle. Filling the ring is a signal that
// somebody left the trace on in a firefight, so it says so once and stops
// rather than wrapping: a window that silently slides is a window whose
// aggregates cannot be trusted.
constexpr size_t MAX_SHOTS = 512;

struct Rec
{
    ShotLatency::Sample s;
    u64 t_impact{0}; // shooter: GE_HIT handed to the transport
    u64 t_arrive{0}; // recorder: off the wire, being queued
    u64 t_apply{0};  // recorder: dequeued, before any gate
    u64 t_write{0};  // recorder: the record exists
    bool relayed{false};
};

Rec s_shots[MAX_SHOTS];
size_t s_count = 0;
size_t s_last_applied = MAX_SHOTS; // index OnRecorded closes
bool s_full_warned = false;

// ⭐Volume counters, deliberately OUTSIDE the ring. The ring holds per-shot
// detail and is bounded, so in a hundred-man firefight it fills in seconds and
// stops accepting -- which is fine for latency and useless for totals. These
// keep counting, so a hit rate stays honest however long the fight runs.
u64 s_fired = 0;
u64 s_fired_by_player = 0;
u64 s_impacts = 0;

// Two GE_HITs are the same bullet when they land on the same bone of the same
// victim at the same point in that bone's space. The point is copied by
// w_vec3/r_vec3, bit for bit, so exact float comparison is the correct test
// here and a tolerance would only invent collisions.
bool same_shot(const ShotLatency::Sample& a, const ShotLatency::Sample& b)
{
    return a.victim == b.victim && a.bone == b.bone && a.in_bone.x == b.in_bone.x && a.in_bone.y == b.in_bone.y && a.in_bone.z == b.in_bone.z;
}

Rec* find_or_add(const ShotLatency::Sample& s)
{
    for (size_t i = s_count; i-- > 0;)
        if (same_shot(s_shots[i].s, s))
            return &s_shots[i];

    if (s_count >= MAX_SHOTS)
    {
        if (!s_full_warned)
        {
            s_full_warned = true;
            Msg("! [shot] %u shots recorded, the window is full -- 'shot_trace 0' or 'shot_report' to start over", u32(MAX_SHOTS));
        }
        return nullptr;
    }

    Rec& r = s_shots[s_count++];
    r = Rec{};
    r.s = s;
    return &r;
}

void fmt_ib(const Fvector& v, string128& out)
{
    // Six decimals: enough that two bullets would have to land inside a
    // micrometre of each other on the same bone to collide, which dispersion
    // makes impossible, and it is the shot's join key in the harness.
    xr_sprintf(out, "%.6f/%.6f/%.6f", v.x, v.y, v.z);
}

// min / median / max of a leg, in microseconds. Legs are reported as three
// numbers rather than a mean because a single stall is the interesting case
// and a mean hides it.
struct Span
{
    u32 n{0};
    u64 lo{0}, mid{0}, hi{0};
};

Span span_of(xr_vector<u64>& v)
{
    Span s;
    if (v.empty())
        return s;
    std::sort(v.begin(), v.end());
    s.n = u32(v.size());
    s.lo = v.front();
    s.mid = v[v.size() / 2];
    s.hi = v.back();
    return s;
}
} // namespace

void ShotLatency::ReadHit(NET_Packet& P, Sample& out)
{
    // SHit::Write_Packet_Cont's layout, read from wherever the caller's cursor
    // sits (the victim id) and put back. Even if a future field shifts this,
    // only the measurement is wrong -- the hit itself is untouched.
    const u32 saved = P.r_tell();

    Fvector dir;
    u16 aim = 0;
    P.r_u16(out.victim);
    P.r_u16(out.attacker);
    P.r_u16(out.weapon);
    P.r_dir(dir);
    P.r_float(out.power);
    P.r_u16(out.bone);
    P.r_vec3(out.in_bone);
    P.r_float(out.impulse);
    P.r_u16(aim);
    P.r_u16(out.hit_type);
    out.aim = aim != 0;

    out.ap = 0.f;
    if (ALife::eHitTypeFireWound == out.hit_type)
        P.r_float(out.ap);

    P.r_seek(saved);
}

void ShotLatency::OnFired(u16 shooter_id)
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    ++s_fired;

    // Split so the number stays readable on a host where a player is also
    // shooting: the interesting rate on a server is the one its NPCs achieve.
    if (g_pGameLevel && Level().CurrentControlEntity() && Level().CurrentControlEntity()->ID() == shooter_id)
        ++s_fired_by_player;
}

void ShotLatency::OnImpact(const Sample& s)
{
    const u64 now = CPU::QPC_us();
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    ++s_impacts;

    Rec* r = find_or_add(s);
    if (r)
        r->t_impact = now;

    if (g_shot_trace)
    {
        string128 ib;
        fmt_ib(s.in_bone, ib);
        Msg("~ [shot] impact t=%llu vic=%u att=%u wpn=%u bone=%u ht=%u pow=%.4f ap=%.3f ib=%s", now, s.victim, s.attacker, s.weapon, s.bone, s.hit_type, s.power, s.ap, ib);
    }
}

void ShotLatency::OnArrived(NET_Packet& P)
{
    const u64 now = CPU::QPC_us();
    std::lock_guard<std::recursive_mutex> guard(s_lock);

    Sample s;
    ReadHit(P, s);
    Rec* r = find_or_add(s);
    if (r && !r->t_arrive)
        r->t_arrive = now;

    if (g_shot_trace)
    {
        string128 ib;
        fmt_ib(s.in_bone, ib);
        Msg("~ [shot] arrive t=%llu vic=%u bone=%u ib=%s", now, s.victim, s.bone, ib);
    }
}

void ShotLatency::OnApplied(NET_Packet& P)
{
    const u64 now = CPU::QPC_us();
    std::lock_guard<std::recursive_mutex> guard(s_lock);

    Sample s;
    ReadHit(P, s);
    Rec* r = find_or_add(s);
    s_last_applied = MAX_SHOTS;
    if (r)
    {
        r->t_apply = now;
        s_last_applied = size_t(r - s_shots);
    }
}

void ShotLatency::OnRecorded(bool relayed)
{
    const u64 now = CPU::QPC_us();
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    if (s_last_applied >= MAX_SHOTS)
        return;

    Rec& r = s_shots[s_last_applied];
    r.t_write = now;
    r.relayed = relayed;
    s_last_applied = MAX_SHOTS;

    if (g_shot_trace)
    {
        string128 ib;
        fmt_ib(r.s.in_bone, ib);
        Msg("~ [shot] record t=%llu vic=%u att=%u bone=%u pow=%.4f ap=%.3f ht=%u relayed=%d queue_us=%llu write_us=%llu ib=%s", now, r.s.victim, r.s.attacker, r.s.bone, r.s.power,
            r.s.ap, r.s.hit_type, relayed ? 1 : 0, r.t_arrive ? (r.t_apply - r.t_arrive) : 0ull, r.t_apply ? (now - r.t_apply) : 0ull, ib);
    }
}

void ShotLatency::Reset()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    s_count = 0;
    s_last_applied = MAX_SHOTS;
    s_full_warned = false;
    s_fired = 0;
    s_fired_by_player = 0;
    s_impacts = 0;
    Msg("~ [shot] window cleared");
}

void ShotLatency::Report()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);

    // ⭐Volume first, and printed even when nothing hit anything: "10 000 rounds
    // and no impacts" is a result, and the old early-return hid it behind
    // "nothing measured yet".
    if (s_fired || s_impacts)
    {
        const u64 npc_fired = s_fired - s_fired_by_player;
        Msg("~ [shot] rounds fired=%llu (npc %llu, player %llu) | impacts on something alive=%llu | hit rate %.1f%%", s_fired, npc_fired, s_fired_by_player, s_impacts,
            s_fired ? 100.0 * double(s_impacts) / double(s_fired) : 0.0);
    }

    if (!s_count)
    {
        Msg("~ [shot] no per-shot detail (shot_trace is %d)", g_shot_trace);
        return;
    }

    u32 impacts = 0, arrived = 0, applied = 0, written = 0, relayed = 0;
    xr_vector<u64> queue, write, whole;

    for (size_t i = 0; i < s_count; ++i)
    {
        const Rec& r = s_shots[i];
        if (r.t_impact)
            ++impacts;
        if (r.t_arrive)
            ++arrived;
        if (r.t_apply)
            ++applied;
        if (r.t_write)
            ++written;
        if (r.relayed)
            ++relayed;

        if (r.t_arrive && r.t_apply)
            queue.push_back(r.t_apply - r.t_arrive);
        if (r.t_apply && r.t_write)
            write.push_back(r.t_write - r.t_apply);
        // Only meaningful where one process saw both ends (the fused host).
        // Across processes the two halves live in two logs and are joined by
        // the harness on the impact fingerprint.
        if (r.t_impact && r.t_write)
            whole.push_back(r.t_write - r.t_impact);
    }

    Msg("~ [shot] %u shots: impact=%u arrive=%u apply=%u record=%u relayed=%u dropped=%u", u32(s_count), impacts, arrived, applied, written, relayed, written - relayed);

    const Span q = span_of(queue), w = span_of(write), a = span_of(whole);
    if (q.n)
        Msg("~ [shot] wire->dequeue  n=%u  %llu / %llu / %llu us (min/med/max)", q.n, q.lo, q.mid, q.hi);
    if (w.n)
        Msg("~ [shot] dequeue->record n=%u  %llu / %llu / %llu us", w.n, w.lo, w.mid, w.hi);
    if (a.n)
        Msg("~ [shot] impact->record  n=%u  %llu / %llu / %llu us (same process only)", a.n, a.lo, a.mid, a.hi);

    // Shots this process fired that it never saw recorded. On a network client
    // that is every shot (the record is written in the other process); on the
    // host it is a real loss.
    if (impacts && written < impacts)
        Msg("~ [shot] %u impacts have no record in THIS process", impacts - written);
}

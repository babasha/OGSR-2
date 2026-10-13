#include "StdAfx.h"
#include "sv_motion_audit.h"
#include "Level.h"

#include <mutex>

namespace
{
std::recursive_mutex s_lock;

struct Sample
{
    u32 wall_ms; // Device.dwTimeGlobal at the sample
    u32 server_ms; // Level().timeServer() -- the clock the stamp is read from
    u32 stamp_ms; // the creature's own timestamp: what LEAVES in the packet
    Fvector pos;
    float yaw;
    bool driven;
    char site[16];
};

// A run is bounded on purpose. This samples at tick rate from two points in
// the frame, and an unbounded recorder on a two-hour session is a log nobody
// reads and a memory leak nobody notices.
constexpr u32 k_max_samples = 40000;

bool s_active = false;
u16 s_id = 0;
xr_vector<Sample> s_samples;

// Per-site analysis. The two sites answer different questions and must not be
// mixed: "assert" is the server's intent, "export" is what the client got.
struct SiteStats
{
    u32 n{0};
    // time between consecutive samples at this site
    float dt_min{flt_max}, dt_max{0.f}, dt_sum{0.f};
    // distance covered between consecutive samples
    float step_min{flt_max}, step_max{0.f}, step_sum{0.f};
    // instantaneous speed = step/dt. Its SPREAD is the smoothness number:
    // a body moving evenly has one speed, a body that lurches has many.
    float spd_min{flt_max}, spd_max{0.f}, spd_sum{0.f}, spd_sum2{0.f};
    // how sharply the direction of travel turned between steps, in degrees
    float turn_max{0.f}, turn_sum{0.f};
    u32 turns_over_30{0}, turns_over_90{0};
    // a step that projects NEGATIVELY onto the previous one: the creature
    // went backwards. This is what a replan looks like from the outside.
    u32 backsteps{0};
    float backstep_worst{0.f};
    // ⭐⭐THE THREE-CLOCK VERDICT. The client's interpolator paces by the
    // stamp, so a stamp that fails to advance while the body does is jitter it
    // cannot possibly absorb -- but "the stamp did not advance" has two
    // different causes and they need different fixes. Counted separately:
    //
    //   clock_stalls  timeServer() read the same value twice while real
    //                 milliseconds passed -- the CLOCK is not monotone
    //   stamp_stalls  the mirror carried the same stamp twice while the body
    //                 moved -- the clock is fine, the EXPORT did not run
    //
    // Both zero, with the position still jumping, acquits the wire entirely
    // and sends the question back to the drive.
    // ⭐How sharply the HEADING moved between consecutive samples -- which is a
    // different question from `turn` below. `turn` measures the direction of
    // TRAVEL, and at a reversal that is honestly 180 degrees however smoothly
    // the body rotates. What the client draws is the heading it is sent, so
    // this is the one that says whether a turn is a turn or a spin.
    //
    // ⚠Sampled diagnostics cannot answer it: the client's own yaw line prints
    // about 2% of snapshots, and with a dozen reversals in a run the odds of
    // catching one are poor. Every sample is counted here.
    float yaw_step_max{0.f}; // degrees, between consecutive samples
    float yaw_rate_max{0.f}; // rad/s
    u32 clock_stalls{0};
    u32 stamp_stalls{0};
    u32 stamp_pairs{0}; // pairs where a stamp was actually recorded
    // how far the stamp's own advance drifts from real elapsed time. A wire
    // whose stamp tracks the wall says d_stamp ~= d_wall on every packet.
    float stamp_lag_max{0.f};
};

void accumulate(SiteStats& st, const Sample& prev, const Sample& cur)
{
    const float dt = float(cur.wall_ms - prev.wall_ms);
    Fvector d;
    d.sub(cur.pos, prev.pos);
    d.y = 0.f;
    const float step = d.magnitude();

    if (dt <= 0.f)
        return;

    ++st.n;
    st.dt_min = _min(st.dt_min, dt);
    st.dt_max = _max(st.dt_max, dt);
    st.dt_sum += dt;

    st.step_min = _min(st.step_min, step);
    st.step_max = _max(st.step_max, step);
    st.step_sum += step;

    const float spd = step / (dt / 1000.f);
    st.spd_min = _min(st.spd_min, spd);
    st.spd_max = _max(st.spd_max, spd);
    st.spd_sum += spd;
    st.spd_sum2 += spd * spd;

    const float yaw_d = angle_difference(cur.yaw, prev.yaw); // 0..PI
    st.yaw_step_max = _max(st.yaw_step_max, rad2deg(yaw_d));
    st.yaw_rate_max = _max(st.yaw_rate_max, yaw_d / (dt / 1000.f));

    // ⚠The clock is only on trial when real time actually passed: two samples
    // inside the same millisecond agreeing about the time is not a stall.
    if (cur.server_ms == prev.server_ms && dt >= 8.f)
        ++st.clock_stalls;

    // ⚠Stamp of 0 means the site did not record one (the drive has no mirror
    // to read), not that the clock returned zero -- do not convict on it.
    if (cur.stamp_ms && prev.stamp_ms)
    {
        ++st.stamp_pairs;
        if (cur.stamp_ms == prev.stamp_ms && step > 0.01f)
            ++st.stamp_stalls;
        const float d_stamp = float(cur.stamp_ms - prev.stamp_ms);
        st.stamp_lag_max = _max(st.stamp_lag_max, _abs(d_stamp - dt));
    }
}

void accumulate_turn(SiteStats& st, const Fvector& prev_dir, const Fvector& dir)
{
    const float dot = prev_dir.dotproduct(dir);
    float a = acosf(_max(-1.f, _min(1.f, dot)));
    const float deg = rad2deg(a);
    st.turn_max = _max(st.turn_max, deg);
    st.turn_sum += deg;
    if (deg > 30.f)
        ++st.turns_over_30;
    if (deg > 90.f)
        ++st.turns_over_90;
    if (dot < 0.f)
    {
        ++st.backsteps;
        st.backstep_worst = _max(st.backstep_worst, deg);
    }
}

void report_site(const char* site)
{
    SiteStats st;
    const Sample* prev = nullptr;
    Fvector prev_dir{};
    bool have_dir = false;

    for (const Sample& s : s_samples)
    {
        if (0 != xr_strcmp(s.site, site))
            continue;
        if (prev)
        {
            accumulate(st, *prev, s);

            Fvector d;
            d.sub(s.pos, prev->pos);
            d.y = 0.f;
            // only steps long enough to HAVE a direction: normalising noise
            // manufactures turns that never happened
            if (d.magnitude() > 0.02f)
            {
                d.normalize();
                if (have_dir)
                    accumulate_turn(st, prev_dir, d);
                prev_dir = d;
                have_dir = true;
            }
        }
        prev = &s;
    }

    if (!st.n)
    {
        Msg("~ [motion] %-6s: no samples", site);
        return;
    }

    const float spd_mean = st.spd_sum / float(st.n);
    const float var = _max(0.f, st.spd_sum2 / float(st.n) - spd_mean * spd_mean);
    const float spd_sd = _sqrt(var);
    // ⭐The headline number. Coefficient of variation of the instantaneous
    // speed: 0 is a body gliding, and anything approaching 1 is a body that
    // spends as much time lurching as moving.
    const float cv = spd_mean > EPS ? spd_sd / spd_mean : 0.f;

    Msg("~ [motion] %-6s: n=%u | dt ms min/avg/max %.1f/%.1f/%.1f | step m min/avg/max %.4f/%.4f/%.4f", site, st.n, st.dt_min, st.dt_sum / float(st.n), st.dt_max,
        st.step_min, st.step_sum / float(st.n), st.step_max);
    // ⚠ASCII only in the format string: the build's code page is 1252 and a
    // star here fails the compile, however fine it is in a comment.
    Msg("~ [motion] %-6s: speed m/s mean %.3f sd %.3f min %.3f max %.3f | *** CV %.3f  (0=glide, >0.35=lurching)", site, spd_mean, spd_sd, st.spd_min, st.spd_max, cv);
    Msg("~ [motion] %-6s: turn deg avg %.1f max %.1f | >30deg %u | >90deg %u | BACKSTEPS %u (worst %.0f deg)", site,
        st.turn_sum / float(_max(1u, st.turns_over_30 + st.n)), st.turn_max, st.turns_over_30, st.turns_over_90, st.backsteps, st.backstep_worst);
    // ⭐The three-clock line. Read it before anything else on the "wire" site:
    // it says whether the stamp the client paces by is telling the truth.
    Msg("~ [motion] %-6s: *** clocks: clock stalls %u/%u | stamp stalls %u/%u | worst stamp-vs-wall drift %.0f ms", site, st.clock_stalls, st.n, st.stamp_stalls,
        st.stamp_pairs, st.stamp_lag_max);
    // ⭐The heading, not the direction of travel -- see yaw_step_max.
    Msg("~ [motion] %-6s: *** heading: worst step %.1f deg in one sample, worst rate %.2f rad/s", site, st.yaw_step_max, st.yaw_rate_max);
}

// Where the two sites DISAGREE. Matching them by nearest wall time, because
// they are sampled at different points in the same frame.
void report_divergence()
{
    u32 pairs = 0, mismatched = 0;
    float worst = 0.f;
    u32 worst_ms = 0;

    const Sample* last_assert = nullptr;
    for (const Sample& s : s_samples)
    {
        if (0 == xr_strcmp(s.site, "assert"))
        {
            last_assert = &s;
            continue;
        }
        if (0 != xr_strcmp(s.site, "export") || !last_assert)
            continue;
        // same frame only
        if (s.wall_ms - last_assert->wall_ms > 40)
            continue;

        ++pairs;
        const float d = s.pos.distance_to(last_assert->pos);
        if (d > 0.02f)
        {
            ++mismatched;
            if (d > worst)
            {
                worst = d;
                worst_ms = s.wall_ms;
            }
        }
    }

    if (!pairs)
    {
        Msg("~ [motion] divergence: no assert/export pairs in the same frame");
        return;
    }
    Msg("~ [motion] *** assert vs export: %u pairs, %u differ by >2cm (%.1f%%), worst %.3f m at t=%u", pairs, mismatched, 100.f * float(mismatched) / float(pairs), worst,
        worst_ms);
    if (mismatched)
        Msg("~ [motion] !! the wire does NOT carry what the drive asserted -- look upstream of the network before touching the client");
}

// The raw stream, capped: the summary says WHETHER the motion is ragged, the
// samples say WHERE -- and a number without the sequence that produced it is
// the kind of evidence that gets misread.
//
// ⭐Prints all three clocks as DELTAS side by side, because that is the only
// form in which they convict anybody. On a healthy packet dw, ds and dst are
// the same number.
void report_raw(const char* site, u32 cap)
{
    u32 printed = 0;
    const Sample* prev = nullptr;
    for (const Sample& s : s_samples)
    {
        if (0 != xr_strcmp(s.site, site))
            continue;
        if (prev && printed < cap)
        {
            Fvector d;
            d.sub(s.pos, prev->pos);
            d.y = 0.f;
            const float dt = float(s.wall_ms - prev->wall_ms);
            Msg("~ [motion] raw %-6s t=%u dw=%.0f ds=%d dst=%d step=%.4f spd=%.3f pos=(%.2f,%.2f) yaw=%.3f driven=%d", site, s.wall_ms, dt,
                int(s.server_ms - prev->server_ms), int(s.stamp_ms - prev->stamp_ms), d.magnitude(), dt > 0.f ? d.magnitude() / (dt / 1000.f) : 0.f, s.pos.x, s.pos.z,
                s.yaw, s.driven ? 1 : 0);
            ++printed;
        }
        prev = &s;
    }
}
} // namespace

void SVMotionAudit::Note(u16 id, const char* site, const Fvector& pos, float yaw, bool driven, u32 stamp)
{
    if (!s_active || id != s_id)
        return;

    std::lock_guard<std::recursive_mutex> guard(s_lock);
    if (s_samples.size() >= k_max_samples)
        return;

    Sample s;
    s.wall_ms = Device.dwTimeGlobal;
    s.server_ms = g_pGameLevel ? Level().timeServer() : 0;
    s.stamp_ms = stamp;
    s.pos = pos;
    s.yaw = yaw;
    s.driven = driven;
    xr_strcpy(s.site, site);
    s_samples.push_back(s);
}

void SVMotionAudit::Tick() {}

void SVMotionAudit::Start(u16 id)
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    s_samples.clear();
    s_id = id;
    s_active = true;
    Msg("~ [motion] recording %u -- sv_motion_audit 0 to stop and report", id);
}

void SVMotionAudit::Stop()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    if (!s_active)
    {
        Msg("~ [motion] not recording");
        return;
    }
    s_active = false;

    Msg("~ [motion] ================ audit of %u: %u samples ================", s_id, u32(s_samples.size()));
    // ⭐First, because it is the only site that records a creature nobody is
    // driving and nobody is watching -- and because a report whose other three
    // sites are all empty used to read as "no defect" when it meant "not
    // measured". See the call site in CCustomMonster::UpdateCL.
    report_site("body");
    report_site("assert");
    report_site("export");
    // ⭐The one the client actually has to interpolate between -- see the call
    // site in xrServer::write_entity_update for why "export" is not it.
    report_site("wire");
    report_divergence();

    // ⭐"wire" first and with the bigger cap: it is the sequence the client
    // actually has to interpolate between. "export" is kept, smaller, because
    // the two disagreeing is itself a finding -- but it samples at frame rate
    // and reading it as the wire is the mistake this site exists to prevent.
    report_raw("wire", 250);
    report_raw("export", 60);
    // Raw body samples too: with no client connected this is the ONLY sequence
    // there is, and the jerk being hunted has to be visible in it or it is not
    // coming from the server at all.
    report_raw("body", 120);

    s_samples.clear();
}

void SVMotionAudit::Reset()
{
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    s_active = false;
    s_samples.clear();
}

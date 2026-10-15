////////////////////////////////////////////////////////////////////////////
//  How smooth is a creature ON SCREEN, as a number.
//
//  ⭐WHY BONES AND NOT POSITIONS.
//
//  "Дёргается" is a statement about what the eye sees, and what the eye sees
//  is the final bone transforms -- not the object's position, not the wire,
//  not which animation was selected. A creature can have a perfectly smooth
//  position and still read as broken because its legs restart, and it can have
//  a clean animation and still read as broken because the body it is played on
//  lurches. Sampling the bones catches both, because the bones are downstream
//  of everything.
//
//  ⭐AND WHY THE ANSWER IS A HISTOGRAM OF INTERVALS.
//
//  The single most useful fact about a stutter is its PERIOD, because the
//  period names the culprit outright:
//
//     ~16 ms  -> per frame (something recomputed every frame disagrees)
//     ~50 ms  -> the server's build/export rate
//     ~100 ms -> the AI's schedule push
//     ~200 ms -> the posture poll
//     ~500 ms+-> a replan or a debounce
//
//  Two sessions were spent guessing between exactly these. Measuring the
//  interval between jerks answers it in one run.
//
//  The comparison this exists to serve: run it on an ordinary offline NPC,
//  then on a networked puppet, and read the two reports side by side. Same
//  metric, same units, no opinion.
//
//  Console:
//     cl_bone_audit <id>   record that creature (0 = stop and report)
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "cl_bone_audit.h"
#include "Level.h"
#include "GameObject.h"
#include "../Include/xrRender/Kinematics.h"
#include "../xr_3da/bone.h" // BI_NONE
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager.h"

namespace
{
// Bones worth watching: the body as a whole, the part a walk cycle drives, and
// the extremities where a restarted blend shows first.
const char* k_bone_names[] = {"bip01_pelvis", "bip01_spine", "bip01_head", "bip01_r_hand", "bip01_l_foot", "bip01_r_foot"};
constexpr u32 k_bones = sizeof(k_bone_names) / sizeof(k_bone_names[0]);
constexpr u32 k_max_frames = 20000;

struct BoneFrame
{
    u32 ms;
    float dt;
    Fvector obj; // the object transform, for reference
    Fvector bone[k_bones];
    // ⭐⭐The same bones WITHOUT the object transform, i.e. the pose alone.
    //
    // A world-space bone is pose*root, and the two fail in different ways: a
    // root that lurches drags every bone with it, while a pose that refreshes
    // slower than the root moves reads as the body gliding and the limbs
    // snapping to catch up. In the combined series they are indistinguishable,
    // and the first two recordings of this hunt could not tell them apart --
    // which is the whole reason the vibration survived a night of measuring.
    Fvector loc[k_bones];
};

bool s_active = false;
u16 s_id = 0;
u16 s_bone_id[k_bones];
xr_vector<BoneFrame> s_frames;

struct Series
{
    // per-frame speed of this bone in world space
    float v_mean{0.f}, v_sd{0.f}, v_max{0.f};
    // acceleration: the derivative of that. A jerk IS an acceleration spike.
    float a_mean{0.f}, a_sd{0.f}, a_max{0.f};
    u32 spikes{0}; // |a| beyond mean + 3 sd
    float spikes_per_sec{0.f};
};

// Inter-spike intervals, bucketed by the periods that mean something here.
struct IntervalHisto
{
    u32 f16{0}, f50{0}, f100{0}, f200{0}, f500{0}, other{0};
    void add(float ms)
    {
        if (ms < 30.f)
            ++f16;
        else if (ms < 75.f)
            ++f50;
        else if (ms < 150.f)
            ++f100;
        else if (ms < 350.f)
            ++f200;
        else if (ms < 800.f)
            ++f500;
        else
            ++other;
    }
};

// Which of the three series a pass is looking at. Same statistics, same units,
// so the three lines can be read against each other directly.
enum ESpace
{
    space_world = 0, // pose carried by the root: what the eye actually sees
    space_pose = 1,  // the skeleton alone, root motion removed
    space_root = 2,  // the object transform alone, pose removed
};

IC const Fvector& sample(const BoneFrame& f, u32 b, ESpace sp)
{
    return (space_world == sp) ? f.bone[b] : ((space_pose == sp) ? f.loc[b] : f.obj);
}

void analyse(u32 b, ESpace sp, Series& s, IntervalHisto& h)
{
    const u32 n = u32(s_frames.size());
    if (n < 8)
        return;

    // Speeds and accelerations carry their own timestamps: deriving twice off
    // a variable frame time and then indexing back into the frame array by
    // guesswork is how a measurement quietly starts describing the wrong
    // moment. v[i] and a[i] each know the millisecond they belong to.
    xr_vector<float> v, a, a_ms;
    v.reserve(n);
    a.reserve(n);
    a_ms.reserve(n);

    xr_vector<float> v_ms;
    v_ms.reserve(n);
    for (u32 i = 1; i < n; ++i)
    {
        const float dt = s_frames[i].dt;
        if (dt <= EPS)
            continue;
        v.push_back(sample(s_frames[i], b, sp).distance_to(sample(s_frames[i - 1], b, sp)) / dt);
        v_ms.push_back(float(s_frames[i].ms));
    }
    for (u32 i = 1; i < v.size(); ++i)
    {
        const float dt = (v_ms[i] - v_ms[i - 1]) / 1000.f;
        if (dt <= EPS)
            continue;
        a.push_back(_abs(v[i] - v[i - 1]) / dt);
        a_ms.push_back(v_ms[i]);
    }
    if (v.empty() || a.empty())
        return;

    auto stats = [](const xr_vector<float>& x, float& mean, float& sd, float& mx) {
        double sum = 0, sum2 = 0;
        mx = 0.f;
        for (float f : x)
        {
            sum += f;
            sum2 += double(f) * f;
            mx = _max(mx, f);
        }
        mean = float(sum / double(x.size()));
        const double var = sum2 / double(x.size()) - double(mean) * mean;
        sd = float(_sqrt(_max(0.0, var)));
    };

    stats(v, s.v_mean, s.v_sd, s.v_max);
    stats(a, s.a_mean, s.a_sd, s.a_max);

    // A spike is an acceleration well outside this creature's own normal --
    // deliberately relative, so a fast creature is not judged by a slow one's
    // scale and the metric is comparable between single player and the wire.
    const float thr = s.a_mean + 3.f * s.a_sd;
    float last_ms = -1.f;
    for (u32 i = 0; i < a.size(); ++i)
    {
        if (a[i] <= thr)
            continue;
        ++s.spikes;
        if (last_ms >= 0.f)
            h.add(a_ms[i] - last_ms);
        last_ms = a_ms[i];
    }
    const float span_ms = a_ms.back() - a_ms.front();
    if (span_ms > 0.f)
        s.spikes_per_sec = float(s.spikes) * 1000.f / span_ms;
}
} // namespace

void ClBoneAudit::Frame_()
{
    if (!s_active)
        return;
    if (s_frames.size() >= k_max_frames)
        return;

    CObject* O = Level().Objects.net_Find(s_id);
    if (!O || O->getDestroy())
        return;
    IKinematics* K = smart_cast<IKinematics*>(O->Visual());
    if (!K)
        return;

    BoneFrame f;
    f.ms = Device.dwTimeGlobal;
    f.dt = Device.fTimeDelta;
    f.obj = O->XFORM().c;
    for (u32 i = 0; i < k_bones; ++i)
    {
        if (BI_NONE == s_bone_id[i])
        {
            f.bone[i] = f.obj;
            f.loc[i].set(0.f, 0.f, 0.f); // no bone: a constant, so it adds no pose motion
            continue;
        }
        // world space: the bone's render transform carried by the object's own
        const Fmatrix& L = K->LL_GetTransform(s_bone_id[i]);
        f.loc[i] = L.c;
        Fmatrix m;
        m.mul_43(O->XFORM(), L);
        f.bone[i] = m.c;
    }
    s_frames.push_back(f);
}

// Which creatures are here, how far, and how fast they are going.
//
// ⭐Exists so a measurement never has to NAME a creature. Ids shift with
// whatever spawned alongside, and a guessed id measures the wrong thing
// silently -- a lesson this project has already paid for. A harness reads this
// list and picks a subject by the property it actually cares about, which for
// smoothness work is "is it moving".
//
// ⭐⭐And it is what makes the single-player REFERENCE reachable at all: in a
// loaded save there is no console command that knows which stalker is which,
// so without this there is no way to point the audit at one from a script.
void ClBoneAudit::List()
{
    if (!g_pGameLevel)
        return;

    Fvector eye = Device.vCameraPosition;
    u32 n = 0;
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O || O->getDestroy())
            continue;
        CAI_Stalker* S = smart_cast<CAI_Stalker*>(O);
        if (!S || !S->g_Alive())
            continue;

        const float dist = eye.distance_to(S->Position());
        if (dist > 80.f)
            continue;

        ++n;
        Msg("~ [bone] stalker id=%u '%s' dist=%.1f speed=%.2f", S->ID(), S->cName().c_str(), dist, S->movement().speed());
    }
    Msg("~ [bone] %u alive stalkers within 80 m", n);
}

void ClBoneAudit::Start(u16 id)
{
    s_frames.clear();
    s_id = id;

    CObject* O = Level().Objects.net_Find(id);
    IKinematics* K = O ? smart_cast<IKinematics*>(O->Visual()) : nullptr;
    if (!K)
    {
        Msg("! [bone] %u: no skeleton -- nothing to record", id);
        return;
    }
    for (u32 i = 0; i < k_bones; ++i)
    {
        s_bone_id[i] = K->LL_BoneID(k_bone_names[i]);
        if (BI_NONE == s_bone_id[i])
            Msg("~ [bone] '%s' not on this skeleton -- that slot will track the object instead", k_bone_names[i]);
    }
    s_active = true;
    Msg("~ [bone] recording %u '%s' -- cl_bone_audit 0 to stop and report", id, O->cName().c_str());
}

void ClBoneAudit::Stop()
{
    if (!s_active)
    {
        Msg("~ [bone] not recording");
        return;
    }
    s_active = false;

    Msg("~ [bone] ============ audit of %u: %u frames ============", s_id, u32(s_frames.size()));
    if (s_frames.size() < 8)
    {
        Msg("~ [bone] too few frames");
        s_frames.clear();
        return;
    }

    // ⭐⭐⭐How often did the SKELETON actually advance?
    //
    // CKinematics::CalculateBones takes a "slow update" early-out and leaves the
    // pose untouched for UCalc_Interval (100 ms) unless somebody asks for an
    // exact recompute; the renderer does ask, but only for what it is drawing.
    // So the root can move every frame while the pose refreshes ten times a
    // second, and the difference between those two rates is visible as
    // vibration. This measures the pose rate directly instead of inferring it:
    // a frame counts as a refresh when ANY watched bone moved at all.
    //
    // ⚠It is also the guard on this whole instrument. A creature that is not
    // being drawn has a frozen pose, and every number below would then be
    // describing the camera rather than the creature -- so the share is printed
    // whether it is interesting or not.
    {
        u32 refreshed = 0;
        float prev_ms = -1.f, sum = 0.f, mn = 1e9f, mx = 0.f;
        for (u32 i = 1; i < s_frames.size(); ++i)
        {
            bool moved = false;
            for (u32 b = 0; b < k_bones && !moved; ++b)
                moved = !s_frames[i].loc[b].similar(s_frames[i - 1].loc[b], EPS_S);
            if (!moved)
                continue;
            ++refreshed;
            const float t = float(s_frames[i].ms);
            if (prev_ms >= 0.f)
            {
                const float d = t - prev_ms;
                sum += d;
                mn = _min(mn, d);
                mx = _max(mx, d);
            }
            prev_ms = t;
        }
        const u32 total = u32(s_frames.size()) - 1;
        const float share = total ? 100.f * float(refreshed) / float(total) : 0.f;
        if (refreshed > 1)
            Msg("~ [bone] *** pose refresh: %u of %u frames advanced the skeleton (%.0f%%), interval ms min/avg/max %.0f/%.1f/%.0f -> %.1f Hz", refreshed, total, share,
                mn, sum / float(refreshed - 1), mx, 1000.f * float(refreshed - 1) / _max(sum, 1.f));
        else
            Msg("~ [bone] *** pose refresh: %u of %u frames -- the skeleton never advanced (off screen?)", refreshed, total);
    }

    IntervalHisto all;
    for (u32 b = 0; b < k_bones; ++b)
    {
        Series s;
        IntervalHisto h;
        analyse(b, space_world, s, h);
        all.f16 += h.f16;
        all.f50 += h.f50;
        all.f100 += h.f100;
        all.f200 += h.f200;
        all.f500 += h.f500;
        all.other += h.other;

        Msg("~ [bone] %-14s v m/s mean %.3f sd %.3f max %.3f | accel mean %.1f sd %.1f max %.1f | spikes %u (%.2f/s)", k_bone_names[b], s.v_mean, s.v_sd, s.v_max,
            s.a_mean, s.a_sd, s.a_max, s.spikes, s.spikes_per_sec);
    }

    // The split. Read the three together: if root is clean and pose is not, the
    // fault is in the animation or its refresh rate; if root is dirty, no
    // animator can save it and the wire or the interpolator is to blame.
    {
        Series r;
        IntervalHisto rh;
        analyse(0, space_root, r, rh);
        Msg("~ [bone] %-14s v m/s mean %.3f sd %.3f max %.3f | accel mean %.1f sd %.1f max %.1f | spikes %u (%.2f/s)", "ROOT(xform)", r.v_mean, r.v_sd, r.v_max, r.a_mean,
            r.a_sd, r.a_max, r.spikes, r.spikes_per_sec);

        for (u32 b = 0; b < k_bones; ++b)
        {
            Series s;
            IntervalHisto h;
            analyse(b, space_pose, s, h);
            string64 nm;
            xr_sprintf(nm, "POSE %s", k_bone_names[b] + 6); // drop the "bip01_" prefix: the column is narrow
            Msg("~ [bone] %-14s v m/s mean %.3f sd %.3f max %.3f | accel mean %.1f sd %.1f max %.1f | spikes %u (%.2f/s)", nm, s.v_mean, s.v_sd, s.v_max, s.a_mean, s.a_sd,
                s.a_max, s.spikes, s.spikes_per_sec);
        }
    }

    // The headline: WHEN the jerks happen relative to each other.
    Msg("~ [bone] *** jerk period histogram: ~16ms(frame)=%u  ~50ms(build)=%u  ~100ms(AI push)=%u  ~200ms(posture poll)=%u  ~500ms(replan)=%u  longer=%u", all.f16,
        all.f50, all.f100, all.f200, all.f500, all.other);
    Msg("~ [bone] read it as: the fullest bucket names the rate of whatever is causing the stutter");

    s_frames.clear();
}

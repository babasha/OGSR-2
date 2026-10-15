#include "stdafx.h"

#include "SoundRender_Emitter.h"
#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include "SoundRender_TargetA.h"

extern u32 psSoundModel;
extern float psSoundVEffects;

void CSoundRender_Emitter::set_position(const Fvector& pos)
{
    if (source()->channels_num() == 1 && _valid(pos))
        p_source.position = pos;
    else
        p_source.position.set(0, 0, 0);

    bMoved = TRUE;
}
// Перемотка звука на заданную секунду [rewind snd to target time] --#SM+#--
void CSoundRender_Emitter::set_time(float t)
{
    if (t < 0.0f)
        t = 0.0f;
    R_ASSERT2(get_length_sec() >= t, "set_time: time is bigger than length of sound");
    fTimeToRewind = t;
}

CSoundRender_Emitter::CSoundRender_Emitter(void)
{
    target = NULL;
    //.	source						= NULL;
    owner_data = NULL;
    smooth_volume = 1.f;
    occluder_volume = 1.f;
    fade_volume = 1.f;
    occluder[0].set(0, 0, 0);
    occluder[1].set(0, 0, 0);
    occluder[2].set(0, 0, 0);
    m_current_state = stStopped;
    set_cursor(0);
    bMoved = TRUE;
    b2D = FALSE;
    bStopping = FALSE;
    bRewind = FALSE;
    iPaused = 0;
    fTimeStarted = 0.0f;
    fTimeToStop = 0.0f;
    fTimeToPropagade = 0.0f;
    fTimeToRewind = 0.0f; //--#SM+#--
    marker = 0xabababab;
    starting_delay = 0.f;
    priority_scale = 1.f;
    m_cur_handle_cursor = 0;
}

CSoundRender_Emitter::~CSoundRender_Emitter(void)
{
    // try to release dependencies, events, for example
    Event_ReleaseOwner();
}

//////////////////////////////////////////////////////////////////////
void CSoundRender_Emitter::Event_ReleaseOwner()
{
    if (!(owner_data))
        return;

    for (u32 it = 0; it < SoundRender->s_events.size(); it++)
    {
        if (owner_data == SoundRender->s_events[it].first)
        {
            SoundRender->s_events.erase(SoundRender->s_events.begin() + it);
            it--;
        }
    }
}

void CSoundRender_Emitter::Event_Propagade()
{
    fTimeToPropagade += ::Random.randF(s_f_def_event_pulse - 0.030f, s_f_def_event_pulse + 0.030f);
    if (!(owner_data))
        return;
    if (0 == owner_data->g_type)
        return;
    if (0 == owner_data->g_object)
        return;
    if (0 == SoundRender->Handler)
        return;

    VERIFY(_valid(p_source.volume));
    // Calculate range
    float clip = p_source.max_ai_distance * p_source.volume;
    float range = _min(p_source.max_ai_distance, clip);
    if (range < 0.1f)
        return;

    // Inform objects
    SoundRender->s_events.push_back(mk_pair(owner_data, range));
}

void CSoundRender_Emitter::switch_to_2D()
{
    b2D = TRUE;
    set_priority(100.f);
}

void CSoundRender_Emitter::switch_to_3D() { b2D = FALSE; }

u32 CSoundRender_Emitter::play_time()
{
    if (m_current_state == stPlaying || m_current_state == stPlayingLooped || m_current_state == stSimulating || m_current_state == stSimulatingLooped)
        return iFloor((SoundRender->fTimer_Value - fTimeStarted) * 1000.0f);
    else
        return 0;
}

#include "SoundRender_Source.h"
void CSoundRender_Emitter::set_cursor(u32 p)
{
    m_stream_cursor = p;

    if (owner_data._get() && owner_data->fn_attached[0].size())
    {
        u32 bt = ((CSoundRender_Source*)owner_data->handle)->dwBytesTotal;
        if (m_stream_cursor >= m_cur_handle_cursor + bt)
        {
            SoundRender->i_destroy_source((CSoundRender_Source*)owner_data->handle);
            owner_data->handle = SoundRender->i_create_source(owner_data->fn_attached[0].c_str());
            owner_data->fn_attached[0] = owner_data->fn_attached[1];
            owner_data->fn_attached[1] = "";
            m_cur_handle_cursor = get_cursor(true);

            if (target)
                target->source_changed();
        }
    }
}

u32 CSoundRender_Emitter::get_cursor(bool b_absolute) const
{
    if (b_absolute)
        return m_stream_cursor;
    else
    {
        VERIFY(m_stream_cursor - m_cur_handle_cursor >= 0);
        return m_stream_cursor - m_cur_handle_cursor;
    }
}

void CSoundRender_Emitter::move_cursor(int offset) { set_cursor(get_cursor(true) + offset); }

// ⭐snd_occ_mode 1 (08-10-2026). The old occlusion was one ray to a random point 0.2 m around the source: any hit
// made the sound x0.5 and nothing else, so a sound behind a wall was the same sound, quieter -- and a sound behind
// a bush was as blocked as one behind a hill. Now four paths around the source (a body is not a point), each
// through the MATERIALS it crosses, and what is blocked loses its highs (the target's low-pass) more than its
// level: through a wall you hear the boom, not the crack. One path is refreshed per update (snd_occ_rays), so it
// costs what the old ray did.
#include "../COMMON_AI/ai_sounds.h"

namespace
{
// the 4 sampled points: (right, up) from the source. ⚠08-10-2026 at 0.3 m up the rays to an NPC's footsteps
// grazed the field's bumps and the ground at his feet: steps 15-25 m off in the open were "fully blocked" and the
// way round pulled them 6-13 m aside ("they run along a corridor"). A body's sound comes from a body.
constexpr float occ_offsets[CSoundRender_Emitter::occ_paths][2] = {{0.f, 0.8f}, {0.6f, 0.8f}, {-0.6f, 0.8f}, {0.f, 1.6f}};

IC float lerp_f(float a, float b, float t) { return a + (b - a) * t; }

// OpenAL's distance law as the targets set it up (inverse clamped: REF = min distance, ROLLOFF = snd_rolloff)
IC float al_att(float dist, float ref)
{
    ref = _max(ref, 0.1f);
    return dist <= ref ? 1.f : ref / (ref + psSoundRolloff * (dist - ref));
}

// listener -> P as a unit direction and a length
IC float dir_to(Fvector& dir, const Fvector& from, const Fvector& to)
{
    dir.sub(to, from);
    const float m = dir.magnitude();
    if (m > EPS_L)
        dir.div(m);
    else
        dir.set(0.f, 0.f, 1.f);
    return m;
}
} // namespace

void CSoundRender_Emitter::occ_rays_refresh(u32 count)
{
    const Fvector& L = SoundRender->listener_position();
    const Fvector& P = p_source.position;
    Fvector to;
    if (dir_to(to, L, P) < 1.f)
    {
        // at the listener's ear: nothing between
        for (float& t : occ_path_T)
            t = 1.f;
        return;
    }
    Fvector right;
    right.crossproduct(to, Fvector{0.f, 1.f, 0.f});
    if (right.square_magnitude() < 1e-4f)
        right.set(1.f, 0.f, 0.f);
    right.normalize();

    // 12-10-2026 (Switch, 12-bandit fight: the occlusion rays were a third of the sound update, the second thread's).
    // A burst of fire starts a new source at the gun's muzzle every shot, each with all four paths at once: one that
    // starts where another started a moment ago, the listener where he was, takes that one's paths.
    struct Snap
    {
        Fvector P, L;
        float t;
        float T[occ_paths];
    };
    thread_local Snap s_snaps[16]{};
    thread_local u32 s_snap_next = 0;
    const float now = SoundRender->fTimer_Value;
    if (count >= occ_paths)
    {
        for (const Snap& s : s_snaps)
            if (now - s.t < .25f && s.P.distance_to_sqr(P) < _sqr(.3f) && s.L.distance_to_sqr(L) < _sqr(.5f))
            {
                for (u32 i = 0; i < occ_paths; ++i)
                    occ_path_T[i] = s.T[i];
                return;
            }
    }

    // ...but not through the ceiling of the source's own room: the points stay under what is overhead. The same place,
    // the same room: rayed again only when the source has moved
    if (occ_head_v < 0.f || occ_head_P.distance_to_sqr(P) > _sqr(.25f))
    {
        occ_head_v = SoundRender->occ_headroom(P, occ_offsets[occ_paths - 1][1]);
        occ_head_P = P;
    }
    const float headroom = occ_head_v;

    for (u32 k = 0; k < count; ++k)
    {
        const u32 i = occ_path_next;
        occ_path_next = (occ_path_next + 1) % occ_paths;
        Fvector Q;
        Q.mad(P, right, occ_offsets[i][0]);
        Q.y += _min(occ_offsets[i][1], headroom);
        occ_path_T[i] = SoundRender->occ_transmission(L, Q);
    }
    if (count >= occ_paths)
    {
        Snap& s = s_snaps[s_snap_next++ % 16];
        s.P = P;
        s.L = L;
        s.t = now;
        for (u32 i = 0; i < occ_paths; ++i)
            s.T[i] = occ_path_T[i];
    }
}

void CSoundRender_Emitter::occ_update(float dt)
{
    const bool snap = dt < 0.f; // a start: all paths now, no smoothing
    const float now = SoundRender->fTimer_Value;
    const Fvector& L = SoundRender->listener_position();
    Fvector to_src;
    const float D = dir_to(to_src, L, p_source.position);

    // ⭐On a clock, not on every update: the PC updates the sound 200-300 times a second, and a ray per source per
    // update was 15000 rays and as many path queries a second (08-10-2026, Cordon, 86 sources: 0.40 ms an update
    // against the old 0.05). A source that is mixed and near gets one of its 4 paths 20 times a second (all four in
    // 0.2 s), the rest 4 times a second; silence ($no_sound placeholders, zero volume) never.
    if (snap || now >= occ_next)
    {
        const bool silent = (source()->fname.size() && source()->fname[0] == '$') || p_source.base_volume * p_source.volume <= 0.f;
        const bool near_mixed = target && D < 60.f;
        occ_next = now + (near_mixed ? 1.f / 20.f : 1.f / 4.f);
        if (silent)
        {
            for (float& t : occ_path_T)
                t = 1.f;
        }
        else
            occ_rays_refresh(snap ? occ_paths : u32(_max(1, _min(snd_occ_rays, int(occ_paths)))));

        float T = 0.f;
        for (const float t : occ_path_T)
            T += t;
        T /= float(occ_paths);

        // through: all frequencies down to snd_occ_scale, the highs down to snd_occ_hf
        occ_through_gain = lerp_f(psSoundOcclusionScale, 1.f, T);
        occ_through_hf = lerp_f(snd_occ_hf, 1.f, T);

        // ⭐snd_prop: and round it. The game's map knows the walkable way from the listener to the source; if that
        // carries more than the wall lets through, the sound moves toward the opening it comes out of and keeps
        // most of its highs (a corner bends the lows better, but the open path is not a wall).
        occ_w_target = 0.f;
        occ_round_e = 0.f;
        if (snd_prop && !silent && T < .95f && D > 2.f && D < snd_prop_radius * 1.2f)
        {
            float len;
            Fvector exit;
            Fvector to_exit;
            if (SoundRender->prop_query(p_source.position, len, exit) && dir_to(to_exit, L, exit) > .3f)
            {
                len = _max(len, D);
                // what an opening / a corner passes of the open-air sound: the walkable way also runs through
                // doors that are shut, so not much
                constexpr float k_round = .5f;
                const float e_round = k_round * al_att(len, p_source.min_distance);
                const float e_through = occ_through_gain * al_att(D, p_source.min_distance);
                // energies add as powers
                const float p_round = e_round * e_round, p_through = e_through * e_through;
                if (p_round + p_through > 1e-8f)
                    occ_w_target = p_round / (p_round + p_through);
                prop_pos.mad(L, to_exit, len);
                occ_round_e = e_round;
            }
        }
    }

    const float a = snap ? 1.f : _min(1.f, dt * 4.f); // ~0.25 s
    prop_w = lerp_f(prop_w, occ_w_target, a);
    if (prop_w < .01f)
        prop_w = 0.f;

    // both paths reach the ear; OpenAL attenuates by where the source is rendered, so the gain is relative to that
    float gain = occ_through_gain, hf = occ_through_hf;
    if (prop_w > 0.f && occ_round_e > 0.f)
    {
        const float e_through = occ_through_gain * al_att(D, p_source.min_distance);
        const float p_through = e_through * e_through, p_round = occ_round_e * occ_round_e;
        const float total = _sqrt(p_through + p_round);
        const float render_d = render_position().distance_to(L);
        gain = _min(1.f, total / al_att(render_d, p_source.min_distance));
        constexpr float round_hf = .75f;
        hf = (occ_through_hf * p_through + round_hf * p_round) / (p_through + p_round);
    }

    if (snap)
    {
        occluder_volume = gain;
        occluder_hf = hf;
    }
    else
    {
        // ~0.5 s to settle, the speed the old volume_lerp had
        const float s = _min(1.f, dt * 2.f);
        occluder_volume = lerp_f(occluder_volume, gain, s);
        occluder_hf = lerp_f(occluder_hf, hf, s);
    }
    clamp(occluder_volume, 0.f, 1.f);
    clamp(occluder_hf, 0.f, 1.f);
}

Fvector CSoundRender_Emitter::render_position() const
{
    if (b2D || prop_w <= 0.f)
        return p_source.position;
    // the direction swings toward the opening and the distance toward the path's length
    const Fvector& L = SoundRender->listener_position();
    Fvector a, b;
    const float da = dir_to(a, L, p_source.position);
    const float db = dir_to(b, L, prop_pos);
    Fvector d;
    d.lerp(a, b, prop_w);
    if (d.square_magnitude() < 1e-6f)
        d = b;
    d.normalize();
    Fvector out;
    out.mad(L, d, lerp_f(da, db, prop_w));
    return out;
}

bool CSoundRender_Emitter::hdr_driver() const
{
    if (!owner_data)
        return false;
    const u32 t = u32(owner_data->g_type);
    return (t & SOUND_TYPE_WEAPON_SHOOTING) == SOUND_TYPE_WEAPON_SHOOTING || (t & SOUND_TYPE_WORLD_OBJECT_EXPLODING) == SOUND_TYPE_WORLD_OBJECT_EXPLODING;
}

float CSoundRender_Emitter::hdr_weight() const
{
    if (!owner_data || hdr_driver())
        return 0.f;
    if (owner_data->s_type == st_Music)
        return 0.5f;
    const u32 t = u32(owner_data->g_type);
    if (t == 0)
        return b2D ? 0.f : 0.6f; // 2D without a game type: the interface
    if ((t & SOUND_TYPE_WORLD_AMBIENT) == SOUND_TYPE_WORLD_AMBIENT)
        return 1.f;
    if ((t & SOUND_TYPE_MONSTER_TALKING) == SOUND_TYPE_MONSTER_TALKING)
        return 0.3f; // speech stays understandable
    return 0.6f;
}

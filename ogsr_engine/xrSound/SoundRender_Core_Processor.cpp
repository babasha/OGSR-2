#include "stdafx.h"

#include <efx.h>
#include <chrono>

#include "../xrCDB/cl_intersect.h"
#include "SoundRender_Core.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Target.h"
#include "SoundRender_Source.h"

CSoundRender_Emitter* CSoundRender_Core::i_play(ref_sound* S, BOOL _loop, float delay)
{
    VERIFY(S->_p->feedback == 0);
    CSoundRender_Emitter* E = xr_new<CSoundRender_Emitter>();
    S->_p->feedback = E;
    E->start(S, _loop, delay);
    s_emitters.push_back(E);
    return E;
}

void CSoundRender_Core::update(const Fvector& P, const Fvector& D, const Fvector& N) // called on seqFrameMT
{
    u32 it;

    if (0 == bReady)
        return;

    std::scoped_lock m{m_bLocked};

    bLocked = TRUE;
    Timer.time_factor(psSoundTimeFactor); //--#SM+#--
    float new_tm = Timer.GetElapsed_sec();
    fTimer_Delta = new_tm - fTimer_Value;
    float dt_sec = fTimer_Delta;
    fTimer_Value = new_tm;

    s_emitters_u++;

    const auto t_begin = std::chrono::steady_clock::now();

    // [snd prof] (snd_prof <ms>, 10-10-2026): 📏Switch snd1 the update was ~1 ms on average but 18-63 ms in its worst
    // frame every few seconds, the second thread -- and the main thread behind it -- waiting. Which part, by laps.
    enum { P_PROP, P_EMIT_MIXED, P_EMIT_VIRT, P_STREAM, P_START_FILL, P_ENV, P_START_RENDER, P_EVENTS, P_COUNT };
    float prof[P_COUNT]{};
    auto t_lap = t_begin;
    auto lap = [&](int p) {
        const auto now = std::chrono::steady_clock::now();
        prof[p] += std::chrono::duration<float, std::milli>(now - t_lap).count();
        t_lap = now;
    };
    bool prof_rebuilt = false;
    u32 prof_streams = 0;
    g_snd_prof = {};

    // ⭐snd_voices: what does not fit is virtual -- the emitter simulates on (cursor, events) and gets a target back
    // through i_allow_play once it outranks a mixed one. Cutting the cap at run time sends the extra ones there.
    const u32 voices = u32(_min(int(s_targets.size()), _max(snd_voices, 1)));
    for (it = voices; it < s_targets.size(); it++)
        if (CSoundRender_Emitter* E = s_targets[it]->get_emitter())
            E->cancel();

    // snd_prop: the game's walkable paths from where the listener is now, a few times a second
    if (snd_prop && m_propagation && fTimer_Value >= m_prop_next)
    {
        m_propagation->prop_update(P, snd_prop_radius);
        m_prop_next = fTimer_Value + 0.2f;
        dbg.prop_rebuilds++;
        prof_rebuilt = true;
    }
    lap(P_PROP);

    // Firstly update emitters, which are now being rendered
    // Msg	("! update: r-emitters");
    for (it = 0; it < s_targets.size(); it++)
    {
        CSoundRender_Target* T = s_targets[it];
        CSoundRender_Emitter* E = T->get_emitter();
        if (E)
        {
            E->update(dt_sec);
            E->marker = s_emitters_u;
            E = T->get_emitter(); // update can stop itself
            if (E)
                T->priority = E->priority();
            else
                T->priority = -1;
        }
        else
        {
            T->priority = -1;
        }
    }
    lap(P_EMIT_MIXED);

    // Update emmitters
    // Msg	("! update: emitters");
    for (it = 0; it < s_emitters.size(); it++)
    {
        CSoundRender_Emitter* pEmitter = s_emitters[it];
        if (pEmitter->marker != s_emitters_u)
        {
            pEmitter->update(dt_sec);
            pEmitter->marker = s_emitters_u;
        }
        if (!pEmitter->isPlaying())
        {
            // Stopped
            xr_delete(pEmitter);
            s_emitters.erase(s_emitters.begin() + it);
            it--;
        }
    }
    lap(P_EMIT_VIRT);

    // ⚠the first update's dt is minus the time since the manager was made (fTimer_Value is read before
    // Timer.Start) -- ~-350000 s; the new parts get a sane step
    const float dt_safe = _min(_max(dt_sec, 0.f), .25f);
    hdr_update(dt_safe);

    // Get currently rendering emitters
    // Msg	("! update: targets");
    s_targets_defer.clear();
    s_targets_pu++;
    // u32 PU				= s_targets_pu%s_targets.size();
    for (it = 0; it < s_targets.size(); it++)
    {
        CSoundRender_Target* T = s_targets[it];
        if (T->get_emitter())
        {
            // Has emmitter, maybe just not started rendering
            if (T->get_Rendering())
            {
                T->fill_parameters(this);
                T->update();
                prof_streams++;
            }
            else
                s_targets_defer.push_back(T);
        }
    }
    lap(P_STREAM);

    // Commit parameters from pending targets
    if (!s_targets_defer.empty())
    {
        // Msg	("! update: start render - commit");
        s_targets_defer.erase(std::unique(s_targets_defer.begin(), s_targets_defer.end()), s_targets_defer.end());
        for (it = 0; it < s_targets_defer.size(); it++)
            s_targets_defer[it]->fill_parameters(this);
    }
    lap(P_START_FILL);

    // update EAX or EFX
    if (psSoundFlags.test(ss_EAX) && (bEAX || bEFX || bNativeEnv))
    {
        static shared_str curr_env;

        if (bListenerMoved)
        {
            bListenerMoved = FALSE;
            e_zone = get_environment(P);

            if (!curr_env.size() || curr_env != e_zone->name)
            {
                curr_env = e_zone->name;
                MsgDbg("~ current environment sound zone name [%s]", curr_env.c_str());
            }
        }
        e_target = e_zone;

        // ⭐snd_env_auto: most of a level has no hand zone, and there the old code gave the identity -- no reverb
        // at all, in a cellar as in a field. The rays around the listener say what the room is.
        // ⚠Cordon (08-10-2026) is covered wall to wall by hand zones -- open_space, default, mountains -- so "auto
        // where there is no zone" never happened. Those broad outdoor zones are boxes over a whole area, and a house
        // standing in one got the open air's reverb. So: a SPECIFIC zone (room, corridor, underground, hangar...) is
        // the designer's and stays; a broad one stays outdoors and gives way to the rays where they find a roof.
        if (snd_env_auto)
        {
            env_auto_update(P);
            if (snd_env_auto == 2 || e_zone == get_environment_def())
                e_target = &e_auto;
            else if (env_zone_broad(e_zone))
            {
                // only a real room: "in" 0.5..0.85
                float w = (env_in - .5f) / .35f;
                clamp(w, 0.f, 1.f);
                if (w > 0.f)
                {
                    e_blend.lerp(*e_zone, e_auto, w);
                    e_target = &e_blend;
                }
            }
        }

        if (!e_currentPaused)
            e_current.lerp(e_current, *e_target, dt_sec);
        else
            e_current.set_from(*e_target);

        if (bNativeEnv)
            i_native_env(&e_current);
        else if (bEAX)
        {
            i_eax_listener_set(&e_current);
            i_eax_commit_setting();
        }
        else if (bEFX)
        {
            i_efx_listener_set(&e_current);
            bEFX = i_efx_commit_setting();

            if (!bEFX)
            {
                for (u32 it = 0; it < s_targets.size(); it++)
                {
                    CSoundRender_Target* T = s_targets[it];

                    T->alAuxInit(AL_EFFECTSLOT_NULL);

                    T->bEFX = false;
                }

                release_efx_objects();
            }
        }
    }
    else if (bNativeEnv)
        i_native_env(nullptr); // snd_efx off

    // update listener
    update_listener(P, D, N, dt_sec);
    lap(P_ENV);

    // Start rendering of pending targets
    if (!s_targets_defer.empty())
    {
        // Msg	("! update: start render");
        for (it = 0; it < s_targets_defer.size(); it++)
            s_targets_defer[it]->render();
    }
    lap(P_START_RENDER);

    // Events
    update_events();
    lap(P_EVENTS);

    i_commit();

    dbg_update_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t_begin).count();
    static u32 s_prof_printed = 0;
    if (snd_prof > 0 && dbg_update_ms >= float(snd_prof) && s_prof_printed < 500)
    {
        ++s_prof_printed;
        Msg("~ [snd prof] update %.1f ms: prop %.1f%s | mixed emitters %.1f | virtual emitters %.1f (%u total) | streams %.1f (%u) | "
            "start fill %.1f + render %.1f (%u) | env+listener %.1f | events %.1f || inside: open %.1f ms (%u files) decode %.1f ms (%u lines)",
            dbg_update_ms, prof[P_PROP], prof_rebuilt ? " (rebuilt)" : "", prof[P_EMIT_MIXED], prof[P_EMIT_VIRT], u32(s_emitters.size()),
            prof[P_STREAM], prof_streams, prof[P_START_FILL], prof[P_START_RENDER], u32(s_targets_defer.size()), prof[P_ENV], prof[P_EVENTS],
            g_snd_prof.open_ms, g_snd_prof.opens, g_snd_prof.dec_ms, g_snd_prof.lines);
    }
    dbg_report(dt_safe);

    bLocked = FALSE;
}

// ⭐snd_hdr (08-10-2026): a gunshot next to you and the crickets 40 m off played at their own levels, as if the
// ear had no dynamics. Now the loudest shot / explosion sets a level (instant up, snd_hdr_release down) and the
// rest of the mix ducks under it by how much its kind should (hdr_weight: ambience fully, speech a little, the
// shots themselves and the interface not at all). The quiet world comes back as the ringing fades.
void CSoundRender_Core::hdr_update(float dt)
{
    float peak = 0.f;
    if (snd_hdr > 0.f)
    {
        for (CSoundRender_Emitter* E : s_emitters)
        {
            if (!E->owner_data || !E->hdr_driver())
                continue;
            if (E->m_current_state != CSoundRender_Emitter::stPlaying && E->m_current_state != CSoundRender_Emitter::stPlayingLooped &&
                E->m_current_state != CSoundRender_Emitter::stSimulating && E->m_current_state != CSoundRender_Emitter::stSimulatingLooped)
                continue;
            const float loud = E->b2D ? E->smooth_volume : E->smooth_volume * E->att();
            peak = _max(peak, loud);
        }
    }
    clamp(peak, 0.f, 1.f);
    hdr_level = peak > hdr_level ? peak : _max(peak, hdr_level - dt / _max(snd_hdr_release, 0.05f));

    for (CSoundRender_Emitter* E : s_emitters)
    {
        const float target = 1.f - snd_hdr * hdr_level * E->hdr_weight();
        // ducking in ~50 ms, out with the level itself
        const float rate = target < E->hdr_gain ? 20.f : 4.f;
        const float step = _min(1.f, dt * rate);
        E->hdr_gain += (target - E->hdr_gain) * step;
    }
}

// ⭐snd_env_auto (08-10-2026): 32 fixed directions around the listener, snd_env_rays of them re-cast per update
// against the level's static collision. A roof over the head and walls around make a room; the mean distance
// to them makes its size; the size makes the decay (Sabine for a room about that big: RT ~ 0.161 V/(S a),
// V/S ~ d/3, a ~ 0.25 => ~0.2 s per metre). Out in the open the same rays mostly miss: a short faint tail.
void CSoundRender_Core::env_auto_update(const Fvector& P)
{
    static Fvector dirs[env_dirs];
    static bool dirs_ready = false;
    if (!dirs_ready)
    {
        // golden-angle spiral: evenly over the sphere
        for (u32 i = 0; i < env_dirs; ++i)
        {
            const float y = 1.f - (float(i) + .5f) * 2.f / float(env_dirs);
            const float r = _sqrt(_max(0.f, 1.f - y * y));
            const float phi = float(i) * 2.39996323f;
            dirs[i].set(_cos(phi) * r, y, _sin(phi) * r);
        }
        dirs_ready = true;
    }
    if (!geom_MODEL)
        return;
    // on a clock, 30 times a second (the update itself runs at the frame rate: 300 a second on a PC)
    if (env_valid && fTimer_Value < env_next_t)
        return;
    env_next_t = fTimer_Value + 1.f / 30.f;

    constexpr float range = 60.f;
    thread_local CDB::COLLIDER C;
    const u32 n = env_valid ? u32(_max(1, _min(snd_env_rays, int(env_dirs)))) : env_dirs;
    for (u32 k = 0; k < n; ++k)
    {
        const u32 i = env_next;
        env_next = (env_next + 1) % env_dirs;
        C.ray_query(CDB::OPT_ONLYNEAREST, geom_MODEL, P, dirs[i], range);
        float d = range;
        if (C.r_count())
        {
            // foliage, bushes, fences, glass do not make a room: a miss
            const u32 m = geom_MODEL->get_tris()[C.r_begin()->id].material;
            const float t = m < occ_mtl.size() ? occ_mtl[m] : 0.f;
            if (t < .5f)
                d = C.r_begin()->range;
        }
        env_dist[i] = d;
        dbg.env_rays++;
    }
    env_valid = true;

    // ⚠08-10-2026 the first version took ANY hit within 60 m for a wall or a roof: in the open by the Cordon camp
    // the up-rays caught crowns, wires and whatever stood over the listener -- "in" 0.92, a room's reverb on a
    // field ("we fight in a bathroom"). A ceiling is solid and within 12 m overhead; a wall within 30 m.
    constexpr float roof_max = 12.f, wall_max = 30.f;
    u32 walls = 0, up = 0, up_hits = 0;
    float sum = 0.f, nearest = range;
    for (u32 i = 0; i < env_dirs; ++i)
    {
        const float d = env_dist[i];
        if (dirs[i].y > .35f)
        {
            up++;
            up_hits += d < roof_max ? 1 : 0;
        }
        if (d < wall_max)
        {
            walls++;
            sum += d;
            nearest = _min(nearest, d);
        }
    }
    const float enclosure = float(walls) / float(env_dirs);
    const float roof = up ? float(up_hits) / float(up) : 0.f;
    const float mean = walls ? sum / float(walls) : range;
    // a roof is what makes it inside, and only a mostly closed one
    float in = (roof - .3f) / .6f;
    clamp(in, 0.f, 1.f);
    in *= .5f + .5f * enclosure;
    env_in = in;
    env_roof = roof;
    env_encl = enclosure;
    env_mean = mean;

    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    CSoundRender_Environment& E = e_auto;
    E.set_default();
    const float rt_room = _min(7.f, _max(.3f, .2f * mean));
    E.DecayTime = lerp(1.0f, rt_room, in);
    E.Reverb = lerp(-2200.f, 200.f, in); // mB
    E.Reflections = lerp(-2800.f, -1000.f, in) + (nearest < 3.f ? 400.f * (1.f - nearest / 3.f) : 0.f);
    E.ReflectionsDelay = _min(.3f, _max(.002f, 2.f * nearest / 343.f));
    E.ReverbDelay = _min(.1f, _max(.002f, mean / 343.f));
    E.RoomHF = lerp(-1500.f, -250.f, in);
    E.DecayHFRatio = lerp(.5f, .83f, in);
    E.EnvironmentSize = _min(100.f, _max(1.f, mean * 1.5f));
    E.EnvironmentDiffusion = lerp(.3f, 1.f, in);
    E.clamp();
}

bool CSoundRender_Core::env_zone_broad(const CSoundRender_Environment* E)
{
    if (!E || !E->name.size())
        return true;
    const char* n = E->name.c_str();
    for (const char* broad : {"default", "identity", "open_space", "mountains", "forest", "city", "yard", "_engine_default_"})
        if (!xr_strcmp(n, broad))
            return true;
    return false;
}

void CSoundRender_Core::dbg_report(float dt)
{
    dbg_time += dt;
    dbg_hdr_peak = _max(dbg_hdr_peak, hdr_level);
    if (snd_dbg)
    {
        // the duck lasts ~a second: count it while it is on, not only at the report's instant
        u32 ducked = 0;
        for (CSoundRender_Emitter* E : s_emitters)
            ducked += E->hdr_gain < .95f ? 1 : 0;
        dbg_ducked_peak = _max(dbg_ducked_peak, ducked);
    }
    dbg_ms_sum += dbg_update_ms;
    dbg_ms_max = _max(dbg_ms_max, dbg_update_ms);
    dbg_updates++;
    if (dbg_time < 2.f)
        return;

    if (snd_dbg)
    {
        u32 rendering = 0, mixed_cap = u32(_min(int(s_targets.size()), _max(snd_voices, 1))), occluded = 0, round = 0;
        for (CSoundRender_Target* T : s_targets)
            if (T->get_emitter() && T->get_Rendering())
                rendering++;
        u32 ducked = 0;
        for (CSoundRender_Emitter* E : s_emitters)
        {
            if (E->occluder_hf < .9f)
                occluded++;
            if (E->prop_w > .05f)
                round++;
            if (E->hdr_gain < .95f)
                ducked++;
        }
        dbg_ducked_peak = _max(dbg_ducked_peak, ducked);
        const float per_s = 1.f / dbg_time;
        Msg("[snd] voices %u/%u emitters %u | occluded %u round %u | rays occ %.0f/s env %.0f/s | prop q %.0f/s hit %.0f/s rebuild %.1f/s | "
            "update %.3f ms avg %.3f max | hdr %.2f peak %.2f, ducked %u peak %u | room in %.2f (roof %.2f walls %.2f) mean %.1f m decay %.2f s%s [%s]",
            rendering, mixed_cap, u32(s_emitters.size()), occluded, round, dbg.occ_rays * per_s, dbg.env_rays * per_s, dbg.prop_queries * per_s,
            dbg.prop_hits * per_s, dbg.prop_rebuilds * per_s, dbg_ms_sum / float(_max(dbg_updates, 1u)), dbg_ms_max, hdr_level, dbg_hdr_peak, ducked,
            dbg_ducked_peak, env_in, env_roof, env_encl, env_mean,
            e_current.DecayTime,
            (e_target == &e_auto) ? " (auto)" : (e_target == &e_blend ? " (zone+auto)" : (e_zone == get_environment_def() ? " (none)" : " (zone)")),
            e_zone && e_zone->name.size() ? e_zone->name.c_str() : "-");

        if (snd_dbg >= 2)
        {
            const Fvector& L = listener_position();
            for (CSoundRender_Emitter* E : s_emitters)
            {
                if (E->b2D || !E->owner_data || (E->occluder_hf > .9f && E->prop_w < .05f))
                    continue;
                const Fvector rp = E->render_position();
                Msg("[snd]   %s d %.1f | gain %.2f hf %.2f | round %.2f (%.1f m off) | duck %.2f | %s", E->source()->fname.c_str(), E->p_source.position.distance_to(L),
                    E->occluder_volume, E->occluder_hf, E->prop_w, rp.distance_to(E->p_source.position), E->hdr_gain, E->target ? "mixed" : "virtual");
            }
        }
    }

    if (snd_dbg)
    {
        string1024 vb;
        snd_vorbis_report(vb, sizeof(vb), dbg_time);
        Msg("[snd] vorbis on the CPU %s", vb);
    }

    dbg = {};
    dbg_hdr_peak = 0.f;
    dbg_ducked_peak = 0;
    dbg_time = 0.f;
    dbg_ms_sum = 0.f;
    dbg_ms_max = 0.f;
    dbg_updates = 0;
}

static u32 g_saved_event_count = 0;
void CSoundRender_Core::update_events()
{
    g_saved_event_count = s_events.size();
    for (u32 it = 0; it < s_events.size(); it++)
    {
        event& E = s_events[it];
        Handler(E.first, E.second);
    }
    s_events.clear();
}

void CSoundRender_Core::statistic(CSound_stats* dest, CSound_stats_ext* ext)
{
    if (dest)
    {
        dest->_rendered = 0;
        for (u32 it = 0; it < s_targets.size(); it++)
        {
            CSoundRender_Target* T = s_targets[it];
            if (T->get_emitter() && T->get_Rendering())
                dest->_rendered++;
        }
        dest->_simulated = s_emitters.size();
        dest->_cache_hits = cache._stat_hit;
        dest->_cache_misses = cache._stat_miss;
        dest->_events = g_saved_event_count;
        cache.stats_clear();
    }
    if (ext)
    {
        for (u32 it = 0; it < s_emitters.size(); it++)
        {
            CSoundRender_Emitter* _E = s_emitters[it];
            CSound_stats_ext::SItem _I;
            _I._3D = !_E->b2D;
            _I._rendered = !!_E->target;
            _I.params = _E->p_source;
            _I.volume = _E->smooth_volume;
            if (_E->owner_data)
            {
                _I.name = _E->source()->fname;
                _I.game_object = _E->owner_data->g_object;
                _I.game_type = _E->owner_data->g_type;
                _I.type = _E->owner_data->s_type;
            }
            else
            {
                _I.game_object = 0;
                _I.game_type = 0;
                _I.type = st_Effect;
            }
            ext->append(_I);
        }
    }
}

// ⚠The three occlusion queries below run on different threads -- the AI's hearing (get_occlusion_to) and the
// level's static sounds (SoundRenderGetOcculution) on the main one, the emitters on the sound one -- and all of
// them used to share geom_DB, one collider with one result vector. Each thread now has its own.
static CDB::COLLIDER& occ_collider()
{
    thread_local CDB::COLLIDER C;
    return C;
}

float CSoundRender_Core::occ_transmission(const Fvector& from, const Fvector& to)
{
    Fvector dir;
    dir.sub(to, from);
    const float range = dir.magnitude();
    if (range < .5f)
        return 1.f;
    dir.div(range);
    dbg.occ_rays++;

    CDB::COLLIDER& C = occ_collider();
    float T = 1.f;
    if (geom_MODEL)
    {
        // every surface the path ENTERS (a wall is one, its far side faces away): each lets through what its
        // material does, scaled by snd_occ_mtl; 0 = the old "a hit blocks"
        C.ray_query(CDB::OPT_CULL, geom_MODEL, from, dir, range);
        const CDB::TRI* tris = geom_MODEL->get_tris();
        const u32 n = u32(C.r_count());
        const CDB::RESULT* R = C.r_begin();
        for (u32 k = 0; k < n && T > .01f; ++k)
        {
            // the last metre is the source's own floor / ground / the thing it stands in, not something between
            if (R[k].range > range - 1.f)
                continue;
            const u32 m = tris[R[k].id].material;
            const float t = m < occ_mtl.size() ? occ_mtl[m] * snd_occ_mtl : 0.f;
            T *= t;
        }
    }
    if (geom_SOM && T > .01f)
    {
        // the level's sound occluders (level.som), their own factor per face
        C.ray_query(CDB::OPT_CULL, geom_SOM, from, dir, range);
        const u32 n = u32(C.r_count());
        const CDB::RESULT* R = C.r_begin();
        for (u32 k = 0; k < n; ++k)
            T *= *(const float*)&R[k].dummy;
    }
    return T < .01f ? 0.f : T;
}

float CSoundRender_Core::occ_headroom(const Fvector& P, float want)
{
    if (!geom_MODEL)
        return want;
    CDB::COLLIDER& C = occ_collider();
    C.ray_query(CDB::OPT_ONLYNEAREST, geom_MODEL, P, Fvector{0.f, 1.f, 0.f}, want + .2f);
    dbg.occ_rays++;
    return C.r_count() ? _max(0.f, C.r_begin()->range - .2f) : want;
}

bool CSoundRender_Core::prop_query(const Fvector& P, float& path_len, Fvector& exit_point)
{
    if (!m_propagation)
        return false;
    dbg.prop_queries++;
    const bool ok = m_propagation->prop_query(P, path_len, exit_point);
    dbg.prop_hits += ok ? 1 : 0;
    return ok;
}

float CSoundRender_Core::get_occlusion_to(const Fvector& hear_pt, const Fvector& snd_pt, float dispersion)
{
    CDB::COLLIDER& geom_DB = occ_collider();
    float occ_value = 1.f;

    if (0 != geom_SOM)
    {
        // Calculate RAY params
        Fvector pos, dir;
        pos.random_dir();
        pos.mul(dispersion);
        pos.add(snd_pt);
        dir.sub(pos, hear_pt);
        float range = dir.magnitude();
        dir.div(range);

        geom_DB.ray_query(CDB::OPT_CULL, geom_SOM, hear_pt, dir, range);
        u32 r_cnt = u32(geom_DB.r_count());
        CDB::RESULT* _B = geom_DB.r_begin();

        if (0 != r_cnt)
        {
            for (u32 k = 0; k < r_cnt; k++)
            {
                CDB::RESULT* R = _B + k;
                occ_value *= *(float*)&R->dummy;
            }
        }
    }
    return occ_value;
}

float CSoundRender_Core::get_occlusion(Fvector& P, float R, Fvector* occ)
{
    CDB::COLLIDER& geom_DB = occ_collider();
    float occ_value = 1.f;

    // Calculate RAY params
    Fvector base = listener_position();
    Fvector pos, dir;
    float range;
    pos.random_dir();
    pos.mul(R);
    pos.add(P);
    dir.sub(pos, base);
    range = dir.magnitude();
    dir.div(range);

    if (0 != geom_MODEL)
    {
        bool bNeedFullTest = true;
        // 1. Check cached polygon
        float _u, _v, _range;
        if (CDB::TestRayTri(base, dir, occ, _u, _v, _range, true))
            if (_range > 0 && _range < range)
            {
                occ_value = psSoundOcclusionScale;
                bNeedFullTest = false;
            }
        // 2. Polygon doesn't picked up - real database query
        if (bNeedFullTest)
        {
            geom_DB.ray_query(CDB::OPT_ONLYNEAREST, geom_MODEL, base, dir, range);
            if (0 != geom_DB.r_count())
            {
                // cache polygon
                const CDB::RESULT* R = geom_DB.r_begin();
                const CDB::TRI& T = geom_MODEL->get_tris()[R->id];
                const Fvector* V = geom_MODEL->get_verts();
                occ[0].set(V[T.verts[0]]);
                occ[1].set(V[T.verts[1]]);
                occ[2].set(V[T.verts[2]]);
                occ_value = psSoundOcclusionScale;
            }
        }
    }
    if (0 != geom_SOM)
    {
        geom_DB.ray_query(CDB::OPT_CULL, geom_SOM, base, dir, range);
        u32 r_cnt = u32(geom_DB.r_count());
        CDB::RESULT* _B = geom_DB.r_begin();

        if (0 != r_cnt)
        {
            for (u32 k = 0; k < r_cnt; k++)
            {
                CDB::RESULT* R = _B + k;
                occ_value *= *(float*)&R->dummy;
            }
        }
    }
    return occ_value;
}

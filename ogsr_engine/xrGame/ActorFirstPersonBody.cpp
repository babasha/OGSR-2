#include "StdAfx.h"

#include "ActorFirstPersonBody.h"

#include "Actor.h"
#include "Actor_Flags.h"
#include "Inventory.h"
#include "Weapon.h"

using namespace ACTOR_DEFS;

// Flags the SWM tables never key on: they would double the table for no visual
// gain (turning and leaning do not change which leg cycle plays).
static constexpr u32 ANIM_IGNORED_FLAGS = mcTurn | mcLLookout | mcRLookout;

CActorFirstPersonBody::CActorFirstPersonBody() { m_transform.identity(); }

CActorFirstPersonBody::~CActorFirstPersonBody()
{
    if (m_model)
    {
        IRenderVisual* v = m_model->dcast_RenderVisual();
        ::Render->model_Delete(v);
        m_model = nullptr;
    }
    xr_delete(m_ini);
}

void CActorFirstPersonBody::LoadAnimTable(LPCSTR section, AnimTable& dst, shared_str& def)
{
    dst.clear();
    def = nullptr;

    if (!m_ini || !m_ini->section_exist(section))
    {
        Msg("![FPBody] animation table section [%s] is missing", section);
        return;
    }

    for (const auto& [key, value] : m_ini->r_section(section).Data)
    {
        if (xr_strcmp(key.c_str(), "default") == 0)
        {
            def = value;
            continue;
        }

        char* stop{};
        const u32 mask = std::strtoul(key.c_str(), &stop, 10);
        if (stop == key.c_str() || (stop && *stop))
        {
            Msg("![FPBody] [%s]: key '%s' is not a movement bitmask, skipped", section, key.c_str());
            continue;
        }
        dst[mask] = value;
    }
}

void CActorFirstPersonBody::Load(LPCSTR section)
{
    // Own config file rather than a system.ltx section: system.ltx lives inside a
    // packed .db in shipping installs, so adding an #include there would mean
    // overriding the whole file. This keeps the feature self-contained.
    string_path fname;
    FS.update_path(fname, "$game_config$", "actor_legs.ltx");
    if (!FS.exist(fname))
    {
        Msg("![FPBody] '%s' not found -- first-person body disabled", fname);
        return;
    }

    m_ini = xr_new<CInifile>(fname);

    if (!m_ini->section_exist(section))
    {
        Msg("![FPBody] section [%s] is missing in '%s' -- first-person body disabled", section, fname);
        xr_delete(m_ini);
        return;
    }

    m_height = READ_IF_EXISTS(m_ini, r_float, section, "height", m_height);
    m_height_ladder = READ_IF_EXISTS(m_ini, r_float, section, "height_ladder", m_height_ladder);
    m_hide_distance = READ_IF_EXISTS(m_ini, r_float, section, "hide_distance", m_hide_distance);
    m_shift_fwd = READ_IF_EXISTS(m_ini, r_float, section, "shift_fwd", m_shift_fwd);
    m_shift_fwd_noncombat = READ_IF_EXISTS(m_ini, r_float, section, "shift_fwd_noncombat", m_shift_fwd_noncombat);
    m_shift_rgt = READ_IF_EXISTS(m_ini, r_float, section, "shift_rgt", m_shift_rgt);
    m_drop_stand = READ_IF_EXISTS(m_ini, r_float, section, "drop_stand", m_drop_stand);
    m_drop_crouch = READ_IF_EXISTS(m_ini, r_float, section, "drop_crouch", m_drop_crouch);
    m_drop_crouch_accel = READ_IF_EXISTS(m_ini, r_float, section, "drop_crouch_accel", m_drop_crouch_accel);
    m_pin_distance = READ_IF_EXISTS(m_ini, r_float, section, "pin_distance", m_pin_distance);
    m_look_down_threshold = READ_IF_EXISTS(m_ini, r_float, section, "look_down_threshold", m_look_down_threshold);
    m_follow_view = !!READ_IF_EXISTS(m_ini, r_u32, section, "follow_view", m_follow_view ? 1u : 0u);
    m_turn_speed = READ_IF_EXISTS(m_ini, r_float, section, "turn_speed", m_turn_speed);
    m_turn_threshold = READ_IF_EXISTS(m_ini, r_float, section, "turn_threshold", m_turn_threshold);
    m_turn_threshold_down = READ_IF_EXISTS(m_ini, r_float, section, "turn_threshold_down", m_turn_threshold_down);
    m_turn_hold = READ_IF_EXISTS(m_ini, r_u32, section, "turn_hold_ms", m_turn_hold);
    m_turn_anim_left = READ_IF_EXISTS(m_ini, r_string, section, "turn_anim_left", "lancew_legs_turn_left");
    m_turn_anim_right = READ_IF_EXISTS(m_ini, r_string, section, "turn_anim_right", "lancew_legs_turn_right");

    LoadAnimTable(READ_IF_EXISTS(m_ini, r_string, section, "anim_table", "legs_anims_basic"), m_anim_basic, m_anim_basic_default);
    LoadAnimTable(READ_IF_EXISTS(m_ini, r_string, section, "anim_table_noncombat", "legs_anims_noncombat"), m_anim_noncombat, m_anim_noncombat_default);

    // Per-cycle position tweaks: `<cycle> = fwd, rgt` (SWM's SPEC_SHIFT).
    m_spec_shift.clear();
    if (m_ini->section_exist("legs_shifts"))
        for (const auto& [key, value] : m_ini->r_section("legs_shifts").Data)
        {
            Fvector2 s;
            if (sscanf(value.c_str(), "%f,%f", &s.x, &s.y) == 2)
                m_spec_shift[key] = s;
        }

    if (LPCSTR v = READ_IF_EXISTS(m_ini, r_string, section, "visual", nullptr))
        SetVisual(v);
}

void CActorFirstPersonBody::SetVisual(LPCSTR visual_name)
{
    if (!visual_name || !visual_name[0])
        return;
    if (m_visual_name.size() && xr_strcmp(m_visual_name.c_str(), visual_name) == 0)
        return;

    if (m_model)
    {
        IRenderVisual* old = m_model->dcast_RenderVisual();
        ::Render->model_Delete(old);
        m_model = nullptr;
    }

    m_visual_name = visual_name;
    m_current_anim = nullptr;

    m_model = smart_cast<IKinematicsAnimated*>(::Render->model_Create(visual_name));
    if (!m_model)
        Msg("![FPBody] model_Create failed for '%s'", visual_name);
}

const shared_str& CActorFirstPersonBody::PickAnim(u32 mstate, bool calm) const
{
    // SWM's "calm state" set: relaxed strides used only while nothing is readied.
    // It is sparse (idle / crouch / walk), so anything it misses falls through.
    if (calm)
    {
        const auto it = m_anim_noncombat.find(mstate);
        if (it != m_anim_noncombat.end())
            return it->second;
    }

    const auto it = m_anim_basic.find(mstate);
    return (it != m_anim_basic.end()) ? it->second : m_anim_basic_default;
}

void CActorFirstPersonBody::PlayAnim(const shared_str& anim)
{
    if (!m_model || !anim.size() || anim == m_current_anim)
        return;

    m_model->PlayCycle(anim, TRUE);
    m_current_anim = anim;
}

void CActorFirstPersonBody::OnFrame(CActor* actor)
{
    m_visible = false;

    // Hidden for any reason: forget the smoothed heading and any held step-turn, so
    // coming back (third person, a vehicle, a death cam) starts aligned with the
    // camera instead of turning across whatever angle accrued while unseen.
    const auto hide = [this]() {
        m_heading_valid = false;
        m_turn_until = 0;
        m_pin_relock = 0;
    };

    if (!psActorFlags.test(AF_FIRST_PERSON_BODY))
    {
        hide();
        return;
    }
    if (!actor || !m_model)
    {
        hide();
        return;
    }
    // Third person already draws the real actor model; death hands the camera off.
    if (actor->active_cam() != eacFirstEye || !actor->g_Alive())
    {
        hide();
        return;
    }

    const Fvector& cam_pos = Device.vCameraPosition;
    const float dist_to_actor = cam_pos.distance_to(actor->Position());
    if (dist_to_actor > m_hide_distance)
    {
        hide();
        return;
    }

    // --- which cycle -------------------------------------------------------
    // ⚠TWO masks on purpose. The animation TABLE is keyed on a mask with the
    // ignored bits stripped (SWM's DISABLED_FLAGS), but the heading and step-turn
    // rules below are keyed on mcTurn / the lookouts -- exactly the bits that get
    // stripped. Testing the stripped mask there silently disables both.
    const u32 raw_mstate = actor->MovingState();
    const u32 mstate = raw_mstate & ~ANIM_IGNORED_FLAGS;
    // OGSR has no "weapon lowered" state (a CoP/Anomaly feature), so calm simply
    // means nothing weapon-like is in hand.
    const bool calm = !smart_cast<CWeapon*>(actor->inventory().ActiveItem());
    shared_str anim = PickAnim(mstate, calm);

    // --- where ------------------------------------------------------------
    float shift_fwd = calm ? m_shift_fwd_noncombat : m_shift_fwd;
    float shift_rgt = m_shift_rgt;
    if (const auto it = m_spec_shift.find(anim); it != m_spec_shift.end())
    {
        shift_fwd = it->second.x;
        shift_rgt = it->second.y;
    }

    Fvector pos = cam_pos;

    // Pin the feet to the actor capsule. Only when the camera has been pulled away
    // from the body (cutscene effectors, animated cam) fall back to hanging the body
    // a fixed drop below the camera -- and once that happens, stay unpinned for a
    // second (SWM's posTimer) so a camera hovering on the threshold can't flicker
    // between the two placements every frame.
    const u32 now_ms = Device.dwTimeGlobal;
    if (m_pin_relock < now_ms && dist_to_actor < m_pin_distance)
    {
        pos.y = actor->Position().y;
        m_pin_relock = 0;
    }
    else
    {
        float drop = m_drop_stand;
        if (mstate & mcCrouch)
            drop = (mstate & mcAccel) ? m_drop_crouch_accel : m_drop_crouch;
        pos.y -= drop;
        if (m_pin_relock == 0)
            m_pin_relock = now_ms + 1000;
    }

    pos.y += (mstate & mcClimb) ? m_height_ladder : m_height;

    // --- heading -----------------------------------------------------------
    // ⭐⭐⭐WHY THE BODY MUST NOT BE DRIVEN BY THE ACTOR'S HEADING.
    //
    // SWM drives it that way: looking down, the legs follow db.actor:direction(), so
    // they stay planted while a free head looks around. On OGSR that reads as broken,
    // because the actor only realigns after the camera has run 45 deg away from it
    // (Actor_Movement.cpp, PI_DIV_4) and then lerps across at PI_MUL_2. Whip the
    // view and the torso arrives a beat later -- there is no beat in first person,
    // the torso IS the view.
    //
    // So: follow the view azimuth, every frame, no rate limit. m_follow_view = 0
    // restores SWM's planted-legs rule (and with it that catch-up), m_turn_speed > 0
    // adds back an explicit rate for anyone who wants some weight.
    //
    // ⭐vCameraTop, not vCameraDirection, once pitched steeply down: the view
    // vector's horizontal part collapses there and getH() goes unstable, while the
    // top vector's horizontal part still points along the view azimuth. This is the
    // one bit of SWM's rule worth keeping unconditionally.
    const bool looking_down = (Device.vCameraDirection.y <= m_look_down_threshold);
    const float view_h = looking_down ? Device.vCameraTop.getH() : Device.vCameraDirection.getH();

    float target = view_h;
    if (!m_follow_view && looking_down && !((raw_mstate & mcTurn) && !(raw_mstate & mcClimb)) && !(raw_mstate & mcLookout))
        target = actor->Direction().getH();

    if (!m_heading_valid || m_turn_speed <= 0.0f)
    {
        m_heading = target;
        m_heading_valid = true;
    }
    else
        angle_lerp(m_heading, target, m_turn_speed, Device.fTimeDelta);

    // --- step-turn cycle ---------------------------------------------------
    // The shuffle-step, kept for flavour but re-keyed. SWM triggers it off body-vs-
    // target, which is always ~0 once the body rides the view; the interesting angle
    // is body-vs-ACTOR -- how far your legs are twisted from where you are looking.
    // That grows to 45 deg, trips mcTurn, and the legs step round to catch up. The
    // step is now purely an animation: it never moves the body, so it cannot lag.
    if (!(raw_mstate & mcAnyMove) && !(raw_mstate & mcClimb))
    {
        if (m_turn_until > now_ms)
        {
            if (m_turn_anim.size())
                anim = m_turn_anim;
        }
        else if (raw_mstate & mcTurn)
        {
            const float thr = (looking_down && !(raw_mstate & mcLookout)) ? m_turn_threshold_down : m_turn_threshold;
            const float twist = angle_normalize_signed(m_heading - actor->Direction().getH());
            if (_abs(twist) > thr)
            {
                // Flip these two if the shuffle ever steps the wrong way -- the sign
                // convention is the only thing that decides it.
                m_turn_anim = (twist >= 0.0f) ? m_turn_anim_right : m_turn_anim_left;
                m_turn_until = now_ms + m_turn_hold;
                if (m_turn_anim.size())
                    anim = m_turn_anim;
            }
        }
    }
    else
        m_turn_until = 0;

    Fvector dir_fwd, dir_rgt;
    dir_fwd.setHP(m_heading, 0.0f);
    dir_rgt.setHP(Device.vCameraRight.getH(), 0.0f);

    pos.mad(dir_fwd, shift_fwd);
    pos.mad(dir_rgt, shift_rgt);

    m_transform.setHPB(m_heading, 0.0f, 0.0f);
    m_transform.c = pos;

    // --- advance the skeleton ---------------------------------------------
    PlayAnim(anim);

    m_model->UpdateTracks();
    IKinematics* K = m_model->dcast_PKinematics();
    K->CalculateBones_Invalidate();
    // bForceExact: CKinematics::CalculateBones has a "slow update" early-out that
    // leaves bones stale for UCalc_Interval ms -- visibly jerky at HUD distance.
    K->CalculateBones(TRUE);

    m_visible = true;
}

void CActorFirstPersonBody::Render(u32 context_id, IRenderable* root)
{
    if (!m_visible || !m_model)
        return;

    // renderable_HUD stays false on purpose: the body belongs to the world pass,
    // so it receives scene lighting, fog and shadows instead of HUD-FOV treatment.
    //
    // ⚠But it must not CAST while the actor's own model is being rasterized into the
    // shadow maps (g_actor_shadow): that model is the full silhouette, arms and all,
    // while this one has no arm bones whatsoever -- so both casting gives you two
    // human shadows 0.65 m apart, one of them armless. The renderer only honours
    // this when the stand-in is really there; with the actor shadow off, an armless
    // shadow still beats no shadow.
    ::Render->set_no_shadow_collect(true);
    ::Render->add_Visual(context_id, root, m_model->dcast_RenderVisual(), m_transform);
    ::Render->set_no_shadow_collect(false);
}

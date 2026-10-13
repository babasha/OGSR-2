#pragma once

#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"

#include "actor_defs.h"

class CActor;

// First-person visible body ("you can see your own legs").
//
// A standalone animated visual -- deliberately NOT a game object: no alife
// entity, no server side, no spatial/AI registration. It is created and owned
// by CActor, glued to the camera every frame, and submitted straight to the
// renderer via ::Render->add_Visual, the same way player_hud submits the hands.
// Because it is submitted with renderable_HUD() cleared it lands in the normal
// dynamic list and picks up world lighting, fog and shadows for free.
//
// The movement-state -> animation-cycle tables live in actor_legs.ltx and are
// generated from Shoker Weapon Mod's swm_legs.script (see tools/legs_table_to_ltx.js).
class CActorFirstPersonBody
{
public:
    CActorFirstPersonBody();
    ~CActorFirstPersonBody();

    // Reads tunables + animation tables. Safe to call again to hot-reload.
    void Load(LPCSTR section = "actor_legs");

    // Swaps the mesh (outfit change). No-op when the name is unchanged.
    void SetVisual(LPCSTR visual_name);

    // Per-frame: decides visibility, places the body relative to the camera and
    // advances the skeleton. Must run before Render().
    void OnFrame(CActor* actor);

    // Submits the visual. Called from CActor::RenderFirstPersonBody.
    void Render(u32 context_id, IRenderable* root);

    bool Visible() const { return m_visible; }

private:
    using AnimTable = xr_map<u32, shared_str>;

    void LoadAnimTable(LPCSTR section, AnimTable& dst, shared_str& def);
    void PlayAnim(const shared_str& anim);
    const shared_str& PickAnim(u32 mstate, bool calm) const;

    CInifile* m_ini{};
    IKinematicsAnimated* m_model{};
    shared_str m_visual_name;
    shared_str m_current_anim;
    Fmatrix m_transform;
    bool m_visible{};

    // Body heading, smoothed toward the target every frame. SWM turns its fake mob
    // with an AI `look` action, which is a RATE, not a snap -- gluing the yaw
    // straight to the target makes the body twitch under you.
    float m_heading{0.0f};
    bool m_heading_valid{false};
    // Step-turn cycle currently held (SWM's turnAnim/lastTurnTime pair).
    shared_str m_turn_anim;
    u32 m_turn_until{0};
    // SWM's posTimer: once the camera has been judged "detached" the feet stay
    // unpinned for a second, so a brush against the threshold can't flicker.
    u32 m_pin_relock{0};

    // Tables. `basic` is always used; `noncombat` overrides it while the actor
    // carries no readied weapon (SWM's "calm state").
    AnimTable m_anim_basic;
    AnimTable m_anim_noncombat;
    shared_str m_anim_basic_default;
    shared_str m_anim_noncombat_default;

    // Per-cycle position overrides (SWM's SPEC_SHIFT): x = forward, y = right.
    xr_map<shared_str, Fvector2> m_spec_shift;

    // Tunables (actor_legs.ltx). Defaults mirror the SWM script constants.
    float m_height{0.0f};
    float m_height_ladder{-0.1f};
    float m_hide_distance{2.0f};
    float m_shift_fwd{-0.65f};
    // SWM's DISTANCE_MOD_FWD_NONCOMBAT: the body sits closer to the camera when
    // nothing is readied, because the calm stance leans back.
    float m_shift_fwd_noncombat{-0.5f};
    float m_shift_rgt{0.0f};
    float m_drop_stand{1.5f};
    float m_drop_crouch{1.23f};
    float m_drop_crouch_accel{0.95f};
    // Camera-to-actor distance under which the feet are pinned to the actor capsule.
    // ⚠SWM uses 1.7, NOT the stand drop: a standing first-person camera sits ~1.65-1.75
    // above the feet, so keying this off drop_stand (1.5) means the pin NEVER engages
    // and the body just hangs off the camera, inheriting every bob and lean.
    float m_pin_distance{1.7f};
    // Below this camera pitch the body follows the actor's body heading instead
    // of the view heading -- otherwise the legs swing around as you look down.
    float m_look_down_threshold{-0.65f};
    // 1 = the body always faces the view azimuth (default). 0 = SWM's rule, where
    // looking down hands the heading to the actor's own body -- which on OGSR only
    // realigns after 45 deg, so the torso visibly arrives after the camera.
    bool m_follow_view{true};
    // Rate limit on the body yaw, rad/s. 0 = snap, and 0 is the default: a rate here
    // is exactly the "torso catches up afterwards" artefact, just self-inflicted.
    float m_turn_speed{0.0f};
    // Yaw error that triggers a step-turn cycle. SWM: 0.2 rad normally, 0.08 while
    // looking down (where a small error is much more visible).
    float m_turn_threshold{0.2f};
    float m_turn_threshold_down{0.08f};
    u32 m_turn_hold{500};   // ms the step-turn cycle is held once started
    shared_str m_turn_anim_left;
    shared_str m_turn_anim_right;
};

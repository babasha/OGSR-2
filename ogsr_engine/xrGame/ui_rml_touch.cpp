#include "StdAfx.h"
#include "ui_rml.h"

// ⭐14-10 (the owner: "touch controls like the mobile RE7 -- a minimum; a tap on the minimap opens the map, taps on the
// quick slots work"). The phone in play: a floating stick on the left part of the screen (it jumps under the thumb,
// pushed past its ring = sprint), a drag anywhere else on the right turns the camera, and a few round buttons --
// fire (a drag on it turns too), aim (a toggle), reload, crouch, use (only while something can be used, named by
// what it does to it), pause; and (the owner, the same evening) the weapon (a tap = the next one, held = the wheel)
// and the inventory.
//
// All of it is one virtual pad (g_touch_pad, xr_3da/IInputReceiver.h) that CInput::PollNxPad merges into the real
// pad's state on its next poll: the buttons land in the Switch's scheme (ZR fire, ZL aim, Y reload / held = the next
// ammo, R3 crouch / held = the deep one, A use, Plus pause, L3 sprint) with every rule it has, a pad plugged into the
// phone keeps working next to the screen, and the look goes in as mouse units, by what the finger moved.
//
// touch.rml only draws: the hit area of a button is its border box there, a little bigger, so the layout lives in one
// place (touch.rcss). The minimap's and the quick slots' boxes come from the HUD document (HudMapView, HudSlotRect).

// windowsx.h (the precompiled header) has these as macros over GetWindow(); Rml::Element declares methods so named
#undef GetNextSibling
#undef GetPreviousSibling
#undef GetPrevSibling
#undef GetFirstChild

#include <RmlUi/Core.h>
#include "../xr_3da/IInputReceiver.h"
#include "../xr_3da/nx/xr_nx_pad.h"
#include "Level.h"
#include "Actor.h"
#include "xr_level_controller.h"
#include "ui/UIPdaAux.h"
#include "InventoryBox.h"
#include "ai_space.h"
#include "script_engine.h"

#ifdef __ANDROID__
int g_touch_controls = 1; // touch_controls (console_commands.cpp)
#else
int g_touch_controls = 0;
#endif
// The look (14-10 evening, the owner: "like CoD Mobile -- now it takes a lot of swiping to turn round or aim"): in
// degrees per INCH the finger moves (the panel's dpi), whatever mouse_sens says; a fast flick turns more (up to
// 1 + touch_look_accel times), aiming gets its own factor on top of the engine's own slowing by the zoom's FOV.
float g_touch_look = 100.f; // touch_look_deg
float g_touch_look_ads = 1.25f; // touch_look_ads
float g_touch_look_accel = 1.f; // touch_look_accel

namespace
{
Rml::ElementDocument* s_doc = nullptr;
bool s_shown = false;
u32 s_next_try = 0;

struct Btn
{
    const char* id;
    u32 bit;
    bool toggle; // a tap flips it (aim), else held while the finger is
    bool look; // a drag on it turns the camera too (fire)
    Rml::Element* el;
};
Btn s_btns[] = {
    {"t-fire", XRPAD_ZR, false, true, nullptr},     {"t-aim", XRPAD_ZL, true, false, nullptr},
    {"t-reload", XRPAD_Y, false, false, nullptr},   {"t-crouch", XRPAD_RSTICK, false, false, nullptr},
    {"t-use", XRPAD_A, false, false, nullptr},      {"t-pause", XRPAD_PLUS, false, false, nullptr},
    // the pad's X: a tap = the next weapon, held = the wheel, which the drag on the button turns (UIRml::WheelMouse)
    {"t-weapon", XRPAD_X, false, true, nullptr},    {"t-inv", XRPAD_MINUS, false, false, nullptr},
};
constexpr int kUse = 4;
const char* s_use_kind = nullptr; // the t-use attribute its text is from now
u32 s_use_seen = 0; // when something to use was last in front: the button outlives a frame or two without it
Rml::Element* s_stick = nullptr;
Rml::Element* s_knob = nullptr;

// The way out of a screen (touch_close.rml): shown over any screen that takes input, a tap on it is the pad's Plus
// (Esc over a screen, xr_3da/xr_input_sdl.cpp) for a few frames.
Rml::ElementDocument* s_close = nullptr;
bool s_close_shown = false;
u32 s_close_try = 0;
int s_close_frames = 0; // Plus held this many frames more
struct CloseListener final : public Rml::EventListener
{
    void ProcessEvent(Rml::Event& ev) override
    {
        if (ev.GetId() == Rml::EventId::Click)
            s_close_frames = 3;
    }
} s_close_listener;

enum class Role
{
    Stick,
    Look,
    Button,
    Map,
    Slot,
};
struct Finger
{
    u32 id;
    Role role;
    int idx; // the button, the quick slot
    Rml::Vector2f start, last;
    u32 t0;
};
xr_vector<Finger> s_fingers;
bool s_aim = false; // the aim toggle
Rml::Vector2f s_anchor; // the stick's centre while a finger holds it
bool s_run = false; // pushed past the ring: sprint was sent, again only after it came back in

Rml::String px(float v) { return Rml::String(std::to_string(int(v + 0.5f)) + "px"); }

bool build()
{
    s_doc = UIRmlInternal::BuildDocument("touch.rml");
    if (!s_doc)
    {
        s_next_try = Device.dwFrame + 600;
        return false;
    }
    for (Btn& b : s_btns)
        b.el = s_doc->GetElementById(b.id);
    s_stick = s_doc->GetElementById("t-stick");
    s_knob = s_doc->GetElementById("t-knob");
    s_doc->Hide();
    s_shown = false;
    Msg("~ [touch] controls built (dpi %.0f)", g_display_dpi);
    return true;
}

bool inside(Rml::Element* e, const Rml::Vector2f& p, float grow)
{
    if (!e || !e->IsVisible(true))
        return false;
    const Rml::Vector2f pos = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
    const float gx = size.x * grow, gy = size.y * grow;
    return p.x >= pos.x - gx && p.x <= pos.x + size.x + gx && p.y >= pos.y - gy && p.y <= pos.y + size.y + gy;
}

// What A does to the thing in front, as the t-use attribute to take the text from; nullptr = nothing to use.
// The actor's own choice (Actor.cpp, the default action), plus the boxes, which it leaves without one.
const char* use_kind(CActor* a)
{
    CGameObject* o = a->ObjectWeLookingAt();
    if (!o)
        return nullptr;
    if (smart_cast<IInventoryBox*>(o))
        return "data-open";
    const char* act = a->GetDefaultActionForObject();
    if (!act || !act[0])
        return nullptr;
    if (!xr_strcmp(act, "character_use"))
        return "data-talk";
    if (!xr_strcmp(act, "dead_character_use") || !xr_strcmp(act, "dead_character_use_or_drag"))
        return "data-search";
    if (!xr_strcmp(act, "car_character_use"))
        return "data-drive";
    if (!xr_strcmp(act, "inventory_item_use") || !xr_strcmp(act, "inventory_item_use_or_drag"))
        return "data-take";
    if (!xr_strcmp(act, "game_object_drag"))
        return "data-drag";
    return "data-use"; // a script object's own tip (doors, switches, ...)
}

bool inside01(const Frect& r, float x, float y) { return x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2; }

float stick_radius() { return s_stick ? std::max(1.f, s_stick->GetBox().GetSize(Rml::BoxArea::Border).x * 0.5f) : 80.f; }

void knob_at(const Rml::Vector2f& off) // off: from the stick's centre, px
{
    if (!s_stick || !s_knob)
        return;
    const float r = stick_radius();
    const float k = s_knob->GetBox().GetSize(Rml::BoxArea::Border).x * 0.5f;
    s_knob->SetProperty("left", px(r + off.x - k));
    s_knob->SetProperty("top", px(r + off.y - k));
}

void stick_place(bool held)
{
    if (!s_stick)
        return;
    s_stick->SetClass("active", held);
    if (held)
    {
        const float r = stick_radius();
        s_stick->SetProperty("left", px(s_anchor.x - r));
        s_stick->SetProperty("top", px(s_anchor.y - r));
        s_stick->SetProperty("bottom", "auto");
        return;
    }
    s_stick->RemoveProperty("left");
    s_stick->RemoveProperty("top");
    s_stick->RemoveProperty("bottom");
    if (s_knob)
    {
        s_knob->RemoveProperty("left");
        s_knob->RemoveProperty("top");
    }
}

void open_map()
{
    // the map key's own way (the 3D PDA, the "no PDA" rules), the 2D map screen where it has no binding
    const int dik = get_action_dik(kMAP);
    if (dik > 0 && g_pGameLevel)
    {
        Level().IR_OnKeyboardPress(dik);
        Level().IR_OnKeyboardRelease(dik);
        return;
    }
    UIRml::PdaOpen(eptMap);
}

void use_slot(int n)
{
    luabind::functor<void> f;
    if (!ai().script_engine().functor("ogsr_quick_slots.use_slot", f))
    {
        static bool s_said = false;
        if (!s_said)
            Msg("! [touch] ogsr_quick_slots.use_slot does not exist: a tap on a quick slot does nothing");
        s_said = true;
        return;
    }
    f(n);
}

void release(const Finger& f, const Rml::Vector2i& dims)
{
    const Rml::Vector2f d = f.last - f.start;
    const float tapR = 0.04f * float(dims.y);
    const bool tap = d.x * d.x + d.y * d.y < tapR * tapR && Device.dwTimeContinual - f.t0 < 600;
    if (f.role == Role::Stick)
    {
        s_run = false;
        stick_place(false);
    }
    else if (f.role == Role::Map && tap)
        open_map();
    else if (f.role == Role::Slot && tap)
        use_slot(f.idx);
}

void drop_all(const Rml::Vector2i& dims)
{
    for (const Finger& f : s_fingers)
        if (f.role == Role::Stick)
            release(f, dims); // the stick only: nothing is tapped on the way out of play
    s_fingers.clear();
    s_aim = false;
    s_run = false;
    stick_place(false);
    for (Btn& b : s_btns)
        if (b.el)
            b.el->SetClass("held", false);
}

Role pick(const Rml::Vector2f& p, const Rml::Vector2i& dims, int& idx)
{
    for (int i = 0; i < int(std::size(s_btns)); ++i)
        if (inside(s_btns[i].el, p, 0.15f))
        {
            idx = i;
            return Role::Button;
        }
    const float x01 = p.x / float(dims.x), y01 = p.y / float(dims.y);
    Frect r;
    if (UIRml::HudMapView(r) && inside01(r, x01, y01))
        return Role::Map;
    for (int n = 1; n <= 4; ++n)
        if (UIRml::HudSlotRect(n, r) && inside01(r, x01, y01))
        {
            idx = n;
            return Role::Slot;
        }
    const bool stickFree = std::none_of(s_fingers.begin(), s_fingers.end(), [](const Finger& f) { return f.role == Role::Stick; });
    if (stickFree && p.x < 0.45f * float(dims.x))
        return Role::Stick;
    return Role::Look;
}

// The px a finger moved this frame -> the mouse units CActor::IR_OnMouseMove turns by: inches (the panel's dpi) times
// touch_look_deg, the speed's gain, the aim's factor, over what one unit turns (mouse_sens).
void look_units(float px_x, float px_y, float& ux, float& uy)
{
    static float s_speed = 0.f; // in/s, smoothed over a few frames: one jumpy sample must not kick the gain
    const float dpi = g_display_dpi > 0.f ? g_display_dpi : 400.f;
    const float in_x = px_x / dpi, in_y = px_y / dpi;
    const float dt = std::max(Device.fTimeDeltaReal, 0.001f);
    const float v = std::sqrt(in_x * in_x + in_y * in_y) / dt;
    s_speed += (v - s_speed) * 0.5f;
    // the gain: 1 below 2 in/s (aiming), rising smoothly to 1 + accel at 10 in/s (a flick)
    float t = std::clamp((s_speed - 2.f) / 8.f, 0.f, 1.f);
    t = t * t * (3.f - 2.f * t);
    float deg = g_touch_look * (1.f + g_touch_look_accel * t);
    if (s_aim)
        deg *= g_touch_look_ads;
    const float per_unit = std::max(psMouseSens * psMouseSensScale / 50.f, 1e-5f); // radians a mouse unit turns
    const float k = deg2rad(deg) / per_unit;
    ux = in_x * k;
    uy = in_y * k;
}

// A real pad plugged into the phone (14-10, the owner: "a pad connected -- the touch buttons hide"): the last one used
// wins. A button or a stick of the pad hides the controls on the screen (and the close button over the screens: the
// pad has its own way out), a finger landing on the screen or the pad going away brings them back.
bool s_pad_mode = false;
u32 s_pad_seen = 0; // g_pad_real_frame last looked at
u32 s_touch_prev = 0; // fingers on the screen the frame before

void pad_mode_frame()
{
    const xr_pad_touch_state& t = g_pad_touch;
    const u32 fingers = (t.frame && Device.dwFrame - t.frame <= 1) ? t.count : 0;
    if (!g_pad_attached)
        s_pad_mode = false;
    else if (g_pad_real_frame != s_pad_seen)
        s_pad_mode = true;
    else if (fingers > s_touch_prev)
        s_pad_mode = false;
    s_pad_seen = g_pad_real_frame;
    s_touch_prev = fingers;
}

// The close button over the screens; true while it presses Plus (the play's controls stay out of the way then).
bool close_frame()
{
    const bool want = g_touch_controls && !s_pad_mode && UIRml::Ready() && UIRml::WantsInput() && !UIRml::MenuActive() && g_pad_play_frame != Device.dwFrame;
    if (!s_close && want && Device.dwFrame >= s_close_try)
    {
        s_close = UIRmlInternal::BuildDocument("touch_close.rml");
        if (s_close)
        {
            s_close->AddEventListener(Rml::EventId::Click, &s_close_listener);
            s_close->Hide();
            s_close_shown = false;
        }
        else
            s_close_try = Device.dwFrame + 600;
    }
    if (s_close)
    {
        if (want)
        {
            // a thumb's size whatever the screen's unit: 0.28 inch
            const float side = std::max(48.f, (g_display_dpi > 0.f ? g_display_dpi : 300.f) * 0.28f);
            s_close->SetProperty("width", px(side));
            s_close->SetProperty("height", px(side));
            s_close->SetProperty("line-height", px(side));
            s_close->SetProperty("border-radius", px(side * 0.5f));
            s_close->SetProperty("font-size", px(side * 0.62f));
            if (!s_close_shown)
            {
                s_close->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
                s_close_shown = true;
            }
            s_close->PullToFront();
        }
        else if (s_close_shown)
        {
            s_close->Hide();
            s_close_shown = false;
        }
    }
    if (s_close_frames <= 0)
        return false;
    --s_close_frames;
    g_touch_pad.frame = Device.dwFrame;
    g_touch_pad.buttons = XRPAD_PLUS;
    g_touch_pad.lx = g_touch_pad.ly = 0;
    return true;
}
} // namespace

void UIRmlInternal::TouchControlsFrame()
{
    pad_mode_frame();
    if (close_frame())
        return;
    const bool active = g_touch_controls && !s_pad_mode && g_pad_play_frame == Device.dwFrame && UIRml::Ready();
    if (!s_doc)
    {
        if (!active || Device.dwFrame < s_next_try || !build())
            return;
    }
    Rml::Context* ctx = s_doc->GetContext();
    const Rml::Vector2i dims = ctx ? ctx->GetDimensions() : Rml::Vector2i(0, 0);
    if (!active || dims.x <= 0 || dims.y <= 0)
    {
        if (s_shown)
        {
            s_doc->Hide();
            s_shown = false;
            drop_all(dims);
        }
        return;
    }
    if (!s_shown)
    {
        s_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        s_shown = true;
    }
    s_doc->PullToFront();

    // use: only while there is something to use, named by what it is. The target drops out for a frame now and then
    // (the ray from the crosshair), so the button stays 0.3 s after it, and while a finger is on it.
    if (Rml::Element* use = s_btns[kUse].el)
    {
        CActor* a = Actor();
        const char* kind = a ? use_kind(a) : nullptr;
        const u32 now = Device.dwTimeContinual;
        if (kind)
        {
            s_use_seen = now;
            if (kind != s_use_kind)
            {
                s_use_kind = kind;
                use->SetInnerRML(use->GetAttribute<Rml::String>(kind, ""));
            }
        }
        const bool held = std::any_of(s_fingers.begin(), s_fingers.end(), [](const Finger& f) { return f.role == Role::Button && f.idx == kUse; });
        const bool show = s_use_kind && (now - s_use_seen < 300 || held);
        if (!show)
            s_use_kind = nullptr;
        use->SetClass("off", !show);
    }

    // the fingers this frame
    const xr_pad_touch_state& t = g_pad_touch;
    const u32 n = (t.frame && Device.dwFrame - t.frame <= 1) ? std::min<u32>(t.count, 4) : 0;
    for (auto it = s_fingers.begin(); it != s_fingers.end();)
    {
        bool still = false;
        for (u32 i = 0; i < n && !still; ++i)
            still = t.id[i] == it->id;
        if (still)
            ++it;
        else
        {
            release(*it, dims);
            it = s_fingers.erase(it);
        }
    }

    u32 buttons = 0;
    s16 lx = 0, ly = 0;
    float look_x = 0.f, look_y = 0.f; // px the look fingers moved this frame
    for (u32 i = 0; i < n; ++i)
    {
        const Rml::Vector2f p(t.x[i] * float(dims.x), t.y[i] * float(dims.y));
        auto it = std::find_if(s_fingers.begin(), s_fingers.end(), [&](const Finger& f) { return f.id == t.id[i]; });
        if (it == s_fingers.end())
        {
            Finger f{t.id[i], Role::Look, 0, p, p, Device.dwTimeContinual};
            f.role = pick(p, dims, f.idx);
            if (f.role == Role::Button && s_btns[f.idx].toggle)
                s_aim = !s_aim;
            if (f.role == Role::Stick)
            {
                const float r = stick_radius();
                s_anchor = Rml::Vector2f(std::clamp(p.x, r, float(dims.x) - r), std::clamp(p.y, r, float(dims.y) - r));
                stick_place(true);
            }
            s_fingers.push_back(f);
            continue; // moves count from the next frame
        }
        Finger& f = *it;
        const Rml::Vector2f d = p - f.last;
        f.last = p;
        switch (f.role)
        {
        case Role::Look:
            look_x += d.x;
            look_y += d.y;
            break;
        case Role::Button:
            if (s_btns[f.idx].look)
            {
                look_x += d.x;
                look_y += d.y;
            }
            break;
        case Role::Stick: {
            const float r = stick_radius();
            Rml::Vector2f v = p - s_anchor;
            float len = std::sqrt(v.x * v.x + v.y * v.y);
            if (len > 1.6f * r) // the base follows a thumb that went far: the run stays one push away
            {
                s_anchor = s_anchor + v * ((len - 1.6f * r) / len);
                stick_place(true);
                v = p - s_anchor;
                len = 1.6f * r;
            }
            const Rml::Vector2f dir = len > 0.5f ? v / len : Rml::Vector2f(0.f, 0.f);
            const float m = std::min(len / r, 1.f);
            lx = s16(dir.x * m * 32767.f);
            ly = s16(-dir.y * m * 32767.f); // the pad's +y is up
            knob_at(dir * std::min(len, r));
            if (!s_run && len > 1.3f * r)
            {
                s_run = true;
                buttons |= XRPAD_LSTICK; // one frame: the sprint key tapped
            }
            else if (s_run && len < 0.9f * r)
                s_run = false;
            break;
        }
        default: break;
        }
    }

    for (Btn& b : s_btns)
        if (b.el)
            b.el->SetClass("held", false);
    for (const Finger& f : s_fingers)
        if (f.role == Role::Button)
        {
            Btn& b = s_btns[f.idx];
            if (!b.toggle)
                buttons |= b.bit;
            if (b.el)
                b.el->SetClass("held", true);
        }
    if (s_aim)
        buttons |= XRPAD_ZL;
    if (s_btns[1].el)
        s_btns[1].el->SetClass("on", s_aim);

    g_touch_pad.frame = Device.dwFrame;
    g_touch_pad.buttons = buttons;
    g_touch_pad.lx = lx;
    g_touch_pad.ly = ly;
    float gx, gy;
    look_units(look_x, look_y, gx, gy);
    g_touch_pad.look_x += gx;
    g_touch_pad.look_y += gy;
}

void UIRmlInternal::TouchControlsShutdown()
{
    s_doc = nullptr; // released with the context
    s_shown = false;
    s_fingers.clear();
    s_aim = false;
    s_run = false;
    s_use_kind = nullptr; // the new document's t-use has its default text
    s_close = nullptr;
    s_close_shown = false;
    s_close_frames = 0;
    for (Btn& b : s_btns)
        b.el = nullptr;
    s_stick = s_knob = nullptr;
}

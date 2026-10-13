#include "StdAfx.h"
#include "ui_rml.h"

// The HUD on RmlUi -- bars, the active weapon's panel, the condition icons and the
// "what am I looking at" hint -- as a document that only DRAWS.
//
// ⭐A VIEW, NOT A PORT. CUIMainIngameWnd (ui/UIMainIngameWnd.cpp) stays the model:
// it reads the actor, runs the thresholds, folds in what Lua does through
// setup_game_icon / GetStatic, and every frame hands a UIRml::HudState to this
// file. Nothing a mod script can reach is replaced -- only who paints it. That is
// also why the legacy statics keep being updated while this is on: they ARE the
// state (a warning icon's "on" is its static's IsShown(), its colour is the
// static's colour).
//
// ⭐Everything the legacy window showed is drawn here, except the minimap's
// PICTURE (a level texture plus spots, rotated and clipped -- CUIZoneMap's job).
// The document leaves a hole for it, #map-view; HudMapView hands its box to the
// legacy window, which draws the map there before this document draws its frame
// on top (see CUIMainIngameWnd::Draw). One layout decides where the map is, so
// the picture and the frame cannot drift apart.
//
// ⭐THE INDICATORS LOG THEIR SWITCHES. Every time a condition icon, the armour
// row, the weapon panel, the car bar, the pick-up picture or the minimap frame
// goes on or off, one `~ [rml] hud:` line says so (and an icon's colour when it
// changes). The HUD has no other witness on a console nobody is filming: from the
// log alone one can tell that radiation lit at the anomaly and went out after the
// vodka.
//
// ⭐THE LAYOUT IS CONTENT. gamedata\config\ui_rml\hud.rml/.rcss, in dp anchored to
// a 1080-tall reference with a floor of 1 -- the in-game space -- so a 720p
// handheld gets one pixel per dp and everything comes out proportionally larger
// than on a desktop monitor, which is what a small screen needs. The geometry
// comes from the mod's own maingame.xml (extracted with dbtool, config\ui\):
// health and armour frames 153x20 with a 110x10 bar inside, the ammo frame
// 157x65, 45x45 condition icons.
//
// ⚠NO GATE since 03-10 (the `ui_new_hud` cvar went with the legacy windows).
// HudActive() is asked every frame by the legacy window; while the document is
// not built (content missing, RmlUi could not start) it answers false and the
// model window draws itself, so a failure here is never a screen with no HUD.
//
// ⚠The document must never take the pointer: `pointer-events: none` on the root,
// and WantsInput() does not know about it. The HUD is shown while the pickers or
// the inventory are open and must not eat their clicks.
// ⚠windowsx.h (through the precompiled header) defines these as one-argument
// macros over GetWindow(); Rml::Element declares methods with the same names and
// the macro turns the declaration into a comma expression. Same dance as the
// other files.
#undef GetNextSibling
#undef GetPreviousSibling
#undef GetPrevSibling
#undef GetFirstChild

#include <RmlUi/Core.h>

#include "ui/UITextureMaster.h"
#include "ui/UIStatic.h"
#include "ui/UIProgressBar.h"
#include "ui_base.h"
#include "HUDManager.h"
#include "UIGameCustom.h"
#include "UI.h"
#include "Level.h"
#include "Actor.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "WeaponMagazined.h"
#include "xr_level_controller.h"
#include "string_table.h"
#include "game_news.h"
#include "ui/UIInventoryUtilities.h"

namespace
{
using namespace UIRmlInternal;

Rml::ElementDocument* s_doc = nullptr;
bool s_shown = false;

UIRml::HudState s_state; // the newest snapshot from the legacy window
u32 s_pushed_frame = u32(-1);
u32 s_drawn_frame = u32(-1); // the last frame the legacy window drew
u32 s_next_try = 0; // frame before which a failed bring-up is not retried

// ⭐What is on the DOM right now, so a frame that changes nothing writes nothing.
// Every SetProperty / SetInnerRML dirties layout, and the HUD has a dozen values
// of which two or three move in a typical second.
struct Applied
{
    int health{-1}, armor{-1}; // percent
    int armor_on{-1}, weapon_on{-1}, hint_on{-1};
    xr_string name, count, section, hint;
    float grid_w{-1.f};
    xr_string ammo_file;
    int motion_on{-1}, motion_state{-1}, stamina{-1}, noise{-1}, light{-1};
    int car_on{-1}, car{-1};
    int icon_on[UIRml::HudState::ICONS];
    u32 icon_argb[UIRml::HudState::ICONS];
    int map_on{-1}, heading{INT_MIN}; // heading in tenths of a degree
    xr_string dist;
    int pickup_on{-1};
    u32 pickup_serial{u32(-1)};
    // the True Stalker panel (06-10)
    int mag{INT_MIN}, mode{INT_MIN}, empty{-1}, overweight{-1};
    xr_string reserve, ammo_type;
    int icon_sev[UIRml::HudState::ICONS];
    int flash_on[UIRml::HudState::FLASHES];

    Applied()
    {
        for (int i = 0; i < UIRml::HudState::ICONS; ++i)
        {
            icon_on[i] = -1;
            icon_argb[i] = 0;
            icon_sev[i] = -1;
        }
        for (int& f : flash_on)
            f = -1;
    }
    void reset() { *this = Applied(); }
} s_applied;

// ⭐What the LOG last said, kept apart from s_applied: that one is reset every
// time the document is shown again (the DOM may be stale), and a reset must not
// read as forty indicators switching at once.
struct Logged
{
    int icon_on[UIRml::HudState::ICONS];
    u32 icon_argb[UIRml::HudState::ICONS]{};
    int armor_on{-1}, weapon_on{-1}, car_on{-1}, pickup_on{-1}, map_on{-1};
    int flash_on[UIRml::HudState::FLASHES];
    Logged()
    {
        for (int& v : icon_on)
            v = -1;
        for (int& v : flash_on)
            v = -1;
    }
} s_logged;

struct Els
{
    Rml::Element *hp_fill{}, *ar_fill{}, *armor{}, *weapon{}, *w_name{}, *w_count{}, *w_icon{}, *hint{};
    Rml::Element *motion{}, *st_fill{}, *no_fill{}, *li_fill{}, *car{}, *car_fill{};
    Rml::Element* icon[UIRml::HudState::ICONS]{};
    Rml::Element* soldier[6]{};
    Rml::Element *map{}, *map_view{}, *compass{}, *dist{}, *pickup{};
    Rml::Element *w_mag{}, *w_res{}, *w_type{}, *w_mode{}, *weight{};
    Rml::Element* flash[UIRml::HudState::FLASHES]{};
} s_el;

// EWarningIcons, for the log.
constexpr const char* s_icon_name[UIRml::HudState::ICONS] = {"", "jammed", "radiation", "wound", "hunger", "psy", "god", "thirst"};

Rml::Element* by_id(const char* id) { return s_doc ? s_doc->GetElementById(id) : nullptr; }

// Text that reaches RML from the engine goes through the markup parser, so a
// stray '<' or '&' in an item's name would be read as markup.
xr_string markup_safe(const char* cp1251)
{
    const xr_string utf8 = ToUtf8(cp1251 ? cp1251 : "");
    xr_string out;
    out.reserve(utf8.size());
    for (char c : utf8)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    }
    return out;
}

// ⭐THE USE HINT (06-10, "the prompts look awful, worst on the Switch"). The mod's
// texts are sentences with the key in brackets -- "Search the body ($$ACTION_USE$$).
// \n Drag the body (SHIFT+$$ACTION_USE$$)." -- and a literal backslash-n that
// RmlUi showed as part of one long run-on line. Here each line becomes a row of
// its own and every bracketed key a badge; a row that ends in its only key gets
// the badge in FRONT, the True Stalker way: [A] Search the body.
xr_string s_test_hint; // ui_test hint
u32 s_test_hint_until = 0;

bool hint_is_key(const xr_string& s) { return !s.empty() && s.size() <= 24 && s.find('.') == xr_string::npos; }

xr_string hint_badges(xr_string key)
{
#ifdef __SWITCH__
    // the texts' own "SHIFT+..." / "CTRL+..." are the R / L shoulders on the pad
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"SHIFT", "R"}, {"Shift", "R"}, {"CTRL", "L"}, {"Ctrl", "L"}})
        for (size_t at; (at = key.find(from)) != xr_string::npos;)
            key.replace(at, xr_strlen(from), to);
#endif
    xr_string out;
    size_t from = 0;
    while (from <= key.size())
    {
        size_t plus = key.find('+', from);
        if (plus == xr_string::npos)
            plus = key.size();
        xr_string part = key.substr(from, plus - from);
        while (!part.empty() && part.front() == ' ')
            part.erase(part.begin());
        while (!part.empty() && part.back() == ' ')
            part.pop_back();
        // the keyboard names are the binding's own ("kF", "kSPACE"): the k goes
        if (part.size() > 1 && part[0] == 'k' && (isupper(u8(part[1])) || isdigit(u8(part[1]))))
            part.erase(part.begin());
        if (!part.empty())
        {
            if (!out.empty())
                out += "<span class=\"hk-plus\">+</span>";
            out += "<span class=\"hk\">" + markup_safe(part.c_str()) + "</span>";
        }
        from = plus + 1;
    }
    return out;
}

xr_string hint_markup(const char* cp1251)
{
    xr_string src = cp1251 ? cp1251 : "";
    for (size_t at; (at = src.find("\\n")) != xr_string::npos;)
        src.replace(at, 2, "\n");

    xr_string rml;
    size_t pos = 0;
    while (pos <= src.size())
    {
        size_t nl = src.find('\n', pos);
        if (nl == xr_string::npos)
            nl = src.size();
        xr_string line = src.substr(pos, nl - pos);
        pos = nl + 1;

        // the bracketed keys of this line
        struct Seg
        {
            xr_string text;
            bool key;
        };
        xr_vector<Seg> segs;
        size_t i = 0;
        while (i < line.size())
        {
            const size_t open = line.find('(', i);
            const size_t close = open == xr_string::npos ? xr_string::npos : line.find(')', open);
            if (close == xr_string::npos || !hint_is_key(line.substr(open + 1, close - open - 1)))
            {
                const size_t stop = (close == xr_string::npos) ? line.size() : close + 1;
                segs.push_back({line.substr(i, stop - i), false});
                i = stop;
                continue;
            }
            if (open > i)
                segs.push_back({line.substr(i, open - i), false});
            segs.push_back({line.substr(open + 1, close - open - 1), true});
            i = close + 1;
        }
        auto trim = [](xr_string s, const char* junk) {
            while (!s.empty() && strchr(junk, s.front()))
                s.erase(s.begin());
            while (!s.empty() && strchr(junk, s.back()))
                s.pop_back();
            return s;
        };
        int keys = 0;
        for (const Seg& s : segs)
            keys += s.key ? 1 : 0;
        xr_string row;
        if (keys == 1 && segs.back().key)
        {
            // "Talk (F)." -> [A] Talk
            xr_string text;
            for (size_t k = 0; k + 1 < segs.size(); ++k)
                text += segs[k].text;
            text = trim(text, " .,:");
            row = hint_badges(segs.back().text) + (text.empty() ? "" : "<span class=\"hk-txt\">" + markup_safe(text.c_str()) + "</span>");
        }
        else if (keys == 1 && segs.size() >= 2 && segs[segs.size() - 2].key && trim(segs.back().text, " .,").empty())
        {
            // the same with the full stop after the bracket: "Talk (F)."
            xr_string text;
            for (size_t k = 0; k + 2 < segs.size(); ++k)
                text += segs[k].text;
            text = trim(text, " .,:");
            row = hint_badges(segs[segs.size() - 2].text) + (text.empty() ? "" : "<span class=\"hk-txt\">" + markup_safe(text.c_str()) + "</span>");
        }
        else
        {
            for (size_t k = 0; k < segs.size(); ++k)
            {
                const Seg& s = segs[k];
                if (!s.key)
                {
                    row += "<span class=\"hk-txt\">" + markup_safe(trim(s.text, " ").c_str()) + "</span>";
                    continue;
                }
                // a badge right before a comma or full stop hugs it
                const xr_string next = (k + 1 < segs.size() && !segs[k + 1].key) ? trim(segs[k + 1].text, " ") : xr_string();
                const bool tight = !next.empty() && strchr(",.;", next[0]); // (strchr finds the terminator too)
                row += tight ? "<span class=\"hk-tight\">" + hint_badges(s.text) + "</span>" : hint_badges(s.text);
            }
        }
        if (!trim(line, " .").empty())
            rml += "<div class=\"hk-row\">" + row + "</div>";
    }
    return rml;
}

void set_width_pct(Rml::Element* e, int pct)
{
    if (e)
        e->SetProperty("width", Rml::String(std::to_string(pct) + "%"));
}

void set_height_pct(Rml::Element* e, int pct)
{
    if (e)
        e->SetProperty("height", Rml::String(std::to_string(pct) + "%"));
}

void set_display(Rml::Element* e, bool on, const char* shown = "block")
{
    if (e)
        e->SetProperty("display", on ? shown : "none");
}

int to_pct(float v01) { return int(clampr(v01, 0.f, 1.f) * 100.f + 0.5f); }

// argb as the engine stores it -> the #rrggbbaa RCSS wants.
Rml::String rcss_colour(u32 argb)
{
    char buf[16];
    xr_sprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", (argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, (argb >> 24) & 0xff);
    return buf;
}

void cache_elements()
{
    s_el = Els();
    s_el.hp_fill = by_id("hp-fill");
    s_el.ar_fill = by_id("ar-fill");
    s_el.armor = by_id("armor");
    s_el.weapon = by_id("weapon");
    s_el.w_name = by_id("w-name");
    s_el.w_count = by_id("w-count");
    s_el.w_icon = by_id("w-icon");
    s_el.hint = by_id("hint");
    s_el.motion = by_id("motion");
    s_el.st_fill = by_id("st-fill");
    s_el.no_fill = by_id("no-fill");
    s_el.li_fill = by_id("li-fill");
    s_el.car = by_id("car");
    s_el.car_fill = by_id("car-fill");
    for (int i = 0; i < 6; ++i)
        s_el.soldier[i] = by_id((xr_string("sol-") + std::to_string(i).c_str()).c_str());
    for (int i = 1; i < UIRml::HudState::ICONS; ++i)
        s_el.icon[i] = by_id((xr_string("ico-") + std::to_string(i).c_str()).c_str());
    s_el.map = by_id("map");
    s_el.map_view = by_id("map-view");
    s_el.compass = by_id("map-compass");
    s_el.dist = by_id("map-dist");
    s_el.pickup = by_id("pickup");
    for (int i = 0; i < UIRml::HudState::FLASHES; ++i)
        s_el.flash[i] = by_id((xr_string("flash-") + std::to_string(i).c_str()).c_str());
    s_el.w_mag = by_id("w-mag");
    s_el.w_res = by_id("w-res");
    s_el.w_type = by_id("w-type");
    s_el.w_mode = by_id("w-mode");
    s_el.weight = by_id("ico-weight");
}

// ⚠A sprite the mod emptied (0x0 in its textures_descr -- this mod does it to the
// PDA icon) is not in the generated sheet, and `image(name)` then falls through to
// a FILE called that, which does not load and draws as a white box. So an element
// that may name such a sprite says it in `data-sprite`, and gets its decorator
// only if the atlas really has that rectangle; otherwise it stays blank.
void apply_data_sprites(Rml::Element* e)
{
    if (!e)
        return;
    if (const Rml::Variant* v = e->GetAttribute("data-sprite"))
    {
        const Rml::String name = v->Get<Rml::String>();
        const auto& table = CUITextureMaster::Textures();
        const auto it = table.find(shared_str(name.c_str()));
        if (it != table.end() && it->second.rect.width() > 0.f && it->second.rect.height() > 0.f)
            e->SetProperty("decorator", "image(" + name + ")");
        else
            Msg("~ [rml] hud: sprite '%s' is missing or empty in the atlas -- '%s' stays blank", name.c_str(), e->GetId().c_str());
    }
    for (int i = 0; i < e->GetNumChildren(); ++i)
        apply_data_sprites(e->GetChild(i));
}

void hide_doc()
{
    if (s_doc && s_shown)
    {
        s_doc->Hide();
        s_shown = false;
    }
}

// With the mod's statics, further down.
void forget_takeovers(bool restore);
void collect_takeovers(Rml::Element* e);
void apply_hud_slots(); // the quick slots, at the end of the file
void wheel_frame(); // the weapon wheel, at the very end
void news_frame(); // the news, before apply()
void news_forget_dom();

void drop_doc()
{
    forget_takeovers(true);
    news_forget_dom();
    if (s_doc)
    {
        s_doc->Close();
        s_doc = nullptr;
    }
    s_shown = false;
    s_el = Els();
    s_applied.reset();
}

bool build()
{
    drop_doc();

    s_doc = BuildDocument("hud.rml");
    if (!s_doc)
    {
        // ⚠Not retried every frame: a missing file would otherwise be re-read and
        // re-logged sixty times a second. The legacy HUD draws meanwhile.
        s_next_try = Device.dwFrame + 600;
        return false;
    }

    AttachItemSprites(s_doc);
    cache_elements();
    apply_data_sprites(s_doc);
    collect_takeovers(s_doc); // drop_doc above has forgotten the old ones
    s_applied.reset();
    apply_hud_slots(); // what the script handed over before this document existed

    // BuildDocument shows what it builds; the HUD stays hidden until the legacy
    // window says it is on screen.
    s_doc->Hide();
    s_shown = false;

    Msg("~ [rml] hud: built");
    return true;
}

void log_switch(int& was, bool now, const char* what)
{
    const int v = now ? 1 : 0;
    if (v == was)
        return;
    // The very first value is the starting state, not a switch, but it is worth
    // one line too: "nothing lit at spawn" is part of the evidence.
    Msg("~ [rml] hud: %s %s", what, now ? "ON" : "off");
    was = v;
}

void log_switches(const UIRml::HudState& s)
{
    Logged& l = s_logged;
    for (int i = 1; i < UIRml::HudState::ICONS; ++i)
    {
        const bool on = s.icon_on[i];
        if (on && (l.icon_on[i] != 1 || l.icon_argb[i] != s.icon_argb[i]))
        {
            const u32 c = s.icon_argb[i];
            Msg("~ [rml] hud: icon %s ON colour %02x%02x%02x a%u%s", s_icon_name[i], (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, c >> 24,
                s_el.icon[i] ? "" : " (no element for it in hud.rml!)");
            l.icon_on[i] = 1;
            l.icon_argb[i] = c;
        }
        else if (!on && l.icon_on[i] != 0)
        {
            if (l.icon_on[i] == 1)
                Msg("~ [rml] hud: icon %s off", s_icon_name[i]);
            l.icon_on[i] = 0;
        }
    }
    log_switch(l.armor_on, s.armor_on, "armour row");
    log_switch(l.weapon_on, s.weapon_on, "weapon panel");
    log_switch(l.car_on, s.car_on, "car bar");
    log_switch(l.map_on, s.map_on, "minimap frame");
    log_switch(l.pickup_on, s.pickup_on, "pick-up picture");
    for (int i = 0; i < UIRml::HudState::FLASHES; ++i)
        log_switch(l.flash_on[i], s.flash_on[i], i ? "flashing mail icon" : "flashing PDA icon");
}

// The pick-up picture: the item and its addons, fitted into #pickup the way the
// legacy static fitted it into its 150x100 -- shrunk to fit, never enlarged past
// the scale the box's own size implies, centred.
void build_pickup(const UIRml::HudState& s)
{
    Rml::Element* box = s_el.pickup;
    if (!box || s.pickup_parts <= 0)
        return;

    const UIRml::HudState::IconPart& item = s.pickup[0];
    if (item.w <= 0.f || item.h <= 0.f)
        return;

    // ⭐The box is in dp and so is the cap: data-max-scale is how many dp one
    // icon pixel may become at most (the legacy never enlarged an icon).
    const Rml::Vector2f box_px = box->GetBox().GetSize(Rml::BoxArea::Content);
    float max_scale = 1.f;
    if (const Rml::Variant* v = box->GetAttribute("data-max-scale"))
        max_scale = v->Get<float>();
    const float dp = box->GetContext() ? box->GetContext()->GetDensityIndependentPixelRatio() : 1.f;
    float k = max_scale * dp;
    if (box_px.x > 0.f && box_px.y > 0.f)
        k = _min(k, _min(box_px.x / item.w, box_px.y / item.h));

    const float ox = (box_px.x - item.w * k) * 0.5f;
    const float oy = (box_px.y - item.h * k) * 0.5f;

    xr_string rml;
    for (int i = 0; i < s.pickup_parts; ++i)
    {
        const UIRml::HudState::IconPart& p = s.pickup[i];
        if (p.w <= 0.f || p.h <= 0.f)
            continue;
        string512 div;
        xr_sprintf(div, sizeof(div), "<div class=\"pk\" style=\"left: %.0fpx; top: %.0fpx; width: %.0fpx; height: %.0fpx; decorator: %s;\"/>", ox + p.x * k, oy + p.y * k,
                   p.w * k, p.h * k, ItemDecorator(p.section.c_str(), p.file.c_str()).c_str());
        rml += div;
    }
    box->SetInnerRML(rml.c_str());
}

// =========================================== the mod's own statics ==========
//
// ⭐A mod puts pieces of its own on the HUD through get_hud():AddCustomStatic,
// in the legacy layout's coordinates: this one stacks a radiation and a psy bar
// over the armour row (ui_rad.script), hangs a clock inside the minimap frame
// (ogse.script), a sleep icon under the condition column, booster indicators
// beside the motion icon, quick slots on the left. With the layout now the
// document's, those land wherever the OLD layout had a gap -- the psy bar ends up
// behind the new armour row. Three ways for hud.rml to take them over, by name:
//
//   data-adopt="a b"   the DOCUMENT draws it, from the static's own state: shown
//                      while the first of the named statics exists and is shown
//                      (and has the class `on` then, for an entrance animation);
//                      a .fill inside gets the width of the static's progress bar,
//                      data-adopt-text copies its text -- into a .txt inside when
//                      there is one (which also asks for the text by itself), the
//                      legacy %c[...] colours and \n kept -- data-adopt-colour its
//                      colour (image-color). The static stops drawing itself.
//   data-place="a b"   the static still draws itself (heading, light animation,
//                      text -- whatever the script does to it), but in this
//                      element's box.
//   data-shift="pfx"   every static whose name starts with pfx moves by the same
//   data-anchor="a"    offset, so that `a` lands at this element's top-left; the
//                      group keeps its own spacing and sizes.
//
// The script side stays the model, exactly as CUIMainIngameWnd does for the rest.
struct Adopted
{
    Rml::Element* el{};
    Rml::Element* fill{};
    Rml::Element* txt_el{}; // where the text goes: a .txt inside, or el itself
    xr_vector<shared_str> names;
    bool text{}, colour{};
    int on{-1}, pct{-1};
    u32 argb{};
    xr_string txt;
    int logged{-1};
};
struct Placed
{
    Rml::Element* el{};
    xr_vector<shared_str> names; // data-place: statics; data-shift: one prefix
    shared_str anchor; // data-shift only
};
xr_vector<Adopted> s_adopted;
xr_vector<Placed> s_placed;
// Where a placed or shifted static was before the document moved it: the base a
// shift is measured from, and what `ui_new_hud 0` puts back.
xr_map<shared_str, Frect> s_orig_rect;

CUIGameCustom* ui_game()
{
    if (!g_hud)
        return nullptr;
    CUI* ui = g_hud->GetUI();
    return ui ? ui->UIGame() : nullptr;
}

xr_vector<shared_str> split_names(const Rml::String& list)
{
    xr_vector<shared_str> out;
    Rml::StringList parts;
    Rml::StringUtilities::ExpandString(parts, list, ' ');
    for (const Rml::String& p : parts)
        if (!p.empty())
            out.emplace_back(p.c_str());
    return out;
}

void collect_takeovers(Rml::Element* e)
{
    if (!e)
        return;
    if (e->HasAttribute("data-adopt"))
    {
        Adopted a;
        a.el = e;
        a.names = split_names(e->GetAttribute<Rml::String>("data-adopt", ""));
        a.colour = e->HasAttribute("data-adopt-colour");
        Rml::ElementList found;
        e->GetElementsByClassName(found, "fill");
        a.fill = found.empty() ? nullptr : found.front();
        found.clear();
        e->GetElementsByClassName(found, "txt");
        a.txt_el = found.empty() ? e : found.front();
        a.text = e->HasAttribute("data-adopt-text") || !found.empty();
        s_adopted.push_back(std::move(a));
    }
    if (e->HasAttribute("data-place"))
        s_placed.push_back({e, split_names(e->GetAttribute<Rml::String>("data-place", "")), {}});
    if (e->HasAttribute("data-shift"))
        s_placed.push_back({e, split_names(e->GetAttribute<Rml::String>("data-shift", "")), shared_str(e->GetAttribute<Rml::String>("data-anchor", "").c_str())});
    for (int i = 0; i < e->GetNumChildren(); ++i)
        collect_takeovers(e->GetChild(i));
}

// The element's border box in the legacy UI space (1024x768), or false while it
// has no size (display:none, or not laid out yet).
bool box_in_ui(Rml::Element* e, Frect& out)
{
    Rml::Context* ctx = e ? e->GetContext() : nullptr;
    if (!ctx)
        return false;
    const Rml::Vector2i dims = ctx->GetDimensions();
    const Rml::Vector2f pos = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
    if (dims.x <= 0 || dims.y <= 0 || size.x < 1.f || size.y < 1.f)
        return false;
    const float kx = float(UI_BASE_WIDTH) / dims.x, ky = float(UI_BASE_HEIGHT) / dims.y;
    out.set(pos.x * kx, pos.y * ky, (pos.x + size.x) * kx, (pos.y + size.y) * ky);
    return true;
}

// Moves a static to `abs` (absolute UI rect), remembering where it was first.
void put_static(const shared_str& name, CUIStatic* st, const Frect& abs)
{
    if (s_orig_rect.find(name) == s_orig_rect.end())
        s_orig_rect[name] = st->GetWndRect();
    Frect r = abs;
    // ⚠A static with heading is drawn through S2DVert::rotate_pt, which squeezes x
    // by the wide-screen factor around the centre (the legacy way of keeping a
    // rotating picture round). Widened by the same factor here, the picture comes
    // out exactly the box's width on screen.
    if (st->Heading() && UI()->is_widescreen())
    {
        const float kx = UI()->get_current_kx();
        if (kx > 0.f)
        {
            const float cx = (r.x1 + r.x2) * 0.5f, hw = r.width() * 0.5f / kx;
            r.x1 = cx - hw;
            r.x2 = cx + hw;
        }
    }
    if (CUIWindow* parent = st->GetParent())
    {
        Frect pr;
        parent->GetAbsoluteRect(pr);
        r.sub(pr.lt.x, pr.lt.y);
    }
    const Frect& cur = st->GetWndRect();
    if (!fsimilar(cur.x1, r.x1, 0.25f) || !fsimilar(cur.y1, r.y1, 0.25f) || !fsimilar(cur.x2, r.x2, 0.25f) || !fsimilar(cur.y2, r.y2, 0.25f))
        st->SetWndRect(r);
}

float progress_frac(CUIStatic* st)
{
    for (CUIWindow* w : st->GetChildWndList())
        if (auto* pb = smart_cast<CUIProgressBar*>(w))
        {
            const float len = pb->GetRange_max() - pb->GetRange_min();
            return len > 0.f ? clampr((pb->GetProgressPos() - pb->GetRange_min()) / len, 0.f, 1.f) : 0.f;
        }
    return 0.f;
}

void apply_takeovers()
{
    CUIGameCustom* game = ui_game();

    for (Adopted& a : s_adopted)
    {
        SDrawStaticStruct* st = nullptr;
        if (game)
            for (const shared_str& n : a.names)
            {
                SDrawStaticStruct* c = game->GetCustomStatic(n);
                if (c && c->m_static && c->m_static->IsShown())
                {
                    st = c;
                    break;
                }
            }

        // a text block with nothing to say is not shown: scripts keep a static up with SetText("") (detector_upgrade_hud)
        if (st && a.text && !a.fill)
        {
            LPCSTR t = st->m_static->GetText();
            if (!t || !*t)
                st = nullptr;
        }
        const int on = st ? 1 : 0;
        if (on != a.on)
        {
            a.on = on;
            set_display(a.el, on != 0);
            a.el->SetClass("on", on != 0);
        }
        if (on != a.logged)
        {
            Msg("~ [rml] hud: '%s' (%s) %s", a.el->GetId().c_str(), a.names.empty() ? "" : a.names.front().c_str(), on ? "ON" : "off");
            a.logged = on;
        }
        if (!st)
            continue;

        if (a.fill)
        {
            const int pct = to_pct(progress_frac(st->m_static));
            if (pct != a.pct)
            {
                a.pct = pct;
                set_width_pct(a.fill, pct);
            }
        }
        if (a.text)
        {
            LPCSTR t = st->m_static->GetText();
            if (!t)
                t = "";
            if (a.txt != t)
            {
                a.txt = t;
                a.txt_el->SetInnerRML(UIRml::LuaText(t).c_str());
            }
        }
        if (a.colour)
        {
            const u32 c = st->m_static->GetColor();
            if (c != a.argb)
            {
                a.argb = c;
                a.el->SetProperty("image-color", rcss_colour(c));
            }
        }
    }

    if (!game)
        return;
    for (Placed& p : s_placed)
    {
        Frect box;
        if (!box_in_ui(p.el, box))
            continue;

        if (!p.anchor.size())
        {
            // data-place: each named static fills the box.
            for (const shared_str& n : p.names)
                if (SDrawStaticStruct* c = game->GetCustomStatic(n); c && c->m_static)
                    put_static(n, c->m_static, box);
            continue;
        }

        // data-shift: the anchor's ORIGINAL top-left goes to the box's top-left.
        // It need not exist right now -- once seen, its original is remembered.
        if (SDrawStaticStruct* c = game->GetCustomStatic(p.anchor); c && c->m_static && s_orig_rect.find(p.anchor) == s_orig_rect.end())
            s_orig_rect[p.anchor] = c->m_static->GetWndRect();
        const auto anchor = s_orig_rect.find(p.anchor);
        if (anchor == s_orig_rect.end() || p.names.empty())
            continue;
        const Fvector2 delta{box.x1 - anchor->second.x1, box.y1 - anchor->second.y1};
        const char* prefix = p.names.front().c_str();
        const size_t plen = xr_strlen(prefix);
        for (SDrawStaticStruct& c : game->m_custom_statics)
        {
            if (!c.m_static || !c.m_name.size() || strncmp(c.m_name.c_str(), prefix, plen) != 0)
                continue;
            if (s_orig_rect.find(c.m_name) == s_orig_rect.end())
                s_orig_rect[c.m_name] = c.m_static->GetWndRect();
            Frect r = s_orig_rect[c.m_name];
            r.add(delta.x, delta.y);
            put_static(c.m_name, c.m_static, r);
        }
    }
}

// `ui_new_hud 0`, or the document going away: every static the document moved
// goes back to where its script and XML put it, and none is adopted any more.
void forget_takeovers(bool restore)
{
    if (restore)
        if (CUIGameCustom* game = ui_game())
            for (const auto& [name, rect] : s_orig_rect)
                if (SDrawStaticStruct* c = game->GetCustomStatic(name); c && c->m_static)
                    c->m_static->SetWndRect(rect);
    s_orig_rect.clear();
    s_adopted.clear();
    s_placed.clear();
}

// ================================================================ the news ====
//
// ⭐10-10 (the owner: "the pop-up messages look bad, put them in the bottom left"). The legacy log was a strip 800 units
// wide halfway down the left edge, each line a stretched ui_actor_news plate. Here a column of cards at the bottom
// left, the newest at the bottom: the picture the script names, the sender, the game time, the text. The sender is
// the first line of the text, the way the mod writes news -- "%c[255,238,155,23]Who:\n%c[default]text"; a line with
// no such head is all text. A card lives show_time ms of the HUD being ON screen (news that came while a dialog or the
// inventory was up waits for the HUD) and then fades (.nw.out in hud.rcss).
constexpr int kNewsMax = 4; // standing cards; one more pushes the oldest out
constexpr int kNewsFadeMs = 600; // = the transition of .nw.out in hud.rcss

struct NewsCard
{
    xr_string rml; // the card's inner markup
    int left_ms{}; // of the HUD on screen, the fade included
    bool out{}; // fading
    Rml::Element* el{}; // on the DOM; null until the document is shown
};
xr_vector<NewsCard> s_news;
u32 s_news_clock = 0; // Device.dwTimeContinual of the last frame the HUD was shown; 0 = not counting

// A picture of the legacy UI by name -- a sprite of ui\textures_descr or a file -- with the rectangle of the file to
// show (x, y, WIDTH, HEIGHT, as SetOriginalRect takes it; zero = the sprite's own, or the whole file). The same as the
// PDA's news list draws (ui_rml_pda.cpp img_tag).
xr_string news_picture(const char* tex, Frect r)
{
    if (!tex || !*tex)
        return "";
    xr_string file = tex;
    if (CUITextureMaster::Textures().find(tex) != CUITextureMaster::Textures().end())
    {
        file = CUITextureMaster::GetTextureFileName(tex);
        if (r.x2 <= 0.f || r.y2 <= 0.f)
        {
            const Frect s = CUITextureMaster::GetTextureRect(tex);
            r.set(s.x1, s.y1, s.width(), s.height());
        }
    }
    for (char& ch : file) // ⚠a backslash is an escape to the markup parser
        if (ch == '\\')
            ch = '/';
    // fitted into the 72x48 dp of .nw-ico, never enlarged past 1 dp a pixel (the news icons are 83x47, the portraits
    // square): an <img> given both max sizes would be squeezed rather than scaled
    string512 buf;
    if (r.x2 > 0.f && r.y2 > 0.f)
    {
        const float k = std::min({1.f, 72.f / r.x2, 48.f / r.y2});
        xr_sprintf(buf, sizeof(buf), "<img class=\"nw-pic\" src=\"%s\" rect=\"%.0f %.0f %.0f %.0f\" style=\"width: %.0fdp; height: %.0fdp;\"/>", file.c_str(), r.x1,
                   r.y1, r.x2, r.y2, r.x2 * k, r.y2 * k);
    }
    else
        xr_sprintf(buf, sizeof(buf), "<img class=\"nw-pic\" src=\"%s\" style=\"height: 48dp;\"/>", file.c_str());
    return buf;
}

// "%c[255,238,155,23]Who:\n%c[default]text" -> "Who" and the rest.
void news_split(const xr_string& src, xr_string& who, xr_string& text)
{
    who.clear();
    text = src;
    const size_t nl = src.find("\\n");
    if (nl == xr_string::npos)
        return;
    xr_string head;
    for (size_t i = 0; i < nl;)
    {
        if (src.compare(i, 3, "%c[") == 0)
        {
            const size_t end = src.find(']', i);
            if (end != xr_string::npos && end < nl)
            {
                i = end + 1;
                continue;
            }
        }
        head += src[i++];
    }
    while (!head.empty() && strchr(" :", head.back()))
        head.pop_back();
    while (!head.empty() && head.front() == ' ')
        head.erase(head.begin());
    if (head.empty() || head.size() > 96) // a long first line is the message itself (bytes: UTF-8 Cyrillic is two)
        return;
    who = head;
    text = src.substr(nl + 2);
}

void news_drop(size_t i)
{
    if (Rml::Element* e = s_news[i].el)
        if (Rml::Element* parent = e->GetParentNode())
            parent->RemoveChild(e);
    s_news.erase(s_news.begin() + i);
}

void news_forget_dom()
{
    for (NewsCard& c : s_news)
        c.el = nullptr;
}

void news_frame()
{
    const u32 now = Device.dwTimeContinual;
    const int dt = s_news_clock ? int(std::min<u32>(now - s_news_clock, 100)) : 0;
    s_news_clock = now;
    Rml::Element* box = by_id("news");
    for (size_t i = 0; i < s_news.size();)
    {
        NewsCard& c = s_news[i];
        c.left_ms -= dt;
        if (c.left_ms <= 0)
        {
            news_drop(i);
            continue;
        }
        if (!c.el && box)
        {
            Rml::ElementPtr e = s_doc->CreateElement("div");
            e->SetClass("nw", true);
            e->SetClass("out", c.out);
            e->SetInnerRML(c.rml);
            c.el = box->AppendChild(std::move(e));
        }
        if (!c.out && c.left_ms <= kNewsFadeMs)
        {
            c.out = true;
            if (c.el)
                c.el->SetClass("out", true);
        }
        ++i;
    }
}

void apply()
{
    const UIRml::HudState& s = s_state;
    Applied& a = s_applied;

    log_switches(s);
    apply_takeovers();
    news_frame();

    // --- the bars ---------------------------------------------------------
    const int hp = to_pct(s.health);
    if (hp != a.health)
    {
        a.health = hp;
        set_width_pct(s_el.hp_fill, hp);
    }

    const int armor_on = s.armor_on ? 1 : 0;
    if (armor_on != a.armor_on)
    {
        a.armor_on = armor_on;
        set_display(s_el.armor, armor_on != 0);
    }
    if (armor_on)
    {
        const int ar = to_pct(s.armor);
        if (ar != a.armor)
        {
            a.armor = ar;
            set_width_pct(s_el.ar_fill, ar);
        }
    }

    // --- the active item's panel -----------------------------------------
    const int weapon_on = s.weapon_on ? 1 : 0;
    if (weapon_on != a.weapon_on)
    {
        a.weapon_on = weapon_on;
        set_display(s_el.weapon, weapon_on != 0);
    }
    if (weapon_on)
    {
        if (s.weapon_name != a.name)
        {
            a.name = s.weapon_name;
            if (s_el.w_name)
                s_el.w_name->SetInnerRML(markup_safe(s.weapon_name.c_str()).c_str());
        }
        if (s.ammo_text != a.count)
        {
            a.count = s.ammo_text;
            if (s_el.w_count)
                s_el.w_count->SetInnerRML(markup_safe(s.ammo_text.c_str()).c_str());
        }
        if (s.ammo_section != a.section || s.ammo_icon_file != a.ammo_file || !fsimilar(s.ammo_grid_w, a.grid_w))
        {
            a.section = s.ammo_section;
            a.ammo_file = s.ammo_icon_file;
            a.grid_w = s.ammo_grid_w;
            if (s_el.w_icon)
            {
                if (s.ammo_section.empty())
                    s_el.w_icon->SetProperty("display", "none");
                else
                {
                    s_el.w_icon->SetProperty("display", "block");
                    s_el.w_icon->SetProperty("decorator", ItemDecorator(s.ammo_section.c_str(), s.ammo_icon_file.c_str()).c_str());
                    // The legacy icon is one cell tall and one or two wide.
                    const float gw = clampr(s.ammo_grid_w, 1.f, 2.f);
                    s_el.w_icon->SetProperty("width", Rml::String(std::to_string(int(gw * 52.f)) + "dp"));
                }
            }
        }

        // The True Stalker split (06-10): loaded rounds big, the rest beside them,
        // the type, the fire mode as pips; an empty magazine turns the number red.
        if (s.ammo_mag != a.mag)
        {
            a.mag = s.ammo_mag;
            if (s_el.w_mag)
                s_el.w_mag->SetInnerRML(s.ammo_mag >= 0 ? std::to_string(s.ammo_mag).c_str() : "");
        }
        const int empty = s.ammo_mag == 0 ? 1 : 0;
        if (empty != a.empty && s_el.weapon)
        {
            a.empty = empty;
            s_el.weapon->SetClass("empty", empty != 0);
        }
        if (s.ammo_reserve != a.reserve)
        {
            a.reserve = s.ammo_reserve;
            if (s_el.w_res)
                s_el.w_res->SetInnerRML(markup_safe(s.ammo_reserve.c_str()).c_str());
        }
        if (s.ammo_type != a.ammo_type)
        {
            a.ammo_type = s.ammo_type;
            if (s_el.w_type)
                s_el.w_type->SetInnerRML(markup_safe(s.ammo_type.c_str()).c_str());
        }
        const int mode = s.gl_mode ? 1000 : s.fire_mode;
        if (mode != a.mode && s_el.w_mode)
        {
            a.mode = mode;
            Rml::String rml;
            if (mode == 1000) // the grenade launcher: its word is in the markup (data-t-gl)
                rml = "<div class=\"mode-txt\">" + s_el.w_mode->GetAttribute<Rml::String>("data-t-gl", "GL") + "</div>";
            else if (mode < 0) // automatic: a run of pips joined into a bar
                rml = "<div class=\"pip long\"/>";
            else
                for (int i = 0; i < std::min(mode, 5); ++i)
                    rml += "<div class=\"pip\"/>";
            s_el.w_mode->SetInnerRML(rml);
        }
    }

    // --- carrying too much (06-10) -----------------------------------------
    if (s.overweight != a.overweight && s_el.weight)
    {
        a.overweight = s.overweight;
        set_display(s_el.weight, s.overweight > 0);
        s_el.weight->SetClass("sev-2", s.overweight == 1);
        s_el.weight->SetClass("sev-3", s.overweight == 2);
    }

    // --- the condition icons ---------------------------------------------
    for (int i = 1; i < UIRml::HudState::ICONS; ++i)
    {
        Rml::Element* e = s_el.icon[i];
        if (!e)
            continue;
        const int on = s.icon_on[i] ? 1 : 0;
        if (on != a.icon_on[i])
        {
            a.icon_on[i] = on;
            set_display(e, on != 0);
        }
        if (on && s.icon_argb[i] != a.icon_argb[i])
        {
            a.icon_argb[i] = s.icon_argb[i];
            // ⭐06-10: the window's threshold colour (green / yellow / red) becomes a
            // severity class and the skin picks the shade -- the raw colours were
            // pure 00ff00 / ffff00 / ff0000 and screamed against the True Stalker
            // palette. An icon without data-severity keeps the raw colour.
            if (e->HasAttribute("data-severity"))
            {
                const u32 c = s.icon_argb[i];
                const int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff;
                const int sev = (g > r + 30) ? 1 : (r > 180 && g > 120) ? 2 : 3;
                if (sev != a.icon_sev[i])
                {
                    a.icon_sev[i] = sev;
                    for (int k = 1; k <= 3; ++k)
                        e->SetClass(k == 1 ? "sev-1" : k == 2 ? "sev-2" : "sev-3", k == sev);
                }
            }
            else
                e->SetProperty("image-color", rcss_colour(s.icon_argb[i]));
        }
    }

    // --- the motion icon -------------------------------------------------
    const int motion_on = s.motion_on ? 1 : 0;
    if (motion_on != a.motion_on)
    {
        a.motion_on = motion_on;
        set_display(s_el.motion, motion_on != 0);
    }
    if (motion_on)
    {
        // The stance picture: exactly one of six is shown.
        const int st = std::clamp(s.motion_state, 0, 5);
        if (st != a.motion_state)
        {
            a.motion_state = st;
            for (int i = 0; i < 6; ++i)
                set_display(s_el.soldier[i], i == st);
        }
        const int stamina = to_pct(s.stamina), noise = to_pct(s.noise), light = to_pct(s.light);
        if (stamina != a.stamina)
        {
            a.stamina = stamina;
            set_width_pct(s_el.st_fill, stamina);
        }
        // The two vertical bars fill from the bottom, so it is their height that moves.
        if (noise != a.noise)
        {
            a.noise = noise;
            set_height_pct(s_el.no_fill, noise);
        }
        if (light != a.light)
        {
            a.light = light;
            set_height_pct(s_el.li_fill, light);
        }
    }

    // --- the car's health ------------------------------------------------
    const int car_on = s.car_on ? 1 : 0;
    if (car_on != a.car_on)
    {
        a.car_on = car_on;
        set_display(s_el.car, car_on != 0);
    }
    if (car_on)
    {
        const int car = to_pct(s.car_health);
        if (car != a.car)
        {
            a.car = car;
            set_width_pct(s_el.car_fill, car);
        }
    }

    // --- the hint under the crosshair ------------------------------------
    // (ui_test hint <id> shows a string of the table for a few seconds instead)
    const bool test_hint = Device.dwTimeGlobal < s_test_hint_until;
    const xr_string& hint = test_hint ? s_test_hint : s.hint;
    const int hint_on = ((s.hint_on || test_hint) && !hint.empty()) ? 1 : 0;
    if (hint_on != a.hint_on)
    {
        a.hint_on = hint_on;
        set_display(s_el.hint, hint_on != 0, "flex"); // rows centred, see hud.rcss
    }
    if (hint_on && hint != a.hint)
    {
        a.hint = hint;
        if (s_el.hint)
            s_el.hint->SetInnerRML(hint_markup(hint.c_str()).c_str());
    }

    // --- the minimap's frame, compass and distance -----------------------
    const int map_on = s.map_on ? 1 : 0;
    if (map_on != a.map_on)
    {
        a.map_on = map_on;
        set_display(s_el.map, map_on != 0);
    }
    if (map_on)
    {
        // ⚠The legacy static was handed -h and its rotation runs the other way
        // round from CSS (S2DVert::rotate_pt turns +angle anticlockwise on a
        // y-down screen), so the camera's own heading is the CSS angle as is.
        const int tenths = iFloor(rad2deg(s.map_heading) * 10.f + 0.5f);
        if (tenths != a.heading && s_el.compass)
        {
            a.heading = tenths;
            char buf[48];
            xr_sprintf(buf, sizeof(buf), "rotate(%.1fdeg)", float(tenths) * 0.1f);
            s_el.compass->SetProperty("transform", buf);
        }
        if (s.map_dist != a.dist)
        {
            a.dist = s.map_dist;
            if (s_el.dist)
                s_el.dist->SetInnerRML(markup_safe(s.map_dist.c_str()).c_str());
        }
    }

    // --- the pick-up picture ---------------------------------------------
    const int pickup_on = (s.pickup_on && s.pickup_parts > 0) ? 1 : 0;
    if (pickup_on != a.pickup_on)
    {
        a.pickup_on = pickup_on;
        set_display(s_el.pickup, pickup_on != 0);
    }
    if (pickup_on && s.pickup_serial != a.pickup_serial)
    {
        a.pickup_serial = s.pickup_serial;
        // The fit needs the box's size, and a box that was display:none a moment
        // ago has none until the next layout -- so lay out now. Once per new item.
        s_doc->UpdateDocument();
        build_pickup(s);
    }

    // --- the flashing icons ----------------------------------------------
    for (int i = 0; i < UIRml::HudState::FLASHES; ++i)
    {
        const int on = s.flash_on[i] ? 1 : 0;
        if (on != a.flash_on[i])
        {
            a.flash_on[i] = on;
            set_display(s_el.flash[i], on != 0);
        }
    }
}
} // namespace

// ======================================================= the quick slots ====
//
// ⭐06-10: the mod's quick slots (ogsr_quick_slots.script) were four legacy statics
// in a COLUMN, 33x33 UI units each, shifted as a group into a box. The True
// Stalker row is drawn by the document instead: the script still makes its
// statics (they decide whether the row is up -- the cells adopt them), and hands
// over what each slot holds through rml.hud_slot. Remembered here, so a rebuilt
// document gets them back without the script having to know.
namespace
{
struct HudSlot
{
    xr_string section;
    int count{};
    bool have{};
    bool set{};
};
HudSlot s_hslots[8];

void apply_hud_slot(int n)
{
    if (!s_doc || n < 1 || n > int(std::size(s_hslots)))
        return;
    char id[16];
    xr_sprintf(id, sizeof(id), "hqs-%d", n);
    Rml::Element* cell = s_doc->GetElementById(id);
    Rml::Element* pic = nullptr;
    Rml::Element* cnt = nullptr;
    if (cell)
    {
        Rml::ElementList l;
        cell->GetElementsByClassName(l, "hq-pic");
        pic = l.empty() ? nullptr : l.front();
        l.clear();
        cell->GetElementsByClassName(l, "hq-cnt");
        cnt = l.empty() ? nullptr : l.front();
    }
    if (!pic)
        return;
    const HudSlot& h = s_hslots[n - 1];
    const bool full = h.set && !h.section.empty() && pSettings->section_exist(h.section.c_str());
    cell->SetClass("empty", !full);
    cell->SetClass("none", full && !h.have);
    if (!full)
    {
        pic->SetProperty("decorator", "none");
        if (cnt)
            cnt->SetInnerRML("");
        return;
    }
    // The picture fitted into the box the markup gives .hq-pic (data-w x data-h dp),
    // grid cells kept square: one cell is 50 units of the item's icon.
    const char* sect = h.section.c_str();
    const shared_str from = READ_IF_EXISTS(pSettings, r_string, sect, "icon_section", sect);
    const float gw = std::max(1.f, READ_IF_EXISTS(pSettings, r_float, from, "inv_grid_width", 1.f));
    const float gh = std::max(1.f, READ_IF_EXISTS(pSettings, r_float, from, "inv_grid_height", 1.f));
    const float bw = pic->GetAttribute<float>("data-w", 60.f), bh = pic->GetAttribute<float>("data-h", 46.f);
    const float k = std::min(bw / gw, bh / gh);
    const char* file = pSettings->line_exist(from, "icon_name") ? pSettings->r_string(from, "icon_name") : "";
    pic->SetProperty("decorator", ItemDecorator(sect, file).c_str());
    pic->SetProperty("width", Rml::String(std::to_string(int(gw * k + 0.5f)) + "dp"));
    pic->SetProperty("height", Rml::String(std::to_string(int(gh * k + 0.5f)) + "dp"));
    if (cnt)
        cnt->SetInnerRML(h.count > 0 ? (Rml::String("x") + std::to_string(h.count)).c_str() : "");
}

void apply_hud_slots()
{
    for (int i = 1; i <= int(std::size(s_hslots)); ++i)
        apply_hud_slot(i);
}
} // namespace

void UIRml::LuaHudSlot(int n, LPCSTR section, int count, bool have)
{
    if (n < 1 || n > int(std::size(s_hslots)))
        return;
    HudSlot& h = s_hslots[n - 1];
    h.section = section ? section : "";
    h.count = count;
    h.have = have;
    h.set = true;
    apply_hud_slot(n);
}

// ================================================================= the API ====

bool UIRml::HudActive() { return s_doc && UIRml::Ready(); }

void UIRml::HudDrawn() { s_drawn_frame = Device.dwFrame; }

void UIRml::HudPush(const HudState& s)
{
    s_state = s;
    s_pushed_frame = Device.dwFrame;
}

bool UIRml::HudAdopts(const shared_str& name)
{
    if (!UIRml::HudActive())
        return false;
    for (const Adopted& a : s_adopted)
        for (const shared_str& n : a.names)
            if (n == name)
                return true;
    return false;
}

void UIRml::HudNews(const GAME_NEWS_DATA& n)
{
    xr_string who, text;
    news_split(CStringTable().translate(n.news_text).c_str(), who, text);
    const xr_string pic = news_picture(n.texture_name.c_str(), n.tex_rect);

    NewsCard c;
    c.rml = pic.empty() ? "<div class=\"nw-ico none\"/>" : "<div class=\"nw-ico\">" + pic + "</div>";
    c.rml += "<div class=\"nw-body\"><div class=\"nw-head\">";
    if (!who.empty())
        c.rml += "<span class=\"nw-who\">" + markup_safe(who.c_str()) + "</span>";
    c.rml += "<span class=\"nw-time\">" + markup_safe(InventoryUtilities::GetTimeAsString(n.receive_time, InventoryUtilities::etpTimeToMinutes).c_str()) + "</span></div>";
    c.rml += "<div class=\"nw-text\">" + UIRml::LuaText(text.c_str()) + "</div></div>";
    c.left_ms = (n.show_time > 0 ? n.show_time : DEFAULT_NEWS_SHOW_TIME) + kNewsFadeMs;
    s_news.push_back(std::move(c));

    // Too many standing: the oldest of them starts fading now. A long dialog may pile news up meanwhile -- what is
    // past twice the column is dropped unseen.
    int standing = 0;
    for (const NewsCard& x : s_news)
        standing += x.out ? 0 : 1;
    for (NewsCard& x : s_news)
        if (standing > kNewsMax && !x.out)
        {
            x.left_ms = std::min(x.left_ms, kNewsFadeMs);
            --standing;
        }
    while (s_news.size() > size_t(kNewsMax * 2))
        news_drop(0);

    Msg("~ [rml] hud: news [%s] %d ms, %u on the column", who.c_str(), n.show_time, u32(s_news.size()));
}

void UIRml::HudTestNews(LPCSTR text)
{
    GAME_NEWS_DATA n;
    // cp1251 bytes, "Сидорович:" and "Проверка сообщения." -- a Cyrillic literal does not pass the 1252 code page
    n.news_text = text && *text ? text
                                : "%c[255,238,155,23]\xD1\xE8\xE4\xEE\xF0\xEE\xE2\xE8\xF7:\\n%c[default]\xCF\xF0\xEE\xE2\xE5\xF0\xEA\xE0 \xF1\xEE\xEE\xE1\xF9\xE5\xED\xE8\xFF.";
    n.texture_name = "ui\\ui_iconsTotal";
    n.tex_rect.set(0.f, 188.f, 83.f, 47.f);
    n.show_time = 8000;
    if (g_pGameLevel)
        n.receive_time = Level().GetGameTime();
    HudNews(n);
}

bool UIRml::HudMapRound() { return s_el.map_view && s_el.map_view->HasAttribute("data-round"); }

bool UIRml::HudMapView(Frect& r01)
{
    if (!s_doc || !s_shown || !s_el.map_view || s_applied.map_on != 1)
        return false;
    Rml::Context* ctx = s_doc->GetContext();
    if (!ctx)
        return false;
    const Rml::Vector2i dims = ctx->GetDimensions();
    // Last frame's layout: the box only moves when the screen does, and a frame
    // late on a resolution change is invisible.
    const Rml::Vector2f pos = s_el.map_view->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = s_el.map_view->GetBox().GetSize(Rml::BoxArea::Border);
    if (dims.x <= 0 || dims.y <= 0 || size.x < 1.f || size.y < 1.f)
        return false;
    r01.set(pos.x / dims.x, pos.y / dims.y, (pos.x + size.x) / dims.x, (pos.y + size.y) / dims.y);
    return true;
}

void UIRmlInternal::HudPreUpdate()
{
    // ⭐A level that has never opened a panel has no RmlUi yet; the HUD is the
    // first thing to want it, so it brings it up. Init() refuses without a
    // renderer, which is the headless server -- and is retried slowly rather than
    // every frame there.
    if (UIRml::Ready())
        return;
    if (Device.dwFrame < s_next_try)
        return;
    if (!UIRml::Init())
        s_next_try = Device.dwFrame + 600;
}

void UIRmlInternal::HudFrame()
{
    if (!s_doc)
    {
        if (Device.dwFrame < s_next_try)
            return;
        if (!build())
            return;
    }

    // On screen exactly while the legacy window is: it is drawn only when the
    // indicators are shown, the HUD is not hidden and there is an actor. Two
    // frames of grace so a frame the legacy window skips does not blink the HUD.
    // ⚠Never over the inventory: that screen runs the context in another unit.
    const bool recent = (s_drawn_frame != u32(-1)) && (Device.dwFrame - s_drawn_frame <= 2);
    const bool want = recent && s_state.actor && !UIRmlInternal::InvVisible();

    if (want && !s_shown)
    {
        s_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        s_shown = true;
        s_applied.reset(); // whatever was on the DOM may be stale
        s_news_clock = 0; // the news did not age while the HUD was away
    }
    else if (!want && s_shown)
        hide_doc();

    if (s_shown)
        apply();
    wheel_frame();
}

void UIRmlInternal::HudHide() { hide_doc(); }

void UIRmlInternal::HudShutdown()
{
    // Rml::Shutdown has released the document by now; only forget it. The HUD
    // (and its statics) may be gone already, so nothing is put back either.
    forget_takeovers(false);
    s_doc = nullptr;
    s_shown = false;
    s_el = Els();
    s_applied.reset();
    s_news.clear();
}

// ====================================================== the weapon wheel ====
//
// ⭐06-10 (the owner: "hold Y for a selector like RDR2 -- knife, bolt, binoculars").
// The wpn_next key (X on the pad since the evening of 06-10 -- reload went to Y; Y on the keyboard) is taken over here:
//   tap   -- the Switch: the next weapon slot, as the mouse wheel does (Y used
//            to reach kWPN_NEXT, which in OGSR is the next AMMO TYPE, and with
//            1..4 moved to the D-pad there was no way left to draw a weapon at
//            all); the keyboard: kWPN_NEXT as before.
//   hold  -- the wheel (wheel.rml): eight sectors around the screen centre, the
//            pointer motion (the right stick on the Switch) pushes a virtual
//            stick that picks one, letting go takes it. Time runs at a third.
// Only in play: not over a screen, not with the console or a dialog up.
namespace
{
enum class Wheel
{
    Idle,
    Pressed,
    Open,
};
Wheel s_wh = Wheel::Idle;
u32 s_wh_t0 = 0; // Device.dwTimeContinual at the press (not slowed by the wheel itself)
int s_wh_key = 0;
Rml::ElementDocument* s_wh_doc = nullptr;
float s_wh_x = 0.f, s_wh_y = 0.f; // the virtual stick, in pointer counts
int s_wh_sel = -1;
float s_wh_tf = 1.f; // the time factor to give back
// The left stick (06-10, the owner: "pick with the left stick, not the right"). On the
// Switch it arrives as W/A/S/D (xr_input_sdl.cpp), so the wheel reads the movement
// keys: which of forward/back/left/right is held is watched ALWAYS (the wheel must
// know what is down when it opens), and while it is open they pick a sector -- eight
// from the combinations -- and do not reach the actor, who stops and walks on after.
bool s_mv[4]{}; // forward, back, left, right
constexpr EGameActions kMoveAct[4] = {kFWD, kBACK, kL_STRAFE, kR_STRAFE};
int move_index(int dik)
{
    for (int i = 0; i < 4; ++i)
        if (is_binded(kMoveAct[i], dik))
            return i;
    return -1;
}
constexpr u32 kWheelHoldMs = 220;
constexpr float kWheelRadius = 140.f, kWheelDead = 45.f;

// Clockwise from the top. slot >= 0: that inventory slot; -1 put the weapon away;
// -2 the next ammo type (the only way to it on the pad now).
constexpr int kSectors[8] = {SECOND_WEAPON_SLOT, FIRST_WEAPON_SLOT, KNIFE_SLOT, GRENADE_SLOT, BOLT_SLOT, APPARATUS_SLOT, -1, -2};

Rml::Element* wh_sector(int i)
{
    char id[8];
    xr_sprintf(id, sizeof(id), "wh-%d", i);
    return s_wh_doc ? s_wh_doc->GetElementById(id) : nullptr;
}

void wheel_press(CActor* a, int action)
{
    a->IR_OnKeyboardPress(action);
    a->IR_OnKeyboardRelease(action);
}

// The pad's tap: the next slot that holds something, round and round. Not
// CActor::OnNextWeaponSlot -- its list ends in the PDA slot, so tapping on and on
// opened the PDA (06-10), and it stops at the end instead of wrapping.
[[maybe_unused]] void wheel_next_weapon(CActor* a) // the pad only (__SWITCH__ below)
{
    static constexpr u32 order[] = {KNIFE_SLOT, FIRST_WEAPON_SLOT, SECOND_WEAPON_SLOT, GRENADE_SLOT, APPARATUS_SLOT, BOLT_SLOT};
    constexpr int n = int(std::size(order));
    u32 cur = a->inventory().GetActiveSlot();
    if (cur == NO_ACTIVE_SLOT)
        cur = a->inventory().GetPrevActiveSlot();
    int at = -1;
    for (int i = 0; i < n; ++i)
        if (order[i] == cur)
            at = i;
    for (int step = 1; step <= n; ++step)
    {
        const u32 slot = order[(at + step + n) % n];
        if (a->inventory().ItemFromSlot(slot) && slot != a->inventory().GetActiveSlot())
        {
            wheel_press(a, kWPN_1 + int(slot));
            return;
        }
    }
}

bool wheel_allowed()
{
    CActor* a = Actor();
    return a && a->g_Alive() && !Device.Paused() && !UIRmlInternal::InvVisible() && !UIRml::WantsInput() && HUD().GetUI() &&
        !HUD().GetUI()->MainInputReceiver();
}

// What each sector shows: the picture, the short name, rounds for a gun.
void wheel_fill()
{
    CActor* a = Actor();
    if (!a || !s_wh_doc)
        return;
    for (int i = 0; i < 8; ++i)
    {
        Rml::Element* e = wh_sector(i);
        if (!e)
            continue;
        const int slot = kSectors[i];
        Rml::String rml;
        bool none = false;
        if (slot >= 0)
        {
            PIItem it = a->inventory().ItemFromSlot(u32(slot));
            none = !it;
            if (it)
            {
                const char* sect = it->object().cNameSect().c_str();
                const shared_str from = READ_IF_EXISTS(pSettings, r_string, sect, "icon_section", sect);
                const float gw = std::max(1.f, READ_IF_EXISTS(pSettings, r_float, from, "inv_grid_width", 1.f));
                const float gh = std::max(1.f, READ_IF_EXISTS(pSettings, r_float, from, "inv_grid_height", 1.f));
                const float k = std::min(120.f / gw, 46.f / gh); // the picture box, dp
                const char* file = pSettings->line_exist(from, "icon_name") ? pSettings->r_string(from, "icon_name") : "";
                char pic[512];
                xr_sprintf(pic, sizeof(pic), "<div class=\"wh-pic\" style=\"decorator: %s; width: %.0fdp; height: %.0fdp;\"/>", ItemDecorator(sect, file).c_str(), gw * k,
                           gh * k);
                rml = pic;
                rml += "<div class=\"wh-name\">" + Rml::String(markup_safe(it->NameShort()).c_str()) + "</div>";
                if (auto wm = smart_cast<CWeaponMagazined*>(it); wm && !wm->m_ammoTypes.empty()) // binoculars are "magazined" too, with no rounds
                {
                    const int ae = wm->GetAmmoElapsed();
                    rml += "<div class=\"wh-ammo\">" + std::to_string(ae) + " / " +
                        (wm->unlimited_ammo() ? Rml::String("--") : std::to_string(_max(0, wm->GetAmmoCurrent() - ae))) + "</div>";
                }
            }
            else
                rml = "<div class=\"wh-name\">" + e->GetAttribute<Rml::String>("data-t-empty", "") + "</div>";
        }
        else
        {
            // the words are in the markup: a Cyrillic literal here would not build
            rml = "<div class=\"wh-glyph\"/><div class=\"wh-name\">" + e->GetAttribute<Rml::String>("data-t", "") + "</div>";
            if (slot == -2)
            {
                auto wm = smart_cast<CWeaponMagazined*>(a->inventory().ActiveItem());
                none = !wm || wm->m_ammoTypes.size() < 2;
            }
            else
                none = !a->inventory().ActiveItem();
        }
        e->SetInnerRML(rml);
        e->SetClass("none", none);
        e->SetClass("sel", false);
    }
}

void wheel_mark()
{
    for (int i = 0; i < 8; ++i)
        if (Rml::Element* e = wh_sector(i))
            e->SetClass("sel", i == s_wh_sel);
    Rml::Element* c = s_wh_doc ? s_wh_doc->GetElementById("wh-c") : nullptr;
    if (!c)
        return;
    Rml::String t;
    if (Rml::Element* e = s_wh_sel >= 0 ? wh_sector(s_wh_sel) : nullptr)
    {
        Rml::ElementList l;
        e->GetElementsByClassName(l, "wh-name");
        if (!l.empty())
            t = l.front()->GetInnerRML();
    }
    c->SetInnerRML(t);
}

void wheel_open()
{
    if (!s_wh_doc)
    {
        s_wh_doc = BuildDocument("wheel.rml");
        if (!s_wh_doc)
        {
            s_wh = Wheel::Idle;
            return;
        }
        AttachItemSprites(s_wh_doc);
    }
    s_wh_x = s_wh_y = 0.f;
    s_wh_sel = -1;
    wheel_fill();
    wheel_mark();
    s_wh_doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    s_wh_tf = Device.time_factor();
    Device.time_factor(s_wh_tf * 0.3f);
    s_wh = Wheel::Open;
    // the left stick is the selector now: stop the walk it was doing
    if (CActor* a = Actor())
        for (int i = 0; i < 4; ++i)
            if (s_mv[i])
                a->IR_OnKeyboardRelease(kMoveAct[i]);
    Msg("~ [rml] wheel: open");
}

void wheel_close(bool take)
{
    if (s_wh_doc)
        s_wh_doc->Hide();
    const bool was_open = s_wh == Wheel::Open;
    if (was_open)
        Device.time_factor(s_wh_tf);
    s_wh = Wheel::Idle;
    CActor* a = Actor();
    // and walk on where the stick still points
    if (was_open && a)
        for (int i = 0; i < 4; ++i)
            if (s_mv[i])
                a->IR_OnKeyboardPress(kMoveAct[i]);
    if (!take || s_wh_sel < 0 || !a)
        return;
    const int slot = kSectors[s_wh_sel];
    Msg("~ [rml] wheel: take sector %d (slot %d)", s_wh_sel, slot);
    if (slot >= 0)
    {
        // kWPN_1 + slot toggles: asked for what is already in hand, it would put it away
        if (a->inventory().ItemFromSlot(u32(slot)) && a->inventory().GetActiveSlot() != u32(slot))
            wheel_press(a, kWPN_1 + slot);
    }
    else if (slot == -1)
    {
        const u32 cur = a->inventory().GetActiveSlot();
        if (cur != NO_ACTIVE_SLOT && a->inventory().ActiveItem() && cur <= BOLT_SLOT)
            wheel_press(a, kWPN_1 + int(cur));
    }
    else
        wheel_press(a, kWPN_NEXT);
}

void wheel_frame()
{
    if (s_wh == Wheel::Idle)
        return;
    if (!wheel_allowed())
    {
        wheel_close(false);
        return;
    }
    if (s_wh == Wheel::Pressed && Device.dwTimeContinual - s_wh_t0 >= kWheelHoldMs)
        wheel_open();
}
} // namespace

bool UIRml::WheelKey(int dik, bool down)
{
    if (const int mv = move_index(dik); mv >= 0)
    {
        s_mv[mv] = down;
        if (s_wh != Wheel::Open)
            return false; // only watched
        if (down)
        {
            // 0 = up, clockwise; [back - forward + 1][right - left + 1]
            static constexpr int sec[3][3] = {{7, 0, 1}, {6, -1, 2}, {5, 4, 3}};
            const int sel = sec[int(s_mv[1]) - int(s_mv[0]) + 1][int(s_mv[3]) - int(s_mv[2]) + 1];
            if (sel >= 0 && sel != s_wh_sel)
            {
                s_wh_sel = sel;
                wheel_mark();
            }
        }
        return true; // the actor stands while the wheel is open
    }
    if (down)
    {
        if (s_wh != Wheel::Idle)
            return dik == s_wh_key;
        if (!is_binded(kWPN_NEXT, dik) || !wheel_allowed())
            return false;
        s_wh = Wheel::Pressed;
        s_wh_key = dik;
        s_wh_t0 = Device.dwTimeContinual;
        return true;
    }
    if (s_wh == Wheel::Idle || dik != s_wh_key)
        return false;
    if (s_wh == Wheel::Pressed)
    {
        s_wh = Wheel::Idle;
        if (CActor* a = Actor())
        {
#ifdef __SWITCH__
            wheel_next_weapon(a);
#else
            wheel_press(a, kWPN_NEXT);
#endif
        }
        return true;
    }
    wheel_close(true);
    return true;
}

bool UIRml::WheelMouse(int dx, int dy)
{
    if (s_wh != Wheel::Open)
        return false;
    s_wh_x += float(dx);
    s_wh_y += float(dy);
    const float len = _sqrt(s_wh_x * s_wh_x + s_wh_y * s_wh_y);
    if (len > kWheelRadius)
    {
        s_wh_x *= kWheelRadius / len;
        s_wh_y *= kWheelRadius / len;
    }
    if (len > kWheelDead)
    {
        // 0 = straight up, clockwise, 45 degrees a sector
        float ang = atan2f(s_wh_x, -s_wh_y);
        if (ang < 0.f)
            ang += PI_MUL_2;
        const int sel = int(ang / (PI_MUL_2 / 8.f) + 0.5f) % 8;
        if (sel != s_wh_sel)
        {
            s_wh_sel = sel;
            wheel_mark();
        }
    }
    return true; // the view does not turn while choosing
}

bool UIRml::WheelOpen() { return s_wh == Wheel::Open; }

// For the unattended driver (ui_test wheel ...): hold, push towards a sector, let go.
void UIRml::WheelTest(int sector, bool release)
{
    if (s_wh == Wheel::Idle)
    {
        if (!wheel_allowed())
            return;
        s_wh = Wheel::Pressed;
        s_wh_key = -1;
        s_wh_t0 = Device.dwTimeContinual - kWheelHoldMs;
        wheel_open();
    }
    if (sector >= 0 && sector < 8)
    {
        const float a = float(sector) * PI_MUL_2 / 8.f;
        s_wh_x = s_wh_y = 0.f;
        WheelMouse(int(_sin(a) * kWheelRadius), int(-_cos(a) * kWheelRadius));
    }
    if (release)
        wheel_close(true);
}

void UIRml::HudTestHint(LPCSTR id)
{
    s_test_hint = (id && id[0]) ? CStringTable().translate(id).c_str() : "";
    s_test_hint_until = Device.dwTimeGlobal + 8000;
}

#include "StdAfx.h"
#include "ui_rml.h"

// The main menu on RmlUi: a port of the legacy front end -- ui_mm_main.xml plus
// ui_main_menu.script, ui_mm_opt*.xml/.script and ui_mm_save/load_dlg.xml --
// onto HTML and CSS.
//
// ⚠⚠A SECOND HOST. Everything else on this track hangs off the level: Update
// from Level.cpp, Render from CHUDManager::RenderUI, input from
// Level_input.cpp. In the menu not one of those runs -- CMainMenu puts ITSELF
// into Device.seqRender, takes input through its own IR_* handlers, and exists
// before any level does. Hence the separate entry points at the bottom, and
// hence this file rather than more of ui_rml.cpp.
//
// ⭐WHAT LIVES WHERE. The screens, their text keys, their option rows and the
// order of everything are CONTENT -- gamedata\config\ui_rml\main_menu.rml and
// .rcss -- exactly as they were content in the XML before. This file supplies
// only what markup cannot know: which button set the game state calls for, what
// a console entry currently holds, which saves are on disk, what a key is bound
// to. Editing a caption or moving a row does not come back through here.
#include "xr_platform_keys.h"
#include "xr_level_controller.h"
#include "ui_defs.h" // ui_shader -- the background .ogm is a UI shader
#include "ui/UITextureMaster.h" // the background's rectangle inside its texture
#include "../Include/xrRender/UIRender.h"
#include "../xr_3da/XR_IOConsole.h"
#include "../xr_3da/xr_ioc_cmd.h"
#include "../xr_3da/IGame_Level.h"
#include "MainMenu.h"
#include "Actor.h"
#include "saved_game_wrapper.h"
#include "string_table.h"
#include "date_time.h" // split_time -- the in-game clock a save was taken at
#include "ui/xrUIXmlParser.h" // ui_keybinding.xml, the same file the old screen read
#include "Level.h" // the in-game hour, which decides the day/night background
#ifdef _MSC_VER
#include <shellapi.h> // the three links along the bottom of the start screen
#endif
#include <sys/stat.h>

// ⚠windowsx.h (via the precompiled header) defines these as one-argument macros
// over GetWindow(); Rml::Element declares methods with the same names and the
// macro turns the declaration into a comma expression. See ui_rml.cpp.
#undef GetNextSibling
#undef GetPreviousSibling
#undef GetPrevSibling
#undef GetFirstChild

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

extern string_path g_last_saved_game;
void start_tutorial(LPCSTR name); // game_sv_base_script.cpp -- what "credits" is

// 18-08: THE GATE IS GONE, and with it the legacy main menu.
//
// `ui_new_menu` existed to keep the mod's Lua screen (clsid MAIN_MNU) reachable
// while this port was unproven. It shipped off, then on by default, and is now
// removed together with what it guarded: the MAIN_MNU branch in
// CMainMenu::Activate, ui/UIMMShniaga.* (494 lines that nothing else used), its
// luabind class and CScriptXmlInit::InitMMShniaga.
//
// ⚠What was NOT removed, and must not be: CUIMessageBox/CUIMessageBoxEx. It is
// not part of the menu -- UIGameSP builds the change-level prompt on it and mod
// scripts use half of message_box.xml's templates during play. Deleting it
// breaks those at RUNTIME, with nothing failing at build time to warn you.
//
// ⚠The mod's Lua screen itself was never in this repository (ui_main_menu.script
// and ui_mm_*.xml live in its archives); the engine only ever instantiated it by
// clsid. Removing that instantiation is all this side can do -- the files stay
// in the archives, unreferenced.

namespace
{
using namespace UIRmlInternal;

Rml::ElementDocument* s_menu = nullptr;

// ⚠⚠EVERY action is DEFERRED by one frame, never run inside the click.
// Almost everything a menu button does is drastic by nature -- quit, disconnect,
// start server, switch screen -- and every one of those deletes or rebuilds the
// very tree RmlUi is still dispatching the click on. The picker learned the
// narrow form of this (Context::Update must not run from a handler); this is the
// general form, and it costs one string and one branch.
xr_string s_pending;

void queue(const char* action)
{
    if (!s_pending.empty())
        return; // first click of a frame wins; a second is a double-fire
    s_pending = action;
}

// ============================================================== background ====
//
// ⚠⚠NOT a file-scope ui_shader object, and the reason is written out in
// cl_zone_debug.cpp: ui_shader is FactoryPtr<IUIShader>, whose constructor asks
// the render factory for an object the moment it is constructed. At namespace
// scope that runs during static initialisation of the module -- long before any
// renderer exists -- and the headless server, which links this same code, died
// instantly without managing to open a log.
ui_shader* s_video = nullptr;
xr_string s_video_name;
// The part of the video texture that is actually picture. See ensure_video.
Frect s_video_rect{0.f, 0.f, 0.f, 0.f};

// ⚠Built from the frame pass and never from the render pass -- cl_zone_debug.cpp
// records what happens otherwise: a UI shader created inside the render pass
// never resolved its texture, and the renderer said so, "SetShader:
// descriptor=NULL, using white fallback".
void ensure_video(const char* name, const char* rect_spec)
{
    if (!name || !name[0])
        return;
    if (s_video && s_video_name == name)
        return;

    // ⚠The rectangle first, because the name may or may not carry one. The old
    // XML spelled it inline next to the texture -- <texture x y width height> --
    // and that is what this attribute is: the same four numbers, same meaning.
    s_video_rect.set(0.f, 0.f, 0.f, 0.f);
    if (rect_spec && rect_spec[0])
    {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        if (sscanf(rect_spec, "%f %f %f %f", &x, &y, &w, &h) == 4 && w > 0.f && h > 0.f)
            s_video_rect.set(x, y, x + w, y + h);
        else
            Msg("! [rml] data-video-rect '%s' is not four numbers -- using the whole texture", rect_spec);
    }

    // ⚠⚠THE VIDEO TEXTURE IS BIGGER THAN THE VIDEO. Theora pads the frame up to
    // a multiple of 16, and this one is padded a great deal more than that: the
    // background is 876x400 inside a 1024x1024 texture. The old screen never met
    // this because it drew the name `ui_mm_main_back_crop`, which is a texture
    // description carrying exactly that rectangle -- and a full-texture quad
    // without it shows the picture squashed into the top-left corner with junk
    // around it, which reads as "the background is striped" and points nowhere.
    //
    // So the same description is consulted here. A name with no entry means the
    // whole texture, which is the right answer for a plain image.
    // ⚠Looked up in the table directly rather than through GetTextureFileName:
    // that one logs a complaint for a name it does not know, and "not a sprite,
    // just a file" is the normal case here, not a fault.
    s_video_name = name;
    const auto& table = CUITextureMaster::Textures();
    auto it = table.find(name);
    if (it != table.end())
    {
        s_video_rect = it->second.rect;
        name = it->second.file.c_str();
    }

    // Same slash rule as every other image name -- content may spell a path
    // either way, the archives index only one of them. See ui_rml.cpp.
    xr_string file = name;
    for (char& c : file)
        if (c == '/')
            c = '\\';

    xr_delete(s_video);
    s_video = xr_new<ui_shader>();
    // ⭐The .ogm needs no special handling here: the renderer's UI shader detects
    // a Theora-backed texture by extension, starts it looping, and decodes one
    // frame per bind (vk_RenderFactory.cpp).
    (*s_video)->create("hud\\default", file.c_str());
    Msg("~ [rml] menu background: '%s' -> '%s' rect %.0f,%.0f %.0fx%.0f", s_video_name.c_str(), file.c_str(), s_video_rect.x1, s_video_rect.y1,
        s_video_rect.width(), s_video_rect.height());
}

// The background, drawn as one screen-filling quad under the document.
void draw_video()
{
    if (!UIRender || !s_video || !(*s_video)->inited())
        return;

    // ⭐Bound BEFORE the primitive rather than after it, unlike the legacy UI,
    // and both halves of that are wanted: SetShader is what ticks the Theora
    // decode (this renderer decodes inside GetDescriptorSet, deduped per frame),
    // and it is also what makes GetActiveTextureResolution answer for this
    // texture. StartPrimitive does not disturb the binding.
    UIRender->SetShader(**s_video);

    Fvector2 ts{1.f, 1.f};
    UIRender->GetActiveTextureResolution(ts);
    ts.x = _max(1.f, ts.x);
    ts.y = _max(1.f, ts.y);

    // The picture inside the texture: whatever the markup said, else all of it.
    Frect pic = s_video_rect;
    if (pic.width() <= 0.f || pic.height() <= 0.f)
        pic.set(0.f, 0.f, ts.x, ts.y);

    // ⭐⭐THE ASPECT SLICE, and it is the old menu's arithmetic rather than mine.
    // ui_main_menu.script:InitBackground picks a horizontal slice of the clip
    // whose width scales with the screen's aspect ratio, centres it, and
    // stretches that over the whole screen:
    //
    //     mult  = aspect > 2 ? 0.40 : 0.5625
    //     width = aspect * 1024 * mult          (in texture pixels)
    //     x     = (1024 - width) / 2
    //
    // On 16:9 that is exactly the full 1024 and no offset; on a wider screen it
    // takes a narrower slice so the picture is not stretched into a smear. A
    // cover-crop of my own looked better on one monitor and wrong on the next,
    // which is the whole reason to copy the formula instead of inventing one.
    const float aspect = float(Device.dwWidth) / float(_max(1u, Device.dwHeight));
    const float mult = (aspect > 2.f) ? 0.40f : 0.5625f;
    float slice_w = aspect * pic.width() * mult;
    slice_w = _min(slice_w, pic.width());
    const float sx = pic.x1 + (pic.width() - slice_w) * 0.5f;

    const float u0 = sx / ts.x, v0 = pic.y1 / ts.y;
    const float u1 = (sx + slice_w) / ts.x, v1 = pic.y2 / ts.y;

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Msg("~ [rml] menu background texture %.0fx%.0f, picture %.0fx%.0f, aspect %.3f, uv %.3f,%.3f..%.3f,%.3f", ts.x, ts.y, pic.width(), pic.height(), aspect,
            u0, v0, u1, v1);
    }

    const float w = float(Device.dwWidth), h = float(Device.dwHeight);

    const u32 c = color_rgba(255, 255, 255, 255);
    UIRender->StartPrimitive(6, IUIRender::ptTriList, IUIRender::pttTL);
    UIRender->PushPoint(0.f, 0.f, 0.f, c, u0, v0);
    UIRender->PushPoint(w, 0.f, 0.f, c, u1, v0);
    UIRender->PushPoint(w, h, 0.f, c, u1, v1);
    UIRender->PushPoint(0.f, 0.f, 0.f, c, u0, v0);
    UIRender->PushPoint(w, h, 0.f, c, u1, v1);
    UIRender->PushPoint(0.f, h, 0.f, c, u0, v1);
    UIRender->FlushPrimitive();
}

// ============================================================ DOM helpers ====

Rml::Element* by_id(const char* id) { return s_menu ? s_menu->GetElementById(id) : nullptr; }

// ⚠⚠X-RAY STRETCHES ITS 1024x768 UI OVER THE WHOLE SCREEN, and anisotropically:
// on 16:9 every element ends up a third wider, relative to its height, than the
// numbers in the XML suggest. Letterboxing a 4:3 stage instead -- which is what
// this port did first -- looks tidier and is WRONG in a way that shows: anything
// anchored to an edge lands hundreds of pixels inside the frame (the three links
// meant for the bottom-left corner drift towards the middle), and the screen as
// a whole reads as "not the same menu" without any single thing being nameable.
//
// dp is one scalar and cannot carry two scales, so the vertical one is dp
// (height / 768) and the horizontal remainder is a single transform on the root.
void stretch_stage()
{
    Rml::Element* stage = by_id("stage");
    if (!stage)
        return;
    // ⚠One formula, one place: the font engine needs the SAME number inverted,
    // to keep glyphs out of this stretch (X-Ray never stretched them). Two
    // copies of it would drift and the symptom would be squashed text.
    const float k = UIRmlInternal::StageStretchX();
    string64 buf;
    xr_sprintf(buf, sizeof(buf), "scaleX(%f)", k);
    stage->SetProperty("transform", buf);
    stage->SetProperty("transform-origin", "left top");
}

void show(const char* id, bool on)
{
    if (Rml::Element* e = by_id(id))
        e->SetProperty("display", on ? "block" : "none");
}

// ⚠⚠RmlUi's `text-transform: uppercase` IS ASCII-ONLY (StringUtilities::ToUpper
// touches a..z and nothing else), so a Cyrillic caption under it stays lowercase
// and only its Latin words shout -- "Выйти в WINDOWS". The skin asks for capitals
// on buttons, tabs and headings, so the capitals are made here, in the text
// itself: every text node whose COMPUTED text-transform is uppercase gets its
// Cyrillic raised. Done once after the document is laid out (s_caps_pending) and
// again whenever set_text replaces a caption.
bool caps_utf8(xr_string& s)
{
    bool changed = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z')
        {
            s[i] = char(c - 32);
            changed = true;
        }
        else if (c == 0xD0 && i + 1 < s.size())
        {
            unsigned char n = (unsigned char)s[i + 1];
            if (n >= 0xB0 && n <= 0xBF) // а..п -> А..П
            {
                s[i + 1] = char(n - 0x20);
                changed = true;
            }
            ++i;
        }
        else if (c == 0xD1 && i + 1 < s.size())
        {
            unsigned char n = (unsigned char)s[i + 1];
            if (n >= 0x80 && n <= 0x8F) // р..я -> Р..Я
            {
                s[i] = char(0xD0);
                s[i + 1] = char(n + 0x20);
                changed = true;
            }
            else if (n == 0x91) // ё -> Ё
            {
                s[i] = char(0xD0);
                s[i + 1] = char(0x81);
                changed = true;
            }
            ++i;
        }
    }
    return changed;
}

bool wants_caps(Rml::Element* e) { return e && e->GetComputedValues().text_transform() == Rml::Style::TextTransform::Uppercase; }

void apply_caps(Rml::Element* e)
{
    if (!e)
        return;
    if (auto* t = rmlui_dynamic_cast<Rml::ElementText*>(e))
    {
        if (wants_caps(t))
        {
            xr_string s = t->GetText().c_str();
            if (caps_utf8(s))
                t->SetText(s.c_str());
        }
        return;
    }
    for (int i = 0; i < e->GetNumChildren(); ++i)
        apply_caps(e->GetChild(i));
}

bool s_caps_pending = false; // the document is new: raise its captions after the first layout

void set_text(const char* id, const char* utf8)
{
    if (Rml::Element* e = by_id(id))
    {
        xr_string src = utf8 ? utf8 : "";
        if (wants_caps(e))
            caps_utf8(src);
        xr_string html;
        for (const char* p = src.c_str(); *p; ++p)
        {
            if (*p == '<')
                html += "&lt;";
            else if (*p == '>')
                html += "&gt;";
            else if (*p == '&')
                html += "&amp;";
            else if (*p == '\n' || (*p == '\\' && p[1] == 'n' && ++p))
                html += "<br/>";
            else
                html += *p;
        }
        e->SetInnerRML(html.c_str());
    }
}

// Walks the tree collecting elements that carry an attribute. A DOM question
// answered with a DOM walk: attribute selectors are a stylesheet feature, and
// borrowing one here would tie this to how RmlUi compiles CSS.
void collect(Rml::Element* e, const char* attr, xr_vector<Rml::Element*>& out)
{
    if (!e)
        return;
    if (e->HasAttribute(attr))
        out.push_back(e);
    for (int i = 0; i < e->GetNumChildren(); ++i)
        collect(e->GetChild(i), attr, out);
}

xr_string attr(Rml::Element* e, const char* name, const char* def = "")
{
    return e ? xr_string(e->GetAttribute<Rml::String>(name, def).c_str()) : xr_string(def);
}

// ⭐⭐ONE MENU FOR EVERY SCREEN. `<body data-space="screen">` lays the document
// out in Space::Screen -- dp = height / 1080, no stretch, fonts in dp too -- so
// the same markup and the same stylesheet draw the same picture on a 1440p
// monitor and on the 720p Switch, only at a different pixel count. Without the
// attribute the legacy 1024x768 stage and its scaleX stand, as before.
bool screen_space() { return s_menu && attr(s_menu, "data-space") == "screen"; }

// ⚠Re-applied every frame, not once: a vid_restart from the options page changes
// the resolution under us, and both the dp ratio and the stretch follow it.
void apply_space()
{
    if (screen_space())
    {
        UIRmlInternal::UseSpace(UIRmlInternal::Space::Screen);
        if (Rml::Element* stage = by_id("stage"))
            if (stage->GetLocalProperty(Rml::PropertyId::Transform))
                stage->RemoveProperty(Rml::PropertyId::Transform);
        return;
    }
    UIRmlInternal::UseSpace(UIRmlInternal::Space::Menu);
    stretch_stage();
}

// The option row the keyboard / pad is on (class "cur"). Rows are markup and
// live as long as the document; only their controls are rebuilt.
Rml::Element* s_cur_row = nullptr;

// Anything carrying data-act queues that action; anything carrying data-cmd
// queues a raw console command. Both are content, and that is the point of the
// track: the address a server button connects to is a line of markup.
struct ActionListener final : public Rml::EventListener
{
    xr_string action;
    explicit ActionListener(const char* a) : action(a) {}
    void ProcessEvent(Rml::Event&) override { queue(action.c_str()); }
    void OnDetach(Rml::Element*) override { delete this; }
};

// ⭐The three links along the bottom of the start screen. The old script shelled
// out (`os.execute('start "" <url>')`), which flashes a console window and hands
// a URL to the command interpreter; ShellExecute is the same intent without
// either. ⚠Not deferred like the other actions: opening a browser touches
// nothing this document owns.
struct LinkListener final : public Rml::EventListener
{
    xr_string url;
    explicit LinkListener(const char* u) : url(u) {}
    void ProcessEvent(Rml::Event&) override
    {
        Msg("~ [rml] menu: opening %s", url.c_str());
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    void OnDetach(Rml::Element*) override { delete this; }
};

// ============================================================== the versions ==
//
// Three lines along the bottom of the start screen, exactly what the old script
// put there: which build of the mod, which build of the engine, which DLC.

// The mod's own version, from system.ltx, stamped with when its config archive
// was built -- the same pair the old screen showed, which is what makes the line
// worth anything: "OGSR: Gunslinger Addon" alone says nothing about which one.
xr_string mod_version()
{
    xr_string out;
    if (pSettings && pSettings->section_exist("mod_ver") && pSettings->line_exist("mod_ver", "mod_ver"))
    {
        const char* v = pSettings->r_string("mod_ver", "mod_ver");
        // ⚠The script read it "without quotes"; r_string keeps whatever is in
        // the file, so trim them here or the line comes out quoted.
        while (v && (*v == '"' || *v == ' '))
            ++v;
        out = v ? v : "";
        while (!out.empty() && (out.back() == '"' || out.back() == ' '))
            out.pop_back();
    }

    string_path db;
    FS.update_path(db, "$fs_root$", "gamedata.db_base_configs");
    struct _stat64i32 st{};
    if (0 == _stat64i32(db, &st))
    {
        tm t{};
        localtime_s(&t, &st.st_mtime);
        string64 stamp;
        xr_sprintf(stamp, sizeof(stamp), " %02d:%02d:%04d %02d:%02d:%02d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
        out += stamp;
    }
    return out;
}

// Which background the start screen wears. ⚠The old script asks the LEVEL for
// the hour when there is one and the wall clock when there is not -- so the menu
// over a night game is dark even if it is the middle of the afternoon outside.
bool night_outside()
{
    u32 hour = 12;
    if (g_pGameLevel)
    {
        u32 y, m, d, h, mi, s, ms;
        split_time(Level().GetGameTime(), y, m, d, h, mi, s, ms);
        hour = h;
    }
    else
    {
        const time_t now = time(nullptr);
        tm t{};
        localtime_s(&t, &now);
        hour = u32(t.tm_hour);
    }
    return hour <= 8 || hour >= 20;
}

// ============================================================== the screens ====

enum class Screen
{
    Main,
    Options,
    Save,
    Load
};
Screen s_screen = Screen::Main;

// Which of the legacy button sets is up. The names are the ones ui_mm_main.xml
// used, because the sets themselves still live in the markup under those ids.
const char* const BTN_SETS[] = {"set-main", "set-main-last-save", "set-single", "set-single-dead", "set-newgame"};
xr_string s_btn_set;
int s_selected = 0;

// ⭐⭐THE WHEEL, and it is the one thing that makes this screen readable.
//
// CUIMMShniaga never highlighted anything -- it moved a part instead. ⚠⚠AND THE
// PART THAT MOVES IS THE DRUM, NOT THE COLUMN. That is easy to get backwards
// from the formula alone, and I did: SelectBtn ends in
//
//     m_destination = m_selected->GetWndPos().y - m_magnifier->GetWndPos().y;
//     ... m_shniaga->SetWndPos({x, m_destination});
//
// and it is `m_shniaga` -- the strip carrying the magnifier, the gratings and
// the two spinning cogs -- that gets the new position. The buttons live in a
// separate CUIScrollView (`buttons_region`) that is never touched. So the frame
// slides ONTO the hovered item; the captions stand still. Ported the other way
// round the screen still animates on hover and still looks vaguely right, which
// is why it survived a build: the user's word for it was that the menu TEXT
// moves, and that it is strange.
//
// The arithmetic drops out of the same two lines. Landing the magnifier
// (<magnifire y="22"> inside a drum whose own origin is the row grid) on row i
// means the drum sits at 30*i - 22, and <y_offset> -- 0 in this mod -- is added
// on top.
//
// ⚠Also ported here and NOT invented: the travel time. The legacy easing is
// logarithmic (`log(1 + 10t/T) / log(11)`), which quadratic-out matches almost
// exactly at the midpoint (0.747 vs 0.750), but the DURATION is a function of
// the distance -- one row takes ~0.6 s and the full column ~0.95 s. A flat
// fast tween reads as a snap and loses the drum's weight.
constexpr int BTN_HEIGHT = 30; // <menu_main btn_height="30">
constexpr int MAGNIFIER_Y = 22; // <magnifire y="22" height="36">
constexpr int SHNIAGA_H = 280; // <shniaga_wnd height="280"> -- the log base below

// Switch skin: the markup may declare its own row pitch and drum height as data-row /
// data-drum on #shniaga (dp). Without them the legacy 30 / 73 stand, so the PC layout is
// numerically what it was. The magnifier row inside the drum scales with the drum (22 of 73).
constexpr float DRUM_H = 73.f; // <shniaga height="73">
float s_row_pitch = float(BTN_HEIGHT);
float s_mag_y = float(MAGNIFIER_Y);

float wheel_top(int index) { return float(index) * s_row_pitch - s_mag_y; }

Rml::Element* current_set() { return s_btn_set.empty() ? nullptr : by_id(s_btn_set.c_str()); }

void select_button(int index)
{
    if (Rml::Element* sh = by_id("shniaga"))
    {
        const float row = float(atof(attr(sh, "data-row", "30").c_str()));
        const float drum = float(atof(attr(sh, "data-drum", "73").c_str()));
        s_row_pitch = (row > 0.f) ? row : float(BTN_HEIGHT);
        s_mag_y = float(MAGNIFIER_Y) * ((drum > 0.f) ? drum : DRUM_H) / DRUM_H;
        // ⭐data-mag: the selector's own offset above a row, in dp, said outright.
        // A skin whose selector is not the legacy drum (the True Stalker brackets
        // are a strip exactly as tall as a row) has no drum height to derive it from.
        if (sh->HasAttribute("data-mag"))
            s_mag_y = float(atof(attr(sh, "data-mag").c_str()));
    }
    Rml::Element* set = current_set();
    if (!set)
        return;
    const int n = set->GetNumChildren();
    if (n <= 0)
        return;

    clamp(index, 0, n - 1);
    const int previous = s_selected;
    s_selected = index;

    for (int i = 0; i < n; ++i)
        set->GetChild(i)->SetClass("sel", i == index);

    Rml::Element* wheel = by_id("wheel");
    if (!wheel)
        return;

    // ⚠⚠A `transition` in the stylesheet DOES NOT COVER THIS, and the reason is
    // worth writing down: RmlUi starts transitions from
    // ElementStyle::TransitionPropertyChanges, which compares the element's OLD
    // and NEW *definitions* -- what the style sheet resolves to. A property set
    // straight onto the element never goes through a definition change, so the
    // rule parses, applies to nothing, and the drum jumps. Animate() is the
    // API for a value C++ owns.
    // ⚠⚠NO SetProperty(to) BEFORE Animate. An animation started from C++ is a
    // USER-origin one, and RmlUi KEEPS its last value when it completes
    // (Element::AdvanceAnimations removes the property only for transition- and
    // stylesheet-origin animations) -- so the end position needs no help. Setting
    // it first did harm: a frame could render with the destination before the
    // animation's first step pulled the frame back to its start, which on the pad
    // read as "jumps there, jumps back, then slides". An earlier comment here
    // claimed the opposite; it described transitions, not this.
    // ⭐FROM WHERE THE SELECTOR IS NOW, not from the previous row. With the mouse
    // the hover moves faster than any animation, and restarting each one at the
    // previous row's position made the frame jump back before every move -- the
    // "it crawls after the pointer" feel. The layout offset already carries the
    // animated `top` of the last update.
    const float now_dp = wheel->GetOffsetTop() / _max(0.01f, DpRatio());
    const Rml::Property from(now_dp, Rml::Unit::DP);
    const Rml::Property to(wheel_top(index), Rml::Unit::DP);
    if (previous == index || _abs(now_dp - wheel_top(index)) < 0.5f)
        wheel->SetProperty(Rml::PropertyId::Top, to); // nothing to travel: land there
    else
    {
        const float travel = _abs(wheel_top(index) - now_dp);
        float run;
        Rml::Element* sh = by_id("shniaga");
        if (sh && sh->HasAttribute("data-move"))
        {
            // ⭐data-move: seconds for ONE row. Each further row adds a quarter of
            // that, capped at twice it -- a snap with a hint of travel. The legacy
            // drum below took ~0.7 s per row: weight for a cog-and-grating drum,
            // sludge for a pair of brackets, worst of all under a mouse.
            const float one = _max(0.01f, float(atof(attr(sh, "data-move").c_str())));
            const float rows = travel / _max(1.f, s_row_pitch);
            run = _min(one * 2.f, one * (0.75f + 0.25f * rows));
        }
        else // CUIMMShniaga::ProcessEvent(E_Begin), in seconds and with its floor.
            run = _max(0.1f, logf(1.f + travel) / logf(float(SHNIAGA_H)));
        wheel->Animate("top", to, run, Rml::Tween(Rml::Tween::Cubic, Rml::Tween::Out), 1, false, 0.f, &from);
    }
}

// Hovering an item selects it, exactly as STATIC_FOCUS_RECEIVED did.
struct HoverSelect final : public Rml::EventListener
{
    int index;
    explicit HoverSelect(int i) : index(i) {}
    void ProcessEvent(Rml::Event&) override { select_button(index); }
    void OnDetach(Rml::Element*) override { delete this; }
};

// ⭐What is on screen is tracked here rather than read back out of the DOM. A
// computed `display` is a string RmlUi is free to normalise, and asking it "are
// you visible" to decide what ESC means is a question with a plausible wrong
// answer -- the sort that fails once, quietly, on somebody else's screen.
void show_button_set(const char* which)
{
    s_btn_set = which;
    for (const char* s : BTN_SETS)
        show(s, xr_strcmp(s, which) == 0);
    // A fresh set starts on its first item, as ShowMain did -- and lands there
    // rather than sliding, because there is no previous position to slide from.
    s_selected = 0;
    select_button(0);
}

// ⭐The same four-way decision CUIMMShniaga::Init makes, and made in the same
// order. A fifth set (the network one) is in the markup but unreachable here,
// exactly as its <menu_main_mm> block was commented out of the old XML.
xr_string newest_save();

const char* main_set_for_state()
{
    if (!g_pGameLevel)
    {
        // ⭐ANY save on disk, not just the one g_last_saved_game remembers: that
        // name is only set by saving or loading in this install, so a fresh
        // Switch card (or a cleared user.ltx) had saves and no "Continue".
        const bool has_last = !newest_save().empty();
        return has_last ? "set-main-last-save" : "set-main";
    }
    return (Actor() && !Actor()->g_Alive()) ? "set-single-dead" : "set-single";
}

// Is there a live game behind the menu? Several confirmations turn on this and
// on nothing else: loading over a running game asks first, loading from the
// start-up menu does not.
bool game_in_progress() { return g_pGameLevel && Actor() && Actor()->g_Alive(); }

// ============================================================= message box ====
//
// The legacy boxes are templates in message_box.xml driven by CUIMessageBoxEx,
// and this build has FORTY-TWO of them. They are not forty-two layouts: every
// one is the same 510x206 frame at (200,250), the same 77x77 sign at (35,55),
// the same text box at (170,40), and buttons at x=250 / x=370 on y=154. What a
// template actually carries is three things -- the caption, whether there are
// two buttons or one, and WHICH SIGN. So that is what this function takes, and
// the geometry lives once in the stylesheet.
//
// ⚠The sign is not decoration: `_sign_alarm` is the yellow "are you sure" and
// `_sign_red` the red "that did not work". Sorting the call sites by which
// template each one replaces is the only way to get them right -- the string id
// does not tell you (ui_st_invalid_saved_game is an ALARM box, and reads like an
// error).
enum class MbIcon
{
    Alarm, // ui_frame_error_sign_alarm -- a question the user can answer
    Red, // ui_frame_error_sign_red -- something already went wrong
};

xr_string s_mb_yes_action;
bool s_mb_open = false;

void message_box(const char* text_id, const char* yes_action, bool two_buttons = true, MbIcon icon = MbIcon::Alarm)
{
    s_mb_yes_action = yes_action ? yes_action : "";
    s_mb_open = true;
    set_text("mb-text", Translate(text_id).c_str());
    if (Rml::Element* e = by_id("mb-icon"))
        e->SetClass("red", icon == MbIcon::Red);
    show("mb-yes", two_buttons);
    show("mb-no", two_buttons);
    // ⚠The single button sits in the NO slot, not the YES one: <button_ok> is at
    // x=370 in every "ok" template, the same place <button_no> occupies. An OK
    // that appears where YES was would move under the pointer between two boxes.
    show("mb-ok", !two_buttons);
    show("msgbox", true);
}

void close_message_box()
{
    s_mb_open = false;
    show("msgbox", false);
    s_mb_yes_action.clear();
}

// ================================================================= options ====
//
// ⭐The legacy contract, kept exactly: a control edits a PENDING value and the
// console is not touched until Accept. Cancel therefore needs no snapshot -- it
// simply throws the pending map away, which is what CUIOptionsManager::UndoGroup
// achieved by re-reading each console entry into its widget.
xr_map<xr_string, xr_string> s_pending_opts;

// ⚠The legacy list came from CUIOptionsItem::SaveValue and was written for the
// DX renderers: two thirds of its entries (r2_volumetric_lights, r2_sun_quality,
// r_sslr_enable, ssfx_indirect_light …) do not exist as anything the Vulkan
// backend reads, so a device rebuild for them was a two-second stall in exchange
// for nothing. What genuinely needs the device rebuilt here is what the SWAPCHAIN
// is made of: its size, the window it lives in, and its present mode.
// 📏Everything else in this renderer is read per frame and takes effect on the
// next one — that is why the advanced page applies live.
bool needs_vid_restart(const char* entry)
{
    static const char* const LIST[] = {"vid_mode", "rs_fullscreen", "rs_v_sync"};
    for (const char* e : LIST)
        if (xr_strcmp(e, entry) == 0)
            return true;
    return false;
}

// ⚠"on"/"off" or "1"/"0" — and getting it wrong is a SILENT no-op. CCC_Mask
// parses only the two words, CCC_Integer only the number, and each answers a
// value it cannot read with InvalidSyntax into the log and no change on screen.
// The renderer's own switches are integers (r_ssao, r_dlss, r_vsm_hzb …) while
// the engine's are masks (rs_v_sync, rs_fullscreen), so a checkbox cannot assume
// either. Ask the command what it currently PRINTS: only a mask prints a word.
bool opt_uses_words(const char* entry)
{
    IConsole_Command* cc = Console->GetCommand(entry);
    if (!cc)
        return true;
    IConsole_Command::TStatus s{};
    cc->Status(s);
    return xr_strcmp(s, "on") == 0 || xr_strcmp(s, "off") == 0;
}

// What a console entry currently reads as, in the form the entry itself would
// accept back. Used to pick the selected row of a list.
xr_string opt_status(const char* entry)
{
    IConsole_Command* cc = Console->GetCommand(entry);
    if (!cc)
        return xr_string();
    IConsole_Command::TStatus s{};
    cc->Status(s);
    return xr_string(s);
}

// ⚠Numbers compared as numbers. A list row says `11` and the command prints
// `11`, but a float entry prints `0.750000` against a row that says `0.75`, and
// a string compare would leave nothing selected — which reads on screen as "the
// setting reset itself".
bool same_value(const char* a, const char* b)
{
    if (!a || !b)
        return false;
    char *ea = nullptr, *eb = nullptr;
    const double da = strtod(a, &ea), db = strtod(b, &eb);
    if (ea != a && eb != b && *ea == 0 && *eb == 0)
        return fabs(da - db) < 1e-6;
    return _stricmp(a, b) == 0;
}
bool needs_snd_restart(const char* entry) { return xr_strcmp(entry, "snd_efx") == 0 || xr_strcmp(entry, "snd_device") == 0; }

// A token's caption. ⚠Tokens are ids in the string table -- gd_novice reads as
// "Новичок" -- and the ones that are not (a sound device name, a resolution)
// come back as themselves, which is exactly what should be shown.
xr_string token_caption(const char* token)
{
    const xr_string t = Translate(token);
    return t.empty() ? ToUtf8(token) : t;
}

// The numeric readout next to a slider, formatted the way CUITrackBar formatted
// it: the step decides how many digits are worth showing.
xr_string format_track(float v, float step, bool is_int)
{
    string64 buf;
    if (is_int)
        xr_sprintf(buf, sizeof(buf), "%d", int(v));
    else if (step >= 1.f)
        xr_sprintf(buf, sizeof(buf), "%2.0f", v);
    else if (step >= 0.1f)
        xr_sprintf(buf, sizeof(buf), "%3.1f", v);
    else
        xr_sprintf(buf, sizeof(buf), "%4.2f", v);
    return buf;
}

xr_string escape_rml(const xr_string& s)
{
    xr_string out;
    for (char c : s)
    {
        if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '&')
            out += "&amp;";
        else
            out += c;
    }
    return out;
}

// ⭐⭐ONE CONTROL FOR EVERY ROW: `‹‹ value ››` over a strip of cells -- the
// True Stalker options screen. It replaced three RmlUi widgets (a <select>
// drop-down, a checkbox and an <input type="range">), and not for looks alone:
// a drop-down is unusable with a pad, and the Switch has nothing else. Left /
// right step it, a click on an arrow steps it, a click on the value cycles it,
// a click or a drag on the strip jumps to that point.
//
// ⭐The state lives HERE and the DOM is redrawn from it (refresh), never read
// back out of it -- so the pending value, the caption and the lit cells cannot
// disagree, which is the class of bug the old slider had (it dispatched Change
// before writing its own `value`, see the note in commit()).
//
//   <div class="stp [track|check]">
//       <span class="arr dec [lock]"/>
//       <div class="mid"> <div class="val"/> <div class="strip"> <span class="cell [on]"/>… </div> </div>
//       <span class="arr inc [lock]"/>
//   </div>
// A list longer than MAX_LIST_CELLS draws <div class="line"><div class="fill"/></div>
// instead of the cells: forty resolutions would be forty slivers.
struct OptCtl;
xr_map<Rml::Element*, OptCtl*> s_ctls; // root element -> its control, for the keyboard and the drag

struct OptCtl final : public Rml::EventListener
{
    enum class Kind
    {
        List,
        Check,
        Track
    };
    static constexpr int TRACK_CELLS = 20;
    static constexpr int MAX_LIST_CELLS = 12;

    Kind kind = Kind::List;
    Rml::Element* root = nullptr;
    xr_string entry;
    bool enabled = true, restart = false, preset = false, words = true;

    xr_vector<std::pair<xr_string, xr_string>> items; // List / Check: value, caption
    int index = 0;

    float v = 0.f, lo = 0.f, hi = 1.f, step = 0.01f; // Track
    bool is_int = false, invert = false;

    int count() const { return kind == Kind::Track ? TRACK_CELLS : int(items.size()); }
    bool has_cells() const { return kind == Kind::Track || count() <= MAX_LIST_CELLS; }
    // Where the handle stands. ⭐Inverted sliders show min+max-v, so the strip
    // fills the other way while the value it stands for is untouched --
    // r__tf_mipbias is the one that needs it: "more" is a smaller number.
    float shown() const { return invert ? (lo + hi - v) : v; }

    float snap(float x) const
    {
        if (is_int)
            x = float(iFloor(x + 0.5f));
        else if (step > 0.f)
            x = lo + float(iFloor((x - lo) / step + 0.5f)) * step;
        clamp(x, lo, hi);
        return x;
    }

    void build(Rml::Element* slot);
    void refresh();
    void commit();
    void nudge(int dir);
    void activate();
    void pick(float f);
    bool set_value(const xr_string& value);

    void ProcessEvent(Rml::Event& ev) override;
    void OnDetach(Rml::Element* e) override
    {
        s_ctls.erase(e);
        delete this;
    }
};

// The strip also listens for drag. ⚠A SEPARATE, stateless listener on purpose:
// RmlUi calls OnDetach once per attachment, so attaching the control itself
// twice would delete it twice.
struct OptDrag final : public Rml::EventListener
{
    void ProcessEvent(Rml::Event& ev) override;
};
OptDrag s_opt_drag;

// Hovering an option row puts its two hint strings in the panel on the left --
// the STATIC_FOCUS_RECEIVED callback the advanced-video script wired by hand for
// every single row.
struct HintListener final : public Rml::EventListener
{
    xr_string name, caption;
    HintListener(const char* n, const char* c) : name(n), caption(c) {}
    void ProcessEvent(Rml::Event&) override
    {
        set_text("hint-name", name.c_str());
        set_text("hint-text", caption.c_str());
    }
    void OnDetach(Rml::Element*) override { delete this; }
};

// Builds the control for one option row and seeds it from the console.
//
// The row is markup and says what it is:
//   <div class="opt" data-opt="fov" data-type="track" data-step="1"
//        data-min="50" data-max="120"
//        data-hint="video_settings_name_61" data-hint2="video_settings_desc_61">
// and everything below is the part markup cannot answer.
void build_option(Rml::Element* row)
{
    const xr_string entry = attr(row, "data-opt");
    const xr_string type = attr(row, "data-type", "check");
    if (entry.empty())
        return;

    // ⚠A row whose console entry this build does not have is HIDDEN rather than
    // drawn dead. The option lists carry entries from several renderers and
    // several engine generations; a slider that cannot move is worse than an
    // absent one, and the log line says which.
    if (!Console->GetCommand(entry.c_str()))
    {
        row->SetProperty("display", "none");
        Msg("~ [rml] options: no console entry '%s' in this build -- row hidden", entry.c_str());
        return;
    }

    // ⭐A row may name a PREREQUISITE: `data-need="r_dlss_avail"`. Existing but
    // false means the feature is compiled in and unusable on THIS machine, and a
    // DLSS group on a card without NGX is the same lie as a setting nothing
    // reads. The renderer publishes such answers as console entries precisely so
    // that a line of markup can ask.
    const xr_string need = attr(row, "data-need");
    if (!need.empty() && (!Console->GetCommand(need.c_str()) || !Console->GetBool(need.c_str())))
    {
        row->SetProperty("display", "none");
        Msg("~ [rml] options: '%s' hidden -- '%s' says this build/GPU cannot", entry.c_str(), need.c_str());
        return;
    }

    Rml::Element* slot = nullptr;
    for (int i = 0; i < row->GetNumChildren() && !slot; ++i)
        if (row->GetChild(i)->IsClassSet("ctl"))
            slot = row->GetChild(i);
    if (!slot)
    {
        Msg("! [rml] option row '%s' has no element with class 'ctl'", entry.c_str());
        return;
    }
    slot->SetInnerRML("");

    const bool restart = attr(row, "data-restart") == "1";

    // ⭐A row can depend on the game being up: the field-of-view sliders mean
    // nothing on the start screen, and the old advanced page greyed them out
    // there rather than letting somebody drag a value into nowhere.
    // ⚠The mod's condition also excludes riding the BTR and the psycho-fov
    // effect; those are script state with no C++ side, so this is the honest
    // subset -- "there is a live game" -- and no more.
    const bool enabled = attr(row, "data-cond") != "ingame" || game_in_progress();
    row->SetClass("off", !enabled);

    auto* ctl = new OptCtl();
    ctl->entry = entry;
    ctl->enabled = enabled;
    ctl->restart = restart;

    if (type == "check")
    {
        ctl->kind = OptCtl::Kind::Check;
        // ⚠The two dialects, decided here once (see opt_uses_words). Sending "on"
        // to a CCC_Integer changes nothing and says so only in the log.
        ctl->words = opt_uses_words(entry.c_str());
        // ⚠The captions are markup (data-off / data-on on the row, else on
        // <body>), not literals here: the source is UTF-8 but the execution
        // charset is not, so a Cyrillic literal would reach RmlUi as cp1251.
        const xr_string off = attr(row, "data-off", attr(s_menu, "data-off", "Off").c_str());
        const xr_string on = attr(row, "data-on", attr(s_menu, "data-on", "On").c_str());
        ctl->items.emplace_back(ctl->words ? "off" : "0", off);
        ctl->items.emplace_back(ctl->words ? "on" : "1", on);
        ctl->index = Console->GetBool(entry.c_str()) ? 1 : 0;
    }
    else if (type == "list")
    {
        ctl->kind = OptCtl::Kind::List;
        ctl->preset = attr(row, "data-preset") == "1";

        // ⭐data-values — the list spelled out in the markup:
        //   data-values="0|Драйвер;6|F — CNN;11|K — трансформер"
        //
        // Two things it buys, both of which the token path cannot. First, PLAIN
        // NUMBERS become lists: r_dlss_quality, r_dlss_preset, r_light_cones and
        // most of this renderer's switches are CCC_Integer with a range, not
        // token commands, and a 0..4 slider labelled with a number is not a
        // setting anybody can use. Second, the caption stops being the token id:
        // token names are machine-readable (`st_opt_dlss_k`) and only some of
        // them exist in a mod's string table, so half a list would read as its
        // own source code.
        const xr_string values = attr(row, "data-values");
        int selected = -1;
        if (!values.empty())
        {
            const xr_string cur = opt_status(entry.c_str());
            for (size_t at = 0; at <= values.size();)
            {
                const size_t end = values.find(';', at);
                const xr_string item = values.substr(at, end == xr_string::npos ? xr_string::npos : end - at);
                at = (end == xr_string::npos) ? values.size() + 1 : end + 1;
                if (item.empty())
                    continue;
                const size_t bar = item.find('|');
                const xr_string val = item.substr(0, bar);
                const xr_string cap = (bar == xr_string::npos) ? val : item.substr(bar + 1);
                if (same_value(cur.c_str(), val.c_str()))
                    selected = int(ctl->items.size());
                ctl->items.emplace_back(val, cap);
            }
        }
        else
        {
            const xr_token* tok = Console->GetXRToken(entry.c_str());
            if (!tok)
            {
                delete ctl;
                row->SetProperty("display", "none");
                Msg("~ [rml] options: '%s' is not a token command and the row gave no data-values -- row hidden", entry.c_str());
                return;
            }
            const char* current = Console->GetToken(entry.c_str());
            for (const xr_token* t = tok; t->name; ++t)
            {
                if (current && xr_strcmp(current, t->name) == 0)
                    selected = int(ctl->items.size());
                ctl->items.emplace_back(t->name, token_caption(t->name));
            }
        }
        if (ctl->items.empty())
        {
            delete ctl;
            row->SetProperty("display", "none");
            Msg("~ [rml] options: '%s' has an empty list -- row hidden", entry.c_str());
            return;
        }
        // ⚠A value the list does not know (a hand-edited user.ltx, a resolution
        // the monitor dropped) shows the first item rather than nothing, and is
        // not written back until somebody actually moves the control.
        if (selected < 0)
            Msg("~ [rml] options: '%s' = '%s' is not in its list -- showing the first item", entry.c_str(), opt_status(entry.c_str()).c_str());
        ctl->index = _max(selected, 0);
    }
    else // track
    {
        ctl->kind = OptCtl::Kind::Track;
        ctl->is_int = attr(row, "data-int") == "1";
        ctl->invert = attr(row, "data-invert") == "1";
        ctl->step = float(atof(attr(row, "data-step", "0.01").c_str()));

        if (ctl->is_int)
        {
            int iv, imin, imax;
            iv = Console->GetInteger(entry.c_str(), imin, imax);
            ctl->v = float(iv);
            ctl->lo = float(imin);
            ctl->hi = float(imax);
        }
        else
            ctl->v = Console->GetFloat(entry.c_str(), ctl->lo, ctl->hi);

        // ⚠The markup may narrow the range but never widen it -- the same rule
        // CUITrackBar applied to its min/max attributes. A slider that can send
        // a console entry outside its own bounds is a crash waiting for a
        // clamp somewhere else.
        const xr_string smin = attr(row, "data-min"), smax = attr(row, "data-max");
        if (!smin.empty())
            ctl->lo = _max(ctl->lo, float(atof(smin.c_str())));
        if (!smax.empty())
            ctl->hi = _min(ctl->hi, float(atof(smax.c_str())));
        if (ctl->hi <= ctl->lo)
            ctl->hi = ctl->lo + _max(ctl->step, 1.f);
        clamp(ctl->v, ctl->lo, ctl->hi);
    }

    // A disabled row keeps its control -- so the current value is still
    // readable -- but refuses to be moved, which is what Enable(false) did.
    ctl->build(slot);

    // The hint pair, if the row named one. Two ways to say it, and they coexist:
    // data-hint/-hint2 are STRING TABLE KEYS (that is how the legacy advanced
    // page was written, and its texts are worth keeping), data-tip/-tip2 are the
    // literal text. The renderer's own settings have no legacy strings and adding
    // them would mean editing a mod archive to describe an engine feature, so
    // those rows carry their own words.
    const xr_string h1 = attr(row, "data-hint"), h2 = attr(row, "data-hint2");
    const xr_string t1 = attr(row, "data-tip"), t2 = attr(row, "data-tip2");
    if (!h1.empty() || !h2.empty() || !t1.empty() || !t2.empty())
    {
        const xr_string name = t1.empty() ? Translate(h1.c_str()) : t1;
        const xr_string text = t2.empty() ? Translate(h2.c_str()) : t2;
        row->AddEventListener(Rml::EventId::Mouseover, new HintListener(name.c_str(), text.c_str()), false);
    }
}

void OptCtl::build(Rml::Element* slot)
{
    Rml::ElementPtr r = s_menu->CreateElement("div");
    r->SetClass("stp", true);
    r->SetClass("track", kind == Kind::Track);
    r->SetClass("check", kind == Kind::Check);

    Rml::ElementPtr dec = s_menu->CreateElement("span");
    dec->SetClass("arr", true);
    dec->SetClass("dec", true);
    r->AppendChild(std::move(dec));

    Rml::ElementPtr mid = s_menu->CreateElement("div");
    mid->SetClass("mid", true);
    Rml::ElementPtr val = s_menu->CreateElement("div");
    val->SetClass("val", true);
    mid->AppendChild(std::move(val));
    Rml::ElementPtr strip = s_menu->CreateElement("div");
    if (has_cells())
    {
        strip->SetClass("strip", true);
        for (int i = 0; i < count(); ++i)
        {
            Rml::ElementPtr cell = s_menu->CreateElement("span");
            cell->SetClass("cell", true);
            strip->AppendChild(std::move(cell));
        }
    }
    else
    {
        strip->SetClass("line", true);
        Rml::ElementPtr fill = s_menu->CreateElement("div");
        fill->SetClass("fill", true);
        strip->AppendChild(std::move(fill));
    }
    strip->AddEventListener(Rml::EventId::Dragstart, &s_opt_drag, false);
    strip->AddEventListener(Rml::EventId::Drag, &s_opt_drag, false);
    mid->AppendChild(std::move(strip));
    r->AppendChild(std::move(mid));

    Rml::ElementPtr inc = s_menu->CreateElement("span");
    inc->SetClass("arr", true);
    inc->SetClass("inc", true);
    r->AppendChild(std::move(inc));

    root = slot->AppendChild(std::move(r));
    root->SetClass("off", !enabled);
    root->AddEventListener(Rml::EventId::Click, this, false);
    s_ctls[root] = this;
    refresh();
}

void OptCtl::refresh()
{
    if (!root || root->GetNumChildren() < 3)
        return;
    Rml::Element* dec = root->GetChild(0);
    Rml::Element* mid = root->GetChild(1);
    Rml::Element* inc = root->GetChild(2);
    Rml::Element* val = mid->GetChild(0);
    Rml::Element* strip = mid->GetChild(1);

    float f; // how full the strip is, 0..1
    xr_string caption;
    bool at_lo, at_hi;
    if (kind == Kind::Track)
    {
        f = (hi > lo) ? (shown() - lo) / (hi - lo) : 0.f;
        caption = format_track(v, step, is_int);
        at_lo = shown() <= lo + 1e-5f;
        at_hi = shown() >= hi - 1e-5f;
    }
    else
    {
        const int n = count();
        f = float(index + 1) / float(_max(n, 1));
        caption = items[index].second;
        // ⭐A switch has two states and both arrows flip it, so neither locks.
        at_lo = kind == Kind::List && index <= 0;
        at_hi = kind == Kind::List && index >= n - 1;
    }
    clamp(f, 0.f, 1.f);

    val->SetInnerRML(escape_rml(caption).c_str());
    dec->SetClass("lock", at_lo);
    inc->SetClass("lock", at_hi);

    if (strip->IsClassSet("strip"))
    {
        const int n = strip->GetNumChildren();
        const int lit = (kind == Kind::Track) ? iFloor(f * float(n) + 0.5f) : index + 1;
        for (int i = 0; i < n; ++i)
            strip->GetChild(i)->SetClass("on", i < lit);
    }
    else if (strip->GetNumChildren() > 0)
        strip->GetChild(0)->SetProperty(Rml::PropertyId::Width, Rml::Property(f * 100.f, Rml::Unit::PERCENT));
}

// The pending map, from the state. Nothing reaches the console before Accept --
// see s_pending_opts.
void OptCtl::commit()
{
    if (kind == Kind::Track)
    {
        // ⚠From our own state, never from a widget attribute: the RmlUi slider
        // this replaces dispatched Change BEFORE writing its own `value`, and a
        // handler reading the attribute applied the previous position of every
        // slider.
        string64 buf;
        if (is_int)
            xr_sprintf(buf, sizeof(buf), "%d", iFloor(v + 0.5f));
        else
            xr_sprintf(buf, sizeof(buf), "%f", v);
        s_pending_opts[entry] = buf;
    }
    else
    {
        s_pending_opts[entry] = items[index].first;
        // ⭐A GROUP row rewrites a page-full of OTHER entries the moment it is
        // picked, so everything on screen has to be re-read or the page lies --
        // the same OnPresetChanged the old dialog had. The markup says which
        // rows are groups (data-preset), so a second one needs no branch here.
        if (preset)
            queue((xr_string("opt-preset:") + entry).c_str());
    }
    if (restart)
        queue("opt-restart-hint");
}

void OptCtl::nudge(int dir)
{
    if (!enabled || !dir)
        return;
    if (kind == Kind::Check)
        index ^= 1;
    else if (kind == Kind::List)
    {
        const int n = index + (dir > 0 ? 1 : -1);
        if (n < 0 || n >= int(items.size()))
            return; // the arrow is locked, as in the original: no wrap-around
        index = n;
    }
    else
    {
        // ⚠A step of 0.01 over 0..1 is a hundred presses of the D-pad. One
        // press moves one CELL (a twentieth of the range), rounded to the step
        // so the value stays on the grid; the mouse still reaches every value
        // by dragging.
        float s = (hi - lo) / float(TRACK_CELLS);
        if (is_int)
            s = _max(1.f, float(iFloor(s + 0.5f)));
        else if (step > 0.f)
            s = _max(step, float(iFloor(s / step + 0.5f)) * step);
        const float before = v;
        const float want = shown() + (dir > 0 ? s : -s);
        v = snap(invert ? (lo + hi - want) : want);
        if (fsimilar(v, before))
            return;
    }
    refresh();
    commit();
}

// Enter / A on a row: a switch flips, a list goes round. A slider has no
// "next", so it ignores it rather than guessing a direction.
void OptCtl::activate()
{
    if (!enabled || kind == Kind::Track)
        return;
    if (kind == Kind::Check)
        index ^= 1;
    else
        index = (index + 1) % int(items.size());
    refresh();
    commit();
}

void OptCtl::pick(float f)
{
    if (!enabled)
        return;
    clamp(f, 0.f, 1.f);
    if (kind == Kind::Track)
    {
        const float before = v;
        const float want = lo + f * (hi - lo);
        v = snap(invert ? (lo + hi - want) : want);
        if (fsimilar(v, before))
            return;
    }
    else
    {
        const int n = int(items.size());
        int i = iFloor(f * float(n));
        clamp(i, 0, n - 1);
        if (i == index)
            return;
        index = i;
    }
    refresh();
    commit();
}

// The harness path (`ui_menu_act opt-set:entry=value`): it moves THE CONTROL and
// lets it commit, so the whole chain control -> pending -> Accept -> console is
// exercised, not just the console.
bool OptCtl::set_value(const xr_string& value)
{
    if (kind == Kind::Track)
    {
        v = snap(float(atof(value.c_str())));
    }
    else if (kind == Kind::Check)
    {
        index = (value == "on" || value == "1" || value == "true") ? 1 : 0;
    }
    else
    {
        int found = -1;
        for (int i = 0; i < int(items.size()) && found < 0; ++i)
            if (same_value(items[i].first.c_str(), value.c_str()))
                found = i;
        if (found < 0)
            return false;
        index = found;
    }
    refresh();
    commit();
    return true;
}

// Where along the strip a pointer event landed, 0..1.
float strip_fraction(Rml::Element* strip, Rml::Event& ev)
{
    const float x = ev.GetParameter<float>("mouse_x", 0.f);
    const float left = strip->GetAbsoluteOffset(Rml::BoxArea::Border).x;
    const float w = strip->GetBox().GetSize(Rml::BoxArea::Border).x;
    return (w > 0.f) ? (x - left) / w : 0.f;
}

void OptCtl::ProcessEvent(Rml::Event& ev)
{
    for (Rml::Element* e = ev.GetTargetElement(); e && e != root; e = e->GetParentNode())
    {
        if (e->IsClassSet("dec"))
            return nudge(-1);
        if (e->IsClassSet("inc"))
            return nudge(+1);
        if (e->IsClassSet("val"))
            return kind == Kind::Track ? void() : activate();
        if (e->IsClassSet("strip") || e->IsClassSet("line"))
            return pick(strip_fraction(e, ev));
    }
}

void OptDrag::ProcessEvent(Rml::Event& ev)
{
    Rml::Element* strip = ev.GetCurrentElement();
    Rml::Element* root = (strip && strip->GetParentNode()) ? strip->GetParentNode()->GetParentNode() : nullptr;
    auto it = s_ctls.find(root);
    if (it != s_ctls.end())
        it->second->pick(strip_fraction(strip, ev));
}

// ============================================================ pad / keyboard ==

enum class Nav
{
    None,
    Up,
    Down,
    Left,
    Right,
    Ok,
    Back,
    TabPrev,
    TabNext,
    PadY, // the pad's face/system buttons with no keyboard meaning:
    PadX, //   they press whichever visible dialog button carries
    PadStart, //   the matching data-pad (see pad_button)
    PadMinus
};

Nav nav_of(int dik)
{
    switch (dik)
    {
    case DIK_UP: return Nav::Up;
    case DIK_DOWN: return Nav::Down;
    case DIK_LEFT: return Nav::Left;
    case DIK_RIGHT: return Nav::Right;
    case DIK_RETURN:
    case DIK_NUMPADENTER: return Nav::Ok;
#ifdef __SWITCH__
    case DIK_ESCAPE: return Nav::PadStart; // the + button; B is "back" on the pad
#else
    case DIK_ESCAPE: return Nav::Back;
#endif
    case DIK_PRIOR: return Nav::TabPrev;
    case DIK_NEXT: return Nav::TabNext;
#ifdef __SWITCH__
    // ⚠⚠THE PAD ARRIVES AS KEYS: xr_input_sdl.cpp (kPadKeys) turns the D-pad
    // into 1/2/3/4, A into F, B into Space, L/R into Ctrl/Shift and the left
    // stick into WASD, because the game itself only knows a keyboard. Read
    // back here, for the menu only and only on the Switch -- on a PC those are
    // keys somebody may be typing.
    case DIK_1:
    case DIK_W: return Nav::Up;
    case DIK_3:
    case DIK_S: return Nav::Down;
    case DIK_4:
    case DIK_A: return Nav::Left;
    case DIK_2:
    case DIK_D: return Nav::Right;
    case DIK_F: return Nav::Ok;
    case DIK_SPACE: return Nav::Back;
    case DIK_LCONTROL: return Nav::TabPrev;
    case DIK_LSHIFT: return Nav::TabNext;
    // the pad's Y sends R and its X sends Y (xr_input_sdl.cpp kPadKeys, 06-10); I is Minus now, not Y
    case DIK_R: return Nav::PadY;
    case DIK_Y: return Nav::PadX;
    case DIK_P: return Nav::PadMinus;
#endif
    default: return Nav::None;
    }
}
//
// The options screen is walked row by row: up/down picks a row, left/right moves
// its control, Enter / A flips or cycles it, L / R (PgUp / PgDn) change the tab.

void set_cur(Rml::Element* row)
{
    if (row == s_cur_row)
        return;
    if (s_cur_row)
        s_cur_row->SetClass("cur", false);
    s_cur_row = row;
    if (!row)
        return;
    row->SetClass("cur", true);
    row->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
    // ⭐The hint panel listens for Mouseover on the row; a row reached by the pad
    // deserves its hint just as much, so the same event is sent.
    Rml::Dictionary none;
    row->DispatchEvent(Rml::EventId::Mouseover, none);
}

struct RowHover final : public Rml::EventListener
{
    void ProcessEvent(Rml::Event& ev) override { set_cur(ev.GetCurrentElement()); }
    void OnDetach(Rml::Element*) override { delete this; }
};

OptCtl* cur_ctl()
{
    if (!s_cur_row)
        return nullptr;
    for (auto& kv : s_ctls)
        if (kv.first->GetParentNode() && kv.first->GetParentNode()->GetParentNode() == s_cur_row)
            return kv.second;
    return nullptr;
}

// Rows on the page that is up, in document order. ⚠IsVisible(true): a row on a
// hidden page is "visible" by its own display and must not be landed on.
void visible_rows(xr_vector<Rml::Element*>& out)
{
    xr_vector<Rml::Element*> all;
    collect(s_menu, "data-opt", all);
    for (Rml::Element* r : all)
        if (r->IsClassSet("opt") && r->IsVisible(true))
            out.push_back(r);
}

void move_cur(int dir)
{
    xr_vector<Rml::Element*> rows;
    visible_rows(rows);
    if (rows.empty())
        return;
    int at = -1;
    for (int i = 0; i < int(rows.size()); ++i)
        if (rows[i] == s_cur_row)
            at = i;
    int next = (at < 0) ? (dir > 0 ? 0 : int(rows.size()) - 1) : at + dir;
    clamp(next, 0, int(rows.size()) - 1);
    set_cur(rows[next]);
}

xr_string s_tab = "video";

// ⭐THE DIALOG BUTTONS ON THE PAD, as markup: `data-pad="A|B|Y|X|+|-"` on a
// button says which pad button presses it. On the Switch a pad button first
// looks for a VISIBLE element carrying its name -- inside the message box when
// one is open, anywhere on the current screen otherwise -- and queues that
// element's data-act; only when there is none does it fall back to its plain
// meaning (A = activate, B = back, + = back). The glyph on the button is drawn
// from the same attribute (build_menu), so what the screen shows and what the
// pad does cannot drift apart.
const char* pad_name(Nav n)
{
    switch (n)
    {
    case Nav::Ok: return "A";
    case Nav::Back: return "B";
    case Nav::PadY: return "Y";
    case Nav::PadX: return "X";
    case Nav::PadStart: return "+";
    case Nav::PadMinus: return "-";
    default: return nullptr;
    }
}

bool inside(Rml::Element* e, Rml::Element* box)
{
    for (; e; e = e->GetParentNode())
        if (e == box)
            return true;
    return false;
}

Rml::Element* pad_button(const char* pad, bool msgbox_open)
{
    Rml::Element* box = by_id("msgbox");
    xr_vector<Rml::Element*> all;
    collect(s_menu, "data-pad", all);
    for (Rml::Element* e : all)
        if (attr(e, "data-pad") == pad && e->IsVisible(true) && inside(e, box) == msgbox_open)
            return e;
    return nullptr;
}

void cycle_tab(int dir)
{
    static const char* const ORDER[] = {"video", "sound", "gameplay", "controls"};
    constexpr int N = int(sizeof(ORDER) / sizeof(ORDER[0]));
    int at = 0;
    for (int i = 0; i < N; ++i)
        if (s_tab == ORDER[i] || (s_tab == "video-adv" && i == 0))
            at = i;
    at = (at + dir + N) % N;
    queue((xr_string("opt-tab:") + ORDER[at]).c_str());
}

void build_all_options()
{
    xr_vector<Rml::Element*> rows;
    collect(s_menu, "data-opt", rows);
    for (Rml::Element* r : rows)
        if (r->IsClassSet("opt"))
            build_option(r);
    s_pending_opts.clear();
}

void options_accept()
{
    bool vid = false, snd = false;
    for (const auto& kv : s_pending_opts)
    {
        string512 cmd;
        xr_sprintf(cmd, sizeof(cmd), "%s %s", kv.first.c_str(), kv.second.c_str());
        // ⭐Logged one line per change, and not for tidiness: the failure this
        // pass was about is a setting that goes through the whole screen and
        // then does nothing, and the console answers a value it cannot parse
        // with an InvalidSyntax line. With the pair next to each other in the
        // log, "the checkbox does nothing" is one grep instead of a session.
        Msg("~ [rml] options: apply '%s'", cmd);
        Console->Execute(cmd);
        vid |= needs_vid_restart(kv.first.c_str());
        snd |= needs_snd_restart(kv.first.c_str());
    }
    Msg("~ [rml] options: applied %u change(s)%s%s", u32(s_pending_opts.size()), vid ? ", vid_restart" : "", snd ? ", snd_restart" : "");
    s_pending_opts.clear();

    // ⭐Order matters and is the legacy one: the values first, the device
    // rebuild second, the file last -- cfg_save writes what the console now
    // holds, so it has to come after both.
    if (vid)
        Console->Execute("vid_restart");
    if (snd)
        Console->Execute("snd_restart");
    Console->Execute("cfg_save");
}

// ================================================================ key binds ====
//
// The action list is ui_keybinding.xml, the same file the legacy screen reads,
// so a mod that adds a row there gets it in both.
struct BindEdit final : public Rml::EventListener
{
    void ProcessEvent(Rml::Event& ev) override;
    void OnDetach(Rml::Element*) override { delete this; }
};

// Which cell is waiting for a key, if any. ⚠A raw pointer into the document is
// safe only because the document outlives the wait: every path that rebuilds it
// clears this first.
Rml::Element* s_bind_target = nullptr;

void bind_cell_text(Rml::Element* cell, const _keyboard* k)
{
    // The legacy screen shows "---" for an unbound action rather than an empty
    // box, so that a row reads as deliberate.
    cell->SetInnerRML(k ? ToUtf8(k->key_local_name.c_str()).c_str() : "---");
}

void BindEdit::ProcessEvent(Rml::Event& ev)
{
    Rml::Element* e = ev.GetCurrentElement();
    if (!e)
        return;
    if (s_bind_target)
        s_bind_target->SetClass("waiting", false);
    s_bind_target = e;
    e->SetClass("waiting", true);
    e->SetInnerRML("...");
}

void fill_key_bindings()
{
    Rml::Element* list = by_id("keys");
    if (!list)
        return;
    list->SetInnerRML("");
    s_bind_target = nullptr;

    CUIXml xml;
    if (!xml.Init(CONFIG_PATH, UI_PATH, "ui_keybinding.xml"))
    {
        Msg("! [rml] ui_keybinding.xml not found -- the controls page will be empty");
        return;
    }

    const bool no_alt = Core.Features.test(xrCore::Feature::remove_alt_keybinding);
    const int groups = xml.GetNodesNum("", 0, "group");
    int rows = 0;

    for (int g = 0; g < groups; ++g)
    {
        const shared_str group_id = xml.ReadAttrib("group", g, "name");

        Rml::ElementPtr head = s_menu->CreateElement("div");
        head->SetClass("kgroup", true);
        head->SetInnerRML(Translate(group_id.c_str()).c_str());
        list->AppendChild(std::move(head));

        const int commands = xml.GetNodesNum("group", g, "command");
        XML_NODE* node = xml.NavigateToNode("group", g);
        xml.SetLocalRoot(node);

        for (int c = 0; c < commands; ++c)
        {
            const shared_str id = xml.ReadAttrib("command", c, "id");
            const shared_str exe = xml.ReadAttrib("command", c, "exe");
            _action* act = action_name_to_ptr(exe.c_str());
            if (!act)
                continue; // an action this build does not have

            Rml::ElementPtr row = s_menu->CreateElement("div");
            row->SetClass("krow", true);

            Rml::ElementPtr label = s_menu->CreateElement("div");
            label->SetClass("kname", true);
            label->SetInnerRML(Translate(id.c_str()).c_str());
            row->AppendChild(std::move(label));

            const _binding& b = g_key_bindings.at(act->id);
            for (int slot = 0; slot < (no_alt ? 1 : 2); ++slot)
            {
                Rml::ElementPtr cell = s_menu->CreateElement("div");
                cell->SetClass("kkey", true);
                cell->SetAttribute("data-action", act->action_name);
                cell->SetAttribute("data-slot", slot == 0 ? "0" : "1");
                bind_cell_text(cell.get(), b.m_keyboard[slot]);
                cell->AddEventListener(Rml::EventId::Click, new BindEdit(), false);
                row->AppendChild(std::move(cell));
            }

            list->AppendChild(std::move(row));
            ++rows;
        }
        xml.SetLocalRoot(xml.GetRoot());
    }
    Msg("~ [rml] key bindings: %d action(s)", rows);
}

// The key the cell was waiting for. Applied straight away rather than pended:
// bind/unbind are console commands with no "changed" state to hold, and this is
// how the legacy editor behaved once you let go of the key.
bool take_bind_key(int dik)
{
    if (!s_bind_target)
        return false;

    const xr_string action = attr(s_bind_target, "data-action");
    const bool primary = attr(s_bind_target, "data-slot") == "0";
    Rml::Element* cell = s_bind_target;
    s_bind_target = nullptr;
    cell->SetClass("waiting", false);

    if (dik == DIK_ESCAPE)
    {
        // Cancel: put the cell back to whatever it was showing.
        if (_action* a = action_name_to_ptr(action.c_str()))
            bind_cell_text(cell, g_key_bindings.at(a->id).m_keyboard[primary ? 0 : 1]);
        return true;
    }

    _keyboard* k = dik_to_ptr(dik, true);
    if (!k)
        return true; // a key with no name; the cell is already restored below

    string256 cmd;
    xr_sprintf(cmd, sizeof(cmd), "%s %s", primary ? "unbind" : "unbind_sec", action.c_str());
    Console->Execute(cmd);
    xr_sprintf(cmd, sizeof(cmd), "%s %s %s", primary ? "bind" : "bind_sec", action.c_str(), k->key_name);
    Console->Execute(cmd);
    bind_cell_text(cell, k);
    return true;
}

// ============================================================== save / load ====

struct SaveRow
{
    xr_string name;
    time_t stamp;
};
xr_vector<SaveRow> s_saves;
xr_string s_selected_save;

struct SaveClick final : public Rml::EventListener
{
    xr_string name;
    explicit SaveClick(const char* n) : name(n) {}
    void ProcessEvent(Rml::Event&) override;
    void OnDetach(Rml::Element*) override { delete this; }
};

// ⚠Two lists, not one: the save screen and the load screen each carry their own,
// laid out where the old ui_mm_save_dlg.xml and ui_mm_load_dlg.xml put them.
// They cannot share an id -- GetElementById would answer with whichever came
// first in the document and the other screen would fill nothing.
Rml::Element* save_list() { return by_id(s_screen == Screen::Save ? "list-save" : "list-load"); }

// The two lines the legacy load screen printed under the thumbnail: which level
// the save is on, when in game time it was taken, and how the actor was doing.
xr_string save_details(const char* name)
{
    if (!CSavedGameWrapper::saved_game_exist(name))
        return Translate("ui_st_invalid_saved_game");

    CSavedGameWrapper sg(name);
    u32 y, m, d, h, mi, s, ms;
    split_time(sg.game_time(), y, m, d, h, mi, s, ms);

    string512 buf;
    xr_sprintf(buf, sizeof(buf), "%s: %s\n%s: %02d:%02d %02d/%02d/%d\n%s %d%%", Translate("st_level").c_str(), Translate(sg.level_name()).c_str(),
               Translate("ui_inv_time").c_str(), h, mi, m, d, y, Translate("ui_inv_health").c_str(), int(sg.actor_health() * 100.f));
    return buf;
}

void select_save(const char* name)
{
    s_selected_save = name ? name : "";

    if (Rml::Element* list = save_list())
        for (int i = 0; i < list->GetNumChildren(); ++i)
        {
            Rml::Element* row = list->GetChild(i);
            row->SetClass("sel", attr(row, "data-save") == s_selected_save);
        }

    if (Rml::Element* box = by_id("save-name"))
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(box))
            input->SetValue(s_selected_save.c_str());

    set_text("save-caption", ToUtf8(s_selected_save.c_str()).c_str());
    set_text("save-info", s_selected_save.empty() ? "" : save_details(s_selected_save.c_str()).c_str());

    if (Rml::Element* pic = by_id("save-shot"))
    {
        // ⭐The thumbnail is not an engine texture: it lives in $game_saves$ and
        // this renderer writes it as a TGA. The "save:" scheme in ui_rml.cpp is
        // what reads it.
        if (s_selected_save.empty())
            pic->SetProperty("display", "none");
        else
        {
            string_path probe;
            const bool has_shot = FS.exist(probe, "$game_saves$", s_selected_save.c_str(), ".dds");
            pic->SetProperty("display", has_shot ? "block" : "none");
            if (has_shot)
            {
                xr_string src = "save:";
                src += s_selected_save;
                pic->SetAttribute("src", src.c_str());
            }
        }
    }
}

void SaveClick::ProcessEvent(Rml::Event&) { select_save(name.c_str()); }

// Up / down on the save and load screens: the next row, kept in view.
void move_save(int dir)
{
    if (s_saves.empty())
        return;
    int at = -1;
    for (int i = 0; i < int(s_saves.size()); ++i)
        if (s_saves[i].name == s_selected_save)
            at = i;
    int next = (at < 0) ? 0 : at + dir;
    clamp(next, 0, int(s_saves.size()) - 1);
    select_save(s_saves[next].name.c_str());
    if (Rml::Element* list = save_list())
        for (int i = 0; i < list->GetNumChildren(); ++i)
            if (list->GetChild(i)->IsClassSet("sel"))
                list->GetChild(i)->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
}

void fill_saves()
{
    s_saves.clear();
    s_selected_save.clear();

    FS_FileSet files;
    // ⚠NoLower, and it matters: a save is loaded by the name on disk, and
    // lower-casing "Dark Night" makes a file the loader cannot find.
    FS.file_list(files, "$game_saves$", FS_ListFiles | FS_ClampExt | FS_RootOnly | FS_NoLower, "*.sav");
    for (const auto& f : files)
        s_saves.push_back({f.name.c_str(), f.time_write});

    // Newest first, as the old list sorted itself.
    std::sort(s_saves.begin(), s_saves.end(), [](const SaveRow& a, const SaveRow& b) { return a.stamp > b.stamp; });

    Rml::Element* list = save_list();
    if (!list)
        return;
    list->SetInnerRML("");

    for (const SaveRow& s : s_saves)
    {
        Rml::ElementPtr row = s_menu->CreateElement("div");
        row->SetClass("srow", true);
        row->SetAttribute("data-save", s.name.c_str());

        Rml::ElementPtr name = s_menu->CreateElement("span");
        name->SetClass("sname", true);
        name->SetInnerRML(ToUtf8(s.name.c_str()).c_str());
        row->AppendChild(std::move(name));

        tm t{};
        localtime_s(&t, &s.stamp);
        string64 when;
        xr_sprintf(when, sizeof(when), "%02d:%02d %02d.%02d.%04d", t.tm_hour, t.tm_min, t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
        Rml::ElementPtr age = s_menu->CreateElement("span");
        age->SetClass("sage", true);
        age->SetInnerRML(when);
        row->AppendChild(std::move(age));

        row->AddEventListener(Rml::EventId::Click, new SaveClick(s.name.c_str()), false);
        list->AppendChild(std::move(row));
    }

    // ⭐The load screen opens on the newest save, so A loads it straight away and
    // the D-pad has somewhere to start from. The save screen does not: selecting
    // a row there copies its name into the box, i.e. proposes an overwrite.
    select_save((s_screen == Screen::Load && !s_saves.empty()) ? s_saves.front().name.c_str() : "");
    Msg("~ [rml] saves: %u", u32(s_saves.size()));
}

void delete_selected_save()
{
    if (s_selected_save.empty())
        return;
    string_path fn;
    xr_sprintf(fn, sizeof(fn), "%s.sav", s_selected_save.c_str());
    FS.file_delete("$game_saves$", fn);
    xr_sprintf(fn, sizeof(fn), "%s.dds", s_selected_save.c_str());
    string_path full;
    if (FS.exist(full, "$game_saves$", s_selected_save.c_str(), ".dds"))
        FS.file_delete("$game_saves$", fn);
    fill_saves();
}

// ⭐"Continue": the most recent VALID save by file time -- what a player means by
// "the last one", whichever way it was made (quick, auto, named).
xr_string newest_save()
{
    FS_FileSet files;
    FS.file_list(files, "$game_saves$", FS_ListFiles | FS_ClampExt | FS_RootOnly | FS_NoLower, "*.sav");
    xr_vector<std::pair<time_t, xr_string>> all;
    for (const auto& f : files)
        all.emplace_back(f.time_write, f.name.c_str());
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& s : all)
        if (CSavedGameWrapper::valid_saved_game(s.second.c_str()))
            return s.second;
    return xr_string();
}

void load_selected_save()
{
    if (s_selected_save.empty())
        return;

    // ⚠The two roads are not interchangeable, and the legacy condition is
    // inverted from what one would guess: with no simulator running the save
    // has to bring the world up (start server ... /load), and with one running
    // the level loader takes it.
    string_path cmd;
    if (g_pGameLevel)
        xr_sprintf(cmd, sizeof(cmd), "load %s", s_selected_save.c_str());
    else
    {
        Console->Execute("disconnect");
        xr_sprintf(cmd, sizeof(cmd), "start server(%s/single/alife/load)", s_selected_save.c_str());
    }
    Console->Execute(cmd);
}

// The name in the edit box, with the characters the filesystem will not take
// stripped -- the same clean-up the save dialog's Lua did before writing.
xr_string typed_save_name()
{
    xr_string s;
    if (Rml::Element* box = by_id("save-name"))
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(box))
            s = input->GetValue().c_str();

    xr_string out;
    for (char c : s)
        if (c != '(' && c != ')')
            out += c;
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// ============================================================ the router ====

void show_screen(Screen s);

void do_action(const xr_string& a)
{
    // Raw console command, straight from the markup.
    if (a.compare(0, 4, "cmd:") == 0)
    {
        Console->Execute(a.c_str() + 4);
        return;
    }

    // ---- main screen --------------------------------------------------------
    if (a == "newgame")
        show_button_set("set-newgame");
    else if (a == "back")
        show_button_set(main_set_for_state());
    else if (a.compare(0, 11, "difficulty:") == 0)
    {
        // cmd("g_game_difficulty ..") / disconnect if a game is up / start
        string256 cmd;
        xr_sprintf(cmd, sizeof(cmd), "g_game_difficulty %s", a.c_str() + 11);
        Console->Execute(cmd);
        if (g_pGameLevel)
            Console->Execute("disconnect");
        Console->Execute("start server(all/single/alife/new)");
        Console->Execute("main_menu off");
    }
    else if (a == "return")
        Console->Execute("main_menu off");
    else if (a == "credits")
        start_tutorial("credits_seq");
    else if (a == "quit")
        message_box("ui_mm_quit_windows_message", "do-quit");
    else if (a == "do-quit")
        Console->Execute("quit");
    else if (a == "disconnect")
        message_box("ui_mm_quit_game_message", "do-disconnect");
    else if (a == "do-disconnect")
    {
        // ⚠Just the one command, as the old screen sent it. `disconnect` tears
        // the level down and the engine raises the menu again by itself on the
        // way out; asking for it here as well opens a second one over the first.
        close_message_box();
        Console->Execute("disconnect");
    }
    else if (a == "continue")
    {
        if (newest_save().empty())
            message_box("ui_st_invalid_saved_game", "", false);
        else if (game_in_progress())
            message_box("ui_st_load_game_confirm", "do-continue");
        else
            do_action("do-continue");
    }
    else if (a == "do-continue")
    {
        close_message_box();
        s_selected_save = newest_save();
        Msg("~ [rml] menu: continue -> '%s'", s_selected_save.c_str());
        load_selected_save();
        Console->Execute("main_menu off");
    }
    else if (a == "lastsave")
    {
        if (game_in_progress())
            message_box("ui_st_load_game_confirm", "do-lastsave");
        else
            do_action("do-lastsave");
    }
    else if (a == "do-lastsave")
    {
        close_message_box();
        Console->Execute("load_last_save");
        Console->Execute("main_menu off");
    }

    // ---- screens ------------------------------------------------------------
    else if (a == "options")
        show_screen(Screen::Options);
    else if (a == "save")
        show_screen(Screen::Save);
    else if (a == "load")
        show_screen(Screen::Load);
    else if (a == "main")
        show_screen(Screen::Main);

    // ---- options ------------------------------------------------------------
    else if (a == "opt-accept")
    {
        options_accept();
        show_screen(Screen::Main);
    }
    else if (a == "opt-cancel")
    {
        s_pending_opts.clear(); // nothing was written; there is nothing to undo
        show_screen(Screen::Main);
    }
    else if (a.compare(0, 8, "opt-tab:") == 0)
    {
        const xr_string tab = a.substr(8);
        s_tab = tab;
        set_cur(nullptr);
        static const char* const TABS[] = {"video", "video-adv", "sound", "gameplay", "controls"};
        for (const char* t : TABS)
        {
            xr_string page = "page-";
            page += t;
            show(page.c_str(), tab == t);
            // ⭐The advanced graphics page has no tab of its own -- it is reached
            // by a button, and the Video tab stays lit while it is up. That is
            // how the old dialog behaved: OnBtnAdvGraphic swapped the page and
            // left the tab strip alone.
            const bool lit = (tab == t) || (tab == "video-adv" && xr_strcmp(t, "video") == 0);
            if (Rml::Element* button = by_id((xr_string("tab-") + t).c_str()))
                button->SetClass("on", lit);
        }
        // ⚠The three page buttons live at DIALOG level, not inside their pages,
        // because two of those pages scroll -- a button parked at the bottom
        // corner of a scrolling box rides away with the list. Which one is up
        // follows the tab.
        show("btn-adv", tab == "video");
        show("btn-simple", tab == "video-adv");
        show("btn-keys-default", tab == "controls");

        // The hint panel has two texts of its own -- one for the plain video
        // page, another for the advanced one -- exactly as HintRefresh had.
        set_text("hint-name", Translate("hint_name_text").c_str());
        set_text("hint-text", Translate(tab == "video-adv" ? "hint_caption_adv_text" : "hint_caption_text").c_str());
    }
    else if (a.compare(0, 11, "opt-preset:") == 0)
    {
        // ⚠A group row is the one option that cannot wait for Accept: picking
        // it rewrites a page-full of other entries inside the engine, and the
        // controls showing those entries have to be re-read afterwards or the
        // page lies. Applied here, then everything rebuilt -- the same
        // OnPresetChanged the old dialog had.
        // ⭐WHICH group is in the action, not hardcoded: `_preset` drives the
        // whole renderer, `r_shadows` drives the VSM block. Both are console
        // commands taking one token, so one line runs either.
        const xr_string entry = a.substr(11);
        auto it = s_pending_opts.find(entry);
        if (it != s_pending_opts.end())
        {
            Console->Execute((entry + " " + it->second).c_str());
            s_pending_opts.erase(it);
        }
        build_all_options();
        // ⚠The old dialog put a "needs a restart" box up here and the box was
        // telling the truth about the DX presets: they set shadow-map sizes and
        // tessellation, which the device is built around. The Vulkan presets
        // (rspec_vk_*.ltx) set nothing of the kind — every entry in them is read
        // per frame — so the box would now be a lie, and the kind of lie that
        // teaches a player to ignore the next one.
    }
    else if (a == "opt-restart-hint")
        message_box("ui_mm_need_restart", "", false, MbIcon::Red);
    else if (a.compare(0, 8, "opt-set:") == 0)
    {
        // ⭐HARNESS: move one control, exactly as a hand would.
        //   ui_menu_act opt-set:r_ssao=off
        //   ui_menu_act opt-set:vid_mode=1280x720
        //   ui_menu_act opt-set:r_render_scale=0.75
        //
        // ⚠It sets the WIDGET, not the pending map, and that is the whole point.
        // The interesting part of an options screen is not "does the console
        // accept a value" — it is the chain control → Change → pending → Accept
        // → console, and half the bugs this pass fixed lived in that chain (a
        // checkbox writing "on" to a command that only parses "1", a slider
        // reporting the previous drag). A harness that wrote s_pending_opts
        // directly would exercise none of it, and would pass while the screen
        // was broken.
        const xr_string body = a.substr(8);
        const size_t eq = body.find('=');
        if (eq == xr_string::npos)
        {
            Msg("! [rml] opt-set needs <entry>=<value>, got '%s'", body.c_str());
            return;
        }
        const xr_string entry = body.substr(0, eq), value = body.substr(eq + 1);

        xr_vector<Rml::Element*> rows;
        collect(s_menu, "data-opt", rows);
        bool done = false;
        for (Rml::Element* row : rows)
        {
            if (!row->IsClassSet("opt") || attr(row, "data-opt") != entry)
                continue;
            // The control is the .stp inside the row's <span class="ctl"/>.
            Rml::Element* stp = nullptr;
            for (int i = 0; i < row->GetNumChildren() && !stp; ++i)
                if (row->GetChild(i)->IsClassSet("ctl") && row->GetChild(i)->GetNumChildren() > 0)
                    stp = row->GetChild(i)->GetChild(0);
            auto it = s_ctls.find(stp);
            if (it == s_ctls.end())
                break;
            done = it->second->set_value(value);
            if (!done)
                Msg("! [rml] opt-set: '%s' has no option '%s'", entry.c_str(), value.c_str());
            break;
        }
        Msg("~ [rml] opt-set %s=%s -> %s", entry.c_str(), value.c_str(), done ? "ok" : "NO SUCH ROW");
    }
    else if (a.compare(0, 6, "wheel:") == 0)
    {
        // ⭐HARNESS: the wheel as the ENGINE delivers it, raw -- `wheel:-120` is
        // one DirectInput notch down, `wheel:-1` one SDL notch -- through the
        // same MenuOnWheel a real mouse reaches. Synthetic input never gets to
        // DirectInput, so this is the only way an unattended run can scroll.
        const int raw = atoi(a.c_str() + 6);
        UIRml::MenuOnWheel(raw);
        Msg("~ [rml] wheel %d -> %.2f notch(es)", raw, -WheelNotches(raw));
    }
    else if (a == "opt-keys-default")
    {
        // ⭐`unbindall` is not just an unbind: it loads default_controls.ltx
        // straight after (CCC_UnBindAll), which is exactly what the old
        // "default" button relied on.
        Console->Execute("unbindall");
        fill_key_bindings();
    }

    // ---- save / load --------------------------------------------------------
    else if (a == "save-do")
    {
        const xr_string name = typed_save_name();
        if (name.empty())
            message_box("ui_st_file_name_is_empty", "", false, MbIcon::Red);
        else
        {
            string_path probe;
            if (FS.exist(probe, "$game_saves$", name.c_str(), ".sav"))
            {
                s_selected_save = name; // do-save reads it back
                message_box("ui_st_file_exist_owerwrite_q", "do-save", true, MbIcon::Red);
            }
            else
            {
                s_selected_save = name;
                do_action("do-save");
            }
        }
    }
    else if (a == "do-save")
    {
        close_message_box();
        Console->Execute((xr_string("save ") + s_selected_save).c_str());
        show_screen(Screen::Main);
    }
    else if (a == "load-do")
    {
        if (!s_selected_save.empty())
        {
            if (!CSavedGameWrapper::valid_saved_game(s_selected_save.c_str()))
                message_box("ui_st_invalid_saved_game", "", false);
            else if (game_in_progress())
                message_box("ui_st_load_game_confirm", "do-load");
            else
                do_action("do-load");
        }
    }
    else if (a == "do-load")
    {
        close_message_box();
        load_selected_save();
        Console->Execute("main_menu off");
    }
    else if (a == "save-del")
    {
        if (!s_selected_save.empty())
            message_box("ui_mm_delete_existing_file_q", "do-del", true, MbIcon::Red);
    }
    else if (a == "do-del")
    {
        close_message_box();
        delete_selected_save();
    }

    // ---- message box --------------------------------------------------------
    else if (a == "mb-yes")
    {
        const xr_string yes = s_mb_yes_action;
        close_message_box();
        if (!yes.empty())
            do_action(yes);
    }
    else if (a == "mb-no")
        close_message_box();
    else
        Msg("! [rml] menu: nothing is wired to the action '%s'", a.c_str());
}

void show_screen(Screen s)
{
    s_screen = s;
    close_message_box();

    // ⭐In the unified skin the start screen STAYS UP under every other screen,
    // dimmed -- True Stalker opens its options beside the menu column, not
    // instead of it. Each overlay starts with a full-screen .dim layer that
    // takes the pointer, so nothing behind it can be hovered or clicked.
    show("scr-main", s == Screen::Main || screen_space());
    if (Rml::Element* m = by_id("scr-main"))
        m->SetClass("behind", s != Screen::Main);
    s_cur_row = nullptr;
    show("scr-options", s == Screen::Options);
    show("scr-save", s == Screen::Save);
    show("scr-load", s == Screen::Load);

    switch (s)
    {
    case Screen::Main: show_button_set(main_set_for_state()); break;
    case Screen::Options:
        build_all_options();
        fill_key_bindings();
        do_action("opt-tab:video");
        break;
    case Screen::Save:
    case Screen::Load: fill_saves(); break;
    }
}

// ============================================================ construction ====

// Same discipline as the pickers: torn down and reparsed on every open, with the
// stylesheet cache dropped, so editing the .rml or .rcss and reopening the menu
// shows the change. A menu is opened a handful of times per session; the cost of
// one parse is not worth defending against.
bool build_menu()
{
    if (s_menu)
    {
        s_menu->Close();
        s_menu = nullptr;
    }
    s_bind_target = nullptr;
    s_cur_row = nullptr;
    s_pending_opts.clear();

    s_menu = BuildDocument("main_menu.rml");
    if (!s_menu)
    {
        Msg("! [rml] the menu document did not load -- the legacy menu will come up instead");
        return false;
    }

    // ⭐The document element IS <body> in RmlUi, so these read attributes off
    // the body tag. Content decides its own background and music; C++ only
    // supplies the fallbacks, which are what the legacy menu used.
    const bool night = night_outside();
    const Rml::String clip = s_menu->GetAttribute<Rml::String>(night ? "data-video-night" : "data-video", "ui/ui_mm_main_back");
    ensure_video(clip.c_str(), s_menu->GetAttribute<Rml::String>("data-video-rect", "").c_str());

    // ⚠Only from the start-up menu, and only once: the old screen started its
    // track in CUIMMShniaga::Init, which never ran while a game was up either.
    static bool music_started = false;
    if (!g_pGameLevel && !music_started)
    {
        music_started = true;
        const Rml::String track = s_menu->GetAttribute<Rml::String>("data-music", "");
        if (!track.empty() && MainMenu())
            MainMenu()->PlaySound(track.c_str());
    }

    int wired = 0;
    xr_vector<Rml::Element*> acts;
    collect(s_menu, "data-act", acts);
    for (Rml::Element* e : acts)
    {
        e->AddEventListener(Rml::EventId::Click, new ActionListener(attr(e, "data-act").c_str()), false);
        ++wired;
    }
    xr_vector<Rml::Element*> cmds;
    collect(s_menu, "data-cmd", cmds);
    for (Rml::Element* e : cmds)
    {
        e->AddEventListener(Rml::EventId::Click, new ActionListener((xr_string("cmd:") + attr(e, "data-cmd")).c_str()), false);
        ++wired;
    }
    // Each item in each button set learns its own index, so hovering it can roll
    // the column to that row.
    for (const char* set_id : BTN_SETS)
        if (Rml::Element* set = by_id(set_id))
            for (int i = 0; i < set->GetNumChildren(); ++i)
                set->GetChild(i)->AddEventListener(Rml::EventId::Mouseover, new HoverSelect(i), false);

    xr_vector<Rml::Element*> links;
    collect(s_menu, "data-url", links);
    for (Rml::Element* e : links)
    {
        e->AddEventListener(Rml::EventId::Click, new LinkListener(attr(e, "data-url").c_str()), false);
        ++wired;
    }

    // The three lines along the bottom, as the old screen composed them. The
    // labels are content (a translated build is somebody else's problem), the
    // values are the only part C++ can answer.
    const xr_string mod = mod_version();
    if (!mod.empty())
        set_text("ver-mod", (xr_string(attr(s_menu, "data-mod-label", "Mod version: ").c_str()) + mod).c_str());
    if (MainMenu())
        set_text("ver-engine", (xr_string(attr(s_menu, "data-engine-label", "Engine version: ").c_str()) + MainMenu()->GetGSVer()).c_str());

    // An option row under the pointer becomes the keyboard's row too, so the
    // D-pad carries on from wherever the mouse left off.
    xr_vector<Rml::Element*> rows;
    collect(s_menu, "data-opt", rows);
    for (Rml::Element* r : rows)
        if (r->IsClassSet("opt"))
            r->AddEventListener(Rml::EventId::Mouseover, new RowHover(), false);

#ifdef __SWITCH__
    // ⭐Console wording and pad glyphs, both from the markup. `data-nx-text`
    // replaces a caption on the Switch only ("Выйти в Windows" means nothing
    // there); `data-pad` puts the pad button that presses a dialog button in
    // front of its caption. Done after BuildDocument's string-table pass, which
    // would otherwise write the translated caption back over them.
    xr_vector<Rml::Element*> nx;
    collect(s_menu, "data-nx-text", nx);
    for (Rml::Element* e : nx)
        e->SetInnerRML(escape_rml(attr(e, "data-nx-text")).c_str());
    xr_vector<Rml::Element*> pads;
    collect(s_menu, "data-pad", pads);
    for (Rml::Element* e : pads)
    {
        const xr_string caption = e->GetInnerRML().c_str();
        e->SetInnerRML(("<span class=\"pad\">" + escape_rml(attr(e, "data-pad")) + "</span>" + caption).c_str());
    }
#endif

    apply_space();
    s_caps_pending = true;
    show_screen(Screen::Main);
    Msg("~ [rml] menu: %d button(s) wired, %s space", wired, screen_space() ? "screen (1080)" : "legacy (1024x768)");
    return true;
}
} // namespace

// ================================================================= the API ====

bool UIRmlInternal::CapsUtf8(xr_string& s) { return caps_utf8(s); }

void UIRmlInternal::MenuReleaseGpu()
{
    s_menu = nullptr;
    xr_delete(s_video);
    s_video_name.clear();
}

// ⭐Presses a menu button from the console: `ui_menu_act quit`, `ui_menu_act
// options`, `ui_menu_act opt-tab:sound`.
//
// Every screen in here is reachable only by clicking, and a harness has no
// hands -- so half of this port could be built but not LOOKED AT without a
// person at the keyboard, and each round of "is the message box laid out right"
// cost a human. It goes through the same queue a click does, deliberately: an
// action that would tear the document down must still run between frames, and a
// test path that skipped that would be testing something else.
void UIRml::MenuAction(const char* action)
{
    if (!MenuActive() || !action || !action[0])
    {
        Msg("! [rml] ui_menu_act: the new menu is not up (ui_new_menu 1 first)");
        return;
    }
    queue(action);
}

bool UIRml::MenuActive() { return Ready() && s_menu && s_menu->IsVisible(); }

bool UIRml::MenuOpen()
{
    if (!Init())
    {
        // ⚠Said out loud. Init's one silent refusal is "no UIRender", which is
        // the headless case and also what a menu raised before the renderer is
        // up would hit. There is no second menu to fall back to any more, so the
        // caller refuses to activate at all -- and a screen that never appears
        // has to say why.
        Msg("! [rml] RmlUi would not start -- no menu this time");
        return false;
    }

    // ⭐The menu is laid out in X-Ray's own 1024x768 UI space, so that every
    // coordinate carried over from ui_mm_main.xml and ui_mm_opt.xml means what
    // it meant. See UIRmlInternal::Space.
    UseSpace(Space::Menu);

    // The HUD document shares this context and was laid out in the in-game unit;
    // it has no business on the menu (it stays visible when ESC opens the menu
    // over a live level, because the level stops updating and cannot hide it).
    HudHide();

    // Middle of the screen, so the first movement does not have to cross it.
    CursorTo(int(Device.dwWidth) / 2, int(Device.dwHeight) / 2);
    s_pending.clear();

    if (!build_menu())
        return false;

    // ⭐Where the box actually landed -- the one line that separates "no geometry
    // at all" from "laid out off-screen", which look identical from a chair.
    // Safe here: Activate is never called from inside an RmlUi handler, because
    // every action is deferred a frame first.
    Context()->Update();
    if (Rml::Element* stage = s_menu->GetElementById("stage"))
    {
        const Rml::Vector2f at = stage->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f sz = stage->GetBox().GetSize(Rml::BoxArea::Border);
        Msg("~ [rml] menu stage at %.0f,%.0f size %.0fx%.0f on %ux%u (dp %.3f)", at.x, at.y, sz.x, sz.y, Device.dwWidth, Device.dwHeight, DpRatio());
    }
    return true;
}

void UIRml::MenuClose()
{
    if (!s_menu)
        return;
    s_bind_target = nullptr;
    s_menu->Close();
    s_menu = nullptr;
    s_pending.clear();

    // ⚠Close only detaches; the release waits for the next Context::Update, and
    // MenuFrame stops being called the moment the menu goes inactive. Run one
    // here or the document is held until the menu next opens.
    if (Context())
        Context()->Update();
}

void UIRml::MenuFrame()
{
    if (!MenuActive())
        return;

    // ⭐The queued action runs here: one frame after the click, outside RmlUi's
    // dispatch. See s_pending.
    if (!s_pending.empty())
    {
        // ⚠Copied first. An action can close the menu, and closing clears the
        // very string being read from.
        const xr_string action = s_pending;
        s_pending.clear();
        Msg("~ [rml] menu: %s", action.c_str());
        do_action(action);
        if (!MenuActive())
            return; // whatever it did, this frame is no longer ours to solve
    }

    // ⚠The stretch depends on the resolution, so it is re-applied rather than
    // set once: a vid_restart from the options page changes it under us.
    apply_space();
    Context()->Update();
    // ⭐After the first layout: only then are the computed text-transforms there
    // to be read. The next frame's Update re-measures the raised captions.
    if (s_caps_pending)
    {
        s_caps_pending = false;
        apply_caps(s_menu);
    }
}

void UIRml::MenuRender()
{
    if (!MenuActive())
        return;

    draw_video();
    Context()->Render();
    if (UIRender)
        UIRender->SetScissor(nullptr); // leave the legacy UI the state it expects
#ifndef __SWITCH__
    DrawCursor(); // after the scissor is off: the pointer belongs to the screen
#endif
}

bool UIRml::MenuOnMouseDelta(int dx, int dy)
{
    if (!MenuActive())
        return false;
    // ⭐Swallowed rather than ignored while `ui_input_mute` is running: the event
    // must not fall through to the legacy receiver either, or a scripted sweep
    // would still be steered by whoever is at the machine. See ui_rml.h.
    if (InputMuted())
        return true;
#ifdef __SWITCH__
    // ⭐No pointer in the menu on the Switch: every screen is walked with the pad
    // (nav_of), and the right stick -- which arrives here as mouse motion --
    // would otherwise steer an invisible cursor that hovers, and so SELECTS,
    // whatever it drifts over.
    return true;
#else
    CursorBy(dx, dy);
    return true;
#endif
}

bool UIRml::MenuOnWheel(int direction)
{
    if (!MenuActive())
        return false;
    if (InputMuted())
        return true;
    Context()->ProcessMouseWheel(-WheelNotches(direction), 0);
    return true;
}

bool UIRml::MenuOnKey(int dik, bool down)
{
    if (!MenuActive())
        return false;
    // ⚠Safe to swallow every key here: CMainMenu::IR_OnKeyboardPress takes the
    // console key and the screenshot key BEFORE calling this, so the way out of
    // a mute set by mistake is still open.
    if (InputMuted())
        return true;

#ifdef __SWITCH__
    // ZR is the left mouse button on the pad; with no pointer it would click
    // whatever an unseen cursor sits on.
    if (dik == MOUSE_1 || dik == MOUSE_2 || dik == MOUSE_3)
        return true;
#endif
    if (dik == MOUSE_1)
    {
        // ⭐Both ends, because a click is a press AND a release: RmlUi sends
        // Click to the element that was under the pointer at press time, and
        // only if the same one is still under it at release. Hooking only the
        // press was what made the first picker highlight rows and select
        // nothing.
        if (down)
            Context()->ProcessMouseButtonDown(0, 0);
        else
            Context()->ProcessMouseButtonUp(0, 0);
        return true;
    }

    if (!down)
        return true;

    // ⚠The key-binding editor takes the NEXT key, whatever it is, and that has
    // to come before everything below -- including ESC, which is a bindable key
    // and here means "never mind".
    if (s_bind_target && take_bind_key(dik))
        return true;

    Nav nav = nav_of(dik);

#ifdef __SWITCH__
    // A pad button presses the visible dialog button that names it (data-pad):
    // A = load / save, B = cancel, Y = delete / advanced, + = apply, ...
    if (const char* pad = pad_name(nav))
        if (Rml::Element* b = pad_button(pad, s_mb_open))
        {
            const xr_string act = attr(b, "data-act");
            if (!act.empty())
            {
                queue(act.c_str());
                return true;
            }
        }
#endif
    // + with nothing bound to it backs out, exactly as ESC does on a keyboard.
    if (nav == Nav::PadStart)
        nav = Nav::Back;

    if ((s_screen == Screen::Save || s_screen == Screen::Load) && !s_mb_open && (nav == Nav::Up || nav == Nav::Down))
    {
        move_save(nav == Nav::Down ? 1 : -1);
        return true;
    }

    if (nav == Nav::Back)
    {
        // One step back per press, and the order is the order things stack in:
        // a box over a screen, a screen over the main menu, the main menu over
        // the game. ⚠From the start-up menu ESC leads nowhere and must not
        // quit -- that would be a surprise, and the legacy screen did not.
        if (s_mb_open)
            close_message_box();
        else if (s_screen != Screen::Main)
            queue(s_screen == Screen::Options ? "opt-cancel" : "main");
        else if (s_btn_set == "set-newgame")
            queue("back");
        else if (g_pGameLevel)
            queue("return");
        return true;
    }

    // ⭐The wheel takes up/down, as CUIMMShniaga::OnKeyboard did -- and it is the
    // same selection the mouse moves, so the two never disagree.
    if (s_screen == Screen::Main && !s_mb_open && (nav == Nav::Up || nav == Nav::Down))
    {
        select_button(s_selected + (nav == Nav::Down ? 1 : -1));
        return true;
    }

    if (s_screen == Screen::Options && !s_mb_open)
    {
        switch (nav)
        {
        case Nav::Up:
        case Nav::Down: move_cur(nav == Nav::Down ? 1 : -1); return true;
        case Nav::Left:
        case Nav::Right:
            if (OptCtl* c = cur_ctl())
                c->nudge(nav == Nav::Right ? 1 : -1);
            return true;
        case Nav::TabPrev:
        case Nav::TabNext: cycle_tab(nav == Nav::TabNext ? 1 : -1); return true;
        default: break;
        }
    }

    if (nav == Nav::Ok)
    {
        if (s_mb_open)
            queue("mb-yes");
        else if (s_screen == Screen::Save)
            queue("save-do");
        else if (s_screen == Screen::Load)
            queue("load-do");
        else if (s_screen == Screen::Options)
        {
            if (OptCtl* c = cur_ctl())
                c->activate();
        }
        else if (s_screen == Screen::Main)
        {
            if (Rml::Element* set = current_set())
                if (s_selected < set->GetNumChildren())
                    queue(attr(set->GetChild(s_selected), "data-act").c_str());
        }
        return true;
    }

    if (dik == DIK_DELETE && (s_screen == Screen::Save || s_screen == Screen::Load))
    {
        queue("save-del");
        return true;
    }

    // Everything else is text for whatever has focus -- the save-name box is
    // the only thing that wants it, and it is the only thing that takes focus.
    if (dik == DIK_BACK)
        Context()->ProcessKeyDown(Rml::Input::KI_BACK, 0);
    else
        FeedText(dik);

    return true; // while the menu is up it owns the keyboard
}

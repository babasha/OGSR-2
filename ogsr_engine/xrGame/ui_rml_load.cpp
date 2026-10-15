#include "StdAfx.h"
#include "ui_rml.h"

// ⭐THE LOAD SCREEN OVER THE LEVEL PICTURE (10-10): a gauge in the corner, the
// level's name, a rule and its description along the bottom -- the True Stalker
// arrangement, drawn our way. The document is loading.rml/.rcss next to the other
// screens; the picture and the backdrop stay with UILoadingScreen (legacy statics,
// a full-screen 16:9 texture -- nothing to gain by moving them).
//
// Why it is not the legacy captions any more: they were laid out in the 1024x768
// stage and the description, up to ~600 characters, went into a 506-unit column.
// On the Switch that was ten lines running off the bottom of the screen. Here the
// unit is H/1080 (the menu's Space::Screen), the column is 1400dp, and the text is
// capped at five lines whatever the language.
//
// ⚠ITS OWN CONTEXT. A load started from a running game still has the HUD document
// in the main context, and Context::Render draws everything a context holds.
//
// ⚠Frames are SPARSE here: the screen is drawn once per load stage and per
// load-event frame, with gaps of hundreds of ms between some of them. So the needle
// catches up on wall-clock time, quickly, rather than easing over a fixed number of
// frames -- otherwise it would trail the real progress for the whole load.

#include "../Include/xrRender/UIRender.h"

// windowsx.h (precompiled header) macros over Rml::Element's method names -- see ui_rml_hud.cpp.
#undef GetNextSibling
#undef GetPreviousSibling
#undef GetPrevSibling
#undef GetFirstChild

#include <RmlUi/Core.h>

namespace
{
using namespace UIRmlInternal;

Rml::Context* s_ctx = nullptr;
Rml::ElementDocument* s_doc = nullptr;

// The gauge: TICKS marks over a half circle, left (0) to right (100 %).
constexpr int TICKS = 31;
constexpr float SWEEP_DEG = 180.f;
Rml::Element* s_tick[TICKS]{};
Rml::Element* s_needle = nullptr;
Rml::Element* s_percent = nullptr;
Rml::Element* s_stage = nullptr;
Rml::Element* s_name = nullptr;
Rml::Element* s_desc = nullptr;

float s_target = 0.f; // what the engine reported
float s_shown = 0.f; // what the needle shows
CTimer s_clock;
int s_lit = -1; // what the DOM shows -- touched only when it changes
int s_pct = -1;
float s_angle = -1000.f;
float s_dp_applied = 0.f;

// The string table's text into RML: markup characters escaped, its \n breaks kept.
xr_string to_rml(const char* engine_text)
{
    const xr_string text = ToUtf8(engine_text);
    xr_string html;
    html.reserve(text.size() + 16);
    for (const char* p = text.c_str(); *p; ++p)
    {
        if (*p == '<')
            html += "&lt;";
        else if (*p == '>')
            html += "&gt;";
        else if (*p == '&')
            html += "&amp;";
        else if (*p == '\\' && p[1] == 'n')
        {
            html += "<br/>";
            ++p;
        }
        else if (*p == '\n')
            html += "<br/>";
        else if (*p != '\r')
            html += *p;
    }
    return html;
}

void sync_ctx()
{
    const Rml::Vector2i want(int(Device.dwWidth), int(Device.dwHeight));
    if (s_ctx->GetDimensions() != want)
        s_ctx->SetDimensions(want);
    // Space::Screen: one picture on every screen, 720p handheld included.
    const float dp = float(Device.dwHeight) / 1080.f;
    if (!fsimilar(dp, s_dp_applied))
    {
        s_dp_applied = dp;
        s_ctx->SetDensityIndependentPixelRatio(dp);
    }
}

// The marks are made here rather than written out in the document: 31 of them,
// each turned to its own angle, would be 31 lines of inline style to keep in step
// with TICKS by hand. The document gives the box (#dial) and the look (.tick, .major).
void build_ticks(Rml::Element* dial)
{
    for (int i = 0; i < TICKS; ++i)
    {
        Rml::ElementPtr t = s_doc->CreateElement("div");
        const bool major = (i % 5) == 0;
        t->SetClass("tick", true);
        t->SetClass("major", major);
        const float deg = -SWEEP_DEG * 0.5f + SWEEP_DEG * float(i) / float(TICKS - 1);
        string64 rot;
        xr_sprintf(rot, "rotate(%.2fdeg)", deg);
        t->SetProperty("transform", rot);
        s_tick[i] = dial->AppendChild(std::move(t));
    }
}

void forget_elements()
{
    s_doc = nullptr;
    for (auto& t : s_tick)
        t = nullptr;
    s_needle = s_percent = s_stage = s_name = s_desc = nullptr;
}

void set_rml(Rml::Element* e, const xr_string& rml)
{
    if (e)
        e->SetInnerRML(rml.c_str());
}

void show_progress()
{
    if (!s_doc)
        return;

    const float dt = _min(0.25f, s_clock.GetElapsed_sec());
    s_clock.Start();
    // ~95 % of the gap closed in 0.3 s; never backwards, never past the report.
    s_shown += (s_target - s_shown) * _min(1.f, dt * 10.f);
    if (s_target - s_shown < 0.002f)
        s_shown = s_target;

    const int lit = iFloor(s_shown * float(TICKS - 1) + 0.5f) + 1; // the 0 mark is lit from the start
    if (lit != s_lit)
    {
        for (int i = 0; i < TICKS; ++i)
            if (s_tick[i])
                s_tick[i]->SetClass("on", i < lit);
        s_lit = lit;
    }

    const float angle = -SWEEP_DEG * 0.5f + SWEEP_DEG * s_shown;
    if (s_needle && _abs(angle - s_angle) >= 0.25f)
    {
        string64 rot;
        xr_sprintf(rot, "rotate(%.2fdeg)", angle);
        s_needle->SetProperty("transform", rot);
        s_angle = angle;
    }

    const int pct = iFloor(s_shown * 100.f + 0.5f);
    if (s_percent && pct != s_pct)
    {
        string16 buf;
        xr_sprintf(buf, "%d", pct);
        s_percent->SetInnerRML(buf);
        s_pct = pct;
        // The last stage line is "press any key" -- the one a player must not miss.
        if (s_stage)
            s_stage->SetClass("done", pct >= 100);
    }
}
} // namespace

void UIRmlInternal::LoadShutdown()
{
    // Rml::Shutdown, right after this, destroys the context and the document in it.
    forget_elements();
    s_ctx = nullptr;
    s_dp_applied = 0.f;
}

bool UIRml::LoadScreenOpen()
{
    // A level started from the command line never showed the menu, so nothing has
    // brought RmlUi up yet. Init is a no-op when it is already up.
    if (!Init())
        return false;

    if (!s_ctx)
    {
        s_ctx = Rml::CreateContext("load", Rml::Vector2i(int(Device.dwWidth), int(Device.dwHeight)));
        if (!s_ctx)
        {
            Msg("! [rml] load screen: CreateContext failed");
            return false;
        }
        s_dp_applied = 0.f;
    }
    sync_ctx();

    LoadScreenClose();

    const xr_string path = ContentPath("loading.rml");
    s_doc = s_ctx->LoadDocument(path.c_str());
    if (!s_doc)
    {
        Msg("! [rml] load screen: could not load '%s' -- the legacy one draws instead", path.c_str());
        return false;
    }
    Localise(s_doc);

    if (Rml::Element* dial = s_doc->GetElementById("dial"))
        build_ticks(dial);
    s_needle = s_doc->GetElementById("needle");
    s_percent = s_doc->GetElementById("percent");
    s_stage = s_doc->GetElementById("stage");
    s_name = s_doc->GetElementById("level-name");
    s_desc = s_doc->GetElementById("level-desc");

    s_target = s_shown = 0.f;
    s_lit = s_pct = -1;
    s_angle = -1000.f;
    s_clock.Start();
    show_progress();

    s_doc->Show();
    return true;
}

void UIRml::LoadScreenClose()
{
    if (!s_doc)
        return;
    s_doc->Close();
    forget_elements();
    // Close only queues the removal; the context carries it out on its next update,
    // and the next one may be a whole level away.
    if (s_ctx)
        s_ctx->Update();
}

void UIRml::LoadScreenProgress(float progress01)
{
    clamp(progress01, 0.f, 1.f);
    if (progress01 > s_target) // the stage counter never goes back; neither does the needle
        s_target = progress01;
}

void UIRml::LoadScreenStage(const char* text)
{
    // The renderer's four phases come through with no title. Keep the last real
    // one up rather than blanking the line for a moment four times a load.
    if (!s_stage || !text || !text[0])
        return;
    set_rml(s_stage, to_rml(text));
}

void UIRml::LoadScreenLevel(const char* name, const char* description)
{
    if (!s_doc)
        return;

    // Capitals in the text itself: text-transform would leave Cyrillic as it is.
    xr_string caps = ToUtf8(name ? name : "");
    CapsUtf8(caps);
    set_rml(s_name, to_rml(caps.c_str()));

    // The string table answers a missing key with the key itself; a level without
    // a description must not show "foo_description" under its name.
    const bool has_desc = description && description[0] && !strstr(description, "_description");
    set_rml(s_desc, has_desc ? to_rml(description) : xr_string());
    if (s_desc)
        s_desc->SetClass("empty", !has_desc);
}

void UIRml::LoadScreenRender()
{
    if (!s_ctx || !s_doc || !UIRender)
        return;

    // ⚠The font engine reads the GLOBAL space: in the legacy 1024x768 one it
    // narrows every glyph to undo the menu stage's stretch (text_stretch_x), and
    // this document has no such stage. Only that one space is a problem, and
    // switching away from it is free -- whoever owns it sets it again on open.
    if (CurrentSpace() == Space::Menu)
        UseSpace(Space::Screen);

    sync_ctx();
    show_progress();
    s_ctx->Update();
    s_ctx->Render();
    UIRender->SetScissor(nullptr); // leave the legacy UI the state it expects
}

#include "StdAfx.h"
#include "ui_rml.h"

// ⭐THE PDA ON RMLUI (10-10, the owner: "the PDA screens cannot be used, on the PC or
// on the Switch above all"). One full-screen document, pda.rml/.rcss, six tabs:
//   tasks         the task list (active / done / failed), objectives, "track", "on the map"
//   map           the level map with its spots, the Zone map with the levels on it
//   journal       the news log and the diary (journal articles)
//   contacts      who is in PDA range, with portrait, relation and biography
//   statistics    the actor's card and the statistic sections (actor_statistic.xml)
//   encyclopedia  the articles, by group
// The legacy layout had none of this readable: the mod's 3D PDA drew CUIPdaWnd --
// the 1024x768 SoC screens with 16-pixel letterica text -- as a flat overlay (the
// Vulkan renderer has no PDA screen texture, AfterWorldRender is a stub), and on the
// Switch that text came out at ~8 px.
//
// ⭐A SCREEN OF ui_rml_inv.cpp, NOT A HOST. The inventory file owns "the one document
// up": Esc, the inventory key, the cursor, the proxy main_input_receiver() hands the
// scripts, `ui_test`. The PDA is Screen::Pda there; this file is only the content --
// it is handed the document when built (PdaBuilt), a frame (PdaFrame), the keys
// (PdaKey) and the close (PdaClosed), and listens to its document itself.
//
// ⭐CUIPdaWnd STAYS AS THE MODEL. The engine tells it about new tasks, news and
// articles (PdaContentsChanged -- the HUD's flashing icon, g_pda_info_state); it
// forwards that here and is never shown. The data is read where the legacy tabs
// read it: the task manager, the news and encyclopedia registries, the PDA's
// contacts, the statistic manager, the map manager.
//
// ⭐THE 3D PDA IS KEPT AS AN ANIMATION: P takes it into the hands as before
// (CInventory::Action), and when the draw has played the screen opens
// (PdaOpenWhenDrawn, from CUIGameSP::ShowHidePda); closing the screen puts it away.
// With g_3d_pda off (or a 2D PDA) the key opens the screen at once.
//
// ⭐THE PAD (Switch; the PC's arrows / Enter / Backspace are the same buttons):
//   L / R              previous / next tab (PC: Q / E, PgUp / PgDn, 1..6)
//   up / down          the lit row of the focused column; a column without rows scrolls
//   left / right       the focused column (the list, the details)
//   A                  the lit row: select / track / open a group / "on the map"
//   X                  the tab's own: tasks -- next filter, journal -- news / diary
//   B, Plus            close
//   map: left stick / D-pad pans, X / Y zoom in / out (held), A on the Zone map
//   enters the level under the cross, on a level map centres on the actor; the
//   right stick is the pointer everywhere, ZR clicks.

#include "Level.h"
#include "Actor.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "PDA.h"
#include "HudItem.h"
#include "GameTask.h"
#include "GametaskManager.h"
#include "map_manager.h"
#include "map_location.h"
#include "map_location_defs.h"
#include "encyclopedia_article.h"
#include "alife_registry_wrappers.h"
#include "game_news.h"
#include "character_info.h"
#include "relation_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "character_community.h"
#include "character_reputation.h"
#include "actor_statistic_mgr.h"
#include "string_table.h"
#include "xr_level_controller.h"
#include "xr_platform_keys.h"
#include "../xr_3da/xr_input.h"
#include "game_object_space.h"
#include "script_callback_ex.h"
#include "script_game_object.h"
#include "HUDManager.h"
#include "UIGameSP.h"
#include "UI.h"
#include "ui/UIMainIngameWnd.h"
#include "ui/UIPdaWnd.h"
#include "ui/UIPdaAux.h"
#include "ui/UIInventoryUtilities.h"
#include "ui/UITextureMaster.h"
#include "ui/xrUIXmlParser.h"

// windowsx.h (precompiled header) macros over Rml::Element's method names -- see ui_rml_hud.cpp.
#undef GetNextSibling
#undef GetPreviousSibling
#undef GetPrevSibling
#undef GetFirstChild

#include <RmlUi/Core.h>

CSE_ALifeTraderAbstract* ch_info_get_from_id(u16 id);
extern u32 g_pda_info_state;

namespace
{
using namespace UIRmlInternal;

Rml::ElementDocument* s_doc = nullptr;

// ------------------------------------------------------------------ helpers ----

Rml::Element* by_id(const char* id) { return s_doc ? s_doc->GetElementById(id) : nullptr; }

void set_inner(const char* id, const xr_string& rml)
{
    if (Rml::Element* e = by_id(id))
        e->SetInnerRML(rml.c_str());
}

xr_string tr(const char* id) { return id && *id ? xr_string(CStringTable().translate(id).c_str()) : xr_string(); }

// ⚠The screen's own words live in the markup (<body data-t-KEY="...">), not here: a
// Cyrillic literal in this file is C4566 under MSVC without /utf-8 (same rule as
// pad_t in ui_rml_inv.cpp). Already UTF-8; the key itself if the markup lacks it.
xr_string txt(const char* key)
{
    if (!s_doc)
        return key;
    const Rml::String attr = Rml::String("data-t-") + key;
    return s_doc->GetAttribute<Rml::String>(attr, key).c_str();
}

// Engine text (cp1251 or UTF-8) as RML-safe UTF-8.
xr_string html(const char* text)
{
    const xr_string utf = ToUtf8(text ? text : "");
    xr_string out;
    out.reserve(utf.size());
    for (char ch : utf)
    {
        if (ch == '<')
            out += "&lt;";
        else if (ch == '>')
            out += "&gt;";
        else if (ch == '&')
            out += "&amp;";
        else if (ch == '"')
            out += "&quot;";
        else
            out += ch;
    }
    return out;
}

// Legacy rich text (%c[colour] and the \n escape, see legacy_text_to_rml) plus real line breaks.
xr_string rich(const char* text)
{
    xr_string s = UIRml::LuaText(text ? text : "");
    xr_string out;
    out.reserve(s.size());
    for (char ch : s)
    {
        if (ch == '\n')
            out += "<br/>";
        else if (ch != '\r')
            out += ch;
    }
    return out;
}

xr_string css_colour(u32 argb)
{
    char buf[16];
    xr_sprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color_get_R(argb), color_get_G(argb), color_get_B(argb), color_get_A(argb));
    return buf;
}

xr_string fmt(const char* f, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

xr_string slashes(xr_string s)
{
    for (char& ch : s)
        if (ch == '\\')
            ch = '/';
    return s;
}

// A picture of the legacy UI by name -- a sprite of ui\textures_descr or a file --
// with the rectangle of the file to show (x, y, WIDTH, HEIGHT as the legacy
// SetOriginalRect takes it; zero = the sprite's own, or the whole file).
xr_string img_tag(const char* tex, Frect r, const char* cls, const char* style = "")
{
    if (!tex || !*tex)
        return "";
    xr_string file = tex;
    const bool sprite = CUITextureMaster::Textures().find(tex) != CUITextureMaster::Textures().end();
    if (sprite)
    {
        file = CUITextureMaster::GetTextureFileName(tex);
        if (r.x2 <= 0.f || r.y2 <= 0.f)
        {
            const Frect s = CUITextureMaster::GetTextureRect(tex);
            r.set(s.x1, s.y1, s.width(), s.height());
        }
    }
    xr_string out = "<img class=\"" + xr_string(cls) + "\" src=\"" + slashes(file) + "\"";
    if (r.x2 > 0.f && r.y2 > 0.f)
        out += fmt(" rect=\"%.0f %.0f %.0f %.0f\"", r.x1, r.y1, r.x2, r.y2);
    if (style && *style)
        out += xr_string(" style=\"") + style + "\"";
    return out + "/>";
}

// A character's portrait: CharacterInfo::IconName() is a sprite of ui_npc_*.
xr_string portrait_tag(const shared_str& icon, const char* cls)
{
    if (!icon.size())
        return "<div class=\"" + xr_string(cls) + " none\"/>";
    return img_tag(icon.c_str(), Frect{}, cls);
}

// Rml data-act of the element or its ancestors.
Rml::Element* up_to_act(Rml::Element* e)
{
    for (; e && e != s_doc; e = e->GetParentNode())
        if (e->HasAttribute("data-act"))
            return e;
    return nullptr;
}

Rml::Vector2f abs_pos(Rml::Element* e) { return e->GetAbsoluteOffset(Rml::BoxArea::Border); }
Rml::Vector2f box_size(Rml::Element* e) { return e->GetBox().GetSize(Rml::BoxArea::Border); }

// ------------------------------------------------------------------- tabs ----

// The tabs as shown, by EPdaTabs. The ranking (never filled in this engine:
// g_all_statistic_humans is only ever cleared) folds into the statistics.
constexpr int kTabs[] = {eptQuests, eptMap, eptDiary, eptContacts, eptActorStatistic, eptEncyclopedia};
constexpr int kTabCount = int(std::size(kTabs));

int s_tab = eptQuests; // the one up (an EPdaTabs value)
int s_last_tab = eptQuests; // remembered between opens
bool s_dirty[eptEncyclopedia + 1]{}; // rebuild the tab when next shown / this frame if shown
bool s_pad_mode = false;
int s_col = 0; // the pad's column in the tab (the list, the details)

// The 3D PDA: open when the draw has played; put it away on close.
bool s_pending = false;
int s_pending_tab = -1;
u32 s_pending_t0 = 0;
bool s_from_hands = false;

int tab_index(int ept)
{
    for (int i = 0; i < kTabCount; ++i)
        if (kTabs[i] == ept)
            return i;
    return 0;
}

int normal_tab(int ept)
{
    if (ept == eptRanking)
        return eptActorStatistic;
    for (int t : kTabs)
        if (t == ept)
            return t;
    return eptQuests;
}

u32 tab_bits(int ept)
{
    switch (ept)
    {
    case eptQuests: return pda_section::quests;
    case eptMap: return pda_section::map;
    case eptDiary: return pda_section::diary | pda_section::news | pda_section::journal | pda_section::info;
    case eptContacts: return pda_section::contacts;
    case eptActorStatistic: return pda_section::statistics | pda_section::ranking;
    case eptEncyclopedia: return pda_section::encyclopedia;
    default: return 0;
    }
}

// =============================================================== the tasks ====

int s_task_filter = 0; // 0 active, 1 completed, 2 failed (and skipped)
shared_str s_task_sel;
xr_vector<shared_str> s_task_ids;

ETaskState task_state(CGameTask* t) { return t->m_Objectives.empty() ? eTaskStateDummy : t->Objective(0).TaskState(); }

bool task_passes(CGameTask* t, int filter)
{
    const ETaskState s = task_state(t);
    switch (filter)
    {
    case 0: return s == eTaskStateInProgress;
    case 1: return s == eTaskStateCompleted;
    default: return s == eTaskStateFail || s == eTaskStateSkiped;
    }
}

xr_vector<CGameTask*> tasks_of(int filter)
{
    xr_vector<CGameTask*> out;
    CActor* a = Actor();
    if (!a)
        return out;
    for (const SGameTaskKey& k : a->GameTaskManager().GameTasks())
        if (k.game_task && !k.game_task->m_Objectives.empty() && task_passes(k.game_task, filter))
            out.push_back(k.game_task);
    if (filter == 0)
        std::sort(out.begin(), out.end(), [](CGameTask* a, CGameTask* b) {
            if (a->m_priority == b->m_priority)
                return a->m_ReceiveTime > b->m_ReceiveTime;
            return a->m_priority < b->m_priority;
        });
    else
        std::sort(out.begin(), out.end(), [](CGameTask* a, CGameTask* b) { return a->m_FinishTime > b->m_FinishTime; });
    return out;
}

// Which objectives the legacy list showed (CUIEventsWnd::ReloadList): all of a
// finished task, else the done ones and the next one, at least two.
u32 visible_objectives(CGameTask* t)
{
    const u32 n = u32(t->m_Objectives.size());
    if (t->m_show_all_objectives || t->Objective(0).TaskState() != eTaskStateInProgress || n <= 2)
        return n;
    u32 v = 0;
    for (u32 i = 0; i < n; ++i)
        if (t->m_Objectives[i].TaskState() != eTaskStateInProgress)
            v = i + 1;
    if (v < 2)
        v = 2;
    else if (v < n)
        ++v;
    return v;
}

xr_string time_text(ALife::_TIME_ID t)
{
    return xr_string(InventoryUtilities::GetDateAsString(t, InventoryUtilities::edpDateToDay).c_str()) + "  " +
        InventoryUtilities::GetTimeAsString(t, InventoryUtilities::etpTimeToMinutes).c_str();
}

const char* state_class(ETaskState s)
{
    switch (s)
    {
    case eTaskStateCompleted: return "done";
    case eTaskStateFail: return "fail";
    case eTaskStateSkiped: return "skip";
    default: return "prog";
    }
}

bool objective_on_map(SGameTaskObjective& o)
{
    CMapLocation* ml = o.LinkedMapLocation();
    return ml && ml->SpotEnabled();
}

void build_task_detail(CGameTask* t)
{
    if (!t)
    {
        set_inner("t-detail", "<div class=\"empty\">" + html(tr("pda_no_tasks").c_str()) + "</div>");
        return;
    }
    CActor* actor = Actor();
    SGameTaskObjective* active = actor ? actor->GameTaskManager().ActiveObjective() : nullptr;
    SGameTaskObjective& root = t->Objective(0);
    const ETaskState st = root.TaskState();

    xr_string r = "<div class=\"d-title\">" + html(tr(t->m_Title.c_str()).c_str()) + "</div>";
    r += "<div class=\"d-sub\">" + html(time_text(t->m_ReceiveTime).c_str());
    if (st == eTaskStateInProgress && t->m_ReceiveTime != t->m_TimeToComplete && t->m_TimeToComplete)
    {
        string256 buf;
        InventoryUtilities::GetTimePeriodAsString(buf, sizeof(buf), Level().GetGameTime(), t->m_TimeToComplete);
        r += "<span class=\"d-rem\">" + html(tr("ui_st_time_remains").c_str()) + " " + html(buf) + "</span>";
    }
    r += "</div>";
    if (st != eTaskStateInProgress)
    {
        const xr_string word = txt(st == eTaskStateCompleted ? "done" : st == eTaskStateFail ? "fail" : "skip");
        r += fmt("<div class=\"d-state %s\">%s</div>", state_class(st), word.c_str());
    }
    if (root.icon_texture_name.size())
        r += img_tag(root.icon_texture_name.c_str(), root.icon_rect, "d-ticon");
    if (root.description.size())
        r += "<div class=\"d-text\">" + rich(tr(root.description.c_str()).c_str()) + "</div>";
    if (objective_on_map(root))
        r += "<div class=\"btns\"><div class=\"row btn\" data-act=\"omap:0\">" + txt("show_on_map") + "</div></div>";

    const u32 vis = visible_objectives(t);
    if (vis > 1)
    {
        r += "<div class=\"d-cap\">" + txt("stages") + "</div>";
        for (u32 i = 1; i < vis; ++i)
        {
            SGameTaskObjective& o = t->Objective(i);
            const bool is_active = active == &o;
            r += fmt("<div class=\"obj %s%s\"><div class=\"ob-mark\"/><div class=\"ob-body\"><div class=\"ob-text\">", state_class(o.TaskState()), is_active ? " active" : "");
            r += rich(tr(o.description.c_str()).c_str()) + "</div>";
            xr_string btns;
            if (o.TaskState() == eTaskStateInProgress)
                btns += fmt("<div class=\"row btn%s\" data-act=\"oact:%u\">%s</div>", is_active ? " on" : "", i, txt(is_active ? "tracking" : "track").c_str());
            if (objective_on_map(o))
                btns += fmt("<div class=\"row btn\" data-act=\"omap:%u\">%s</div>", i, txt("on_map").c_str());
            if (!btns.empty())
                r += "<div class=\"btns\">" + btns + "</div>";
            r += "</div></div>";
        }
    }

    // The task's articles (CUIEventsWnd::ShowDescription, the description mode).
    if (actor && actor->encyclopedia_registry->registry().objects_ptr())
        for (const ARTICLE_DATA& a : *actor->encyclopedia_registry->registry().objects_ptr())
        {
            if (a.article_type != ARTICLE_DATA::eTaskArticle)
                continue;
            CEncyclopediaArticle A;
            A.Load(a.article_id);
            if (!(t->m_ID == A.data()->group) && !(root.article_id.size() && a.article_id == root.article_id))
                continue;
            // A task article's name is often its bare id, never translated: no caption then.
            const xr_string name = tr(A.data()->name.c_str());
            const bool raw = name == A.data()->name.c_str() && name.find(' ') == xr_string::npos;
            r += "<div class=\"d-art\">" + (raw ? xr_string() : "<div class=\"d-cap\">" + html(name.c_str()) + "</div>") + "<div class=\"d-text\">" +
                rich(tr(A.data()->text.c_str()).c_str()) + "</div></div>";
        }

    set_inner("t-detail", r);
}

CGameTask* task_by_id(const shared_str& id)
{
    CActor* a = Actor();
    return a && id.size() ? a->GameTaskManager().HasGameTask(id) : nullptr;
}

void build_tasks()
{
    xr_vector<CGameTask*> list = tasks_of(s_task_filter);
    if (!task_by_id(s_task_sel) || std::find_if(list.begin(), list.end(), [](CGameTask* t) { return t->m_ID == s_task_sel; }) == list.end())
        s_task_sel = list.empty() ? shared_str() : list.front()->m_ID;

    CActor* actor = Actor();
    CGameTask* active_task = actor ? actor->GameTaskManager().ActiveTask() : nullptr;

    s_task_ids.clear();
    xr_string r;
    for (CGameTask* t : list)
    {
        const int i = int(s_task_ids.size());
        s_task_ids.push_back(t->m_ID);
        SGameTaskObjective& root = t->Objective(0);
        r += fmt("<div class=\"row auto task%s%s\" data-act=\"task:%d\">", t->m_ID == s_task_sel ? " sel" : "", t == active_task ? " act" : "", i);
        r += root.icon_texture_name.size() ? img_tag(root.icon_texture_name.c_str(), root.icon_rect, "ticon") : xr_string("<div class=\"ticon none\"/>");
        r += "<div class=\"tbody\"><div class=\"tt\">" + html(tr(t->m_Title.c_str()).c_str()) + "</div>";
        r += "<div class=\"tm\">" + html(time_text(t->m_ReceiveTime).c_str()) + "</div></div></div>";
    }
    if (list.empty())
        r = "<div class=\"empty\">" + txt("no_tasks") + "</div>";
    set_inner("t-list", r);

    // The filter buttons: their captions are the markup's, the counts ours.
    for (int f = 0; f < 3; ++f)
    {
        if (Rml::Element* b = by_id(fmt("tf-%d", f).c_str()))
            b->SetClass("on", f == s_task_filter);
        set_inner(fmt("tfn-%d", f).c_str(), fmt("%u", u32(tasks_of(f).size())));
    }

    build_task_detail(task_by_id(s_task_sel));
}

// ================================================================= the map ====

struct MapInfo
{
    shared_str name;
    xr_string tex;
    Frect bound{}; // world x/z (level) or texture pixels (global)
    Frect global{}; // where it lies on the Zone map
    bool on_global{};
};

xr_vector<MapInfo> s_maps;
MapInfo s_global;
bool s_maps_loaded = false;

void load_maps()
{
    if (s_maps_loaded)
        return;
    s_maps_loaded = true;
    CInifile& ini = *pGameIni;
    auto bound_of = [&](const shared_str& sect, Frect& r) {
        const Fvector4 v = ini.r_fvector4(sect, "bound_rect");
        r.set(v.x, v.y, v.z, v.w);
    };
    if (ini.section_exist("global_map"))
    {
        s_global.name = "global_map";
        s_global.tex = ini.r_string("global_map", "texture");
        bound_of("global_map", s_global.bound);
    }
    for (const char* list : {"level_maps_single", "level_maps_single_ext"})
    {
        if (!ini.section_exist(list))
            continue;
        const bool on_global = !xr_strcmp(list, "level_maps_single");
        for (const auto& it : ini.r_section(list).Ordered_Data)
        {
            shared_str name = it.first;
            if (!ini.section_exist(name) || !ini.line_exist(name, "texture") || !ini.line_exist(name, "bound_rect"))
                continue;
            MapInfo m;
            m.name = name;
            m.tex = ini.r_string(name, "texture");
            bound_of(name, m.bound);
            if (on_global && ini.line_exist(name, "global_rect"))
            {
                const Fvector4 g = ini.r_fvector4(name, "global_rect");
                m.global.set(g.x, g.y, g.z, g.w);
                m.on_global = true;
            }
            s_maps.push_back(std::move(m));
        }
    }
    Msg("~ [rml] pda: %u level maps, Zone map '%s'", u32(s_maps.size()), s_global.tex.c_str());
}

const MapInfo* map_of(const shared_str& level)
{
    for (const MapInfo& m : s_maps)
        if (m.name == level)
            return &m;
    return nullptr;
}

// The look of a spot: map_spots.xml, <spot_name width height heading><texture r g b a x y width height>.
struct SpotLook
{
    xr_string tex;
    Frect rect{};
    float w{}, h{};
    u32 colour{0xffffffff};
    bool heading{};
};
CUIXml* s_spot_xml = nullptr;
xr_map<shared_str, SpotLook> s_looks;

const SpotLook& look_of(const shared_str& spot)
{
    auto it = s_looks.find(spot);
    if (it != s_looks.end())
        return it->second;
    SpotLook l;
    if (!s_spot_xml)
    {
        s_spot_xml = xr_new<CUIXml>();
        if (!s_spot_xml->Init(CONFIG_PATH, UI_PATH, "map_spots.xml"))
            Msg("! [rml] pda: map_spots.xml did not load");
    }
    if (XML_NODE* n = s_spot_xml->NavigateToNode(spot.c_str(), 0))
    {
        l.w = s_spot_xml->ReadAttribFlt(n, "width", 16.f);
        l.h = s_spot_xml->ReadAttribFlt(n, "height", 16.f);
        l.heading = s_spot_xml->ReadAttribInt(n, "heading", 0) != 0;
        if (XML_NODE* t = s_spot_xml->NavigateToNode(n, "texture", 0))
        {
            l.tex = s_spot_xml->Read(n, "texture", 0, "");
            l.colour = color_argb(s_spot_xml->ReadAttribInt(t, "a", 255), s_spot_xml->ReadAttribInt(t, "r", 255), s_spot_xml->ReadAttribInt(t, "g", 255),
                                  s_spot_xml->ReadAttribInt(t, "b", 255));
            l.rect.set(s_spot_xml->ReadAttribFlt(t, "x", 0.f), s_spot_xml->ReadAttribFlt(t, "y", 0.f), s_spot_xml->ReadAttribFlt(t, "width", 0.f),
                       s_spot_xml->ReadAttribFlt(t, "height", 0.f));
        }
    }
    return s_looks.emplace(spot, std::move(l)).first->second;
}

// The view. The canvas is the map picture laid out at `zoom` times the size that
// fits the view, (cx, cy) the point of it (0..1) in the view's centre.
shared_str s_map_level; // empty: the Zone map
float s_zoom = 1.f;
float s_cx = 0.5f, s_cy = 0.5f;
// What the canvas holds: a level name, "<zone>", or "-" = rebuild. ⚠Not a shared_str: a
// global one built from a literal runs before the string container exists (AV at start).
xr_string s_canvas_for = "-";
float s_spots_t = 0.f; // seconds to the next spot refresh
bool s_drag = false, s_dragged = false;
float s_drag_x = 0.f, s_drag_y = 0.f, s_drag_cx = 0.f, s_drag_cy = 0.f;
float s_mx = 0.f, s_my = 0.f; // the pointer, context px
Fvector2 s_focus{}; // "on the map": a ring here (u, v) for a while
float s_focus_left = 0.f;
bool s_map_inited = false;

struct SpotOnMap
{
    float u{}, v{}; // on the canvas, 0..1
    float r_px{}; // pick radius
    xr_string hint; // RML
};
xr_vector<SpotOnMap> s_spots;
int s_hint_spot = -1;

constexpr float kMaxZoomLevel = 6.f;
constexpr float kMaxZoomGlobal = 4.f;

float map_aspect()
{
    if (!s_map_level.size())
        return s_global.bound.width() / _max(1.f, s_global.bound.height());
    const MapInfo* m = map_of(s_map_level);
    return m ? m->bound.width() / _max(1.f, m->bound.height()) : 1.f;
}

// World x/z of a level to the canvas of that level (0..1).
Fvector2 level_uv(const MapInfo& m, const Fvector2& p) { return {(p.x - m.bound.x1) / _max(1.f, m.bound.width()), 1.f - (p.y - m.bound.y1) / _max(1.f, m.bound.height())}; }

// A level's rectangle on the Zone canvas (0..1).
Frect global_rect01(const MapInfo& m)
{
    const Frect& g = s_global.bound;
    Frect r;
    r.set((m.global.x1 - g.x1) / g.width(), (m.global.y1 - g.y1) / g.height(), (m.global.x2 - g.x1) / g.width(), (m.global.y2 - g.y1) / g.height());
    return r;
}

struct ViewGeom
{
    float vx{}, vy{}, vw{}, vh{}; // the view, context px
    float cw{}, ch{}, left{}, top{}; // the canvas inside it
};

ViewGeom view_geom()
{
    ViewGeom g;
    Rml::Element* view = by_id("map-view");
    if (!view)
        return g;
    const Rml::Vector2f o = abs_pos(view), s = box_size(view);
    g.vx = o.x, g.vy = o.y, g.vw = s.x, g.vh = s.y;
    const float a = map_aspect();
    if (g.vw <= 1.f || g.vh <= 1.f)
        return g;
    if (g.vw / g.vh > a)
        g.ch = g.vh, g.cw = g.vh * a;
    else
        g.cw = g.vw, g.ch = g.vw / a;
    g.cw *= s_zoom, g.ch *= s_zoom;
    // Keep the picture on screen: a canvas smaller than the view stays centred.
    auto clamp_c = [](float& c, float canvas, float view) {
        if (canvas <= view)
            c = 0.5f;
        else
            clamp(c, view * 0.5f / canvas, 1.f - view * 0.5f / canvas);
    };
    clamp_c(s_cx, g.cw, g.vw);
    clamp_c(s_cy, g.ch, g.vh);
    g.left = g.vw * 0.5f - s_cx * g.cw;
    g.top = g.vh * 0.5f - s_cy * g.ch;
    return g;
}

void place_canvas()
{
    Rml::Element* c = by_id("map-canvas");
    if (!c)
        return;
    const ViewGeom g = view_geom();
    if (g.cw <= 0.f)
        return;
    c->SetProperty("left", fmt("%.1fpx", g.left).c_str());
    c->SetProperty("top", fmt("%.1fpx", g.top).c_str());
    c->SetProperty("width", fmt("%.1fpx", g.cw).c_str());
    c->SetProperty("height", fmt("%.1fpx", g.ch).c_str());

    if (Rml::Element* ring = by_id("map-focus"))
    {
        ring->SetClass("on", s_focus_left > 0.f);
        ring->SetProperty("left", fmt("%.1fpx", g.left + s_focus.x * g.cw).c_str());
        ring->SetProperty("top", fmt("%.1fpx", g.top + s_focus.y * g.ch).c_str());
    }
}

xr_string level_title(const shared_str& level) { return html(tr(level.c_str()).c_str()); }

// The spots of the level map, or the levels of the Zone map: the canvas content.
void fill_canvas()
{
    CActor* actor = Actor();
    s_spots.clear();
    xr_string r;
    if (!s_map_level.size())
    {
        r = img_tag(s_global.tex.c_str(), Frect{}, "map-pic");
        // The levels of the tracked objectives, marked on the Zone map.
        xr_set<shared_str> task_levels;
        if (actor)
            if (SGameTaskObjective* o = actor->GameTaskManager().ActiveObjective())
                if (CMapLocation* ml = o->LinkedMapLocation())
                    task_levels.insert(ml->LevelName());
        for (const MapInfo& m : s_maps)
        {
            if (!m.on_global)
                continue;
            const Frect g = global_rect01(m);
            const bool here = Level().name() == m.name;
            r += fmt("<div class=\"glevel%s%s\" data-act=\"lvl:%s\" style=\"left:%.3f%%;top:%.3f%%;width:%.3f%%;height:%.3f%%;\">", here ? " here" : "",
                     task_levels.count(m.name) ? " task" : "", m.name.c_str(), g.x1 * 100.f, g.y1 * 100.f, g.width() * 100.f, g.height() * 100.f);
            r += img_tag(m.tex.c_str(), Frect{}, "map-pic");
            r += "<div class=\"glname\">" + level_title(m.name) + "</div>";
            if (here && actor)
            {
                const Fvector2 uv = level_uv(m, Fvector2{actor->Position().x, actor->Position().z});
                r += fmt("<div class=\"gactor\" style=\"left:%.3f%%;top:%.3f%%;\"/>", uv.x * 100.f, uv.y * 100.f);
            }
            r += "</div>";
            SpotOnMap s;
            s.u = (g.x1 + g.x2) * 0.5f, s.v = (g.y1 + g.y2) * 0.5f;
            s.r_px = 0.f; // levels are picked by rectangle, not by radius
            s.hint = level_title(m.name);
            s_spots.push_back(std::move(s));
        }
    }
    else if (const MapInfo* m = map_of(s_map_level))
    {
        r = img_tag(m->tex.c_str(), Frect{}, "map-pic");
        const float dp = DpRatio();
        // why spots do not show, once per level opened: how many fall at each test
        u32 n_all = 0, n_named = 0, n_here = 0, n_look = 0, n_drawn = 0;
        bool me_drawn = false;
        for (SLocationKey& k : Level().MapManager().Locations())
        {
            CMapLocation* ml = k.location;
            ++n_all;
            Fvector2 pos;
            if (!ml || !ml->LevelSpotName().size())
                continue;
            ++n_named;
            if (!ml->PdaSpot(s_map_level, pos))
                continue;
            ++n_here;
            const SpotLook& l = look_of(ml->LevelSpotName());
            if (l.tex.empty())
                continue;
            ++n_look;
            const Fvector2 uv = level_uv(*m, pos);
            if (uv.x < -0.02f || uv.x > 1.02f || uv.y < -0.02f || uv.y > 1.02f)
                continue;
            ++n_drawn;
            if (actor && ml->ObjectID() == actor->ID())
                me_drawn = true;
            // legacy pixels of a 1024x768 screen -> dp of 2560x1440 (x1.875) and a little more, floored so a 3-px dot stays visible
            const float w = _max(32.f, l.w * 2.2f), h = _max(32.f, l.h * 2.2f);
            xr_string style = fmt("left:%.3f%%;top:%.3f%%;width:%.0fdp;height:%.0fdp;margin-left:%.0fdp;margin-top:%.0fdp;", uv.x * 100.f, uv.y * 100.f, w, h, -w * 0.5f, -h * 0.5f);
            if (l.heading)
            {
                const Fvector2 d = ml->Direction();
                if (d.x != 0.f || d.y != 0.f)
                    style += fmt("transform:rotate(%.1fdeg);", rad2deg(atan2f(d.x, d.y)));
            }
            const bool me = actor && ml->ObjectID() == actor->ID();
            r += fmt("<div class=\"spot%s\" style=\"%s\">", me ? " me" : "", style.c_str());
            r += img_tag(l.tex.c_str(), l.rect, "spic", fmt("image-color:%s;", css_colour(l.colour).c_str()).c_str());
            r += "</div>";

            SpotOnMap s;
            s.u = uv.x, s.v = uv.y;
            s.r_px = _max(18.f, _max(w, h) * 0.6f) * dp;
            const LPCSTR hint = ml->GetHint();
            s.hint = hint && *hint ? rich(tr(hint).c_str()) : xr_string();
            if (me)
                s.hint = "<span class=\"hme\">" + txt("you_are_here") + "</span>";
            if (!s.hint.empty())
                s_spots.push_back(std::move(s));
        }
        // The actor, whatever the spots say (as on the Zone map): an arrow along the camera.
        if (actor && !me_drawn && Level().name() == m->name)
        {
            const Fvector2 uv = level_uv(*m, Fvector2{actor->Position().x, actor->Position().z});
            r += fmt("<div class=\"lactor\" style=\"left:%.3f%%;top:%.3f%%;transform:rotate(%.1fdeg);\"><div class=\"la-tip\"/></div>", uv.x * 100.f, uv.y * 100.f,
                     rad2deg(atan2f(Device.vCameraDirection.x, Device.vCameraDirection.z)));
            SpotOnMap s;
            s.u = uv.x, s.v = uv.y;
            s.r_px = 24.f * dp;
            s.hint = "<span class=\"hme\">" + txt("you_are_here") + "</span>";
            s_spots.push_back(std::move(s));
        }
        static shared_str s_logged;
        if (s_logged != m->name)
        {
            s_logged = m->name;
            Msg("~ [rml] pda: map %s -- %u locations, %u with a level spot, %u on this level, %u with a look, %u drawn%s", m->name.c_str(), n_all, n_named, n_here, n_look,
                n_drawn, me_drawn ? "" : ", actor by its own marker");
        }
    }
    set_inner("map-canvas", r);
    s_canvas_for = s_map_level.size() ? s_map_level.c_str() : "<zone>";
    s_spots_t = 0.35f;

    // The bar over the map.
    set_inner("map-title", s_map_level.size() ? level_title(s_map_level) : txt("zone"));
    if (Rml::Element* b = by_id("map-global"))
        b->SetInnerRML(txt(s_map_level.size() ? "zone_map" : "to_me").c_str());
}

// Sets the view on a level (or the Zone map), optionally on a point of it (world x/z).
void map_show(const shared_str& level, const Fvector2* world, float zoom)
{
    load_maps();
    const MapInfo* m = level.size() ? map_of(level) : nullptr;
    if (level.size() && !m)
    {
        Msg("! [rml] pda: no map for level '%s'", level.c_str());
        return;
    }
    s_map_level = m ? m->name : shared_str();
    s_zoom = zoom;
    s_cx = s_cy = 0.5f;
    if (m && world)
    {
        const Fvector2 uv = level_uv(*m, *world);
        s_cx = uv.x, s_cy = uv.y;
    }
    s_canvas_for = "-";
}

void map_show_actor()
{
    CActor* a = Actor();
    if (!a)
        return;
    const Fvector2 p{a->Position().x, a->Position().z};
    if (map_of(Level().name()))
        map_show(Level().name(), &p, 2.5f);
    else
        map_show(shared_str(), nullptr, 1.f);
}

// Zoom by `k` keeping the canvas point under (px, py) -- context px -- where it is.
void map_zoom(float k, float px, float py)
{
    ViewGeom g = view_geom();
    if (g.cw <= 0.f)
        return;
    const float u = (px - g.vx - g.left) / g.cw, v = (py - g.vy - g.top) / g.ch;
    float z = s_zoom * k;
    const float zmax = s_map_level.size() ? kMaxZoomLevel : kMaxZoomGlobal;
    if (z < 1.f && s_map_level.size() && k < 1.f && s_zoom <= 1.001f)
    {
        // Out of a level: to the Zone map, on that level.
        const MapInfo* m = map_of(s_map_level);
        if (m && m->on_global)
        {
            const Frect r = global_rect01(*m);
            map_show(shared_str(), nullptr, 2.f);
            s_cx = (r.x1 + r.x2) * 0.5f, s_cy = (r.y1 + r.y2) * 0.5f;
        }
        return;
    }
    clamp(z, 1.f, zmax);
    s_zoom = z;
    g = view_geom();
    s_cx = u - (px - g.vx - g.vw * 0.5f) / g.cw;
    s_cy = v - (py - g.vy - g.vh * 0.5f) / g.ch;
}

// The Zone map: the level under a point (context px), if any.
const MapInfo* level_at(float px, float py)
{
    const ViewGeom g = view_geom();
    if (g.cw <= 0.f || s_map_level.size())
        return nullptr;
    const float u = (px - g.vx - g.left) / g.cw, v = (py - g.vy - g.top) / g.ch;
    for (const MapInfo& m : s_maps)
        if (m.on_global)
        {
            const Frect r = global_rect01(m);
            if (u >= r.x1 && u <= r.x2 && v >= r.y1 && v <= r.y2)
                return &m;
        }
    return nullptr;
}

void map_enter_at(float px, float py)
{
    const MapInfo* m = level_at(px, py);
    if (!m)
        return;
    const ViewGeom g = view_geom();
    const Frect r = global_rect01(*m);
    const float u = ((px - g.vx - g.left) / g.cw - r.x1) / _max(1e-4f, r.width());
    const float v = ((py - g.vy - g.top) / g.ch - r.y1) / _max(1e-4f, r.height());
    map_show(m->name, nullptr, 1.5f);
    s_cx = u, s_cy = v;
}

// The pointer (or, with the pad, the view's centre): which spot's hint to show.
void map_hint_frame()
{
    Rml::Element* hint = by_id("map-hint");
    if (!hint)
        return;
    const ViewGeom g = view_geom();
    const bool pad = s_pad_mode;
    const float px = pad ? g.vx + g.vw * 0.5f : s_mx, py = pad ? g.vy + g.vh * 0.5f : s_my;
    int best = -1;
    // The Zone map's levels carry their names on them: no hint box there.
    if (s_map_level.size() && px >= g.vx && px <= g.vx + g.vw && py >= g.vy && py <= g.vy + g.vh && !s_drag)
    {
        {
            float bd = 1e30f;
            for (size_t i = 0; i < s_spots.size(); ++i)
            {
                const float sx = g.vx + g.left + s_spots[i].u * g.cw, sy = g.vy + g.top + s_spots[i].v * g.ch;
                const float d = (sx - px) * (sx - px) + (sy - py) * (sy - py);
                if (d < s_spots[i].r_px * s_spots[i].r_px && d < bd)
                    bd = d, best = int(i);
            }
        }
    }
    if (best != s_hint_spot)
    {
        s_hint_spot = best;
        if (best >= 0)
            hint->SetInnerRML(s_spots[size_t(best)].hint.c_str());
        hint->SetClass("on", best >= 0);
    }
    if (best >= 0)
    {
        const float sx = g.vx + g.left + s_spots[size_t(best)].u * g.cw, sy = g.vy + g.top + s_spots[size_t(best)].v * g.ch;
        // beside the spot, kept inside the view
        const Rml::Vector2f hs = box_size(hint);
        float x = sx + 24.f, y = sy - hs.y - 12.f;
        if (x + hs.x > g.vx + g.vw)
            x = sx - hs.x - 24.f;
        if (y < g.vy)
            y = sy + 24.f;
        const Rml::Vector2f po = abs_pos(hint->GetParentNode());
        hint->SetProperty("left", fmt("%.0fpx", x - po.x).c_str());
        hint->SetProperty("top", fmt("%.0fpx", y - po.y).c_str());
    }
}

// Held keys pan and zoom, at a speed of the view, not of the map.
// ⚠Only keys PRESSED while the PDA is up: W held to walk when M opened it must not pan
// the map. PdaKey arms a key; a key seen released is disarmed.
bool s_armed[256]{};
bool held(int dik)
{
    if (dik < 0 || dik >= 256 || !s_armed[dik])
        return false;
    if (pInput && pInput->iGetAsyncKeyState(dik))
        return true;
    s_armed[dik] = false;
    return false;
}

void map_frame()
{
    load_maps();
    if (!s_map_inited)
    {
        s_map_inited = true;
        map_show_actor();
    }
    const float dt = _min(0.1f, Device.fTimeDelta);
    // ⚠fTimeDelta is game time -- the PDA may be open with the game slowed; it is not paused.
    float px = 0.f, py = 0.f;
    if (held(DIK_LEFT) || held(DIK_A) || held(DIK_F4))
        px -= 1.f;
    if (held(DIK_RIGHT) || held(DIK_D) || held(DIK_F2))
        px += 1.f;
    if (held(DIK_UP) || held(DIK_W) || held(DIK_F1))
        py -= 1.f;
    if (held(DIK_DOWN) || held(DIK_S) || held(DIK_F3))
        py += 1.f;
    if (px != 0.f || py != 0.f)
    {
        const ViewGeom g = view_geom();
        if (g.cw > 0.f)
        {
            s_cx += px * dt * 0.7f * g.vw / g.cw;
            s_cy += py * dt * 0.7f * g.vh / g.ch;
        }
        s_pad_mode = true;
    }
    float zk = 0.f;
#ifdef __SWITCH__
    if (held(DIK_Y)) // the pad's X
        zk += 1.f;
    if (held(DIK_R)) // the pad's Y
        zk -= 1.f;
#endif
    if (held(DIK_EQUALS) || held(DIK_ADD))
        zk += 1.f;
    if (held(DIK_MINUS) || held(DIK_SUBTRACT))
        zk -= 1.f;
    if (zk != 0.f)
    {
        const ViewGeom g = view_geom();
        map_zoom(powf(2.2f, zk * dt), g.vx + g.vw * 0.5f, g.vy + g.vh * 0.5f);
    }

    const xr_string want = s_map_level.size() ? s_map_level.c_str() : "<zone>";
    s_spots_t -= dt;
    if (want != s_canvas_for || s_spots_t <= 0.f)
        fill_canvas();
    if (s_focus_left > 0.f)
        s_focus_left -= dt;
    place_canvas();
    map_hint_frame();
    if (Rml::Element* cross = by_id("map-cross"))
        cross->SetClass("on", s_pad_mode);
}

// "On the map" from a task.
void show_objective_on_map(SGameTaskObjective& o);

// ============================================================= the articles ====

// The journal's diary and the encyclopedia: the same browser over two article types.
struct Art
{
    shared_str id;
    xr_string name; // RML
    xr_string group; // RML, the translated path
    shared_str group_key;
    bool readed{};
};
struct ArtBrowser
{
    ARTICLE_DATA::EArticleType type;
    const char* list_id;
    const char* detail_id;
    bool sorted; // the encyclopedia: groups and names alphabetical; the diary: as received
    xr_vector<Art> arts;
    shared_str sel;
    xr_set<shared_str> open_groups;
    bool first = true;
};
ArtBrowser s_diary{ARTICLE_DATA::eJournalArticle, "j-list", "j-detail", false};
ArtBrowser s_encyc{ARTICLE_DATA::eEncyclopediaArticle, "e-list", "e-detail", true};

xr_string group_title(const shared_str& group)
{
    xr_string out;
    xr_string g = group.c_str();
    size_t at = 0;
    while (at <= g.size())
    {
        const size_t sl = g.find('/', at);
        const xr_string part = g.substr(at, sl == xr_string::npos ? xr_string::npos : sl - at);
        if (!part.empty())
        {
            if (!out.empty())
                out += " / ";
            out += html(tr(part.c_str()).c_str());
        }
        if (sl == xr_string::npos)
            break;
        at = sl + 1;
    }
    return out;
}

void collect_arts(ArtBrowser& b)
{
    b.arts.clear();
    CActor* a = Actor();
    if (!a || !a->encyclopedia_registry->registry().objects_ptr())
        return;
    for (const ARTICLE_DATA& d : *a->encyclopedia_registry->registry().objects_ptr())
    {
        if (d.article_type != b.type)
            continue;
        if (std::find_if(b.arts.begin(), b.arts.end(), [&](const Art& x) { return x.id == d.article_id; }) != b.arts.end())
            continue;
        CEncyclopediaArticle A;
        A.Load(d.article_id);
        Art art;
        art.id = d.article_id;
        art.name = html(tr(A.data()->name.c_str()).c_str());
        art.group_key = A.data()->group;
        art.group = group_title(A.data()->group);
        art.readed = d.readed;
        b.arts.push_back(std::move(art));
    }
}

void build_art_detail(ArtBrowser& b)
{
    if (!b.sel.size())
    {
        set_inner(b.detail_id, "<div class=\"empty\">" + txt("pick_article") + "</div>");
        return;
    }
    CEncyclopediaArticle A;
    A.Load(b.sel);
    SArticleData* d = A.data();
    xr_string r = "<div class=\"d-title\">" + html(tr(d->name.c_str()).c_str()) + "</div>";
    if (d->group.size())
        r += "<div class=\"d-sub\">" + group_title(d->group) + "</div>";
    if (d->image_ltx.size())
        r += "<div class=\"d-pic\">" + UIRml::LuaItemIcon(d->image_ltx.c_str(), 1.6f) + "</div>";
    else if (d->image_texture.size())
    {
        Frect rc = d->image_rect;
        if (rc.x2 <= 0.f || rc.y2 <= 0.f)
        {
            if (CUITextureMaster::Textures().count(d->image_texture))
            {
                const Frect s = CUITextureMaster::GetTextureRect(d->image_texture.c_str());
                rc.set(s.x1, s.y1, s.width(), s.height());
            }
        }
        xr_string style;
        if (rc.x2 > 0.f && rc.y2 > 0.f)
        {
            // legacy units -> dp, no wider than the column
            float w = rc.x2 * 1.6f, h = rc.y2 * 1.6f;
            if (w > 900.f)
                h *= 900.f / w, w = 900.f;
            style = fmt("width:%.0fdp;height:%.0fdp;", w, h);
        }
        r += "<div class=\"d-pic\">" + img_tag(d->image_texture.c_str(), rc, "d-img", style.c_str()) + "</div>";
    }
    r += "<div class=\"d-text\">" + rich(tr(d->text.c_str()).c_str()) + "</div>";
    set_inner(b.detail_id, r);

    // Read now (the legacy diary did this on click; the encyclopedia too here).
    if (CActor* a = Actor())
        for (ARTICLE_DATA& x : a->encyclopedia_registry->registry().objects())
            if (x.article_type == b.type && x.article_id == b.sel)
                x.readed = true;
    for (Art& x : b.arts)
        if (x.id == b.sel)
            x.readed = true;
}

void build_arts(ArtBrowser& b)
{
    collect_arts(b);
    // The groups, in order (first seen, or alphabetical).
    xr_vector<shared_str> groups;
    for (const Art& a : b.arts)
        if (std::find(groups.begin(), groups.end(), a.group_key) == groups.end())
            groups.push_back(a.group_key);
    auto title_of = [&](const shared_str& g) {
        for (const Art& a : b.arts)
            if (a.group_key == g)
                return a.group;
        return xr_string();
    };
    if (b.sorted)
        std::sort(groups.begin(), groups.end(), [&](const shared_str& x, const shared_str& y) { return title_of(x) < title_of(y); });

    if (b.first)
    {
        b.first = false;
        if (!b.sorted) // the diary opens with everything visible
            for (const shared_str& g : groups)
                b.open_groups.insert(g);
    }
    if (b.sel.size() && std::find_if(b.arts.begin(), b.arts.end(), [&](const Art& a) { return a.id == b.sel; }) == b.arts.end())
        b.sel = nullptr;

    xr_string r;
    for (const shared_str& g : groups)
    {
        const bool open = b.open_groups.count(g) != 0;
        u32 unread = 0, n = 0;
        xr_vector<const Art*> items;
        for (const Art& a : b.arts)
            if (a.group_key == g)
            {
                items.push_back(&a);
                ++n;
                unread += a.readed ? 0 : 1;
            }
        if (b.sorted)
            std::sort(items.begin(), items.end(), [](const Art* x, const Art* y) { return x->name < y->name; });
        else
            std::reverse(items.begin(), items.end()); // the newest first
        r += fmt("<div class=\"row grp%s%s\" data-act=\"grp:%s\"><div class=\"gmark\"/><div class=\"gname\">%s</div><div class=\"gn\">%u</div></div>", open ? " open" : "",
                 unread ? " new" : "", g.c_str(), title_of(g).c_str(), n);
        if (!open)
            continue;
        for (const Art* a : items)
            r += fmt("<div class=\"row auto art%s%s\" data-act=\"art:%s\">%s</div>", a->id == b.sel ? " sel" : "", a->readed ? "" : " new", a->id.c_str(), a->name.c_str());
    }
    if (b.arts.empty())
        r = "<div class=\"empty\">" + txt("no_articles") + "</div>";
    set_inner(b.list_id, r);
    build_art_detail(b);
}

// ================================================================ the news ====

int s_journal_mode = 0; // 0 the news, 1 the diary

void build_news()
{
    xr_string r;
    CActor* a = Actor();
    if (a)
    {
        GAME_NEWS_VECTOR& news = a->game_news_registry->registry().objects();
        u32 n = 0;
        for (auto it = news.rbegin(); it != news.rend() && n < 300; ++it, ++n)
        {
            r += "<div class=\"news\">";
            if (it->texture_name.size())
                r += img_tag(it->texture_name.c_str(), it->tex_rect, "nicon");
            else
                r += "<div class=\"nicon none\"/>";
            r += "<div class=\"nbody\"><div class=\"ntime\">" + html(time_text(it->receive_time).c_str()) + "</div>";
            r += "<div class=\"ntext\">" + rich(tr(it->news_text.c_str()).c_str()) + "</div></div></div>";
        }
    }
    if (r.empty())
        r = "<div class=\"empty\">" + txt("no_news") + "</div>";
    set_inner("n-list", r);
}

void build_journal()
{
    if (Rml::Element* p = by_id("pane-2"))
        p->SetClass("diary", s_journal_mode == 1);
    for (int i = 0; i < 2; ++i)
        if (Rml::Element* b = by_id(fmt("jf-%d", i).c_str()))
            b->SetClass("on", i == s_journal_mode);
    if (Rml::Element* b = by_id("jf-1"))
        b->SetClass("new", (g_pda_info_state & (pda_section::journal & ~pda_section::diary)) != 0);
    if (s_journal_mode == 0)
        build_news();
    else
        build_arts(s_diary);
}

// ============================================================ the contacts ====

xr_vector<u16> s_contact_ids;
u16 s_contact_sel = u16(-1);
float s_contacts_t = 0.f;

xr_vector<u16> current_contacts()
{
    xr_vector<u16> ids;
    CActor* a = Actor();
    CPda* pda = a ? a->GetPDA() : nullptr;
    if (!pda)
        return ids;
    for (const auto& [id, p] : pda->ActivePDAContacts())
        ids.push_back(id);
    return ids;
}

struct CharCard
{
    xr_string name, community, rank, reputation, relation, bio;
    u32 relation_colour{0xffb3a381};
    shared_str icon;
    bool ok{};
};

CharCard card_of(u16 id)
{
    CharCard c;
    CSE_ALifeTraderAbstract* T = ch_info_get_from_id(id);
    if (!T)
        return c;
    CCharacterInfo info;
    info.Init(T);
    c.ok = true;
    c.name = html(T->name_translated().c_str());
    c.community = html(tr(info.Community().id().c_str()).c_str());
    c.rank = html(tr(InventoryUtilities::GetRankAsText(info.Rank().value())).c_str());
    c.reputation = html(tr(InventoryUtilities::GetReputationAsText(info.Reputation().value())).c_str());
    c.icon = info.IconName();
    c.bio = rich(tr(info.Bio().c_str()).c_str());
    CActor* a = Actor();
    if (a && id != a->ID())
        if (CSE_ALifeTraderAbstract* TA = ch_info_get_from_id(a->ID()))
        {
            const ALife::ERelationType rel = RELATION_REGISTRY().GetRelationType(T, TA);
            c.relation = html(tr(InventoryUtilities::GetGoodwillAsText(RELATION_REGISTRY().GetAttitude(T, TA))).c_str());
            c.relation_colour = InventoryUtilities::GetRelationColor(rel);
        }
    return c;
}

void build_contact_detail()
{
    const CharCard c = s_contact_sel != u16(-1) ? card_of(s_contact_sel) : CharCard{};
    if (!c.ok)
    {
        set_inner("c-detail", "<div class=\"empty\">" + txt(s_contact_ids.empty() ? "no_contacts" : "pick_contact") + "</div>");
        return;
    }
    xr_string r = "<div class=\"card big\">" + portrait_tag(c.icon, "cpic") + "<div class=\"cinfo\">";
    r += "<div class=\"cname\">" + c.name + "</div>";
    r += "<div class=\"kv\"><span class=\"k\">" + txt("community") + "</span><span class=\"v\">" + c.community + "</span></div>";
    r += "<div class=\"kv\"><span class=\"k\">" + txt("rank") + "</span><span class=\"v\">" + c.rank + "</span></div>";
    r += "<div class=\"kv\"><span class=\"k\">" + txt("reputation") + "</span><span class=\"v\">" + c.reputation + "</span></div>";
    if (!c.relation.empty())
        r += "<div class=\"kv\"><span class=\"k\">" + txt("relation") + "</span><span class=\"v\" style=\"color:" + css_colour(c.relation_colour) + ";\">" + c.relation + "</span></div>";
    r += "</div></div>";
    if (!c.bio.empty())
        r += "<div class=\"d-cap\">" + txt("bio") + "</div><div class=\"d-text\">" + c.bio + "</div>";
    set_inner("c-detail", r);
}

void build_contacts()
{
    s_contact_ids = current_contacts();
    if (s_contact_sel != u16(-1) && std::find(s_contact_ids.begin(), s_contact_ids.end(), s_contact_sel) == s_contact_ids.end())
        s_contact_sel = u16(-1);
    if (s_contact_sel == u16(-1) && !s_contact_ids.empty())
        s_contact_sel = s_contact_ids.front();
    xr_string r;
    for (u16 id : s_contact_ids)
    {
        const CharCard c = card_of(id);
        if (!c.ok)
            continue;
        r += fmt("<div class=\"row auto contact%s\" data-act=\"contact:%u\">", id == s_contact_sel ? " sel" : "", u32(id));
        r += portrait_tag(c.icon, "cicon");
        r += "<div class=\"cbody\"><div class=\"cn\">" + c.name + "</div><div class=\"cc\">" + c.community + ", " + c.rank + "</div>";
        if (!c.relation.empty())
            r += "<div class=\"cr\" style=\"color:" + css_colour(c.relation_colour) + ";\">" + c.relation + "</div>";
        r += "</div></div>";
    }
    if (r.empty())
        r = "<div class=\"empty\">" + txt("no_contacts") + "</div>";
    set_inner("c-list", r);
    build_contact_detail();
    s_contacts_t = 1.f;
}

// ========================================================== the statistics ====

struct StatPart
{
    shared_str id;
    xr_string caption; // RML
    bool counts{};
    bool header{}; // "foo": a caption with no number
};
xr_vector<StatPart> s_stat_parts;
xr_vector<shared_str> s_rel_communities;
bool s_stats_loaded = false;
shared_str s_stat_sel;

void load_stats()
{
    if (s_stats_loaded)
        return;
    s_stats_loaded = true;
    CUIXml xml;
    if (!xml.Init(CONFIG_PATH, UI_PATH, "actor_statistic.xml"))
        return;
    XML_NODE* root = xml.NavigateToNode("actor_stats_wnd", 0);
    if (!root)
        return;
    xml.SetLocalRoot(root);
    const int n = xml.GetNodesNum(root, "master_part");
    for (int i = 0; i < n; ++i)
    {
        XML_NODE* mp = xml.NavigateToNode("master_part", i);
        if (!mp)
            continue;
        StatPart p;
        p.id = xml.ReadAttrib(mp, "id", "");
        p.counts = xml.ReadAttribInt(mp, "show_counts", 0) != 0;
        p.header = p.id == "foo";
        if (XML_NODE* t1 = xml.NavigateToNode(mp, "text_1", 0))
            p.caption = html(tr(xml.Read(t1, "text", 0, "")).c_str());
        if (p.id.size() && p.id != "total")
            s_stat_parts.push_back(std::move(p));
    }
    if (XML_NODE* rl = xml.NavigateToNode("relation_communities_list", 0))
    {
        const int c = xml.GetNodesNum(rl, "r");
        for (int i = 0; i < c; ++i)
            s_rel_communities.push_back(xml.Read(rl, "r", i, "unknown_community"));
    }
}

void build_stat_detail()
{
    CActor* a = Actor();
    if (!a || !s_stat_sel.size())
    {
        set_inner("s-detail", "");
        return;
    }
    xr_string r;
    if (s_stat_sel == "reputation")
    {
        r += "<div class=\"d-title\">" + html(tr("st_detail_list_for_community_relations").c_str()) + "</div>";
        CHARACTER_REPUTATION rep_actor, rep_neutral;
        rep_actor.set(a->Reputation());
        rep_neutral.set(NEUTAL_REPUTATION);
        const CHARACTER_GOODWILL d_neutral = CHARACTER_REPUTATION::relation(rep_actor.index(), rep_neutral.index());
        for (const shared_str& cname : s_rel_communities)
        {
            CHARACTER_COMMUNITY comm;
            comm.set(cname.c_str());
            CHARACTER_GOODWILL gw = RELATION_REGISTRY().GetCommunityGoodwill(comm.index(), a->ID());
            gw += CHARACTER_COMMUNITY::relation(a->Community(), comm.index());
            gw += d_neutral;
            r += fmt("<div class=\"srow\"><span class=\"sk\">%s</span><span class=\"sv\" style=\"color:%s;\">%s</span><span class=\"sp\">%d</span></div>",
                     html(tr(comm.id().c_str()).c_str()).c_str(), css_colour(InventoryUtilities::GetGoodwillColor(gw)).c_str(),
                     html(tr(InventoryUtilities::GetGoodwillAsText(gw)).c_str()).c_str(), int(gw));
        }
    }
    else
    {
        r += "<div class=\"d-title\">" + html(tr(fmt("st_detail_list_for_%s", s_stat_sel.c_str()).c_str()).c_str()) + "</div>";
        SStatSectionData& sect = a->StatisticMgr().GetSection(s_stat_sel);
        int k = 1;
        for (const SStatDetailBData& d : sect.data)
        {
            if (d.str_value.size())
                r += fmt("<div class=\"srow\"><span class=\"sn\">%d.</span><span class=\"sk\">%s</span><span class=\"sv\">%s</span><span class=\"sp\"/></div>", k++,
                         html(tr(d.key.c_str()).c_str()).c_str(), html(d.str_value.c_str()).c_str());
            else
                r += fmt("<div class=\"srow\"><span class=\"sn\">%d.</span><span class=\"sk\">%s</span><span class=\"sv\">x%d</span><span class=\"sp\">%d</span></div>", k++,
                         html(tr(d.key.c_str()).c_str()).c_str(), d.int_count, d.int_points);
        }
        if (sect.data.empty())
            r += "<div class=\"empty\">" + txt("nothing_yet") + "</div>";
    }
    set_inner("s-detail", r);
}

void build_stats()
{
    load_stats();
    CActor* a = Actor();
    if (!a)
        return;

    // The actor's card.
    {
        const CharCard c = card_of(a->ID());
        xr_string r = "<div class=\"card\">" + portrait_tag(c.icon, "cpic") + "<div class=\"cinfo\">";
        r += "<div class=\"cname\">" + c.name + "</div>";
        r += "<div class=\"kv\"><span class=\"k\">" + txt("community") + "</span><span class=\"v\">" + c.community + "</span></div>";
        r += "<div class=\"kv\"><span class=\"k\">" + txt("rank") + "</span><span class=\"v\">" + c.rank + "</span></div>";
        r += "<div class=\"kv\"><span class=\"k\">" + txt("reputation") + "</span><span class=\"v\" style=\"color:" + css_colour(InventoryUtilities::GetReputationColor(a->Reputation())) +
            ";\">" + c.reputation + "</span></div>";
        r += fmt("<div class=\"kv\"><span class=\"k\">%s</span><span class=\"v\">%d RU</span></div>", txt("money").c_str(), int(a->get_money()));
        r += "</div></div>";
        set_inner("s-card", r);
    }

    if (!s_stat_sel.size())
        for (const StatPart& p : s_stat_parts)
            if (!p.header)
            {
                s_stat_sel = p.id;
                break;
            }
    xr_string r;
    for (const StatPart& p : s_stat_parts)
    {
        if (p.header)
        {
            r += "<div class=\"shead\">" + p.caption + "</div>";
            continue;
        }
        xr_string value;
        if (p.id == "reputation")
            value = html(tr(InventoryUtilities::GetReputationAsText(a->Reputation())).c_str());
        else
        {
            const s32 v = p.counts ? a->StatisticMgr().GetSectionCounts(p.id) : a->StatisticMgr().GetSectionPoints(p.id);
            if (v != -1)
                value = fmt("%d", v);
        }
        r += fmt("<div class=\"row auto stat%s\" data-act=\"stat:%s\"><span class=\"sk\">%s</span><span class=\"sv\">%s</span></div>", p.id == s_stat_sel ? " sel" : "", p.id.c_str(),
                 p.caption.c_str(), value.c_str());
    }
    set_inner("s-list", r);
    build_stat_detail();
}

// =============================================================== the frame ====

Rml::Element* pane_el(int ept) { return by_id(fmt("pane-%d", ept).c_str()); }

void build_tab(int ept)
{
    s_dirty[ept] = false;
    switch (ept)
    {
    case eptQuests: build_tasks(); break;
    case eptMap: s_canvas_for = "-"; break;
    case eptDiary: build_journal(); break;
    case eptContacts: build_contacts(); break;
    case eptActorStatistic: build_stats(); break;
    case eptEncyclopedia: build_arts(s_encyc); break;
    default: break;
    }
}

// Keeps the scroll of the lists across a rebuild of the same tab.
void rebuild_keeping_scroll(int ept)
{
    xr_vector<std::pair<Rml::String, float>> scroll;
    if (Rml::Element* p = pane_el(ept))
    {
        Rml::ElementList cols;
        p->GetElementsByClassName(cols, "scroll");
        for (Rml::Element* c : cols)
            if (!c->GetId().empty())
                scroll.emplace_back(c->GetId(), c->GetScrollTop());
    }
    build_tab(ept);
    for (auto& [id, top] : scroll)
        if (Rml::Element* c = by_id(id.c_str()))
            c->SetScrollTop(top);
}

void show_tab(int ept)
{
    ept = normal_tab(ept);
    const bool changed = ept != s_tab;
    if (changed)
        s_col = 0; // the pad starts on the list of a new tab
    s_tab = ept;
    s_last_tab = ept;
    for (int i = 0; i < kTabCount; ++i)
    {
        if (Rml::Element* t = by_id(fmt("tab-%d", kTabs[i]).c_str()))
            t->SetClass("on", kTabs[i] == ept);
        if (Rml::Element* p = pane_el(kTabs[i]))
            p->SetClass("shown", kTabs[i] == ept);
    }
    if (s_doc)
        s_doc->SetAttribute("data-tab", fmt("%d", ept).c_str());
    build_tab(ept);
    g_pda_info_state &= ~tab_bits(ept);
    // The infos the legacy SetActiveSubdialog gave (bind_stalker turns them into signals).
    switch (ept)
    {
    case eptDiary: InventoryUtilities::SendInfoToActor("ui_pda_events"); break;
    case eptContacts: InventoryUtilities::SendInfoToActor("ui_pda_contacts"); break;
    case eptMap: InventoryUtilities::SendInfoToActor("ui_pda_map_local"); break;
    case eptEncyclopedia: InventoryUtilities::SendInfoToActor("ui_pda_encyclopedia"); break;
    case eptActorStatistic: InventoryUtilities::SendInfoToActor("ui_pda_actor_info"); break;
    default: break;
    }
    if (changed)
        Msg("~ [rml] pda: tab %d", ept);
}

void update_tab_marks()
{
    for (int t : kTabs)
        if (Rml::Element* e = by_id(fmt("tab-%d", t).c_str()))
            e->SetClass("new", t != s_tab && (g_pda_info_state & tab_bits(t)) != 0);
}

void update_clock()
{
    static xr_string last;
    xr_string now = xr_string(InventoryUtilities::GetGameTimeAsString(InventoryUtilities::etpTimeToMinutes).c_str());
    if (now == last)
        return;
    last = now;
    set_inner("pda-time", html(now.c_str()));
    set_inner("pda-date", html(InventoryUtilities::GetGameDateAsString(InventoryUtilities::edpDateToDay).c_str()));
}

// ------------------------------------------------------------- the actions ----

void show_objective_on_map(SGameTaskObjective& o)
{
    CMapLocation* ml = o.LinkedMapLocation();
    if (!ml || !ml->SpotEnabled())
        return;
    load_maps();
    const Fvector2 p = ml->Position();
    const MapInfo* m = map_of(ml->LevelName());
    if (!m)
        return;
    show_tab(eptMap);
    s_map_inited = true;
    map_show(m->name, &p, 3.f);
    s_focus = level_uv(*m, p);
    s_focus_left = 3.f;
}

void act(const Rml::String& what)
{
    const xr_string a = what.c_str();
    const size_t colon = a.find(':');
    const xr_string verb = a.substr(0, colon);
    const xr_string arg = colon == xr_string::npos ? xr_string() : a.substr(colon + 1);

    if (verb == "tab")
        show_tab(atoi(arg.c_str()));
    else if (verb == "close")
        UIRml::ScreenClose("pda");
    else if (verb == "tf")
    {
        s_task_filter = atoi(arg.c_str());
        build_tasks();
    }
    else if (verb == "task")
    {
        const int i = atoi(arg.c_str());
        if (i >= 0 && size_t(i) < s_task_ids.size())
        {
            s_task_sel = s_task_ids[size_t(i)];
            if (Rml::Element* list = by_id("t-list"))
            {
                Rml::ElementList rows;
                list->GetElementsByClassName(rows, "task");
                for (Rml::Element* r : rows)
                    r->SetClass("sel", r->GetAttribute<Rml::String>("data-act", "") == what);
            }
            build_task_detail(task_by_id(s_task_sel));
        }
    }
    else if (verb == "oact" || verb == "omap")
    {
        CGameTask* t = task_by_id(s_task_sel);
        const u32 i = u32(atoi(arg.c_str()));
        if (!t || i >= t->m_Objectives.size())
            return;
        SGameTaskObjective& o = t->Objective(int(i));
        if (verb == "omap")
            show_objective_on_map(o);
        else if (o.TaskState() == eTaskStateInProgress)
        {
            // CUITaskSubItem::OnActiveObjectiveClicked: a second press stops tracking.
            CActor* actor = Actor();
            const bool was = actor->GameTaskManager().ActiveObjective() == &o;
            actor->GameTaskManager().SetActiveTask(was ? shared_str("") : t->m_ID, u16(i));
            rebuild_keeping_scroll(eptQuests);
        }
    }
    else if (verb == "jf")
    {
        s_journal_mode = atoi(arg.c_str()) ? 1 : 0;
        if (s_journal_mode == 1)
            g_pda_info_state &= ~(pda_section::journal & ~pda_section::diary);
        build_journal();
    }
    else if (verb == "grp" || verb == "art")
    {
        ArtBrowser& b = s_tab == eptEncyclopedia ? s_encyc : s_diary;
        const shared_str key = arg.c_str();
        if (verb == "grp")
        {
            if (b.open_groups.count(key))
                b.open_groups.erase(key);
            else
                b.open_groups.insert(key);
        }
        else
            b.sel = key;
        // the whole list: a group's rows appear or go, the read marks change
        Rml::Element* list = by_id(b.list_id);
        const float top = list ? list->GetScrollTop() : 0.f;
        build_arts(b);
        if ((list = by_id(b.list_id)))
            list->SetScrollTop(top);
    }
    else if (verb == "contact")
    {
        s_contact_sel = u16(atoi(arg.c_str()));
        if (Rml::Element* list = by_id("c-list"))
        {
            Rml::ElementList rows;
            list->GetElementsByClassName(rows, "contact");
            for (Rml::Element* r : rows)
                r->SetClass("sel", r->GetAttribute<Rml::String>("data-act", "") == what);
        }
        build_contact_detail();
        if (g_actor)
            g_actor->callback(GameObject::eSelectPdaContact)(s_contact_sel);
    }
    else if (verb == "stat")
    {
        s_stat_sel = arg.c_str();
        if (Rml::Element* list = by_id("s-list"))
        {
            Rml::ElementList rows;
            list->GetElementsByClassName(rows, "stat");
            for (Rml::Element* r : rows)
                r->SetClass("sel", r->GetAttribute<Rml::String>("data-act", "") == what);
        }
        build_stat_detail();
    }
    else if (verb == "lvl")
    {
        load_maps();
        if (const MapInfo* m = map_of(arg.c_str()))
            map_show(m->name, nullptr, 1.f);
    }
    else if (verb == "map")
    {
        const ViewGeom g = view_geom();
        if (arg == "global")
        {
            if (s_map_level.size())
                map_show(shared_str(), nullptr, 1.f);
            else
                map_show_actor();
        }
        else if (arg == "me")
            map_show_actor();
        else if (arg == "in")
            map_zoom(1.5f, g.vx + g.vw * 0.5f, g.vy + g.vh * 0.5f);
        else if (arg == "out")
            map_zoom(1.f / 1.5f, g.vx + g.vw * 0.5f, g.vy + g.vh * 0.5f);
    }
}

// ----------------------------------------------------------------- the pad ----

xr_vector<Rml::Element*> pane_cols()
{
    xr_vector<Rml::Element*> out;
    Rml::Element* p = pane_el(s_tab);
    if (!p)
        return out;
    Rml::ElementList cols;
    p->GetElementsByClassName(cols, "col");
    for (Rml::Element* c : cols)
        if (box_size(c).x > 1.f && box_size(c).y > 1.f)
            out.push_back(c);
    return out;
}


xr_vector<Rml::Element*> col_rows(Rml::Element* col)
{
    xr_vector<Rml::Element*> out;
    Rml::ElementList rows;
    col->GetElementsByClassName(rows, "row");
    // ⚠No size test: rows just put in by a rebuild have no layout until the next update,
    // and lighting the same row again right after the rebuild is exactly that case.
    for (Rml::Element* r : rows)
        out.push_back(r);
    return out;
}

// The scroll container a row sits in (the column itself, or a list inside it).
Rml::Element* scroller_of(Rml::Element* e, Rml::Element* col)
{
    for (Rml::Element* p = e->GetParentNode(); p; p = p->GetParentNode())
    {
        if (p->IsClassSet("scroll"))
            return p;
        if (p == col)
            break;
    }
    return col;
}

void ensure_visible(Rml::Element* row, Rml::Element* col)
{
    Rml::Element* sc = scroller_of(row, col);
    const float top = abs_pos(row).y - abs_pos(sc).y;
    const float h = box_size(row).y, ch = sc->GetClientHeight();
    if (top < 0.f)
        sc->SetScrollTop(sc->GetScrollTop() + top - 8.f);
    else if (top + h > ch)
        sc->SetScrollTop(sc->GetScrollTop() + top + h - ch + 8.f);
}

void mark_focus(const xr_vector<Rml::Element*>& cols)
{
    for (size_t i = 0; i < cols.size(); ++i)
        cols[i]->SetClass("focus", s_pad_mode && int(i) == s_col);
}

void pad_move(int dy)
{
    xr_vector<Rml::Element*> cols = pane_cols();
    if (cols.empty())
        return;
    clamp(s_col, 0, int(cols.size()) - 1);
    Rml::Element* col = cols[size_t(s_col)];
    const xr_vector<Rml::Element*> rows = col_rows(col);
    if (rows.empty())
    {
        // Text only: scroll it.
        Rml::Element* sc = col->IsClassSet("scroll") ? col : nullptr;
        if (!sc)
        {
            Rml::ElementList l;
            col->GetElementsByClassName(l, "scroll");
            sc = l.empty() ? col : l.front();
        }
        sc->SetScrollTop(sc->GetScrollTop() + dy * sc->GetClientHeight() * 0.25f);
        mark_focus(cols);
        return;
    }
    int cur = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i]->IsClassSet("cur"))
            cur = int(i);
    if (cur < 0)
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i]->IsClassSet("sel"))
                cur = int(i);
    const int was = cur;
    cur = cur < 0 ? 0 : cur + dy;
    clamp(cur, 0, int(rows.size()) - 1);
    for (Rml::Element* r : rows)
        r->SetClass("cur", false);
    rows[size_t(cur)]->SetClass("cur", true);
    ensure_visible(rows[size_t(cur)], col);
    mark_focus(cols);
    // A list row shows its details as soon as it is lit.
    if (cur != was && rows[size_t(cur)]->IsClassSet("auto"))
    {
        const Rml::String what = rows[size_t(cur)]->GetAttribute<Rml::String>("data-act", "");
        act(what);
        // the rebuild may have replaced the row: light the new one
        for (Rml::Element* r : col_rows(cols[size_t(s_col)]))
            r->SetClass("cur", r->GetAttribute<Rml::String>("data-act", "") == what);
    }
}

void pad_col(int dx)
{
    xr_vector<Rml::Element*> cols = pane_cols();
    if (cols.empty())
        return;
    s_col = std::clamp(s_col + dx, 0, int(cols.size()) - 1);
    Rml::Element* col = cols[size_t(s_col)];
    const xr_vector<Rml::Element*> rows = col_rows(col);
    bool lit = false;
    for (Rml::Element* r : rows)
        lit |= r->IsClassSet("cur");
    if (!lit && !rows.empty())
    {
        rows.front()->SetClass("cur", true);
        ensure_visible(rows.front(), col);
    }
    mark_focus(cols);
}

void pad_a()
{
    xr_vector<Rml::Element*> cols = pane_cols();
    if (cols.empty())
        return;
    clamp(s_col, 0, int(cols.size()) - 1);
    for (Rml::Element* r : col_rows(cols[size_t(s_col)]))
        if (r->IsClassSet("cur"))
        {
            const Rml::String what = r->GetAttribute<Rml::String>("data-act", "");
            act(what);
            // keep the light on the same row after a rebuild
            xr_vector<Rml::Element*> again = pane_cols();
            if (size_t(s_col) < again.size())
                for (Rml::Element* x : col_rows(again[size_t(s_col)]))
                    x->SetClass("cur", x->GetAttribute<Rml::String>("data-act", "") == what);
            return;
        }
    pad_move(0);
}

void next_tab(int d)
{
    int i = tab_index(s_tab);
    i = (i + d + kTabCount) % kTabCount;
    s_col = 0;
    show_tab(kTabs[i]);
}

// Auto-repeat of a held direction (PC arrows, the Switch's stick and D-pad).
int s_rep_dy = 0;
int s_rep_dik = 0;
float s_rep_t = 0.f;

// ----------------------------------------------------------- the listener ----

struct PdaListener final : public Rml::EventListener
{
    void ProcessEvent(Rml::Event& ev) override
    {
        Rml::Element* t = ev.GetTargetElement();
        switch (ev.GetId())
        {
        case Rml::EventId::Mousemove: {
            const float x = ev.GetParameter<float>("mouse_x", s_mx), y = ev.GetParameter<float>("mouse_y", s_my);
            if (s_drag)
            {
                const ViewGeom g = view_geom();
                if (std::fabs(x - s_drag_x) + std::fabs(y - s_drag_y) > 6.f)
                    s_dragged = true;
                if (g.cw > 0.f)
                {
                    s_cx = s_drag_cx - (x - s_drag_x) / g.cw;
                    s_cy = s_drag_cy - (y - s_drag_y) / g.ch;
                }
            }
            if (std::fabs(x - s_mx) + std::fabs(y - s_my) > 0.5f && s_pad_mode)
            {
                s_pad_mode = false;
                mark_focus(pane_cols());
            }
            s_mx = x, s_my = y;
        }
        break;

        case Rml::EventId::Mousedown:
            if (ev.GetParameter<int>("button", 0) == 0 && s_tab == eptMap)
            {
                for (Rml::Element* e = t; e; e = e->GetParentNode())
                    if (e->GetId() == "map-view")
                    {
                        s_drag = true;
                        s_dragged = false;
                        s_drag_x = ev.GetParameter<float>("mouse_x", s_mx);
                        s_drag_y = ev.GetParameter<float>("mouse_y", s_my);
                        s_drag_cx = s_cx, s_drag_cy = s_cy;
                        break;
                    }
            }
            break;

        case Rml::EventId::Mouseup:
            if (ev.GetParameter<int>("button", 0) == 0)
                s_drag = false;
            break;

        case Rml::EventId::Mousescroll:
            if (s_tab == eptMap)
                for (Rml::Element* e = t; e; e = e->GetParentNode())
                    if (e->GetId() == "map-view")
                    {
                        const float d = ev.GetParameter<float>("wheel_delta_y", 0.f);
                        if (d != 0.f)
                            map_zoom(d < 0.f ? 1.25f : 0.8f, s_mx, s_my);
                        ev.StopPropagation();
                        break;
                    }
            break;

        case Rml::EventId::Click:
            if (s_dragged)
            {
                s_dragged = false; // the end of a pan, not a click
                break;
            }
            if (Rml::Element* a = up_to_act(t))
                act(a->GetAttribute<Rml::String>("data-act", ""));
            break;

        case Rml::EventId::Dblclick:
            if (s_tab == eptMap)
            {
                if (!s_map_level.size())
                    map_enter_at(s_mx, s_my);
                else
                    map_zoom(2.f, s_mx, s_my);
            }
            break;

        default: break;
        }
    }
} s_listener;

void reset_view_state()
{
    for (bool& a : s_armed)
        a = false;
    s_pad_mode = false;
    s_drag = s_dragged = false;
    s_hint_spot = -1;
    s_col = 0;
    s_rep_dy = 0;
    s_rep_dik = 0;
}
} // namespace

// ================================================================= the API ====

void UIRmlInternal::PdaBuilt(Rml::ElementDocument* doc, int tab)
{
    s_doc = doc;
    reset_view_state();
    for (bool& d : s_dirty)
        d = true;
    for (Rml::EventId id : {Rml::EventId::Mousemove, Rml::EventId::Mousedown, Rml::EventId::Mouseup, Rml::EventId::Mousescroll, Rml::EventId::Click, Rml::EventId::Dblclick})
        doc->AddEventListener(id, &s_listener, false);
    // The actor's name in the corner.
    if (CActor* a = Actor())
        set_inner("pda-owner", html(static_cast<CInventoryOwner*>(a)->Name()));
    s_map_inited = false; // the map opens on the actor
    update_clock();
    show_tab(tab >= 0 && tab != eptNoActiveTab ? tab : s_last_tab);
    update_tab_marks();
}

void UIRmlInternal::PdaFrame()
{
    if (!s_doc)
        return;
    update_clock();
    static float mark_t = 0.f;
    mark_t -= Device.fTimeDelta;
    if (mark_t <= 0.f)
    {
        mark_t = 0.5f;
        update_tab_marks();
    }
    if (s_dirty[s_tab])
        rebuild_keeping_scroll(s_tab);

    if (s_tab == eptMap)
        map_frame();
    else if (s_rep_dik && s_rep_dy)
    {
        // held direction: repeat after 0.35 s, every 0.09 s
        if (!held(s_rep_dik))
            s_rep_dik = 0;
        else if ((s_rep_t -= Device.fTimeDelta) <= 0.f)
        {
            s_rep_t = 0.09f;
            pad_move(s_rep_dy);
        }
    }

    if (s_tab == eptContacts && (s_contacts_t -= Device.fTimeDelta) <= 0.f)
    {
        s_contacts_t = 1.f;
        if (current_contacts() != s_contact_ids)
            rebuild_keeping_scroll(eptContacts);
    }
}

bool UIRmlInternal::PdaKey(int dik, PadBtn pad)
{
    if (!s_doc)
        return false;
    if (dik > 0 && dik < 256)
        s_armed[dik] = true; // pressed with the PDA up: may drive the map while held

    // The PDA's own keys: P / M / C -- its tab again closes it, another tab switches.
    const EGameActions bind = get_binded_action(dik);
    if (bind == kACTIVE_JOBS || bind == kMAP || bind == kCONTACTS)
    {
        const int want = bind == kACTIVE_JOBS ? eptQuests : bind == kMAP ? eptMap : eptContacts;
#ifdef __SWITCH__
        UIRml::ScreenClose("pda"); // Minus held: the same button that opened it
#else
        if (want == s_tab)
            UIRml::ScreenClose("pda");
        else
            show_tab(want);
#endif
        return true;
    }

    // Tabs from the keyboard.
    if (dik == DIK_Q || dik == DIK_PRIOR)
        pad = PadBtn::L;
    else if (dik == DIK_E || dik == DIK_NEXT)
        pad = PadBtn::R;
    else if (dik >= DIK_1 && dik < DIK_1 + kTabCount)
    {
        s_col = 0;
        show_tab(kTabs[dik - DIK_1]);
        return true;
    }

    switch (pad)
    {
    case PadBtn::L: next_tab(-1); return true;
    case PadBtn::R: next_tab(1); return true;
    case PadBtn::B: UIRml::ScreenClose("pda"); return true;
    default: break;
    }

    if (s_tab == eptMap)
    {
        // Held keys drive the map (map_frame); the presses only turn the pad mode on.
        switch (pad)
        {
        case PadBtn::Up:
        case PadBtn::Down:
        case PadBtn::Left:
        case PadBtn::Right:
        case PadBtn::X:
        case PadBtn::Y: s_pad_mode = true; return true;
        case PadBtn::A: {
            s_pad_mode = true;
            const ViewGeom g = view_geom();
            if (!s_map_level.size())
                map_enter_at(g.vx + g.vw * 0.5f, g.vy + g.vh * 0.5f);
            else
                map_show_actor();
            return true;
        }
        default: break;
        }
        return dik == DIK_EQUALS || dik == DIK_MINUS || dik == DIK_ADD || dik == DIK_SUBTRACT;
    }

    switch (pad)
    {
    case PadBtn::Up:
    case PadBtn::Down:
        s_pad_mode = true;
        s_rep_dy = pad == PadBtn::Up ? -1 : 1;
        s_rep_dik = dik;
        s_rep_t = 0.35f;
        pad_move(s_rep_dy);
        return true;
    case PadBtn::Left:
    case PadBtn::Right:
        s_pad_mode = true;
        pad_col(pad == PadBtn::Left ? -1 : 1);
        return true;
    case PadBtn::A:
        s_pad_mode = true;
        pad_a();
        return true;
    case PadBtn::X:
        if (s_tab == eptQuests)
            act(fmt("tf:%d", (s_task_filter + 1) % 3).c_str());
        else if (s_tab == eptDiary)
            act(fmt("jf:%d", s_journal_mode ? 0 : 1).c_str());
        return true;
    default: break;
    }
    return false;
}

void UIRmlInternal::PdaClosed()
{
    s_doc = nullptr;
    s_drag = false;
    s_spots.clear();
    if (g_hud)
        if (CUI* ui = g_hud->GetUI())
            if (ui->UIMainIngameWnd)
                ui->UIMainIngameWnd->SetFlashIconState_(CUIMainIngameWnd::efiPdaTask, false); // CUIPdaWnd::Hide
    // The 3D PDA back out of the hands.
    if (s_from_hands)
    {
        s_from_hands = false;
        if (CActor* a = Actor())
            if (a->inventory().GetActiveSlot() == PDA_SLOT && a->inventory().ActiveItem())
                a->inventory().Activate(NO_ACTIVE_SLOT);
    }
}

bool UIRmlInternal::PdaPadMode() { return s_pad_mode; }

void UIRmlInternal::PdaPendingFrame()
{
    if (!s_pending)
        return;
    CActor* a = Actor();
    PIItem active = a ? a->inventory().ActiveItem() : nullptr;
    if (!a || !a->g_Alive() || !smart_cast<CPda*>(active))
    {
        s_pending = false; // put away (or swapped) before the screen came up
        return;
    }
    auto hud = smart_cast<CHudItem*>(active);
    const bool drawn = !hud || hud->GetState() == CHudItem::eIdle;
    if (!drawn && Device.dwTimeGlobal - s_pending_t0 < 1500)
        return;
    s_pending = false;
    if (UIRml::PdaOpen(s_pending_tab))
        s_from_hands = true;
    else
        a->inventory().Activate(NO_ACTIVE_SLOT); // no screen: no PDA left standing in the hands
}

void UIRml::PdaOpenWhenDrawn(int tab)
{
    s_pending = true;
    s_pending_tab = tab;
    s_pending_t0 = Device.dwTimeGlobal;
}

void UIRml::PdaCancelPending()
{
    s_pending = false;
    s_from_hands = false; // going out of the hands already: the close must not put it away again
}

void UIRml::PdaContentsChanged(u32 part)
{
    if (part & pda_section::quests)
        s_dirty[eptQuests] = true;
    if (part == pda_section::news)
        s_dirty[eptDiary] = true;
    if (part == pda_section::journal || part == pda_section::info)
    {
        s_dirty[eptDiary] = true;
    }
    if (part & pda_section::encyclopedia)
        s_dirty[eptEncyclopedia] = true;
    if (part & pda_section::contacts)
        s_dirty[eptContacts] = true;
    // Any change can move a map spot or a task marker.
    s_canvas_for = "-";
}

// `ui_test pda <verb> [arg]`
//   open [tab]       the screen (EPdaTabs, default: the last one)
//   tab <n>          switch (EPdaTabs)
//   act <what>       a data-act as if clicked (task:0, tf:1, omap:1, lvl:l01_escape, map:in, jf:1, ...)
//   pad <btn>        up|down|left|right|a|b|x|y|l|r
//   map <level>      the map tab on that level ("zone" = the Zone map)
//   dump             what the tab shows, to the log
void UIRmlInternal::PdaTest(const xr_string& verb, const xr_string& arg)
{
    if (verb == "open")
    {
        const bool ok = UIRml::PdaOpen(arg.empty() ? -1 : atoi(arg.c_str()));
        Msg("~ [rmltest] pda open %s: %s", arg.c_str(), ok ? "ok" : "FAILED");
        return;
    }
    if (!s_doc)
    {
        Msg("! [rmltest] pda %s: the PDA is not open", verb.c_str());
        return;
    }
    if (verb == "tab")
        show_tab(atoi(arg.c_str()));
    else if (verb == "act")
        act(arg.c_str());
    else if (verb == "map")
    {
        show_tab(eptMap);
        s_map_inited = true;
        map_show(arg == "zone" ? shared_str() : shared_str(arg.c_str()), nullptr, 1.f);
    }
    else if (verb == "pad")
    {
        static const std::pair<const char*, PadBtn> names[] = {{"up", PadBtn::Up}, {"down", PadBtn::Down}, {"left", PadBtn::Left}, {"right", PadBtn::Right},
                                                               {"a", PadBtn::A},   {"b", PadBtn::B},       {"x", PadBtn::X},       {"y", PadBtn::Y},
                                                               {"l", PadBtn::L},   {"r", PadBtn::R}};
        for (const auto& [n, b] : names)
            if (arg == n)
            {
                PdaKey(0, b);
                s_rep_dik = 0; // no key is really held
                break;
            }
    }
    else if (verb == "dump")
    {
        Msg("~ [rmltest] pda tab %d: tasks %u (filter %d, sel '%s'), contacts %u, diary %u, encyclopedia %u, map '%s' zoom %.2f spots %u", s_tab, u32(s_task_ids.size()),
            s_task_filter, s_task_sel.c_str(), u32(s_contact_ids.size()), u32(s_diary.arts.size()), u32(s_encyc.arts.size()), s_map_level.c_str(), s_zoom, u32(s_spots.size()));
        return;
    }
    else
    {
        Msg("! [rmltest] pda: unknown verb '%s'", verb.c_str());
        return;
    }
    Msg("~ [rmltest] pda %s %s", verb.c_str(), arg.c_str());
}

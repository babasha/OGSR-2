#include "StdAfx.h"
#include "xr_platform_keys.h" // DIK_* -- the raw key codes, as Level_input.cpp does
#include "cl_zone_edit.h"
#include "cl_zone_debug.h"

#include "Level.h"
#include "Actor.h"
#include "space_restrictor.h"
#include "CustomZone.h"
#include "HUDManager.h"
#include "ui_defs.h"
#include "xr_level_controller.h"
#include "../xr_3da/GameFont.h"
#include "../xr_3da/XR_IOConsole.h"

namespace
{
bool s_active = false;
float s_radius = 6.f;
u16 s_selected = 0xffff;
int s_section = 0;

Fvector s_aim{};
bool s_aim_valid = false;

xr_vector<shared_str> s_sections;

constexpr float AIM_RANGE = 120.f;
constexpr float RADIUS_MIN = 1.f;
constexpr float RADIUS_MAX = 60.f;

// ⚠⚠Every number that goes into a console command goes through here.
//
// printf writes the decimal separator of the C locale, and on a Russian locale
// that is a COMMA. The server would then read "sv_anomaly_spawn zharka 6,00
// 109,03 ..." and sscanf would stop at the comma -- the command would not be
// refused, it would be half-parsed into a DIFFERENT command with different
// arguments. That exact trap put an anomaly on the player's head once already
// (a PowerShell script, same cause), so the separator is forced here rather
// than trusted.
void fixed(float v, string32& out)
{
    xr_sprintf(out, "%.2f", v);
    for (char* p = out; *p; ++p)
        if (*p == ',')
            *p = '.';
}

void say(const char* fmt, ...)
{
    string1024 buf;
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(buf, fmt, ap);
    va_end(ap);
    Msg("~ [ed_zone] %s", buf);
}

// Everything goes to the server as a console line. Nothing here touches the
// world directly -- see the note in the header.
//
// ⚠⚠Not always an rcon line: `rcon` reaches somebody ELSE'S server, and when
// this process is the host it refuses by design ("this is the host; run it
// directly") -- which made the editor mute on a listen host, the setup an
// operator tests in. Same trap, same cure as cl_prop_edit.
void send(const char* fmt, ...)
{
    string1024 cmd;
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(cmd, fmt, ap);
    va_end(ap);

    const bool hosting = g_pGameLevel && Level().Server;

    string1024 line;
    if (hosting)
        xr_strcpy(line, cmd);
    else
        xr_sprintf(line, "rcon %s", cmd);

    Msg("~ [ed_zone] > %s%s", hosting ? "" : "rcon ", cmd);
    Console->Execute(line);
}

// Zone sections, taken from the configs the client already has. A section is
// taken to be a zone when it declares a class and carries the two fields every
// anomaly has. ⚠This CANNOT be authoritative -- the mod registers zone classes
// from Lua, so a list built from config alone may miss some
// (see the code-list-cannot-enumerate lesson). It only fills the [ ] cycle;
// ed_zone_section names anything directly, and the server validates either way.
void build_sections()
{
    if (!s_sections.empty())
        return;

    for (const auto& kv : pSettings->sections())
    {
        const shared_str& name = kv.first;
        if (!pSettings->line_exist(name, "class"))
            continue;
        if (!pSettings->line_exist(name, "max_start_power") || !pSettings->line_exist(name, "hit_type"))
            continue;
        s_sections.push_back(name);
    }
    std::sort(s_sections.begin(), s_sections.end(), [](const shared_str& a, const shared_str& b) { return xr_strcmp(a.c_str(), b.c_str()) < 0; });
    say("%u zone section(s) available; [ and ] cycle them", u32(s_sections.size()));
}

const char* current_section()
{
    if (s_sections.empty())
        return "zone_zharka_static_weak";
    if (s_section < 0)
        s_section = int(s_sections.size()) - 1;
    if (s_section >= int(s_sections.size()))
        s_section = 0;
    return s_sections[s_section].c_str();
}

// Where the operator is looking, on the world. One ray per frame.
void trace_aim()
{
    s_aim_valid = false;
    if (!g_pGameLevel || !Level().game_configured)
        return;

    collide::rq_result RQ;
    const Fvector from = Device.vCameraPosition;
    const Fvector dir = Device.vCameraDirection;
    if (!Level().ObjectSpace.RayPick(from, dir, AIM_RANGE, collide::rqtStatic, RQ, nullptr))
        return;

    s_aim.mad(from, dir, RQ.range);
    s_aim_valid = true;
}

// The zone whose centre is nearest the aim point. Nearest-to-the-aim rather
// than "the one the ray hit": a zone's collision form is not something a ray
// picks up reliably, and the operator is pointing at a place, not a surface.
u16 pick_zone()
{
    if (!s_aim_valid)
        return 0xffff;

    u16 best = 0xffff;
    float best_d = 1e9f;
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O || O->getDestroy())
            continue;
        CSpaceRestrictor* SR = smart_cast<CSpaceRestrictor*>(O);
        if (!SR || !SR->CFORM())
            continue;

        Fvector c;
        SR->Center(c);
        const float d = c.distance_to(s_aim);
        // Inside its own radius, or very close to the rim.
        if (d < SR->Radius() + 3.f && d < best_d)
        {
            best_d = d;
            best = u16(SR->ID());
        }
    }
    return best;
}

CSpaceRestrictor* find_zone(u16 id)
{
    if (id == 0xffff)
        return nullptr;
    return smart_cast<CSpaceRestrictor*>(Level().Objects.net_Find(id));
}
} // namespace

bool ClZoneEdit::Active() { return s_active; }

void ClZoneEdit::EnumerateSections(xr_vector<shared_str>& out)
{
    build_sections();
    out = s_sections;
}

void ClZoneEdit::Toggle()
{
    s_active = !s_active;
    if (s_active)
    {
        build_sections();
        // The overlay is the editor's eyes; turning it on with the editor saves
        // the operator from placing blind, which is the whole point.
        Console->Execute("cl_zone_debug 2");
        say("ON -- wheel=radius  LMB=place  RMB=pick  DEL=delete  [ ]=section  ESC=off");
        say("placing '%s' r=%.1f", current_section(), s_radius);
    }
    else
    {
        s_selected = 0xffff;
        ClZoneDebug::SetSelected(0xffff);
        say("OFF");
    }
}

void ClZoneEdit::SetSection(const char* section)
{
    if (!section || !section[0])
    {
        say("current section is '%s'", current_section());
        return;
    }
    build_sections();
    for (int i = 0; i < int(s_sections.size()); ++i)
        if (0 == xr_strcmp(s_sections[i].c_str(), section))
        {
            s_section = i;
            say("section '%s'", current_section());
            return;
        }

    // Not in the list is not an error: the list is a convenience, not the truth.
    // The server validates the section and refuses cleanly if it is wrong.
    s_sections.push_back(section);
    s_section = int(s_sections.size()) - 1;
    say("section '%s' (not in the scanned list -- the server will have the last word)", section);
}

bool ClZoneEdit::OnWheel(int direction)
{
    if (!s_active)
        return false;

    const float step = (direction > 0) ? 0.5f : -0.5f;

    if (CSpaceRestrictor* SR = find_zone(s_selected))
    {
        float r = SR->Radius() + step;
        clamp(r, RADIUS_MIN, RADIUS_MAX);
        string32 rs;
        fixed(r, rs);
        send("sv_anomaly_size %u %s", u32(s_selected), rs);
        return true;
    }

    s_radius += step;
    clamp(s_radius, RADIUS_MIN, RADIUS_MAX);
    return true;
}

bool ClZoneEdit::OnKey(int key)
{
    if (!s_active)
        return false;

    switch (key)
    {
    case DIK_ESCAPE: Toggle(); return true;

    case MOUSE_1: {
        if (!s_aim_valid)
        {
            say("nothing under the crosshair to place on");
            return true;
        }
        string32 rs, xs, ys, zs;
        fixed(s_radius, rs);
        fixed(s_aim.x, xs);
        fixed(s_aim.y, ys);
        fixed(s_aim.z, zs);
        send("sv_anomaly_spawn %s %s %s %s %s", current_section(), rs, xs, ys, zs);
        return true;
    }

    case MOUSE_2: {
        const u16 hit = pick_zone();
        if (hit == s_selected || hit == 0xffff)
        {
            s_selected = 0xffff;
            ClZoneDebug::SetSelected(0xffff);
            say("nothing selected");
        }
        else
        {
            s_selected = hit;
            ClZoneDebug::SetSelected(hit);
            CSpaceRestrictor* SR = find_zone(hit);
            say("selected id=%u '%s' r=%.1f", u32(hit), SR ? SR->cNameSect().c_str() : "?", SR ? SR->Radius() : 0.f);
        }
        return true;
    }

    case DIK_DELETE: {
        if (s_selected == 0xffff)
        {
            say("nothing selected to delete");
            return true;
        }
        send("sv_anomaly_del %u", u32(s_selected));
        s_selected = 0xffff;
        ClZoneDebug::SetSelected(0xffff);
        return true;
    }

    case DIK_LBRACKET:
        --s_section;
        say("section '%s'", current_section());
        return true;

    case DIK_RBRACKET:
        ++s_section;
        say("section '%s'", current_section());
        return true;
    }

    return false;
}

void ClZoneEdit::Frame()
{
    if (s_active)
        trace_aim();
}

void ClZoneEdit::Render()
{
    if (!s_active)
        return;

    // The ghost: where the next zone would land and how big it would be.
    if (s_aim_valid && s_selected == 0xffff)
    {
        Fvector c = s_aim;
        c.y += 0.06f;
        ClZoneDebug::DrawFlatDisc(c, s_radius, color_rgba(80, 255, 120, 255), color_rgba(60, 200, 90, 90), 1.0f);
    }

    CGameFont* F = HUD().Font().pFontSmall;
    if (!F)
        return;

    F->SetAligment(CGameFont::alLeft);
    F->SetColor(color_rgba(120, 255, 150, 255));
    F->Out(20.f, 200.f, "ZONE EDITOR");
    F->SetColor(color_rgba(255, 255, 255, 255));
    F->Out(20.f, 216.f, "section : %s", current_section());

    if (CSpaceRestrictor* SR = find_zone(s_selected))
        F->Out(20.f, 232.f, "selected: id=%u  r=%.1f m   (wheel resizes it, DEL removes it)", u32(s_selected), SR->Radius());
    else
        F->Out(20.f, 232.f, "radius  : %.1f m   (wheel changes it, LMB places)", s_radius);

    F->Out(20.f, 248.f, "aim     : %s", s_aim_valid ? "on the world" : "no surface in range");
    F->SetColor(color_rgba(190, 190, 190, 220));
    F->Out(20.f, 268.f, "LMB place   RMB pick   DEL delete   [ ] section   ESC off");
}

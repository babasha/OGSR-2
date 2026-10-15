#pragma once

// An in-game anomaly editor for the server operator.
//
// ⭐WHAT IT IS. Every piece already existed and none of them were joined up: the
// server can place, resize and delete zones (sv_anomaly_*), a client can send it
// console commands as an authenticated administrator (rcon, gated on the role in
// the signed join ticket), and cl_zone_debug can draw a zone's true volume. This
// is the handle on top: look somewhere, place a zone there, roll the wheel to
// size it, pick an existing one and change it.
//
// ⚠It sends commands rather than doing anything itself. The world belongs to the
// server; the client only ever asks. That also means the editor obeys the same
// authority check as everything else -- a player without the admin role gets
// "you are not an administrator" from the server and the world does not move.
//
//     ed_zone              toggle edit mode
//     ed_zone_section <s>  set the section to place by name
//
// While edit mode is on:
//     wheel        radius (of the selection if one is picked, else of the ghost)
//     LMB          place the ghost
//     RMB          pick the zone under the crosshair / deselect
//     DELETE       delete the selection
//     [  ]         cycle through zone sections
//     ESC          leave edit mode
namespace ClZoneEdit
{
void Toggle();
void SetSection(const char* section);
bool Active();

// The zone sections the editor knows about, for a picker to list.
void EnumerateSections(xr_vector<shared_str>& out);

// Input hooks, from CLevel. Return true when the key was consumed.
bool OnKey(int key);
bool OnWheel(int direction);

// Once per client frame, from CLevel::OnFrame -- traces the aim point.
void Frame();
// During UI rendering, from CHUDManager::RenderUI -- ghost and panel.
void Render();
} // namespace ClZoneEdit

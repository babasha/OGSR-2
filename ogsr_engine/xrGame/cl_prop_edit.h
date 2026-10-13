#pragma once

// An in-game furniture editor for the server operator.
//
// ⭐WHAT IT IS. The same handle cl_zone_edit is, over sv_prop_* instead of
// sv_anomaly_*: look somewhere, put a crate there, roll the wheel to turn it,
// nudge it until it sits right. The difference in feel is that furniture is
// placed by eye and adjusted, where an anomaly is placed and sized -- so the
// verbs here are move / turn / lift and they act on a SELECTION, and whatever
// was placed last is selected automatically. Placing then adjusting is the loop;
// making the operator re-pick the thing they just put down is not.
//
// ⚠It sends commands rather than doing anything itself. The world belongs to the
// server; the client only ever asks. That also means the editor obeys the same
// authority check as everything else -- a player without the admin role gets
// "you are not an administrator" from the server and nothing moves.
//
//     ed_prop              toggle edit mode
//     ed_prop_model <m>    set the model to place by name
//     ed_prop_ghost 0/1    draw the model under the crosshair, or just its box
//     ed_prop_align 0/1    tilt what is placed to the ground, or keep it upright
//     ed_prop_clearance 0/1  refuse to put a thing inside another thing
//     ed_prop_light_range <m> / _cone <deg> / _shadow 0/1 / _tone 0..2
//
// While edit mode is on:
//     LMB          place the current model where you are looking
//     RMB          pick the object under the crosshair / deselect
//     wheel        turn the selection (or the ghost) by 15 degrees
//     G            move the selection to where you are looking
//     PGUP/PGDN    lift the selection by 5 cm
//     DELETE       delete the selection
//     [  ]         previous / next model in the catalogue
//     H            force floor / wall / ceiling, or back to following the surface
//     L            build a LIGHT / a plain prop
//     F            place a campfire (logs and fire, as one act)
//     ESC          leave edit mode
//
// ⭐⭐WHERE a thing mounts is asked of the SURFACE, not of the operator: a floor
// stands it up, a ceiling hangs it, a wall mounts it with its back to the wall.
// H exists to override that, not to enable it. The first version of this had
// hanging behind a keystroke and the first session's log settled the argument --
// ten placements, none hung, none lit.
// ⭐WHAT gets built is a separate question (a chandelier with no bulb hangs, a
// desk lamp stands), so it has its own switch -- but picking a model out of the
// lights folder sets it, because that is what picking a lamp means.
namespace ClPropEdit
{
void Toggle();
bool Active();

// Name a model directly. Anything the server's filesystem has will do; the
// catalogue below is a convenience for the picker and for [ ].
void SetModel(const char* model);
const char* CurrentModel();

// ⭐The catalogue, read off the CLIENT's own filesystem.
//
// The same files the server will look for, because both mount the same gamedata
// -- and reading it here means the picker costs no round trip and works before
// anything has been placed. ⚠It cannot be authoritative and does not need to be:
// the server checks the model exists before it spends an entity id, and refuses
// cleanly if it does not.
//
// Groups are the top folders under the prop roots (physics\box, physics\stol,
// ...). A flat list of 647 models is a scroll, not a choice.
void EnumerateGroups(xr_vector<shared_str>& out);
void EnumerateModels(const char* group, xr_vector<shared_str>& out);

// Draw the model itself as the ghost (1) or only its outline and arrow (0).
extern int ghost_solid;
// Tilt what is placed to match the ground under its footprint (1), or stand
// everything at right angles to the world the way it used to (0).
extern int ghost_align;
// Refuse to place or move a thing into another thing (1), or allow it (0).
extern int clearance;
// What a placed light is: reach in metres, cone in degrees (0 = a point light),
// whether it casts shadows (off means it shines through walls) and its tone
// (0 flame, 1 warm white, 2 cold white).
extern int light_range;
extern int light_cone;
extern int light_shadow;
extern int light_tone;

// Input hooks, from CLevel. Return true when the key was consumed.
bool OnKey(int key);
bool OnWheel(int direction);

// Once per client frame, from CLevel::OnFrame -- traces the aim point.
void Frame();
// ⭐From CLevel::OnRender, BETWEEN Render->Calculate() and Render->Render():
// hands the ghost model to the world pass. See the note at the definition --
// neither of the other two hooks can do this.
void SubmitGhost();
// During UI rendering, from CHUDManager::RenderUI -- outline, arrow and panel.
void Render();
// From CLevel's teardown: the ghost visual dies with the level's model pool.
void Unload();
} // namespace ClPropEdit

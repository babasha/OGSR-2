#include "StdAfx.h"
#include "xr_platform_keys.h" // DIK_* -- the raw key codes, as Level_input.cpp does
#include "cl_prop_edit.h"
#include "cl_zone_debug.h" // DrawLine -- the world-space segment, already clipped

#include "Level.h"
#include "PhysicObject.h"
#include "HangingLamp.h"
#include "HUDManager.h"
#include "ui_defs.h"
#include "xr_level_controller.h"
#include "../xr_3da/GameFont.h"
#include "../xr_3da/XR_IOConsole.h"
#include "../xr_3da/Render.h"
#include "../Include/xrRender/RenderVisual.h"

namespace
{
bool s_active = false;
u16 s_selected = 0xffff;
float s_yaw = 0.f; // degrees, of the ghost

// ⭐⭐WHERE a thing mounts, and the default is to ASK THE SURFACE.
//
// The first cut made hanging a mode the operator had to know about and turn on
// by hand, and the log of the first session with it says exactly how that went:
// ten placements, not one of them hung, not one of them a light. An editor that
// needs a keystroke nobody was told about to do the obvious thing has not
// implemented the feature -- it has hidden it. So the surface under the
// crosshair decides: floor stands, ceiling hangs, wall mounts. The keystroke
// stays, as an override for when the surface guesses wrong.
enum Mount
{
    mAuto = 0,
    mFloor,
    mWall,
    mCeiling
};
Mount s_mount = mAuto; // what the operator forced, if anything
Mount s_mount_now = mFloor; // what it resolved to this frame, for the panel
bool s_pose_ok = false; // the mode can be satisfied where the crosshair is

bool s_as_lamp = false; // build a light, not just a model

Fvector s_aim{};
bool s_aim_valid = false;
u16 s_aim_id = 0xffff; // the prop the crosshair is actually ON, when it is on one
u16 s_blocked_by = 0xffff; // the prop the ghost is currently buried in, if any

// The catalogue, and where we are in it.
xr_vector<shared_str> s_models;
xr_vector<shared_str> s_groups;
int s_model = 0;
shared_str s_named; // set by ed_prop_model / the picker, outranks the cycle

// ⭐The chosen model ITSELF, standing where it would land.
//
// A wire box is enough to answer "how big" and useless for "which way round" --
// a wardrobe reads the same from all four sides as a box, and furniture placed
// by eye is mostly a question of facing. So the model is loaded and handed to
// the renderer each frame at the aim point: the preview IS the thing.
// ⚠It is loaded once per model and kept, where before it was loaded to be
// measured and dropped -- so the cost is unchanged for browsing, and the box
// dimensions come off the same load.
IRenderVisual* s_ghost_visual = nullptr;
Fvector s_ghost_half{0.5f, 0.5f, 0.5f};
Fvector s_ghost_centre{0.f, 0.5f, 0.f};
shared_str s_ghost_of;

constexpr float AIM_RANGE = 120.f;
constexpr float TURN_STEP = 15.f; // degrees per wheel notch
constexpr float LIFT_STEP = 0.05f; // metres per PgUp/PgDn

// ⭐The prop roots. Everything under meshes\ is a model, but most of it is a
// weapon, a face or a corpse -- listing all of it would bury the furniture. A
// name outside these still works: ed_prop_model takes anything and the server
// checks it exists.
const char* const ROOTS[] = {"physics\\", "objects\\dynamics\\", "ogse\\prop\\", "equipments\\"};

// ⚠⚠Every number that goes into a console command goes through here.
//
// printf writes the decimal separator of the C locale, and on a Russian locale
// that is a COMMA. The server would then read "sv_prop_spawn box 90,00 109,03
// ..." and sscanf would stop at the comma -- the command would not be refused,
// it would be half-parsed into a DIFFERENT command with different arguments.
// That exact trap once put an anomaly on the player's head (a PowerShell script,
// same cause), so the separator is forced here rather than trusted.
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
    Msg("~ [ed_prop] %s", buf);
}

// Everything goes to the server as a console line. Nothing here touches the
// world directly -- see the note in the header.
//
// ⚠⚠It is NOT always an rcon line, and assuming it was made the editor mute in
// the one setup an operator actually tests it in. `rcon` is the way to reach
// SOMEBODY ELSE'S server; when this process is the host it has nowhere to send
// the packet and refuses by design ("this is the host; run it directly"). So
// every verb -- place, turn, lift, delete -- printed a polite refusal and moved
// nothing, on a machine where the operator is both the player and the server.
// ⭐The distinction is `Level().Server`, exactly the one CCC_Rcon makes; on the
// host the command is simply run, which is what its own refusal advises.
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

    Msg("~ [ed_prop] > %s%s", hosting ? "" : "rcon ", cmd);
    Console->Execute(line);
}

bool under_a_root(const char* name)
{
    for (const char* r : ROOTS)
        if (0 == strncmp(name, r, xr_strlen(r)))
            return true;
    return false;
}

// The group a model belongs to: two path components, which is what the mesh
// library is actually organised by (physics\box, physics\stol, objects\dynamics).
void group_of(const char* name, string_path& out)
{
    xr_strcpy(out, sizeof(string_path), name);
    char* first = strchr(out, '\\');
    if (!first)
    {
        out[0] = 0;
        return;
    }
    char* second = strchr(first + 1, '\\');
    if (second)
        *second = 0;
    else
        *first = 0; // a model sitting directly in a root folder
}

void build_catalogue()
{
    if (!s_models.empty())
        return;

    // ⭐Read off the filesystem, which sees inside the packed .db archives
    // exactly as the renderer will when it comes to draw one. A list compiled
    // into the engine would describe the install it was written on and nothing
    // else -- the lesson the anomaly toolkit paid for once already.
    FS_FileSet found;
    FS.file_list(found, "$game_meshes$", FS_ListFiles, "*.ogf");

    for (const FS_File& f : found)
    {
        if (!under_a_root(f.name.c_str()))
            continue;
        xr_string name = f.name;
        if (name.size() > 4)
            name.erase(name.size() - 4); // the .ogf
        s_models.push_back(name.c_str());
    }

    std::sort(s_models.begin(), s_models.end(), [](const shared_str& a, const shared_str& b) { return xr_strcmp(a.c_str(), b.c_str()) < 0; });

    for (const shared_str& m : s_models)
    {
        string_path g;
        group_of(m.c_str(), g);
        if (!g[0])
            continue;
        if (s_groups.empty() || 0 != xr_strcmp(s_groups.back().c_str(), g))
            if (std::find_if(s_groups.begin(), s_groups.end(), [&](const shared_str& s) { return 0 == xr_strcmp(s.c_str(), g); }) == s_groups.end())
                s_groups.push_back(g);
    }

    say("%u model(s) in %u group(s); [ and ] cycle them", u32(s_models.size()), u32(s_groups.size()));
}

const char* current_model()
{
    if (s_named.size())
        return s_named.c_str();
    build_catalogue();
    if (s_models.empty())
        return "physics\\box\\box_wood_01";
    if (s_model < 0)
        s_model = int(s_models.size()) - 1;
    if (s_model >= int(s_models.size()))
        s_model = 0;
    return s_models[s_model].c_str();
}

void drop_ghost()
{
    if (s_ghost_visual && ::Render)
        ::Render->model_Delete(s_ghost_visual, FALSE);
    s_ghost_visual = nullptr;
    s_ghost_of = "";
}

// ⭐Load the model to BE the ghost, and take its box off the same load.
//
// Loading is not free, which is why the answer is cached per model rather than
// taken per frame -- and it is not wasted either: placing this model is what
// happens next, and the visual will be resident by then.
void measure_ghost(const char* model)
{
    if (s_ghost_of.size() && 0 == xr_strcmp(s_ghost_of.c_str(), model))
        return;
    drop_ghost();
    s_ghost_of = model;
    s_ghost_half.set(0.5f, 0.5f, 0.5f);
    s_ghost_centre.set(0.f, 0.5f, 0.f);

    if (!::Render)
        return;

    string_path with_ext;
    xr_sprintf(with_ext, "%s.ogf", model);
    if (!FS.exist("$game_meshes$", with_ext))
    {
        // Not an error here: the catalogue is a convenience and a hand-typed
        // name is allowed to be wrong. The server has the last word.
        say("'%s' is not in this install -- the ghost is a guess", model);
        return;
    }

    IRenderVisual* V = ::Render->model_Create(model);
    if (!V)
        return;
    V->getVisData().box.get_CD(s_ghost_centre, s_ghost_half);
    s_ghost_visual = V;
}

// ⭐⭐The surface under the FOOTPRINT, not under the crosshair.
//
// One ray answers "how high, here" and says nothing whatever about slope, so
// everything stood bolt upright: on a hillside that buries one corner and hangs
// the opposite one in the air, and no amount of nudging fixes it because the
// object is the wrong SHAPE for the ground. Four rays at the corners of the
// model's own footprint give a plane instead of a point -- its normal is which
// way up is here, its centroid is where the base goes.
//
// ⚠Probes that land far above or below the centre hit are DISCARDED rather than
// averaged in. Standing a stool on a table means two of the four rays miss the
// table and hit the floor two metres down; averaging those would tip the stool
// into the table, which is worse than not aligning at all. Fewer than three
// survivors means there is no plane worth trusting here, and the answer is the
// flat one -- exactly the old behaviour, kept for the case it was right for.
bool probe_surface(const Fvector& half, float yaw_deg, Fvector& n, Fvector& centre)
{
    constexpr float PROBE_UP = 1.0f; // start the drop rays this far above the hit
    constexpr float PROBE_DOWN = 2.0f; // and let them fall this far past it
    constexpr float PROBE_TOL = 0.45f; // a hit further than this is another surface

    // The footprint, clamped: a wardrobe should not sample the whole room, and a
    // cup should not sample a patch smaller than the terrain's own triangles.
    const float hx = std::clamp(half.x, 0.25f, 1.25f);
    const float hz = std::clamp(half.z, 0.25f, 1.25f);

    const float s = _sin(deg2rad(yaw_deg)), c = _cos(deg2rad(yaw_deg));
    const float cx[4] = {+hx, -hx, -hx, +hx}; // cyclic, so the corners form a quad
    const float cz[4] = {+hz, +hz, -hz, -hz};

    Fvector hit[4];
    int got = 0;
    for (int i = 0; i < 4; ++i)
    {
        Fvector p = s_aim;
        p.x += cx[i] * c + cz[i] * s; // the footprint turns with the model
        p.z += cz[i] * c - cx[i] * s;
        p.y += PROBE_UP;

        collide::rq_result RQ;
        const Fvector down{0.f, -1.f, 0.f};
        if (!Level().ObjectSpace.RayPick(p, down, PROBE_UP + PROBE_DOWN, collide::rqtBoth, RQ, Level().CurrentEntity()))
            continue;

        Fvector h = p;
        h.y -= RQ.range;
        if (_abs(h.y - s_aim.y) > PROBE_TOL)
            continue; // a different surface, not this one

        hit[got++] = h;
    }

    if (got < 3)
        return false;

    // Newell's normal: right for a quad whose corners are not exactly coplanar,
    // which on terrain they never are.
    n.set(0.f, 0.f, 0.f);
    centre.set(0.f, 0.f, 0.f);
    for (int i = 0; i < got; ++i)
    {
        const Fvector& a = hit[i];
        const Fvector& b = hit[(i + 1) % got];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
        centre.add(a);
    }
    centre.div(float(got));

    if (n.square_magnitude() < EPS_S)
        return false;
    n.normalize();
    if (n.y < 0.f)
        n.invert(); // the corners may wind either way; up is up

    return true;
}

// ⭐The whole pose of a box of this size, standing on what the crosshair hit:
// where it rests, and which way "up" is there.
//
// ⚠Built as a BASIS and then converted to the three angles the commands carry,
// rather than the other way round: the matrix is what can be drawn and the
// angles are what can be sent, and going basis → angles → basis once here means
// the preview is drawn from exactly the numbers that will be transmitted. Any
// other order leaves room for the ghost and the result to disagree.
// ⚠Takes the box rather than reading the ghost's, because `G` poses the object
// already in the world, and that is a different box with a different heading.
void surface_pose(const Fvector& centre_of_model, const Fvector& half, float yaw_deg, Fmatrix& m, Fvector& angles_deg)
{
    Fvector up{0.f, 1.f, 0.f};
    Fvector base = s_aim;

    if (ClPropEdit::ghost_align)
    {
        Fvector n, centre;
        if (probe_surface(half, yaw_deg, n, centre))
        {
            // ⚠A wardrobe does not lie down on a cliff. Past this much slope the
            // align stops being a service and becomes a thing to undo by hand,
            // so steep ground gets the upright answer and keeps its height.
            constexpr float MAX_TILT_COS = 0.76f; // ~40 degrees
            base = centre;
            if (n.y >= MAX_TILT_COS)
                up = n;
        }
    }

    // The heading, projected onto the plane: yaw stays the operator's, the
    // surface only decides what it turns AROUND.
    Fvector k;
    k.set(_sin(deg2rad(yaw_deg)), 0.f, _cos(deg2rad(yaw_deg)));
    Fvector along;
    along.mul(up, k.dotproduct(up));
    k.sub(along);
    if (k.square_magnitude() < EPS_S)
        k.set(1.f, 0.f, 0.f); // aiming straight down a vertical face; any heading
    k.normalize();

    Fvector i;
    i.crossproduct(up, k); // left-handed: i = j x k

    m.identity();
    m.i = i;
    m.j = up;
    m.k = k;

    Fvector rad;
    m.getXYZ(rad);
    angles_deg.set(rad2deg(rad.x), rad2deg(rad.y), rad2deg(rad.z));

    // Rebuilt from the angles that will be sent, then dropped onto the surface
    // by the model's own bottom -- measured along the object's up, because on a
    // slope "down" for the model is not down for the world.
    m.setXYZ(deg2rad(angles_deg.x), deg2rad(angles_deg.y), deg2rad(angles_deg.z));
    Fvector foot;
    foot.mul(m.j, centre_of_model.y - half.y);
    base.sub(foot);
    m.translate_over(base);
}

// ⭐The ceiling over the aim point, and the LOWEST thing that counts as one.
//
// Rays up from where the crosshair landed, at the same footprint corners the
// ground probe uses: a chandelier hung from the highest hit would go through the
// beam next to it, and through the pipe that runs under the slab.
// ⚠⚠`start_y` is not a detail. The rays used to start a few centimetres ABOVE
// the aim point, which is right when the operator is pointing at the floor and
// catastrophically wrong when they are pointing at the CEILING -- which is what
// anyone hanging a lamp actually does. Five centimetres above a ceiling is
// inside the slab, so the search found the floor above, and the lamp was hung
// there: its model vanished behind the concrete and its light leaked back down.
// "It did not hang but the light stayed" is exactly what that looks like.
bool probe_ceiling(const Fvector& half, float yaw_deg, float start_y, float& ceiling_y)
{
    constexpr float REACH = 8.f; // no room in this content is taller than this

    const float hx = std::clamp(half.x, 0.15f, 1.25f);
    const float hz = std::clamp(half.z, 0.15f, 1.25f);
    const float s = _sin(deg2rad(yaw_deg)), c = _cos(deg2rad(yaw_deg));
    const float cx[5] = {0.f, +hx, -hx, -hx, +hx};
    const float cz[5] = {0.f, +hz, +hz, -hz, -hz};

    bool found = false;
    for (int i = 0; i < 5; ++i)
    {
        Fvector p = s_aim;
        p.x += cx[i] * c + cz[i] * s;
        p.z += cz[i] * c - cx[i] * s;
        p.y = start_y;

        collide::rq_result RQ;
        const Fvector up{0.f, 1.f, 0.f};
        if (!Level().ObjectSpace.RayPick(p, up, REACH, collide::rqtBoth, RQ, Level().CurrentEntity()))
            continue;

        const float y = p.y + RQ.range;
        if (!found || y < ceiling_y)
            ceiling_y = y;
        found = true;
    }
    return found;
}

// ⭐The plane the crosshair is standing on.
//
// Three rays from the camera through slightly separated directions; the normal
// of the triangle their hits form is the surface's. ⚠Done this way rather than
// by reading the triangle out of the collision model because the ray can land on
// a dynamic object just as easily as on the level, and a crate's top is a
// surface too. The spread grows with distance so the samples never collapse onto
// one another, and if the three hits are not close together the fan has crossed
// an edge -- better to answer "unknown" than to average across a corner.
bool aim_plane(Fvector& n)
{
    if (!s_aim_valid)
        return false;

    const float range = _max(1.f, Device.vCameraPosition.distance_to(s_aim));
    const float spread = 0.015f; // radians; at 10 m that is 15 cm apart

    Fvector p[3];
    for (int i = 0; i < 3; ++i)
    {
        Fvector d = Device.vCameraDirection;
        if (i == 1)
            d.mad(Device.vCameraRight, spread);
        else if (i == 2)
            d.mad(Device.vCameraTop, spread);
        d.normalize();

        collide::rq_result RQ;
        if (!Level().ObjectSpace.RayPick(Device.vCameraPosition, d, range * 1.5f + 1.f, collide::rqtBoth, RQ, Level().CurrentEntity()))
            return false;

        p[i].mad(Device.vCameraPosition, d, RQ.range);
        if (p[i].distance_to(s_aim) > _max(0.35f, range * spread * 6.f))
            return false; // the fan straddles an edge; no single plane here
    }

    Fvector a, b;
    a.sub(p[1], p[0]);
    b.sub(p[2], p[0]);
    n.crossproduct(a, b);
    if (n.square_magnitude() < EPS_S)
        return false;
    n.normalize();

    // Toward the camera: a surface is only ever seen from its front.
    Fvector back;
    back.sub(Device.vCameraPosition, s_aim);
    if (n.dotproduct(back) < 0.f)
        n.invert();
    return true;
}

// Which mounting the surface asks for, unless the operator has overridden it.
Mount resolve_mount(Fvector& n)
{
    n.set(0.f, 1.f, 0.f);
    const bool have = aim_plane(n);

    if (s_mount != mAuto)
        return s_mount;
    if (!have)
        return mFloor; // no answer is not a reason to do something exotic

    if (n.y > 0.5f)
        return mFloor;
    if (n.y < -0.5f)
        return mCeiling;
    return mWall;
}

// The pose for a box of this size in the resolved mode. False means it cannot be
// satisfied here -- asked to hang something under open sky.
bool pose_for(const Fvector& centre, const Fvector& half, float yaw_deg, Fmatrix& m, Fvector& angles_deg)
{
    Fvector n;
    const Mount mount = resolve_mount(n);
    s_mount_now = mount;

    if (mount == mFloor)
    {
        surface_pose(centre, half, yaw_deg, m, angles_deg);
        return true;
    }

    if (mount == mCeiling)
    {
        // ⭐Start the search just BELOW the aimed surface when that surface is
        // itself a ceiling -- the operator is pointing AT the thing to hang
        // from, and the same rays then also catch a beam or a pipe that hangs
        // lower inside the footprint. Only when the ceiling mount was forced
        // from somewhere else (aiming at the floor, H pressed) is there a real
        // search to do, and then it starts just above the floor.
        const bool aiming_up = n.y < -0.5f;
        const float start_y = aiming_up ? s_aim.y - 0.05f : s_aim.y + 0.05f;

        float ceiling_y = 0.f;
        if (!probe_ceiling(half, yaw_deg, start_y, ceiling_y))
            return false;

        // ⚠Hung things stay VERTICAL, and that is not an omission of the tilt
        // code. A chandelier under a sloping roof hangs straight down -- it is
        // held by a chain and gravity, not glued to the slab. Matching the
        // ceiling's angle here is the one case where following the surface is
        // plainly wrong.
        angles_deg.set(0.f, yaw_deg, 0.f);
        m.setXYZ(0.f, deg2rad(yaw_deg), 0.f);

        Fvector p = s_aim; // over the point the operator is pointing at
        p.y = ceiling_y - (centre.y + half.y); // and its TOP against the ceiling
        m.translate_over(p);
        return true;
    }

    // ⭐On a wall: upright, back against it, facing into the room.
    //
    // ⚠The heading comes from the WALL and not from the wheel. A bracket lamp
    // turned by hand is a bracket lamp pointing into brickwork; the one degree
    // of freedom that matters here is already spoken for.
    Fvector flat{n.x, 0.f, n.z};
    if (flat.square_magnitude() < EPS_S)
        return false;
    flat.normalize();

    const float yaw = rad2deg(atan2f(flat.x, flat.z));
    angles_deg.set(0.f, yaw, 0.f);
    m.setXYZ(0.f, deg2rad(yaw), 0.f);

    // The model's own back plane onto the wall. Which face of a mesh is its back
    // is not written down anywhere, so this is the box's answer -- and the ghost
    // is on screen to be disagreed with.
    Fvector p;
    p.mad(s_aim, m.k, -(centre.z - half.z));
    m.translate_over(p);
    return true;
}

// The ghost's pose: the model in hand, where the current mode puts it. Most of
// this library is modelled base-at-origin and for those the origin lands on the
// surface unchanged; the ones that are not used to sink by half their height,
// and nobody could tell, because a crate half in the floor still looks like a
// crate on the floor. Standing things on each other makes it obvious.
bool ghost_pose(Fmatrix& m, Fvector& angles_deg) { return pose_for(s_ghost_centre, s_ghost_half, s_yaw, m, angles_deg); }

// Where the ghost stands. The SAME matrix feeds the model and its outline, so
// the two can never disagree about where the thing is.
bool ghost_xform(Fmatrix& m)
{
    Fvector a;
    return ghost_pose(m, a);
}

bool is_prop(CObject* O) { return O && !O->getDestroy() && (smart_cast<CPhysicObject*>(O) || smart_cast<CHangingLamp*>(O)); }

// Where the operator is looking, on the world. One ray per frame.
//
// ⭐⭐`rqtBoth`, not `rqtStatic`, and that single flag is two features. With only
// the level's own geometry in the query the ray went THROUGH everything the
// editor had placed and landed on the wall behind: nothing could be stood on
// anything (the crate was not a surface), and RMB had no hit to name, so it had
// to guess by proximity. Asking for objects too makes a placed crate a floor
// like any other, and makes "what am I pointing at" an answer rather than a
// guess.
// ⚠The actor is excluded, or the ray would stop on the operator's own body.
void trace_aim()
{
    s_aim_valid = false;
    s_aim_id = 0xffff;
    if (!g_pGameLevel || !Level().game_configured)
        return;

    collide::rq_result RQ;
    const Fvector from = Device.vCameraPosition;
    const Fvector dir = Device.vCameraDirection;
    if (!Level().ObjectSpace.RayPick(from, dir, AIM_RANGE, collide::rqtBoth, RQ, Level().CurrentEntity()))
        return;

    s_aim.mad(from, dir, RQ.range);
    s_aim_valid = true;

    if (is_prop(RQ.O))
        s_aim_id = u16(RQ.O->ID());
}

// What RMB grabs. ⭐The one the ray HIT, when it hit one -- that is not a
// heuristic and cannot be argued with. Only when the ray landed on the world
// does the old rule apply: the nearest prop to that point, for the cases a ray
// is bad at (a lamp on a ceiling, a thin rail, a stool half under a table).
// ⚠That fallback radius used to be 4 m and was the ONLY rule; with an exact hit
// available it can be tight, and it should be -- at 4 m, pointing at the floor
// beside a crate handed you the crate.
u16 pick_prop()
{
    if (s_aim_id != 0xffff)
        return s_aim_id;

    if (!s_aim_valid)
        return 0xffff;

    u16 best = 0xffff;
    float best_d = 2.f; // beyond this, they were not pointing at anything
    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!is_prop(O))
            continue;

        const float d = O->Position().distance_to(s_aim);
        if (d < best_d)
        {
            best_d = d;
            best = u16(O->ID());
        }
    }
    return best;
}

CObject* find_prop(u16 id)
{
    if (id == 0xffff)
        return nullptr;
    CObject* O = Level().Objects.net_Find(id);
    return is_prop(O) ? O : nullptr;
}

// ⭐Do these two oriented boxes actually overlap? Separating-axis, all fifteen.
//
// ⚠The obvious cheap version -- world AABBs of both -- is worthless HERE. A
// two-metre wardrobe turned 45 degrees has an AABB half again its size, so
// standing one beside another would report a collision across a gap you can
// walk through, and the operator would learn to ignore the warning. A check
// people learn to ignore is worse than no check.
bool boxes_overlap(const Fmatrix& ma, const Fvector& ca, const Fvector& ea, const Fmatrix& mb, const Fvector& cb, const Fvector& eb)
{
    // Centres in the world, and the two sets of axes.
    Fvector wa, wb;
    ma.transform_tiny(wa, ca);
    mb.transform_tiny(wb, cb);

    const Fvector* A[3] = {&ma.i, &ma.j, &ma.k};
    const Fvector* B[3] = {&mb.i, &mb.j, &mb.k};
    const float EA[3] = {ea.x, ea.y, ea.z};
    const float EB[3] = {eb.x, eb.y, eb.z};

    float R[3][3], AbsR[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            R[i][j] = A[i]->dotproduct(*B[j]);
            // ⚠The epsilon is not decoration: two parallel boxes (which is what
            // furniture in a room mostly is) make the cross-product axes very
            // nearly zero, and without it they turn into false separations.
            AbsR[i][j] = _abs(R[i][j]) + 1e-4f;
        }

    Fvector d;
    d.sub(wb, wa);
    const float t[3] = {d.dotproduct(*A[0]), d.dotproduct(*A[1]), d.dotproduct(*A[2])};

    for (int i = 0; i < 3; ++i)
        if (_abs(t[i]) > EA[i] + EB[0] * AbsR[i][0] + EB[1] * AbsR[i][1] + EB[2] * AbsR[i][2])
            return false;

    for (int j = 0; j < 3; ++j)
        if (_abs(t[0] * R[0][j] + t[1] * R[1][j] + t[2] * R[2][j]) > EB[j] + EA[0] * AbsR[0][j] + EA[1] * AbsR[1][j] + EA[2] * AbsR[2][j])
            return false;

    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            const int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
            const int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            const float ra = EA[i1] * AbsR[i2][j] + EA[i2] * AbsR[i1][j];
            const float rb = EB[j1] * AbsR[i][j2] + EB[j2] * AbsR[i][j1];
            if (_abs(t[i2] * R[i1][j] - t[i1] * R[i2][j]) > ra + rb)
                return false;
        }

    return true;
}

// The prop this pose would be INSIDE of, or 0xffff.
//
// ⚠Both boxes are shrunk before the test, so touching is not overlapping.
// Everything the editor places rests ON something, and half of it rests on
// something else it placed -- an exact-contact test would call every stack a
// collision.
u16 blocking_prop(const Fmatrix& pose, const Fvector& centre, const Fvector& half, u16 ignore)
{
    constexpr float SLACK = 0.05f; // this much interpenetration is contact, not burial

    Fvector e = half;
    e.sub(SLACK);
    if (e.x <= 0.f || e.y <= 0.f || e.z <= 0.f)
        return 0xffff; // smaller than the slack: nothing to be inside of

    for (u32 i = 0; i < Level().Objects.o_count(); ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!is_prop(O) || u16(O->ID()) == ignore || !O->Visual())
            continue;

        // Cheap reject first: the editor runs in a room, not in a vacuum, and
        // most of the level is nowhere near the crosshair.
        if (O->Position().distance_to(pose.c) > 12.f)
            continue;

        Fvector c, h;
        O->Visual()->getVisData().box.get_CD(c, h);
        h.sub(SLACK);
        if (h.x <= 0.f || h.y <= 0.f || h.z <= 0.f)
            continue;

        if (boxes_overlap(pose, centre, e, O->XFORM(), c, h))
            return u16(O->ID());
    }
    return 0xffff;
}

// An oriented box, drawn as twelve segments.
void draw_box(const Fmatrix& xform, const Fvector& centre, const Fvector& half, u32 colour)
{
    Fvector c[8];
    for (int i = 0; i < 8; ++i)
    {
        Fvector local;
        local.set(centre.x + ((i & 1) ? half.x : -half.x), centre.y + ((i & 2) ? half.y : -half.y), centre.z + ((i & 4) ? half.z : -half.z));
        xform.transform_tiny(c[i], local);
    }
    static const int EDGES[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : EDGES)
        ClZoneDebug::DrawLine(c[e[0]], c[e[1]], colour);
}

// Which way it faces, because a wardrobe that is the right size and the wrong
// way round is still wrong -- and a box on its own cannot say.
void draw_facing(const Fmatrix& xform, const Fvector& centre, const Fvector& half, u32 colour)
{
    const float reach = _max(0.6f, half.z + 0.5f);
    Fvector base, tip, l, r;
    xform.transform_tiny(base, Fvector().set(centre.x, centre.y, centre.z));
    xform.transform_tiny(tip, Fvector().set(centre.x, centre.y, centre.z + reach));
    xform.transform_tiny(l, Fvector().set(centre.x - 0.15f, centre.y, centre.z + reach - 0.2f));
    xform.transform_tiny(r, Fvector().set(centre.x + 0.15f, centre.y, centre.z + reach - 0.2f));
    ClZoneDebug::DrawLine(base, tip, colour);
    ClZoneDebug::DrawLine(tip, l, colour);
    ClZoneDebug::DrawLine(tip, r, colour);
}
} // namespace

// ed_prop_ghost: draw the model itself under the crosshair, or only its outline.
// A knob rather than a decision, because a solid preview is the right answer
// while orienting one piece and the wrong one while scanning a room full of
// them. ⚠Not saved -- an operator tool has no business in the player's user.ltx.
namespace ClPropEdit
{
int ghost_solid = 1;

// ed_prop_align: stand things ON the ground rather than at right angles to the
// world. Off is the old behaviour, and it is the right answer when the operator
// wants a row of crates that agree with each other rather than with the hill.
int ghost_align = 1;

// ed_prop_clearance: refuse to put a thing inside another thing. ⚠A knob and not
// a rule, because burying one prop in another is sometimes exactly the intent --
// a crate half through a wall of shelves reads as "on the shelf", and a lamp
// inside its own shade is how that gets built.
int clearance = 1;

// ed_prop_light_*: what a placed light IS. None of it can be read off a model,
// so it is the operator's to say -- and all four defaults are the common case,
// which is a shadow-casting warm bulb lighting one room.
int light_range = 6; // metres
int light_cone = 0; // degrees; 0 is a point light, more is a spot
int light_shadow = 1; // ⚠off = light through walls; see SVProp::Light
int light_tone = 1; // 0 flame, 1 warm white, 2 cold white
}

bool ClPropEdit::Active() { return s_active; }

const char* ClPropEdit::CurrentModel() { return current_model(); }

void ClPropEdit::EnumerateGroups(xr_vector<shared_str>& out)
{
    build_catalogue();
    out = s_groups;
}

void ClPropEdit::EnumerateModels(const char* group, xr_vector<shared_str>& out)
{
    build_catalogue();
    out.clear();
    const bool all = !group || !group[0];
    const size_t n = all ? 0 : xr_strlen(group);
    for (const shared_str& m : s_models)
        if (all || 0 == strncmp(m.c_str(), group, n))
            out.push_back(m);
}

void ClPropEdit::Toggle()
{
    s_active = !s_active;
    if (s_active)
    {
        build_catalogue();
        measure_ghost(current_model());
        say("ON -- LMB=place  RMB=pick  wheel=turn  G=move  PGUP/PGDN=lift  DEL=remove  ESC=off");
        say("     the surface decides: floors stand, walls mount, ceilings hang. H forces one, L=light/prop, F=campfire");
        say("placing '%s' facing %.0f", current_model(), s_yaw);
    }
    else
    {
        s_selected = 0xffff;
        say("OFF");
    }
}

// ⭐Picking a light sets the editor to build lights.
//
// ⚠A default, not a rule, and it is derived from the one thing that just
// changed -- the model. This content files its lights under physics\light, and
// an operator who picks lampa_abajur_03 out of that folder means a lamp; the
// first session with the manual switch produced ten lamp-shaped objects that
// emitted nothing, which is the honest measure of how discoverable a hidden
// toggle is. `L` still overrides it, and the panel always says which way it is
// set, so the guess is visible and one keystroke from being undone.
static void suggest_kind(const char* model)
{
    if (!model)
        return;

    // ⚠The lights folder is not only lights. It holds the switches, sockets and
    // conduit that go WITH lighting, and one of those hung on a wall as a lamp
    // lit an entire floor. The folder is still the signal -- these few are the
    // exceptions to it, and being wrong here now costs a keystroke rather than a
    // floodlight, since a placed light's reach is a number the operator sets.
    static const char* const NOT_A_LIGHT[] = {"switch", "vikluchatel", "viklu", "rozetka", "socket", "provoda", "cable", "wire_"};

    bool light = (0 == strncmp(model, "physics\\light\\", 14));
    if (light)
        for (const char* no : NOT_A_LIGHT)
            if (strstr(model, no))
            {
                light = false;
                break;
            }
    if (light == s_as_lamp)
        return;
    s_as_lamp = light;
    say(light ? "...and switching to LIGHTS, since that is a lamp (L switches back)" : "...and back to plain props (L switches)");
}

void ClPropEdit::SetModel(const char* model)
{
    if (!model || !model[0])
    {
        say("current model is '%s'", current_model());
        return;
    }

    // ⭐A named model outranks the catalogue cycle rather than being appended to
    // it: the operator asked for THIS one, and [ ] should still walk the library
    // from where it was when they come back to browsing.
    s_named = model;
    measure_ghost(current_model());
    say("model '%s'", current_model());
    suggest_kind(current_model());
}

bool ClPropEdit::OnWheel(int direction)
{
    if (!s_active)
        return false;

    const float step = (direction > 0) ? TURN_STEP : -TURN_STEP;

    if (CObject* O = find_prop(s_selected))
    {
        // The selection's CURRENT heading plus a step, so turning is relative to
        // what is on screen rather than to whatever the ghost happened to be set
        // to. XFORM's k vector is where the thing faces.
        const Fvector& k = O->XFORM().k;
        const float now = rad2deg(atan2f(k.x, k.z));
        string32 ys;
        fixed(now + step, ys);
        send("sv_prop_turn %u %s", u32(s_selected), ys);
        return true;
    }

    s_yaw += step;
    while (s_yaw >= 360.f)
        s_yaw -= 360.f;
    while (s_yaw < 0.f)
        s_yaw += 360.f;
    return true;
}

bool ClPropEdit::OnKey(int key)
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
        if (s_blocked_by != 0xffff)
        {
            CObject* B = find_prop(s_blocked_by);
            say("that would be inside id=%u '%s' -- aim elsewhere, or 'ed_prop_clearance 0' if you mean it", u32(s_blocked_by), B ? B->cNameSect().c_str() : "?");
            return true;
        }

        // ⭐The ghost's pose, whole: position, heading AND tilt. What was on
        // screen is what gets sent -- the yaw included, because once a thing is
        // tilted its heading is no longer the number the wheel holds.
        Fmatrix m;
        Fvector a;
        if (!ghost_pose(m, a))
        {
            say("nothing overhead to hang it from -- H puts it back on the floor");
            return true;
        }
        string32 ys, xs, yys, zs, ps, bs;
        fixed(a.y, ys);
        fixed(m.c.x, xs);
        fixed(m.c.y, yys);
        fixed(m.c.z, zs);
        fixed(a.x, ps);
        fixed(a.z, bs);
        // ⭐Which verb, from the mode -- the one difference between a chandelier
        // that lights the room and a chandelier-shaped crate.
        if (s_as_lamp)
        {
            // ⚠The reach travels with the placement rather than being left to
            // the server's idea of a lamp: what looks right is a property of the
            // room, and the person choosing it is standing in one.
            string32 rs, cs;
            fixed(float(ClPropEdit::light_range), rs);
            fixed(float(ClPropEdit::light_cone), cs);
            send("sv_prop_lamp %s %s %s %s %s %s %s %s %s %d %d", current_model(), ys, xs, yys, zs, ps, bs, rs, cs, ClPropEdit::light_shadow ? 1 : 0,
                 ClPropEdit::light_tone);
        }
        else
            send("sv_prop_spawn %s %s %s %s %s %s %s", current_model(), ys, xs, yys, zs, ps, bs);
        return true;
    }

    case DIK_H: {
        // Auto first, then the three forced ones -- an override is for when the
        // surface guessed wrong, and it should be easy to hand the decision back.
        s_mount = Mount((int(s_mount) + 1) % 4);
        static const char* const NAME[4] = {"following the surface (floor stands, wall mounts, ceiling hangs)", "always on the floor", "always on a wall",
                                            "always under the ceiling"};
        say("mounting: %s", NAME[int(s_mount)]);
        return true;
    }

    case DIK_L:
        s_as_lamp = !s_as_lamp;
        say(s_as_lamp ? "placing LIGHTS (the model plus a lamp that lights the room)" : "placing props (the model alone)");
        return true;

    case MOUSE_2: {
        const u16 hit = pick_prop();
        if (hit == s_selected || hit == 0xffff)
        {
            s_selected = 0xffff;
            say("nothing selected");
        }
        else
        {
            s_selected = hit;
            CObject* O = find_prop(hit);
            say("selected id=%u '%s'", u32(hit), O ? O->cNameSect().c_str() : "?");
        }
        return true;
    }

    case DIK_G: {
        if (s_selected == 0xffff)
        {
            say("nothing selected to move -- RMB picks");
            return true;
        }
        if (!s_aim_valid)
        {
            say("nowhere to move it to");
            return true;
        }
        CObject* O = find_prop(s_selected);
        if (!O || !O->Visual())
        {
            say("the selection is gone");
            s_selected = 0xffff;
            return true;
        }

        // ⭐Moved with the same rules placing uses -- rested on the surface and
        // laid to its slope, using the object's OWN box and its OWN heading. A
        // move that kept the old tilt would carry the last hill's angle onto
        // this one, which looks like a bug wherever it lands.
        Fvector c, h;
        O->Visual()->getVisData().box.get_CD(c, h);
        const Fvector& facing = O->XFORM().k;
        const float yaw_now = rad2deg(atan2f(facing.x, facing.z));

        Fmatrix m;
        Fvector a;
        if (!pose_for(c, h, yaw_now, m, a))
        {
            say("nothing overhead to hang it from");
            return true;
        }

        if (ClPropEdit::clearance)
        {
            const u16 b = blocking_prop(m, c, h, s_selected);
            if (b != 0xffff)
            {
                CObject* B = find_prop(b);
                say("that would put it inside id=%u '%s'", u32(b), B ? B->cNameSect().c_str() : "?");
                return true;
            }
        }

        string32 xs, ys, zs, yaws, ps, bs;
        fixed(m.c.x, xs);
        fixed(m.c.y, ys);
        fixed(m.c.z, zs);
        fixed(a.y, yaws);
        fixed(a.x, ps);
        fixed(a.z, bs);
        send("sv_prop_move %u %s %s %s %s %s %s", u32(s_selected), xs, ys, zs, yaws, ps, bs);
        return true;
    }

    case DIK_PRIOR:
    case DIK_NEXT: {
        if (s_selected == 0xffff)
        {
            say("nothing selected to lift -- RMB picks");
            return true;
        }
        string32 ds;
        fixed((key == DIK_PRIOR) ? LIFT_STEP : -LIFT_STEP, ds);
        send("sv_prop_lift %u %s", u32(s_selected), ds);
        return true;
    }

    case DIK_DELETE: {
        if (s_selected == 0xffff)
        {
            say("nothing selected to delete");
            return true;
        }
        send("sv_prop_del %u", u32(s_selected));
        s_selected = 0xffff;
        return true;
    }

    case DIK_F: {
        if (!s_aim_valid)
        {
            say("nothing under the crosshair to build a fire on");
            return true;
        }
        // ⚠The surface, but NOT the current model's footprint offset: a campfire
        // is the server's own recipe of two entities and neither of them is the
        // model in hand. So the fire sits on the measured ground, at the tilt
        // that ground has, and nothing is lifted by somebody else's bounding box.
        Fvector n{0.f, 1.f, 0.f}, at = s_aim;
        Fvector ang;
        ang.set(0.f, s_yaw, 0.f); // degrees, upright unless the ground says otherwise
        if (ClPropEdit::ghost_align && probe_surface(s_ghost_half, s_yaw, n, at))
        {
            Fvector k;
            k.set(_sin(deg2rad(s_yaw)), 0.f, _cos(deg2rad(s_yaw)));
            Fvector along;
            along.mul(n, k.dotproduct(n));
            k.sub(along);
            if (k.square_magnitude() > EPS_S)
            {
                k.normalize();
                Fmatrix m;
                m.identity();
                m.j = n;
                m.k = k;
                m.i.crossproduct(n, k);
                Fvector rad;
                m.getXYZ(rad);
                ang.set(rad2deg(rad.x), rad2deg(rad.y), rad2deg(rad.z));
            }
        }
        string32 ys, xs, yys, zs, ps, bs;
        fixed(ang.y, ys);
        fixed(at.x, xs);
        fixed(at.y, yys);
        fixed(at.z, zs);
        fixed(ang.x, ps);
        fixed(ang.z, bs);
        send("sv_prop_campfire %s %s %s %s %s %s", ys, xs, yys, zs, ps, bs);
        return true;
    }

    case DIK_LBRACKET:
        s_named = "";
        --s_model;
        measure_ghost(current_model());
        say("model '%s'", current_model());
        suggest_kind(current_model());
        return true;

    case DIK_RBRACKET:
        s_named = "";
        ++s_model;
        measure_ghost(current_model());
        say("model '%s'", current_model());
        suggest_kind(current_model());
        return true;
    }

    return false;
}

void ClPropEdit::Frame()
{
    if (!s_active)
        return;

    // The ghost dies with the level (Unload). If the editor was left on across a
    // transition, rebuild it here rather than leaving the operator aiming a
    // default-sized box on the far side.
    if (!s_ghost_of.size())
        measure_ghost(current_model());

    trace_aim();

    // ⭐Once per frame, and the answer is what BOTH the outline and the click
    // use. Computing it separately in each would let the ghost say one thing and
    // the button do another, which is the one failure a guard must not have.
    s_blocked_by = 0xffff;
    s_pose_ok = false;
    if (s_aim_valid)
    {
        Fmatrix m;
        Fvector a;
        s_pose_ok = ghost_pose(m, a);
        if (s_pose_ok && ClPropEdit::clearance)
            s_blocked_by = blocking_prop(m, s_ghost_centre, s_ghost_half, 0xffff);
    }
}

// ⭐From CLevel::OnRender, BETWEEN Render->Calculate() and Render->Render().
//
// That window is not arbitrary: Calculate() clears the per-frame list of dynamic
// visuals and then fills it by walking the visible objects, and Render() drains
// it. A ghost belongs to no object and is in no spatial tree, so it has to be
// handed over after the walk and before the drain -- submitted from the editor's
// own Render() (which runs with the UI, long after the world) it would be
// silently dropped, and from Frame() it would be wiped by the clear.
// ⚠A null root means no owner to read sky-visibility off, so the renderer lights
// the ghost at full hemi. Correct for what this is: a preview should be legible
// in a cellar, not lit like the furniture it is not yet.
void ClPropEdit::SubmitGhost()
{
    if (!s_active || !s_aim_valid || !s_ghost_visual || !ghost_solid || !::Render)
        return;

    Fmatrix m;
    ghost_xform(m);
    ::Render->add_Visual(0u, nullptr, s_ghost_visual, m);
}

// ⚠Before the level goes. The visual belongs to the renderer's model pool, which
// the level unload empties -- keeping the pointer across it would hand the next
// frame a model that no longer exists.
void ClPropEdit::Unload()
{
    drop_ghost();
    s_selected = 0xffff;
    s_aim_valid = false;
}

void ClPropEdit::Render()
{
    if (!s_active)
        return;

    // The outline around the ghost: the model is drawn by SubmitGhost, in the
    // world pass where models belong, and this is what says it is not placed yet
    // -- plus the arrow, because a solid preview still cannot say which end the
    // wheel is about to swing.
    // ⚠The box is the model's REAL box through the ghost's transform, the same
    // pair the selection outline uses. It used to be pushed up by half its
    // height on the assumption that a model's origin is its base; that guess is
    // unnecessary now that the model itself is on screen to be compared against
    // -- and where it was wrong, the box and the preview would have disagreed.
    Fmatrix ghost;
    if (s_aim_valid && ghost_xform(ghost))
    {
        // Red when the click would be refused. The refusal has to be visible
        // BEFORE the click, or the guard just reads as the editor ignoring you.
        const bool blocked = s_blocked_by != 0xffff;
        draw_box(ghost, s_ghost_centre, s_ghost_half, blocked ? color_rgba(255, 70, 60, 230) : color_rgba(80, 255, 120, 220));
        draw_facing(ghost, s_ghost_centre, s_ghost_half, blocked ? color_rgba(255, 70, 60, 200) : color_rgba(255, 220, 90, 220));

        // ⭐Hanging, the thing is metres from the point being aimed at, so the
        // drop is drawn: without it the operator is looking at a chandelier in
        // mid-air with no way to tell WHICH spot on the floor put it there.
        if (s_mount_now != mFloor)
            ClZoneDebug::DrawLine(s_aim, ghost.c, color_rgba(120, 200, 255, 160));
    }

    // ⭐The selection, and it PULSES. A static outline reads as part of the
    // scene -- a rusty crate in a rusty room wearing a thin orange line is
    // something an operator loses track of the moment they turn their head. A
    // brightness that moves is the one thing nothing else on screen does.
    // ⚠The box is grown by a few centimetres so its edges do not lie exactly on
    // the model's own silhouette, where they z-fight into a dotted line and read
    // as "half the object is selected".
    if (CObject* O = find_prop(s_selected))
    {
        if (O->Visual())
        {
            Fvector c, h;
            O->Visual()->getVisData().box.get_CD(c, h);
            h.add(0.04f);

            const float pulse = 0.5f + 0.5f * sinf(Device.fTimeGlobal * 6.f);
            const u32 a = u32(140.f + 115.f * pulse);
            draw_box(O->XFORM(), c, h, color_rgba(255, 150, 40, a));
            draw_facing(O->XFORM(), c, h, color_rgba(255, 150, 40, a));
        }
    }
    // ...and what RMB would grab, dimly, when nothing is held yet. Picking is
    // nearest-to-the-aim rather than whatever the ray struck (see pick_prop), so
    // without this the operator has to press the button to find out what the
    // rule chose. ⚠Only while nothing is selected: two outlines in two colours
    // on two objects is a question, not an answer.
    else if (s_aim_valid)
    {
        if (CObject* C = find_prop(pick_prop()))
            if (C->Visual())
            {
                Fvector c, h;
                C->Visual()->getVisData().box.get_CD(c, h);
                draw_box(C->XFORM(), c, h, color_rgba(150, 150, 160, 130));
            }
    }

    CGameFont* F = HUD().Font().pFontSmall;
    if (!F)
        return;

    F->SetAligment(CGameFont::alLeft);
    F->SetColor(color_rgba(120, 255, 150, 255));
    static const char* const WHERE[4] = {"floor", "floor", "wall", "ceiling"};
    F->Out(20.f, 200.f, "PROP EDITOR   [%s on the %s%s]", s_as_lamp ? "LIGHT" : "prop", WHERE[int(s_mount_now)], (s_mount == mAuto) ? "" : ", forced");
    F->SetColor(color_rgba(255, 255, 255, 255));
    F->Out(20.f, 216.f, "model   : %s", current_model());

    if (CObject* O = find_prop(s_selected))
        F->Out(20.f, 232.f, "selected: id=%u  %s   (wheel turns, G moves, PGUP/PGDN lifts, DEL removes)", u32(s_selected), O->cNameSect().c_str());
    else if (s_blocked_by != 0xffff)
    {
        CObject* B = find_prop(s_blocked_by);
        F->SetColor(color_rgba(255, 90, 80, 255));
        F->Out(20.f, 232.f, "blocked : would be inside id=%u %s", u32(s_blocked_by), B ? B->cNameSect().c_str() : "?");
        F->SetColor(color_rgba(255, 255, 255, 255));
    }
    else
        F->Out(20.f, 232.f, "facing  : %.0f deg   (wheel turns it, LMB places)", s_yaw);

    // What the crosshair is on, by name -- "on the world" and "on a prop" are
    // different offers (place here / RMB takes this one), and the operator
    // should not have to press a button to find out which one is on the table.
    if (s_aim_valid && !s_pose_ok)
    {
        F->SetColor(color_rgba(255, 90, 80, 255));
        F->Out(20.f, 248.f, "aim     : nothing here to %s from", (s_mount_now == mCeiling) ? "hang" : "mount");
        F->SetColor(color_rgba(255, 255, 255, 255));
    }
    else if (!s_aim_valid)
        F->Out(20.f, 248.f, "aim     : nothing in range");
    else if (CObject* C = find_prop(s_aim_id))
        F->Out(20.f, 248.f, "aim     : on id=%u  %s   (RMB takes it, LMB stands the model on it)", u32(s_aim_id), C->cNameSect().c_str());
    else
        F->Out(20.f, 248.f, "aim     : on the world");
    F->SetColor(color_rgba(190, 190, 190, 220));
    F->Out(20.f, 268.f, "LMB place  RMB pick  wheel turn  G move  PGUP/PGDN lift  DEL remove");
    F->Out(20.f, 284.f, "H force floor/wall/ceiling   L light/prop   F campfire   [ ] model   ESC off");
}

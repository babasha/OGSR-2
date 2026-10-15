#include "StdAfx.h"
#include "xr_platform_seh.h"

#include "PHWorld.h"
#include "tri-colliderknoopc/dTriList.h"
#include "PhysicsCommon.h"
#include "PhysicsGamePars.h" // ph_max_correcting_vel
#include "Level.h"
#include "ExtendedGeom.h"
#include "dRayMotions.h"
#include "PHCollideValidator.h"
#include "../xr_3da/GameMtlLib.h"
#include "PHCommander.h"
#include "PHSimpleCalls.h"
#include "PHSynchronize.h"
#include "PHNetState.h"
#include "GeometryBits.h"
#include "PhysicsShell.h"
#include "PHCharacter.h"
#include "PhysicsShellHolder.h"
#include "ode/src/joint.h"
#include <chrono>

// ⚠⚠Switch (01-10): after EVERY save load physics costs 65-210 ms a frame for ~15 s (normal play: 0.03 ms), the frame
// drops to 4 fps, and the player sinks through the ground in exactly that window (v83: sank 11 m and came back when the
// cost dropped; v84: fell to death while it stayed at 137 ms). Which objects are paying, and how big are their islands?
// Armed by one slow step, prints the next slow ones (capped), disarms when steps are cheap again.
namespace
{
struct PhTop
{
    CPHObject* o = nullptr;
    float ms = 0.f;
    int nb = 0, nj = 0;
};
inline void ph_top_push(PhTop* t, CPHObject* o, float ms, int nb, int nj)
{
    for (int i = 0; i < 3; ++i)
        if (ms > t[i].ms)
        {
            for (int k = 2; k > i; --k)
                t[k] = t[k - 1];
            t[i] = {o, ms, nb, nj};
            return;
        }
}
inline void ph_obj_desc(CPHObject* o, char* buf, size_t sz)
{
    CPhysicsShellHolder* h = nullptr;
    const char* kind = "obj";
    if (auto* sh = dynamic_cast<CPhysicsShell*>(o))
    {
        if (sh->get_ElementsNumber() > 0)
            if (CPhysicsElement* e = sh->get_ElementByStoreOrder(0))
                h = e->PhysicsRefObject();
        kind = "shell";
    }
    else if (auto* ch = dynamic_cast<CPHCharacter*>(o))
    {
        h = ch->PhysicsRefObject();
        kind = "char";
    }
    const Fvector& P = o->spatial.sphere.P;
    xr_sprintf(buf, sz, "%s '%s'@(%.0f,%.0f,%.0f)", kind, h ? h->cName().c_str() : "?", P.x, P.y, P.z);
}
using ph_clk = std::chrono::steady_clock;
inline float ph_ms(ph_clk::time_point a, ph_clk::time_point b) { return std::chrono::duration<float, std::milli>(b - a).count(); }
bool s_ph_prof = false;
u32 s_ph_prof_logs = 0;

// ---------------------------------------------------------------------------
// [ph avg] (`ph_avg_prof 1`, 11-10-2026): the AVERAGE step, part by part and by kind of object. The second thread of a
// Switch fight is ~7 ms and physics (50 Hz) ~3.4 of it, also ~2.5 in a quiet village -- [ph prof] above only speaks
// about steps over 8 ms. Kind = "char"/"shell" + the C++ class of the object owning it. Second thread only.
struct PhAvgKind
{
    const char* kind;
    const char* cls;
    float collide = 0.f, island = 0.f;
    u32 n = 0;
};
struct PhAvgObj
{
    CPHObject* o;
    float ms;
    u32 n;
    char desc[112]; // taken when first seen: the object may be gone by the report
};
struct PhAvg
{
    u32 steps = 0, objects = 0, active = 0, bodies = 0;
    PhAvgObj objs[96];
    int no = 0;
    void add_obj(CPHObject* o, float ms)
    {
        for (int i = 0; i < no; ++i)
            if (objs[i].o == o)
            {
                objs[i].ms += ms;
                ++objs[i].n;
                return;
            }
        if (no < 96)
        {
            PhAvgObj& r = objs[no++];
            r.o = o;
            r.ms = ms;
            r.n = 1;
            ph_obj_desc(o, r.desc, sizeof(r.desc));
        }
    }
    s64 contacts = 0;
    float collide = 0.f, islands = 0.f, update = 0.f, tune = 0.f;
    PhAvgKind kinds[48];
    int nk = 0;
    ph_clk::time_point t0{};
    PhAvgKind* kind_of(CPHObject* o)
    {
        const char* kind = "obj";
        const char* cls = "?";
        CPhysicsShellHolder* h = nullptr;
        if (auto* sh = dynamic_cast<CPhysicsShell*>(o))
        {
            kind = "shell";
            if (sh->get_ElementsNumber() > 0)
                if (CPhysicsElement* e = sh->get_ElementByStoreOrder(0))
                    h = e->PhysicsRefObject();
        }
        else if (auto* ch = dynamic_cast<CPHCharacter*>(o))
        {
            kind = "char";
            h = ch->PhysicsRefObject();
        }
        if (h)
            cls = typeid(*h).name();
        for (int i = 0; i < nk; ++i)
            if (kinds[i].cls == cls && kinds[i].kind == kind)
                return &kinds[i];
        if (nk >= 48)
            return &kinds[47];
        kinds[nk] = {kind, cls};
        return &kinds[nk++];
    }
};
PhAvg s_ph_avg;
} // namespace
int g_ph_avg_prof = 0;

// ---------------------------------------------------------------------------
// ⭐ph_far_freeze <m> (11-10-2026, Switch). [ph avg] in the Cordon village fight named the objects behind ~3.4 ms of
// physics a frame on the second thread: props 300-500 m away (physic_object0028, esc_physic_destroyable_object_0070...)
// and a table stuck in the cellar wall, awake on 98 % of the steps for the whole session -- not one of them settles,
// whatever the sleep thresholds (ph_disable_k 4 changed nothing). Twice a second: a body (not a walker -- those belong to
// the AI) farther than this from the camera, awake for 5 s or more on its own, is frozen (CPHObject::Freeze: out of the
// step, still collidable -- a contact wakes it through EnableObject -> activate); a frozen one closer than 80 % of it is
// thawed. A global world freeze/unfreeze (ragdoll and shell activation) thaws everything, the next pass refreezes.
// 0 = off.
float g_ph_far_freeze = 0.f;

void CPHWorld::far_freeze_scan()
{
    const Fvector cam = Device.vCameraPosition;
    const float far2 = _sqr(g_ph_far_freeze), near2 = _sqr(g_ph_far_freeze * .8f);
    const u64 settle = 250; // steps: 5 s at 50 Hz
    static xr_vector<CPHObject*> s_tmp;
    static u32 s_frozen_total = 0, s_thawed_total = 0, s_said = 0;
    s_tmp.clear();
    for (PH_OBJECT_I it = m_objects.begin(); m_objects.end() != it; ++it)
    {
        CPHObject* o = *it;
        // shells only: walkers belong to the AI, and the short-lived CPHActivationShape is torn down through a plain
        // CPHObject::deactivate that knows nothing of a freeze (CPHShell::Deactivate thaws first)
        if (!dynamic_cast<CPhysicsShell*>(o))
            continue;
        if (m_steps_num - o->m_active_since < settle)
            continue;
        if (o->spatial.sphere.P.distance_to_sqr(cam) > far2)
            s_tmp.push_back(o);
    }
    for (CPHObject* o : s_tmp)
        o->Freeze();
    s_frozen_total += u32(s_tmp.size());
    s_tmp.clear();
    for (PH_OBJECT_I it = m_freezed_objects.begin(); m_freezed_objects.end() != it; ++it)
        if ((*it)->spatial.sphere.P.distance_to_sqr(cam) < near2)
            s_tmp.push_back(*it);
    for (CPHObject* o : s_tmp)
        o->UnFreeze();
    s_thawed_total += u32(s_tmp.size());
    if (s_said < 40 && (m_steps_num % 1500) == 0) // every 30 s, a few times
    {
        ++s_said;
        Msg("~ [ph far] frozen %u, thawed %u so far | now awake %u, frozen %u", s_frozen_total, s_thawed_total, u32(m_objects.count()),
            u32(m_freezed_objects.count()));
    }
}

// ---------------------------------------------------------------------------
// ⭐ph_settle <s> (12-10-2026, Switch). [ph avg] in the 12-bandit fight: the corpses are the dearest bodies of the step
// (a ragdoll 0.2-0.3 ms a step) and stay awake on 80-100 % of the steps for 40 s and more after the death, the table in
// the cellar wall for the whole session -- the sleep test is on VELOCITIES and these bodies never stop shivering, so
// no threshold catches them (ph_disable_k 4 changed nothing). This one looks at where the bodies ARE: twice a second
// every element of a shell awake for 2 s or more is compared with where it was half a second before; a shell none of
// whose elements moved more than 3 cm or turned more than ~2 degrees for <s> seconds running is frozen (the same
// CPHObject::Freeze as ph_far_freeze: out of the step, still collidable; a hit or a contact wakes it through
// EnableObject -> activate). What moves -- a fan's blades, a swinging lamp, a body sliding down -- is never frozen.
// 0 = off.
float g_ph_settle = 0.f;

void CPHWorld::settle_scan()
{
    struct Snap
    {
        u64 seen;
        u8 quiet, n;
        Fvector p[16], i[16], k[16];
    };
    static xr_unordered_map<CPHObject*, Snap> s_snap;
    static u32 s_frozen_total = 0, s_said = 0;
    static xr_vector<CPHObject*> s_tmp;
    s_tmp.clear();
    const u8 need = u8(_max(1, iCeil(g_ph_settle / .5f)));
    for (PH_OBJECT_I it = m_objects.begin(); m_objects.end() != it; ++it)
    {
        CPHObject* o = *it;
        CPhysicsShell* sh = dynamic_cast<CPhysicsShell*>(o);
        if (!sh || m_steps_num - o->m_active_since < 100) // 2 s at 50 Hz: the stock sleep test goes first
            continue;
        const u16 ne = sh->get_ElementsNumber();
        if (!ne)
            continue;
        Snap& s = s_snap[o];
        const bool have = s.seen + 25 == m_steps_num && s.n == u8(_min(ne, u16(16)));
        bool quiet = have;
        s.n = u8(_min(ne, u16(16)));
        // the elements past 16 are not compared: a ragdoll has 15, a prop a few
        for (u16 e = 0; e < s.n; ++e)
        {
            dBodyID b = sh->get_ElementByStoreOrder(e)->get_body();
            if (!b)
                continue;
            const dReal* P = dBodyGetPosition(b);
            const dReal* R = dBodyGetRotation(b); // 3x4, rows: the axes' x, y, z components
            const Fvector p{P[0], P[1], P[2]}, ax{R[0], R[4], R[8]}, az{R[2], R[6], R[10]};
            if (quiet && (p.distance_to_sqr(s.p[e]) > _sqr(.03f) || ax.distance_to_sqr(s.i[e]) > _sqr(.035f) ||
                          az.distance_to_sqr(s.k[e]) > _sqr(.035f)))
                quiet = false;
            s.p[e] = p;
            s.i[e] = ax;
            s.k[e] = az;
        }
        s.seen = m_steps_num;
        s.quiet = quiet ? u8(s.quiet + 1) : 0;
        if (s.quiet >= need)
            s_tmp.push_back(o);
    }
    for (CPHObject* o : s_tmp)
    {
        if (s_said < 60)
        {
            ++s_said;
            char desc[112];
            ph_obj_desc(o, desc, sizeof(desc));
            Msg("~ [ph settle] frozen after %.1f s awake: %s", float(m_steps_num - o->m_active_since) * fixed_step, desc);
        }
        s_snap.erase(o);
        o->Freeze();
    }
    s_frozen_total += u32(s_tmp.size());
    // forget the bodies that are no longer awake (asleep, frozen, gone): the map holds pointers, not owners
    for (auto it = s_snap.begin(); it != s_snap.end();)
        it = it->second.seen == m_steps_num ? std::next(it) : s_snap.erase(it);
    if (s_said < 80 && (m_steps_num % 1500) == 0)
    {
        ++s_said;
        Msg("~ [ph settle] frozen %u so far | now awake %u, frozen %u", s_frozen_total, u32(m_objects.count()), u32(m_freezed_objects.count()));
    }
}

//////////////////////////////////////////////////////////////
//////////////CPHMesh///////////////////////////////////////////
///////////////////////////////////////////////////////////
BOOL g_bDebugDumpPhysicsStep = 0;

void CPHMesh ::Create(dSpaceID space, dWorldID world) { 
    Geom = dCreateTriList(space, 0, 0); 
    CPHGeometryBits::init_geom(*this);
}
/////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////

void CPHMesh ::Destroy()
{
    dGeomDestroy(Geom);
    dTriListClass = -1;
}

////////////////////////////////////////////////////////////////////////////
///////////CPHWorld/////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

CPHWorld::CPHWorld()
{
    disable_count = 0;
    m_frame_time = 0.f;
    m_previous_frame_time = 0.f;
    b_frame_mark = false;
    m_steps_num = 0;
    m_frame_sum = 0.f;
    m_delay = 0;
    m_previous_delay = 0;
    m_reduce_delay = 0;
    m_update_delay_count = 0;
    b_world_freezed = false;
    b_processing = false;
    m_processing_frame = u32(-1);
    m_gravity = default_world_gravity;
    b_exist = false;
}
void CPHWorld::SetStep(dReal s)
{
    fixed_step = s;
    world_cfm = CFM(SPRING_S(base_cfm, base_erp, base_fixed_step), DAMPING(base_cfm, base_erp));
    world_erp = ERP(SPRING_S(base_cfm, base_erp, base_fixed_step), DAMPING(base_cfm, base_erp));
    world_spring = 1.0f * SPRING(world_cfm, world_erp);
    world_damping = 1.0f * DAMPING(world_cfm, world_erp);
    if (ph_world && ph_world->Exist())
    {
        float frame_time = Device.fTimeDelta;
        u32 it_number = iFloor(frame_time / fixed_step);
        frame_time -= it_number * fixed_step;
        ph_world->m_previous_frame_time = frame_time;
        ph_world->m_frame_time = frame_time;
    }
}
void CPHWorld::Create()
{
    dWorldID phWorld = 0;

    // See g_ph_single_thread (device.cpp): a host with no renderer puts game
    // logic where the render used to be, and physics must not run beside it.
    extern ENGINE_API bool g_ph_single_thread;
    if (g_ph_single_thread)
    {
        Msg("* [PHWorld] stepping inside the main frame (single-threaded physics)");
        Device.seqFrame.Add(this, REG_PRIORITY_HIGH);
    }
    else
        Device.seqFrameMT.Add(this, REG_PRIORITY_HIGH);

    m_commander = xr_new<CPHCommander>();
    // dVector3 extensions={2048,256,2048};

    dWorldSetAutoEnableDepthSF1(phWorld, 100000000);
    /// dWorldSetContactSurfaceLayer(phWorld,0.f);
    // phWorld->contactp.min_depth =0.f;

    ContactGroup = dJointGroupCreate(0);
    dWorldSetGravity(phWorld, 0, -Gravity(), 0); //-2.f*9.81f
    Mesh.Create(0, phWorld);

    // const  dReal k_p=2400000.f;//550000.f;///1000000.f;
    // const dReal k_d=200000.f;
    dWorldSetERP(phWorld, ERP(world_spring, world_damping));
    dWorldSetCFM(phWorld, CFM(world_spring, world_damping));
    // dWorldSetERP(phWorld,  0.2f);
    // dWorldSetCFM(phWorld,  0.000001f);

    // ⭐⭐⭐HOW FAST THE SOLVER MAY PUSH TWO OVERLAPPING BODIES APART.
    //
    // ODE's default is dInfinity (ode.cpp: w->contactp.max_vel = dInfinity), and
    // nothing in this tree ever set it. So the depth of an overlap could be
    // turned into ANY velocity -- error correction with no speed limit.
    //
    // USER-REPORTED 31-07-2026: "убился прыгнув на НПС", twice. Measured at the
    // point where a contact becomes damage:
    //
    //   single_player <-> stalker closing  4.82 m/s
    //                                      4.74
    //                                     11.84   <- doubling
    //                                      9.88
    //   collision damage: contact 19.54 travelling 22.02 -> health lost 0.35
    //                     contact 23.97 travelling 23.78 -> health lost 0.62
    //                     contact 22.92 travelling 23.58 -> health lost 0.56  DEAD
    //
    // ⚠The player really was moving at 22 m/s -- `travelling` is the character's
    // own saved velocity, and it agrees with the contact speed. This was not a
    // misread number, it was a launch. A driven puppet's capsule is TELEPORTED
    // by CCustomMonster::net_MoveTo on every server assertion and then pinned
    // with SetVelocity(0,0,0), so it re-penetrates whatever it stands in,
    // absorbs nothing, and the solver spends the depth on the only body left --
    // deeper overlap next tick, bigger impulse, 4.8 -> 22 m/s in 120 ms.
    //
    // ⭐The cap is not a workaround for that: an unbounded correcting velocity
    // is a bug wherever the depth comes from, and the ODE manual says as much.
    // What it buys here is an INVARIANT worth stating: kept below the actor's
    // fMinCrashSpeed (14 m/s in this content), penetration recovery can never
    // by construction produce a speed the damage model charges for. Error
    // correction must not be able to hurt you.
    //
    // ⚠⚠AND WHY THIS WORKS THROUGH A NULL HANDLE, because the next person to
    // read it will assume it does not. `phWorld` here is a LOCAL initialised to
    // 0 (see the top of this function), shadowing the global of the same name,
    // and it really is null at runtime -- measured, printed below. Every
    // dWorld* call in this function is made on it, gravity and ERP and CFM
    // included.
    //
    // They all take effect anyway: in this vendored ODE the world's parameters
    // are STATIC members of dxWorld (objects.h) with file-scope initialisers
    // (ode.cpp: `dxContactParameters dxWorld::contactp = {dInfinity, 0.001f}`).
    // `w->contactp` therefore names a static and never dereferences w. That is
    // also how this fork affords a separate CPHIsland world per physics object
    // -- one shared parameter block, many islands.
    //
    // ⭐So the setting is global by construction, and so was the defect.
    dWorldSetContactMaxCorrectingVel(phWorld, ph_max_correcting_vel);
    Msg("* [PHWorld] contact max correcting velocity = %.2f m/s (ODE ships dInfinity) | world handle = %p (null is expected -- the parameters are static)",
        ph_max_correcting_vel, (void*)phWorld);
    disable_count = 0;
    m_motion_ray = dCreateRayMotions(0);
    phBoundaries.set(Level().ObjectSpace.GetBoundingVolume());
    phBoundaries.y1 -= 30.f;
    CPHCollideValidator::Init();
    b_exist = true;
}

/////////////////////////////////////////////////////////////////////////////

void CPHWorld::Destroy()
{
    r_spatial.clear();
    xr_delete(m_commander);
    Mesh.Destroy();
    dGeomDestroy(m_motion_ray);
    dJointGroupEmpty(ContactGroup);
    dJointGroupDestroy(ContactGroup);
    ContactFeedBacks.clear();
    ContactEffectors.clear();
    dCloseODE();
    dCylinderClassUser = -1;
    dRayMotionsClassUser = -1;
    Device.seqFrameMT.Remove(this);
    Device.seqFrame.Remove(this);
    b_exist = false;
}
void CPHWorld::SetGravity(float g)
{
    m_gravity = g;
    dWorldID phWorld = 0;
    dWorldSetGravity(phWorld, 0, -m_gravity, 0); //-2.f*9.81f
}

void CPHWorld::OnFrame()
{
    // ⚠Whatever the last step left behind, a new frame means no step is in
    // flight. CRegistrator::Process wraps every frame member in a try/catch
    // (see pure.h) precisely because this engine's members throw -- and an
    // exception carried out of a step past the guard below leaves the flag
    // raised, which turns half a dozen "can not do X during physics
    // processing" fatals live for the rest of the process.
    b_processing = false;

    // ⭐⭐⭐No freeze scope spans a frame -- every one of them freezes, hand-steps,
    // and unfreezes inside a single call. So a freeze still open here was
    // abandoned: something left the scope without unwinding it (a structured
    // fault inside the hand-stepping, which the scope guard cannot catch). End
    // it. Left alone it is not a slow leak but an immediate death sentence for
    // whoever calls Freeze() next -- and the physics world stops stepping at all,
    // because FrameStep returns early while IsFreezed().
    if (m_freeze_depth != 0 && m_freeze_frame != Device.dwFrame)
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            Msg("! [PHWorld] a world freeze from frame %u was still open at frame %u (depth %u) -- abandoned mid-scope, ending it", m_freeze_frame, Device.dwFrame,
                m_freeze_depth);
        }
        m_freeze_depth = 1; // collapse whatever nesting was abandoned
        UnFreeze();
    }

    // Teardowns that arrived mid-step, now that no step is running.
    DrainDeferred();

    // Msg									("------------- physics: %d / %d",u32(Device.dwFrame),u32(m_steps_num));
    //просчитать полет пуль
    Device.Statistic->Physics.Begin();
    FrameStepGuarded(Device.fTimeDelta);
    Device.Statistic->Physics.End();

    // ⚠If this ever fires, the scope guard inside FrameStep did not run -- which
    // a C++ exception cannot cause, so the step was left by a longjmp (LuaJIT
    // errors out of a contact callback that way, and this level throws plenty).
    // Clear it regardless: the step IS over, whatever the flag says.
    if (b_processing)
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            Msg("! [PHWorld] FrameStep left the processing flag raised -- the step was exited without unwinding (longjmp)");
        }
        b_processing = false;
    }
}

//////////////////////////////////////////////////////////////////////////////
// static dReal frame_time=0.f;
static u32 start_time = 0;
void CPHWorld::Step()
{
    // [mod-compat diag] heartbeat: is the physics world stepping at all? ("actor
    // frozen in mcFall" hunt — remove once the foreign-map freeze is understood)
    {
        static u32 s_steps = 0;
        if ((++s_steps % 1024) == 1)
            Msg("[PHWorld] step #%u", s_steps);
    }

    VERIFY(b_processing || IsFreezed());

    if (g_ph_far_freeze > 0.f && !b_world_freezed && (m_steps_num % 25) == 0)
        far_freeze_scan();
    if (g_ph_settle > 0.f && !b_world_freezed && (m_steps_num % 25) == 0)
        settle_scan();

    PH_OBJECT_I i_object;
    PH_UPDATE_OBJECT_I i_update_object;

    if (disable_count == 0)
    {
        disable_count = worldDisablingParams.objects_params.L2frames;
        for (i_object = m_recently_disabled_objects.begin(); m_recently_disabled_objects.end() != i_object;)
        {
            CPHObject* obj = (*i_object);
            obj->check_recently_deactivated();
            ++i_object;
        }
    }
    --disable_count;

    ++m_steps_num;
    Device.Statistic->ph_collision.Begin();

    const bool prof = s_ph_prof;
    const bool avg = g_ph_avg_prof != 0;
    PhTop topC[3], topI[3];
    u32 nObjects = 0;
    const auto tStart = ph_clk::now();
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        ++nObjects;
        if (prof || avg)
        {
            const auto a = ph_clk::now();
            obj->Collide();
            const float ms = ph_ms(a, ph_clk::now());
            if (prof)
                ph_top_push(topC, obj, ms, obj->Island().nb, obj->Island().nj);
            if (avg)
            {
                PhAvgKind* k = s_ph_avg.kind_of(obj);
                k->collide += ms;
                ++k->n;
                s_ph_avg.add_obj(obj, ms);
            }
        }
        else
            obj->Collide();

        ++i_object;
    }
    Device.Statistic->ph_collision.End();
    const auto tCollide = ph_clk::now();
    const int nContacts = ContactGroup ? ContactGroup->num : -1;

    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        ++i_object;
        obj->PhTune(fixed_step);
    }

    for (i_update_object = m_update_objects.begin(); m_update_objects.end() != i_update_object;)
    {
        CPHUpdateObject* obj = (*i_update_object);
        ++i_update_object;
        obj->PhTune(fixed_step);
    }
    const auto tTune = ph_clk::now();

    Device.Statistic->ph_core.Begin();
    //////////////////////////////////////////////////////////////////////
    m_commander->update();
    //////////////////////////////////////////////////////////////////////
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        ++i_object;

        if (prof || avg)
        {
            const int nb = obj->Island().nb, nj = obj->Island().nj; // merged counts, before the step
            const bool active = obj->Island().IsActive();
            const auto a = ph_clk::now();
            obj->IslandStep(fixed_step);
            const float ms = ph_ms(a, ph_clk::now());
            if (prof)
                ph_top_push(topI, obj, ms, nb, nj);
            if (avg)
            {
                s_ph_avg.kind_of(obj)->island += ms;
                s_ph_avg.add_obj(obj, ms);
                if (active)
                {
                    ++s_ph_avg.active;
                    s_ph_avg.bodies += u32(nb);
                }
            }
        }
        else
            obj->IslandStep(fixed_step);
    }

    Device.Statistic->ph_core.End();
    const auto tIsland = ph_clk::now();

    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        ++i_object;
        obj->IslandReinit();
        obj->PhDataUpdate(fixed_step);
        obj->spatial_move();
    }

    for (i_update_object = m_update_objects.begin(); m_update_objects.end() != i_update_object;)
    {
        CPHUpdateObject* obj = *i_update_object;
        ++i_update_object;
        obj->PhDataUpdate(fixed_step);
    }
    const auto tEnd = ph_clk::now();
    if (avg)
    {
        PhAvg& A = s_ph_avg;
        if (A.steps == 0 && A.t0 == ph_clk::time_point{})
            A.t0 = tStart;
        ++A.steps;
        A.objects += nObjects;
        A.contacts += nContacts;
        A.collide += ph_ms(tStart, tCollide);
        A.tune += ph_ms(tCollide, tTune);
        A.islands += ph_ms(tTune, tIsland);
        A.update += ph_ms(tIsland, tEnd);
        const float sec = ph_ms(A.t0, tEnd) / 1000.f;
        if (sec >= 5.f && A.steps)
        {
            const float s = float(A.steps);
            Msg("~ [ph avg] %u steps in %.1f s: %.2f ms a step = collide %.2f + tune %.2f + islands %.2f + update %.2f | objects %.0f, "
                "active islands %.1f (%.0f bodies), contacts %.0f",
                A.steps, sec, (A.collide + A.tune + A.islands + A.update) / s, A.collide / s, A.tune / s, A.islands / s, A.update / s,
                float(A.objects) / s, float(A.active) / s, float(A.bodies) / s, float(A.contacts) / s);
            std::sort(A.kinds, A.kinds + A.nk, [](const PhAvgKind& x, const PhAvgKind& y) { return x.collide + x.island > y.collide + y.island; });
            for (int i = 0; i < A.nk && i < 8; ++i)
                Msg("~ [ph avg]   %-5s %-36s collide %.3f + island %.3f ms a step | %.1f objects", A.kinds[i].kind, A.kinds[i].cls,
                    A.kinds[i].collide / s, A.kinds[i].island / s, float(A.kinds[i].n) / s);
            // the dearest single objects
            std::sort(A.objs, A.objs + A.no, [](const PhAvgObj& x, const PhAvgObj& y) { return x.ms > y.ms; });
            for (int i = 0; i < A.no && i < 10; ++i)
                Msg("~ [ph avg]     %.3f ms a step, in %.0f%% of steps: %s", A.objs[i].ms / s, 100.f * float(A.objs[i].n) / (2.f * s), A.objs[i].desc);
            A = PhAvg{};
            A.t0 = tEnd;
        }
    }
    {
        const float total = ph_ms(tStart, tEnd);
        if (total > 8.f)
        {
            if (prof && s_ph_prof_logs < 48)
            {
                ++s_ph_prof_logs;
                Msg("![ph prof] step %u: %.1f ms = collide %.1f + tune/commander/islands %.1f + update %.1f | objects %u contacts %d", m_steps_num, total,
                    ph_ms(tStart, tCollide), ph_ms(tCollide, tIsland), ph_ms(tIsland, tEnd), nObjects, nContacts);
                string256 b;
                for (int i = 0; i < 3 && topC[i].o; ++i)
                {
                    ph_obj_desc(topC[i].o, b, sizeof(b));
                    Msg("!   [ph prof] collide #%d %.2f ms %s island nb=%d nj=%d", i, topC[i].ms, b, topC[i].nb, topC[i].nj);
                }
                for (int i = 0; i < 3 && topI[i].o; ++i)
                {
                    ph_obj_desc(topI[i].o, b, sizeof(b));
                    Msg("!   [ph prof] island  #%d %.2f ms %s island nb=%d nj=%d", i, topI[i].ms, b, topI[i].nb, topI[i].nj);
                }
            }
            s_ph_prof = true;
        }
        else if (total < 4.f)
            s_ph_prof = false;
    }
    dJointGroupEmpty(ContactGroup); // this is to be called after PhDataUpdate!!!-the order is critical!!!
    ContactFeedBacks.empty();
    ContactEffectors.empty();

    if (physics_step_time_callback)
    {
        physics_step_time_callback(start_time, start_time + u32(fixed_step * 1000));
        start_time += u32(fixed_step * 1000);
    };
}

void CPHWorld::StepTouch()
{
    PH_OBJECT_I i_object;
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        obj->Collide();

        ++i_object;
    }

    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        ++i_object;
        obj->Island().Enable();
    }
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        ++i_object;
        obj->IslandReinit();
        obj->spatial_move();
    }
    dJointGroupEmpty(ContactGroup);
    ContactFeedBacks.empty();
    ContactEffectors.empty();
}

// The frame registrar up in pure.h reports a faulting member as a number in a
// doubling counter. That is the right amount of noise for a member that misbehaves
// once; it is the wrong amount for one that faults a hundred times a second, and
// it never says WHERE. Here the stack is still standing, so ask it.
static LONG ph_frame_fault_filter(_EXCEPTION_POINTERS* ep)
{
    static u32 s_faults = 0;
    const u32 n = ++s_faults;
    if (n <= 2) // two traces is enough to see whether it is always the same site
        LogStackTrace("!![PHWorld] physics frame faulted -- stack trace:\n", ep, false);
    else if ((n & (n - 1)) == 0) // 4, 8, 16, ...
        Msg("!![PHWorld] physics frame faulted, code 0x%08x (total %u)", ep->ExceptionRecord->ExceptionCode, n);
    return EXCEPTION_EXECUTE_HANDLER;
}

void CPHWorld::FrameStepGuarded(dReal step)
{
    XR_SEH_TRY
    {
        FrameStep(step);
    }
    XR_SEH_EXCEPT(ph_frame_fault_filter(GetExceptionInformation()))
    {
        // This frame's physics is gone. Nothing is cleaned up on the way out --
        // a structured fault runs no destructors -- which is exactly why the
        // processing flag, the freeze scope and the deferred teardowns are all
        // reconciled at the TOP of the next OnFrame rather than trusted to
        // unwind here.
    }
}

extern ENGINE_API int g_cpu_off;
void CPHWorld::FrameStep(dReal step)
{
    if (IsFreezed() || g_cpu_off)
        return;

    VERIFY(_valid(step));
    step *= phTimefactor;
    // compute contact joints and forces

    // step+=astep;

    // const  dReal k_p=24000000.f;//550000.f;///1000000.f;
    // const dReal k_d=400000.f;
    u32 it_number;
    float frame_time = m_frame_time;
    frame_time += step;
    // m_frame_sum+=step;
    if (!(frame_time < fixed_step))
    {
        it_number = iFloor(frame_time / fixed_step);
        frame_time -= it_number * fixed_step;
        m_previous_frame_time = m_frame_time;
        m_frame_time = frame_time;
        b_frame_mark = !b_frame_mark;
    }
    else
    {
        m_frame_time = frame_time;
        return;
    }
    // for(UINT i=0;i<(m_reduce_delay+1);++i)

    // ⚠⚠⚠b_processing used to be a bare pair of assignments around the step
    // loop, and Step() reaches game code -- contact callbacks, and through them
    // Lua. One exception out of there and the flag stayed TRUE for the rest of
    // the process. Nothing notices until something tries to tear down a physics
    // character: CPHSimpleCharacter::Destroy is FATAL while the world is
    // "processing", so from that moment on the FIRST creature to die or fall
    // wounded kills the server -- with a stack that points at the death, miles
    // from the exception that actually broke it.
    //
    // ⚠Found 27-07 on the headless host, which throws Lua errors from script
    // binders all through a level and had never had an NPC death reach this
    // code before. The leak is not headless-specific.
    if (Processing())
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            Msg("! [PHWorld] FrameStep re-entered inside a step -- physics is being driven recursively");
        }
    }

    struct processing_scope
    {
        bool& flag;
        explicit processing_scope(bool& f) : flag(f) { flag = true; }
        ~processing_scope() { flag = false; }
    } _processing{b_processing};
    m_processing_frame = Device.dwFrame;

    start_time = Device.dwTimeGlobal; // - u32(m_frame_time*1000);
    if (g_bDebugDumpPhysicsStep && it_number > 20)
        Msg("!!!TOO MANY PHYSICS STEPS PER FRAME = %d !!!", it_number);
    // Death-spiral cap: a slow frame owes more sub-steps, each of which is slower
    // in a crowd, owing still more next frame -> runaway. Capping the count lets
    // physics time dilate (brief slow-motion) instead of freezing. The accumulator
    // (m_frame_time) already holds only the sub-fixed_step remainder, so the excess
    // owed time is simply dropped here rather than carried forward as debt.
    // ph_max_substeps == 0 keeps stock (uncapped) behaviour -- except on a thin
    // network client, which ALWAYS gets a cap: its load->game transition hands
    // physics a multi-second first frame (sync stall) on a GPU it may share with
    // another client, and uncapped catch-up spirals into a permanent fps=0 state
    // (observed 23-07: 359k steps in 4 min, log alive, screen frozen).
    extern ENGINE_API bool g_net_client_st;
    int max_substeps = ph_max_substeps;
    if (max_substeps == 0 && g_net_client_st)
        max_substeps = 10;
#ifdef __SWITCH__
    {   // "fell under the map ~50 m after loading a save" hunt (Switch, 01-10): how much time does physics owe, how much is dropped
        static u32 s_capLog = 0;
        if (it_number > 3 && s_capLog < 60)
        {
            ++s_capLog;
            Msg("[PHWorld] frame step=%.3f s owes %u sub-steps (cap %d) frame %u t=%u", step, it_number, max_substeps, Device.dwFrame, Device.dwTimeGlobal);
        }
    }
#endif
    if (max_substeps > 0 && it_number > (u32)max_substeps)
        it_number = (u32)max_substeps;
    for (UINT i = 0; i < it_number; ++i)
        Step();
#ifdef __SWITCH__
    {   // same hunt: the controlled entity's height, logged when it changes by > 3 m between physics frames (and every ~5 s)
        static float s_lastY = 1e9f;
        static u32 s_lastT = 0, s_n = 0;
        if (CObject* o = Level().CurrentEntity())
        {
            const float y = o->Position().y;
            if (s_lastY > 1e8f || fabsf(y - s_lastY) > 3.f || Device.dwTimeGlobal - s_lastT > 5000)
            {
                if (s_n++ < 400)
                    Msg("[PHWorld] actor y=%.1f (was %.1f) pos=(%.1f,%.1f) steps=%u frame %u t=%u", y, s_lastY > 1e8f ? y : s_lastY, o->Position().x, o->Position().z,
                        it_number, Device.dwFrame, Device.dwTimeGlobal);
                s_lastY = y;
                s_lastT = Device.dwTimeGlobal;
            }
        }
    }
#endif
}

void CPHWorld::AddObject(CPHObject* object)
{
    m_objects.push_back(object);
    // xr_list <CPHObject*> ::iterator i= m_objects.end();
    // return (--m_objects.end());
};
void CPHWorld::AddRecentlyDisabled(CPHObject* object) { m_recently_disabled_objects.push_back(object); }
void CPHWorld::RemoveFromRecentlyDisabled(PH_OBJECT_I i) { m_recently_disabled_objects.erase(i); }

void CPHWorld::AddUpdateObject(CPHUpdateObject* object)
{
    //.	if(object->IsFreezed())m_freezed_update_objects.erase(i);
    m_update_objects.push_back(object);
}

void CPHWorld::RemoveUpdateObject(PH_UPDATE_OBJECT_I i) { m_update_objects.erase(i); }

void CPHWorld::RemoveObject(PH_OBJECT_I i) { m_objects.erase((i)); };

void CPHWorld::AddFreezedObject(CPHObject* obj) { m_freezed_objects.push_back(obj); }

void CPHWorld::RemoveFreezedObject(PH_OBJECT_I i) { m_freezed_objects.erase(i); }
void CPHWorld::DeferDeactivate(CPhysicsShell* shell)
{
    if (!shell)
        return;
    if (std::find(m_deferred_deactivate.begin(), m_deferred_deactivate.end(), shell) == m_deferred_deactivate.end())
        m_deferred_deactivate.push_back(shell);
}

void CPHWorld::ForgetDeferred(CPhysicsShell* shell)
{
    auto i = std::find(m_deferred_deactivate.begin(), m_deferred_deactivate.end(), shell);
    if (i != m_deferred_deactivate.end())
        m_deferred_deactivate.erase(i);
}

void CPHWorld::DrainDeferred()
{
    // Taken by swap: Deactivate can put another shell on the list (it steps the
    // touch pass, which reaches game code), and growing a vector under its own
    // iterator is the bug this project already paid for once in the NPC roster.
    if (m_deferred_deactivate.empty())
        return;

    xr_vector<CPhysicsShell*> due;
    due.swap(m_deferred_deactivate);
    for (CPhysicsShell* s : due)
    {
        // isActive() is re-checked here and not only in Deactivate: between the
        // deferral and this drain the shell may have been switched off by some
        // other path, and calling Deactivate on an inactive shell walks into
        // the freeze bookkeeping for nothing.
        if (s->isActive())
            s->Deactivate();
    }
}

void CPHWorld::Freeze()
{
    // A freeze inside a freeze wants what the outer one already did. See the
    // comment on Freeze() in PHWorld.h for why this is counted and not fatal.
    if (m_freeze_depth++ != 0)
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            Msg("! [PHWorld] a world freeze was taken inside another one (depth %u) -- the inner scope is a no-op", m_freeze_depth);
        }
        return;
    }

    m_freeze_frame = Device.dwFrame;
    // only the awake ones: m_freezed_objects may already hold bodies frozen one by one (ph_far_freeze) -- freezing
    // them again was the "FreezeContent on an object already in that state" warning. UnFreeze still thaws them all.
    PH_OBJECT_I iter = m_objects.begin(), e = m_objects.end();
    for (; e != iter; ++iter)
        (*iter)->FreezeContent();
    m_freezed_objects.move_items(m_objects);
    m_freezed_update_objects.move_items(m_update_objects);
    b_world_freezed = true;
}
void CPHWorld::UnFreeze()
{
    if (m_freeze_depth == 0)
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            Msg("! [PHWorld] UnFreeze with no freeze open -- an abandoned scope was already ended by OnFrame, or an unfreeze is unpaired");
        }
        return;
    }
    if (--m_freeze_depth != 0)
        return;

    // the bodies frozen one by one (ph_far_freeze / ph_settle) go back to their freeze: thawed here with the rest, they
    // woke on every shell activation -- 📏cpu25 ph_settle: 797 freezes in one fight for 27 bodies frozen at a time
    static xr_vector<CPHObject*> s_solo;
    s_solo.clear();
    PH_OBJECT_I iter = m_freezed_objects.begin(), e = m_freezed_objects.end();
    for (; e != iter; ++iter)
    {
        (*iter)->UnFreezeContent();
        if ((*iter)->m_solo_frozen)
            s_solo.push_back(*iter);
    }
    m_objects.move_items(m_freezed_objects);
    m_update_objects.move_items(m_freezed_update_objects);
    b_world_freezed = false;
    for (CPHObject* o : s_solo)
        o->Freeze();
}
bool CPHWorld::IsFreezed() { return b_world_freezed; }

ph_world_freeze_scope::ph_world_freeze_scope() : m_held(true) { ph_world->Freeze(); }
ph_world_freeze_scope::~ph_world_freeze_scope()
{
    if (!m_held)
        return;
    // Reaching here means the function was left without passing its own
    // UnFreeze() -- the case this class exists for. Say so; a frozen world that
    // outlives its scope is how the wrong caller ends up dying.
    static bool s_said = false;
    if (!s_said)
    {
        s_said = true;
        Msg("! [PHWorld] a world freeze scope was left without unfreezing -- ended by the scope guard");
    }
    ph_world->UnFreeze();
    m_held = false;
}
void ph_world_freeze_scope::unfreeze()
{
    if (!m_held)
        return;
    m_held = false;
    ph_world->UnFreeze();
}

void CPHWorld::CutVelocity(float l_limit, float a_limit)
{
    PH_OBJECT_I i_object;
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        obj->CutVelocity(l_limit, a_limit);
        ++i_object;
    }
}
void CPHWorld::NetRelcase(CPhysicsShell* s)
{
    CPHReqComparerHasShell c(s);
    m_commander->remove_calls(&c);
}
void CPHWorld::AddCall(CPHCondition* c, CPHAction* a) { m_commander->add_call(c, a); }
u16 CPHWorld::ObjectsNumber() { return m_objects.count(); }
void CPHWorld::GetState(V_PH_WORLD_STATE& state)
{
    state.clear();
    PH_OBJECT_I i_object;
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        const u16 els = obj->get_elements_number();
        for (u16 i = 0; els > i; ++i)
        {
            std::pair<CPHSynchronize*, SPHNetState> s;
            s.first = obj->get_element_sync(i);
            s.first->get_State(s.second);
            state.push_back(s);
        }
        ++i_object;
    }
}

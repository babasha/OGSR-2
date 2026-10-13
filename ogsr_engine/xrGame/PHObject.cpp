#include "StdAfx.h"
#include "Physics.h"
#include "PHObject.h"
#include "PHWorld.h"
#include "PHMoveStorage.h"
#include "dRayMotions.h"
#include "PHCollideValidator.h"
#ifdef DEBUG
#include "PHDebug.h"
#endif
extern CPHWorld* ph_world;

CPHObject::CPHObject() : ISpatial(g_SpatialSpacePhysic)
{
    m_flags.flags = 0;
    spatial.type |= STYPE_PHYSIC;
    spatial.dbg_name = "CPHObject";
    m_island.Init();
    m_check_count = 0;
    CPHCollideValidator::InitObject(*this);
}

// Module-relative addresses for the log below: subtract (anchor printed) - (nm value of ph_dtor_anchor).
static void ph_dtor_anchor() {}

// ⚠⚠⚠Switch v81 (01-10): Data Abort in dWorldAddJoint <- CollideIntoGroup <- CPHObject::CollideDynamics on the physics
// thread, island firstjoint == 0x30 -- a malloc chunk header, i.e. the island of a FREED object was still reachable from
// the world's lists. The destructor used to be empty: an object deleted while still linked into m_objects /
// m_freezed_objects / m_recently_disabled_objects left a dangling node in an intrusive list. On Windows the freed bytes
// usually survive untouched (and the step is wrapped in SEH), on Horizon newlib reuses them at once. Unlink it here and
// say who did it, so the next run names the culprit instead of crashing a step later.
CPHObject::~CPHObject()
{
    const bool act = !!m_flags.test(st_activated), frz = !!m_flags.test(st_freezed), rec = !!m_flags.test(st_recently_deactivated);
    if (!(act || frz || rec) || !ph_world)
        return;
    static u32 s_n = 0;
    if (s_n++ < 64)
    {
        Msg("! [PHObject] %p destroyed while still linked into the physics world (activated=%d freezed=%d recent=%d processing=%d islandActive=%d nj=%d nb=%d) frame %u -- unlinking | anchor=%p",
            this, act, frz, rec, ph_world->Processing() ? 1 : 0, m_island.IsActive() ? 1 : 0, m_island.nj, m_island.nb, Device.dwFrame, (void*)&ph_dtor_anchor);
#if defined(__GNUC__) || defined(__clang__)
        // Frame-pointer walk: who deleted it (aarch64/x64 frame record = {prev fp, return address}).
        void** fp = (void**)__builtin_frame_address(0);
        for (int i = 0; i < 12 && fp; ++i)
        {
            void** prev = (void**)fp[0];
            void* ret = fp[1];
            if (!ret || prev <= fp)
                break;
            Msg("!   [PHObject] dtor caller %d: %p", i, ret);
            fp = prev;
        }
#endif
    }
    if (act)
        ph_world->RemoveObject(PH_OBJECT_I(this));
    else if (frz)
        ph_world->RemoveFreezedObject(PH_OBJECT_I(this));
    else
        ph_world->RemoveFromRecentlyDisabled(PH_OBJECT_I(this));
    m_flags.set(st_activated | st_freezed | st_recently_deactivated, FALSE);
}

void CPHObject::activate()
{
    R_ASSERT2(dSpacedGeom(), "trying to activate destroyed or not created object!");
    if (m_flags.test(st_activated))
        return;
    if (m_flags.test(st_freezed))
    {
        UnFreeze();
        return;
    }
    if (m_flags.test(st_recently_deactivated))
        remove_from_recently_deactivated();
    ph_world->AddObject(this);
    vis_update_activate();
    m_flags.set(st_activated, TRUE);
    m_active_since = ph_world->StepsNum();
}

void CPHObject::EnableObject(CPHObject* obj) { activate(); }

void CPHObject::deactivate()
{
    if (!m_flags.test(st_activated))
        return;
    VERIFY2(m_island.IsActive(), "can not do it during processing");
    ph_world->RemoveObject(PH_OBJECT_I(this));
    vis_update_deactivate();
    m_flags.set(st_activated, FALSE);
}

void CPHObject::put_in_recently_deactivated()
{
    VERIFY(!m_flags.test(st_activated) && !m_flags.test(st_freezed));
    if (m_flags.test(st_recently_deactivated))
        return;
    m_check_count = u8(ph_tri_clear_disable_count);
    m_flags.set(st_recently_deactivated, TRUE);
    ph_world->AddRecentlyDisabled(this);
}
void CPHObject::remove_from_recently_deactivated()
{
    if (!m_flags.test(st_recently_deactivated))
        return;
    m_check_count = 0;
    m_flags.set(st_recently_deactivated, FALSE);
    ph_world->RemoveFromRecentlyDisabled(PH_OBJECT_I(this));
}
void CPHObject::check_recently_deactivated()
{
    if (m_check_count == 0)
    {
        ClearRecentlyDeactivated();
        remove_from_recently_deactivated();
    }
    else
        m_check_count--;
}
void CPHObject::spatial_move()
{
    get_spatial_params();
    ISpatial::spatial_move();
    m_flags.set(st_dirty, TRUE);
}

void CPHObject::Collide()
{
    if (m_flags.test(fl_ray_motions))
    {
        CPHMoveStorage* tracers = MoveStorage();
        CPHMoveStorage::iterator I = tracers->begin(), E = tracers->end();
        for (; E != I; ++I)
        {
            const Fvector *from = 0, *to = 0;
            Fvector dir;
            I.Positions(from, to);
            if (from->x == -dInfinity)
                continue;
            dir.sub(*to, *from);
            float magnitude = dir.magnitude();
            if (magnitude < EPS)
                continue;
            dir.mul(1.f / magnitude);
            g_SpatialSpacePhysic->q_ray(ph_world->r_spatial, 0, STYPE_PHYSIC, *from, dir, magnitude); //|ISpatial_DB::O_ONLYFIRST
#ifdef DEBUG
            if (ph_dbg_draw_mask.test(phDbgDrawRayMotions))
            {
                DBG_OpenCashedDraw();
                DBG_DrawLine(*from, Fvector().add(*from, Fvector().mul(dir, magnitude)), color_xrgb(0, 255, 0));
                DBG_ClosedCashedDraw(30000);
            }

#endif
            qResultVec& result = ph_world->r_spatial;
            qResultIt i = result.begin(), e = result.end();
            for (; i != e; ++i)
            {
                CPHObject* obj2 = static_cast<CPHObject*>(*i);
                if (obj2 == this || !obj2->m_flags.test(st_dirty))
                    continue;
                dGeomID motion_ray = ph_world->GetMotionRayGeom();
                dGeomRayMotionSetGeom(motion_ray, I.dGeom());
                dGeomRayMotionsSet(motion_ray, (const dReal*)from, (const dReal*)&dir, magnitude);
                NearCallback(this, obj2, motion_ray, obj2->dSpacedGeom());
            }
        }
    }
    CollideDynamics();
    ///////////////////////////////
    if (CPHCollideValidator::DoCollideStatic(*this))
        CollideStatic(dSpacedGeom(), this);
    m_flags.set(st_dirty, FALSE);
}
void CPHObject::CollideDynamics()
{
    g_SpatialSpacePhysic->q_box(ph_world->r_spatial, 0, STYPE_PHYSIC, spatial.sphere.P, AABB);
    qResultVec& result = ph_world->r_spatial;

    // --- NPC-vs-NPC crowd gate (ph_npc_crowd_gate) --------------------------
    // Stock ODE lets every character capsule rigid-collide every other one; in a
    // hot-spot that is the near-quadratic driver behind the crowd blow-up. When
    // the gate is armed we drop character<->character contacts for this capsule
    // once its local character-neighbour count reaches the threshold: sparse
    // groups still push apart, dense crowds stop paying the O(n^2) contact cost.
    // The neighbour list is already in hand from the broadphase query above, so
    // the density probe is free. Character-vs-static and character-vs-prop
    // contacts are untouched.
    bool suppress_char_char = false;
    if (ph_npc_crowd_gate > 0 && CPHCollideValidator::IsCharacter(*this))
    {
        int char_neighbours = 0;
        for (auto i = result.begin(), e = result.end(); i != e; ++i)
        {
            CPHObject* n = static_cast<CPHObject*>(*i);
            if (n != this && CPHCollideValidator::IsCharacter(*n))
                ++char_neighbours;
        }
        suppress_char_char = (char_neighbours >= ph_npc_crowd_gate);
    }
    // ------------------------------------------------------------------------

    auto i = result.begin(), e = result.end();
    for (; i != e; ++i)
    {
        CPHObject* obj2 = static_cast<CPHObject*>(*i);

        if (obj2 == this || !obj2->m_flags.test(st_dirty))
            continue;

        if (suppress_char_char && CPHCollideValidator::IsCharacter(*obj2))
            continue;

        if (CPHCollideValidator::DoCollide(*this, *obj2))
            NearCallback(this, obj2, dSpacedGeom(), obj2->dSpacedGeom());
    }
}
void CPHObject::reinit_single()
{
    IslandReinit();
    qResultVec& result = ph_world->r_spatial;
    qResultIt i = result.begin(), e = result.end();
    for (; i != e; ++i)
    {
        CPHObject* obj = static_cast<CPHObject*>(*i);
        obj->IslandReinit();
    }
    dJointGroupEmpty(ContactGroup);
    ContactFeedBacks.empty();
    ContactEffectors.empty();
}

void CPHObject::step_prediction(float time)
{
    // general idea:
    // perform normal step by time as local as possible for this object then return world to
    // the pervious state
}
bool CPHObject::step_single(dReal step)
{
    CollideDynamics();
    bool ret = !m_island.IsObjGroun();
    if (ret)
    {
        // PhTune							(step);
        IslandStep(step);
        reinit_single();
        // PhDataUpdate					(step);
        spatial_move();
        CollideDynamics();
        ret = !m_island.IsObjGroun();
    }
    reinit_single();
    return ret;
}

void CPHObject::step(float time) // it is still not a true step for object because it collide the object only not subsequent collision is doing
{
    ph_world->r_spatial.clear();
    reinit_single();
    Collide();
    IslandStep(time);
    reinit_single();
}

bool CPHObject::DoCollideObj()
{
    CollideDynamics();
    bool ret = m_island.IsObjGroun();
    reinit_single();
    return ret;
}

// ⚠⚠⚠These two used to be fatal when the flag was already in the state being
// asked for. The assert guards a real invariant -- the world's frozen set and
// the per-object flag must agree -- but it guards it by KILLING THE PROCESS,
// and the imbalance it catches is recoverable: the object ends up in exactly
// the requested state either way.
//
// ⚠Measured 28-07 under a hundred fighting NPCs: with corpses dropping weapons
// and ragdolls activating, something puts the two out of step, and then the
// next caller of ph_world->Freeze() dies -- and there are many, including the
// ACTOR's own movement update. Three different call sites died in three runs,
// none of them the one that caused it. A dedicated server cannot be taken down
// by bookkeeping that it can simply correct.
//
// Said once so the imbalance stays visible: this treats the symptom, and the
// leak that causes it is still unaccounted for.
namespace
{
void warn_freeze_imbalance(const char* what)
{
    static bool s_said = false;
    if (!s_said)
    {
        s_said = true;
        Msg("! [physics] %s on an object already in that state -- the world's frozen set and the object flag disagree. Corrected, not fatal; the cause is still open.", what);
    }
}
} // namespace

void CPHObject::FreezeContent()
{
    if (m_flags.test(st_freezed))
    {
        warn_freeze_imbalance("FreezeContent");
        return;
    }
    m_flags.set(st_freezed, TRUE);
    m_flags.set(st_activated, FALSE);
    vis_update_deactivate();
}
void CPHObject::UnFreezeContent()
{
    if (!m_flags.test(st_freezed))
    {
        warn_freeze_imbalance("UnFreezeContent");
        return;
    }
    m_flags.set(st_freezed, FALSE);
    m_flags.set(st_activated, TRUE);
    vis_update_activate();
}

void CPHObject::spatial_register()
{
    get_spatial_params();
    ISpatial::spatial_register();
    m_flags.set(st_dirty, TRUE);
}

void CPHObject::collision_disable() { ISpatial::spatial_unregister(); }
void CPHObject::collision_enable() { ISpatial::spatial_register(); }

void CPHObject::Freeze()
{
    if (!m_flags.test(st_activated))
        return;
    ph_world->RemoveObject(this);
    ph_world->AddFreezedObject(this);
    FreezeContent();
    m_solo_frozen = true;
}

void CPHObject::UnFreeze()
{
    if (!m_flags.test(st_freezed))
        return;
    m_solo_frozen = false;
    UnFreezeContent();
    ph_world->RemoveFreezedObject(this);
    ph_world->AddObject(this);
    m_active_since = ph_world->StepsNum();
}

CPHUpdateObject::CPHUpdateObject() { b_activated = false; }

void CPHUpdateObject::Activate()
{
    if (b_activated)
        return;
    ph_world->AddUpdateObject(this);
    b_activated = true;
}

void CPHUpdateObject::Deactivate()
{
    if (!b_activated)
        return;
    ph_world->RemoveUpdateObject(this);
    b_activated = false;
}

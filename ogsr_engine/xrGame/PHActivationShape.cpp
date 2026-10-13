#include "StdAfx.h"
#include "PHObject.h"
#include "Physics.h"
#include "MathUtils.h"
#include "PHValideValues.h"
#include "PHActivationShape.h"
#include "ExtendedGeom.h"
#include "SpaceUtils.h"
#include "MathUtils.h"
#include "../xr_3da/GameMtlLib.h"
#include "Level.h"
#include "PHWorld.h"
#include "ode/src/util.h"

#ifdef DEBUG
#include "PHDebug.h"
#endif // DEBUG

#include "PHDynamicData.h"
#include "PHSynchronize.h"
#include "PHNetState.h"
#include "PhysicsShellHolder.h" // [ph activate] the name of the object
static float max_depth = 0.f;
static float friction_factor = 0.f;
static float cfm = 1.e-10f;
static float erp = 1.f;
int g_ph_activate_stall = 15; // ph_activate_stall: steps without the depth going down before the search gives up; 0 = the stock 10 x 15
#ifdef DEBUG
#define CHECK_POS(pos, msg, br) \
    if (!valid_pos(pos, phBoundaries)) \
    { \
        Msg("pos:%f,%f,%f", pos.x, pos.y, pos.z); \
        Msg(msg); \
        VERIFY(!br); \
    }

#else
#define CHECK_POS(pos, msg, br)
#endif
void ActivateTestDepthCallback(bool& do_colide, bool bo1, dContact& c, SGameMtl* material_1, SGameMtl* material_2)
{
    if (do_colide && !material_1->Flags.test(SGameMtl::flPassable) && !material_2->Flags.test(SGameMtl::flPassable))
    {
        float& depth = c.geom.depth;
        float test_depth = depth;
        save_max(max_depth, test_depth);
        c.surface.mu *= friction_factor;

        c.surface.soft_cfm = cfm;
        c.surface.soft_erp = erp;
    }
}
void StaticEnvironment(bool& do_colide, bool bo1, dContact& c, SGameMtl* material_1, SGameMtl* material_2)
{
    dJointID contact_joint = dJointCreateContact(0, ContactGroup, &c);

    if (bo1)
    {
        ((CPHActivationShape*)(retrieveGeomUserData(c.geom.g1)->callback_data))->DActiveIsland()->ConnectJoint(contact_joint);
        dJointAttach(contact_joint, dGeomGetBody(c.geom.g1), 0);
    }
    else
    {
        ((CPHActivationShape*)(retrieveGeomUserData(c.geom.g2)->callback_data))->DActiveIsland()->ConnectJoint(contact_joint);
        dJointAttach(contact_joint, 0, dGeomGetBody(c.geom.g2));
    }
    do_colide = false;
}
void GetMaxDepthCallback(bool& do_colide, bool bo1, dContact& c, SGameMtl* material_1, SGameMtl* material_2)
{
    if (do_colide && !material_1->Flags.test(SGameMtl::flPassable) && !material_2->Flags.test(SGameMtl::flPassable))
    {
        float& depth = c.geom.depth;
        float test_depth = depth;
        // save_max(max_depth,test_depth);
        max_depth += test_depth;
    }
    // do_colide=false;
}

void RestoreVelocityState(V_PH_WORLD_STATE& state)
{
    V_PH_WORLD_STATE::iterator i = state.begin(), e = state.end();
    for (; e != i; ++i)
    {
        CPHSynchronize& sync = *i->first;
        SPHNetState& old_s = i->second;
        SPHNetState new_s;
        sync.get_State(new_s);
        new_s.angular_vel.set(old_s.angular_vel);
        new_s.linear_vel.set(old_s.linear_vel);
        new_s.enabled = old_s.enabled;
        sync.set_State(new_s);
    }
}

CPHActivationShape::CPHActivationShape()
{
    m_geom = NULL;
    m_body = NULL;
    m_flags.zero();
    m_flags.set(flFixedRotation, TRUE);
}
CPHActivationShape::~CPHActivationShape() { VERIFY(!m_body && !m_geom); }
void CPHActivationShape::Create(const Fvector start_pos, const Fvector start_size, CPhysicsShellHolder* ref_obj, EType _type /*=etBox*/, u16 flags)
{
    VERIFY(ref_obj);
    m_body = dBodyCreate(0);
    dMass m;
    dMassSetSphere(&m, 1.f, 100000.f);
    dMassAdjust(&m, 1.f);
    dBodySetMass(m_body, &m);
    switch (_type)
    {
    case etBox: m_geom = dCreateBox(0, start_size.x, start_size.y, start_size.z); break;

    case etSphere: m_geom = dCreateSphere(0, start_size.x); break;
    };

    dGeomCreateUserData(m_geom);
    dGeomUserDataSetObjectContactCallback(m_geom, ActivateTestDepthCallback);
    dGeomUserDataSetPhysicsRefObject(m_geom, ref_obj);
    dGeomSetBody(m_geom, m_body);
    dBodySetPosition(m_body, start_pos.x, start_pos.y, start_pos.z);
    Island().AddBody(m_body);
    dBodyEnable(m_body);
    m_safe_state.create(m_body);
    spatial_register();
    m_flags.set(flags, TRUE);
}
void CPHActivationShape::Destroy()
{
    VERIFY(m_geom && m_body);
    spatial_unregister();
    CPHObject::deactivate();
    dGeomDestroyUserData(m_geom);
    dGeomDestroy(m_geom);
    m_geom = NULL;
    dBodyDestroy(m_body);
    m_body = NULL;
}
bool CPHActivationShape::Activate(const Fvector need_size, u16 steps, float max_displacement, float max_rotation, bool un_freeze_later /*	=false*/)
{
#ifdef DEBUG
    if (ph_dbg_draw_mask.test(phDbgDrawDeathActivationBox))
    {
        DBG_OpenCashedDraw();
        Fmatrix M;
        PHDynamicData::DMXPStoFMX(dBodyGetRotation(m_body), dBodyGetPosition(m_body), M);
        Fvector v;
        dGeomBoxGetLengths(m_geom, cast_fp(v));
        v.mul(0.5f);
        DBG_DrawOBB(M, v, color_xrgb(0, 255, 0));
    }
#endif
    VERIFY(m_geom && m_body);
    const u64 prof_t0 = CPU::QPC(); // [ph activate]
    CPHObject::activate();
    ph_world_freeze_scope freeze; // unfreezes on any path out of this function
    UnFreeze();
    const u32 objects_touch = ph_world->ObjectsNumber(); // should be 1: the frozen world and this box
    max_depth = 0.f;

    dGeomUserDataSetObjectContactCallback(m_geom, GetMaxDepthCallback);
    // ph_world->Step();
    ph_world->StepTouch();
    const float max_depth_touch = max_depth; // [ph activate] the summed depth it starts from
    const u64 prof_touch = CPU::QPC();
    u16 num_it = 15;
    float fnum_it = float(num_it);
    float fnum_steps = float(steps);
    float fnum_steps_r = 1.f / fnum_steps;
    float resolve_depth = 0.01f;
    float max_vel = max_depth / fnum_it * fnum_steps_r / fixed_step;
    float limit_l_vel = _max(_max(need_size.x, need_size.y), need_size.z) / fnum_it * fnum_steps_r / fixed_step;
    if (limit_l_vel > default_l_limit)
        limit_l_vel = default_l_limit;
    if (max_vel > limit_l_vel)
        max_vel = limit_l_vel;

    float max_a_vel = max_rotation / fnum_it * fnum_steps_r / fixed_step;
    if (max_a_vel > default_w_limit)
        max_a_vel = default_w_limit;
    // ph_world->CutVelocity(0.f,0.f);
    dGeomUserDataSetCallbackData(m_geom, this);
    dGeomUserDataSetObjectContactCallback(m_geom, ActivateTestDepthCallback);
    if (m_flags.test(flStaticEnvironment))
        dGeomUserDataAddObjectContactCallback(m_geom, StaticEnvironment);
    max_depth = 0.f;

    Fvector from_size;
    Fvector step_size, size;
    dGeomBoxGetLengths(m_geom, cast_fp(from_size));
    step_size.sub(need_size, from_size);
    step_size.mul(fnum_steps_r);
    size.set(from_size);
    bool ret = false;
    V_PH_WORLD_STATE temp_state;
    ph_world->GetState(temp_state);
    // [ph activate]: 📏08-10-2026 Switch v223/v229 a weapon falling out of a dead
    // NPC's hands = ph.correct_spawn 25-39 ms; the sampler puts it all in the
    // collisions of these steps (up to 10 x 15 world steps). The stall cut ends
    // the search once the depth has not gone down by a tenth for ph_activate_stall
    // steps: a box wedged where it cannot get out (a corpse, a wall) used to run
    // all 150 and end in the same place, with ret false.
    const float depth0 = max_depth_touch;
    u32 nsteps = 0, max_objects = 0;
    bool stalled = false; // this size step gave up
    u32 stalls = 0;
    for (int m = 0; steps > m; ++m)
    {
        // float param =fnum_steps_r*(1+m);
        // InterpolateBox(id,param);
        size.add(step_size);
        dGeomBoxSetLengths(m_geom, size.x, size.y, size.z);
        u16 attempts = 10;
        float best_depth = flt_max;
        u32 since_best = 0;
        stalled = false;
        do
        {
            ret = false;
            for (int i = 0; num_it > i; ++i)
            {
                max_depth = 0.f;
                ph_world->Step();
                ++nsteps;
                max_objects = std::max(max_objects, u32(ph_world->ObjectsNumber()));
                CHECK_POS(Position(), "pos after ph_world->Step()", false);
                ph_world->CutVelocity(max_vel, max_a_vel);
                CHECK_POS(Position(), "pos after CutVelocity", true);
                // if(m==0&&i==0)ph_world->GetState(temp_state);
                if (max_depth < resolve_depth)
                {
                    ret = true;
                    break;
                }
                if (max_depth < best_depth * 0.9f)
                {
                    best_depth = max_depth;
                    since_best = 0;
                }
                else if (g_ph_activate_stall > 0 && ++since_best >= u32(g_ph_activate_stall))
                {
                    stalled = true;
                    break;
                }
            }
            attempts--;
        } while (!ret && !stalled && attempts > 0);
        stalls += stalled;
#ifdef DEBUG
        Msg("correction attempts %d", 10 - attempts);
#endif
    }
    {
        const double ms = 1000.0 * double(CPU::QPC() - prof_t0) / double(CPU::QPCFreq());
        static u32 s_logs = 0;
        if (ms >= 3.0 && s_logs < 64)
        {
            ++s_logs;
            const CPhysicsShellHolder* ref = retrieveGeomUserData(m_geom)->ph_ref_object;
            const double touch_ms = 1000.0 * double(prof_touch - prof_t0) / double(CPU::QPCFreq());
            Msg("~ [ph activate] '%s' %.1f ms: touch %.1f ms (objects %u), %u steps (%.2f ms each), objects up to %u, depth %.3f -> %.3f%s, box %.2f x %.2f x %.2f",
                ref ? ref->cName().c_str() : "?", ms, touch_ms, objects_touch, nsteps, nsteps ? (ms - touch_ms) / nsteps : 0.0, max_objects, depth0, max_depth,
                ret ? "" : stalls ? " STALLED (cut)" : " not resolved", need_size.x, need_size.y, need_size.z);
        }
    }
    RestoreVelocityState(temp_state);
    CHECK_POS(Position(), "pos after RestoreVelocityState(temp_state);", true);
    if (un_freeze_later)
        freeze.release(); // the caller unfreezes; not ours to end
    else
        freeze.unfreeze();
#ifdef DEBUG
    if (ph_dbg_draw_mask.test(phDbgDrawDeathActivationBox))
    {
        DBG_OpenCashedDraw();
        Fmatrix M;
        PHDynamicData::DMXPStoFMX(dBodyGetRotation(m_body), dBodyGetPosition(m_body), M);
        Fvector v;
        v.set(need_size);
        v.mul(0.5f);
        DBG_DrawOBB(M, v, color_xrgb(0, 255, 255));
        DBG_ClosedCashedDraw(30000);
    }
#endif
    return ret;
}
const Fvector& CPHActivationShape::Position() { return cast_fv(dBodyGetPosition(m_body)); }
void CPHActivationShape::Size(Fvector& size) { dGeomBoxGetLengths(m_geom, cast_fp(size)); }

void CPHActivationShape::PhDataUpdate(dReal step) { m_safe_state.new_state(m_body); }
void CPHActivationShape::PhTune(dReal step) {}
dGeomID CPHActivationShape::dSpacedGeom() { return m_geom; }
void CPHActivationShape::get_spatial_params() { spatialParsFromDGeom(m_geom, spatial.sphere.P, AABB, spatial.sphere.R); }

void CPHActivationShape::InitContact(dContact* c, bool& do_collide, u16, u16) {}

void CPHActivationShape::CutVelocity(float l_limit, float /*a_limit*/)
{
    dVector3 limitedl, diffl;
    if (dVectorLimit(dBodyGetLinearVel(m_body), l_limit, limitedl))
    {
        dVectorSub(diffl, limitedl, dBodyGetLinearVel(m_body));
        dBodySetLinearVel(m_body, diffl[0], diffl[1], diffl[2]);
        dBodySetAngularVel(m_body, 0.f, 0.f, 0.f);
        dxStepBody(m_body, fixed_step);
        dBodySetLinearVel(m_body, limitedl[0], limitedl[1], limitedl[2]);
    }
}

void CPHActivationShape::set_rotation(const Fmatrix& sof)
{
    dMatrix3 rot;
    PHDynamicData::FMXtoDMX(sof, rot);
    dBodySetRotation(ODEBody(), rot);
}
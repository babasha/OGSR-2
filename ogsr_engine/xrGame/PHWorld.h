#pragma once

#include "Physics.h"

// refs
struct SGameMtlPair;
class CPHCommander;
class CPHCondition;
class CPHAction;
struct SPHNetState;
class CPHSynchronize;
typedef xr_vector<std::pair<CPHSynchronize*, SPHNetState>> V_PH_WORLD_STATE;
class CPHMesh
{
    dGeomID Geom;

public:
    dGeomID GetGeom() { return Geom; }
    void Create(dSpaceID space, dWorldID world);
    void Destroy();
};

#define PHWORLD_SOUND_CACHE_SIZE 8

////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

class CPHWorld : public pureFrame
{
    double m_start_time{};
    u32 m_delay;
    u32 m_previous_delay;
    u32 m_reduce_delay;
    u32 m_update_delay_count;
    bool b_world_freezed;
    u32 m_freeze_depth{}; // see Freeze()
    u32 m_freeze_frame{}; // the frame the outermost Freeze() was taken in
    bool b_processing;
    u32 m_processing_frame; // the frame b_processing was raised in -- see Processing()
    bool b_exist;
    xr_vector<CPhysicsShell*> m_deferred_deactivate; // see DeferDeactivate
    static const u32 update_delay = 1;
    ///	dSpaceID					Space														;

    CPHMesh Mesh{};
    PH_OBJECT_STORAGE m_objects;
    PH_OBJECT_STORAGE m_freezed_objects;
    PH_OBJECT_STORAGE m_recently_disabled_objects;
    PH_UPDATE_OBJECT_STORAGE m_update_objects;
    PH_UPDATE_OBJECT_STORAGE m_freezed_update_objects;
    dGeomID m_motion_ray{};
    CPHCommander* m_commander{};

public:
    xr_vector<ISpatial*> r_spatial;

public:
    u64 m_steps_num;
    double m_frame_sum;
    dReal m_previous_frame_time;
    bool b_frame_mark;
    dReal m_frame_time;
    float m_update_time{};
    u16 disable_count;
    float m_gravity;
    CPHWorld();
    virtual ~CPHWorld(){};

    // IC	dSpaceID					GetSpace						()			{return Space;}	;
    IC bool Exist() { return b_exist; }
    void Create();
    void SetGravity(float g);
    IC float Gravity() { return m_gravity; }
    void AddObject(CPHObject* object);
    void AddUpdateObject(CPHUpdateObject* object);
    void AddRecentlyDisabled(CPHObject* object);
    void RemoveFromRecentlyDisabled(PH_OBJECT_I i);
    void RemoveObject(PH_OBJECT_I i);
    void RemoveUpdateObject(PH_UPDATE_OBJECT_I i);
    dGeomID GetMeshGeom() { return Mesh.GetGeom(); }
    IC dGeomID GetMotionRayGeom() { return m_motion_ray; }
    void static SetStep(dReal s);
    void Destroy();
    IC float FrameTime(bool frame_mark) { return b_frame_mark == frame_mark ? m_frame_time : m_previous_frame_time; }

    void FrameStep(dReal step = 0.025f);
    // ⭐⭐⭐FrameStep, with whatever faults inside it given a name.
    //
    // ⚠⚠This project is built with no /EH switch at all, which means the
    // catch(...) that CRegistrator::Process wraps every frame member in (pure.h)
    // catches STRUCTURED exceptions too -- an access violation inside a step is
    // indistinguishable, from up there, from a thrown object, and both vanish
    // into the same one-line doubling counter. Measured 28-07: 4096 of them in a
    // two-minute run, ~100 a second, i.e. the physics world skipped on most
    // frames, and the only trace was "member #3 threw [non-std]". The cause was a
    // one-line dereference in a car.
    //
    // Handling it here does not save the frame -- it is lost either way -- but it
    // is lost with a stack trace, and OnFrame gets to run its backstops instead
    // of being unwound out of.
    void FrameStepGuarded(dReal step);
    void Step();
    void far_freeze_scan(); // ph_far_freeze (PHWorld.cpp)
    void settle_scan();     // ph_settle (PHWorld.cpp)
    void StepTouch();
    void CutVelocity(float l_limit, float a_limit);
    void GetState(V_PH_WORLD_STATE& state);

    // ⭐⭐⭐Freezing the world is a SCOPE, not a state: half a dozen callers take
    // the whole world out of the stepping lists, hand-step it a few times to
    // settle a body into a place, and put it back. Nobody holds it across a
    // frame.
    //
    // ⚠These used to be R_ASSERT2(!b_world_freezed, "already freezed!!!") and its
    // mirror -- fatal on an imbalance. Two ways to get one, both real:
    //
    //  - a scope ABANDONED: the hand-stepping runs game code, game code faults,
    //    and the unfreeze at the bottom of the function never happens. The world
    //    stays frozen for the life of the process and the fatal lands on the NEXT
    //    caller of Freeze() -- which is never the one that broke it. Measured
    //    28-07: the run died in CPHShell::Deactivate's freeze, 72 seconds after a
    //    car had begun faulting inside a step.
    //  - a scope NESTED: hand-stepping reaches a contact callback, which kills a
    //    creature, whose ragdoll wants a freeze of its own.
    //
    // Both are answerable rather than fatal. Nesting is what a depth counter is
    // for -- the inner scope wants exactly what the outer one already did.
    // Abandonment cannot be answered from here at all, so OnFrame ends it: a
    // freeze still open at the top of a frame was abandoned by definition.
    void Freeze();
    void UnFreeze();
    IC u32 FreezeDepth() const { return m_freeze_depth; } // diagnosis only
    void AddFreezedObject(CPHObject* obj);
    void RemoveFreezedObject(PH_OBJECT_I i);
    bool IsFreezed();

    // ⭐⭐⭐Tearing physics down is only safe between steps, and the callers that
    // want to do it -- an item changing hands, a creature dying -- have no idea
    // what the physics world is doing. So they ask here instead of asserting.
    //
    // ⚠Measured 28-07 under a hundred fighting NPCs: this is not theoretical.
    // Every third run died on one of the three "can not deactivate physics X
    // during physics processing" fatals, and the probe caught the world genuinely
    // mid-step (`step frame 3859 vs now 3859`) -- so the flag was right and the
    // CALLERS were wrong to insist.
    //
    // Drained at the top of OnFrame, where no step can be running. Anything
    // destroyed before its turn takes itself off the list.
    void DeferDeactivate(CPhysicsShell* shell);
    void ForgetDeferred(CPhysicsShell* shell);
    void DrainDeferred();
    // ⭐⭐⭐"A step is running RIGHT NOW", and it expires by itself.
    //
    // ⚠A bare flag was not survivable. Half a dozen fatals across the engine
    // read it ("can not deactivate physics X during physics processing"), and a
    // single leak -- a frame abandoned mid-step during level load was enough --
    // latched it TRUE for the life of the process. Nothing notices until the
    // first creature dies or a weapon changes hands, and then the crash points
    // at the death, nowhere near the load that broke it. Measured 27-07: three
    // different fatals in three runs, all with a stuck flag behind them.
    //
    // A step never spans a frame, so a raise from an earlier frame is a leak by
    // definition and is ignored. The RAII guard in FrameStep still clears it
    // normally; this is the backstop that makes a missed clear harmless.
    IC bool Processing() { return b_processing && m_processing_frame == Device.dwFrame; }
    IC u32 ProcessingFrame() const { return m_processing_frame; } // diagnosis only
    u16 ObjectsNumber();

    inline u64& StepsNum() { return m_steps_num; }

    void NetRelcase(CPhysicsShell* s);
    void AddCall(CPHCondition* c, CPHAction* a);
    virtual void OnFrame();

    DECLARE_SCRIPT_REGISTER_FUNCTION
};
add_to_type_list(CPHWorld)
#undef script_type_list
#define script_type_list save_type_list(CPHWorld)

// ⭐⭐A world freeze that ends when the scope does.
//
// Every caller writes the same shape by hand -- Freeze(), settle something with
// a few manual steps, UnFreeze() -- and several of them have early returns in
// between, so the unfreeze is already duplicated two and three times per
// function. One more `return` added later and the world stays frozen.
//
// ⚠⚠What this does NOT do, so nobody counts on it: it does not save the
// abandoned-scope case that actually killed the server. xrGame is compiled with
// no /EH switch at all, so the compiler emits no unwind code and destructors do
// NOT run when a fault or a throw leaves this function -- see the measurement on
// CPHWorld::FrameStepGuarded. That case is caught one level up, by the check at
// the top of CPHWorld::OnFrame. This guard covers the ordinary paths, and would
// cover the rest if the project ever gains an exception model.
//
// ⚠Also deliberately NOT a replacement for the explicit UnFreeze() calls: they
// stay where they are, because the statements around them are order-sensitive
// and moving the unfreeze to the closing brace would quietly change when the
// world comes back.
class ph_world_freeze_scope
{
    bool m_held;

public:
    ph_world_freeze_scope();
    ~ph_world_freeze_scope();
    // The freeze ended normally, here, in the order the caller wanted.
    void unfreeze();
    // Someone else owns the frozen world now; do not end it at the brace.
    void release() { m_held = false; }

    ph_world_freeze_scope(const ph_world_freeze_scope&) = delete;
    ph_world_freeze_scope& operator=(const ph_world_freeze_scope&) = delete;
};

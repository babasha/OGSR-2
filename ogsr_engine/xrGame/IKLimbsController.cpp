#include "StdAfx.h"

#include "IKLimbsController.h"
#include "ik/IKLimb.h"
#include "PhysicsShellHolder.h"
#include "ik_anim_state.h"
#include "CharacterPhysicsSupport.h"
#include "../xr_3da/motion.h"
#include "../xrCore/pinned_worker.h"   // ph_ik_mt: the legs on cores 1 and 2
#include <atomic>

// ⛔DEFAULT 0: 📏07-10-2026 Switch v225 crashed in the batch -- a worker in CIKLimb::Update ->
// step_predict jumped to a garbage address (Alignment Fault, PC=LR=0x3f155ddb). foot_matrix_predict
// advances the skeleton's LIVE blends and swaps its bone callbacks for the duration of the
// prediction, then restores them: single-threaded design. In a parallel batch another creature's
// foot rays read this skeleton mid-swap. Needs the prediction on copies before this can be on.
int g_ik_mt = 0;   // ph_ik_mt -- see CIKLimbsController::FlushBatch
namespace
{
xr_vector<CIKLimbsController*> s_ikBatch;   // queued by Update(), run by FlushBatch()
}

CIKLimbsController::CIKLimbsController()
    : m_legs_blend(nullptr), m_object(nullptr)
{}

void CIKLimbsController::Create(CGameObject* O)
{
    VERIFY(O);
    m_legs_blend = 0;

    IKinematics* K = smart_cast<IKinematics*>(O->Visual());
    m_object = O;
    VERIFY(K);
    u16 sz = 2;
    if (K->LL_UserData() && K->LL_UserData()->section_exist("ik"))
        sz = K->LL_UserData()->r_u16("ik", "num_limbs");
    VERIFY(sz <= max_size);

    _bone_chains.reserve(sz);
    for (u16 i = 0; sz > i; ++i)
        LimbSetup();

    bool already_has_callbacks = !O->visual_callbacks().empty();
    O->add_visual_callback(IKVisualCallback);
    if (already_has_callbacks)
        std::swap(*(O->visual_callbacks().begin()), *(O->visual_callbacks().end() - 1));
    _pose_extrapolation.init(O->XFORM());
}

void CIKLimbsController::LimbSetup()
{
    IKinematicsAnimated* skeleton_animated = m_object->Visual()->dcast_PKinematicsAnimated();

    _bone_chains.emplace_back().Create((u16)_bone_chains.size() - 1, skeleton_animated, true);
}

void CIKLimbsController::LimbCalculate(SCalculateData& cd)
{
    cd.do_collide = m_legs_blend && !cd.m_limb->KinematicsAnimated()->LL_GetMotionDef(m_legs_blend->motionID)->marks.empty(); // m_legs_blend->;
    cd.m_limb->ApplyState(cd);
}

void CIKLimbsController::LimbUpdate(CIKLimb& L)
{
    VERIFY(m_object->Visual()->dcast_PKinematicsAnimated());
    L.Update(m_object, m_legs_blend, _pose_extrapolation);
}

IC void update_blend(CBlend*& b)
{
    if (b && CBlend::eFREE_SLOT == b->blend_state())
        b = 0;
}

void y_shift_bones(IKinematics* K, float shift)
{
    u16 bc = K->LL_BoneCount();
    for (u16 i = 0; bc > i; ++i)
        K->LL_GetTransform(i).c.y += shift;
}
float CIKLimbsController::LegLengthShiftLimit(float current_shift, const SCalculateData cd[max_size])
{
    float shift_down = -phInfinity;
    const u16 sz = (u16)_bone_chains.size();
    for (u16 j = 0; sz > j; ++j)
        if (cd[j].state.foot_step)
        {
            float s_down = cd[j].m_limb->ObjShiftDown(current_shift, cd[j]);
            if (shift_down < s_down)
                shift_down = s_down;
        }
    return shift_down;
}
static const float static_shift_object_speed = .2f;
float CIKLimbsController::StaticObjectShift(const SCalculateData cd[max_size])
{
    const float current_shift = _object_shift.shift();

    u16 cnt = 0;
    float shift_up = 0;
    const u16 sz = (u16)_bone_chains.size();
    for (u16 j = 0; sz > j; ++j)
        if (cd[j].state.foot_step)
        {
            float s_up = cd[j].cl_shift.y + current_shift;
            if (0.f < s_up)
            {
                shift_up += s_up;
                ++cnt;
            }
        }
    if (0 < cnt)
        shift_up /= cnt;
    float shift_down = LegLengthShiftLimit(current_shift, cd);
    float shift = 0;
    if (shift_down > 0.f)
        shift = -shift_down;
    else if (-shift_down < shift_up)
        shift = -shift_down;
    else
        shift = shift_up;
    VERIFY(_valid(shift));
    _object_shift.set_taget(shift, _abs(current_shift - shift) / static_shift_object_speed);
    return shift;
}
static float doun_shift_to_correct = 0.3f;
static float doun_shift_correct = 0.1f;
bool CIKLimbsController::PredictObjectShift(const SCalculateData cd[max_size])
{
    float predict_time_shift_down = FLT_MAX;
    float predict_time_shift_up = FLT_MAX;
    float predict_shift_down = 0.f;
    // float predict_shift_up = 0.f;
    bool shift_down = false;
    bool shift_up = false;
    const u16 sz = (u16)_bone_chains.size();
    float current_shift = _object_shift.shift();
    for (u16 j = 0; sz > j; ++j)
        if (!cd[j].state.foot_step)
        {
            float time = cd[j].m_limb->time_to_footstep();

            float lshift = cd[j].m_limb->footstep_shift();
            if (lshift < 0.f)
            {
                if (time < predict_time_shift_down)
                {
                    predict_time_shift_down = time;
                    predict_shift_down = lshift;
                    shift_down = true;
                }
            }
            else if (current_shift < -doun_shift_to_correct && time < predict_time_shift_up)
            {
                predict_time_shift_up = time;

                shift_up = true;
            }
        }
    float predict_shift = 0;
    float predict_time_shift = FLT_MAX;

    if (shift_down)
    {
        predict_shift = predict_shift_down;
        predict_time_shift = predict_time_shift_down;
    }
    else if (shift_up)
    {
        predict_shift = 0;
        predict_time_shift = predict_time_shift_up;
    }
    else
        return false;
    //{
    //	predict_shift = 0;
    //	predict_time_shift = Device.fTimeDelta;
    //}

    if (predict_time_shift < EPS_S)
        predict_time_shift = Device.fTimeDelta;
    _object_shift.set_taget(predict_shift, predict_time_shift);
    return true;
}

void CIKLimbsController::ObjectShift(float static_shift, const SCalculateData cd[max_size])
{
    u16 cnt_in_step = 0;
    const u16 sz = (u16)_bone_chains.size();
    for (u16 j = 0; sz > j; ++j)
        if (cd[j].m_limb->foot_step())
            ++cnt_in_step;

    VERIFY(smart_cast<CPhysicsShellHolder*>(m_object));
    // CCharacterPhysicsSupport *ch = sh->character_physics_support();
    _object_shift.freeze(!!Device.Paused()); // ch->is_interactive_motion() ||

    if (cnt_in_step != sz && PredictObjectShift(cd)) // cnt_in_step > 0 &&
        return;
    StaticObjectShift(cd);
}

void CIKLimbsController::ShiftObject(const SCalculateData cd[max_size])
{
    IKinematics* skeleton_animated = m_object->Visual()->dcast_PKinematics();
    VERIFY(skeleton_animated);
    //	u16 root = skeleton_animated->LL_GetBoneRoot( ) ;

    // CBoneData &BD=skeleton_animated->LL_GetData(root);

    const float y_shift = _object_shift.shift();
    const u16 bones_count = skeleton_animated->LL_BoneCount();
    for (u16 i = 0; i < bones_count; ++i)
        skeleton_animated->LL_GetTransform(i).c.y += y_shift;

    for (u16 i = 0; i < bones_count; ++i)
    {
        CBoneInstance& bi = skeleton_animated->LL_GetBoneInstance(i);
        if (bi.callback())
            bi.callback()(&bi);
        skeleton_animated->LL_GetTransform_R(i).c.y += y_shift;
    }
    //	skeleton_animated->LL_GetTransform(root).c.y += _object_shift.shift();
    //	skeleton_animated->Bone_Calculate(&BD, &Fidentity );
}

int ik_shift_object = 1;
void CIKLimbsController::Calculate()
{
    update_blend(m_legs_blend);

    Fmatrix& obj = m_object->XFORM();

    SCalculateData cd[max_size]{};

    xr_vector<CIKLimb>::iterator i, b = _bone_chains.begin(), e = _bone_chains.end();
    for (i = b; e != i; ++i)
    {
        cd[i - b] = SCalculateData(*i, obj);
        LimbCalculate(cd[i - b]);
    }

    IKinematics* K = m_object->Visual()->dcast_PKinematics();
    u16 root = K->LL_GetBoneRoot();
    CBoneInstance& root_bi = K->LL_GetBoneInstance(root);

    BOOL sv_root_cb_ovwr = root_bi.callback_overwrite();
    BoneCallback sv_root_cb = root_bi.callback();

    root_bi.set_callback(root_bi.callback_type(), 0, root_bi.callback_param(), TRUE);

    if (ik_shift_object) //&& ! m_object->animation_movement_controlled( )
    {
        ShiftObject(cd);
    }

    const u16 sz = (u16)_bone_chains.size();
    for (u16 j = 0; sz > j; ++j)
        cd[j].m_limb->SetGoal(cd[j]);

    for (u16 j = 0; sz > j; ++j)
    {
        cd[j].m_limb->SolveBones(cd[j]);
    }
    ObjectShift(0, cd);

    root_bi.set_callback(root_bi.callback_type(), sv_root_cb, root_bi.callback_param(), sv_root_cb_ovwr);
}

void CIKLimbsController::Destroy(CGameObject* O)
{
    O->remove_visual_callback(IKVisualCallback);
    // queued this frame and gone before the flush (ph_ik_mt): out of the batch
    s_ikBatch.erase(std::remove(s_ikBatch.begin(), s_ikBatch.end(), this), s_ikBatch.end());
    xr_vector<CIKLimb>::iterator i = _bone_chains.begin(), e = _bone_chains.end();
    for (; e != i; ++i)
        i->Destroy();
    _bone_chains.clear();
}

void _stdcall CIKLimbsController::IKVisualCallback(IKinematics* K)
{
    // if (Device.Paused())
    //	return;

    CGameObject* O = ((CGameObject*)K->GetUpdateCallbackParam());
    CPhysicsShellHolder* Sh = smart_cast<CPhysicsShellHolder*>(O);
    VERIFY(Sh);
    CIKLimbsController* ik = Sh->character_ik_controller();
    VERIFY(ik);
    // not updated this frame or the last (beyond ph_ik_dist, or out of view):
    // the collision it would solve against is stale -- plain animation instead
    if (Device.dwFrame - ik->m_updated_frame > 1)
        return;
    ik->Calculate();
}

void CIKLimbsController::PlayLegs(CBlend* b)
{
    m_legs_blend = b;
}
// ⭐ph_ik_mt (07-10-2026): the legs of every creature in one batch, spread over the
// main thread and workers pinned to cores 1 and 2. 📏Switch fight: CIKLimbsController
// 2.9% of the main thread (v223 sampler) -- foot rays (static + dynamic) and the solve.
// Safe because of WHEN it runs: after every object's UpdateCL (UpdateTracks, the
// blends, the pose -- all done in Update(), on the main thread), so nothing moves and
// no blend changes while the legs read them. Bone chains are evaluated on COPIES
// (BoneChain_Calculate); other skeletons are hit through CCF_Skeleton, which takes its
// shared lock and does not rebuild off the main thread; the spatial DB locks itself.
// A controller destroyed before the flush takes itself out of the batch (Destroy).
void CIKLimbsController::UpdateLimbs()
{
    xr_vector<CIKLimb>::iterator i = _bone_chains.begin(), e = _bone_chains.end();
    for (; e != i; ++i)
        LimbUpdate(*i);
}

void CIKLimbsController::FlushBatch()
{
    if (s_ikBatch.empty())
        return;
    static std::atomic<u32> s_next{0};
    static PinnedWorker* s_w1 = new PinnedWorker(1); // leaked: no join at exit
    static PinnedWorker* s_w2 = new PinnedWorker(2);
    auto drain = [] {
        const u32 n = (u32)s_ikBatch.size();
        for (u32 i = s_next.fetch_add(1, std::memory_order_relaxed); i < n; i = s_next.fetch_add(1, std::memory_order_relaxed))
            s_ikBatch[i]->UpdateLimbs();
    };
    s_next.store(0, std::memory_order_relaxed);
    const u32 n = (u32)s_ikBatch.size();
    if (n > 1)
        s_w1->Run(drain);
    if (n > 2)
        s_w2->Run(drain);
    drain();
    if (n > 1)
        s_w1->Wait();
    if (n > 2)
        s_w2->Wait();
    s_ikBatch.clear();
}

void CIKLimbsController::Update()
{
    IKinematicsAnimated* skeleton_animated = m_object->Visual()->dcast_PKinematicsAnimated();
    VERIFY(skeleton_animated);

    m_updated_frame = Device.dwFrame;
    skeleton_animated->UpdateTracks();
    update_blend(m_legs_blend);

    _pose_extrapolation.update(m_object->XFORM());
    if (g_ik_mt)
    {
        s_ikBatch.push_back(this);   // the legs in FlushBatch, with everybody else's
        return;
    }
    UpdateLimbs();
}

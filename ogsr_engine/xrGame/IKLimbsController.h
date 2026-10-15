#pragma once

#include "ik/IKLimb.h"
#include "pose_extrapolation.h"
#include "ik_object_shift.h"
class IKinematicsAnimated;
class CGameObject;
class CBlend;
struct SIKCrlCalcData;

class CIKLimbsController
{
private:
    static const u16 max_size = 4;

public:
    CIKLimbsController();
    void Create(CGameObject* O);
    void Destroy(CGameObject* O);

public:
    void PlayLegs(CBlend* b);
    void Update();
    float Shift() { return _object_shift.shift(); }

    // ph_ik_mt: Update() does the animation part here and queues the legs (rays + solve);
    // FlushBatch() runs every queued creature's legs at once, on the main thread and the
    // core-1/core-2 workers, after all objects are updated (CLevel::OnFrame_impl).
    static void FlushBatch();

private:
    void UpdateLimbs();

private:
    void Calculate();
    void LimbCalculate(SCalculateData& cd);
    void ShiftObject(const SCalculateData cd[max_size]);
    float StaticObjectShift(const SCalculateData cd[max_size]);
    float LegLengthShiftLimit(float current_shift, const SCalculateData cd[max_size]);
    bool PredictObjectShift(const SCalculateData cd[max_size]);
    void ObjectShift(float static_shift, const SCalculateData cd[max_size]);
    void LimbUpdate(CIKLimb& L);
    void LimbSetup();

private:
    static void __stdcall IKVisualCallback(IKinematics* K);

private:
    CBlend* m_legs_blend;
    CGameObject* m_object;
    // the frame of the last Update(): the bone callback applies the legs only
    // while they are fresh -- a creature left out of the update (far, or out of
    // view) gets its plain animation, not the ground under it some steps ago
    u32 m_updated_frame{};
    xr_vector<CIKLimb> _bone_chains;
    object_shift _object_shift;
    extrapolation::points _pose_extrapolation;
};

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Inert IKinematicsAnimated visual.
//
// Returned by vkVisual_Create() for MT_SKELETON_RIGID / MT_SKELETON_ANIM until
// real skinned-mesh classes port over. Satisfies the engine's `smart_cast<
// IKinematics*|IKinematicsAnimated*>(visual)` everywhere by reporting:
//   - zero bones        → engine bone-iteration loops do nothing
//   - BI_NONE bone IDs  → callers see "bone missing" and silently skip
//   - invalid MotionID  → animation playback is a no-op
//   - nullptr motions   → no key updates, no blends
// The visual itself is non-rendering (Render is no-op) — gameplay code that
// only needs to walk bones / play anims survives, but nothing draws on screen.

#pragma once

#include "vk_Visual.h"
#include "../../Include/xrRender/Kinematics.h"
#include "../../Include/xrRender/KinematicsAnimated.h"
#include "../../xr_3da/bone.h"      // CBoneInstance, CBoneData, IBoneData
#include "../../xr_3da/vismask.h"   // VisMask

class vkKinematicsAnimated_Stub
    : public vkRender_Visual
    , public IKinematics
    , public IKinematicsAnimated
{
public:
    vkKinematicsAnimated_Stub();
    ~vkKinematicsAnimated_Stub() override;

    // ---- IRenderVisual dcasts route to ourselves ---------------------------
    IKinematics*          dcast_PKinematics()         override { return this; }
    IKinematicsAnimated*  dcast_PKinematicsAnimated() override { return this; }

    // ---- IRenderVisual::getDebugName/Info (const, from IRenderVisual) ------
    shared_str getDebugName() const override { return dbg_name; }
    shared_str getDebugInfo() const override { return dbg_name; }

    // ---- IKinematics: visibility flags / mesh metadata --------------------
    void     SetRFlag(const u32, const bool) override {}
    bool     GetRFlag(const u32) const       override { return false; }
    u32      RChildCount() const             override { return 0; }
    Fvector3 RC_VisBox(const u32)            override { return Fvector3().set(0,0,0); }
    Fvector3 RC_VisCenter(const u32)         override { return Fvector3().set(0,0,0); }
    Fvector3 RC_VisBorderMin(const u32)      override { return Fvector3().set(0,0,0); }
    Fvector3 RC_VisBorderMax(const u32)      override { return Fvector3().set(0,0,0); }
    void     RC_Dump()                       override {}

    // ---- IKinematics: bone calculation ------------------------------------
    void Bone_Calculate(CBoneData*, Fmatrix*)                          override {}
    void Bone_GetAnimPos(Fmatrix& pos, u16, u8, bool)                  override { pos.identity(); }
    bool PickBone(const Fmatrix&, pick_result&, float, const Fvector&,
                  const Fvector&, u16)                                  override { return false; }
    void EnumBoneVertices(SEnumVerticesCallback&, u16)                  override {}

    // ---- IKinematics: low-level bone access -------------------------------
    u16          LL_BoneID(const char*)      const override { return BI_NONE; }
    u16          LL_BoneID(const shared_str&) const override { return BI_NONE; }
    const char*  LL_BoneName(const u16)      const override { return ""; }

    CInifile*    LL_UserData() override { return nullptr; }

    CBoneInstance& LL_GetBoneInstance(u16) override;
    CBoneData&     LL_GetData(u16)         override;
    const IBoneData& GetBoneData(u16) const override;

    u16 LL_BoneCount()        const override { return 0; }
    u16 LL_VisibleBoneCount()       override { return 0; }

    Fmatrix&        LL_GetTransform(u16)       override;
    const Fmatrix&  LL_GetTransform(u16) const override;
    Fmatrix&        LL_GetTransform_R(u16)     override;
    Fobb&           LL_GetBox(u16)             override;
    const Fbox&     GetBox()             const override;
    void LL_GetBindTransform(xr_vector<Fmatrix>&)         override {}
    int  LL_GetBoneGroups(xr_vector<xr_vector<u16>>&)     override { return 0; }

    u16  LL_GetBoneRoot()                override { return BI_NONE; }
    void LL_SetBoneRoot(u16)             override {}
    BOOL LL_GetBoneVisible(u16)          override { return FALSE; }
    void LL_SetBoneVisible(u16, BOOL, BOOL) override {}

    VisMask LL_GetBonesVisible()         override;
    void    LL_SetBonesVisible(VisMask)  override {}

    // ---- IKinematics: main functionality ----------------------------------
    void CalculateBones(BOOL = FALSE)    override {}
    void CalculateBones_Invalidate()     override {}
    void Callback(UpdateCallback, void*) override {}

    void           SetUpdateCallback(UpdateCallback c)      override { m_cb = c; }
    void           SetUpdateCallbackParam(void* p)          override { m_cb_param = p; }
    UpdateCallback GetUpdateCallback()                      override { return m_cb; }
    void*          GetUpdateCallbackParam()                 override { return m_cb_param; }

    IRenderVisual* dcast_RenderVisual() override { return this; }

#ifdef DEBUG
    void DebugRender(Fmatrix&) override {}
#endif
    shared_str getDebugName() override { return dbg_name; }   // non-const overload from IKinematics

    // ---- IKinematicsAnimated ----------------------------------------------
    void OnCalculateBones() override {}

    std::pair<LPCSTR, LPCSTR> LL_MotionDefName_dbg(MotionID) override { return {"", ""}; }
    void LL_DumpBlends_dbg() override {}

    u32     LL_PartBlendsCount(u32)         override { return 0; }
    CBlend* LL_PartBlend(u32, u32)          override { return nullptr; }
    void    LL_IterateBlends(IterateBlendsCallback&) override {}

    u16                    LL_MotionsSlotCount()  override { return 0; }
    const shared_motions&  LL_MotionsSlot(u16)    override;

    CMotionDef* LL_GetMotionDef(MotionID)        override { return nullptr; }
    CMotion*    LL_GetRootMotion(MotionID)       override { return nullptr; }
    CMotion*    LL_GetMotion(MotionID, u16)      override { return nullptr; }

    void LL_BuldBoneMatrixDequatize(const CBoneData*, u8, SKeyTable&) override {}
    void LL_BoneMatrixBuild(CBoneInstance&, const Fmatrix*, const SKeyTable&) override {}

    IBlendDestroyCallback* GetBlendDestroyCallback()                 override { return nullptr; }
    void                   SetBlendDestroyCallback(IBlendDestroyCallback*) override {}
    void                   SetUpdateTracksCalback(IUpdateTracksCallback*)  override {}
    IUpdateTracksCallback* GetUpdateTracksCalback()                  override { return nullptr; }

    MotionID LL_MotionID(LPCSTR) override { return MotionID(); }
    u16      LL_PartID(LPCSTR)   override { return BI_NONE; }

    CBlend* LL_PlayCycle(u16, MotionID, BOOL, float, float, float, BOOL,
                         PlayCallback, LPVOID, u8 = 0)               override { return nullptr; }
    CBlend* LL_PlayCycle(u16, MotionID, BOOL, PlayCallback,
                         LPVOID, u8 = 0)                              override { return nullptr; }

    void LL_CloseCycle(u16, u8 = (1 << 0))    override {}
    void LL_SetChannelFactor(u16, float)      override {}

    void UpdateTracks()                       override {}
    void LL_UpdateTracks(float, bool, bool)   override {}

    MotionID ID_Cycle(const shared_str&)      override { return MotionID(); }
    MotionID ID_Cycle_Safe(const shared_str&) override { return MotionID(); }
    CBlend* PlayCycle(const shared_str&, BOOL = TRUE, PlayCallback = nullptr,
                      LPVOID = nullptr, u8 = 0)                      override { return nullptr; }
    CBlend* PlayCycle(MotionID, BOOL = TRUE, PlayCallback = nullptr,
                      LPVOID = nullptr, u8 = 0)                      override { return nullptr; }
    CBlend* PlayCycle(u16, MotionID, BOOL = TRUE, PlayCallback = nullptr,
                      LPVOID = nullptr, u8 = 0)                      override { return nullptr; }

    MotionID ID_FX(LPCSTR)                    override { return MotionID(); }
    MotionID ID_FX_Safe(LPCSTR)               override { return MotionID(); }
    CBlend* PlayFX(LPCSTR, float)             override { return nullptr; }
    CBlend* PlayFX(MotionID, float)           override { return nullptr; }

    const CPartition& partitions() const      override;

    float get_animation_length(MotionID)      override { return 0.0f; }

private:
    UpdateCallback m_cb       = nullptr;
    void*          m_cb_param = nullptr;
};

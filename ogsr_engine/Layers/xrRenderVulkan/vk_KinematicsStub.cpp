// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - inert IKinematicsAnimated implementation. See header for
// the contract; this file backs the few methods that return references with
// shared static dummies so writes to "the bone" go to deterministic scratch
// space rather than crashing.

#include "stdafx.h"
#include "vk_KinematicsStub.h"
#include "../xrRender/KinematicAnimatedDefs.h"  // CPartition

// ---- Module-local sinks for reference-returning methods -------------------
// Engine code reads/writes these as if they were real bone data; with bone
// count reported as 0 it shouldn't normally reach LL_GetTransform / LL_GetBox
// / LL_GetData / LL_GetBoneInstance, but if it does the operations land here
// instead of dereferencing nullptr.
namespace {
CBoneInstance       g_dummy_bone_instance{};
CBoneData           g_dummy_bone_data{ 0 };
Fmatrix             g_dummy_matrix      = Fidentity;
Fobb                g_dummy_obb{};
Fbox                g_dummy_box{};
shared_motions      g_dummy_motions;
CPartition          g_dummy_partition;
} // namespace

vkKinematicsAnimated_Stub::vkKinematicsAnimated_Stub()
{
    Type = MT_SKELETON_ANIM;
    dbg_name = "kinematics_stub";
    g_dummy_box.set(Fvector().set(0,0,0), Fvector().set(0,0,0));
}

vkKinematicsAnimated_Stub::~vkKinematicsAnimated_Stub() = default;

CBoneInstance& vkKinematicsAnimated_Stub::LL_GetBoneInstance(u16 /*bone_id*/) { return g_dummy_bone_instance; }
CBoneData&     vkKinematicsAnimated_Stub::LL_GetData(u16 /*bone_id*/)         { return g_dummy_bone_data; }

const IBoneData& vkKinematicsAnimated_Stub::GetBoneData(u16 /*bone_id*/) const
{
    return g_dummy_bone_data;
}

Fmatrix&        vkKinematicsAnimated_Stub::LL_GetTransform(u16 /*bone_id*/)        { return g_dummy_matrix; }
const Fmatrix&  vkKinematicsAnimated_Stub::LL_GetTransform(u16 /*bone_id*/) const  { return g_dummy_matrix; }
Fmatrix&        vkKinematicsAnimated_Stub::LL_GetTransform_R(u16 /*bone_id*/)      { return g_dummy_matrix; }
Fobb&           vkKinematicsAnimated_Stub::LL_GetBox(u16 /*bone_id*/)              { return g_dummy_obb; }
const Fbox&     vkKinematicsAnimated_Stub::GetBox() const                          { return g_dummy_box; }

VisMask vkKinematicsAnimated_Stub::LL_GetBonesVisible() { return VisMask(); }

const shared_motions& vkKinematicsAnimated_Stub::LL_MotionsSlot(u16 /*idx*/) { return g_dummy_motions; }

const CPartition& vkKinematicsAnimated_Stub::partitions() const { return g_dummy_partition; }

// Factory — keeps vk_Visual.cpp from needing the full stub header.
vkRender_Visual* vkCreateKinematicsStub() { return xr_new<vkKinematicsAnimated_Stub>(); }

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// PATCHED COPY of Layers/xrRender/SkeletonCustom.cpp for the Vulkan renderer.
// Included by vk_SkeletonCustom.cpp under the dxRender_Visual->vkRender_Visual
// mapping. Only the skeleton-CHILD handling is adapted: the GEOMDEF leaves are
// vkFVisual-based vkSkeletonX_ST/_PM (Vulkan-native skinning), NOT CSkeletonX,
// so smart_cast<CSkeletonX*>(child) returns null. The few sites that linked or
// drove children through CSkeletonX are routed through the vk_child_* helpers
// below or made null-safe. Bone math / animation / blending are untouched.
// Diff vs the shared file is intentionally minimal — keep in sync on engine bumps.
#include "../xrRender/SkeletonCustom.h"
#include "../xrRender/SkeletonX.h"
#include "../../xr_3da/fmesh.h"
#include "../../xr_3da/Render.h"
#include "vk_Visual.h"  // vkSkeletonX_ST / vkSkeletonX_PM (the real Vulkan leaves)
#include <unordered_map>   // vkWM skin cache

// Link a skeleton child to its parent. Vulkan leaves are vkSkeletonX_ST/_PM
// (dynamic_cast), not CSkeletonX. No-op for any other child type.
static void vk_child_AfterLoad(dxRender_Visual* V, CKinematics* parent, u16 idx)
{
    if (auto* st = dynamic_cast<vkSkeletonX_ST*>(V)) { st->AfterLoad(parent, idx); return; }
    if (auto* pm = dynamic_cast<vkSkeletonX_PM*>(V)) { pm->AfterLoad(parent, idx); return; }
}
static void vk_child_SetParent(dxRender_Visual* V, CKinematics* parent)
{
    if (auto* st = dynamic_cast<vkSkeletonX_ST*>(V)) { st->SetParent(parent); return; }
    if (auto* pm = dynamic_cast<vkSkeletonX_PM*>(V)) { pm->SetParent(parent); return; }
}

// ============================================================================
// Skeleton wallmarks on vk leaves (blood decals on NPCs). The R4 CSkeletonX
// PickBone/FillVertices machinery is dead here (leaves aren't CSkeletonX), so
// these helpers reimplement _PickBoneSoftXW/_FillVerticesSoftXW over the CPU
// data the leaves retain at load (vkSkinCPUData — see vk_Visual.h/.cpp).
// ============================================================================
namespace {

const vkSkinCPUData* vk_child_wmData(dxRender_Visual* V)
{
    if (auto* st = dynamic_cast<vkSkeletonX_ST*>(V)) return st->wmCPU.get();
    if (auto* pm = dynamic_cast<vkSkeletonX_PM*>(V)) return pm->wmCPU.get();
    return nullptr;
}

const std::shared_ptr<vkSkinCPUData>* vk_child_wmShared(dxRender_Visual* V)
{
    if (auto* st = dynamic_cast<vkSkeletonX_ST*>(V)) return &st->wmCPU;
    if (auto* pm = dynamic_cast<vkSkeletonX_PM*>(V)) return &pm->wmCPU;
    return nullptr;
}

// Per-layout bone/weight extraction (mirrors _FillVerticesSoftXW id/weight fill).
inline void vkWM_Extract(const vertBoned1W& v, u16 b[4], float w[3])
{ b[0] = b[1] = b[2] = b[3] = (u16)v.matrix; w[0] = w[1] = w[2] = 0.f; }
inline void vkWM_Extract(const vertBoned2W& v, u16 b[4], float w[3])
{ b[0] = v.matrix0; b[1] = v.matrix1; b[2] = b[3] = b[1]; w[0] = v.w; w[1] = w[2] = 0.f; }
inline void vkWM_Extract(const vertBoned3W& v, u16 b[4], float w[3])
{ b[0] = v.m[0]; b[1] = v.m[1]; b[2] = v.m[2]; b[3] = b[2]; w[0] = v.w[0]; w[1] = v.w[1]; w[2] = 0.f; }
inline void vkWM_Extract(const vertBoned4W& v, u16 b[4], float w[3])
{ b[0] = v.m[0]; b[1] = v.m[1]; b[2] = v.m[2]; b[3] = v.m[3]; w[0] = v.w[0]; w[1] = v.w[1]; w[2] = v.w[2]; }

// Skin a bind-pose point into MODEL space (mRenderTransform — same branch
// scheme RenderWallmark uses on the WMFace it produces).
inline Fvector vkWM_Skin(CKinematics* P, const Fvector& pos, const u16 b[4], const float w[3])
{
    Fvector R;
    if (b[0] == b[1]) {
        P->LL_GetBoneInstance(b[0]).mRenderTransform.transform_tiny(R, pos);
    } else if (b[1] == b[2]) {
        Fvector P0, P1;
        P->LL_GetBoneInstance(b[0]).mRenderTransform.transform_tiny(P0, pos);
        P->LL_GetBoneInstance(b[1]).mRenderTransform.transform_tiny(P1, pos);
        R.lerp(P0, P1, w[0]);
    } else if (b[2] == b[3]) {
        Fvector P0, P1, P2;
        P->LL_GetBoneInstance(b[0]).mRenderTransform.transform_tiny(P0, pos);
        P->LL_GetBoneInstance(b[1]).mRenderTransform.transform_tiny(P1, pos);
        P->LL_GetBoneInstance(b[2]).mRenderTransform.transform_tiny(P2, pos);
        P0.mul(w[0]); P1.mul(w[1]); P2.mul(1.f - w[0] - w[1]);
        R = P0; R.add(P1); R.add(P2);
    } else {
        Fvector PB[4]; float s = 0.f;
        for (int i = 0; i < 4; ++i)
            P->LL_GetBoneInstance(b[i]).mRenderTransform.transform_tiny(PB[i], pos);
        for (int i = 0; i < 3; ++i) { PB[i].mul(w[i]); s += w[i]; }
        PB[3].mul(1.f - s);
        R = PB[0]; R.add(PB[1]); R.add(PB[2]); R.add(PB[3]);
    }
    return R;
}

// ⭐Skinned positions, cached per thread for ONE operation (a decal, a bone pick).
// 📏07-10-2026 Switch v224: wm.fill ~6-7 ms a decal -- every face re-skinned its
// three vertices, and a vertex sits in ~6 faces (and in the pick and the fill both).
// vkWM_NewEpoch() starts an operation: the pose may differ between operations, so
// nothing survives one. Slots are keyed by the leaf's CPU data; a stale slot (a freed
// leaf's address reused) is harmless -- its stamps belong to an older epoch.
struct WMSkinSlot { xr_vector<Fvector> pos; xr_vector<u32> stamp; xr_vector<u32> faceStamp; };
thread_local std::unordered_map<const vkSkinCPUData*, WMSkinSlot> t_wmSkin;
thread_local u32 t_wmEpoch = 1;

inline void vkWM_NewEpoch()
{
    if (++t_wmEpoch == 0 || t_wmSkin.size() > 24)
    {
        t_wmSkin.clear();
        t_wmEpoch = 1;
    }
}

inline WMSkinSlot& vkWM_Slot(const vkSkinCPUData& d)
{
    WMSkinSlot& s = t_wmSkin[&d];
    if (s.stamp.size() != d.vertCount)
    {
        s.stamp.assign(d.vertCount, 0);
        s.pos.resize(d.vertCount);
    }
    return s;
}

template <typename TV>
inline const Fvector& vkWM_SkinCached(WMSkinSlot& s, const TV* verts, CKinematics* P, u32 vi)
{
    if (s.stamp[vi] != t_wmEpoch)
    {
        const TV& v = verts[vi];
        u16 b[4]; float w[3];
        vkWM_Extract(v, b, w);
        s.pos[vi] = vkWM_Skin(P, v.P, b, w);
        s.stamp[vi] = t_wmEpoch;
    }
    return s.pos[vi];
}

template <typename TV>
BOOL vkWM_PickT(const vkSkinCPUData& d, CKinematics* P, IKinematics::pick_result& r,
                float dist, const Fvector& S, const Fvector& D, u16 bone)
{
    const auto& boneFaces = d.boneFaces();
    if (bone >= boneFaces.size()) return FALSE;
    const TV* verts = (const TV*)d.verts.data();
    WMSkinSlot& slot = vkWM_Slot(d);
    for (u32 face : boneFaces[bone])
    {
        const u32 idx = face * 3;
        for (u32 k = 0; k < 3; ++k)
            r.tri[k] = vkWM_SkinCached(slot, verts, P, d.indices[idx + k]);
        float u, v;
        r.dist = flt_max;
        if (CDB::TestRayTri(S, D, r.tri, u, v, r.dist, true) && (r.dist < dist)) {
            r.normal.mknormal(r.tri[0], r.tri[1], r.tri[2]);
            return TRUE;
        }
    }
    return FALSE;
}

template <typename TV>
void vkWM_FillT(const vkSkinCPUData& d, CKinematics* P, const Fmatrix& view,
                CSkeletonWallmark& wm, const Fvector& normal, float size, u16 bone)
{
    const auto& boneFaces = d.boneFaces();
    if (bone >= boneFaces.size()) return;
    const TV* verts = (const TV*)d.verts.data();
    WMSkinSlot& slot = vkWM_Slot(d);
    if (slot.faceStamp.size() != d.indices.size() / 3)
        slot.faceStamp.assign(d.indices.size() / 3, 0);
    for (u32 face : boneFaces[bone])
    {
        // a face of several bones in the decal's sphere: once, not once per bone
        if (slot.faceStamp[face] == t_wmEpoch)
            continue;
        slot.faceStamp[face] = t_wmEpoch;
        const u32 idx = face * 3;
        Fvector p[3];
        for (u32 k = 0; k < 3; ++k)
            p[k] = vkWM_SkinCached(slot, verts, P, d.indices[idx + k]);
        Fvector test_normal;
        test_normal.mknormal(p[0], p[1], p[2]);
        if (test_normal.dotproduct(normal) < EPS)
            continue;
        if (CDB::TestSphereTri(wm.ContactPoint(), size, p))
        {
            // the face's record only for the faces the decal takes
            CSkeletonWallmark::WMFace F;
            for (u32 k = 0; k < 3; ++k) {
                const TV& v = verts[d.indices[idx + k]];
                u16 b[4]; float w[3];
                vkWM_Extract(v, b, w);
                for (int j = 0; j < 4; ++j) F.bone_id[k][j] = b[j];
                for (int j = 0; j < 3; ++j) F.weight[k][j] = w[j];
                F.vert[k].set(v.P);
            }
            Fvector UV;
            for (u32 k = 0; k < 3; ++k) {
                view.transform_tiny(UV, p[k]);
                F.uv[k].x = (1 + UV.x) * .5f;
                F.uv[k].y = (1 - UV.y) * .5f;
            }
            wm.m_Faces.push_back(F);
        }
    }
}

BOOL vkWM_PickChild(dxRender_Visual* child, CKinematics* P, IKinematics::pick_result& r,
                    float dist, const Fvector& S, const Fvector& D, u16 bone)
{
    const vkSkinCPUData* d = vk_child_wmData(child);
    if (!d || d->indices.empty()) return FALSE;
    switch (d->links) {
    case 1: return vkWM_PickT<vertBoned1W>(*d, P, r, dist, S, D, bone);
    case 2: return vkWM_PickT<vertBoned2W>(*d, P, r, dist, S, D, bone);
    case 3: return vkWM_PickT<vertBoned3W>(*d, P, r, dist, S, D, bone);
    case 4: return vkWM_PickT<vertBoned4W>(*d, P, r, dist, S, D, bone);
    }
    return FALSE;
}

void vkWM_FillChild(dxRender_Visual* child, CKinematics* P, const Fmatrix& view,
                    CSkeletonWallmark& wm, const Fvector& normal, float size, u16 bone)
{
    const vkSkinCPUData* d = vk_child_wmData(child);
    if (!d || d->indices.empty()) return;
    switch (d->links) {
    case 1: vkWM_FillT<vertBoned1W>(*d, P, view, wm, normal, size, bone); break;
    case 2: vkWM_FillT<vertBoned2W>(*d, P, view, wm, normal, size, bone); break;
    case 3: vkWM_FillT<vertBoned3W>(*d, P, view, wm, normal, size, bone); break;
    case 4: vkWM_FillT<vertBoned4W>(*d, P, view, wm, normal, size, bone); break;
    }
}

}  // namespace

int psSkeletonUpdate = 32;

u16 CKinematics::LL_BoneID(const char* B) const
{
    if (const auto I = bone_map_N.find(B); I != bone_map_N.end())
        return I->second;

    return BI_NONE;
}

u16 CKinematics::LL_BoneID(const shared_str& B) const
{
    if (const auto I = bone_map_N.find(B); I != bone_map_N.end())
        return I->second;

    return BI_NONE;
}

const char* CKinematics::LL_BoneName(const u16 ID) const
{
    for (const auto& bone : bone_map_N)
        if (bone.second == ID)
            return bone.first.c_str();

    return nullptr;
}

CKinematics::CKinematics()
{
    m_is_original_lod = false;

    mOldWorldMartrix.identity();
    mOldWorldMartrixTmp.identity();

    dwFirstRenderFrame = static_cast<u32>(-1);

    // FIX (Vulkan port): the ctor must zero these. Pool instances are built via
    // Copy() (NOT Load()), and neither the ctor (stock) nor Copy() initialised
    // Update_Callback — so an instance's CalculateBones called Update_Callback()
    // on uninitialised heap garbage and AV'd. (Latent in stock OGSR; R4 just got
    // lucky with zeroed memory.) Init here so every CKinematics is safe.
    Update_Callback = nullptr;
    Update_Callback_Param = nullptr;
}

CKinematics::~CKinematics()
{
    WaitWallmarks();   // r_wm_async: the worker reads bone_instances, freed right below
    IBoneInstances_Destroy();

    // wallmarks
    ClearWallmarks();

    if (m_lod)
    {
        if (CKinematics* lod_kinematics = smart_cast<CKinematics*>(m_lod))
        {
            if (lod_kinematics->m_is_original_lod)
            {
                lod_kinematics->Release();
            }
        }

        xr_delete(m_lod);
    }
}


void CKinematics::StoreVisualMatrix(const Fmatrix& world_matrix)
{
    if (dwFirstRenderFrame != Device.dwFrame)
    {
        dwFirstRenderFrame = Device.dwFrame;
        mOldWorldMartrix.set(mOldWorldMartrixTmp);
        mOldWorldMartrixTmp.set(world_matrix);

        for (u16 i = 0; i < LL_BoneCount(); ++i)
        {
            auto& Bi = LL_GetBoneInstance(i);

            Bi.mRenderTransform_old.set(Bi.mRenderTransform_tmp);
            Bi.mRenderTransform_tmp.set(Bi.mRenderTransform);
        }
    }
}

void CKinematics::IBoneInstances_Create()
{
    // VERIFY				(bones->size() < 64, "More than 64 bones is a crazy thing!");
    const u32 size = bones->size();
    bone_instances = xr_alloc<CBoneInstance>(size);
    for (u32 i = 0; i < size; i++)
        bone_instances[i].construct();
}

void CKinematics::IBoneInstances_Destroy()
{
    if (bone_instances)
    {
        xr_free(bone_instances);
        bone_instances = nullptr;
    }
}

CSkeletonX* CKinematics::LL_GetChild(const u32 idx) const
{
    IRenderVisual* V = children[idx];
    CSkeletonX* B = smart_cast<CSkeletonX*>(V);
    return B;
}

void CKinematics::Load(const char* N, IReader* data, const u32 dwFlags)
{
    // Msg				("skeleton: %s",N);
    LoadProf::Scope _lpKin("kin.bones");
    inherited::Load(N, data, dwFlags);

    pUserData = nullptr;
    m_lod = nullptr;
    // loading lods

    IReader* LD = data->open_chunk(OGF_S_LODS);
    if (LD)
    {
        // From stream
        {
            string_path lod_name;
            LD->r_string(lod_name, sizeof(lod_name));

            m_lod = smart_cast<dxRender_Visual*>(RImplementation.model_CreateChild(lod_name, nullptr));

            if (CKinematics* lod_kinematics = smart_cast<CKinematics*>(m_lod))
            {
                lod_kinematics->m_is_original_lod = true;
            }

            VERIFY(m_lod, "Cant create LOD model for", N);
        }
        LD->close();
    }

    IReader* UD;

    if (const auto userdata_override_ini = RImplementation.Models->userdata_override_ini; 
        userdata_override_ini && userdata_override_ini->section_exist("data"))
    {
        for (const auto& pair : userdata_override_ini->r_section("data").Ordered_Data)
        {
            if (!strncmp(N, pair.first.c_str(), strlen(pair.first.c_str())))
            {
                UD = FS.r_open(fsgame::game_configs, pair.second.c_str());
                if (UD)
                {
                    Msg("Loading user_data [%s] for %s...", pair.second.c_str(), N);

                    pUserData = xr_new<CInifile>(UD, FS.get_path(fsgame::game_configs)->m_Path);
                    UD->close();
                }

                break;
            }
        }
    }

    {
    LoadProf::Scope _lpUser("kin.userdata");
    // try to read custom user data for module from ltx file
    if (!pUserData)
    {
        string_path ini_path;
        strcpy_s(ini_path, N);
        if (strext(ini_path))
            *strext(ini_path) = 0;
        strcat_s(ini_path, ".ltx");

        // Ask the file index first: almost no model has a <model>.ltx, and r_open of a
        // name the index lacks falls back to stat() on disk -- 3-5 ms per model on the
        // Switch SD (v159 [spawn prof] kin.userdata), paid by every first spawn.
        UD = FS.exist(fsgame::game_meshes, ini_path) ? FS.r_open(fsgame::game_meshes, ini_path) : nullptr;
        if (UD)
        {
            Msg("Loading user_data [%s] for %s...", ini_path, N);

            pUserData = xr_new<CInifile>(UD, FS.get_path(fsgame::game_configs)->m_Path);
            UD->close();
        }
    }

    // read user data from model if no custom found
    if (!pUserData)
    {
        // User data
        UD = data->open_chunk(OGF_S_USERDATA);
        pUserData = UD ? xr_new<CInifile>(UD, FS.get_path(fsgame::game_configs)->m_Path) : nullptr;
        if (UD)
            UD->close();
    }

    }

    // Globals
    bones = xr_new<vecBones>();
    bone_instances = nullptr;

    // Load bones
    xr_vector<shared_str> L_parents;

    R_ASSERT(data->find_chunk(OGF_S_BONE_NAMES));

    visimask.zero();

    u32 dwCount = data->r_u32();

    bone_map_N.reserve(dwCount);

    const auto bone_override_ini = RImplementation.Models->bone_override_ini;

    const bool has_bones = bone_override_ini && bone_override_ini->section_exist(N);

    for (; dwCount; dwCount--)
    {
        string256 buf;

        // Bone
        u16 ID = static_cast<u16>(bones->size());
        data->r_stringZ(buf, sizeof(buf));
        _strlwr(buf);
        CBoneData* pBone = CreateBoneData(ID);
        pBone->name = buf;
        pBone->child_faces.resize(children.size());
        bones->push_back(pBone);

        bone_map_N.emplace(pBone->name, ID);

        // It's parent
        data->r_stringZ(buf, sizeof(buf));
        _strlwr(buf);
        L_parents.emplace_back(buf);

        data->r(&pBone->obb, sizeof(Fobb));
        visimask.set(ID, true);
    }

    
    if (has_bones)
    {
        for (const auto& it : bone_override_ini->r_section(N).Ordered_Data)
        {
            shared_str bone_nm_old = it.first;
            shared_str bone_nm = it.second;

            u16 ID = LL_BoneID(bone_nm);

            if (ID != BI_NONE)
            {
                bone_map_N.emplace(bone_nm_old, ID);

                MsgDbg("Bone override model [%s] from [%s] to [%s]", N, bone_nm_old.c_str(), bone_nm.c_str());
            }
        }
    }

    // Attach bones to their parents
    iRoot = BI_NONE;
    for (u32 i = 0; i < bones->size(); i++)
    {
        shared_str P = L_parents[i];
        CBoneData* B = (*bones)[i];
        if (!P.c_str() || !P[0])
        {
            // no parent - this is root bone
            R_ASSERT(BI_NONE == iRoot);
            iRoot = static_cast<u16>(i);
            B->SetParentID(BI_NONE);
            continue;
        }
        else
        {
            const u16 ID = LL_BoneID(P);
            R_ASSERT(ID != BI_NONE);
            (*bones)[ID]->children.push_back(B);
            B->SetParentID(ID);
        }
    }
    R_ASSERT(BI_NONE != iRoot);

    // Free parents
    L_parents.clear();

    // IK data
    if (IReader* IKD = data->open_chunk(OGF_S_IKDATA))
    {
        const bool fix_cop_joints = pUserData ? READ_IF_EXISTS(pUserData, r_bool, "compat", "fix_cop_joints", false) : false;
        for (const auto& B : *bones)
        {
            const u16 vers = static_cast<u16>(IKD->r_u32());
            IKD->r_stringZ(B->game_mtl_name);
            IKD->r(&B->shape, sizeof(SBoneShape));
            B->IK_data.Import(*IKD, vers);

            IKD->r_fvector3(B->rotation);
            IKD->r_fvector3(B->position);
            B->bind_transform.setXYZi(B->rotation);
            B->bind_transform.translate_over(B->position);
            B->mass = IKD->r_float();
            IKD->r_fvector3(B->center_of_mass);

            if (fix_cop_joints)
            {
                // https://bitbucket.org/stalker/xray_re-tools/commits/209b9014129ceeb7d92375a77f60835553266bf1
                for (auto& it : B->IK_data.limits)
                {
                    const Fvector2 vec = it.limit;
                    const float tmp = vec.x;
                    it.limit.x = -vec.y;
                    it.limit.y = -tmp;
                }
            }
        }
        // calculate model to bone converting matrix
        (*bones)[LL_GetBoneRoot()]->CalculateM2B(Fidentity);
        IKD->close();
    }

    // after load process
    {
        LoadProf::Scope _lp("kin.afterload");
        for (u16 child_idx = 0; child_idx < static_cast<u16>(children.size()); child_idx++)
            vk_child_AfterLoad(children[child_idx], this, child_idx);  // vk: dynamic_cast to vkSkeletonX_*
    }

    // unique bone faces
    {
        LoadProf::Scope _lp("kin.faces");
        for (const auto& B : *bones)
        {
            for (u32 child_idx = 0; child_idx < children.size(); child_idx++)
            {
                CBoneData::FacesVec faces = B->child_faces[child_idx];
                std::sort(faces.begin(), faces.end());
                const CBoneData::FacesVecIt new_end = std::unique(faces.begin(), faces.end());
                faces.erase(new_end, faces.end());
                B->child_faces[child_idx].clear();
                B->child_faces[child_idx] = faces;
            }
        }
    }

    // reset update_callback
    Update_Callback = nullptr;
    // reset update frame
    wm_frame = static_cast<u32>(-1);

    LoadProf::Scope _lp("kin.validate");
    LL_Validate();
}

IC void iBuildGroups(CBoneData* B, U16Vec& tgt, u16 id, u16& last_id)
{
    if (B->IK_data.ik_flags.is(SJointIKData::flBreakable))
        id = ++last_id;
    tgt[B->GetSelfID()] = id;
    for (const auto& bone_it : B->children)
        iBuildGroups(bone_it, tgt, id, last_id);
}

void CKinematics::LL_Validate()
{
    // check breakable
    BOOL bCheckBreakable = FALSE;
    for (u16 k = 0; k < LL_BoneCount(); k++)
    {
        if (LL_GetData(k).IK_data.ik_flags.is(SJointIKData::flBreakable) && (LL_GetData(k).IK_data.type != jtNone))
        {
            bCheckBreakable = TRUE;
            break;
        }
    }

    if (bCheckBreakable)
    {
        BOOL bValidBreakable = TRUE;

        xr_vector<xr_vector<u16>> groups;
        LL_GetBoneGroups(groups);

        xr_vector<u16> b_parts(LL_BoneCount(), BI_NONE);
        CBoneData* root = &LL_GetData(LL_GetBoneRoot());
        u16 last_id = 0;
        iBuildGroups(root, b_parts, 0, last_id);

        for (u16 g = 0; g < static_cast<u16>(groups.size()); ++g)
        {
            xr_vector<u16>& group = groups[g];
            // vk: skinned leaves are vkSkeletonX_* (not CSkeletonX), so they never
            // run _CollectBoneFaces -> child_faces stays empty -> LL_GetBoneGroups
            // yields empty groups. group[0] would deref a null vector. Skip them.
            if (group.empty())
                continue;
            const u16 bp_id = b_parts[group[0]];
            for (u32 b = 1; b < groups[g].size(); b++)
                if (bp_id != b_parts[groups[g][b]])
                {
                    bValidBreakable = FALSE;
                    break;
                }
        }

        if (bValidBreakable == FALSE)
        {
            for (u16 k = 0; k < LL_BoneCount(); k++)
            {
                CBoneData& BD = LL_GetData(k);
                if (BD.IK_data.ik_flags.is(SJointIKData::flBreakable))
                    BD.IK_data.ik_flags.set(SJointIKData::flBreakable, FALSE);
            }
        }
    }
}

void CKinematics::Copy(dxRender_Visual* P)
{
    inherited::Copy(P);

    const CKinematics* pFrom = smart_cast<CKinematics*>(P);
    VERIFY(pFrom);
    pUserData = pFrom->pUserData;
    bones = pFrom->bones;
    iRoot = pFrom->iRoot;
    bone_map_N = pFrom->bone_map_N;
    visimask = pFrom->visimask;

    IBoneInstances_Create();

    for (u32 i = 0; i < children.size(); i++)
        vk_child_SetParent(children[i], this);  // vk: dynamic_cast to vkSkeletonX_*

    CalculateBones_Invalidate();

    m_lod = (pFrom->m_lod) ? smart_cast<dxRender_Visual*>(RImplementation.model_Duplicate(pFrom->m_lod)) : nullptr;
}

void CKinematics::CalculateBones_Invalidate()
{
    UCalc_Time = 0x0;
    UCalc_Visibox = psSkeletonUpdate;
}

void CKinematics::Spawn()
{
    inherited::Spawn();
    // bones
    for (u32 i = 0; i < bones->size(); i++)
        bone_instances[i].construct();
    Update_Callback = nullptr;
    CalculateBones_Invalidate();
    // wallmarks
    ClearWallmarks();
    Visibility_Invalidate();
    LL_SetBoneRoot(0);
}

void CKinematics::Depart()
{
    inherited::Depart();
    // wallmarks
    ClearWallmarks();

    // unmask all bones
    visimask.zero();
    if (bones)
    {
        for (u16 b = 0; b < bones->size(); b++)
            visimask.set(b, true);
    }

    // visibility
    children.insert(children.end(), children_invisible.begin(), children_invisible.end());
    children_invisible.clear();
}

void CKinematics::Release()
{
    // xr_free bones
    for (auto& B : *bones)
    {
        xr_delete(B);
    }

    // destroy shared data
    xr_delete(pUserData);
    xr_delete(bones);
    bone_map_N.clear();

    inherited::Release();
}

void CKinematics::LL_SetBoneVisible(const u16 bone_id, const BOOL val, const BOOL bRecursive)
{
    //R_ASSERT(bone_id < LL_BoneCount());
    if (bone_id >= LL_BoneCount())
        return;
    visimask.set(bone_id, !!val);
    if (!visimask.is(bone_id))
    {
        bone_instances[bone_id].mTransform.scale(0.f, 0.f, 0.f);
        if (LL_GetData(bone_id).GetParentID() < LL_BoneCount() && LL_GetData(bone_id).GetParentID() != BI_NONE)
            bone_instances[bone_id].mTransform.c = LL_GetBoneInstance(LL_GetData(bone_id).GetParentID()).mTransform.c;
    }
    else
    {
        CalculateBones_Invalidate();
    }
    bone_instances[bone_id].mRenderTransform.mul_43(bone_instances[bone_id].mTransform, (*bones)[bone_id]->m2b_transform);
    if (bRecursive)
    {
        for (const auto& C : (*bones)[bone_id]->children)
            LL_SetBoneVisible(C->GetSelfID(), val, bRecursive);
    }
    Visibility_Invalidate();
}

void CKinematics::LL_SetBonesVisible(VisMask mask)
{
    visimask.zero();
    for (u16 b = 0; b < bones->size(); b++)
    {
        if (mask.is(b))
        {
            visimask.set(b, true);
        }
        else
        {
            Fmatrix& A = bone_instances[b].mTransform;
            Fmatrix& B = bone_instances[b].mRenderTransform;
            A.scale(0.f, 0.f, 0.f);
            B.mul_43(A, (*bones)[b]->m2b_transform);
        }
    }
    CalculateBones_Invalidate();
    Visibility_Invalidate();
}

void CKinematics::Visibility_Update()
{
    ZoneScoped;

    Update_Visibility = FALSE;

    // check visible
    for (u32 c_it = 0; c_it < children.size(); c_it++)
    {
        CSkeletonX* _c = smart_cast<CSkeletonX*>(children[c_it]);
        if (!_c) continue;  // vk leaf (vkSkeletonX_*): not CSkeletonX — skip visibility cull
        if (!_c->has_visible_bones())
        {
            // move into invisible list
            children_invisible.push_back(children[c_it]);
            swap(children[c_it], children.back());
            children.pop_back();
            Update_Visibility = TRUE;
        }
    }

    // check invisible
    for (u32 _it = 0; _it < children_invisible.size(); _it++)
    {
        CSkeletonX* _c = smart_cast<CSkeletonX*>(children_invisible[_it]);
        if (!_c) continue;  // vk leaf: skip
        if (_c->has_visible_bones())
        {
            // move into visible list
            children.push_back(children_invisible[_it]);
            swap(children_invisible[_it], children_invisible.back());
            children_invisible.pop_back();
            Update_Visibility = TRUE;
        }
    }
}

IC static void RecursiveBindTransform(CKinematics* K, xr_vector<Fmatrix>& matrices, const u16 bone_id, const Fmatrix& parent)
{
    const CBoneData& BD = K->LL_GetData(bone_id);
    Fmatrix& BM = matrices[bone_id];
    // Build matrix
    BM.mul_43(parent, BD.bind_transform);
    for (const auto& C : BD.children)
        RecursiveBindTransform(K, matrices, C->GetSelfID(), BM);
}

void CKinematics::LL_GetBindTransform(xr_vector<Fmatrix>& matrices)
{
    matrices.resize(LL_BoneCount());
    RecursiveBindTransform(this, matrices, iRoot, Fidentity);
}

void BuildMatrix(Fmatrix& mView, const float invsz, const Fvector norm, const Fvector& from)
{
    // build projection
    Fmatrix mScale;
    Fvector at, up, right, y;
    at.sub(from, norm);
    y.set(0, 1, 0);
    if (_abs(norm.y) > .99f)
        y.set(1, 0, 0);
    right.crossproduct(y, norm);
    up.crossproduct(norm, right);
    mView.build_camera(from, at, up);
    mScale.scale(invsz, invsz, invsz);
    mView.mulA_43(mScale);
}
void CKinematics::EnumBoneVertices(SEnumVerticesCallback& C, const u16 bone_id)
{
    // Vulkan: leaves are vkSkeletonX_* (not CSkeletonX) so LL_GetChild() is null.
    // Bone-vertex enumeration (IK foot placement / skinned decals) is skipped here.
    (void)C; (void)bone_id;
}

DEFINE_VECTOR(Fobb, OBBVec, OBBVecIt);

bool CKinematics::PickBone(const Fmatrix& parent_xform, IKinematics::pick_result& r, const float dist, const Fvector& start, const Fvector& dir, const u16 bone_id)
{
    vkWM_NewEpoch();   // this pose, this call

    Fvector S, D; // normal		= {0,0,0}
    // transform ray from world to model
    Fmatrix P;
    P.invert(parent_xform);
    P.transform_tiny(S, start);
    P.transform_dir(D, dir);
    for (u32 i = 0; i < children.size(); i++)
        if (vkWM_PickChild(children[i], this, r, dist, S, D, bone_id))  // vk leaves: CPU data pick
        {
            parent_xform.transform_dir(r.normal);
            parent_xform.transform_tiny(r.tri[0]);
            parent_xform.transform_tiny(r.tri[1]);
            parent_xform.transform_tiny(r.tri[2]);
            return true;
        }
    return false;
}

void CKinematics::wallmark_calculate_details::add_wallmark_internal(CKinematics* parent)
{
    ZoneScoped;
    vkWM_NewEpoch();   // one pose for the pick and the fill of this decal

    // The per-bone face lists are built in the background, never here: this
    // runs on the second thread, which the main thread waits for at the end of
    // the frame. Until they are ready this model takes no blood decal -- the
    // next hit on it does.
    bool faces_ready = true;
    for (u32 i = 0; i < parent->children.size(); i++)
        if (const std::shared_ptr<vkSkinCPUData>* sp = vk_child_wmShared(parent->children[i]))
            if (*sp && !(*sp)->indices.empty() && !(*sp)->facesReady())
            {
                vkSkinCPUData::RequestFaces(*sp);
                faces_ready = false;
            }
    if (!faces_ready)
        return;

    Fvector S, D, normal = {0, 0, 0};
    // transform ray from world to model
    Fmatrix P;
    P.invert(*parent_xform);
    P.transform_tiny(S, position);
    P.transform_dir(D, direction);
    // find pick point
    float dist = flt_max;
    BOOL picked = FALSE;

    DEFINE_VECTOR(Fobb, OBBVec, OBBVecIt);
    OBBVec cache_obb;
    cache_obb.resize(parent->LL_BoneCount());
    IKinematics::pick_result r;
    r.normal = normal;
    r.dist = dist;
    LoadProf::Scope _lp_pick("wm.pick");
    for (u16 k = 0; k < parent->LL_BoneCount(); k++)
    {
        CBoneData& BD = parent->LL_GetData(k);
        if (parent->LL_GetBoneVisible(k) && !BD.shape.flags.is(SBoneShape::sfNoPickable))
        {
            Fobb& obb = cache_obb[k];
            obb.transform(BD.obb, parent->LL_GetBoneInstance(k).mTransform);
            if (CDB::TestRayOBB(S, D, obb))
                for (u32 i = 0; i < parent->children.size(); i++)
                {
                    if (vkWM_PickChild(parent->children[i], parent, r, dist, S, D, k))  // vk leaves: CPU data pick
                    {
                        picked = TRUE;
                        dist = r.dist;
                        normal = r.normal;
                    }
                }
        }
    }
    if (!picked)
    {
        return;
    }

    // calculate contact point
    Fvector cp;
    cp.mad(S, D, dist);

    // collect collide boxes
    Fsphere test_sphere;
    test_sphere.set(cp, size);
    U16Vec test_bones;
    test_bones.reserve(parent->LL_BoneCount());
    for (u16 k = 0; k < parent->LL_BoneCount(); k++)
    {
        CBoneData& BD = parent->LL_GetData(k);
        if (parent->LL_GetBoneVisible(k) && !BD.shape.flags.is(SBoneShape::sfNoPickable))
        {
            Fobb& obb = cache_obb[k];
            if (CDB::TestSphereOBB(test_sphere, obb))
                test_bones.push_back(k);
        }
    }

    // ok. allocate wallmark
    intrusive_ptr<CSkeletonWallmark> wm = xr_new<CSkeletonWallmark>(parent, parent_xform, shader, cp, Device.fTimeGlobal);
    wm->m_LocalBounds.set(cp, size * 2.f);
    wm->XFORM()->transform_tiny(wm->m_Bounds.P, cp);
    wm->m_Bounds.R = wm->m_LocalBounds.R;

    Fvector tmp;
    tmp.invert(D);
    normal.add(tmp).normalize();

    // build UV projection matrix
    Fmatrix mView, mRot;
    BuildMatrix(mView, 1 / (0.9f * size), normal, cp);
    mRot.rotateZ(::Random.randF(deg2rad(-20.f), deg2rad(20.f)));
    mView.mulA_43(mRot);

    // fill vertices (vk leaves: CPU data fill — see vkWM_FillChild above)
    LoadProf::Scope _lp_fill("wm.fill");
    for (u32 i = 0; i < parent->children.size(); i++)
        for (u16& test_bone : test_bones)
            vkWM_FillChild(parent->children[i], parent, mView, *wm, normal, size, test_bone);

    {
        parent->wallmarks_lock.lock();

        // find similar wm and remove
        for (u32 wm_idx = 0; wm_idx < parent->wallmarks.size(); wm_idx++)
        {
            intrusive_ptr<CSkeletonWallmark>& wm = parent->wallmarks[wm_idx];
            if (wm->Similar(shader, cp, 0.02f))
            {
                if (wm_idx < parent->wallmarks.size() - 1)
                    wm = parent->wallmarks.back();
                parent->wallmarks.pop_back();
                break;
            }
        }

        // add new
        parent->wallmarks.push_back(wm);

        parent->wallmarks_lock.unlock();
    }
}

// ⭐r_wm_async (07-10-2026): a blood decal is built on a worker pinned to core 2 and NOT
// waited for -- it lands in `wallmarks` under wallmarks_lock (which the render already
// takes) a frame or two later. 📏Switch v224: through seqParallel a burst of hits cost
// the second thread 34-36 ms (wm.fill ~6-7 ms a decal) and the main thread waited for it
// at the end of the frame. The job holds its own copy of the details; m_wm_pending keeps
// the model alive (ClearWallmarks / the destructor wait for it).
extern int ps_r_wm_async;
static PinnedWorker* WallmarkWorker()
{
    static PinnedWorker* w = new PinnedWorker(2, 0x30); // below the particle/ray workers; leaked: no join at exit
    return w;
}

void CKinematics::WaitWallmarks()
{
    while (m_wm_pending.load(std::memory_order_acquire))
        std::this_thread::yield();
}

void CKinematics::AddWallmark(Fmatrix* parent_xform, const Fvector3& start, const Fvector3& dir, const ref_shader& shader, const float size)
{
    ZoneScoped;

    if (ps_r_wm_async)
    {
        wallmark_calculate_details d;
        d.parent_xform = parent_xform;
        d.position = start;
        d.direction = dir;
        d.shader = shader;
        d.size = size;
        m_wm_pending.fetch_add(1, std::memory_order_acq_rel);
        WallmarkWorker()->Run([this, d]() mutable {
            LoadProf::Arm arm;   // a decal is its own outermost operation on the worker
            d.add_wallmark_internal(this);
            m_wm_pending.fetch_sub(1, std::memory_order_acq_rel);
        });
        return;
    }

    wallmarks_to_add.emplace_back(parent_xform, start, dir, shader, size);

    Device.add_to_seq_parallel(fastdelegate::MakeDelegate(this, &CKinematics::AddWallmarkAsync), "skin wallmark");
}

void CKinematics::AddWallmarkAsync()
{
    for (auto & item : wallmarks_to_add)
    {
        item.add_wallmark_internal(this);
    }

    wallmarks_to_add.clear();
}

void CKinematics::CalculateWallmarks(const bool hud) // called on render
{
    ZoneScoped;

    if (wm_frame == Device.dwFrame)
        return;

    wm_frame = Device.dwFrame;

    wallmarks_lock.lock();

    if (!wallmarks.empty())
    {
        std::erase_if(wallmarks, [&](const auto& wm) {
            if ((Device.fTimeGlobal - wm->TimeStart()) / ps_r__WallmarkTTL >= 1.f)
            {
                return true; // remove wallmark
            }

            // append wm to WallmarkEngine
            if (!hud && RImplementation.ViewBase.testSphere_dirty(wm->m_Bounds.P, wm->m_Bounds.R))
                ::RImplementation.append_SkeletonWallmark(wm);

            return false;
        });
    }

    wallmarks_lock.unlock();
}

void CKinematics::RenderWallmark(intrusive_ptr<CSkeletonWallmark> wm, FVF::LIT*& V)
{
    VERIFY(wm);
    VERIFY(V);
    VERIFY(bones, "Invalid visual. Bones already released.");
    VERIFY(bone_instances, "Invalid visual. bone_instances already deleted.");

    if ((wm == nullptr) || (nullptr == bones) || (nullptr == bone_instances))
        return;

    ZoneScoped;

    // skin vertices
    for (u32 f_idx = 0; f_idx < wm->m_Faces.size(); f_idx++)
    {
        const CSkeletonWallmark::WMFace F = wm->m_Faces[f_idx];
        const float w = (Device.fTimeGlobal - wm->TimeStart()) / ps_r__WallmarkTTL;
        for (u32 k = 0; k < 3; k++)
        {
            Fvector P;
            if (F.bone_id[k][0] == F.bone_id[k][1])
            {
                // 1-link
                Fmatrix& xform0 = LL_GetBoneInstance(F.bone_id[k][0]).mRenderTransform;
                xform0.transform_tiny(P, F.vert[k]);
            }
            else if (F.bone_id[k][1] == F.bone_id[k][2])
            {
                // 2-link
                Fvector P0, P1;
                Fmatrix& xform0 = LL_GetBoneInstance(F.bone_id[k][0]).mRenderTransform;
                Fmatrix& xform1 = LL_GetBoneInstance(F.bone_id[k][1]).mRenderTransform;
                xform0.transform_tiny(P0, F.vert[k]);
                xform1.transform_tiny(P1, F.vert[k]);
                P.lerp(P0, P1, F.weight[k][0]);
            }
            else if (F.bone_id[k][2] == F.bone_id[k][3])
            {
                // 3-link
                Fvector P0, P1, P2;
                Fmatrix& xform0 = LL_GetBoneInstance(F.bone_id[k][0]).mRenderTransform;
                Fmatrix& xform1 = LL_GetBoneInstance(F.bone_id[k][1]).mRenderTransform;
                Fmatrix& xform2 = LL_GetBoneInstance(F.bone_id[k][2]).mRenderTransform;
                xform0.transform_tiny(P0, F.vert[k]);
                xform1.transform_tiny(P1, F.vert[k]);
                xform2.transform_tiny(P2, F.vert[k]);
                const float w0 = F.weight[k][0];
                const float w1 = F.weight[k][1];
                P0.mul(w0);
                P1.mul(w1);
                P2.mul(1 - w0 - w1);
                P = P0;
                P.add(P1);
                P.add(P2);
            }
            else
            {
                // 4-link
                Fvector PB[4];
                for (int i = 0; i < 4; ++i)
                {
                    Fmatrix& xform = LL_GetBoneInstance(F.bone_id[k][i]).mRenderTransform;
                    xform.transform_tiny(PB[i], F.vert[k]);
                }

                float s = 0.f;
                for (int i = 0; i < 3; ++i)
                {
                    PB[i].mul(F.weight[k][i]);
                    s += F.weight[k][i];
                }
                PB[3].mul(1 - s);

                P = PB[0];
                for (int i = 1; i < 4; ++i)
                    P.add(PB[i]);
            }
            wm->XFORM()->transform_tiny(V->p, P);
            // vk: lift the decal towards the CAMERA — its triangles are exactly
            // coplanar with the body mesh (z-fight flicker otherwise). Statics
            // solve this with a build-time normal push; a skinned surface
            // deforms every frame, so push at skinning time instead (R4 shifts
            // the projection matrix — Device.mProject is unusable on VK).
            {
                Fvector toCam;
                toCam.sub(Device.vCameraPosition, V->p);
                const float len = toCam.magnitude();
                if (len > EPS)
                    V->p.mad(V->p, toCam.div(len), 0.012f);
            }
            V->t.set(F.uv[k]);
            // vk: straight alpha blend (our PBM_BLEND pipeline) — R4 wrote the
            // AGE into alpha for an inverse-alpha blender; we write the
            // remaining-life fade directly. Capped at ~0.7 so the cloth/armor
            // texture reads through the blood — "soaked in", not a sticker.
            int aC = iFloor((1.f - w) * 178.f);
            clamp(aC, 0, 255);
            V->color = color_rgba(255, 255, 255, aC);
            V++;
        }
    }
    wm->XFORM()->transform_tiny(wm->m_Bounds.P, wm->m_LocalBounds.P);
}

void CKinematics::ClearWallmarks()
{
    WaitWallmarks();   // r_wm_async: a decal still being built would land after the clear
    Device.remove_from_seq_parallel(fastdelegate::MakeDelegate(this, &CKinematics::AddWallmarkAsync));

    wallmarks_to_add.clear();

    wallmarks_lock.lock();

    wallmarks.clear();

    wallmarks_lock.unlock();
}

int CKinematics::LL_GetBoneGroups(xr_vector<xr_vector<u16>>& groups)
{
    groups.resize(children.size());
    for (u16 bone_idx = 0; bone_idx < static_cast<u16>(bones->size()); bone_idx++)
    {
        const CBoneData* B = (*bones)[bone_idx];
        for (u32 child_idx = 0; child_idx < children.size(); child_idx++)
        {
            if (!B->child_faces[child_idx].empty())
            {
                groups[child_idx].push_back(bone_idx);
            }
        }
    }
    return groups.size();
}

/************************* Add by Zander *************************************************************/
/************************* Дебаговые функции для исследования моделей. *******************************/
static Fbox& get_mesh_RC_data(const FHierrarhyVisual* pHV, const u32 id) { return pHV->children.at(id)->getVisData().box; }

Fvector3 CKinematics::RC_VisBox(const u32 id)
{
    const Fbox& t = get_mesh_RC_data(this, id);
    Fvector3 modelSize;
    t.getsize(modelSize);
    return modelSize;
}

Fvector3 CKinematics::RC_VisCenter(const u32 id)
{
    const Fbox& t = get_mesh_RC_data(this, id);
    Fvector3 modelSize;
    t.getcenter(modelSize);
    return modelSize;
}

Fvector3 CKinematics::RC_VisBorderMin(const u32 id)
{
    const Fbox& t = get_mesh_RC_data(this, id);
    Fvector3 borderModelMin;
    borderModelMin.set(t.min);
    return borderModelMin;
}

Fvector3 CKinematics::RC_VisBorderMax(const u32 id)
{
    const Fbox& t = get_mesh_RC_data(this, id);
    Fvector3 borderModelMax;
    borderModelMax.set(t.max);
    return borderModelMax;
}

void CKinematics::RC_Dump()
{
    Msg("|********** Dump children meshes **************|");
    const Fbox& mBox = getVisData().box;
    Fvector3 temp;
    mBox.getsize(temp);
    Msg("[core] : box[%3.3f,%3.3f,%3.3f]", temp.x, temp.y, temp.z);
    mBox.getcenter(temp);
    Msg("[core] : center[%3.3f,%3.3f,%3.3f]", temp.x, temp.y, temp.z);
    temp.set(mBox.min);
    Msg("[core] : min[%3.3f,%3.3f,%3.3f]", temp.x, temp.y, temp.z);
    temp.set(mBox.max);
    Msg("[core] : max[%3.3f,%3.3f,%3.3f]", temp.x, temp.y, temp.z);
    for (u32 i = 0; i < children.size(); i++)
    {
        temp.set(RC_VisBox(i));
        Msg("[child %u] : box[%3.3f,%3.3f,%3.3f]", i, temp.x, temp.y, temp.z);
        temp.set(RC_VisCenter(i));
        Msg("[child %u] : center[%3.3f,%3.3f,%3.3f]", i, temp.x, temp.y, temp.z);
        temp.set(RC_VisBorderMin(i));
        Msg("[child %u] : min[%3.3f,%3.3f,%3.3f]", i, temp.x, temp.y, temp.z);
        temp.set(RC_VisBorderMax(i));
        Msg("[child %u] : max[%3.3f,%3.3f,%3.3f]", i, temp.x, temp.y, temp.z);
        FHierrarhyVisual* HV = smart_cast<FHierrarhyVisual*>(children.at(i));
        if (HV && HV->children.size())
        {
            for (u32 j = 0; j < HV->children.size(); j++)
            {
                const Fbox& FB = get_mesh_RC_data(HV, j);
                FB.getsize(temp);
                Msg("[child %u->%u] : box[%3.3f,%3.3f,%3.3f]", i, j, temp.x, temp.y, temp.z);
                FB.getcenter(temp);
                Msg("[child %u->%u] : center[%3.3f,%3.3f,%3.3f]", i, j, temp.x, temp.y, temp.z);
                temp.set(FB.min);
                Msg("[child %u->%u] : min[%3.3f,%3.3f,%3.3f]", i, j, temp.x, temp.y, temp.z);
                temp.set(FB.max);
                Msg("[child %u->%u] : max[%3.3f,%3.3f,%3.3f]", i, j, temp.x, temp.y, temp.z);
                FHierrarhyVisual* kHV = smart_cast<FHierrarhyVisual*>(HV->children.at(j));
                if (kHV && kHV->children.size())
                {
                    for (u32 k = 0; k < kHV->children.size(); k++)
                    {
                        const Fbox& kFB = get_mesh_RC_data(kHV, k);
                        kFB.getsize(temp);
                        Msg("[child %u->%u->%u] : box[%3.3f,%3.3f,%3.3f]", i, j, k, temp.x, temp.y, temp.z);
                        kFB.getcenter(temp);
                        Msg("[child %u->%u->%u] : center[%3.3f,%3.3f,%3.3f]", i, j, k, temp.x, temp.y, temp.z);
                        temp.set(kFB.min);
                        Msg("[child %u->%u->%u] : min[%3.3f,%3.3f,%3.3f]", i, j, k, temp.x, temp.y, temp.z);
                        temp.set(kFB.max);
                        Msg("[child %u->%u->%u] : max[%3.3f,%3.3f,%3.3f]", i, j, k, temp.x, temp.y, temp.z);
                    }
                }
            }
        }
    }

    Msg("|********** End Dump children meshes **********|");

    Msg("|********** Dump children bones ***************|");
    for (u32 i = 0; i < bones->size(); i++)
    {
        const CBoneData* B = (*bones)[i];
        Msg("Bone [%u][%s]:", i, LL_BoneName(static_cast<u16>(i)));
        Msg("bind_transform[%3.3f,%3.3f,%3.3f] m2b_transform[%3.3f,%3.3f,%3.3f] center_of_mass[%3.3f,%3.3f,%3.3f]", B->bind_transform.c.x, B->bind_transform.c.y,
            B->bind_transform.c.z, B->m2b_transform.c.x, B->m2b_transform.c.y, B->m2b_transform.c.z, B->center_of_mass.x, B->center_of_mass.y, B->center_of_mass.z);
    }
    Msg("|********** End Dump children bones ***********|");
}
/************************* End add *************************************/

u16 CKinematics::LL_VisibleBoneCount() {
    u16 cnt = 0;
    for (u16 i = 0; i < LL_BoneCount(); i++)
        if (LL_GetBoneVisible(i)) cnt++;
    return cnt;
}
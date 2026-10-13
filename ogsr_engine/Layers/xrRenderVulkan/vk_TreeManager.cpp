// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - CTreeManager Session A: extract MT_TREE_ST/PM visuals
// from the level, group by (vb,ib,tcOffset,texture), upload metadata +
// transforms SSBOs, allocate indirect/draw-count buffers + per-texture
// descriptor sets. No rendering yet — Session B adds compute cull + draw.

#include "stdafx.h"
#include "vk_descriptors.h"     // VK::DescriptorWriter
#include "vk_TreeManager.h"
#include "vk_LODManager.h"      // nfs21: g_TreeLodRref, TreeLodKRaw
#include <unordered_map>
#include "vk_Visual.h"
#include "vk_material.h"
#include "vk_world_material.h"   // VK::WorldMaterial — live diffuse for level statics
#include "vk_texture.h"
#include "vk_texture_stream.h"   // SetStreamable — tree-path textures opt OUT of mip streaming
#include "vk_buffer.h"
#include "vk_parallel.h"      // VK::ParallelChunks — the per-instance packs run on the idle cores
#include "HW_Vulkan.h"
#include "CRender_Vulkan.h"

#include <algorithm>
#include <cfloat>
#include <map>       // icosphere edge-midpoint dedup (crown-hull bake)
#include <thread>    // per-species parallel crown bake (BuildMeshlets)
#include <atomic>
#include <cstdlib>   // std::getenv — XROS_LEAFWALK_FULL (recursive reference walk)

// GLOBAL scope (an extern inside namespace VK would mangle as VK::* → LNK2001).
extern int ps_r_vsm_tree_impostor;   // r_vsm_tree_impostor — parked/default 0; gates the load-time silhouette bake
extern int ps_r_vsm_tree_hull_vox;   // r_vsm_tree_hull_vox — hull shape: 0 = PCA lobes, >0 = voxel-shell resolution (baked)
extern int ps_r_tree_stats;          // r_tree_stats — 2 also runs the load-time card-tightness survey

// Subdivision orders the card survey reports. 1 = drop whole empty triangles;
// 2 and 4 = split into N² sub-triangles and drop the empty ones. Kept small on
// purpose: each level costs 7 alpha taps per sub-triangle across the whole level.
constexpr u32 kCardLevels   = 3;
constexpr int kCardSplit[kCardLevels] = { 1, 2, 4 };
extern int ps_r_vsm_tree_hull_debug; // r_vsm_tree_hull_debug — the ONLY consumer of the visual voxel cloud (see the upload below)
extern int ps_r_vsm_tree_vox_cloud;  // r_vsm_tree_vox_cloud — bake the cube cloud even with the viewmode off (the old always-on behaviour)
extern float ps_r_tree_vox_dist;     // r_tree_vox_dist — forward crown range where the cube cloud replaces the mesh (the cloud's shipping consumer)
extern int ps_r_vsm;                 // nfs42 r_vsm (vk_console_min.cpp)
extern int ps_r_tree_crown_bake;     // r_tree_crown_bake (vk_console_min.cpp): 1 = always bake (PC), 0 = only with a reader at load
// nfs42: the crown bake (BuildMeshlets) serves only VSM tree shadows and the cube tier. r_tree_crown_bake 0 (the
// Switch preset) skips it (and its alpha preload thread) when none of them is on at level load: ~2 s of Kordon load
// on one thread, no reader. 1 (default) keeps the old always-bake: a reader switched on mid-level (shadow tier menu
// -> r_vsm 1) then still finds its meshlets/hulls/voxels.
static bool NxCrownBakeNeeded()
{
    return ps_r_tree_crown_bake != 0 || ps_r_vsm != 0 || ps_r_tree_vox_dist > 0.f || ps_r_vsm_tree_hull_debug != 0
        || ps_r_vsm_tree_vox_cloud != 0;
}

namespace VK
{

CTreeManager::CTreeManager() {}
CTreeManager::~CTreeManager() { Destroy(); }

// ============================================================================
// Recursive extraction. Mirrors monolith's ExtractMeshesFromVisual but only
// the tree-relevant cases — no AREF static or shadow GBuffer paths here.
// ============================================================================
void CTreeManager::ExtractFromVisual(::vkRender_Visual* vis, xr_vector<::vkFTreeVisual*>& outTrees,
                                     xr_vector<::vkFTreeVisual*>& outFlodBacked, u32 source, bool underValidFlod)
{
    if (!vis) return;

    const u32 type = vis->Type;

    // static_cast, not dynamic_cast, on all three arms below: vkVisual_Create maps
    // MT_TREE_ST/PM to vkFTreeVisual_ST/PM, MT_HIERRARHY to vkFHierrarhyVisual and
    // MT_LOD to vkFLOD, exactly and exclusively. RTTI on every node of a 450k-visual
    // level was measurable load time for an answer Type already gives.
    if (type == MT_TREE_ST || type == MT_TREE_PM)
    {
        auto* tv = static_cast<vkFTreeVisual*>(vis);
        if (!tv->m_mesh.p_rm_Vertices || !tv->m_mesh.p_rm_Indices) return;
        if (tv->m_mesh.dwPrimitives == 0) return;
        // Live diffuse for level statics lives in m_pWorldMaterial (the parked
        // CMaterial / m_pMaterial is a stub for the deferred path — null here).
        if (!tv->m_pWorldMaterial || tv->m_pWorldMaterial->view == VK_NULL_HANDLE) return;
        outTrees.push_back(tv);
        if (underValidFlod) outFlodBacked.push_back(tv);
        return;
    }

    if (type == MT_HIERRARHY)
    {
        for (auto* child : static_cast<vkFHierrarhyVisual*>(vis)->children)
            ExtractFromVisual(child, outTrees, outFlodBacked, 1, underValidFlod);
        return;
    }

    // LOD containers (MT_LOD = FLOD, extends FHierrarhyVisual): at close range
    // R4 renders ALL children — the container holds the tree's SEPARATE parts
    // (trunk + leaf/branch visuals), NOT a stack of LOD levels. Taking only
    // children[0] dropped every other part → missing twigs, and floating crowns
    // when children[0] happened to be a leaf cluster (the trunk lives in a later
    // child). Recurse over ALL children, same as MT_HIERRARHY.
    // (Ref: r__dsgraph_build.cpp:557-562 `for (Vis : pV->children) add_leafs_static`.)
    if (type == MT_LOD)
    {
        // Does CLODManager actually draw a billboard for THIS container? Its
        // accept test is the authority — mirror it exactly (vk_LODManager.cpp,
        // "Collect FLODs with valid billboard facets"). A container that fails it
        // has no imposter, so its trees must keep their full mesh at any range.
        auto* flod = static_cast<vkFLOD*>(vis);
        const bool validFlod = flod->facetsValid && flod->vis.sphere.R > 0.01f;
        for (auto* child : flod->children)
            ExtractFromVisual(child, outTrees, outFlodBacked, 2, underValidFlod || validFlod);
        return;
    }
    // MT_NORMAL / MT_PROGRESSIVE / MT_SKELETON_*: skip (not trees).
}

// Latches an out-of-VRAM upload (see UploadDeviceLocal) so Build() refuses to mark
// itself built. Meta and transforms are NOT optional: every bin dispatch and every
// page draw indexes them, so shipping a half-built manager only defers the crash by
// a few frames instead of degrading. Big maps DO hit the ceiling — Pripyat loads
// with ~5 GB of tree pools alone against a 7123 MB budget.
static bool s_UploadFailed = false;
static bool UploadDeviceLocal(CVulkanBuffer*& dst, const void* data, VkDeviceSize size, VkBufferUsageFlags extraUsage);   // nfs21: used before its definition
// nfs21: per-tree FLOD group radius (largest group holding it) and the mean group radius, for the size-scaled cut.
struct FlodGroupRef { float R = 0.f; Fvector C{}; };
static std::unordered_map<const ::vkFTreeVisual*, FlodGroupRef> s_FlodGroupR;
static double s_FlodRSum = 0.0;
static u32    s_FlodRN = 0;

// Trees reachable THROUGH a billboard-backed FLOD. This is a relation, not a set of
// leaves, so it is the one thing the flat pass below cannot answer on its own  but
// only an FLOD can start it, so descending from the FLOD entries alone replaces a walk
// of all 450k visuals with one of ~92k.
static void MarkFlodBacked(::vkRender_Visual* v, xr_vector<::vkFTreeVisual*>& out)
{
    if (!v) return;
    const u32 t = v->Type;
    if (t == MT_TREE_ST || t == MT_TREE_PM) { out.push_back(static_cast<::vkFTreeVisual*>(v)); return; }
    if (t == MT_HIERRARHY || t == MT_LOD)
        for (auto* c : static_cast<::vkFHierrarhyVisual*>(v)->children) MarkFlodBacked(c, out);
}


// Crown alpha preload — defined next to the map it fills (see s_alphaCache), in
// the anonymous namespace this file keeps its bake helpers in.
namespace { void StartCrownAlphaPreload(const xr_vector<WorldMaterial*>& mats); void JoinCrownAlphaPreload(); }

// Wind class by diffuse TEXTURE NAME (tree materials don't expose alphaRef, and
// this "tree" path also carries non-foliage statics — vehicles, props, walls).
// Only real foliage gets wind:
//   2 = foliage (bend + flow-map flutter): under "trees\" and NOT bark/spil
//   1 = trunk   (gentle bend only): under "trees\" bark/spil (sways with crown)
//   0 = rigid   (no wind): everything else (veh\, prop\, mtl\, wood\, ...)
// Per MATERIAL, not per instance: as a per-instance test it lowercased the same
// few dozen names 193471 times.
static u8 WindClassOf(const WorldMaterial* wm)
{
    auto hasKw = [](const char* s, std::initializer_list<const char*> kws) {
        if (!s || !s[0]) return false;
        xr_string low = s; std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        for (auto* k : kws) if (low.find(k) != xr_string::npos) return true;
        return false;
    };
    const char* nm = "";
    if (wm) { const char* n = wm->name.c_str(); if (n) nm = n; }
    const bool underTrees = (strncmp(nm, "trees\\", 6) == 0) || (strncmp(nm, "trees/", 6) == 0);
    if (!underTrees) return 0u;
    return hasKw(nm, { "bark", "spil" }) ? 1u : 2u;   // trunk / cut-stump cross-section
}

// ----------------------------------------------------------------------------
// TREE TEXTURE LEDGER (always on, ~0 cost: one pass over the ~65 unique tree
// materials at load). It exists to answer ONE question with a number instead of
// a guess: how many BYTES PER TEXEL does a crown alpha fetch actually read?
//
// The whole "split the leaf alpha into a single-channel BC4 texture" family of
// optimizations rests on the diffuse being an 8 bpp format (BC3/BC7/RGBA8) — a
// 4 bpp BC4 alpha then halves what the depth prepass, the shadow caster and the
// motion pass read. If the diffuse is already BC1, a BC4 alpha is THE SAME 8
// bytes per 4x4 block and the idea has no mechanism at all. Four "obvious" tree
// optimizations have already died to a measurement; this one is decided at load
// for free, so it gets decided before anything is built.
//
// Weighted by INSTANCES, not by material count: one BC7 bush among 65 materials
// says nothing, the format of the texture on 2300 crowns says everything.
// ----------------------------------------------------------------------------
static const char* TreeTexFmtName(VkFormat f)
{
    switch (f) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:   return "BC1_RGB";
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:  return "BC1_RGBA";
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:       return "BC2";
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:       return "BC3";
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:      return "BC4";
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:      return "BC5";
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:       return "BC7";
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:        return "RGBA8";
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:        return "BGRA8";
        case VK_FORMAT_R8_UNORM:             return "R8";
        default:                             return "other";
    }
}

// Bits per texel of the image the sampler reads — the number the alpha-split
// question turns on. A BC4 alpha replacement is always 4 bpp.
static float TreeTexBpp(VkFormat f)
{
    // CVulkanTexture::GetBlockSize would say the same thing, but it is private —
    // and a table here cannot go stale behind an #include either way.
    switch (f) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:  case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:      case VK_FORMAT_BC4_SNORM_BLOCK:
            return 4.0f;                       // 8 bytes per 4x4 block
        case VK_FORMAT_BC2_UNORM_BLOCK:      case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:      case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:      case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:      case VK_FORMAT_BC7_SRGB_BLOCK:
            return 8.0f;                       // 16 bytes per 4x4 block
        case VK_FORMAT_R8_UNORM: return 8.0f;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return 32.0f;
        default: return 0.0f;   // unknown — reported separately, never averaged in
    }
}
// ============================================================================
// Build — main entry point. Called from CRender::level_Load after Visuals[]
// is populated.
// ============================================================================
void CTreeManager::Build()
{
    VK::Vram::Scope _vram_scope("Trees");
    if (m_bBuilt) return;
    s_UploadFailed = false;

    // Sub-timers: this pass is 1.70 s of a 21 s pripyat_full load (18-08). It walks
    // 450k visuals, forces a texture residency floor (which can RE-LOAD textures),
    // packs and uploads GPU arrays, builds meshlets and creates pipelines — four
    // different fixes depending on which one owns the time. Measure first.
    CTimer _t;
    float msExtract, msTexFloor = 0.f, msPack = 0.f, msUpload = 0.f, msMeshlet = 0.f, msPipes = 0.f;

    xr_vector<vkFTreeVisual*> trees;
    xr_vector<vkFTreeVisual*> flodBacked;   // reached through an FLOD with a real billboard
    s_FlodGroupR.clear(); s_FlodRSum = 0.0; s_FlodRN = 0;   // nfs21
    trees.reserve(2048);
    flodBacked.reserve(2048);
    _t.Start();
    // FLAT pass, same reasoning as WorldGPU::LeafVisuals: MT_HIERRARHY/MT_LOD children
    // are getVisual(id) results, i.e. entries of Visuals[] themselves, so every tree
    // visual is already a top-level entry and the old recursion only re-found them
    // (which is exactly what its own dedup pass was cleaning up afterwards).
    // Set XROS_LEAFWALK_FULL=1 to run the recursive reference implementation and
    // compare the two counts in the log.
    static const bool s_fullWalk = [] {
        const char* e = std::getenv("XROS_LEAFWALK_FULL");
        return e && e[0] && e[0] != '0';
    }();
    if (s_fullWalk) {
        for (IRenderVisual* iv : RImplementation.Visuals)
            ExtractFromVisual(static_cast<vkRender_Visual*>(iv), trees, flodBacked);
    } else {
        for (IRenderVisual* iv : RImplementation.Visuals) {
            auto* rv = static_cast<vkRender_Visual*>(iv);
            if (!rv) continue;
            if (rv->Type == MT_TREE_ST || rv->Type == MT_TREE_PM) {
                auto* tv = static_cast<::vkFTreeVisual*>(rv);
                if (!tv->m_mesh.p_rm_Vertices || !tv->m_mesh.p_rm_Indices) continue;
                if (tv->m_mesh.dwPrimitives == 0) continue;
                if (!tv->m_pWorldMaterial || tv->m_pWorldMaterial->view == VK_NULL_HANDLE) continue;
                trees.push_back(tv);
            }
            else if (rv->Type == MT_LOD) {
                // CLODManager's accept test is the authority for "this container really
                // draws a billboard"  mirror it exactly (vk_LODManager.cpp).
                auto* flod = static_cast<::vkFLOD*>(rv);
                if (!flod->facetsValid || flod->vis.sphere.R <= 0.01f) continue;
                // nfs21: remember the group radius of every tree under it (the largest, if several groups hold one:
                // its mesh then lasts to the farthest of their billboard starts -- overlap, never a hole).
                const size_t from = flodBacked.size();
                for (auto* c : flod->children) MarkFlodBacked(c, flodBacked);
                const float gR = flod->vis.sphere.R;
                for (size_t j = from; j < flodBacked.size(); ++j) {
                    FlodGroupRef& g = s_FlodGroupR[flodBacked[j]];
                    if (gR > g.R) { g.R = gR; g.C = flod->vis.sphere.P; }   // nfs23: centre of that same group
                }
                s_FlodRSum += gR; ++s_FlodRN;
            }
        }
    }
    msExtract = _t.GetElapsed_ms_total();
    VK::g_TreeLodRref = s_FlodRN ? float(s_FlodRSum / double(s_FlodRN)) : 0.f;   // nfs21: mean FLOD group radius

    // Sorted+uniqued so the per-tree lookup below is a binary search. Must happen
    // BEFORE `trees` is re-sorted for grouping — these are independent orderings.
    std::sort(flodBacked.begin(), flodBacked.end());
    flodBacked.erase(std::unique(flodBacked.begin(), flodBacked.end()), flodBacked.end());

    // Dedup: MT_HIERRARHY/MT_LOD children are getVisual(id) REFERENCES into
    // Visuals[], so the same tree object gets collected both as a top-level
    // entry and via container recursion. Rendering it 2-3× overlaps alpha-tested
    // foliage (z-fight → "moth-eaten"/floating leaf cards). Keep one per object.
    {
        const u32 before = (u32)trees.size();
        std::sort(trees.begin(), trees.end());
        trees.erase(std::unique(trees.begin(), trees.end()), trees.end());
        if (before != (u32)trees.size())
            Msg("[VK Trees] Dedup: %u -> %u unique tree visuals", before, (u32)trees.size());
    }

    if (trees.empty())
    {
        // Diagnostic dump: tally MT_* types across Visuals[] so we can spot
        // when a level uses an unexpected wrapping (e.g. trees baked as
        // MT_NORMAL inside MT_LOD containers we never recurse through).
        u32 typeCount[32]{};
        for (IRenderVisual* iv : RImplementation.Visuals) {
            auto* v = static_cast<vkRender_Visual*>(iv);
            if (!v) continue;
            const u32 t = v->Type;
            if (t < 32) typeCount[t]++;
        }
        Msg("[VK Trees] No MT_TREE_ST/PM visuals — top-level type tally:");
        for (u32 t = 0; t < 32; ++t)
            if (typeCount[t]) Msg("[VK Trees]   type=%u : %u", t, typeCount[t]);
        m_bBuilt = true;
        return;
    }

    // ----- Sort by (tcOffset, diffuse, vb, ib) so consecutive trees share
    // pipeline + descriptor + buffer binds.
    // Timed: the comparator chases FOUR pointers per comparison into 193k visuals
    // scattered across the heap, O(n log n) times. texFloor next door pays the same
    // chase once per instance and measures 75 ms for what is a pointer compare and
    // an array write -- so the chase, not the work, is what this phase costs.
    _t.Start();
    std::sort(trees.begin(), trees.end(),
        [](vkFTreeVisual* a, vkFTreeVisual* b) {
            if (a->m_mesh.tcOffset != b->m_mesh.tcOffset) return a->m_mesh.tcOffset < b->m_mesh.tcOffset;
            if (a->m_pWorldMaterial != b->m_pWorldMaterial)
                return a->m_pWorldMaterial < b->m_pWorldMaterial;
            const VkBuffer av = a->m_mesh.p_rm_Vertices->GetHandle();
            const VkBuffer bv = b->m_mesh.p_rm_Vertices->GetHandle();
            if (av != bv) return av < bv;
            return a->m_mesh.p_rm_Indices->GetHandle() < b->m_mesh.p_rm_Indices->GetHandle();
        });
    const float msSortTrees = _t.GetElapsed_ms_total();

    // ----- Collect unique diffuse views and assign per-tree desc index.
    xr_vector<VkImageView> uniqueViews;
    uniqueViews.reserve(64);
    xr_vector<u32> treeTexIdx(trees.size(), 0u);
    _t.Start();
    // Everything here is per MATERIAL -- raising the residency floor, freezing the
    // streaming state and picking the descriptor slot all belong to the material,
    // not the instance. 193471 tree instances share 65 of them.
    //
    // Three passes instead of one interleaved walk, because the middle step wants
    // the whole set at once: ApplyResidencyPlan ends in a transfer wait, so the
    // old per-material call waited 65 times (73 ms of pure queue round trips).
    CTimer _tf;
    float msGather = 0, msResidency = 0;

    // (1) Material pointer per tree, on the idle cores: one scattered visual each.
    _tf.Start();
    xr_vector<WorldMaterial*> treeMat(trees.size(), nullptr);
    ParallelChunks((int)trees.size(), [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) treeMat[i] = trees[i]->m_pWorldMaterial;
    });
    // Dense material index in FIRST-APPEARANCE order — the order the view slots
    // are handed out in, so the descriptor table comes out exactly as before.
    xr_vector<WorldMaterial*> uniqMat; uniqMat.reserve(128);
    xr_vector<u32> treeMatIdx(trees.size(), 0u);
    {
        std::unordered_map<const WorldMaterial*, u32> seen;
        const WorldMaterial* lastMat = nullptr;
        u32 lastIdx = 0;
        for (size_t i = 0; i < trees.size(); ++i) {
            WorldMaterial* const wmi = treeMat[i];
            if (wmi != lastMat) {
                if (const auto it = seen.find(wmi); it != seen.end()) lastIdx = it->second;
                else { lastIdx = (u32)uniqMat.size(); uniqMat.push_back(wmi); seen.emplace(wmi, lastIdx); }
                lastMat = wmi;
            }
            treeMatIdx[i] = lastIdx;
        }
    }
    msGather = _tf.GetElapsed_ms_total();

    // The crown bake at the end of this function needs one alpha map per species.
    // Start that load NOW, on its own thread: everything between here and the bake
    // is texture residency, packing and uploads, none of which touch these files.
    const bool crownBake = NxCrownBakeNeeded();   // nfs42: decided ONCE, the preload and the bake must agree
    if (crownBake)
        StartCrownAlphaPreload(uniqMat);

    // (2) ONE residency order for the whole set, one wait.
    // This instanced path bakes the RAW VkImageView into its own descriptor sets
    // for the whole level lifetime — a dynamic mip swap would retire and destroy
    // that view under us (white/transparent trunks), and this path's FS writes no
    // streaming feedback, so the streamer would judge these textures "invisible"
    // and demote them while they fill the screen. So: raise anything the load-time
    // cap crushed to a presentable floor (safe: nothing captured our view yet),
    // THEN opt out of streaming for good — residency is frozen from here on.
    // 1024px floor: this path also carries building walls/vehicles — at the earlier
    // 512 floor large facades still read as mush (frozen forever).
    _tf.Start();
    {
        xr_vector<CVulkanTexture*> texs; texs.reserve(uniqMat.size());
        for (WorldMaterial* wm : uniqMat) if (wm && wm->tex) texs.push_back(wm->tex);
        if (!texs.empty())
            VK::TextureStreamer::Instance().EnsureMinResidencyBatch(texs.data(), texs.size(), 1024);
    }
    msResidency = _tf.GetElapsed_ms_total();

    // (3) Freeze + view slots + wind class, per unique material. The view is read
    // AFTER the residency batch: that is what may have swapped it.
    xr_vector<u32> matSlot(uniqMat.size(), 0u);
    xr_vector<u8>  matWind(uniqMat.size(), 0u);
    for (u32 m = 0; m < (u32)uniqMat.size(); ++m) {
        WorldMaterial* wm = uniqMat[m];
        if (!wm) continue;
        VK::TextureStreamer::Instance().SetStreamable(wm->tex, false);
        wm->streamID = 0xFFFFFFFFu;   // stale feedback slot must not be pushed by the world pass

        const VkImageView view = wm->view;
        u32 idx = ~0u;
        for (u32 j = 0; j < uniqueViews.size(); ++j)
            if (uniqueViews[j] == view) { idx = j; break; }
        if (idx == ~0u) { idx = (u32)uniqueViews.size(); uniqueViews.push_back(view); }
        matSlot[m] = idx;
        matWind[m] = WindClassOf(wm);   // per material: the pack loop just reads it
    }

    xr_vector<u8> treeWind(trees.size(), 0u);
    ParallelChunks((int)trees.size(), [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            const u32 m = treeMatIdx[i];
            treeTexIdx[i] = matSlot[m];
            treeWind[i]   = matWind[m];
        }
    });

    msTexFloor = _t.GetElapsed_ms_total();
    Msg("[load step]   Trees::texFloor: gather %.0f | residency %.0f (%u materials, %u views) | slots %.0f ms",
        msGather, msResidency, (u32)uniqMat.size(), (u32)uniqueViews.size(),
        msTexFloor - msGather - msResidency);
    // Which half of that residency wait is which: rebuilding the images (DDS
    // re-read + create + staged copy, serial on this thread) or the transfer wait.
    Msg("[load step]   Trees::texFloor residency split: %u rebuilt | parallel read %.0f | build %.0f | flush %.0f | swap %.0f ms",
        VK::g_lastResidencyBuilt, VK::g_lastResidencyReadMs, VK::g_lastResidencyBuildMs,
        VK::g_lastResidencyFlushMs, VK::g_lastResidencySwapMs);
    Msg("[load step]     of the build: read %.0f | repack %.0f | create %.0f | upload %.0f | close %.0f ms",
        VK::g_lastResidencyOpenMs, VK::g_lastResidencyRepackMs, VK::g_lastResidencyCreateMs,
        VK::g_lastResidencyUploadMs, VK::g_lastResidencyCloseMs);

    // ----- TREE TEXTURE LEDGER (see TreeTexFmtName above for why this exists).
    // Runs on the frozen, residency-floored images, i.e. exactly what the sampler
    // will read for the level's lifetime — not what the .dds file happens to say.
    {
        struct FmtRow { VkFormat fmt; u32 mats; u32 inst; double bytes; };
        xr_vector<FmtRow> rows;
        xr_vector<u32>    matInst(uniqMat.size(), 0u);
        for (size_t i = 0; i < trees.size(); ++i) ++matInst[treeMatIdx[i]];

        double instBitsNow = 0.0, instTotal = 0.0, unknownInst = 0.0;
        for (u32 m = 0; m < (u32)uniqMat.size(); ++m) {
            WorldMaterial* wm = uniqMat[m];
            CVulkanTexture* tx = wm ? wm->tex : nullptr;
            if (!tx) continue;
            const VkFormat f = tx->GetFormat();
            const float    bpp = TreeTexBpp(f);
            // Whole mip chain, the way it sits in VRAM (4/3 of the base level).
            double bytes = (double)tx->GetWidth() * tx->GetHeight() * bpp / 8.0;
            if (tx->GetMipLevels() > 1) bytes *= 4.0 / 3.0;

            FmtRow* row = nullptr;
            for (FmtRow& r : rows) if (r.fmt == f) { row = &r; break; }
            if (!row) { rows.push_back({ f, 0u, 0u, 0.0 }); row = &rows.back(); }
            ++row->mats;
            row->inst  += matInst[m];
            row->bytes += bytes;

            instTotal += matInst[m];
            if (bpp > 0.0f) instBitsNow += (double)matInst[m] * bpp;
            else            unknownInst += matInst[m];
        }

        std::sort(rows.begin(), rows.end(), [](const FmtRow& a, const FmtRow& b) { return a.inst > b.inst; });
        double totalBytes = 0.0;
        for (const FmtRow& r : rows) totalBytes += r.bytes;
        Msg("[VK TreeTex] %u materials / %u views | %u instances | %.1f MB resident",
            (u32)uniqMat.size(), (u32)uniqueViews.size(), (u32)trees.size(), totalBytes / (1024.0 * 1024.0));
        for (const FmtRow& r : rows)
            Msg("[VK TreeTex]   %-9s %4.1f bpp | %3u mats | %6u inst (%5.1f%%) | %6.1f MB",
                TreeTexFmtName(r.fmt), TreeTexBpp(r.fmt), r.mats, r.inst,
                instTotal > 0.0 ? 100.0 * r.inst / instTotal : 0.0, r.bytes / (1024.0 * 1024.0));

        // THE VERDICT LINE. The alpha-only passes (depth prepass, shadow caster,
        // motion vectors) read one channel out of whatever the diffuse is. A BC4
        // alpha twin costs a flat 4 bpp, so the saving per instance is
        // max(0, bpp_now - 4) — and it is ZERO for anything already at BC1.
        double bc4Bits = 0.0;
        for (u32 m = 0; m < (u32)uniqMat.size(); ++m) {
            WorldMaterial* wm = uniqMat[m];
            CVulkanTexture* tx = wm ? wm->tex : nullptr;
            if (!tx) continue;
            const float bpp = TreeTexBpp(tx->GetFormat());
            if (bpp > 0.0f) bc4Bits += (double)matInst[m] * _min(bpp, 4.0f);
        }
        const double now = instTotal > 0.0 ? instBitsNow / _max(1.0, instTotal - unknownInst) : 0.0;
        const double bc4 = instTotal > 0.0 ? bc4Bits    / _max(1.0, instTotal - unknownInst) : 0.0;
        Msg("[VK TreeTex] alpha fetch reads %.1f bpp per instance (inst-weighted); a BC4 alpha twin would read %.1f"
            " => %.0f%% less bandwidth in the alpha-only passes%s",
            now, bc4, now > 0.0 ? 100.0 * (1.0 - bc4 / now) : 0.0,
            (now - bc4) < 0.25 ? " => NO MECHANISM, do not build it" : "");
        if (unknownInst > 0.0)
            Msg("[VK TreeTex] %u instances on formats with no bpp entry - excluded from the verdict", (u32)unknownInst);
    }

    // ----- Pack per-instance + per-mesh GPU arrays.
    _t.Start();
    m_TotalCount = (u32)trees.size();
    xr_vector<GpuTreeMeta>     meta(m_TotalCount);
    xr_vector<GpuTreeInstance> xforms(m_TotalCount);
    m_WindClassCPU.assign(m_TotalCount, 0);

    // Index-independent: entry i reads its own visual and writes meta[i],
    // xforms[i], m_WindClassCPU[i]. flodBacked/treeWind are read-only here. The
    // cost is the pointer chase into 193k scattered visuals, which is exactly
    // what spreads well across cores.
    ParallelChunks((int)m_TotalCount, [&](int lo, int hi) {
    for (u32 i = (u32)lo; i < (u32)hi; ++i)
    {
        vkFTreeVisual* t = trees[i];
        GpuTreeMeta& m   = meta[i];
        m.sphere_P     = t->vis.sphere.P;
        m.sphere_R     = t->vis.sphere.R;
        m.index_count  = t->m_mesh.dwPrimitives * 3;
        m.ib_first     = t->m_mesh.iBase;       // element offset into the IB pool
        m.first_vertex = t->m_mesh.vBase;       // element offset into the VB pool
        m.flags        = std::binary_search(flodBacked.begin(), flodBacked.end(), t)
                       ? TREE_FLOD_BACKED : 0u;
        // nfs21: the size-scaled cut factor of this tree's FLOD group, 1/64 steps in bits 24..31 (0 = 1.0)
        if (m.flags & TREE_FLOD_BACKED) {
            auto it = s_FlodGroupR.find(t);
            if (it != s_FlodGroupR.end()) {
                const u32 q = (u32)iFloor(VK::TreeLodKRaw(it->second.R) * 64.f + 0.5f);
                m.flags |= (q & 255u) << 24;
            }
        }

        // Progressive trees (MT_TREE_PM): draw the finest sliding window sw[0]
        // (== R4 select_lod_id at closest range), NOT the full container. The
        // container's first ~80% of indices are progressive vsplit/collapse
        // records; drawing them as a flat triangle list yields long stretched
        // degenerate triangles ("streaky branches in the air"). sw[0] is the
        // clean full-detail crown ([iBase+offset, iBase+offset+num_tris*3]).
        if (t->Type == MT_TREE_PM)
        {
            auto* pm = static_cast<vkFTreeVisual_PM*>(t);
            if (pm->sw_count > 0)
            {
                m.index_count = pm->sw_counts[0];
                m.ib_first    = t->m_mesh.iBase + pm->sw_offsets[0];
            }
        }

        GpuTreeInstance& x = xforms[i];
        x.xform        = t->xform;
        x.c_scale_hemi = t->c_scale.hemi;
        x.c_bias_hemi  = t->c_bias.hemi;
        // Wind class is the MATERIAL's (see WindClassOf) — decided once per
        // material in the texture-floor pass above, not per instance.
        x._pad0 = treeWind[i];
        x._pad1 = 0;
        m_WindClassCPU[i] = (u8)treeWind[i];   // hull tier gates on foliage (crowns-only)
    }
    });

    // How much of the forest the r_tree_dist cut can even touch. A low share here
    // means most trees have no billboard stand-in, so raising r_tree_dist won't
    // help and the honest next step is a real tree LOD, not a bigger cut.
    {
        u32 backed = 0;
        for (const GpuTreeMeta& m : meta) if (m.flags & TREE_FLOD_BACKED) ++backed;
        m_FlodBackedCount = backed;
        Msg("[VK Trees] FLOD-backed: %u of %u instances (%.1f%%) have a billboard stand-in — r_tree_dist can drop these past its range",
            backed, m_TotalCount, m_TotalCount ? 100.0 * double(backed) / double(m_TotalCount) : 0.0);
    }

    // ----- ASSET LEDGER (load-time half of r_tree_stats) ----------------------
    // The per-frame ledger says how many triangles the canopy costs; this says WHERE
    // they come from, and it is the half a runtime counter cannot answer: a frame
    // counter can only ever report the total, while the fix — if the answer turns out
    // to be "one species is half the forest" — is made in the asset, not in the
    // renderer. Printed unconditionally: it costs one sort at load, it never repeats,
    // and it is exactly the table you want in the log of a run you already made.
    {
        struct Uniq { u64 key; u32 tris; u32 count; u32 firstTree; };
        xr_vector<std::pair<u64, u32>> keys;   // (mesh key, instance index)
        keys.reserve(m_TotalCount);
        u64 levelTris = 0;
        u32 hist[8] = {};                       // instances by triangle count, octaves from 128
        for (u32 i = 0; i < m_TotalCount; ++i) {
            const GpuTreeMeta& m = meta[i];
            const u32 tris = m.index_count / 3u;
            levelTris += tris;
            // Identity of a MESH, not of an instance: the same species placed 900 times
            // shares one (first index, index count) pair in the shared pools.
            keys.emplace_back(((u64)m.ib_first << 32) | (u64)m.index_count, i);
            u32 b = 0; for (u32 t = tris; t >= 256u && b < 7u; t >>= 1) ++b;
            hist[b] += 1;
        }
        std::sort(keys.begin(), keys.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        xr_vector<Uniq> uniq; uniq.reserve(512);
        for (size_t i = 0; i < keys.size(); ) {
            size_t j = i;
            while (j < keys.size() && keys[j].first == keys[i].first) ++j;
            const GpuTreeMeta& m = meta[keys[i].second];
            uniq.push_back({ keys[i].first, m.index_count / 3u, (u32)(j - i), keys[i].second });
            i = j;
        }
        // ⭐ Sorted by TOTAL triangles on the level (tris × instances), not by mesh
        // size. A 40 k-triangle hero tree placed twice is a rounding error; a
        // 900-triangle bush placed 30 000 times is the forest.
        std::sort(uniq.begin(), uniq.end(), [](const Uniq& a, const Uniq& b) {
            return (u64)a.tris * a.count > (u64)b.tris * b.count;
        });

        Msg("[VK TreeAssets] %u unique meshes, %u instances, %.2f M triangles if the whole level were in frame",
            (u32)uniq.size(), m_TotalCount, double(levelTris) / 1e6);
        const u32 nTop = _min((u32)uniq.size(), 10u);
        for (u32 k = 0; k < nTop; ++k) {
            const Uniq& u = uniq[k];
            // ⚠ The DIFFUSE TEXTURE name, not the visual's. Level trees come out of
            // level.geom rather than their own .ogf, so dbg_name is empty for every one
            // of them — the first version of this table printed ten rows of "<unnamed>"
            // and was useless for the one thing it exists to do: name the asset. The
            // crown texture identifies the species just as well and is always there.
            const char* nm = nullptr;
            if (u.firstTree < treeMatIdx.size()) {
                const u32 mi = treeMatIdx[u.firstTree];
                if (mi < uniqMat.size() && uniqMat[mi]) nm = uniqMat[mi]->name.c_str();
            }
            Msg("[VK TreeAssets]  %2u. %-40s %5u tris x %5u inst = %6.2f M (%.1f%% of the level)",
                k + 1, nm ? nm : "<unnamed>", u.tris, u.count,
                double((u64)u.tris * u.count) / 1e6,
                levelTris ? 100.0 * double((u64)u.tris * u.count) / double(levelTris) : 0.0);
        }
        Msg("[VK TreeAssets] instances by mesh weight: <256:%u 256-512:%u .5-1k:%u 1-2k:%u 2-4k:%u 4-8k:%u 8-16k:%u >16k:%u",
            hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
    }

    // ----- Group consecutive trees with same (tcOffset, descSet, vb, ib, stride).
    m_Groups.clear();
    m_Groups.reserve(64);
    m_MaxGroupMeshCount = 0;
    {
        TreeIndirectGroup cur{};
        auto seedFrom = [&](u32 i) {
            cur.vb         = trees[i]->m_mesh.p_rm_Vertices->GetHandle();
            cur.ib         = trees[i]->m_mesh.p_rm_Indices->GetHandle();
            cur.stride     = trees[i]->m_mesh.vStride;
            cur.tcOffset   = trees[i]->m_mesh.tcOffset;
            cur.descSetIdx = treeTexIdx[i];
            cur.meshOffset = i;
            cur.meshCount  = 1;
        };
        seedFrom(0);
        for (u32 i = 1; i < m_TotalCount; ++i)
        {
            const VkBuffer vb = trees[i]->m_mesh.p_rm_Vertices->GetHandle();
            const VkBuffer ib = trees[i]->m_mesh.p_rm_Indices->GetHandle();
            const u32 stride  = trees[i]->m_mesh.vStride;
            const u32 tcOff   = trees[i]->m_mesh.tcOffset;
            const u32 desc    = treeTexIdx[i];
            if (vb == cur.vb && ib == cur.ib && stride == cur.stride &&
                tcOff == cur.tcOffset && desc == cur.descSetIdx)
            {
                cur.meshCount++;
            }
            else
            {
                if (cur.meshCount > m_MaxGroupMeshCount) m_MaxGroupMeshCount = cur.meshCount;
                m_Groups.push_back(cur);
                seedFrom(i);
            }
        }
        if (cur.meshCount > m_MaxGroupMeshCount) m_MaxGroupMeshCount = cur.meshCount;
        m_Groups.push_back(cur);
    }

    // ----- Per-group enclosing sphere, for the GPU-culled draw paths.
    // An indirect draw is issued per group whether the cull left anything in it or not
    // (the count is on the device), so a shadow target with ~100 survivors was paying
    // 161 zero-count draws. This bound lets the record loop skip a group outright.
    // Centre = midpoint of the AABB over the member spheres, radius = the largest
    // distance from it to any member's far side. That is a genuine ENCLOSING sphere:
    // reject it and no tree inside could have passed, so the skip cannot drop a caster
    // the GPU kept. Loose is fine (it only costs a draw that would have been empty);
    // WRONG is not, which is why the radius is measured to the far side, not the centre.
    for (TreeIndirectGroup& g : m_Groups)
    {
        if (!g.meshCount) { g.bsR = 0.f; continue; }
        Fvector lo = meta[g.meshOffset].sphere_P, hi = lo;
        for (u32 i = 0; i < g.meshCount; ++i) {
            const GpuTreeMeta& m = meta[g.meshOffset + i];
            const float r = _max(0.f, m.sphere_R);
            lo.x = _min(lo.x, m.sphere_P.x - r); hi.x = _max(hi.x, m.sphere_P.x + r);
            lo.y = _min(lo.y, m.sphere_P.y - r); hi.y = _max(hi.y, m.sphere_P.y + r);
            lo.z = _min(lo.z, m.sphere_P.z - r); hi.z = _max(hi.z, m.sphere_P.z + r);
        }
        g.bsP.lerp(lo, hi, 0.5f);
        float rr = 0.f;
        for (u32 i = 0; i < g.meshCount; ++i) {
            const GpuTreeMeta& m = meta[g.meshOffset + i];
            rr = _max(rr, g.bsP.distance_to(m.sphere_P) + _max(0.f, m.sphere_R));
        }
        g.bsR = rr;
    }

    msPack = _t.GetElapsed_ms_total();

    // ----- Upload + allocate.
    _t.Start();
    UploadTransforms(xforms);
    msUpload = _t.GetElapsed_ms_total();
    // Out of VRAM: stop here rather than build pipelines around buffers that do not
    // exist. The level still loads — it just renders without trees, and the log says
    // why. Previously this path dereferenced a null buffer and took the process down.
    if (s_UploadFailed) {
        JoinCrownAlphaPreload();   // a joinable static thread terminates at exit
        Msg("!![VK Trees] DISABLED for this level: tree buffers did not fit in VRAM "
            "(%u instances). Lower texture quality / r_bump 0, or free VRAM.", m_TotalCount);
        return;
    }
    _t.Start();
    if (!crownBake)
        Msg("[VK Trees] crown bake skipped (r_tree_crown_bake 0): r_vsm 0, no cube tier -- nothing reads meshlets/hulls/voxels");
    else
    BuildMeshlets(trees, meta);   // Phase A: VSM meshlet-cull clusters (r_vsm_meshlet)
    msMeshlet = _t.GetElapsed_ms_total();
    // METADATA RIDES *AFTER* THE CROWN BAKE. BuildMeshlets stamps TREE_VOX_BACKED into
    // `meta`, and that bit may only claim cubes the bake actually produced: a tree
    // flagged without a cloud behind it gets cut from the mesh path with nothing to
    // replace it — an invisible tree. Uploading here is also what keeps m_MetaCPU (the
    // CPU cull in RenderDepth) and the GPU metadata buffer agreeing on the flags.
    _t.Start();
    UploadMetadata(meta);
    // nfs21: sliding-window tables of the progressive crowns (R4 FTreeVisual_PM). One table per distinct
    // (index base, window array); every tree points at its table: base | count << 24.
    {
        xr_vector<u32> swiInst(size_t(m_TotalCount) * 4u, 0u);   // nfs23: uvec4 per tree
        // The crown's crossfade input (tree.vert): the same group centre and the same quantised k (q/64) the cull cuts
        // the mesh by and the billboard fades in by, so the two dithers are exact complements. k = 0: the tree has no
        // billboard group, it never fades.
        xr_vector<Fvector4> lodC(m_TotalCount);
        m_TreeGroupC.assign(m_TotalCount, Fvector{});
        for (u32 i = 0; i < m_TotalCount; ++i) {
            Fvector gc = trees[i]->vis.sphere.P;
            auto git = s_FlodGroupR.find(trees[i]);
            if (git != s_FlodGroupR.end()) gc = git->second.C;
            m_TreeGroupC[i] = gc;
            memcpy(&swiInst[size_t(i) * 4u], &gc, sizeof(float) * 3);
            const u32 q = (meta[i].flags & TREE_FLOD_BACKED) ? ((meta[i].flags >> 24) & 255u) : 0u;
            lodC[i].set(gc.x, gc.y, gc.z, float(q) / 64.f);
        }
        UploadDeviceLocal(m_TreeLodCBuffer, lodC.data(), lodC.size() * sizeof(Fvector4), 0);
        xr_vector<u32> swiWin;   // pairs: first index, index count
        std::unordered_map<u64, u32> tables;
        u32 withWin = 0;
        for (u32 i = 0; i < m_TotalCount; ++i) {
            vkFTreeVisual* t = trees[i];
            if (t->Type != MT_TREE_PM) continue;
            auto* pm = static_cast<vkFTreeVisual_PM*>(t);
            if (pm->sw_count < 2 || pm->sw_count > 255 || !pm->sw_offsets || !pm->sw_counts) continue;
            const u64 key = (u64(t->m_mesh.iBase) << 32) ^ u64(uintptr_t(pm->sw_offsets));
            auto it = tables.find(key);
            u32 base;
            if (it == tables.end()) {
                base = (u32)(swiWin.size() / 2);
                for (u32 k = 0; k < pm->sw_count; ++k) {
                    swiWin.push_back(t->m_mesh.iBase + pm->sw_offsets[k]);
                    swiWin.push_back(pm->sw_counts[k]);
                }
                tables.emplace(key, base);
            } else base = it->second;
            if (base > 0xFFFFFFu) continue;
            swiInst[size_t(i) * 4u + 3u] = base | (pm->sw_count << 24);
            ++withWin;
        }
        if (swiWin.empty()) { swiWin.push_back(0u); swiWin.push_back(0u); }
        UploadDeviceLocal(m_SwiInstBuffer, swiInst.data(), swiInst.size() * sizeof(u32), 0);
        UploadDeviceLocal(m_SwiWinBuffer, swiWin.data(), swiWin.size() * sizeof(u32), 0);
        Msg("[VK Trees] nfs21 LOD: %u of %u trees have sliding windows (%u tables, %u windows) | mean FLOD radius %.2f m over %u groups",
            withWin, m_TotalCount, (u32)tables.size(), (u32)(swiWin.size() / 2), VK::g_TreeLodRref, s_FlodRN);
    }
    msUpload += _t.GetElapsed_ms_total();
    if (s_UploadFailed) {
        JoinCrownAlphaPreload();
        Msg("!![VK Trees] DISABLED for this level: tree metadata did not fit in VRAM "
            "(%u instances).", m_TotalCount);
        return;
    }
    _t.Start();
    CreateIndirectBuffers();
    CreateTextureDescriptors(uniqueViews);
    // r_vsm_tree_impostor: bake per-mesh crown silhouettes — ONLY when the (parked,
    // default-0) feature is enabled AT LOAD. Off = nothing baked/allocated at all, so
    // the impostor is provably inert (it never touched bushes anyway — R<3m visuals are
    // excluded from the near/dyn set in VsmUpdateNearSet). Toggling it on now needs the
    // cvar set before the level loads (reload), which is fine for a parked debug path.
    if (ps_r_vsm_tree_impostor)
        BuildImpostorAtlas(trees, meta, treeTexIdx);

    // ----- Session B: compute-cull + graphics pipelines + per-frame resources.
    CreateFrustumUBO();
    CreateCullPipeline();
    CreateXformDescriptor();
    CreateGIPass();            // per-tree probe field (r_tree_gi_inst) — needs the buffer CreateXformDescriptor made
    CreateGfxPipelines();
    msPipes = _t.GetElapsed_ms_total();

    Msg("[VK Trees] Built: %u instances in %u groups, %u textures, max group %u",
        m_TotalCount, (u32)m_Groups.size(), (u32)uniqueViews.size(), m_MaxGroupMeshCount);
    Msg("[load step]   Trees::Build: extract %.0f | sort %.0f | texFloor %.0f | pack %.0f | upload %.0f | meshlets %.0f | indirect+desc+pipes %.0f ms",
        msExtract, msSortTrees, msTexFloor, msPack, msUpload, msMeshlet, msPipes);

    m_bBuilt = true;
}

// ============================================================================
// Upload helpers — staging via host-visible TRANSFER_SRC + one-shot copy.
// Same pattern as CDetailManager::BakeHeightmap (host stage → cmd copy).
// ============================================================================
// Returns false when VRAM ran out. ⚠ EVERY caller must respect that: `Create` is
// void and VK_CHECK only LOGS, so a failed allocation leaves VK_NULL_HANDLE behind
// and the old code went on to vkCmdCopyBuffer with it — which is how an
// out-of-memory condition became a c0000005 inside the driver instead of a message
// (25-07: Pripyat crashed on load, `Vulkan error: -2` seven lines up in the log).
// The level is at the VRAM ceiling on big maps; refusing the upload is survivable,
// dereferencing a null buffer is not.
static bool UploadDeviceLocal(CVulkanBuffer*& dst, const void* data, VkDeviceSize size,
                              VkBufferUsageFlags extraUsage)
{
    dst = xr_new<CVulkanBuffer>();
    // gpuOnly=true: without it the storage-usage default requests host access and VMA
    // places the buffer in BAR/system memory — tree META is read by every thread of
    // the 193k-thread bins, so that means PCIe per warp (the Pripyat Bins/Tree* burn).
    dst->Create(size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extraUsage,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    if (dst->GetHandle() == VK_NULL_HANDLE) {
        Msg("!![VK Trees] upload FAILED: no VRAM for a %llu KB device buffer",
            (unsigned long long)(size >> 10));
        return false;
    }

    CVulkanBuffer staging;
    staging.Create(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    void* mapped = (staging.GetHandle() != VK_NULL_HANDLE) ? staging.Map() : nullptr;
    if (!mapped) {
        Msg("!![VK Trees] upload FAILED: no staging for %llu KB (host/VRAM full)",
            (unsigned long long)(size >> 10));
        staging.Destroy();
        return false;
    }
    memcpy(mapped, data, size);
    staging.Flush();

    bool ok = false;
    VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
    if (cmd != VK_NULL_HANDLE)
    {
        VkBufferCopy cp{ 0, 0, size };
        vkCmdCopyBuffer(cmd, staging.GetHandle(), dst->GetHandle(), 1, &cp);
        VulkanHW.EndSingleTimeCommands(cmd);
        ok = true;
    }
    staging.Destroy();
    return ok;
}

void CTreeManager::UploadMetadata(const xr_vector<GpuTreeMeta>& meta)
{
    if (!UploadDeviceLocal(m_TreeMetadataBuffer, meta.data(),
                           meta.size() * sizeof(GpuTreeMeta), 0))
        s_UploadFailed = true;
    m_MetaCPU = meta;   // kept for the CPU-culled shadow caster path (RenderDepth)
}

// ============================================================================
//  RT world exports (vk_rt_world) — see the block comment in vk_TreeManager.h.
// ============================================================================
bool CTreeManager::RTGroup(u32 g, VkBuffer& vb, VkBuffer& ib, u32& stride, u32& tcOffset,
                           u32& meshOffset, u32& meshCount, VkImageView& view) const
{
    if (g >= m_Groups.size()) return false;
    const TreeIndirectGroup& grp = m_Groups[g];
    vb = grp.vb; ib = grp.ib; stride = grp.stride; tcOffset = grp.tcOffset;
    meshOffset = grp.meshOffset; meshCount = grp.meshCount;
    view = (grp.descSetIdx < m_TexViews.size()) ? m_TexViews[grp.descSetIdx] : VK_NULL_HANDLE;
    return true;
}

VkBuffer CTreeManager::RTMetaBuffer() const
{
    return m_TreeMetadataBuffer ? m_TreeMetadataBuffer->GetHandle() : VK_NULL_HANDLE;
}

VkBuffer CTreeManager::RTXformBuffer() const
{
    return m_TreeTransformsBuffer ? m_TreeTransformsBuffer->GetHandle() : VK_NULL_HANDLE;
}

void CTreeManager::UploadTransforms(const xr_vector<GpuTreeInstance>& xforms)
{
    if (!UploadDeviceLocal(m_TreeTransformsBuffer, xforms.data(),
                           xforms.size() * sizeof(GpuTreeInstance), 0))
        s_UploadFailed = true;
}

// ============================================================================
// Phase A — meshlet clusters for the VSM per-page meshlet-cull path.
// ⛔ NO NET PERF GAIN on dGPU (2026-07-02) — VSMrender is fill-bound, cutting vertices didn't help.
// KEPT (correct, picture identical) but DISABLED via r_vsm_meshlet=0. Don't re-chase; see the cvar
// block in vk_console_min.cpp + memory [[vulkan-vsm-meshlet-plan]]. Possible future value on iGPU.
// ============================================================================
namespace {

// Triangles per meshlet cluster (tunable — Phase C sweeps 64/128/256).
constexpr u32 kMeshletTris = 128;

// 10-bit Morton (Z-order) — spatial sort so a meshlet's triangles stay compact.
inline u32 Part1By2(u32 n) {
    n &= 0x3ff;
    n = (n | (n << 16)) & 0x030000ff;
    n = (n | (n <<  8)) & 0x0300f00f;
    n = (n | (n <<  4)) & 0x030c30c3;
    n = (n | (n <<  2)) & 0x09249249;
    return n;
}
inline u32 Morton3(u32 x, u32 y, u32 z) { return Part1By2(x) | (Part1By2(y) << 1) | (Part1By2(z) << 2); }

// GPU-readback of a device-local sub-range into a freshly-allocated host vector.
// Pools carry TRANSFER_SRC (rvk_loader). Returns false on any failure.
bool ReadbackRange(VkBuffer src, VkDeviceSize offset, VkDeviceSize size, xr_vector<u8>& out)
{
    if (src == VK_NULL_HANDLE || size == 0) return false;
    CVulkanBuffer stg;
    stg.Create(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
               false, false, /*hostRead*/ true);   // cached: this mapping is READ
    VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) { stg.Destroy(); return false; }
    VkBufferCopy cp{ offset, 0, size };
    vkCmdCopyBuffer(cmd, src, stg.GetHandle(), 1, &cp);
    VulkanHW.EndSingleTimeCommands(cmd);   // waits (one-shot)
    stg.Invalidate();
    void* p = stg.Map();
    if (!p) { stg.Destroy(); return false; }
    out.resize((size_t)size);
    memcpy(out.data(), p, (size_t)size);
    stg.Unmap();
    stg.Destroy();
    return true;
}

// Batched sibling of ReadbackRange: ONE host staging buffer, ONE command buffer, ONE
// submit+wait for the whole set. BuildMeshlets used to call ReadbackRange twice per
// unique mesh — 568 allocate/submit/fence-wait round trips to move 21 MB, measured at
// 256 ms of the 18-08 load. The bytes are the same; the round trips are not.
struct RbRange { VkBuffer src; VkDeviceSize off, size; size_t dst; };

bool ReadbackRanges(const xr_vector<RbRange>& rr, size_t total, xr_vector<u8>& out)
{
    if (rr.empty() || total == 0) return false;
    // Split (`[load step]`): this measured 93 ms for 21 MB, which is nowhere near
    // a copy rate — so the question is whether it is the staging allocation, the
    // recording, or the queue round trip, and only one of those has a fix.
    CTimer _r; _r.Start();
    float msAlloc = 0, msRecord = 0, msWait = 0, msCopyOut = 0;
    CVulkanBuffer stg;
    stg.Create(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
               false, false, /*hostRead*/ true);   // cached: this mapping is READ
    if (!stg.IsValid()) {
        Msg("![VK Trees] batched readback: staging alloc failed (%u MB)", (u32)(total >> 20));
        return false;
    }
    msAlloc = _r.GetElapsed_ms_total();
    _r.Start();
    VkCommandBuffer cmd = VulkanHW.BeginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) { stg.Destroy(); return false; }
    // One vkCmdCopyBuffer per SOURCE buffer (a pool), N regions each — the ranges
    // alternate VB/IB per mesh, so grouping by source is what keeps this to a handful
    // of commands instead of one per range. Destination ranges are disjoint, so the
    // order the groups are recorded in does not matter.
    xr_map<VkBuffer, xr_vector<VkBufferCopy>> byBuf;
    for (const RbRange& r : rr)
        byBuf[r.src].push_back({ r.off, (VkDeviceSize)r.dst, r.size });
    for (auto& kv : byBuf)
        vkCmdCopyBuffer(cmd, kv.first, stg.GetHandle(), (u32)kv.second.size(), kv.second.data());
    msRecord = _r.GetElapsed_ms_total();
    _r.Start();
    VulkanHW.EndSingleTimeCommands(cmd);   // waits (one-shot)
    msWait = _r.GetElapsed_ms_total();
    _r.Start();
    stg.Invalidate();
    void* p = stg.Map();
    if (!p) { stg.Destroy(); return false; }
    out.resize(total);
    memcpy(out.data(), p, total);
    stg.Unmap();
    stg.Destroy();
    msCopyOut = _r.GetElapsed_ms_total();
    Msg("[load step]     readback split: staging alloc %.0f (%u MB) | record %.0f (%u sources) | submit+wait %.0f | map+copy %.0f ms",
        msAlloc, (u32)(total >> 20), msRecord, (u32)byBuf.size(), msWait, msCopyOut);
    return true;
}

// ----- Crown-HULL bake helpers (r_vsm_tree_hull) — see BuildMeshlets' hull block. -----

// Unit icosphere, subdiv 1: 42 verts / 80 tris. Deterministic; built once, reused per lobe.
struct IcoSphere { xr_vector<Fvector> v; xr_vector<u16> i; };
static const IcoSphere& UnitIcosphere()
{
    static IcoSphere s;
    if (!s.v.empty()) return s;
    const float t = 1.6180339887f;   // golden ratio
    const float base[12][3] = {
        {-1,t,0},{1,t,0},{-1,-t,0},{1,-t,0},{0,-1,t},{0,1,t},
        {0,-1,-t},{0,1,-t},{t,0,-1},{t,0,1},{-t,0,-1},{-t,0,1} };
    for (auto& b : base) { Fvector p{ b[0], b[1], b[2] }; p.normalize(); s.v.push_back(p); }
    const u16 f[20][3] = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
    // One midpoint subdivision (edge-midpoint dedup) → 42/80.
    std::map<u32, u16> mid;
    auto midpoint = [&](u16 a, u16 b) -> u16 {
        const u32 key = (u32)(std::min)(a, b) << 16 | (std::max)(a, b);
        auto it = mid.find(key);
        if (it != mid.end()) return it->second;
        Fvector p; p.add(s.v[a], s.v[b]); p.normalize();
        const u16 idx = (u16)s.v.size();
        s.v.push_back(p); mid[key] = idx;
        return idx;
    };
    for (auto& tri : f) {
        const u16 a = tri[0], b = tri[1], c = tri[2];
        const u16 ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
        const u16 out[4][3] = { {a,ab,ca},{b,bc,ab},{c,ca,bc},{ab,bc,ca} };
        for (auto& o : out) { s.i.push_back(o[0]); s.i.push_back(o[1]); s.i.push_back(o[2]); }
    }
    return s;
}

// Cluster the crown's used vertices (AREA-WEIGHTED k-means, farthest-point init) and emit
// one ORIENTED ellipsoid icosphere lobe per leaf-mass cluster into hullVerts/hullIdx
// (indices RELATIVE to the species' first hull vertex — the indirect draw supplies
// vertexOffset). The lobe union is the merged "AC Shadows"-style crown shadow proxy.
// Smart placement (user feedback 11-07): per-vertex weights = incident triangle area, so
// centroids gravitate to the leaf TUFTS (big leaf cards dominate area; twig strips are
// tiny) — lobes hang ON the foliage. Per-cluster PCA then CLASSIFIES the shape: a long
// thin cluster (σ0 >> σ1, second extent < ~12 cm) is a bare TWIG → dropped entirely (its
// shadow is noise); everything else (tufts = planar/volumetric, thick boughs = fat
// sticks) gets a lobe. Trunks never reach here (bark visuals are excluded from the hull
// tier — they cast their real opaque mesh). Returns the emitted index count.
static u32 BuildHullLobes(const xr_vector<Fvector>& ptsIn, const xr_vector<float>& wtsIn,
                          xr_vector<Fvector>& hullVerts, xr_vector<u16>& hullIdx)
{
    // Load-time bound: k-means is O(k·n·iters) — stride-subsample huge crowns (the lobe
    // statistics converge long before 3k points; per-species, once per level load).
    xr_vector<Fvector> sampledP; xr_vector<float> sampledW;
    const xr_vector<Fvector>* p = &ptsIn;
    const xr_vector<float>*   w = &wtsIn;
    if (ptsIn.size() > 3000) {
        const u32 stride = ((u32)ptsIn.size() + 2999) / 3000;
        sampledP.reserve(3000); sampledW.reserve(3000);
        for (u32 i = 0; i < (u32)ptsIn.size(); i += stride) { sampledP.push_back(ptsIn[i]); sampledW.push_back(wtsIn[i]); }
        p = &sampledP; w = &sampledW;
    }
    const xr_vector<Fvector>& pts = *p;
    const xr_vector<float>&   wts = *w;
    const u32 n = (u32)pts.size();
    if (n < 12) return 0;
    // Lobe budget: a big crown gets ~25-32 lobes, bushes ~10 — the union must READ as the
    // tree's silhouette (branch structure), not 3-4 balloons (user feedback 11-07).
    const u32 k = (std::min)(32u, (std::max)(10u, 6u + (u32)ptsIn.size() / 100u));

    // Farthest-point init (deterministic), then area-weighted Lloyd iterations.
    xr_vector<Fvector> ctr; ctr.reserve(k);
    ctr.push_back(pts[0]);
    xr_vector<float> bestD(n, FLT_MAX);
    for (u32 c = 1; c < k; ++c) {
        u32 far_i = 0; float far_d = -1.f;
        for (u32 i = 0; i < n; ++i) {
            const float d = (std::min)(bestD[i], ctr.back().distance_to_sqr(pts[i]));
            bestD[i] = d;
            if (d > far_d) { far_d = d; far_i = i; }
        }
        ctr.push_back(pts[far_i]);
    }
    xr_vector<u32> assign(n, 0);
    for (u32 iter = 0; iter < 6; ++iter) {
        for (u32 i = 0; i < n; ++i) {
            u32 bi = 0; float bd = FLT_MAX;
            for (u32 c = 0; c < k; ++c) {
                const float d = ctr[c].distance_to_sqr(pts[i]);
                if (d < bd) { bd = d; bi = c; }
            }
            assign[i] = bi;
        }
        xr_vector<Fvector> sum(k, Fvector{ 0, 0, 0 });
        xr_vector<float> cw(k, 0.f);
        for (u32 i = 0; i < n; ++i) {
            const float wi = (std::max)(wts[i], 1e-6f);
            Fvector pw = pts[i]; pw.mul(wi);
            sum[assign[i]].add(pw); cw[assign[i]] += wi;
        }
        for (u32 c = 0; c < k; ++c)
            if (cw[c] > 0.f) { ctr[c].set(sum[c].x / cw[c], sum[c].y / cw[c], sum[c].z / cw[c]); }
    }

    // Per cluster: ORIENTED ellipsoid lobe via PCA (3x3 covariance → Jacobi eigenvectors).
    // A branch/trunk cluster stretches along its own axis → a thin "stick" lobe instead of
    // an axis-aligned balloon; leaf tufts stay roundish. Extents = min(1.9σ, max projection)
    // per principal axis (σ covers the bulk, the projection clamp stops outlier twigs from
    // ballooning the lobe), floored so flat leaf-card clusters still cast.
    const IcoSphere& ico = UnitIcosphere();
    const u32 vbase0 = (u32)hullVerts.size();
    const u32 ibase0 = (u32)hullIdx.size();
    for (u32 c = 0; c < k; ++c) {
        Fvector mean{ 0, 0, 0 };
        u32 cnt = 0; double sumW = 0.0;
        for (u32 i = 0; i < n; ++i) {
            if (assign[i] != c) continue;
            const float wi = (std::max)(wts[i], 1e-6f);
            Fvector pw = pts[i]; pw.mul(wi);
            mean.add(pw); sumW += wi; ++cnt;
        }
        if (cnt < 4 || sumW <= 0.0) continue;
        mean.div((float)sumW);

        // Symmetric AREA-WEIGHTED covariance (leaf mass shapes the lobe, twig verts barely).
        double C[3][3] = {};
        for (u32 i = 0; i < n; ++i) {
            if (assign[i] != c) continue;
            const double wi = (std::max)(wts[i], 1e-6f);
            const double d[3] = { pts[i].x - mean.x, pts[i].y - mean.y, pts[i].z - mean.z };
            for (int a = 0; a < 3; ++a)
                for (int b = a; b < 3; ++b) C[a][b] += wi * d[a] * d[b];
        }
        for (int a = 0; a < 3; ++a)
            for (int b = a; b < 3; ++b) { C[a][b] /= sumW; C[b][a] = C[a][b]; }

        // Cyclic Jacobi eigendecomposition: V columns = principal axes, diag(C) = variances.
        double V[3][3] = { {1,0,0},{0,1,0},{0,0,1} };
        for (int sweep = 0; sweep < 8; ++sweep) {
            for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (fabs(C[p][q]) < 1e-12) continue;
                const double theta = 0.5 * atan2(2.0 * C[p][q], C[q][q] - C[p][p]);
                const double s = sin(theta), co = cos(theta);
                for (int r = 0; r < 3; ++r) {
                    const double crp = C[r][p], crq = C[r][q];
                    C[r][p] = co * crp - s * crq;
                    C[r][q] = s * crp + co * crq;
                }
                for (int r = 0; r < 3; ++r) {
                    const double crp = C[p][r], crq = C[q][r];
                    C[p][r] = co * crp - s * crq;
                    C[q][r] = s * crp + co * crq;
                }
                for (int r = 0; r < 3; ++r) {
                    const double vrp = V[r][p], vrq = V[r][q];
                    V[r][p] = co * vrp - s * vrq;
                    V[r][q] = s * vrp + co * vrq;
                }
            }
        }
        Fvector axis[3];
        float sigma[3], maxProj[3] = { 0, 0, 0 };
        for (int a = 0; a < 3; ++a) {
            axis[a].set((float)V[0][a], (float)V[1][a], (float)V[2][a]);
            axis[a].normalize_safe();
            sigma[a] = sqrtf((float)(std::max)(C[a][a], 0.0));
        }
        for (u32 i = 0; i < n; ++i) {
            if (assign[i] != c) continue;
            const Fvector d{ pts[i].x - mean.x, pts[i].y - mean.y, pts[i].z - mean.z };
            for (int a = 0; a < 3; ++a)
                maxProj[a] = (std::max)(maxProj[a], fabsf(d.dotproduct(axis[a])));
        }
        float ext[3];
        for (int a = 0; a < 3; ++a)
            ext[a] = (std::max)((std::min)(1.9f * sigma[a], maxProj[a]), 0.08f);

        // Shape classification (extents sorted descending): LINEAR + thin = bare TWIG —
        // no lobe at all (its shadow is noise the merged proxy shouldn't fake). Planar
        // leaf cards / volumetric tufts / thick boughs all pass.
        int ord[3] = { 0, 1, 2 };
        std::sort(ord, ord + 3, [&](int a, int b) { return ext[a] > ext[b]; });
        if (ext[ord[1]] < 0.12f && ext[ord[0]] > 2.5f * ext[ord[1]]) continue;

        const u16 lobeBase = (u16)(hullVerts.size() - vbase0);
        for (const Fvector& u : ico.v) {
            Fvector p = mean;
            p.mad(axis[0], u.x * ext[0]);
            p.mad(axis[1], u.y * ext[1]);
            p.mad(axis[2], u.z * ext[2]);
            hullVerts.push_back(p);
        }
        for (const u16 ii : ico.i) hullIdx.push_back((u16)(lobeBase + ii));
    }
    return (u32)hullIdx.size() - ibase0;
}

// VOXEL-SHELL crown hull (r_vsm_tree_hull_vox > 0) — UE5 ShapePreservation=Voxelize in
// spirit: bin the crown's area-weighted leaf vertices into a 3D occupancy grid, then emit
// a WATERTIGHT OPAQUE surface: only the faces of occupied cells whose neighbour is empty
// (interior cells are fully occluded → pure overdraw in a depth-only shadow, so skipped).
// The exposed-face shell reads as the crown SILHOUETTE from every angle — a truer shape
// than the few PCA balloons — while staying opaque (no alpha-discard, the whole point).
// Indices are RELATIVE to the species' first hull vertex (the indirect draw supplies
// vertexOffset), matching BuildHullLobes' contract. `tris` = flat triangle corners
// (3 per triangle) of the crown foliage. `voxN` = target cells across the crown's
// largest axis. Returns the emitted index count.
//
// Occupancy is by TRIANGLE RASTERIZATION, not vertices: STALKER foliage is mostly
// billboards / low-poly cards (a whole crown = a few big quads = ~8 verts), so marking
// vertex cells collapsed the crown to one box. We sample each leaf triangle at ~cell
// density and mark every cell it crosses → the shell fills along the real cards. The
// AABB is ROBUST (mean ± 3σ) so one stray far vertex can't inflate the grid and crush
// the crown into a corner cell.
// Robust crown AABB: mean ± 3σ per axis, clamped to the true extent. A far outlier
// vertex barely moves the mean but would blow up a raw min/max — clipping keeps the
// voxel grid on the actual crown. Shared by the shell and the voxel-cloud bakes.
static bool RobustTriBounds(const xr_vector<Fvector>& tris, Fvector& lo, Fvector& hi)
{
    const u32 nC = (u32)tris.size();
    if (nC < 6) return false;
    Fvector mean{ 0,0,0 };
    for (u32 i = 0; i < nC; ++i) mean.add(tris[i]);
    mean.div((float)nC);
    Fvector var{ 0,0,0 }, tmin = tris[0], tmax = tris[0];
    for (u32 i = 0; i < nC; ++i) {
        const Fvector d{ tris[i].x - mean.x, tris[i].y - mean.y, tris[i].z - mean.z };
        var.x += d.x * d.x; var.y += d.y * d.y; var.z += d.z * d.z;
        tmin.x = (std::min)(tmin.x, tris[i].x); tmin.y = (std::min)(tmin.y, tris[i].y); tmin.z = (std::min)(tmin.z, tris[i].z);
        tmax.x = (std::max)(tmax.x, tris[i].x); tmax.y = (std::max)(tmax.y, tris[i].y); tmax.z = (std::max)(tmax.z, tris[i].z);
    }
    const Fvector sig{ sqrtf(var.x / nC), sqrtf(var.y / nC), sqrtf(var.z / nC) };
    lo = Fvector{ (std::max)(tmin.x, mean.x - 3.f * sig.x), (std::max)(tmin.y, mean.y - 3.f * sig.y), (std::max)(tmin.z, mean.z - 3.f * sig.z) };
    hi = Fvector{ (std::min)(tmax.x, mean.x + 3.f * sig.x), (std::min)(tmax.y, mean.y + 3.f * sig.y), (std::min)(tmax.z, mean.z + 3.f * sig.z) };
    return true;
}

static u32 BuildHullVoxels(const xr_vector<Fvector>& tris,
                           xr_vector<Fvector>& hullVerts, xr_vector<u16>& hullIdx, u32 voxN)
{
    const u32 nC = (u32)tris.size();          // corner count (= 3 * triangles)
    Fvector lo, hi;
    if (!RobustTriBounds(tris, lo, hi)) return 0;
    const float ext[3] = { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z };
    const float maxExt = (std::max)(ext[0], (std::max)(ext[1], ext[2]));
    if (maxExt < 1e-3f) return 0;
    const float cell = maxExt / float((std::max)(2u, voxN));
    const float inv  = 1.f / cell;
    // Cap at 36/axis: a filled shell's worst-case vertex count (~48·D²) must stay under the
    // u16 index range per species (65536) — and it bounds bake time. voxN normally sets the
    // max-axis dim to ~voxN anyway (cell scales with the crown), so this only clamps extremes.
    auto dim = [&](int a) { return (std::min)(36, (std::max)(1, (int)ceilf(ext[a] * inv) + 1)); };
    const int gx = dim(0), gy = dim(1), gz = dim(2);
    auto cellIdx = [&](int x, int y, int z) { return (z * gy + y) * gx + x; };
    auto clampi = [](int v, int m) { return v < 0 ? 0 : (v >= m ? m - 1 : v); };

    // Rasterize each triangle: barycentric samples at ~cell density mark every cell the
    // leaf card crosses. Samples outside the robust AABB (stray geometry) are dropped.
    xr_vector<u8> occ((size_t)gx * gy * gz, 0);
    auto mark = [&](const Fvector& p) {
        if (p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z) return;
        const int x = clampi((int)((p.x - lo.x) * inv), gx);
        const int y = clampi((int)((p.y - lo.y) * inv), gy);
        const int z = clampi((int)((p.z - lo.z) * inv), gz);
        occ[cellIdx(x, y, z)] = 1u;
    };
    for (u32 t = 0; t + 2 < nC; t += 3) {
        const Fvector &a = tris[t], &b = tris[t + 1], &c = tris[t + 2];
        const float eab = a.distance_to(b), eac = a.distance_to(c), ebc = b.distance_to(c);
        const int S = (std::min)(48, (std::max)(1, (int)ceilf((std::max)(eab, (std::max)(eac, ebc)) * inv)));
        for (int u = 0; u <= S; ++u)
        for (int v = 0; v <= S - u; ++v) {
            const float fu = (float)u / (float)S, fv = (float)v / (float)S;
            Fvector p = a;
            p.x += (b.x - a.x) * fu + (c.x - a.x) * fv;
            p.y += (b.y - a.y) * fu + (c.y - a.y) * fv;
            p.z += (b.z - a.z) * fu + (c.z - a.z) * fv;
            mark(p);
        }
    }
    auto occAt = [&](int x, int y, int z) -> bool {
        if (x < 0 || y < 0 || z < 0 || x >= gx || y >= gy || z >= gz) return false;
        return occ[cellIdx(x, y, z)] != 0;
    };

    // Emit exposed faces only. Per face: 4 verts + 2 tris (CULL_MODE_NONE world pipelines →
    // winding doesn't matter for depth). Corner positions in local mesh space.
    const u32 vbase0 = (u32)hullVerts.size();
    const u32 ibase0 = (u32)hullIdx.size();
    // 6 face normals; each face's 4 corner offsets (unit cube corners of the +face).
    static const int nrm[6][3] = { {-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1} };
    static const int corner[6][4][3] = {
        { {0,0,0},{0,1,0},{0,1,1},{0,0,1} },   // -X
        { {1,0,0},{1,0,1},{1,1,1},{1,1,0} },   // +X
        { {0,0,0},{0,0,1},{1,0,1},{1,0,0} },   // -Y
        { {0,1,0},{1,1,0},{1,1,1},{0,1,1} },   // +Y
        { {0,0,0},{1,0,0},{1,1,0},{0,1,0} },   // -Z
        { {0,0,1},{0,1,1},{1,1,1},{1,0,1} },   // +Z
    };
    for (int z = 0; z < gz; ++z)
    for (int y = 0; y < gy; ++y)
    for (int x = 0; x < gx; ++x) {
        if (!occ[cellIdx(x, y, z)]) continue;
        for (int f = 0; f < 6; ++f) {
            if (occAt(x + nrm[f][0], y + nrm[f][1], z + nrm[f][2])) continue;   // interior face → skip
            const u16 q = (u16)(hullVerts.size() - vbase0);   // base of this quad's 4 verts
            for (int cc = 0; cc < 4; ++cc) {
                Fvector p{ lo.x + float(x + corner[f][cc][0]) * cell,
                           lo.y + float(y + corner[f][cc][1]) * cell,
                           lo.z + float(z + corner[f][cc][2]) * cell };
                hullVerts.push_back(p);
            }
            hullIdx.push_back(q); hullIdx.push_back((u16)(q + 1)); hullIdx.push_back((u16)(q + 2));
            hullIdx.push_back(q); hullIdx.push_back((u16)(q + 2)); hullIdx.push_back((u16)(q + 3));
        }
    }
    return (u32)hullIdx.size() - ibase0;
}

// Deterministic per-cell hash → color/size variety (stable across bakes).
static inline u32 VoxHash(int x, int y, int z)
{
    u32 h = (u32)(x * 73856093) ^ (u32)(y * 19349663) ^ (u32)(z * 83492791);
    h ^= h >> 13; h *= 0x85ebca6bu; h ^= h >> 16;
    return h;
}

// ============================================================================
// Crown alpha map — CPU-side alpha channel of the foliage diffuse, sampled by
// the voxel bake so cells are marked by actual LEAVES, not by the leaf CARDS.
// A stalker crown is a handful of big quads whose leaves live entirely in the
// texture alpha; rasterizing the quads alpha-blind marks every cell the card
// crosses and the voxel union reads as one solid blob at any grid resolution.
// With the alpha test the fine grids get real gaps where the card is
// transparent — walking up to a tree, the voxel shadow dapples like foliage
// before the crossfade hands it to the true alpha-tested crown (the same idea
// as UE's per-voxel ray-traced Coverage, Cluster.cpp bVoxelOpacity).
// Decodes the alpha block of BC1/BC2/BC3 only (~all stalker foliage); any
// other format falls back to the alpha-blind bake for that species.
// ============================================================================
// Leaf-coverage nibble 1–15 from the bake sample counters (opaque hits / all card
// samples in the cell). Shared by the visual voxel cloud and the shadow bricks.
static u32 Cov4(u32 hits, u32 total)
{
    return (std::min)(15u, (std::max)(1u, (hits * 15u + total / 2) / (std::max)(total, 1u)));
}

struct CrownAlphaMap
{
    u32 w = 0, h = 0;
    xr_vector<u8> a;
    bool ok() const { return w && h && a.size() == (size_t)w * h; }
    bool opaque(float u, float v, u8 aref) const
    {
        u -= floorf(u); v -= floorf(v);   // wrap (leaf cards tile freely)
        u32 x = (u32)(u * (float)w); if (x >= w) x = w - 1;
        u32 y = (u32)(v * (float)h); if (y >= h) y = h - 1;
        return a[(size_t)y * w + x] >= aref;
    }
};

// Crown alpha maps are file IO plus a BC alpha decode (68 ms for 91 species) and
// nothing before the crown bake needs them — so the load runs on its own thread
// from the START of Build, behind the texture-floor and pack passes, and the bake
// joins it. It used to hide behind the meshlet readback instead, which stopped
// being a place to hide once that readback dropped from 97 ms to 8.
// File-static (not a member): CrownAlphaMap is local to this translation unit.
static xr_vector<std::pair<xr_string, CrownAlphaMap>> s_alphaCache;
static std::thread                                    s_alphaThread;

void JoinCrownAlphaPreload()
{
    if (s_alphaThread.joinable()) s_alphaThread.join();
}

static bool LoadCrownAlphaMap(const char* texName, CrownAlphaMap& out)
{
    if (!texName || !texName[0]) return false;
    string_path leaf, full;
    xr_strcpy(leaf, texName);
    xr_strcat(leaf, ".dds");
    FS.update_path(full, "$game_textures$", leaf);
    IReader* F = FS.r_open(full);
    if (!F) return false;

    bool okRead = false;
    do {
        u32 magic = 0;
        if (F->elapsed() < 4) break;
        F->r(&magic, 4);
        if (magic != 0x20534444u) break;   // "DDS "
        struct { u32 size, flags, height, width, pitch, depth, mipCount, res1[11];
                 struct { u32 size, flags, fourCC, bitCount, rM, gM, bM, aM; } pf;
                 u32 caps, caps2, caps3, caps4, res2; } H{};
        static_assert(sizeof(H) == 124, "DDS header must be 124 B");
        if (F->elapsed() < (int)sizeof(H)) break;
        F->r(&H, sizeof(H));

        constexpr u32 kDXT1 = 0x31545844u, kDXT3 = 0x33545844u, kDXT5 = 0x35545844u, kDX10 = 0x30315844u;
        u32 fourCC = (H.pf.flags & 0x4u) ? H.pf.fourCC : 0u;
        int mode = 0;                       // 1 = BC1 punch-through, 2 = BC2 explicit, 3 = BC3 interpolated
        if (fourCC == kDX10) {
            struct { u32 fmt, dim, misc, arr, misc2; } h10{};
            if (F->elapsed() < (int)sizeof(h10)) break;
            F->r(&h10, sizeof(h10));
            if      (h10.fmt == 71u || h10.fmt == 72u) mode = 1;   // BC1_UNORM(_SRGB)
            else if (h10.fmt == 74u || h10.fmt == 75u) mode = 2;   // BC2_UNORM(_SRGB)
            else if (h10.fmt == 77u || h10.fmt == 78u) mode = 3;   // BC3_UNORM(_SRGB)
            else break;                                            // BC7 etc. — no cheap alpha decode
        }
        else if (fourCC == kDXT1) mode = 1;
        else if (fourCC == kDXT3) mode = 2;
        else if (fourCC == kDXT5) mode = 3;
        else break;                                                // uncompressed — rare for foliage
        const u32 blockBytes = (mode == 1) ? 8u : 16u;

        // Walk mips down to ≤256/axis — leaf clumps survive the downfilter and the
        // decode stays trivially cheap (a 256² alpha grid per unique texture).
        u32 w = (std::max)(1u, H.width), h = (std::max)(1u, H.height);
        u32 mips = (H.flags & 0x20000u) ? (std::max)(1u, H.mipCount) : 1u;
        while (mips > 1 && (std::max)(w, h) > 256u) {
            F->advance(((w + 3) / 4) * ((h + 3) / 4) * blockBytes);
            w = (std::max)(1u, w >> 1); h = (std::max)(1u, h >> 1);
            --mips;
        }
        if ((std::max)(w, h) > 2048u) break;   // mipless oversize oddity — not worth a 16 MB+ cache entry
        const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
        const size_t bytes = (size_t)bw * bh * blockBytes;
        if ((size_t)F->elapsed() < bytes) break;
        xr_vector<u8> blocks(bytes);
        F->r(blocks.data(), (int)bytes);

        out.w = w; out.h = h;
        out.a.assign((size_t)w * h, 255);
        for (u32 by = 0; by < bh; ++by)
        for (u32 bx = 0; bx < bw; ++bx) {
            const u8* b = &blocks[((size_t)by * bw + bx) * blockBytes];
            u8 al[16];
            if (mode == 1) {
                const u16 c0 = (u16)(b[0] | (b[1] << 8)), c1 = (u16)(b[2] | (b[3] << 8));
                const u32 idx = (u32)b[4] | ((u32)b[5] << 8) | ((u32)b[6] << 16) | ((u32)b[7] << 24);
                const bool hasA = c0 <= c1;
                for (int t = 0; t < 16; ++t)
                    al[t] = (hasA && ((idx >> (2 * t)) & 3u) == 3u) ? 0 : 255;
            } else if (mode == 2) {
                for (int t = 0; t < 16; ++t) {
                    const u8 nib = (u8)((b[t >> 1] >> ((t & 1) * 4)) & 0xFu);
                    al[t] = (u8)(nib * 17u);
                }
            } else {
                const u8 a0 = b[0], a1 = b[1];
                u64 bits = 0;
                for (int i = 0; i < 6; ++i) bits |= (u64)b[2 + i] << (8 * i);
                u8 lut[8];
                lut[0] = a0; lut[1] = a1;
                if (a0 > a1) for (int i = 1; i < 7; ++i) lut[1 + i] = (u8)(((7 - i) * a0 + i * a1) / 7);
                else {
                    for (int i = 1; i < 5; ++i) lut[1 + i] = (u8)(((5 - i) * a0 + i * a1) / 5);
                    lut[6] = 0; lut[7] = 255;
                }
                for (int t = 0; t < 16; ++t) al[t] = lut[(bits >> (3 * t)) & 7u];
            }
            for (int t = 0; t < 16; ++t) {
                const u32 x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                if (x < w && y < h) out.a[(size_t)y * w + x] = al[t];
            }
        }
        okRead = true;
    } while (false);

    FS.r_close(F);
    if (!okRead) { out = CrownAlphaMap{}; return false; }
    return true;
}

// One alpha map per unique tree MATERIAL, loaded off-thread. Names are copied in
// (the worker must not chase material pointers the main thread is rebuilding) and
// the cache is written only here — from the join on, it is read-only, which is
// what makes the pointers handed to the parallel crown bake stable.
void StartCrownAlphaPreload(const xr_vector<WorldMaterial*>& mats)
{
    JoinCrownAlphaPreload();   // paranoia: a previous level
    s_alphaCache.clear();
    xr_vector<xr_string> names; names.reserve(mats.size());
    for (WorldMaterial* wm : mats) if (wm) names.emplace_back(wm->name.c_str());
    if (names.empty()) return;
    s_alphaThread = std::thread([names = std::move(names)] {
        for (const xr_string& nm : names) {
            if (nm.empty()) continue;
            bool have = false;
            for (const auto& e : s_alphaCache) if (e.first == nm) { have = true; break; }
            if (have) continue;
            s_alphaCache.emplace_back(nm, CrownAlphaMap{});
            LoadCrownAlphaMap(nm.c_str(), s_alphaCache.back().second);
        }
    });
}

// VOXEL CLOUD crown (the UE Nanite-foliage / "Witcher 4 demo" look): individual small
// colored cubes where the leaf cards actually are — NOT a merged watertight shell.
// The shell (BuildHullVoxels above) reads as one solid blob and is only right for the
// depth-only shadow caster; visually a crown is a SPARSE set of cubes with gaps, hue
// variety and darker interior. Per occupied cell we bake:
//   rgb — foliage palette pick by cell hash (greens/olive/rare brown), graded by
//         normalized height in the crown (canopy top lit, underside dark) and by
//         neighbor crowding (a cheap bake-time AO: dense interior → darker);
//   a   — size-jitter seed (the VS scales the cube 0.75–1.15) so the grid reads
//         organic instead of minecraft-regular.
// Cells fully enclosed on all 6 sides are skipped (never visible through the gaps at
// this density in practice — pure instance-count waste). `voxN` = cells across the
// crown's largest axis for THIS level. Returns the emitted voxel count; `cellOut`
// reports the voxel edge (mesh-local meters) for the renderer's screen-size LOD cut.
// `uvs`/`am` (both optional): per-corner UVs + the diffuse alpha map — samples on
// transparent parts of a leaf card don't mark cells, so fine grids grow real gaps
// where there are no leaves (the near-range "cubes at leaf positions" restructure);
// coarse grids stay ~solid since a big cell always holds some opaque texels.
// `outBricks` additionally receives the SHADOW representation of the same grid:
// 4×4×4-cell bricks with 64-bit occupancy masks (full + high-coverage core) — see
// GpuTreeBrick. Interior (fully-enclosed) cells stay IN the masks: they can never be
// the first hit of a ray, and the bits are free.
// Where the bake's time goes, summed per builder thread: the rasterization that
// fills the occupancy grid (shared by both outputs), the CUBE emission (a sweep
// over every cell of every LOD plus a 26-neighbour crowding lookup per occupied
// one), and the brick pass. Only the first and last feed anything the shipping
// renderer reads.
struct VoxProf { float raster = 0, cubes = 0, bricks = 0; };

static u32 BuildVoxelCloud(const xr_vector<Fvector>& tris, const xr_vector<Fvector2>& uvs,
                           const CrownAlphaMap* am,
                           xr_vector<GpuTreeVoxel>& outVox, xr_vector<GpuTreeBrick>& outBricks,
                           u32 voxN, float& cellOut, bool emitVox, VoxProf& prof)
{
    CTimer _vp;
    cellOut = 0.f;
    const u32 nC = (u32)tris.size();
    Fvector lo, hi;
    if (!RobustTriBounds(tris, lo, hi)) return 0;
    const float ext[3] = { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z };
    const float maxExt = (std::max)(ext[0], (std::max)(ext[1], ext[2]));
    if (maxExt < 1e-3f) return 0;
    const float cell = maxExt / float((std::max)(2u, voxN));
    const float inv  = 1.f / cell;
    // 144/axis cap only guards degenerate extents (voxN itself is clamped ≤128 by the
    // cvar); occupancy grid worst case 144³ = ~3 MB u8, transient.
    auto dim = [&](int a) { return (std::min)(144, (std::max)(1, (int)ceilf(ext[a] * inv) + 1)); };
    const int gx = dim(0), gy = dim(1), gz = dim(2);
    auto cellIdx = [&](int x, int y, int z) { return ((size_t)z * gy + y) * gx + x; };
    auto clampi = [](int v, int m) { return v < 0 ? 0 : (v >= m ? m - 1 : v); };

    // Occupancy by triangle rasterization — same scheme as the shell: barycentric
    // samples at ~cell density mark every cell a leaf card crosses. Per cell we also
    // count ALL card samples vs alpha-PASSING ones: their ratio is the cell's leaf
    // COVERAGE (how much of the card area in this cell is actual foliage), baked into
    // the voxel's low alpha nibble — the near-range sparsification kills cubes in
    // coverage order (card edges and thin wisps first, dense leaf clumps last).
    xr_vector<u8>  occ((size_t)gx * gy * gz, 0);
    xr_vector<u16> hitC((size_t)gx * gy * gz, 0), totC((size_t)gx * gy * gz, 0);
    auto mark = [&](const Fvector& p, bool pass) {
        if (p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z) return;
        const int x = clampi((int)((p.x - lo.x) * inv), gx);
        const int y = clampi((int)((p.y - lo.y) * inv), gy);
        const int z = clampi((int)((p.z - lo.z) * inv), gz);
        const size_t c = cellIdx(x, y, z);
        if (totC[c] < 0xFFFFu) ++totC[c];
        if (pass) { occ[c] = 1u; if (hitC[c] < 0xFFFFu) ++hitC[c]; }
    };
    // Alpha threshold matches the crown shadow FS (alphaRef 200/255) with a little
    // slack for the mip downfilter — the voxel gaps line up with the leaf shadow
    // the crossfade hands over to.
    constexpr u8 kARef = 160;
    const bool alphaAware = am && am->ok() && uvs.size() == tris.size();
    _vp.Start();
    for (u32 t = 0; t + 2 < nC; t += 3) {
        const Fvector &a = tris[t], &b = tris[t + 1], &c = tris[t + 2];
        const float eab = a.distance_to(b), eac = a.distance_to(c), ebc = b.distance_to(c);
        // 2× sample density when alpha-testing: pass/fail per cell becomes a vote of
        // ~4 samples instead of ~1, so cell marking tracks the leaf clumps instead of
        // single-texel luck.
        const int S = (std::min)(96, (std::max)(1, (int)ceilf((std::max)(eab, (std::max)(eac, ebc)) * inv * (alphaAware ? 2.f : 1.f))));
        for (int u = 0; u <= S; ++u)
        for (int v = 0; v <= S - u; ++v) {
            const float fu = (float)u / (float)S, fv = (float)v / (float)S;
            bool pass = true;
            if (alphaAware) {
                const Fvector2 &ta = uvs[t], &tb = uvs[t + 1], &tc = uvs[t + 2];
                const float su = ta.x + (tb.x - ta.x) * fu + (tc.x - ta.x) * fv;
                const float sv = ta.y + (tb.y - ta.y) * fu + (tc.y - ta.y) * fv;
                pass = am->opaque(su, sv, kARef);
            }
            Fvector p = a;
            p.x += (b.x - a.x) * fu + (c.x - a.x) * fv;
            p.y += (b.y - a.y) * fu + (c.y - a.y) * fv;
            p.z += (b.z - a.z) * fu + (c.z - a.z) * fv;
            mark(p, pass);
        }
    }
    prof.raster += _vp.GetElapsed_ms_total();
    auto occAt = [&](int x, int y, int z) -> bool {
        if (x < 0 || y < 0 || z < 0 || x >= gx || y >= gy || z >= gz) return false;
        return occ[cellIdx(x, y, z)] != 0;
    };

    // Foliage palette — hue variety like the reference: mid/dark greens dominate,
    // olive and yellow-green sprinkled in, the odd brown (inner branches).
    static const float kPal[6][3] = {
        { 0.30f, 0.42f, 0.16f },   // mid green
        { 0.21f, 0.33f, 0.13f },   // dark green
        { 0.15f, 0.25f, 0.11f },   // deep green
        { 0.38f, 0.44f, 0.15f },   // olive
        { 0.47f, 0.44f, 0.17f },   // yellow-green
        { 0.30f, 0.23f, 0.12f },   // brown
    };
    static const u8 kPick[16] = { 0,0,0,0, 1,1,1, 2,2, 3,3, 4,4, 5, 0,1 };   // weighted

    // The cube cloud has exactly ONE consumer, the r_vsm_tree_hull_debug viewmode,
    // and that viewmode needs a level reload anyway — so when it is off, do not
    // build the cubes at all. Not just "do not upload them": this loop sweeps every
    // cell of every LOD of every species (~85M cells on pripyat_full) and does a
    // 26-neighbour crowding lookup plus a push_back for each of the 777k occupied
    // ones. The occupancy grid above is what the SHADOW BRICKS need, and it is
    // already built. r_vsm_tree_vox_cloud 1 restores the unconditional bake.
    const u32 base = (u32)outVox.size();
    _vp.Start();
    const float invExtY = 1.f / (std::max)(ext[1], 1e-3f);
    for (int z = 0; emitVox && z < gz; ++z)
    for (int y = 0; y < gy; ++y)
    for (int x = 0; x < gx; ++x) {
        if (!occ[cellIdx(x, y, z)]) continue;
        if (occAt(x - 1, y, z) && occAt(x + 1, y, z) && occAt(x, y - 1, z) &&
            occAt(x, y + 1, z) && occAt(x, y, z - 1) && occAt(x, y, z + 1)) continue;   // fully enclosed
        // Bake-time AO: 26-neighborhood crowding — interior/dense cells darker.
        int crowd = 0;
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if ((dx | dy | dz) && occAt(x + dx, y + dy, z + dz)) ++crowd;
        const u32   h    = VoxHash(x, y, z);
        const float* pc  = kPal[kPick[h & 15u]];
        const float tUp  = ((float)y + 0.5f) * cell * invExtY;            // 0 = crown bottom, 1 = top
        const float ao   = 1.f - 0.45f * ((float)crowd / 26.f);
        const float jit  = 0.85f + 0.30f * (float)((h >> 8) & 255u) / 255.f;
        const float br   = (0.70f + 0.40f * tUp) * ao * jit;
        auto ch = [&](float c) { return (u32)(std::min)(255.f, (std::max)(0.f, c * br * 255.f + 0.5f)); };
        // Alpha byte split: HIGH nibble = size-jitter seed, LOW nibble = leaf coverage
        // 1–15 (opaque-sample fraction; 15 everywhere on the alpha-blind fallback) —
        // the caster dissolves cubes in coverage order across the crossfade band.
        const size_t ci   = cellIdx(x, y, z);
        const u32    cov4 = Cov4(hitC[ci], totC[ci]);
        GpuTreeVoxel vx;
        vx.p    = Fvector{ lo.x + ((float)x + 0.5f) * cell,
                           lo.y + ((float)y + 0.5f) * cell,
                           lo.z + ((float)z + 0.5f) * cell };
        vx.rgba = ch(pc[0]) | (ch(pc[1]) << 8) | (ch(pc[2]) << 16) | ((((h >> 16) & 0xF0u) | cov4) << 24);
        outVox.push_back(vx);
    }
    prof.cubes += _vp.GetElapsed_ms_total();
    _vp.Start();

    // ---- SHADOW bricks: 4×4×4 cells → one 64-bit mask (+ high-coverage core mask).
    // A brick with an empty core simply vanishes at its band swap point — that IS the
    // "extras removed" stage, no fallback wanted.
    const size_t brickBase = outBricks.size();
    const int bgx = (gx + 3) / 4, bgy = (gy + 3) / 4, bgz = (gz + 3) / 4;
    for (int bz = 0; bz < bgz; ++bz)
    for (int by = 0; by < bgy; ++by)
    for (int bx = 0; bx < bgx; ++bx) {
        GpuTreeBrick bk{};
        u32 covSum = 0, covN = 0;
        for (int lz = 0; lz < 4; ++lz)
        for (int ly = 0; ly < 4; ++ly)
        for (int lx = 0; lx < 4; ++lx) {
            const int x = bx * 4 + lx, y = by * 4 + ly, z = bz * 4 + lz;
            if (x >= gx || y >= gy || z >= gz) continue;
            const size_t ci = cellIdx(x, y, z);
            if (!occ[ci]) continue;
            const u32 bit  = (u32)lx | ((u32)ly << 2) | ((u32)lz << 4);
            const u32 cov4 = Cov4(hitC[ci], totC[ci]);
            (bit < 32u ? bk.fullLo : bk.fullHi) |= 1u << (bit & 31u);
            if (cov4 >= 6u) (bit < 32u ? bk.coreLo : bk.coreHi) |= 1u << (bit & 31u);
            covSum += cov4; ++covN;
        }
        if (!(bk.fullLo | bk.fullHi)) continue;
        bk.p = Fvector{ lo.x + (float)(bx * 4) * cell,
                        lo.y + (float)(by * 4) * cell,
                        lo.z + (float)(bz * 4) * cell };
        const u32 h = VoxHash(bx, by, bz);
        bk.meta = ((covSum + covN / 2) / (std::max)(covN, 1u)) | ((h >> 8) & 0xFF0u);   // avgCov4 | hash8<<4
        outBricks.push_back(bk);
    }
    // Sort the slice by DESCENDING dissolve rank (the same 0.5·cov + 0.5·hash the VS
    // uses, both read from meta) → the band's survivors are always a PREFIX, so
    // vsm_hull_cmd can shrink vertexCount to ~fade·count instead of drawing every
    // brick and letting the VS kill most of them (the band double-cast was the whole
    // remaining dyn-tier brick cost on light scenes).
    std::sort(outBricks.begin() + brickBase, outBricks.end(),
              [](const GpuTreeBrick& a, const GpuTreeBrick& b) {
                  const u32 ra = ((a.meta & 0xFu) * 255u * 8u) / 15u + ((a.meta >> 4) & 0xFFu) * 8u;
                  const u32 rb = ((b.meta & 0xFu) * 255u * 8u) / 15u + ((b.meta >> 4) & 0xFFu) * 8u;
                  return ra > rb;
              });

    prof.bricks += _vp.GetElapsed_ms_total();
    cellOut = cell;
    return (u32)outVox.size() - base;
}

}  // namespace

void CTreeManager::BuildMeshlets(const xr_vector<vkFTreeVisual*>& trees,
                                 xr_vector<GpuTreeMeta>& meta)
{
    if (m_TotalCount == 0 || trees.size() < m_TotalCount || meta.size() < m_TotalCount) return;

    // A unique mesh = one (vb, ib, vBase, vCount, ib_first, index_count) span. Instances
    // of the same species share it, so we meshletize + read back geometry ONCE per span.
    struct MeshKey {
        VkBuffer vb, ib; u32 vBase, vCount, ibFirst, idxCount, stride;
        bool operator==(const MeshKey& o) const {
            return vb == o.vb && ib == o.ib && vBase == o.vBase && vCount == o.vCount &&
                   ibFirst == o.ibFirst && idxCount == o.idxCount && stride == o.stride;
        }
    };
    xr_vector<GpuTreeMeshletRange> uniqRange;   // meshlet slice per unique mesh
    xr_vector<u32>               treeUniq(m_TotalCount, ~0u);   // tree -> unique-mesh index

    // Output accumulators (uploaded once at the end).
    xr_vector<GpuMeshlet> meshlets;   meshlets.reserve(4096);
    xr_vector<u16>        mIndices;   mIndices.reserve(1 << 18);

    // Crown-HULL accumulators (r_vsm_tree_hull) — baked here to reuse the same unique-mesh
    // readback. Always built (a few hundred KB total) so the cvar toggles live at runtime.
    xr_vector<Fvector>         hullVerts;  hullVerts.reserve(1 << 14);
    xr_vector<u16>             hullIdx;    hullIdx.reserve(1 << 15);
    xr_vector<GpuTreeHullInfo> uniqHull;      // hull range per unique mesh (→ shadow caster)
    xr_vector<GpuTreeVoxel>    voxData;    voxData.reserve(1 << 16);   // voxel-cloud viewmode cubes
    xr_vector<TreeVoxLod>      uniqVoxLods;  // kHullLods voxel ranges per unique mesh
    xr_vector<GpuTreeBrick>    brickData;  brickData.reserve(1 << 13); // shadow bricks (4³ masks)
    xr_vector<TreeVoxLod>      uniqBrickLods;
    // Diffuse alpha maps for the alpha-aware voxel bake, one per unique texture
    // (species share textures). The returned pointer is only valid until the next
    // lookup (vector may reallocate) — consumed within the same tree iteration.
    // Filled by the preload thread started in Build (see s_alphaCache).
    u32 alphaSpecies = 0, blindSpecies = 0;   // diag: voxel bakes with/without texture alpha

    // Sub-timers (`[load step]`, printed once). This function measured 747 ms of a
    // 19.5 s load and nobody knew which half was the CPU bake and which was the two
    // GPU readbacks per unique mesh — each of those is a full VMA alloc + submit +
    // fence wait. Split before fixing.
    CTimer _p;
    float msReadback = 0, msCluster = 0, msHullPrep = 0, msHull = 0, msVox = 0, msUp = 0;
    VoxProf voxProf;
    float msBakeWall = 0, msMerge = 0;
    // Card-tightness survey totals (r_tree_stats >= 2).
    double cardArea = 0, cardCovered = 0, cardDrop[kCardLevels] = {}, cardDropDense = 0, cardFaintArea = 0;
    u32    cardTris = 0, cardSpecies = 0, cardEmptyTris = 0;
    float  msCards  = 0;
    u64   bytesRead = 0;
    u32   readbacks = 0, msBakeThreads = 1;
    // Loading touches the filesystem and grows `alphaCache`, so it happens ONCE up
    // front, serially (see the preload below). During the parallel bake the cache is
    // read-only, which also makes the returned pointers stable — they used to be valid
    // only until the next lookup reallocated the vector.
    // Card-tightness survey — see the block in bakeOne. Gated at r_tree_stats 2 because
    // it costs real load time (7 alpha taps per sub-triangle over every foliage
    // triangle on the level) and answers a question you ask once per level, not per run.
    const bool cardSurvey = ps_r_tree_stats >= 2;

    auto crownAlpha = [](const char* nm) -> const CrownAlphaMap* {
        if (!nm || !nm[0]) return nullptr;
        for (const auto& e : s_alphaCache) if (e.first == nm) return e.second.ok() ? &e.second : nullptr;
        return nullptr;
    };

    // ---- Phase 1: which meshes are unique (no IO, no GPU work) ----------------
    // Instances share a species mesh, so the bake below runs once per unique span.
    // `firstTree` is the instance that introduced the span: its wind class, tcOffset
    // and material are what the bake reads — exactly as when discovery and bake were
    // one interleaved loop.
    struct Uniq { MeshKey k; u32 firstTree; size_t vbOff, ibOff; };
    xr_vector<Uniq> uniq; uniq.reserve(512);
    for (u32 i = 0; i < m_TotalCount; ++i)
    {
        const vkFTreeVisual* t = trees[i];
        if (!t || !t->m_mesh.p_rm_Vertices || !t->m_mesh.p_rm_Indices) continue;
        MeshKey k{ t->m_mesh.p_rm_Vertices->GetHandle(), t->m_mesh.p_rm_Indices->GetHandle(),
                   t->m_mesh.vBase, t->m_mesh.vCount, meta[i].ib_first, meta[i].index_count,
                   t->m_mesh.vStride };
        if (k.vCount == 0 || k.idxCount < 3 || k.stride < 12) continue;

        u32 u = ~0u;
        for (u32 j = 0; j < (u32)uniq.size(); ++j) if (uniq[j].k == k) { u = j; break; }
        if (u == ~0u) { u = (u32)uniq.size(); uniq.push_back({ k, i, 0, 0 }); }
        treeUniq[i] = u;
    }
    if (uniq.empty()) { Msg("[VK Trees] Meshlets: no eligible tree meshes"); return; }

    // ---- Phase 2: ONE batched readback of every unique mesh's VB + IB ---------
    xr_vector<RbRange> rr; rr.reserve(uniq.size() * 2);
    size_t blobSize = 0;
    auto plan = [&](VkBuffer b, VkDeviceSize off, VkDeviceSize size) -> size_t {
        blobSize = (blobSize + 15) & ~size_t(15);   // keep vertex reads naturally aligned
        const size_t at = blobSize;
        rr.push_back({ b, off, size, at });
        blobSize += (size_t)size;
        return at;
    };
    for (Uniq& u : uniq) {
        u.vbOff = plan(u.k.vb, (VkDeviceSize)u.k.vBase * u.k.stride, (VkDeviceSize)u.k.vCount * u.k.stride);
        u.ibOff = plan(u.k.ib, (VkDeviceSize)u.k.ibFirst * sizeof(u16), (VkDeviceSize)u.k.idxCount * sizeof(u16));
    }
    xr_vector<u8> blob;

    _p.Start();
    const bool rbOk = ReadbackRanges(rr, blobSize, blob);
    msReadback = _p.GetElapsed_ms_total();
    readbacks  = (u32)rr.size();
    bytesRead  = blobSize;
    if (!rbOk) {
        JoinCrownAlphaPreload();   // it writes s_alphaCache
        // All-or-nothing by construction: a partial batch would desync the per-unique
        // tables below (they are indexed by unique id), so bail instead of half-filling.
        Msg("![VK Trees] meshlet/hull readback failed — VSM meshlet cull and crown hulls unavailable");
        return;
    }

    // ---- Phase 3: bake, once per unique mesh, from the blob -------------------
    // Collect the alpha maps started above. From here the cache is read-only, which
    // is what makes the pointers handed to the parallel bake stable.
    _p.Start();
    JoinCrownAlphaPreload();
    const float msAlpha = _p.GetElapsed_ms_total();   // what is LEFT of the preload

    // Read the cvars ONCE, before the workers start: the cube cloud is baked at load
    // and the viewmode that reads it already needs a level reload, so this cannot
    // change under the bake.
    const bool s_emitVoxCloud = (ps_r_tree_vox_dist > 0.f) || (ps_r_vsm_tree_hull_debug > 0)
                             || (ps_r_vsm_tree_vox_cloud > 0);

    // Per-species bake outputs. Absolute offsets inside them (meshlet first_index,
    // range base, hull ib/vb first, voxel/brick lod first) are species-local here and
    // get rebased during the merge.
    struct Baked {
        xr_vector<GpuMeshlet>   meshlets;
        xr_vector<u16>          mIndices;
        xr_vector<Fvector>      hullVerts;
        xr_vector<u16>          hullIdx;
        xr_vector<GpuTreeVoxel> vox;
        xr_vector<GpuTreeBrick> bricks;
        GpuTreeMeshletRange     range{ 0, 0 };
        GpuTreeHullInfo         hull{ 0, 0, 0, 0 };
        TreeVoxLod              voxLod[CTreeManager::kHullLods]{};
        TreeVoxLod              brickLod[CTreeManager::kHullLods]{};
        u32   alphaHit = 0, alphaMiss = 0;
        float msCluster = 0, msHullPrep = 0, msHull = 0, msVox = 0;
        VoxProf voxProf;
        // Card-tightness survey (r_tree_stats >= 2). Doubles: summed over ~1.2 M
        // triangles, a float accumulator loses the small ones against the big ones.
        double cardArea = 0, cardCovered = 0, cardDrop[kCardLevels] = {}, cardDropDense = 0, cardFaintArea = 0;
        u32    cardTris = 0, cardEmptyTris = 0;
        float  msCards  = 0;
    };
    xr_vector<Baked> baked(uniq.size());

    // One species' bake writes ONLY into its own Baked slot, so the 284 of them are
    // independent and run on worker threads (measured: this phase was 480 ms of CPU,
    // ~85% of it the voxel rasterization). The merge below concatenates the slots in
    // unique order and rebases the few absolute offsets, so the result is bit-identical
    // to the serial version — the counts in the two summary lines are the check.
    auto bakeOne = [&](u32 ui)
    {
        Baked& B = baked[ui];
        // Aliases: the body below is unchanged from when these were the shared
        // accumulators. Everything it appends to is now per-species.
        xr_vector<GpuMeshlet>&   meshlets  = B.meshlets;
        xr_vector<u16>&          mIndices  = B.mIndices;
        xr_vector<Fvector>&      hullVerts = B.hullVerts;
        xr_vector<u16>&          hullIdx   = B.hullIdx;
        xr_vector<GpuTreeVoxel>& voxData   = B.vox;
        xr_vector<GpuTreeBrick>& brickData = B.bricks;
        CTimer _p;

        const MeshKey&       k = uniq[ui].k;
        const u32            i = uniq[ui].firstTree;
        const vkFTreeVisual* t = trees[i];
        const u8*  vbytes = blob.data() + uniq[ui].vbOff;
        const u16* idx    = (const u16*)(blob.data() + uniq[ui].ibOff);
        auto pos = [&](u32 v) -> Fvector {
            const float* f = (const float*)(vbytes + (size_t)v * k.stride);
            return Fvector{ f[0], f[1], f[2] };
        };
        const u32 triCount = k.idxCount / 3;

        // --- Per-triangle centroids + their AABB (Morton normalization box). ---
        _p.Start();
        xr_vector<Fvector> cen(triCount);
        Fvector cmin{ FLT_MAX, FLT_MAX, FLT_MAX }, cmax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (u32 tri = 0; tri < triCount; ++tri) {
            const Fvector a = pos(idx[tri * 3 + 0]), b = pos(idx[tri * 3 + 1]), c = pos(idx[tri * 3 + 2]);
            Fvector ctr{ (a.x + b.x + c.x) / 3.f, (a.y + b.y + c.y) / 3.f, (a.z + b.z + c.z) / 3.f };
            cen[tri] = ctr;
            cmin.min(ctr); cmax.max(ctr);
        }
        const float ex = (std::max)(cmax.x - cmin.x, 1e-4f);
        const float ey = (std::max)(cmax.y - cmin.y, 1e-4f);
        const float ez = (std::max)(cmax.z - cmin.z, 1e-4f);

        // --- Morton-sort triangle order. ---
        xr_vector<u32> order(triCount);
        for (u32 tri = 0; tri < triCount; ++tri) order[tri] = tri;
        xr_vector<u32> code(triCount);
        for (u32 tri = 0; tri < triCount; ++tri) {
            const u32 qx = (u32)(std::min)(1023.f, (std::max)(0.f, (cen[tri].x - cmin.x) / ex * 1023.f));
            const u32 qy = (u32)(std::min)(1023.f, (std::max)(0.f, (cen[tri].y - cmin.y) / ey * 1023.f));
            const u32 qz = (u32)(std::min)(1023.f, (std::max)(0.f, (cen[tri].z - cmin.z) / ez * 1023.f));
            code[tri] = Morton3(qx, qy, qz);
        }
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return code[a] < code[b]; });

        // --- Slice into clusters; local sphere per cluster; emit reordered indices. ---
        const u32 base = 0;   // rebased at merge
        for (u32 s = 0; s < triCount; s += kMeshletTris) {
            const u32 e = (std::min)(triCount, s + kMeshletTris);
            const u32 firstIndex = (u32)mIndices.size();
            Fvector bmin{ FLT_MAX, FLT_MAX, FLT_MAX }, bmax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (u32 o = s; o < e; ++o) {
                const u32 tri = order[o];
                for (u32 w = 0; w < 3; ++w) {
                    const u16 vi = idx[tri * 3 + w];
                    mIndices.push_back(vi);
                    const Fvector p = pos(vi);
                    bmin.min(p); bmax.max(p);
                }
            }
            Fvector ctr{ (bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f, (bmin.z + bmax.z) * 0.5f };
            float r2 = 0.f;
            for (u32 o = s; o < e; ++o) {
                const u32 tri = order[o];
                for (u32 w = 0; w < 3; ++w) r2 = (std::max)(r2, ctr.distance_to_sqr(pos(idx[tri * 3 + w])));
            }
            GpuMeshlet ml{};
            ml.center = ctr; ml.radius = sqrtf(r2);
            ml.first_index = firstIndex; ml.index_count = (e - s) * 3;
            meshlets.push_back(ml);
        }
        B.msCluster = _p.GetElapsed_ms_total();

        // --- Crown hull: FOLIAGE meshes only (windClass 2). Trunks/props never enter the
        // hull tier (crowns-only gate in VsmUpdateNearSet), so baking them would be waste.
        if (i < m_WindClassCPU.size() && m_WindClassCPU[i] == 2)
        {
            // Per-vertex weight = incident triangle area: leaf cards (big quads) dominate,
            // twig strips barely register → lobes centre on the leaf TUFTS.
            _p.Start();
            xr_vector<float> vertArea(k.vCount, 0.f);
            for (u32 tri = 0; tri < triCount; ++tri) {
                const u16 i0 = idx[tri * 3 + 0], i1 = idx[tri * 3 + 1], i2 = idx[tri * 3 + 2];
                if (i0 >= k.vCount || i1 >= k.vCount || i2 >= k.vCount) continue;
                const Fvector a = pos(i0), b = pos(i1), cc = pos(i2);
                Fvector e1{ b.x - a.x, b.y - a.y, b.z - a.z }, e2{ cc.x - a.x, cc.y - a.y, cc.z - a.z }, cr;
                cr.crossproduct(e1, e2);
                const float third = cr.magnitude() * 0.5f / 3.f;
                vertArea[i0] += third; vertArea[i1] += third; vertArea[i2] += third;
            }
            xr_vector<Fvector> pts; pts.reserve(k.vCount);
            xr_vector<float>   wts; wts.reserve(k.vCount);
            for (u32 v = 0; v < k.vCount; ++v)
                if (vertArea[v] > 0.f) { pts.push_back(pos(v)); wts.push_back(vertArea[v]); }
            // Flat triangle corners for voxel rasterization (fills the crown from leaf
            // cards, not sparse verts). Lobes still use the pts/wts point cloud.
            // UVs ride along (SHORT2 SSCALED @ tcOffset, ×1/2048 — same decode as the
            // tree VS) so the voxel bake can alpha-test against the diffuse.
            const u32  tcOff = t->m_mesh.tcOffset;
            const bool hasUV = tcOff + 2 * sizeof(s16) <= k.stride;
            auto uvAt = [&](u32 v) -> Fvector2 {
                const s16* s = (const s16*)(vbytes + (size_t)v * k.stride + tcOff);
                return Fvector2{ (float)s[0] / 2048.f, (float)s[1] / 2048.f };
            };
            xr_vector<Fvector>  tris;  tris.reserve((size_t)triCount * 3);
            xr_vector<Fvector2> triUV; if (hasUV) triUV.reserve((size_t)triCount * 3);
            for (u32 tri = 0; tri < triCount; ++tri) {
                const u16 i0 = idx[tri * 3 + 0], i1 = idx[tri * 3 + 1], i2 = idx[tri * 3 + 2];
                if (i0 >= k.vCount || i1 >= k.vCount || i2 >= k.vCount) continue;
                tris.push_back(pos(i0)); tris.push_back(pos(i1)); tris.push_back(pos(i2));
                if (hasUV) { triUV.push_back(uvAt(i0)); triUV.push_back(uvAt(i1)); triUV.push_back(uvAt(i2)); }
            }
            const CrownAlphaMap* am = hasUV && t->m_pWorldMaterial
                                    ? crownAlpha(t->m_pWorldMaterial->name.c_str()) : nullptr;
            if (ps_r_vsm_tree_hull_vox > 0) { am ? ++B.alphaHit : ++B.alphaMiss; }

            // ---- CARD-TIGHTNESS SURVEY (r_tree_stats >= 2) -----------------------
            // The tree ledger established that 96% of the canopy's shaded fragments are
            // thrown away by the leaf alpha test, and that this is NOT a broken early-Z
            // (r_tree_zwrite moved the fragment count by 0.1%): it is the geometric area
            // of leaf quads that carry no opaque texels. The fix for that is to shrink
            // the quads. But how much CAN be shrunk is a property of the ART, not of the
            // renderer, and it is knowable right here — the mesh, its UVs and the diffuse
            // alpha are all in hand during the bake.
            //
            // ⭐ MEASURED BEFORE BUILT, on purpose. Two tree LOD arcs were built first and
            // measured after, and both were thrown away. If the leaf textures turn out to
            // be densely covered, tightening the cards cannot pay and this survey costs a
            // load-time diagnostic instead of a week.
            //
            // Reported per subdivision level: level 1 = drop whole triangles that contain
            // no opaque texel, 2 and 4 = split each triangle into N² and drop the empty
            // sub-triangles. Higher N recovers more area but emits more triangles, and
            // triangles are the currency this canopy has to spare (1.7 M drawn against
            // 80 M fragment invocations).
            if (cardSurvey && am && !triUV.empty()) {
                CTimer _pc; _pc.Start();
                const u8 aref = 200u;   // == tree.frag alphaRef (200/255)
                auto alphaAt = [&](float u, float v) -> u8 {
                    u -= floorf(u); v -= floorf(v);            // foliage UVs tile
                    u32 x = (u32)(u * (float)am->w); if (x >= am->w) x = am->w - 1;
                    u32 y = (u32)(v * (float)am->h); if (y >= am->h) y = am->h - 1;
                    return am->a[(size_t)y * am->w + x];
                };
                const u32 nTri = (u32)(triUV.size() / 3);
                for (u32 tri = 0; tri < nTri; ++tri) {
                    const Fvector& p0 = tris[tri * 3 + 0];
                    const Fvector& p1 = tris[tri * 3 + 1];
                    const Fvector& p2 = tris[tri * 3 + 2];
                    Fvector e1{ p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
                    Fvector e2{ p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
                    Fvector cr; cr.crossproduct(e1, e2);
                    const double area = (double)cr.magnitude() * 0.5;
                    if (area <= 0.0) continue;
                    const Fvector2& t0 = triUV[tri * 3 + 0];
                    const Fvector2& t1 = triUV[tri * 3 + 1];
                    const Fvector2& t2 = triUV[tri * 3 + 2];
                    // UV at barycentric (a,b,c), a+b+c == 1.
                    auto uvAtBary = [&](float a, float b, float c, float& u, float& v) {
                        u = t0.x * a + t1.x * b + t2.x * c;
                        v = t0.y * a + t1.y * b + t2.y * c;
                    };
                    // 7 taps per sub-triangle: 3 corners, 3 edge midpoints, centroid. Corners
                    // alone miss a leaf that sits in the middle of an otherwise empty cell,
                    // and dropping a cell that had one is a HOLE IN THE CANOPY — the survey
                    // must not report area as droppable that the real thing could not drop.
                    auto subOpaque = [&](float a0, float b0, float c0, float a1, float b1, float c1,
                                         float a2, float b2, float c2) -> bool {
                        const float aa[7] = { a0, a1, a2, (a0+a1)*0.5f, (a1+a2)*0.5f, (a0+a2)*0.5f, (a0+a1+a2)/3.f };
                        const float bb[7] = { b0, b1, b2, (b0+b1)*0.5f, (b1+b2)*0.5f, (b0+b2)*0.5f, (b0+b1+b2)/3.f };
                        const float cc[7] = { c0, c1, c2, (c0+c1)*0.5f, (c1+c2)*0.5f, (c0+c2)*0.5f, (c0+c1+c2)/3.f };
                        for (int s = 0; s < 7; ++s) {
                            float u, v; uvAtBary(aa[s], bb[s], cc[s], u, v);
                            if (alphaAt(u, v) >= aref) return true;
                        }
                        return false;
                    };
                    B.cardArea += area;
                    ++B.cardTris;
                    // Coverage estimate (order-8 barycentric lattice) — "how much of this
                    // card is opaque at all", the ceiling any tightening could approach.
                    {
                        u32 hit = 0, tot = 0;
                        const int N = 16;
                        for (int i = 0; i <= N; ++i)
                        for (int j = 0; i + j <= N; ++j) {
                            const float a = (float)i / N, b = (float)j / N, c = 1.f - a - b;
                            float u, v; uvAtBary(a, b, c, u, v);
                            if (alphaAt(u, v) >= aref) ++hit;
                            ++tot;
                        }
                        B.cardCovered += area * (tot ? double(hit) / double(tot) : 0.0);
                        // ⚠⚠ THE HONEST "droppable whole triangle" NUMBER, and it must be
                        // taken from THIS lattice, not from the 7 taps the subdivision
                        // levels use. A foliage card here averages 2.2 m² — 7 taps across
                        // something that size can miss a leaf entirely, and a triangle
                        // wrongly called empty is A HOLE IN THE CANOPY, not a saving. 153
                        // taps is still a sample rather than a rasterisation, so even this
                        // is an upper bound: a real implementation has to test every texel
                        // the triangle covers.
                        if (hit == 0) { B.cardDropDense += area; ++B.cardEmptyTris; }
                        // How many cards are nearly-but-not-quite empty — the population a
                        // more aggressive threshold would reach, and the risk it carries.
                        if (hit && double(hit) / double(tot) < 0.05) B.cardFaintArea += area;
                    }
                    // Droppable area per subdivision order N (N² sub-triangles of equal area).
                    for (u32 li = 0; li < kCardLevels; ++li) {
                        const int N = kCardSplit[li];
                        u32 empty = 0, total = 0;
                        const float inv = 1.f / (float)N;
                        for (int i = 0; i < N; ++i)
                        for (int j = 0; i + j < N; ++j) {
                            // "Upward" sub-triangle of the barycentric lattice.
                            const float a0 = (i + 1) * inv, b0 = j * inv;
                            const float a1 = i * inv,       b1 = (j + 1) * inv;
                            const float a2 = i * inv,       b2 = j * inv;
                            if (!subOpaque(a0, b0, 1.f - a0 - b0, a1, b1, 1.f - a1 - b1,
                                           a2, b2, 1.f - a2 - b2)) ++empty;
                            ++total;
                            if (i + j < N - 1) {   // the interleaved "downward" one
                                const float d0 = (i + 1) * inv, e0 = (j + 1) * inv;
                                const float d1 = i * inv,       e1 = (j + 1) * inv;
                                const float d2 = (i + 1) * inv, e2 = j * inv;
                                if (!subOpaque(d0, e0, 1.f - d0 - e0, d1, e1, 1.f - d1 - e1,
                                               d2, e2, 1.f - d2 - e2)) ++empty;
                                ++total;
                            }
                        }
                        B.cardDrop[li] += area * (total ? double(empty) / double(total) : 0.0);
                    }
                }
                B.msCards = _pc.GetElapsed_ms_total();
            }
            B.msHullPrep = _p.GetElapsed_ms_total();
            _p.Start();
            // SHADOW hull — one merged watertight shell at a modest resolution (depth-only
            // caster wants a cheap SOLID silhouette, not the fine visual cubes). Kept
            // deliberately close to the pre-voxel default (~12–16 cells across) no matter
            // how fine the visual voxels go. Lobes when the voxel cvar is off.
            GpuTreeHullInfo hs{};
            hs.ib_first = (u32)hullIdx.size();
            hs.vb_first = (u32)hullVerts.size();
            if (ps_r_vsm_tree_hull_vox > 0) {
                const u32 shellN = (std::min)(20u, (std::max)(8u, (u32)ps_r_vsm_tree_hull_vox / 4u));
                hs.ib_count = BuildHullVoxels(tris, hullVerts, hullIdx, shellN);
            } else {
                hs.ib_count = BuildHullLobes(pts, wts, hullVerts, hullIdx);
            }
            B.hull = hs;
            B.msHull = _p.GetElapsed_ms_total();
            _p.Start();

            // VISUAL voxel cloud (UE Nanite-foliage style) — kHullLods levels of individual
            // colored cubes; voxel edge DOUBLES per level (resolution halves), exactly the
            // Nanite DAG progression. Level 0 = r_vsm_tree_hull_vox cells across the crown.
            for (u32 l = 0; l < CTreeManager::kHullLods; ++l) {
                TreeVoxLod vl{}, bl{};
                vl.first = (u32)voxData.size();
                bl.first = (u32)brickData.size();
                if (ps_r_vsm_tree_hull_vox > 0) {
                    const u32 voxN = (std::max)(4u, (u32)ps_r_vsm_tree_hull_vox >> l);
                    vl.count = BuildVoxelCloud(tris, triUV, am, voxData, brickData, voxN, vl.size,
                                               s_emitVoxCloud, B.voxProf);
                    bl.count = (u32)brickData.size() - bl.first;
                    bl.size  = vl.size;
                }
                B.voxLod[l] = vl;
                B.brickLod[l] = bl;
            }
            B.msVox = _p.GetElapsed_ms_total();
        }
        // else: not foliage — no hull, so B.hull / B.voxLod / B.brickLod stay zeroed,
        // which is exactly what the old `push_back(GpuTreeHullInfo{0,0,0,0})` meant:
        // ib_count 0 = "hull unavailable", the tree keeps its real mesh.

        B.range = { base, (u32)meshlets.size() };
    };

    // ---- Run the bakes ------------------------------------------------------
    _p.Start();
    {
        const u32 nUniq = (u32)uniq.size();
        u32 nThr = std::thread::hardware_concurrency();
        nThr = _min(_max(1u, nThr), 16u);
        nThr = _min(nThr, nUniq);
        if (nThr <= 1) {
            for (u32 ui = 0; ui < nUniq; ++ui) bakeOne(ui);
        } else {
            // Dynamic hand-out, not a static split: species differ by orders of
            // magnitude in triangle count, so equal-sized chunks would leave most
            // threads idle behind one big crown.
            std::atomic<u32> next{ 0 };
            xr_vector<std::thread> pool;
            pool.reserve(nThr);
            for (u32 w = 0; w < nThr; ++w)
                pool.emplace_back([&] { for (u32 ui = next++; ui < nUniq; ui = next++) bakeOne(ui); });
            for (auto& th : pool) th.join();
        }
        msBakeWall = _p.GetElapsed_ms_total();
        msBakeThreads = nThr;
    }

    // ---- Merge in unique order (deterministic, independent of thread order) --
    _p.Start();
    for (u32 ui = 0; ui < (u32)uniq.size(); ++ui) {
        Baked& B = baked[ui];
        const u32 meshletBase = (u32)meshlets.size();
        const u32 idxBase     = (u32)mIndices.size();
        const u32 hullVBase   = (u32)hullVerts.size();
        const u32 hullIBase   = (u32)hullIdx.size();
        const u32 voxBase     = (u32)voxData.size();
        const u32 brickBase   = (u32)brickData.size();

        for (GpuMeshlet& m : B.meshlets) m.first_index += idxBase;
        meshlets.insert(meshlets.end(), B.meshlets.begin(), B.meshlets.end());
        mIndices.insert(mIndices.end(), B.mIndices.begin(), B.mIndices.end());
        uniqRange.push_back({ B.range.base + meshletBase, B.range.count });

        // Hull indices are LOCAL to the species (BuildHullLobes/BuildHullVoxels emit
        // them relative to the vertex base they started at, which is what vb_first
        // feeds the draw as vertexOffset) — so only the two bases move.
        GpuTreeHullInfo h = B.hull;
        if (h.ib_count) { h.ib_first += hullIBase; h.vb_first += hullVBase; }
        uniqHull.push_back(h);
        hullVerts.insert(hullVerts.end(), B.hullVerts.begin(), B.hullVerts.end());
        hullIdx.insert(hullIdx.end(), B.hullIdx.begin(), B.hullIdx.end());

        for (u32 l = 0; l < CTreeManager::kHullLods; ++l) {
            TreeVoxLod vl = B.voxLod[l], bl = B.brickLod[l];
            if (vl.count) vl.first += voxBase;
            if (bl.count) bl.first += brickBase;
            uniqVoxLods.push_back(vl);
            uniqBrickLods.push_back(bl);
        }
        voxData.insert(voxData.end(), B.vox.begin(), B.vox.end());
        brickData.insert(brickData.end(), B.bricks.begin(), B.bricks.end());

        alphaSpecies += B.alphaHit; blindSpecies += B.alphaMiss;
        msCluster += B.msCluster; msHullPrep += B.msHullPrep;
        msHull    += B.msHull;    msVox      += B.msVox;
        voxProf.raster += B.voxProf.raster; voxProf.cubes += B.voxProf.cubes;
        voxProf.bricks += B.voxProf.bricks;

        cardArea += B.cardArea; cardCovered += B.cardCovered; cardTris += B.cardTris;
        cardDropDense += B.cardDropDense; cardFaintArea += B.cardFaintArea; cardEmptyTris += B.cardEmptyTris;
        for (u32 l = 0; l < kCardLevels; ++l) cardDrop[l] += B.cardDrop[l];
        msCards += B.msCards;
        if (B.cardTris) ++cardSpecies;
    }
    msMerge = _p.GetElapsed_ms_total();

    // ---- Card-tightness verdict (r_tree_stats >= 2) --------------------------
    // ⭐ THE NUMBER THAT DECIDES WHETHER TO BUILD THE TIGHT-CARD BAKE AT ALL. It is
    // the geometric area of leaf cards that contains no opaque texel — i.e. the share
    // of the canopy's fill that exists to be discarded. Whatever fraction of it a real
    // implementation recovers, it recovers in EVERY tree pass at once (colour, prepass,
    // motion vectors, shadow), because all four pay the same alpha test.
    // ⚠ Area here is MESH-LOCAL and unweighted by how often a species appears or how
    // near it stands. It is the ceiling, not the frame saving.
    if (cardSurvey && cardTris) {
        Msg("[VK TreeCards] foliage surveyed: %u species, %u tris, %.0f m2 of card area (%.0f ms) | opaque coverage %.1f%%",
            cardSpecies, cardTris, cardArea, msCards,
            100.0 * cardCovered / cardArea);
        Msg("[VK TreeCards] EMPTY area droppable: whole-tri %.1f%% | split 2x2 %.1f%% | split 4x4 %.1f%%  (7-tap estimate, of card area)",
            100.0 * cardDrop[0] / cardArea, 100.0 * cardDrop[1] / cardArea,
            100.0 * cardDrop[2] / cardArea);
        // ⚠ The number to actually believe. 153 taps instead of 7 — see the note at the
        // dense lattice. Where this sits well BELOW the 7-tap figure, the cheap estimate
        // was calling cards empty that hold a leaf, and shipping that would punch holes.
        Msg("[VK TreeCards] whole-tri droppable, 153-tap check: %.1f%% of area (%u of %u tris) | near-empty (<5%% opaque) a further %.1f%%",
            100.0 * cardDropDense / cardArea, cardEmptyTris, cardTris,
            100.0 * cardFaintArea / cardArea);
        // The verdict, stated where it will be read next time rather than left to be
        // re-derived: geometry can only remove area that is EMPTY IN BLOCKS. Foliage
        // alpha is fine-grained (gaps BETWEEN leaves), so most of the 69% transparent
        // area is unreachable by any triangle, at any subdivision that stays cheaper
        // than the fill it saves.
        Msg("[VK TreeCards] => tightening can reach %.0f%% of the %.0f%% transparent area; the rest is gaps between leaves, not empty margins",
            100.0 * cardDrop[2] / (cardArea - cardCovered),
            100.0 * (cardArea - cardCovered) / cardArea);
    }

    if (meshlets.empty() || mIndices.empty()) { Msg("[VK Trees] Meshlets: nothing built"); return; }
    _p.Start();   // → msUp: per-tree table expansion + every UploadDeviceLocal below

    // Per-tree range (index-aligned with transforms/meta); trees with no mesh get {0,0}.
    xr_vector<GpuTreeMeshletRange> treeRange(m_TotalCount, GpuTreeMeshletRange{ 0, 0 });
    for (u32 i = 0; i < m_TotalCount; ++i)
        if (treeUniq[i] != ~0u) treeRange[i] = uniqRange[treeUniq[i]];

    // Upload: GpuMeshlet[] (SSBO), dedicated meshlet IB (u16), per-tree ranges (SSBO).
    UploadDeviceLocal(m_MeshletBuffer, meshlets.data(), meshlets.size() * sizeof(GpuMeshlet), 0);
    UploadDeviceLocal(m_MeshletIndexBuffer, mIndices.data(), mIndices.size() * sizeof(u16),
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    UploadDeviceLocal(m_TreeMeshletRangeBuffer, treeRange.data(),
                      treeRange.size() * sizeof(GpuTreeMeshletRange), 0);

    m_MeshletTotal      = (u32)meshlets.size();
    m_MeshletIndexTotal = (u32)mIndices.size();
    m_MeshletsReady     = true;
    Msg("[VK Trees] Meshlets: %u clusters (%u tris/cluster) from %u unique meshes, %u idx (%u KB) — %u instances",
        m_MeshletTotal, kMeshletTris, (u32)uniq.size(), m_MeshletIndexTotal,
        (u32)(m_MeshletIndexTotal * sizeof(u16) / 1024), m_TotalCount);

    // --- Crown-HULL upload (r_vsm_tree_hull): tight vec3 VB + u16 IB + per-tree info. ---
    if (!hullVerts.empty() && !hullIdx.empty()) {
        xr_vector<GpuTreeHullInfo> treeHull(m_TotalCount, GpuTreeHullInfo{ 0, 0, 0, 0 });
        for (u32 i = 0; i < m_TotalCount; ++i)
            if (treeUniq[i] != ~0u) treeHull[i] = uniqHull[treeUniq[i]];
        // Per-tree × kHullLods voxel-cloud table for the graded viewmode + the parallel
        // brick table for the shadow caster.
        m_VoxLodInfoCPU.assign((size_t)m_TotalCount * CTreeManager::kHullLods, TreeVoxLod{});
        m_BrickLodInfoCPU.assign((size_t)m_TotalCount * CTreeManager::kHullLods, TreeVoxLod{});
        for (u32 i = 0; i < m_TotalCount; ++i)
            if (treeUniq[i] != ~0u)
                for (u32 l = 0; l < CTreeManager::kHullLods; ++l) {
                    m_VoxLodInfoCPU[(size_t)i * CTreeManager::kHullLods + l] =
                        uniqVoxLods[(size_t)treeUniq[i] * CTreeManager::kHullLods + l];
                    m_BrickLodInfoCPU[(size_t)i * CTreeManager::kHullLods + l] =
                        uniqBrickLods[(size_t)treeUniq[i] * CTreeManager::kHullLods + l];
                }
        UploadDeviceLocal(m_HullVB, hullVerts.data(), hullVerts.size() * sizeof(Fvector),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        UploadDeviceLocal(m_HullIB, hullIdx.data(), hullIdx.size() * sizeof(u16),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        UploadDeviceLocal(m_HullInfoBuffer, treeHull.data(), treeHull.size() * sizeof(GpuTreeHullInfo), 0);
        m_HullInfoCPU = treeHull;   // shadow-shell ranges (also the debug fallback overlay)
        // The VISUAL cube cloud is what the forward voxel tier draws (r_tree_vox_dist),
        // and what the r_vsm_tree_hull_debug viewmode inspects. It costs ~12 MB of VRAM
        // on a big level, so it is uploaded only when something will read it — for a
        // while (12-08 .. 24-08) nothing did and the upload was gated off entirely.
        // The bake itself can never be skipped: BuildVoxelCloud emits the cubes and the
        // SHADOW BRICKS (which the live caster path does use) in one pass.
        // ⚠ Decided at LOAD: turning r_tree_vox_dist on from 0 needs a level reload to
        // put the cubes in VRAM. Turning it OFF is live (a clean A/B).
        const bool wantVoxCloud = ps_r_tree_vox_dist > 0.f
                               || ps_r_vsm_tree_hull_debug > 0
                               || ps_r_vsm_tree_vox_cloud > 0;
        m_VoxTotal = wantVoxCloud ? (u32)voxData.size() : 0u;
        if (m_VoxTotal)
            UploadDeviceLocal(m_VoxVB, voxData.data(), voxData.size() * sizeof(GpuTreeVoxel),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        // ----- Stamp TREE_VOX_BACKED (see vk_TreeManager.h). The bit says "this crown
        // can be REPLACED by cubes", so it demands all three: the cloud reached the GPU,
        // this tree owns a non-empty finest level, and the buffer actually exists. Trees
        // that fail any of them keep their mesh at every range — the same availability
        // rule TREE_FLOD_BACKED follows, and the reason neither cut can make a tree
        // vanish. Trunks never qualify: only windClass 2 is voxelized at all.
        u32 voxBacked = 0;
        if (m_VoxTotal && m_VoxVB && m_VoxVB->GetHandle() != VK_NULL_HANDLE) {
            // The LOD ladder is uploaded PER SPECIES (uniqVoxLods, one row of kHullLods
            // per unique mesh) — expanding it per instance would be 11 MB of VRAM to
            // repeat a few hundred rows. The instance's row index rides in the spare
            // high bits of `flags`, so the cull shader needs no second per-tree array.
            UploadDeviceLocal(m_VoxLodTable, uniqVoxLods.data(),
                              uniqVoxLods.size() * sizeof(TreeVoxLod), 0);
            if (m_VoxLodTable && m_VoxLodTable->GetHandle() != VK_NULL_HANDLE) {
                m_VoxLodSpecies = (u32)uniq.size();
                for (u32 i = 0; i < m_TotalCount; ++i) {
                    if (treeUniq[i] == ~0u) continue;
                    if (!m_VoxLodInfoCPU[(size_t)i * CTreeManager::kHullLods].count) continue;
                    // nfs21 parks the FLOD group's k in bits 24..31, so the species row owns only bits 8..23
                    // (tree_cull masks it with 0xFFFF). Without the mask k was OR-ed into the row and voxSelect
                    // read past the table. A level with > 65535 baked species would simply keep its meshes.
                    if (treeUniq[i] > TREE_VOXLOD_MASK) continue;
                    meta[i].flags |= TREE_VOX_BACKED | (treeUniq[i] << TREE_VOXLOD_SHIFT);
                    ++voxBacked;
                }
            }
        }
        m_VoxBackedCount = voxBacked;
        Msg("[VK Trees] VOX-backed: %u of %u instances (%.1f%%) can hand their crown to the "
            "cube cloud (%u species x %u LODs, %u KB table) — r_tree_vox_dist replaces the "
            "leaf-card mesh past its range",
            voxBacked, m_TotalCount, m_TotalCount ? 100.0 * double(voxBacked) / double(m_TotalCount) : 0.0,
            m_VoxLodSpecies, CTreeManager::kHullLods,
            // m_VoxLodSpecies, not uniqVoxLods.size(): the CPU table is built either way,
            // but with the tier off nothing was uploaded, and printing the would-be size
            // next to "0 species" reads as an allocation that does not exist. Same rule
            // the cube-count line above already follows.
            (u32)((size_t)m_VoxLodSpecies * CTreeManager::kHullLods * sizeof(TreeVoxLod) / 1024));
        m_BrickTotal = (u32)brickData.size();
        if (m_BrickTotal)
            UploadDeviceLocal(m_BrickVB, brickData.data(), brickData.size() * sizeof(GpuTreeBrick), 0);
        m_HullLobeTotal = (u32)hullIdx.size() / 3;   // total hull triangles (shape-agnostic: lobes or voxels)
        m_HullDataReady = true;
        if (!wantVoxCloud && !voxData.empty())
            Msg("[VK Trees] visual voxel cloud NOT uploaded (%u cubes, %u KB saved) — r_tree_vox_dist is 0; set it and reload the level for the forward cube tier",
                (u32)voxData.size(), (u32)(voxData.size() * sizeof(GpuTreeVoxel) / 1024));
        Msg("[VK Trees] Crown hulls: %s, %u tris / %u species, %u verts (%u KB); voxel cloud %u cubes + %u shadow bricks x%u LODs (%u+%u KB), alpha-aware %u/%u species — r_vsm_tree_hull ready",
            ps_r_vsm_tree_hull_vox > 0 ? "VOXEL shell" : "PCA lobes",
            m_HullLobeTotal, (u32)uniq.size(), (u32)hullVerts.size(),
            (u32)((hullVerts.size() * sizeof(Fvector) + hullIdx.size() * sizeof(u16)) / 1024),
            m_VoxTotal, m_BrickTotal, CTreeManager::kHullLods,
            // m_VoxTotal, not voxData.size(): when the cloud is gated off the buffer
            // is empty, and printing the would-be size next to "0 cubes" reads as a
            // 12 MB allocation that does not exist.
            (u32)((size_t)m_VoxTotal * sizeof(GpuTreeVoxel) / 1024),
            (u32)(brickData.size() * sizeof(GpuTreeBrick) / 1024),
            alphaSpecies, alphaSpecies + blindSpecies);
    }

    msUp = _p.GetElapsed_ms_total();
    Msg("[load step]   Trees::Meshlets: readback %.0f (%u ranges, 1 submit, %u MB) | alpha %.0f | bake WALL %.0f on %u threads | merge %.0f | expand+upload %.0f ms",
        msReadback, readbacks, (u32)(bytesRead >> 20), msAlpha, msBakeWall, msBakeThreads, msMerge, msUp);
    Msg("[load step]     bake CPU-time across threads: cluster %.0f | hullPrep %.0f | hull %.0f | vox %.0f ms (sum %.0f vs wall %.0f)",
        msCluster, msHullPrep, msHull, msVox, msCluster + msHullPrep + msHull + msVox, msBakeWall);
    Msg("[load step]     of the vox bake: raster %.0f | cubes %.0f (%s) | bricks %.0f ms",
        voxProf.raster, voxProf.cubes, s_emitVoxCloud ? "emitted" : "skipped", voxProf.bricks);
}

void CTreeManager::CreateIndirectBuffers()
{
    const VkDeviceSize indirectSize =
        (VkDeviceSize)m_Groups.size() * m_MaxGroupMeshCount *
        sizeof(VkDrawIndexedIndirectCommand);
    // ⚠ gpuOnly = true, and it is NOT decoration. Without it the storage-buffer branch
    // in CVulkanBuffer::Create adds HOST_ACCESS_SEQUENTIAL_WRITE, VMA then picks
    // HOST_VISIBLE memory, and a buffer this size lands in the ~256 MB BAR heap — or in
    // system RAM once BAR is full, at which point every shader access rides PCIe. That
    // is the pathology written up at the top of vk_buffer.cpp, and this buffer was
    // sitting in it: `[VK Vram] Trees buf:storage 98 MB -> DEVICE_LOCAL +HOST_VISIBLE
    // (BAR)` on pripyat_full (25-08). Nothing on the CPU ever writes it — the cull
    // compute fills it and vkCmdDrawIndexedIndirectCount reads it.
    m_TreeIndirectBuffer = xr_new<CVulkanBuffer>();
    m_TreeIndirectBuffer->Create(indirectSize,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT  |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true /*gpuOnly*/);

    // One u32 per group, plus TWO more slots at the end for the forward cube tier:
    // [groups] = its draw count, [groups+1] = the CUBES those draws carry. Sharing the
    // buffer is what makes the tier free of extra plumbing: the single vkCmdFillBuffer
    // below already zeroes it, and vkCmdDrawIndirectCount reads its count straight from
    // offset groups*4. The cube total is telemetry, and it is not optional — draw count
    // alone cannot tell a cheap tier from a ruinous one, since one draw carries anything
    // from a handful of cubes to a whole level-0 grid.
    const VkDeviceSize countSize = ((VkDeviceSize)m_Groups.size() + 2) * sizeof(u32);
    m_TreeDrawCountBuffer = xr_new<CVulkanBuffer>();
    m_TreeDrawCountBuffer->Create(countSize,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT  |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT    |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,   // forward drawn-count readback (r_profiler)
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true /*gpuOnly*/);

    // ---- Forward crown cube tier (r_tree_vox_dist). Allocated only when the species
    // LOD table exists, i.e. when the cloud was actually baked AND uploaded — the tier
    // is otherwise inert and nothing should reserve VRAM for it.
    if (m_VoxLodSpecies) {
        m_VoxDrawCmds = xr_new<CVulkanBuffer>();
        m_VoxDrawCmds->Create((VkDeviceSize)kVoxDrawCap * sizeof(VkDrawIndirectCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true /*gpuOnly*/);
        m_VoxDrawParams = xr_new<CVulkanBuffer>();
        m_VoxDrawParams->Create((VkDeviceSize)kVoxDrawCap * sizeof(GpuVoxDraw),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true /*gpuOnly*/);
        if (m_VoxDrawCmds->GetHandle() == VK_NULL_HANDLE ||
            m_VoxDrawParams->GetHandle() == VK_NULL_HANDLE) {
            Msg("![VK Trees] no VRAM for the crown cube tier (%u KB) — r_tree_vox_dist inert this level",
                (u32)((kVoxDrawCap * (sizeof(VkDrawIndirectCommand) + sizeof(GpuVoxDraw))) / 1024));
            m_VoxLodSpecies = 0;   // the one gate every consumer reads
        }
    }

    Msg("[VK Trees] Indirect buffer: %u KB, draw-count: %u B, cube tier: %s",
        (u32)(indirectSize / 1024), (u32)countSize,
        m_VoxLodSpecies ? "ready" : "off");
}

// ============================================================================
// One COMBINED_IMAGE_SAMPLER per unique diffuse texture. Layout is reused by
// Session B's graphics pipeline (set=1).
// ============================================================================
void CTreeManager::CreateTextureDescriptors(const xr_vector<VkImageView>& uniqueViews)
{
    if (uniqueViews.empty()) return;
    m_TexViews = uniqueViews;   // RT world: the gather needs the image, not this set

    // Sampler — REPEAT addressing (UVs are SHORT4 quantized with 16:1 tile).
    {
        VkSamplerCreateInfo sci{};
        sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod       = VK_LOD_CLAMP_NONE;
        sci.anisotropyEnable = VK_FALSE;
        vkCreateSampler(VulkanHW.m_Device, &sci, nullptr, &m_TexSampler);
    }

    // Layout: 1 binding, COMBINED_IMAGE_SAMPLER (fragment); one set per unique view.
    const u32 setCount = (u32)uniqueViews.size();
    m_TexDescLayout = VK::MakeSetLayout({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
                                        VK_SHADER_STAGE_FRAGMENT_BIT, "Trees.Tex");
    m_TexDescPool   = VK::MakeDescriptorPool({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
                                             setCount, "Trees.Tex");
    if (!m_TexDescLayout || !m_TexDescPool) return;

    // Allocate + write.
    m_TexDescSets.resize(setCount);
    for (u32 i = 0; i < setCount; ++i)
    {
        if (!VK::AllocSets(m_TexDescPool, m_TexDescLayout, 1, &m_TexDescSets[i], "Trees.Tex")) return;

        VK::DescriptorWriter(m_TexDescSets[i])
            .ImageSampler(0, uniqueViews[i], m_TexSampler)
            .Flush();
    }
}

// ============================================================================
// Destroy — free all GPU resources. Safe to call on partially-built or empty
// state (Build() may early-return on no trees).
// ============================================================================
void CTreeManager::Destroy()
{
    // The crown-alpha preload must never outlive the level: a still-joinable
    // std::thread at static destruction time calls std::terminate.
    JoinCrownAlphaPreload();
    s_alphaCache.clear();

    DestroySessionB();

    auto destroyBuf = [](CVulkanBuffer*& b) {
        if (b) { b->Destroy(); xr_delete(b); }
    };
    destroyBuf(m_TreeMetadataBuffer);
    destroyBuf(m_TreeTransformsBuffer);
    destroyBuf(m_SwiInstBuffer);   // nfs21
    destroyBuf(m_SwiWinBuffer);
    destroyBuf(m_TreeLodCBuffer);  // crown crossfade (set 0 binding 2)
    destroyBuf(m_TreeIndirectBuffer);
    destroyBuf(m_TreeDrawCountBuffer);
    destroyBuf(m_FwdCountRB);
    m_FwdCountPtr = nullptr;
    destroyBuf(m_TreeGIBuffer);   // per-tree probe field (r_tree_gi_inst)
    // Tree ledger (r_tree_stats). The query pools are level-scoped like everything
    // else here: the slot ring is keyed on frames in flight, and carrying half-written
    // pools across a level load would harvest the previous level's canopy.
    // GPU-culled prepass (r_tree_depth_gpu). The pool owns the set — freeing the pool
    // frees it; the set handle must be cleared with it or the next level re-uses a
    // dangling one.
    destroyBuf(m_DepthIndirect);
    destroyBuf(m_DepthCount);
    if (m_DepthCullPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(VulkanHW.m_Device, m_DepthCullPool, nullptr);
        m_DepthCullPool = VK_NULL_HANDLE;
    }
    m_DepthCullSet   = VK_NULL_HANDLE;
    m_DepthCullValid = false;
    // GPU-culled shadow targets (r_tree_shadow_gpu) — same ownership rule: the pool owns
    // the set, and m_ShadowCullFailed has to be cleared with it or a level that ran out
    // of VRAM once would keep the path switched off for every level after it.
    destroyBuf(m_ShadowIndirect);
    destroyBuf(m_ShadowCount);
    if (m_ShadowCullPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(VulkanHW.m_Device, m_ShadowCullPool, nullptr);
        m_ShadowCullPool = VK_NULL_HANDLE;
    }
    m_ShadowCullSet    = VK_NULL_HANDLE;
    m_ShadowCullValid  = 0;
    m_ShadowCullFailed = false;
    m_GroupCmdBase.clear();
    m_DepthSortKeys.clear();

    destroyBuf(m_CullStats);
    destroyBuf(m_CullStatsRB);
    m_CullStatsPtr = nullptr;
    if (m_StatPool != VK_NULL_HANDLE) { vkDestroyQueryPool(VulkanHW.m_Device, m_StatPool, nullptr); m_StatPool = VK_NULL_HANDLE; }
    if (m_OccPool  != VK_NULL_HANDLE) { vkDestroyQueryPool(VulkanHW.m_Device, m_OccPool,  nullptr); m_OccPool  = VK_NULL_HANDLE; }
    for (bool& u : m_StatUsed) u = false;
    m_StatOpen = -1; m_OccOpen = false;
    memset(m_StatVal, 0, sizeof(m_StatVal));
    m_OccSamples = 0;
    destroyBuf(m_MeshletBuffer);           // Phase A meshlet clusters
    destroyBuf(m_MeshletIndexBuffer);
    destroyBuf(m_TreeMeshletRangeBuffer);
    m_MeshletsReady = false;
    m_MeshletTotal = m_MeshletIndexTotal = 0;
    destroyBuf(m_HullVB);                  // crown-hull bake (r_vsm_tree_hull)
    destroyBuf(m_HullIB);
    destroyBuf(m_HullInfoBuffer);
    destroyBuf(m_VoxVB);
    destroyBuf(m_BrickVB);
    destroyBuf(m_VoxDrawCmds);             // forward crown cube tier (r_tree_vox_dist)
    destroyBuf(m_VoxDrawParams);
    destroyBuf(m_VoxLodTable);
    destroyBuf(m_VoxCountRB);
    m_VoxCountPtr   = nullptr;
    m_VoxLodSpecies = 0;
    m_VoxFwdDraws   = 0;
    m_VoxBackedCount = 0;
    m_HullInfoCPU.clear();
    m_VoxLodInfoCPU.clear();
    m_BrickLodInfoCPU.clear();
    m_WindClassCPU.clear();
    m_HullDataReady = false;
    m_HullLobeTotal = 0;
    m_VoxTotal      = 0;
    m_BrickTotal    = 0;

    m_TexDescSets.clear();
    m_TexViews.clear();
    if (m_TexDescPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(VulkanHW.m_Device, m_TexDescPool, nullptr);
        m_TexDescPool = VK_NULL_HANDLE;
    }
    if (m_TexDescLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(VulkanHW.m_Device, m_TexDescLayout, nullptr);
        m_TexDescLayout = VK_NULL_HANDLE;
    }
    if (m_TexSampler != VK_NULL_HANDLE) {
        vkDestroySampler(VulkanHW.m_Device, m_TexSampler, nullptr);
        m_TexSampler = VK_NULL_HANDLE;
    }

    m_Groups.clear();
    m_TotalCount = 0;
    m_MaxGroupMeshCount = 0;
    m_bBuilt = false;
}

}  // namespace VK

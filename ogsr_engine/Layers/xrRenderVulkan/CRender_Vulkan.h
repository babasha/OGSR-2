// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// CRender_Vulkan — top-level renderer (the IRender_interface impl) that the
// engine talks to via the global ::Render pointer. Mirrors R4's class CRender.
//
// Most overrides are no-op stubs that get fleshed out as each subsystem ports
// over. Frame work and resource management belong to the framegraph; this
// class stays a thin adapter on top.

#pragma once
#include "vk_core.h"
#include "../../xr_3da/Render.h"
#include "../../xr_3da/pure.h"
#include "../../xr_3da/fmesh.h"  // FSlideWindowItem

namespace VK { class CVulkanShader; class CVulkanBuffer; class CDetailManager; class CTreeManager; class CLODManager; }
class vkModelPool;
class vkCWallmarksEngine;
class vkCHOM;
class CStreamReader;
class CSkeletonWallmark;  // shared SkeletonCustom.cpp -> append_SkeletonWallmark

// Naming follows OGSR R4 convention: class CRender + global RImplementation.
// The filename keeps the _Vulkan suffix so VK sources are obviously distinct
// from R4. Not marked `final` so monolith-derived render structures can
// extend it as they get grafted in.
class CRender : public IRender_interface, public pureFrame
{
public:
    // ----- Level data ------------------------------------------------------
    // Level-loaded shader table — index matches OGF visual `shader_id`.
    // Populated by level_Load() from the fsL_SHADERS chunk; visuals look up
    // their material via `Shaders[shader_id]->GetMaterial()`.
    xr_vector<VK::CVulkanShader*>  Shaders;

    // Level visuals (one per OGF entry in fsL_VISUALS chunk).
    xr_vector<IRenderVisual*>      Visuals;

    // Level vertex/index buffers — normal (`level.geom`) and extended/fast
    // (`level.geomx`, used for shadow/HOM passes once those land).
    xr_vector<VK::CVulkanBuffer*>  nVB, xVB;
    xr_vector<VK::CVulkanBuffer*>  nIB, xIB;
    xr_vector<u32>                 nVB_Strides, xVB_Strides;

    // Sliding-window items (LOD index ranges per progressive mesh).
    xr_vector<FSlideWindowItem>    SWIs;

    BOOL                           b_loaded = FALSE;

    // ----- Subsystems ------------------------------------------------------
    // Real impl: vkModelPool. Created in CRender::create() once the device is
    // up so model_Create can resolve names against `$level$` / `$game_meshes$`.
    vkModelPool*                   Models   = nullptr;

    // Stubs until those subsystems port over. nullptr is the working contract:
    // rvk_loader.cpp and level_Unload guard with `if (X)` before touching them.
    vkCWallmarksEngine*            Wallmarks = nullptr;
    vkCHOM*                        HOM       = nullptr;
    VK::CDetailManager*            Details   = nullptr;
    VK::CTreeManager*              Trees     = nullptr;
    VK::CLODManager*               LODs      = nullptr;

    // ----- Render options (only the flags rvk_loader actually reads) -------
    struct _options
    {
        u32 volumetricfog : 1;  // gates Load3DFluid (currently always 0)
    } o{};

public:
    CRender();
    ~CRender() override;

    // ----- Level loading helpers (bodies in rvk_loader.cpp) ----------------
    void LoadBuffers (CStreamReader* base_fs, BOOL alternative);
    void LoadSWIs    (CStreamReader* base_fs);
    FSlideWindowItem* getSWI(int id);   // resolve OGF_SWICONTAINER id → pooled SWI
    void LoadVisuals (IReader* fs);
    void LoadSectors (IReader* fs);
    void LoadLights  (IReader* fs);
    void Load3DFluid ();

    // ----- IRender_interface : Loading / Unloading --------------------------
    void create() override;
    void destroy() override;
    void reset_begin() override;
    void reset_end() override;
    void level_Load(IReader*) override;
    void level_Unload() override;

    // ----- IRender_interface : Shader compile -------------------------------
    HRESULT shader_compile(LPCSTR name, DWORD const* pSrcData, UINT SrcDataLen,
                           LPCSTR pFunctionName, LPCSTR pTarget, DWORD Flags,
                           void*& result) override;

    // ----- IRender_interface : Information ----------------------------------
    LPCSTR getShaderPath() override;
    IRenderVisual* getVisual(int id) override;
    u32 getVisualCount() override;
    IRender_Target* getTarget() override;

    // ----- IRender_interface : Visuals & wallmarks --------------------------
    void add_Visual(u32 context_id, IRenderable* root,
                    IRenderVisual* V, Fmatrix& m) override;
    // Set only around the SMAP collection at the tail of Calculate(): while true,
    // add_Visual routes into VK::g_ShadowOnlyVisuals instead of the drawn lists.
    bool b_collect_shadow_only{false};
    void set_no_shadow_collect(bool on) override { b_collect_no_shadow = on; }
    bool b_collect_no_shadow{false};
    void add_StaticWallmark(const wm_shader& S, const Fvector& P, float s,
                            CDB::TRI* T, Fvector* V) override;
    void add_StaticWallmark(IWallMarkArray* pArray, const Fvector& P, float s,
                            CDB::TRI* T, Fvector* V) override;
    void add_SkeletonWallmark(Fmatrix* xf, IKinematics* obj,
                              IWallMarkArray* pArray, Fvector& start,
                              Fvector& dir, float size) override;
    void clear_static_wallmarks() override;

    IRender_ObjectSpecific* ros_create(IRenderable* parent) override;
    void ros_destroy(IRender_ObjectSpecific*&) override;

    // ----- IRender_interface : Lighting -------------------------------------
    IRender_Light* light_create() override;

    // ----- IRender_interface : Particles ------------------------------------
    void ParticleEffectFillName(xr_vector<shared_str>& s) override;
    void ParticleGroupFillName(xr_vector<shared_str>& s) override;
    float GetParticlesTimeLimit(LPCSTR name) override;

    // ----- IRender_interface : Models ---------------------------------------
    IRenderVisual* model_CreateParticles(LPCSTR name, BOOL bNoPool) override;
    IRenderVisual* model_Create(LPCSTR name, IReader* data) override;
    IRenderVisual* model_CreateChild(LPCSTR name, IReader* data) override;
    IRenderVisual* model_Duplicate(IRenderVisual* V) override;
    void model_Delete(IRenderVisual*& V, BOOL bDiscard = FALSE) override;
    void model_Logging(BOOL bEnable) override;
    void models_Prefetch() override;
    void models_Clear(BOOL b_complete) override;
    void models_savePrefetch() override;
    void models_begin_prefetch1(bool val) override;

    // Skeleton wallmarks (decals on skinned meshes). No-op for now — the Vulkan
    // path doesn't render skeleton wallmarks yet. Shared SkeletonCustom.cpp calls
    // this from CKinematics::AddWallmark. Param by const-ref so the definition
    // needs only a forward decl of CSkeletonWallmark.
    void append_SkeletonWallmark(const intrusive_ptr<CSkeletonWallmark>& wm);

    // ----- IRender_interface : Frame ----------------------------------------
    void Calculate() override;
    void Render() override;
    void AfterWorldRender() override;
    void AfterUIRender() override;
    void Screenshot(ScreenshotMode mode, LPCSTR name) override;

    // ----- IRender_interface : Render mode ----------------------------------
    void rmNear(CBackend& cmd_list) override;
    void rmFar(CBackend& cmd_list) override;
    void rmNormal(CBackend& cmd_list) override;

    // ----- IRender_interface : Stats ----------------------------------------
    u32 memory_usage() override;
    u32 GetCacheStatPolys() override;

    // ----- IRender_interface : Backbuffer -----------------------------------
    void Begin() override;
    // TWO-SLOT PIPELINE: Begin() = Begin_Extract() only (main thread: the frame's view/environment snapshots, ring slot, the
    // wait for that slot's previous GPU user, the texture-streamer tick). Begin_Render is the render side -- cluster streaming
    // tick, swapchain acquire, command buffer begin, frame-graph begin, jitter -- and runs on the render thread at the head of
    // the frame's first job (EnsureRenderBegun).
    void Begin_Extract();
    void Begin_Render();
    void EnsureRenderBegun();   // render thread: Begin_Render once per frame, at the head of the frame's first job
    // Render() = the extract finish (PREPARE) + a Render_Record() job queued to the render thread (every frame, never inline).
    void Render_Record();
    void Clear() override;
    void End() override;
    void ClearTarget() override;
    bool PrecacheWarmupPending() override;

    // ----- IRender_interface : Cached transforms ----------------------------
    void SetCacheXform(Fmatrix& mView, Fmatrix& mProject) override;
    void SetCacheXformOld(Fmatrix& mView, Fmatrix& mProject) override;

    // ----- IRender_interface : Backend & camera -----------------------------
    CBackend& get_imm_command_list() override;
    void OnCameraUpdated(bool from_actor) override;

    // ----- pureFrame --------------------------------------------------------
    void OnFrame() override;

protected:
    void ScreenshotImpl(ScreenshotMode mode, LPCSTR name) override;
};

extern CRender RImplementation;

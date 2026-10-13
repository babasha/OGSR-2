// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_ModelPool.h"
#include "CRender_Vulkan.h"  // forward-decl for `friend class CRender`
#include "../../xr_3da/fmesh.h"  // ogf_header
#include "../../xr_3da/SkeletonMotions.h"  // g_pMotionsContainer
#include "../../xrCore/load_prof.h"         // per-spawn breakdown (model.read/load/dup/pool)
#include <unordered_set>  // DeleteQueue() dedup (avoid double-free on duplicate queue entries)

#ifdef __SWITCH__
extern "C" volatile int g_xr_nx_loading;          // nx/xr_nx_pad.c: 1 while the engine loads
extern "C" void xr_nx_backtrace(const char* tag); // nx/xr_nx_throw.cpp
#endif

// ============================================================================
// vkModelPool - Constructor/Destructor
// ============================================================================
vkModelPool::vkModelPool()
{
    bLogging = FALSE;
    bForceDiscard = FALSE;
    bAllowChildrenDuplicate = TRUE;

    // Initialize motions container (required for skeletal animations)
    g_pMotionsContainer = xr_new<motions_container>();
}

vkModelPool::~vkModelPool()
{
    Destroy();
    Msg("[VK] vkModelPool::~: Destroy() done, freeing motions container ...");
    xr_delete(g_pMotionsContainer);
    Msg("[VK] vkModelPool::~: motions container freed");
}

void vkModelPool::Destroy()
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    Msg("[VK] vkModelPool::Destroy: queue=%u pool=%u models=%u registry=%u",
        (u32)ModelsToDelete.size(), (u32)Pool.size(), (u32)Models.size(), (u32)Registry.size());

    // 1. Drain the deferred-delete queue: registered instances Depart() (shared
    //    bones still alive here) and are xr_delete'd or moved to Pool. Never
    //    frees shared base data — see DeleteInternal.
    DeleteQueue();

    // 2. Delete pooled instances. They share their base model's bones / buffers,
    //    so xr_delete WITHOUT Release() (~CKinematics frees only per-instance
    //    bone_instances; ~VK_Render_Mesh skips shared buffers).
    for (POOL_IT it = Pool.begin(); it != Pool.end(); ++it)
        xr_delete(it->second);
    Pool.clear();

    // 3. Now that every instance is gone, free the base models. THIS is the one
    //    place the shared bones / GPU buffers are Release()'d — exactly once.
    for (u32 i = 0; i < Models.size(); ++i)
    {
        if (Models[i].model)
        {
            Models[i].model->Release();
            xr_delete(Models[i].model);
        }
    }
    Models.clear();
    Msg("[VK] vkModelPool::Destroy: all models freed");

    Registry.clear();

    // Cleanup motions container
    if (g_pMotionsContainer)
        g_pMotionsContainer->clean(false);
}

// ============================================================================
// Instance Management
// ============================================================================
vkRender_Visual* vkModelPool::Instance_Create(u32 Type)
{
    vkRender_Visual* V = vkVisual_Create(Type);
    return V;
}

vkRender_Visual* vkModelPool::Instance_Duplicate(vkRender_Visual* V)
{
    if (!V) return nullptr;

    // Create new instance of same type
    vkRender_Visual* N = Instance_Create(V->Type);
    if (!N) {
        Msg("![Vulkan] Instance_Duplicate: failed to create instance type %d", V->Type);
        return nullptr;
    }

    // Copy data
    N->Copy(V);

    return N;
}

vkRender_Visual* vkModelPool::Instance_Load(LPCSTR N, BOOL allow_register, bool assert_on_fail)
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    return Instance_Load(N, nullptr, allow_register);
}

vkRender_Visual* vkModelPool::Instance_Load(LPCSTR N, IReader* data, BOOL allow_register)
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    // Build filename
    string_path name;
    string_path fn;

    if (!N || !N[0])
    {
        Msg("![Vulkan] Instance_Load: empty name");
        return nullptr;
    }

    // Add extension if missing
    if (0 == strext(N))
    {
        xr_strcpy(name, N);
        xr_strcat(name, ".ogf");
    }
    else
    {
        xr_strcpy(name, N);
    }

    // Convert to lowercase
    _strlwr(name);

    // Find file
    IReader* file_data = data;
    bool need_close = false;

    if (!file_data)
    {
        // Try $level$ first, then $game_meshes$
        LoadProf::Scope _lp("model.read");
        if (FS.exist(fn, "$level$", name))
        {
            file_data = FS.r_open(fn);
            need_close = true;
        }
        else if (FS.exist(fn, "$game_meshes$", name))
        {
            file_data = FS.r_open(fn);
            need_close = true;
        }
        else
        {
            Msg("![Vulkan] Model not found: %s", name);
            return nullptr;
        }
    }

    // Read header to determine type
    ogf_header H;
    if (!file_data->find_chunk(OGF_HEADER))
    {
        Msg("![Vulkan] Missing OGF_HEADER in %s", name);
        if (need_close) FS.r_close(file_data);
        return nullptr;
    }
    file_data->r(&H, sizeof(H));
    file_data->seek(0); // Rewind for full load

    // Create visual of appropriate type
    vkRender_Visual* V = Instance_Create(H.type);
    if (!V)
    {
        Msg("![Vulkan] Failed to create visual type %u for %s", H.type, name);
        if (need_close) FS.r_close(file_data);
        return nullptr;
    }

    // Load visual data
    {
        LoadProf::Scope _lp("model.load");
        V->Load(name, file_data, 0);
    }

    // Register if requested
    if (allow_register)
    {
        Instance_Register(name, V);
    }

    if (need_close)
    {
        FS.r_close(file_data);
    }

    return V;
}

void vkModelPool::Instance_Register(LPCSTR N, vkRender_Visual* V)
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    // Check if already registered
    for (auto& def : Models)
    {
        if (def.name == N)
        {
            // Already exists
            return;
        }
    }

    // Add to models
    ModelDef def;
    def.name = N;
    def.model = V;
    def.refs = 1;
    Models.push_back(def);
}

vkRender_Visual* vkModelPool::Instance_Find(LPCSTR N)
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    string_path name;
    xr_strcpy(name, N);

    // Add extension if missing (must match Instance_Load behavior)
    if (0 == strext(N))
        xr_strcat(name, ".ogf");

    _strlwr(name);

    for (auto& def : Models)
    {
        if (def.name == name)
        {
            return def.model;
        }
    }

    return nullptr;
}

// ============================================================================
// Public API
// ============================================================================
vkRender_Visual* vkModelPool::Create(LPCSTR name, IReader* data, bool assert_on_fail)
{
    LoadProf::Scope _lp("model.pool");
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    if (!name || !name[0])
    {
        return nullptr;
    }

    string_path low_name;
    xr_strcpy(low_name, name);
    _strlwr(low_name);

    // 1. Check pool for cached instance
    POOL_IT it = Pool.find(low_name);
    if (it != Pool.end())
    {
        // Reuse from pool
        vkRender_Visual* V = it->second;
        Pool.erase(it);
        V->Spawn();
        Registry.insert(std::make_pair(V, low_name));
        return V;
    }

    // 2. Find base model
    vkRender_Visual* Base = Instance_Find(low_name);

    // 3. Load if not found
    if (!Base)
    {
        const u64 t0 = CPU::QPC();
        Base = Instance_Load(low_name, data, TRUE);
        // [model late]: a file read + mesh build on the MAIN thread in gameplay.
        // 📏07-10-2026 Switch v211: the fight-start stall (249 ms frame_move) was
        // model.read 72 + geom.buffers 57 under level.events, and no
        // cNameVisual_set logged it -- so the caller is some other path. Names
        // it, and on Switch prints who asked.
        const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
#ifdef __SWITCH__
        const bool loading = g_xr_nx_loading != 0;
#else
        const bool loading = Device.dwPrecacheFrame != 0;
#endif
        static u32 s_late = 0;
        if (ms >= 3.0 && !loading && Device.OnMainThread() && s_late < 64)
        {
            ++s_late;
            Msg("~ [model late] '%s' %.1f ms on the main thread (frame %u)", low_name, ms, Device.dwFrame);
#ifdef __SWITCH__
            xr_nx_backtrace("model late");
#endif
        }
        if (!Base)
        {
            if (assert_on_fail)
                Msg("![Vulkan] Failed to load model: %s", low_name);
            return nullptr;
        }
    }

    // 4. Duplicate base
    vkRender_Visual* V;
    {
        LoadProf::Scope _lp("model.dup");
        V = Instance_Duplicate(Base);
    }

    // 5. Register instance
    Registry.insert(std::make_pair(V, low_name));

    // 6. Increment reference count
    for (auto& def : Models)
    {
        if (def.name == low_name)
        {
            def.refs++;
            break;
        }
    }

    return V;
}

vkRender_Visual* vkModelPool::CreateChild(LPCSTR name, IReader* data)
{
    LoadProf::Scope _lp("model.child");
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    if (!data)
    {
        return nullptr;
    }

    string_path low_name;
    xr_strcpy(low_name, name);
    _strlwr(low_name);

    // Read header
    ogf_header H;
    if (!data->find_chunk(OGF_HEADER))
    {
        return nullptr;
    }
    data->r(&H, sizeof(H));
    data->seek(0);

    // Create visual
    vkRender_Visual* V = Instance_Create(H.type);
    if (!V)
    {
        return nullptr;
    }

    // Load
    V->Load(low_name, data, 0);

    // Register as active (but not as base model)
    Registry.insert(std::make_pair(V, low_name));

    return V;
}

void vkModelPool::Delete(vkRender_Visual*& V, BOOL bDiscard)
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    if (!V) return;

    // Add to deletion queue
    ModelsToDelete.push_back(V);

    // Store discard flag (use bForceDiscard as global override)
    if (bDiscard)
    {
        bForceDiscard = TRUE;
    }

    V = nullptr;
}

void vkModelPool::Discard(vkRender_Visual*& V, BOOL b_complete)
{
    DeleteInternal(V, TRUE);
}

void vkModelPool::DeleteInternal(vkRender_Visual*& V, BOOL bDiscard)
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    if (!V) return;

    // Find in registry
    REGISTRY_IT it = Registry.find(V);
    if (it != Registry.end())
    {
        shared_str name = it->second;
        Registry.erase(it);

        if (bDiscard || bForceDiscard)
        {
            // Instance teardown. CRITICAL: duplicated instances SHARE the base
            // model's heavy data — CKinematics::Copy does a shallow
            // `bones = src->bones`, and vkFVisual copies set bOwnsBuffers=false.
            // Only the BASE model may Release() (which frees the shared bones /
            // GPU buffers). An instance is destroyed via xr_delete alone:
            // ~CKinematics frees only the per-instance bone_instances, and
            // ~VK_Render_Mesh skips shared buffers. Calling Release() on an
            // instance would free the shared bones out from under the base and
            // every sibling instance → dangling pointer / double-free (this was
            // the root of the exit-teardown hang). Mirrors R4 CModelPool.
            V->Depart();
            xr_delete(V);

            // Drop a ref on the base model; free it (and its shared data) once
            // the last instance is gone. Depart() above already ran while the
            // shared bones were still alive, so this ordering is safe.
            for (auto& def : Models)
            {
                if (def.name == name)
                {
                    if (def.refs > 0)
                        def.refs--;
                    if (def.refs == 0 && def.model)
                    {
                        def.model->Release();
                        xr_delete(def.model);
                        def.model = nullptr;
                    }
                    break;
                }
            }
        }
        else
        {
            // Move to pool for reuse (no Release — shared data stays with base).
            V->Depart();
            Pool.insert(std::make_pair(name, V));
        }
    }
    else
    {
        // Not a registered instance (special/particle visual that owns its own
        // resources) — Release then delete.
        V->Release();
        xr_delete(V);
    }

    V = nullptr;
}

void vkModelPool::DeleteQueue()
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    // Index loop, not range-for: DeleteInternal can re-enter Delete() and
    // push_back into ModelsToDelete (e.g. a hierarchy's owned children), growing
    // the vector mid-drain.
    //
    // De-duplicate while draining: the same visual can be queued via multiple
    // parents (shared/link-based children). Processing one pointer twice would
    // hit DeleteInternal's "not in registry" branch on the 2nd pass and
    // Release()+xr_delete() an already-freed pointer = double-free. `seen`
    // guarantees each visual is torn down exactly once.
    std::unordered_set<vkRender_Visual*> seen;
    for (u32 i = 0; i < ModelsToDelete.size(); ++i)
    {
        vkRender_Visual* V = ModelsToDelete[i];
        if (!V || !seen.insert(V).second)
            continue;  // null or already processed this pointer

        DeleteInternal(V, bForceDiscard);  // DeleteInternal nulls its arg (local copy)
    }
    ModelsToDelete.clear();
    bForceDiscard = FALSE;
}

// ============================================================================
// Particle System — creation goes through vkCreateParticlesByName (vk_PSLibrary.cpp,
// a compat TU) so the PS visual headers stay out of this non-compat TU. These
// pool hooks are unused by the active path; kept as nullptr for the interface.
// ============================================================================
vkRender_Visual* vkModelPool::CreatePE(PS::CPEDef* /*source*/) { return nullptr; }
vkRender_Visual* vkModelPool::CreatePG(PS::CPGDef* /*source*/) { return nullptr; }

vkRender_Visual* vkModelPool::CreateParticleEffect(LPCSTR /*name*/) { return nullptr; }

// ============================================================================
// Utility
// ============================================================================
void vkModelPool::Prefetch()
{
    // TODO: Implement model prefetching
    Msg("[Vulkan] vkModelPool::Prefetch() - not implemented");
}

void vkModelPool::Prefetch_One(LPCSTR N, bool assert_on_fail)
{
    // Load into pool without creating instance
    vkRender_Visual* V = Instance_Find(N);
    if (!V)
    {
        V = Instance_Load(N, TRUE, assert_on_fail);
    }
}

bool vkModelPool::Exists(LPCSTR N)
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    string_path fn;
    string_path name;

    if (0 == strext(N))
    {
        xr_strcpy(name, N);
        xr_strcat(name, ".ogf");
    }
    else
    {
        xr_strcpy(name, N);
    }

    return FS.exist(fn, "$level$", name) || FS.exist(fn, "$game_meshes$", name);
}

void vkModelPool::ClearPool(BOOL /*b_complete*/)
{
    std::lock_guard<std::recursive_mutex> poolLock(m_Mx);
    // Drain the reuse pool only. Pooled entries are duplicated instances that
    // SHARE their base model's bones / GPU buffers, so they must be destroyed
    // via xr_delete WITHOUT Release() (Release would free the shared data the
    // base + live Registry instances still own).
    //
    // NOTE: we deliberately do NOT free base Models here, even on b_complete.
    // base-model lifetime belongs to Destroy(), which runs after every instance
    // is gone. The old code bulk-Release()'d all base Models on b_complete; at
    // level_Unload (rvk_loader.cpp ClearPool(true)) that freed the actor's
    // shared `bones` out from under 1300+ still-registered instances, so the
    // next CKinematics::Depart() walked a dangling `bones` (size()≈2.7e9) in a
    // `for(u16 b; b<bones->size())` loop that never terminates → exit hang.
    // Mirrors R4 CModelPool::ClearPool, which likewise never frees base Models.
    for (POOL_IT it = Pool.begin(); it != Pool.end(); ++it)
        xr_delete(it->second);
    Pool.clear();
}

void vkModelPool::dump()
{
    Msg("[Vulkan] === Model Pool Dump ===");
    Msg("[Vulkan] Base models: %u", Models.size());
    for (auto& def : Models)
    {
        Msg("[Vulkan]   %s (refs: %u)", def.name.c_str(), def.refs);
    }
    Msg("[Vulkan] Active instances: %u", Registry.size());
    Msg("[Vulkan] Pooled instances: %u", Pool.size());
    Msg("[Vulkan] Pending deletion: %u", ModelsToDelete.size());
}

void vkModelPool::memory_stats(u32& vb_mem_video, u32& vb_mem_system, u32& ib_mem_video, u32& ib_mem_system)
{
    // TODO: Implement memory statistics
    vb_mem_video = 0;
    vb_mem_system = 0;
    ib_mem_video = 0;
    ib_mem_system = 0;
}

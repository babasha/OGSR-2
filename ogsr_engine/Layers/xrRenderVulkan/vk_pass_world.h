// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - World (level static geometry) pass.
//
// Phase 1 of the pass refactor: render-loop body lives here, pipeline
// caching/teardown stays in vk_pipeline_cache. Caller is CRender::Render().

#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"
#include "../../xrCore/render_thread.h"

class vkRender_Visual;

namespace VK
{
    void Pass_World(FrameContext& ctx);

    // Late translucent glass flush (see Pass_WorldGlass in vk_pass_world.cpp) —
    // registered after Sky/Shafts so panes blend over the finished opaque scene.
    void Pass_WorldGlass(FrameContext& ctx);

    // PHASE B (editor-on-Vulkan, chunk 2): draw add_Visual'd model(s) with NO level.
    // Replicates Pass_World's dynamics block (rigid queue + Pass_Skinned) so the
    // -vk_editor viewport can show a lit model on the grey backdrop. See
    // vk_pass_world.cpp. Registered only in editor mode (VKEditor::Active()).
    void Pass_EditorDynamics(FrameContext& ctx);

    // Dynamic (spawned) visuals — NPCs, weapons, items, HUD. CRender::add_Visual
    // pushes {visual, world-xform} here every frame during the object traversal;
    // Pass_World drains them (in the same render pass as the level statics) with a
    // per-item MVP; CRender::Calculate() clears the list before the traversal.
    // hemi = sky-visibility factor (0..1) ray-traced at the object's position
    // (CRender::add_Visual). Gates the hemisphere sky ambient so dynamics indoors
    // (basements, rooms) go dark instead of glowing with the full sky — they have
    // no baked lightmap occlusion of their own. 1.0 = fully open sky.
    // noShadow: drawn, but withheld from the shadow/VSM caster walks WHEN something
    // else is standing in for it (see IRender_interface::set_no_shadow_collect).
    struct DynVisual { vkRender_Visual* vis; Fmatrix xform; float hemi = 1.0f; bool noShadow = false; };
    // (two-slot pipeline: the three lists live in a FRAME SLOT -- see VisualLists below)

    // First-person HUD visuals (player hands + active weapon/item). Collected
    // separately from world dynamics because they render with the HUD projection
    // (Device.mFullTransform_hud, narrow FOV, camera at origin) and a near depth
    // range so they draw on top of the world without clipping into it. Populated
    // by CRender::add_Visual when root->renderable_HUD() is set (g_hud->Render_MAIN).

    // SHADOW-ONLY visuals: casters that must reach the sun shadow maps / VSM but
    // must never be drawn into the colour pass. The one client today is the actor's
    // own model in first person (g_actor_shadow): the body is setVisible(false) so
    // you don't see yourself, yet "look down and see your own shadow -- arms and
    // weapon included" needs it rasterized into the shadow map anyway. R4 does this
    // by re-entering the dsgraph collection for the SMAP phase
    // (CHUDManager::Render_SMAP); here the collection is one list, so the routing is
    // a flag on CRender that add_Visual reads. Consumed by Skinned_UploadBones ->
    // Skinned_RenderShadow / VSM's Skinned_CollectCasters; Pass_Skinned ignores it.

    // TWO-SLOT PIPELINE (replaces RENDER_THREAD_PLAN step 5's extract/record pair and its swap): the three
    // lists live in the frame's slot. CRender::add_Visual (extract, main thread) fills the slot the main
    // thread extracts into; the PREPARE phases read it on the main thread, and the record passes on the
    // render thread read the slot of THEIR frame -- the accessors pick by role (RT::Slot()). There is no
    // swap, so extract of frame N+1 never touches what record of frame N is still walking.
    //   * BeginVisualExtract(): clear this role's slot (Begin_Extract, and again at the start of Calculate).
    //   * ClearAllVisualLists(): every slot -- level unload, behind RT_WaitIdle.
    // ⚠The entries hold raw vkRender_Visual*. They stay valid because vkModelPool::Delete only
    // queues and DeleteQueue() runs from Destroy() alone (vk_ModelPool.cpp). If that queue is ever
    // drained during a level, the release has to wait VK_FRAMES_IN_FLIGHT + 2 frames.
    struct VisualLists { xr_vector<DynVisual> dyn, hud, shadowOnly; };
    extern RT::PerSlot<VisualLists> g_VisualLists;
    inline xr_vector<DynVisual>& DynamicVisuals()    { return g_VisualLists.cur().dyn; }
    inline xr_vector<DynVisual>& HudVisuals()        { return g_VisualLists.cur().hud; }
    inline xr_vector<DynVisual>& ShadowOnlyVisuals() { return g_VisualLists.cur().shadowOnly; }
    void BeginVisualExtract();
    void ClearAllVisualLists();
}

// ed_facade.h — Editor-host facade (architecture a2: monolith-as-loadable-module).
//
// The SDK level editor loads xrEngine_VK.dll (built with EdDllBuild=true) and drives
// the engine's Vulkan renderer through these C-ABI entry points, instead of its own
// DX9 ECore device. The whole engine+renderer+game monolith stays intact behind this
// narrow facade — see the "editor-renderer-unification" design.
//
// A2.1: Ed_Ping — DLL-load / GetProcAddress liveness probe.
// A2.2: Ed_Init/Ed_RenderFrame/Ed_Shutdown — boot the engine WITHOUT its message loop
//       so an external host drives frames.
// A2.3: Ed_InitEx — render into a HOST-OWNED window (the SDK's viewport panel) instead
//       of the engine's own top-level window.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Liveness probe. Returns the magic 0x0A2D0001 so a loader confirms it linked THIS facade.
__declspec(dllexport) int Ed_Ping();

// Boot the engine + Vulkan renderer up to (not including) the main message loop.
// `extra_params` is appended to Core.Params (e.g. "-vk_editor") — read later at
// menu/render time. Returns 0 on success, 1 if already booted. Call ONCE, and call
// Ed_RenderFrame/Ed_Shutdown from the SAME thread.
__declspec(dllexport) int Ed_Init(const char* extra_params);

// Same, but renders into a window the HOST owns: the engine creates a WS_CHILD render
// surface inside `parent_hwnd` sized `width`x`height` (client coords, origin 0,0) and
// never touches the parent's style/position. `parent_hwnd` is the SDK's viewport panel
// HWND (passed as void* to keep this header free of <windows.h>). Passing null
// parent_hwnd is identical to Ed_Init (engine creates its own top-level window).
// The host must call Ed_Resize when that panel changes size.
__declspec(dllexport) int Ed_InitEx(void* parent_hwnd, int width, int height, const char* extra_params);

// Headless boot: same boot sequence as Ed_Init, but the window handed
// to Device is created without WS_VISIBLE and never shown: no taskbar entry,
// no WM_ACTIVATE, nothing to make Device.b_is_Active (device.cpp) ever become
// true, so CRenderDevice::on_idle's render half stays permanently unreachable.
// Unlike Ed_InitEx, this does NOT force b_is_Active on. Call Ed_RenderFrame /
// Ed_Shutdown exactly as with Ed_Init.
__declspec(dllexport) int Ed_InitHeadless(const char* extra_params);

// Resize the embedded render surface (host panel resized). No-op unless embedded.
__declspec(dllexport) void Ed_Resize(int width, int height);

// ---------------------------------------------------------------------------
// A2.4 (second half) — the HOST owns the camera and the object list.
//
// Until Ed_SetCamera is called the engine drives its own orbit camera and shows a
// demo model (standalone `-vk_editor`). The first Ed_SetCamera hands both over to
// the host permanently. All of these must be called from the same thread as
// Ed_RenderFrame, and only after Ed_Init/Ed_InitEx succeeded.
// ---------------------------------------------------------------------------

// Camera basis in world space (X-Ray convention, left-handed, Y up). `fov_deg` is the
// vertical FOV. Aspect is NOT taken from the host — the engine uses its own viewport
// surface's aspect, which is what the picture is actually rendered into.
__declspec(dllexport) void Ed_SetCamera(const float* pos3, const float* dir3, const float* up3, float fov_deg, float znear, float zfar);

// Read back the camera the engine is ACTUALLY rendering with, whatever set it — the
// host via Ed_SetCamera, or the engine's own orbit camera when the host does not drive
// it. Any pointer may be null. This is the "one picture" seam: with the engine owning
// the viewport input, the host mirrors this into its own camera each frame so that
// picking, gizmos and tools all reason about the same pose the user is looking at.
__declspec(dllexport) void Ed_GetCamera(float* pos3, float* dir3, float* up3, float* fov_deg);

// Load a visual by engine-relative name (e.g. "dynamics\\devices\\dev_pda\\dev_pda")
// and place it at `xform16` (4x4, row-major, same layout as Fmatrix).
// Returns a handle >= 0, or -1 if the visual could not be loaded.
__declspec(dllexport) int Ed_AddModel(const char* visual_name, const float* xform16);

// Point the renderer at an EXTRA texture root (an absolute directory, e.g. the editor's
// own gamedata\textures). Searched after the engine's own $game_textures$ and $level$.
// Without it a level authored against other assets resolves its geometry but none of its
// textures, and a missing base map becomes the 1x1 white default — an all-white terrain.
// Pass null or "" to clear.
__declspec(dllexport) void Ed_SetTextureRoot(const char* dir);

// Load a visual from an ABSOLUTE .ogf path the host produced itself, and place it at
// `xform16`. `logical_name` is the model-pool key: call it N times with the same name and
// the N instances share one base model, so a 7000-object level costs only as many loads
// as it has distinct visuals.
//
// This exists because Ed_AddModel's name lookup structurally cannot resolve an editor
// scene: the game's gamedata ships only dynamic visuals plus BAKED per-level geometry, and
// never the individual static props a level source is made of (measured: 0 of Cordon's 434
// visuals resolve). The host owns those as source .object files and exports each to a real
// .ogf, which then loads through the engine's normal pipeline — materials, shaders and LOD
// behave exactly as they do in the game.
//
// Returns a handle >= 0, or -1 if the file could not be read or parsed.
__declspec(dllexport) int Ed_AddModelFile(const char* ogf_path, const char* logical_name, const float* xform16);

// Move an already-added model.
__declspec(dllexport) void Ed_SetModelXform(int id, const float* xform16);

// Drop every model added via Ed_AddModel (handles become invalid).
__declspec(dllexport) void Ed_ClearScene();

// Unproject a point of the render window into a world-space ray: `x`,`y` are client pixels
// of the engine's own viewport window (origin top-left), `out_start3` receives the ray
// origin and `out_dir3` a normalized direction. Returns 1 on success, 0 if the engine has
// not rendered a frame yet.
//
// The ENGINE must build this ray, not the host. It is derived from Device.mInvFullTransform
// — the very matrix the frame was drawn with — so the ray agrees with the picture by
// construction. A host-built ray cannot: the engine takes its aspect from its own swapchain
// (the child window it created), while the host's projection describes the host's whole
// window, so the two disagree by exactly the viewport inset and any DLSS render-vs-display
// difference. The host still owns PICKING itself — it hit-tests this ray against its own
// scene, which knows mesh-level geometry and object classes the renderer never sees.
__declspec(dllexport) int Ed_ScreenRay(int x, int y, float* out_start3, float* out_dir3);

// ---- transform gizmo ------------------------------------------------------------
// The engine draws the gizmo and does the hit-testing; the HOST keeps every decision —
// which operation, which coordinate space, what snap, which objects, and the undo entry.
// It lives here only because a gizmo must be drawn with the matrices that drew the frame:
// the host's own ImGuizmo renders onto its DX9 surface, which is underneath this
// renderer's window and therefore invisible, and its rect covers the host's whole window
// rather than the inset viewport.
//
// Call Ed_SetGizmo every frame a gizmo should be shown (op: 0 translate, 1 rotate,
// 2 scale; mode: 0 local, 1 world; snap: null for none), then read Ed_GizmoResult, which
// returns 1 when the manipulation changed the transform and fills the updated matrix and
// this frame's delta. Results describe the previous frame — one frame of lag, which is
// imperceptible mid-drag and far simpler than driving one widget from two ImGui contexts.
__declspec(dllexport) void Ed_SetGizmo(int op, int mode, const float* snap, const float* xform16);
__declspec(dllexport) int Ed_GizmoResult(float* out_xform16, float* out_delta16);

// A drag is in progress — the host holds off re-sending transforms it did not cause.
__declspec(dllexport) int Ed_GizmoIsUsing();

// The pointer is over a gizmo handle (or dragging one). The host suppresses picking so a
// click on the gizmo does not also select whatever is behind it.
__declspec(dllexport) int Ed_GizmoWantsMouse();

// Highlight the given model handles (as returned by Ed_AddModelFile) with a wireframe box.
// Drawn post-tonemap without depth test, so a selection stays visible through geometry.
// Pass count 0 to clear. The host re-sends the whole set whenever selection changes; the
// engine holds no selection state of its own, since the host's scene is the authority.
__declspec(dllexport) void Ed_SetSelection(const int* ids, int count);

// Append one line to the editor's Log window (rendered over the viewport by the engine's
// own ImGui-on-Vulkan). The host mirrors its log here; only text crosses the boundary, so
// the host's and engine's ImGui versions never need to be ABI-compatible. isError != 0
// tints the line red.
__declspec(dllexport) void Ed_PushLog(const char* text, int isError);

// Update the editor Statistics overlay shown over the viewport (FPS/RFPS, verts, tris,
// draw calls, lights). The host mirrors its own render stats here each frame.
__declspec(dllexport) void Ed_SetStats(float fps, float rfps, int verts, int tris, int dips, int lights, int total_lights);

// Render exactly one frame (pumps pending window messages, then Device.on_idle()).
// The host calls this in its own loop instead of the engine's internal message loop.
__declspec(dllexport) void Ed_RenderFrame();

// Tear down the engine (reverse of Ed_Init).
__declspec(dllexport) void Ed_Shutdown();

#ifdef __cplusplus
}
#endif

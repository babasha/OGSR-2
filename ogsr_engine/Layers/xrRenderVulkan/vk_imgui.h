// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).
//
// ============================================================================
//  Minimal Dear ImGui Vulkan backend (vk_imgui)
// ============================================================================
//  ImGui core is already vendored + built (3rd_party/Src/imgui, imgui.lib); the
//  repo only ships the DX11 backend (xrRenderDX10/imgui_impl_dx11). This is a
//  small, self-contained Vulkan backend in our own style (dynamic rendering,
//  VMA, the .spv loader) — no upstream imgui_impl_vulkan dependency.
//
//  v1 = DISPLAY-ONLY overlay (no mouse/keyboard routing yet): it renders the
//  global profiler window (vk_profiler getters) on top of the final frame,
//  gated by the `r_profiler 2` console var. Input wiring + the r_* knob panel
//  are a later step.
//
//  Lifecycle is lazy: the first DrawOverlay() builds the context, font atlas and
//  pipeline. Call DrawOverlay() once per frame from CRender::End AFTER the UI
//  pass (the swapchain image is COLOR_ATTACHMENT there); Shutdown() on teardown.
// ============================================================================
#pragma once
#include "stdafx.h"

struct ImDrawData;

namespace VK { namespace ImGuiVK {

// Build + record the profiler overlay into the current swapchain image. Safe to
// call every frame; no-ops if r_profiler < 2 or init fails. `dt` = real frame
// seconds (Device.fTimeDeltaReal).
void DrawOverlay(VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D extent, float dt);

// A2.4 (editor): append one line to the editor Log window (shown over the viewport in
// -vk_editor mode). `isError` tints it red. The host (SDK) mirrors its own log here via
// the Ed_PushLog facade — only text crosses the module boundary, so the SDK's ImGui
// version (1.92) and the engine's (1.89) never have to agree on ABI.
void PushEditorLog(const char* text, bool isError);

// Update the editor Statistics overlay (mirrored from the SDK's IM_Stats each frame).
void PushEditorStats(float fps, float rfps, int verts, int tris, int dips, int lights, int totalLights);

// True while the editor overlay's ImGui has the mouse captured (pointer over the Log
// window or a gizmo handle). The orbit camera reads this to yield the mouse to the UI.
bool EditorWantsMouse();

// ---- transform gizmo, driven by the host ----------------------------------------
// We are only the widget: the host picks the operation, the space and the snap, owns the
// objects and the undo stack, and applies the result. It runs here because a gizmo has to
// be drawn with the matrices that drew the frame — the host's own ImGuizmo lives on its
// DX9 surface, which is underneath our Vulkan child and therefore invisible.
//
// SetGizmo arms one frame; the host re-arms every frame it wants a gizmo, and simply
// stops calling when nothing is selected. GizmoResult reports the LAST serviced frame,
// so the host reads one frame behind — invisible during a drag.
//
// op: 0 translate, 1 rotate, 2 scale.  mode: 0 local, 1 world.  snap: null for none.
void SetGizmo(int op, int mode, const float* snap, const Fmatrix& xform);
int GizmoResult(Fmatrix& out_xform, Fmatrix& out_delta); // 1 = the manipulation changed it
bool GizmoIsUsing();     // a drag is in progress
bool GizmoWantsMouse();  // hovering a handle or dragging — suppress picking behind it

// ---- the embedded GAME editor (xrGame embedded_editor, Shift+F10) ----------------
// The engine's ImGui_Backend_NewFrame / ImGui_Backend_RenderDrawData hooks (vk_engine_stubs.cpp) land here.
// GameNewFrame : main thread, before the frame's logic -- makes the game context current and starts its frame.
// GameSubmit   : render thread, End_Render, after ImGui::Render -- remembers the draw data.
// DrawGame     : CRender::End, next to DrawOverlay -- records it over the finished frame.
void GameNewFrame();
void GameSubmit(ImDrawData* dd);
void DrawGame(VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D extent);

void Shutdown();

}} // namespace VK::ImGuiVK

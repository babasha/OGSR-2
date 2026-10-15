// xrRenderVulkan - compatibility stub for shared X-Ray Engine code.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha) for the
// stub itself; the interface it mirrors is GSC Game World code (root LICENSE.md).
// Non-commercial use only; keep this notice on redistribution.

// xrRenderVulkan - Engine-side symbols that the R4 renderer normally provides
// but the Vulkan launcher must also satisfy because xrEngine and xrGame
// reference them unconditionally.
//
// All globals get safe defaults (mostly zero). All functions are no-ops. When
// the Vulkan code grows real implementations of these subsystems, the
// matching definitions move to their proper home and these stubs disappear.

#include "stdafx.h"

// ---------------------------------------------------------------------------
// luabind global allocator hook.
//
// In R4 this lives in Layers/xrRender/ResourceManager_Scripting.cpp:509 and
// xrEngine reaches it through the dxRenderDeviceRender path. We don't link
// xrRender here, so the allocator stays nullptr and the very first luabind::open
// (called from CScriptEngine::init via ai() lazy-init) crashes inside
// luabind::call_allocator with a null function-pointer execute.
//
// We don't include <luabind/luabind_memory.h> here because that header pulls
// LUABIND_API symbols only available after <luabind/luabind.h>'s preamble.
// Forward-declare the two extern globals with the matching signatures instead.
// ---------------------------------------------------------------------------
namespace luabind {
    typedef void* memory_allocation_function_parameter;
    typedef void* (__cdecl* memory_allocation_function_pointer)
        (memory_allocation_function_parameter, void const*, size_t);
    extern memory_allocation_function_pointer    allocator;
    extern memory_allocation_function_parameter  allocator_parameter;
}

static void* __cdecl vk_luabind_allocator(luabind::memory_allocation_function_parameter,
                                          const void* pointer, size_t const size)
{
    if (!size) {
        void* p = const_cast<void*>(pointer);
        xr_free(p);
        return nullptr;
    }
    if (!pointer)
        return Memory.mem_alloc(size);
    return Memory.mem_realloc(const_cast<void*>(pointer), size);
}

// Static-init binding so the allocator is wired before any code path that
// could reach luabind::open (object_factory().init() → ai() → script_engine).
namespace {
struct LuabindAllocatorInstaller
{
    LuabindAllocatorInstaller()
    {
        luabind::allocator           = &vk_luabind_allocator;
        luabind::allocator_parameter = nullptr;
    }
};
LuabindAllocatorInstaller g_LuabindAllocatorInstaller;
} // namespace


// SSFX shader-tunable globals + ps_lens_flare_sun_blend now defined in
// vk_console_min.cpp as part of the full xrRender_console port. Their
// previous stub definitions here would conflict at link time.

// ---------------------------------------------------------------------------
// xrGame ↔ renderer hand-off flag (R4 toggles in r4_R_render.cpp).
// ---------------------------------------------------------------------------
bool RESET_SECTORS_HACK{};

// ---------------------------------------------------------------------------
// xrGame's Level_network.cpp calls this to flush HW occlusion queries on
// object removal. R4 wires it to RImplementation.HWOCC; we have no occlusion
// path yet, so the call is a no-op.
// ---------------------------------------------------------------------------
void Cleanup_R_occlusion() {}

// ---------------------------------------------------------------------------
// Renderer debug-layer message dump. Called unconditionally from
// xrCore/xrDebugNew.cpp on crash and from device.cpp::on_idle. Vulkan has its
// own validation pipe (debug_utils messenger), so this stays empty here.
// ---------------------------------------------------------------------------
void LogRenderDebugMessages() {}

// ---------------------------------------------------------------------------
// ImGui backend hooks for the embedded game editor (Shift+F10), called from xr_3da/device.cpp while it is open.
// They were empty here, so the editor had no ImGui context on Vulkan and crashed on its first window. The
// backend is vk_imgui's (VK::ImGuiVK::Game*), see vk_imgui.h.
// ---------------------------------------------------------------------------
#include "vk_imgui.h"
void ImGui_Backend_NewFrame() { VK::ImGuiVK::GameNewFrame(); }
void ImGui_Backend_RenderDrawData(ImDrawData* draw_data) { VK::ImGuiVK::GameSubmit(draw_data); }

// ---------------------------------------------------------------------------
// Material / lighting subsystems — stubs until vk_material.cpp / vk_lighting.cpp
// graduate from _parked/. Active code that links against vk_material.h and
// vk_lighting.h (vk_Visual.cpp's LoadTexture path, vk_shader.cpp, vk_pipeline.cpp)
// references these symbols; without minimal definitions linking fails.
// CreateMaterial returns nullptr → vkRender_Visual falls back to the
// shader-id resolution path that uses RImplementation.Shaders.
// ---------------------------------------------------------------------------
#include "vk_material.h"
#include "vk_lighting.h"

VK::CMaterialManager* g_MaterialManager = nullptr;

namespace VK
{
    CMaterial* CMaterialManager::CreateMaterial(LPCSTR /*name*/) { return nullptr; }
    CVulkanLighting* g_VulkanLighting = nullptr;
}

// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_RenderDeviceRender.h"
#include "HW_Vulkan.h"
#include "vk_swapchain.h"
#include "vk_sync.h"
#include "vk_command_buffer.h"
#include "vk_UIPipeline.h"
#include "vk_pipeline_cache.h"
#include "vk_world_material.h"
#include "vk_terrain_cache.h"    // TerrainCache/TerrainMask teardown
#include "vk_texture_stream.h"  // VK::TextureStreamer — DeferredLoad bracketing + VRAM report
#include "vk_env_light.h"      // VK::EnvLight — shared per-frame sun/hemi/ambient UBO (set 1/2)
#include "vk_shadow.h"         // VK::ShadowMap — sun shadow map (created with EnvLight)
#include "vk_pass_sky.h"
#include "vk_scene_color.h"    // VK::SceneColor — HDR scene target
#include "vk_pass_tonemap.h"   // VK::TonemapPass — HDR → swapchain composite
#include "vk_hzb_readback.h"   // VK::HzbReadback::Destroy
#include "vk_pass_bloom.h"     // VK::BloomPass — bright-pass + blur for the composite
#include "vk_pass_ssao.h"      // VK::SSAOPass — GTAO + folded-in SSIL (one-bounce SSGI in the horizon march)
#include "vk_pass_rtgi.h"      // VK::RTGIPass — RT preview / future DDGI probe update
#include "vk_ddgi.h"           // VK::DDGI — probe volume
#include "vk_gi_probe.h"       // VK::GIProbe — the GI pixel debugger (r_gi_probe)
#include "vk_gi_froxel.h"      // VK::GiFroxel — the probe field resampled into the view frustum
#include "vk_pass_gather.h"    // VK::GatherPass — the final gather (step 4)
#include "vk_wrc.h"            // VK::WRC — the world radiance cache (step 5)
#include "vk_motionvec.h"      // VK::MotionVec — screen-space motion vectors (DLSS/FSR/PT foundation)
#include "vk_dlss.h"           // VK::Dlss — NVIDIA DLSS 4.5 (NGX) init + capability probe
#include "vk_fsr.h"
#include "vk_rr_guides.h"    // VK::RRGuide — Ray Reconstruction guide G-buffer teardown
#include "vk_sl.h"             // VK::SL — Streamline (DLSS/Reflex/FG) shutdown
#include "vk_pass_registry.h"  // VK::PassTimingDestroy() — GPU timing query pool teardown
#include "vk_framegraph.h"     // VK::g_FrameGraph.Reset() — resource-state registry teardown
#include "vk_imgui.h"          // VK::ImGuiVK::Shutdown() — profiler overlay teardown
#include "vk_vrs.h"            // VK::VRS::Destroy() — shading-rate image teardown
#include "vk_vsm.h"            // VK::VSM::Destroy() — virtual shadow maps teardown
#include "vk_instance_gpu.h"   // VK::InstanceGPU::Destroy() — instanced rigid casters teardown
#include "vk_clustered.h"      // VK::Clustered::Destroy() — clustered forward teardown
#include "vk_async.h"          // VK::Async::Destroy() — async compute teardown
#include "vk_volumetrics.h"    // VK::Vol::Init/Destroy() — froxel volumetrics
#include "vk_pass_lightcones.h" // VK::LightCones_Destroy()
#include "vk_pass_capsule_shadow.h" // VK::CapsuleShadows_Destroy()
#include "vk_nx_bench.h"        // VK::NxBench::Run() (-nx_bench)
#include "vk_pass_water.h"      // VK::Water_Destroy()
#include "vk_water_ripple.h"    // VK::WaterRipple::Destroy()
#include "vk_pass_shadow.h"    // VK::SunShadow_Destroy() — grass-spot caster pipeline
#include "vk_pass_skinned.h"   // VK::Skinned_Destroy() — frees the bone SSBO at teardown
#include "vk_pass_particles.h" // VK::ParticlePass_Init/Destroy — billboard particle pass
#include "vk_gpu_particles.h"  // VK::GPUParticles — GPU-driven particles (Phase 0)
#include "vk_wallmarks.h"      // VK::Wallmarks::Destroy — static decals teardown
#include "vk_rain.h"           // VK::RainPass_Destroy — weather effects teardown
#include "vk_water_sim.h"      // VK::WaterSim::Destroy — water flow sim teardown
#include "vk_deform.h"         // VK::Deform::Destroy — snow deform texture teardown
#include "vk_pass_snow.h"      // VK::SnowMesh_Init/Destroy — dense snow surface mesh
#include "vk_shader.h"   // g_VulkanShaderManager (level-shader table)
#include "vk_shaders.h"  // g_ShaderManager (SPIRV module loader/cache)
#include "vk_ModelPool.h"
#include "CRender_Vulkan.h"
#include "vk_stub.h"
#if !defined(_WIN32) && !defined(__SWITCH__)
#  include <SDL2/SDL.h>
#  include <SDL2/SDL_vulkan.h>
#endif

bool g_bDeviceLost = false;

vkRenderDeviceRender::vkRenderDeviceRender()  = default;
vkRenderDeviceRender::~vkRenderDeviceRender() = default;

void vkRenderDeviceRender::Copy(IRenderDeviceRender&)            { VK_STUB_ONCE("DevRender"); }
void vkRenderDeviceRender::OnDeviceDestroy(BOOL)                 { VK_STUB_ONCE("DevRender"); }
void vkRenderDeviceRender::SetupStates()                         { VK_STUB_ONCE("DevRender"); }
void vkRenderDeviceRender::OnDeviceCreate()                      { VK_STUB_ONCE("DevRender"); }

// ----- Hardware lifecycle (the active wiring) -------------------------------

// ⭐ONE place decides the window geometry, because Create and Reset have to agree.
//
// Until 18-08 this logic lived inside Create only, and Reset recreated the
// swapchain at whatever size the window already had. That is precisely why the
// `vid_mode` row in the options screen was a placebo: it wrote psCurrentVidMode,
// the menu then issued `vid_restart`, and Reset dutifully rebuilt the swapchain
// at the OLD size. Worse, Create overwrote psCurrentVidMode with the monitor
// resolution, so the row did not even keep the number the player picked.
//
// The three exotic hosts (embedded editor viewport, -vk_spike window, an engine
// that passed 0x0) keep their old behaviour verbatim; the change is that the
// ordinary game now has two honest modes:
//   rs_fullscreen on  → borderless over the monitor, vid_mode := the monitor
//   rs_fullscreen off → a bordered window whose CLIENT area is vid_mode
//
// Returns the client size the swapchain should be built at.
#if !defined(_WIN32) && !defined(__SWITCH__)
// Off Windows the engine's HWND is the SDL_Window*. Same contract as the Win32
// body below: borderless fullscreen over the window's display, or a fixed-size
// window of the chosen CLIENT size clamped to the usable area, then report the
// size the surface will actually have.
static void vk_apply_window_mode_sdl(SDL_Window* wnd, u32& dwWidth, u32& dwHeight, const char* whence,
                                     bool wantFullscreen)
{
    extern u32 psCurrentVidMode[2];

    if (!wnd) {
        if (dwWidth == 0 || dwHeight == 0) {
            dwWidth  = psCurrentVidMode[0];
            dwHeight = psCurrentVidMode[1];
        }
        return;
    }

    const int disp = SDL_GetWindowDisplayIndex(wnd);
    SDL_Rect work{};
    const bool haveMon = disp >= 0 && SDL_GetDisplayUsableBounds(disp, &work) == 0;

    if (wantFullscreen) {
        SDL_SetWindowFullscreen(wnd, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(wnd, 0);
        SDL_SetWindowBordered(wnd, SDL_TRUE);
        SDL_SetWindowResizable(wnd, SDL_FALSE); // fixed size: the swapchain follows the client rect

        int w = psCurrentVidMode[0] ? int(psCurrentVidMode[0]) : 1024;
        int h = psCurrentVidMode[1] ? int(psCurrentVidMode[1]) : 768;
        if (haveMon) {
            int top = 0, left = 0, bottom = 0, right = 0; // frame overhead, if the WM tells
            SDL_GetWindowBordersSize(wnd, &top, &left, &bottom, &right);
            if (w + left + right > work.w || h + top + bottom > work.h) {
                w = _max(320, work.w - left - right);
                h = _max(240, work.h - top - bottom);
                Msg("[VK] DevRender::%s — vid_mode does not fit the work area, clamped to %dx%d", whence, w, h);
            }
        }
        SDL_SetWindowSize(wnd, w, h);
        if (disp >= 0)
            SDL_SetWindowPosition(wnd, SDL_WINDOWPOS_CENTERED_DISPLAY(disp), SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
    }
    SDL_ShowWindow(wnd);

    // Trust the drawable size, not the request (HiDPI, tiling WMs).
    int cw = 0, ch = 0;
    SDL_Vulkan_GetDrawableSize(wnd, &cw, &ch);
    dwWidth  = u32(cw);
    dwHeight = u32(ch);
    psCurrentVidMode[0] = dwWidth;
    psCurrentVidMode[1] = dwHeight;
    Msg("[VK] DevRender::%s — %s %ux%u", whence, wantFullscreen ? "borderless fullscreen" : "windowed", dwWidth, dwHeight);
}
#endif

static void vk_apply_window_mode(HWND hWnd, u32& dwWidth, u32& dwHeight, const char* whence)
{
    extern u32 psCurrentVidMode[2];

    // A normal bordered window, forced from the command line. Kept as an
    // override of the flag (a launch param outranks a saved config), and it is
    // the same param R4 honours.
    const bool bDrawBorders = Core.Params && strstr(Core.Params, "-draw_borders");

#if defined(__SWITCH__)
    // Horizon: the layer's size is fixed by the console (handheld 720p / docked 1080p) and the swapchain
    // follows the surface, so there is no window to restyle. Keep the requested mode as the current one.
    (void)hWnd; (void)bDrawBorders;
    psCurrentVidMode[0] = dwWidth;
    psCurrentVidMode[1] = dwHeight;
    Msg("[VK] DevRender::%s — Horizon layer %ux%u", whence, dwWidth, dwHeight);
#elif !defined(_WIN32)
    // The spike window and the embedded editor viewport are Windows-only.
    vk_apply_window_mode_sdl(static_cast<SDL_Window*>(hWnd), dwWidth, dwHeight, whence,
                             psDeviceFlags.test(rsFullscreen) && !bDrawBorders);
#else

    // SPIKE 1 (editor-on-Vulkan): render into a normal, resizable, bordered
    // window instead of taking over the whole monitor — the embeddable-viewport
    // probe. The swapchain sizes itself to the client rect (ChooseSwapExtent uses
    // the surface currentExtent), so no fullscreen restyle is needed. Additive /
    // experimental; see EDITOR_ON_VULKAN_ROADMAP.md in the SDK repo.
    const bool bSpikeWindow = Core.Params && (strstr(Core.Params, "-vk_spike") || strstr(Core.Params, "-vk_editor"));

    // A2.3 — embedded editor viewport: the HOST owns this window's size and position
    // (it is a child of the SDK's panel), so take the client rect as-is and restyle
    // nothing. Checked BEFORE the spike/fullscreen branches, which both move the window.
    extern bool g_ed_embedded;

    if (hWnd && g_ed_embedded) {
        RECT cr{};
        GetClientRect(hWnd, &cr);
        dwWidth  = u32(cr.right - cr.left);
        dwHeight = u32(cr.bottom - cr.top);
        psCurrentVidMode[0] = dwWidth;
        psCurrentVidMode[1] = dwHeight;
        Msg("[VK] DevRender::%s — EMBEDDED host window %ux%u", whence, dwWidth, dwHeight);
        return;
    }

    if (hWnd && bSpikeWindow) {
        RECT rc{ 0, 0, 1280, 720 };
        SetWindowLongPtr(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW); // без WS_VISIBLE — см. коммент в fullscreen-ветке ниже
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hWnd, HWND_TOP, 60, 60, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        RECT cr{};
        GetClientRect(hWnd, &cr);
        dwWidth  = u32(cr.right - cr.left);
        dwHeight = u32(cr.bottom - cr.top);
        psCurrentVidMode[0] = dwWidth;
        psCurrentVidMode[1] = dwHeight;
        Msg("[VK] DevRender::%s — SPIKE windowed %ux%u", whence, dwWidth, dwHeight);
        return;
    }

    if (!hWnd) {
        if (dwWidth == 0 || dwHeight == 0) {
            dwWidth  = psCurrentVidMode[0];
            dwHeight = psCurrentVidMode[1];
        }
        return;
    }

    // Which monitor this window lives on — everything below is relative to it,
    // so a second screen of a different size is handled without special cases.
    HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    const bool haveMon = GetMonitorInfo(mon, &mi) != 0;

    const bool wantFullscreen = psDeviceFlags.test(rsFullscreen) && !bDrawBorders;

    if (wantFullscreen && haveMon) {
        const LONG mw = mi.rcMonitor.right  - mi.rcMonitor.left;
        const LONG mh = mi.rcMonitor.bottom - mi.rcMonitor.top;

        dwWidth  = (u32)mw;
        dwHeight = (u32)mh;
        // Keep the engine's notion of the screen size in sync, exactly like
        // R4's CHW ctor forces psCurrentVidMode to the desktop resolution —
        // UI layout & aspect ratio read from here, and so does the options row,
        // which must show the resolution actually in effect.
        psCurrentVidMode[0] = dwWidth;
        psCurrentVidMode[1] = dwHeight;

        // ⚠️WS_VISIBLE НЕ ставить через SetWindowLongPtr: бит видимости
        // выставляется в обход оконного менеджера, перехода «скрыто →
        // показано» не происходит, шелл не получает HSHELL_WINDOWCREATED —
        // и кнопки в панели задач у игры нет (а Alt+Tab её показывает, он
        // перечисляет окна на лету). Идущий следом SWP_SHOWWINDOW уже
        // бесполезен: показывать нечего, бит стоит. Рестайлим стиль, а
        // показ отдаём SWP_SHOWWINDOW — он и уведомит шелл.
        // WS_SYSMENU | WS_MINIMIZEBOX рамку у WS_POPUP не рисуют (для этого
        // нужен WS_CAPTION) — они лишь возвращают окну системное меню и
        // нормальное сворачивание с кнопки в панели задач.
        SetWindowLongPtr(hWnd, GWL_STYLE, WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX);
        SetWindowPos(hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mw, mh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        Msg("[VK] DevRender::%s — borderless fullscreen %ux%u", whence, dwWidth, dwHeight);
        return;
    }

    // ---- windowed -----------------------------------------------------------
    // The number in the options screen is the CLIENT size; AdjustWindowRect adds
    // the frame. Clamped to the monitor's work area, otherwise picking a mode
    // larger than the screen hides the title bar and the window can no longer be
    // moved — a settings screen must not be able to lock the player out.
    u32 w = psCurrentVidMode[0] ? psCurrentVidMode[0] : 1024;
    u32 h = psCurrentVidMode[1] ? psCurrentVidMode[1] : 768;

    const DWORD style   = WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME); // fixed size: the swapchain follows the client rect
    const DWORD exStyle = (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE);

    RECT rc{ 0, 0, (LONG)w, (LONG)h };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    LONG fw = rc.right - rc.left, fh = rc.bottom - rc.top;

    LONG x = 60, y = 60;
    if (haveMon) {
        const LONG aw = mi.rcWork.right - mi.rcWork.left;
        const LONG ah = mi.rcWork.bottom - mi.rcWork.top;
        if (fw > aw || fh > ah) {
            // Shrink the CLIENT until the framed window fits the work area.
            const LONG dx = fw - (LONG)w, dy = fh - (LONG)h; // frame overhead
            w = (u32)_max(320L, aw - dx);
            h = (u32)_max(240L, ah - dy);
            rc = RECT{ 0, 0, (LONG)w, (LONG)h };
            AdjustWindowRectEx(&rc, style, FALSE, exStyle);
            fw = rc.right - rc.left; fh = rc.bottom - rc.top;
            Msg("[VK] DevRender::%s — vid_mode does not fit the work area, clamped to %ux%u", whence, w, h);
        }
        x = mi.rcWork.left + (aw - fw) / 2;
        y = mi.rcWork.top  + (ah - fh) / 2;
    }

    SetWindowLongPtr(hWnd, GWL_STYLE, style);
    SetWindowPos(hWnd, HWND_TOP, x, y, fw, fh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    // Trust the client rect, not the arithmetic: DPI scaling and the shell can
    // both disagree with AdjustWindowRect, and the swapchain will take the
    // surface's currentExtent regardless of what we believe here.
    RECT cr{};
    GetClientRect(hWnd, &cr);
    dwWidth  = u32(cr.right - cr.left);
    dwHeight = u32(cr.bottom - cr.top);
    psCurrentVidMode[0] = dwWidth;
    psCurrentVidMode[1] = dwHeight;
    Msg("[VK] DevRender::%s — windowed %ux%u", whence, dwWidth, dwHeight);
#endif
}

#ifdef __SWITCH__
extern "C" void xr_nx_display_size(unsigned* w, unsigned* h);
#endif

void vkRenderDeviceRender::Create(HWND hWnd, u32& dwWidth, u32& dwHeight,
                                  float& fWidth_2, float& fHeight_2)
{
    Msg("[VK] DevRender::Create — hWnd=%p, requested %ux%u", hWnd, dwWidth, dwHeight);
    m_hWnd = hWnd;

    // Engine doesn't set dwWidth/dwHeight before calling us — R4 fills them
    // from its DXGI swapchain. Mirror that: take the configured video-mode
    // (i.e. the resolution the user sees) regardless of the tiny default window.
    extern u32 psCurrentVidMode[2];
#ifdef __SWITCH__
    // No window and no saved video mode: size the swapchain to the console's layer (handheld 720p / docked 1080p).
    // Without this everything is 0x0 and the swapchain is skipped as "minimized" -> a black screen.
    if (dwWidth == 0 || dwHeight == 0) {
        unsigned w = 0, h = 0;
        xr_nx_display_size(&w, &h);
        dwWidth = w;
        dwHeight = h;
        psCurrentVidMode[0] = w;
        psCurrentVidMode[1] = h;
        Msg("[VK] DevRender::Create -- Horizon layer %ux%u", w, h);
    }
#endif
    vk_apply_window_mode(hWnd, dwWidth, dwHeight, "Create");

    if (dwWidth == 0 || dwHeight == 0) {
        dwWidth  = psCurrentVidMode[0];
        dwHeight = psCurrentVidMode[1];
        Msg("[VK] DevRender::Create — engine passed 0×0, using cfg %ux%u", dwWidth, dwHeight);
    }

    if (!VulkanHW.CreateDevice(hWnd)) {
        FATAL("VK: VulkanHW.CreateDevice failed — see log for instance/device errors");
    }

    Sync.Create();
    CommandManager.Create();
    Swapchain.Create(dwWidth, dwHeight);

    // Engine reads back the actual framebuffer dimensions in case driver clamped them.
    dwWidth   = Swapchain.GetWidth();
    dwHeight  = Swapchain.GetHeight();
    fWidth_2  = float(dwWidth)  * 0.5f;
    fHeight_2 = float(dwHeight) * 0.5f;

    // UI pipeline + vertex buffer + white-texture descriptor.
    VulkanUI::Create();

    // World forward pipeline cache (needs swapchain format) + model pool.
    // OGSR engine doesn't call CRender::create() through this build path, so
    // we own the CRender-side subsystems here, where Create is actually
    // invoked. WorldMaterialCache must come first — PipelineCache builds
    // pipelines whose layout includes the material descriptor set layout.
    VK::WorldMaterialCache::Init();
    VK::EnvLight::Init();          // before PipelineCache — its set layout joins the world pipeline layout (set 1)
    VK::PipelineCache::Init();
    VK::SkyPass::Init();
    VK::SnowMesh_Init();           // dense snow surface mesh (after EnvLight — shares its set layout)
    VK::TonemapPass::Init();       // HDR → swapchain composite (after SkyPass — shares the SPIRV loader)
    VK::BloomPass::Init();         // bright-pass + blur feeding the tonemap composite
    VK::SSAOPass::Init();          // GTAO from the depth prepass (EnvLight binding 8) + folded-in SSIL (tonemap binding 5)
    VK::MotionVec::Init();         // screen-space motion vectors from the prepass depth (DLSS/FSR/PT foundation; needs Swapchain.m_Format)
    VK::Dlss::Init();              // NVIDIA DLSS (NGX) init + capability probe — logs SuperRes/FrameGen availability
    VK::Fsr::Init();               // AMD FidelityFX Super Resolution 3.1.4 - the upscaler for everything NGX cannot serve
    VK::GPUParticles::Init();      // GPU-driven particles Phase 1 — pool + free-list + compute emit/sim/draw
    VK::Vol::Init();               // froxel volumetrics (3D volume eager so the tonemap binding 4 is valid)
    VK::ParticlePass_Init();

    // Load the particle-definition library (particles.xr / .pe-.pg) so
    // model_CreateParticles can resolve effect/group names. Mirrors R4's
    // CRender::create() → PSLibrary.OnCreate() (which this build path skips).
    { extern void vkParticles_OnCreate(); vkParticles_OnCreate(); }

    // Shader manager owns the level-shader table that maps shader_id →
    // (shader name, diffuse texture name). Without it the loader leaves all
    // Shaders[i] nullptr and every visual ends up with the default white
    // material. Cheap to spin up — no GPU work, just the hashmap + a default
    // entry.
    if (!g_VulkanShaderManager) {
        g_VulkanShaderManager = xr_new<VK::CVulkanShaderManager>();
        g_VulkanShaderManager->Create();
    }
    if (!RImplementation.Models)
        RImplementation.Models = xr_new<vkModelPool>();

    m_bInitialized = true;
    Msg("[VK] DevRender::Create OK — swapchain %ux%u, %u images",
        Swapchain.GetWidth(), Swapchain.GetHeight(), Swapchain.m_ImageCount);
    if (Core.Params && strstr(Core.Params, "-nx_bench"))
        VK::NxBench::Run();   // synthetic device benchmark, once, before any level or menu work
}

void vkRenderDeviceRender::Destroy()
{
    Msg("[VK] DevRender::Destroy");
    if (!m_bInitialized) return;

    // Teardown is logged step-by-step: log.cpp flushes every line, so if a future
    // change ever hangs/crashes here the last line pinpoints the failing step.
    // Low-noise (one block per exit); kept as a permanent guard.
    if (VulkanHW.m_Device != VK_NULL_HANDLE) {
        Msg("[VK] DevRender::Destroy: vkDeviceWaitIdle ...");
        VkResult wi = vkDeviceWaitIdle(VulkanHW.m_Device);
        Msg("[VK] DevRender::Destroy: vkDeviceWaitIdle done (res=%d)", wi);
    }

    if (RImplementation.Models) { xr_delete(RImplementation.Models); RImplementation.Models = nullptr; }
    Msg("[VK] DevRender::Destroy: Models freed");
    if (g_VulkanShaderManager) {
        g_VulkanShaderManager->Destroy();
        xr_delete(g_VulkanShaderManager);
        g_VulkanShaderManager = nullptr;
    }
    Msg("[VK] DevRender::Destroy: ShaderManager freed");
    { extern void vkParticles_OnDestroy(); vkParticles_OnDestroy(); }
    VK::Wallmarks::Destroy();           Msg("[VK] DevRender::Destroy: Wallmarks done");
    VK::RainPass_Destroy();             Msg("[VK] DevRender::Destroy: RainPass done");
    VK::WaterSim::Destroy();            Msg("[VK] DevRender::Destroy: WaterSim done");
    // ⚠Was missing (the include above already claimed it): the ripple tile's images and
    // its splat buffer outlived the device, so shutdown printed five VMA leak-asserts and
    // then FAULTED (c0000005) in the atexit destructor of the file-scope s_splatBuf —
    // by then both the allocator and the device are gone. A crash on exit is easy to
    // shrug off; it is also how every session ended with a minidump.
    VK::WaterRipple::Destroy();         Msg("[VK] DevRender::Destroy: WaterRipple done");
    VK::Deform::Destroy();              Msg("[VK] DevRender::Destroy: Deform done");
    VK::SnowMesh_Destroy();            Msg("[VK] DevRender::Destroy: SnowMesh done");
    VK::ParticlePass_Destroy();         Msg("[VK] DevRender::Destroy: ParticlePass done");
    VK::GPUParticles::Destroy();        Msg("[VK] DevRender::Destroy: GPUParticles done");
    VK::SkyPass::Destroy();             Msg("[VK] DevRender::Destroy: SkyPass done");
    // Before SSAOPass: that one owns RTScene::Destroy, which frees the buffers the
    // RTGI descriptor sets point at.
    VK::RTGIPass::Destroy();            Msg("[VK] DevRender::Destroy: RTGIPass done");
    VK::GatherPass::Destroy();          Msg("[VK] DevRender::Destroy: GatherPass done");
    // Before DDGI: the cache's shade set points at DDGI's atlases and its light SSBO.
    VK::WRC::Destroy();                 Msg("[VK] DevRender::Destroy: WRC done");
    VK::DDGI::Destroy();                Msg("[VK] DevRender::Destroy: DDGI done");
    VK::SSAOPass::Destroy();            Msg("[VK] DevRender::Destroy: SSAOPass done");
    VK::MotionVec::Destroy();           Msg("[VK] DevRender::Destroy: MotionVec done");
    VK::RRGuide::Shutdown();            Msg("[VK] DevRender::Destroy: RR guides done");
    VK::Dlss::Shutdown();               Msg("[VK] DevRender::Destroy: DLSS done");
    VK::Fsr::Shutdown();                Msg("[VK] DevRender::Destroy: FSR done");
    VK::SL::Shutdown();                 Msg("[VK] DevRender::Destroy: Streamline done");
    VK::BloomPass::Destroy();           Msg("[VK] DevRender::Destroy: BloomPass done");
    VK::GIProbe::Destroy();             Msg("[VK] DevRender::Destroy: GIProbe done");
    VK::TonemapPass::Destroy();         Msg("[VK] DevRender::Destroy: TonemapPass done");
    VK::HzbReadback::Destroy();         Msg("[VK] DevRender::Destroy: HzbReadback done");
    VK::SceneColor::Destroy();          Msg("[VK] DevRender::Destroy: SceneColor done");
    VK::LightCones_Destroy();           Msg("[VK] DevRender::Destroy: LightCones done");
    VK::CapsuleShadows_Destroy();
    VK::Water_Destroy();                Msg("[VK] DevRender::Destroy: Water done");
    VK::Skinned_Destroy();              Msg("[VK] DevRender::Destroy: SkinnedPass done");
    VK::EnvLight::Destroy();            Msg("[VK] DevRender::Destroy: EnvLight done");
    VK::SunShadow_Destroy();            Msg("[VK] DevRender::Destroy: SunShadow done");
    VK::ShadowMap::Destroy();          Msg("[VK] DevRender::Destroy: ShadowMap done");
    VK::VRS::Destroy();                 Msg("[VK] DevRender::Destroy: VRS done");
    VK::VSM::Destroy();                 Msg("[VK] DevRender::Destroy: VSM done");
    VK::InstanceGPU::Destroy();         Msg("[VK] DevRender::Destroy: InstanceGPU done");
    VK::Clustered::Destroy();           Msg("[VK] DevRender::Destroy: Clustered done");
    VK::Async::Destroy();               Msg("[VK] DevRender::Destroy: Async done");
    // Before Vol: the fog inject set points at the GI field's two volumes.
    VK::GiFroxel::Destroy();            Msg("[VK] DevRender::Destroy: GiFroxel done");
    VK::Vol::Destroy();                 Msg("[VK] DevRender::Destroy: Vol done");
    VK::ImGuiVK::Shutdown();            Msg("[VK] DevRender::Destroy: ImGui overlay done");
    VK::PassTimingDestroy();            Msg("[VK] DevRender::Destroy: PassTiming done");
    // Resource-state registry: drop every tracked handle. The subsystems above
    // have just destroyed the images/buffers those entries describe, and the
    // driver reuses handle values — a surviving entry would hand a NEW resource
    // the dead one's layout on the next level load.
    VK::g_FrameGraph.Reset();           Msg("[VK] DevRender::Destroy: FrameGraph done");
    VK::PipelineCache::Destroy();       Msg("[VK] DevRender::Destroy: PipelineCache done");
    VK::TerrainCache::Destroy();        Msg("[VK] DevRender::Destroy: TerrainCache done");
    VK::TerrainMask::Destroy();         Msg("[VK] DevRender::Destroy: TerrainMask done");
    VK::WorldMaterialCache::Destroy();  Msg("[VK] DevRender::Destroy: WorldMaterial done");
    VK::TextureStreamer::Instance().Shutdown();  Msg("[VK] DevRender::Destroy: TexStreamer done");
    VulkanUI::Destroy();                Msg("[VK] DevRender::Destroy: VulkanUI done");

    // Free the SPIRV module cache LAST among shader consumers: SkyPass / Skinned /
    // PipelineCache / VulkanUI / DetailManager only null their own handles (the
    // modules are owned here), so their VkShaderModules outlive them until now.
    // Must run before VulkanHW.DestroyDevice() — otherwise vkDestroyDevice trips
    // VUID-vkDestroyDevice-device-05137 (undestroyed child shader modules).
    if (g_ShaderManager) {
        g_ShaderManager->DestroyAll();
        xr_delete(g_ShaderManager);
        g_ShaderManager = nullptr;
    }
    Msg("[VK] DevRender::Destroy: SPIRV shader modules freed");

    Swapchain.Destroy();                Msg("[VK] DevRender::Destroy: Swapchain done");
    CommandManager.Destroy();           Msg("[VK] DevRender::Destroy: CommandManager done");
    Sync.Destroy();                     Msg("[VK] DevRender::Destroy: Sync done");
    VulkanHW.DestroyDevice();           Msg("[VK] DevRender::Destroy: device destroyed");

    m_bInitialized = false;
    m_hWnd         = nullptr;
}

void vkRenderDeviceRender::Reset(HWND hWnd, u32& dwWidth, u32& dwHeight,
                                 float& fWidth_2, float& fHeight_2)
{
    Msg("[VK] DevRender::Reset — was %ux%u", dwWidth, dwHeight);
    if (VulkanHW.m_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(VulkanHW.m_Device);
    }

    // ⭐The window FIRST, the swapchain after. On Win32 the surface reports a
    // currentExtent equal to the window's client rect and the spec says a
    // swapchain must use it, so ChooseSwapExtent ignores whatever numbers we
    // hand it — resizing the swapchain means resizing the window. This call is
    // the whole reason `vid_mode` and `rs_fullscreen` do anything at all.
    vk_apply_window_mode(hWnd ? hWnd : m_hWnd, dwWidth, dwHeight, "Reset");

    Swapchain.Recreate(dwWidth, dwHeight);
    dwWidth  = Swapchain.GetWidth();
    dwHeight = Swapchain.GetHeight();

    // ⚠These two were dropped on the floor (the parameters were commented out),
    // and they are not decoration: fWidth_2/fHeight_2 are the half-extents the
    // projection and every screen-space transform are built from. A resolution
    // change without them leaves the picture correct only by coincidence — at
    // the resolution the game started at.
    fWidth_2  = float(dwWidth)  * 0.5f;
    fHeight_2 = float(dwHeight) * 0.5f;
    Msg("[VK] DevRender::Reset — now %ux%u", dwWidth, dwHeight);
}

// ----- Resource stubs (filled in when texture/asset wiring lands) -----------

// DeferredLoad(TRUE) at level-load start, DeferredLoad(FALSE) after geometry lands.
// The DX renderers use this to batch texture uploads; the VK backend uploads eagerly
// via the transfer queue, so here it just brackets the texture streamer's per-level
// accounting (budget snapshot + demotion tally + end-of-load residency report).
void vkRenderDeviceRender::DeferredLoad(BOOL E)
{
    if (E) VK::TextureStreamer::Instance().BeginLevelLoad();
    else   VK::TextureStreamer::Instance().EndLevelLoad();
}

// Called right after DeferredLoad(FALSE) (Level_network_start_client). Textures are
// already resident on the VK path; log the final footprint so the load report is
// complete even when DeferredLoad(FALSE) wasn't paired.
void vkRenderDeviceRender::ResourcesDeferredUpload()
{
    VK::TextureStreamer::Instance().EndLevelLoad();
}

void vkRenderDeviceRender::ResourcesDumpMemoryUsage()
{
    VK::TextureStreamer::Instance().DumpStats();
}

void vkRenderDeviceRender::ResourcesPrefetchCreateTexture(LPCSTR){ VK_STUB_ONCE("DevRender"); }

// R4's HUD reads these as (texture bytes, texture count, lightmap bytes, lightmap
// count). We report the streamer's tracked texture footprint as the base figure;
// lightmaps are folded into the same registry (Lmap class), so keep the split simple.
void vkRenderDeviceRender::ResourcesGetMemoryUsage(u32& m_base, u32& c_base,
                                                   u32& m_lmaps, u32& c_lmaps)
{
    u32 bytes = 0, count = 0;
    VK::TextureStreamer::Instance().GetMemoryUsage(bytes, count);
    m_base  = bytes;
    c_base  = count;
    m_lmaps = 0;
    c_lmaps = 0;
}

IRenderDeviceRender::DeviceState vkRenderDeviceRender::GetDeviceState()
{
    return g_bDeviceLost ? dsLost : dsOK;
}

void vkRenderDeviceRender::OnAssetsChanged()                     { VK_STUB_ONCE("DevRender"); }
IResourceManager* vkRenderDeviceRender::GetResourceManager() const { return nullptr; }

#include "stdafx.h"
#include "resource.h"
#ifdef _MSC_VER
#include <winternl.h> // unused in this file (grepped) -- dead include
#elif !defined(__SWITCH__)
#include <SDL2/SDL.h> // the render window (Linux / Android)
#endif

#ifdef _MSC_VER
static void LogOsVersion()
{
    static auto RtlGetVersion = reinterpret_cast<NTSTATUS(WINAPI*)(LPOSVERSIONINFOEXW)>(GetProcAddress(GetModuleHandle("ntdll"), "RtlGetVersion"));

    if (RtlGetVersion)
    {
        OSVERSIONINFOEXW osInfo{};
        osInfo.dwOSVersionInfoSize = sizeof(osInfo);

        if (NT_SUCCESS(RtlGetVersion(&osInfo)))
        {
            Msg("--OS Version major: [%d] minor: [%d], build: [%d]. Server OS: [%s]", osInfo.dwMajorVersion, osInfo.dwMinorVersion, osInfo.dwBuildNumber,
                osInfo.wProductType != VER_NT_WORKSTATION ? "yes" : "no");
            return;
        }
    }
    Msg("!![%s] Can't get RtlGetVersion", __FUNCTION__);
}
#elif defined(__SWITCH__)
// Horizon has no uname(); libnx knows the firmware version (hosversionGet), but the engine does not
// link libnx's version API yet. Name the platform instead of pretending to have a kernel string.
static void LogOsVersion() { Msg("--OS Version: [Horizon OS (Nintendo Switch homebrew)]"); }
#else
#include <sys/utsname.h>
static void LogOsVersion()
{
    struct utsname u{};
    if (0 == uname(&u))
        Msg("--OS Version: [%s %s %s]", u.sysname, u.release, u.machine);
    else
        Msg("!![%s] uname() failed", __FUNCTION__);
}
#endif // _MSC_VER

static void LogWorkingDriveInfo()
{
    // Setup the DWORD variables.
    ULARGE_INTEGER TotalNumberOfBytes, TotalNumberOfFreeBytes;

    if (GetDiskFreeSpaceEx(Core.ApplicationPath, nullptr, &TotalNumberOfBytes, &TotalNumberOfFreeBytes))
    {
        Msg("Current drive space free: [%0.f]Mb total: [%0.f]Mb", static_cast<float>(TotalNumberOfFreeBytes.QuadPart) / 1024.f / 1024.f,
            static_cast<float>(TotalNumberOfBytes.QuadPart) / 1024.f / 1024.f);
    }
}


extern LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void CRenderDevice::Initialize()
{
    ZoneScoped;

    LogOsVersion();
    LogWorkingDriveInfo();

    Log("Initializing Engine...");
    TimerGlobal.Start();
    TimerMM.Start();

    // Unless a substitute hWnd has been specified, create a window to render into
#ifdef _MSC_VER
    if (m_hWnd == nullptr)
    {
        const char* wndclass = "_XRAY_1.5";

        // Register the windows class.
        // WNDCLASSEX, а не WNDCLASS, ради hIconSm: без него маленькую иконку для
        // кнопки в панели задач и для Alt+Tab Windows получает ужатием большой,
        // и она выходит мыльной. Просим у ресурса каждый размер отдельно.
        HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(nullptr);

        auto load_icon = [hInstance](int cx, int cy) -> HICON {
            HICON ico = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
            return ico ? ico : LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
        };

        WNDCLASSEX wndClass{};
        wndClass.cbSize = sizeof(wndClass);
        wndClass.lpfnWndProc = WndProc;
        wndClass.hInstance = hInstance;
        wndClass.hIcon = load_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
        wndClass.hIconSm = load_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        wndClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wndClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wndClass.lpszClassName = wndclass;
        RegisterClassEx(&wndClass);

        // Set the window's initial style.
        // WS_SYSMENU | WS_MINIMIZEBOX добавлены не для рамки: без MINIMIZEBOX клик
        // по кнопке в панели задач не сворачивает окно как положено, а без SYSMENU
        // нет системного меню (в т.ч. у той же кнопки по правому клику).
        const DWORD dwStyle = WS_BORDER | WS_DLGFRAME | WS_SYSMENU | WS_MINIMIZEBOX;

        // Set the window's initial width
        RECT rc;
        SetRect(&rc, 0, 0, 640, 480);
        AdjustWindowRect(&rc, dwStyle, FALSE);

        // Create the render window
        m_hWnd = CreateWindow /*Ex*/ ( // WS_EX_TOPMOST,
            wndclass, "OGSR Engine", dwStyle,
            /*rc.left, rc.top, */ CW_USEDEFAULT, CW_USEDEFAULT, (rc.right - rc.left), (rc.bottom - rc.top), 0L, 0, hInstance, 0L);
        gGameWindow = m_hWnd;

        TracySetProgramName("OGSR Engine");
    }
#elif defined(__SWITCH__)
    // Horizon: no window object at all -- the swapchain takes libnx's default NWindow (VK_NN_vi_surface).
#else
    // Linux / Android: an SDL window; HWND is SDL_Window* (vk_core.cpp creates the surface with SDL_Vulkan_CreateSurface,
    // xr_input_sdl.cpp pumps its events). The renderer sets the real size/mode later (vk_apply_window_mode_sdl).
    if (m_hWnd == nullptr)
    {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
            Msg("!![%s] SDL_InitSubSystem: %s", __FUNCTION__, SDL_GetError());
        Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef __ANDROID__
        flags |= SDL_WINDOW_FULLSCREEN; // the activity's surface; landscape comes from the manifest
#else
        flags |= SDL_WINDOW_HIDDEN;     // ShowMainWindow/the renderer shows it once it has its size
#endif
        SDL_Window* wnd = SDL_CreateWindow("OGSR Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, flags);
        if (!wnd)
            Msg("!![%s] SDL_CreateWindow: %s", __FUNCTION__, SDL_GetError());
        m_hWnd = static_cast<HWND>(wnd);
        gGameWindow = m_hWnd;
    }
#endif // _MSC_VER

    // Здесь снимался «слепок свойств окна» в m_dwWindowStyle / m_rcWindowBounds /
    // m_rcWindowClient. Снят вместе с полями: по обоим деревьям их не читал НИКТО,
    // а если бы читал — получил бы ложь. Слепок делается ДО рестайла рендером, то
    // есть описывает бордюрное окно 640×480, которого на экране не бывает.
    //
    // Заодно убран закомментированный разбор -gpu_sw / -gpu_nopure / -gpu_ref:
    // software vertex processing, non-pure device и reference rasterizer — это
    // понятия Direct3D 8/9, в вулкановской сборке не значащие ничего.
}

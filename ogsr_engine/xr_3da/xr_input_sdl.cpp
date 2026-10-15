#include "stdafx.h"

#include "xr_input_sdl.h"
#include "xr_platform_keys.h" // DIK_* -- non-MSVC branch (this file only builds off-MSVC)
#include "IInputReceiver.h"
#include "XR_IOConsole.h"     // Console->Execute("quit")
#ifdef XR_INPUT_PAD
#include "nx/xr_nx_pad.h"     // the pad state: libnx HID on the Switch, android/xr_android_pad.cpp on Android
#include "xr_platform_time.h" // xrPlatform::TickCount64
#endif

CInput* pInput = nullptr;
IInputReceiver dummyController;

ENGINE_API float psMouseSens = 0.1f;
ENGINE_API float psMouseSensScale = 1.f;
ENGINE_API Flags32 psMouseInvert = {FALSE};
ENGINE_API xr_pad_look_state g_pad_look{};
ENGINE_API u32 g_pad_play_frame = 0;
ENGINE_API xr_pad_touch_state g_pad_touch{};
ENGINE_API xr_touch_pad_state g_touch_pad{};
ENGINE_API u32 g_pad_real_frame = 0;
ENGINE_API bool g_pad_attached = false;

#define MOUSE_HOLD_TIME 60

// ---------------------------------------------------------------------------
// SDL_Scancode <-> DIK_*. Пары подобраны ПО СМЫСЛУ (имя к имени), не по
// числу -- опечатка в имени ловится компилятором (неизвестный символ),
// опечатка в числе была бы тихой. NEC PC98/J3100-специфичные коды
// (DIK_AT/COLON/UNDERLINE/KANJI/STOP/AX/UNLABELED/CIRCUMFLEX) не имеют
// современного соответствия -- современных клавиатур с ними не существует,
// сопоставлены с SDL_SCANCODE_UNKNOWN (никогда не придут от реального SDL).
// ---------------------------------------------------------------------------
namespace
{
struct KeyPair { SDL_Scancode sc; int dik; };
constexpr KeyPair kKeyMap[] = {
    {SDL_SCANCODE_A, DIK_A}, {SDL_SCANCODE_B, DIK_B}, {SDL_SCANCODE_C, DIK_C}, {SDL_SCANCODE_D, DIK_D},
    {SDL_SCANCODE_E, DIK_E}, {SDL_SCANCODE_F, DIK_F}, {SDL_SCANCODE_G, DIK_G}, {SDL_SCANCODE_H, DIK_H},
    {SDL_SCANCODE_I, DIK_I}, {SDL_SCANCODE_J, DIK_J}, {SDL_SCANCODE_K, DIK_K}, {SDL_SCANCODE_L, DIK_L},
    {SDL_SCANCODE_M, DIK_M}, {SDL_SCANCODE_N, DIK_N}, {SDL_SCANCODE_O, DIK_O}, {SDL_SCANCODE_P, DIK_P},
    {SDL_SCANCODE_Q, DIK_Q}, {SDL_SCANCODE_R, DIK_R}, {SDL_SCANCODE_S, DIK_S}, {SDL_SCANCODE_T, DIK_T},
    {SDL_SCANCODE_U, DIK_U}, {SDL_SCANCODE_V, DIK_V}, {SDL_SCANCODE_W, DIK_W}, {SDL_SCANCODE_X, DIK_X},
    {SDL_SCANCODE_Y, DIK_Y}, {SDL_SCANCODE_Z, DIK_Z},

    {SDL_SCANCODE_1, DIK_1}, {SDL_SCANCODE_2, DIK_2}, {SDL_SCANCODE_3, DIK_3}, {SDL_SCANCODE_4, DIK_4},
    {SDL_SCANCODE_5, DIK_5}, {SDL_SCANCODE_6, DIK_6}, {SDL_SCANCODE_7, DIK_7}, {SDL_SCANCODE_8, DIK_8},
    {SDL_SCANCODE_9, DIK_9}, {SDL_SCANCODE_0, DIK_0},

    {SDL_SCANCODE_RETURN, DIK_RETURN}, {SDL_SCANCODE_ESCAPE, DIK_ESCAPE},
    {SDL_SCANCODE_BACKSPACE, DIK_BACK}, {SDL_SCANCODE_TAB, DIK_TAB}, {SDL_SCANCODE_SPACE, DIK_SPACE},
    {SDL_SCANCODE_MINUS, DIK_MINUS}, {SDL_SCANCODE_EQUALS, DIK_EQUALS},
    {SDL_SCANCODE_LEFTBRACKET, DIK_LBRACKET}, {SDL_SCANCODE_RIGHTBRACKET, DIK_RBRACKET},
    {SDL_SCANCODE_BACKSLASH, DIK_BACKSLASH}, {SDL_SCANCODE_SEMICOLON, DIK_SEMICOLON},
    {SDL_SCANCODE_APOSTROPHE, DIK_APOSTROPHE}, {SDL_SCANCODE_GRAVE, DIK_GRAVE},
    {SDL_SCANCODE_COMMA, DIK_COMMA}, {SDL_SCANCODE_PERIOD, DIK_PERIOD}, {SDL_SCANCODE_SLASH, DIK_SLASH},
    {SDL_SCANCODE_CAPSLOCK, DIK_CAPITAL},

    {SDL_SCANCODE_F1, DIK_F1}, {SDL_SCANCODE_F2, DIK_F2}, {SDL_SCANCODE_F3, DIK_F3}, {SDL_SCANCODE_F4, DIK_F4},
    {SDL_SCANCODE_F5, DIK_F5}, {SDL_SCANCODE_F6, DIK_F6}, {SDL_SCANCODE_F7, DIK_F7}, {SDL_SCANCODE_F8, DIK_F8},
    {SDL_SCANCODE_F9, DIK_F9}, {SDL_SCANCODE_F10, DIK_F10}, {SDL_SCANCODE_F11, DIK_F11}, {SDL_SCANCODE_F12, DIK_F12},
    {SDL_SCANCODE_F13, DIK_F13}, {SDL_SCANCODE_F14, DIK_F14}, {SDL_SCANCODE_F15, DIK_F15},

    {SDL_SCANCODE_PRINTSCREEN, DIK_SYSRQ}, {SDL_SCANCODE_SCROLLLOCK, DIK_SCROLL}, {SDL_SCANCODE_PAUSE, DIK_PAUSE},
    {SDL_SCANCODE_INSERT, DIK_INSERT}, {SDL_SCANCODE_HOME, DIK_HOME}, {SDL_SCANCODE_PAGEUP, DIK_PRIOR},
    {SDL_SCANCODE_DELETE, DIK_DELETE}, {SDL_SCANCODE_END, DIK_END}, {SDL_SCANCODE_PAGEDOWN, DIK_NEXT},
    {SDL_SCANCODE_RIGHT, DIK_RIGHT}, {SDL_SCANCODE_LEFT, DIK_LEFT}, {SDL_SCANCODE_DOWN, DIK_DOWN}, {SDL_SCANCODE_UP, DIK_UP},

    {SDL_SCANCODE_NUMLOCKCLEAR, DIK_NUMLOCK}, {SDL_SCANCODE_KP_DIVIDE, DIK_DIVIDE},
    {SDL_SCANCODE_KP_MULTIPLY, DIK_MULTIPLY}, {SDL_SCANCODE_KP_MINUS, DIK_SUBTRACT}, {SDL_SCANCODE_KP_PLUS, DIK_ADD},
    {SDL_SCANCODE_KP_ENTER, DIK_NUMPADENTER},
    {SDL_SCANCODE_KP_1, DIK_NUMPAD1}, {SDL_SCANCODE_KP_2, DIK_NUMPAD2}, {SDL_SCANCODE_KP_3, DIK_NUMPAD3},
    {SDL_SCANCODE_KP_4, DIK_NUMPAD4}, {SDL_SCANCODE_KP_5, DIK_NUMPAD5}, {SDL_SCANCODE_KP_6, DIK_NUMPAD6},
    {SDL_SCANCODE_KP_7, DIK_NUMPAD7}, {SDL_SCANCODE_KP_8, DIK_NUMPAD8}, {SDL_SCANCODE_KP_9, DIK_NUMPAD9},
    {SDL_SCANCODE_KP_0, DIK_NUMPAD0}, {SDL_SCANCODE_KP_PERIOD, DIK_DECIMAL}, {SDL_SCANCODE_KP_EQUALS, DIK_NUMPADEQUALS},
    {SDL_SCANCODE_KP_COMMA, DIK_NUMPADCOMMA},

    {SDL_SCANCODE_APPLICATION, DIK_APPS}, {SDL_SCANCODE_LCTRL, DIK_LCONTROL}, {SDL_SCANCODE_LSHIFT, DIK_LSHIFT},
    {SDL_SCANCODE_LALT, DIK_LMENU}, {SDL_SCANCODE_LGUI, DIK_LWIN}, {SDL_SCANCODE_RCTRL, DIK_RCONTROL},
    {SDL_SCANCODE_RSHIFT, DIK_RSHIFT}, {SDL_SCANCODE_RALT, DIK_RMENU}, {SDL_SCANCODE_RGUI, DIK_RWIN},

    {SDL_SCANCODE_INTERNATIONAL2, DIK_KANA}, {SDL_SCANCODE_INTERNATIONAL3, DIK_YEN},
    {SDL_SCANCODE_INTERNATIONAL4, DIK_CONVERT}, {SDL_SCANCODE_INTERNATIONAL5, DIK_NOCONVERT},
};
} // namespace

int CInput::ScancodeToDik(SDL_Scancode sc)
{
    for (const auto& kv : kKeyMap)
        if (kv.sc == sc)
            return kv.dik;
    return -1; // не в таблице -- нет портируемого соответствия, пропустить
}

SDL_Scancode CInput::DikToScancode(int dik)
{
    for (const auto& kv : kKeyMap)
        if (kv.dik == dik)
            return kv.sc;
    return SDL_SCANCODE_UNKNOWN;
}

// ---------------------------------------------------------------------------
CInput::CInput(bool bExclusive, int deviceForInit)
{
    is_exclusive_mode = bExclusive;
    (void)deviceForInit; // SDL инициализирует клавиатуру+мышь разом, отдельных устройств нет

    Log("Starting INPUT device (SDL2)...");

#ifdef __SWITCH__
    // Horizon: no SDL video at all. SDL's Switch video driver is EGL/GLES and would claim the ONE
    // NWindow this title has, which the Vulkan swapchain (VK_NN_vi_surface) needs. Input comes straight
    // from libnx HID; there is no window handle and no text input (an on-screen keyboard is a later job).
    xr_nx_pad_init();
    m_window = nullptr;
#else
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
        SDL_InitSubSystem(SDL_INIT_VIDEO);

    // ⚠Реальное окно создаёт device.cpp (шаг вне этого файла -- см. память
    // platform-seam-step3-input-keys про Device_wndproc.cpp/WM_SYSCOMMAND).
    // Здесь ожидается, что окно уже существует к моменту создания CInput,
    // как и на Windows-пути (CreateInputDevice работал с уже созданным HWND).
    m_window = SDL_GetKeyboardFocus();

    // ⚠Включено всегда, не только при фокусе текстового поля: DirectInput-путь
    // тоже не разделял "текстовый" и "игровой" режим ввода, DikToChar был
    // доступен всегда. Точная настройка (глушить SDL_TEXTINPUT вне текстовых
    // полей, IME candidate window и т.п.) -- часть проводки IR_OnTextInput
    // через слои UI, см. platform-seam-step3-input-keys.
#ifdef __ANDROID__
    // No text field is focused at start: on a phone SDL_StartTextInput pops up the on-screen keyboard.
    SDL_StopTextInput();
    m_text_input_active = false;
#else
    SDL_StartTextInput();
    m_text_input_active = true;
#endif
#ifdef XR_INPUT_PAD
    xr_nx_pad_init(); // game controllers (+ touch overlay)
#endif
#endif

    iCapture(&dummyController);

    // OnFrame pumps the events (or polls the pad) once per frame on every platform this file builds for.
    Device.seqAppActivate.Add(this);
    Device.seqAppDeactivate.Add(this);
    Device.seqFrame.Add(this, REG_PRIORITY_HIGH);
}

CInput::~CInput()
{
    Device.seqFrame.Remove(this);
    Device.seqAppDeactivate.Remove(this);
    Device.seqAppActivate.Remove(this);
#ifndef __SWITCH__ // the SDL library is not linked on Horizon
    if (m_text_input_active)
        SDL_StopTextInput();
#endif
}

void CInput::DispatchKey(SDL_Scancode sc, bool down)
{
#ifdef XR_INPUT_PAD
    DispatchDik(ScancodeToDik(sc), down);
}

void CInput::DispatchDik(int dik, bool down)
{
#else
    const int dik = ScancodeToDik(sc);
#endif
    if (dik < 0 || dik >= COUNT_KB_BUTTONS)
        return;

    KBState[dik] = down ? 0x80 : 0;
    if (cbStack.empty())
        return;

    if (down)
        cbStack.back()->IR_OnKeyboardPress(dik);
    else
        cbStack.back()->IR_OnKeyboardRelease(dik);
}

void CInput::DispatchMouseButton(int sdlButton, bool down)
{
    // SDL: 1=left, 2=middle, 3=right, 4/5=side. DIK-эра: 0=left, 1=right, 2=middle.
    static constexpr int kMap[6] = {-1, 0, 2, 1, 3, 4};
    if (sdlButton < 1 || sdlButton > 5)
        return;
    const int btn = kMap[sdlButton];
    if (btn < 0 || btn >= COUNT_MOUSE_BUTTONS)
        return;

    mouseState[btn] = down ? TRUE : FALSE;
    mouseTime[btn] = down ? dwCurTime : 0;
    if (cbStack.empty())
        return;

    if (down)
        cbStack.back()->IR_OnMousePress(btn);
    else
        cbStack.back()->IR_OnMouseRelease(btn);
}

void CInput::DispatchTextInput(const char* utf8)
{
    if (!cbStack.empty())
        cbStack.back()->IR_OnTextInput(utf8);
}

BOOL CInput::iGetAsyncBtnState(int btn, bool hold) const
{
    if (btn < 0 || btn >= COUNT_MOUSE_BUTTONS)
        return FALSE;
    if (mouseState[btn] && hold)
        return dwCurTime - mouseTime[btn] >= MOUSE_HOLD_TIME;
    return mouseState[btn];
}

BOOL CInput::iGetAsyncKeyState(int dik, bool hold) const
{
    if (dik >= 0 && dik < COUNT_KB_BUTTONS)
        return KBState[dik] != 0;
    if (dik >= MOUSE_1 && dik <= MOUSE_8)
        return iGetAsyncBtnState(dik - MOUSE_1, hold);
    return FALSE;
}

void CInput::iCapture(IInputReceiver* p)
{
    VERIFY(p);
    if (!cbStack.empty())
        cbStack.back()->IR_OnDeactivate();
    cbStack.push_back(p);
    cbStack.back()->IR_OnActivate();
}

void CInput::iRelease(IInputReceiver* p)
{
    if (p == cbStack.back())
    {
        cbStack.back()->IR_OnDeactivate();
        cbStack.pop_back();
        cbStack.back()->IR_OnActivate();
    }
    else
    {
        for (u32 i = cbStack.size(); i > 0; --i)
            if (cbStack[i - 1] == p)
            {
                cbStack.erase(cbStack.begin() + (i - 1));
                break;
            }
    }
}

IInputReceiver* CInput::CurrentIR() const { return cbStack.empty() ? nullptr : cbStack.back(); }

void CInput::exclusive_mode(const bool exclusive)
{
    is_exclusive_mode = exclusive;
    // SDL_SetRelativeMouseMode -- ближайший аналог DirectInput EXCLUSIVE для
    // мыши (грабит курсор, шлёт дельты вместо абсолютных координат). Для
    // клавиатуры на SDL нет отдельного эксклюзивного режима -- она всегда
    // "неэксклюзивна" в терминах DirectInput (события идут через ОС как
    // обычно), что и было НЕэксклюзивным путём на Windows.
#ifndef __SWITCH__
    SDL_SetRelativeMouseMode(exclusive ? SDL_TRUE : SDL_FALSE);
#endif // Horizon: no mouse to capture; the right stick already produces relative motion
}

void CInput::OnAppActivate(void)
{
    if (CurrentIR())
        CurrentIR()->IR_OnActivate();
    memset(mouseState, 0, sizeof(mouseState));
    memset(KBState, 0, sizeof(KBState));
}

void CInput::OnAppDeactivate(void)
{
    if (CurrentIR())
        CurrentIR()->IR_OnDeactivate();
    memset(mouseState, 0, sizeof(mouseState));
    memset(KBState, 0, sizeof(KBState));
}

// ---------------------------------------------------------------------------
// Насос событий. Вызывается раз в кадр -- в отличие от DirectInput'а (буфер
// устройства читается по частям), SDL сам копит очередь, здесь она вся
// разбирается за один OnFrame. Наблюдаемый порядок вызовов IR_On* тот же:
// KeyUpdate/MouseUpdate на Windows-пути тоже разбирали НАКОПИВШИЙСЯ буфер
// целиком за кадр, не по одному событию.
// ---------------------------------------------------------------------------
void CInput::OnFrame(void)
{
#ifdef __SWITCH__
    dwCurTime = (u32)xrPlatform::TickCount64();
    PollNxPad();
#else
    dwCurTime = SDL_GetTicks();

    SDL_Event ev;
    while (SDL_PollEvent(&ev))
    {
        switch (ev.type)
        {
        case SDL_KEYDOWN:
            if (!ev.key.repeat) // hold ловит IR_OnKeyboardHold ниже, не повторный KEYDOWN
                DispatchKey(ev.key.keysym.scancode, true);
            break;
        case SDL_KEYUP:
            DispatchKey(ev.key.keysym.scancode, false);
            break;
        case SDL_TEXTINPUT:
            DispatchTextInput(ev.text.text);
            break;
        case SDL_MOUSEBUTTONDOWN:
            DispatchMouseButton(ev.button.button, true);
            break;
        case SDL_MOUSEBUTTONUP:
            DispatchMouseButton(ev.button.button, false);
            break;
        case SDL_MOUSEMOTION:
            if (!cbStack.empty() && (ev.motion.xrel || ev.motion.yrel))
                cbStack.back()->IR_OnMouseMove(ev.motion.xrel, ev.motion.yrel);
            break;
        case SDL_MOUSEWHEEL:
            if (!cbStack.empty() && ev.wheel.y)
                cbStack.back()->IR_OnMouseWheel(ev.wheel.y);
            break;
        case SDL_QUIT: // window closed / Android activity finishing: leave the way the menu's Quit does
        {
            static bool s_quitIssued = false;
            if (!s_quitIssued && Console)
            {
                s_quitIssued = true;
                Console->Execute("quit");
            }
            break;
        }
        default: break;
        }
    }
#ifdef XR_INPUT_PAD
    PollNxPad();
#endif
#endif // !__SWITCH__

    if (!cbStack.empty())
    {
        for (int dik = 0; dik < COUNT_KB_BUTTONS; ++dik)
            if (KBState[dik])
                cbStack.back()->IR_OnKeyboardHold(dik);
        for (int btn = 0; btn < COUNT_MOUSE_BUTTONS; ++btn)
            if (mouseState[btn] && dwCurTime - mouseTime[btn] >= MOUSE_HOLD_TIME)
                cbStack.back()->IR_OnMouseHold(btn);
    }
}

bool CInput::get_dik_name(int dik, char* dest, int dest_sz) const
{
#ifdef __SWITCH__
    // SDL_GetScancodeName lives in the SDL library, which the Switch build does not link at all (see
    // Layers/xrRenderVulkan/CMakeLists.txt). Key names only label bindings in the options UI.
    (void)dik; (void)dest; (void)dest_sz;
    return false;
#endif
    const SDL_Scancode sc = DikToScancode(dik);
    if (sc == SDL_SCANCODE_UNKNOWN)
        return false;
    const char* name = SDL_GetScancodeName(sc);
    if (!name || !*name)
        return false;
    strncpy(dest, name, dest_sz - 1);
    dest[dest_sz - 1] = 0;
    return true;
}

void CInput::clip_cursor(bool clip)
{
#ifdef __SWITCH__
    (void)clip; // no cursor to grab on Horizon
    return;
#endif
    if (!pInput || !pInput->window())
        return;
    SDL_SetWindowGrab(pInput->window(), clip ? SDL_TRUE : SDL_FALSE);
    SDL_ShowCursor(clip ? SDL_DISABLE : SDL_ENABLE);
}

#ifdef XR_INPUT_PAD
// ---------------------------------------------------------------------------
// Horizon / Android: pad -> the events a keyboard+mouse would have produced. A FIRST-BOOT SCHEME, not a designed
// one: the engine has no gamepad concept at all (movement is WASD, aim is mouse deltas), so the pad is
// translated at this seam and nothing above it changes. Contextual bindings (A = click in menus, a
// radial weapon wheel, gyro aim, touch as a pointer) are follow-up work.
//
//   left stick      W/A/S/D (digital, ~1/3 deflection)      ZR  left mouse button (fire / UI click)
//   right stick     mouse motion (look / UI cursor)         ZL  right mouse button (aim)
//   A F (use; R + A = Shift + F, drag)   B Space (jump)   X Y (wpn_next: tap = next weapon, hold = the weapon wheel)
//   (06-10 evening, the owner: wheel on X, reload on Y -- the two were the other way round)
//   13-10, the owner -- in play:  L Q / R E (lean left / right)   Y tap R (reload), Y hold F13 (next ammo type),
//   L + Y 0 (next fire mode)   R-stick click: tap = crouch (Ctrl held down), hold = deep crouch (Ctrl + Shift), the
//   next click stands up (so does the L-stick sprint)   R + R-stick click L (torch)   + tap Esc, + hold F6
//   (quick save), L + + F7 (quick load).
//   Over a screen as before: L Ctrl, R Shift, Y R, R-stick click L (the screens read those as L / R / Y / R3).
//   L-stick click X (sprint_toggle: press to run, stops by itself when you stop -- 05-10: was C, bound to nothing)
//   D-pad F1 F2 F3 F4 (quick slots: up/right/down/left; 06-10: was weapon slots 1-4)   + Esc
//   - tap I (inventory; closes the PDA), hold - P (PDA, 06-10)   R + - P (PDA)   [in the inventory the D-pad / A / B / X / Y are navigation, see ui_rml_inv.cpp]
// ---------------------------------------------------------------------------
namespace
{
struct PadKey { uint32_t bit; int dik; };
constexpr PadKey kPadKeys[] = {
    {XRPAD_B, DIK_SPACE},   {XRPAD_X, DIK_Y},       {XRPAD_LSTICK, DIK_X},
    {XRPAD_DUP, DIK_F1},    {XRPAD_DRIGHT, DIK_F2}, {XRPAD_DDOWN, DIK_F3},  {XRPAD_DLEFT, DIK_F4},
    // A, Y, L, R, the R-stick click, Plus and Minus are special: see PollNxPad
};
constexpr u32 kPadHoldMs = 350; // Y and the R-stick click: held this long is the hold, not the tap
constexpr u32 kPadSaveHoldMs = 700; // Plus held this long in play is the quick save
constexpr int   kMoveDead = 11000;       // of 32767: the stick becomes a key past ~1/3
constexpr int   kLookDead = 3500;
constexpr float kLookSpeed = 1400.f;     // "mouse counts" per second at full deflection
} // namespace

void CInput::PollNxPad()
{
    xr_nx_pad_state s;
    xr_nx_pad_poll(&s);
    // The real pad alone (before the touch controls are merged in): any button or a stick pushed past its dead zone.
    g_pad_attached = s.pad_attached != 0;
    if (s.buttons || std::abs(s.lx) > kMoveDead || std::abs(s.ly) > kMoveDead || std::abs(s.rx) > kMoveDead || std::abs(s.ry) > kMoveDead)
        g_pad_real_frame = Device.dwFrame;
    // The touch controls (a phone): a virtual pad over the real one -- its buttons OR-ed in, its stick used while
    // the real one rests. Written by the game a frame ago, only while in play.
    const bool touchPad = g_touch_pad.frame && Device.dwFrame - g_touch_pad.frame <= 2;
    if (touchPad)
    {
        s.buttons |= g_touch_pad.buttons;
        if (std::abs(s.lx) < kMoveDead && std::abs(s.ly) < kMoveDead)
        {
            s.lx = g_touch_pad.lx;
            s.ly = g_touch_pad.ly;
        }
    }

    // HOME > Close / the OS ending the title: leave the way the window close button does.
    static bool s_quitIssued = false;
    if (s.quit && !s_quitIssued)
    {
        s_quitIssued = true;
        Console->Execute("quit");
    }

    // Buttons: edge-triggered, exactly like a key going down/up.
    const uint32_t changed = s.buttons ^ m_nx_buttons;
    // Shadow ladder: hold ZL + ZR and press D-pad Up to step through four shadow set-ups, standing still, and read
    // [VK Perf] / [VK Frame] of each in the log (the line "[nx shadow] mode N" marks where it starts).
    if ((changed & XRPAD_DUP) && (s.buttons & XRPAD_DUP) && (s.buttons & XRPAD_ZL) && (s.buttons & XRPAD_ZR))
    {
        static int s_mode = 0;
        s_mode = (s_mode + 1) % 5;
        static const char* const kName[5] = { "no shadows, no lights", "sun cascades only (r_vsm 0)", "sun cascades + 2 lights",
                                              "sun cascades + 4 lights", "LITE: cascades (static only) + capsule shadows for characters + 2 lights, no point cubes" };
        static const char* const kCmd[5][5] = {
            { "r_shadows Off", "r_sun_shadows 0", "r_light_cap 0", "r_vsm 0", "r_shadow_lite 0" },
            { "r_shadows Off", "r_sun_shadows 1", "r_light_cap 0", "r_vsm 0", "r_shadow_lite 0" },
            { "r_shadows Off", "r_sun_shadows 1", "r_light_cap 2", "r_vsm 0", "r_shadow_lite 0" },
            { "r_shadows Off", "r_sun_shadows 1", "r_light_cap 4", "r_vsm 0", "r_shadow_lite 0" },
            { "r_shadows Off", "r_sun_shadows 1", "r_light_cap 2", "r_vsm 0", "r_shadow_lite 1" },
        };
        for (const char* c : kCmd[s_mode])
            Console->Execute(c);
        Msg("[nx shadow] mode %d: %s", s_mode, kName[s_mode]);
    }
    // Tree ladder: hold ZL + ZR and press D-pad Down. Stand still in a wood and give each step 10-15 s; the log line
    // "[nx tree] mode N" marks where it starts ([VK Perf] Trees / World/Depth / gpu_total, [VK Frame] fps).
    if ((changed & XRPAD_DDOWN) && (s.buttons & XRPAD_DDOWN) && (s.buttons & XRPAD_ZL) && (s.buttons & XRPAD_ZR))
    {
        static int s_tmode = 0;
        s_tmode = (s_tmode + 1) % 5;
        static const char* const kTName[5] = { "trees as shipped (mesh 80 m, prepass on)", "no tree prepass",
                                               "mesh only inside 40 m, no tree prepass", "mesh only inside 20 m, no tree prepass",
                                               "TREES OFF" };
        static const char* const kTCmd[5][3] = {
            { "r_tree_mesh_dist 80", "r_tree_prepass 1", "r_trees 1" },
            { "r_tree_mesh_dist 80", "r_tree_prepass 0", "r_trees 1" },
            { "r_tree_mesh_dist 40", "r_tree_prepass 0", "r_trees 1" },
            { "r_tree_mesh_dist 20", "r_tree_prepass 0", "r_trees 1" },
            { "r_tree_mesh_dist 80", "r_tree_prepass 0", "r_trees 0" },
        };
        for (const char* c : kTCmd[s_tmode])
            Console->Execute(c);
        Msg("[nx tree] mode %d: %s", s_tmode, kTName[s_tmode]);
    }
    // nfs19 crown sky brightness: hold ZL + ZR and press D-pad Right -- r_tree_sky_boost 1.0 1.5 2.0 2.5 3.0, around.
    if ((changed & XRPAD_DRIGHT) && (s.buttons & XRPAD_DRIGHT) && (s.buttons & XRPAD_ZL) && (s.buttons & XRPAD_ZR))
    {
        static int s_sky = 0;
        s_sky = (s_sky + 1) % 5;
        string64 cmd;
        xr_sprintf(cmd, "r_tree_sky_boost %.1f", 1.0f + 0.5f * float(s_sky));
        Console->Execute(cmd);
        Msg("[nx tree] %s (ZL+ZR+Right)", cmd);
    }
    // Benchmark tumbler: hold ZL + ZR and press Minus = toggle cpu_off (no AI / scripts / A-Life / physics; render only).
    if ((changed & XRPAD_MINUS) && (s.buttons & XRPAD_MINUS) && (s.buttons & XRPAD_ZL) && (s.buttons & XRPAD_ZR))
    {
        static int s_cpuOff = 0;
        s_cpuOff ^= 1;
        Console->Execute(s_cpuOff ? "cpu_off 1" : "cpu_off 0");
        Msg("[nx] cpu_off %d (ZL+ZR+Minus)", s_cpuOff);
    }
    // 13-10, the owner's scheme (the comment at the top). In play or over a screen is decided at the press, and the
    // key a press sent down is the one its release sends up, whatever happened in between.
    const bool inPlay = Device.dwFrame - g_pad_play_frame <= 2;
    const u32 padNow = Device.dwTimeContinual;
    auto tap = [this](int dik) {
        DispatchDik(dik, true);
        DispatchDik(dik, false);
    };
    auto pressed = [&](uint32_t bit) { return (changed & bit) && (s.buttons & bit); };
    auto released = [&](uint32_t bit) { return (changed & bit) && !(s.buttons & bit); };
    // The stance is kept by holding keys down: crouch is a held key in the engine, and accel is wiped every frame.
    enum
    {
        Stand,
        Crouch,
        Deep, // crouch + accel: the creep
    };
    static int s_stance = Stand;
    auto set_stance = [&](int st) {
        if ((s_stance != Stand) != (st != Stand))
            DispatchDik(DIK_LCONTROL, st != Stand);
        if ((s_stance == Deep) != (st == Deep))
            DispatchDik(DIK_LSHIFT, st == Deep);
        s_stance = st;
    };
    if (s_stance != Stand && !KBState[DIK_LCONTROL]) // the keys were let go under us (OnAppDeactivate: HOME)
    {
        if (s_stance == Deep && KBState[DIK_LSHIFT])
            DispatchDik(DIK_LSHIFT, false);
        s_stance = Stand;
    }
    // L / R
    {
        static int s_lDik = 0, s_rDik = 0;
        auto shoulder = [&](uint32_t bit, int& sent, int play, int screen) {
            if (pressed(bit))
                DispatchDik(sent = inPlay ? play : screen, true);
            else if (released(bit) && sent)
            {
                DispatchDik(sent, false);
                // a screen's L / R let go of a key the stance holds: it stays down, silently (no second press)
                if ((sent == DIK_LCONTROL && s_stance != Stand) || (sent == DIK_LSHIFT && s_stance == Deep))
                    KBState[sent] = 0x80;
                sent = 0;
            }
        };
        shoulder(XRPAD_L, s_lDik, DIK_Q, DIK_LCONTROL);
        shoulder(XRPAD_R, s_rDik, DIK_E, DIK_LSHIFT);
    }
    // A: use. R + A drags (CActor::ActorUse reads Shift held), so in the deep crouch Shift goes up for the press,
    // or a body could not be searched there.
    {
        static bool s_aShift = false;
        if (pressed(XRPAD_A))
        {
            const bool drag = inPlay && (s.buttons & XRPAD_R);
            if (drag && s_stance != Deep)
            {
                DispatchDik(DIK_LSHIFT, true);
                s_aShift = true;
            }
            const bool lift = inPlay && !drag && s_stance == Deep;
            if (lift)
                DispatchDik(DIK_LSHIFT, false);
            DispatchDik(DIK_F, true);
            if (lift)
                DispatchDik(DIK_LSHIFT, true);
        }
        else if (released(XRPAD_A))
        {
            DispatchDik(DIK_F, false);
            if (s_aShift)
                DispatchDik(DIK_LSHIFT, false);
            s_aShift = false;
        }
    }
    // Y: a tap reloads, held it is the next ammo type, L + Y the next fire mode. The reload goes at the release:
    // until then the press may still become a hold.
    {
        static bool s_yArmed = false;
        static u32 s_yT0 = 0;
        static int s_yDik = 0;
        if (pressed(XRPAD_Y))
        {
            if (!inPlay)
                DispatchDik(s_yDik = DIK_R, true);
            else if (s.buttons & XRPAD_L)
                tap(DIK_0); // wpn_firemode_next
            else
            {
                s_yArmed = true;
                s_yT0 = padNow;
            }
        }
        if (s_yArmed && (s.buttons & XRPAD_Y) && padNow - s_yT0 >= kPadHoldMs)
        {
            s_yArmed = false;
            tap(XR_PAD_DIK_AMMO);
        }
        if (released(XRPAD_Y))
        {
            if (s_yArmed)
                tap(DIK_R);
            else if (s_yDik)
                DispatchDik(s_yDik, false);
            s_yArmed = false;
            s_yDik = 0;
        }
    }
    // The R-stick click: from standing it crouches at once and, held, goes on down to the deep crouch; from either
    // the next click stands up. R + the click is the torch, and over a screen it is L as before (TAB in the
    // inventory, "where am I" on the PDA map).
    {
        static bool s_rsArmed = false;
        static u32 s_rsT0 = 0;
        static int s_rsDik = 0;
        if (pressed(XRPAD_RSTICK))
        {
            if (!inPlay || (s.buttons & XRPAD_R))
                DispatchDik(s_rsDik = DIK_L, true);
            else if (s_stance != Stand)
                set_stance(Stand);
            else
            {
                set_stance(Crouch);
                s_rsArmed = true;
                s_rsT0 = padNow;
            }
        }
        if (s_rsArmed && (s.buttons & XRPAD_RSTICK) && padNow - s_rsT0 >= kPadHoldMs)
        {
            s_rsArmed = false;
            set_stance(Deep);
        }
        if (released(XRPAD_RSTICK))
        {
            s_rsArmed = false;
            if (s_rsDik)
                DispatchDik(s_rsDik, false);
            s_rsDik = 0;
        }
    }
    // Plus: Esc (the menu). In play (13-10, the owner) held it is the quick save (F6) and L + Plus the quick load
    // (F7), so the Esc goes at the release there.
    {
        static bool s_plusArmed = false;
        static u32 s_plusT0 = 0;
        static int s_plusDik = 0;
        if (pressed(XRPAD_PLUS))
        {
            if (!inPlay)
                DispatchDik(s_plusDik = DIK_ESCAPE, true);
            else if (s.buttons & XRPAD_L)
                tap(DIK_F7); // quick_load
            else
            {
                s_plusArmed = true;
                s_plusT0 = padNow;
            }
        }
        if (s_plusArmed && (s.buttons & XRPAD_PLUS) && padNow - s_plusT0 >= kPadSaveHoldMs)
        {
            s_plusArmed = false;
            tap(DIK_F6); // quick_save
        }
        if (released(XRPAD_PLUS))
        {
            if (s_plusArmed)
                tap(DIK_ESCAPE);
            else if (s_plusDik)
                DispatchDik(s_plusDik, false);
            s_plusArmed = false;
            s_plusDik = 0;
        }
    }
    // the sprint (the L-stick click, below) gets up first
    if (inPlay && pressed(XRPAD_LSTICK) && s_stance != Stand)
        set_stance(Stand);

    for (const PadKey& pk : kPadKeys)
        if (changed & pk.bit)
            DispatchDik(pk.dik, (s.buttons & pk.bit) != 0);
    // Minus (06-10 evening, the owner's scheme): a TAP is the inventory (I) -- and closes
    // the PDA if it is up (CUIGameSP takes I as "close" there); HOLDING it ~0.4 s is the PDA
    // (P). With R held it is the PDA at once, as before. So nothing is sent on the press:
    // the key is chosen at the release (tap) or when the hold runs out, and the key sent
    // down is the one sent up. ZL + ZR + Minus stays the benchmark tumbler above.
    {
        constexpr u32 kMinusHoldMs = 400;
        static int s_minusDik = 0; // the key sent down and not yet up
        static u32 s_minusT0 = 0;
        static bool s_minusArmed = false; // pressed, nothing decided yet
        const u32 now = Device.dwTimeContinual;
        if ((changed & XRPAD_MINUS) && (s.buttons & XRPAD_MINUS))
        {
            if ((s.buttons & XRPAD_ZL) && (s.buttons & XRPAD_ZR))
                s_minusArmed = false;
            else if (s.buttons & XRPAD_R)
            {
                s_minusDik = DIK_P;
                DispatchDik(s_minusDik, true);
                s_minusArmed = false;
            }
            else
            {
                s_minusArmed = true;
                s_minusT0 = now;
            }
        }
        if (s_minusArmed && (s.buttons & XRPAD_MINUS) && now - s_minusT0 >= kMinusHoldMs)
        {
            s_minusArmed = false;
            s_minusDik = DIK_P; // held: the PDA
            DispatchDik(s_minusDik, true);
        }
        if ((changed & XRPAD_MINUS) && !(s.buttons & XRPAD_MINUS))
        {
            if (s_minusArmed) // let go in time: a tap
            {
                s_minusArmed = false;
                DispatchDik(DIK_I, true);
                DispatchDik(DIK_I, false);
            }
            else if (s_minusDik)
                DispatchDik(s_minusDik, false);
            s_minusDik = 0;
        }
    }
    if (changed & XRPAD_ZR)
        DispatchMouseButton(1, (s.buttons & XRPAD_ZR) != 0); // SDL numbering: 1 = left
    if (changed & XRPAD_ZL)
        DispatchMouseButton(3, (s.buttons & XRPAD_ZL) != 0); // 3 = right
    if (changed)
    {
        static int s_padLogs = 0;
        if (s_padLogs++ < 200)
            Msg("[nx pad] buttons=%04x (changed %04x) lx=%d ly=%d rx=%d ry=%d", s.buttons, changed, s.lx, s.ly, s.rx, s.ry);
    }
    m_nx_buttons = s.buttons;

    // Left stick -> WASD.
    const uint8_t want = uint8_t((s.ly > kMoveDead ? 1 : 0) | (s.ly < -kMoveDead ? 2 : 0) | (s.lx < -kMoveDead ? 4 : 0) |
                                 (s.lx > kMoveDead ? 8 : 0));
    static constexpr int kStickDik[4] = {DIK_W, DIK_S, DIK_A, DIK_D};
    const uint8_t diff = want ^ m_nx_stick_keys;
    for (int i = 0; i < 4; ++i)
        if (diff & (1 << i))
            DispatchDik(kStickDik[i], ((want >> i) & 1) != 0);
    m_nx_stick_keys = want;

    // Right stick -> relative "mouse" motion, scaled by the time since the last poll so the look speed
    // does not depend on the frame rate. Quadratic response: fine aim near centre, fast turn at the edge.
    const u32 dtMs = m_nx_last_tick ? std::min<u32>(dwCurTime - m_nx_last_tick, 100u) : 0u;
    m_nx_last_tick = dwCurTime;
    auto axis = [](int v) -> float {
        const int a = v < 0 ? -v : v;
        if (a <= kLookDead)
            return 0.f;
        const float n = float(a - kLookDead) / float(32767 - kLookDead);
        const float c = n * n;
        return v < 0 ? -c : c;
    };
    // The touch panel (the Switch's, a phone's screen): handed on as is, the screens make taps and pinches of it,
    // the phone's touch controls a virtual pad (xrGame/ui_rml_touch.cpp).
    g_pad_touch.frame = Device.dwFrame;
    g_pad_touch.count = s.touch_count;
    for (int i = 0; i < 4; ++i)
    {
        g_pad_touch.id[i] = s.touch_id[i];
        g_pad_touch.x[i] = float(s.touch_x[i]) / 1280.f;
        g_pad_touch.y[i] = float(s.touch_y[i]) / 720.f;
    }
    g_pad_look.frame = Device.dwFrame; // the look is the pad's: the aim assist may act (xrGame/actor_aim_assist.cpp)
    g_pad_look.x = axis(s.rx);
    g_pad_look.y = axis(s.ry);
    if (touchPad)
    {
        // a drag on the touch controls turns by what it moved, not by time: added as it is
        m_nx_look_rem[0] += g_touch_pad.look_x;
        m_nx_look_rem[1] += g_touch_pad.look_y;
    }
    g_touch_pad.look_x = g_touch_pad.look_y = 0.f;
    if ((dtMs || touchPad) && !cbStack.empty())
    {
        m_nx_look_rem[0] += axis(s.rx) * kLookSpeed * float(dtMs) * 0.001f;
        m_nx_look_rem[1] += -axis(s.ry) * kLookSpeed * float(dtMs) * 0.001f; // stick up = mouse up = negative y
        const int dx = int(m_nx_look_rem[0]);
        const int dy = int(m_nx_look_rem[1]);
        m_nx_look_rem[0] -= float(dx);
        m_nx_look_rem[1] -= float(dy);
        if (dx || dy)
            cbStack.back()->IR_OnMouseMove(dx, dy);
    }
}
#endif // XR_INPUT_PAD

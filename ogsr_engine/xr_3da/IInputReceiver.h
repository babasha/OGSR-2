// IInputReceiver.h: interface for the IInputReceiver class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

class ENGINE_API IInputReceiver
{
public:
    
    void IR_Capture(void);
    void IR_Release(void);

    virtual void IR_OnDeactivate(void);
    virtual void IR_OnActivate(void);

    virtual void IR_OnMousePress(int btn){};
    virtual void IR_OnMouseRelease(int btn){};
    virtual void IR_OnMouseHold(int btn){};
    virtual void IR_OnMouseWheel(int direction){};
    virtual void IR_OnMouseMove(int x, int y){};
    virtual void IR_OnMouseStop(int x, int y){};

    virtual void IR_OnKeyboardPress(int dik){};
    virtual void IR_OnKeyboardRelease(int dik){};
    virtual void IR_OnKeyboardHold(int dik){};

    // ⭐Шов для SDL-порта ввода (шаг 3 плана): текст для полей ввода (чат,
    // консоль, переименование сейвов) отдельно от кодов клавиш. На Windows/
    // DirectInput этот метод НИКЕМ НЕ ЗОВЁТСЯ -- виджеты по-прежнему тянут
    // символ через `pInput->DikToChar()` из своих IR_OnKeyboardPress/
    // OnKeyboard, как раньше, нулевое изменение поведения. Будущий SDL-бэкенд
    // будет звать ЭТОТ метод из SDL_TEXTINPUT (юзер выбрал событийную модель,
    // не эмуляцию poll-API -- см. [[platform-seam-step3-input-keys]]) --
    // текст приходит уже разрешённым по раскладке/Shift/IME, ничего
    // переводить самому не надо. utf8 -- null-terminated, обычно 1-4 байта на
    // вызов (может прийти состав из нескольких кодпоинтов, как в SDL).
    // ⚠Оба пути (DikToChar в OnKeyboard И этот метод) НЕ МОГУТ звонить
    // одновременно в одном бинаре -- линкуется только ОДИН бэкенд ввода на
    // платформу, поэтому двойной вставки символа не бывает по конструкции.
    virtual void IR_OnTextInput(const char* /*utf8*/) {};
};

ENGINE_API extern float psMouseSens;
ENGINE_API extern float psMouseSensScale;
ENGINE_API extern Flags32 psMouseInvert;

// The pad's look stick, for the game's aim assist (xrGame/actor_aim_assist.cpp): it has to know the look is not a mouse.
// Written by the pad backend on every poll (xr_input_sdl.cpp PollNxPad: Switch, Android); a mouse-only PC leaves it zero.
struct xr_pad_look_state
{
    u32 frame; // Device.dwFrame of the last pad poll, 0 = never
    float x, y; // the right stick past the dead zone and the response curve, -1..1; x right = +, y up = +
};
ENGINE_API extern xr_pad_look_state g_pad_look;
// The Switch's touch panel, for the RmlUi screens (xrGame/ui_rml.cpp TouchFrame): tap = click, swipe = scroll, a long
// press = the right button, two fingers = a pinch on the PDA map. Written by the pad backend on every poll.
struct xr_pad_touch_state
{
    u32 frame; // Device.dwFrame of the last poll, 0 = never
    u32 count; // fingers down (the first four below; the Switch's panel fills two)
    u32 id[4]; // a finger keeps its id while it stays down
    float x[4], y[4]; // 0..1 across the screen
};
ENGINE_API extern xr_pad_touch_state g_pad_touch;
// The phone's touch controls (xrGame/ui_rml_touch.cpp) as a virtual pad: the game writes it while in play, and
// CInput::PollNxPad merges it into the real pad (buttons OR-ed, the stick when the real one rests) on the next poll.
// look_x/y: mouse units to turn by, added up by the game and taken (zeroed) by the poll.
struct xr_touch_pad_state
{
    u32 frame; // Device.dwFrame it was written in, 0 = never
    u32 buttons; // XRPAD_* bits (nx/xr_nx_pad.h)
    s16 lx, ly; // the left stick, -32767..32767, +y = up
    float look_x, look_y;
};
ENGINE_API extern xr_touch_pad_state g_touch_pad;
// The real pad, for the touch controls to step aside while it plays (xrGame/ui_rml_touch.cpp): the Device.dwFrame it
// last pressed a button or moved a stick in (0 = never), and whether one is attached at all.
ENGINE_API extern u32 g_pad_real_frame;
ENGINE_API extern bool g_pad_attached;
// The screen's physical dots per inch where the platform says (Android: SDL), 0 = unknown. The HUD's size on a phone.
ENGINE_API extern float g_display_dpi;
// Device.dwFrame of the last frame the game was in play (an actor alive, no screen, dialog or console up), written by
// the game (xrGame/ui_rml_hud.cpp). The pad sends other keys in play than over a screen: L / R lean in play and are
// Ctrl / Shift (the screens' L / R) over one, the right stick's click is a stance in play and the screens' L.
ENGINE_API extern u32 g_pad_play_frame;
// The pad's "next ammo type" (Y held): a key bound to nothing (DIK_F13), taken by the weapon wheel -- wpn_next itself
// is the wheel's own key there (xrGame/ui_rml_hud.cpp UIRml::WheelKey).
constexpr int XR_PAD_DIK_AMMO = 0x64;

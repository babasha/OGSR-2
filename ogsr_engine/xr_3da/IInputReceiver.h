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

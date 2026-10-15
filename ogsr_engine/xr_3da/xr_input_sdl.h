#pragma once

// ---------------------------------------------------------------------------
// ШАГ 3б плана порта: SDL2-бэкенд ввода/окна.
//
// ⚠⚠⚠ЭТОТ ФАЙЛ НЕ ПОДКЛЮЧЁН НИ К ОДНОМУ .vcxproj -- он не компилируется
// сегодняшней Windows-сборкой вообще, поэтому не может её сломать. Это
// АЛЬТЕРНАТИВА xr_input.h/Xr_input.cpp: на платформе без DirectInput в
// проект собирается ЭТОТ файл ВМЕСТО того, не одновременно с ним (оба
// определяют класс `CInput`). Публичный интерфейс — тот же набор методов,
// что вызывающий код (40+ файлов) уже использует через `pInput->...`, чтобы
// не трогать их вообще.
//
// ⭐Архитектурное решение юзера (19-09): текст для полей ввода (чат, консоль,
// переименование сейвов) идёт через `IInputReceiver::IR_OnTextInput` —
// SDL_TEXTINPUT-событие, а НЕ через опрос "дай символ для этого скан-кода"
// (как делал `DikToChar` на DirectInput). Поэтому `DikToChar` здесь —
// НЕРАБОЧАЯ ЗАГЛУШКА (возвращает 0); переводить текст самому не нужно, SDL
// уже отдаёт готовую строку с учётом раскладки/Shift/IME.
//
// ⚠Второе решение юзера: `g_ed_embedded` (рендер встроен в окно редактора
// хоста) остаётся ТОЛЬКО на Windows/DirectInput-пути. Здесь этой концепции
// нет вообще -- SDL_Window создаётся и управляется этим классом целиком.
// ---------------------------------------------------------------------------

#include <SDL2/SDL.h>

// A game pad drives the same IR_On* calls a keyboard+mouse would make (PollNxPad): the Switch (libnx HID,
// nx/xr_nx_pad.c) and Android (SDL game controller + the touch overlay, android/xr_android_pad.cpp).
#if defined(__SWITCH__) || defined(__ANDROID__)
#define XR_INPUT_PAD 1
#endif

class ENGINE_API IInputReceiver;

const int mouse_device_key = 1;
const int keyboard_device_key = 2;
const int all_device_key = mouse_device_key | keyboard_device_key;
const int default_key = mouse_device_key | keyboard_device_key;

#define MOUSE_1 (0xED + 100)
#define MOUSE_8 (0xED + 107)

class ENGINE_API CInput : public pureFrame, public pureAppActivate, public pureAppDeactivate
{
public:
    enum
    {
        COUNT_MOUSE_BUTTONS = 8,
        COUNT_KB_BUTTONS = 256
    };

private:
    SDL_Window* m_window{};
    bool is_exclusive_mode{};
    bool m_text_input_active{};

    uint8_t KBState[COUNT_KB_BUTTONS]{};
    BOOL mouseState[COUNT_MOUSE_BUTTONS]{};
    u32 mouseTime[COUNT_MOUSE_BUTTONS]{};
    u32 dwCurTime{};

    xr_vector<IInputReceiver*> cbStack;

    // SDL_Scancode -> DIK_* и обратно. Реализация -- в .cpp, таблица большая.
    static int ScancodeToDik(SDL_Scancode sc);
    static SDL_Scancode DikToScancode(int dik);

    void DispatchKey(SDL_Scancode sc, bool down);
    void DispatchMouseButton(int sdlButton, bool down);
    void DispatchTextInput(const char* utf8);

#ifdef XR_INPUT_PAD
    // Horizon has no keyboard/mouse events to pump: the pad (libnx HID, nx/xr_nx_pad.c) is turned into the
    // same IR_On* calls a keyboard+mouse would make. See PollNxPad() for the mapping.
    void DispatchDik(int dik, bool down);
    void PollNxPad();
    uint32_t m_nx_buttons{};   // XRPAD_* held last frame (for edge detection)
    uint8_t m_nx_stick_keys{}; // left stick as digital keys: bit0 W, bit1 S, bit2 A, bit3 D
    u32 m_nx_last_tick{};
    float m_nx_look_rem[2]{};  // sub-pixel remainder of the right-stick "mouse" motion
#endif

    BOOL iGetAsyncBtnState(int btn, bool hold = false) const;

public:
    CInput(bool bExclusive = true, int deviceForInit = default_key);
    ~CInput();

    void iCapture(IInputReceiver* pc);
    void iRelease(IInputReceiver* pc);
    BOOL iGetAsyncKeyState(int dik, bool hold = false) const;

    virtual void OnFrame(void);
    virtual void OnAppActivate(void);
    virtual void OnAppDeactivate(void);

    IInputReceiver* CurrentIR() const;

    void exclusive_mode(const bool exclusive);
    bool exclusive_mode() const { return is_exclusive_mode; }

    // ⚠Заглушка -- см. обоснование в шапке файла. Текст идёт через
    // IR_OnTextInput, не через опрос.
    u16 DikToChar(const int dik, const bool utf) const { (void)dik; (void)utf; return 0; }

    bool get_dik_name(int dik, char* dest, int dest_sz) const;
    static void clip_cursor(bool clip);

    SDL_Window* window() const { return m_window; }
};

extern ENGINE_API CInput* pInput;

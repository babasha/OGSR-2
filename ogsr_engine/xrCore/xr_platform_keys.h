#pragma once

// ---------------------------------------------------------------------------
// ШОВ xrCore/platform, шаг 3а: коды клавиш DIK_*.
//
// ~130 имён DIK_* используются ПО ИМЕНИ в 40+ файлах игровой логики, ИИ и UI
// (раскладка, бинды, дебаг-редакторы) -- переименовывать сотни мест было бы
// большой и рискованной механической правкой без прямой пользы. Вместо этого
// меняется ТОЛЬКО ИСТОЧНИК имён: на Windows это по-прежнему `<dinput.h>`
// (нулевое изменение поведения), на другой платформе -- ТЕ ЖЕ имена и ТЕ ЖЕ
// ЧИСЛЕННЫЕ ЗНАЧЕНИЯ (скопированы дословно из Windows SDK `dinput.h` ниже,
// это стандартный PC/AT scan-code набор), заданные без единого DirectInput-
// типа.
//
// ⚠До этого шва `<dinput.h>` подключался в ДВУХ местах: централизованно из
// `xr_3da/xr_input.h` (настоящий DirectInput-слой, класс CInput) и НАПРЯМУЮ
// ещё в 24 файлах (xrGame/ui/*, xrGame/ai/monsters/*, редакторы, ...),
// которым нужны были только числа DIK_*, а не COM-интерфейсы DirectInput --
// проверено грепом на LPDIRECTINPUT*/DIDATAFORMAT/DIDEVICEINSTANCE/DIDEVCAPS:
// ни один из 24 их не использует. Все 24 файла + xr_input.h переведены на
// этот заголовок вместо `<dinput.h>`.
//
// ⚠⚠Сам DirectInput-слой (`CInput` в xr_input.h/Xr_input.cpp: LPDIRECTINPUT8,
// создание/поллинг устройств) этим НЕ портирован -- на не-MSVC он не
// соберётся вообще, там нужен отдельный SDL-бэкенд (шаг 3б плана, другая по
// риску задача: функциональная замена подсистемы, а не только имён кодов,
// и её нельзя проверить компиляцией -- нужен живой прогон с клавиатурой).
// ---------------------------------------------------------------------------

#ifdef _MSC_VER

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#else

// Скопировано дословно из Windows SDK <dinput.h> -- числа НЕ придуманы
// заново, чтобы любые сериализованные бинды/раскладки, хранящие DIK_* как
// число (а не по имени), остались совместимы между платформами.
#define DIK_ESCAPE          0x01
#define DIK_1               0x02
#define DIK_2               0x03
#define DIK_3               0x04
#define DIK_4               0x05
#define DIK_5               0x06
#define DIK_6               0x07
#define DIK_7               0x08
#define DIK_8               0x09
#define DIK_9               0x0A
#define DIK_0               0x0B
#define DIK_MINUS           0x0C
#define DIK_EQUALS          0x0D
#define DIK_BACK            0x0E
#define DIK_TAB             0x0F
#define DIK_Q               0x10
#define DIK_W               0x11
#define DIK_E               0x12
#define DIK_R               0x13
#define DIK_T               0x14
#define DIK_Y               0x15
#define DIK_U               0x16
#define DIK_I               0x17
#define DIK_O               0x18
#define DIK_P               0x19
#define DIK_LBRACKET        0x1A
#define DIK_RBRACKET        0x1B
#define DIK_RETURN          0x1C
#define DIK_LCONTROL        0x1D
#define DIK_A               0x1E
#define DIK_S               0x1F
#define DIK_D               0x20
#define DIK_F               0x21
#define DIK_G               0x22
#define DIK_H               0x23
#define DIK_J               0x24
#define DIK_K               0x25
#define DIK_L               0x26
#define DIK_SEMICOLON       0x27
#define DIK_APOSTROPHE      0x28
#define DIK_GRAVE           0x29
#define DIK_LSHIFT          0x2A
#define DIK_BACKSLASH       0x2B
#define DIK_Z               0x2C
#define DIK_X               0x2D
#define DIK_C               0x2E
#define DIK_V               0x2F
#define DIK_B               0x30
#define DIK_N               0x31
#define DIK_M               0x32
#define DIK_COMMA           0x33
#define DIK_PERIOD          0x34
#define DIK_SLASH           0x35
#define DIK_RSHIFT          0x36
#define DIK_MULTIPLY        0x37
#define DIK_LMENU           0x38
#define DIK_SPACE           0x39
#define DIK_CAPITAL         0x3A
#define DIK_F1              0x3B
#define DIK_F2              0x3C
#define DIK_F3              0x3D
#define DIK_F4              0x3E
#define DIK_F5              0x3F
#define DIK_F6              0x40
#define DIK_F7              0x41
#define DIK_F8              0x42
#define DIK_F9              0x43
#define DIK_F10             0x44
#define DIK_NUMLOCK         0x45
#define DIK_SCROLL          0x46
#define DIK_NUMPAD7         0x47
#define DIK_NUMPAD8         0x48
#define DIK_NUMPAD9         0x49
#define DIK_SUBTRACT        0x4A
#define DIK_NUMPAD4         0x4B
#define DIK_NUMPAD5         0x4C
#define DIK_NUMPAD6         0x4D
#define DIK_ADD             0x4E
#define DIK_NUMPAD1         0x4F
#define DIK_NUMPAD2         0x50
#define DIK_NUMPAD3         0x51
#define DIK_NUMPAD0         0x52
#define DIK_DECIMAL         0x53
#define DIK_OEM_102         0x56
#define DIK_F11             0x57
#define DIK_F12             0x58
#define DIK_F13             0x64
#define DIK_F14             0x65
#define DIK_F15             0x66
#define DIK_KANA            0x70
#define DIK_ABNT_C1         0x73
#define DIK_CONVERT         0x79
#define DIK_NOCONVERT       0x7B
#define DIK_YEN             0x7D
#define DIK_ABNT_C2         0x7E
#define DIK_NUMPADEQUALS    0x8D
#define DIK_PREVTRACK       0x90
#define DIK_AT              0x91
#define DIK_COLON           0x92
#define DIK_UNDERLINE       0x93
#define DIK_KANJI           0x94
#define DIK_STOP            0x95
#define DIK_AX              0x96
#define DIK_UNLABELED       0x97
#define DIK_NEXTTRACK       0x99
#define DIK_NUMPADENTER     0x9C
#define DIK_RCONTROL        0x9D
#define DIK_MUTE            0xA0
#define DIK_CALCULATOR      0xA1
#define DIK_PLAYPAUSE       0xA2
#define DIK_MEDIASTOP       0xA4
#define DIK_VOLUMEDOWN      0xAE
#define DIK_VOLUMEUP        0xB0
#define DIK_WEBHOME         0xB2
#define DIK_NUMPADCOMMA     0xB3
#define DIK_DIVIDE          0xB5
#define DIK_SYSRQ           0xB7
#define DIK_RMENU           0xB8
#define DIK_PAUSE           0xC5
#define DIK_HOME            0xC7
#define DIK_UP              0xC8
#define DIK_PRIOR           0xC9
#define DIK_LEFT            0xCB
#define DIK_RIGHT           0xCD
#define DIK_END             0xCF
#define DIK_DOWN            0xD0
#define DIK_NEXT            0xD1
#define DIK_INSERT          0xD2
#define DIK_DELETE          0xD3
#define DIK_LWIN            0xDB
#define DIK_RWIN            0xDC
#define DIK_APPS            0xDD
#define DIK_POWER           0xDE
#define DIK_SLEEP           0xDF
#define DIK_WAKE            0xE3
#define DIK_WEBSEARCH       0xE5
#define DIK_WEBFAVORITES    0xE6
#define DIK_WEBREFRESH      0xE7
#define DIK_WEBSTOP         0xE8
#define DIK_WEBFORWARD      0xE9
#define DIK_WEBBACK         0xEA
#define DIK_MYCOMPUTER      0xEB
#define DIK_MAIL            0xEC
#define DIK_MEDIASELECT     0xED

// Альтернативные имена (перенесены как есть из dinput.h).
#define DIK_BACKSPACE       DIK_BACK
#define DIK_NUMPADSTAR      DIK_MULTIPLY
#define DIK_LALT            DIK_LMENU
#define DIK_CAPSLOCK        DIK_CAPITAL
#define DIK_NUMPADMINUS     DIK_SUBTRACT
#define DIK_NUMPADPLUS      DIK_ADD
#define DIK_NUMPADPERIOD    DIK_DECIMAL
#define DIK_NUMPADSLASH     DIK_DIVIDE
#define DIK_RALT            DIK_RMENU
#define DIK_UPARROW         DIK_UP
#define DIK_PGUP            DIK_PRIOR
#define DIK_LEFTARROW       DIK_LEFT
#define DIK_RIGHTARROW      DIK_RIGHT
#define DIK_DOWNARROW       DIK_DOWN
#define DIK_PGDN            DIK_NEXT
#define DIK_CIRCUMFLEX      DIK_PREVTRACK

// DIKEYBOARD_* (dinput.h action-mapping codes: 0x81000000 | DIK_*). UICustomEdit's key switch lists
// them next to the plain DIK_ codes; values copied from dinput.h so the case labels stay distinct.
#define DIKEYBOARD_LEFT     int(0x810000CBu) // int: the key switch is on int dik
#define DIKEYBOARD_RIGHT    int(0x810000CDu)
#define DIKEYBOARD_DELETE   int(0x810000D3u)

// ---------------------------------------------------------------------------
// Opaque DirectInput COM types -- kept only so CInput's declaration (member
// fields, method signatures) parses on this platform; xr_input.h/Xr_input.cpp
// already document (see the header comment above) that the real DirectInput
// device layer is NOT ported here, same deferred-to-an-SDL-backend decision
// as platform-seam-step3-input-keys. Never populated with real device data
// on this platform -- Xr_input.cpp's non-MSVC branch stubs every method that
// would touch them.
struct DIDEVCAPS { int _unused; };
struct DIDEVICEINSTANCE { int _unused; };
struct DIDEVICEOBJECTINSTANCE { int _unused; };
struct DIDATAFORMAT { int _unused; };
using LPDIRECTINPUT8 = void*;
using LPDIRECTINPUTDEVICE8 = void*;

#endif // _MSC_VER

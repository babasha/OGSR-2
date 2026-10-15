#include "stdafx.h"

// ⚠19-09: этот файл задуман ПОРТИРУЕМЫМ (используется обоими бэкендами
// ввода), но нёс #include "xr_input.h" (DirectInput) и DIMOFS_X/DIMOFS_Y
// (числа оттуда же) -- найдено при сборке SDL-бэкенда (xr_input_sdl.h)
// реальным компилятором. Значение DIMOFS_X/Y НЕ важно ни одному потребителю
// IR_OnMouseStop -- проверено грепом, все реализации либо игнорируют
// параметр, либо прокидывают его дальше без сравнения (xrGame/Level_input.cpp,
// MainMenu.cpp, ui/UIGameTutorial.cpp). Заменены на свои независимые от
// DirectInput константы.
#ifdef _MSC_VER
#include "xr_input.h"
#else
#include "xr_input_sdl.h"
#endif
#include "IInputReceiver.h" // ⚠ регистр важен на case-sensitive ФС (Linux)

namespace { constexpr int kMouseAxisX = 0, kMouseAxisY = 1; }

void IInputReceiver::IR_Capture(void)
{
    VERIFY(pInput);
    pInput->iCapture(this);
}

void IInputReceiver::IR_Release(void)
{
    VERIFY(pInput);
    pInput->iRelease(this);
}

void IInputReceiver::IR_OnDeactivate(void)
{
    int i;
    for (i = 0; i < CInput::COUNT_KB_BUTTONS; i++)
        if (pInput->iGetAsyncKeyState(i))
            IR_OnKeyboardRelease(i);
    for (i = 0; i < CInput::COUNT_MOUSE_BUTTONS; i++)
        if (pInput->iGetAsyncKeyState(i + MOUSE_1))
            IR_OnMouseRelease(i);

    IR_OnMouseStop(kMouseAxisX, 0);
    IR_OnMouseStop(kMouseAxisY, 0);
}

void IInputReceiver::IR_OnActivate(void) {}

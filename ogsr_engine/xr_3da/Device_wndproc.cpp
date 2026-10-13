#include "stdafx.h"

#include "XR_IOConsole.h"

bool CRenderDevice::on_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    switch (uMsg)
    {
    case WM_SYSKEYDOWN: {
        // Alt+F4 обязан дойти до DefWindowProc — только там он превращается в
        // SC_CLOSE → WM_CLOSE. Раньше вся ветка съедалась целиком, и вместе с
        // «Alt открывает оконное меню и вешает кадровый цикл» глушился и выход.
        if (wParam == VK_F4)
            return (false);

        // ⚠️Остальное по-прежнему съедаем (Alt в одиночку, F10, Alt+Enter), но
        // ЯВНО присвоив result: в WndProc он объявлен без инициализации, и
        // `return true` без присваивания возвращал системе мусор со стека.
        result = 0;
        return (true);
    }
    case WM_ACTIVATE: {
        OnWM_Activate(wParam, lParam);
        return (false);
    }
    case WM_SETCURSOR: {
        // Embedded in the SDK: show a normal arrow over the viewport. We must set it
        // AND consume the message — returning false would let DefWindowProc forward
        // WM_SETCURSOR up to the parent (the SDK), whose ImGui handler hides the OS
        // cursor (it draws a software cursor on its DX9 backbuffer, which our Vulkan
        // child occludes → an invisible pointer). The game path is unchanged.
        extern bool g_ed_embedded;
        if (g_ed_embedded)
        {
            if (LOWORD(lParam) == HTCLIENT)
            {
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                result = TRUE;
                return (true);
            }
            return (false); // non-client (borders) — default handling
        }
        result = 1;
        return (true);
    }
    case WM_SYSCOMMAND: {
        // Prevent moving/sizing and power loss in fullscreen mode
        switch (wParam)
        {
        case SC_MOVE:
        case SC_SIZE:
        case SC_MAXIMIZE:
        // SC_SCREENSAVE ходит в паре с SC_MONITORPOWER — без него хранитель
        // экрана мог влезть посреди сессии. Современная половина того же
        // лечения (SetThreadExecutionState) стоит в CRenderDevice::Run.
        case SC_SCREENSAVE:
        // Alt/F10 в одиночку открывают оконное меню: это модальный цикл, он
        // останавливает кадры, а с WS_SYSMENU (добавлен в Device_Initialize)
        // стал по-настоящему достижим.
        case SC_KEYMENU:
        case SC_MONITORPOWER: result = 1; return (true);
        }
        return (false);
    }
    case WM_CLOSE: {
        // Раньше здесь стояла глухая заглушка: крестик, Alt+F4 и «Закрыть окно»
        // из меню кнопки в панели задач не делали НИЧЕГО, и Windows через пару
        // секунд предлагала прибить процесс — то есть выход мимо
        // KERNEL:disconnect, о котором сервер узнавал только по таймауту.
        // Ведём выход штатным путём, тем же, что и консольный `quit`.
        extern bool g_ed_embedded;
        static bool s_quit_requested = false; // WM_CLOSE может прийти не один раз

        if (!g_ed_embedded && !s_quit_requested && Console)
        {
            s_quit_requested = true;
            Console->Execute("quit");
        }

        result = 0;
        return (true);
    }
    }

    return (false);
}
//-----------------------------------------------------------------------------
// Name: WndProc()
// Desc: Static msg handler which passes messages to the application class.
//-----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // ⚠️Инициализация обязательна: on_message возвращает true как «сообщение
    // обработано, отдай result», и любая ветка, забывшая его присвоить, отдаёт
    // системе мусор со стека. Одна такая ветка (WM_SYSKEYDOWN) уже была; ноль
    // по умолчанию закрывает весь класс ошибок, а не её одну.
    LRESULT result = 0;
    if (Device.on_message(hWnd, uMsg, wParam, lParam, result))
        return (result);

    return (DefWindowProc(hWnd, uMsg, wParam, lParam));
}

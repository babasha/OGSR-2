#include "stdafx.h"

#include "xr_input.h"
#include "IInputReceiver.h"

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

#ifdef _MSC_VER

#define MOUSEBUFFERSIZE 64
#define KEYBOARDBUFFERSIZE 64

// A2.3 (editor-embedded): DirectInput requires the cooperative-level window to be
// TOP-LEVEL. When the engine renders into a host-owned panel, Device.m_hWnd is a
// WS_CHILD — SetCooperativeLevel then fails with DIERR_INVALIDPARAM and the CHK_DX in
// CInput's ctor aborts the process. Use the child's top-level ancestor (the host's
// window) instead; input still belongs to the same window from the OS's point of view.
static HWND input_coop_hwnd()
{
    extern bool g_ed_embedded;
    if (g_ed_embedded && Device.m_hWnd)
        return GetAncestor(Device.m_hWnd, GA_ROOT);
    return Device.m_hWnd;
}

CInput::CInput(bool bExclusive, int deviceForInit)
{
    is_exclusive_mode = bExclusive;

    Log("Starting INPUT device...");

    mouse_property.mouse_dt = 25;

    //===================== Dummy pack
    iCapture(&dummyController);

    if (!pDI)
        CHK_DX(DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&pDI, NULL));

    // KEYBOARD
    if (deviceForInit & keyboard_device_key)
        CHK_DX(CreateInputDevice(&pKeyboard, GUID_SysKeyboard, &c_dfDIKeyboard
            , (is_exclusive_mode ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND, KEYBOARDBUFFERSIZE));

    // MOUSE
    if (deviceForInit & mouse_device_key)
        CHK_DX(CreateInputDevice(&pMouse, GUID_SysMouse, &c_dfDIMouse2
            , (is_exclusive_mode ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY, MOUSEBUFFERSIZE));

    Device.seqAppActivate.Add(this);
    Device.seqAppDeactivate.Add(this);
    Device.seqFrame.Add(this, REG_PRIORITY_HIGH);
}

CInput::~CInput(void)
{
    Device.seqFrame.Remove(this);
    Device.seqAppDeactivate.Remove(this);
    Device.seqAppActivate.Remove(this);

    // Unacquire and release the device's interfaces
    if (pMouse)
    {
        pMouse->Unacquire();
        _RELEASE(pMouse);
    }

    if (pKeyboard)
    {
        pKeyboard->Unacquire();
        _RELEASE(pKeyboard);
    }

    _SHOW_REF("Input: ", pDI);
    _RELEASE(pDI);
}

//-----------------------------------------------------------------------------
// Name: CreateInputDevice()
// Desc: Create a DirectInput device.
//-----------------------------------------------------------------------------
HRESULT CInput::CreateInputDevice(LPDIRECTINPUTDEVICE8* device, GUID guidDevice, const DIDATAFORMAT* pdidDataFormat, u32 dwFlags, u32 buf_size) const
{
    // Obtain an interface to the input device
    //.	CHK_DX( pDI->CreateDeviceEx( guidDevice, IID_IDirectInputDevice8, (void**)device, NULL ) );
    CHK_DX(pDI->CreateDevice(guidDevice, /*IID_IDirectInputDevice8,*/ device, NULL));

    // Set the device data format. Note: a data format specifies which
    // controls on a device we are interested in, and how they should be
    // reported.
    CHK_DX((*device)->SetDataFormat(pdidDataFormat));

    // Set the cooperativity level to let DirectInput know how this device
    // should interact with the system and with other DirectInput applications.
    HRESULT _hr = (*device)->SetCooperativeLevel(input_coop_hwnd(), dwFlags);
    if (FAILED(_hr) && (_hr == E_NOTIMPL))
        Msg("! INPUT: Can't set coop level. Emulation???");
    else
        R_CHK(_hr);

    // setup the buffer size for the keyboard data
    DIPROPDWORD dipdw;
    dipdw.diph.dwSize = sizeof(DIPROPDWORD);
    dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dipdw.diph.dwObj = 0;
    dipdw.diph.dwHow = DIPH_DEVICE;
    dipdw.dwData = buf_size;

    CHK_DX((*device)->SetProperty(DIPROP_BUFFERSIZE, &dipdw.diph));

    return S_OK;
}

//-----------------------------------------------------------------------

void CInput::SetAllAcquire(BOOL bAcquire) const
{
    if (pMouse)
        bAcquire ? pMouse->Acquire() : pMouse->Unacquire();
    if (pKeyboard)
        bAcquire ? pKeyboard->Acquire() : pKeyboard->Unacquire();
}

void CInput::exclusive_mode(const bool exclusive)
{
    is_exclusive_mode = exclusive;

    pKeyboard->Unacquire();
    R_CHK(pKeyboard->SetCooperativeLevel(input_coop_hwnd(), (exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND));
    pKeyboard->Acquire();

    pMouse->Unacquire();
    R_CHK(pMouse->SetCooperativeLevel(input_coop_hwnd(), (exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY));
    pMouse->Acquire();
}

//-----------------------------------------------------------------------
BOOL b_altF4 = FALSE;

void CInput::KeyUpdate()
{
    HRESULT hr;
    DWORD dwElements = KEYBOARDBUFFERSIZE;
    DIDEVICEOBJECTDATA od[KEYBOARDBUFFERSIZE];
    DWORD key = 0;

    VERIFY(pKeyboard);

    hr = pKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
    if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
    {
        hr = pKeyboard->Acquire();
        if (hr != S_OK)
            return;
        hr = pKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
        if (hr != S_OK)
            return;
    }
    u32 i = 0;
    for (i = 0; i < dwElements; i++)
    {
        key = od[i].dwOfs;
        KBState[key] = od[i].dwData & 0x80;
        if (KBState[key])
        {
            if (this->is_exclusive_mode && (key == DIK_LSHIFT || key == DIK_RSHIFT) && (this->iGetAsyncKeyState(DIK_LMENU) || this->iGetAsyncKeyState(DIK_RMENU)))
                PostMessage(gGameWindow, WM_INPUTLANGCHANGEREQUEST, 2, 0); //Переключили язык. В эксклюзивном режиме это обязательно для правильной работы функции DikToChar

            cbStack.back()->IR_OnKeyboardPress(key);
        }
        else
        {
            cbStack.back()->IR_OnKeyboardRelease(key);
        }
    }
    for (i = 0; i < COUNT_KB_BUTTONS; i++)
        if (KBState[i])
            cbStack.back()->IR_OnKeyboardHold(i);

    if (!b_altF4 && iGetAsyncKeyState(DIK_F4) && (iGetAsyncKeyState(DIK_RMENU) || iGetAsyncKeyState(DIK_LMENU)))
    {
        b_altF4 = TRUE;
        Engine.Event.Defer("KERNEL:disconnect");
        Engine.Event.Defer("KERNEL:quit");
    }
}

bool CInput::get_dik_name(int dik, LPSTR dest_str, int dest_sz) const
{
    DIPROPSTRING keyname;
    keyname.diph.dwSize = sizeof(DIPROPSTRING);
    keyname.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    keyname.diph.dwObj = static_cast<DWORD>(dik);
    keyname.diph.dwHow = DIPH_BYOFFSET;
    HRESULT hr = pKeyboard->GetProperty(DIPROP_KEYNAME, &keyname.diph);
    if (FAILED(hr))
        return false;

    const wchar_t* wct = keyname.wsz;
    if (!wcslen(wct))
        return false;

    const size_t cnt = wcstombs(dest_str, wct, dest_sz);

    return cnt != -1;
}

BOOL CInput::iGetAsyncKeyState(int dik, bool hold) const
{
    // KRodin: да-да, я знаю, что этот код ужасен.
    switch (dik)
    {
    case DIK_LMENU: return GetAsyncKeyState(VK_LMENU) & 0x8000;
    case DIK_RMENU: return GetAsyncKeyState(VK_RMENU) & 0x8000;
    case DIK_TAB: return GetAsyncKeyState(VK_TAB) & 0x8000;
    case DIK_LCONTROL: return GetAsyncKeyState(VK_LCONTROL) & 0x8000;
    case DIK_RCONTROL: return GetAsyncKeyState(VK_RCONTROL) & 0x8000;
    case DIK_DELETE: return GetAsyncKeyState(VK_DELETE) & 0x8000;
    default: {
        if (dik < COUNT_KB_BUTTONS)
        {
            return KBState[dik];
        }
        if (dik >= MOUSE_1 && dik <= MOUSE_8)
        {
            const int btn = dik - MOUSE_1;
            return iGetAsyncBtnState(btn, hold);
        }
        return FALSE; // unknown key ???
    }
    }
}

#define MOUSE_HOLD_TIME 60

BOOL CInput::iGetAsyncBtnState(int btn, bool hold) const
{
    if (mouseState[btn] && hold)
        return Device.dwTimeGlobal - mouseTime[btn] >= MOUSE_HOLD_TIME;
    return mouseState[btn];
}

void CInput::MouseUpdate()
{
#pragma push_macro("FIELD_OFFSET")
#undef FIELD_OFFSET
#define FIELD_OFFSET \
    offsetof // Фиксим warning C4644 - просто переводим макрос из винсдк на
             // использование стандартного оффсетофа.

    static u32 swapped = GetSystemMetrics(SM_SWAPBUTTON); // может проверять на каждом апдейте ?

    HRESULT hr;
    DWORD dwElements = MOUSEBUFFERSIZE;
    DIDEVICEOBJECTDATA od[MOUSEBUFFERSIZE];

    VERIFY(pMouse);

    hr = pMouse->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
    if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
    {
        hr = pMouse->Acquire();
        if (hr != S_OK)
            return;
        hr = pMouse->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
        if (hr != S_OK)
            return;
    };
    BOOL mouse_prev[COUNT_MOUSE_BUTTONS];

    for (int i = 0; i < COUNT_MOUSE_BUTTONS; i++)
        mouse_prev[i] = mouseState[i];

    auto processMouseButton = [&](int i, bool state) {
        if (state)
        {
            mouseState[i] = TRUE;
            if (mouseTime[i] == 0)
                mouseTime[i] = dwCurTime;
            cbStack.back()->IR_OnMousePress(i);
        }
        else
        {
            mouseState[i] = FALSE;
            mouseTime[i] = 0;
            cbStack.back()->IR_OnMouseRelease(i);
        }
    };

    offs[0] = offs[1] = offs[2] = 0;

    for (u32 i = 0; i < dwElements; i++)
    {
        switch (od[i].dwOfs)
        {
        case DIMOFS_X:
            offs[0] += od[i].dwData;
            timeStamp[0] = od[i].dwTimeStamp;
            break;
        case DIMOFS_Y:
            offs[1] += od[i].dwData;
            timeStamp[1] = od[i].dwTimeStamp;
            break;
        case DIMOFS_Z:
            offs[2] += od[i].dwData;
            timeStamp[2] = od[i].dwTimeStamp;
            break;

        case DIMOFS_BUTTON0:
        case DIMOFS_BUTTON1:
        case DIMOFS_BUTTON2:
        case DIMOFS_BUTTON3:
        case DIMOFS_BUTTON4:
        case DIMOFS_BUTTON5:
        case DIMOFS_BUTTON6:
        case DIMOFS_BUTTON7: {
            int btn = od[i].dwOfs - DIMOFS_BUTTON0;

            if (swapped)
            {
                if (btn == 0)
                    btn = 1;
                else if (btn == 1)
                    btn = 0;
            }

            processMouseButton(btn, od[i].dwData & 0x80);
        }break;
        }
    }

    // Giperion: double check mouse buttons state
    DIMOUSESTATE2 MouseState;
    hr = pMouse->GetDeviceState(sizeof(MouseState), &MouseState);

    auto RecheckMouseButtonFunc = [&](int i) {

        int btn = i;

        if (swapped)
        {
            if (btn == 0)
                btn = 1;
            else if (btn == 1)
                btn = 0;
        }

        if (MouseState.rgbButtons[i] & 0x80 && mouseState[btn] == FALSE)
            processMouseButton(btn, true);
        else if (!(MouseState.rgbButtons[i] & 0x80) && mouseState[btn] == TRUE)
            processMouseButton(btn, false);
    };

    if (hr == S_OK)
    {
        for (int i = 0; i < COUNT_MOUSE_BUTTONS; i++)
            RecheckMouseButtonFunc(i);
    }
    //-Giperion

    const u32 dwCurTime = Device.dwTimeGlobal;

    auto isButtonOnHold = [&](int i) {
        if (mouseState[i] && mouse_prev[i] && dwCurTime - mouseTime[i] >= MOUSE_HOLD_TIME)
            cbStack.back()->IR_OnMouseHold(i);
    };

    for (int i = 0; i < COUNT_MOUSE_BUTTONS; i++)
    {
        isButtonOnHold(i);
    }

    if (dwElements)
    {
        if (offs[0] || offs[1])
            cbStack.back()->IR_OnMouseMove(offs[0], offs[1]);
        if (offs[2])
            cbStack.back()->IR_OnMouseWheel(offs[2]);
    }
    else
    {
        if (timeStamp[1] && ((dwCurTime - timeStamp[1]) >= mouse_property.mouse_dt))
            cbStack.back()->IR_OnMouseStop(DIMOFS_Y, timeStamp[1] = 0);
        if (timeStamp[0] && ((dwCurTime - timeStamp[0]) >= mouse_property.mouse_dt))
            cbStack.back()->IR_OnMouseStop(DIMOFS_X, timeStamp[0] = 0);
    }

#pragma pop_macro("FIELD_OFFSET")
}

//-------------------------------------------------------
void CInput::iCapture(IInputReceiver* p)
{
    VERIFY(p);
    if (pMouse)
        MouseUpdate();
    if (pKeyboard)
        KeyUpdate();

    // change focus
    if (!cbStack.empty())
        cbStack.back()->IR_OnDeactivate();
    cbStack.push_back(p);
    cbStack.back()->IR_OnActivate();

    // prepare for _new_ controller
    ZeroMemory(timeStamp, sizeof(timeStamp));
    ZeroMemory(timeSave, sizeof(timeSave));
    ZeroMemory(offs, sizeof(offs));
}

void CInput::iRelease(IInputReceiver* p)
{
    if (p == cbStack.back())
    {
        cbStack.back()->IR_OnDeactivate();
        cbStack.pop_back();
        IInputReceiver* ir = cbStack.back();
        ir->IR_OnActivate();
    }
    else
    { // we are not topmost receiver, so remove the nearest one
        u32 cnt = cbStack.size();
        for (; cnt > 0; --cnt)
            if (cbStack[cnt - 1] == p)
            {
                xr_vector<IInputReceiver*>::iterator it = cbStack.begin();
                std::advance(it, cnt - 1);
                cbStack.erase(it);
                break;
            }
    }
}

void CInput::OnAppActivate(void)
{
    if (CurrentIR())
        CurrentIR()->IR_OnActivate();

    SetAllAcquire(true);
    ZeroMemory(mouseState, sizeof(mouseState));
    ZeroMemory(KBState, sizeof(KBState));
    ZeroMemory(timeStamp, sizeof(timeStamp));
    ZeroMemory(timeSave, sizeof(timeSave));
    ZeroMemory(offs, sizeof(offs));
}

void CInput::OnAppDeactivate(void)
{
    if (CurrentIR())
        CurrentIR()->IR_OnDeactivate();

    SetAllAcquire(false);
    ZeroMemory(mouseState, sizeof(mouseState));
    ZeroMemory(KBState, sizeof(KBState));
    ZeroMemory(timeStamp, sizeof(timeStamp));
    ZeroMemory(timeSave, sizeof(timeSave));
    ZeroMemory(offs, sizeof(offs));
}

void CInput::OnFrame(void)
{
    ZoneScoped;

    Device.Statistic->Input.Begin();
    dwCurTime = Device.TimerAsync_MMT();
    if (pKeyboard)
        KeyUpdate();
    if (pMouse)
        MouseUpdate();
    Device.Statistic->Input.End();
}

IInputReceiver* CInput::CurrentIR() const
{
    if (cbStack.size())
        return cbStack.back();
    else
        return nullptr;
}

u16 CInput::DikToChar(const int dik, const bool utf) const
{
    switch (dik)
    {
    // Эти клавиши через ToAscii не обработать, поэтому пропишем явно
    case DIK_NUMPAD0: return '0';
    case DIK_NUMPAD1: return '1';
    case DIK_NUMPAD2: return '2';
    case DIK_NUMPAD3: return '3';
    case DIK_NUMPAD4: return '4';
    case DIK_NUMPAD5: return '5';
    case DIK_NUMPAD6: return '6';
    case DIK_NUMPAD7: return '7';
    case DIK_NUMPAD8: return '8';
    case DIK_NUMPAD9: return '9';
    case DIK_NUMPADSLASH: return '/';
    case DIK_NUMPADPERIOD: return '.';
    //
    default:
        u8 State[256]{};
        if (this->is_exclusive_mode)
        { // GetKeyboardState в данном случае не используем, потому что оно очень глючно работает в эксклюзивном режиме
            if (this->iGetAsyncKeyState(DIK_LSHIFT) || this->iGetAsyncKeyState(DIK_RSHIFT))
                State[VK_SHIFT] = 0x80; //Для получения правильных символов при зажатом shift
        }
        else
        {
            if (!GetKeyboardState(State))
                return 0;
        }

        u16 output{};
        if (utf)
        {
            WCHAR symbol{};
            if (this->is_exclusive_mode)
            {
                auto layout = GetKeyboardLayout(GetWindowThreadProcessId(gGameWindow, nullptr));
                if (ToUnicodeEx(MapVirtualKeyEx(dik, MAPVK_VSC_TO_VK, layout), dik, State, &symbol, 1, 0, layout) != 1)
                    return 0;
            }
            else
            {
                if (ToUnicode(MapVirtualKey(dik, MAPVK_VSC_TO_VK), dik, State, &symbol, 1, 0) != 1)
                    return 0;
            }
            WideCharToMultiByte(CP_UTF8, 0, &symbol, 1, reinterpret_cast<char*>(&output), sizeof output, nullptr, nullptr);
            return output;
        }
        else
        {
            if (this->is_exclusive_mode)
            {
                auto layout = GetKeyboardLayout(GetWindowThreadProcessId(gGameWindow, nullptr));
                if (ToAsciiEx(MapVirtualKeyEx(dik, MAPVK_VSC_TO_VK, layout), dik, State, &output, 0, layout) == 1)
                    return output;
            }
            else
            {
                if (ToAscii(MapVirtualKey(dik, MAPVK_VSC_TO_VK), dik, State, &output, 0) == 1)
                    return output;
            }
        }
    }

    return 0;
}

// https://stackoverflow.com/a/36827574
void CInput::clip_cursor(bool clip)
{
    // Embedded in a host UI: never hide or trap the cursor — it belongs to the SDK,
    // which needs it for its menus, panels and the viewport gizmos.
    {
        extern bool g_ed_embedded;
        if (g_ed_embedded)
            clip = false;
    }

    if (clip)
    {
        while (ShowCursor(FALSE) >= 0)
            ;

        if (Device.m_hWnd)
        {
            RECT rect;
            GetClientRect(Device.m_hWnd, &rect);

            POINT ul;
            ul.x = rect.left;
            ul.y = rect.top;

            POINT lr;
            lr.x = rect.right;
            lr.y = rect.bottom;

            MapWindowPoints(Device.m_hWnd, nullptr, &ul, 1);
            MapWindowPoints(Device.m_hWnd, nullptr, &lr, 1);

            rect.left = ul.x;
            rect.top = ul.y;

            rect.right = lr.x;
            rect.bottom = lr.y;

            ClipCursor(&rect);
        }
    }
    else
    {
        while (ShowCursor(TRUE) < 0)
            ;
        ClipCursor(nullptr);
    }
}

#else

// No DirectInput device layer on this platform yet -- a real subsystem
// replacement (SDL2 is already written standalone as xr_input_sdl.h/.cpp,
// see platform-seam-step3-input-keys), not something a compile shim can
// stand in for. Every method below is an inert stub so CInput links and
// the rest of the engine (which only ever touches CInput through pInput)
// compiles; none of it is reachable yet since nothing constructs a CInput
// on this platform.
CInput::CInput(bool bExclusive, int) : is_exclusive_mode(bExclusive) {}
CInput::~CInput(void) {}
HRESULT CInput::CreateInputDevice(LPDIRECTINPUTDEVICE8*, GUID, const DIDATAFORMAT*, u32, u32) const { return -1; }
void CInput::SetAllAcquire(BOOL) const {}
void CInput::exclusive_mode(const bool exclusive) { is_exclusive_mode = exclusive; }
void CInput::KeyUpdate() {}
bool CInput::get_dik_name(int, LPSTR dest_str, int dest_sz) const { if (dest_sz > 0) dest_str[0] = 0; return false; }
BOOL CInput::iGetAsyncKeyState(int, bool) const { return FALSE; }
BOOL CInput::iGetAsyncBtnState(int, bool) const { return FALSE; }
void CInput::MouseUpdate() {}
void CInput::iCapture(IInputReceiver* p) { if (!cbStack.empty()) cbStack.back()->IR_OnDeactivate(); cbStack.push_back(p); cbStack.back()->IR_OnActivate(); }
void CInput::iRelease(IInputReceiver* p)
{
    for (u32 cnt = cbStack.size(); cnt > 0; --cnt)
        if (cbStack[cnt - 1] == p) { cbStack.erase(cbStack.begin() + (cnt - 1)); break; }
}
void CInput::OnAppActivate(void) {}
void CInput::OnAppDeactivate(void) {}
void CInput::OnFrame(void) {}
IInputReceiver* CInput::CurrentIR() const { return cbStack.empty() ? nullptr : cbStack.back(); }
u16 CInput::DikToChar(const int, const bool) const { return 0; }
void CInput::clip_cursor(bool) {}

#endif // _MSC_VER

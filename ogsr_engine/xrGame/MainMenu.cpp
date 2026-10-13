#include "StdAfx.h"
#include "xr_platform_seh.h"
#include "MainMenu.h"
#include "ui/UIDialogWnd.h"
#include "ui/UIMessageBoxEx.h"
#include "../xr_3da/XR_IOConsole.h"
#include "../xr_3da/IGame_Level.h"
#include "../xr_3da/CameraManager.h"
#include "xr_level_controller.h"
#include "ui/UITextureMaster.h"
#include "ui/UIXmlInit.h"
#include "xr_platform_keys.h"
#include "ui/UIBtnHint.h"
#include "UICursor.h"
#include "string_table.h"
#include "../xr_3da/DiscordRPC.hpp"
#include "object_broker.h"
#include "script_engine.h"
#include "ai_space.h"
#include "ui_rml.h"

string128 ErrMsgBoxTemplate[] = {"message_box_session_full", "msg_box_error_loading"};

CMainMenu* MainMenu() { return (CMainMenu*)g_pGamePersistent->m_pMainMenu; };
//----------------------------------------------------------------------------------
#define INIT_MSGBOX(_box, _template) \
    { \
        _box = xr_new<CUIMessageBoxEx>(); \
        _box->Init(_template); \
    }
//----------------------------------------------------------------------------------

CMainMenu::CMainMenu()
{
    m_Flags.zero();
    m_startDialog = NULL;
    m_screenshotFrame = u32(-1);
    g_pGamePersistent->m_pMainMenu = this;
    if (Device.b_is_Ready)
        OnDeviceCreate();

    ReadTextureInfo();
    CUIXmlInit::InitColorDefs();

    g_btnHint = NULL;
    m_deactivated_frame = 0;
    languageChanged = false;

    m_NeedErrDialog = ErrNoError;
    m_start_time = 0;

    g_btnHint = xr_new<CUIButtonHint>();

    for (u32 i = 0; i < u32(ErrMax); i++)
    {
        CUIMessageBoxEx* pNewErrDlg;
        INIT_MSGBOX(pNewErrDlg, ErrMsgBoxTemplate[i]);
        m_pMB_ErrDlgs.push_back(pNewErrDlg);
    }
}

CMainMenu::~CMainMenu()
{
    xr_delete(g_btnHint);
    xr_delete(m_startDialog);
    g_pGamePersistent->m_pMainMenu = NULL;
    delete_data(m_pMB_ErrDlgs);
    CurrentSound.destroy();
}

void CMainMenu::ReadTextureInfo()
{
    if (pSettings->section_exist("texture_desc"))
    {
        const LPCSTR itemsList = pSettings->r_string("texture_desc", "files");
        if (itemsList)
        {
            const int itemsCount = _GetItemCount(itemsList);

            string256 single_item;
            for (int i = 0; i < itemsCount; i++)
            {
                _GetItem(itemsList, i, single_item);
                strcat_s(single_item, ".xml");
                CUITextureMaster::ParseShTexInfo(single_item);
            }
        }
    }

    // autoload
    FS_FileSet fset;
    FS.file_list(fset, "$game_config$", FS_ListFiles, "ui\\textures_descr\\*.xml");
    FS_FileSetIt fit = fset.begin();
    FS_FileSetIt fit_e = fset.end();

    for (; fit != fit_e; ++fit)
    {
        string_path fn1, fn2, fn3;
        _splitpath((*fit).name.c_str(), fn1, fn2, fn3, 0);

        xr_strcpy(fn1, "textures_descr\\");
        xr_strcat(fn1, fn3);
        xr_strcat(fn1, ".xml");

        //xr_string fn1 = (*fit).name;

        CUITextureMaster::ParseShTexInfo(fn1);
    }
}

extern ENGINE_API BOOL bShowPauseString;

void CMainMenu::Activate(bool bActivate)
{
    // PHASE B (editor-on-Vulkan): in -vk_editor mode the viewport owns the frame;
    // never raise the game menu (boot, ESC, or disconnect).
    if (bActivate && Core.Params && strstr(Core.Params, "-vk_editor"))
        return;

    if (!!m_Flags.test(flActive) == bActivate)
        return;

    if (m_Flags.test(flGameSaveScreenshot))
        return;

    if ((m_screenshotFrame == Device.dwFrame) || (m_screenshotFrame == Device.dwFrame - 1) || (m_screenshotFrame == Device.dwFrame + 1))
        return;

    if (bActivate)
    {
        Device.Pause(TRUE, FALSE, TRUE, "mm_activate1");
        m_Flags.set(flActive | flNeedChangeCapture, TRUE);

        // ⭐The menu, and now the only one. Until 18-08 this branched on the
        // ui_new_menu gate and fell back to the mod's Lua screen (clsid
        // MAIN_MNU); the port has replaced it everywhere, so the branch, the
        // gate and CUIMMShniaga went with it. See ui_rml_menu.cpp.
        //
        // ⚠A start dialog can still EXIST here -- m_startDialog outlives a
        // deactivation, because DestroyInternal only frees it while the menu is
        // inactive (GamePersistent::OnFrame). Anything the mod raised through
        // CMainMenu (a message box, say) must go before the screen comes up, or
        // it is left underneath it, reacting to the same clicks.
        if (!UIRml::MenuOpen())
        {
            // ⚠Init's one silent refusal is "no UIRender" -- the headless case.
            // There is nothing to fall back to any more, so refuse to activate
            // rather than sit on an empty screen that eats input.
            Msg("! [rml] the menu would not start -- staying out of it");
            m_Flags.set(flActive | flNeedChangeCapture, FALSE);
            return;
        }
        xr_delete(m_startDialog);

        m_Flags.set(flRestoreConsole, Console->bVisible);

        m_Flags.set(flRestorePause, Device.Paused());

        Console->Hide();

        m_Flags.set(flRestoreCursor, GetUICursor()->IsVisible());

        m_Flags.set(flRestorePauseStr, bShowPauseString);

        if (!m_Flags.test(flRestorePause))
            Device.Pause(TRUE, TRUE, FALSE, "mm_activate2");

        bShowPauseString = FALSE;

        // ⚠Guarded: with the RmlUi menu up there is no start dialog at all, and
        // StartStopMenu on a null one is the whole cost of that.
        if (m_startDialog)
        {
            m_startDialog->m_bWorkInPause = true;
            StartStopMenu(m_startDialog, true);
        }

        if (g_pGameLevel)
        {
            Device.seqFrame.Remove(g_pGameLevel);
            Device.seqRender.Remove(g_pGameLevel);
            CCameraManager::ResetPP();
        }
        Device.seqRender.Add(this, 5); // 1-console 2-cursor 3-tutorial

        if (!g_pGameLevel)
        {
            Discord.Set_active_task_text(nullptr); //Апдейт таска должен быть выше апдейта значка уровня!

            const char* menu_status = CStringTable().translate("discord_status_mm").c_str();
            Discord.Update(!strcmp(menu_status, "discord_status_mm") ? "In main menu" : menu_status);
        }
    }
    else
    {
        m_deactivated_frame = Device.dwFrame;
        m_Flags.set(flActive, FALSE);
        m_Flags.set(flNeedChangeCapture, TRUE);

        Device.seqRender.Remove(this);

        bool b = !!Console->bVisible;
        if (b)
        {
            Console->Hide();
        }

        IR_Release();
        if (b)
        {
            Console->Show();
        }

        UIRml::MenuClose(); // no-op when it was never up
        if (m_startDialog)
            StartStopMenu(m_startDialog, true);
        CleanInternals();

        if (g_pGameLevel)
        {
            Device.seqFrame.Add(g_pGameLevel);
            Device.seqRender.Add(g_pGameLevel);
        };
        if (m_Flags.test(flRestoreConsole))
            Console->Show();

        if (!m_Flags.test(flRestorePause))
            Device.Pause(FALSE, TRUE, FALSE, "mm_deactivate1");

        bShowPauseString = m_Flags.test(flRestorePauseStr);

        if (m_Flags.test(flRestoreCursor))
            GetUICursor()->Show();

        Device.Pause(FALSE, FALSE, TRUE, "mm_deactivate2");

        if (m_Flags.test(flNeedVidRestart))
        {
            m_Flags.set(flNeedVidRestart, FALSE);
            Device.PreCache(20, false, false); // Console->Execute("vid_restart");
        }
    }
}

bool CMainMenu::IsActive() { return !!m_Flags.test(flActive); }

bool CMainMenu::CanSkipSceneRendering() { return IsActive() && !m_Flags.test(flGameSaveScreenshot); }

bool CMainMenu::IsLanguageChanged() const { return languageChanged; }
void CMainMenu::SetLanguageChanged(bool status) { languageChanged = status; }

// IInputReceiver
static int mouse_button_2_key[] = {MOUSE_1, MOUSE_2, MOUSE_3, MOUSE_4, MOUSE_5, MOUSE_6, MOUSE_7, MOUSE_8};
void CMainMenu::IR_OnMousePress(int btn)
{
    if (!IsActive())
        return;

    IR_OnKeyboardPress(mouse_button_2_key[btn]);
};

void CMainMenu::IR_OnMouseRelease(int btn)
{
    if (!IsActive())
        return;

    IR_OnKeyboardRelease(mouse_button_2_key[btn]);
};

void CMainMenu::IR_OnMouseHold(int btn)
{
    if (!IsActive())
        return;

    IR_OnKeyboardHold(mouse_button_2_key[btn]);
};

// ⚠Deltas, not a position, despite the parameter names -- CUIDialogWnd's own
// override spells it "int dx, int dy" and feeds them to the engine cursor. The
// RmlUi menu accumulates its own for the same reason the in-game documents do:
// with no legacy dialog up, nothing is driving GetUICursor().
void CMainMenu::IR_OnMouseMove(int x, int y)
{
    if (!IsActive())
        return;

    if (UIRml::MenuOnMouseDelta(x, y))
        return;

    if (MainInputReceiver())
        MainInputReceiver()->IR_OnMouseMove(x, y);
};

void CMainMenu::IR_OnMouseStop(int x, int y){};

void CMainMenu::IR_OnKeyboardPress(int dik)
{
    if (!IsActive())
        return;

    if (is_binded(kCONSOLE, dik))
    {
        Console->Show();
        return;
    }
    if (DIK_F12 == dik)
    {
        Render->Screenshot();
        return;
    }

    // ⚠After the console and the screenshot key, deliberately: while a document
    // is up the RmlUi menu answers "mine" to every key, and hooking it first
    // would take the console away -- which is the only road back when the new
    // menu itself is the thing that is broken.
    if (UIRml::MenuOnKey(dik, true))
        return;

    if (MainInputReceiver())
        MainInputReceiver()->IR_OnKeyboardPress(dik);
};

void CMainMenu::IR_OnKeyboardRelease(int dik)
{
    if (!IsActive())
        return;

    // ⭐The release half matters as much as the press: RmlUi only raises Click
    // when the same element is under the pointer at both ends.
    if (UIRml::MenuOnKey(dik, false))
        return;

    if (MainInputReceiver())
        MainInputReceiver()->IR_OnKeyboardRelease(dik);
};

void CMainMenu::IR_OnKeyboardHold(int dik)
{
    if (!IsActive())
        return;

    if (MainInputReceiver())
        MainInputReceiver()->IR_OnKeyboardHold(dik);
};

void CMainMenu::IR_OnMouseWheel(int direction)
{
    if (!IsActive())
        return;

    if (UIRml::MenuOnWheel(direction))
        return;

    if (MainInputReceiver())
        MainInputReceiver()->IR_OnMouseWheel(direction);
}

// ⚠The `b_shniaganeed_pp` term went with CUIMMShniaga (18-08): the only thing
// that ever cleared it was SetVisibleMagnifier, a Lua method on that widget, so
// with the widget gone the flag was a constant true and the term meant nothing.
bool CMainMenu::OnRenderPPUI_query() { return IsActive() && !m_Flags.test(flGameSaveScreenshot); }

extern void draw_wnds_rects();

// The catch(...) below was written for LuaJIT's raw errors -- but xrGame is
// built WITHOUT /EH, so it swallows access violations just as quietly, and an
// AV here reads as "threw [non-std, lua top: <no lua error on stack>]" with no
// stack and no dump while the menu keeps drawing every frame. Take the fault
// where it happens instead: inside __except the faulting stack is still
// standing. Same shape as CPHWorld::FrameStepGuarded, and for the same reason.
static LONG mm_render_fault_filter(_EXCEPTION_POINTERS* ep)
{
    static u32 s_faults = 0;
    const u32 n = ++s_faults;
    if (n <= 2)   // two traces is enough to see whether it is always the same site
        LogStackTrace("!![CMainMenu] menu render faulted -- stack trace:\n", ep, false);
    else if ((n & (n - 1)) == 0)
        Msg("!![CMainMenu] menu render faulted, code 0x%08x (total %u)", ep->ExceptionRecord->ExceptionCode, n);
    return EXCEPTION_EXECUTE_HANDLER;
}

void CMainMenu::OnRenderGuarded()
{
    XR_SEH_TRY
    {
        OnRenderBody();
    }
    XR_SEH_EXCEPT(mm_render_fault_filter(GetExceptionInformation()))
    {
        // This frame's menu is gone. Nothing unwinds out of a structured fault,
        // so anything that must be reconciled belongs at the top of the next
        // frame, not here.
    }
}

void CMainMenu::OnRender()
{
    if (m_Flags.test(flGameSaveScreenshot))
        return;

    // After an in-process level teardown ai().unload() restarts the Lua VM; a menu
    // dialog then raises a raw LuaJIT error EVERY frame. Letting it unwind into the
    // pureRender guard abandons the error mid-flight (illegal for LuaJIT — state
    // degrades, ends in heap corruption: AVs 16-07 02:34/14:29). Catch at this
    // boundary instead: log the actual error text and finish it like pcall would.
    try
    {
        OnRenderGuarded();
    }

    catch (const std::exception& e)
    {
        static u32 s_throws = 0;
        const u32 n = ++s_throws;
        if ((n & (n - 1)) == 0)
            Msg("!![CMainMenu::OnRender] threw [%s: %s] (total %u)", typeid(e).name(), e.what(), n);
    }
    catch (...)
    {
        lua_State* L = ai().script_engine().lua();
        const char* err = (L && lua_gettop(L) > 0 && lua_isstring(L, -1)) ? lua_tostring(L, -1) : "<no lua error on stack>";
        static u32 s_throws = 0;
        const u32 n = ++s_throws;
        if ((n & (n - 1)) == 0)
            Msg("!![CMainMenu::OnRender] threw [non-std, lua top: %s] (total %u)", err, n);
        if (L)
            lua_settop(L, 0);
    }
}


void CMainMenu::OnRenderBody()
{
    Render->Calculate();
    Render->Render();

    DoRenderDialogs();
    UI()->RenderFont();
    draw_wnds_rects();

    // ⭐LAST, and this order is not a preference. RenderFont is what actually
    // puts the legacy UI's pixels down, so anything drawn before it ends up
    // underneath -- the in-game panel spent its first day hidden behind the
    // minimap for exactly this reason, with a comment next to the call claiming
    // the opposite.
    UIRml::MenuRender();
}

void CMainMenu::OnRenderPPUI_main()
{
/*
    if (!IsActive())
        return;

    if (m_Flags.test(flGameSaveScreenshot))
        return;

    UI()->pp_start();

    if (OnRenderPPUI_query())
    {
        DoRenderDialogs();
        UI()->RenderFont();
    }

    UI()->pp_stop();
*/
}

void CMainMenu::OnRenderPPUI_PP()
{
/*
    if (!IsActive())
        return;

    if (m_Flags.test(flGameSaveScreenshot))
        return;

    UI()->pp_start();

    xr_vector<CUIWindow*>::iterator it = m_pp_draw_wnds.begin();
    for (; it != m_pp_draw_wnds.end(); ++it)
    {
        (*it)->Draw();
    }
    UI()->pp_stop();
*/
}

void CMainMenu::StartStopMenu(CUIDialogWnd* pDialog, bool bDoHideIndicators)
{
    pDialog->m_bWorkInPause = true;
    CDialogHolder::StartStopMenu(pDialog, bDoHideIndicators);
}

// pureFrame
void CMainMenu::OnFrame()
{
    if (m_Flags.test(flNeedChangeCapture))
    {
        m_Flags.set(flNeedChangeCapture, FALSE);
        if (m_Flags.test(flActive))
            IR_Capture();
        else
            IR_Release();
    }
    CDialogHolder::OnFrame();

    // ⚠This runs EVERY frame, active or not -- CDialogHolder puts itself into
    // Device.seqFrame in its constructor -- so the gate is inside MenuFrame, on
    // whether a document is up. The layout pass and the deferred button command
    // both live there, and neither may run from inside the render pass or from
    // inside an RmlUi event handler.
    if (IsActive())
        UIRml::MenuFrame();

    // screenshot stuff
    if (m_Flags.test(flGameSaveScreenshot) && Device.dwFrame > m_screenshotFrame)
    {
        m_Flags.set(flGameSaveScreenshot, FALSE);

        ::Render->Screenshot(IRender_interface::SM_FOR_GAMESAVE, m_screenshot_name);

        if (g_pGameLevel && m_Flags.test(flActive))
        {
            Device.seqFrame.Remove(g_pGameLevel);
            Device.seqRender.Remove(g_pGameLevel);
        };

        if (m_Flags.test(flRestoreConsole))
            Console->Show();
    }

    if (IsActive())
        CheckForErrorDlg();

    if (languageChanged)
    {
        languageChanged = false;
    }
}

void CMainMenu::OnDeviceCreate() {}

void CMainMenu::Screenshot(IRender_interface::ScreenshotMode mode, LPCSTR name)
{
    if (mode != IRender_interface::SM_FOR_GAMESAVE)
    {
        ::Render->Screenshot(mode, name);
    }
    else
    {
        m_Flags.set(flGameSaveScreenshot, TRUE);
        strcpy_s(m_screenshot_name, name);
        if (g_pGameLevel && m_Flags.test(flActive))
        {
            Device.seqFrame.Add(g_pGameLevel);
            Device.seqRender.Add(g_pGameLevel);
        };
        m_screenshotFrame = Device.dwFrame + 1;
        m_Flags.set(flRestoreConsole, Console->bVisible);
        Console->Hide();
    }
}

void CMainMenu::RegisterPPDraw(CUIWindow* w)
{
    UnregisterPPDraw(w);
    m_pp_draw_wnds.push_back(w);
}

void CMainMenu::UnregisterPPDraw(CUIWindow* w) { m_pp_draw_wnds.erase(std::remove(m_pp_draw_wnds.begin(), m_pp_draw_wnds.end(), w), m_pp_draw_wnds.end()); }

void CMainMenu::SetErrorDialog(EErrorDlg ErrDlg) { m_NeedErrDialog = ErrDlg; };

void CMainMenu::CheckForErrorDlg()
{
    if (m_NeedErrDialog == ErrNoError)
        return;
    StartStopMenu(m_pMB_ErrDlgs[m_NeedErrDialog], false);
    m_NeedErrDialog = ErrNoError;
};

void CMainMenu::DestroyInternal(bool bForce)
{
    if (m_startDialog && ((m_deactivated_frame < Device.dwFrame + 4) || bForce))
        xr_delete(m_startDialog);
}

void CMainMenu::OnSessionTerminate(LPCSTR reason)
{
    if (m_NeedErrDialog == SessionTerminate && (Device.dwTimeGlobal - m_start_time) < 8000)
        return;

    m_start_time = Device.dwTimeGlobal;
    string1024 Text;
    strcpy_s(Text, sizeof(Text), "Client disconnected. ");
    strcat_s(Text, sizeof(Text), reason);
    m_pMB_ErrDlgs[SessionTerminate]->SetText(Text);
    SetErrorDialog(CMainMenu::SessionTerminate);
}

void CMainMenu::SetNeedVidRestart() { m_Flags.set(flNeedVidRestart, TRUE); }

void CMainMenu::OnDeviceReset()
{
    if (IsActive() && g_pGameLevel)
        SetNeedVidRestart();
}

LPCSTR CMainMenu::GetGSVer() { return Core.GetEngineVersion(); }

void CMainMenu::PlaySound(LPCSTR path)
{
    CurrentSound.destroy();
    CurrentSound.create(path, st_Music, sg_SourceType);
    CurrentSound.play(nullptr, sm_2D);
}

#include "StdAfx.h"

#include "xr_platform_keys.h"

#include "imgui.h"

#include "../../xr_3da/xr_input.h"
#include "../../xr_3da/IGame_Persistent.h"
#include "xr_level_controller.h"

#include "embedded_editor_main.h"
#include "embedded_editor_helper.h"
#include "embedded_editor_weather.h"
#include "embedded_editor_hud.h"
#include "embedded_editor_pos_informer.h"
#include "embedded_editor_sound_env.h"

bool bShowWindow = true;

bool show_test_window = false;
bool show_weather_window = false;
bool show_hud_editor = false;
bool show_position_informer = false;
bool show_sound_env_window = false;

/*
bool show_info_window = false;
bool show_prop_window = false;
bool show_restr_window = false;
bool show_shader_window = false;
bool show_occ_window = false;
bool show_node_editor = false;
*/

enum class EditorStage
{
    None,
    Light,
    Full,

    Count,
};

EditorStage curr_stage{EditorStage::None}, target_stage{EditorStage::None};

bool IsEditorActive() { return curr_stage == EditorStage::Full; }

bool IsEditorShouldOpen()
{
    curr_stage = target_stage;

    return target_stage != EditorStage::None;
}

bool IsEditor() { return curr_stage != EditorStage::None; }

// `ui_test editor on|off` (unattended runs): what Shift+F10 does, without the key DirectInput would not take.
void EditorSetOpenForTest(bool open)
{
    target_stage = open ? EditorStage::Full : EditorStage::None;
    if (open)
        show_weather_window = true;   // a real window, so the ImGui draw data is not empty
}

void ShowMain()
{
    ImguiWnd wnd("Main");
    if (wnd.Collapsed)
        return;

    ImGui::Text("OGSR Editor");

    if (g_pGamePersistent->Environment().USED_COP_WEATHER && ImGui::Button("Weather"))
        show_weather_window = !show_weather_window;
    if (ImGui::Button("Position Informer"))
        show_position_informer = !show_position_informer;
    if (ImGui::Button("HUD Editor"))
        show_hud_editor = !show_hud_editor;
    if (ImGui::Button("Sound Env Editor"))
        show_sound_env_window = !show_sound_env_window;

    if (ImGui::Button("Test Window"))
        show_test_window = !show_test_window;

    /*
    if (ImGui::Button("Test Node Editor"))
        show_node_editor = !show_node_editor;
    */

    bool full = target_stage == EditorStage::Full;
    if (ImGui::Checkbox("Active", &full))
        target_stage = full ? EditorStage::Full : EditorStage::Light;

    float framerate = ImGui::GetIO().Framerate;

    static xr_vector<float> frames;

    	// Get frames
    if (frames.size() > 100) // Max seconds to show
    {
        for (size_t i = 1; i < frames.size(); i++)
        {
            frames[i - 1] = frames[i];
        }
        frames[frames.size() - 1] = framerate;
    }
    else
    {
        frames.push_back(framerate);
    }

    ImGui::Text("TPS %.3f ms/frame (%.1f FPS)", 1000.0f / framerate, framerate);

    ImGui::PlotHistogram("", &frames[0], frames.size(), 0, nullptr, 0.0f, 300.0f, ImVec2(300, 100));

    // A per-pass timing tree used to live here, fed by PIXEventsStatistics() from
    // the D3D11 renderer's PIX event wrapper. It went out with that renderer;
    // reinstating it means sourcing the timings from Vulkan timestamp queries.
}

void ShowEditor()
{
    ::Sound->DbgCurrentEnvPaused(false);

    if (!IsEditor())
        return;
    if (!ImGui::GetCurrentContext())   // the renderer's backend could not build one: no editor, not a crash
        return;

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = IsEditorActive();

    ShowMain();

    if (show_test_window)
    {
        ImGui::SetNextWindowPos(ImVec2(650, 20), ImGuiCond_FirstUseEver);
        ImGui::ShowDemoWindow(&show_test_window);
    }
    // if (show_node_editor)
    //{
    //     ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_FirstUseEver);
    //     if (ImGui::Begin("Example: Custom Node Graph", &show_node_editor))
    //     {
    //         ImGui::TestNodeGraphEditor();
    //     }
    //     ImGui::End();
    // }
    if (g_pGamePersistent->Environment().USED_COP_WEATHER && show_weather_window)
        ShowWeatherEditor(show_weather_window);
    if (show_position_informer)
        ShowPositionInformer(show_position_informer);
    if (show_hud_editor)
        ShowHudEditor(show_hud_editor);
    if (show_sound_env_window)
        ShowSoundEnvEditor(show_sound_env_window);
    //if (show_prop_window)
    //    ShowPropEditor(show_prop_window);
    //if (show_lua_binder)
    //    ShowLuaBinder(show_lua_binder);
    //if (show_logic_editor)
    //    ShowLogicEditor(show_logic_editor);
}

bool isRControl = false, isLControl = false, isRShift = false, isLShift = false;
extern bool GetShift();

bool Editor_KeyPress(int key)
{
    if (key == DIK_F10)
    {
        if (GetShift())
        {
            target_stage = static_cast<EditorStage>((static_cast<int>(target_stage) + 1) % static_cast<int>(EditorStage::Count));
            return true;
        }
    }

    if (!IsEditorActive())
        return false;
    if (!ImGui::GetCurrentContext())   // first frame after Shift+F10: the context comes with the next NewFrame
        return true;

    ImGuiIO& io = ImGui::GetIO();

    switch (key)
    {
    case DIK_F10: break;

    case DIK_RCONTROL:
        isRControl = true;
        io.KeyCtrl = true;
        break;
    case DIK_LCONTROL:
        isLControl = true;
        io.KeyCtrl = true;
        break;
    case DIK_RSHIFT:
        isRShift = true;
        io.KeyShift = true;
        break;
    case DIK_LSHIFT:
        isLShift = true;
        io.KeyShift = true;
        break;
    case MOUSE_1: io.MouseDown[0] = true; break;
    case MOUSE_2: io.MouseDown[1] = true; break;
    case MOUSE_3: io.MouseDown[2] = true; break;

    default:
        if (key < 512)
            io.KeysDown[key] = true;

        if (const u16 ch = pInput->DikToChar(key, false))
            io.AddInputCharacter(ch);

            // пример подключения шрифт и нормального ввода в utf
            // запишем что б не забыть, но хз надо ли оно реально

            //io.Fonts->AddFontFromFileTTF("DroidSans.ttf", 13.0f, &font_config, io.Fonts->GetGlyphRangesCyrillic());

            //wchar_t wch;
            //MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, (char*)&ch, 1, &wch, 1);
            //io.AddInputCharacter(wch);
    }
    return true;
}

bool Editor_KeyRelease(int key)
{
    // Closed editor: nothing to release into. (Also the one input path that ran while the editor was closed --
    // with r_profiler 2 the overlay may have switched ImGui's current context on the render thread right now.)
    if (!IsEditor() || !ImGui::GetCurrentContext())
        return false;
    bool active = IsEditorActive();
    ImGuiIO& io = ImGui::GetIO();

    switch (key)
    {
    case DIK_RCONTROL:
        isRControl = false;
        io.KeyCtrl = isRControl || isLControl;
        break;
    case DIK_LCONTROL:
        isLControl = false;
        io.KeyCtrl = isRControl || isLControl;
        break;
    case DIK_RSHIFT:
        isRShift = false;
        io.KeyShift = isRShift || isLShift;
        break;
    case DIK_LSHIFT:
        isLShift = false;
        io.KeyShift = isRShift || isLShift;
        break;
    case MOUSE_1: io.MouseDown[0] = false; break;
    case MOUSE_2: io.MouseDown[1] = false; break;
    case MOUSE_3: io.MouseDown[2] = false; break;
    default:
        if (key < 512)
            io.KeysDown[key] = false;
    }
    return active;
}

bool Editor_KeyHold(int key)
{
    if (!IsEditorActive())
        return false;
    return true;
}

bool Editor_MouseMove(int dx, int dy)
{
    if (!IsEditorActive())
        return false;

    if (!ImGui::GetCurrentContext())
        return true;
    // Accumulated from the relative motion the input layer reports, not read from the OS cursor: in a level the game
    // holds the mouse captured, so GetCursorPos stays put (and is in screen, not client, coordinates anyway).
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos.x = std::clamp(io.MousePos.x + float(dx), 0.f, std::max(io.DisplaySize.x - 1.f, 0.f));
    io.MousePos.y = std::clamp(io.MousePos.y + float(dy), 0.f, std::max(io.DisplaySize.y - 1.f, 0.f));
    return true;
}

static int s_direction;

bool Editor_MouseWheel(int direction)
{
    if (!IsEditorActive())
        return false;

    s_direction = direction;

    //ImGuiIO& io = ImGui::GetIO();
    //io.MouseWheel += direction > 0 ? +1.0f : -1.0f;
    return true;
}

void Editor_OnFrame()
{
    if (s_direction != 0 && ImGui::GetCurrentContext())
    {       
        ImGuiIO& io = ImGui::GetIO();
        io.MouseWheel += s_direction > 0 ? +1.0f : -1.0f;
        s_direction = 0;
    }
}
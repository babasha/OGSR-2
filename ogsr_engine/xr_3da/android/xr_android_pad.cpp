// Android: the pad state behind nx/xr_nx_pad.h (the same plain C interface the Switch's libnx HID backend fills),
// so CInput::PollNxPad (xr_input_sdl.cpp) turns it into keyboard+mouse events with the Switch's scheme unchanged.
//
// Source: the first SDL game controller. SDL pumps it inside SDL_PollEvent, which CInput::OnFrame calls just
// before PollNxPad, so the values read here are this frame's.
//
// Buttons go by POSITION, not by label: the Switch scheme was designed on Nintendo's layout (A east, B south,
// X north, Y west), and SDL names buttons by the Xbox layout (A south, B east, X west, Y north). So the east
// button is XRPAD_A (use) on every pad, the south one XRPAD_B (jump), the west one XRPAD_Y (reload), the north
// one XRPAD_X (weapon wheel) -- which is also where an Xbox-layout shooter puts them.
#include <SDL2/SDL.h>
#include <stdlib.h>

#include "../nx/xr_nx_pad.h"

namespace
{
SDL_GameController* s_pad = nullptr;
Uint32 s_nextScanMs = 0;

void open_first_controller()
{
    const Uint32 now = SDL_GetTicks();
    if (now < s_nextScanMs)
        return;
    s_nextScanMs = now + 1000; // re-scan once a second while nothing is connected
    for (int i = 0, n = SDL_NumJoysticks(); i < n; ++i)
    {
        if (!SDL_IsGameController(i))
            continue;
        if ((s_pad = SDL_GameControllerOpen(i)))
        {
            SDL_Log("[pad] game controller %d: %s", i, SDL_GameControllerName(s_pad));
            return;
        }
    }
}

int16_t axis(SDL_GameControllerAxis a, bool flip)
{
    const int v = SDL_GameControllerGetAxis(s_pad, a); // -32768..32767, +y = down
    const int c = v < -32767 ? -32767 : v;
    return int16_t(flip ? -c : c);
}
} // namespace

extern "C" void xr_nx_pad_init(void)
{
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0"); // positions, see the header
    open_first_controller();
}

extern "C" void xr_nx_pad_poll(xr_nx_pad_state* out)
{
    *out = {};
    if (s_pad && !SDL_GameControllerGetAttached(s_pad))
    {
        SDL_Log("[pad] game controller disconnected");
        SDL_GameControllerClose(s_pad);
        s_pad = nullptr;
    }
    if (!s_pad)
        open_first_controller();
    if (!s_pad)
        return;

    struct Map { SDL_GameControllerButton b; uint32_t bit; };
    static constexpr Map kMap[] = {
        {SDL_CONTROLLER_BUTTON_B, XRPAD_A},             {SDL_CONTROLLER_BUTTON_A, XRPAD_B},
        {SDL_CONTROLLER_BUTTON_Y, XRPAD_X},             {SDL_CONTROLLER_BUTTON_X, XRPAD_Y},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, XRPAD_LSTICK}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, XRPAD_RSTICK},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XRPAD_L},  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XRPAD_R},
        {SDL_CONTROLLER_BUTTON_START, XRPAD_PLUS},      {SDL_CONTROLLER_BUTTON_BACK, XRPAD_MINUS},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, XRPAD_DLEFT}, {SDL_CONTROLLER_BUTTON_DPAD_UP, XRPAD_DUP},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XRPAD_DRIGHT}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, XRPAD_DDOWN},
    };
    for (const Map& m : kMap)
        if (SDL_GameControllerGetButton(s_pad, m.b))
            out->buttons |= m.bit;
    // Analog triggers -> the Switch's digital ZL/ZR, past a third of the travel.
    constexpr int kTrigger = 10000;
    if (SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTrigger)
        out->buttons |= XRPAD_ZL;
    if (SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTrigger)
        out->buttons |= XRPAD_ZR;

    out->lx = axis(SDL_CONTROLLER_AXIS_LEFTX, false);
    out->ly = axis(SDL_CONTROLLER_AXIS_LEFTY, true); // the pad state is +y = up
    out->rx = axis(SDL_CONTROLLER_AXIS_RIGHTX, false);
    out->ry = axis(SDL_CONTROLLER_AXIS_RIGHTY, true);
}

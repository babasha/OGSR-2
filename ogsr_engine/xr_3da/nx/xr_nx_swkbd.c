// Switch-only: the console's own keyboard (the swkbd applet) for an edit box of a script dialog (13-10-2026).
//
// The pad has no keys to type with: pressing A on an <input> of a script dialog (ui_rml_inv.cpp, dialog_key) asks this
// for the text instead. The applet takes the screen and blocks the calling thread until the player is done -- the game
// stands still meanwhile, which is what the legacy edit box's focus meant too. Text goes in and out as UTF-8.
// In a C file: <switch.h> collides with the engine's types in C++ TUs.
#include <switch.h>

#include <string.h>

// 1: the player confirmed (out holds the text), 0: cancelled or the applet is not available (applet mode).
int xr_nx_text_input(const char* initial, const char* guide, int numeric, int max_len, char* out, unsigned out_size)
{
    if (!out || out_size == 0)
        return 0;
    out[0] = 0;

    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return 0;
    if (numeric)
    {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    }
    else
        swkbdConfigMakePresetDefault(&kbd);
    if (initial && *initial)
        swkbdConfigSetInitialText(&kbd, initial);
    if (guide && *guide)
        swkbdConfigSetGuideText(&kbd, guide);
    if (max_len > 0)
        swkbdConfigSetStringLenMax(&kbd, (u32)max_len);
    const Result rc = swkbdShow(&kbd, out, out_size);
    swkbdClose(&kbd);
    if (R_FAILED(rc))
    {
        out[0] = 0;
        return 0;
    }
    out[out_size - 1] = 0;
    return 1;
}

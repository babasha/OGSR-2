// Horizon HID (libnx) behind a plain C interface. Kept out of C++ translation units on purpose:
// <switch.h> defines Result/Handle/bool-ish types and macros that collide with the engine's.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Own button bits, independent of libnx's HidNpadButton_* values.
enum
{
    XRPAD_A = 1u << 0,
    XRPAD_B = 1u << 1,
    XRPAD_X = 1u << 2,
    XRPAD_Y = 1u << 3,
    XRPAD_LSTICK = 1u << 4,
    XRPAD_RSTICK = 1u << 5,
    XRPAD_L = 1u << 6,
    XRPAD_R = 1u << 7,
    XRPAD_ZL = 1u << 8,
    XRPAD_ZR = 1u << 9,
    XRPAD_PLUS = 1u << 10,
    XRPAD_MINUS = 1u << 11,
    XRPAD_DLEFT = 1u << 12,
    XRPAD_DUP = 1u << 13,
    XRPAD_DRIGHT = 1u << 14,
    XRPAD_DDOWN = 1u << 15,
};

typedef struct xr_nx_pad_state
{
    uint32_t buttons;    // XRPAD_* bits currently held (any connected pad, handheld included)
    int16_t lx, ly;      // left stick,  -32767..32767, +y = up
    int16_t rx, ry;      // right stick, same
    uint32_t touch_count;        // fingers on the panel; the first four are below (the Switch fills two)
    uint32_t touch_id[4];        // the panel's finger ids: the same finger keeps its id while it stays down
    int32_t touch_x[4], touch_y[4]; // in the 1280x720 panel space
    int32_t quit;             // 1 once Horizon asks the title to exit (HOME > Close, or the applet loop ended)
    int32_t pad_attached;     // 1 while a real pad is there (Android: a game controller; 0 on the Switch, unused)
} xr_nx_pad_state;

void xr_nx_pad_init(void);
void xr_nx_pad_poll(xr_nx_pad_state* out);

#ifdef __cplusplus
}
#endif

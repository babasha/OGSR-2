#include "stdafx.h"

#ifdef DEBUG
ECORE_API BOOL bDebug = FALSE;
#endif

// Video
u32 psCurrentVidMode[2] = {1024, 768};

// release version always has "mt_*" enabled
// ⚠rsFullscreen belongs in the DEFAULTS, not just in the enum: the flag decides
// the window geometry on the very first Create, long before user.ltx is read, and
// the shipped behaviour is borderless-fullscreen. Leaving it clear here would
// start every fresh install in a 1024x768 window.
Flags32 psDeviceFlags{rsFullscreen | rsDetails | /*rsDrawStatic | rsDrawDynamic |*/ rsExclusiveMode | rsAlwaysActive | rs_SSFX_HUD_RAINDROPS};

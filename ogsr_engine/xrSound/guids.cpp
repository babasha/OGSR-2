
// Real DirectSound is dead code here: the only other GUID/property-ID
// consumers in this module (SoundRender_Core*.cpp, SoundRender_Environment.cpp)
// go through eax.h's portable OPENAL branch (see AdditionalOptions -DOPENAL
// on non-MSVC), which never touches <dsound.h> at all -- this file is the
// sole real Windows dependency in the whole xrSound module, kept
// byte-identical on MSVC.
#ifdef _MSC_VER

#define INITGUID
#include <dsound.h>
#include <eax.h>

#else

// Off MSVC the same file still has to be the one TU that DEFINES the EAX property-set
// GUIDs the OPENAL branch references (SoundRender_Core*.cpp) — eax.h emits definitions
// instead of extern declarations under INITGUID. Without it: undefined reference to
// DSPROPSETID_EAX20_ListenerProperties at the final link.
#include "stdafx.h"
#define INITGUID
#include <eax.h>

#endif // _MSC_VER

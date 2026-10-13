// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#include "../xrCore/xrCore.h"

#ifdef _MSC_VER
// mmsystem.h
#define MMNOSOUND
#define MMNOMIDI
#define MMNOAUX
#define MMNOMIXER
#define MMNOJOY
#include <mmsystem.h>

// mmreg.h
#define NOMMIDS
#define NONEWRIFF
#define NOJPEGDIB
#define NONEWIC
#define NOBITMAP
#include <mmreg.h>
#endif // _MSC_VER
// non-MSVC: only WAVEFORMATEX/WAVE_FORMAT_PCM are actually used out of these
// two headers (see xr_platform_win32_shim.h) -- no real waveOut*/mixer API
// calls anywhere in xrSound.

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#include "../xrCDB/xrCDB.h"
#include "Sound.h"

#include "../xrCore/xr_resource.h"

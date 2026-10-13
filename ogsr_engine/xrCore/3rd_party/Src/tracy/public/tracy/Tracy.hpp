#pragma once
#if !defined(TRACY_ENABLE)
#define TracyZoneScoped
#define TracyZoneScopedC(...)
#define TracyZoneScopedN(...)
#define TracyZoneScopedNC(...)
#define FrameMark
#define FrameMarkNamed(...)
#define FrameMarkStart(...)
#define FrameMarkEnd(...)
#else
#error Tracy enabled but header is a stub
#endif

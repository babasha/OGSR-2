#pragma once

#pragma warning(disable : 4995)
#pragma warning(disable : 4577)
#include "../xr_3da/stdafx.h"
#pragma warning(default : 4995)
#pragma warning(4 : 4018)
#pragma warning(4 : 4244)
#pragma warning(disable : 4505)

#include "../xr_3da/ai_script_space.h" //KRodin: пусть луа для рендера и скриптов инитится в одном месте.

#define THROW VERIFY
#define THROW2 VERIFY2
#define THROW3 VERIFY3

#include "../xr_3da/GameFont.h"
#include "../xr_3da/xr_object.h"
#include "../xr_3da/IGame_Level.h"

#include "../xrCore/_vector3d_ext.h"


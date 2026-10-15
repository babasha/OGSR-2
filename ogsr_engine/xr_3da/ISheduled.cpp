#include "stdafx.h"
#include "xrSheduler.h"
#include "xr_object.h"

ISheduled::ISheduled()
{
    shedule.t_min = 20;
    shedule.t_max = 1000;
    shedule.b_locked = FALSE;
}

ISheduled::~ISheduled()
{
    // sad, but true
    // we need this to become MASTER_GOLD
    Engine.Sheduler.Unregister(this);
}

void ISheduled::shedule_register() { Engine.Sheduler.Register(this); }

void ISheduled::shedule_unregister(bool force) { Engine.Sheduler.Unregister(this, force); }

void ISheduled::shedule_Update(u32 dt)
{
}

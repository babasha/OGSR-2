#include "StdAfx.h"
#include "ai_monster_motion_stats.h"
#include "basemonster/base_monster.h"

void CMotionStats::update()
{
    elem _new;
    _new.speed = pMonster->control().movement().velocity_current();
    _new.position = pMonster->Position();
    _new.time = pMonster->m_dwCurrentTime;
    _data[index] = _new;

    // обновить значение index
    if ((index + 1) >= MAX_ELEMS)
    {
        for (u32 i = 0; i < (MAX_ELEMS - 1); i++)
        {
            _data[i] = _data[i + 1];
        }
    }
    else
        index++;
}


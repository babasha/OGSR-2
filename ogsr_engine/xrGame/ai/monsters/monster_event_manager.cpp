#include "StdAfx.h"
#include "monster_event_manager.h"

CMonsterEventManager::CMonsterEventManager() {}

CMonsterEventManager::~CMonsterEventManager() { clear(); }

void CMonsterEventManager::raise(EEventType event, IEventData* data)
{
    EVENT_MAP_IT it = m_event_storage.find(event);
    if (it == m_event_storage.end())
        return;

    for (EVENT_VECTOR_IT I = it->second.begin(); I != it->second.end(); I++)
    {
        if (!I->need_remove)
            (I->delegate)(data);
    }

    EVENT_VECTOR_IT it_del = std::remove_if(it->second.begin(), it->second.end(), pred_remove());
    it->second.erase(it_del, it->second.end());
}

void CMonsterEventManager::clear()
{
    for (EVENT_MAP_IT I_map = m_event_storage.begin(); I_map != m_event_storage.end(); I_map++)
    {
        I_map->second.clear();
    }

    m_event_storage.clear();
}

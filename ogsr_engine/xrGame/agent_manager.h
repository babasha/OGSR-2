////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_manager.h
//	Created 	: 24.05.2004
//  Modified 	: 24.05.2004
//	Author		: Dmitriy Iassenev
//	Description : Agent manager
////////////////////////////////////////////////////////////////////////////

#pragma once

class CAgentCorpseManager;
class CAgentEnemyManager;
class CAgentExplosiveManager;
class CAgentLocationManager;
class CAgentMemberManager;
class CAgentMemoryManager;
class CAgentManagerPlanner;


class CAgentManager
{
private:
    CAgentCorpseManager* m_corpse;
    CAgentEnemyManager* m_enemy;
    CAgentExplosiveManager* m_explosive;
    CAgentLocationManager* m_location;
    CAgentMemberManager* m_member;
    CAgentMemoryManager* m_memory;
    CAgentManagerPlanner* m_brain{};

private:
    u32 m_last_update_time;
    u32 m_update_rate;

private:
    void init_scheduler();
    void init_components();
    void remove_components();
    void update_impl();

public:
    CAgentManager();
    // final class, no virtual destructor needed
    ~CAgentManager();
    void update();
    shared_str cName() const;
    void remove_links(CObject* object);

public:
    IC CAgentCorpseManager& corpse() const;
    IC CAgentEnemyManager& enemy() const;
    IC CAgentExplosiveManager& explosive() const;
    IC CAgentLocationManager& location() const;
    IC CAgentMemberManager& member() const;
    IC CAgentMemoryManager& memory() const;
    IC CAgentManagerPlanner& brain() const;
};

#include "agent_manager_inline.h"
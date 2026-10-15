////////////////////////////////////////////////////////////////////////////
//	Module 		: item_manager.cpp
//	Created 	: 27.12.2003
//  Modified 	: 27.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Item manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "npc_perf.h"
#include "item_manager.h"
#include "inventory_item.h"
#include "CustomMonster.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "restricted_object.h"
#include "movement_manager.h"
#include "ai_space.h"
#include "space_restriction_manager.h"
#include "Level.h"


bool CItemManager::is_useful(const CGameObject* object) const { return (m_object->useful(this, object)); }

// Квар из console_commands.cpp. Дефолт 0 -- обоснование в item_manager.h.
int g_mem_item_access_cache = 0;

bool CItemManager::accessible_cached(const CGameObject* object) const
{
    const u32 vertex_id = object->ai_location().level_vertex_id();
    const Fvector& position = object->Position();

    if (!g_mem_item_access_cache)
        return (m_object->movement().restrictions().accessible(position) && m_object->movement().restrictions().accessible(vertex_id));

    const u32 epoch = Level().space_restriction_manager().epoch();
    const u16 id = object->ID();

    auto I = m_access_cache.find(id);
    if ((I != m_access_cache.end()) && ((*I).second.epoch == epoch) && ((*I).second.vertex_id == vertex_id) && (*I).second.position.similar(position, EPS_L))
        return ((*I).second.ok);

    const bool ok = m_object->movement().restrictions().accessible(position) && m_object->movement().restrictions().accessible(vertex_id);

    SAccess& entry = m_access_cache[id];
    entry.vertex_id = vertex_id;
    entry.position = position;
    entry.epoch = epoch;
    entry.ok = ok;
    return (ok);
}

bool CItemManager::useful(const CGameObject* object) const
{
    {
        NPC_ZONE(U_BASE);
        if (!inherited::is_useful(object))
            return (false);

        if (m_object->getDestroy())
            return (false);

        // we do not want to keep in memory attached objects
        if (m_object->H_Parent())
            return (false);
    }

    {
        NPC_ZONE(U_AILOC);
        if (!const_cast<CGameObject*>(object)->UsedAI_Locations())
            return (false);
    }

    {
        NPC_ZONE(U_ACCESS_POS);
        if (!accessible_cached(object))
            return (false);
    }

    {
        NPC_ZONE(U_INVITEM);
        const CInventoryItem* inventory_item = smart_cast<const CInventoryItem*>(object);
        if (!inventory_item || !inventory_item->useful_for_NPC())
            return (false);
    }

    {
        NPC_ZONE(U_GRAPH);
        if (!ai().get_level_graph() || !ai().level_graph().valid_vertex_id(object->ai_location().level_vertex_id()))
            return (false);
    }

    return (true);
}

float CItemManager::do_evaluate(const CGameObject* object) const
{
    VERIFY3(m_object->movement().restrictions().accessible(object->ai_location().level_vertex_id()), *m_object->cName(), *object->cName());
    return (m_object->evaluate(this, object));
}

float CItemManager::evaluate(const CGameObject* object) const
{
    const CInventoryItem* inventory_item = smart_cast<const CInventoryItem*>(object);
    VERIFY(inventory_item);
    VERIFY(inventory_item->useful_for_NPC());
    return (1000000.f - (float)inventory_item->Cost());
}

void CItemManager::update()
{
    START_PROFILE("Memory Manager/items::update")

    inherited::update();

    VERIFY3(!selected() || m_object->movement().restrictions().accessible(selected()->ai_location().level_vertex_id()), *m_object->cName(),
            selected() ? *selected()->cName() : "<no selected item>");

    STOP_PROFILE
}

void CItemManager::remove_links(CObject* object)
{
    m_access_cache.erase(u16(object->ID()));
    // since we use no members in CGameObject during search,
    // we just use the pinter itself, we can just statically cast object
    OBJECTS::iterator I = std::find(m_objects.begin(), m_objects.end(), (CGameObject*)object);
    if (I != m_objects.end())
        m_objects.erase(I);

    if (m_selected && (m_selected->ID() == object->ID()))
        m_selected = 0;
}

void CItemManager::on_restrictions_change()
{
    m_access_cache.clear();
    if (!m_selected)
        return;

    if (!m_object->movement().restrictions().accessible(m_selected->ai_location().level_vertex_id()))
    {
        m_selected = 0;
        return;
    }

    if (m_object->movement().restrictions().accessible(m_selected->Position()))
        return;

    m_selected = 0;
}

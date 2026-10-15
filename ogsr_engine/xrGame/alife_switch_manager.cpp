////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_switch_manager.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator switch manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_switch_manager.h"
#include "xrServer_Objects_ALife.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "alife_schedule_registry.h"
#include "game_level_cross_table.h"
#include "xrServer.h"
#include "ai_space.h"
#include "level_graph.h"

using namespace ALife;

struct remove_non_savable_predicate
{
    xrServer* m_server;

    IC remove_non_savable_predicate(xrServer* server)
    {
        VERIFY(server);
        m_server = server;
    }

    IC bool operator()(const ALife::_OBJECT_ID& id) const
    {
        CSE_Abstract* object = m_server->game->get_entity_from_eid(id);
        VERIFY(object);
        CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(object);
        VERIFY(alife_object);
        return (!alife_object->can_save());
    }
};

CALifeSwitchManager::~CALifeSwitchManager() {}

void CALifeSwitchManager::add_online(CSE_ALifeDynamicObject* object, bool update_registries)
{
    START_PROFILE("ALife/switch/add_online")
    VERIFY((ai().game_graph().vertex(object->m_tGraphID)->level_id() == graph().level().level_id()));

    object->m_bOnline = true;

    NET_Packet tNetPacket;
    CSE_Abstract* l_tpAbstract = smart_cast<CSE_Abstract*>(object);
    server().entity_Destroy(l_tpAbstract);
    object->s_flags.Or(M_SPAWN_UPDATE);
    ClientID clientID;
    clientID.set(server().GetServerClient() ? server().GetServerClient()->ID.value() : 0);
    server().Process_spawn(tNetPacket, clientID, FALSE, l_tpAbstract);
    object->s_flags.And(u16(-1) ^ M_SPAWN_UPDATE);

    if (object->used_ai_locations() && !ai().level_graph().valid_vertex_id(object->m_tNodeID))
    {
        Msg("Trying to correct invalid vertex %u for object %s", object->m_tNodeID, object->name_replace());
        object->m_tNodeID = ai().level_graph().vertex_id(object->m_tNodeID, object->o_Position);
        Msg("  new vertex: %u", object->m_tNodeID);
    }
    ASSERT_FMT(!object->used_ai_locations() || ai().level_graph().valid_vertex_id(object->m_tNodeID), "Invalid vertex %u for object %s", object->m_tNodeID, object->name_replace());

    object->add_online(update_registries);
    STOP_PROFILE
}

void CALifeSwitchManager::remove_online(CSE_ALifeDynamicObject* object, bool update_registries)
{
    START_PROFILE("ALife/switch/remove_online")
    object->m_bOnline = false;

    m_saved_chidren = object->children;
    CSE_ALifeTraderAbstract* inventory_owner = smart_cast<CSE_ALifeTraderAbstract*>(object);
    CSE_InventoryBox* inventory_box = smart_cast<CSE_InventoryBox*>(object);
    CSE_InventoryContainer* inventory_cont = smart_cast<CSE_InventoryContainer*>(object);

    if (inventory_owner || inventory_box || inventory_cont)
    {
        m_saved_chidren.erase(std::remove_if(m_saved_chidren.begin(), m_saved_chidren.end(), remove_non_savable_predicate(&server())), m_saved_chidren.end());
    }

    server().Perform_destroy(object, net_flags(TRUE, TRUE));
    VERIFY(object->children.empty());

    _OBJECT_ID object_id = object->ID;
    object->ID = server().PerformIDgen(object_id);

    object->add_offline(m_saved_chidren, update_registries);
    STOP_PROFILE
}

void CALifeSwitchManager::switch_online(CSE_ALifeDynamicObject* object)
{
    START_PROFILE("ALife/switch/switch_online")
    object->switch_online();
    STOP_PROFILE
}

void CALifeSwitchManager::switch_offline(CSE_ALifeDynamicObject* object)
{
    START_PROFILE("ALife/switch/switch_offline")
    object->switch_offline();
    STOP_PROFILE
}

// ---------------------------------------------------------------------------
// HOW MANY OBJECTS ONE SWEEP MAY SEND OFFLINE BY DISTANCE (console `al_offline_budget`)
//
// An NPC going offline is destroyed on the client together with its whole
// inventory in ONE frame: ownership rejects with on_item_drop in Lua, then
// every destroyed object is relcased against every object of the level.
// 📏09-10 Switch, walking away from the Cordon camp: 16-19 frames of 22-31 ms
// frame_move, ~3 s in a row, two NPCs with ~15 items each per sweep.
//
// The weight is the object plus its children (an inventory owner and what it
// carries), a monster group counts its members. The first switch of a sweep
// always goes, so a heavy NPC cannot be stuck online; the rest wait for the
// next sweep (al_switch_period, 100 ms). Forced offline (can_switch_online()
// false) is not budgeted. 0 = no budget, as it was.
// ---------------------------------------------------------------------------
int g_al_offline_budget = 0;

namespace
{
u32 s_off_switched = 0, s_off_objects = 0, s_off_deferred = 0, s_off_last_ms = 0;
}

bool CALifeSwitchManager::offline_budget_take(u32 weight)
{
    if (g_al_offline_budget <= 0)
        return true;

    const bool ok = !m_offline_spent || m_offline_spent + weight <= (u32)g_al_offline_budget;
    if (ok)
    {
        m_offline_spent += weight;
        ++s_off_switched;
        s_off_objects += weight;
    }
    else
        ++s_off_deferred;

    const u32 now = Device.dwTimeContinual;
    if (!s_off_last_ms)
        s_off_last_ms = now;
    if (now - s_off_last_ms >= 10000)
    {
        if (s_off_deferred)
            Msg("~ [al offline] 10 s: %u switched offline by distance (%u objects with inventories), %u deferred to a later sweep (budget %d)", s_off_switched,
                s_off_objects, s_off_deferred, g_al_offline_budget);
        s_off_switched = s_off_objects = s_off_deferred = 0;
        s_off_last_ms = now;
    }
    return ok;
}

bool CALifeSwitchManager::synchronize_location(CSE_ALifeDynamicObject* I)
{
    START_PROFILE("ALife/switch/synchronize_location")

    // check if we do not use ai locations
    if (!I->used_ai_locations())
        return (true);

    // check if we are not attached
    if (0xffff != I->ID_Parent)
        return (true);

    // check if we are not online and have an invalid level vertex id
    if (!I->m_bOnline && !ai().level_graph().valid_vertex_id(I->m_tNodeID))
        return (true);

    return ((*I).synchronize_location());
    STOP_PROFILE
}

void CALifeSwitchManager::try_switch_online(CSE_ALifeDynamicObject* I)
{
    START_PROFILE("ALife/switch/try_switch_online")
    // so, the object is offline
    // checking if the object is not attached
    if (0xffff != I->ID_Parent)
    {
        // so, object is attached
        // checking if parent is offline too
        return;
    }

    VERIFY2((ai().game_graph().vertex(I->m_tGraphID)->level_id() != ai().level_graph().level_id()) || !Level().Objects.net_Find(I->ID) || Level().Objects.dump_all_objects(),
            make_string("frame [%d] time [%d] object [%s] with id [%d] is offline, but is on the level", Device.dwFrame, Device.dwTimeGlobal, I->name_replace(), I->ID));

    I->try_switch_online();

    if (!I->m_bOnline && !I->keep_saved_data_anyway())
        I->client_data.clear();

    STOP_PROFILE
}

void CALifeSwitchManager::try_switch_offline(CSE_ALifeDynamicObject* I)
{
    START_PROFILE("ALife/switch/try_switch_offline")
    // checking if the object is not attached
    if (0xffff != I->ID_Parent)
    {
        return;
    }

    I->try_switch_offline();
    STOP_PROFILE
}

void CALifeSwitchManager::switch_object(CSE_ALifeDynamicObject* I)
{
    if (I->redundant())
    {
        release(I);
        return;
    }

    if (!synchronize_location(I))
        return;

    if (I->m_bOnline)
        try_switch_offline(I);
    else
        try_switch_online(I);

    if (I->redundant())
        release(I);
}

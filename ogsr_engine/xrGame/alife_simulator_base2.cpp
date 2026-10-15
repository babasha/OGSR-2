////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_simulator_base2.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator base class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_simulator_base.h"
#include "relation_registry.h"
#include "alife_registry_wrappers.h"
#include "xrServer_Objects_ALife_Items.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "alife_story_registry.h"
#include "alife_schedule_registry.h"
#include "alife_smart_terrain_registry.h"
#include "alife_group_registry.h"

using namespace ALife;

// Where the loading of a saved game actually goes: 1792 objects, 437 ms, all of it
// in this function. Split it once, in place — the parts are unrelated registries
// and guessing which one costs 250 us per object has no reason to be right.
namespace ALifeRegProf
{
float s_before = 0.f, s_add = 0.f, s_graph = 0.f, s_sched = 0.f, s_story = 0.f, s_smart = 0.f,
      s_groups = 0.f, s_setup = 0.f, s_item = 0.f, s_onreg = 0.f;
u32   s_calls = 0;

void Dump()
{
    Msg("[load step]   register_object x%u: on_before %.0f | objects.add %.0f | graph.update %.0f | scheduled %.0f | story %.0f | smart_terrains %.0f | groups %.0f | setup_simulator %.0f | item attach %.0f | on_register %.0f ms",
        s_calls, s_before, s_add, s_graph, s_sched, s_story, s_smart, s_groups, s_setup, s_item, s_onreg);
    s_before = s_add = s_graph = s_sched = s_story = s_smart = s_groups = s_setup = s_item = s_onreg = 0.f;
    s_calls = 0;
}
}   // namespace ALifeRegProf

void CALifeSimulatorBase::register_object(CSE_ALifeDynamicObject* object, bool add_object)
{
    CTimer _t; _t.Start();
    auto lap = [&_t] { const float v = _t.GetElapsed_ms_total(); _t.Start(); return v; };
    ++ALifeRegProf::s_calls;

    object->on_before_register();
    ALifeRegProf::s_before += lap();

    if (add_object)
        objects().add(object);
    ALifeRegProf::s_add += lap();

    graph().update(object);
    ALifeRegProf::s_graph += lap();
    scheduled().add(object);
    ALifeRegProf::s_sched += lap();
    story_objects().add(object->m_story_id, object);
    ALifeRegProf::s_story += lap();
    smart_terrains().add(object);
    ALifeRegProf::s_smart += lap();
    groups().add(object);
    ALifeRegProf::s_groups += lap();

    setup_simulator(object);
    ALifeRegProf::s_setup += lap();

    CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(object);
    if (item && item->attached())
    {
        // ⚠⚠The parent is looked up with the ASSERTING overload, and in a
        // release build that assert is compiled out (THROW2 -> VERIFY2 -> empty,
        // xrGame/StdAfx.h). An unregistered parent therefore does not fail here:
        // object() returns null, and the two lines below dereference it. That is
        // an access violation with no message in front of it, and it killed the
        // server during the first live session (28-07-2026) -- an item was given
        // to a player body that xrServer knows about but ALife does not, and the
        // whole process went down inside vector::push_back with nothing in the
        // log to say which object was to blame.
        //
        // The parent being absent is a real, reachable state, not a corruption:
        // every entity in xrServer::entities is NOT necessarily an ALife object
        // (see Perform_spawn_net_actor). So it is refused here, loudly, and the
        // item stays parentless rather than taking the process with it.
        CSE_ALifeDynamicObject* II = objects().object(item->base()->ID_Parent, true);
        if (!II)
        {
            Msg("!![LSS] item [%s][%d] names parent [%d], which is NOT an ALife object -- registered UNATTACHED", item->base()->name_replace(), item->base()->ID,
                item->base()->ID_Parent);
            item->base()->ID_Parent = 0xffff;
        }
        else
        {
            II->children.push_back(item->base()->ID);
            II->attach(item, true, false);
        }
    }
    ALifeRegProf::s_item += lap();

    if (can_register_objects())
        object->on_register();
    ALifeRegProf::s_onreg += lap();
}

void CALifeSimulatorBase::unregister_object(CSE_ALifeDynamicObject* object, bool alife_query)
{
    object->on_unregister();

    CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(object);
    if (item && item->attached())
    {
        // Same asserting lookup, same release-build silence, and here it is
        // dereferenced TWICE on one line -- see the note in register_object.
        CSE_ALifeDynamicObject* parent = objects().object(item->base()->ID_Parent, true);
        if (!parent)
            Msg("!![LSS] item [%s][%d] is detaching from parent [%d], which is not an ALife object -- nothing to detach from", item->base()->name_replace(), item->base()->ID,
                item->base()->ID_Parent);
        else
            graph().detach(*parent, item, parent->m_tGraphID, alife_query);
    }

    objects().remove(object->ID);
    story_objects().remove(object->m_story_id);
    smart_terrains().remove(object);
    groups().remove(object);

    if (!object->m_bOnline)
    {
        graph().remove(object, object->m_tGraphID);
        scheduled().remove(object);
    }
    else if (object->ID_Parent == 0xffff)
    {
        //			if (object->used_ai_locations())
        graph().level().remove(object, !object->used_ai_locations());
    }
}

void CALifeSimulatorBase::on_death(CSE_Abstract* killed, CSE_Abstract* killer)
{
    typedef CSE_ALifeOnlineOfflineGroup::MEMBER GROUP_MEMBER;

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(killed);
    if (creature)
        creature->on_death(killer);

    GROUP_MEMBER* member = smart_cast<GROUP_MEMBER*>(killed);
    if (!member)
        return;

    if (member->m_group_id == 0xffff)
        return;

    groups().object(member->m_group_id).notify_on_member_death(member);
}

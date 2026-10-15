////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_schedule_registry.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife schedule registry
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "safe_map_iterator.h"
#include "xrServer_Objects_ALife.h"
#include "ai_debug.h"
#include "alife_slow_probe.h"


class CALifeScheduleRegistry : public CSafeMapIterator<ALife::_OBJECT_ID, CSE_ALifeSchedulable, std::less<ALife::_OBJECT_ID>, false>
{
private:
    struct CUpdatePredicate
    {
        u32 m_count;
        mutable u32 m_current;
        u64 m_t0;

        IC CUpdatePredicate(const u32& count)
        {
            m_count = count;
            m_current = 0;
            m_t0 = CPU::QPC();
        }

        IC bool operator()(_iterator& i, u64 cycle_count, bool) const
        {
            if ((*i).second->m_schedule_counter == cycle_count)
                return (false);

            if (m_current >= m_count)
                return (false);

            // ⭐БЮДЖЕТ ПАЧКИ (`al_sched_budget_us`, 0 = как было). system.ltx
            // objects_per_update = 10 -- это ШТУКИ, а не время: 📏Switch v155,
            // смарты Тёмной долины проверяются в одном апдейте (lair_6 5 мс +
            // lair_8 1.5 + lair_1 1.3 ...) и пачка выходит 8-10 мс одним куском.
            // Хотя бы один объект за апдейт берётся всегда, остальные -- пока
            // не истёк бюджет; итератор встаёт на следующем, и очередь
            // продолжается со следующего апдейта.
            if (m_current > 0 && alife_slow::sched_budget_us > 0 &&
                double(CPU::QPC() - m_t0) * 1e6 >= double(alife_slow::sched_budget_us) * double(CPU::QPCFreq()))
            {
                alife_slow::note_budget_cut(m_current);
                return (false);
            }

            ++m_current;
            (*i).second->m_schedule_counter = cycle_count;

            return (true);
        }

        IC void operator()(_iterator& i, u64 cycle_count) const
        {
            START_PROFILE("ALife/scheduled/update")
            alife_slow::item_begin((*i).second->base());
            (*i).second->update();
            alife_slow::item_end(alife_slow::phase_scheduled);
            STOP_PROFILE
        }
    };

protected:
    typedef CSafeMapIterator<ALife::_OBJECT_ID, CSE_ALifeSchedulable, std::less<ALife::_OBJECT_ID>, false> inherited;

protected:
    u32 m_objects_per_update;

public:
    IC CALifeScheduleRegistry();
    virtual ~CALifeScheduleRegistry();
    void add(CSE_ALifeDynamicObject* object);
    void remove(CSE_ALifeDynamicObject* object, bool no_assert = false);
    IC void update();
    IC CSE_ALifeSchedulable* object(const ALife::_OBJECT_ID& id, bool no_assert = false) const;
    IC const u32& objects_per_update() const;
    IC void objects_per_update(const u32& objects_per_update);
};

#include "alife_schedule_registry_inline.h"
////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker_cover.cpp
//	Created 	: 25.04.2006
//  Modified 	: 25.04.2006
//	Author		: Dmitriy Iassenev
//	Description :
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "ai_stalker.h"
#include "cover_point.h"
#include "../../cover_evaluators.h"
#include "../../ai_space.h"
#include "../../cover_manager.h"
#include "../../stalker_movement_restriction.h"
#include "level_graph.h"
#include "../../memory_manager.h"
#include "../../enemy_manager.h"
#include "../../ai_position_query.h"
#include "../../agent_manager.h"
#include "../../agent_member_manager.h"
#include "../../member_order.h"
#include "../../stalker_combat_actions.h"
#include "../../stalker_movement_manager.h"

extern const float MIN_SUITABLE_ENEMY_DISTANCE = 3.f;

#ifdef DEBUG
static int g_advance_search_count = 0;
static int g_near_cover_search_count = 0;
static int g_far_cover_search_count = 0;
#endif

void CAI_Stalker::subscribe_on_best_cover_changed(const on_best_cover_changed_delegate& delegate)
{
    VERIFY(m_cover_delegates.end() == std::find(m_cover_delegates.begin(), m_cover_delegates.end(), delegate));
    m_cover_delegates.push_back(delegate);
}

void CAI_Stalker::unsubscribe_on_best_cover_changed(const on_best_cover_changed_delegate& delegate)
{
    cover_delegates::iterator I = std::find(m_cover_delegates.begin(), m_cover_delegates.end(), delegate);
    VERIFY(I != m_cover_delegates.end());
    m_cover_delegates.erase(I);
}

void CAI_Stalker::on_best_cover_changed(const CCoverPoint* new_cover, const CCoverPoint* old_cover)
{
    cover_delegates::const_iterator I = m_cover_delegates.begin();
    cover_delegates::const_iterator E = m_cover_delegates.end();
    for (; I != E; ++I)
        (*I)(new_cover, old_cover);
}

bool stalker_siege_inside(u16 enemy_id, const Fvector& position);

const CCoverPoint* CAI_Stalker::find_best_cover(const Fvector& position_to_cover_from)
{
    // not inside the house the enemy is kept in: the stock pick found the wall
    // between him and us there, a log wall away from him
    const CEntityAlive* enemy = memory().enemy().selected();
    const u16 enemy_id = enemy ? enemy->ID() : u16(-1);
    const auto outside = [enemy_id](const CCoverPoint* cover) { return enemy_id == u16(-1) || !stalker_siege_inside(enemy_id, cover->position()); };
#ifdef DEBUG
//	Msg									("* [%6d][%s] search for new cover performed",Device.dwTimeGlobal,*cName());
#endif
#ifdef DEBUG
    ++g_near_cover_search_count;
#endif
    m_ce_best->setup(position_to_cover_from, MIN_SUITABLE_ENEMY_DISTANCE, 170.f, MIN_SUITABLE_ENEMY_DISTANCE, outside);
    const CCoverPoint* point = ai().cover_manager().best_cover(Position(), 10.f, *m_ce_best, CStalkerMovementRestrictor(this, true));
    if (point)
        return (point);

#ifdef DEBUG
    ++g_far_cover_search_count;
#endif
    m_ce_best->setup(position_to_cover_from, 10.f, 170.f, 10.f, outside);
    point = ai().cover_manager().best_cover(Position(), 30.f, *m_ce_best, CStalkerMovementRestrictor(this, true));
    return (point);
}

float CAI_Stalker::best_cover_value(const Fvector& position_to_cover_from)
{
    m_ce_best->setup(position_to_cover_from, MIN_SUITABLE_ENEMY_DISTANCE, 170.f, MIN_SUITABLE_ENEMY_DISTANCE);
    m_ce_best->initialize(Position(), true);
    m_ce_best->evaluate(m_best_cover, CStalkerMovementRestrictor(this, true).weight(m_best_cover));
    return (m_ce_best->best_value());
}

void CAI_Stalker::best_cover_can_try_advance()
{
    if (!m_best_cover_actual)
        return;

    if (m_best_cover_advance_cover == m_best_cover)
        return;

    m_best_cover_can_try_advance = true;
}

void CAI_Stalker::update_best_cover_actuality(const Fvector& position_to_cover_from)
{
    if (!m_best_cover_actual)
        return;

    if (!m_best_cover)
        return;

    if (m_best_cover->position().distance_to_sqr(position_to_cover_from) < _sqr(MIN_SUITABLE_ENEMY_DISTANCE))
    {
        m_best_cover_actual = false;
#if 0 // def DEBUG
		Msg								("* [%6d][%s] enemy too close",Device.dwTimeGlobal,*cName());
#endif
        return;
    }

    float cover_value = best_cover_value(position_to_cover_from);
    if (cover_value >= m_best_cover_value + 1.f)
    {
        m_best_cover_actual = false;
#if 0 // def DEBUG
		Msg								("* [%6d][%s] cover became too bad",Device.dwTimeGlobal,*cName());
#endif
        return;
    }

    //	if (cover_value >= 1.5f*m_best_cover_value) {
    //		m_best_cover_actual				= false;
    //		Msg								("* [%6d][%s] cover became too bad2",Device.dwTimeGlobal,*cName());
    //		return;
    //	}

    if (!m_best_cover_can_try_advance)
        return;

    if (m_best_cover_advance_cover == m_best_cover)
        return;

    m_best_cover_advance_cover = m_best_cover;
    m_best_cover_can_try_advance = false;

#ifdef DEBUG
//	Msg									("* [%6d][%s] advance search performed",Device.dwTimeGlobal,*cName());
#endif
#ifdef DEBUG
    ++g_advance_search_count;
#endif
    m_ce_best->setup(position_to_cover_from, MIN_SUITABLE_ENEMY_DISTANCE, 170.f, MIN_SUITABLE_ENEMY_DISTANCE);
    m_best_cover = ai().cover_manager().best_cover(Position(), 10.f, *m_ce_best, CStalkerMovementRestrictor(this, true));
}

// ⭐ai_tactics_v2: the cover through the position query. Event-driven like the
// stock -- asked when the cover stopped being actual (enemy changed, a danger
// there, it got a point worse, the house he hides in, a try to get nearer) --
// and answered a frame later: meanwhile the old one stands. What it adds to the
// stock: a cover must hide him crouched from where the enemy would shoot (a
// ray, not only the map's cover value), let him look out at him, be outside
// the house the enemy is kept in, and apart from the squad's places.
constexpr u32 COVER_ASK_EVERY = 1000; // a cover gone bad is asked about again at most this often
constexpr u32 COVER_ASK_EVERY_NONE = 250; // ...having none at all
constexpr u32 COVER_ADVANCE_EVERY = 3000; // "is there one nearer?" at most this often

void CAI_Stalker::ask_cover(const Fvector& position_to_cover_from, float radius)
{
    using namespace PositionQuery;
    SRequest r;
    r.kind = eCover;
    r.npc = u16(ID());
    r.self = Position();
    r.threat = position_to_cover_from;
    r.near_ = MIN_SUITABLE_ENEMY_DISTANCE + 1.f;
    r.ideal = 15.f;
    r.far_ = 170.f;
    if (const CEntityAlive* enemy = memory().enemy().selected())
    {
        SStalkerDbgSiege siege;
        if (stalker_dbg_siege(enemy->ID(), siege) && siege.indoors)
        {
            r.house = true;
            r.house_origin = siege.origin;
            r.house_radius = 12.f;
        }
    }
    for (const CMemberOrder* member : agent_manager().member().members())
    {
        if (&member->object() == this)
            continue;
        // where he is going, if on his way; else where he is
        CAI_Stalker& other = member->object();
        const u32 dest = other.movement().level_dest_vertex_id();
        r.friends.push_back(!other.movement().path_completed() && ai().level_graph().valid_vertex_id(dest) ? ai().level_graph().vertex_position(dest) : other.Position());
        if (const CCoverPoint* cover = member->cover())
            r.friends.push_back(cover->position());
    }
    r.spacing = 2.5f;
    GatherCovers(r, *this, Position(), radius, 48);
    Issue(std::move(r));
}

void CAI_Stalker::adopt_cover(const CCoverPoint* cover, const Fvector& position_to_cover_from)
{
    if (cover != m_best_cover)
    {
        on_best_cover_changed(cover, m_best_cover);
        m_best_cover = cover;
        m_best_cover_advance_cover = 0;
        m_best_cover_can_try_advance = false;
    }
    // the squad's bookkeeping of who holds which cover, as the stock pick does it
    if (cover)
        CStalkerMovementRestrictor(this, true).finalize(cover);
    m_best_cover_value = m_best_cover ? best_cover_value(position_to_cover_from) : flt_max;
}

const CCoverPoint* CAI_Stalker::best_cover_v2(const Fvector& position_to_cover_from)
{
    using namespace PositionQuery;
    SResult answer;
    if (Poll(u16(ID()), eCover, answer))
        adopt_cover(answer.found ? answer.point.cover : nullptr, position_to_cover_from);

    // the stock's reasons for a cover to stop being good, without its search
    if (m_best_cover_actual && m_best_cover)
    {
        const CEntityAlive* enemy = memory().enemy().selected();
        if (m_best_cover->position().distance_to_sqr(position_to_cover_from) < _sqr(MIN_SUITABLE_ENEMY_DISTANCE))
            m_best_cover_actual = false;
        else if (best_cover_value(position_to_cover_from) >= m_best_cover_value + 1.f)
            m_best_cover_actual = false;
        else if (enemy && stalker_siege_inside(enemy->ID(), m_best_cover->position()))
        {
            // not a second more towards it: dropped now, asked again soon
            adopt_cover(nullptr, position_to_cover_from);
            m_best_cover_actual = false;
        }
        else if (m_best_cover_can_try_advance && m_best_cover_advance_cover != m_best_cover && !Pending(u16(ID()), eCover) &&
                 Device.dwTimeGlobal > LastAsked(u16(ID()), eCover) + COVER_ADVANCE_EVERY)
        {
            // nearer, if there is better near: the answer replaces it or not
            m_best_cover_advance_cover = m_best_cover;
            m_best_cover_can_try_advance = false;
            ask_cover(position_to_cover_from, 10.f);
        }
    }

    // ⚠At most this often: 📏06-10 18:38, up to 2042 asks a fight -- the enemy's
    // remembered spot moves with every sound and the cover falls a point "worse"
    // with it; the stock re-searched on each, synchronously. Without a cover at
    // all, sooner.
    if (!m_best_cover_actual && !Pending(u16(ID()), eCover) &&
        Device.dwTimeGlobal > LastAsked(u16(ID()), eCover) + (m_best_cover ? COVER_ASK_EVERY : COVER_ASK_EVERY_NONE))
    {
        ask_cover(position_to_cover_from, 20.f);
        m_best_cover_actual = true;
    }
    return m_best_cover;
}

const CCoverPoint* CAI_Stalker::best_cover(const Fvector& position_to_cover_from)
{
    if (PositionQuery::g_ai_tactics_v2)
        return best_cover_v2(position_to_cover_from);

    update_best_cover_actuality(position_to_cover_from);

    // picked before the house was being kept, and it is inside it
    if (m_best_cover_actual && m_best_cover && memory().enemy().selected() && stalker_siege_inside(memory().enemy().selected()->ID(), m_best_cover->position()))
        m_best_cover_actual = false;

    if (m_best_cover_actual)
        return (m_best_cover);

    m_best_cover_actual = true;

    const CCoverPoint* best_cover = find_best_cover(position_to_cover_from);
    if (best_cover != m_best_cover)
    {
        on_best_cover_changed(best_cover, m_best_cover);
        m_best_cover = best_cover;
        m_best_cover_advance_cover = 0;
        m_best_cover_can_try_advance = false;
    }
    m_best_cover_value = m_best_cover ? best_cover_value(position_to_cover_from) : flt_max;

    return (m_best_cover);
}

void CAI_Stalker::on_restrictions_change()
{
    inherited::on_restrictions_change();
    m_best_cover_actual = false;
#ifdef DEBUG
    Msg("* [%6d][%s] on_restrictions_change", Device.dwTimeGlobal, *cName());
#endif
}

void CAI_Stalker::on_enemy_change(const CEntityAlive* enemy)
{
    inherited::on_enemy_change(enemy);
    m_item_actuality = false;
    m_best_cover_actual = false;
#ifdef DEBUG
//	Msg									("* [%6d][%s] on_enemy_change",Device.dwTimeGlobal,*cName());
#endif
}

void CAI_Stalker::on_danger_location_add(const CDangerLocation& location)
{
    if (!m_best_cover)
        return;

    if (m_best_cover->position().distance_to_sqr(location.position()) <= _sqr(location.m_radius))
    {
#ifdef DEBUG
//		Msg								("* [%6d][%s] on_danger_add",Device.dwTimeGlobal,*cName());
#endif
        m_best_cover_actual = false;
    }
}

void CAI_Stalker::on_danger_location_remove(const CDangerLocation& location)
{
    if (!m_best_cover)
    {
        if (Position().distance_to_sqr(location.position()) <= _sqr(location.m_radius))
        {
#ifdef DEBUG
//			Msg							("* [%6d][%s] on_danger_remove",Device.dwTimeGlobal,*cName());
#endif
            m_best_cover_actual = false;
        }

        return;
    }

    if (m_best_cover->position().distance_to_sqr(location.position()) <= _sqr(location.m_radius))
    {
#ifdef DEBUG
//		Msg								("* [%6d][%s] on_danger_remove",Device.dwTimeGlobal,*cName());
#endif
        m_best_cover_actual = false;
    }
}

void CAI_Stalker::on_cover_blocked(const CCoverPoint* cover)
{
#ifdef DEBUG
//	Msg									("* [%6d][%s] cover is blocked",Device.dwTimeGlobal,*cName());
#endif
    m_best_cover_actual = false;
}

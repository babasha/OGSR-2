////////////////////////////////////////////////////////////////////////////
//	Module 		: space_restriction_manager.h
//	Created 	: 17.08.2004
//  Modified 	: 27.08.2004
//	Author		: Dmitriy Iassenev
//	Description : Space restriction manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "space_restriction_holder.h"
#include "alife_space.h"
#include "level_graph.h"
#include "space_restriction.h"

namespace RestrictionSpace
{
struct CTimeIntrusiveBase;
};

template <typename _1, typename _2>
class intrusive_ptr;

class CSpaceRestrictionManager : public CSpaceRestrictionHolder
{
    friend class CLevelGraph;

    struct CClientRestriction;

protected:
    typedef intrusive_ptr<CSpaceRestriction, RestrictionSpace::CTimeIntrusiveBase> CRestrictionPtr;
    typedef xr_map<shared_str, CSpaceRestriction*> SPACE_RESTRICTIONS;
    typedef xr_map<ALife::_OBJECT_ID, CClientRestriction> CLIENT_RESTRICTIONS;

private:
protected:
    SPACE_RESTRICTIONS m_space_restrictions;
    CLIENT_RESTRICTIONS* m_clients;
    // ⭐НОМЕР ПОКОЛЕНИЯ ОГРАНИЧЕНИЙ. Растёт на ЛЮБОМ изменении
    // состава или геометрии ограничителей. Нужен потребителям, которые
    // кэшируют ответ accessible(): полагаться на on_restrictions_change() НЕЛЬЗЯ --
    // ⚠add_border/remove_border меняют геометрию и его НЕ зовут.
    u32 m_epoch{1};

    // restrict()'s string work (the level's default restrictors merged in, both lists normalized) by its four inputs --
    // equal shared_str are the same pointer. The inputs are held, so a pointer is never reused while it is a key.
    struct RestrictMemo
    {
        shared_str out, in, def_out, def_in;
        shared_str out_norm, in_norm, key;
    };
    xr_vector<RestrictMemo> m_restrict_memo;

public:
    using CSpaceRestrictionHolder::restriction;

protected:
    IC void join_restrictions(shared_str& restrictions, shared_str update);
    IC void difference_restrictions(shared_str& restrictions, shared_str update);
    /*IC*/ CRestrictionPtr restriction(ALife::_OBJECT_ID id);
    CRestrictionPtr restriction(shared_str out_restrictors, shared_str in_restrictors);
    IC void collect_garbage();
    virtual void on_default_restrictions_changed();

public:
    CSpaceRestrictionManager();
    virtual ~CSpaceRestrictionManager();
    void restrict(ALife::_OBJECT_ID id, shared_str out_restrictors, shared_str in_restrictors);
    void unrestrict(ALife::_OBJECT_ID id);
    void add_restrictions(ALife::_OBJECT_ID id, shared_str out_restrictions, shared_str in_restrictions);
    void remove_restrictions(ALife::_OBJECT_ID id, shared_str out_restrictions, shared_str in_restrictions);
    void change_restrictions(ALife::_OBJECT_ID id, shared_str add_out_restrictions, shared_str add_in_restrictions, shared_str remove_out_restrictions,
                             shared_str remove_in_restrictions);
    void clear();

    template <typename T1, typename T2>
    IC void add_border(ALife::_OBJECT_ID id, T1 p1, T2 p2);
    void remove_border(ALife::_OBJECT_ID id);

    shared_str in_restrictions(ALife::_OBJECT_ID id);
    shared_str out_restrictions(ALife::_OBJECT_ID id);

    shared_str base_in_restrictions(ALife::_OBJECT_ID id);
    shared_str base_out_restrictions(ALife::_OBJECT_ID id);

    bool accessible(ALife::_OBJECT_ID id, const Fsphere& sphere);
    bool accessible(ALife::_OBJECT_ID id, u32 level_vertex_id, float radius);
    u32 accessible_nearest(ALife::_OBJECT_ID id, const Fvector& position, Fvector& result);

    /*IC*/ bool restriction_presented(shared_str restrictions, shared_str restriction) const;

    IC const SPACE_RESTRICTIONS& restrictions() const;
    IC u32 epoch() const { return m_epoch; }
};

#include "space_restriction_manager_inline.h"
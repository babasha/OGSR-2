////////////////////////////////////////////////////////////////////////////
//	Module 		: restricted_object.h
//	Created 	: 18.08.2004
//  Modified 	: 23.08.2004
//	Author		: Dmitriy Iassenev
//	Description : Restricted object
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "restriction_space.h"

#include "alife_space.h"

class CSE_Abstract;
class CCustomMonster;
class CGameObject;

template <bool add>
struct CRestrictionPredicate;

class CRestrictedObject
{
    friend struct CRestrictionPredicate<true>;
    friend struct CRestrictionPredicate<false>;

private:
    typedef CGameObject inherited;

private:
    CCustomMonster* m_object;
    mutable bool m_applied;
    mutable bool m_removed;
    bool m_actual;
    bool m_lifted{};
    shared_str m_lifted_out, m_lifted_in;

protected:
    IC void add_object_restriction(ALife::_OBJECT_ID id, const RestrictionSpace::ERestrictorTypes& restrictor_type);
    IC void remove_object_restriction(ALife::_OBJECT_ID id, const RestrictionSpace::ERestrictorTypes& restrictor_type);

public:
    IC CRestrictedObject(CCustomMonster* object);
    virtual ~CRestrictedObject();
    virtual BOOL net_Spawn(CSE_Abstract* data);
    virtual void net_Destroy();
    void add_border(u32 start_vertex_id, float radius) const;
    void add_border(const Fvector& start_position, const Fvector& dest_position) const;
    void add_border(u32 start_vertex_id, u32 dest_vertex_id) const;
    void remove_border() const;
    u32 accessible_nearest(const Fvector& position, Fvector& result) const;
    bool accessible(const Fvector& position) const;
    bool accessible(const Fvector& position, float radius) const;
    bool accessible(u32 level_vertex_id) const;
    bool accessible(u32 level_vertex_id, float radius) const;
    void add_restrictions(const xr_vector<ALife::_OBJECT_ID>& out_restrictions, const xr_vector<ALife::_OBJECT_ID>& in_restrictions);
    void remove_restrictions(const xr_vector<ALife::_OBJECT_ID>& out_restrictions, const xr_vector<ALife::_OBJECT_ID>& in_restrictions);
    void add_restrictions(const shared_str& out_restrictions, const shared_str& in_restrictions);
    void remove_restrictions(const shared_str& out_restrictions, const shared_str& in_restrictions);
    void remove_all_restrictions(const RestrictionSpace::ERestrictorTypes& restrictor_type);
    void remove_all_restrictions();
    shared_str in_restrictions() const;
    shared_str out_restrictions() const;
    shared_str base_in_restrictions() const;
    shared_str base_out_restrictions() const;
    // ⭐A fight lifts the restrictors the object was given -- the camp a
    // stalker keeps to, which held a whole village to one side of a house the
    // player sat in -- and puts them back once it is over, so he walks home.
    // The level's default ones, the anomaly fields everybody keeps out of,
    // stay. Client side only: a save keeps what the server has, as before.
    void lift_restrictions();
    void restore_restrictions();
    bool restrictions_lifted() const { return m_lifted; }
    // given while lifted (a logic section switching): lifted too, put back with the rest
    bool restrictions_regiven() const;
    IC bool applied() const;
    IC CCustomMonster& object() const;
    IC bool actual() const;
    void actual(bool value);

public:
#ifdef DEBUG
    IC void initialize();
#endif
};

#include "restricted_object_inline.h"
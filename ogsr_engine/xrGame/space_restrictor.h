////////////////////////////////////////////////////////////////////////////
//	Module 		: space_restrictor.h
//	Created 	: 17.08.2004
//  Modified 	: 17.08.2004
//	Author		: Dmitriy Iassenev
//	Description : Space restrictor
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "GameObject.h"
#include "restriction_space.h"
#include "../xr_3da/Feel_Touch.h"

class CSpaceRestrictor : public CGameObject, public Feel::Touch
{
private:
    typedef CGameObject inherited;

private:
    enum
    {
        PLANE_COUNT = 6,
    };

private:
    typedef Fplane CPlanesArray[PLANE_COUNT];

private:
    struct CPlanes
    {
        CPlanesArray m_planes;
    };

private:
    typedef xr_vector<Fsphere> SPHERES;
    typedef xr_vector<CPlanes> BOXES;

private:
    mutable SPHERES m_spheres;
    mutable BOXES m_boxes;
    Fsphere m_selfbounds{};
    bool m_actuality{false};

private:
    u8 m_space_restrictor_type;

private:
    IC void actual(bool value) { m_actuality = value; }
    void prepare();
    bool prepared_inside(const Fsphere& sphere) const;

public:
    IC CSpaceRestrictor();
    virtual ~CSpaceRestrictor();
    virtual BOOL net_Spawn(CSE_Abstract* data);
    virtual void net_Destroy();
    bool inside(const Fsphere& sphere);
    virtual void Center(Fvector& C) const;
    virtual float Radius() const;
    virtual BOOL UsedAI_Locations();
    virtual void spatial_move();
    virtual void OnEvent(NET_Packet& P, u16 type);

    // ⭐Replace the whole shape with one sphere of this radius, live.
    //
    // Everything downstream of the collision form recomputes itself from it:
    // Radius()/Center() read CFORM directly, prepare() rebuilds the plane and
    // sphere caches, the feel-touch ball is re-read from CFORM every scheduled
    // update, and a zone's effective radius is a fraction of Radius(). So the
    // one thing that has to happen here is that the form changes and the object
    // is told to re-derive -- hence spatial_move(), which is also what marks the
    // caches stale.
    //
    // ⚠Deliberately a REPLACEMENT rather than a scale. A shape may hold several
    // spheres and boxes the SDK authored around a piece of geometry, and scaling
    // that arrangement produces something nobody designed; saying "this anomaly
    // is now a ball of radius R" is a statement an operator can predict.
    void Reshape(float radius);

    IC bool actual() const;
    virtual CSpaceRestrictor* cast_restrictor() { return this; }
    virtual bool register_schedule() const { return false; }

    IC RestrictionSpace::ERestrictorTypes restrictor_type() const;
    IC void change_restrictor_type(RestrictionSpace::ERestrictorTypes);

    virtual void OnRender();

private:
    bool b_scheduled;

public:
    void ScheduleRegister();
    void ScheduleUnregister();
    IC bool IsScheduled() { return b_scheduled; }

    virtual void net_Relcase(CObject*);
    virtual void shedule_Update(u32);
    virtual void feel_touch_new(CObject*);
    virtual void feel_touch_delete(CObject*);
    virtual BOOL feel_touch_contact(CObject*);
    bool active_contact(u16) const;
    float distance_to(Fvector&);
};

#include "space_restrictor_inline.h"

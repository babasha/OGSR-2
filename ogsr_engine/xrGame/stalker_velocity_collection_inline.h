////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_velocity_collection_inline.h
//	Created 	: 23.12.2005
//  Modified 	: 23.12.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker velocity collection inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

inline float CStalkerVelocityCollection::velocity(const MonsterSpace::EMentalState& mental_state, const MonsterSpace::EBodyState& body_state,
                                                  const MonsterSpace::EMovementType& movement_type, const MonsterSpace::EMovementDirection& movement_direction) const
{
    VERIFY(movement_type != MonsterSpace::eMovementTypeStand);

    switch (mental_state)
    {
    case MonsterSpace::eMentalStateDanger: {
        return (m_danger[body_state][movement_type][movement_direction]);
    }
    case MonsterSpace::eMentalStateFree: {
        VERIFY(body_state == MonsterSpace::eBodyStateStand);
        VERIFY(movement_direction == MonsterSpace::eMovementDirectionForward);
        return (m_free[movement_type]);
    }
    case MonsterSpace::eMentalStatePanic: {
        VERIFY(body_state == MonsterSpace::eBodyStateStand);
        VERIFY(movement_type == MonsterSpace::eMovementTypeRun);
        VERIFY(movement_direction == MonsterSpace::eMovementDirectionForward);
        return (m_panic);
    };
    default: NODEFAULT;
    };
}

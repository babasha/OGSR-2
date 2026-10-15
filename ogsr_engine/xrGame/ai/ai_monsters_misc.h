
//	Module 		: ai_monsters_misc.cpp
//	Created 	: 23.07.2002
//  Modified 	: 23.07.2002
//	Author		: Dmitriy Iassenev
//	Description : Miscellanious routines for monsters
////////////////////////////////////////////////////////////////////////////

#pragma once

class CBaseFunction;
class CEntity;
class CEntityAlive;

namespace GroupHierarchyHolder
{
typedef xr_vector<CEntity*> MEMBER_REGISTRY;
};

extern bool bfGetActionSuccessProbability(GroupHierarchyHolder::MEMBER_REGISTRY& Members, const xr_set<const CEntityAlive*>& VisibleEnemies, float fMinProbability,
                                          CBaseFunction& fSuccessProbabilityFunction);
extern u32 dwfChooseAction(u32 dwActionRefreshRate, float fMinProbability0, float fMinProbability1, float fMinProbability2, float fMinProbability3, u32 dwTeam, u32 dwSquad,
                           u32 dwGroup, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, CEntity* tpEntity = 0, float fGroupDistance = 100.f);

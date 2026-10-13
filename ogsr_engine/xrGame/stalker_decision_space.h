////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_decision_space.h
//	Created 	: 30.03.2004
//  Modified 	: 30.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker decision space
////////////////////////////////////////////////////////////////////////////

#pragma once

namespace StalkerDecisionSpace
{
enum EWorldProperties
{
    eWorldPropertyAlive = u32(0),
    eWorldPropertyDead,
    eWorldPropertyAlreadyDead,

    eWorldPropertyALife,
    eWorldPropertyPuzzleSolved,

    eWorldPropertySmartTerrainTask,
    eWorldPropertyItems,
    eWorldPropertyEnemy,
    eWorldPropertyDanger,

    eWorldPropertyItemToKill,
    eWorldPropertyFoundItemToKill,
    eWorldPropertyItemCanKill,
    eWorldPropertyFoundAmmo,
    eWorldPropertyReadyToKill,
    eWorldPropertyReadyToDetour,
    eWorldPropertySeeEnemy,
    eWorldPropertyEnemySeeMe,
    eWorldPropertyPanic,
    eWorldPropertyInCover,
    eWorldPropertyLookedOut,
    eWorldPropertyPositionHolded,
    eWorldPropertyEnemyDetoured,
    eWorldPropertyUseSuddenness,
    eWorldPropertyPureEnemy,
    eWorldPropertyUseCrouchToLookOut,
    eWorldPropertyEnemyWounded,
    eWorldPropertyWoundedEnemyReached,
    eWorldPropertyWoundedEnemyPrepared,
    eWorldPropertyPlayerOnThePath,
    eWorldPropertyCriticallyWounded,
    eWorldPropertyEnemyCriticallyWounded,
    eWorldPropertyWoundedEnemyAimed,
    eWorldPropertyPausedAfterKill,
    eWorldPropertyKilledWounded,

    eWorldPropertyDangerUnknown,
    eWorldPropertyDangerInDirection,
    eWorldPropertyDangerGrenade,
    eWorldPropertyDangerBySound,

    eWorldPropertyCoverActual,
    eWorldPropertyCoverReached,
    eWorldPropertyLookedAround,
    eWorldPropertyGrenadeExploded,

    eWorldPropertyAnomaly,
    eWorldPropertyInsideAnomaly,

    eWorldPropertyScript,
    eWorldPropertyDummy = u32(-1),
};

enum EWorldOperators
{
    // death
    eWorldOperatorDead = u32(0),
    eWorldOperatorDying,

    // alife
    eWorldOperatorGatherItems,
    eWorldOperatorALifeEmulation,
    eWorldOperatorSmartTerrainTask,

    // alife : tasks
    eWorldOperatorSolveZonePuzzle,
    eWorldOperatorReachTaskLocation,
    eWorldOperatorAccomplishTask,
    eWorldOperatorReachCustomerLocation,
    eWorldOperatorCommunicateWithCustomer,

    // anomaly
    eWorldOperatorGetOutOfAnomaly,
    eWorldOperatorDetectAnomaly,

    // combat
    eWorldOperatorGetItemToKill,
    eWorldOperatorFindItemToKill,
    eWorldOperatorMakeItemKilling,
    eWorldOperatorFindAmmo,

    eWorldOperatorAimEnemy,
    eWorldOperatorGetReadyToKill,
    eWorldOperatorGetReadyToDetour,
    eWorldOperatorKillEnemy,
    eWorldOperatorRetreatFromEnemy,
    eWorldOperatorTakeCover,
    eWorldOperatorLookOut,
    eWorldOperatorHoldPosition,
    eWorldOperatorGetDistance,
    eWorldOperatorDetourEnemy,
    eWorldOperatorSearchEnemy,
    eWorldOperatorHideFromGrenade,
    eWorldOperatorSuddenAttack,
    eWorldOperatorKillEnemyIfNotVisible,
    eWorldOperatorReachWoundedEnemy,
    eWorldOperatorAimWoundedEnemy,
    eWorldOperatorPrepareWoundedEnemy,
    eWorldOperatorKillWoundedEnemy,
    eWorldOperatorPostCombatWait,
    eWorldOperatorKillEnemyIfPlayerOnThePath,
    eWorldOperatorCriticallyWounded,
    eWorldOperatorKillEnemyIfCriticallyWounded,
    eWorldOperatorPauseAfterKill,

    // danger
    eWorldOperatorDangerUnknownPlanner,
    eWorldOperatorDangerInDirectionPlanner,
    eWorldOperatorDangerGrenadePlanner,
    eWorldOperatorDangerBySoundPlanner,

    eWorldOperatorDangerUnknownTakeCover,
    eWorldOperatorDangerUnknownLookAround,
    eWorldOperatorDangerUnknownSearchEnemy,

    eWorldOperatorDangerInDirectionTakeCover,
    eWorldOperatorDangerInDirectionLookOut,
    eWorldOperatorDangerInDirectionHoldPosition,
    eWorldOperatorDangerInDirectionDetourEnemy,
    eWorldOperatorDangerInDirectionSearchEnemy,

    eWorldOperatorDangerGrenadeTakeCover,
    eWorldOperatorDangerGrenadeWaitForExplosion,
    eWorldOperatorDangerGrenadeTakeCoverAfterExplosion,
    eWorldOperatorDangerGrenadeLookAround,
    eWorldOperatorDangerGrenadeSearch,

    // global
    eWorldOperatorDeathPlanner,
    eWorldOperatorALifePlanner,
    eWorldOperatorCombatPlanner,
    eWorldOperatorAnomalyPlanner,
    eWorldOperatorDangerPlanner,

    // script
    eWorldOperatorScript,
    eWorldOperatorDummy = u32(-1),
};

// ⭐The commander's tasks (ai_commander.h, stalker_combat_task_actions.h).
// ⚠Outside the enums, at the top of the range: from eWorldPropertyScript and
// eWorldOperatorScript up the values are the mod scripts' own (property_script
// + n), and the planners' state is saved by id -- a value added inside the
// enum would shift every one of them.
constexpr u32 eWorldPropertyHasTask = u32(-64);
constexpr u32 eWorldPropertyAtPlace = u32(-63);
constexpr u32 eWorldOperatorReachPlace = u32(-64);
constexpr u32 eWorldOperatorFightFromPlace = u32(-63);

enum ESightActionType
{
    eSightActionTypeWatchItem = u32(0),
    eSightActionTypeWatchEnemy,
    eSightActionTypeDummy = u32(-1),
};
}; // namespace StalkerDecisionSpace
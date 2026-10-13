#pragma once

// =====================================================================
//  Putting somebody back on their feet, from the server.
//
//  ⭐WHY THIS HAS TO EXIST HERE. Neither half of the original game needs
//  it: single player reloads a save, and stock multiplayer answers death
//  by building you a NEW body. A persistent world can do neither. The
//  body IS the character here -- its id is what the inventory hangs off,
//  what the A-Life registry knows, what every other client is drawing --
//  so "respawn" in the stock sense throws away the thing being kept.
//
//  ⚠⚠DEATH IS NOT A FLAG. It is a state spread over three layers, and
//  undoing two of them leaves a corpse that walks or a live man who
//  cannot be shot:
//
//    condition   health at zero            CEntityCondition
//    entity      m_level_death_time != 0   CEntity::AlreadyDie
//    physics     a ragdoll shell in place of a walking capsule
//
//  Only the machine that owns the body can undo them -- the ragdoll is a
//  physics object living in that process -- so this is an EVENT to the
//  owner (GE_REVIVE), not a health number written into an update.
//
//  ⚠And the server's own bookkeeping has to be told too. SVHealth
//  deliberately re-anchors only DOWNWARD (a ledger that follows health up
//  cannot catch a cheat inventing it), so a revived player would look
//  exactly like one healing himself out of thin air. The ledger is made to
//  FORGET instead, and re-anchors from the next honest update.
// =====================================================================

namespace SVRevive
{
// sv_revive <id|all|actor|faction> [health 0..1]
//
// `health` is a fraction of the entity's maximum, defaulting to full. Anything
// already alive is left alone and said so -- reviving the living would silently
// top up health, which is a different order and should be typed as one.
//
// Returns how many were actually brought back.
u32 Revive(const xr_vector<u16>& ids, float health_fraction);
} // namespace SVRevive

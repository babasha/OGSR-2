////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_spawn_registry_inline.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife spawn registry inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC const CALifeSpawnHeader& CALifeSpawnRegistry::header() const { return (m_header); }

IC const CALifeSpawnRegistry::SPAWN_GRAPH& CALifeSpawnRegistry::spawns() const { return (m_spawns); }

IC void CALifeSpawnRegistry::process_spawns(SPAWN_IDS& spawns)
{
    std::sort(spawns.begin(), spawns.end());
    spawns.erase(std::unique(spawns.begin(), spawns.end()), spawns.end());
}

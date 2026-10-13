#pragma once

// =====================================================================
//  The level, as the SERVER means it to be played.
//
//  sv_npc gives the server a handle on one creature. This is what turns
//  a pile of handles into a place worth walking into: it finds the
//  anomalous fields the level already contains, decides which of them
//  matter, and posts a faction detail on each -- so a player arriving at
//  a field finds it held, and taking an artefact out of one is a thing
//  somebody objects to.
//
//  ⚠The fields are DISCOVERED, not authored. Coordinates typed into a
//  config are a guess about a level, go stale the moment anyone edits
//  it, and are wrong for every other level. The anomalies are already in
//  the spawn: clustering them is both more honest and less work, and it
//  means this same command populates a level nobody has hand-tuned.
//
//  See game-design-extraction-concept: five anomalous fields, held by
//  Duty and Freedom, are the first half of the economic loop. The second
//  half -- carrying an artefact to the scientists -- is not here.
//
//  Console:
//    sv_fields [count]              what the level has, strongest first
//    sv_scenario [fields] [guards]  post details on the strongest fields
//    sv_scenario_status
//    sv_scenario_clear
// =====================================================================

namespace SVScenario
{
// Report the anomalous fields on the current level, strongest first. A field
// is a cluster of anomaly zones; "strongest" means most zones in it, which is
// also roughly how dangerous it is and how much it is worth.
void Fields(u32 max_report);

// Post `guards` creatures of alternating factions on each of the strongest
// `fields` fields, armed, on a leash the size of the field.
void Apply(u32 fields, u32 guards, const char* section);

void Status();

// Remove everything this module placed. Puppets spawned by hand are left
// alone -- clearing the scenario should not clear somebody's test squad.
void Clear();

// Level change: the ids and the fields meant something only in that world.
void Reset();
} // namespace SVScenario

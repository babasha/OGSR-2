#pragma once

// =====================================================================
//  What a server-driven creature actually carries -- and exact control
//  over it.
//
//  ⚠Measured 28-07-2026, and this is why the file exists: a creature's
//  equipment on this server is ADDITIVE and only half authored by us.
//  CSE_ALifeTraderAbstract::spawn_supplies runs inside spawn_item and
//  hands the new stalker whatever its character profile says (the mod's
//  character_desc_*.xml), and sv_npc_give then piles our items on top.
//  So a "5 v 5 with identical AK-74s" was nothing of the sort: every one
//  of the ten also carried a profile weapon, and NOBODY had checked which
//  of the two it was fighting with.
//
//  ⭐A test whose inputs are unknown measures nothing. Two commands make
//  them known and one makes them chosen:
//
//    sv_npc_gear    -- what each of them carries, what is in its hands
//                      right now, what it is wearing, grouped so that
//                      "are the two sides equipped the same?" is one line
//                      and not ten of them read side by side.
//    sv_npc_strip   -- take it all away, so a loadout is exact rather
//                      than added to whatever the profile decided.
//    sv_npc_loadout -- strip, then give this list. One order, and the
//                      creature ends up with precisely it.
//
//  The authority everywhere here is the SERVER ENTITY's children, not the
//  online object's inventory: children exist whether or not the creature
//  has gone online, they are what survives a switch, and they are what
//  SVDamage already reads to bound armour. The online object is consulted
//  for exactly two things it alone knows -- what is in the hands and what
//  is worn -- and both are reported as unknown when it is offline rather
//  than guessed at.
// =====================================================================

namespace SVGear
{
// sv_npc_gear: the inventory census. Empty list means "every tracked puppet".
void Census(const xr_vector<u16>& ids);

// sv_npc_strip: destroy everything the named creatures carry.
//
// ⚠The PDA is kept by default. spawn_supplies creates it before it looks at
// anything else and stamps the creature's own id into it; it is identity rather
// than equipment, and no test is about it. `bare` removes that too.
//
// Returns how many items were destroyed.
u32 Strip(const xr_vector<u16>& ids, bool bare);

// sv_npc_templates: which creature sections are tough, and how tough.
//
// ⭐The lever this exists to expose. An outfit handed to a creature never
// reaches its outfit slot, so "arm them better" and "make them harder to kill"
// are NOT the same order here: protection comes from the SECTION a creature was
// spawned as (its bones_protection_sect, the table CAI_Stalker::Hit applies) and
// from that section's per-bone damage table. Choosing the template IS the armour
// decision, and until this printed it there was no way to make it on evidence.
//
// `filter` is an optional substring; empty lists every section that declares
// either table.
void Templates(const char* filter);
} // namespace SVGear

#pragma once

// =====================================================================
//  Server-side gameplay telemetry.
//
//  Append-only newline-delimited JSON -- one self-contained record per
//  line -- written to $logs$\telemetry_<stamp>.jsonl. Only a process that
//  owns a server writes it, and only from state the server holds itself,
//  never from a value a client reported about its own condition.
//
//  Two kinds of record:
//    * events  -- something happened once: a client joined or left, an
//                 entity was hit, an entity died, an item changed hands.
//    * census  -- a periodic snapshot: population per community, what
//                 stock exists and who holds it, where the players are.
//
//  Sampling raw state instead of maintaining purpose-built counters is
//  deliberate. A question asked later ("how long was that area
//  contested?", "which weapons actually kill players?") can be answered
//  from samples already on disk, while a counter has to have existed
//  before the question was asked -- and history cannot be re-created.
//
//  Records are flat and never refer to earlier lines, so a file cut short
//  (server killed, disk full) stays parseable up to its last newline.
//
//  Console knobs (registered in console_commands.cpp):
//    sv_telemetry         0 = off, 1 = events + census, 2 = also every hit
//    sv_telemetry_census  seconds between snapshots (0 = no snapshots)
//    sv_telemetry_stock   include the per-section stock breakdown
//    sv_telemetry_pos     include one position sample per player
//    sv_telemetry_dump    write a snapshot right now, print the file path
// =====================================================================

class CSE_Abstract;
class NET_Packet;

namespace SVTelemetry
{
// True when a record written now would reach a file.
bool Active();

// Path of the file currently being written, "" when closed.
const char* FileName();

// Per-server-tick housekeeping: schedules the census, flushes to disk,
// and closes the file if sv_telemetry has just been turned off.
void Frame();

void Close();

// One census immediately, regardless of the schedule.
void CensusNow();

// --- call sites -------------------------------------------------------

// GE_HIT as queued by the server. Reads the packet without leaving its
// read cursor moved. Also feeds the "what weapon was this entity last hit
// with" cache that OnDeath attributes kills through.
//
// `dropped` says the server refused to relay this hit (the attacker is not an
// online server entity), so nobody took the damage. It is still recorded --
// discarded damage is history too -- but it must be recorded as discarded, and
// it must not enter the weapon cache: a hit that hurt nobody cannot be the
// cause of a later death.
void OnHit(NET_Packet& P, bool dropped = false);

void OnDeath(CSE_Abstract* victim, CSE_Abstract* killer);

// how: "take" (item gained a parent) or "drop" (item lost one)
void OnItemMoved(const char* how, CSE_Abstract* item, CSE_Abstract* from, CSE_Abstract* to);

// what: "join" or "leave". `local` marks the server's own in-process client --
// it is not a person, and counting it as one silently inflates every
// player-derived figure.
void OnClient(const char* what, u32 client_id, const char* name, CSE_Abstract* entity, bool local);

// --- escape hatch for systems written later ---------------------------
// Emits {"ev":<kind>,...} with the caller's fields appended. `fields` must
// already be a valid JSON object body with no leading comma:
//     Raw("hold", "\"pt\":3,\"by\":\"stalker\",\"secs\":420");
// Preferred over starting a second log file: one stream keeps every record
// time-ordered against every other one.
void Raw(const char* kind, const char* fields);
} // namespace SVTelemetry

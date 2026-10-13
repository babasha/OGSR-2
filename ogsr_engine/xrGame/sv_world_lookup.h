#pragma once

// =====================================================================
//  Finding server-side things by id.
//
//  Shared because getting it wrong is silent and expensive: the server's
//  own entity map holds only what is currently ONLINE, and a persistent
//  world is mostly offline. Looking there alone was measured turning every
//  offline holder in the stock census into "unknown". The ALife registry
//  has to be asked first, and every server-side subsystem that resolves an
//  id needs the same order -- so it lives in one place rather than being
//  re-derived per file.
// =====================================================================

class xrServer;
class CSE_Abstract;

namespace SVWorld
{
// The server this process owns, or null. Guarded rather than asserted:
// this compiles into the client too, where there is no server at all and
// no level for most of the process lifetime.
xrServer* Server();

// ALife registry first, server entity map second. Returns null for
// 0xffff and for ids nobody knows.
CSE_Abstract* Find(u16 id);

// sv_orphans: every level object whose id no longer resolves to anything on
// the server -- an object that outlived its own entity. Returns how many.
// See the .cpp for what one of these does to a running world.
u32 AuditOrphans(bool verbose = true);

// ⭐⭐⭐Say that something just read an object whose server entity is gone.
//
// This is the complaint half of a whole CLASS of guard. The AI resolves a
// creature's own server entity through the ALife registry and walks straight
// into the result; the only check on the way is `VERIFY`/`THROW2`, and both
// compile to nothing in Release (xrDebug_macros.h). One such line cost 3021
// swallowed faults in ten minutes on 05-08-2026, and there are seven more of
// the same shape in the stalker and monster planners.
//
// ⚠Throttled ACROSS every call site, not per site: a storm diagnosed by seven
// independent throttles is still a storm, only in seven voices. The running
// total travels with the line because a throttled line is not a rate.
void NoteOrphanRead(u16 id, const char* name, const char* where);

// sv_orphan_make <id>: strand an ONLINE creature -- take its server entity and
// ALife record away and leave the object running. ⚠A test instrument that
// deliberately breaks the world, so that the guards against this state can be
// shown to hold. Returns 1 if it happened.
u32 OrphanEntity(u16 id);
} // namespace SVWorld

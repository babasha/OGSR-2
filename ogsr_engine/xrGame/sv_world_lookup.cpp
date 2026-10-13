#include "StdAfx.h"
#include "sv_world_lookup.h"

#include "xrServer.h"
#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "game_sv_single.h"

xrServer* SVWorld::Server()
{
    if (!g_pGameLevel)
        return nullptr;
    return Level().Server;
}

CSE_Abstract* SVWorld::Find(u16 id)
{
    if (0xffff == id)
        return nullptr;

    // ALife first: it knows the whole world. The server's map is the online
    // subset, and in a persistent world that is the minority.
    if (ai().get_alife())
        if (CSE_Abstract* e = ai().alife().objects().object(id, true))
            return e;

    xrServer* SV = Server();
    return SV ? SV->ID_to_entity(id) : nullptr;
}

void SVWorld::NoteOrphanRead(u16 id, const char* name, const char* where)
{
    static u32 s_seen = 0;
    static u32 s_next_ms = 0;

    ++s_seen;
    if (Device.dwTimeGlobal < s_next_ms)
        return;

    s_next_ms = Device.dwTimeGlobal + 5000;
    Msg("!! [orphan] %s: creature %u '%s' has no server entity -- it outlived its own despawn and is still being asked to think (%u such reads so far)", where, u32(id),
        name ? name : "?", s_seen);
}

// ⭐⭐sv_orphans -- objects that outlived their own server entity.
//
// This exists because that state turned out to be REACHABLE and completely
// invisible. Live 05-08-2026: `sv_npc_del all` released a creature's ALife
// object and destroyed its server entity, and its level object went on being
// scheduled for the next ten minutes, thinking five times a second, faulting
// every time (see the note over CStalkerPropertyEvaluatorSmartTerrainTask::
// evaluate). Nothing in the world census could see it: sv_npc_list walks the
// puppet roster, which had already forgotten it; sv_npc_gear walks entities,
// which no longer had it. The only surviving trace was an anonymous fault.
//
// A zombie is not merely a leak. Its inventory, its ALife record and its
// ownership are gone, so every subsystem that resolves it by id gets null --
// the mod's own spawn path said so in the same log ("invalid parent id
// [5376]"). It cannot be saved, cannot be despawned again, and cannot die.
//
// ⚠Reports rather than repairs. Destroying one from here would send the very
// GE_DESTROY that already failed to remove it, and a census that quietly
// changes the world cannot be used to measure whether a fix worked.
u32 SVWorld::AuditOrphans(bool verbose)
{
    if (!g_pGameLevel)
    {
        Msg("! [orphan] no level");
        return 0;
    }

    // ⚠⚠HOST ONLY, and this is not a detail. On a pure network client there is
    // no server in the process at all, so SVWorld::Find returns null for
    // EVERY id -- this census would report the entire world as orphaned and
    // read as a catastrophe. The question "does this object still have a
    // server entity" is only meaningful where the entities live.
    if (!Server())
    {
        Msg("! [orphan] this process has no server -- nothing here owns entities, so nothing can be orphaned");
        return 0;
    }

    u32 total = 0, orphans = 0, doomed = 0;
    for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
    {
        CObject* O = Level().Objects.o_get_by_iterator(i);
        if (!O)
            continue;

        // Never had a network id (the demo spectator is the stock example), so
        // there is no entity it could have lost.
        if (0xffff == O->ID())
            continue;
        ++total;

        // Already on its way out: marked this frame and not yet swept. That is
        // the normal one-frame window, not a zombie, and counting it as one
        // would make every despawn look like a defect.
        if (O->getDestroy())
        {
            ++doomed;
            continue;
        }

        if (Find(O->ID()))
            continue;

        ++orphans;
        if (verbose)
        {
            CEntityAlive* alive = smart_cast<CEntityAlive*>(O);
            Msg("!! [orphan]   id=%u '%s' [%s] pos=(%.1f, %.1f, %.1f)%s -- no server entity", u32(O->ID()), O->cName().c_str(), O->cNameSect().c_str(), VPUSH(O->Position()),
                alive ? (alive->g_Alive() ? " alive" : " dead") : "");
        }
    }

    Msg("~ [orphan] %u objects: %u orphaned, %u already marked for destruction", total, orphans, doomed);
    return orphans;
}

// ⭐⭐sv_orphan_make -- BREAK a creature on purpose, the exact way the world
// broke itself on 05-08-2026.
//
// ⚠⚠This is a test instrument and it damages the world it is pointed at. It
// exists for the same reason sv_hit_fake does: the only faithful way to prove
// a guard works is to produce the thing it guards against, through the same
// door the real one came in.
//
// The state being synthesised is precise: the creature's ALife record is
// released and its server entity destroyed, WITHOUT the GE_DESTROY that would
// take its level object with them. That is what the despawn left behind live,
// and it is the whole precondition of the fault storm -- every evaluator that
// resolves the creature by id now gets null while the creature goes on
// thinking five times a second.
//
// ⭐Written because two sessions were spent trying to reproduce the DESPAWN,
// which is a second, separate defect (why does the object survive?). The crash
// does not need that question answered: it needs the state, and the state can
// be made directly. Fix what is understood, keep measuring what is not.
u32 SVWorld::OrphanEntity(u16 id)
{
    xrServer* SV = Server();
    if (!SV || !ai().get_alife())
    {
        Msg("! [orphan] no server with ALife in this process");
        return 0;
    }

    CSE_Abstract* e = Find(id);
    if (!e)
    {
        Msg("! [orphan] nothing on the server with id %u", u32(id));
        return 0;
    }

    // It has to be ONLINE, or there is no object to strand: an offline
    // creature is only a record, and removing a record removes everything.
    CObject* O = g_pGameLevel ? Level().Objects.net_Find(id) : nullptr;
    if (!O)
    {
        Msg("! [orphan] %u '%s' is offline -- there is no running object to strand", u32(id), e->name_replace());
        return 0;
    }

    game_sv_Single* single = smart_cast<game_sv_Single*>(SV->game);
    if (!single)
    {
        Msg("! [orphan] this is not a single/ALife game type");
        return 0;
    }

    if (ai().alife().objects().object(id, true))
        single->alife().release(e, false); // false: we destroy the entity ourselves, below

    SV->entity_Destroy(e);

    Msg("!! [orphan] %u '%s' DELIBERATELY stranded: ALife record and server entity gone, object still running", u32(id), O->cName().c_str());
    return 1;
}

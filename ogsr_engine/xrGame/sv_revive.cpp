#include "StdAfx.h"
#include "sv_revive.h"

#include "sv_world_lookup.h"
#include "sv_health.h"
#include "sv_npc_health.h"

#include "xrServer.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "Level.h"
#include "GameObject.h"
#include "xrMessages.h"

u32 SVRevive::Revive(const xr_vector<u16>& ids, float health_fraction)
{
    clamp(health_fraction, 0.01f, 1.f);

    xrServer* SV = SVWorld::Server();
    if (!SV)
    {
        Msg("! [revive] no server");
        return 0;
    }

    u32 revived = 0, skipped = 0, refused = 0;
    for (u16 id : ids)
    {
        CSE_Abstract* e = SVWorld::Find(id);
        CSE_ALifeCreatureAbstract* creature = e ? smart_cast<CSE_ALifeCreatureAbstract*>(e) : nullptr;
        if (!creature)
        {
            Msg("! [revive] %u is not a creature (or does not exist)", id);
            ++refused;
            continue;
        }

        if (creature->g_Health() > 0.f)
        {
            // ⭐Refused rather than silently topped up. "Heal" and "revive" are
            // different orders and an operator who typed one should not get the
            // other -- especially since this one rebuilds physics.
            Msg("~ [revive] %u '%s' is already alive at %.3f -- left alone", id, e->s_name.c_str(), creature->g_Health());
            ++skipped;
            continue;
        }

        const float was = creature->g_Health();
        creature->s_Health(health_fraction);

        // ⚠The server's own ledgers have to forget this body BEFORE the client
        // is told anything. SVHealth re-anchors only downward on purpose (see
        // sv_health.h), so a revived player reads as one inventing health out of
        // nothing; and the creature ledger keeps a `dead` mark that stops it
        // ever applying damage again -- a revived NPC would be immortal.
        SVHealth::OnDeath(id); // "stop tracking", so the next honest update re-anchors
        SVNpcHealth::Forget(id);

        // The host's own copy, if this body lives in this process. Done here
        // rather than left to the broadcast, which the host ignores by design.
        if (CObject* O = Level().Objects.net_Find(id))
            if (CGameObject* GO = smart_cast<CGameObject*>(O))
                GO->OnServerRevive(health_fraction);

        // ...and the machine that actually owns the body. ⚠Broadcast rather
        // than a directed send: a corpse is drawn by EVERY client that can see
        // it, and one that only heard the death keeps a body lying in the world
        // that the owner is walking around in.
        NET_Packet P;
        P.w_begin(M_EVENT);
        P.w_u32(Level().timeServer());
        P.w_u16(GE_REVIVE);
        P.w_u16(id);
        P.w_float(health_fraction);
        SV->SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));

        Msg("~ [revive] %u '%s' brought back: health %.3f -> %.3f, ledgers cleared, every client told", id, e->s_name.c_str(), was, health_fraction);
        ++revived;
    }

    Msg("~ [revive] %u revived, %u already alive, %u refused", revived, skipped, refused);
    return revived;
}

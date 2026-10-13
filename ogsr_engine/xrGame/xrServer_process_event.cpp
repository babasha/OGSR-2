#include "StdAfx.h"
#include "xrServer.h"
#include "game_sv_single.h"
#include "alife_simulator.h"
#include "xrServer_Objects.h"
#include "game_base.h"
#include "game_cl_base.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "sv_health.h"
#include "sv_npc_health.h"
#include "sv_damage_model.h"
#include "sv_hit_trail.h"
#include "shot_latency.h"

#pragma todo("KRodin: заменить на прямые вызовы вместо этих всратых ивентов!")

void xrServer::Process_event(NET_Packet& P, ClientID sender)
{
#ifdef SLOW_VERIFY_ENTITIES
    VERIFY(verify_entities());
#endif

    u32 timestamp;
    u16 type;
    u16 destination;
    u32 MODE = net_flags(TRUE, TRUE);

    // correct timestamp with server-unique-time (note: direct message correction)
    P.r_u32(timestamp);

    // read generic info
    P.r_u16(type);
    P.r_u16(destination);

    // Where an event's own payload starts. receiver->OnEvent below is free to
    // consume it, so anything read after that point has to come back here
    // first -- and put the cursor back when it is done.
    const u32 body_pos = P.r_tell();

    CSE_Abstract* receiver = game->get_entity_from_eid(destination);
    if (receiver)
    {
        if (!receiver->owner)
        {
            Msg("!![%s] Cnt't find owner for receiver with id [%u]. May be it already destroyed.", __FUNCTION__, destination);
            return;
        }

        receiver->OnEvent(P, type, timestamp, sender);
    }

    switch (type)
    {
    // ⚠⚠GE_WPN_STATE_CHANGE goes back to EVERYONE, the sender included, and that
    // is deliberate until somebody measures it.
    //
    // 28-07-2026 I excluded the sender here, reasoning that CHudItem::SwitchState
    // has already applied the state locally before sending (the send is gated on
    // object().Local()), so the round trip could only replay the switch a second
    // time -- which matched a client stack that died in
    // svector<CBlend*,64>::push_back with a HUD animation started from
    // CLevel::cl_Process_Event.
    //
    // ⛔It was wrong, and the way it was wrong is worth keeping: with the sender
    // excluded the player could not FIRE at all. So on this setup the event is
    // not an echo of the player's own action -- their weapon's state genuinely
    // arrives over the wire. Which client actually sends it, and whether the
    // weapon is Local on the player's side at all, is exactly what nobody has
    // printed yet; dbg_wpn_trace prints it. Do not touch this line again from a
    // reading of the code alone.
    case GE_INFO_TRANSFER:
    case GE_WPN_STATE_CHANGE:
    case GEG_PLAYER_ATTACH_HOLDER:
    case GEG_PLAYER_DETACH_HOLDER:
    case GEG_PLAYER_ACTIVATEARTEFACT:
    case GE_GRENADE_EXPLODE: {
        SendBroadcast(BroadcastCID, P, MODE);
    }
    break;
    case GEG_PLAYER_ITEM2SLOT:
    case GEG_PLAYER_ITEM2BELT:
    case GEG_PLAYER_ITEM2RUCK: {
        // A client announcing where it just put an item. This is the ONLY way
        // the server ever learns a belt from a rucksack -- the server-side item
        // carries no slot -- and the damage floor needs it, because artefacts
        // only protect from the belt (CActor::HitArtefactsOnBelt).
        u16 item = 0xffff;
        const u32 saved = P.r_tell();
        P.r_seek(body_pos);
        P.r_u16(item);
        P.r_seek(saved);

        SVDamage::NoteItemPlace(item, (type == GEG_PLAYER_ITEM2BELT) ? SVDamage::Place::Belt : SVDamage::Place::NotBelt);

        SendBroadcast(BroadcastCID, P, MODE);
    }
    break;
    case GE_TRADE_BUY:
    case GE_OWNERSHIP_TAKE:
    case GE_TRANSFER_TAKE: {
        // Taking an item re-places it from its OWN m_eItemPlace (CInventory::Take)
        // and sends no ITEM2* event for that, so whatever the server remembered
        // about where it sat is now a guess. Drop it and let the bound go back
        // to assuming the belt, which is the safe assumption.
        u16 taken = 0xffff;
        const u32 saved = P.r_tell();
        P.r_seek(body_pos);
        P.r_u16(taken);
        P.r_seek(saved);
        SVDamage::ForgetItemPlace(taken);

        Process_event_ownership(P, sender, timestamp, destination, FALSE, type);
        VERIFY(verify_entities());
    }
    break;
    case GE_TRADE_SELL:
    case GE_OWNERSHIP_REJECT:
    case GE_TRANSFER_REJECT:
    case GE_LAUNCH_ROCKET: {
        const u16 rejected = P.r_u16();
        SVDamage::ForgetItemPlace(rejected); // left the inventory, see above
        Process_event_reject(P, sender, timestamp, destination, rejected);
        VERIFY(verify_entities());
    }
    break;
    case GE_DESTROY: {
        SVDamage::ForgetItemPlace(destination); // and ids get recycled
        Process_event_destroy(P, sender, timestamp, destination, NULL);
        VERIFY(verify_entities());
    }
    break;
    case GE_TRANSFER_AMMO: {
        u16 id_entity;
        P.r_u16(id_entity);
        CSE_Abstract* e_parent = receiver; // кто забирает (для своих нужд)
        CSE_Abstract* e_entity = game->get_entity_from_eid(id_entity); // кто отдает
        if (!e_entity)
            break;
        if (0xffff != e_entity->ID_Parent)
            break; // this item already taken
        xrClientData* c_parent = e_parent->owner;
        xrClientData* c_from = ID_to_client(sender);
        R_ASSERT(c_from == c_parent); // assure client ownership of event

        // Signal to everyone (including sender)
        SendBroadcast(BroadcastCID, P, MODE);

        // Perfrom real destroy
        entity_Destroy(e_entity);
        VERIFY(verify_entities());
    }
    break;
    case GE_HIT:{
        P.r_pos -= 2; // put the victim id back: GAME_EVENT_ON_HIT reads it first
        // Earliest point the server can honestly claim to have the hit. What
        // separates this stamp from the one in GAME_EVENT_ON_HIT is the delayed
        // event queue, which is a leg of the journey and deserves its own number.
        ShotLatency::OnArrived(P);
        // ⚠The sender used to be dropped here (a default-constructed ClientID),
        // and with it the only trustworthy answer to "who asked for this hit".
        // The attacker id inside the packet is payload; the sender is the
        // transport's. GAME_EVENT_ON_HIT needs the second one to tell the
        // server's own simulation of the world apart from a client's claim.
        game->AddDelayedEvent(P, GAME_EVENT_ON_HIT, 0, sender);
    }
    break;
    case GE_ASSIGN_KILLER: {
        u16 id_src;
        P.r_u16(id_src);

        CSE_Abstract* e_dest = receiver; // кто умер
        // this is possible when hit event is sent before destroy event
        if (!e_dest)
            break;

        CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(e_dest);
        if (creature)
            creature->m_killer_id = id_src;

        //		Msg							("[%d][%s] killed [%d][%s]",id_src,id_src==u16(-1) ? "UNKNOWN" :
        //game->get_entity_from_eid(id_src)->name_replace(),id_dest,e_dest->name_replace());

        break;
    }
    case GE_CHANGE_VISUAL: {
        CSE_Visual* visual = smart_cast<CSE_Visual*>(receiver);
        VERIFY(visual);
        string256 tmp;
        P.r_stringZ(tmp);
        visual->set_visual(tmp);
    }
    break;
    case GE_DIE: {
        // Parse message
        u16 id_src;
        P.r_u16(id_src);

        VERIFY(game && ID_to_client(sender));

        CSE_Abstract* e_dest = receiver; // кто умер
        // this is possible when hit event is sent before destroy event
        if (!e_dest)
            break;

        CSE_Abstract* e_src = game->get_entity_from_eid(id_src); // кто убил
        if (!e_src)
        {
            xrClientData* C = (xrClientData*)game->get_client(id_src);
            if (C)
                e_src = C->owner;
        };
        if (!e_src)
            e_src = e_dest;
        VERIFY(e_src);
        //			R_ASSERT2			(e_dest && e_src, "Killer or/and being killed are offline or not exist at all :(");

        // ⭐A death the server did not decide. Printed with the ledger's own
        // number beside it, because that is the whole question: if the ledger
        // still has health left, some other authority just killed this creature
        // and the server is finding out about it. See dbg_death.
        //
        // ⚠⚠THERE ARE TWO LEDGERS AND THEY HOLD DIFFERENT POPULATIONS. Until
        // 30-07-2026 this block asked sv_npc_health about every victim, so a
        // player death -- which is never on that book -- always printed "the
        // ledger never saw it hit". True, and about the wrong ledger; it was
        // read as evidence that damage bypasses the server. Whether it does is
        // now answered by the tape (SVHitTrail), which records at the single
        // point every hit passes through and belongs to neither authority.
        {
            const xrClientData* c_from = ID_to_client(sender);
            CSE_ALifeCreatureAbstract* victim = smart_cast<CSE_ALifeCreatureAbstract*>(e_dest);
            const bool from_network = c_from && !c_from->flags.bLocal;

            SVHealth::Account acct;
            const bool on_player_books = SVHealth::Report(destination, acct);

            float ledger_hp = 0.f;
            bool ledger_dead = false;
            const bool on_creature_books = SVNpcHealth::Ledger(destination, ledger_hp, ledger_dead);

            // Wide enough to cover a burst plus the round trip a client's death
            // takes to get back here, narrow enough that a firefight two minutes
            // ago cannot explain this death.
            constexpr u32 k_window_ms = 5000;
            const u32 carried = SVHitTrail::Count(destination, k_window_ms);

            extern int g_dbg_death;
            if (g_dbg_death)
            {
                Msg("~ [death] GE_DIE from client %u%s: victim %u '%s' cse_hp=%.4f killer=%u; the server carried %u hit(s) at it in the last %u s", sender.value(),
                    (c_from && c_from->flags.bLocal) ? " (the host's own)" : "", destination, e_dest->name_replace(), victim ? victim->g_Health() : 0.f, id_src, carried,
                    k_window_ms / 1000);

                if (on_player_books)
                    Msg("~ [death]   player ledger: accounts for %.4f, client last claimed %.4f, %u hit(s) charged %.4f, BLED %.4f with no hit behind it, client admitted losing "
                        "%.4f%s",
                        acct.hp, acct.reported, acct.hits, acct.drained, acct.bled, acct.lost, acct.dead ? " (already written off)" : "");
                else if (on_creature_books)
                    Msg("~ [death]   creature ledger: %s at hp=%.4f (%s)", ledger_dead ? "had it dead too" : "says it is alive", ledger_hp,
                        SVNpcHealth::EngineOwns(destination) ? "a RECORD of what the engine spent, checked against the object below" : "and it is the authority here");
                else
                    Msg("~ [death]   on neither ledger -- no authority here was tracking this one's health");

                SVHitTrail::Print(destination, k_window_ms, "death");
            }

            // Not gated, and not a diagnostic. Two creatures, two questions --
            // see SVNpcHealth::EngineOwns for why they have to be different.
            //
            // ⚠⚠MEASURED 30-07-2026, and it invalidated the old single question:
            // a fracture was shot down to 0.0165 and then BLED OUT, and the
            // watchdog called that a second authority. It was not. The ledger
            // heard about every bullet and nothing about the bleeding, so it sat
            // above the truth on a death that was entirely ordinary -- and the
            // same is true of any death this record does not describe. NoteDrain
            // closes that gap; the split below stops the accusation being aimed
            // at the wrong thing while keeping it pointed at a real one.
            //
            // ⚠Simply muting it for engine-paid creatures was the tempting fix
            // and the wrong one: that is exactly the population the watchdog was
            // born for (28-07), and it would have gone blind there.
            if (on_creature_books && !ledger_dead)
            {
                float live_hp = 0.f;
                if (!SVNpcHealth::EngineOwns(destination))
                {
                    // The ledger IS the health here. Anything that killed this
                    // creature while the ledger had health left is a second
                    // authority, which is the whole reason this line exists.
                    Msg("!! [death] the server's record had %u at hp=%.4f -- something else applied the killing blow", destination, ledger_hp);
                }
                else if (SVNpcHealth::LiveHealth(destination, live_hp) && _abs(live_hp - ledger_hp) > 0.02f)
                {
                    // The ledger is a record, so the test is whether it kept up.
                    // A record that has drifted from the object it describes is
                    // still a defect -- just a different one, and naming it
                    // wrongly is what sent this hunt down a blind alley once.
                    Msg("!! [death] the server's record of %u reads %.4f while the object that spends its health reads %.4f -- %.4f of damage was applied through a path the "
                        "record never saw",
                        destination, ledger_hp, live_hp, _abs(live_hp - ledger_hp));
                }
            }

            // ⭐⭐⭐The player-side watchdog, and the one this block was rebuilt
            // for. A body a person drives died, and no damage aimed at it ever
            // reached the server -- so nothing here could have bounded it, and
            // the anti-cheat model has no footing under this death at all.
            //
            // ⚠It is NOT automatically a defect. Falling, bleeding, radiation,
            // starvation and script deaths are all applied on the client alone
            // and legitimately produce no hit event. The line names what is
            // missing and leaves the verdict to whoever reads it -- but it is
            // always printed, because a death the server cannot account for is
            // the one thing this whole subsystem exists to notice.
            //
            // ⚠⚠MEASURED 30-07-2026 and it nearly cost this line its meaning: a
            // player died with bullets accounting for 0.2700 and BLEEDING for
            // the rest. One stray 0.0495 stumble landed inside the window, so
            // this stayed silent by luck. Without it, a textbook-legitimate
            // death would have read as damage bypassing the server -- so the
            // bleeding the ledger now counts is named right here, and the line
            // is downgraded to a question whenever it explains the death.
            if (from_network && !carried)
            {
                if (on_player_books && acct.bled > 0.02f)
                    Msg("~ [death] client %u's body (%u '%s') died with no hit through the server in %u s, and the ledger saw %.4f leave by BLEEDING -- that accounts for it",
                        sender.value(), destination, e_dest->name_replace(), k_window_ms / 1000, acct.bled);
                else
                    Msg("!! [death] client %u's body (%u '%s') died with NO hit at it having passed through the server in %u s and NO bleeding to explain it -- nothing here could "
                        "bound that damage",
                        sender.value(), destination, e_dest->name_replace(), k_window_ms / 1000);
            }
        }

        game->on_death(e_dest, e_src);
        SVHealth::OnDeath(destination); // the ledger has nothing left to enforce

        xrClientData* c_src = e_src->owner; // клиент, чей юнит убил

        if (c_src->owner->ID == id_src)
        {
            // Main unit
            P.w_begin(M_EVENT);
            P.w_u32(timestamp);
            P.w_u16(type);
            P.w_u16(destination);
            P.w_u16(id_src);
            P.w_clientID(c_src->ID);
        }

        SendBroadcast(BroadcastCID, P, MODE);

        //////////////////////////////////////////////////////////////////////////
        P.w_begin(M_EVENT);
        P.w_u32(timestamp);
        P.w_u16(GE_KILL_SOMEONE);
        P.w_u16(id_src);
        P.w_u16(destination);
        SendTo(c_src->ID, P, net_flags(TRUE, TRUE));
        //////////////////////////////////////////////////////////////////////////

        VERIFY(verify_entities());
    }
    break;
    case GE_CHANGE_POS: {
        SendTo(SV_Client->ID, P, net_flags(TRUE, TRUE));
    }
    break;
    case GEG_PLAYER_ACTIVATE_SLOT:
    case GEG_PLAYER_ITEM_EAT: {
        if (GEG_PLAYER_ITEM_EAT == type)
            SVHealth::OnEaten(destination); // health this client regains next is now explained
        SendTo(SV_Client->ID, P, net_flags(TRUE, TRUE));
#ifdef SLOW_VERIFY_ENTITIES
        VERIFY(verify_entities());
#endif
    }
    break;
    /*case GEG_PLAYER_ITEM_SELL: {
    }
    break;*/
    case GE_TELEPORT_OBJECT: {
        game->teleport_object(P, destination);
    }
    break;
    case GE_ADD_RESTRICTION: {
        game->add_restriction(P, destination);
    }
    break;
    case GE_REMOVE_RESTRICTION: {
        game->remove_restriction(P, destination);
    }
    break;
    case GE_REMOVE_ALL_RESTRICTIONS: {
        game->remove_all_restrictions(P, destination);
    }
    break;
    //case GE_MONEY: {
    //    CSE_Abstract* e_dest = receiver;
    //    CSE_ALifeTraderAbstract* pTa = smart_cast<CSE_ALifeTraderAbstract*>(e_dest);
    //    pTa->m_dwMoney = P.r_u32();
    //}
    //break;
    case GE_FREEZE_OBJECT: break;
    default: R_ASSERT2(0, "Game Event not implemented!!!"); break;
    }
}

// Kill on the server's own authority. Mirrors the GE_DIE case above: the same
// two things have to happen -- the world's death bookkeeping (which drops the
// inventory, credits the killer and reaches ALife), and the broadcast that
// makes every client, the victim's included, run CEntity::Die.
//
// The victim's client is told the same way an honest death would be announced,
// so a client that refuses to apply it is the only one left disagreeing, and
// it disagrees alone: every other client, and the server's own entity, have it
// dead already.
void xrServer::force_kill(CSE_Abstract* e_dest, u16 id_src)
{
    if (!e_dest || !game)
        return;

    CSE_Abstract* e_src = game->get_entity_from_eid(id_src);
    if (!e_src)
    {
        xrClientData* C = (xrClientData*)game->get_client(id_src);
        if (C)
            e_src = C->owner;
    }
    if (!e_src)
        e_src = e_dest; // nobody the server can name: counts as self-inflicted

    game->on_death(e_dest, e_src);
    SVHealth::OnDeath(e_dest->ID);

    NET_Packet P;
    P.w_begin(M_EVENT);
    P.w_u32(Level().timeServer());
    P.w_u16(GE_DIE);
    P.w_u16(e_dest->ID);
    P.w_u16(e_src->ID);
    if (e_src->owner)
        P.w_clientID(e_src->owner->ID);
    else
        P.w_u32(0); // killer belongs to no client -- what an NPC kill sends

    SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
}

#include "StdAfx.h"
#include "xrServer.h"
#include "xrServer_Objects.h"
#include "Level.h"
#include "Actor.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "../COMMON_AI/xrServer_Objects_ALife_Monsters.h"
#include "sv_health.h"

int g_Dump_Update_Read = 0;

void xrServer::Process_update(NET_Packet& P, ClientID sender)
{
    xrClientData* CL = ID_to_client(sender);
    R_ASSERT2(CL, "Process_update client not found");

    if (g_Dump_Update_Read)
        Msg("---- UPDATE_Read --- ");

    // Historically this path only ever ran for the fused in-process client
    // (R_ASSERT(CL->flags.bLocal)). A network client sends the identical
    // [u16 id][u8 size][UPDATE_Write payload] stream over the transport, so the
    // parse loop is shared; a non-local sender additionally claims authority
    // over the entities it exports (see net_client_controls) and has its state
    // pushed onto the in-process game object.
    const bool remote_sender = !CL->flags.bLocal;
    // while has information
    while (!P.r_eof())
    {
        // find entity
        u16 ID;
        u8 size;

        P.r_u16(ID);
        P.r_u8(size);
        u32 _pos = P.r_tell();
        CSE_Abstract* E = ID_to_entity(ID);

        if (E)
        {
            // Msg				("sv_import: %d '%s'",E->ID,E->name_replace());
            E->net_Ready = TRUE;
            E->UPDATE_Read(P);

            if (g_Dump_Update_Read)
                Msg("* %s : %d - %d", E->name(), size, P.r_tell() - _pos);

            if ((P.r_tell() - _pos) != size)
            {
                string16 tmp;
                CLSID2TEXT(E->m_tClassID, tmp);
                Debug.fatal(DEBUG_INFO, "Beer from the creator of '%s'", tmp);
            }

            if (remote_sender)
            {
                m_net_client_entities.insert(ID);
                apply_net_client_state(E, CL);
            }
        }
        else
            P.r_advance(size);
    }
    if (g_Dump_Update_Read)
        Msg("-------------------- ");
}

// Push a network-client-authoritative entity's freshly-read state onto the
// in-process game object, so the simulated world (AI perception, physics
// neighbours) sees the entity where the owning client says it is. Position-only
// for now; orientation/state flags can follow once this seam is proven.
// Movement validation knobs (registered in console_commands.cpp).
//   sv_move_validate  0 = trust the client, 1 = check and log, 2 = check and clamp
//   sv_move_max_speed metres per second a player may cover horizontally
//   sv_move_fall_speed metres per second a player may cover falling
// The speeds are budgets, not physics: they must sit above anything the game can
// legitimately produce (sprint + slope + artefact boosts) or honest players get
// clamped on lag spikes.
int g_sv_move_validate = 1;
float g_sv_move_max_speed = 12.f;
float g_sv_move_fall_speed = 60.f;

// Sprint endurance, checked independently of the client's own stamina.
//   sv_sprint_speed   above this many m/s counts as sprinting
//   sv_sprint_seconds how long that may be sustained before the budget is out
//   sv_sprint_regen   seconds of budget regained per second spent slower
// Once the budget is empty the speed a client may claim drops to sv_sprint_speed
// -- it can still run, it just cannot keep sprinting. These are plausibility
// limits, not the game's stamina curve: they must be impossible to reach by
// honest play (artefacts, low weight, mod tuning) and still catch "never tires".
float g_sv_sprint_speed = 4.5f;
float g_sv_sprint_seconds = 30.f;
float g_sv_sprint_regen = 0.5f;

Fvector xrServer::validate_client_movement(xrClientData* CL, CSE_Abstract* E, const Fvector& reported, const Fvector& server_pos)
{
    if (!g_sv_move_validate || !CL)
        return reported;

    const u32 now = Device.dwTimeGlobal;

    // A fresh client, or one that just took over a different entity, is anchored
    // to where the SERVER has the object -- never to what it just claimed.
    if (!CL->mv_have_baseline || CL->mv_entity_id != E->ID)
    {
        CL->mv_have_baseline = true;
        CL->mv_entity_id = E->ID;
        CL->mv_last_pos = server_pos;
        CL->mv_last_time = now;
        CL->mv_sprint_budget = g_sv_sprint_seconds;
        CL->mv_sprint_spent = false;
    }

    // Lag decides the budget, so it is bounded on both sides: a burst of packets
    // in one tick must still buy a step, and a client that went quiet for a
    // minute must not come back with a minute's worth of teleport.
    float dt = float(now - CL->mv_last_time) / 1000.f;
    clamp(dt, 0.05f, 1.0f);

    Fvector d;
    d.sub(reported, CL->mv_last_pos);
    const float horiz = _sqrt(d.x * d.x + d.z * d.z);

    // Endurance: spend the budget while actually moving at sprint pace, refill it
    // while slower. Note this is driven by MEASURED distance, never by the sprint
    // flag the client sends -- a client that lies about the flag still has to
    // cover the ground, and covering it is what costs.
    const float speed = horiz / dt;
    if (CL->mv_sprint_budget < 0.f)
        CL->mv_sprint_budget = g_sv_sprint_seconds;
    if (speed > g_sv_sprint_speed)
        CL->mv_sprint_budget -= dt;
    else
        CL->mv_sprint_budget += dt * g_sv_sprint_regen;
    clamp(CL->mv_sprint_budget, 0.f, g_sv_sprint_seconds);

    const bool winded = (CL->mv_sprint_budget <= 0.f);
    if (winded && !CL->mv_sprint_spent)
    {
        CL->mv_sprint_spent = true;
        // Reaching this at all means the client held sprint pace for longer than
        // the game can allow. Worth a line even in detect-only mode.
        Msg("! [sv_move] client %u ('%s') ran out of sprint budget after %.0fs of >%.1f m/s -- capping to %.1f m/s%s", CL->ID.value(),
            CL->ps ? CL->ps->getName() : "?", g_sv_sprint_seconds, g_sv_sprint_speed, g_sv_sprint_speed, (g_sv_move_validate < 2) ? " (detect only)" : "");
    }
    else if (!winded && CL->mv_sprint_spent && CL->mv_sprint_budget > g_sv_sprint_seconds * 0.25f)
        CL->mv_sprint_spent = false; // rested enough to report again next time

    // Slack absorbs position quantisation on the wire and the jitter of a capsule
    // settling on geometry; without it a standing player trips the check.
    const float slack = 0.5f;
    const float speed_cap = winded ? g_sv_sprint_speed : g_sv_move_max_speed;
    const float budget_h = speed_cap * dt + slack;
    const float budget_up = g_sv_move_max_speed * dt + slack;
    const float budget_down = g_sv_move_fall_speed * dt + slack;

    const bool ok = (horiz <= budget_h) && (d.y <= budget_up) && (-d.y <= budget_down);

    if (ok)
    {
        CL->mv_last_pos = reported;
        CL->mv_last_time = now;
        return reported;
    }

    ++CL->mv_violations;
    // Throttled, and odd: with several clients an even modulus locks onto one of
    // them forever (the parity trap this codebase has been bitten by before).
    if ((CL->mv_violations % 31) == 1)
        Msg("! [sv_move] client %u ('%s') asked for %.1f m horiz / %.1f m vert in %.0f ms = %.1f m/s (budget %.1f/%.1f m, cap %.1f m/s, sprint left %.0fs) -- violations=%u",
            CL->ID.value(), CL->ps ? CL->ps->getName() : "?", horiz, d.y, dt * 1000.f, speed, budget_h, d.y >= 0.f ? budget_up : budget_down, speed_cap,
            CL->mv_sprint_budget, CL->mv_violations);

    if (g_sv_move_validate < 2)
    {
        // Detection only: the world still follows the client, so nothing breaks
        // for honest players while the budgets are being calibrated.
        CL->mv_last_pos = reported;
        CL->mv_last_time = now;
        return reported;
    }

    // Enforcing: give them the direction they asked for, but only as far as they
    // could have gone. Clamping rather than freezing keeps a lagging honest
    // client moving instead of pinning it in place.
    CL->mv_needs_correction = true; // tell the owner what actually stands

    Fvector clamped = CL->mv_last_pos;
    if (horiz > budget_h && horiz > EPS_S)
    {
        const float k = budget_h / horiz;
        clamped.x += d.x * k;
        clamped.z += d.z * k;
    }
    else
    {
        clamped.x = reported.x;
        clamped.z = reported.z;
    }
    clamped.y += clampr(d.y, -budget_down, budget_up);

    CL->mv_last_pos = clamped;
    CL->mv_last_time = now;
    return clamped;
}

void xrServer::apply_net_client_state(CSE_Abstract* E, xrClientData* CL)
{
    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(E);
    if (!creature)
        return;

    // Health arrives here as whatever the client decided it is. Reconcile it
    // against the damage the server itself relayed before that number is
    // published to anybody else (see sv_health.h).
    {
        const SVHealth::Verdict hv = SVHealth::Reconcile(creature, CL);
        creature->fHealth = hv.health;
        if (hv.force_death)
        {
            Msg("! [sv_health] client %u ('%s') would not die -- server is killing entity %u ('%s'), killer %u", CL ? CL->ID.value() : 0, (CL && CL->ps) ? CL->ps->getName() : "?",
                E->ID, E->name_replace(), hv.killer);
            force_kill(E, hv.killer);
            return; // a corpse does not need its capsule warped to where it claimed to be
        }
    }

    static u32 s_applied_count = 0;
    const bool diag = ((++s_applied_count % 100) == 1);
    if (diag)
        Msg("* [net] client-controlled entity id=%u '%s' pos=(%.2f, %.2f, %.2f)", E->ID, E->name_replace(), VPUSH(creature->o_Position));

    if (!g_pGameLevel)
        return;
    CObject* O = Level().Objects.net_Find(E->ID);
    if (!O)
    {
        // observed on the headless server: the actor object is alive
        // (CurrentEntity) but its map_NETID slot is empty -- recover through
        // the control-entity pointer and re-register; the [netmap] diagnostics
        // in net_Register/net_Unregister track who empties the slot
        CObject* cur = Level().CurrentEntity();
        if (cur && cur->ID() == E->ID)
        {
            O = cur;
            Level().Objects.net_Register(O);
            Msg("* [net] apply: id=%u recovered via CurrentEntity, re-registered", E->ID);
        }
        else
        {
            if (diag)
                Msg("* [net] apply: no in-process game object for id=%u (CurrentEntity id=%d)", E->ID, cur ? int(cur->ID()) : -1);
            return;
        }
    }

    if (CActor* A = smart_cast<CActor*>(O))
    {
        const Fvector accepted = validate_client_movement(CL, E, creature->o_Position, A->Position());
        // The CSE is what every OTHER client is told about this player, so the
        // accepted position has to land there as well -- otherwise the rest of
        // the world would be shown a position this server just refused.
        creature->o_Position = accepted;

        // net_MoveTo, not ForceTransform: this runs at 20 Hz per network
        // client -- ForceTransform's collision-correct settle loop steps the
        // whole ODE world (see CGameObject::net_MoveTo). The owning client
        // already resolved collisions; warp the capsule directly.
        Fmatrix F = A->XFORM();
        F.c = accepted;
        A->net_MoveTo(F);
        if (diag)
            Msg("* [net] apply: actor object alive=%d now at (%.2f, %.2f, %.2f)", A->g_Alive() ? 1 : 0, VPUSH(A->Position()));
    }
    else if (diag)
        Msg("* [net] apply: object id=%u is not an actor (%s)", E->ID, O->cNameSect().c_str());
}

void xrServer::Process_save(NET_Packet& P, ClientID sender)
{
    xrClientData* CL = ID_to_client(sender);
    R_ASSERT2(CL, "Process_save client not found");
    CL->net_Ready = TRUE;

    R_ASSERT(CL->flags.bLocal);
    // while has information
    while (!P.r_eof())
    {
        // find entity
        u16 ID;
        u16 size;

        P.r_u16(ID);
        P.r_u16(size);
        s32 _pos_start = P.r_tell();
        CSE_Abstract* E = ID_to_entity(ID);

        if (E)
        {
            E->net_Ready = TRUE;
            E->load(P);
        }
        else
            P.r_advance(size);
        s32 _pos_end = P.r_tell();
        s32 _size = size;
        // Msg("!![%s] load/save info, object: [%s], size: [%d], _pos_end-_pos_start: [%d], ID_to_entity(ID) is [%s]", __FUNCTION__, E ? E->name_replace() : "unknown", _size,
        // _pos_end - _pos_start, E ? "true" : "false");
        if (_size != (_pos_end - _pos_start))
        {
            Msg("!![%s] load/save mismatch, object: [%s], size: [%d], _pos_end-_pos_start: [%d], ID_to_entity(ID) is [%s]", __FUNCTION__, E ? E->name_replace() : "unknown", _size,
                _pos_end - _pos_start, E ? "true" : "false");
            s32 _rollback = _pos_start + _size;
            P.r_seek(_rollback);
        }
    }
}

#include "StdAfx.h"
#include "xrServer.h"
#include "game_sv_single.h"
#include "xrMessages.h"
#include "game_cl_single.h"
#include "MainMenu.h"
#include "../xrOnlineNet/NET_GNS_Transport.h" // role constants for the admin gate
#include "../xr_3da/XR_IOConsole.h"

xrServer::EConnect xrServer::Connect(shared_str& session_name)
{
#ifdef DEBUG
    Msg("* sv_Connect: %s", *session_name);
#endif

    // Parse options and create game
    if (0 == strchr(*session_name, '/'))
        return ErrConnect;

    string1024 options;
    R_ASSERT2(xr_strlen(session_name) <= sizeof(options), "session_name too BIIIGGG!!!");
    strcpy_s(options, strchr(*session_name, '/') + 1);

    // Parse game type
    string1024 type;
    R_ASSERT2(xr_strlen(options) <= sizeof(type), "session_name too BIIIGGG!!!");
    strcpy_s(type, options);
    if (strchr(type, '/'))
        *strchr(type, '/') = 0;
    game = NULL;

    CLASS_ID clsid = game_GameState::getCLASS_ID(type, true);
    game = smart_cast<game_sv_GameState*>(NEW_INSTANCE(clsid));

    // Options
    if (0 == game)
        return ErrConnect;
    csPlayers.Enter();
//	game->type				= type_id;
#ifdef DEBUG
    Msg("* Created server_game %s", game->type_name());
#endif

    game->Create(session_name);
    csPlayers.Leave();

    return IPureServer::Connect(*session_name);
}

IClient* xrServer::new_client(SClientConnectData* cl_data)
{
    IClient* CL = client_Find_Get(cl_data->clientID);
    VERIFY(CL);

    // copy entity
    CL->ID = cl_data->clientID;
    CL->process_id = cl_data->process_id;
    CL->flags.bLocal = cl_data->bLocal;

    string64 new_name;
    strcpy_s(new_name, cl_data->name);
    CL->name = new_name;
    CL->pass = cl_data->pass;

    NET_Packet P;
    P.B.count = 0;
    P.r_pos = 0;

    game->AddDelayedEvent(P, GAME_EVENT_CREATE_CLIENT, 0, CL->ID);
    if (client_Count() == 1)
    {
        Update();
    }
    return CL;
}

void xrServer::AttachNewClient(IClient* CL)
{
    MSYS_CONFIG msgConfig;
    msgConfig.sign1 = 0x12071980;
    msgConfig.sign2 = 0x26111975;

    // bLocal is set by new_client() from SClientConnectData -- don't
    // stomp it here, and don't take over SV_Client (the "attached to this
    // process's own view" pointer used by SendTo_LL's local-memory
    // shortcut) for a non-local/network client.
    if (CL->flags.bLocal)
        SV_Client = CL;
    SendTo_LL(CL->ID, &msgConfig, sizeof(msgConfig), net_flags(TRUE, TRUE, TRUE, TRUE));

    CL->m_guid[0] = 0;
}

namespace
{
// Commands a moderator may run. Everything else needs an admin.
//
// A list of what is ALLOWED rather than what is forbidden: a new console command
// should default to "not available to moderators", not to "available until
// somebody remembers to add it here".
bool ModeratorMayRun(const char* command)
{
    static const char* const allowed[] = {"sv_who", "sv_kick", "sv_ban", "sv_unban"};
    for (const char* a : allowed)
        if (xr_strcmp(command, a) == 0)
            return true;
    return false;
}

// Never runnable over the wire, at any role.
//
// These configure the secrets this server's authority rests on. An admin who can
// set the ticket key over the network can mint themselves a new one -- and an
// admin account is exactly what an attacker would try to get first. They are set
// on the host console, which is the machine's owner by construction.
bool IsHostOnly(const char* command)
{
    static const char* const host_only[] = {"sv_ticket_key", "sv_auth_token", "sv_auth_url"};
    for (const char* h : host_only)
        if (xr_strcmp(command, h) == 0)
            return true;
    return false;
}
} // namespace

void xrServer::Process_admin_command(NET_Packet& P, ClientID sender)
{
    string1024 line{};
    P.r_stringZ(line);

    xrClientData* CL = ID_to_client(sender);
    if (!CL)
        return;

    // The command name, for the authority check. The rest is its arguments.
    string128 name{};
    {
        u32 n = 0;
        const char* p = line;
        while (*p && *p != ' ' && n + 1 < sizeof(name))
            name[n++] = *p++;
        name[n] = 0;
    }

    string1024 reply{};

    // account_id 0 means nobody proved anything -- see xrClientData. Checked
    // first and separately from the role, because a role on an unauthenticated
    // client is a number nobody signed.
    if (!CL->account_id || CL->role < NET_GNS::kRoleModerator)
    {
        Msg("! [admin] client id=%u [%s] (account %llu, role %u) tried to run '%s' -- refused", sender.value(), CL->name.c_str(), CL->account_id, u32(CL->role),
            name);
        xr_strcpy(reply, "you are not an administrator");
    }
    else if (IsHostOnly(name))
    {
        Msg("! [admin] account %llu tried to run '%s' over the network -- host console only", CL->account_id, name);
        xr_strcpy(reply, "that command can only be run on the host console");
    }
    else if (CL->role < NET_GNS::kRoleAdmin && !ModeratorMayRun(name))
    {
        Msg("! [admin] moderator %llu tried to run '%s' -- needs admin", CL->account_id, name);
        xr_strcpy(reply, "that command needs an administrator");
    }
    else
    {
        Msg("* [admin] account %llu [%s] runs: %s", CL->account_id, CL->name.c_str(), line);

        // Who ordered it, for the duration of this command, so sv_ban records an
        // author instead of "the console". A plain global rather than a
        // parameter because the console command interface has no room for one --
        // and it is safe: commands run on the game thread, one at a time.
        extern u64 g_admin_actor_account;
        g_admin_actor_account = CL->account_id;

        // The command's own output is what the admin wants back, so it is
        // captured while it runs.
        //
        // ⚠️ This catches EVERYTHING logged in that window, including anything a
        // background thread happens to write. Commands here are human-paced and
        // finish in microseconds, so the practical cost is an occasional extra
        // line in a reply -- worth it for a remote console that actually answers.
        xr_string captured;
        SetLogCB([&captured](const char* text) {
            if (text && captured.size() < 3000)
            {
                captured += text;
                captured += "\n";
            }
        });

        Console->Execute(line);

        SetLogCB(nullptr);
        g_admin_actor_account = 0;

        strncpy_s(reply, sizeof(reply), captured.empty() ? "done" : captured.c_str(), _TRUNCATE);
    }

    NET_Packet answer;
    answer.w_begin(M_ADMIN_REPLY);
    answer.w_stringZ(reply);
    SendTo(sender, answer, net_flags(TRUE, TRUE));
}

void xrServer::OnNetClientConnected(ClientID id, LPCSTR name, u64 account_id, u8 role)
{
    SClientConnectData cd;
    cd.clientID = id;
    cd.bLocal = false;
    // _TRUNCATE rather than strcpy_s: `name` comes from a verified ticket, but
    // "verified" means authd signed it, not that it fits this buffer -- and an
    // over-long name must not take the server down via the invalid-parameter
    // handler.
    strncpy_s(cd.name, sizeof(cd.name), name, _TRUNCATE);
    cd.process_id = 0;

    new_client(&cd);

    // Who this is, recorded on the client record new_client just made. Zero
    // account_id means the host does not require tickets and nobody claimed an
    // identity -- every authority check therefore has to treat 0 as "no rights",
    // not as "account number zero".
    if (xrClientData* CL = ID_to_client(id))
    {
        CL->account_id = account_id;
        CL->role = role;
    }
}

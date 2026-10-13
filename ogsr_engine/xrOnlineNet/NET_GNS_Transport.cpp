#include "stdafx.h"
#include "NET_GNS_Transport.h"

// Compiled per-exe into XR_3DA.vcxproj, XR_3DA_Vulkan.vcxproj AND
// XR_3DA_Server.vcxproj (same convention as NET_Client.cpp/NET_Server.cpp
// in this folder) purely to provide these two extern definitions wherever
// xrGame.lib's xrServer.cpp ends up linked. Zero GameNetworkingSockets
// dependency -- see NET_GNS_Transport.h for why.
namespace NET_GNS
{
SendToClientFn g_SendToClient = nullptr;
PumpServerFn g_PumpServer = nullptr;
ClientConnectFn g_ClientConnect = nullptr;
ClientSendFn g_ClientSend = nullptr;
PumpClientFn g_PumpClient = nullptr;
ClientDisconnectFn g_ClientDisconnect = nullptr;
DeliverToClientFn g_DeliverToClient = nullptr;
ClientLostFn g_ClientLost = nullptr;
ClientPingFn g_ClientPing = nullptr;

namespace
{
char s_levelNameBuf[kLevelNameBufSize] = {};
char s_joinTicketBuf[kMaxTicketChars + 1] = {};
} // namespace
char* g_ConnectedLevelName = s_levelNameBuf;
char* g_JoinTicket = s_joinTicketBuf;

int g_auth_required = 0; // off by default: see sv_auth_required in the header
int g_ban_refresh_seconds = 30;

ApplyTicketKeyFn g_ApplyTicketKey = nullptr;
ApplyAuthEndpointFn g_ApplyAuthEndpoint = nullptr;
ApplyAuthTokenFn g_ApplyAuthToken = nullptr;
AdminActionFn g_AdminAction = nullptr;

void SetJoinTicket(const char* text)
{
    if (!text)
    {
        s_joinTicketBuf[0] = 0;
        return;
    }

    // Truncate rather than refuse: a ticket that does not fit cannot verify
    // anyway, and the refusal message from the server is more useful than a
    // silent empty field here.
    strncpy_s(s_joinTicketBuf, sizeof(s_joinTicketBuf), text, _TRUNCATE);
}
} // namespace NET_GNS

#pragma once
// Transport hook table for xrServer::SendTo_LL's non-local branch
// (otherwise FATAL("")).
//
// xrServer.cpp lives in xrGame.vcxproj, which compiles ONCE into a single
// xrGame.lib shared by every exe -- unlike xr_3da-level files (NET_Client.cpp,
// NET_Server.cpp, x_ray.cpp) which each exe project compiles separately.
// Because xrServer.cpp can't be guarded per-exe, this header stays a plain
// function-pointer hook table with ZERO external transport dependency, so
// xrGame.vcxproj (and every exe linking it) needs no new include/lib.
//
// An external server-side module installs the real backend via
// NET_GNS::InstallBackend(). Everywhere else these hooks stay null,
// SendToClient()/PumpServer() are no-ops, and xrServer::SendTo_LL's non-local
// branch still hits FATAL("") as before -- stock builds are unaffected.
#include "../xr_3da/Engine.h" // ENGINE_API -- don't rely on the including TU having pulled this in already
#include "../xr_3da/NET_Server_Trash/client_id.h"

namespace NET_GNS
{
using SendToClientFn = bool (*)(ClientID id, const void* data, u32 size, bool reliable);
using PumpServerFn = void (*)();

ENGINE_API extern SendToClientFn g_SendToClient; // null unless a backend installed it
ENGINE_API extern PumpServerFn g_PumpServer; // null unless a backend installed it

IC void PumpServer()
{
    if (g_PumpServer)
        g_PumpServer();
}

// Symmetric client-side hooks. The real backend is provided by an external
// client-side module (same isolation reasoning as the server hooks above --
// xrGame.lib is shared, so this header stays transport-dependency-free).
// address is the bare "ip:port" target (the /name=... suffix already stripped
// by the caller). ClientConnect blocks (bounded) until the transport-level
// handshake completes or times out -- it does NOT wait for the engine's own
// M_SV_CONFIG_NEW_CLIENT/M_CLIENTREADY protocol, which still flows over
// ClientSend/PumpClient once bytes can move.
using ClientConnectFn = bool (*)(const char* address);
using ClientSendFn = void (*)(const void* data, u32 size, bool reliable);
using PumpClientFn = void (*)();
using ClientDisconnectFn = void (*)();
// Delivers received bytes into CLevel::OnMessage -- kept as its own hook
// (rather than the client backend including xrGame/Level.h directly) because
// a module compiled into the client exe doesn't share xrGame's own include
// dirs/PCH context that Level.h's full form needs (fastdelegate, ref_sound,
// etc). xrGame itself (Level_network_messages.cpp) installs this hook, since
// it already has everything Level.h needs.
using DeliverToClientFn = void (*)(void* data, u32 size);
// Latching "the server connection dropped after we were fully connected" query.
// The transport backend detects the drop (peer closed / crash / timeout) but
// cannot unload the level itself, so the engine polls this once per frame and
// initiates a clean disconnect. Returns true EXACTLY ONCE per drop (the backend
// self-clears after reporting) so the engine needs no latch of its own.
using ClientLostFn = bool (*)();
// Current round-trip ping to the server in milliseconds, or -1 if not connected
// (the transport backend reads it from the live connection's real-time status).
using ClientPingFn = int (*)();

ENGINE_API extern ClientConnectFn g_ClientConnect;
ENGINE_API extern ClientSendFn g_ClientSend;
ENGINE_API extern PumpClientFn g_PumpClient;
ENGINE_API extern ClientDisconnectFn g_ClientDisconnect;
ENGINE_API extern DeliverToClientFn g_DeliverToClient;
ENGINE_API extern ClientLostFn g_ClientLost;
ENGINE_API extern ClientPingFn g_ClientPing;

// The level name a network client needs before CLevel::net_start_client3()
// can resolve/load it. The legacy X-Ray M_CLIENT_CONNECT_RESULT round trip
// would normally carry this, but that path is dead in this stripped-down
// protocol -- nothing ever sends M_AUTH_CHALLENGE, so
// xrServer::SendConnectResult/Check_BuildVersion_Success never fire. Instead,
// the server backend sends this as ONE raw message immediately after accepting
// the connection (before any NET_Packet-formatted engine traffic on the same
// ordered/reliable connection), and the client backend's g_ClientConnect
// consumes it synchronously, before returning, alongside the transport
// handshake it already waits for.
constexpr u32 kLevelHelloMagic = 0x4C56484Cu; // 'LVHL'
constexpr u32 kLevelNameBufSize = 64;
struct LevelHello
{
    u32 magic;
    char level_name[kLevelNameBufSize];
    // Salt for the content fingerprint the host wants back (content_integrity.h).
    // Zero means it is not asking. The salt has to come FROM the host and be
    // different every time, or an answer recorded once could simply be replayed.
    u64 content_nonce;
    // Non-zero when the host wants a join ticket (see AuthReply below). The
    // client cannot know this in advance -- the same build has to be able to
    // join an authenticated server and a local test one.
    u32 wants_ticket;
};

// The answer to that: what the joining side's content fingerprints to under the
// given salt. Also one raw transport message, sent before any engine traffic, so
// a host that dislikes the answer can drop the connection while the other side is
// still nobody -- no player, no entity, nothing to clean up.
constexpr u32 kContentReplyMagic = 0x43524C59u; // 'CRLY'
struct ContentReply
{
    u32 magic;
    u32 files;
    u64 bytes;
    u32 list_crc;
    u32 window_crc;
};

// --- who is joining ---------------------------------------------------------
//
// The join ticket, issued by authd (online/accounts) and verified by the server
// (online/server/sv_ticket.cpp). It rides the same raw pre-engine channel as the
// content answer and for the same reason: a refusal must land while the other
// side is still nobody.
//
// The client only CARRIES a ticket -- it has no key and verifies nothing, so
// nothing about the format needs to be understood outside the server.
constexpr u32 kAuthReplyMagic = 0x50525441u; // 'ATRP'
// A ticket is 171 base64url characters today; the field is round and roomy so a
// future field in the ticket does not become a wire-format change here too.
constexpr u32 kMaxTicketChars = 255;
struct AuthReply
{
    u32 magic;
    char ticket[kMaxTicketChars + 1]; // NUL-terminated; empty means "I have none"
};

// The ticket this client was started with (`-ticket <text>`), or empty.
//
// A pointer, NOT an extern array -- MSVC's dllimport codegen for an extern
// array across this ENGINE_API boundary is unreliable and has already produced
// a genuine access violation here once (see g_ConnectedLevelName below).
ENGINE_API extern char* g_JoinTicket;
ENGINE_API void SetJoinTicket(const char* text);

// Whether this host demands a valid ticket: 0 = do not ask (anyone may join
// unauthenticated, as before), 1 = ask and log, 2 = refuse without one.
// Console: sv_auth_required. Same three-step shape as sv_content_check, and for
// the same reason -- turning refusals on blind locks everybody out at once.
ENGINE_API extern int g_auth_required;

// Role values, so code outside the server exe (xrServer.cpp lives in the shared
// xrGame.lib) can compare authority without knowing about sv_ticket.h.
//
// ⚠️ The SAME numbers as SV_Ticket::Role and the Rust `Role` enum -- they are
// stored in PostgreSQL and travel in the ticket. Spaced so a rank can be added
// between two existing ones later without renumbering anybody.
constexpr u8 kRolePlayer = 0;
constexpr u8 kRoleModerator = 10;
constexpr u8 kRoleAdmin = 100;

// How often the server re-reads the ban list from authd, in seconds. A ban takes
// effect no later than this plus the ticket lifetime -- and immediately for
// anyone already in the world, because banning also kicks.
ENGINE_API extern int g_ban_refresh_seconds;

// Settings that only the server exe can act on. Null everywhere else, so the
// console commands report "server only" instead of silently appearing to work.
// Function pointers rather than exported variables for the reason stated above
// g_ConnectedLevelName: only pointer-sized symbols cross this boundary reliably.
using ApplyTicketKeyFn = bool (*)(const char* hex);
using ApplyAuthEndpointFn = bool (*)(const char* host, int port);
using ApplyAuthTokenFn = bool (*)(const char* token);

ENGINE_API extern ApplyTicketKeyFn g_ApplyTicketKey;
ENGINE_API extern ApplyAuthEndpointFn g_ApplyAuthEndpoint;
ENGINE_API extern ApplyAuthTokenFn g_ApplyAuthToken;

// Administrative actions that need authd (ban, unban, set role) or the server's
// own client list (kick, who).
//
// ONE hook taking a verb rather than one hook per action: the parsing and the
// authd round trip both belong on the server side, and a new verb should not
// mean a new symbol crossing this boundary. Writes a human-readable outcome into
// `reply`, which is what the admin sees whether they typed the command on the
// host console or sent it over the wire.
//
// `actor_account` is the account that ordered it, or 0 for the host console.
using AdminActionFn = bool (*)(const char* verb, const char* args, u64 actor_account, char* reply, u32 reply_size);
ENGINE_API extern AdminActionFn g_AdminAction;

// Exposed as a pointer, NOT a raw array -- MSVC's dllimport codegen for an
// extern array-typed variable across this header's ENGINE_API boundary
// (dllimport in every TU that doesn't define ENGINE_BUILD, e.g. xrGame.vcxproj
// consuming this from its prebuilt xrGame.lib) is unreliable and produced a
// genuine access violation reading it from Level_network_start_client.cpp.
// A pointer-sized symbol works correctly, same as the function-pointer hooks
// above -- points at a fixed kLevelNameBufSize buffer owned by
// NET_GNS_Transport.cpp.
ENGINE_API extern char* g_ConnectedLevelName; // empty string if the hello never arrived

IC void PumpClient()
{
    if (g_PumpClient)
        g_PumpClient();
}

IC void ClientDisconnect()
{
    if (g_ClientDisconnect)
        g_ClientDisconnect();
}

IC bool ClientConnectionLost()
{
    return g_ClientLost ? g_ClientLost() : false;
}

// Round-trip ping to the server in ms, or -1 when not connected / no backend.
IC int ClientPing()
{
    return g_ClientPing ? g_ClientPing() : -1;
}
} // namespace NET_GNS

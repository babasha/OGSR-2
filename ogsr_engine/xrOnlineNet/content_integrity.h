#pragma once

#include "../xr_3da/Engine.h" // ENGINE_API

// Content fingerprint, salted by a value the asking side chooses.
//
// The point of the salt: a fingerprint computed over fixed bytes can be answered
// from a table of pre-computed replies, so it proves nothing. Here the asker
// picks a nonce, the answer covers byte windows that only that nonce selects,
// and a process that no longer has the original bytes cannot produce it.
//
// What this DOES catch: content edited on disk while the program that reads it is
// stock -- retuned configs, repainted textures, swapped models. That is the
// common case and it is cheap to check.
//
// What this does NOT catch, and no client-side check can: a program modified to
// report whatever the other side wants to hear. Treat a passing fingerprint as
// "the easy path is closed", never as proof of anything.
namespace ContentIntegrity
{
struct Summary
{
    u32 files{}; // entries in the manifest
    u64 bytes{}; // their total size
    u32 list_crc{}; // over the sorted "path|size" list: catches added, removed or resized files
    u32 window_crc{}; // over the nonce-selected byte windows: catches edited contents
};

// Builds (once, then cached) the manifest of shipped content and digests the
// windows this nonce selects. Returns false if the content could not be read at
// all, which callers should treat as a failed check rather than a pass.
ENGINE_API bool Compute(u64 nonce, Summary& out);

ENGINE_API bool Equal(const Summary& a, const Summary& b);

// "files=N bytes=M list=0x... win=0x..." -- for logs on both sides, so a mismatch
// says WHICH half disagrees: a differing list_crc is a different file set, a
// differing window_crc alone is edited content.
ENGINE_API void Describe(const Summary& s, char* dst, u32 dst_size);

// How strictly a host treats the answer: 0 = do not ask at all, 1 = ask and log
// the verdict, 2 = ask and refuse the connection on mismatch. Console:
// sv_content_check. Scalar on purpose -- an ENGINE_API extern ARRAY does not
// survive this boundary reliably (see NET_GNS_Transport.h's g_ConnectedLevelName).
ENGINE_API extern int g_content_check_mode;

// How long a host waits for the answer before giving up on that connection.
ENGINE_API extern int g_content_check_timeout_ms;
} // namespace ContentIntegrity

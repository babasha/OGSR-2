////////////////////////////////////////////////////////////////////////////
//  A command channel INTO THE CLIENT.
//
//  ⭐WHY.
//
//  The server has had one since 28-07 (XROS_CONSOLE_WATCH: it polls a file and
//  executes every line appended to it), and it changed how fast the server
//  side could be worked on -- a world could be driven without a human at a
//  keyboard. The client never got the same thing, and the asymmetry quietly
//  set the pace of everything client-side: every client-side measurement had
//  to be started by a person typing into the console, so every experiment
//  needed a human awake, and a loop of "measure, change one number, measure
//  again" was simply not available.
//
//  This is that channel. With it the whole client-side loop -- switch a cvar,
//  start a recorder, run a scenario, read the numbers, change one constant --
//  runs unattended.
//
//     XROS_CLIENT_CONSOLE=<file>
//
//  ⚠Read from the END of the file at startup, not the beginning: a file left
//  over from the last run would otherwise replay its whole history into a
//  fresh world. (The server's channel learned this the hard way.)
//
//  ⚠And only up to the last newline, because the writer may be halfway
//  through a line when we look.
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "cl_console_watch.h"
#include "../xr_3da/XR_IOConsole.h"

namespace
{
bool s_checked = false;
bool s_active = false;
string_path s_path{};
long s_offset = 0;
u32 s_next_poll = 0;

// Same cadence as the server's channel: fast enough to feel immediate to a
// script, far too slow to matter to a frame.
constexpr u32 k_poll_ms = 200;

void open_channel()
{
    s_checked = true;

    const char* env = std::getenv("XROS_CLIENT_CONSOLE");
    if (!env || !env[0])
        return;

    xr_strcpy(s_path, env);

    // Start at the END: whatever is already in the file belonged to a previous
    // run and must not be replayed into this one.
    if (FILE* f = fopen(s_path, "rb"))
    {
        fseek(f, 0, SEEK_END);
        s_offset = ftell(f);
        fclose(f);
    }
    else
        s_offset = 0;

    s_active = true;
    Msg("~ [cl_console] watching '%s' (starting at byte %ld)", s_path, s_offset);
}
} // namespace

void ClientConsoleWatch::Tick()
{
    if (!s_checked)
        open_channel();
    if (!s_active)
        return;

    // ⚠⚠dwTimeContinual, NOT dwTimeGlobal, AND THE DIFFERENCE IS WHETHER THIS
    // CHANNEL WORKS IN THE MENU AT ALL. dwTimeGlobal comes from TimerGlobal,
    // which Device::Pause(bTimer=TRUE) STOPS -- and the main menu pauses exactly
    // that way. So the channel would open, print "watching", and then sit on a
    // deadline the clock could never reach: every command appended while the
    // menu was up was ignored, for ever, in silence. Found 18-08 trying to run
    // fs_extract from the start screen; the same shape was already recorded for
    // the "press any key" screen, which pauses the same way.
    // dwTimeContinual runs off TimerMM and is never paused.
    const u32 now = Device.dwTimeContinual;

    // ⚠The clock RESTARTS when a level loads, and a deadline stamped before
    // the load then sits permanently in the future -- the channel opens, says
    // it is watching, and never reads another line. Measured 29-07-2026:
    // loading a save through -load, the "watching" line printed during the
    // load screen and every command afterwards was ignored for ever.
    //
    // ⭐Only became reachable once this moved to CGamePersistent::OnFrame,
    // which runs during loading; from CLevel::OnFrame the first tick always
    // happened after the clock had been reset. A fix that exposes a latent
    // bug in its own neighbourhood is worth the comment.
    if (now + k_poll_ms < s_next_poll)
        s_next_poll = now; // the clock went backwards: resync, do not wait it out

    if (now < s_next_poll)
        return;
    s_next_poll = now + k_poll_ms;

    FILE* f = fopen(s_path, "rb");
    if (!f)
        return;

    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
    if (end <= s_offset)
    {
        // truncated or replaced: follow it rather than reading garbage
        if (end < s_offset)
            s_offset = end;
        fclose(f);
        return;
    }

    const long want = end - s_offset;
    xr_vector<char> buf;
    buf.resize(size_t(want) + 1);
    fseek(f, s_offset, SEEK_SET);
    const size_t got = fread(buf.data(), 1, size_t(want), f);
    fclose(f);
    if (!got)
        return;
    buf[got] = 0;

    // Only up to the LAST newline -- the tail may be a half-written line, and
    // executing half a command is worse than executing it a poll later.
    long consumed = 0;
    char* p = buf.data();
    char* line = p;
    for (size_t i = 0; i < got; ++i)
    {
        if (p[i] != '\n')
            continue;
        p[i] = 0;
        char* e = p + i;
        // strip CR and trailing blanks
        while (e > line && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        while (*line == ' ' || *line == '\t')
            ++line;

        if (*line && *line != '#')
        {
            Msg("~ [cl_console] > %s", line);
            Console->Execute(line);
        }
        consumed = long(i) + 1;
        line = p + i + 1;
    }

    s_offset += consumed;
}

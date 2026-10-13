#pragma once

// A command channel into the client, mirroring the server's XROS_CONSOLE_WATCH.
// See the .cpp for why the asymmetry mattered.
//
//    XROS_CLIENT_CONSOLE=<file>   append lines, they get executed
namespace ClientConsoleWatch
{
// Polled from CLevel::OnFrame. A no-op unless the env var is set.
void Tick();
} // namespace ClientConsoleWatch

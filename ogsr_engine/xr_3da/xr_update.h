#pragma once

// ⭐THE UPDATER (12-10). The game checks for a newer build on start, offers it in the main menu, downloads it next to
// itself and swaps the files in on the NEXT start -- Switch and Android players have no launcher and no Steam to do it.
//
// Where updates come from: GitHub Releases of the engine's public repository, babasha/OGSR-2 (xr_update.cpp). Each release carries a signed text manifest per
// platform (manifest-nx.txt / manifest-android.txt / manifest-pc.txt) and the files it lists as assets; the newest
// release is found through github.com/<repo>/releases/latest/download/<manifest>. See _tools/update/README.md for
// how a release is made.
//
// Trust: HTTPS on every platform (WinHTTP / the Switch's own ssl service / Java's HttpURLConnection), AND an Ed25519
// signature over the manifest checked against the public key compiled in here (xr_update.cpp), AND a BLAKE2b hash of
// every file checked before it is used. A copy of the repository's release rights alone cannot ship code to players.
//
// The one rule that shapes the whole design: NOTHING THE RUNNING GAME HAS OPEN IS EVER OVERWRITTEN. Downloads land in
// _update/staging, a journal (_update/pending.txt) is written once everything is verified, and the swap happens at the
// next start, in WinMain, before the file system mounts a single archive (ApplyPending). A crash or power cut at any
// point leaves either the old set or the journal to finish -- never half of each.
//
// Layout of _update/ next to fsgame.ltx (the working directory on every platform):
//   installed-<plat>.txt   the manifest of what is on disk now (what the next check diffs against)
//   dl/                    whole assets as downloaded (kept across runs: an interrupted download resumes per asset)
//   staging/               the new files, verified, at their final relative paths
//   pending.txt            the swap journal; its presence is what makes the next start apply
//   old/                   what the swap moved aside (deleted on the following start)
//   update.log             what the updater did, also on stderr

#include "Engine.h" // ENGINE_API

namespace XrUpdate
{
enum class State
{
    Idle, // nothing asked yet
    Checking, // fetching / verifying the manifest, comparing with the disk
    UpToDate,
    Offline, // the check could not reach the server (no network, no release yet): silent
    Available, // a newer build: Info says how much
    Downloading,
    Ready, // everything verified in staging, the journal written: restart (or install the APK) to finish
    Failed, // a check or a download went wrong in a way worth showing (bad signature, hash mismatch, disk)
};

struct Info
{
    State state{State::Idle};
    int build{}; // the offered build
    int installed_build{}; // the one on disk (0: unknown)
    char version[32]{}; // the offered version string, "0.2.0"
    char notes[1024]{}; // what is new, UTF-8, '\n' between lines
    unsigned long long need_bytes{}; // what the download will fetch
    unsigned long long done_bytes{}; // ...and how much of it already arrived
    unsigned files{}; // files to replace / add
    unsigned removed{}; // files to delete
    bool apk{}; // Android: a new APK is part of it (installed through the system installer)
    char error[256]{}; // State::Failed / Offline: why
};

// The background check, once per run (later calls are ignored unless force). Safe from any thread after Core is up.
ENGINE_API void StartCheck(bool force = false);
ENGINE_API void GetInfo(Info& out);
ENGINE_API void StartDownload();
ENGINE_API void Cancel(); // a download in flight stops; what is complete in dl/ stays for next time
// The player said "later": the menu will not offer it again this run.
ENGINE_API void Dismiss();
ENGINE_API bool Dismissed();

// Ready: quit and start again (the swap runs at that start). The menu executes `quit` right after.
ENGINE_API void RequestRestart();
// Ready on Android with an APK: hands it to the system installer (the game keeps running; Android stops it when the
// installer finishes). False: could not start the installer (Info.error says why -- e.g. the user must first allow
// this app to install apps, the settings page has been opened for that).
ENGINE_API bool InstallApk();

// ---- process lifetime (x_ray.cpp) ----
// The Switch's NRO path (argv[0]): what the manifest's "@self" means there.
void SetSelfPath(const char* argv0);
// Before Core: finish a downloaded update. Returns true when the running binary itself was replaced -- the caller
// should relaunch at once (OnExit with the restart flag) rather than run the old code over the new data.
bool ApplyPending();
// After WinMain_impl returned (the engine is torn down, the archives closed): relaunch if RequestRestart asked.
void OnExit();
// upd_auto / upd_check / upd_status (from CCC_Register).
void RegisterConsole();
} // namespace XrUpdate

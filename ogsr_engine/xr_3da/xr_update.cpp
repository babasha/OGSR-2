// The updater -- see xr_update.h for the design, _tools/update/README.md for making a release.
//
// ⚠ApplyPending runs BEFORE Core._initialize: nothing here may use the engine (no Msg, no FS, no xr_ allocations).
// Everything is std:: and the C library; the worker logs to _update/update.log and stderr, and the main thread copies
// its lines into the engine log when it polls (GetInfo).
#include "stdafx.h"
#include "xr_update.h"
#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"
#include "update/upd_http.h"
#include "update/upd_android.h"
#include "update/monocypher/monocypher.h"
#include "update/monocypher/monocypher-ed25519.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

int g_upd_auto = 1; // `upd_auto 0`: no check on start (the console's `upd_check` still works)

namespace XrUpdate
{
namespace
{
// ----------------------------------------------------------------- config ----
// The releases of the engine's public repository (OGSR-2, 13-10). ⚠Its NEWEST release must carry the three manifests:
// a release without them (a plain CI build of the engine) silences every player's check until the next one that has
// them -- make_release.py copies the other platforms' manifests into each release for that reason.
// _update/channel.txt (one line: a URL ending in .../download) points a test install somewhere else -- a beta
// repository, or http://<pc>:8000 serving a folder.
constexpr const char* kDefaultChannel = "https://github.com/babasha/OGSR-2/releases/latest/download";

// The Ed25519 key the manifests are signed with (_tools/update/make_release.py keygen; the private half is on the
// release machine only). A manifest this key did not sign is never acted on.
const uint8_t kPublicKey[32] = {
    0xba, 0xb2, 0x42, 0x51, 0xe1, 0x81, 0xa8, 0x17,
    0xa3, 0x6f, 0xf0, 0xe1, 0x53, 0x08, 0xda, 0xfa,
    0x6b, 0xa8, 0x8d, 0x46, 0x64, 0xa4, 0x66, 0x95,
    0x5c, 0xcb, 0x46, 0xbd, 0xa8, 0x93, 0x10, 0x58,
};

#if defined(__SWITCH__)
constexpr const char* kPlatform = "nx";
#elif defined(__ANDROID__)
constexpr const char* kPlatform = "android";
#elif defined(_WIN32)
constexpr const char* kPlatform = "pc";
#else
constexpr const char* kPlatform = "linux";
#endif

// The game's root: the folder of fsgame.ltx. The working directory on the Switch and Android; on the PC the exe runs
// from bin_x64 and the root is one up. Every path of a manifest, and _update/ itself, is relative to it.
const std::string& root()
{
    static const std::string r = [] {
        auto has = [](const char* p) {
            FILE* f = fopen(p, "rb");
            if (f)
                fclose(f);
            return f != nullptr;
        };
        return has("fsgame.ltx") ? std::string() : has("../fsgame.ltx") ? std::string("../") : std::string();
    }();
    return r;
}
const char* const kDirName = "_update/";
std::string udir() { return root() + kDirName; }
std::string installed_name() { return udir() + "installed-" + kPlatform + ".txt"; }
std::string new_name() { return udir() + "new-" + kPlatform + ".txt"; }
std::string pending_name() { return udir() + "pending.txt"; }
std::string staging_dir() { return udir() + "staging/"; }
std::string dl_dir() { return udir() + "dl/"; }
std::string old_dir() { return udir() + "old/"; }

std::string s_self; // Switch: the running NRO (argv[0])

// -------------------------------------------------------------------- log ----
std::mutex s_log_lock;
std::vector<std::string> s_log_queue; // for the engine log, drained on the main thread

void ulog(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[upd] %s\n", line);
    std::lock_guard<std::mutex> g(s_log_lock);
    if (FILE* f = fopen((udir() + "update.log").c_str(), "a"))
    {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    if (s_log_queue.size() < 200)
        s_log_queue.emplace_back(line);
}

// ------------------------------------------------------------------- disk ----
using u64x = unsigned long long;

long long file_size(const std::string& p)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p.c_str(), &st) != 0 || (st.st_mode & _S_IFDIR))
        return -1;
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || S_ISDIR(st.st_mode))
        return -1;
#endif
    return (long long)st.st_size;
}

bool exists(const std::string& p) { return file_size(p) >= 0; }

int fseek64(FILE* f, long long off)
{
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

void make_dirs_for(const std::string& file)
{
    // Every parent of the file, in order. Failures are left to the fopen/rename that follows to report.
    for (size_t i = 1; i < file.size(); ++i)
    {
        if (file[i] != '/')
            continue;
        if (file[i - 1] == ':') // "sdmc:/"
            continue;
        const std::string dir = file.substr(0, i);
#ifdef _WIN32
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0777);
#endif
    }
}

bool move_file(const std::string& from, const std::string& to)
{
    // ⚠Windows: a running .exe/.dll can be RENAMED (not overwritten, not deleted) -- which is all the swap ever does.
    // Retried: an antivirus scanner holds a fresh file for a moment.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
#ifdef _WIN32
        if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
            return true;
        Sleep(100);
#else
        if (rename(from.c_str(), to.c_str()) == 0)
            return true;
        // ⚠The Switch's SD (FAT under Horizon) does not replace an existing target the way POSIX rename does: the second
        // update ever applied left installed-nx.txt at the build before and offered the same build again (13-10).
        if (access(to.c_str(), F_OK) == 0 && access(from.c_str(), F_OK) == 0)
        {
            remove(to.c_str());
            if (rename(from.c_str(), to.c_str()) == 0)
                return true;
        }
        usleep(100 * 1000);
#endif
    }
    return false;
}

void remove_tree(const std::string& dir)
{
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::string n = fd.cFileName;
            if (n == "." || n == "..")
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(dir + n + "/");
            else
                DeleteFileA((dir + n).c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    _rmdir(dir.c_str());
#else
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            const std::string n = e->d_name;
            if (n == "." || n == "..")
                continue;
            const std::string p = dir + n;
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                remove_tree(p + "/");
            else
                remove(p.c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
#endif
}

bool read_all(const std::string& p, std::string& out)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

bool write_all(const std::string& p, const std::string& data)
{
    make_dirs_for(p);
    const std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok)
    {
        remove(tmp.c_str());
        return false;
    }
    return move_file(tmp, p);
}

// -------------------------------------------------------------- hashing ----
struct Hash
{
    uint8_t b[32]{};
    bool operator==(const Hash& o) const { return memcmp(b, o.b, 32) == 0; }
    bool operator!=(const Hash& o) const { return !(*this == o); }
};

bool parse_hex(const char* s, size_t n, uint8_t* out)
{
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < n; ++i)
    {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = uint8_t(hi * 16 + lo);
    }
    return true;
}

// BLAKE2b-256 of [off, off+size) of a file; with copy_to, the bytes are also written there. stop: abandon.
bool hash_range(const std::string& path, long long off, long long size, Hash& out, FILE* copy_to, const volatile int* stop)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    if (off && fseek64(f, off) != 0)
    {
        fclose(f);
        return false;
    }
    static thread_local std::vector<uint8_t> buf(1 << 20);
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);
    long long left = size;
    bool ok = true;
    while (left > 0)
    {
        if (stop && *stop)
        {
            ok = false;
            break;
        }
        const size_t want = size_t(left < (long long)buf.size() ? left : (long long)buf.size());
        const size_t got = fread(buf.data(), 1, want, f);
        if (got != want)
        {
            ok = false;
            break;
        }
        crypto_blake2b_update(&ctx, buf.data(), got);
        if (copy_to && fwrite(buf.data(), 1, got, copy_to) != got)
        {
            ok = false;
            break;
        }
        left -= (long long)got;
    }
    fclose(f);
    crypto_blake2b_final(&ctx, out.b);
    return ok;
}

// -------------------------------------------------------------- manifest ----
//   ogsr-update 1
//   platform nx | android | pc
//   build <n>                       increasing per platform
//   version <text>
//   base <url>                      asset URL = base + "/" + src
//   notes <text>                    repeated, one line each (UTF-8)
//   apk <versionCode> <src>         android: the APK, an asset like any other
//   a <hash> <size> <src>           every asset the files below come from (src = "<tag>/<name>")
//   f <hash> <size> <src> <off> <path>   a file: bytes [off, off+size) of asset src
//   sig <128 hex>                   Ed25519 over every byte before this line -- always the last line
struct Asset
{
    Hash hash;
    long long size{};
};
struct Entry
{
    Hash hash;
    long long size{};
    std::string src;
    long long off{};
};
struct Manifest
{
    std::string platform, version, base, notes;
    int build{};
    long long apk_code{-1};
    std::string apk_src;
    std::map<std::string, Asset> assets;
    std::map<std::string, Entry> files; // by path
};

// The game only ever writes inside its own folder, whatever a manifest says.
bool safe_path(const std::string& p)
{
    if (p == "@self")
        return !s_self.empty();
    if (p.empty() || p.size() > 400 || p[0] == '/' || p.compare(0, strlen(kDirName), kDirName) == 0)
        return false;
    size_t seg = 0;
    for (size_t i = 0; i <= p.size(); ++i)
    {
        const char c = i < p.size() ? p[i] : '/';
        if (c == '/')
        {
            const std::string s = p.substr(seg, i - seg);
            if (s.empty() || s == "." || s == "..")
                return false;
            seg = i + 1;
        }
        else if (c < 0x20 || c > 0x7e || c == '\\' || c == ':')
            return false;
    }
    return true;
}

std::string disk_path(const std::string& p) { return p == "@self" ? s_self : root() + p; }

bool parse_manifest(const std::string& text, bool verify, Manifest& m, std::string& why)
{
    m = Manifest{};
    size_t body_end = text.size();
    if (verify)
    {
        const size_t at = text.rfind("\nsig ");
        if (at == std::string::npos || text.size() - (at + 5) < 128)
        {
            why = "unsigned manifest";
            return false;
        }
        uint8_t sig[64];
        if (!parse_hex(text.c_str() + at + 5, 64, sig))
        {
            why = "bad signature line";
            return false;
        }
        body_end = at + 1;
        if (crypto_ed25519_check(sig, kPublicKey, reinterpret_cast<const uint8_t*>(text.data()), body_end) != 0)
        {
            why = "BAD SIGNATURE";
            return false;
        }
    }
    size_t pos = 0;
    int line_no = 0;
    while (pos < body_end)
    {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos || nl > body_end)
            nl = body_end;
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line_no++ == 0)
        {
            if (line != "ogsr-update 1")
            {
                why = "not an update manifest";
                return false;
            }
            continue;
        }
        const size_t sp = line.find(' ');
        const std::string key = line.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        auto fields = [&](int n, std::vector<std::string>& out) {
            // n-1 space-separated fields, the n-th takes the rest of the line (paths may hold spaces)
            out.clear();
            size_t q = 0;
            for (int i = 0; i < n - 1; ++i)
            {
                const size_t e = rest.find(' ', q);
                if (e == std::string::npos)
                    return false;
                out.push_back(rest.substr(q, e - q));
                q = e + 1;
            }
            out.push_back(rest.substr(q));
            return !out.back().empty();
        };
        std::vector<std::string> f;
        if (key == "platform")
            m.platform = rest;
        else if (key == "build")
            m.build = atoi(rest.c_str());
        else if (key == "version")
            m.version = rest.substr(0, 31);
        else if (key == "base")
            m.base = rest;
        else if (key == "notes")
            m.notes += (m.notes.empty() ? "" : "\n") + rest;
        else if (key == "apk" && fields(2, f))
        {
            m.apk_code = atoll(f[0].c_str());
            m.apk_src = f[1];
        }
        else if (key == "a" && fields(3, f) && f[0].size() == 64)
        {
            Asset a;
            if (!parse_hex(f[0].c_str(), 32, a.hash.b))
                continue;
            a.size = atoll(f[1].c_str());
            m.assets[f[2]] = a;
        }
        else if (key == "f" && fields(5, f) && f[0].size() == 64)
        {
            Entry e;
            if (!parse_hex(f[0].c_str(), 32, e.hash.b))
                continue;
            e.size = atoll(f[1].c_str());
            e.src = f[2];
            e.off = atoll(f[3].c_str());
            if (!safe_path(f[4]))
            {
                why = "unsafe path in manifest: " + f[4];
                return false;
            }
            const auto a = m.assets.find(e.src);
            if (a == m.assets.end() || e.off < 0 || e.size < 0 || e.off + e.size > a->second.size)
            {
                why = "file outside its asset: " + f[4];
                return false;
            }
            m.files[f[4]] = e;
        }
    }
    if (verify && m.platform != kPlatform)
    {
        why = "manifest is for '" + m.platform + "'";
        return false;
    }
    if (!m.apk_src.empty() && !m.assets.count(m.apk_src))
    {
        why = "apk asset missing";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ state ----
std::mutex s_lock; // guards s_info, s_plan
Info s_info;
std::atomic<bool> s_busy{false};
std::atomic<bool> s_checked{false};
std::atomic<bool> s_dismissed{false};
std::atomic<bool> s_restart{false};
std::thread s_worker;
upd_progress s_prog{};
std::atomic<long long> s_done_base{0}; // bytes of the assets finished before the one in flight

struct Plan
{
    Manifest m;
    std::string manifest_text; // as downloaded, signature included
    std::vector<std::string> replace; // paths
    std::vector<std::string> remove;
    std::vector<std::string> assets; // srcs to fetch
    bool apk{};
};
Plan s_plan;
std::string s_apk_path; // Ready on Android: the verified APK

void set_state(State st, const char* err = nullptr)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_info.state = st;
    if (err)
        snprintf(s_info.error, sizeof(s_info.error), "%s", err);
}

std::string channel()
{
    std::string c;
    if (read_all(udir() + "channel.txt", c))
    {
        while (!c.empty() && (c.back() == '\n' || c.back() == '\r' || c.back() == ' ' || c.back() == '/'))
            c.pop_back();
        if (!c.empty())
            return c;
    }
    return kDefaultChannel;
}

std::string dl_name(const std::string& src)
{
    std::string n = src;
    for (char& c : n)
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '&' || c == '=' || c == '%')
            c = '_';
    if (n.size() > 120) // a full URL: FAT on the Switch's SD caps a name at 255, keep well under it
        n = n.substr(n.size() - 120);
    return dl_dir() + n;
}

// ------------------------------------------------------------------ check ----
void run_check()
{
    make_dirs_for(dl_dir() + "x");
    ulog("check: platform %s, channel %s", kPlatform, channel().c_str());

    // A finished download the player has not restarted for yet: nothing to fetch, just say so again.
    if (exists(pending_name()))
    {
        std::string text, why;
        Manifest m;
        if (read_all(new_name(), text) && parse_manifest(text, true, m, why))
        {
            std::lock_guard<std::mutex> g(s_lock);
            s_info.build = m.build;
            snprintf(s_info.version, sizeof(s_info.version), "%s", m.version.c_str());
            snprintf(s_info.notes, sizeof(s_info.notes), "%s", m.notes.c_str());
#ifdef __ANDROID__
            if (!m.apk_src.empty() && file_size(dl_name(m.apk_src)) == m.assets[m.apk_src].size && m.apk_code > upd_android_version_code())
            {
                s_apk_path = dl_name(m.apk_src);
                s_info.apk = true;
            }
#endif
            s_info.state = State::Ready;
            ulog("check: build %d is downloaded and waits for a restart", m.build);
            return;
        }
        ulog("check: a pending update without a valid manifest (%s) -- dropped", why.c_str());
        remove(pending_name().c_str());
        remove_tree(staging_dir());
    }

    make_dirs_for(dl_dir() + "x");
    const std::string tmp = udir() + "manifest.dl";
    char err[256] = {};
    upd_progress p{};
    const std::string url = channel() + "/manifest-" + kPlatform + ".txt";
    if (upd_http_get(url.c_str(), tmp.c_str(), &p, err, sizeof(err)) != 0)
    {
        ulog("check: %s -> %s", url.c_str(), err);
        set_state(State::Offline, err);
        return;
    }
    std::string text, why;
    read_all(tmp, text);
    remove(tmp.c_str());
    Manifest m;
    if (!parse_manifest(text, true, m, why))
    {
        ulog("check: manifest rejected: %s", why.c_str());
        set_state(State::Failed, why.c_str());
        return;
    }

    Manifest inst;
    std::string inst_text;
    const bool have_inst = read_all(installed_name(), inst_text) && parse_manifest(inst_text, false, inst, why);
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_info.installed_build = have_inst ? inst.build : 0;
    }
    // Android: the APK is compared with what is INSTALLED, not with the build number -- data can be current while
    // the player never confirmed the new APK.
    bool apk = false;
#ifdef __ANDROID__
    if (!m.apk_src.empty())
    {
        const long long have = upd_android_version_code();
        apk = m.apk_code > have;
        if (apk)
            ulog("check: APK %lld > installed %lld", m.apk_code, have);
    }
#endif
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_info.build = m.build;
        snprintf(s_info.version, sizeof(s_info.version), "%s", m.version.c_str());
    }
    if (have_inst && m.build <= inst.build && !apk)
    {
        ulog("check: up to date (build %d)", inst.build);
        set_state(State::UpToDate);
        return;
    }

    // What differs. With an installed manifest, a file whose recorded hash matches and whose size on disk agrees is
    // taken as is -- no reading of gigabytes on every start. Without one (the first check of an install that came
    // from a zip / an SD copy), files of the right size are hashed once to find out.
    Plan plan;
    long long need = 0;
    std::map<std::string, bool> want_asset;
    unsigned hashed = 0;
    const bool data_current = have_inst && m.build <= inst.build;
    for (const auto& [path, e] : m.files)
    {
        if (data_current)
            break;
        if (s_prog.cancel)
            return set_state(State::Idle);
        const std::string dp = disk_path(path);
        const long long sz = file_size(dp);
        bool same = false;
        if (sz == e.size)
        {
            const auto it = inst.files.find(path);
            if (have_inst && it != inst.files.end())
                same = it->second.hash == e.hash;
            else
            {
                Hash h;
                same = hash_range(dp, 0, sz, h, nullptr, &s_prog.cancel) && h == e.hash;
                ++hashed;
            }
        }
        if (same)
            continue;
        plan.replace.push_back(path);
        want_asset[e.src] = true;
    }
    for (const auto& [path, e] : inst.files)
        if (!data_current && !m.files.count(path) && exists(disk_path(path)))
            plan.remove.push_back(path);
    if (apk)
    {
        plan.apk = true;
        want_asset[m.apk_src] = true;
    }
    for (const auto& [src, yes] : want_asset)
    {
        plan.assets.push_back(src);
        const Asset& a = m.assets[src];
        if (file_size(dl_name(src)) != a.size) // a complete one from an interrupted run is checked, not fetched
            need += a.size;
    }
    if (hashed)
        ulog("check: hashed %u files to compare (no installed manifest yet)", hashed);

    if (plan.replace.empty() && plan.remove.empty() && !plan.apk)
    {
        // Nothing to fetch (an install made from this very build): remember it as installed.
        write_all(installed_name(), text);
        ulog("check: build %d matches the disk -- recorded as installed", m.build);
        set_state(State::UpToDate);
        return;
    }

    ulog("check: build %d (%s) offered: %u files, %u removed, %s%.1f MB to download", m.build, m.version.c_str(), unsigned(plan.replace.size()),
         unsigned(plan.remove.size()), plan.apk ? "APK, " : "", double(need) / 1048576.0);
    std::lock_guard<std::mutex> g(s_lock);
    s_info.build = m.build;
    snprintf(s_info.version, sizeof(s_info.version), "%s", m.version.c_str());
    snprintf(s_info.notes, sizeof(s_info.notes), "%s", m.notes.c_str());
    s_info.need_bytes = u64x(need);
    s_info.done_bytes = 0;
    s_info.files = unsigned(plan.replace.size());
    s_info.removed = unsigned(plan.remove.size());
    s_info.apk = plan.apk;
    s_info.state = State::Available;
    plan.m = std::move(m);
    plan.manifest_text = std::move(text);
    s_plan = std::move(plan);
}

// --------------------------------------------------------------- download ----
bool fetch_asset(const Plan& plan, const std::string& src, std::string& err)
{
    const Asset& a = plan.m.assets.at(src);
    const std::string dst = dl_name(src);
    Hash h;
    if (file_size(dst) == a.size && hash_range(dst, 0, a.size, h, nullptr, &s_prog.cancel) && h == a.hash)
    {
        s_done_base += a.size;
        ulog("download: %s already here", src.c_str());
        return true;
    }
    const std::string part = dst + ".part";
    // A src may also be a full URL: game data can live somewhere other than the engine's releases without a new
    // client (the ones already out there cannot be changed). The hash check makes the host irrelevant to trust.
    const bool absolute = src.compare(0, 8, "https://") == 0 || src.compare(0, 7, "http://") == 0;
    const std::string url = absolute ? src : plan.m.base + "/" + src;
    char e[256] = {};
    s_prog.done = 0;
    if (upd_http_get(url.c_str(), part.c_str(), &s_prog, e, sizeof(e)) != 0)
    {
        err = src + ": " + e;
        return false;
    }
    if (file_size(part) != a.size || !hash_range(part, 0, a.size, h, nullptr, &s_prog.cancel) || h != a.hash)
    {
        remove(part.c_str());
        err = src + ": damaged in transit (hash mismatch)";
        return false;
    }
    if (!move_file(part, dst))
    {
        err = src + ": cannot move into place";
        return false;
    }
    s_prog.done = 0;
    s_done_base += a.size;
    ulog("download: %s ok (%.1f MB)", src.c_str(), double(a.size) / 1048576.0);
    return true;
}

void run_download()
{
    Plan plan;
    {
        std::lock_guard<std::mutex> g(s_lock);
        plan = s_plan;
    }
    s_done_base = 0;
    std::string err;
    make_dirs_for(dl_dir() + "x");

    for (const std::string& src : plan.assets)
    {
        if (!fetch_asset(plan, src, err))
        {
            ulog("download: %s", err.c_str());
            set_state(s_prog.cancel ? State::Available : State::Failed, err.c_str());
            return;
        }
    }

    // Out of the assets into staging, each file verified on its way.
    std::map<std::string, int> uses;
    for (const std::string& path : plan.replace)
        ++uses[plan.m.files.at(path).src];
    for (const std::string& path : plan.replace)
    {
        const Entry& e = plan.m.files.at(path);
        const std::string from = dl_name(e.src);
        const std::string to = staging_dir() + path;
        make_dirs_for(to);
        const Asset& a = plan.m.assets.at(e.src);
        if (e.off == 0 && e.size == a.size && --uses[e.src] == 0)
        {
            if (!move_file(from, to)) // a whole asset used once: no copy (the archives are GBs on an SD card)
            {
                err = "cannot stage " + path;
                break;
            }
            continue;
        }
        FILE* out = fopen(to.c_str(), "wb");
        Hash h;
        const bool ok = out && hash_range(from, e.off, e.size, h, out, &s_prog.cancel);
        if (out && fclose(out) != 0)
            err = "disk full? " + path;
        if (!ok || h != e.hash)
        {
            err = "cannot stage " + path;
            break;
        }
    }
    if (!err.empty())
    {
        ulog("download: %s", err.c_str());
        set_state(State::Failed, err.c_str());
        return;
    }

    // The journal last: until it exists, nothing at the next start will touch the game's files.
    std::string journal = "ogsr-pending 1\n";
    for (const std::string& p : plan.replace)
        journal += "r " + p + "\n";
    for (const std::string& p : plan.remove)
        journal += "d " + p + "\n";
    if (!write_all(new_name(), plan.manifest_text) || !write_all(pending_name(), journal))
    {
        set_state(State::Failed, "cannot write the update journal");
        return;
    }
    if (plan.apk)
        s_apk_path = dl_name(plan.m.apk_src);
    else
        remove_tree(dl_dir());
    ulog("download: build %d ready (%u files staged)", plan.m.build, unsigned(plan.replace.size()));
    set_state(State::Ready);
}

void spawn(void (*job)())
{
    if (s_busy.exchange(true))
        return;
    if (s_worker.joinable())
        s_worker.join();
    s_prog.cancel = 0;
    s_worker = std::thread([job] {
        job();
        s_busy = false;
    });
}
} // namespace

// ================================================================= public ====

void StartCheck(bool force)
{
    if (!force && (s_checked || !g_upd_auto || strstr(Core.Params, "-noupdate")))
        return;
    s_checked = true;
    set_state(State::Checking);
    spawn(run_check);
}

void GetInfo(Info& out)
{
    {
        std::lock_guard<std::mutex> g(s_log_lock);
        for (const std::string& l : s_log_queue)
            Msg("~ [upd] %s", l.c_str());
        s_log_queue.clear();
    }
#ifdef __ANDROID__
    upd_android_poll();
#endif
    std::lock_guard<std::mutex> g(s_lock);
    out = s_info;
    if (out.state == State::Downloading)
        out.done_bytes = u64x(s_done_base.load() + s_prog.done);
}

void StartDownload()
{
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (s_info.state != State::Available && s_info.state != State::Failed)
            return;
        if (s_plan.assets.empty() && s_plan.replace.empty() && s_plan.remove.empty())
            return;
        s_info.state = State::Downloading;
        s_info.error[0] = 0;
    }
    spawn(run_download);
}

void Cancel() { s_prog.cancel = 1; }
void Dismiss() { s_dismissed = true; }
bool Dismissed() { return s_dismissed; }
void RequestRestart() { s_restart = true; }

bool InstallApk()
{
#ifdef __ANDROID__
    char err[256] = {};
    const int rc = s_apk_path.empty() ? -1 : upd_android_install_apk(s_apk_path.c_str(), err, sizeof(err));
    if (rc != 0)
    {
        std::lock_guard<std::mutex> g(s_lock);
        snprintf(s_info.error, sizeof(s_info.error), "%s", s_apk_path.empty() ? "no APK downloaded" : err);
    }
    ulog("install APK %s: %d %s", s_apk_path.c_str(), rc, err);
    return rc == 0;
#else
    return false;
#endif
}

void SetSelfPath(const char* argv0) { s_self = argv0 ? argv0 : ""; }

bool ApplyPending()
{
    // What the previous swap set aside (a replaced .exe on Windows could not be deleted while it ran).
    if (!exists(pending_name()))
    {
        remove_tree(old_dir());
        return false;
    }
    std::string journal;
    if (!read_all(pending_name(), journal) || journal.compare(0, 15, "ogsr-pending 1\n") != 0)
    {
        ulog("apply: unreadable journal -- dropped");
        remove(pending_name().c_str());
        return false;
    }
    ulog("apply: start");

    struct Done
    {
        std::string path, target, aside; // aside empty: the target did not exist before
        bool removed{};
    };
    std::vector<Done> done;
    bool binary = false, failed = false;
    size_t pos = 15;
    while (pos < journal.size() && !failed)
    {
        size_t nl = journal.find('\n', pos);
        if (nl == std::string::npos)
            nl = journal.size();
        const std::string line = journal.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.size() < 3 || line[1] != ' ')
            continue;
        const std::string path = line.substr(2);
        if (!safe_path(path))
        {
            ulog("apply: unsafe path %s skipped", path.c_str());
            continue;
        }
        const std::string target = disk_path(path);
        if (line[0] == 'r')
        {
            const std::string staged = staging_dir() + path;
            if (!exists(staged))
                continue; // moved in by an earlier start that was cut short
            Done d{path, target, {}, false};
            if (exists(target))
            {
                d.aside = old_dir() + path;
                make_dirs_for(d.aside);
                if (!move_file(target, d.aside))
                {
                    ulog("apply: cannot move %s aside", target.c_str());
                    failed = true;
                    break;
                }
            }
            make_dirs_for(target);
            if (!move_file(staged, target))
            {
                ulog("apply: cannot move %s into place", target.c_str());
                if (!d.aside.empty())
                    move_file(d.aside, target);
                failed = true;
                break;
            }
            done.push_back(d);
            const size_t n = path.size();
            if (path == "@self" || (n > 4 && (path.compare(n - 4, 4, ".exe") == 0 || path.compare(n - 4, 4, ".dll") == 0)))
                binary = true;
        }
        else if (line[0] == 'd' && exists(target))
        {
            Done d{path, target, old_dir() + path, true};
            make_dirs_for(d.aside);
            if (move_file(target, d.aside))
                done.push_back(d);
        }
    }

    if (failed)
    {
        // All or nothing for this start: put back what moved, keep the journal and staging for the next one.
        for (auto it = done.rbegin(); it != done.rend(); ++it)
        {
            if (!it->removed)
                move_file(it->target, staging_dir() + it->path); // back to staging
            if (!it->aside.empty())
                move_file(it->aside, it->target);
        }
        ulog("apply: FAILED, rolled back %u files; will retry at the next start", unsigned(done.size()));
        return false;
    }
    move_file(new_name(), installed_name());
    remove(pending_name().c_str());
    remove_tree(staging_dir());
    remove_tree(old_dir());
    ulog("apply: done, %u files%s", unsigned(done.size()), binary ? " (the program itself: relaunching)" : "");
    return binary;
}

void OnExit()
{
    s_prog.cancel = 1;
    for (int i = 0; i < 30 && s_busy; ++i) // a transfer stops at its next chunk
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (s_worker.joinable())
    {
        if (s_busy)
            s_worker.detach(); // stuck in a blocking read: it only touches its own files, let the exit take it
        else
            s_worker.join();
    }
    if (!s_restart)
        return;
    ulog("restart");
#if defined(_WIN32)
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
        ulog("restart: CreateProcess failed %lu", GetLastError());
#elif defined(__SWITCH__)
    if (s_self.empty() || upd_nx_relaunch(s_self.c_str()) != 0)
        ulog("restart: this loader cannot chain-load -- start the game again from the menu");
#elif defined(__ANDROID__)
    if (!upd_android_schedule_restart())
        ulog("restart: could not schedule -- start the game again");
    fflush(nullptr);
    _exit(0); // ⚠SDL cannot run SDL_main twice in one process: the restart must find this one gone
#else
    ulog("restart: start the game again");
#endif
}

// ---------------------------------------------------------------- console ----
namespace
{
class CCC_UpdCheck : public IConsole_Command
{
public:
    CCC_UpdCheck(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    void Execute(LPCSTR) override
    {
        s_dismissed = false;
        StartCheck(true);
    }
};

class CCC_UpdStatus : public IConsole_Command
{
public:
    CCC_UpdStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    void Execute(LPCSTR) override
    {
        XrUpdate::Info i;
        GetInfo(i);
        static const char* names[] = {"idle", "checking", "up to date", "offline", "available", "downloading", "ready", "failed"};
        Msg("[upd] %s: platform %s, installed build %d, offered %d (%s), %.1f/%.1f MB, %u files, %u removed%s%s%s", names[int(i.state)], kPlatform,
            i.installed_build, i.build, i.version, double(i.done_bytes) / 1048576.0, double(i.need_bytes) / 1048576.0, i.files, i.removed, i.apk ? ", APK" : "",
            i.error[0] ? " -- " : "", i.error);
    }
};
} // namespace

void RegisterConsole()
{
    CMD4(CCC_Integer, "upd_auto", &g_upd_auto, 0, 1);
    CMD1(CCC_UpdCheck, "upd_check");
    CMD1(CCC_UpdStatus, "upd_status");
}
} // namespace XrUpdate

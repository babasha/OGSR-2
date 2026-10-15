#include "stdafx.h"
#include "content_integrity.h"
#include "../xrCore/crc32.h"

namespace ContentIntegrity
{
// Default: ask and log. Refusing joins is a policy decision an operator makes
// once the reference fingerprint is known to be right -- turning it on blind
// would lock out every player over a content difference nobody has looked at.
ENGINE_API int g_content_check_mode = 1;
ENGINE_API int g_content_check_timeout_ms = 15000;

namespace
{
struct Entry
{
    string_path rel; // lower-cased, backslash-separated, relative to the content root
    u64 size;
};

struct Manifest
{
    bool built{false};
    bool usable{false};
    string_path root{};
    xr_vector<Entry> files;
    u64 total_bytes{};
    u32 list_crc{};
};

Manifest g_manifest;

// splitmix64: one multiply-xor chain per draw, no state to seed badly. Both sides
// must walk the SAME sequence from the same nonce, so this is deliberately a
// fixed algorithm here rather than anything from the standard library, whose
// generators are not guaranteed identical across implementations.
u64 next_random(u64& state)
{
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void append_entry(xr_vector<Entry>& out, LPCSTR rel, u64 size)
{
    Entry e;
    strcpy_s(e.rel, sizeof(e.rel), rel);
    xr_strlwr(e.rel);
    for (char* p = e.rel; *p; ++p)
        if (*p == '/')
            *p = '\\';
    e.size = size;
    out.push_back(e);
}

void scan_directory(xr_vector<Entry>& out, LPCSTR root, LPCSTR rel_dir, u32 depth)
{
    if (depth > 16) // content trees are not this deep; a symlink loop is
        return;

    string_path pattern;
    if (rel_dir[0])
        sprintf_s(pattern, sizeof(pattern), "%s%s\\*", root, rel_dir);
    else
        sprintf_s(pattern, sizeof(pattern), "%s*", root);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (INVALID_HANDLE_VALUE == h)
        return;

    do
    {
        if (0 == xr_strcmp(fd.cFileName, ".") || 0 == xr_strcmp(fd.cFileName, ".."))
            continue;

        string_path rel;
        if (rel_dir[0])
            sprintf_s(rel, sizeof(rel), "%s\\%s", rel_dir, fd.cFileName);
        else
            strcpy_s(rel, sizeof(rel), fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            scan_directory(out, root, rel, depth + 1);
        else
        {
            const u64 size = (u64(fd.nFileSizeHigh) << 32) | u64(fd.nFileSizeLow);
            append_entry(out, rel, size);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

void scan_archives(xr_vector<Entry>& out, LPCSTR root)
{
    // The mod ships as gamedata.db* archives next to the loose tree; both are
    // content and both must be covered.
    string_path pattern;
    sprintf_s(pattern, sizeof(pattern), "%s*.db*", root);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (INVALID_HANDLE_VALUE == h)
        return;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const u64 size = (u64(fd.nFileSizeHigh) << 32) | u64(fd.nFileSizeLow);
        append_entry(out, fd.cFileName, size);
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

bool entry_less(const Entry& a, const Entry& b) { return xr_strcmp(a.rel, b.rel) < 0; }

// Built once: the shipped content does not change while the process runs, and a
// server would otherwise re-walk the whole tree for every connecting client.
const Manifest& manifest()
{
    if (g_manifest.built)
        return g_manifest;

    g_manifest.built = true;

    FS_Path* P = FS.get_path("$fs_root$");
    if (!P)
    {
        Msg("![content] no $fs_root$ -- content fingerprint unavailable");
        return g_manifest;
    }
    strcpy_s(g_manifest.root, sizeof(g_manifest.root), P->m_Path);

    scan_archives(g_manifest.files, g_manifest.root);

    string_path gamedata;
    sprintf_s(gamedata, sizeof(gamedata), "%sgamedata", g_manifest.root);
    if (INVALID_FILE_ATTRIBUTES != GetFileAttributesA(gamedata))
        scan_directory(g_manifest.files, g_manifest.root, "gamedata", 0);

    std::sort(g_manifest.files.begin(), g_manifest.files.end(), entry_less);

    // list_crc covers names AND sizes, so an added, removed or resized file moves
    // it without a single byte being read.
    u32 crc = 0;
    for (const Entry& e : g_manifest.files)
    {
        g_manifest.total_bytes += e.size;
        string_path line;
        sprintf_s(line, sizeof(line), "%s|%llu\n", e.rel, (unsigned long long)e.size);
        crc = crc32_fast(line, xr_strlen(line), crc);
    }
    g_manifest.list_crc = crc;
    g_manifest.usable = !g_manifest.files.empty();

    Msg("* [content] manifest: %u files, %llu bytes, list=0x%08x, root='%s'", (u32)g_manifest.files.size(), (unsigned long long)g_manifest.total_bytes,
        g_manifest.list_crc, g_manifest.root);

    return g_manifest;
}

// Bounded on purpose: enough windows that editing any one file is likely to be
// sampled over a few connects, few enough that the check costs milliseconds.
const u32 kWindows = 48;
const u32 kWindowBytes = 32 * 1024;
} // namespace

bool Compute(u64 nonce, Summary& out)
{
    const Manifest& m = manifest();
    out = Summary{};
    if (!m.usable)
        return false;

    out.files = (u32)m.files.size();
    out.bytes = m.total_bytes;
    out.list_crc = m.list_crc;

    u64 rng = nonce ^ 0xD1B54A32D192ED03ull;
    u32 crc = crc32_fast(&nonce, sizeof(nonce), 0); // salt the digest itself, not just the choice of windows
    u8* buffer = (u8*)xr_malloc(kWindowBytes);
    u32 read_ok = 0;

    for (u32 i = 0; i < kWindows && m.total_bytes; ++i)
    {
        // Pick a file weighted by size, so big archives get looked at as often as
        // they contribute bytes instead of every tiny config being equally likely.
        const u64 target = next_random(rng) % m.total_bytes;
        u64 acc = 0;
        const Entry* pick = nullptr;
        for (const Entry& e : m.files)
        {
            acc += e.size;
            if (acc > target)
            {
                pick = &e;
                break;
            }
        }
        if (!pick || !pick->size)
            continue;

        const u64 span = (pick->size > kWindowBytes) ? (pick->size - kWindowBytes) : 0;
        const u64 offset = span ? (next_random(rng) % span) : 0;
        const u32 want = (u32)_min((u64)kWindowBytes, pick->size - offset);

        string_path full;
        sprintf_s(full, sizeof(full), "%s%s", m.root, pick->rel);

        FILE* f = nullptr;
        if (0 != fopen_s(&f, full, "rb") || !f)
            continue;
        if (0 == _fseeki64(f, (__int64)offset, SEEK_SET))
        {
            const size_t got = fread(buffer, 1, want, f);
            if (got)
            {
                // The path and offset go into the digest as well: without them two
                // different files with identical bytes would be interchangeable.
                crc = crc32_fast(pick->rel, xr_strlen(pick->rel), crc);
                crc = crc32_fast(&offset, sizeof(offset), crc);
                crc = crc32_fast(buffer, (u32)got, crc);
                ++read_ok;
            }
        }
        fclose(f);
    }

    xr_free(buffer);

    if (!read_ok)
    {
        Msg("![content] could not read any content window -- fingerprint unavailable");
        return false;
    }

    out.window_crc = crc;
    return true;
}

bool Equal(const Summary& a, const Summary& b)
{
    return a.files == b.files && a.bytes == b.bytes && a.list_crc == b.list_crc && a.window_crc == b.window_crc;
}

void Describe(const Summary& s, char* dst, u32 dst_size)
{
    if (!dst || !dst_size)
        return;
    sprintf_s(dst, dst_size, "files=%u bytes=%llu list=0x%08x win=0x%08x", s.files, (unsigned long long)s.bytes, s.list_crc, s.window_crc);
}
} // namespace ContentIntegrity

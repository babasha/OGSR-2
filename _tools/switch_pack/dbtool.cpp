// dbtool — reads X-Ray .db archives (chunked, LZHuf+encrypted index, LZO entries), and rebuilds their
// contents into a directory tree for the Switch package, dropping what Kordon does not need and stripping
// the top mip levels off big DDS textures. Output trees are packed with mksquashfs (zstd) afterwards: the
// engine detects a squashfs archive by magic (CLocatorAPI::archive::open) and mounts it like a .db.
//
// Reuses the engine's own LzHuf.cpp (copied next to this file, compiled against shim/stdafx.h) and LZO
// (3rd_party/Src/lzo), so the decoding is the engine's, not a re-implementation.
//
//   dbtool list    <archive.db>
//   dbtool extract <archive.db> <outdir> [--skip <prefix>]... [--ddsmax N]
//   dbtool dir     <srcdir>     <outdir> [--skip <prefix>]... [--ddsmax N]     (loose gamedata folder)
//
// <prefix> matches the lower-cased archive-relative path, backslashes: e.g. levels\l05_bar\
#include "stdafx.h" // the shim: uint8_t/u32 types for the engine's LzHuf
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "ddsenc.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "lzo/lzo1x.h"
}

bool _decompressLZ(uint8_t** dest, size_t* dest_sz, void* src, size_t src_sz, size_t total_size);

namespace fs = std::filesystem;

// ---------------------------------------------------------------- trivial_encryptor (xrCore/trivial_encryptor.cpp)
struct Rand32
{
    uint32_t seed;
    uint32_t next(uint32_t range)
    {
        seed = 0x08088405u * seed + 1;
        return uint32_t(uint64_t(seed) * uint64_t(range) >> 32);
    }
};
struct Key { uint32_t iters, tseed, eseed; };
static const Key kWorldwide{1024, 6011979, 24031979}, kRussian{2048, 20091958, 20031955};

struct Cipher
{
    uint8_t fwd[256], back[256];
    Key k;
    explicit Cipher(Key key) : k(key)
    {
        for (int i = 0; i < 256; ++i) fwd[i] = (uint8_t)i;
        Rand32 r{k.tseed};
        for (uint32_t i = 0; i < k.iters; ++i)
        {
            uint32_t j = r.next(256), kk = r.next(256);
            while (j == kk) kk = r.next(256);
            std::swap(fwd[j], fwd[kk]);
        }
        for (int i = 0; i < 256; ++i) back[fwd[i]] = (uint8_t)i;
    }
    void decode(const uint8_t* s, size_t n, uint8_t* d) const
    {
        Rand32 r{k.eseed};
        for (size_t i = 0; i < n; ++i) d[i] = back[s[i] ^ uint8_t(r.next(256) & 0xff)];
    }
};

// ---------------------------------------------------------------- archive reading
struct Entry { std::string name; uint32_t size_real, size_compr, ptr; };

struct Mapped
{
    const uint8_t* p = nullptr;
    size_t n = 0;
    int fd = -1;
    bool open(const char* path)
    {
        fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;
        struct stat st{};
        fstat(fd, &st);
        n = (size_t)st.st_size;
        p = (const uint8_t*)mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
        return p != MAP_FAILED;
    }
};

static bool read_index(const Mapped& m, std::vector<Entry>& out)
{
    size_t off = 0;
    while (off + 8 <= m.n)
    {
        uint32_t type, size;
        memcpy(&type, m.p + off, 4);
        memcpy(&size, m.p + off + 4, 4);
        off += 8;
        if ((type & 0x7fffffffu) == 1)
        {
            std::vector<uint8_t> data(m.p + off, m.p + off + size), plain;
            if (type & 0x80000000u)
            {
                // engine order: worldwide key, then russian key, then no cipher at all
                static const Cipher ww(kWorldwide), ru(kRussian);
                bool ok = false;
                for (int attempt = 0; attempt < 3 && !ok; ++attempt)
                {
                    std::vector<uint8_t> tmp(data);
                    if (attempt == 0) ww.decode(data.data(), size, tmp.data());
                    if (attempt == 1) ru.decode(data.data(), size, tmp.data());
                    uint8_t* dest = nullptr;
                    size_t dsz = 0;
                    if (_decompressLZ(&dest, &dsz, tmp.data(), size, m.n) && dest)
                    {
                        plain.assign(dest, dest + dsz);
                        free(dest);
                        ok = true;
                        fprintf(stderr, "  index: %s key\n", attempt == 0 ? "worldwide" : attempt == 1 ? "russian" : "no");
                    }
                }
                if (!ok) return false;
            }
            else
                plain = std::move(data);

            size_t o = 0;
            while (o + 2 <= plain.size())
            {
                uint16_t bsz;
                memcpy(&bsz, &plain[o], 2);
                o += 2;
                if (bsz < 16 || o + bsz > plain.size()) return false;
                const uint8_t* b = &plain[o];
                Entry e;
                memcpy(&e.size_real, b, 4);
                memcpy(&e.size_compr, b + 4, 4);
                const size_t nl = bsz - 16;
                e.name.assign((const char*)b + 12, nl);
                memcpy(&e.ptr, b + 12 + nl, 4);
                out.push_back(std::move(e));
                o += bsz;
            }
            return true;
        }
        off += size;
    }
    return false;
}

static bool entry_bytes(const Mapped& m, const Entry& e, std::vector<uint8_t>& out)
{
    if ((uint64_t)e.ptr + e.size_compr > m.n) return false;
    if (e.size_real == e.size_compr)
    {
        out.assign(m.p + e.ptr, m.p + e.ptr + e.size_real);
        return true;
    }
    out.resize(e.size_real);
    lzo_uint got = e.size_real;
    const int rc = lzo1x_decompress_safe(m.p + e.ptr, e.size_compr, out.data(), &got, nullptr);
    return rc == LZO_E_OK && got == e.size_real;
}

// ---------------------------------------------------------------- DDS mip stripping
// Only classic FourCC block formats (DXT1/3/5, ATI1/BC4, ATI2/BC5), no DX10 header, not cube/volume.
static bool dds_strip(std::vector<uint8_t>& d, uint32_t maxdim, uint32_t* w_out = nullptr, uint32_t* h_out = nullptr)
{
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return false;
    auto rd = [&](size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; };
    auto wr = [&](size_t o, uint32_t v) { memcpy(&d[o], &v, 4); };
    const uint32_t h = rd(12), w = rd(16), mips = rd(28) ? rd(28) : 1, caps2 = rd(112);
    if (w_out) *w_out = w;
    if (h_out) *h_out = h;
    if (caps2 & (0x200 | 0x200000)) return false; // cubemap / volume
    const char* cc = (const char*)&d[84];
    uint32_t block;
    if (!memcmp(cc, "DXT1", 4) || !memcmp(cc, "ATI1", 4) || !memcmp(cc, "BC4U", 4)) block = 8;
    else if (!memcmp(cc, "DXT3", 4) || !memcmp(cc, "DXT5", 4) || !memcmp(cc, "ATI2", 4) || !memcmp(cc, "BC5U", 4)) block = 16;
    else return false; // DX10 / uncompressed: leave alone
    uint32_t k = 0, cw = w, ch = h;
    while (std::max(cw, ch) > maxdim && k + 1 < mips) { cw = std::max(1u, cw >> 1); ch = std::max(1u, ch >> 1); ++k; }
    if (!k) return false;
    size_t skip = 0;
    uint32_t lw = w, lh = h;
    for (uint32_t i = 0; i < k; ++i)
    {
        skip += (size_t)std::max(1u, (lw + 3) / 4) * std::max(1u, (lh + 3) / 4) * block;
        lw = std::max(1u, lw >> 1);
        lh = std::max(1u, lh >> 1);
    }
    if (128 + skip > d.size()) return false;
    d.erase(d.begin() + 128, d.begin() + 128 + skip);
    wr(12, ch); wr(16, cw); wr(28, mips - k);
    wr(20, (uint32_t)((size_t)std::max(1u, (cw + 3) / 4) * std::max(1u, (ch + 3) / 4) * block)); // linear size of the new top level
    wr(8, rd(8) | 0x00081007u); // CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE
    return true;
}


// ---------------------------------------------------------------- DDS transcode (uncompressed -> DXT1/DXT5)
// Uncompressed DDS (A8R8G8B8 / X8R8G8B8 / R8G8B8 / 16-bit / L8 / A8) with a side >= bcmin is re-encoded to block compression
// and capped to maxdim, in the same pass. The source file is never modified: this runs while the package is built, so
// the original install and every mod's own archives stay intact and keep working on PC.
struct TcStats { double psnrSum = 0, psnrMin = 99; std::string worst; uint64_t files = 0, before = 0, after = 0; std::map<std::string, uint64_t> byFmt; std::vector<std::pair<uint64_t, std::string>> big; };
static TcStats g_tc;
struct OpaqueStats { uint64_t files = 0, saved = 0; std::vector<std::string> saved_by_name; };
static OpaqueStats g_opaque;
// histogram of the DDS files that leave the tool, by longest side (what actually lands on the SD card)
static std::map<uint32_t, std::pair<uint64_t, uint64_t>> g_hist;

static bool dds_transcode(std::vector<uint8_t>& d, const std::string& name, uint32_t maxdim, uint32_t bcmin)
{
    if (!bcmin || d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return false;
    auto rd = [&](size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; };
    auto wr = [&](std::vector<uint8_t>& v, size_t o, uint32_t x) { memcpy(&v[o], &x, 4); };
    const uint32_t h = rd(12), w = rd(16), mips = rd(28) ? rd(28) : 1, caps2 = rd(112), pfFlags = rd(80), bits = rd(88);
    if (caps2 & (0x200 | 0x200000)) return false;       // cubemap / volume
    if (pfFlags & 0x4) return false;                      // FourCC: already block compressed (or DX10)
    if (std::max(w, h) < bcmin) return false;
    ddsenc::PixFmt f; f.bits = bits; f.rm = rd(92); f.gm = rd(96); f.bm = rd(100); f.am = (pfFlags & 0x1) ? rd(104) : 0;
    if (pfFlags & 0x20000) { f.lum = true; }
    else if ((pfFlags & 0x2) && !(pfFlags & 0x40)) { f.alphaOnly = true; f.am = rd(104); }
    else if (!(pfFlags & 0x40)) return false;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return false;

    // decode the levels we keep (skip the top ones beyond the cap)
    uint32_t k = 0, cw = w, ch = h;
    if (mips > 1) while (std::max(cw, ch) > maxdim && k + 1 < mips) { cw = std::max(1u, cw >> 1); ch = std::max(1u, ch >> 1); ++k; }
    size_t off = 128;
    std::vector<ddsenc::Img> lv;
    {
        uint32_t lw = w, lh = h;
        for (uint32_t i = 0; i < mips; ++i)
        {
            const size_t lsz = (size_t)lw * lh * (bits / 8);
            if (off + lsz > d.size()) return false;
            if (i >= k)
            {
                ddsenc::Img im; size_t used = 0;
                if (!ddsenc::decodeUncompressed(&d[off], d.size() - off, lw, lh, f, im, used)) return false;
                lv.push_back(std::move(im));
            }
            off += lsz;
            lw = std::max(1u, lw >> 1); lh = std::max(1u, lh >> 1);
        }
    }
    if (mips == 1) while (maxdim && std::max(lv[0].w, lv[0].h) > maxdim) lv[0] = ddsenc::boxHalf(lv[0]);   // no chain in the file: just shrink
    bool alpha = f.alphaOnly;
    if (!alpha && f.am) for (auto& im : lv) { for (size_t i = 3; i < im.rgba.size(); i += 4) if (im.rgba[i] != 255) { alpha = true; break; } if (alpha) break; }

    std::vector<uint8_t> nd(d.begin(), d.begin() + 128);
    for (auto& im : lv) ddsenc::encodeImage(im, alpha, nd);
    const size_t bs = alpha ? 16 : 8;
    wr(nd, 12, lv[0].h); wr(nd, 16, lv[0].w); wr(nd, 28, (uint32_t)lv.size());
    wr(nd, 20, (uint32_t)((size_t)std::max(1u, (lv[0].w + 3) / 4) * std::max(1u, (lv[0].h + 3) / 4) * bs));
    wr(nd, 8, (rd(8) & ~0x8u) | 0x000A1007u);             // + LINEARSIZE + MIPMAPCOUNT, no PITCH
    wr(nd, 80, 0x4);                                       // pixel format: FourCC only
    memcpy(&nd[84], alpha ? "DXT5" : "DXT1", 4);
    wr(nd, 88, 0); wr(nd, 92, 0); wr(nd, 96, 0); wr(nd, 100, 0); wr(nd, 104, 0);
    wr(nd, 108, 0x1000 | (lv.size() > 1 ? 0x400008u : 0u)); // TEXTURE (+ COMPLEX|MIPMAP)

    char fmt[64]; snprintf(fmt, sizeof fmt, "%ubpp%s -> %s", bits, f.lum ? " lum" : f.alphaOnly ? " a8" : f.am ? " argb" : " rgb", alpha ? "DXT5" : "DXT1");
    g_tc.files++; g_tc.before += d.size(); g_tc.after += nd.size(); g_tc.byFmt[fmt] += d.size() - nd.size();
    {   // self-check: decode the top level we just wrote and measure it against the input
        ddsenc::Img back; ddsenc::decodeBlocks(&nd[128], lv[0].w, lv[0].h, alpha, back);
        const double ps = ddsenc::psnr(lv[0], back, alpha);
        g_tc.psnrSum += ps; if (ps < g_tc.psnrMin) { g_tc.psnrMin = ps; g_tc.worst = name; }
    }
    g_tc.big.emplace_back(d.size() - nd.size(), name);
    d.swap(nd);
    return true;
}

// ---------------------------------------------------------------- rules
struct Rules
{
    std::vector<std::string> skip;
    uint32_t ddsmax = 0; // 0 = off
    uint32_t bcmin = 0;  // 0 = off: transcode uncompressed DDS whose longest side is >= this
    std::vector<std::pair<std::string, std::string>> skipExt; // (path prefix, file extension): e.g. textures\\intro\\ + .ogm
    bool skipped(const std::string& n) const
    {
        for (auto& s : skip)
            if (n.compare(0, s.size(), s) == 0) return true;
        for (auto& pe : skipExt)
            if (n.compare(0, pe.first.size(), pe.first) == 0 && n.size() >= pe.second.size() &&
                n.compare(n.size() - pe.second.size(), pe.second.size(), pe.second) == 0) return true;
        return false;
    }
    // textures\ only, never the UI / sky (pixel-addressed atlases and cubemaps) and never below the cap
    bool dds_candidate(const std::string& n) const
    {
        if (!ddsmax || n.size() < 5 || n.compare(n.size() - 4, 4, ".dds") != 0) return false;
        if (n.compare(0, 9, "textures\\") != 0) return false;
        return n.compare(0, 12, "textures\\ui\\") != 0 && n.compare(0, 14, "textures\\sky\\") != 0;
    }
};

struct Stats
{
    uint64_t in_files = 0, out_files = 0, in_bytes = 0, out_bytes = 0, skipped_files = 0, skipped_bytes = 0, dds_files = 0, dds_saved = 0;
    std::map<std::string, std::pair<uint64_t, uint64_t>> top; // top folder -> (files, kept bytes)
};

static void write_out(const fs::path& root, const std::string& name, const std::vector<uint8_t>& data, Stats& st)
{
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    fs::path p = root / rel;
    fs::create_directories(p.parent_path());
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", p.c_str()); exit(2); }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    st.out_files++;
    st.out_bytes += data.size();
    const size_t s = name.find('\\');
    auto& t = st.top[s == std::string::npos ? "(root)" : name.substr(0, s)];
    t.first++;
    t.second += data.size();
}

static void handle(const Rules& r, const std::string& nameIn, std::vector<uint8_t>& data, const fs::path& out, Stats& st)
{
    std::string name = nameIn;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (!name.empty() && name.back() == '\\') // folder record in the index: make the directory, nothing to write
    {
        std::string rel = name;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (!r.skipped(name)) fs::create_directories(out / rel);
        return;
    }
    st.in_files++;
    st.in_bytes += data.size();
    if (r.skipped(name)) { st.skipped_files++; st.skipped_bytes += data.size(); return; }
    if (r.dds_candidate(name))
    {
        const size_t before = data.size();
        if (dds_transcode(data, name, r.ddsmax, r.bcmin)) { st.dds_files++; st.dds_saved += before - data.size(); }
        else if (dds_strip(data, r.ddsmax)) { st.dds_files++; st.dds_saved += before - data.size(); }
        if (r.bcmin)   // same switch as the transcoder: "optimise textures" mode
        {
            const size_t b2 = data.size();
            if (ddsenc::dropOpaqueAlpha(data)) { g_opaque.files++; g_opaque.saved += b2 - data.size(); if (g_opaque.saved_by_name.size() < 6) g_opaque.saved_by_name.push_back(name); }
        }
    }
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dds") == 0 && data.size() >= 128 && !memcmp(data.data(), "DDS ", 4))
    {
        uint32_t hh, ww; memcpy(&hh, &data[12], 4); memcpy(&ww, &data[16], 4);
        auto& hb = g_hist[std::max(ww, hh)]; hb.first++; hb.second += data.size();
    }
    write_out(out, name, data, st);
}

static void report(const Stats& st)
{
    printf("in : %llu files, %.2f GB\n", (unsigned long long)st.in_files, st.in_bytes / 1e9);
    printf("out: %llu files, %.2f GB\n", (unsigned long long)st.out_files, st.out_bytes / 1e9);
    printf("skipped: %llu files, %.2f GB | DDS stripped: %llu files, saved %.2f GB\n", (unsigned long long)st.skipped_files, st.skipped_bytes / 1e9,
           (unsigned long long)st.dds_files, st.dds_saved / 1e9);
    if (g_opaque.files)
        printf("DXT3/DXT5 with fully opaque alpha -> DXT1 (lossless): %llu files, saved %.1f MB (e.g. %s)\n", (unsigned long long)g_opaque.files, g_opaque.saved / 1e6,
               g_opaque.saved_by_name.empty() ? "" : g_opaque.saved_by_name[0].c_str());
    if (!g_hist.empty())
    {
        printf("DDS output by longest side:\n");
        for (auto& kv : g_hist) printf("  %5u px %6llu files %9.1f MB\n", kv.first, (unsigned long long)kv.second.first, kv.second.second / 1e6);
    }
    if (g_tc.files)
    {
        printf("DDS transcoded (uncompressed -> DXT): %llu files, %.1f MB -> %.1f MB\n", (unsigned long long)g_tc.files, g_tc.before / 1e6, g_tc.after / 1e6);
        printf("  PSNR vs input: mean %.1f dB, worst %.1f dB (%s)\n", g_tc.psnrSum / g_tc.files, g_tc.psnrMin, g_tc.worst.c_str());
        for (auto& kv : g_tc.byFmt) printf("  %-24s saved %8.1f MB\n", kv.first.c_str(), kv.second / 1e6);
        std::sort(g_tc.big.rbegin(), g_tc.big.rend());
        for (size_t i = 0; i < g_tc.big.size() && i < 8; ++i) printf("  top: %8.1f MB  %s\n", g_tc.big[i].first / 1e6, g_tc.big[i].second.c_str());
    }
    std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> v(st.top.begin(), st.top.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.second > b.second.second; });
    for (size_t i = 0; i < v.size() && i < 12; ++i)
        printf("  %-18s %8llu files %8.3f GB\n", v[i].first.c_str(), (unsigned long long)v[i].second.first, v[i].second.second / 1e9);
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: dbtool list|extract|dir ...\n"); return 1; }
    const std::string cmd = argv[1];
    Rules rules;
    for (int i = 4; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--skip") && i + 1 < argc) rules.skip.push_back(argv[++i]);
        else if (!strcmp(argv[i], "--ddsmax") && i + 1 < argc) rules.ddsmax = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skipext") && i + 2 < argc) { rules.skipExt.emplace_back(argv[i + 1], argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--bcmin") && i + 1 < argc) rules.bcmin = (uint32_t)atoi(argv[++i]);
    }
    if (cmd == "list" || cmd == "extract")
    {
        Mapped m;
        if (!m.open(argv[2])) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
        std::vector<Entry> idx;
        if (!read_index(m, idx)) { fprintf(stderr, "cannot read index of %s\n", argv[2]); return 1; }
        if (cmd == "list")
        {
            for (auto& e : idx) printf("%u\t%u\t%s\n", e.size_real, e.size_compr, e.name.c_str());
            fprintf(stderr, "%zu entries\n", idx.size());
            return 0;
        }
        Stats st;
        const fs::path out = argv[3];
        for (auto& e : idx)
        {
            std::vector<uint8_t> data;
            if (!entry_bytes(m, e, data)) { fprintf(stderr, "bad entry %s\n", e.name.c_str()); return 3; }
            handle(rules, e.name, data, out, st);
        }
        report(st);
        return 0;
    }
    if (cmd == "dir")
    {
        Stats st;
        const fs::path src = argv[2], out = argv[3];
        for (auto& de : fs::recursive_directory_iterator(src))
        {
            if (!de.is_regular_file()) continue;
            std::string rel = fs::relative(de.path(), src).string();
            std::replace(rel.begin(), rel.end(), '/', '\\');
            std::vector<uint8_t> data(de.file_size());
            FILE* f = fopen(de.path().c_str(), "rb");
            if (!f || fread(data.data(), 1, data.size(), f) != data.size()) { fprintf(stderr, "read fail %s\n", de.path().c_str()); return 3; }
            fclose(f);
            handle(rules, rel, data, out, st);
        }
        report(st);
        return 0;
    }
    return 1;
}

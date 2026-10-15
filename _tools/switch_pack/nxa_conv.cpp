// nxa_conv: .ogg -> .nxa (DSP-ADPCM, see ogsr_engine/xrSound/SoundRender_Adpcm.h) for the Switch pack.
// 11-10-2026, "layer B": short sounds play straight out of memory on the console's audio DSP, no CPU decode.
//
//   nxa_conv stat <dir>                          durations / sizes of every .ogg under dir (what a threshold costs)
//   nxa_conv conv <in_dir> <out_dir> [max_sec [shard nshards [min_snr]]]   every .ogg not longer than max_sec (default 8) and
//                                                encoding at >= min_snr dB (default 20) -> out_dir/<same>.nxa
//                                                (shard k of n: every n-th file, for n processes at once)
//   nxa_conv test <file.ogg> <out_prefix>        encode+decode one file: SNR, and <prefix>_ref.wav / _nxa.wav to listen
//   nxa_conv index <out_dir> [ogg_dir]          <out_dir>/sounds/nxa_index.bin from the .nxa there + the .ogg of ogg_dir
//                                                that have no .nxa (headers, see SoundRender_Adpcm.h)
//
// Build (WSL): bash _tools/switch_pack/build_nxa_conv.sh  (vendored vorbis + the engine's codec)
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <vorbis/vorbisfile.h>

#include "stdafx.h" // the stand-in build_nxa_conv.sh puts next to the codec
#include "SoundRender_Adpcm.h"

namespace fs = std::filesystem;

struct Pcm
{
    u32 rate = 0, channels = 0, samples = 0; // samples per channel
    std::vector<s16> data; // interleaved
    bool has_params = false;
    float min_dist = 1.f, max_dist = 300.f, base_volume = 1.f, max_ai_dist = 300.f;
    u32 game_type = 0;
};

static bool read_ogg(const fs::path& p, Pcm& out, bool want_pcm)
{
    OggVorbis_File vf;
    if (ov_fopen(p.string().c_str(), &vf) != 0)
        return false;
    vorbis_info* vi = ov_info(&vf, -1);
    out.rate = u32(vi->rate);
    out.channels = u32(vi->channels);
    out.samples = u32(ov_pcm_total(&vf, -1));

    // X-Ray's ogg comment (the same versions SoundRender_Source_loader.cpp reads)
    vorbis_comment* vc = ov_comment(&vf, -1);
    if (vc && vc->comments && vc->comment_lengths[0] >= 4)
    {
        const u8* c = reinterpret_cast<const u8*>(vc->user_comments[0]);
        const int len = vc->comment_lengths[0];
        auto rd_u32 = [&](int off) { u32 v; memcpy(&v, c + off, 4); return v; };
        auto rd_f = [&](int off) { float v; memcpy(&v, c + off, 4); return v; };
        const u32 vers = rd_u32(0);
        if (vers == 1 && len >= 16)
        {
            out.min_dist = rd_f(4); out.max_dist = rd_f(8); out.base_volume = 1.f; out.game_type = rd_u32(12); out.max_ai_dist = out.max_dist;
            out.has_params = true;
        }
        else if (vers == 2 && len >= 20)
        {
            out.min_dist = rd_f(4); out.max_dist = rd_f(8); out.base_volume = rd_f(12); out.game_type = rd_u32(16); out.max_ai_dist = out.max_dist;
            out.has_params = true;
        }
        else if (vers == 3 && len >= 24)
        {
            out.min_dist = rd_f(4); out.max_dist = rd_f(8); out.base_volume = rd_f(12); out.game_type = rd_u32(16); out.max_ai_dist = rd_f(20);
            out.has_params = true;
        }
    }

    if (want_pcm)
    {
        out.data.resize(size_t(out.samples) * out.channels);
        size_t got = 0;
        const size_t want = out.data.size() * 2;
        int bs = 0;
        char* dst = reinterpret_cast<char*>(out.data.data());
        while (got < want)
        {
            long r = ov_read(&vf, dst + got, int(std::min<size_t>(want - got, 1 << 16)), 0, 2, 1, &bs);
            if (r == OV_HOLE || r == OV_EBADLINK)
                continue;
            if (r <= 0)
                break;
            got += size_t(r);
        }
        if (got < want)
            memset(dst + got, 0, want - got);
    }
    ov_clear(&vf);
    return true;
}

static std::vector<u8> make_nxa(const Pcm& p)
{
    const u32 stride = nxa::stride_for(p.samples);
    std::vector<u8> f(sizeof(nxa::FileHeader) + nxa::payload_size(p.channels, p.samples), 0);
    nxa::FileHeader h{};
    h.magic = nxa::magic;
    h.version = nxa::version;
    h.sample_rate = p.rate;
    h.channels = p.channels;
    h.samples = p.samples;
    h.data_stride = stride;
    h.has_params = p.has_params ? 1 : 0;
    h.min_dist = p.min_dist; h.max_dist = p.max_dist; h.base_volume = p.base_volume; h.game_type = p.game_type; h.max_ai_dist = p.max_ai_dist;
    memcpy(f.data(), &h, sizeof h);
    u8* payload = f.data() + sizeof h;
    for (u32 c = 0; c < p.channels; ++c)
    {
        s16 coefs[16];
        nxa::encode(p.data.data() + c, p.channels, p.samples, coefs, payload + nxa::coef_block + c * stride);
        memcpy(payload + c * 64, coefs, sizeof coefs);
    }
    return f;
}

static void write_wav(const fs::path& p, const s16* d, u32 frames, u32 ch, u32 rate)
{
    FILE* f = fopen(p.string().c_str(), "wb");
    if (!f)
        return;
    const u32 bytes = frames * ch * 2;
    auto w32 = [&](u32 v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](u16 v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(u16(ch)); w32(rate); w32(rate * ch * 2); w16(u16(ch * 2)); w16(16);
    fwrite("data", 1, 4, f); w32(bytes); fwrite(d, 1, bytes, f);
    fclose(f);
}

static std::vector<s16> decode_nxa(const std::vector<u8>& f)
{
    nxa::FileHeader h;
    memcpy(&h, f.data(), sizeof h);
    const u8* payload = f.data() + sizeof h;
    std::vector<s16> out(size_t(h.samples) * h.channels);
    for (u32 c = 0; c < h.channels; ++c)
        nxa::decode(nxa::data(payload, c, h.data_stride), nxa::coefs(payload, c), h.samples, 0, h.samples, out.data() + c, h.channels);
    return out;
}

static double snr_db(const std::vector<s16>& a, const std::vector<s16>& b)
{
    double s = 0, n = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        s += double(a[i]) * a[i];
        const double d = double(a[i]) - b[i];
        n += d * d;
    }
    return n > 0 ? 10.0 * std::log10(s / n) : 999.0;
}

static std::vector<fs::path> oggs_under(const fs::path& dir)
{
    std::vector<fs::path> v;
    for (auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file())
        {
            std::string ext = e.path().extension().string();
            for (auto& ch : ext)
                ch = char(tolower(ch));
            if (ext == ".ogg")
                v.push_back(e.path());
        }
    std::sort(v.begin(), v.end());
    return v;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: nxa_conv stat <dir> | conv <in> <out> [max_sec] | test <ogg> <prefix>\n");
        return 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "stat")
    {
        // buckets by duration: count, ogg bytes, ADPCM bytes
        const double edges[] = {0.5, 1, 2, 3, 5, 8, 12, 20, 40, 1e9};
        struct B { u64 n = 0, ogg = 0, adpcm = 0; double sec = 0; } b[10];
        u64 stereo = 0, mono = 0;
        std::map<u32, u64> rates;
        for (auto& p : oggs_under(argv[2]))
        {
            Pcm pcm;
            if (!read_ogg(p, pcm, false))
            {
                printf("! cannot read %s\n", p.string().c_str());
                continue;
            }
            const double sec = pcm.rate ? double(pcm.samples) / pcm.rate : 0;
            int k = 0;
            while (sec > edges[k])
                ++k;
            b[k].n++;
            b[k].ogg += fs::file_size(p);
            b[k].adpcm += sizeof(nxa::FileHeader) + nxa::payload_size(pcm.channels, pcm.samples);
            b[k].sec += sec;
            (pcm.channels == 2 ? stereo : mono)++;
            rates[pcm.rate]++;
        }
        u64 n = 0, ogg = 0, ad = 0;
        printf("%-8s %7s %10s %10s %9s | cumulative ogg / adpcm MB\n", "<= sec", "files", "ogg MB", "adpcm MB", "minutes");
        for (int k = 0; k < 10; ++k)
        {
            n += b[k].n; ogg += b[k].ogg; ad += b[k].adpcm;
            printf("%-8g %7llu %10.1f %10.1f %9.1f | %8.1f %8.1f (%llu files)\n", edges[k], (unsigned long long)b[k].n, b[k].ogg / 1048576.0, b[k].adpcm / 1048576.0,
                   b[k].sec / 60.0, ogg / 1048576.0, ad / 1048576.0, (unsigned long long)n);
        }
        printf("mono %llu stereo %llu; rates:", (unsigned long long)mono, (unsigned long long)stereo);
        for (auto& r : rates)
            printf(" %u:%llu", r.first, (unsigned long long)r.second);
        printf("\n");
        return 0;
    }
    if (cmd == "test" && argc >= 4)
    {
        Pcm pcm;
        if (!read_ogg(argv[2], pcm, true))
            return 1;
        const auto f = make_nxa(pcm);
        const auto dec = decode_nxa(f);
        printf("%s: %u Hz %u ch %u samples, params %d (min %.2f max %.2f vol %.2f type 0x%x ai %.2f); nxa %zu bytes, SNR %.1f dB\n", argv[2], pcm.rate,
               pcm.channels, pcm.samples, int(pcm.has_params), pcm.min_dist, pcm.max_dist, pcm.base_volume, pcm.game_type, pcm.max_ai_dist, f.size(),
               snr_db(pcm.data, dec));
        write_wav(std::string(argv[3]) + "_ref.wav", pcm.data.data(), pcm.samples, pcm.channels, pcm.rate);
        write_wav(std::string(argv[3]) + "_nxa.wav", dec.data(), pcm.samples, pcm.channels, pcm.rate);
        return 0;
    }
    if (cmd == "index")
    {
        // every .nxa under <dir>/sounds -> <dir>/sounds/nxa_index.bin: the headers in one file, so the engine creates
        // a sound without opening it (nxa::IndexEntry; keys relative to sounds\, backslashes, no extension)
        const fs::path root = fs::path(argv[2]) / "sounds";
        std::vector<std::pair<std::string, nxa::IndexEntry>> all;
        for (auto& e : fs::recursive_directory_iterator(root))
        {
            if (!e.is_regular_file() || e.path().extension() != ".nxa")
                continue;
            FILE* f = fopen(e.path().string().c_str(), "rb");
            nxa::FileHeader h{};
            const bool ok = f && fread(&h, sizeof h, 1, f) == 1 && h.magic == nxa::magic;
            if (f)
                fclose(f);
            if (!ok)
            {
                fprintf(stderr, "bad %s\n", e.path().string().c_str());
                return 2;
            }
            std::string key = fs::relative(e.path(), root).replace_extension("").string();
            for (auto& ch : key)
                ch = ch == '/' ? '\\' : char(tolower(ch));
            nxa::IndexEntry ie{};
            ie.sample_rate = h.sample_rate, ie.channels = h.channels, ie.samples = h.samples, ie.data_stride = h.data_stride;
            ie.has_params = h.has_params, ie.min_dist = h.min_dist, ie.max_dist = h.max_dist, ie.base_volume = h.base_volume;
            ie.game_type = h.game_type, ie.max_ai_dist = h.max_ai_dist;
            all.emplace_back(key, ie);
        }
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first < b.first; });
        // index v2: the .ogg that stayed .ogg (longer, or not mono/stereo) -- same keys, format 1, read from their header
        if (argc >= 4)
        {
            const fs::path oroot = fs::path(argv[3]) / "sounds";
            std::vector<std::pair<std::string, nxa::IndexEntry>> oggs;
            for (auto& p : oggs_under(oroot))
            {
                std::string key = fs::relative(p, oroot).replace_extension("").string();
                for (auto& ch : key)
                    ch = ch == '/' ? '\\' : char(tolower(ch));
                if (std::binary_search(all.begin(), all.end(), std::make_pair(key, nxa::IndexEntry{}),
                                       [](const auto& a, const auto& b) { return a.first < b.first; }))
                    continue;
                Pcm pcm;
                if (!read_ogg(p, pcm, false) || !pcm.rate)
                    continue;
                nxa::IndexEntry ie{};
                ie.sample_rate = pcm.rate, ie.channels = pcm.channels, ie.samples = pcm.samples, ie.data_stride = 0;
                ie.has_params = pcm.has_params, ie.min_dist = pcm.min_dist, ie.max_dist = pcm.max_dist, ie.base_volume = pcm.base_volume;
                ie.game_type = pcm.game_type, ie.max_ai_dist = pcm.max_ai_dist, ie.format = 1;
                oggs.emplace_back(key, ie);
            }
            printf("index: %zu .ogg added\n", oggs.size());
            all.insert(all.end(), oggs.begin(), oggs.end());
            std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first < b.first; });
        }
        FILE* o = fopen((root / "nxa_index.bin").string().c_str(), "wb");
        if (!o)
            return 2;
        const u32 hdr[3] = {nxa::index_magic, nxa::index_version, u32(all.size())};
        fwrite(hdr, sizeof hdr, 1, o);
        for (auto& [k, ie] : all)
        {
            const u16 len = u16(k.size());
            fwrite(&len, 2, 1, o);
            fwrite(k.data(), 1, len, o);
            fwrite(&ie, sizeof ie, 1, o);
        }
        printf("index: %zu entries, %ld bytes\n", all.size(), ftell(o));
        fclose(o);
        return 0;
    }
    if (cmd == "conv" && argc >= 4)
    {
        const fs::path in = argv[2], out = argv[3];
        const double max_sec = argc >= 5 ? atof(argv[4]) : 8.0;
        const int shard = argc >= 7 ? atoi(argv[5]) : 0, nshards = argc >= 7 ? atoi(argv[6]) : 1;
        const double min_snr = argc >= 8 ? atof(argv[7]) : 20.0;
        u64 low = 0;
        size_t idx = 0;
        u64 n = 0, skipped = 0, bytes = 0, failed = 0;
        double snr_sum = 0, snr_min = 1e9;
        std::string snr_min_name;
        for (auto& p : oggs_under(in))
        {
            if (int(idx++ % size_t(nshards)) != shard)
                continue;
            Pcm pcm;
            if (!read_ogg(p, pcm, false))
            {
                ++failed;
                continue;
            }
            if (!pcm.rate || pcm.channels < 1 || pcm.channels > nxa::max_channels || double(pcm.samples) / pcm.rate > max_sec || pcm.samples == 0)
            {
                ++skipped;
                continue;
            }
            read_ogg(p, pcm, true);
            const auto f = make_nxa(pcm);
            fs::path rel = fs::relative(p, in);
            std::string rs = rel.string();
            for (auto& ch : rs)
                ch = char(tolower(ch));
            // 4-bit ADPCM cannot follow sharp transients in silence (drops, clicks: 14-18 dB): those stay .ogg
            const double snr = snr_db(pcm.data, decode_nxa(f));
            if (snr < min_snr)
            {
                ++low;
                fprintf(stderr, "low %.1f dB %s\n", snr, rs.c_str());
                continue;
            }
            fs::path dst = out / fs::path(rs).replace_extension(".nxa");
            fs::create_directories(dst.parent_path());
            FILE* o = fopen(dst.string().c_str(), "wb");
            if (!o)
            {
                ++failed;
                continue;
            }
            fwrite(f.data(), 1, f.size(), o);
            fclose(o);
            bytes += f.size();
            ++n;
            snr_sum += snr;
            if (snr < snr_min)
                snr_min = snr, snr_min_name = rs;
            if (n % 1000 == 0)
                fprintf(stderr, "%llu...\n", (unsigned long long)n);
        }
        printf("converted %llu (%.1f MB), skipped %llu (longer than %.1f s or not mono/stereo), %llu under %.0f dB SNR kept as .ogg, failed %llu; "
               "SNR mean %.1f dB, min %.1f dB (%s)\n",
               (unsigned long long)n, bytes / 1048576.0, (unsigned long long)skipped, max_sec, (unsigned long long)low, min_snr, (unsigned long long)failed,
               n ? snr_sum / n : 0.0, snr_min, snr_min_name.c_str());
        return failed ? 2 : 0;
    }
    fprintf(stderr, "bad command\n");
    return 1;
}

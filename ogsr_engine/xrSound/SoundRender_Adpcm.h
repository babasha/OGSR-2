#pragma once

// ⭐DSP-ADPCM and the .nxa container (11-10-2026, "layer B": the Switch sound on audren).
//
// The Switch's audio DSP decodes Nintendo's DSP-ADPCM itself: a voice can play straight out of memory with no
// CPU decode at all, where an .ogg costs the sound thread an open (FS + vorbis headers, 4.5-6.7 ms) and ~4 ms a
// block. So the short sounds are converted once, offline (tools/nxa_conv), into .nxa files: same name, next to the
// .ogg (a separate archive on the console), and CSoundRender_Source prefers them (snd_nxa 1).
//
// DSP-ADPCM: frames of 8 bytes = one header byte (predictor index << 4 | scale) + 14 four-bit samples. Each
// sample = (nibble << scale) + c0 * h0 + c1 * h1, coefficients Q11, eight (c0, c1) pairs per channel. A voice is
// one channel, so a stereo .nxa keeps each channel's data separately (two voices on the console).
//
// .nxa layout (little-endian):
//   NxaFileHeader                       256 bytes
//   payload:                            loaded as one block, 64-byte aligned (audren wants 0x40)
//     coefficients, channel c at c*64   128 bytes (16 x s16 each, the rest zero)
//     data, channel c at 128 + c*stride stride = frames * 8 rounded up to 64
// The payload is what the console keeps resident: one memcpy into the audren arena.

namespace nxa
{
constexpr u32 magic = 0x3141584E; // "NXA1"
constexpr u32 version = 1;
constexpr u32 samples_per_frame = 14;
constexpr u32 bytes_per_frame = 8;
constexpr u32 coef_block = 128; // payload offset of the data
constexpr u32 max_channels = 2;

#pragma pack(push, 1)
struct FileHeader
{
    u32 magic;
    u32 version;
    u32 sample_rate;
    u32 channels;
    u32 samples; // per channel
    u32 data_stride; // bytes per channel, 64-aligned
    // X-Ray's parameters, from the .ogg comment (has_params 0: there was none -- defaults)
    u32 has_params;
    float min_dist, max_dist, base_volume;
    u32 game_type;
    float max_ai_dist;
    u8 reserved[256 - 12 * 4];
};
#pragma pack(pop)
static_assert(sizeof(FileHeader) == 256, "nxa header");

// sounds\nxa_index.bin (nxa_conv index): every .nxa's header in one file -- magic, version, count, then per sound
// u16 key length, the key (relative to sounds\, backslashes, no extension), IndexEntry. A sound is created from it
// without opening its file (📏Switch au2: 54 sounds of a spawning NPC = 100-200 ms of r_open on the 2nd thread).
constexpr u32 index_magic = 0x4941584E; // "NXAI"
constexpr u32 index_version = 2; // 2: also the long .ogg (format 1) -- creating ANY archived sound opens no file
#pragma pack(push, 1)
struct IndexEntry
{
    u32 sample_rate, channels, samples, data_stride, has_params;
    float min_dist, max_dist, base_volume;
    u32 game_type;
    float max_ai_dist;
    u32 format; // 0 = .nxa (data_stride valid), 1 = .ogg (samples = PCM frames; read as before when it plays)
};
#pragma pack(pop)

inline u32 frames_for(u32 samples) { return (samples + samples_per_frame - 1) / samples_per_frame; }
inline u32 stride_for(u32 samples) { return (frames_for(samples) * bytes_per_frame + 63) & ~63u; }
inline u32 payload_size(u32 channels, u32 samples) { return coef_block + channels * stride_for(samples); }

// payload accessors
inline const s16* coefs(const u8* payload, u32 ch) { return reinterpret_cast<const s16*>(payload + ch * 64); }
inline const u8* data(const u8* payload, u32 ch, u32 stride) { return payload + coef_block + ch * stride; }

// Encode one channel (count samples, read with in_stride s16 steps) into coefs[16] (Q11) and frames_for(count)*8
// bytes of out. Offline (the converter); deterministic.
void encode(const s16* in, u32 in_stride, u32 count, s16 coefs_out[16], u8* out);

// Decode samples [start, start + count) of one channel into out (out_stride s16 steps). Starting anywhere but 0
// decodes a few frames before it from zero history (the predictor forgets in a few samples) -- what the cache
// lines need, where any line can be asked for first.
void decode(const u8* data, const s16 coefs[16], u32 total, u32 start, u32 count, s16* out, u32 out_stride);
} // namespace nxa

// Minimal stand-in for xrSound's stdafx.h so SoundRender_Adpcm.cpp compiles inside nxa_conv.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
template <class T> using xr_vector = std::vector<T>;
template <class T> constexpr T _max(T a, T b) { return a > b ? a : b; }
template <class T> constexpr T _min(T a, T b) { return a < b ? a : b; }

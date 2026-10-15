// Minimal stand-in for xrCore's stdafx.h so the engine's LzHuf.cpp compiles standalone inside dbtool.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using s64 = int64_t;
using BYTE = uint8_t;

#define IC inline
#define ICF inline
#define XRCORE_API
#define VERIFY(x) ((void)0)
#define R_ASSERT(x) ((void)0)

inline void* xr_malloc(size_t n) { return malloc(n); }
inline void* xr_realloc(void* p, size_t n) { return realloc(p, n); }
inline void xr_free(void* p) { free(p); }
template <class T, class... A> T* xr_new(A&&... a) { return new T(std::forward<A>(a)...); }
template <class T> void xr_delete(T*& p) { delete p; p = nullptr; }

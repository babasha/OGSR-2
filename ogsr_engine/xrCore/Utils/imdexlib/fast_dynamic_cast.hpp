#pragma once

#include <cstdint>
#include <type_traits>
#include <typeinfo>
#include <exception> // std::terminate -- на MSVC приходит транзитивно, явно нужен на Linux

namespace imdexlib
{

using class_ptr = const uintptr_t*;
using vtable_ptr = const uintptr_t*;

constexpr ptrdiff_t Uncached = PTRDIFF_MAX;
constexpr ptrdiff_t Bad = PTRDIFF_MAX - 1;

template <typename T>
vtable_ptr getVTable(const T* obj) noexcept
{
    return reinterpret_cast<vtable_ptr>(*reinterpret_cast<class_ptr>(obj));
}

// ⭐11-10-2026 (Switch fight): ONE cache for every cast, 256 entries a thread, keyed by (the object's vtable, the target
// type). It was one entry per <To, From> instantiation: a loop over mixed objects -- a stalker's visual memory (stalkers,
// monsters, the actor, weapons...) in CAgentEnemyManager::fill_enemies, the object list, ray callbacks -- missed on every
// type change and paid a real dynamic_cast (__dynamic_cast + __do_dyncast ~2.6 % of the main thread in a fight, the
// agent manager's distribute_enemies 2.9 ms a call). 1138 instantiations x 16 B of TLS was also more than these 6 KB.
// The offset from a given subobject's vtable to the target is fixed (the vtable names the most-derived type and the
// subobject), which is what the single entry relied on too.
template <typename To>
struct CastTag
{
    // its address names To. ⚠Writable on purpose: identical CONSTANT data may be folded into one address (MSVC
    // /OPT:ICF, lld --icf=all), and two target types sharing a key would hand out each other's offsets.
    static inline char tag = 0;
};

struct CastCacheEntry
{
    vtable_ptr vt;
    const void* to;
    ptrdiff_t off;
};

inline thread_local CastCacheEntry g_castCache[256];

template <typename To, typename From>
To fast_dynamic_cast(From* from) noexcept
{
    if (!from)
        return nullptr;

    using T = std::conditional_t<std::is_const_v<From>, const char, char>;

    const auto vtable = getVTable(from);
    const void* to = &CastTag<To>::tag;
    const uintptr_t h = reinterpret_cast<uintptr_t>(vtable) ^ (reinterpret_cast<uintptr_t>(to) * 0x9E3779B97F4A7C15ull);
    CastCacheEntry& e = g_castCache[((h >> 4) ^ (h >> 13) ^ (h >> 29)) & 255];
    if (e.vt == vtable && e.to == to)
    {
        if (e.off == Bad)
            return nullptr;
        return reinterpret_cast<To>(reinterpret_cast<T*>(from) + e.off);
    }

    To res = dynamic_cast<To>(from);
    e.vt = vtable;
    e.to = to;
    e.off = res ? reinterpret_cast<const char*>(res) - reinterpret_cast<const char*>(from) : Bad;
    return res;
}

template <typename To, typename From>
To fast_dynamic_cast(From& from)
{
    using Ptr = std::add_pointer_t<std::remove_reference_t<To>>;
    using ToPtr = std::conditional_t<std::is_const_v<To>, std::add_const_t<Ptr>, Ptr>;
    auto* res = fast_dynamic_cast<ToPtr>(std::addressof(from));
    if (!res)
#if _HAS_EXCEPTIONS
        throw std::bad_cast();
#else
        std::terminate();
#endif
    return *res;
}

} // namespace imdexlib

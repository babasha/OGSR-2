#pragma once

class XRCORE_API xrMemory
{
public:
    xrMemory() = default;
    void _initialize();
    static void _destroy();

    static u32 mem_usage(u32* pBlocksUsed = nullptr, u32* pBlocksFree = nullptr);
    void mem_compact();

    void* mem_alloc(size_t size);
    void* mem_realloc(void* p, size_t size);
    void mem_free(void* p);

    void*(WINAPIV* mem_copy)(void*, const void*, size_t) = std::memcpy;
    void*(WINAPIV* mem_fill)(void*, int, size_t) = std::memset;
};
extern XRCORE_API xrMemory Memory;

#undef ZeroMemory
#undef CopyMemory
#undef FillMemory
#define ZeroMemory(a, b) std::memset(a, 0, b)
#define CopyMemory(a, b, c) std::memcpy(a, b, c)
#define FillMemory(a, b, c) std::memset(a, c, b)

#define xr_malloc(size) Memory.mem_alloc(size)
#define xr_realloc(p, size) Memory.mem_realloc(p, size)
#define xr_mfree(p) Memory.mem_free(p)

// generic "C"-like allocations/deallocations
template <class T>
IC T* xr_alloc(size_t count)
{
    T* ptr = static_cast<T*>(Memory.mem_alloc(count * sizeof(T)));
    return ptr;
}

template <class T>
IC void xr_free(T*& P)
{
    if (P)
    {
        Memory.mem_free((void*)P);
        P = nullptr;
    }
}

template <typename T, typename... Args>
T* xr_new(Args&&... args)
{
    T* ptr = static_cast<T*>(Memory.mem_alloc(sizeof(T)));
    return new (ptr) T(std::forward<Args>(args)...);
}

template <class T>
IC void xr_delete(T*& ptr)
{
    if (ptr)
    {
        if constexpr (std::is_polymorphic_v<T>)
        {
            void* _real_ptr = smart_cast<void*>(ptr);
            ptr->~T();
            Memory.mem_free(_real_ptr);
        }
        else
        {
            ptr->~T();
            Memory.mem_free(ptr);
        }
        ptr = nullptr;
    }
}

XRCORE_API size_t mem_usage_impl(u32* pBlocksUsed, u32* pBlocksFree);

struct SProcessMemInfo
{
    u64 PeakWorkingSetSize;
    u64 WorkingSetSize;

    u64 PeakPagefileUsage;
    u64 PagefileUsage;

    u64 TotalPhysicalMemory;
    u64 FreePhysicalMemory;
    u64 TotalPageFile;
    u64 FreePageFile;

    u32 MemoryLoad;
};
XRCORE_API void GetProcessMemInfo(SProcessMemInfo& minfo);

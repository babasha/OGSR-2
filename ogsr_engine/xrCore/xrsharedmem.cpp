#include "stdafx.h"

XRCORE_API smem_container* g_pSharedMemoryContainer = NULL;

smem_value* smem_container::dock(u32 dwSize, void* ptr)
{
    VERIFY(dwSize && ptr);

    if (bDisable)
    {
        smem_value* result = (smem_value*)xr_malloc(sizeof(smem_value) + dwSize);
        result->dwReference = 0;
        result->dwSize = dwSize;
        result->dwCRC = u32(-1);
        CopyMemory(result->value, ptr, dwSize);

        return result;
    }

    cs.Enter();
    smem_value* result = 0;

    u32 dwCRC = crc32(ptr, dwSize);
    const u64 key = (u64(dwCRC) << 32) | dwSize;

    const auto [first, last] = container.equal_range(key);
    for (auto it = first; it != last; ++it)
    {
        if (0 == memcmp(it->second->value, ptr, dwSize))
        {
            result = it->second;
            break;
        }
    }

    // if not found - create new entry
    if (0 == result)
    {
        result = (smem_value*)xr_malloc(sizeof(smem_value) + dwSize);
        result->dwReference = 0;
        result->dwCRC = dwCRC;
        result->dwSize = dwSize;
        CopyMemory(result->value, ptr, dwSize);
        container.emplace(key, result);
    }

    // exit
    cs.Leave();
    return result;
}

void smem_container::clean()
{
    cs.Enter();
    std::erase_if(container, [](auto& kv) {
        if (0 != kv.second->dwReference)
            return false;
        xr_free(kv.second);
        return true;
    });
    cs.Leave();
}

void smem_container::dump()
{
    string_path fname;
    FS.update_path(fname, fsgame::app_data_root, "$smem_container_dump$.txt");

    cs.Enter();
    FILE* F = fopen(XR_OSPATH(fname), "w");
    for (const auto& [key, v] : container)
        fprintf(F, "%4u : crc[%6x], %u bytes\n", v->dwReference.load(), v->dwCRC, v->dwSize);
    fclose(F);
    cs.Leave();
}

u32 smem_container::stat_economy()
{
    cs.Enter();
    s64 counter = 0;
    counter -= sizeof(*this);
    counter -= sizeof(cdb::allocator_type);
    const int node_size = 20;
    for (const auto& [key, v] : container)
    {
        counter -= 16;
        counter -= node_size;
        counter += s64((s64(v->dwReference) - 1) * s64(v->dwSize));
    }
    cs.Leave();

    return u32(s64(counter) / s64(1024));
}

smem_container::~smem_container() { clean(); }

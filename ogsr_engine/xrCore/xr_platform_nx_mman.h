// Horizon (newlib + libnx) has no <sys/mman.h>. The engine only uses mmap as a READ-ONLY file view
// (MapViewOfFile in xr_platform_win32_shim.h) and madvise as a prefetch hint. This is the emulation:
// a view is a malloc'd COPY of the file range, filled by lseek+read.
//
// ⚠COST, not a semantic gap: real mmap costs nothing until touched; this copies the whole range and
// holds it resident. Fine for small files; a multi-GB archive mapped whole would exhaust the ~3.2 GB
// process pool. Before shipping, arc_db/FS callers must map WINDOWS of an archive, not the file.
// ⚠fds made by dup() share one file offset, so lseek+read is serialised by a global mutex (no pread in newlib).
#pragma once
#include <cstdlib>
#include <cstddef>
#include <mutex>
#include <unistd.h>
#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void*)-1)
#define MADV_WILLNEED 3

inline std::mutex& xr_nx_read_mutex() { static std::mutex m; return m; }

// SD read accounting, cmake/nx_compat/nx_libc_shims.c ([nx io] lines)
extern "C" unsigned long long xr_nx_ticks(void);
extern "C" void xr_nx_io_note(int kind, unsigned long long bytes, unsigned long long wait_ticks, unsigned long long read_ticks);

inline void* mmap(void* /*addr*/, size_t length, int /*prot*/, int /*flags*/, int fd, off_t offset)
{
    void* p = std::malloc(length ? length : 1);
    if (!p)
        return MAP_FAILED;

    const unsigned long long t0 = xr_nx_ticks();
    std::lock_guard<std::mutex> lk(xr_nx_read_mutex());
    const unsigned long long t1 = xr_nx_ticks();
    struct Note
    {
        unsigned long long t0, t1;
        size_t n;
        ~Note() { xr_nx_io_note(0, n, t1 - t0, xr_nx_ticks() - t1); }
    } note{t0, t1, length};
    if (lseek(fd, offset, SEEK_SET) < 0)
    {
        std::free(p);
        return MAP_FAILED;
    }
    size_t done = 0;
    while (done < length)
    {
        const ssize_t r = read(fd, static_cast<char*>(p) + done, length - done);
        if (r <= 0)
        {
            std::free(p);
            return MAP_FAILED;
        }
        done += static_cast<size_t>(r);
    }
    return p;
}

inline int munmap(void* addr, size_t /*length*/)
{
    std::free(addr);
    return 0;
}

inline int madvise(void*, size_t, int) { return 0; } // the copy is already resident

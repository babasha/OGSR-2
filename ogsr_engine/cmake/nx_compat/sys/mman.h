/* Horizon (newlib): no <sys/mman.h>. C-only, ANONYMOUS mappings only -- what libsquashfs's mempool
 * asks for (mmap(NULL, n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)). Served by calloc,
 * which is what anonymous zero-filled memory is. A file-backed request is refused (MAP_FAILED); the
 * engine's own file views use xrCore/xr_platform_nx_mman.h instead.
 * Only on the include path for the libsquashfs target (see its CMakeLists.txt). */
#ifndef OGSR_NX_SYS_MMAN_H
#define OGSR_NX_SYS_MMAN_H
#include <stdlib.h>
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void*)-1)

static inline void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    void* p;
    (void)addr; (void)prot; (void)offset;
    if (!(flags & MAP_ANONYMOUS) || fd != -1)
        return MAP_FAILED;
    p = calloc(1, length ? length : 1);
    return p ? p : MAP_FAILED;
}

static inline int munmap(void* addr, size_t length)
{
    (void)length;
    free(addr);
    return 0;
}
#endif

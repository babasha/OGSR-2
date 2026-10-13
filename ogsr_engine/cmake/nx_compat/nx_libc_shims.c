/* libc functions the engine (and vendored libs) call that newlib + libnx declare or expect, but do not
 * implement. Horizon has ONE process, no shell and no /proc, so most of these are honest one-liners.
 * Built as the static lib `xr_nx_libc` (see ogsr_engine/CMakeLists.txt) and linked last in xr_3da's group. */
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <switch/arm/counter.h>

/* --- SD read accounting (07-10) -------------------------------------------------------------------------------------
 * nfsmw-nx measured an SD read at ~2.3 ms fixed per call + ~70 MB/s, so HOW MANY reads matters as much as how many
 * bytes. Every engine file read on the console goes through one of two doors: the mmap emulation (a file view =
 * a malloc'd copy, xrCore/xr_platform_nx_mman.h: archives, level files) and pread below (squashfs). Both note here:
 * calls, bytes, time inside the read, time waiting for the lock, sizes, the slowest call. xr_nx_io_dump() prints and
 * resets: at each [load phase] (GamePersistent.cpp) and every 10 s in the game (clock watch, xr_nx_pad.c). */
enum { XR_IO_KINDS = 3, XR_IO_BINS = 5 };
static const char* const g_io_kind_name[XR_IO_KINDS] = {"mmap", "pread", "from window"};
static unsigned long long g_io_n[XR_IO_KINDS], g_io_bytes[XR_IO_KINDS], g_io_read[XR_IO_KINDS], g_io_wait[XR_IO_KINDS],
    g_io_max[XR_IO_KINDS], g_io_max_bytes[XR_IO_KINDS];
static unsigned long long g_io_hist[XR_IO_KINDS][XR_IO_BINS];

unsigned long long xr_nx_ticks(void) { return armGetSystemTick(); }

void xr_nx_io_note(int kind, unsigned long long bytes, unsigned long long wait_ticks, unsigned long long read_ticks)
{
    if (kind < 0 || kind >= XR_IO_KINDS)
        return;
    const int bin = bytes < (64u << 10) ? 0 : bytes < (256u << 10) ? 1 : bytes < (1u << 20) ? 2 : bytes < (4u << 20) ? 3 : 4;
    __atomic_fetch_add(&g_io_n[kind], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_io_bytes[kind], bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_io_read[kind], read_ticks, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_io_wait[kind], wait_ticks, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_io_hist[kind][bin], 1, __ATOMIC_RELAXED);
    if (read_ticks > __atomic_load_n(&g_io_max[kind], __ATOMIC_RELAXED)) { /* racy max: a diagnostic, not a ledger */
        __atomic_store_n(&g_io_max[kind], read_ticks, __ATOMIC_RELAXED);
        __atomic_store_n(&g_io_max_bytes[kind], bytes, __ATOMIC_RELAXED);
    }
}

static void xr_ra_dump(const char* tag);
void xr_nx_io_dump(const char* tag)
{
    const double ms_per_tick = 1000.0 / (double)armGetSystemTickFreq();
    for (int k = 0; k < XR_IO_KINDS; ++k) {
        const unsigned long long n = __atomic_exchange_n(&g_io_n[k], 0, __ATOMIC_RELAXED);
        const unsigned long long b = __atomic_exchange_n(&g_io_bytes[k], 0, __ATOMIC_RELAXED);
        const unsigned long long r = __atomic_exchange_n(&g_io_read[k], 0, __ATOMIC_RELAXED);
        const unsigned long long w = __atomic_exchange_n(&g_io_wait[k], 0, __ATOMIC_RELAXED);
        const unsigned long long mx = __atomic_exchange_n(&g_io_max[k], 0, __ATOMIC_RELAXED);
        const unsigned long long mxb = __atomic_exchange_n(&g_io_max_bytes[k], 0, __ATOMIC_RELAXED);
        unsigned long long h[XR_IO_BINS];
        for (int i = 0; i < XR_IO_BINS; ++i)
            h[i] = __atomic_exchange_n(&g_io_hist[k][i], 0, __ATOMIC_RELAXED);
        if (!n)
            continue;
        fprintf(stderr,
                "[nx io] %s %s: %llu calls %.1f MB | read %.0f ms (%.1f MB/s, %.2f ms/call) | lock wait %.0f ms | sizes <64K %llu "
                "<256K %llu <1M %llu <4M %llu >=4M %llu | slowest %.1f ms (%.0f KB)\n",
                tag, g_io_kind_name[k], n, (double)b / 1048576.0, (double)r * ms_per_tick,
                r ? ((double)b / 1048576.0) / ((double)r * ms_per_tick / 1000.0) : 0.0, (double)r * ms_per_tick / (double)n,
                (double)w * ms_per_tick, h[0], h[1], h[2], h[3], h[4], (double)mx * ms_per_tick, (double)mxb / 1024.0);
    }
    xr_ra_dump(tag);
}

/* pread/pwrite: newlib has no positional I/O. lseek+read is NOT atomic across threads that share one fd,
 * so it is serialised, and the file offset is put back so plain read() users of the same fd are unharmed.
 *
 * The lock is per fd (64 stripes by fd number), not one for the process: the file offset belongs to the fd, and
 * every squashfs reader has its own fd, so the texture streamer, the residency helpers and the loader no longer wait
 * for each other's 128 KB blocks (nfsmw-nx: SD reads cost ~2.3 ms fixed per call, one global lock serialised them).
 * Reads are also split into 4 MB pieces, so one big read does not hold the fs service for everyone else
 * (their worst read went 261 -> 60 ms). ⚠The mmap emulation (xrCore/xr_platform_nx_mman.h) does NOT use these stripes:
 * it holds one global mutex of its own for every file view. */
ssize_t pread(int fd, void* buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void* buf, size_t n, off_t off);

#define XR_NX_FD_STRIPES 64
#define XR_NX_IO_CHUNK (4u << 20)
static pthread_mutex_t g_pio[XR_NX_FD_STRIPES] = {
#define XR_M4 PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER
    XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4, XR_M4
#undef XR_M4
};

pthread_mutex_t* xr_nx_fd_lock(int fd) { return &g_pio[(unsigned)fd % XR_NX_FD_STRIPES]; }

/* read() until n bytes, EOF or error, in XR_NX_IO_CHUNK pieces; returns bytes read or -1 if nothing was read */
ssize_t xr_nx_read_full(int fd, void* buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        size_t piece = n - done;
        if (piece > XR_NX_IO_CHUNK)
            piece = XR_NX_IO_CHUNK;
        const ssize_t r = read(fd, (char*)buf + done, piece);
        if (r < 0)
            return done ? (ssize_t)done : -1;
        if (r == 0)
            break;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/* --- read-ahead for the game archives (07-10) -----------------------------------------------------------------------
 * libsquashfs reads one compressed block per pread (~43 KB on average, at most the 128 KB block size), and on the
 * console every call costs ~2 ms whatever its size: a Kordon save load made ~9,000 of them, ~15 s of its 47 s, at an
 * effective 17-30 MB/s, while the one large read of the load (cform, 85 MB) ran at 82 MB/s. A file's blocks lie one
 * after the other in the image, so a read that starts where the previous one on the same fd ended is the start of a
 * run: read XR_RA_WINDOW at once and serve the following blocks from memory.
 * Only for the archives (`*.db_*` opened read-only, marked by the open/close wraps below: a closed fd's number may be
 * reused by another file, so its window dies with it). At most XR_RA_SLOTS windows exist at a time (one per fd stripe
 * that reads an archive; squashfs readers keep their fds for the archive's lifetime). */
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#define XR_RA_WINDOW (1024u << 10) /* the largest window (and buffer); a run starts at 4 x its first read and doubles */
#define XR_RA_SLOTS 16
#define XR_RA_MAXFD 1024
static unsigned char g_ra_ok[XR_RA_MAXFD];      /* fd -> 1 if it is an archive opened read-only */
struct xr_ra
{
    int fd;              /* owner fd, -1 = none */
    char* buf;           /* XR_RA_WINDOW bytes, or NULL */
    off_t woff;          /* window start in the file */
    size_t wlen;         /* valid bytes in it */
    off_t next;          /* where the last read on this fd ended (sequential detector) */
    size_t ra;           /* size of the next window: 0 = not in a run */
};
static struct xr_ra g_ra[XR_NX_FD_STRIPES]; /* guarded by the fd's stripe lock */
static int g_ra_bufs;                       /* windows allocated (atomic) */

int __real_open(const char* path, int flags, ...);
int __real_close(int fd);
int __real_dup(int fd);

/* libsquashfs's readers are copies of one opened file (sqfs_copy -> sqfs_native_file_duplicate -> dup), and every
 * block read of the load went through such a copy: the mark must follow the fd into its duplicate (v196: 0 windows). */
int __wrap_dup(int fd)
{
    const int nfd = __real_dup(fd);
    if (nfd >= 0 && nfd < XR_RA_MAXFD)
        g_ra_ok[nfd] = (fd >= 0 && fd < XR_RA_MAXFD) ? g_ra_ok[fd] : 0;
    return nfd;
}

int __wrap_open(const char* path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    const int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    const int fd = __real_open(path, flags, mode);
    if (fd >= 0 && fd < XR_RA_MAXFD)
        g_ra_ok[fd] = ((flags & O_ACCMODE) == O_RDONLY && path && strstr(path, ".db_")) ? 1 : 0;
    static int s_said;
    if (s_said < 24 && path && strstr(path, ".db")) {
        ++s_said;
        fprintf(stderr, "[nx io] open '%s' flags 0x%x -> fd %d read-ahead %d\n", path, flags, fd,
                (fd >= 0 && fd < XR_RA_MAXFD) ? g_ra_ok[fd] : -1);
    }
    return fd;
}
static unsigned long long g_ra_seq, g_ra_near, g_ra_nonseq, g_ra_hit;   /* read-ahead decisions, for [nx io] */

static void xr_ra_forget(int fd)
{
    pthread_mutex_t* m = xr_nx_fd_lock(fd);
    pthread_mutex_lock(m);
    struct xr_ra* s = &g_ra[(unsigned)fd % XR_NX_FD_STRIPES];
    if (s->fd == fd) {
        s->fd = -1;
        s->wlen = 0;
    }
    pthread_mutex_unlock(m);
}

int __wrap_close(int fd)
{
    if (fd >= 0 && fd < XR_RA_MAXFD && g_ra_ok[fd]) {
        g_ra_ok[fd] = 0;
        xr_ra_forget(fd);
    }
    return __real_close(fd);
}

/* under the fd's stripe lock: serve [off, off+n) from the fd's window or read a new window; -2 = not handled */
static ssize_t xr_ra_read(int fd, void* buf, size_t n, off_t off, unsigned long long* sd_ticks, unsigned long long* sd_bytes)
{
    if (fd < 0 || fd >= XR_RA_MAXFD || !g_ra_ok[fd] || n >= XR_RA_WINDOW)
        return -2;
    struct xr_ra* s = &g_ra[(unsigned)fd % XR_NX_FD_STRIPES];
    if (s->fd != fd) { /* the stripe served another fd: start over for this one */
        s->fd = fd;
        s->wlen = 0;
        s->next = -1;
        s->ra = 0;
    }
    if (s->buf && s->wlen && off >= s->woff && (size_t)(off - s->woff) + n <= s->wlen) {
        memcpy(buf, s->buf + (off - s->woff), n);
        ++g_ra_hit;
        s->next = off + (off_t)n;
        return (ssize_t)n;
    }
    const int sequential = (off == s->next);
    if (sequential) ++g_ra_seq;
    else if (s->next >= 0 && off > s->next && off - s->next <= (off_t)XR_RA_WINDOW) ++g_ra_near;
    else ++g_ra_nonseq;
    s->next = off + (off_t)n;
    if (!sequential) {
        s->ra = 0; /* the run broke: the next window starts small again */
        return -2;
    }
    s->ra = s->ra ? (s->ra * 2 < XR_RA_WINDOW ? s->ra * 2 : XR_RA_WINDOW) : (n * 4 < XR_RA_WINDOW ? n * 4 : XR_RA_WINDOW);
    if (!s->buf) {
        if (__atomic_add_fetch(&g_ra_bufs, 1, __ATOMIC_RELAXED) > XR_RA_SLOTS) {
            __atomic_sub_fetch(&g_ra_bufs, 1, __ATOMIC_RELAXED);
            return -2;
        }
        s->buf = (char*)malloc(XR_RA_WINDOW);
        if (!s->buf) {
            __atomic_sub_fetch(&g_ra_bufs, 1, __ATOMIC_RELAXED);
            return -2;
        }
    }
    const unsigned long long t = armGetSystemTick();
    const off_t save = lseek(fd, 0, SEEK_CUR);
    if (save < 0 || lseek(fd, off, SEEK_SET) < 0)
        return -2;
    const ssize_t got = xr_nx_read_full(fd, s->buf, s->ra > n ? s->ra : n);
    lseek(fd, save, SEEK_SET);
    *sd_ticks = armGetSystemTick() - t;
    *sd_bytes = got > 0 ? (unsigned long long)got : 0;
    if (got <= 0) {
        s->wlen = 0;
        return got;
    }
    s->woff = off;
    s->wlen = (size_t)got;
    const size_t give = n < (size_t)got ? n : (size_t)got;
    memcpy(buf, s->buf, give);
    return (ssize_t)give;
}

ssize_t pread(int fd, void* buf, size_t n, off_t off)
{
    ssize_t r;
    off_t save;
    pthread_mutex_t* m = xr_nx_fd_lock(fd);
    const unsigned long long t0 = armGetSystemTick();
    pthread_mutex_lock(m);
    const unsigned long long t1 = armGetSystemTick();
    {
        unsigned long long sd_ticks = 0, sd_bytes = 0;
        const ssize_t ra = xr_ra_read(fd, buf, n, off, &sd_ticks, &sd_bytes);
        if (ra != -2) {
            pthread_mutex_unlock(m);
            if (sd_bytes || sd_ticks)
                xr_nx_io_note(1, sd_bytes, t1 - t0, sd_ticks);  /* the window read: a real SD call */
            xr_nx_io_note(2, ra > 0 ? (unsigned long long)ra : 0, 0, 0); /* what the caller got from a window */
            return ra;
        }
    }
    save = lseek(fd, 0, SEEK_CUR);
    if (save < 0 || lseek(fd, off, SEEK_SET) < 0) {
        pthread_mutex_unlock(m);
        return -1;
    }
    r = xr_nx_read_full(fd, buf, n);
    lseek(fd, save, SEEK_SET);
    pthread_mutex_unlock(m);
    xr_nx_io_note(1, r > 0 ? (unsigned long long)r : 0, t1 - t0, armGetSystemTick() - t1);
    return r;
}

ssize_t pwrite(int fd, const void* buf, size_t n, off_t off)
{
    ssize_t r;
    off_t save;
    pthread_mutex_t* m = xr_nx_fd_lock(fd);
    pthread_mutex_lock(m);
    save = lseek(fd, 0, SEEK_CUR);
    if (save < 0 || lseek(fd, off, SEEK_SET) < 0) {
        pthread_mutex_unlock(m);
        return -1;
    }
    r = write(fd, buf, n);
    lseek(fd, save, SEEK_SET);
    pthread_mutex_unlock(m);
    return r;
}

/* Advisory locking between processes: there is no second process. */
int flock(int fd, int operation)
{
    (void)fd;
    (void)operation;
    return 0;
}

char* getlogin(void) { return (char*)"switch"; }

/* Horizon gives a homebrew title 3 application cores (the 4th belongs to the system); 4 KiB pages. */
long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE: return 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: return 3;
    default: errno = EINVAL; return -1;
    }
}

int timespec_get(struct timespec* ts, int base)
{
    if (base != TIME_UTC || !ts)
        return 0;
    return clock_gettime(CLOCK_REALTIME, ts) == 0 ? base : 0;
}

/* Launching another program (xdg-open in the shim's ShellExecute): impossible on Horizon. */
int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* fa,
                 const posix_spawnattr_t* attr, char* const argv[], char* const envp[])
{
    (void)pid; (void)file; (void)fa; (void)attr; (void)argv; (void)envp;
    return ENOSYS;
}

static void xr_ra_dump(const char* tag)
{
    const unsigned long long seq = __atomic_exchange_n(&g_ra_seq, 0, __ATOMIC_RELAXED);
    const unsigned long long nr = __atomic_exchange_n(&g_ra_near, 0, __ATOMIC_RELAXED);
    const unsigned long long ns = __atomic_exchange_n(&g_ra_nonseq, 0, __ATOMIC_RELAXED);
    const unsigned long long hit = __atomic_exchange_n(&g_ra_hit, 0, __ATOMIC_RELAXED);
    if (seq || nr || ns || hit)
        fprintf(stderr, "[nx io] %s read-ahead: window hits %llu | misses: sequential %llu, forward gap %llu, elsewhere %llu | windows %d\n",
                tag, hit, seq, nr, ns, g_ra_bufs);
}

/* Debug link (07-10): the console's live log and the engine console over Wi-Fi, TCP port 7000.
 *
 * Why: stderr.log cannot be read while the game runs -- Horizon keeps a file that is open for writing locked against every
 * other opener (ftpsrv: "550 Device or resource busy"), so a run could only be inspected after it ended. Now:
 *   - stderr goes through our own devoptab (like libnx's console and nxlink do): every write lands in stderr.log AND in a
 *     512 KB ring; the engine's Msg/Log lines reach the same ring through g_xr_nx_log_tap (xrCore/log.cpp);
 *   - a client connects and sends one line per request:
 *       log [N]    -> the last N bytes of the ring (all of it without N), then everything new, until the client hangs up
 *       cmd <text> -> queued, executed on the main thread by CRenderDevice::on_idle as Console->Execute(text); reply "ok"
 *       ping       -> "pong <bytes logged so far>"
 *       exit       -> "bye", then the process ends (xr_nx_exit_process)
 * PC side: D:\switch_stage\tools\nx.py (log -f / cmd / exit).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iosupport.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <switch.h>

#define LINK_PORT 7000
#define RING_SIZE (512u * 1024u)
#define CMD_SLOTS 32
#define CMD_LEN 512
#define MAX_CLIENTS 4

extern volatile int g_xr_nx_quit;
void xr_nx_exit_process(void);

static char g_ring[RING_SIZE];
static u64 g_head; /* bytes ever written; the ring holds [g_head - RING_SIZE, g_head) */
static Mutex g_ring_mx;
static CondVar g_ring_cv;

static char g_cmd[CMD_SLOTS][CMD_LEN];
static int g_cmd_r, g_cmd_w;
static Mutex g_cmd_mx;

static int g_err_fd = -1;
static volatile int g_clients;

static void ring_put(const char* p, size_t n)
{
    mutexLock(&g_ring_mx);
    for (size_t i = 0; i < n; ++i)
        g_ring[(g_head + i) % RING_SIZE] = p[i];
    g_head += n;
    condvarWakeAll(&g_ring_cv);
    mutexUnlock(&g_ring_mx);
}

/* Engine log lines (xrCore AddOne, under its own lock): no trailing newline. */
void xr_nx_link_log_line(const char* line)
{
    ring_put(line, strlen(line));
    ring_put("\n", 1);
}

/* stderr.log is written BEHIND (07-10, v203). stderr is unbuffered (a crash must not eat the last lines), so every fprintf
   was a write to the SD on the printing thread, and an SD write is a filesystem round trip plus the FAT/exFAT bookkeeping:
   milliseconds, more while another thread reads. v202 stack samples of a save load: the main thread waited in these writes
   (the [big] allocation traces, the [nx io] dumps) for a large part of its load time, and the load prefetcher's reads made
   every one of them longer -- the ALife phase grew 1-2 s with nothing more to compute. Now a write lands in memory and the
   flusher thread puts it on the SD every 100 ms; LogFlushNow (fatal errors) and the exit path write it out at once. What a
   hard crash can lose is the last 100 ms -- the debug link's ring has them anyway.
   ⚠⚠A printing thread must NEVER wait for the file. The first version (v203) let a writer that found the buffer full flush
   it itself, waiting for the file lock the flusher held -- with stderr's own lock held. The flusher is a 0x3B pthread that
   can sit on core 0 under the 0x2C main thread; the whole process froze in the middle of a load (no further line from any
   thread, no crash report). Now: two buffers; a writer only appends under a lock held for one memcpy, and when the buffer
   is full the bytes are dropped and counted (a "[stderr: N bytes dropped]" line says so in the file). The flusher swaps
   the buffers and writes the full one with no lock a writer could need, from core 2. */
#define ERR_BUF_SIZE (256u * 1024u)
static char g_errbufs[2][ERR_BUF_SIZE];
static int g_errcur;        /* the buffer writers append to */
static size_t g_errlen;
static unsigned long long g_errdropped;
static Mutex g_errbuf_mx;  /* g_errcur / g_errlen / g_errdropped: held for a memcpy, never across I/O */
static Mutex g_errfile_mx; /* the flushers among themselves (thread, LogFlushNow, exit): one at a time, in order */

void xr_nx_stderr_flush(void)
{
    mutexLock(&g_errfile_mx);
    mutexLock(&g_errbuf_mx);
    const int full = g_errcur;
    const size_t n = g_errlen;
    const unsigned long long dropped = g_errdropped;
    g_errcur ^= 1;
    g_errlen = 0;
    g_errdropped = 0;
    mutexUnlock(&g_errbuf_mx);
    if (g_err_fd >= 0) {
        if (n)
            write(g_err_fd, g_errbufs[full], n);
        if (dropped) {
            char note[96];
            const int k = snprintf(note, sizeof(note), "[stderr: %llu bytes dropped, the buffer was full]\n", dropped);
            if (k > 0)
                write(g_err_fd, note, (size_t)k);
        }
    }
    mutexUnlock(&g_errfile_mx);
}

static ssize_t err_write(struct _reent* r, void* fd, const char* ptr, size_t len)
{
    (void)r;
    (void)fd;
    ring_put(ptr, len);
    mutexLock(&g_errbuf_mx);
    if (g_errlen + len <= ERR_BUF_SIZE) {
        memcpy(g_errbufs[g_errcur] + g_errlen, ptr, len);
        g_errlen += len;
    } else {
        g_errdropped += len;
    }
    mutexUnlock(&g_errbuf_mx);
    return (ssize_t)len;
}

static void* err_flush_thread(void* arg)
{
    (void)arg;
    /* core 2, away from the main thread (core 0) that would otherwise starve it */
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1u << 2);
    while (!g_xr_nx_quit) {
        svcSleepThread(100000000LL);
        xr_nx_stderr_flush();
    }
    return NULL;
}

static const devoptab_t g_err_dotab = {
    .name = "ogsrerr",
    .write_r = err_write,
};

/* Replaces freopen("stderr.log", "w", stderr): same file, plus the ring. Call before anything prints. */
void xr_nx_link_stderr_init(void)
{
    g_err_fd = open("stderr.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    devoptab_list[STD_ERR] = &g_err_dotab;
}

int xr_nx_link_pop(char* out, int cap)
{
    int got = 0;
    mutexLock(&g_cmd_mx);
    if (g_cmd_r != g_cmd_w) {
        strncpy(out, g_cmd[g_cmd_r % CMD_SLOTS], (size_t)cap - 1);
        out[cap - 1] = 0;
        ++g_cmd_r;
        got = 1;
    }
    mutexUnlock(&g_cmd_mx);
    return got;
}

static int cmd_push(const char* text)
{
    int ok = 0;
    mutexLock(&g_cmd_mx);
    if (g_cmd_w - g_cmd_r < CMD_SLOTS) {
        strncpy(g_cmd[g_cmd_w % CMD_SLOTS], text, CMD_LEN - 1);
        g_cmd[g_cmd_w % CMD_SLOTS][CMD_LEN - 1] = 0;
        ++g_cmd_w;
        ok = 1;
    }
    mutexUnlock(&g_cmd_mx);
    return ok;
}

static int send_all(int s, const char* p, size_t n)
{
    while (n) {
        const ssize_t k = send(s, p, n, 0);
        if (k <= 0)
            return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static void stream_log(int s, u64 back)
{
    /* Per client, NOT static: two clients stream at once in every measurement (ab_session's logd and `nx.py load`
       watching [nx state]), and while one thread's send() has the buffer in an IPC the kernel locks those pages -- the
       other thread writing the same static chunk was a Data Abort that killed the game (v203/v204, 4 crash reports at
       this line; the faster stderr made both clients send almost continuously). */
    char chunk[16384];
    u64 pos;
    mutexLock(&g_ring_mx);
    {
        const u64 oldest = g_head > RING_SIZE ? g_head - RING_SIZE : 0;
        pos = (back && g_head > back) ? g_head - back : 0;
        if (pos < oldest)
            pos = oldest;
    }
    mutexUnlock(&g_ring_mx);

    while (!g_xr_nx_quit) {
        size_t n = 0;
        mutexLock(&g_ring_mx);
        if (pos == g_head)
            condvarWaitTimeout(&g_ring_cv, &g_ring_mx, 1000000000ULL);
        if (g_head > RING_SIZE && pos < g_head - RING_SIZE)
            pos = g_head - RING_SIZE; /* the client fell a whole ring behind: skip what was overwritten */
        while (pos < g_head && n < sizeof(chunk))
            chunk[n++] = g_ring[pos++ % RING_SIZE];
        mutexUnlock(&g_ring_mx);
        if (n && !send_all(s, chunk, n))
            return;
    }
}

static void* client_thread(void* arg)
{
    const int s = (int)(intptr_t)arg;
    char line[CMD_LEN];
    int len = 0;
    for (;;) {
        char ch;
        const ssize_t k = recv(s, &ch, 1, 0);
        if (k <= 0)
            break;
        if (ch == '\r')
            continue;
        if (ch != '\n') {
            if (len < (int)sizeof(line) - 1)
                line[len++] = ch;
            continue;
        }
        line[len] = 0;
        len = 0;
        if (!strncmp(line, "log", 3) && (line[3] == 0 || line[3] == ' ')) {
            stream_log(s, line[3] ? strtoull(line + 4, NULL, 10) : 0);
            break;
        } else if (!strncmp(line, "cmd ", 4)) {
            const int ok = cmd_push(line + 4);
            send_all(s, ok ? "ok\n" : "busy\n", ok ? 3 : 5);
        } else if (!strcmp(line, "ping")) {
            char msg[64];
            const int n = snprintf(msg, sizeof(msg), "pong %llu\n", (unsigned long long)g_head);
            send_all(s, msg, (size_t)n);
        } else if (!strcmp(line, "exit")) {
            send_all(s, "bye\n", 4);
            close(s);
            xr_nx_exit_process();
        } else {
            send_all(s, "?\n", 2);
        }
    }
    close(s);
    __atomic_sub_fetch(&g_clients, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static int open_listener(void)
{
    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    struct sockaddr_in a;
    if (ls < 0) {
        fprintf(stderr, "[nx link] socket failed errno=%d\n", errno);
        return -1;
    }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(LINK_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(ls, 4) < 0) {
        fprintf(stderr, "[nx link] bind/listen :%d failed errno=%d\n", LINK_PORT, errno);
        close(ls);
        return -1;
    }
    fprintf(stderr, "[nx link] listening on :%d\n", LINK_PORT);
    return ls;
}

static void* listen_thread(void* arg)
{
    (void)arg;
    int ls = open_listener();
    int fails = 0;
    while (!g_xr_nx_quit) {
        int c;
        pthread_t t;
        if (ls < 0) {
            svcSleepThread(2000000000ULL);
            ls = open_listener();
            continue;
        }
        c = accept(ls, NULL, NULL);
        if (c < 0) {
            /* A Wi-Fi drop leaves the listening socket dead: accept fails from then on and the port refuses everyone
               while the game runs on (07-10, mid perf_route; the sysmodules on the same console came back). After a
               few failures in a row the socket is rebuilt. */
            if (++fails == 5) {
                fprintf(stderr, "[nx link] accept keeps failing errno=%d -- reopening :%d\n", errno, LINK_PORT);
                close(ls);
                ls = -1;
                fails = 0;
            }
            svcSleepThread(100000000ULL);
            continue;
        }
        fails = 0;
        {
            /* a log reader whose peer vanished would otherwise block in send() for good and keep its slot */
            const struct timeval tmo = { 10, 0 };
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
        }
        if (__atomic_add_fetch(&g_clients, 1, __ATOMIC_SEQ_CST) > MAX_CLIENTS) {
            __atomic_sub_fetch(&g_clients, 1, __ATOMIC_SEQ_CST);
            close(c);
            continue;
        }
        if (pthread_create(&t, NULL, client_thread, (void*)(intptr_t)c) == 0)
            pthread_detach(t);
        else {
            __atomic_sub_fetch(&g_clients, 1, __ATOMIC_SEQ_CST);
            close(c);
        }
    }
    close(ls);
    return NULL;
}

void xr_nx_link_start(void)
{
    {
        pthread_t ft;
        if (pthread_create(&ft, NULL, err_flush_thread, NULL) == 0)
            pthread_detach(ft);
        else
            fprintf(stderr, "[nx link] no stderr flusher thread: stderr.log is written when its buffer fills\n");
    }
    /* The default config asks for 4 buffers per socket; a debug link with a handful of connections needs far less of the
       process's memory (Кордон already runs close to the 3.2 GB ceiling). */
    static const SocketInitConfig cfg = {
        .tcp_tx_buf_size = 0x8000,
        .tcp_rx_buf_size = 0x4000,
        .tcp_tx_buf_max_size = 0x40000,
        .tcp_rx_buf_max_size = 0x10000,
        .udp_tx_buf_size = 0x2400,
        .udp_rx_buf_size = 0xA500,
        .sb_efficiency = 2,
        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_User,
    };
    pthread_t t;
    /* A measurement run sends no input for minutes; the console's auto-sleep (10 min handheld) then put it to sleep in
       the middle of a perf_route and took Wi-Fi with it -- nothing on the PC can wake it (07-10). Applications only. */
    {
        const Result sr = appletSetAutoSleepDisabled(true);
        fprintf(stderr, "[nx link] auto-sleep off rc=0x%x\n", sr);
    }
    const Result rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        fprintf(stderr, "[nx link] socketInitialize failed rc=0x%x -- no debug link this run\n", rc);
        return;
    }
    if (pthread_create(&t, NULL, listen_thread, NULL) == 0)
        pthread_detach(t);
}

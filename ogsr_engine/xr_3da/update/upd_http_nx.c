/* The updater's HTTPS on the Switch (see upd_http.h): BSD sockets + the console's own ssl service, and a small
 * HTTP/1.1 client on top -- GET, Content-Length or chunked bodies, redirects.
 *
 * ⭐Why the ssl SERVICE and not a TLS library: it is the console's (the eShop's) TLS, with Nintendo's CA list behind
 * SslVerifyOption_PeerCa | HostName -- no certificate bundle to ship and keep current, no mbedTLS in the NRO. GitHub's
 * chains (DigiCert / Sectigo roots) are in that list.
 *
 * Sockets are already up: xr_nx_link.c runs socketInitialize at start for the debug link, every run. */

#include <switch.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <pthread.h>

#include "upd_http.h"

static pthread_mutex_t s_ssl_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_ssl_state; /* 0: not tried, 1: up, -1: failed (no TLS this run) */
static SslContext s_ssl_ctx;

static void seterr(char* err, int n, const char* fmt, ...)
{
    if (!err || n <= 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, (size_t)n, fmt, ap);
    va_end(ap);
}

static int ssl_ready(char* err, int n)
{
    pthread_mutex_lock(&s_ssl_lock);
    if (!s_ssl_state)
    {
        Result rc = sslInitialize(3);
        if (R_SUCCEEDED(rc))
        {
            rc = sslCreateContext(&s_ssl_ctx, SslVersion_Auto);
            if (R_FAILED(rc))
                sslExit();
        }
        s_ssl_state = R_SUCCEEDED(rc) ? 1 : -1;
        fprintf(stderr, "[upd] ssl service %s (rc=0x%x)\n", s_ssl_state > 0 ? "up" : "FAILED", rc);
        if (s_ssl_state < 0)
            seterr(err, n, "ssl service: rc=0x%x", rc);
    }
    else if (s_ssl_state < 0)
        seterr(err, n, "ssl service unavailable");
    const int ok = s_ssl_state > 0;
    pthread_mutex_unlock(&s_ssl_lock);
    return ok;
}

/* ------------------------------------------------------------------ url ---- */

typedef struct url_parts
{
    int tls;
    int port;
    char host[256];
    char path[2048];
} url_parts;

static int parse_url(const char* url, url_parts* u)
{
    memset(u, 0, sizeof(*u));
    const char* p;
    if (!strncasecmp(url, "https://", 8))
        u->tls = 1, u->port = 443, p = url + 8;
    else if (!strncasecmp(url, "http://", 7))
        u->tls = 0, u->port = 80, p = url + 7;
    else
        return 0;
    size_t hl = strcspn(p, ":/?");
    if (!hl || hl >= sizeof(u->host))
        return 0;
    memcpy(u->host, p, hl);
    p += hl;
    if (*p == ':')
    {
        u->port = atoi(p + 1);
        p += 1 + strspn(p + 1, "0123456789");
    }
    if (*p != '/')
        snprintf(u->path, sizeof(u->path), "/%s", p);
    else
        snprintf(u->path, sizeof(u->path), "%s", p);
    return u->port > 0 && u->port < 65536;
}

/* ----------------------------------------------------------- connection ---- */

typedef struct conn
{
    int fd;
    int tls;
    int ssl_open;
    int ssl_fd; /* what socketSslConnectionSetSocketDescriptor handed back: closed before sslConnectionClose */
    SslConnection ssl;
    /* read buffer */
    char buf[64 * 1024];
    int pos, len;
} conn;

static int tcp_connect(const char* host, int port, char* err, int n)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo* res = NULL;
    const int g = getaddrinfo(host, ps, &hints, &res);
    if (g || !res)
    {
        seterr(err, n, "DNS: %s not resolved (%d)", host, g);
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        freeaddrinfo(res);
        seterr(err, n, "socket(): errno %d", errno);
        return -1;
    }
    /* Non-blocking connect so that "no route" is 10 s, not the stack's minutes. */
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS)
    {
        seterr(err, n, "connect %s:%d: errno %d", host, port, errno);
        close(fd);
        return -1;
    }
    if (r < 0)
    {
        struct pollfd pf = {fd, POLLOUT, 0};
        if (poll(&pf, 1, 10000) <= 0)
        {
            seterr(err, n, "connect %s:%d: timeout", host, port);
            close(fd);
            return -1;
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr)
        {
            seterr(err, n, "connect %s:%d: error %d", host, port, soerr);
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval tv = {30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static void conn_close(conn* c)
{
    if (c->ssl_open)
    {
        if (c->ssl_fd >= 0)
            close(c->ssl_fd);
        sslConnectionClose(&c->ssl);
        c->ssl_open = 0;
    }
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static int conn_open(conn* c, const url_parts* u, char* err, int n)
{
    c->fd = -1;
    c->ssl_fd = -1;
    c->ssl_open = 0;
    c->tls = u->tls;
    c->pos = c->len = 0;
    if (u->tls && !ssl_ready(err, n))
        return 0;
    c->fd = tcp_connect(u->host, u->port, err, n);
    if (c->fd < 0)
        return 0;
    if (!u->tls)
        return 1;

    Result rc = sslContextCreateConnection(&s_ssl_ctx, &c->ssl);
    if (R_FAILED(rc))
    {
        seterr(err, n, "ssl connection: rc=0x%x", rc);
        conn_close(c);
        return 0;
    }
    c->ssl_open = 1;
    const int sfd = socketSslConnectionSetSocketDescriptor(&c->ssl, c->fd);
    if (sfd == -1 && errno != ENOENT)
    {
        seterr(err, n, "ssl socket: errno %d", errno);
        conn_close(c);
        return 0;
    }
    c->ssl_fd = sfd;
    rc = sslConnectionSetHostName(&c->ssl, u->host, (u32)strlen(u->host));
    if (R_SUCCEEDED(rc))
        rc = sslConnectionSetVerifyOption(&c->ssl, SslVerifyOption_PeerCa | SslVerifyOption_HostName);
    if (R_SUCCEEDED(rc))
        rc = sslConnectionSetIoMode(&c->ssl, SslIoMode_Blocking);
    if (R_SUCCEEDED(rc))
        rc = sslConnectionDoHandshake(&c->ssl, NULL, NULL, NULL, 0);
    if (R_FAILED(rc))
    {
        /* 0x..d7b / ..ddb-style results here are certificate trouble: wrong console date, unknown root. */
        seterr(err, n, "TLS handshake with %s: rc=0x%x", u->host, rc);
        conn_close(c);
        return 0;
    }
    return 1;
}

/* >0 bytes, 0 end of stream, <0 error. A TLS read error after the body started is reported as the end: the caller's
 * length / hash checks decide whether that end was complete. */
static int conn_recv(conn* c, void* dst, int n)
{
    if (c->tls)
    {
        u32 got = 0;
        const Result rc = sslConnectionRead(&c->ssl, dst, (u32)n, &got);
        if (R_FAILED(rc))
            return 0;
        return (int)got;
    }
    const ssize_t r = recv(c->fd, dst, (size_t)n, 0);
    return r < 0 ? -1 : (int)r;
}

static int conn_send_all(conn* c, const char* src, int n)
{
    while (n > 0)
    {
        int w;
        if (c->tls)
        {
            u32 put = 0;
            if (R_FAILED(sslConnectionWrite(&c->ssl, src, (u32)n, &put)) || !put)
                return 0;
            w = (int)put;
        }
        else
        {
            const ssize_t r = send(c->fd, src, (size_t)n, 0);
            if (r <= 0)
                return 0;
            w = (int)r;
        }
        src += w;
        n -= w;
    }
    return 1;
}

static int rd_fill(conn* c)
{
    if (c->pos < c->len)
        return 1;
    c->pos = 0;
    c->len = conn_recv(c, c->buf, (int)sizeof(c->buf));
    if (c->len < 0)
        c->len = 0;
    return c->len > 0;
}

/* One line without CR/LF; 0 at the end of the stream. */
static int rd_line(conn* c, char* out, int max)
{
    int k = 0;
    for (;;)
    {
        if (!rd_fill(c))
            return k > 0 ? (out[k] = 0, 1) : 0;
        const char ch = c->buf[c->pos++];
        if (ch == '\n')
            break;
        if (ch != '\r' && k < max - 1)
            out[k++] = ch;
    }
    out[k] = 0;
    return 1;
}

/* Copies up to n body bytes into out; -1 when cancelled or the disk refused. */
static long long rd_copy(conn* c, long long n, FILE* out, upd_progress* p, int* eof)
{
    long long left = n;
    *eof = 0;
    while (left != 0)
    {
        if (p && p->cancel)
            return -1;
        if (!rd_fill(c))
        {
            *eof = 1;
            break;
        }
        int take = c->len - c->pos;
        if (left > 0 && take > left)
            take = (int)left;
        if (out && fwrite(c->buf + c->pos, 1, (size_t)take, out) != (size_t)take)
            return -2;
        c->pos += take;
        if (left > 0)
            left -= take;
        if (p)
            p->done += take;
    }
    return n < 0 ? 0 : n - left;
}

/* ----------------------------------------------------------------- GET ---- */

int upd_http_get(const char* url, const char* dst_path, upd_progress* p, char* err, int err_size)
{
    char cur[2048];
    snprintf(cur, sizeof(cur), "%s", url);
    if (p)
        p->done = 0, p->total = -1;

    static __thread conn* s_conn; /* 64 KB: not on the worker's stack */
    if (!s_conn)
        s_conn = (conn*)malloc(sizeof(conn));
    if (!s_conn)
    {
        seterr(err, err_size, "out of memory");
        return -1;
    }
    conn* c = s_conn;

    for (int hop = 0; hop < 8; ++hop)
    {
        url_parts u;
        if (!parse_url(cur, &u))
        {
            seterr(err, err_size, "bad url: %.200s", cur);
            return -1;
        }
        if (!conn_open(c, &u, err, err_size))
            return -1;

        char req[2600];
        const int rl = snprintf(req, sizeof(req),
                                "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ogsr-updater/1 (switch)\r\nAccept: */*\r\n"
                                "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                                u.path, u.host);
        if (rl <= 0 || rl >= (int)sizeof(req) || !conn_send_all(c, req, rl))
        {
            seterr(err, err_size, "request to %s failed", u.host);
            conn_close(c);
            return -1;
        }

        char line[2048];
        if (!rd_line(c, line, sizeof(line)) || strncmp(line, "HTTP/", 5))
        {
            seterr(err, err_size, "no HTTP answer from %s", u.host);
            conn_close(c);
            return -1;
        }
        const char* sp = strchr(line, ' ');
        const int status = sp ? atoi(sp + 1) : 0;
        long long clen = -1;
        int chunked = 0;
        char location[2048] = {0};
        while (rd_line(c, line, sizeof(line)) && line[0])
        {
            if (!strncasecmp(line, "content-length:", 15))
                clen = atoll(line + 15);
            else if (!strncasecmp(line, "transfer-encoding:", 18) && strstr(line + 18, "chunked"))
                chunked = 1;
            else if (!strncasecmp(line, "location:", 9))
            {
                const char* v = line + 9;
                while (*v == ' ' || *v == '\t')
                    ++v;
                snprintf(location, sizeof(location), "%s", v);
            }
        }

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        {
            conn_close(c);
            if (!location[0])
            {
                seterr(err, err_size, "HTTP %d without Location", status);
                return -1;
            }
            if (location[0] == '/')
            {
                char abs[2048];
                if (u.port == (u.tls ? 443 : 80))
                    snprintf(abs, sizeof(abs), "%s://%s%s", u.tls ? "https" : "http", u.host, location);
                else
                    snprintf(abs, sizeof(abs), "%s://%s:%d%s", u.tls ? "https" : "http", u.host, u.port, location);
                snprintf(cur, sizeof(cur), "%s", abs);
            }
            else
                snprintf(cur, sizeof(cur), "%s", location);
            continue;
        }
        if (status != 200)
        {
            seterr(err, err_size, "HTTP %d", status);
            conn_close(c);
            return -1;
        }

        FILE* out = fopen(dst_path, "wb");
        if (!out)
        {
            seterr(err, err_size, "cannot write %s (errno %d)", dst_path, errno);
            conn_close(c);
            return -1;
        }
        setvbuf(out, NULL, _IOFBF, 256 * 1024); /* the SD likes big writes */
        if (p)
            p->total = chunked ? -1 : clen;

        int ok = 1, eof = 0;
        if (chunked)
        {
            for (;;)
            {
                if (!rd_line(c, line, sizeof(line)))
                {
                    ok = 0;
                    seterr(err, err_size, "connection lost in a chunk header");
                    break;
                }
                const long long sz = strtoll(line, NULL, 16);
                if (sz <= 0)
                {
                    while (rd_line(c, line, sizeof(line)) && line[0]) /* trailers */
                        ;
                    break;
                }
                const long long got = rd_copy(c, sz, out, p, &eof);
                if (got != sz)
                {
                    ok = 0;
                    seterr(err, err_size, got == -1 ? "cancelled" : got == -2 ? "disk write failed" : "connection lost");
                    break;
                }
                rd_line(c, line, sizeof(line)); /* the CRLF after the data */
            }
        }
        else
        {
            const long long got = rd_copy(c, clen, out, p, &eof);
            if (got == -1 || got == -2 || (clen >= 0 && got != clen))
            {
                ok = 0;
                seterr(err, err_size, got == -1 ? "cancelled" : got == -2 ? "disk write failed" : "connection lost (%lld of %lld)", got, clen);
            }
        }
        if (fclose(out) != 0 && ok)
        {
            ok = 0;
            seterr(err, err_size, "disk write failed");
        }
        conn_close(c);
        if (!ok)
            remove(dst_path);
        return ok ? 0 : -1;
    }
    seterr(err, err_size, "too many redirects");
    return -1;
}

int upd_nx_relaunch(const char* nro_path)
{
    if (!envHasNextLoad())
        return -1;
    char argv[600];
    if (strchr(nro_path, ' '))
        snprintf(argv, sizeof(argv), "\"%s\"", nro_path);
    else
        snprintf(argv, sizeof(argv), "%s", nro_path);
    const Result rc = envSetNextLoad(nro_path, argv);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

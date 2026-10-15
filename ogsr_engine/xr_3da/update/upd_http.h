#pragma once

/* The updater's transport (see xr_update.h): one HTTPS GET into a file, redirects followed (GitHub's release links
 * redirect to its asset storage). One implementation per platform:
 *   upd_http_win.cpp      WinHTTP (system certificate store, proxy settings)
 *   upd_http_nx.c         BSD sockets + the Switch's own ssl service (system CA list), our own HTTP/1.1
 *   upd_http_android.cpp  com.ogsr.online.OgsrUpdate (java): HttpURLConnection through JNI
 * C, not C++: the Switch one includes <switch.h>, which collides with the engine's types in C++ translation units.
 * Every call blocks; the updater runs them on its worker thread. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct upd_progress
{
    volatile long long done; /* bytes of the body written so far */
    volatile long long total; /* Content-Length, -1 when the server did not say */
    volatile int cancel; /* set from another thread: the transfer stops at the next chunk */
} upd_progress;

/* 0 = the whole body is in dst_path (created or truncated). Otherwise err holds a one-line reason; an HTTP status
 * other than 200 is an error ("HTTP 404"). */
int upd_http_get(const char* url, const char* dst_path, upd_progress* p, char* err, int err_size);

/* Switch only (upd_http_nx.c): start this NRO again once the process exits. 0 = armed. */
int upd_nx_relaunch(const char* nro_path);

#ifdef __cplusplus
}
#endif

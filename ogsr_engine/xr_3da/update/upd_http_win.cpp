// The updater's HTTPS on Windows (see upd_http.h): WinHTTP -- the system's certificate store and proxy settings,
// redirects followed by WinHTTP itself (https -> https only, which is all GitHub does).
#include "stdafx.h"

#ifdef _WIN32
#include "upd_http.h"

#include <winhttp.h>
#include <stdio.h>
#pragma comment(lib, "winhttp.lib")

namespace
{
void seterr(char* err, int n, const char* fmt, ...)
{
    if (!err || n <= 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, size_t(n), fmt, ap);
    va_end(ap);
}

struct Handle
{
    HINTERNET h{};
    ~Handle()
    {
        if (h)
            WinHttpCloseHandle(h);
    }
};
} // namespace

extern "C" int upd_http_get(const char* url, const char* dst_path, upd_progress* p, char* err, int err_size)
{
    if (p)
        p->done = 0, p->total = -1;

    wchar_t wurl[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048))
    {
        seterr(err, err_size, "bad url");
        return -1;
    }
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[1024]{};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
    {
        seterr(err, err_size, "bad url: %.200s", url);
        return -1;
    }
    wcscat_s(path, extra);

    Handle session, connect, request;
    session.h = WinHttpOpen(L"ogsr-updater/1 (windows)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) // before Windows 8.1 there is no AUTOMATIC_PROXY
        session.h = WinHttpOpen(L"ogsr-updater/1 (windows)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h)
    {
        seterr(err, err_size, "WinHttpOpen: %lu", GetLastError());
        return -1;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 30000, 30000);
    connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    if (!connect.h)
    {
        seterr(err, err_size, "connect: %lu", GetLastError());
        return -1;
    }
    const bool tls = uc.nScheme == INTERNET_SCHEME_HTTPS;
    request.h = WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, tls ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h)
    {
        seterr(err, err_size, "request: %lu", GetLastError());
        return -1;
    }
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr))
    {
        const DWORD e = GetLastError();
        seterr(err, err_size, e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "offline (name not resolved)" : e == ERROR_WINHTTP_TIMEOUT ? "timeout" : "send: %lu", e);
        return -1;
    }
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
    {
        seterr(err, err_size, "HTTP %lu", status);
        return -1;
    }
    wchar_t lenbuf[32]{};
    sz = sizeof(lenbuf);
    if (p && WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenbuf, &sz, WINHTTP_NO_HEADER_INDEX))
        p->total = _wtoi64(lenbuf);

    FILE* out = fopen(dst_path, "wb");
    if (!out)
    {
        seterr(err, err_size, "cannot write %s", dst_path);
        return -1;
    }
    static thread_local char buf[256 * 1024];
    bool ok = true;
    for (;;)
    {
        if (p && p->cancel)
        {
            ok = false;
            seterr(err, err_size, "cancelled");
            break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf, sizeof(buf), &got))
        {
            ok = false;
            seterr(err, err_size, "connection lost: %lu", GetLastError());
            break;
        }
        if (!got)
            break;
        if (fwrite(buf, 1, got, out) != got)
        {
            ok = false;
            seterr(err, err_size, "disk write failed");
            break;
        }
        if (p)
            p->done += got;
    }
    if (fclose(out) != 0 && ok)
    {
        ok = false;
        seterr(err, err_size, "disk write failed");
    }
    if (ok && p && p->total >= 0 && p->done != p->total)
    {
        ok = false;
        seterr(err, err_size, "connection lost (%lld of %lld)", p->done, p->total);
    }
    if (!ok)
        remove(dst_path);
    return ok ? 0 : -1;
}
#elif !defined(__ANDROID__) && !defined(__SWITCH__)
// Desktop Linux: no client yet (the updater then reports "offline" and stays quiet).
#include "upd_http.h"
#include <stdio.h>
extern "C" int upd_http_get(const char*, const char*, upd_progress*, char* err, int err_size)
{
    if (err && err_size > 0)
        snprintf(err, size_t(err_size), "no HTTPS client on this platform");
    return -1;
}
#endif

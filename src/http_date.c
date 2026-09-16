/* http_date.c — Date header retrieval. See http_date.h for why this tier exists.
 *
 * No WinHTTP: winhttp.dll is an optional component on some Win7 SKUs, while
 * wininet.dll is guaranteed present and is on the allowed-DLL list. No TLS: a
 * one-second-granular advisory header does not warrant a handshake we would
 * then have to trust-store, and every value is clamped to a sane window.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_NO_WINDOWS
#include "config.h"
#include "http_date.h"
#include "verdict.h"        /* tk_candidate_ok: the same window as the NTP tier */
#include "winutil.h"

#include <winsock2.h>
#include <windows.h>
#include <wininet.h>
#include "tklibc.h"

/* Some MinGW wininet.h releases omit this one; the value is fixed by the
 * platform header (winineti.h) and is 113 on every Windows version that has
 * it, so a guard is safe for both rc/windres and MSVC. */
#ifndef INTERNET_OPTION_RESOLVE_TIMEOUT
#define INTERNET_OPTION_RESOLVE_TIMEOUT 113
#endif
#ifndef HTTP_QUERY_RAW_HEADERS_CRLF
#define HTTP_QUERY_RAW_HEADERS_CRLF 22      /* wininet.h on every supported OS */
#endif

/* --------------------------------------------------------------- validation */

static int accept_epoch(time64_t e, time64_t *out)
{
    if (!tk_candidate_ok(e, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR)) return 0;
    *out = e;
    return 1;
}

/* Split "host[/path]" into a host buffer and a path pointer into spec. */
static int split_spec(const char *spec, char *host, size_t host_cap, const char **path)
{
    size_t i = 0;

    *path = "/";
    while (i + 1 < host_cap && spec[i] && spec[i] != '/') { host[i] = spec[i]; i++; }
    host[i] = 0;
    if (i == 0) return 0;                       /* empty host            */
    if (spec[i] == '\0') return 1;              /* no path component     */
    if (spec[i] == '/')  { *path = spec + i; return 1; }
    return 0;                                   /* host longer than buffer */
}

/* --------------------------------------------------------- A. WinINet, HEAD */

static int try_wininet(const char *spec, time64_t *out, char *fail, size_t fail_cap)
{
    HINTERNET ie = NULL, con = NULL, req = NULL;
    char     host[64];
    const char *path = "/";
    wchar_t  whost[80], wpath[96];
    DWORD    tmo, got = 0;
    char     hdr[96];
    time64_t e = TK_EPOCH_INVALID;
    size_t   q = 0;
    int      rc = 0;

    if (!split_spec(spec, host, sizeof(host), &path)) return 0;
    tk_utf8_wcs(host, whost, sizeof(whost) / sizeof(whost[0]));
    tk_utf8_wcs(path, wpath, sizeof(wpath) / sizeof(wpath[0]));

    ie = InternetOpenW(L"TimeKeeper/" TK_VERSION_STR, INTERNET_OPEN_TYPE_PRECONFIG,
                       NULL, NULL, 0);
    if (!ie) { lstrcpynA(fail, "InternetOpen failed", (int)fail_cap); return 0; }

    /* Bounding each WinINet stage (resolver, connect, send, receive) is the
     * only way this tier can honour the boot budget: the API has no overall
     * deadline of its own. */
#define TK_TMO(opt) do { tmo = TK_HTTP_TIMEOUT_MS; \
        InternetSetOptionW(ie, (opt), &tmo, sizeof(tmo)); } while (0)
    TK_TMO(INTERNET_OPTION_RESOLVE_TIMEOUT);
    TK_TMO(INTERNET_OPTION_CONNECT_TIMEOUT);
    TK_TMO(INTERNET_OPTION_SEND_TIMEOUT);
    TK_TMO(INTERNET_OPTION_RECEIVE_TIMEOUT);
#undef TK_TMO

    con = InternetConnectW(ie, whost, (INTERNET_PORT)TK_HTTP_PORT, NULL, NULL,
                           INTERNET_SERVICE_HTTP, 0, 0);
    if (!con) { lstrcpynA(fail, "InternetConnect failed", (int)fail_cap); goto done; }

    req = HttpOpenRequestW(con, L"HEAD", wpath, NULL, NULL, NULL,
                           INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE |
                           INTERNET_FLAG_KEEP_CONNECTION, 0);
    if (!req) { lstrcpynA(fail, "HttpOpenRequest failed", (int)fail_cap); goto done; }

    if (!HttpSendRequestW(req, NULL, 0, NULL, 0)) {
        lstrcpynA(fail, "HttpSendRequest failed", (int)fail_cap);
        goto done;
    }
    /* Ask for the whole header block and run it through the same extractor the
     * raw transport uses. Two reasons, both learned by running the .exe:
     *   - HTTP_QUERY_DATE follows a two-call buffer contract (probe, then
     *     fetch); a single call with a big buffer can legitimately return
     *     FALSE/0, which reads as "no Date header" on a perfectly good 200.
     *   - one parser for both transports means one place to audit. */
    {
        static char blk[TK_HTTP_MAX_RESP_BYTES];
        size_t blen = 0;
        DWORD  have = (DWORD)sizeof(blk);

        tk_memset(blk, 0, sizeof(blk));
        if (HttpQueryInfoA(req, HTTP_QUERY_RAW_HEADERS_CRLF, blk, &have, NULL)
            && have > 0) {
            blen = (size_t)have;
        } else {
            DWORD need = 0;
            HttpQueryInfoA(req, HTTP_QUERY_RAW_HEADERS_CRLF, NULL, &need, NULL);
            if (need > 0) {
                if (need > sizeof(blk) - 1) need = sizeof(blk) - 1;
                have = need;
                if (HttpQueryInfoA(req, HTTP_QUERY_RAW_HEADERS_CRLF, blk, &have, NULL)
                    && have > 0)
                    blen = (size_t)have;
            }
        }
        if (blen == 0) {
            /* Fall back to the single-header query, sized properly. */
            tk_memset(hdr, 0, sizeof(hdr));
            got = sizeof(hdr);
            if (!HttpQueryInfoA(req, HTTP_QUERY_DATE, hdr, &got, NULL) || got == 0) {
                lstrcpynA(fail, "no Date header", (int)fail_cap);
                goto done;
            }
            hdr[sizeof(hdr) - 1] = 0;
            /* Truncate at the first line break in place. Written as an index
             * loop rather than strpbrk+cast, because casting away const from a
             * returned pointer is the kind of thing a reviewer has to stop and
             * think about, and this is five lines that need no thinking. */
            for (q = 0; q + 1 < sizeof(hdr) && hdr[q] && hdr[q] != '\r' &&
                    hdr[q] != '\n'; q++) { }
            hdr[q] = 0;
            if (!tk_parse_http_date(hdr, tk_strlen(hdr), &e)) {
                lstrcpynA(fail, "unparseable Date header", (int)fail_cap);
                goto done;
            }
        } else {
            blk[blen < sizeof(blk) ? blen : sizeof(blk) - 1] = 0;
            if (!tk_http_extract_date(blk, tk_strlen(blk), &e)) {
                lstrcpynA(fail, "no/undated Date header", (int)fail_cap);
                goto done;
            }
        }
    }
    if (!accept_epoch(e, out)) { lstrcpynA(fail, "Date out of range", (int)fail_cap); goto done; }
    rc = 1;

done:
    if (req) InternetCloseHandle(req);
    if (con) InternetCloseHandle(con);
    if (ie)  InternetCloseHandle(ie);
    return rc;
}

/* ------------------------------------------------ B. raw TCP/80, no DNS/proxy */

/* Status code of "HTTP/1.1 200 OK" parsed without atoi/strtol. */
static int http_status(const char *resp, int len)
{
    int i = 0;
    if (len < 12 || tk_strncmp(resp, "HTTP/1", 6) != 0) return -1;
    while (i < len && resp[i] != ' ') i++;
    if (i + 3 >= len || resp[i+1] < '0' || resp[i+1] > '9' ||
        resp[i+2] < '0' || resp[i+2] > '9' ||
        resp[i+3] < '0' || resp[i+3] > '9') return -1;
    return (resp[i+1] - '0') * 100 + (resp[i+2] - '0') * 10 + (resp[i+3] - '0');
}

static int try_raw_ip(const char *ip, const char *host_hdr, const char *path,
                      uint32_t deadline_tick, time64_t *out,
                      char *fail, size_t fail_cap)
{
    static char resp[TK_HTTP_MAX_RESP_BYTES];
    char        req[256];
    size_t      rn = 0;
    /* SOCKET is a UINT_PTR: 64 bits on x64. Storing it in an int truncates the
     * handle, which "works" on most machines by luck and fails on some. */
    SOCKET      s = INVALID_SOCKET;
    int         total = 0, st;
    unsigned long addr;
    struct sockaddr_in sa;
    uint32_t    t0, remain;
    BOOL        connected = FALSE;

    addr = inet_addr(ip);
    if (addr == INADDR_NONE) { lstrcpynA(fail, "bad IP literal", (int)fail_cap); return 0; }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { lstrcpynA(fail, "socket failed", (int)fail_cap); return 0; }

#define PUT(x) do { const char *p_ = (x); while (*p_ && rn + 2 < sizeof(req)) req[rn++] = *p_++; } while (0)
    PUT("GET "); PUT(path); PUT(" HTTP/1.1\r\nHost: "); PUT(host_hdr);
    PUT("\r\nUser-Agent: TimeKeeper/" TK_VERSION_STR "\r\nConnection: close\r\n\r\n");
#undef PUT

    /* connect() has no timeout option; a SYN into a black hole is the classic
     * boot-time stall, so it goes non-blocking and select() does the waiting. */
    {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        tk_memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(TK_HTTP_PORT);
        sa.sin_addr.s_addr = addr;
        t0 = (uint32_t)tk_tick();
        if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            connected = TRUE;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wf;
            struct timeval tv;
            int err = 0, elen = sizeof(err), r;
            FD_ZERO(&wf); FD_SET(s, &wf);
            tv.tv_sec  = TK_HTTP_IP_TIMEOUT_MS / 1000;
            tv.tv_usec = (TK_HTTP_IP_TIMEOUT_MS % 1000) * 1000;
            r = select(0, NULL, &wf, NULL, &tv);
            if (r > 0 && getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) == 0 &&
                err == 0) connected = TRUE;
        }
        if (!connected) {
            closesocket(s);
            lstrcpynA(fail, (tk_tick() - t0) > (uint32_t)TK_HTTP_IP_TIMEOUT_MS
                            ? "connect timeout" : "connect refused", (int)fail_cap);
            return 0;
        }
        { u_long b = 0; ioctlsocket(s, FIONBIO, &b); }   /* back to blocking */
    }

    {
        struct timeval tv;
        tv.tv_sec  = TK_HTTP_IP_TIMEOUT_MS / 1000;
        tv.tv_usec = (TK_HTTP_IP_TIMEOUT_MS % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    }
    if (send(s, req, (int)rn, 0) <= 0) {
        closesocket(s); lstrcpynA(fail, "send failed", (int)fail_cap); return 0;
    }

    /* Stop at the end of the header block: the body is irrelevant and waiting
     * for it is how a keep-alive server turns a 1 s probe into a timeout. */
    while (total < (int)sizeof(resp) - 1) {
        int n;
        (void)remain;
        if (tk_tick_left((uint32_t)tk_tick(), deadline_tick) < 50u && total > 0) break;
        n = recv(s, resp + total, (int)sizeof(resp) - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += n;
        resp[total] = 0;
        if (tk_strstr(resp, "\r\n\r\n") || tk_strstr(resp, "\n\n")) break;
    }
    closesocket(s);

    if (total < 40) { lstrcpynA(fail, "no response", (int)fail_cap); return 0; }
    st = http_status(resp, total);
    if (st < 0)  { lstrcpynA(fail, "not HTTP", (int)fail_cap); return 0; }
    if (st != 200 && st != 204 && st != 304) {
        /* 3xx and captive-portal 200-with-HTML are refused: a hotel login page
         * serving its own clock is exactly the trap this tier must not fall in.
         * WinINet follows redirects itself, so this check only guards the raw
         * path, where we deliberately do not follow anything. */
        lstrcpynA(fail, "status refused", (int)fail_cap);
        return 0;
    }
    {
        time64_t e = TK_EPOCH_INVALID;
        if (!tk_http_extract_date(resp, (size_t)total, &e)) {
            lstrcpynA(fail, "no/undated Date header", (int)fail_cap);
            return 0;
        }
        if (!accept_epoch(e, out)) { lstrcpynA(fail, "Date out of range", (int)fail_cap); return 0; }
    }
    return 1;
}

/* ------------------------------------------------------------------- driver */

int tk_http_date(uint32_t deadline_tick, tk_http_result_t *out)
{
    static const char *const kHosts[] = TK_HTTP_HOSTS;
    static const char *const kIPs[]   = TK_HTTP_IP_ENDPOINTS;
    WSADATA  wsa;
    int      i, wsa_up = 0;
    time64_t e = TK_EPOCH_INVALID;

    tk_memset(out, 0, sizeof(*out));
    out->utc_sec = TK_EPOCH_INVALID;
    out->status  = -1;
    lstrcpynA(out->last_fail, "not attempted", sizeof(out->last_fail));

    /* The raw path needs Winsock; WinINet does not, so a Winsock failure must
     * not skip tier A. */
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) wsa_up = 1;

    for (i = 0; i < TK_HTTP_HOST_COUNT; i++) {
        if (tk_tick_left((uint32_t)tk_tick(), deadline_tick) < 250u) {
            lstrcpynA(out->last_fail, "budget exhausted", sizeof(out->last_fail));
            out->status = -2;
            break;
        }
        if (try_wininet(kHosts[i], &e, out->fail_wininet, sizeof(out->fail_wininet))) {
            out->utc_sec = e;
            out->transport = 0;
            out->status = 0;
            lstrcpynA(out->server, kHosts[i], sizeof(out->server));
            goto ok;
        }
        out->status = -3;
    }
    /* Copy the first meaningful failure into last_fail so the caller's single
     * line explains BOTH transports: "wininet: X; raw: Y" is what tells an
     * operator whether to fix the proxy or the route. */
    if (out->fail_wininet[0])
        lstrcpynA(out->last_fail, out->fail_wininet, sizeof(out->last_fail));

    if (wsa_up) {
        for (i = 0; i < TK_HTTP_IP_ENDPOINT_COUNT; i++) {
            char host_hdr[64];
            size_t k = 0;
            if (tk_tick_left((uint32_t)tk_tick(), deadline_tick) < 250u) {
                out->status = -2;
                break;
            }
            while (k + 1 < sizeof(host_hdr) && kIPs[i][k]) { host_hdr[k] = kIPs[i][k]; k++; }
            host_hdr[k] = 0;
            if (try_raw_ip(host_hdr, host_hdr, "/", deadline_tick, &e,
                           out->fail_raw, sizeof(out->fail_raw))) {
                out->utc_sec = e;
                out->transport = 1;
                out->status = 0;
                lstrcpynA(out->server, host_hdr, sizeof(out->server));
                goto ok;
            }
            out->status = -4;
        }
        if (out->fail_raw[0]) {
            size_t fn = 0;
            while (fn + 1 < sizeof(out->last_fail) && out->last_fail[fn]) fn++;
            if (fn + 2 < sizeof(out->last_fail)) {
                const char *p = "; raw: ";
                out->last_fail[fn++] = ';'; out->last_fail[fn++] = ' ';
                (void)p;
                { const char *q = out->fail_raw;
                  while (*q && fn + 1 < sizeof(out->last_fail)) out->last_fail[fn++] = *q++;
                  out->last_fail[fn] = 0; }
            }
        }
    }
    if (wsa_up) WSACleanup();
    return 0;

ok:
    if (wsa_up) WSACleanup();
    return 1;
}

#endif /* TIMEKEEPER_NO_WINDOWS */

/* sntp.c — 48-byte UDP SNTP client. No w32time, no third-party NTP library.
 *
 * Deliberately single-threaded and blocking: the whole run must stay under
 * ~1.5 s and the process must not linger, so a thread per probe would add cost
 * (and a cleanup path) for nothing. Instead every wait is bounded twice — by
 * the socket option and by select() — and the loop checks a global deadline
 * before each server so a bad network cannot stretch the run.
 *
 * The one wait Winsock cannot bound is getaddrinfo (no timeout parameter
 * exists). It normally returns in single-digit ms because it consults the host
 * file and the resolver cache first; see README Troubleshooting for the
 * dead-DNS-server case, which the HTTP tier handles without any DNS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_NO_WINDOWS
#include "config.h"
#include "tklibc.h"
#include "sntp.h"
#include "sntp_proto.h"
#include "winutil.h"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

typedef struct { int wsa_up; } tk_wsa_t;

static int wsa_ensure(tk_wsa_t *w)
{
    WSADATA d;
    if (w->wsa_up) return 1;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return 0;
    w->wsa_up = 1;
    return 1;
}

/* Hostname or dotted quad. getaddrinfo only for names, to keep the numeric path
 * free of the resolver entirely (helps on networks where DNS is broken). */
static int resolve(const char *host, uint32_t *addr_out)
{
    unsigned long literal;

    *addr_out = 0;
    literal = inet_addr(host);
    if (literal != INADDR_NONE) { *addr_out = literal; return 1; }

    {
        struct addrinfo hints, *ai = NULL;
        int r;
        tk_memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags    = AI_NUMERICSERV;   /* never return canonical names */
        r = getaddrinfo(host, TK_NTP_PORT_STR, &hints, &ai);
        if (r != 0 || !ai || ai->ai_family != AF_INET ||
            ai->ai_addrlen < sizeof(struct sockaddr_in)) {
            if (ai) freeaddrinfo(ai);
            return 0;
        }
        *addr_out = ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(ai);
    }
    return *addr_out != 0;
}

int tk_sntp_sync(uint32_t deadline_tick, tk_sntp_result_t *out)
{
    static const char *const kServers[] = TK_NTP_SERVERS;
    tk_wsa_t   wsa;
    uint8_t    req[TK_NTP_PACKET_SIZE];
    uint8_t    reply[TK_NTP_PACKET_SIZE + 16];
    int        i, first_fail = 0;
    time64_t   now = TK_EPOCH_INVALID;
    uint32_t   ms  = 0;

    tk_memset(out, 0, sizeof(*out));
    out->utc_sec = TK_EPOCH_INVALID;
    out->status  = 0;
    wsa.wsa_up   = 0;

    if (!wsa_ensure(&wsa)) {
        out->status = -1;
        tk_memcpy(out->last_fail, "WSAStartup failed", 18);
        return 0;
    }
    if (!tk_win_now_epoch(&now, &ms)) {
        out->status = -2;
        tk_memcpy(out->last_fail, "system clock unreadable", 24);
        if (wsa.wsa_up) WSACleanup();
        return 0;
    }
    tk_ntp_build_request(req, now);

    for (i = 0; i < TK_NTP_SERVER_COUNT; i++) {
        const char   *host = kServers[i];
        uint32_t      addr = 0;
        SOCKET        s;
        struct sockaddr_in sa;
        fd_set        rfds;
        struct timeval  tv;
        uint32_t      t_start, left, recv_ms;
        int           n, sel;

        left = tk_tick_left((uint32_t)tk_tick(), deadline_tick);
        if (left < TK_NET_MIN_LEFT_MS) {
            if (!first_fail) tk_memcpy(out->last_fail, "network budget exhausted", 25);
            break;
        }
        recv_ms = (left < TK_NTP_TIMEOUT_MS) ? left : TK_NTP_TIMEOUT_MS;

        int local_dst = 0;
        if (!resolve(host, &addr)) {
            if (!first_fail) first_fail = -3;
            continue;
        }
        /* 127.0.0.0/8 and 169.254.0.0/16 in network order -> host order. */
        {
            uint32_t ha = ntohl(addr);
            local_dst = ((ha >> 24) == 127u) || ((ha >> 16) == 169u && ((ha >> 8) & 0xffu) == 254u);
        }
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) { if (!first_fail) first_fail = -4; continue; }

        tv.tv_sec  = recv_ms / 1000u;
        tv.tv_usec = (long)((recv_ms % 1000u) * 1000u);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

        tk_memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(TK_NTP_PORT);
        sa.sin_addr.s_addr = addr;

        t_start = tk_tick();
        if (sendto(s, (const char *)req, TK_NTP_PACKET_SIZE, 0,
                   (struct sockaddr *)&sa, sizeof(sa)) != TK_NTP_PACKET_SIZE) {
            closesocket(s);
            if (!first_fail) first_fail = -5;
            continue;
        }

        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        tv.tv_sec  = recv_ms / 1000u;
        tv.tv_usec = (long)((recv_ms % 1000u) * 1000u);
        sel = select(0, &rfds, NULL, NULL, &tv);
        if (sel <= 0) {
            closesocket(s);
            if (!first_fail) first_fail = TKNS_ZERO_TS;      /* timeout */
            continue;
        }
        n = recv(s, (char *)reply, (int)sizeof(reply), 0);
        out->rtt_ms = (int)(tk_tick() - t_start);
        closesocket(s);
        if (n < TK_NTP_PACKET_SIZE) { if (!first_fail) first_fail = TKNS_TOO_SHORT; continue; }

        {
            time64_t sec = TK_EPOCH_INVALID;
            uint32_t frac_ms = 0;
            int st = tk_ntp_parse_reply(reply, (size_t)n, req,
                                        TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR,
                                        &sec, &frac_ms);
            if (st != TKNS_OK) { if (!first_fail) first_fail = st; continue; }
            /* Local-source self-reference guard, loopback/link-local only: a
             * server on this very machine that agrees with the clock we already
             * have is a feedback loop, not a time source. A public server may
             * legitimately agree with us (correct clock) and must pass. */
            if (local_dst && tk_ntp_is_looped(sec, now, TK_NTP_TX_TOLERANCE_SEC)) {
                if (!first_fail) first_fail = TKNS_LOOPED;
                continue;
            }
            out->utc_sec = sec;
            out->ms      = frac_ms;
            out->status  = TKNS_OK;
            { size_t k = 0; while (host[k] && k < sizeof(out->server) - 1) {
                    out->server[k] = host[k]; k++; } out->server[k] = 0; }
            if (wsa.wsa_up) WSACleanup();
            return 1;
        }
    }

    if (first_fail) out->status = first_fail;
    else {
        out->status = TKNS_ZERO_TS;
        tk_memcpy(out->last_fail, "no reply from any server", 25);
    }
    if (wsa.wsa_up) WSACleanup();
    return 0;
}

#endif /* TIMEKEEPER_NO_WINDOWS */

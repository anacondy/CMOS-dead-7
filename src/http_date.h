/* http_date.h — secondary time tier: the HTTP/1.1 Date header.
 *
 * This exists because an alarming number of Indian ISP / hostel / corporate
 * firewalls drop outbound UDP/123 while leaving TCP/80 wide open. A server's
 * Date header is only second-granular and unauthenticated, which is entirely
 * sufficient for a machine whose clock is off by years.
 *
 * Two transports, tried in order:
 *   A. WinINet HEAD   — honours the system proxy, which is what a corporate
 *                       network actually needs to reach the internet at all.
 *   B. raw TCP/80 GET — to hard-coded anycast IPs, no DNS and no proxy. This is
 *                       the rescue path when DNS is broken or the proxy is down.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_HTTP_DATE_H
#define TIMEKEEPER_HTTP_DATE_H

#include "timeutil.h"

#ifndef TIMEKEEPER_NO_WINDOWS
#include <windows.h>

typedef struct {
    time64_t utc_sec;
    char     server[48];      /* "www.google.com" or the IP literal */
    int      transport;       /* 0 = wininet, 1 = raw socket        */
    int      status;          /* 0 ok, negative = why not           */
    char     last_fail[64];   /* the most recent attempt's reason    */
    char     fail_wininet[64];/* first WinINet failure, kept separate*/
    char     fail_raw[64];    /* first raw-socket failure, likewise  */
} tk_http_result_t;

/* One shot per endpoint, bounded by deadline_tick (absolute GetTickCount).
 * Returns 1 when *out holds a plausible UTC time. */
int tk_http_date(uint32_t deadline_tick, tk_http_result_t *out);

#endif /* !TIMEKEEPER_NO_WINDOWS */
#endif /* TIMEKEEPER_HTTP_DATE_H */

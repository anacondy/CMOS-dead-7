/* sntp.h — minimal SNTP client (RFC 4330), blocking, deadline-bounded.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_SNTP_H
#define TIMEKEEPER_SNTP_H

#include "timeutil.h"

#ifndef TIMEKEEPER_NO_WINDOWS
#include <windows.h>

typedef struct {
    time64_t utc_sec;          /* TK_EPOCH_INVALID if nothing usable    */
    uint32_t ms;               /* sub-second part of the server's stamp */
    char     server[40];       /* which host answered                   */
    int      rtt_ms;           /* measured, logged, never used          */
    int      status;           /* tk_ntp_status_t of the last attempt   */
    char     last_fail[64];    /* short reason for the log              */
} tk_sntp_result_t;

/* Tries the servers in config.h order and returns on the first valid reply.
 * `deadline_tick` is an absolute GetTickCount value: no server is started when
 * less than TK_NET_MIN_LEFT_MS remain, and the receive timeout is clamped to
 * what is left, so the whole phase is bounded by config.h even if the OS is
 * slow. Returns 1 when *out holds a usable time. */
int tk_sntp_sync(uint32_t deadline_tick, tk_sntp_result_t *out);

#endif /* !TIMEKEEPER_NO_WINDOWS */
#endif /* TIMEKEEPER_SNTP_H */

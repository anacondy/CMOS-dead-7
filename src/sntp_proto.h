/* sntp_proto.h — the RFC 4330 wire format, pure. No winsock, no OS.
 *
 * Kept separate from sntp.c (which owns the sockets) for two reasons:
 *   1. the 48-byte layout and the validation rules are the parts most likely
 *      to be wrong and most important to be right;
 *   2. it lets the unit tests feed captured server replies straight into the
 *      real shipping code on Linux, with no network at all.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_SNTP_PROTO_H
#define TIMEKEEPER_SNTP_PROTO_H

#include "timeutil.h"

#define TK_NTP_PACKET_SIZE 48

/* Offsets per RFC 4330 figure 4. */
#define TK_NTP_OFF_FLAGS   0
#define TK_NTP_OFF_STRAT  1
#define TK_NTP_OFF_POLL    2
#define TK_NTP_OFF_PREC    3
#define TK_NTP_OFF_TX      40
#define TK_NTP_OFF_REFID   12

/* Flags byte: LI(2) VN(3) Mode(3). 0x23 = not-leap-warning, v4, client. */
#define TK_NTP_CLIENT_FLAGS 0x23

typedef enum {
    TKNS_OK            = 0,
    TKNS_TOO_SHORT     = 1,   /* < 48 bytes                            */
    TKNS_BAD_VERSION   = 2,   /* not v3/v4                            */
    TKNS_NOT_SERVER    = 3,   /* mode not 4 (server) or 5 (broadcast) */
    TKNS_KOD           = 4,   /* Kiss-o'-Death: stratum 0             */
    TKNS_BAD_STRAT     = 5,   /* stratum > 15                         */
    TKNS_ZERO_TS       = 6,   /* transmit timestamp all zero          */
    TKNS_LOOPED        = 7,   /* our own request echoed back verbatim */
    TKNS_OUT_OF_RANGE  = 8,   /* decoded time outside the accepted window */
    TKNS_BAD_ORIGINATE = 9    /* server did not echo our Transmit Timestamp */
} tk_ntp_status_t;

/* Build a client request. tx_sec is the current Unix time (used only as an
 * echo-detection token and to help servers that need a nonzero originate). */
void tk_ntp_build_request(uint8_t buf[TK_NTP_PACKET_SIZE], time64_t tx_unix);

/* Validate a reply and extract its transmit timestamp.
 *   out_sec : Unix seconds (era-corrected), TK_EPOCH_INVALID on failure
 *   out_ms  : sub-second part, for nearest-second rounding
 *   req     : the request we sent, used to reject a looped-back copy of it
 * year_min/year_max bound the accepted result (config.h policy values). */
int tk_ntp_parse_reply(const uint8_t *pkt, size_t len,
                       const uint8_t req[TK_NTP_PACKET_SIZE],
                       int year_min, int year_max,
                       time64_t *out_sec, uint32_t *out_ms);

/* Second, narrower guard: a *local* time source (loopback or link-local) whose
 * answer agrees with the clock we already have within tol_sec tells us nothing,
 * and treating it as a sync would let a self-referential loop look like success.
 * Only applied when the destination really was local - see sntp.c. */
int tk_ntp_is_looped(time64_t replied, time64_t sent, int tol_sec);

const char *tk_ntp_status_name(int st);

#endif /* TIMEKEEPER_SNTP_PROTO_H */

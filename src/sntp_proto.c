/* sntp_proto.c — RFC 4330 packet build/validate. Pure, testable, no OS.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sntp_proto.h"

#include "tklibc.h"

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

void tk_ntp_build_request(uint8_t buf[TK_NTP_PACKET_SIZE], time64_t tx_unix)
{
    uint64_t ntp;

    tk_memset(buf, 0, TK_NTP_PACKET_SIZE);
    buf[TK_NTP_OFF_FLAGS] = TK_NTP_CLIENT_FLAGS;     /* LI=0 VN=4 Mode=3 */

    /* Transmit Timestamp = seconds since 1900. tx_unix is already windowed by
     * the caller's config bounds, so the cast cannot go negative in practice;
     * guard anyway so a bad caller yields zeros rather than UB. */
    ntp = (tx_unix > 0) ? (uint64_t)((int64_t)tx_unix + TK_UNIX_TO_NTP_SEC) : 0u;
    put_be32(buf + TK_NTP_OFF_TX, (uint32_t)(ntp & 0xFFFFFFFFu));
    put_be32(buf + TK_NTP_OFF_TX + 4, 0u);            /* fraction = 0 */
}

int tk_ntp_parse_reply(const uint8_t *pkt, size_t len,
                       const uint8_t req[TK_NTP_PACKET_SIZE],
                       int year_min, int year_max,
                       time64_t *out_sec, uint32_t *out_ms)
{
    uint8_t  flags, strat;
    int      li, vn, mode;
    uint64_t secs;
    int      st = TKNS_OK;
    time64_t e = TK_EPOCH_INVALID;
    uint32_t ms = 0;

    if (out_sec) *out_sec = TK_EPOCH_INVALID;
    if (out_ms)  *out_ms  = 0;
    if (!pkt) return (int)TKNS_TOO_SHORT;

    if (len < TK_NTP_PACKET_SIZE) return (int)TKNS_TOO_SHORT;

    flags = pkt[TK_NTP_OFF_FLAGS];
    li    = (flags >> 6) & 3;
    vn    = (flags >> 3) & 7;
    mode  =  flags       & 7;

    if (vn != 3 && vn != 4)                return (int)TKNS_BAD_VERSION;
    if (mode != 4 && mode != 5)            return (int)TKNS_NOT_SERVER;

    strat = pkt[TK_NTP_OFF_STRAT];
    if (strat == 0) {
        /* A Kiss-o'-Death packet is a server telling us to go away
         * (rate limiting / access denied). Never a time source. */
        return (int)TKNS_KOD;
    }
    if (strat > 15)                        return (int)TKNS_BAD_STRAT;
    if (li == 3)                           return (int)TKNS_KOD;   /* unsynchronised */

    /* The Kiss Code lives in the Reference Identifier field when stratum==0;
     * already handled above. Reject an all-zero transmit timestamp. */
    secs = be32(pkt + TK_NTP_OFF_TX);
    if (secs == 0)                          return (int)TKNS_ZERO_TS;

    {
        uint32_t frac = be32(pkt + TK_NTP_OFF_TX + 4);
        if (!tk_ntp_fields_to_epoch((uint32_t)secs, frac, &e, &ms))
            return (int)TKNS_ZERO_TS;
    }

    if (req && len >= TK_NTP_PACKET_SIZE) {
        /* (a) Originate echo. A server answering our request must copy our
         * Transmit Timestamp into its Ownage/Originate field at offset 24.
         * Comparing the Transmit field instead would misjudge a legitimate
         * server that happens to read the same whole second as we do (with a
         * zero fraction the 8 bytes are identical), which is exactly what was
         * observed here against time.google.com behind NAT. */
        if (tk_memcmp(pkt + 24, req + TK_NTP_OFF_TX, 8) != 0)
            return (int)TKNS_BAD_ORIGINATE;
        /* (b) verbatim reflection (belt and braces; mode validation above
         * already catches the common case). */
        if (tk_memcmp(pkt, req, TK_NTP_PACKET_SIZE) == 0)
            return (int)TKNS_LOOPED;
    }

    /* Year window (policy values from config.h) + FILETIME expressibility. */
    {
        int      y = 0, ok;
        unsigned mo, d, hh, mm, ss;
        ok = tk_civil_from_epoch_utc(e, &y, &mo, &d, &hh, &mm, &ss);
        if (!ok || y < year_min || y > year_max) st = TKNS_OUT_OF_RANGE;
    }
    if (st != TKNS_OK) return st;

    if (out_sec) *out_sec = e;
    if (out_ms)  *out_ms  = ms;
    return (int)TKNS_OK;
}

int tk_ntp_is_looped(time64_t replied, time64_t sent, int tol_sec)
{
    int64_t d;
    if (replied == TK_EPOCH_INVALID || sent == TK_EPOCH_INVALID) return 0;
    d = replied - sent;
    if (d < 0) d = -d;
    return d <= (int64_t)tol_sec;
}

const char *tk_ntp_status_name(int st)
{
    switch (st) {
    case TKNS_OK:            return "OK";
    case TKNS_TOO_SHORT:     return "SHORT-PACKET";
    case TKNS_BAD_VERSION:   return "BAD-VERSION";
    case TKNS_NOT_SERVER:    return "NOT-SERVER-MODE";
    case TKNS_KOD:           return "KISS-OF-DEATH";
    case TKNS_BAD_STRAT:     return "BAD-STRATUM";
    case TKNS_ZERO_TS:       return "ZERO-TIMESTAMP";
    case TKNS_LOOPED:        return "LOOPED-REQUEST";
    case TKNS_OUT_OF_RANGE:  return "OUT-OF-RANGE";
    case TKNS_BAD_ORIGINATE: return "NO-ORIGINATE-ECHO";
    default:                 return "UNKNOWN";
    }
}

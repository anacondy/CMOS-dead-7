/* selftest.c — the test suite itself, compiled twice:
 *
 *   1. into the host test binary (tests/host_test.c) together with the pure
 *      modules, where failures print to stdout and libc cross-checks are added;
 *   2. into TimeKeeper.exe, where it backs the `/test` CLI flag so an operator
 *      can validate a suspect machine with no network and no toolchain.
 *
 * One source, two consumers: what the build proves is what the binary proves.
 * No printf, no floats, no OS headers — reporting goes through a callback, so
 * the same code runs in a WinMain GUI binary and in a console test program.
 *
 * SPDX-License-Identifier: MIT
 */
#include "selftest.h"
#include "config.h"

#include "tklibc.h"

#include "timeutil.h"
#include "verdict.h"
#include "sntp_proto.h"
#include "fallback_proto.h"
#include "log_rotate.h"
#include "tests/vectors.h"

struct tk_report {
    int checks, fails;
    tk_report_fn cb;
    void *ud;
};

#ifndef TK_SELFTEST_COMPACT
static void rep(struct tk_report *r, const char *vec_why, const char *what, int got, int want)
{
    char msg[160];
    size_t n = 0;
    const char *p;
    (void)vec_why;
    p = what;
    while (*p && n + 1 < sizeof(msg)) msg[n++] = *p++;
    p = " got=";
    while (*p && n + 1 < sizeof(msg)) msg[n++] = *p++;
    if (got < 0) { msg[n++] = '-'; got = -got; }
    n += tk_fmt_u32(msg + n, sizeof(msg) - n, (uint32_t)got, 1);
    p = " want=";
    while (*p && n + 1 < sizeof(msg)) msg[n++] = *p++;
    if (want < 0) { msg[n++] = '-'; want = -want; }
    n += tk_fmt_u32(msg + n, sizeof(msg) - n, (uint32_t)want, 1);
    msg[n] = 0;
    r->checks++;
    if (r->cb) r->cb(r->ud, msg);
}
#endif /* !TK_SELFTEST_COMPACT */

#ifdef TK_SELFTEST_COMPACT
/* Product build: an assertion costs a compare and a line number, and nothing
 * else. The verbose form below is what the host test suite uses. Reporting the
 * source line keeps a field failure actionable — grep tests/ or src/ for it. */
static void rep_min(struct tk_report *r, int line)
{
    char msg[24];
    size_t n = 0;
    msg[n++] = 'F'; msg[n++] = 'A'; msg[n++] = 'I'; msg[n++] = 'L'; msg[n++] = '@';
    n += tk_fmt_u32(msg + n, sizeof(msg) - n, (uint32_t)(line < 0 ? 0 : line), 1);
    msg[n] = 0;
    if (r->cb) r->cb(r->ud, msg);
}
#define EXPECT(cond, why, what, got, want) do {                     \
    r->checks++;                                                     \
    if (!(cond)) { r->fails++; rep_min(r, __LINE__); }              \
} while (0)
#else
#define EXPECT(cond, why, what, got, want) do {                     \
    r->checks++;                                                     \
    if (!(cond)) { r->fails++; rep(r, (why), (what), (int)(got), (int)(want)); } \
} while (0)
#endif

/* --------------------------------------------------------------- calendar */

static void t_calendar(struct tk_report *r)
{
    struct { int y; unsigned m, d; int ok; } v[] = {
        {2000, 2, 29, 1}, {1900, 2, 29, 0}, {2100, 2, 29, 0}, {2400, 2, 29, 1},
        {2026, 2, 29, 0}, {2024, 2, 29, 1}, {2026, 4, 31, 0}, {2026, 6, 31, 0},
        {2026, 12, 31, 1}, {1970, 1, 1, 1}, {1, 1, 1, 1}, {9999, 12, 31, 1},
        {0, 1, 1, 0}, {-5, 1, 1, 0}, {2026, 13, 1, 0}, {2026, 0, 1, 0},
        {2026, 1, 0, 0}, {2026, 1, 32, 0},
    };
    size_t i;
    for (i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        int ok = 0;
        (void)tk_days_from_civil(v[i].y, v[i].m, v[i].d, &ok);
        EXPECT(ok == v[i].ok, "calendar", "validity", ok, v[i].ok);
    }
    /* A handful of instants pinned by hand: the 1970 epoch, the 2000 rollover,
     * and the 2038 boundary that breaks 32-bit time everywhere else. */
    {
        static const struct { const char *d; time64_t e; } fixed[] = {
            {"1970-01-01", 0},
            {"2000-01-01", 946684800LL},
            {"2026-09-01", 1788220800LL},
            {"2038-01-19", 2147472000LL},
            {"2038-01-18", 2147385600LL},
        };
        for (i = 0; i < sizeof(fixed)/sizeof(fixed[0]); i++) {
            int y = 0, ok = 0;
            unsigned mo = 0, d = 0;
            time64_t e;
            (void)tk_parse_date_text(fixed[i].d, tk_strlen(fixed[i].d), &y, &mo, &d, 0, 0, 0);
            e = tk_epoch_from_civil_utc(y, mo, d, 0, 0, 0, &ok);
            EXPECT(ok == 1, "calendar", "epoch computed", ok, 1);
            EXPECT(e == fixed[i].e, "calendar", "epoch value", (int)(e % 100000),
                   (int)(fixed[i].e % 100000));
            /* full 64-bit compare, since the modulo above can alias */
            r->checks++;
            if (e != fixed[i].e) { r->fails++; EXPECT(e == fixed[i].e, "calendar", "epoch high bits", 0, 1); }
        }
    }
    /* round trip over a dense window: catches off-by-one at month/year edges */
    {
        int y;
        for (y = 2020; y <= 2041; y++) {
            unsigned m;
            for (m = 1; m <= 12; m++) {
                unsigned dim = tk_days_in_month(y, m), d;
                for (d = 1; d <= dim; d++) {
                    int ok = 0, yy = 0;
                    unsigned mm = 0, dd = 0;
                    time64_t e = tk_epoch_from_civil_utc(y, m, d, 12, 34, 56, &ok);
                    int y2, mo2, d2;
                    if (!ok) { EXPECT(0, "roundtrip", "valid date rejected", 0, 1); continue; }
                    y2 = mo2 = d2 = 0;
                    {
                        unsigned um = 0, ud = 0, uh = 0, umi = 0, us = 0;
                        if (!tk_civil_from_epoch_utc(e, &y2, &um, &ud, &uh, &umi, &us)) {
                            EXPECT(0, "roundtrip", "reverse failed", 0, 1);
                            continue;
                        }
                        EXPECT(y2 == y && um == m && ud == d && uh == 12 && umi == 34 && us == 56,
                               "roundtrip", "mismatch", (y2 * 10000 + (int)um * 100 + (int)ud),
                               (y * 10000 + (int)m * 100 + (int)d));
                    }
                    (void)yy; (void)mm; (void)dd;
                }
            }
        }
    }
    /* weekday, checked against dates whose day of week is externally known */
    {
        static const struct { const char *d; int dow; } wd[] = {
            {"1970-01-01", 4}, {"2026-09-01", 2}, {"2024-12-25", 3},
            {"2000-01-01", 6}, {"1900-01-01", 1}, {"2038-01-19", 2},
        };
        for (i = 0; i < sizeof(wd)/sizeof(wd[0]); i++) {
            int y = 0; unsigned mo = 0, d = 0;
            (void)tk_parse_date_text(wd[i].d, tk_strlen(wd[i].d), &y, &mo, &d, 0, 0, 0);
            EXPECT(tk_day_of_week_from_days(tk_days_from_civil(y, mo, d, 0)) == wd[i].dow,
                   "weekday", wd[i].d, 0, wd[i].dow);
        }
    }
}

/* -------------------------------------------------------------- FILETIME */

static void t_filetime(struct tk_report *r)
{
    uint32_t lo = 0, hi = 0;
    /* 1970-01-01T00:00:00Z as a FILETIME, from the documented constant. */
    EXPECT(tk_filetime_from_epoch_ms(0, 0, &lo, &hi), "filetime", "1970 convertible", 0, 1);
    EXPECT(hi == 0x019DB1DEu, "filetime", "high dword", (int)hi, (int)0x019DB1DEu);
    EXPECT(lo == 0xD53E8000u, "filetime", "low dword", (int)(int32_t)lo, (int)(int32_t)0xD53E8000u);
    EXPECT(!tk_filetime_from_epoch_ms(TK_EPOCH_FT_MIN - 1, 0, &lo, &hi),
           "filetime", "below minimum rejected", 1, 0);
    EXPECT(!tk_filetime_from_epoch_ms(TK_EPOCH_FT_MAX + 1, 0, &lo, &hi),
           "filetime", "above maximum rejected", 1, 0);
    EXPECT(!tk_filetime_from_epoch_ms(TK_SEP1_2026, 1000, &lo, &hi),
           "filetime", "ms=1000 rejected", 1, 0);
    /* The SYSTEMTIME cap must map to a *valid* FILETIME (not the max one: the
     * max FILETIME is a further 11 hours beyond what a SYSTEMTIME can hold). */
    EXPECT(tk_filetime_from_epoch_ms(TK_EPOCH_FT_MAX, 0, &lo, &hi),
           "filetime", "SYSTEMTIME cap convertible", 0, 1);
    EXPECT((hi & 0x80000000u) == 0u, "filetime", "stays inside signed INT64", (int)hi, 0);
    {
        time64_t e = 0; uint32_t ms = 0;
        EXPECT(tk_filetime_from_epoch_ms(TK_SEP1_2026, 123, &lo, &hi), "filetime", "with ms", 0, 1);
        EXPECT(tk_epoch_from_filetime(lo, hi, &e, &ms), "filetime", "reverse", 0, 1);
        EXPECT(e == TK_SEP1_2026 && ms == 123, "filetime", "roundtrip ms", (int)ms, 123);
    }
}

/* ---------------------------------------------------------------- NTP */

static void t_ntp(struct tk_report *r)
{
    size_t i;
    for (i = 0; i < sizeof(tk_ntp_vectors)/sizeof(tk_ntp_vectors[0]); i++) {
        const tk_ntp_vec_t *v = &tk_ntp_vectors[i];
        time64_t e = -1; uint32_t ms = 0xFFFFFFFFu;
        int got = tk_ntp_fields_to_epoch(v->sec, v->frac, &e, &ms);
        EXPECT(got == v->accept, v->why, "ntp accept", got, v->accept);
        if (got && v->accept) {
            EXPECT(e == v->expect_epoch, v->why, "ntp epoch (low bits)", (int)(e & 0xFFFFF),
                   (int)(v->expect_epoch & 0xFFFFF));
            EXPECT(e == v->expect_epoch, v->why, "ntp epoch high bits", 0, 1);
            EXPECT(ms == v->expect_ms, v->why, "ntp ms", (int)ms, (int)v->expect_ms);
        }
    }
    /* rounding to the nearest second is applied when building the FILETIME:
     * a half-second-or-more fraction must carry into the next second. */
    {
        time64_t e = 0; uint32_t ms = 0, l2, h2;
        EXPECT(tk_ntp_fields_to_epoch((uint32_t)TK_NTP_20260901, 0x80000000u, &e, &ms),
               "rounding", "parse", 0, 1);
        EXPECT(tk_filetime_from_epoch_ms(e + (ms >= 500u ? 1u : 0u), 0, &l2, &h2),
               "rounding", "apply round-up", 0, 1);
        {
            time64_t back = 0; uint32_t bms = 0;
            EXPECT(tk_epoch_from_filetime(l2, h2, &back, &bms) &&
                   back == TK_SEP1_2026 + 1, "rounding", "rounded instant",
                   (int)(back - TK_SEP1_2026), 1);
        }
    }
}

static void t_sntp_packet(struct tk_report *r)
{
    uint8_t req[TK_NTP_PACKET_SIZE], rep_[TK_NTP_PACKET_SIZE];
    size_t i;
    time64_t e = 0; uint32_t ms = 0;

    tk_ntp_build_request(req, TK_SEP1_2026);
    EXPECT(req[TK_NTP_OFF_FLAGS] == TK_NTP_CLIENT_FLAGS, "packet", "flags byte",
           (int)req[TK_NTP_OFF_FLAGS], (int)TK_NTP_CLIENT_FLAGS);
    EXPECT(((req[0] >> 3) & 7u) == 4u, "packet", "version 4", (int)((req[0]>>3)&7), 4);
    EXPECT((req[0] & 7u) == 3u, "packet", "mode 3 client", (int)(req[0]&7), 3);
    EXPECT(((req[0] >> 6) & 3u) == 0u, "packet", "leap 0", (int)((req[0]>>6)&3), 0);
    for (i = 1; i < TK_NTP_PACKET_SIZE; i++) {
        if (i >= 40 && i < 44) continue;
        EXPECT(req[i] == 0, "packet", "zero padding", (int)req[i], 0);
    }
    {
        uint32_t tx = ((uint32_t)req[40] << 24) | ((uint32_t)req[41] << 16) |
                      ((uint32_t)req[42] << 8) | (uint32_t)req[43];
        EXPECT(tx == (uint32_t)(TK_SEP1_2026 + TK_UNIX_TO_NTP_SEC), "packet",
               "transmit timestamp", (int)(tx & 0xFFFFF),
               (int)(((uint32_t)(TK_SEP1_2026 + TK_UNIX_TO_NTP_SEC)) & 0xFFFFF));
    }

    tk_memset(rep_, 0, sizeof(rep_));
    rep_[0] = (uint8_t)((0 << 6) | (4 << 3) | 4);
    rep_[1] = 2;
    tk_memcpy(rep_ + 24, req + 40, 8);          /* RFC 4330: echo our Tx back */
    {
        uint32_t sec = (uint32_t)(TK_SEP1_2026 + TK_UNIX_TO_NTP_SEC) + 7u;
        rep_[40] = (uint8_t)(sec >> 24); rep_[41] = (uint8_t)(sec >> 16);
        rep_[42] = (uint8_t)(sec >> 8);  rep_[43] = (uint8_t)sec;
        rep_[44] = 0x40;
    }
    EXPECT(tk_ntp_parse_reply(rep_, sizeof(rep_), req, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR,
                              &e, &ms) == TKNS_OK, "reply", "valid server reply", 0, TKNS_OK);
    EXPECT(e == TK_SEP1_2026 + 7, "reply", "reply time", (int)(e - TK_SEP1_2026), 7);
    EXPECT(tk_ntp_parse_reply(rep_, 47, req, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR, 0, 0)
           == TKNS_TOO_SHORT, "reply", "short", 0, TKNS_TOO_SHORT);
    rep_[0] = (uint8_t)((0 << 6) | (4 << 3) | 3);
    EXPECT(tk_ntp_parse_reply(rep_, 48, req, 1, 9999, 0, 0) == TKNS_NOT_SERVER,
           "reply", "mode 3 echo", 0, TKNS_NOT_SERVER);
    rep_[0] = (uint8_t)((0 << 6) | (2 << 3) | 4);
    EXPECT(tk_ntp_parse_reply(rep_, 48, req, 1, 9999, 0, 0) == TKNS_BAD_VERSION,
           "reply", "version 2", 0, TKNS_BAD_VERSION);
    rep_[0] = (uint8_t)((0 << 6) | (4 << 3) | 4);
    rep_[1] = 0;
    EXPECT(tk_ntp_parse_reply(rep_, 48, req, 1, 9999, 0, 0) == TKNS_KOD,
           "reply", "stratum 0 = KoD", 0, TKNS_KOD);
    rep_[1] = 16;
    EXPECT(tk_ntp_parse_reply(rep_, 48, req, 1, 9999, 0, 0) == TKNS_BAD_STRAT,
           "reply", "stratum 16", 0, TKNS_BAD_STRAT);
    rep_[1] = 1;
    rep_[0] = (uint8_t)((3 << 6) | (4 << 3) | 4);
    EXPECT(tk_ntp_parse_reply(rep_, 48, req, 1, 9999, 0, 0) == TKNS_KOD,
           "reply", "LI=3 unsynchronised", 0, TKNS_KOD);
    rep_[0] = (uint8_t)((0 << 6) | (4 << 3) | 4);
    tk_memset(rep_ + 40, 0, 8);
    EXPECT(tk_ntp_parse_reply(rep_, 48, req, 1, 9999, 0, 0) == TKNS_ZERO_TS,
           "reply", "zero transmit ts", 0, TKNS_ZERO_TS);
    {   /* year floor: 0xFFFFFFFF is 2036 and therefore *inside* policy, so
         * drive the rejection with a pre-2005 value instead. */
        uint32_t far = TK_NTP_ERA1_PIVOT;   /* 2000-01-01, below TK_MIN_EPOCH_YEAR */
        rep_[40] = (uint8_t)(far >> 24); rep_[41] = (uint8_t)(far >> 16);
        rep_[42] = (uint8_t)(far >> 8);  rep_[43] = (uint8_t)far;
        EXPECT(tk_ntp_parse_reply(rep_, 48, req, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR, 0, 0)
               == TKNS_OUT_OF_RANGE, "reply", "year beyond ceiling", 0, TKNS_OUT_OF_RANGE);
    }
    /* Copying only the transmit field is a legitimate answer from a server that
     * agrees with our clock; copying the WHOLE packet is a reflector. */
    /* A server that agrees with our clock down to the whole second is legal. */
    tk_memcpy(rep_ + 40, req + 40, 8);
    tk_memcpy(rep_ + 24, req + 40, 8);            /* proper Originate echo */
    rep_[0] = (uint8_t)((0 << 6) | (4 << 3) | 4);
    rep_[1] = 1;
    EXPECT(tk_ntp_parse_reply(rep_, 48, req, 1, 9999, 0, 0) == TKNS_OK,
           "reply", "server agreeing with our clock must not be called a loop", 0, 1);
    /* No Originate echo means the reply is not an answer to our request. */
    tk_memset(rep_ + 24, 0, 8);
    EXPECT(tk_ntp_parse_reply(rep_, 48, req, 1, 9999, 0, 0) == TKNS_BAD_ORIGINATE,
           "reply", "missing originate echo rejected", 0, TKNS_BAD_ORIGINATE);
    /* A verbatim copy of our request is a reflector. */
    tk_memcpy(rep_, req, TK_NTP_PACKET_SIZE);
    EXPECT(tk_ntp_parse_reply(rep_, 48, req, 1, 9999, 0, 0) != TKNS_OK,
           "reply", "verbatim echo rejected", 0, 1);
    EXPECT(!tk_ntp_is_looped(TK_SEP1_2026 + 60, TK_SEP1_2026, 2),
           "loopguard", "60 s apart is not a loop", 1, 0);
}

/* ------------------------------------------------------------- .dat parser */

static void t_dat(struct tk_report *r)
{
    size_t i;
    for (i = 0; i < sizeof(tk_dat_vectors)/sizeof(tk_dat_vectors[0]); i++) {
        const tk_dat_vec_t *v = &tk_dat_vectors[i];
        time64_t e = 0; int y = 0, ok = 0;
        unsigned mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
        /* tk_strlen() stops at the first NUL, which is exactly the byte a real
         * file may contain: vectors with an embedded NUL carry their length
         * via a sizeof-derived constant in the table itself. */
        size_t len = v->len ? (size_t)v->len : tk_strlen(v->text);
        int got = tk_dat_parse(v->text, len,
                               TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR, &e, &y, &mo, &d);
        EXPECT(got == v->accept, v->why, "dat accept", got, v->accept);
        if (got && v->accept) {
            time64_t want = tk_epoch_from_civil_utc(v->y, v->mo, v->d, v->hh, v->mi, v->ss, &ok);
            EXPECT(ok == 1, v->why, "vector date valid", ok, 1);
            EXPECT(e == want, v->why, "dat epoch", 0, 1);
            EXPECT((int)mo == (int)v->mo && (int)d == (int)v->d && y == v->y,
                   v->why, "dat ymd", y * 10000 + (int)mo * 100 + (int)d,
                   v->y * 10000 + (int)v->mo * 100 + (int)v->d);
            if (v->hh || v->mi || v->ss) {
                (void)tk_parse_date_text(v->text, len, 0, 0, 0, &hh, &mi, &ss);
                EXPECT((int)hh == (int)v->hh && (int)mi == (int)v->mi && (int)ss == (int)v->ss,
                       v->why, "dat time", ((int)hh * 10000 + (int)mi * 100 + (int)ss),
                       ((int)v->hh * 10000 + (int)v->mi * 100 + (int)v->ss));
            }
        }
    }
    /* every prefix of a valid file must be rejected, never accepted as a date */
    {
        const char *full = "2026-09-01";
        size_t n;
        for (n = 0; n < tk_strlen(full); n++) {
            time64_t e;
            EXPECT(!tk_dat_parse(full, n, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR, &e, 0, 0, 0),
                   "truncation", "prefix must not parse", 1, 0);
        }
    }
    /* render -> parse round trip across five years of months */
    {
        int y;
        for (y = 2021; y <= 2030; y++) {
            unsigned m;
            for (m = 1; m <= 12; m++) {
                char buf[128];
                size_t n = tk_dat_render(buf, sizeof(buf), y, m);
                time64_t e = 0, want;
                int ok = 0;
                EXPECT(n > 0, "render", "render produced bytes", (int)n, 1);
                want = tk_epoch_from_civil_utc(y, m, 1, 0, 0, 0, &ok);
                EXPECT(ok == 1 && tk_dat_parse(buf, n, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR, &e, 0,0,0)
                       && e == want, "render", "round trip", (int)(e & 0xFF), (int)(want & 0xFF));
            }
        }
    }
}

/* ---------------------------------------------------------- HTTP Date */

static void t_http(struct tk_report *r)
{
    size_t i;
    for (i = 0; i < sizeof(tk_http_vectors)/sizeof(tk_http_vectors[0]); i++) {
        const tk_http_vec_t *v = &tk_http_vectors[i];
        time64_t e = 0; int ok = 0;
        int got = tk_parse_http_date(v->date, tk_strlen(v->date), &e);
        EXPECT(got == v->accept, v->why, "http accept", got, v->accept);
        if (got && v->accept) {
            time64_t want = tk_epoch_from_civil_utc(v->y, v->mo, v->d, v->hh, v->mi, v->ss, &ok);
            EXPECT(ok == 1, v->why, "vector date valid", ok, 1);
            EXPECT(e == want, v->why, "http epoch", 0, 1);
        }
    }
    for (i = 0; i < sizeof(tk_resp_vectors)/sizeof(tk_resp_vectors[0]); i++) {
        const tk_resp_vec_t *v = &tk_resp_vectors[i];
        time64_t e = 0;
        int got = tk_http_extract_date(v->resp, tk_strlen(v->resp), &e);
        EXPECT(got == v->accept, v->why, "extractor accept", got, v->accept);
    }
    /* fuzz: every truncation of a real response must either fail or give the
     * one true instant — never a plausible-looking wrong time. */
    {
        const char *h = "HTTP/1.1 200 OK\r\nDate: Tue, 01 Sep 2026 05:30:00 GMT\r\n\r\n";
        size_t n, total = tk_strlen(h);
        int accepted = 0;
        for (n = 1; n <= total; n++) {
            time64_t e = 0;
            if (tk_http_extract_date(h, n, &e)) {
                accepted++;
                EXPECT(e == 1788240600LL, "truncation fuzz", "wrong value from truncated input",
                       (int)(e % 100000), (int)(1788240600LL % 100000));
            }
        }
        EXPECT(accepted > 0, "truncation fuzz", "some truncations must parse", accepted, 1);
    }
    /* and: an empty/oversized buffer must not read out of bounds — checked by
     * feeding a 1-byte buffer, which the length argument must respect. */
    {
        time64_t e = 0;
        EXPECT(!tk_http_extract_date("D", 1, &e), "short input", "1 byte", 1, 0);
        EXPECT(!tk_parse_http_date("D", 1, &e), "short input", "1 byte", 1, 0);
        EXPECT(!tk_dat_parse("2", 1, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR, &e, 0, 0, 0),
               "short input", "1 byte", 1, 0);
    }
}

/* -------------------------------------------------------------- policy */

static void t_policy(struct tk_report *r)
{
    size_t i;
    for (i = 0; i < sizeof(tk_verdict_vectors)/sizeof(tk_verdict_vectors[0]); i++) {
        const tk_verdict_vec_t *v = &tk_verdict_vectors[i];
        int got = tk_verdict_skip_or_sync(v->now, v->ref, v->ref_fresh,
                                         TK_SKIP_MATCH_MS, v->force, v->date_only);
        EXPECT(got == v->expect, v->why, "verdict", got, v->expect);
    }
    for (i = 0; i < sizeof(tk_fallback_vectors)/sizeof(tk_fallback_vectors[0]); i++) {
        const tk_fb_vec_t *v = &tk_fallback_vectors[i];
        int act = -1;
        int got = tk_fallback_apply(v->now, v->ref, v->fb, TK_FALLBACK_MAX_AGE_DAYS,
                                    v->allow_back, &act);
        EXPECT(got == v->expect && act == v->expect, v->why, "fallback action", act, v->expect);
    }
    for (i = 0; i < sizeof(tk_datupd_vectors)/sizeof(tk_datupd_vectors[0]); i++) {
        const tk_datupd_vec_t *v = &tk_datupd_vectors[i];
        int dec = -1;
        int w = tk_dat_should_write(v->trusted, v->synced, v->stored, v->embedded, &dec);
        EXPECT(w == v->expect_write, v->why, "dat write", w, v->expect_write);
        EXPECT(dec == v->expect_dec, v->why, "dat decision", dec, v->expect_dec);
    }
    /* the invariant, restated without a table: a correct clock is never moved
     * backwards by an offline fallback, under any combination of inputs */
    {
        int act = -1;
        tk_fallback_apply(TK_SEP1_2026 + 86400 * 200, TK_SEP1_2026, TK_SEP1_2026,
                          TK_FALLBACK_MAX_AGE_DAYS, 0, &act);
        EXPECT(act != TKF_APPLY, "invariant", "never regress a good clock", act, TKF_SKIP_OK);
        /* ...and the reference choice is monotone-forward */
        EXPECT(tk_choose_reference(100, 300, 200) == 300, "ref", "max of candidates", 0, 300);
        EXPECT(tk_choose_reference(TK_EPOCH_INVALID, 300, TK_EPOCH_INVALID) == 300,
               "ref", "skips invalid", 0, 300);
        EXPECT(tk_choose_reference(TK_EPOCH_INVALID, TK_EPOCH_INVALID, TK_EPOCH_INVALID)
               == TK_EPOCH_INVALID, "ref", "all invalid", 0, -1);
        EXPECT(tk_ref_is_fresh(TK_SEP1_2026, TK_SEP1_2026, TK_REF_FRESH_DAYS) == 1,
               "fresh", "equal instants", 0, 1);
        EXPECT(tk_ref_is_fresh(TK_SEP1_2026 - 1, TK_SEP1_2026, TK_REF_FRESH_DAYS) == 0,
               "fresh", "behind reference", 1, 0);
        EXPECT(tk_ref_is_fresh(TK_SEP1_2026 + TK_REF_FRESH_DAYS * 86400, TK_SEP1_2026,
                               TK_REF_FRESH_DAYS) == 1, "fresh", "exactly at the cap", 0, 1);
        EXPECT(tk_ref_is_fresh(TK_SEP1_2026 + (TK_REF_FRESH_DAYS + 1) * 86400, TK_SEP1_2026,
                               TK_REF_FRESH_DAYS) == 0, "fresh", "one day past the cap", 1, 0);
        /* black-hole detection */
        EXPECT(tk_udp_blackholed(2, 0, 1700, TK_NET_DEADLINE_MS, 2) == 1, "blackhole",
               "2 timeouts past half budget", 0, 1);
        EXPECT(tk_udp_blackholed(2, 1, 1700, TK_NET_DEADLINE_MS, 2) == 0, "blackhole",
               "a reply means keep trying", 1, 0);
        EXPECT(tk_udp_blackholed(2, 0, 500, TK_NET_DEADLINE_MS, 2) == 0, "blackhole",
               "too early to give up", 1, 0);
    }
    /* candidate window */
    EXPECT(tk_candidate_ok(TK_SEP1_2026, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR) == 1,
           "window", "inside", 0, 1);
    EXPECT(tk_candidate_ok(tk_build_epoch(1970, 1, 1), TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR) == 0,
           "window", "below floor", 1, 0);
    EXPECT(tk_candidate_ok(TK_EPOCH_INVALID, TK_MIN_EPOCH_YEAR, TK_MAX_EPOCH_YEAR) == 0,
           "window", "invalid", 1, 0);
    EXPECT(tk_build_epoch(TK_BUILD_YEAR, TK_BUILD_MONTH, TK_BUILD_DAY) > 1787000000LL,
           "window", "build epoch sane", 0, 1);
}

/* ----------------------------------------------------------- formatters */

static void t_format(struct tk_report *r)
{
    char b[64];
    (void)tk_fmt_date(b, sizeof(b), 2026, 9, 1);
    EXPECT(!tk_strcmp(b, "2026-09-01"), "fmt", "date", b[0], '2');
    (void)tk_fmt_time(b, sizeof(b), 5, 30, 0);
    EXPECT(!tk_strcmp(b, "05:30:00"), "fmt", "time", b[0], '0');
    (void)tk_fmt_epoch_utc(b, sizeof(b), TK_SEP1_2026);
    EXPECT(!tk_strcmp(b, "2026-09-01 00:00:00"), "fmt", "epoch", b[10], ' ');
    (void)tk_fmt_u32(b, sizeof(b), 7, 3);
    EXPECT(!tk_strcmp(b, "007"), "fmt", "padding", b[0], '0');
    /* Buffer discipline: nothing written at or beyond cap, and the string is
     * always NUL-terminated inside it. A canary byte right past the cap proves
     * the first half; strlen proves the second. This is what keeps the log
     * writer safe without any bounds-checking library. */
    {
        size_t cap;
        int    kind;
        for (cap = 0; cap < 24; cap++) {
            for (kind = 0; kind < 3; kind++) {
                char  small[40];
                size_t i2, used;
                for (i2 = 0; i2 < sizeof(small); i2++) small[i2] = 'A';
                small[sizeof(small) - 1] = '\0';
                if (kind == 0)      used = tk_fmt_date(small, cap, 2026, 9, 1);
                else if (kind == 1) used = tk_fmt_time(small, cap, 1, 2, 3);
                else                used = tk_fmt_epoch_utc(small, cap, TK_SEP1_2026);
                if (cap < sizeof(small) - 1)
                    EXPECT(small[cap] == 'A' || small[cap] == '\0', "fmt bounds",
                           "write past cap", 1, 0);
                if (cap) EXPECT(tk_strlen(small) + 1 <= cap, "fmt bounds",
                                 "not terminated within cap", (int)tk_strlen(small), (int)cap);
                EXPECT(used <= cap, "fmt bounds", "returned length > cap", (int)used, (int)cap);
            }
        }
    }
    /* the status/decision names used in log lines must never return NULL */
    EXPECT(tk_verdict_name(TKV_SYNC) != 0, "names", "verdict", 0, 1);
    EXPECT(tk_verdict_name(999) != 0, "names", "verdict unknown", 0, 1);
    EXPECT(tk_fallback_name(-1) != 0, "names", "fallback unknown", 0, 1);
    EXPECT(tk_dat_name(-1) != 0, "names", "dat unknown", 0, 1);
    EXPECT(tk_ntp_status_name(-1) != 0, "names", "ntp unknown", 0, 1);
}

/* ------------------------------------------------------------ log rotation */

/* Rotation keeps the newest 8 KiB as whole lines.
 *
 * The t_rotate cases below are compiled only into the host test binary: they
 * assert properties of the trim function that no field run can observe, at a
 * measured cost of ~2.5 KB in TimeKeeper.exe, and the function they test is pure
 * and platform-independent, so the host build exercises exactly the code that
 * ships. The shipped /test flag still runs every other group.
 *
 * A line, as UTF-16LE with its CRLF, is what `kLine` is: 32 'a's, then
 * CR NUL LF NUL = 68 bytes. Kept as one .rodata constant on purpose: a helper
 * that built lines at runtime cost ~5 KB in the shipping binary, more than the
 * whole rotation module. */
#ifndef TK_SELFTEST_COMPACT
static const char kLine[68] = {
    'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,
    'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,
    'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,
    'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,'a',0,
    '\r',0,'\n',0
};

static void t_rotate(struct tk_report *r)
{
    static char buf[20 * 68];
    size_t kept, dropped, i, n;

    /* Filled once, by one loop, at the top: a fill *macro* expanded at each case
     * duplicated this body three times and cost ~5 KB in the shipped .exe. */
    for (i = 0; i < sizeof(buf); i += sizeof(kLine))
        for (n = 0; n < sizeof(kLine); n++) buf[i + n] = kLine[n];

    /* 1. A window of whole lines is kept in full. This is the usual case: the
     *    keep-size and the line length are both even, so the read window lands
     *    on a boundary. "Everything up to the last terminator" = everything. */
    kept = tk_log_trim_tail((unsigned char *)buf, 20 * sizeof(kLine), &dropped);
    EXPECT(kept == 20 * sizeof(kLine), "rotate", "whole lines kept",
           (int)kept, (int)(20 * sizeof(kLine)));
    EXPECT(dropped == 0, "rotate", "nothing dropped", (int)dropped, 0);

    /* 2. A window that ends inside a line must not emit that fragment: the file
     *    would grow a line out of half a character, which Notepad shows as
     *    mojibake and which reads as corruption. In the shipped path this cannot
     *    happen (every line is written with CRLF), so the rule is asserted here
     *    rather than relied on. Handing over 4 bytes less than a whole line
     *    refuses that entire line: kept is 19 lines, dropped is the 64 bytes of
     *    unfinished line plus the 4 withheld. Losing a line beats half a line. */
    kept = tk_log_trim_tail((unsigned char *)buf, 20 * sizeof(kLine) - 4, &dropped);
    EXPECT(kept == 19 * sizeof(kLine), "rotate", "trailing fragment dropped",
           (int)kept, (int)(19 * sizeof(kLine)));
    EXPECT(dropped == sizeof(kLine) - 4, "rotate", "whole unfinished line refused",
           (int)dropped, (int)(sizeof(kLine) - 4));

    /* 3. Half a UTF-16 unit at the end (odd byte count) is excluded the same
     *    way: a lone lead byte must never be written out. */
    kept = tk_log_trim_tail((unsigned char *)buf, 20 * sizeof(kLine) + 1, &dropped);
    EXPECT(kept == 20 * sizeof(kLine), "rotate", "odd tail excluded",
           (int)kept, (int)(20 * sizeof(kLine)));

    /* 4. Idempotence, which the forward-scan version got wrong: trimming the
     *    kept bytes again must change nothing. Rotation runs after every append,
     *    so a trim that also ate a line each time converges on keeping nothing
     *    while still reporting a nicely bounded file -- precisely the failure an
     *    operator would never notice. */
    kept = tk_log_trim_tail((unsigned char *)buf, 20 * sizeof(kLine), &dropped);
    kept = tk_log_trim_tail((unsigned char *)buf, kept, &dropped);
    EXPECT(kept == 20 * sizeof(kLine) && dropped == 0, "rotate", "second trim is a no-op",
           (int)kept, (int)(20 * sizeof(kLine)));

    /* 5. No terminator at all (one huge partial line, or a file written by
     *    something else): report "nothing recoverable" instead of inventing a
     *    start point. log.c turns that into an in-place truncation. */
    for (i = 0; i < 256; i++) buf[i] = (char)('x' + (i & 7));
    kept = tk_log_trim_tail((unsigned char *)buf, 256, &dropped);
    for (i = 0; i < sizeof(buf); i += sizeof(kLine))       /* refill for 6 and 7 */
        for (n = 0; n < sizeof(kLine); n++) buf[i + n] = kLine[n];
    EXPECT(kept == 0, "rotate", "no boundary keeps nothing", (int)kept, 0);
    EXPECT(dropped == 256, "rotate", "no boundary loses the window", (int)dropped, 256);

    /* 6. Floor: a complete line shorter than 32 bytes is refused, because
     *    rewriting a file through tmp+Flush+Move to store a fragment of a
     *    fragment is not worth the write. The line length includes its
     *    terminator, so 28 bytes of payload plus CRLF is a 32-byte window. */
    for (i = 0; i < 24; i++) buf[i] = 'y';
    buf[24] = '\r'; buf[25] = 0; buf[26] = '\n'; buf[27] = 0;
    kept = tk_log_trim_tail((unsigned char *)buf, 28, &dropped);
    EXPECT(kept == 0, "rotate", "sub-floor remnant refused", (int)kept, 0);
    for (i = 0; i < 28; i++) buf[i] = 'y';
    buf[28] = '\r'; buf[29] = 0; buf[30] = '\n'; buf[31] = 0;
    kept = tk_log_trim_tail((unsigned char *)buf, 32, &dropped);
    EXPECT(kept == 32, "rotate", "remnant at the floor kept", (int)kept, 32);

    /* 7. Degenerate sizes must not read out of bounds: n < 8 never enters the
     *    scan, and a single whole line is kept. */
    kept = tk_log_trim_tail((unsigned char *)buf, 0, &dropped);
    EXPECT(kept == 0 && dropped == 0, "rotate", "empty window", (int)kept, 0);
    kept = tk_log_trim_tail((unsigned char *)buf, 6, &dropped);
    EXPECT(kept == 0, "rotate", "six bytes is not a line", (int)kept, 0);
    kept = tk_log_trim_tail((unsigned char *)buf, sizeof(kLine), &dropped);
    EXPECT(kept == sizeof(kLine), "rotate", "single whole line kept",
           (int)kept, (int)sizeof(kLine));
}

#endif /* !TK_SELFTEST_COMPACT: t_rotate */

/* ---------------------------------------------------------------- driver */

int tk_selftest_run(tk_report_fn cb, void *ud)
{
    struct tk_report r;
    r.checks = 0; r.fails = 0; r.cb = cb; r.ud = ud;

    t_calendar(&r);
    t_filetime(&r);
    t_ntp(&r);
    t_sntp_packet(&r);
    t_dat(&r);
    t_http(&r);
    t_policy(&r);
    t_format(&r);
#ifndef TK_SELFTEST_COMPACT
    t_rotate(&r);   /* host build only, see the note above */
#endif

    if (cb && r.fails) {
        char msg[64];
        size_t n = 0;
        const char *p = "FAILURES: ";
        while (*p) msg[n++] = *p++;
        n += tk_fmt_u32(msg + n, sizeof(msg) - n, (uint32_t)r.fails, 1);
        p = " of ";
        while (*p) msg[n++] = *p++;
        n += tk_fmt_u32(msg + n, sizeof(msg) - n, (uint32_t)r.checks, 1);
        msg[n] = 0;
        cb(ud, msg);
    }
    return r.fails == 0 ? 0 : 1;
}

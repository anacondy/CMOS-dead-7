/* vectors.h — the test corpus shared by the host unit tests and the product's
 * own `TimeKeeper.exe /test`.
 *
 * One table, two consumers: what the build proves is what the shipped binary
 * proves, so a regression cannot pass CI and quietly break the .exe.
 *
 * Epoch values are not magic numbers: they are derived from the calendar inside
 * the assertions, and the host suite separately checks that calendar against
 * libc's timegm(). The table therefore cannot rot when a vector is added.
 *
 * This header is *data definitions only*. Include it from exactly one
 * translation unit (tests/vectors_impl.c) and use vectors_decl.h elsewhere.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_VECTORS_DATA_H
#define TIMEKEEPER_VECTORS_DATA_H

#include "timeutil.h"
#include "fallback_proto.h"
#include "verdict.h"

/* Reference instants used across several vectors, named once. */
#define TK_SEP1_2026   1788220800LL   /* 2026-09-01T00:00:00Z, a Tuesday */
#define TK_OCT1_2026   1790812800LL   /* 2026-10-01T00:00:00Z            */
#define TK_OCT16_2026  1792108800LL   /* 2026-10-16T00:00:00Z            */
#define TK_NOV01_2009  1257033600LL   /* 2009-11-01T00:00:00Z            */
#define TK_JAN01_2009  1230768000LL   /* the classic BIOS default        */

/* --- TimeKeeper.dat contents ---------------------------------------------- */
typedef struct {
    const char *text;
    int         len;          /* 0 = use strlen; >0 = exact byte count */
    int         accept;
    int         y; unsigned mo, d, hh, mi, ss;
    const char *why;
} tk_dat_vec_t;

static const tk_dat_vec_t tk_dat_vectors[] = {
  { "2026-09-01\n",                            0, 1, 2026, 9, 1, 0, 0, 0, "canonical" },
  { "2026-9-1",                                0, 1, 2026, 9, 1, 0, 0, 0, "single digit m/d (8 bytes)" },
  { "# TimeKeeper offline fallback\n2026-09-01\n", 0, 1, 2026, 9, 1, 0, 0, 0, "comment header" },
  { "\xEF\xBB\xBF" "2026-09-01",              0, 1, 2026, 9, 1, 0, 0, 0, "UTF-8 BOM" },
  { "2026-09-01T05:30:00",                     0, 1, 2026, 9, 1, 5,30, 0, "ISO with time" },
  { "2026-09-01 05:30",                         0, 1, 2026, 9, 1, 5,30, 0, "space time, no seconds" },
  { "junk 2026-09-01 more junk\n",             0, 1, 2026, 9, 1, 0, 0, 0, "token not at offset 0" },
  { "date=2026-09-01;",                         0, 1, 2026, 9, 1, 0, 0, 0, "punctuated" },
  { "\x00\x01\x02binary\x00noise 2026-09-01", 33, 1, 2026, 9, 1, 0, 0, 0, "NUL bytes must not stop the scan" },
  { "2026-09-01 00:00:60",                     0, 1, 2026, 9, 1, 0, 0, 59, "leap second clamped to :59" },
  { "2026-02-29",                              0, 0, 0, 0, 0, 0, 0, 0, "not a leap year" },
  { "2024-02-29",                              0, 1, 2024, 2, 29, 0, 0, 0, "leap day ok" },
  { "2026-13-01",                              0, 0, 0, 0, 0, 0, 0, 0, "month 13" },
  { "2026-04-31",                              0, 0, 0, 0, 0, 0, 0, 0, "April 31" },
  { "1999-01-01",                              0, 0, 0, 0, 0, 0, 0, 0, "below year floor" },
  { "2099-01-01",                              0, 0, 0, 0, 0, 0, 0, 0, "above year ceiling" },
  { "",                                        0, 0, 0, 0, 0, 0, 0, 0, "empty file" },
  { "0000-00-00",                              0, 0, 0, 0, 0, 0, 0, 0, "all zeros" },
  { "202-09-01",                               0, 0, 0, 0, 0, 0, 0, 0, "three digit year" },
  { "1970-01-01",                              0, 0, 0, 0, 0, 0, 0, 0, "epoch, out of policy window" },
  { "2026-09-01\r\n",                          0, 1, 2026, 9, 1, 0, 0, 0, "CRLF (Windows Notepad)" },
  { "\n\n\n 2026-09-01",                      0, 1, 2026, 9, 1, 0, 0, 0, "leading blank lines" },
  { "2026-09-01\n# trailing comment\n",        0, 1, 2026, 9, 1, 0, 0, 0, "trailing comment" },
};


/* --- HTTP Date values ------------------------------------------------------ */
typedef struct {
    const char *date;
    int         accept;
    int         y; unsigned mo, d, hh, mi, ss;
    const char *why;
} tk_http_vec_t;

static const tk_http_vec_t tk_http_vectors[] = {
  { "Tue, 01 Sep 2026 05:30:00 GMT",   1, 2026, 9, 1, 5,30, 0, "exactly as servers emit it" },
  { "Tue, 01 Sep 2026 00:00:00 GMT",   1, 2026, 9, 1, 0, 0, 0, "midnight UTC == 05:30 IST" },
  { "Sun, 15 Sep 2024 12:34:56 GMT",   1, 2024, 9,15,12,34,56, "another real weekday" },
  { "Thu, 01 Jan 1970 00:00:00 GMT",   1, 1970, 1, 1, 0, 0, 0, "epoch origin" },
  { "Sat, 26 Jul 1997 13:00:00 GMT",   1, 1997, 7,26,13, 0, 0, "an instant predating 2000" },
  { "Tuesday, 01-Sep-26 05:30:00 UTC", 1, 2026, 9, 1, 5,30, 0, "2-digit year, full weekday, dashes" },
  { "Tue, 1 Sep 2026 05:30:00 GMT",    1, 2026, 9, 1, 5,30, 0, "1-digit day" },
  { "Tue, 01 Sep 2026 05:30 GMT",      1, 2026, 9, 1, 5,30, 0, "seconds omitted" },
  { "Tue, 01 Sep 2026 05:30:00 +0530", 1, 2026, 9, 1, 0, 0, 0, "IST offset folded to UTC (05:30+0530 = midnight)" },
  { "Tue, 01 Sep 2026 05:30:00",       1, 2026, 9, 1, 5,30, 0, "zone omitted -> assume UTC" },
  { "Wed, 09 Sep 2026 18:30:00 -0500", 1, 2026, 9, 9,23,30, 0, "negative offset folded to UTC" },
  { "Fri, 01 Sep 2026 05:30:00 GMT",   0, 0, 0, 0, 0, 0, 0, "weekday lies (2026-09-01 is Tuesday)" },
  { "Tue, 31 Feb 2026 00:00:00 GMT",  0, 0, 0, 0, 0, 0, 0, "Feb 31" },
  { "Tue, 00 Sep 2026 00:00:00 GMT",   0, 0, 0, 0, 0, 0, 0, "day zero" },
  { "Tue, 01 Sep 2026 25:00:00 GMT",   0, 0, 0, 0, 0, 0, 0, "hour 25" },
  { "Tue, 01 Sep 2026 05:30:00 GMTX",  1, 2026, 9, 1, 5,30, 0, "trailing junk after zone is ignored" },
  { "garbage",                         0, 0, 0, 0, 0, 0, 0, "no date at all" },
  { "",                                0, 0, 0, 0, 0, 0, 0, "empty" },
  { "Tue, 01 Sep 1899 05:30:00 GMT",   0, 1899, 9, 1, 5,30, 0, "pre-1970 clamped by the parser itself" },
};

/* --- full HTTP response blocks, for the header extractor ------------------- */
typedef struct {
    const char *resp;
    int         accept;
    const char *why;
} tk_resp_vec_t;

static const tk_resp_vec_t tk_resp_vectors[] = {
  { "HTTP/1.1 200 OK\r\nServer: gws\r\nDate: Tue, 01 Sep 2026 05:30:00 GMT\r\n"
    "Content-Type: text/html\r\n\r\n", 1, "normal response" },
  { "HTTP/1.1 200 OK\r\ndate: Tue, 01 Sep 2026 05:30:00 GMT\r\n\r\n", 1,
    "lowercase header name" },
  { "HTTP/1.1 200 OK\r\n  Date: Tue, 01 Sep 2026 05:30:00 GMT\r\n\r\n", 1,
    "indented (obsolete folding) line is still honoured" },
  { "Date: Tue, 01 Sep 2026 05:30:00 GMT\r\n\r\n", 1, "Date as the only header" },
  { "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0, "no Date header" },
  { "HTTP/1.1 200 OK\r\nX-Date: Tue, 01 Sep 2026 05:30:00 GMT\r\n\r\n", 0,
    "must not match a suffixed header name" },
  { "HTTP/1.1 200 OK\r\nDate: nonsense\r\n\r\n", 0, "malformed value" },
  { "HTTP/1.1 200 OK\r\nDate: Tue, 01 Sep 2026 05:30:00 GMT", 1, "no trailing newline" },
};

/* --- NTP fields -> epoch, era rollover, sub-second ------------------------ */
typedef struct {
    uint32_t sec, frac;
    int      accept;
    time64_t expect_epoch;      /* whole seconds, unrounded */
    uint32_t expect_ms;
    const char *why;
} tk_ntp_vec_t;

#define TK_NTP_20260901 (TK_SEP1_2026 + TK_UNIX_TO_NTP_SEC)

static const tk_ntp_vec_t tk_ntp_vectors[] = {
  { (uint32_t)TK_NTP_20260901, 0u,          1, TK_SEP1_2026,   0, "exact second" },
  { (uint32_t)TK_NTP_20260901, 0x80000000u, 1, TK_SEP1_2026, 500, "half second" },
  { (uint32_t)TK_NTP_20260901, 0x7FFFFFFFu, 1, TK_SEP1_2026, 499, "just under half" },
  { (uint32_t)TK_NTP_20260901, 0xFFFFFFFFu, 1, TK_SEP1_2026, 999, "maximum fraction" },
  { 0u, 0u,                                  0, 0, 0, "all-zero = unsynchronised" },
  { 1000u, 0u,                               1, 2085979496LL,   0,
    "era-0 value from 1900 rolls forward to era 1 (2036+)" },
  { TK_NTP_ERA1_PIVOT, 0u,                   1,  946684800LL,   0, "pivot itself is NOT shifted" },
  { TK_NTP_ERA1_PIVOT - 1u, 0u,              1, 5241652095LL,   0, "one below pivot IS shifted by 2^32" },
};

/* --- policy: skip vs sync -------------------------------------------------- */
typedef struct {
    time64_t now, ref;
    int      ref_fresh;
    int      force;
    int      date_only;
    int      expect;
    const char *why;
} tk_verdict_vec_t;

static const tk_verdict_vec_t tk_verdict_vectors[] = {
  { TK_SEP1_2026,             TK_SEP1_2026, 1, 0, 0, TKV_SKIP_ALREADY_OK, "test 5: right clock, same instant" },
  { TK_SEP1_2026 - 100,       TK_SEP1_2026, 1, 0, 0, TKV_SKIP_ALREADY_OK, "100 s behind a fresh ref: inside 5 min" },
  { TK_SEP1_2026 + 100,       TK_SEP1_2026, 1, 0, 0, TKV_SKIP_ALREADY_OK, "100 s ahead of ref: inside 5 min" },
  { TK_SEP1_2026 - 3*86400,   TK_SEP1_2026, 1, 0, 0, TKV_SYNC,            "3 days off: go to network" },
  { TK_JAN01_2009,            TK_SEP1_2026, 1, 0, 0, TKV_SYNC,            "test 1/2: stuck in 2009 -> network" },
  { TK_SEP1_2026,             TK_SEP1_2026, 1, 1, 0, TKV_SYNC_FORCED,     "/force beats the skip check" },
  { TK_SEP1_2026 + 400*86400, TK_SEP1_2026, 0, 0, 0, TKV_NEED_NETWORK,    "clock far ahead of a stale ref" },
  { TK_SEP1_2026 - 1,         TK_SEP1_2026, 0, 0, 0, TKV_NEED_NETWORK,    "1 s behind ref: ref cannot certify, must go to network" },
  { TK_EPOCH_INVALID,         TK_SEP1_2026, 0, 0, 0, TKV_SYNC,            "unreadable clock" },
};


/* --- policy: offline fallback gate ---------------------------------------- */
typedef struct {
    time64_t now, ref, fb;
    int      allow_back;
    int      expect;
    const char *why;
} tk_fb_vec_t;

static const tk_fb_vec_t tk_fallback_vectors[] = {
  { TK_JAN01_2009, TK_SEP1_2026, TK_SEP1_2026, 0, TKF_APPLY, "test 1: dead CMOS -> apply the fallback date" },
  { TK_SEP1_2026,  TK_SEP1_2026, TK_SEP1_2026, 0, TKF_SKIP_OK, "clock equals the fallback -> nothing to do" },
  { TK_SEP1_2026 + 86400*60, TK_SEP1_2026, TK_SEP1_2026, 0, TKF_SKIP_WOULD_REGRESS, "clock newer -> never regress" },
  { 2300000000LL,  TK_SEP1_2026, TK_SEP1_2026, 0, TKF_SKIP_WOULD_REGRESS, "clock in 2042: refuse to drag it back" },
  { TK_SEP1_2026 + 86400*60, TK_SEP1_2026, TK_SEP1_2026, 1, TKF_APPLY, "/force permits a deliberate backdate" },
  { TK_EPOCH_INVALID, TK_SEP1_2026, TK_SEP1_2026, 0, TKF_APPLY, "no readable clock at all -> apply" },
  { TK_JAN01_2009, TK_SEP1_2026, TK_EPOCH_INVALID, 0, TKF_SKIP_OK, "nothing stored to apply -> do not invent a date" },
  { TK_NOV01_2009, TK_SEP1_2026, TK_SEP1_2026, 0, TKF_APPLY, "any 2009 date, not just the BIOS default" },
};

/* --- policy: .dat self-update gate ---------------------------------------- */
typedef struct {
    int      trusted;
    time64_t synced, stored, embedded;
    int      expect_write;
    int      expect_dec;
    const char *why;
} tk_datupd_vec_t;

static const tk_datupd_vec_t tk_datupd_vectors[] = {
  { 1, TK_OCT16_2026, TK_SEP1_2026, TK_SEP1_2026, 1, TKD_STALE_REWRITE,
    "test 2: October sync with a September .dat -> write 2026-10-01" },
  { 1, TK_SEP1_2026 + 9*86400, TK_SEP1_2026, TK_SEP1_2026, 0, TKD_IDEMPOTENT,
    "test 3: second run in the same month -> no rewrite" },
  { 1, TK_SEP1_2026, TK_SEP1_2026, TK_SEP1_2026, 0, TKD_IDEMPOTENT,
    "sync exactly on the stored first-of-month -> no rewrite" },
  { 1, TK_OCT1_2026, TK_SEP1_2026, TK_SEP1_2026, 1, TKD_STALE_REWRITE,
    "sync at 2026-10-01T00:00:00 -> month turns over on the dot" },
  { 1, TK_OCT1_2026 - 1, TK_SEP1_2026, TK_SEP1_2026, 0, TKD_IDEMPOTENT,
    "2026-09-30T23:59:59 -> still September, no rewrite" },
  { 0, TK_OCT16_2026, TK_SEP1_2026, TK_SEP1_2026, 0, TKD_NOT_WHILE_OFF,
    "safety: never update .dat on an untrusted run" },
  { 1, TK_OCT16_2026, TK_EPOCH_INVALID, TK_OCT1_2026, 0, TKD_NOFILE_CURRENT,
    "no file, embedded default already current -> zero I/O" },
  { 1, TK_OCT16_2026, TK_EPOCH_INVALID, TK_SEP1_2026, 1, TKD_NOFILE_STALE,
    "no file, embedded default stale -> create it" },
  { 1, TK_OCT1_2026 - 1, TK_OCT16_2026, TK_SEP1_2026, 0, TKD_NEWER_ON_DISK,
    "never shrink a .dat that is ahead of today" },
  { 0, TK_OCT16_2026, TK_SEP1_2026, TK_SEP1_2026, 0, TKD_NOT_WHILE_OFF,
    "offline runs must never touch the file" },
};

#endif /* TIMEKEEPER_VECTORS_DATA_H */

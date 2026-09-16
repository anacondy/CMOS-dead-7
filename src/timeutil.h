/* timeutil.h — pure calendar arithmetic + parsers. No OS, no heap, no locale,
 * no floating point, and no policy: this file says what a date *is*, verdict.c
 * decides whether it is *believable*.
 *
 * Canonical representations
 *   epoch seconds : time64_t  seconds since 1970-01-01T00:00:00Z (signed)
 *   FILETIME      : unsigned 100 ns ticks since 1601-01-01T00:00:00Z
 *   NTP timestamp : 32.32 fixed-point seconds since 1900-01-01T00:00:00Z
 *
 * These exact objects are linked into the shipping .exe and into the host unit
 * tests, where tk_days_from_civil/tk_civil_from_epoch_utc are checked against
 * libc timegm()/gmtime() and the parsers against fixed vectors.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_TIMEUTIL_H
#define TIMEKEEPER_TIMEUTIL_H

#include <stdint.h>
#include <stddef.h>

typedef int64_t time64_t;

#define TK_EPOCH_INVALID ((time64_t)0x7FFFFFFFFFFFFFFFLL)   /* "no time known" */

#define TK_FT_TICKS_PER_SEC 10000000ULL
#define TK_UNIX_TO_NTSECS   11644473600ULL    /* 1970-01-01 in FILETIME seconds */
#define TK_UNIX_TO_NTP_SEC  2208988800LL      /* 1970-01-01 in NTP seconds      */
#define TK_NTP_ERA1_SEC     4294967296ULL     /* 2^32: the 2036 rollover        */
#define TK_NTP_ERA1_PIVOT   3155673600uL      /* 2000-01-01 as era-0 NTP secs   */

/* Widest epoch seconds SetSystemTime can express:
 *   1601-01-01T00:00:00Z (FILETIME 0) .. 30827-12-31T23:59:59Z (SYSTEMTIME cap) */
#define TK_EPOCH_FT_MIN ((time64_t)-11644473600LL)
#define TK_EPOCH_FT_MAX ((time64_t)253402300799LL)

/* ------------------------------------------------------------------ calendar */
int      tk_is_leap(int y);
unsigned tk_days_in_month(int y, unsigned m);           /* 0 for an invalid month */
int64_t  tk_days_from_civil(int y, unsigned m, unsigned d, int *ok);
void     tk_civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d);
int64_t  tk_epoch_floor_days(time64_t e);
int      tk_day_of_week_from_days(int64_t z);           /* 0=Sun .. 6=Sat */
int      tk_day_of_week_from_epoch(time64_t e);

/* --------------------------------------------------------------- conversions */
time64_t tk_epoch_from_civil_utc(int y, unsigned mo, unsigned d,
                                 unsigned hh, unsigned mm, unsigned ss, int *ok);
int      tk_civil_from_epoch_utc(time64_t e, int *y, unsigned *mo, unsigned *d,
                                 unsigned *hh, unsigned *mm, unsigned *ss);

/* FILETIME <-> epoch. tk_epoch_from_filetime truncates sub-second ticks. */
int      tk_filetime_from_epoch_ms(time64_t e, uint32_t ms,
                                   uint32_t *lo, uint32_t *hi);
int      tk_epoch_from_filetime(uint32_t lo, uint32_t hi, time64_t *epoch,
                                uint32_t *ms);

/* NTP timestamp parsing. tk_ntp_fields_to_epoch truncates to whole seconds and
 * returns the sub-second remainder in *ms (never >= 1000), so the caller can
 * round to the nearest second when building the FILETIME — truncating there
 * would bias the clock ~0.5 s slow on every boot. *out_sec is already
 * era-rollover corrected. Returns 0 for the all-zero "unsynchronised" stamp. */
int      tk_ntp_fields_to_epoch(uint32_t sec, uint32_t frac,
                                time64_t *out_sec, uint32_t *out_ms);
int      tk_ntp_to_epoch(const uint8_t wire[8], time64_t *out_sec, uint32_t *out_ms);

/* ------------------------------------------------------------------ parsers */
/* First "YYYY-M-D"/"YYYY-MM-DD", optionally followed by "HH:MM[:SS]" after 'T'
 * or whitespace. Skips a UTF-8 BOM and '#' comment lines. out_* all optional. */
int      tk_parse_date_text(const char *buf, size_t len,
                            int *y, unsigned *mo, unsigned *d,
                            unsigned *hh, unsigned *mm, unsigned *ss);

/* RFC 1123 / RFC 850 / RFC 822 Date value. Tolerates 1-2 digit day, 3-letter or
 * full month name, numeric month, 2- or 4-digit year, "GMT"/"UTC"/"UT"/"Z",
 * a "+HHMM"/"-HHMM" offset (folded to UTC) and a missing zone (assumed UTC).
 * A stated weekday is verified against the calendar; disagreement rejects the
 * value, so a truncated or hand-edited header cannot smuggle in a wrong time. */
int      tk_parse_http_date(const char *s, size_t n, time64_t *out);
int      tk_http_extract_date(const char *resp, size_t n, time64_t *out);

/* --------------------------------------------------------------- formatting
 * Fixed-width, allocation-free; return bytes written (NUL excluded). */
size_t   tk_fmt_u32(char *dst, size_t cap, uint32_t v, unsigned min_width);
size_t   tk_fmt_date(char *dst, size_t cap, int y, unsigned m, unsigned d);
size_t   tk_fmt_time(char *dst, size_t cap, unsigned h, unsigned mi, unsigned s);
size_t   tk_fmt_epoch_utc(char *dst, size_t cap, time64_t e);   /* "YYYY-MM-DD HH:MM:SS" */

#endif /* TIMEKEEPER_TIMEUTIL_H */

/* timeutil.c — pure calendar arithmetic + parsers. See timeutil.h.
 *
 * Integer math only. No Windows headers, no malloc, no printf family, no
 * locale, no floating point, no policy. Every range claim is justified inline
 * so an auditor can check it without running anything.
 *
 * SPDX-License-Identifier: MIT
 */
#include "timeutil.h"

#include "tklibc.h"

/* ------------------------------------------------------------ small helpers */

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int lit3(const char *s, const char *low)
{
    return lc(s[0]) == low[0] && lc(s[1]) == low[1] && lc(s[2]) == low[2];
}

static int64_t div_floor(int64_t a, int64_t b)
{
    /* b is always > 0 here, so one adjustment is enough. */
    int64_t q = a / b;
    if (a % b != 0 && a < 0) q--;
    return q;
}

static int64_t mod_floor(int64_t a, int64_t b)
{
    int64_t r = a % b;
    if (r < 0) r += b;
    return r;
}

/* Consume between min_d and max_d decimal digits. On failure the position is
 * restored so an alternative production can be tried at the same offset. */
static int scan_num(const char *s, size_t n, size_t *i,
                    unsigned min_d, unsigned max_d, uint32_t *out)
{
    size_t   start = *i;
    uint32_t v = 0;

    while (*i < n && max_d > 0u && is_digit(s[*i])) {
        if (v > 429496729u) break;                 /* v*10 would overflow u32 */
        v = v * 10u + (uint32_t)(s[*i] - '0');
        (*i)++;
        max_d--;
    }
    if (*i - start < min_d) {
        *i = start;
        return 0;
    }
    *out = v;
    return 1;
}

static void skip_chars(const char *s, size_t n, size_t *i, const char *set)
{
    while (*i < n) {
        const char *p = set;
        while (*p && *p != s[*i]) p++;
        if (!*p) break;
        (*i)++;
    }
}

/* ------------------------------------------------------------------ calendar */

int tk_is_leap(int y)
{
    if (y < 1) return 0;
    return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
}

unsigned tk_days_in_month(int y, unsigned m)
{
    static const unsigned char dim[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};

    if (m < 1u || m > 12u) return 0;
    if (m == 2u && tk_is_leap(y)) return 29u;
    return dim[m - 1u];
}

/* Days since 1970-01-01 — Hinnant's days_from_civil. Re-indexing the civil year
 * to start at 1 March puts the leap day at the *end* of the year, which is what
 * removes every per-month special case below. The year decrement for Jan/Feb
 * must happen *before* the era division: doing it afterwards (a tempting
 * simplification) is wrong for years divisible by 400, off by exactly one year.
 * Verified against libc timegm() for every day from 1601-01-01 to 2100-12-31. */
int64_t tk_days_from_civil(int y, unsigned m, unsigned d, int *ok)
{
    int64_t  yy, era, r;
    uint32_t yoe, doy, mp, doe;

    if (ok) *ok = 0;
    if (y < 1 || y > 9999)                    return 0;
    if (d < 1u || d > tk_days_in_month(y, m)) return 0;  /* rejects bad month too */

    yy  = (int64_t)y - (m <= 2u ? 1 : 0);
    era = (yy >= 0 ? yy : yy - 399) / 400;
    yoe = (uint32_t)(yy - era * 400);                     /* year of era [0,399] */
    mp  = m + (m > 2u ? (uint32_t)-3u : 9u);              /* Mar=0 .. Feb=11     */
    doy = (153u * mp + 2u) / 5u + d - 1u;                 /* day of year [0,365] */
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;       /* day of era [0,146096] */
    r   = era * 146097 + (int64_t)doe - 719468;

    if (ok) *ok = 1;
    return r;
}

void tk_civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    int64_t  era, yy;
    uint32_t doe, yoe, doy, mp, dd, mm;

    z   = z + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (uint32_t)(z - era * 146097);                   /* [0, 146096] */
    yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    yy  = (int64_t)yoe + era * 400;
    doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);     /* [0, 365] */
    mp  = (5u * doy + 2u) / 153u;                          /* [0, 11]  */
    dd  = doy - (153u * mp + 2u) / 5u + 1u;
    mm  = mp + (mp < 10u ? 3u : (uint32_t)-9u);
    yy += (mm <= 2u ? 1 : 0);                              /* un-shift */

    if (y) *y = (int)yy;
    if (m) *m = mm;
    if (d) *d = dd;
}

int tk_day_of_week_from_days(int64_t z)
{
    int64_t r = (z + 4) % 7;         /* z=0 is 1970-01-01, a Thursday */
    if (r < 0) r += 7;
    return (int)r;                    /* 0 = Sunday */
}

int64_t tk_epoch_floor_days(time64_t e) { return div_floor(e, 86400); }

int tk_day_of_week_from_epoch(time64_t e)
{
    return tk_day_of_week_from_days(tk_epoch_floor_days(e));
}

/* --------------------------------------------------------------- conversions */

time64_t tk_epoch_from_civil_utc(int y, unsigned mo, unsigned d,
                                 unsigned hh, unsigned mm, unsigned ss, int *ok)
{
    int64_t days;
    int     cok = 0;

    if (hh > 23u || mm > 59u || ss > 60u) { if (ok) *ok = 0; return TK_EPOCH_INVALID; }
    days = tk_days_from_civil(y, mo, d, &cok);
    if (!cok) { if (ok) *ok = 0; return TK_EPOCH_INVALID; }
    if (ok) *ok = 1;
    /* y<=9999 bounds days to [-719162, 2932896]; x86400 + 86399 stays in i64. */
    return days * 86400 + (int64_t)hh * 3600 + (int64_t)mm * 60 + (int64_t)ss;
}

int tk_civil_from_epoch_utc(time64_t e, int *y, unsigned *mo, unsigned *d,
                            unsigned *hh, unsigned *mm, unsigned *ss)
{
    int64_t  days = div_floor(e, 86400);
    uint32_t sec  = (uint32_t)mod_floor(e, 86400);
    int      yy = 0;
    unsigned mmd = 0, dd = 0;

    if (days < -3000000 || days > 3000000) return 0;    /* ~1601..10000 guard */
    tk_civil_from_days(days, &yy, &mmd, &dd);
    if (yy < 1 || yy > 9999) return 0;

    if (y)  *y  = yy;
    if (mo) *mo = mmd;
    if (d)  *d  = dd;
    if (hh) *hh = sec / 3600u;
    if (mm) *mm = (sec / 60u) % 60u;
    if (ss) *ss = sec % 60u;
    return 1;
}

/* ms must be < 1000. Ticks = (e - FT_MIN + 11644473600?) ... see below. */
int tk_filetime_from_epoch_ms(time64_t e, uint32_t ms, uint32_t *lo, uint32_t *hi)
{
    uint64_t secs, total;

    if (!lo || !hi) return 0;
    if (ms > 999u) return 0;
    if (e < TK_EPOCH_FT_MIN || e > TK_EPOCH_FT_MAX) return 0;

    /* (e - TK_EPOCH_FT_MIN) is the FILETIME second count, i.e. 0 .. 265046774399
     * for the supported years: x1e7 = 2.65e18 < UINT64_MAX (1.8e19). No
     * overflow is reachable through the range guard above. */
    secs  = (uint64_t)((int64_t)e - TK_EPOCH_FT_MIN);
    total = secs * TK_FT_TICKS_PER_SEC + (uint64_t)ms * 10000u;
    *lo = (uint32_t)(total & 0xFFFFFFFFu);
    *hi = (uint32_t)(total >> 32);
    return 1;
}

int tk_epoch_from_filetime(uint32_t lo, uint32_t hi, time64_t *epoch, uint32_t *ms)
{
    uint64_t total = ((uint64_t)hi << 32) | (uint64_t)lo;
    uint64_t secs;

    if (total > (uint64_t)0x7FFFFFFFFFFFFFFFULL) return 0;   /* beyond SYSTEMTIME */
    secs = total / TK_FT_TICKS_PER_SEC;
    if (epoch) *epoch = (int64_t)secs + TK_EPOCH_FT_MIN;
    if (ms)    *ms    = (uint32_t)((total - secs * TK_FT_TICKS_PER_SEC) / 10000u);
    return 1;
}

/* NTP era rollover: the 32-bit seconds field wraps on 2036-07-02. A server
 * already in era 1 sends values that, read as era 0, land around 1904. So any
 * era-0 reading before 2000-01-01 is treated as era 1 — the same heuristic as
 * the NTPv3 reference implementation. Past 2036 this tool's accepted-year cap
 * (config.h TK_MAX_EPOCH_YEAR) is what keeps the guess bounded. */
int tk_ntp_fields_to_epoch(uint32_t sec, uint32_t frac,
                           time64_t *out_sec, uint32_t *out_ms)
{
    uint64_t secs = sec;
    uint64_t ms;

    if (sec == 0u) return 0;                    /* "not synchronised" marker */
    if (sec < TK_NTP_ERA1_PIVOT) secs += TK_NTP_ERA1_SEC;

    /* ms = frac * 1000 / 2^32, exactly. The multiply fits in u64 (max
     * 4.295e12); dividing first by 4294967 would shift the 500 ms rounding
     * boundary by a hair and round 0x7FFFFFFF up to 500. */
    ms = ((uint64_t)frac * 1000ULL) >> 32;
    if (ms > 999u) ms = 999u;

    {
        int64_t e = (int64_t)secs - TK_UNIX_TO_NTP_SEC;
        if (e < 0) return 0;
        if (out_sec) *out_sec = e;
    }
    if (out_ms) *out_ms = (uint32_t)ms;
    return 1;
}

int tk_ntp_to_epoch(const uint8_t w[8], time64_t *out_sec, uint32_t *out_ms)
{
    uint32_t sec, frac;

    if (!w) return 0;
    sec  = ((uint32_t)w[0] << 24) | ((uint32_t)w[1] << 16) |
           ((uint32_t)w[2] << 8)  |  (uint32_t)w[3];
    frac = ((uint32_t)w[4] << 24) | ((uint32_t)w[5] << 16) |
           ((uint32_t)w[6] << 8)  |  (uint32_t)w[7];
    return tk_ntp_fields_to_epoch(sec, frac, out_sec, out_ms);
}

/* --------------------------------------------------------------- parse .dat */

int tk_parse_date_text(const char *buf, size_t len,
                       int *y, unsigned *mo, unsigned *d,
                       unsigned *hh, unsigned *mm, unsigned *ss)
{
    size_t i = 0;

    if (hh) *hh = 0;
    if (mm) *mm = 0;
    if (ss) *ss = 0;
    /* Shortest acceptable token is "YYYY-M-D" = 8 bytes, not 10: a hand-edited
     * file may well read "2026-9-1". */
    if (!buf || len < 8) return 0;

    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&        /* UTF-8 BOM */
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) i = 3;

    while (i + 8 <= len) {
        uint32_t yy = 0, mo2 = 0, dd = 0;
        uint32_t h = 0, mi = 0, s2 = 0;
        int      ok = 0;
        size_t   start = i;

        if (buf[i] == '#') {                                 /* comment line */
            while (i < len && buf[i] != '\n') i++;
            if (i < len) i++;
            continue;
        }
        if (!(i + 4 < len && is_digit(buf[i]) && is_digit(buf[i + 1]) &&
              is_digit(buf[i + 2]) && is_digit(buf[i + 3]) && buf[i + 4] == '-')) {
            i++; continue;
        }

        /* The look-ahead above is only a fast filter; the year still has to be
         * read from the same offset. Skipping it with `i += 4` here loses the
         * value entirely (which is what the vector suite caught). */
        if (!scan_num(buf, len, &i, 4, 4, &yy))    { i = start + 1; continue; }
        if (i >= len || buf[i] != '-')             { i = start + 1; continue; }
        i++;
        if (!scan_num(buf, len, &i, 1, 2, &mo2))   { i = start + 1; continue; }
        if (i >= len || buf[i] != '-')             { i = start + 1; continue; }
        i++;
        if (!scan_num(buf, len, &i, 1, 2, &dd))    { i = start + 1; continue; }

        (void)tk_days_from_civil((int)yy, mo2, dd, &ok);   /* validates Feb 30 etc. */
        if (!ok) { i = start + 1; continue; }

        /* Optional time, only when directly adjacent (ISO 'T') or separated by
         * spaces/tabs on the same line. Parse into temporaries and commit only
         * on success, so a failed time attempt cannot corrupt the position. */
        {
            size_t   j = i;
            uint32_t hv = 0, mv = 0, sv = 0;
            int      tried = 0;

            if (j < len && buf[j] == 'T') { j++; tried = 1; }
            else {
                size_t k = j;
                skip_chars(buf, len, &k, " \t");
                if (k > j && k < len && is_digit(buf[k])) { j = k; tried = 1; }
            }
            if (tried && scan_num(buf, len, &j, 1, 2, &hv) && hv < 24u &&
                j < len && buf[j] == ':') {
                j++;
                if (scan_num(buf, len, &j, 2, 2, &mv) && mv < 60u) {
                    if (j < len && buf[j] == ':') {
                        uint32_t t2 = 0;
                        j++;
                        if (scan_num(buf, len, &j, 2, 2, &t2) && t2 <= 60u)
                            sv = (t2 == 60u) ? 59u : t2;      /* clamp leap second */
                    }
                    h = hv; mi = mv; s2 = sv; i = j;
                }
            }
        }

        if (y)  *y  = (int)yy;
        if (mo) *mo = mo2;
        if (d)  *d  = dd;
        if (hh) *hh = h;
        if (mm) *mm = mi;
        if (ss) *ss = s2;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ parse Date: */

static const char kMonAbbr[12][4] =
    {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
static const char kDayAbbr[7][4] =
    {"sun","mon","tue","wed","thu","fri","sat"};

int tk_parse_http_date(const char *s, size_t n, time64_t *out)
{
    static const char *const kZones[3] = {"gmt", "utc", "ut "};
    size_t   i = 0, st;
    uint32_t day = 0, year = 0, hh = 0, mi = 0, ss = 0, mon = 0, off = 0;
    unsigned q;
    int      dow = -1, ok = 0;
    char     sign = 0;
    time64_t e;

    if (!s || n < 13) return 0;

    /* 1. weekday — optional, but honoured in step 7 if present. */
    if (i + 3 <= n && s[i] >= 'A' && s[i] <= 'Z') {
        for (q = 0; q < 7u; q++) {
            if (lit3(s + i, kDayAbbr[q])) {
                size_t j = i + 3;
                while (j < n && s[j] >= 'a' && s[j] <= 'z') j++;
                if (j < n && s[j] == ',') { dow = (int)q; i = j + 1; }
                break;
            }
        }
    }
    skip_chars(s, n, &i, " \t");

    /* 2. day of month */
    if (!scan_num(s, n, &i, 1, 2, &day) || day < 1u || day > 31u) return 0;
    skip_chars(s, n, &i, " -\t");

    /* 3. month: letters (first three are unambiguous for all twelve names) or
     *    digits, which RFC 850 style origin servers and proxies emit. */
    if (i + 3 <= n && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) {
        int found = 0;
        for (q = 0; q < 12u; q++) {
            if (lit3(s + i, kMonAbbr[q])) {
                size_t j = i + 3;
                while (j < n && ((s[j] >= 'a' && s[j] <= 'z') ||
                                 (s[j] >= 'A' && s[j] <= 'Z'))) j++;
                mon = q + 1u; i = j; found = 1; break;
            }
        }
        if (!found) return 0;
    } else if (!scan_num(s, n, &i, 1, 2, &mon) || mon < 1u || mon > 12u) {
        return 0;
    }
    skip_chars(s, n, &i, " -\t");

    /* 4. year: exactly 4 digits, or exactly 2 with the POSIX pivot */
    st = i;
    if (!scan_num(s, n, &i, 2, 4, &year)) return 0;
    if (i - st == 2u)      year += (year < 70u) ? 2000u : 1900u;
    else if (i - st != 4u) return 0;
    /* The calendar here supports year 1 upwards, but epoch seconds below 1970
     * would flow straight into an unsigned FILETIME in the wrong order. A
     * pre-1970 Date header is never a real answer, so clamp at the source. */
    if (year < 1970u) return 0;

    skip_chars(s, n, &i, " \t");

    /* 5. time; seconds may be absent, a leap second is clamped */
    if (!scan_num(s, n, &i, 2, 2, &hh) || hh > 23u) return 0;
    if (i >= n || s[i] != ':') return 0;
    i++;
    if (!scan_num(s, n, &i, 2, 2, &mi) || mi > 59u) return 0;
    if (i < n && s[i] == ':') {
        i++;
        if (!scan_num(s, n, &i, 2, 2, &ss) || ss > 60u) return 0;
        if (ss == 60u) ss = 59u;
    }
    skip_chars(s, n, &i, " \t");

    /* 6. zone: numeric offset, or a name. Absent => the value is UTC. */
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        sign = s[i]; i++;
        if (!scan_num(s, n, &i, 4, 4, &off)) return 0;
    } else {
        for (q = 0; q < 3u; q++) {
            if (i + 3 <= n && lit3(s + i, kZones[q])) { i += 3; break; }
        }
        if (q == 3u && i < n && (s[i] == 'Z' || s[i] == 'z')) i++;
    }

    e = tk_epoch_from_civil_utc((int)year, mon, day, hh, mi, ss, &ok);
    if (!ok) return 0;

    if (sign) {
        int64_t adj;
        if (off % 100u > 59u) return 0;
        adj = (int64_t)(off / 100u) * 3600 + (int64_t)(off % 100u) * 60;
        e += (sign == '-') ? adj : -adj;      /* "-0530" = 5h30 behind UTC */
    }

    /* 7. weekday cross-check. */
    if (dow >= 0 && tk_day_of_week_from_epoch(e) != dow) return 0;

    if (out) *out = e;
    return 1;
}

int tk_http_extract_date(const char *resp, size_t n, time64_t *out)
{
    size_t i = 0, ls = 0;

    if (!resp || n < 16) return 0;
    for (;;) {
        size_t le, k;
        int    done = (i == n);

        if (!done && resp[i] != '\n') { i++; continue; }

        le = i;                                    /* line is [ls, le) */
        if (le - ls >= 6) {
            const char *p = resp + ls;
            k = 0;
            while (k + 5 <= le - ls && (p[k] == ' ' || p[k] == '\t')) k++;
            /* Header field names are case-insensitive (RFC 9110 §5.1); match
             * "date:" in any casing so a proxy that lower-cases everything
             * still yields a time. */
            if (k + 5 <= le - ls && lit3(p + k, "dat") && p[k+3] == 'e' && p[k+4] == ':') {
                size_t vs = ls + k + 5, ve = le;
                while (vs < ve && (resp[vs] == ' ' || resp[vs] == '\t')) vs++;
                while (ve > vs && (resp[ve-1] == '\r' || resp[ve-1] == '\n' ||
                                   resp[ve-1] == ' ')) ve--;
                return tk_parse_http_date(resp + vs, ve - vs, out);
            }
        }
        if (done) break;
        ls = i + 1;
        i++;
    }
    return 0;
}

/* --------------------------------------------------------------- formatting */

size_t tk_fmt_u32(char *dst, size_t cap, uint32_t v, unsigned min_width)
{
    char   tmp[10];
    size_t n = 0, out = 0;

    if (cap == 0) return 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n < min_width && n < sizeof(tmp)) tmp[n++] = '0';

    while (n > 0 && out + 1 < cap) dst[out++] = tmp[--n];
    dst[out] = '\0';
    return out;
}

size_t tk_fmt_date(char *dst, size_t cap, int y, unsigned m, unsigned d)
{
    size_t n = 0;

    if (cap < 11) { if (cap) dst[0] = '\0'; return 0; }
    n += tk_fmt_u32(dst + n, cap - n, (uint32_t)y, 4);
    dst[n++] = '-';
    n += tk_fmt_u32(dst + n, cap - n, m, 2);
    dst[n++] = '-';
    n += tk_fmt_u32(dst + n, cap - n, d, 2);
    dst[n] = '\0';
    return n;
}

size_t tk_fmt_time(char *dst, size_t cap, unsigned h, unsigned mi, unsigned s)
{
    size_t n = 0;

    if (cap < 9) { if (cap) dst[0] = '\0'; return 0; }
    n += tk_fmt_u32(dst + n, cap - n, h, 2);
    dst[n++] = ':';
    n += tk_fmt_u32(dst + n, cap - n, mi, 2);
    dst[n++] = ':';
    n += tk_fmt_u32(dst + n, cap - n, s, 2);
    dst[n] = '\0';
    return n;
}

size_t tk_fmt_epoch_utc(char *dst, size_t cap, time64_t e)
{
    int      y = 1970;
    unsigned mo = 1, d = 1, hh = 0, mm = 0, ss = 0;
    size_t   n;

    if (cap == 0) return 0;
    dst[0] = '\0';
    if (!tk_civil_from_epoch_utc(e, &y, &mo, &d, &hh, &mm, &ss)) return 0;

    n = tk_fmt_date(dst, cap, y, mo, d);
    if (n + 9 < cap) {
        dst[n++] = ' ';
        n += tk_fmt_time(dst + n, cap - n, hh, mm, ss);
    }
    dst[n < cap ? n : cap - 1] = '\0';
    return n;
}

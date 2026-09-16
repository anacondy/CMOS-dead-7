/* fallback_proto.c — TimeKeeper.dat format + update policy, pure. See header.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fallback_proto.h"
#include "tklibc.h"

int tk_dat_parse(const char *buf, size_t len, int year_min, int year_max,
                 time64_t *epoch_out, int *y_out, unsigned *mo_out, unsigned *d_out)
{
    int      y = 0, ok = 0;
    unsigned mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    time64_t e;

    if (epoch_out) *epoch_out = TK_EPOCH_INVALID;
    if (!buf || len == 0) return 0;

    if (!tk_parse_date_text(buf, len, &y, &mo, &d, &hh, &mm, &ss)) return 0;
    if (y < year_min || y > year_max) return 0;

    /* A file with no explicit time means "midnight UTC of that date" — which is
     * 05:30 IST, the convention documented in ARCHITECTURE.md (D2). With an
     * explicit time we honour it as UTC so a user in another zone can still pin
     * an exact instant. */
    e = tk_epoch_from_civil_utc(y, mo, d, hh, mm, ss, &ok);
    if (!ok || e == TK_EPOCH_INVALID) return 0;

    if (epoch_out) *epoch_out = e;
    if (y_out)     *y_out = y;
    if (mo_out)    *mo_out = mo;
    if (d_out)     *d_out = d;
    return 1;
}

size_t tk_dat_render(char *dst, size_t cap, int y, unsigned mo)
{
    static const char kHdr[] = "# TimeKeeper offline fallback date\n";
    size_t n = 0, sz = 0;

    if (cap < sizeof(kHdr) + 12) return 0;
    while (kHdr[sz]) dst[n++] = kHdr[sz++];
    n += tk_fmt_date(dst + n, cap - n, y, mo, 1u);
    dst[n++] = '\n';
    dst[n]   = '\0';
    return n;
}

int tk_dat_should_write(int have_trusted_time, time64_t synced_utc,
                        time64_t stored, time64_t embedded, int *decision_out)
{
    int      y = 0, ok = 0;
    unsigned mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    time64_t newday;
    int      dec = TKD_NOT_WHILE_OFF;
    int      write = 0;

    if (decision_out) *decision_out = dec;
    if (!have_trusted_time || synced_utc == TK_EPOCH_INVALID) return 0;

    /* First day of the synced month — computed with the calendar, because
     * dividing by 30.44 days would land on the wrong month at month ends. */
    if (!tk_civil_from_epoch_utc(synced_utc, &y, &mo, &d, &hh, &mm, &ss)) return 0;
    newday = tk_epoch_from_civil_utc(y, mo, 1u, 0u, 0u, 0u, &ok);
    if (!ok) return 0;

    if (stored != TK_EPOCH_INVALID) {
        if (stored == newday)                    dec = TKD_IDEMPOTENT;
        else if (stored > synced_utc)            dec = TKD_NEWER_ON_DISK;
        else                                     dec = TKD_STALE_REWRITE;
        /* Note: newday <= synced_utc by construction (it is the 1st of the
         * synced month), so a stored value that is neither equal nor ahead is
         * necessarily behind and must be rewritten. "Refusing backwards" is
         * therefore already covered and needs no separate branch. */
    } else {
        dec = (embedded != TK_EPOCH_INVALID && embedded >= newday)
              ? TKD_NOFILE_CURRENT : TKD_NOFILE_STALE;
    }

    write = (dec == TKD_STALE_REWRITE || dec == TKD_NOFILE_STALE);
    if (decision_out) *decision_out = dec;
    return write;
}

const char *tk_dat_name(int d)
{
    switch (d) {
    case TKD_NOFILE_STALE:      return "CREATE(stale-default)";
    case TKD_NOFILE_CURRENT:    return "NONE(default-current)";
    case TKD_IDEMPOTENT:        return "NOOP(idempotent)";
    case TKD_STALE_REWRITE:     return "REWRITE(stale)";
    case TKD_NEWER_ON_DISK:     return "NOOP(newer-on-disk)";
    case TKD_NOT_WHILE_OFF:     return "NOOP(untrusted-time)";
    case TKD_REFUSED_BACKWARDS: return "REFUSED(backwards)";
    default:                    return "?";
    }
}

/* verdict.c — the policy. No OS calls, no I/O. See verdict.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "verdict.h"
#include "tklibc.h"

#define DAY_SECONDS 86400LL

time64_t tk_build_epoch(int y, unsigned mo, unsigned d)
{
    int ok = 0;
    time64_t e = tk_epoch_from_civil_utc(y, mo, d, 0, 0, 0, &ok);
    return ok ? e : TK_EPOCH_INVALID;
}

int tk_verdict_skip_or_sync(time64_t now, time64_t ref, int ref_fresh,
                            int64_t skip_ms, int force, int ref_is_date_only)
{
    int64_t diff_s, abs_s, skip_s;

    if (force) return TKV_SYNC_FORCED;
    if (now == TK_EPOCH_INVALID || ref == TK_EPOCH_INVALID) return TKV_SYNC;

    /* Held in seconds, never multiplied up to milliseconds: (now - ref) * 1000
     * is an overflow a reviewer has to bound by hand, and the millisecond unit
     * buys nothing because every source here is whole-second granular anyway. */
    diff_s = (int64_t)now - (int64_t)ref;
    abs_s  = diff_s < 0 ? -diff_s : diff_s;
    skip_s = skip_ms / 1000;
    if (skip_ms % 1000) skip_s++;                 /* round the window up, not down */

    /* Date-granular reference, same UTC day, clock not behind the reference.
     * A .dat that only names a day cannot certify the hour, so demanding a
     * 5-minute match against it would re-probe the network on every boot of a
     * machine whose clock is fine (see ARCHITECTURE.md V1). */
    if (ref_is_date_only && ref_fresh && diff_s >= 0 &&
        tk_epoch_floor_days(now) == tk_epoch_floor_days(ref))
        return TKV_SKIP_ALREADY_OK;

    /* Tier 1 — the "don't thrash a good clock" test: the clock sits within
     * skip_ms of what we already believe today is, and what we believe is
     * recent. No network traffic, no file writes; exits in milliseconds. */
    if (ref_fresh && abs_s <= skip_s) return TKV_SKIP_ALREADY_OK;

    /* Tier 2 — the reference is too old to certify anything. Only a network
     * source can prove the clock right, so the caller must attempt it. */
    return ref_fresh ? TKV_SYNC : TKV_NEED_NETWORK;
}

int tk_fallback_apply(time64_t now, time64_t ref, time64_t fallback,
                      int64_t max_age_days, int allow_back, int *action_out)
{
    int64_t age;
    int     act = TKF_APPLY;

    if (fallback == TK_EPOCH_INVALID)            act = TKF_SKIP_OK;
    else if (now == TK_EPOCH_INVALID)            act = TKF_APPLY;
    else if (allow_back)                         act = TKF_APPLY;
    else if (fallback <= now) {
        /* The clock is already at or past the fallback date. Writing it would
         * drag a working machine backwards — the single worst failure mode for
         * a tool that runs unattended at boot. */
        act = (ref != TK_EPOCH_INVALID && now <= ref) ? TKF_SKIP_OK : TKF_SKIP_WOULD_REGRESS;
    } else {
        age = (now - fallback) / DAY_SECONDS;
        if (age > max_age_days) act = TKF_SKIP_OK;   /* fallback too stale to trust */
        else                    act = TKF_APPLY;
    }
    if (action_out) *action_out = act;
    return act;
}

int tk_candidate_ok(time64_t e, int year_min, int year_max)
{
    int y = 0, ok;
    unsigned mo, d, hh, mm, ss;

    if (e == TK_EPOCH_INVALID) return 0;
    if (e < TK_EPOCH_FT_MIN || e > TK_EPOCH_FT_MAX) return 0;
    ok = tk_civil_from_epoch_utc(e, &y, &mo, &d, &hh, &mm, &ss);
    if (!ok) return 0;
    return (y >= year_min && y <= year_max);
}

int tk_clock_is_sane(time64_t now, time64_t ref, int64_t max_age_days)
{
    if (now == TK_EPOCH_INVALID) return 0;
    if (ref == TK_EPOCH_INVALID) return 1;      /* nothing to compare against */
    /* Ahead of the best offline bound: cannot be proved wrong without a network
     * answer, so leave it alone. Written as a comparison rather than a
     * subtraction so there is no signed-overflow question to answer. */
    if (now >= ref) return 1;
    return (ref - now) / DAY_SECONDS <= max_age_days;
}

const char *tk_verdict_name(int v)
{
    switch (v) {
    case TKV_SKIP_ALREADY_OK: return "SKIP-ALREADY-OK";
    case TKV_SYNC:              return "SYNC";
    case TKV_SYNC_FORCED:       return "SYNC-FORCED";
    case TKV_NEED_NETWORK:      return "SYNC-NEEDED(REF-OLD)";
    default:                    return "?";
    }
}

const char *tk_fallback_name(int a)
{
    switch (a) {
    case TKF_APPLY:              return "APPLY";
    case TKF_SKIP_OK:            return "SKIP(CLOCK-PLAUSIBLE)";
    case TKF_SKIP_WOULD_REGRESS: return "SKIP(WOULD-REGRESS)";
    default:                     return "?";
    }
}

time64_t tk_choose_reference(time64_t c0, time64_t c1, time64_t c2)
{
    time64_t c[3], best = TK_EPOCH_INVALID;
    int i;
    c[0] = c0; c[1] = c1; c[2] = c2;
    for (i = 0; i < 3; i++)
        if (c[i] != TK_EPOCH_INVALID && (best == TK_EPOCH_INVALID || c[i] > best))
            best = c[i];
    return best;
}

int tk_ref_is_fresh(time64_t now, time64_t ref, int64_t max_age_days)
{
    int64_t diff;
    if (now == TK_EPOCH_INVALID || ref == TK_EPOCH_INVALID) return 0;
    diff = (int64_t)now - (int64_t)ref;
    if (diff < 0) return 0;                 /* clock is behind the bound: broken */
    return (diff / 86400) <= max_age_days;
}

int tk_udp_blackholed(int servers_tried, int any_reply, uint32_t elapsed_ms,
                      uint32_t budget_ms, int threshold_servers)
{
    if (any_reply) return 0;
    if (servers_tried < threshold_servers) return 0;
    /* Half the budget gone with nothing to show for it: the remaining servers
     * share the fate of the first two, and the boot clock is already ticking. */
    return elapsed_ms >= (budget_ms / 2u) ? 1 : 0;
}

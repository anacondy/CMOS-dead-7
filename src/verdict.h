/* verdict.h — every branch decision in TimeKeeper, as a pure function.
 *
 * Nothing here touches an OS or a file. main.c gathers inputs, asks verdict.c
 * what to do, then performs it. That split is what makes the entire safety
 * policy (never regress a good clock, never trust a stale .dat, never bump the
 * fallback backwards) testable on Linux with zero mocks, and auditable in one
 * screen of code.
 *
 * All times are epoch seconds (timeutil.h); TK_EPOCH_INVALID means "unknown".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_VERDICT_H
#define TIMEKEEPER_VERDICT_H

#include "timeutil.h"

typedef enum {
    TKV_SKIP_ALREADY_OK   = 0,  /* clock agrees with a fresh reference  */
    TKV_SYNC              = 1,  /* go to the network tier               */
    TKV_SYNC_FORCED       = 2,  /* /force: ignore the skip check        */
    TKV_NEED_NETWORK      = 3   /* clock looks wrong, reference stale:
                                 * network is the only trustworthy source,
                                 * so an offline result must not claim OK  */
} tk_verdict_t;

typedef enum {
    TKF_APPLY           = 0,   /* move the clock to the fallback date  */
    TKF_SKIP_OK         = 1,   /* offline, but the clock is plausible  */
    TKF_SKIP_WOULD_REGRESS = 2 /* fallback is older than a good clock  */
} tk_fallback_action_t;

/* --------------------------------------------------------------- decisions */

/* What should we do at all?
 *   now       : current system clock (UTC, epoch secs)
 *   ref       : best offline guess of "today" = max(build time, .dat, .dat mtime)
 *   ref_fresh : ref is within TK_REF_FRESH_DAYS of the real date
 * The |now - ref| comparison the spec calls for is computed here, not passed
 * in, so the sign convention cannot be gotten wrong at the call site.
 *   skip_ms   : TK_SKIP_MATCH_MS
 *   force     : /force given (or the clock is obviously broken)
 *   ref_is_date_only : 1 when REF came from a source with no time-of-day
 *                      (the embedded default, a build date, a bare .dat).
 *                      Such a reference cannot certify the *hour*, only the
 *                      day, so "same UTC day and not behind" is treated as
 *                      correct. Without this rule an already-correct clock
 *                      would re-probe the network on every boot (acceptance
 *                      test 5 requires a sub-500 ms exit).  */
int tk_verdict_skip_or_sync(time64_t now, time64_t ref, int ref_fresh,
                            int64_t skip_ms, int force, int ref_is_date_only);

/* Offline fallback gate.
 *   now / fallback : as above; fallback is 00:00:00 UTC of the stored date
 *   max_age_days   : a fallback older than this is not worth writing
 *   allow_back     : /force lets the user deliberately set an older date      */
int tk_fallback_apply(time64_t now, time64_t ref, time64_t fallback,
                      int64_t max_age_days, int allow_back, int *action_out);

/* Never apply a time outside [year_min, year_max] or outside what FILETIME
 * can express. Used on every candidate from every source, including .dat. */
int tk_candidate_ok(time64_t e, int year_min, int year_max);

/* A date whose calendar day matches the reference but whose clock time is
 * wildly off (typical BIOS default 2009-01-01 12:00:00) still needs a fix. */
int tk_clock_is_sane(time64_t now, time64_t ref, int64_t max_age_days);

/* The build date as an instant, from the calendar rather than __DATE__, so no
 * locale and no parsing. Invalid config.h values yield TK_EPOCH_INVALID, which
 * every caller already treats as "no information". */
time64_t tk_build_epoch(int y, unsigned mo, unsigned d);

const char *tk_verdict_name(int v);
const char *tk_fallback_name(int a);

/* ---------------------------------------------- pure helpers used by main.c
 * These exist so that the arithmetic in main.c reduces to single calls whose
 * behaviour is proven in tests/host. */

/* REF (the best offline justification of "today") = max of every candidate we
 * have: build date, TimeKeeper.dat contents, and the .dat's mtime. Never min():
 * the tool has to be monotone-forward or a stale .dat undoes a repaired clock.
 * Invalid entries are skipped; returns TK_EPOCH_INVALID if nothing is known. */
time64_t tk_choose_reference(time64_t c0, time64_t c1, time64_t c2);

/* A reference only certifies the clock when the clock is at or after it and not
 * more than max_age_days ahead. Both bounds matter: behind means the clock is
 * broken by definition, absurdly far ahead means we cannot trust our own bound. */
int      tk_ref_is_fresh(time64_t now, time64_t ref, int64_t max_age_days);

/* Black-hole detection for the UDP tier. If the first k servers all timed out
 * (no packet at all) and we have already spent most of the budget, probing the
 * remaining servers is pure latency: every one of them is being dropped by the
 * same firewall. Returns 1 to abandon UDP and go straight to HTTP. */
int      tk_udp_blackholed(int servers_tried, int any_reply, uint32_t elapsed_ms,
                           uint32_t budget_ms, int threshold_servers);

#endif /* TIMEKEEPER_VERDICT_H */

/* fallback_proto.h — TimeKeeper.dat file format and update decision, pure.
 *
 * The file is deliberately trivial (one plaintext date, plus optional comments)
 * so that a user can fix a wrong clock with Notepad and so that the GitHub
 * Action can bump it with a regex. Robustness therefore means *tolerant read,
 * strict validate*: never crash on garbage, never accept an insane date.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_FALLBACK_PROTO_H
#define TIMEKEEPER_FALLBACK_PROTO_H

#include "timeutil.h"

#define TK_DAT_MAX_BYTES 512

/* Reasons a rewrite is or is not performed; names appear in the log line. */
typedef enum {
    TKD_NOFILE_STALE      = 0,  /* no .dat, embedded default is old: create */
    TKD_NOFILE_CURRENT    = 1,  /* no .dat needed: default is current       */
    TKD_IDEMPOTENT        = 2,  /* .dat already holds this month            */
    TKD_STALE_REWRITE     = 3,  /* .dat is behind: atomically rewrite it    */
    TKD_NEWER_ON_DISK     = 4,  /* .dat is ahead: never touch it            */
    TKD_NOT_WHILE_OFF     = 5,  /* no trusted time this run                 */
    TKD_REFUSED_BACKWARDS = 6   /* synced instant precedes the stored date  */
} tk_dat_decision_t;

const char *tk_dat_name(int d);

/* Parse the raw contents of TimeKeeper.dat.
 *   epoch_out : 00:00:00 UTC of the parsed date, or a time-of-day applied if the
 *               file carried one ("2026-09-01 06:00" -> that instant, UTC)
 *   y/mo/d    : optional components, for the log line and month arithmetic
 * Returns 1 if a valid, in-window date was found. Rejects:
 *   - no date token, or malformed (Feb 30, month 13, 2026-2-30, ...)
 *   - year outside [year_min, year_max]
 * A file that parses to an out-of-window year is treated as corrupt, and the
 * caller falls back to the embedded default — the same path as a missing file. */
int tk_dat_parse(const char *buf, size_t len, int year_min, int year_max,
                 time64_t *epoch_out, int *y, unsigned *mo, unsigned *d);

/* Render the canonical one-line file body for a given year/month.
 * Produces exactly: "# TimeKeeper offline fallback date\nYYYY-MM-01\n"
 * Returns bytes written, or 0 if cap is too small. */
size_t tk_dat_render(char *dst, size_t cap, int y, unsigned mo);

/* Should the .dat be rewritten after a trusted (network-verified) sync?
 * Rules, in order — all of them safety-critical:
 *   - never on a failed/offline run                 (have_trusted_time)
 *   - never to a date at or before the synced instant, so the fallback can
 *     never be ahead of real time                   (bump forward only)
 *   - never rewrite a file that is already correct  (idempotence: test #3)
 *   - never shrink what is on disk                  (newer-on-disk wins)
 * Returns 1 to write, 0 to skip; *decision_out names the reason for the log. */
int tk_dat_should_write(int have_trusted_time, time64_t synced_utc,
                        time64_t stored, time64_t embedded, int *decision_out);

#endif /* TIMEKEEPER_FALLBACK_PROTO_H */

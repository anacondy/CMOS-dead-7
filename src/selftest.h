/* selftest.h — the built-in verification mode.
 *
 * `TimeKeeper.exe /test` runs the same suite the build runs, so a field
 * machine can be validated with no toolchain and no network. Exit code 0 = all
 * checks passed.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_SELFTEST_H
#define TIMEKEEPER_SELFTEST_H

typedef void (*tk_report_fn)(void *ud, const char *line);

/* Returns 0 on success, 1 if any check failed. cb may be NULL (silent). */
int tk_selftest_run(tk_report_fn cb, void *ud);

#endif /* TIMEKEEPER_SELFTEST_H */
